// OFDMNvisWaveform - Implementation

#define _USE_MATH_DEFINES
#include <cmath>
#include "ofdm_cox_waveform.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include <sstream>

namespace ultra {

namespace {
constexpr size_t LDPC_CODEWORD_BITS = 648;
}

OFDMNvisWaveform::OFDMNvisWaveform() {
    // Default OFDM_COX configuration matches OFDM_CHIRP geometry:
    // 1024-FFT, 59 carriers, 46.875 Hz spacing, MEDIUM CP. This is what
    // used to be the explicit "NVIS preset" — folded into the default
    // because the old 512-FFT/30-carrier preset was strictly worse on
    // every hardware test (lower throughput, no fading robustness, no
    // backward-compat user). The legacy 512-FFT preset is still
    // reachable via createLegacyMode() for regression testing.
    config_.fft_size = 1024;
    config_.num_carriers = 59;
    config_.cp_mode = CyclicPrefixMode::MEDIUM;
    config_.symbol_guard = 0;
    config_.use_pilots = false;  // Caller toggles based on coherent vs differential
    config_.modulation = Modulation::DQPSK;
    config_.code_rate = CodeRate::R1_2;
    config_.sample_rate = 48000;
    initComponents();
}

OFDMNvisWaveform::OFDMNvisWaveform(const ModemConfig& config)
    : config_(config)
{
    initComponents();
}

std::unique_ptr<OFDMNvisWaveform> OFDMNvisWaveform::createNvisMode() {
    // NVIS configuration: 1024 FFT, 59 carriers, 46.875 Hz spacing
    // Matches industry-leader parameters for better fading performance
    ModemConfig cfg;
    cfg.fft_size = 1024;                       // 46.875 Hz spacing (vs 93.75)
    cfg.num_carriers = 59;                     // 59 carriers for ~2.8 kHz bandwidth
    cfg.cp_mode = CyclicPrefixMode::MEDIUM;    // 256 samples (5.3ms) for 1024 FFT
    cfg.symbol_guard = 0;
    cfg.use_pilots = false;                    // All carriers as data (for DQPSK)
    cfg.modulation = Modulation::DQPSK;        // Default to DQPSK
    cfg.code_rate = CodeRate::R1_2;            // R1/2 for fading conditions
    cfg.sample_rate = 48000;

    LOG_MODEM(INFO, "OFDMNvisWaveform: Creating NVIS mode (FFT=1024, carriers=59, spacing=46.875Hz)");
    return std::make_unique<OFDMNvisWaveform>(cfg);
}

void OFDMNvisWaveform::initComponents() {
    modulator_ = std::make_unique<OFDMModulator>(config_);
    demodulator_ = std::make_unique<OFDMDemodulator>(config_);
}

WaveformCapabilities OFDMNvisWaveform::getCapabilities() const {
    WaveformCapabilities caps;
    caps.supports_cfo_correction = true;    // Via Schmidl-Cox
    caps.supports_doppler_correction = true; // Via pilot tracking
    caps.requires_pilots = config_.use_pilots;
    caps.supports_differential = true;       // Can use DQPSK

    // Coherent modulation needs better SNR
    bool is_differential = (config_.modulation == Modulation::DBPSK ||
                           config_.modulation == Modulation::DQPSK ||
                           config_.modulation == Modulation::D8PSK);
    caps.min_snr_db = is_differential ? 12.0f : 17.0f;
    caps.max_snr_db = 35.0f;
    caps.max_throughput_bps = getThroughput(CodeRate::R3_4);

    // Schmidl-Cox preamble is ~2 OFDM symbols
    caps.preamble_duration_ms = 2.0f * getSamplesPerSymbol() * 1000.0f / config_.sample_rate;

    return caps;
}

void OFDMNvisWaveform::configure(Modulation mod, CodeRate rate) {
    config_.modulation = mod;
    config_.code_rate = rate;

    // Automatically determine if we need pilots
    bool is_differential = (mod == Modulation::DBPSK ||
                           mod == Modulation::DQPSK ||
                           mod == Modulation::D8PSK);
    config_.use_pilots = !is_differential;

    // Reinitialize with new config
    initComponents();

    LOG_MODEM(INFO, "OFDMNvisWaveform: configured for %s %s (pilots=%d)",
              modulationToString(mod), codeRateToString(rate), config_.use_pilots ? 1 : 0);
}

void OFDMNvisWaveform::setFrequencyOffset(float cfo_hz) {
    cfo_hz_ = cfo_hz;
    if (demodulator_) {
        demodulator_->setFrequencyOffset(cfo_hz);
    }
}

void OFDMNvisWaveform::setTxFrequencyOffset(float cfo_hz) {
    // Set TX CFO in config and reinitialize
    config_.tx_cfo_hz = cfo_hz;
    initComponents();

    LOG_MODEM(INFO, "OFDMNvisWaveform: TX CFO set to %.1f Hz", cfo_hz);
}

Samples OFDMNvisWaveform::generatePreamble() {
    if (!modulator_) {
        return Samples();
    }
    return modulator_->generatePreamble();
}

Samples OFDMNvisWaveform::modulate(const Bytes& encoded_data) {
    if (!modulator_) {
        return Samples();
    }
    ByteSpan span(encoded_data.data(), encoded_data.size());
    return modulator_->modulate(span, config_.modulation);
}

bool OFDMNvisWaveform::detectSync(SampleSpan samples, SyncResult& result, float threshold) {
    // Use Schmidl-Cox detection to find sync and CFO (like CHIRP does with chirp detection)
    // This does NOT demodulate - it just finds where training starts and estimates CFO
    if (!demodulator_) {
        return false;
    }

    size_t lts_position = 0;
    float cfo_hz = 0.0f;

    // searchForSync finds STS, estimates CFO, returns position where LTS starts
    bool found = demodulator_->searchForSync(samples, lts_position, cfo_hz, threshold);

    if (found) {
        result.detected = true;
        result.start_sample = static_cast<int>(lts_position);
        result.cfo_hz = cfo_hz;
        result.snr_estimate = 0.0f;  // Will be estimated during process()
        result.has_training = true;
        result.correlation = 0.9f;

        cfo_hz_ = cfo_hz;
        synced_ = true;
        training_start_sample_ = lts_position;

        LOG_MODEM(INFO, "OFDMNvisWaveform::detectSync: found sync, LTS at %zu, CFO=%.1f Hz",
                  lts_position, cfo_hz);
        return true;
    }

    return false;
}

void OFDMNvisWaveform::setAbsoluteTrainingPosition(size_t pos) {
    absolute_training_start_sample_ = pos;
    has_absolute_training_start_sample_ = true;
}

bool OFDMNvisWaveform::process(SampleSpan samples) {
    if (!demodulator_) {
        return false;
    }

    // If we already have soft bits, return them
    if (!soft_bits_.empty()) {
        return true;
    }

    // Calculate the initial CFO phase based on elapsed samples since audio start
    // (same approach as OFDM_CHIRP)
    // The CFO has been accumulating since sample 0, so at training_start_sample_:
    //   phase = -2π × CFO × training_start_sample_ / sample_rate
    size_t phase_ref_sample = training_start_sample_;
    if (has_absolute_training_start_sample_) {
        phase_ref_sample = absolute_training_start_sample_;
    }
    float initial_phase_rad = -2.0f * M_PI * cfo_hz_ * phase_ref_sample / config_.sample_rate;

    // Wrap to [-π, π]
    while (initial_phase_rad > M_PI) initial_phase_rad -= 2.0f * M_PI;
    while (initial_phase_rad < -M_PI) initial_phase_rad += 2.0f * M_PI;

    LOG_MODEM(INFO, "OFDMNvisWaveform::process: CFO=%.1f Hz, training_start=%zu, abs_start=%zu, initial_phase=%.1f°, samples=%zu",
              cfo_hz_, training_start_sample_,
              has_absolute_training_start_sample_ ? absolute_training_start_sample_ : 0,
              initial_phase_rad * 180.0f / M_PI, samples.size());

    // Pass CFO and initial phase to demodulator (same as OFDM_CHIRP)
    demodulator_->setFrequencyOffsetWithPhase(cfo_hz_, initial_phase_rad);

    // Use processPresynced - samples should start at LTS position (same as OFDM_CHIRP)
    // 2 = number of LTS training symbols for channel estimation
    bool ready = demodulator_->processPresynced(samples, 2);

    if (ready) {
        // Retrieve all soft bits
        soft_bits_.clear();
        while (demodulator_->hasPendingData()) {
            auto chunk = demodulator_->getSoftBits();
            if (chunk.size() != LDPC_CODEWORD_BITS) break;
            soft_bits_.insert(soft_bits_.end(), chunk.begin(), chunk.end());
        }
    }

    return ready;
}

std::vector<float> OFDMNvisWaveform::getSoftBits() {
    return std::move(soft_bits_);
}

void OFDMNvisWaveform::reset() {
    if (demodulator_) {
        demodulator_->reset();
    }
    soft_bits_.clear();
    synced_ = false;
    training_start_sample_ = 0;
    has_absolute_training_start_sample_ = false;
    absolute_training_start_sample_ = 0;
    // NOTE: CFO is intentionally preserved across reset() for continuous tracking
    // Use setFrequencyOffset(0) to explicitly clear if needed
}

bool OFDMNvisWaveform::isSynced() const {
    return synced_ || (demodulator_ && demodulator_->isSynced());
}

bool OFDMNvisWaveform::hasData() const {
    return !soft_bits_.empty() || (demodulator_ && demodulator_->hasPendingData());
}

float OFDMNvisWaveform::estimatedSNR() const {
    if (demodulator_) {
        return demodulator_->getEstimatedSNR();
    }
    return 0.0f;
}

bool OFDMNvisWaveform::hasLastOFDMBroadbandSNREstimate() const {
    return demodulator_ && demodulator_->hasLastOFDMBroadbandSNREstimate();
}

float OFDMNvisWaveform::getLastOFDMBroadbandSNREstimate() const {
    if (demodulator_) {
        return demodulator_->getLastOFDMBroadbandSNREstimate();
    }
    return 0.0f;
}

float OFDMNvisWaveform::estimatedCFO() const {
    if (demodulator_) {
        return demodulator_->getFrequencyOffset();
    }
    return cfo_hz_;
}

std::vector<std::complex<float>> OFDMNvisWaveform::getConstellationSymbols() const {
    if (demodulator_) {
        return demodulator_->getConstellationSymbols();
    }
    return {};
}

std::string OFDMNvisWaveform::getStatusString() const {
    std::ostringstream oss;
    oss << "OFDM-COX " << config_.num_carriers << " carriers, "
        << modulationToString(config_.modulation) << " "
        << codeRateToString(config_.code_rate);
    if (config_.use_pilots) {
        oss << " (pilots)";
    }
    return oss.str();
}

float OFDMNvisWaveform::getThroughput(CodeRate rate) const {
    // Bits per symbol per carrier
    int bits_per_carrier = 2;  // Default QPSK
    switch (config_.modulation) {
        case Modulation::DBPSK:
        case Modulation::BPSK:  bits_per_carrier = 1; break;
        case Modulation::DQPSK:
        case Modulation::QPSK:  bits_per_carrier = 2; break;
        case Modulation::D8PSK:
        case Modulation::QAM8:  bits_per_carrier = 3; break;
        case Modulation::QAM16: bits_per_carrier = 4; break;
        case Modulation::QAM32: bits_per_carrier = 5; break;
        case Modulation::QAM64: bits_per_carrier = 6; break;
        default: break;
    }

    // Number of data carriers
    int data_carriers = ofdm_link_adaptation::dataCarrierCount(
        static_cast<int>(config_.num_carriers),
        config_.use_pilots,
        static_cast<int>(config_.pilot_spacing));

    // Symbol rate
    float symbol_rate = static_cast<float>(config_.sample_rate) / getSamplesPerSymbol();

    // Raw bit rate
    float raw_bps = symbol_rate * data_carriers * bits_per_carrier;

    return raw_bps * getCodeRateValue(rate);
}

int OFDMNvisWaveform::getSamplesPerSymbol() const {
    if (modulator_) {
        return static_cast<int>(modulator_->samplesPerSymbol());
    }
    // Fallback calculation: FFT + CP
    int cp_samples = 0;
    switch (config_.cp_mode) {
        case CyclicPrefixMode::SHORT:  cp_samples = config_.fft_size / 8; break;
        case CyclicPrefixMode::MEDIUM: cp_samples = config_.fft_size / 4; break;
        case CyclicPrefixMode::LONG:   cp_samples = config_.fft_size / 2; break;
    }
    return config_.fft_size + cp_samples;
}

int OFDMNvisWaveform::getPreambleSamples() const {
    // Full Schmidl-Cox preamble generated by OFDMModulator:
    // one guard symbol, four STS symbols, then two LTS training symbols.
    return 7 * getSamplesPerSymbol();
}

int OFDMNvisWaveform::getMinSamplesForFrame() const {
    return getMinSamplesForCWCount(protocol::v2::kDefaultFixedFrameCodewords);
}

int OFDMNvisWaveform::getMinSamplesForControlFrame() const {
    return getMinSamplesForCWCount(1);
}

int OFDMNvisWaveform::getMinSamplesForCWCount(int num_cw) const {
    if (num_cw < 1) {
        num_cw = 1;
    }

    // Samples are counted from the first LTS symbol returned by detectSync().
    // They include the two LTS training symbols plus the requested data CWs.
    int training_samples = 2 * getSamplesPerSymbol();  // 2 training symbols

    // Bits per carrier based on modulation
    int bits_per_carrier = 2;  // Default QPSK
    switch (config_.modulation) {
        case Modulation::BPSK:
        case Modulation::DBPSK: bits_per_carrier = 1; break;
        case Modulation::QPSK:
        case Modulation::DQPSK: bits_per_carrier = 2; break;
        case Modulation::D8PSK: bits_per_carrier = 3; break;
        case Modulation::QAM16: bits_per_carrier = 4; break;
        case Modulation::QAM32: bits_per_carrier = 5; break;
        case Modulation::QAM64: bits_per_carrier = 6; break;
        default: bits_per_carrier = 2; break;
    }

    // Number of data carriers
    int data_carriers = ofdm_link_adaptation::dataCarrierCount(
        static_cast<int>(config_.num_carriers),
        config_.use_pilots,
        static_cast<int>(config_.pilot_spacing));

    int bits_per_symbol = data_carriers * bits_per_carrier;
    if (bits_per_symbol <= 0) {
        return training_samples;
    }
    int frame_bits = num_cw * static_cast<int>(LDPC_CODEWORD_BITS);
    int data_symbols = (frame_bits + bits_per_symbol - 1) / bits_per_symbol;
    int data_samples = data_symbols * getSamplesPerSymbol();

    return training_samples + data_samples;
}

void OFDMNvisWaveform::setUsePilots(bool use_pilots) {
    config_.use_pilots = use_pilots;
    initComponents();
}

} // namespace ultra
