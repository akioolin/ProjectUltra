// OFDMChirpWaveform - Implementation

#include "ofdm_chirp_waveform.hpp"
#include "fec/carrier_ldpc_interleaver.hpp"
#include "ultra/logging.hpp"
#include "ultra/dsp.hpp"  // FFT class is in here
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/timing_profiler.hpp"
#include <algorithm>
#include <sstream>
#include <cmath>

namespace ultra {

namespace {
constexpr size_t LDPC_CODEWORD_BITS = 648;
constexpr size_t LDPC_CODEWORD_BYTES = LDPC_CODEWORD_BITS / 8;
constexpr size_t CARRIER_LDPC_MASK_CARRIERS = 59;
constexpr size_t CARRIER_LDPC_MIN_CODEWORDS = 2;
constexpr size_t CARRIER_LDPC_MAX_CODEWORDS = 8;
constexpr uint64_t ALL_ON_CARRIER_MASK = UINT64_MAX;

bool isSupportedChirpModulation(Modulation mod) {
    return mod == Modulation::DBPSK ||
           mod == Modulation::DQPSK ||
           mod == Modulation::D8PSK ||
           mod == Modulation::QPSK ||
           mod == Modulation::BPSK ||
           mod == Modulation::QAM16;
}

uint64_t activeBitsMask(size_t carriers) {
    return carriers >= 64 ? UINT64_MAX : ((uint64_t{1} << carriers) - 1);
}

bool isAllOnMask(uint64_t mask, size_t carriers) {
    const uint64_t active = activeBitsMask(carriers);
    return (mask & active) == active;
}

std::vector<size_t> buildDataLogicalCarrierIndices(const ModemConfig& config) {
    std::vector<size_t> logical_indices;
    logical_indices.reserve(config.num_carriers);

    const int neg_limit = static_cast<int>(config.num_carriers / 2);
    const int pos_limit = static_cast<int>((config.num_carriers + 1) / 2);

    int pilot_count = 0;
    size_t logical_carrier = 0;
    for (int i = -neg_limit; i <= pos_limit; ++i) {
        if (i == 0) {
            continue;
        }

        const bool is_pilot = config.use_pilots &&
            config.pilot_spacing != 0 &&
            (pilot_count % static_cast<int>(config.pilot_spacing) == 0);
        if (!is_pilot) {
            logical_indices.push_back(logical_carrier);
        }

        ++pilot_count;
        ++logical_carrier;
    }

    return logical_indices;
}

Bytes applyCarrierLdpcForward(const Bytes& encoded, size_t codeword_count) {
    const size_t total_bits = codeword_count * LDPC_CODEWORD_BITS;
    std::vector<uint8_t> in_bits(total_bits, 0);
    for (size_t i = 0; i < encoded.size() && i * 8 < total_bits; ++i) {
        for (int b = 0; b < 8 && i * 8 + static_cast<size_t>(b) < total_bits; ++b) {
            in_bits[i * 8 + static_cast<size_t>(b)] =
                static_cast<uint8_t>((encoded[i] >> (7 - b)) & 1u);
        }
    }

    const auto interleaver = fec::buildCarrierInterleaverV1(codeword_count);
    std::vector<uint8_t> out_bits(total_bits, 0);
    for (size_t i = 0; i < total_bits; ++i) {
        out_bits[interleaver[i]] = in_bits[i];
    }

    Bytes out((total_bits + 7) / 8, 0);
    for (size_t i = 0; i < total_bits; ++i) {
        if (out_bits[i]) {
            out[i / 8] |= static_cast<uint8_t>(1u << (7 - (i % 8)));
        }
    }
    return out;
}

std::vector<float> eraseMaskedCarrierLLRs(std::vector<float> air_llrs,
                                          const ModemConfig& config,
                                          uint64_t carrier_mask) {
    const std::vector<size_t> data_logical = buildDataLogicalCarrierIndices(config);
    if (data_logical.empty()) {
        return air_llrs;
    }

    const size_t bits_per_carrier = getBitsPerSymbol(config.modulation);
    for (size_t a = 0; a < air_llrs.size(); ++a) {
        const fec::AirGridIndex grid =
            fec::decomposeAirIndex(a, data_logical.size(), bits_per_carrier);
        if (grid.carrier >= data_logical.size()) {
            continue;
        }
        const size_t logical_carrier = data_logical[grid.carrier];
        if (logical_carrier < 64 &&
            (carrier_mask & (uint64_t{1} << logical_carrier)) == 0) {
            air_llrs[a] = 0.0f;
        }
    }
    return air_llrs;
}

std::vector<float> applyCarrierLdpcInverse(std::vector<float> air_llrs,
                                           size_t codeword_count) {
    const size_t total_bits = codeword_count * LDPC_CODEWORD_BITS;
    if (air_llrs.size() < total_bits) {
        return air_llrs;
    }

    const auto deinterleaver = fec::buildCarrierDeinterleaverV1(codeword_count);
    std::vector<float> out(total_bits, 0.0f);
    for (size_t air = 0; air < total_bits; ++air) {
        out[deinterleaver[air]] = air_llrs[air];
    }
    return out;
}
}

OFDMChirpWaveform::OFDMChirpWaveform() {
    // Default OFDM_CHIRP configuration
    config_.fft_size = 512;
    config_.num_carriers = 30;
    config_.modulation = Modulation::DQPSK;  // Differential for fading
    config_.code_rate = CodeRate::R1_2;
    configurePilotsForCodeRate(config_.code_rate);
    initComponents();
}

OFDMChirpWaveform::OFDMChirpWaveform(const ModemConfig& config)
    : config_(config)
{
    // Allow differential and selected coherent modulations for chirp mode.
    if (!isSupportedChirpModulation(config_.modulation)) {
        config_.modulation = Modulation::DQPSK;
    }
    configurePilotsForCodeRate(config_.code_rate);
    initComponents();
}

OFDMChirpWaveform::OFDMChirpWaveform(const ModemConfig& config, protocol::WaveformMode mode)
    : mode_(mode), config_(config)
{
    // Allow differential and selected coherent modulations for chirp mode.
    if (!isSupportedChirpModulation(config_.modulation)) {
        config_.modulation = Modulation::DQPSK;
    }
    configurePilotsForCodeRate(config_.code_rate);
    initComponents();
}

void OFDMChirpWaveform::initComponents() {
    modulator_ = std::make_unique<OFDMModulator>(config_);
    demodulator_ = std::make_unique<OFDMDemodulator>(config_);
    demodulator_->setRXCarrierErasureEnabled(rx_carrier_erasure_enabled_);
    chirp_sync_ = std::make_unique<sync::ChirpSync>(getChirpConfig());
    invalidateDataSyncTemplate();
}

void OFDMChirpWaveform::invalidateDataSyncTemplate() {
    data_sync_template_analytic_.clear();
    data_sync_template_energy_ = 0.0f;
    data_sync_template_symbol_samples_ = 0;
}

bool OFDMChirpWaveform::ensureDataSyncTemplate(int symbol_samples) {
    const size_t template_samples = static_cast<size_t>(symbol_samples) * 2;
    if (symbol_samples <= 0 || template_samples == 0) {
        return false;
    }

    if (data_sync_template_symbol_samples_ == symbol_samples &&
        data_sync_template_analytic_.size() == template_samples &&
        data_sync_template_energy_ > 0.0f) {
        return true;
    }

    ModemConfig template_config = config_;
    template_config.tx_cfo_hz = 0.0f;
    OFDMModulator template_modulator(template_config);
    Samples lts_template = template_modulator.generateTrainingSymbols(2);
    if (lts_template.size() < template_samples) {
        invalidateDataSyncTemplate();
        return false;
    }

    HilbertTransform template_hilbert(65);
    template_hilbert.process(
        SampleSpan(lts_template.data(), template_samples),
        data_sync_template_analytic_);

    data_sync_template_energy_ = 0.0f;
    for (const auto& s : data_sync_template_analytic_) {
        data_sync_template_energy_ += std::norm(s);
    }
    data_sync_template_symbol_samples_ = symbol_samples;

    if (data_sync_template_energy_ <= 1.0e-8f) {
        invalidateDataSyncTemplate();
        return false;
    }
    return true;
}

sync::ChirpConfig OFDMChirpWaveform::getChirpConfig() const {
    sync::ChirpConfig cfg;
    cfg.sample_rate = static_cast<float>(config_.sample_rate);
    cfg.f_start = config_.chirp_f_start;      // 300 Hz (wide) or 1250 Hz (narrow)
    cfg.f_end = config_.chirp_f_end;          // 2700 Hz (wide) or 1750 Hz (narrow)
    cfg.duration_ms = config_.chirp_duration_ms; // 500ms (wide) or 1000ms (narrow)
    cfg.gap_ms = 100.0f;
    cfg.use_dual_chirp = true;  // For CFO estimation
    cfg.tx_cfo_hz = config_.tx_cfo_hz;  // Pass TX CFO for simulation
    return cfg;
}

WaveformCapabilities OFDMChirpWaveform::getCapabilities() const {
    WaveformCapabilities caps;
    caps.supports_cfo_correction = true;    // Via dual chirp
    caps.supports_doppler_correction = true; // OFDM with differential
    caps.requires_pilots = config_.use_pilots;
    caps.supports_differential = true;
    caps.min_snr_db = 10.0f;                // Lower than Schmidl-Cox
    caps.max_snr_db = 20.0f;                // Above this, use OFDM_COX
    caps.max_throughput_bps = getThroughput(CodeRate::R2_3);

    // Chirp preamble + training
    caps.preamble_duration_ms = chirp_sync_ ? (chirp_sync_->getTotalSamples() * 1000.0f / config_.sample_rate)
                                            : 1200.0f;

    return caps;
}

void OFDMChirpWaveform::configurePilotsForCodeRate(CodeRate rate) {
    config_.use_pilots = true;
    config_.pilot_spacing =
        ofdm_link_adaptation::recommendedPilotSpacing(config_.modulation, rate);
}

bool OFDMChirpWaveform::carrierLdpcPlumbingEligible() const {
    return mode_ == protocol::WaveformMode::OFDM_CHIRP &&
           config_.num_carriers == CARRIER_LDPC_MASK_CARRIERS &&
           (config_.fft_size == 512 || config_.fft_size == 1024);
}

bool OFDMChirpWaveform::carrierLdpcCodewordCountSupported(size_t codeword_count) const {
    return codeword_count >= CARRIER_LDPC_MIN_CODEWORDS &&
           codeword_count <= CARRIER_LDPC_MAX_CODEWORDS;
}

void OFDMChirpWaveform::configure(Modulation mod, CodeRate rate) {
    // Allow differential and selected coherent modulations.
    if (!isSupportedChirpModulation(mod)) {
        LOG_MODEM(WARN, "OFDMChirpWaveform: Unsupported modulation %d, using DQPSK",
                  static_cast<int>(mod));
        mod = Modulation::DQPSK;
    }

    config_.modulation = mod;
    config_.code_rate = rate;
    configurePilotsForCodeRate(rate);

    // Reinitialize with new config
    initComponents();

    const int pilot_count = config_.use_pilots
        ? ofdm_link_adaptation::pilotCount(static_cast<int>(config_.num_carriers),
                                           static_cast<int>(config_.pilot_spacing))
        : 0;
    const int data_carriers = ofdm_link_adaptation::dataCarrierCount(
        static_cast<int>(config_.num_carriers),
        config_.use_pilots,
        static_cast<int>(config_.pilot_spacing));

    LOG_MODEM(INFO, "OFDMChirpWaveform: configured for %s %s (%d data, %d pilots)",
              modulationToString(mod), codeRateToString(rate),
              data_carriers, pilot_count);
}

void OFDMChirpWaveform::setFrequencyOffset(float cfo_hz) {
    cfo_hz_ = cfo_hz;
    if (demodulator_) {
        demodulator_->setFrequencyOffset(cfo_hz);
    }
}

void OFDMChirpWaveform::setTxFrequencyOffset(float cfo_hz) {
    // Set TX CFO on chirp sync and modulator for simulation
    config_.tx_cfo_hz = cfo_hz;

    // Reinitialize with new config to apply TX CFO
    initComponents();

    LOG_MODEM(INFO, "OFDMChirpWaveform: TX CFO set to %.1f Hz", cfo_hz);
}

Samples OFDMChirpWaveform::generatePreamble() {
    if (!chirp_sync_ || !modulator_) {
        return Samples();
    }

    // Generate: [CHIRP][TRAINING_SYMBOLS]
    Samples chirp = chirp_sync_->generate();
    Samples training = modulator_->generateTrainingSymbols(2);

    Samples preamble;
    preamble.reserve(chirp.size() + training.size());
    preamble.insert(preamble.end(), chirp.begin(), chirp.end());
    preamble.insert(preamble.end(), training.begin(), training.end());

    return preamble;
}

Samples OFDMChirpWaveform::generateDataPreamble() {
    if (!modulator_) {
        return Samples();
    }

    // Light preamble: just training symbols (no chirp)
    // Saves ~1.2 seconds per frame when already connected
    // Receiver uses known CFO from previous frames
    return modulator_->generateTrainingSymbols(2);
}

Samples OFDMChirpWaveform::modulate(const Bytes& encoded_data) {
    if (!modulator_) {
        return Samples();
    }

    Bytes tx_data = encoded_data;
    uint64_t effective_mask = ALL_ON_CARRIER_MASK;
    bool mask_enabled = false;

    const size_t codeword_count = encoded_data.size() / LDPC_CODEWORD_BYTES;
    const bool full_codewords =
        !encoded_data.empty() && (encoded_data.size() % LDPC_CODEWORD_BYTES == 0);
    const bool eligible = carrierLdpcPlumbingEligible() && full_codewords;
    const bool masked_carriers =
        !isAllOnMask(carrier_mask_, CARRIER_LDPC_MASK_CARRIERS);
    const bool carrier_ldpc_active = eligible &&
        codeword_count != 1 &&
        carrierLdpcCodewordCountSupported(codeword_count) &&
        (carrier_ldpc_interleaver_enabled_ || masked_carriers);

    if (eligible && codeword_count == 1) {
        // Ncw=1 DQPSK can put a one-carrier erasure into only 5 LDPC base
        // columns; sub-phase 3 therefore forces all-on and keeps 1-CW PHY
        // headers/control frames bit-identical to the legacy ordering.
        effective_mask = ALL_ON_CARRIER_MASK;
    } else if (carrier_ldpc_active) {
        // CarrierLDPC is the final TX bit permutation before the air grid.
        // It is mandatory when RX may insert carrier erasures, and remains
        // enabled for legacy explicit mask tests. Direct waveform callers keep
        // the default false flag, preserving deterministic all-on byte identity.
        tx_data = applyCarrierLdpcForward(encoded_data, codeword_count);
        if (masked_carriers) {
            effective_mask = carrier_mask_;
            mask_enabled = true;
        }
    }

    ByteSpan span(tx_data.data(), tx_data.size());
    return modulator_->modulate(span, config_.modulation, effective_mask, mask_enabled);
}

void OFDMChirpWaveform::setCarrierMask(uint64_t active_mask) {
    carrier_mask_ = active_mask;
}

void OFDMChirpWaveform::setCarrierLdpcInterleaverEnabled(bool enabled) {
    carrier_ldpc_interleaver_enabled_ = enabled;
}

bool OFDMChirpWaveform::detectSync(SampleSpan samples, SyncResult& result, float threshold) {
    if (!chirp_sync_) {
        return false;
    }

    burst_interleave_latched_ = false;
    burst_interleaved_detected_ = false;

    // Use dual chirp detection for CFO-tolerant sync
    auto chirp_result = chirp_sync_->detectDualChirp(samples, threshold);

    result.detected = chirp_result.success;
    result.correlation = std::max(chirp_result.up_correlation, chirp_result.down_correlation);
    result.cfo_hz = chirp_result.cfo_hz;
    result.gap_error_samples = chirp_result.gap_error_samples;
    result.has_training = true;

    if (chirp_result.success) {
        synced_ = true;
        last_cfo_ = chirp_result.cfo_hz;

        // Calculate where TRAINING starts (process() needs training for channel estimation)
        // Layout: [UP-CHIRP][GAP][DOWN-CHIRP][GAP][TRAINING_SYMBOLS][DATA...]
        //                                         ^-- start_sample points here
        //
        // IMPORTANT: Use down_chirp position for training_start calculation!
        // With CFO, up_chirp and down_chirp positions shift in OPPOSITE directions:
        //   up_chirp: shifts by -CFO × cfo_to_samples
        //   down_chirp: shifts by +CFO × cfo_to_samples
        // Using up_chirp_start + fixed_offset gives growing error with CFO.
        // Using down_chirp_start gives more accurate training position.
        size_t chirp_samples = chirp_sync_->getChirpSamples();
        size_t gap_samples = static_cast<size_t>(config_.sample_rate * 100.0f / 1000.0f);

        // Training starts after down chirp + gap
        result.start_sample = chirp_result.down_chirp_start +
                              chirp_samples + gap_samples;
        // NOTE: Do NOT add training_samples - process() needs them for channel estimation

        // Store training start position for CFO phase calculation in process()
        training_start_sample_ = result.start_sample;

        const int symbol_samples = getSamplesPerSymbol();
        const size_t training_start = static_cast<size_t>(result.start_sample);
        if (symbol_samples > 0 &&
            training_start + static_cast<size_t>(symbol_samples * 2) <= samples.size()) {
            data_sync_hilbert_.reset();
            auto& analytic = data_sync_analytic_scratch_;
            data_sync_hilbert_.process(samples, analytic);

            if (training_start + static_cast<size_t>(symbol_samples * 2) <= analytic.size()) {
                Complex marker_p(0.0f, 0.0f);
                for (int n = 0; n < symbol_samples; ++n) {
                    const size_t idx1 = training_start + static_cast<size_t>(n);
                    const size_t idx2 = idx1 + static_cast<size_t>(symbol_samples);
                    marker_p += std::conj(analytic[idx1]) * analytic[idx2];
                }

                const float cfo_phase =
                    2.0f * static_cast<float>(M_PI) * chirp_result.cfo_hz *
                    static_cast<float>(symbol_samples) / config_.sample_rate;
                const Complex cfo_comp(std::cos(-cfo_phase), std::sin(-cfo_phase));
                const Complex marker_metric = marker_p * cfo_comp;
                burst_interleaved_detected_ = (marker_metric.real() < 0.0f);
                burst_interleave_latched_ = burst_interleaved_detected_;
            }
        }

        LOG_MODEM(INFO,
                  "OFDMChirpWaveform: Chirp detected at %d, CFO=%.1f Hz, training_start=%d%s",
                  chirp_result.up_chirp_start, chirp_result.cfo_hz, result.start_sample,
                  burst_interleaved_detected_ ? " [BURST-INTERLEAVED]" : "");
    }

    return result.detected;
}

bool OFDMChirpWaveform::detectDataSync(SampleSpan samples, SyncResult& result,
                                        float known_cfo_hz, float threshold) {
    timing::ScopedTimer _profile_(timing::globalDecoderProfile().detect_data_sync);
    // Detect training-only preamble (no chirp) for DATA frames
    // Uses Schmidl-Cox style detection: LTS has two identical symbols,
    // so we correlate sample[n] with sample[n + symbol_length]

    result.detected = false;
    result.correlation = 0.0f;
    result.cfo_hz = known_cfo_hz;  // Use known CFO from previous frames
    result.has_training = true;

    const int symbol_samples = getSamplesPerSymbol();
    const int search_window = symbol_samples * 4;  // Search first 4 symbols worth

    if (samples.size() < static_cast<size_t>(symbol_samples * 3)) {
        return false;  // Need at least 3 symbols (2 LTS + margin)
    }

    // Energy-based gate: find where signal starts
    // When the buffer starts with silence (noise_floor low), the energy gate efficiently
    // skips the quiet region. When the buffer starts with signal (burst continuation,
    // noise_floor high), the energy gate can't find a transition, so we search the
    // full buffer instead — the LTS autocorrelation peak is distinctive enough to
    // stand out from data autocorrelation.
    float noise_floor = 0.0f;
    size_t noise_samples = std::min(samples.size() / 4, size_t(4800));  // First 100ms
    for (size_t i = 0; i < noise_samples; ++i) {
        noise_floor += samples[i] * samples[i];
    }
    noise_floor = std::sqrt(noise_floor / noise_samples);
    float energy_threshold = noise_floor * 3.0f + 0.01f;  // 3x noise or minimum

    size_t signal_start = 0;
    bool signal_in_noise = (noise_floor < 0.05f);  // Buffer starts with silence

    if (signal_in_noise) {
        // Find first sample above energy threshold (skip silence region)
        for (size_t i = 0; i < samples.size() - symbol_samples * 2; ++i) {
            float energy = 0.0f;
            for (int j = 0; j < 64; ++j) {  // Check 64 samples
                if (i + j < samples.size()) {
                    energy += samples[i + j] * samples[i + j];
                }
            }
            energy = std::sqrt(energy / 64);
            if (energy > energy_threshold) {
                signal_start = i;
                break;
            }
        }
    }
    // If !signal_in_noise: signal_start stays 0, search entire buffer

    // CFO-aware Schmidl-Cox style autocorrelation for LTS detection.
    // Use analytic signal (Hilbert) so correlation magnitude is robust to
    // carrier/CFO phase rotation and avoids real-only sign collapse.
    data_sync_hilbert_.reset();
    auto& analytic = data_sync_analytic_scratch_;
    data_sync_hilbert_.process(samples, analytic);
    if (analytic.size() < static_cast<size_t>(symbol_samples * 3)) {
        return false;
    }

    const bool matched_filter_ready = ensureDataSyncTemplate(symbol_samples);
    const bool matched_filter_can_drive_lock = threshold < 0.50f;
    const int lts_pair_samples = symbol_samples * 2;

    struct DataSyncCandidate {
        float combined_corr = 0.0f;
        float schmidl_corr = 0.0f;
        float matched_corr = 0.0f;
        Complex schmidl_p = Complex(0.0f, 0.0f);
    };

    auto evaluate_offset = [&](int offset) -> DataSyncCandidate {
        DataSyncCandidate candidate;
        if (offset < 0 ||
            offset + lts_pair_samples > static_cast<int>(analytic.size())) {
            return candidate;
        }

        // LTS has 2 identical symbols, so correlate with 1 symbol delay.
        Complex P(0.0f, 0.0f);
        float energy1 = 0.0f;
        float energy2 = 0.0f;

        for (int n = 0; n < symbol_samples; ++n) {
            const int idx1 = offset + n;
            const int idx2 = offset + n + symbol_samples;
            const Complex& s1 = analytic[idx1];
            const Complex& s2 = analytic[idx2];
            P += std::conj(s1) * s2;
            energy1 += std::norm(s1);
            energy2 += std::norm(s2);
        }

        const float schmidl_denom = std::sqrt(energy1 * energy2) + 1e-10f;
        candidate.schmidl_corr = std::abs(P) / schmidl_denom;
        candidate.schmidl_p = P;

        if (matched_filter_ready &&
            data_sync_template_analytic_.size() == static_cast<size_t>(lts_pair_samples)) {
            Complex first_symbol_corr(0.0f, 0.0f);
            Complex second_symbol_corr(0.0f, 0.0f);
            float rx_energy = 0.0f;

            const float phase_inc = -2.0f * static_cast<float>(M_PI) *
                                    known_cfo_hz / config_.sample_rate;
            float phase = 0.0f;
            for (int n = 0; n < lts_pair_samples; ++n) {
                const Complex cfo_correction(std::cos(phase), std::sin(phase));
                const Complex rx = analytic[offset + n] * cfo_correction;
                const Complex& ref = data_sync_template_analytic_[static_cast<size_t>(n)];
                const Complex corr = std::conj(ref) * rx;
                if (n < symbol_samples) {
                    first_symbol_corr += corr;
                } else {
                    second_symbol_corr += corr;
                }
                rx_energy += std::norm(rx);
                phase += phase_inc;
            }

            const float matched_denom =
                std::sqrt(data_sync_template_energy_ * rx_energy) + 1e-10f;
            const float normal_score =
                std::abs(first_symbol_corr + second_symbol_corr) / matched_denom;
            const float marker_score =
                std::abs(-first_symbol_corr + second_symbol_corr) / matched_denom;
            candidate.matched_corr = std::max(normal_score, marker_score);
        }

        // The matched filter is allowed to lower the effective detection floor
        // only in the narrowed expected-arrival path. In the wide/cold fallback
        // window, keep legacy Schmidl-Cox acceptance semantics so payload data
        // cannot lock on a matched-only LTS-like projection.
        candidate.combined_corr = matched_filter_can_drive_lock
            ? std::max(candidate.schmidl_corr, candidate.matched_corr)
            : candidate.schmidl_corr;
        return candidate;
    };

    float best_corr = 0.0f;
    float best_schmidl_corr = 0.0f;
    float best_matched_corr = 0.0f;
    int best_offset = 0;
    Complex best_p(0.0f, 0.0f);

    auto adopt_candidate = [&](int offset, const DataSyncCandidate& candidate) {
        best_corr = candidate.combined_corr;
        best_schmidl_corr = candidate.schmidl_corr;
        best_matched_corr = candidate.matched_corr;
        best_offset = offset;
        best_p = candidate.schmidl_p;
    };

    // Keep search local to expected frame start. Scanning the full buffer makes
    // payload autocorrelation peaks compete with LTS and increases false locks.
    int max_connected_search = std::max(search_window, symbol_samples * 8);
    int actual_search_window = signal_in_noise ? search_window : max_connected_search;
    int search_end = std::min(static_cast<int>(signal_start) + actual_search_window,
                              static_cast<int>(samples.size()) - symbol_samples * 2);

    for (int offset = static_cast<int>(signal_start);
         offset < search_end; offset += 8) {  // Step by 8 for speed

        const DataSyncCandidate candidate = evaluate_offset(offset);
        if (candidate.combined_corr > best_corr) {
            adopt_candidate(offset, candidate);
        }

        // Early exit on first high-confidence peak. The LTS (two identical training
        // symbols) is always the FIRST pair of identical symbols in the frame. With
        // 1-CW LDPC zero-padding, data symbols can also be identical (all-zero bits →
        // 0° DQPSK phase change), creating false peaks later in the search window.
        // By stopping at the first peak above 0.95, we always lock onto the real LTS.
        if (candidate.combined_corr > 0.95f) {
            break;
        }
    }

    // Fine refinement: 1-sample steps around coarse peak.
    // The 8-sample coarse search can be up to 4 samples off-peak.
    // For QPSK, even 4-sample offset causes ~40° phase error at edge carriers.
    // Cost: 9 evaluations × symbol_samples MACs — negligible.
    if (best_corr > threshold) {
        int refine_start = std::max(static_cast<int>(signal_start), best_offset - 4);
        int refine_end = std::min(search_end, best_offset + 5);  // exclusive

        for (int offset = refine_start; offset < refine_end; ++offset) {
            if (offset == best_offset) continue;  // Skip already-evaluated

            const DataSyncCandidate candidate = evaluate_offset(offset);
            if (candidate.combined_corr > best_corr) {
                adopt_candidate(offset, candidate);
            }
        }
    }

    // If a weak candidate lands at the edge of the capped connected search,
    // the real LTS can be just beyond the cap in the same streaming buffer.
    // Do one bounded rescue pass only for sub-0.45 edge candidates, preserving
    // the low-SNR relaxed floor when no stronger peak is present.
    constexpr float kWeakConnectedPeakCorr = 0.45f;
    constexpr float kMinLaterPeakImprovement = 0.02f;
    constexpr int kEdgeCandidateMarginSamples = 16;
    const bool weak_edge_candidate =
        best_corr > threshold &&
        best_corr < kWeakConnectedPeakCorr &&
        best_offset + kEdgeCandidateMarginSamples >= search_end;
    if (weak_edge_candidate) {
        const int extended_search_end =
            static_cast<int>(samples.size()) - symbol_samples * 2;
        const int original_best_offset = best_offset;
        const float original_best_corr = best_corr;
        const float original_best_schmidl_corr = best_schmidl_corr;
        const float original_best_matched_corr = best_matched_corr;
        Complex original_best_p = best_p;

        for (int offset = search_end; offset < extended_search_end; offset += 8) {
            const DataSyncCandidate candidate = evaluate_offset(offset);
            if (candidate.combined_corr > best_corr + kMinLaterPeakImprovement) {
                adopt_candidate(offset, candidate);
            }
        }

        if (best_offset != original_best_offset) {
            int refine_start = std::max(search_end, best_offset - 4);
            int refine_end = std::min(extended_search_end, best_offset + 5);

            for (int offset = refine_start; offset < refine_end; ++offset) {
                if (offset == best_offset) continue;

                const DataSyncCandidate candidate = evaluate_offset(offset);
                if (candidate.combined_corr > best_corr) {
                    adopt_candidate(offset, candidate);
                }
            }

            LOG_MODEM(INFO, "OFDMChirpWaveform: Data sync replaced weak peak at %d (corr=%.2f) "
                      "with later peak at %d (corr=%.2f)",
                      original_best_offset, original_best_corr, best_offset, best_corr);
        } else {
            best_corr = original_best_corr;
            best_schmidl_corr = original_best_schmidl_corr;
            best_matched_corr = original_best_matched_corr;
            best_offset = original_best_offset;
            best_p = original_best_p;
        }
    }

    result.correlation = best_corr;

    // Reset latched marker at start of each detection attempt
    burst_interleave_latched_ = false;
    burst_interleaved_detected_ = false;

    if (best_corr > threshold) {
        result.detected = true;
        result.start_sample = best_offset;  // Training starts here
        training_start_sample_ = best_offset;
        synced_ = true;
        last_cfo_ = known_cfo_hz;

        // Check LTS sign for burst interleave marker.
        // Compensate expected CFO phase rotation between repeated halves:
        // phase = 2*pi*CFO*L/fs. Negated first LTS then appears as negative real part.
        float cfo_phase = 2.0f * M_PI * known_cfo_hz * symbol_samples / config_.sample_rate;
        Complex cfo_comp(std::cos(-cfo_phase), std::sin(-cfo_phase));
        Complex marker_metric = best_p * cfo_comp;
        burst_interleaved_detected_ = (marker_metric.real() < 0.0f);
        burst_interleave_latched_ = burst_interleaved_detected_;

        LOG_MODEM(INFO,
                  "OFDMChirpWaveform: Data sync detected at %d, corr=%.2f (sc=%.2f mf=%.2f), using CFO=%.1f Hz%s",
                  best_offset, best_corr, best_schmidl_corr, best_matched_corr, known_cfo_hz,
                  burst_interleaved_detected_ ? " [BURST-INTERLEAVED]" : "");
    }

    return result.detected;
}

void OFDMChirpWaveform::setAbsoluteTrainingPosition(size_t pos) {
    absolute_training_start_sample_ = pos;
    has_absolute_training_start_sample_ = true;
}

bool OFDMChirpWaveform::process(SampleSpan samples) {
    timing::ScopedTimer _profile_(timing::globalDecoderProfile().ofdm_process_total);
    if (!demodulator_) {
        return false;
    }

    // Calculate the initial CFO phase based on elapsed samples since audio start
    // The test harness applies CFO to the entire audio stream from sample 0.
    // By the time we reach training_start_sample_, the CFO has accumulated:
    //   phase = -2π × CFO × elapsed_samples / sample_rate
    //
    // We need to start CFO correction from this accumulated phase, not from 0.
    size_t phase_ref_sample = training_start_sample_;
    if (has_absolute_training_start_sample_) {
        phase_ref_sample = absolute_training_start_sample_;
    }

    float initial_phase_rad = -2.0f * M_PI * cfo_hz_ * phase_ref_sample / config_.sample_rate;

    // Wrap to [-π, π]
    while (initial_phase_rad > M_PI) initial_phase_rad -= 2.0f * M_PI;
    while (initial_phase_rad < -M_PI) initial_phase_rad += 2.0f * M_PI;

    LOG_MODEM(INFO, "OFDMChirpWaveform::process(): samples=%zu, cfo=%.1f, training_start=%zu, abs_start=%zu",
              samples.size(), cfo_hz_, training_start_sample_,
              has_absolute_training_start_sample_ ? absolute_training_start_sample_ : 0);

    // Pass CFO and initial phase to demodulator
    // This ensures CFO correction starts from the correct accumulated phase
    demodulator_->setFrequencyOffsetWithPhase(cfo_hz_, initial_phase_rad);

    // If burst interleave marker was detected, undo LTS negation before channel estimation.
    // The TX negated the first LTS symbol as a marker. We must restore it so the
    // demodulator sees correct training data for channel estimation.
    // ONE-SHOT: consume the flag immediately so continuation frames aren't affected.
    bool ready;
    if (burst_interleaved_detected_) {
        burst_interleaved_detected_ = false;  // One-shot: consume before continuation frames

        // Create mutable copy and negate first LTS symbol
        std::vector<float> modified(samples.begin(), samples.end());
        size_t lts_sym_len = static_cast<size_t>(getSamplesPerSymbol());
        for (size_t i = 0; i < lts_sym_len && i < modified.size(); i++) {
            modified[i] = -modified[i];
        }

        ready = demodulator_->processPresynced(SampleSpan(modified), 2);
    } else {
        // Normal path (no burst marker)
        ready = demodulator_->processPresynced(samples, 2);
    }

    if (ready) {
        // Retrieve ALL soft bits from demodulator
        // getSoftBits() returns LDPC_BLOCK_SIZE (648) bits at a time,
        // so we need to call it multiple times to get all available bits
        soft_bits_.clear();
        while (demodulator_->hasPendingData()) {
            auto chunk = demodulator_->getSoftBits();
            if (chunk.size() != LDPC_CODEWORD_BITS) break;
            soft_bits_.insert(soft_bits_.end(), chunk.begin(), chunk.end());
        }

        const size_t codeword_count = soft_bits_.size() / LDPC_CODEWORD_BITS;
        const bool eligible = carrierLdpcPlumbingEligible() &&
            !soft_bits_.empty() &&
            (soft_bits_.size() % LDPC_CODEWORD_BITS == 0);
        const bool masked_carriers =
            !isAllOnMask(carrier_mask_, CARRIER_LDPC_MASK_CARRIERS);
        const bool carrier_ldpc_active = eligible &&
            codeword_count != 1 &&
            carrierLdpcCodewordCountSupported(codeword_count) &&
            (carrier_ldpc_interleaver_enabled_ || masked_carriers);
        if (eligible && codeword_count == 1) {
            // Ncw=1 remains legacy ordered; this includes the future 1-CW
            // R1/4 PHY mask header, which must stay bit-identical.
        } else if (carrier_ldpc_active) {
            std::vector<float> air_llrs = std::move(soft_bits_);
            if (masked_carriers) {
                air_llrs = eraseMaskedCarrierLLRs(std::move(air_llrs),
                                                  config_, carrier_mask_);
            }
            soft_bits_ = applyCarrierLdpcInverse(std::move(air_llrs), codeword_count);
        }

        last_snr_ = demodulator_->getEstimatedSNR();

        // Feed back pilot-corrected CFO from demodulator
        // On fading channels, chirp-based CFO can be wrong. The demodulator's
        // pilot tracking and LTS residual estimation correct it. Propagate
        // this correction back so subsequent frames use the refined CFO.
        float corrected_cfo = demodulator_->getFrequencyOffset();
        if (std::abs(corrected_cfo - cfo_hz_) > 0.1f) {
            LOG_MODEM(INFO, "OFDMChirpWaveform: CFO feedback: chirp=%.2f -> corrected=%.2f Hz",
                      cfo_hz_, corrected_cfo);
        }
        cfo_hz_ = corrected_cfo;
        last_cfo_ = corrected_cfo;
    }

    return ready;
}

Symbol OFDMChirpWaveform::getLastDataCarrierSymbolsForTesting() const {
    return modulator_ ? modulator_->getLastDataCarrierSymbolsForTesting() : Symbol{};
}

std::vector<float> OFDMChirpWaveform::getSoftBits() {
    return std::move(soft_bits_);
}

void OFDMChirpWaveform::setRXCarrierErasureEnabled(bool enabled) {
    rx_carrier_erasure_enabled_ = enabled;
    if (demodulator_) {
        demodulator_->setRXCarrierErasureEnabled(enabled);
    }
}

void OFDMChirpWaveform::reset() {
    if (demodulator_) {
        demodulator_->reset();
    }
    soft_bits_.clear();
    synced_ = false;
    has_absolute_training_start_sample_ = false;
    absolute_training_start_sample_ = 0;
    // NOTE: CFO is intentionally preserved across reset() for continuous tracking
    // Use setFrequencyOffset(0) to explicitly clear if needed
    // TX CFO in config_ is also preserved for simulation
}

bool OFDMChirpWaveform::isSynced() const {
    return synced_ || (demodulator_ && demodulator_->isSynced());
}

bool OFDMChirpWaveform::hasData() const {
    return !soft_bits_.empty() || (demodulator_ && demodulator_->hasPendingData());
}

float OFDMChirpWaveform::estimatedSNR() const {
    if (demodulator_) {
        return demodulator_->getEstimatedSNR();
    }
    return last_snr_;
}

bool OFDMChirpWaveform::hasLastOFDMBroadbandSNREstimate() const {
    return demodulator_ && demodulator_->hasLastOFDMBroadbandSNREstimate();
}

float OFDMChirpWaveform::getLastOFDMBroadbandSNREstimate() const {
    if (demodulator_) {
        return demodulator_->getLastOFDMBroadbandSNREstimate();
    }
    return 0.0f;
}

float OFDMChirpWaveform::estimatedCFO() const {
    if (std::abs(last_cfo_) > 0.1f) {
        return last_cfo_;
    }
    if (demodulator_) {
        return demodulator_->getFrequencyOffset();
    }
    return cfo_hz_;
}

float OFDMChirpWaveform::getFadingIndex() const {
    if (demodulator_) {
        return demodulator_->getFadingIndex();
    }
    return 0.0f;
}

float OFDMChirpWaveform::getLastTimingOffsetSamples() const {
    if (demodulator_) {
        return demodulator_->getLastTimingOffsetSamples();
    }
    return 0.0f;
}

float OFDMChirpWaveform::getLastLTSSignalPower() const {
    if (demodulator_) {
        return demodulator_->getLastLTSSignalPower();
    }
    return 1.0f;
}

float OFDMChirpWaveform::getLastLTSChannelMagnitude() const {
    if (demodulator_) {
        return demodulator_->getLastLTSChannelMagnitude();
    }
    return 1.0f;
}

float OFDMChirpWaveform::getLastLTSResidualCFOHz() const {
    if (demodulator_) {
        return demodulator_->getLastLTSResidualCFOHz();
    }
    return 0.0f;
}

std::vector<std::complex<float>> OFDMChirpWaveform::getConstellationSymbols() const {
    if (demodulator_) {
        return demodulator_->getConstellationSymbols();
    }
    return {};
}

std::string OFDMChirpWaveform::getStatusString() const {
    std::ostringstream oss;
    oss << "OFDM-Chirp " << config_.num_carriers << " carriers, "
        << modulationToString(config_.modulation) << " "
        << codeRateToString(config_.code_rate);
    if (std::abs(last_cfo_) > 0.5f) {
        oss << " (CFO=" << static_cast<int>(last_cfo_) << " Hz)";
    }
    return oss.str();
}

float OFDMChirpWaveform::getThroughput(CodeRate rate) const {
    // Calculate data carriers based on pilot configuration for the given rate.
    const int pilot_spacing =
        ofdm_link_adaptation::recommendedPilotSpacing(config_.modulation, rate);
    const int data_carriers = ofdm_link_adaptation::dataCarrierCount(
        static_cast<int>(config_.num_carriers), true, pilot_spacing);
    const int bits_per_carrier = static_cast<int>(getBitsPerSymbol(config_.modulation));

    // Symbol rate
    float symbol_rate = static_cast<float>(config_.sample_rate) / getSamplesPerSymbol();

    // Raw bit rate
    float raw_bps = symbol_rate * data_carriers * bits_per_carrier;

    return raw_bps * getCodeRateValue(rate);
}

int OFDMChirpWaveform::getSamplesPerSymbol() const {
    if (modulator_) {
        return static_cast<int>(modulator_->samplesPerSymbol());
    }
    // Fallback calculation
    int cp_samples = 0;
    switch (config_.cp_mode) {
        case CyclicPrefixMode::SHORT:  cp_samples = config_.fft_size / 8; break;
        case CyclicPrefixMode::MEDIUM: cp_samples = config_.fft_size / 4; break;
        case CyclicPrefixMode::LONG:   cp_samples = config_.fft_size / 2; break;
    }
    return config_.fft_size + cp_samples;
}

int OFDMChirpWaveform::getPreambleSamples() const {
    int chirp_total = chirp_sync_ ? static_cast<int>(chirp_sync_->getTotalSamples())
                                  : static_cast<int>(config_.sample_rate * 1.2f);  // ~1.2 sec default
    int training = 2 * getSamplesPerSymbol();  // 2 OFDM training symbols
    return chirp_total + training;
}

int OFDMChirpWaveform::getDataPreambleSamples() const {
    // Light preamble: just training symbols (no chirp)
    // 2 LTS symbols for channel estimation
    return 2 * getSamplesPerSymbol();
}

int OFDMChirpWaveform::getMinSamplesForFrame() const {
    return getMinSamplesForCWCount(4);
}

int OFDMChirpWaveform::getMinSamplesForControlFrame() const {
    return getMinSamplesForCWCount(1);
}

int OFDMChirpWaveform::getMinSamplesForCWCount(int num_cw) const {
    // Training symbols + data for num_cw codewords
    int training_samples = 2 * getSamplesPerSymbol();  // 2 OFDM training symbols

    int frame_bits = num_cw * 648;

    const int bits_per_symbol = ofdm_link_adaptation::bitsPerOFDMSymbol(
        static_cast<int>(config_.num_carriers),
        config_.use_pilots,
        static_cast<int>(config_.pilot_spacing),
        config_.modulation);
    if (bits_per_symbol <= 0) {
        return training_samples;
    }
    int data_symbols = (frame_bits + bits_per_symbol - 1) / bits_per_symbol;
    int data_samples = data_symbols * getSamplesPerSymbol();

    return training_samples + data_samples;
}

} // namespace ultra
