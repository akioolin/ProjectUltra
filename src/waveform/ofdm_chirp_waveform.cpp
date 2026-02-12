// OFDMChirpWaveform - Implementation

#include "ofdm_chirp_waveform.hpp"
#include "ultra/logging.hpp"
#include "ultra/dsp.hpp"  // FFT class is in here
#include "ultra/ofdm_link_adaptation.hpp"
#include <sstream>
#include <cmath>

namespace ultra {

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
    // Allow differential and coherent modulations for chirp mode
    if (config_.modulation != Modulation::DBPSK &&
        config_.modulation != Modulation::DQPSK &&
        config_.modulation != Modulation::D8PSK &&
        config_.modulation != Modulation::QPSK &&
        config_.modulation != Modulation::BPSK) {
        config_.modulation = Modulation::DQPSK;
    }
    configurePilotsForCodeRate(config_.code_rate);
    initComponents();
}

void OFDMChirpWaveform::initComponents() {
    modulator_ = std::make_unique<OFDMModulator>(config_);
    demodulator_ = std::make_unique<OFDMDemodulator>(config_);
    chirp_sync_ = std::make_unique<sync::ChirpSync>(getChirpConfig());
}

sync::ChirpConfig OFDMChirpWaveform::getChirpConfig() const {
    sync::ChirpConfig cfg;
    cfg.sample_rate = static_cast<float>(config_.sample_rate);
    cfg.f_start = 300.0f;
    cfg.f_end = 2700.0f;
    cfg.duration_ms = 500.0f;
    cfg.gap_ms = 100.0f;
    cfg.use_dual_chirp = true;  // For CFO estimation
    cfg.tx_cfo_hz = config_.tx_cfo_hz;  // Pass TX CFO for simulation
    return cfg;
}

WaveformCapabilities OFDMChirpWaveform::getCapabilities() const {
    WaveformCapabilities caps;
    caps.supports_cfo_correction = true;    // Via dual chirp
    caps.supports_doppler_correction = true; // OFDM with differential
    caps.requires_pilots = false;            // Differential modulation
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

void OFDMChirpWaveform::configure(Modulation mod, CodeRate rate) {
    // Allow differential and coherent modulations
    if (mod != Modulation::DBPSK && mod != Modulation::DQPSK && mod != Modulation::D8PSK &&
        mod != Modulation::QPSK && mod != Modulation::BPSK) {
        LOG_MODEM(WARN, "OFDMChirpWaveform: Unsupported modulation %d, using DQPSK",
                  static_cast<int>(mod));
        mod = Modulation::DQPSK;
    }

    config_.modulation = mod;
    config_.code_rate = rate;
    configurePilotsForCodeRate(rate);

    // Reinitialize with new config
    initComponents();

    int pilot_count = 0;
    if (config_.use_pilots) {
        pilot_count = (config_.num_carriers + config_.pilot_spacing - 1) / config_.pilot_spacing;
    }
    int data_carriers = config_.num_carriers - pilot_count;

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
    ByteSpan span(encoded_data.data(), encoded_data.size());
    return modulator_->modulate(span, config_.modulation);
}

bool OFDMChirpWaveform::detectSync(SampleSpan samples, SyncResult& result, float threshold) {
    if (!chirp_sync_) {
        return false;
    }

    // Use dual chirp detection for CFO-tolerant sync
    auto chirp_result = chirp_sync_->detectDualChirp(samples, threshold);

    result.detected = chirp_result.success;
    result.correlation = std::max(chirp_result.up_correlation, chirp_result.down_correlation);
    result.cfo_hz = chirp_result.cfo_hz;
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

        LOG_MODEM(INFO, "OFDMChirpWaveform: Chirp detected at %d, CFO=%.1f Hz, training_start=%d",
                  chirp_result.up_chirp_start, chirp_result.cfo_hz, result.start_sample);
    }

    return result.detected;
}

