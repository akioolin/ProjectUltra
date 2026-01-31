// OTFSWaveform - Implementation

#include "otfs_waveform.hpp"
#include "ultra/logging.hpp"
#include "ultra/dsp.hpp"
#include <sstream>
#include <cmath>

namespace ultra {

OTFSWaveform::OTFSWaveform() {
    // Default OTFS configuration matching OFDM parameters
    config_.M = 32;              // Delay bins (similar to num_carriers)
    config_.N = 16;              // Doppler bins (OFDM symbols per frame)
    config_.fft_size = 512;
    config_.cp_length = 64;
    config_.sample_rate = 48000;
    config_.center_freq = 1500.0f;
    config_.modulation = Modulation::QPSK;
    config_.tf_equalization = true;
    initComponents();
}

OTFSWaveform::OTFSWaveform(const OTFSConfig& config)
    : config_(config)
{
    initComponents();
}

void OTFSWaveform::initComponents() {
    modulator_ = std::make_unique<OTFSModulator>(config_);
    demodulator_ = std::make_unique<OTFSDemodulator>(config_);
    chirp_sync_ = std::make_unique<sync::ChirpSync>(getChirpConfig());
}

sync::ChirpConfig OTFSWaveform::getChirpConfig() const {
    sync::ChirpConfig cfg;
    cfg.sample_rate = static_cast<float>(config_.sample_rate);
    cfg.f_start = 300.0f;
    cfg.f_end = 2700.0f;
    cfg.duration_ms = 500.0f;
    cfg.gap_ms = 100.0f;
    cfg.use_dual_chirp = true;
    cfg.tx_cfo_hz = tx_cfo_hz_;
    return cfg;
}

WaveformCapabilities OTFSWaveform::getCapabilities() const {
    WaveformCapabilities caps;
    caps.supports_cfo_correction = true;
    caps.supports_doppler_correction = true;  // OTFS is designed for Doppler!
    caps.requires_pilots = false;
    caps.supports_differential = false;  // OTFS uses coherent modulation
    caps.min_snr_db = 10.0f;
    caps.max_snr_db = 25.0f;
    caps.max_throughput_bps = getThroughput(CodeRate::R2_3);
    caps.preamble_duration_ms = chirp_sync_ ?
        (chirp_sync_->getTotalSamples() * 1000.0f / config_.sample_rate) : 1200.0f;
    return caps;
}

void OTFSWaveform::configure(Modulation mod, CodeRate rate) {
    // OTFS supports QPSK and 16QAM (coherent modes)
    if (mod != Modulation::QPSK && mod != Modulation::QAM16 && mod != Modulation::BPSK) {
        LOG_MODEM(WARN, "OTFSWaveform: Unsupported modulation %d, using QPSK",
                  static_cast<int>(mod));
        mod = Modulation::QPSK;
    }

    config_.modulation = mod;
    code_rate_ = rate;
    initComponents();

    LOG_MODEM(INFO, "OTFSWaveform: configured for %s %s",
              modulationToString(mod), codeRateToString(rate));
}

void OTFSWaveform::setFrequencyOffset(float cfo_hz) {
    cfo_hz_ = cfo_hz;
    if (demodulator_) {
        demodulator_->setFrequencyOffset(cfo_hz);
    }
}

void OTFSWaveform::setTxFrequencyOffset(float cfo_hz) {
    tx_cfo_hz_ = cfo_hz;
    initComponents();
    LOG_MODEM(INFO, "OTFSWaveform: TX CFO set to %.1f Hz", cfo_hz);
}

Samples OTFSWaveform::generatePreamble() {
    if (!chirp_sync_ || !modulator_) {
        return Samples();
    }

    // Generate: [CHIRP][OTFS_PREAMBLE]
    Samples chirp = chirp_sync_->generate();
    Samples otfs_preamble = modulator_->generatePreamble();

    Samples preamble;
    preamble.reserve(chirp.size() + otfs_preamble.size());
    preamble.insert(preamble.end(), chirp.begin(), chirp.end());
    preamble.insert(preamble.end(), otfs_preamble.begin(), otfs_preamble.end());

    return preamble;
}

Samples OTFSWaveform::modulate(const Bytes& encoded_data) {
    if (!modulator_) {
        return Samples();
    }

    // Map bytes to DD symbols
    ByteSpan span(encoded_data.data(), encoded_data.size());
    auto dd_symbols = modulator_->mapToDD(span, config_.modulation);

    // Modulate DD grid to audio
    return modulator_->modulate(dd_symbols, config_.modulation);
}

bool OTFSWaveform::detectSync(SampleSpan samples, SyncResult& result, float threshold) {
    if (!chirp_sync_) {
        return false;
    }

    auto chirp_result = chirp_sync_->detectDualChirp(samples, threshold);

    result.detected = chirp_result.success;
    result.correlation = std::max(chirp_result.up_correlation, chirp_result.down_correlation);
    result.cfo_hz = chirp_result.cfo_hz;
    result.has_training = true;

    if (chirp_result.success) {
        synced_ = true;
        last_cfo_ = chirp_result.cfo_hz;
        cfo_hz_ = chirp_result.cfo_hz;

        // Calculate where OTFS preamble starts
        size_t chirp_samples = chirp_sync_->getChirpSamples();
        size_t gap_samples = static_cast<size_t>(config_.sample_rate * 100.0f / 1000.0f);

        result.start_sample = chirp_result.down_chirp_start + chirp_samples + gap_samples;
        training_start_sample_ = result.start_sample;

        LOG_MODEM(INFO, "OTFSWaveform: Chirp detected, CFO=%.1f Hz, preamble_start=%d",
                  chirp_result.cfo_hz, result.start_sample);
    }

    return result.detected;
}

bool OTFSWaveform::process(SampleSpan samples) {
    if (!demodulator_) {
        return false;
    }

    // Apply pre-set CFO before processing
    demodulator_->setFrequencyOffset(cfo_hz_);

    // Process using pre-synced method (samples should start at OTFS preamble)
    // OTFS preamble has 4 repeated symbols for channel estimation
    bool ready = demodulator_->processPresynced(samples, 4);

    if (ready) {
        soft_bits_ = demodulator_->getSoftBits();
        constellation_ = demodulator_->getDDSymbols();
        last_snr_ = demodulator_->getEstimatedSNR();

        LOG_MODEM(INFO, "OTFSWaveform: Frame decoded, %zu soft bits, SNR=%.1f dB",
                  soft_bits_.size(), last_snr_);
    }

    return ready;
}

std::vector<float> OTFSWaveform::getSoftBits() {
    return std::move(soft_bits_);
}

void OTFSWaveform::reset() {
    if (demodulator_) {
        demodulator_->reset();
    }
    soft_bits_.clear();
    constellation_.clear();
    synced_ = false;
}

bool OTFSWaveform::isSynced() const {
    return synced_ || (demodulator_ && demodulator_->isSynced());
}

bool OTFSWaveform::hasData() const {
    return !soft_bits_.empty();
}

float OTFSWaveform::estimatedSNR() const {
    if (demodulator_) {
        return demodulator_->getEstimatedSNR();
    }
    return last_snr_;
}

float OTFSWaveform::estimatedCFO() const {
    return last_cfo_;
}

float OTFSWaveform::getFadingIndex() const {
    // OTFS averages across all resources, so effective fading is lower
    // For now, return 0 to indicate "no per-carrier fading visible"
    return 0.0f;
}

std::vector<std::complex<float>> OTFSWaveform::getConstellationSymbols() const {
    return constellation_;
}

std::string OTFSWaveform::getStatusString() const {
    std::ostringstream oss;
    oss << "OTFS " << config_.M << "x" << config_.N << " "
        << modulationToString(config_.modulation) << " "
        << codeRateToString(code_rate_);
    if (std::abs(last_cfo_) > 0.5f) {
        oss << " (CFO=" << static_cast<int>(last_cfo_) << " Hz)";
    }
    return oss.str();
}

float OTFSWaveform::getThroughput(CodeRate rate) const {
    // Bits per DD symbol
    int bits_per_symbol = 2;  // Default QPSK
    switch (config_.modulation) {
        case Modulation::BPSK: bits_per_symbol = 1; break;
        case Modulation::QPSK: bits_per_symbol = 2; break;
        case Modulation::QAM16: bits_per_symbol = 4; break;
        default: bits_per_symbol = 2; break;
    }

    // Total data symbols in DD grid
    size_t data_symbols = config_.total_data_symbols();

    // Frame duration (N OFDM symbols)
    float frame_duration = config_.N * getSamplesPerSymbol() /
                          static_cast<float>(config_.sample_rate);

    // Raw bit rate
    float raw_bps = data_symbols * bits_per_symbol / frame_duration;

    // Apply code rate
    float code_ratio = 0.5f;
    switch (rate) {
        case CodeRate::R1_4: code_ratio = 0.25f; break;
        case CodeRate::R1_3: code_ratio = 0.333f; break;
        case CodeRate::R1_2: code_ratio = 0.5f; break;
        case CodeRate::R2_3: code_ratio = 0.667f; break;
        case CodeRate::R3_4: code_ratio = 0.75f; break;
        case CodeRate::R5_6: code_ratio = 0.833f; break;
    }

    return raw_bps * code_ratio;
}

int OTFSWaveform::getSamplesPerSymbol() const {
    return config_.fft_size + config_.cp_length;
}

int OTFSWaveform::getPreambleSamples() const {
    int chirp_total = chirp_sync_ ? static_cast<int>(chirp_sync_->getTotalSamples())
                                  : static_cast<int>(config_.sample_rate * 1.2f);
    // OTFS preamble is 4 identical OFDM symbols
    int otfs_preamble = 4 * getSamplesPerSymbol();
    return chirp_total + otfs_preamble;
}

int OTFSWaveform::getMinSamplesForFrame() const {
    // OTFS preamble (4 symbols) + N data symbols
    int preamble = 4 * getSamplesPerSymbol();
    int data = config_.N * getSamplesPerSymbol();
    return preamble + data;
}

} // namespace ultra