bool OFDMChirpWaveform::detectDataSync(SampleSpan samples, SyncResult& result,
                                        float known_cfo_hz, float threshold) {
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
    HilbertTransform hilbert(65);
    auto analytic = hilbert.process(samples);
    if (analytic.size() < static_cast<size_t>(symbol_samples * 3)) {
        return false;
    }

    // LTS has 2 identical symbols, so correlate with 1 symbol delay
    float best_corr = 0.0f;
    int best_offset = 0;
    Complex best_p(0.0f, 0.0f);

    // When buffer starts with signal (burst continuation), use wider search window
    // to find the LTS peak anywhere in the buffer. LTS autocorrelation (~0.99) is
    // much higher than random data autocorrelation (~0.2-0.4).
    int actual_search_window = signal_in_noise ? search_window : static_cast<int>(samples.size());
    int search_end = std::min(static_cast<int>(signal_start) + actual_search_window,
                              static_cast<int>(samples.size()) - symbol_samples * 2);

    for (int offset = static_cast<int>(signal_start);
         offset < search_end; offset += 8) {  // Step by 8 for speed

        // Correlate: P = sum(conj(s1[n]) * s2[n])
        Complex P(0.0f, 0.0f);
        float energy1 = 0.0f, energy2 = 0.0f;

        for (int n = 0; n < symbol_samples; ++n) {
            int idx1 = offset + n;
            int idx2 = offset + n + symbol_samples;
            if (idx2 >= static_cast<int>(analytic.size())) break;

            const Complex& s1 = analytic[idx1];
            const Complex& s2 = analytic[idx2];
            P += std::conj(s1) * s2;
            energy1 += std::norm(s1);
            energy2 += std::norm(s2);
        }

        float denom = std::sqrt(energy1 * energy2) + 1e-10f;
        float corr = std::abs(P) / denom;

        if (corr > best_corr) {
            best_corr = corr;
            best_offset = offset;
            best_p = P;
        }

        // Early exit on first high-confidence peak. The LTS (two identical training
        // symbols) is always the FIRST pair of identical symbols in the frame. With
        // 1-CW LDPC zero-padding, data symbols can also be identical (all-zero bits →
        // 0° DQPSK phase change), creating false peaks later in the search window.
        // By stopping at the first peak above 0.95, we always lock onto the real LTS.
        if (corr > 0.95f) {
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

            Complex P(0.0f, 0.0f);
            float energy1 = 0.0f, energy2 = 0.0f;

            for (int n = 0; n < symbol_samples; ++n) {
                int idx1 = offset + n;
                int idx2 = offset + n + symbol_samples;
                if (idx2 >= static_cast<int>(analytic.size())) break;

                const Complex& s1 = analytic[idx1];
                const Complex& s2 = analytic[idx2];
                P += std::conj(s1) * s2;
                energy1 += std::norm(s1);
                energy2 += std::norm(s2);
            }

            float denom = std::sqrt(energy1 * energy2) + 1e-10f;
            float corr = std::abs(P) / denom;

            if (corr > best_corr) {
                best_corr = corr;
                best_offset = offset;
                best_p = P;
            }
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

        LOG_MODEM(INFO, "OFDMChirpWaveform: Data sync detected at %d, corr=%.2f, using CFO=%.1f Hz%s",
                  best_offset, best_corr, known_cfo_hz,
                  burst_interleaved_detected_ ? " [BURST-INTERLEAVED]" : "");
    }

    return result.detected;
}

void OFDMChirpWaveform::setAbsoluteTrainingPosition(size_t pos) {
    absolute_training_start_sample_ = pos;
    has_absolute_training_start_sample_ = true;
}

bool OFDMChirpWaveform::process(SampleSpan samples) {
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
            if (chunk.empty()) break;
            soft_bits_.insert(soft_bits_.end(), chunk.begin(), chunk.end());
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

std::vector<float> OFDMChirpWaveform::getSoftBits() {
    return std::move(soft_bits_);
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
    int bits_per_carrier = 2;  // Default DQPSK/QPSK
    switch (config_.modulation) {
        case Modulation::DBPSK: bits_per_carrier = 1; break;
        case Modulation::BPSK:  bits_per_carrier = 1; break;
        case Modulation::DQPSK: bits_per_carrier = 2; break;
        case Modulation::QPSK:  bits_per_carrier = 2; break;
        case Modulation::D8PSK: bits_per_carrier = 3; break;
        default: bits_per_carrier = 2; break;
    }

    // Calculate data carriers based on pilot configuration for the given rate.
    const int pilot_spacing =
        ofdm_link_adaptation::recommendedPilotSpacing(config_.modulation, rate);
    const int pilot_count =
        ofdm_link_adaptation::pilotCount(static_cast<int>(config_.num_carriers), pilot_spacing);
    int data_carriers = config_.num_carriers - pilot_count;

    // Symbol rate
    float symbol_rate = static_cast<float>(config_.sample_rate) / getSamplesPerSymbol();

    // Raw bit rate
    float raw_bps = symbol_rate * data_carriers * bits_per_carrier;

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

    int bits_per_carrier = 2;  // DQPSK/QPSK
    switch (config_.modulation) {
        case Modulation::DBPSK: bits_per_carrier = 1; break;
        case Modulation::BPSK:  bits_per_carrier = 1; break;
        case Modulation::DQPSK: bits_per_carrier = 2; break;
        case Modulation::QPSK:  bits_per_carrier = 2; break;
        case Modulation::D8PSK: bits_per_carrier = 3; break;
        default: bits_per_carrier = 2; break;
    }

    // Account for pilots reducing available data carriers
    int pilot_count = 0;
    if (config_.use_pilots) {
        pilot_count = (config_.num_carriers + config_.pilot_spacing - 1) / config_.pilot_spacing;
    }
    int data_carriers = static_cast<int>(config_.num_carriers) - pilot_count;

    int bits_per_symbol = data_carriers * bits_per_carrier;
    int data_symbols = (frame_bits + bits_per_symbol - 1) / bits_per_symbol;
    int data_samples = data_symbols * getSamplesPerSymbol();

    return training_samples + data_samples;
}

} // namespace ultra
