// OFDM demodulator public interface and stream processing

#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
#include "ultra/ofdm.hpp"
#include "ultra/dsp.hpp"
#include "ultra/logging.hpp"
#include "ultra/timing_profiler.hpp"
#include "demodulator_impl.hpp"
#include "demodulator_constants.hpp"

namespace ultra {

using namespace demod_constants;

// =============================================================================
// PUBLIC INTERFACE
// =============================================================================

OFDMDemodulator::OFDMDemodulator(const ModemConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

OFDMDemodulator::~OFDMDemodulator() = default;

bool OFDMDemodulator::process(SampleSpan samples) {
    // Add to buffer
    impl_->rx_buffer.insert(impl_->rx_buffer.end(), samples.begin(), samples.end());

    // Preamble size constants
    size_t preamble_symbol_len = impl_->config.fft_size + impl_->config.getCyclicPrefix();
    size_t preamble_total_len = preamble_symbol_len * 6;
    size_t correlation_window = preamble_symbol_len * 2;

    // ========================================================================
    // SEARCHING STATE
    // ========================================================================
    if (impl_->state.load() == Impl::State::SEARCHING) {
        if (impl_->rx_buffer.size() < MIN_SEARCH_SAMPLES) {
            return false;
        }

        // Buffer overflow protection
        if (impl_->rx_buffer.size() > MAX_BUFFER_SAMPLES) {
            size_t keep = OVERLAP_SAMPLES;
            impl_->rx_buffer.erase(impl_->rx_buffer.begin(),
                                   impl_->rx_buffer.end() - keep);
            LOG_SYNC(WARN, "Buffer overflow, trimmed to %zu samples", keep);
        }

        // Search for preamble
        bool found_sync = false;
        size_t sync_offset = 0;
        float sync_corr = 0;

        size_t search_end = (impl_->rx_buffer.size() > preamble_total_len + correlation_window)
                          ? impl_->rx_buffer.size() - preamble_total_len - correlation_window
                          : 0;

        for (size_t i = 0; i < search_end; i += SEARCH_STEP_SIZE) {
            if (!impl_->hasMinimumEnergy(i, correlation_window)) {
                i += correlation_window / 2 - SEARCH_STEP_SIZE;
                continue;
            }

            float corr = impl_->measureCorrelation(i);

            if (corr > impl_->sync_threshold) {
                // Search for plateau
                size_t plateau_count = 0;
                float peak_corr = corr;
                size_t peak_pos = i;

                for (size_t j = 0; j <= PLATEAU_SEARCH_WINDOW && i + j + preamble_total_len < impl_->rx_buffer.size(); j += 8) {
                    float ref_corr = impl_->measureCorrelation(i + j);
                    if (ref_corr >= PLATEAU_THRESHOLD) {
                        plateau_count++;
                    }
                    if (ref_corr > peak_corr) {
                        peak_corr = ref_corr;
                        peak_pos = i + j;
                    }
                }

                if (plateau_count >= MIN_PLATEAU_SAMPLES) {
                    found_sync = true;
                    sync_offset = peak_pos;
                    sync_corr = peak_corr;

                    LOG_SYNC(INFO, "Preamble: coarse=%zu, peak=%zu, sync=%zu, corr=%.3f/%.3f",
                             i, peak_pos, sync_offset, corr, peak_corr);
                    break;
                }
            }
        }

        if (found_sync) {
            float coarse_cfo = impl_->estimateCoarseCFO(sync_offset);
            impl_->freq_offset_hz = coarse_cfo;
            impl_->freq_offset_filtered = coarse_cfo;
            impl_->freq_correction_phase = 0.0f;
            impl_->symbols_since_sync = 0;

            LOG_SYNC(INFO, "SYNC: offset=%zu, corr=%.3f, CFO=%.1f Hz, buffer=%zu",
                     sync_offset, sync_corr, coarse_cfo, impl_->rx_buffer.size());

            // LTS fine timing
            size_t sts_start = sync_offset;
            size_t refined_lts_start = impl_->refineLTSTiming(sts_start);

            if (refined_lts_start == SIZE_MAX) {
                LOG_SYNC(INFO, "LTS confirmation FAILED - Schmidl-Cox false positive, continuing search");
                if (impl_->rx_buffer.size() > OVERLAP_SAMPLES * 2) {
                    size_t trim = std::min(sync_offset + preamble_symbol_len,
                                           impl_->rx_buffer.size() - OVERLAP_SAMPLES);
                    impl_->rx_buffer.erase(impl_->rx_buffer.begin(),
                                           impl_->rx_buffer.begin() + trim);
                }
                found_sync = false;
            } else {
                size_t coarse_lts_pos = sts_start + 4 * preamble_symbol_len;
                int timing_refinement = (int)refined_lts_start - (int)coarse_lts_pos;
                LOG_SYNC(INFO, "LTS fine timing: coarse_lts=%zu, refined=%zu, delta=%+d samples",
                         coarse_lts_pos, refined_lts_start, timing_refinement);

                // Report true preamble start for diagnostics/tests.
                // In practice the Schmidl-Cox coarse peak tends to land one symbol late,
                // so refined_lts_start is commonly the SECOND LTS symbol.
                // Preamble layout: 4 STS + 2 LTS.
                if (refined_lts_start >= 5 * preamble_symbol_len) {
                    impl_->last_sync_offset = refined_lts_start - 5 * preamble_symbol_len;
                } else {
                    impl_->last_sync_offset = 0;
                }

                // Check if differential mode for LTS phase extraction
                bool is_differential = (impl_->config.modulation == Modulation::DQPSK ||
                                        impl_->config.modulation == Modulation::D8PSK ||
                                        impl_->config.modulation == Modulation::DBPSK);
                LOG_SYNC(DEBUG, "LTS phase check: is_differential=%d, use_pilots=%d",
                        is_differential, impl_->config.use_pilots);

                // Consume preamble up through last LTS. With second-LTS lock, data starts
                // one symbol after refined_lts_start.
                size_t consume = refined_lts_start + preamble_symbol_len + impl_->manual_timing_offset;
                LOG_SYNC(DEBUG, "Consume calc: refined_lts=%zu + preamble_sym=%zu + offset=%d = %zu",
                        refined_lts_start, preamble_symbol_len, impl_->manual_timing_offset, consume);
                impl_->rx_buffer.erase(impl_->rx_buffer.begin(),
                                       impl_->rx_buffer.begin() + consume);

                // Transition to SYNCED state
                impl_->state.store(Impl::State::SYNCED);
                impl_->synced_symbol_count.store(0);
                impl_->mixer.reset();

                // Reset channel estimate to unity - will be updated by pilot tracking
                std::fill(impl_->channel_estimate.begin(), impl_->channel_estimate.end(), Complex(1, 0));
                impl_->snr_symbol_count = 0;
                impl_->estimated_snr_linear = 1.0f;
                impl_->noise_variance = 0.1f;
                impl_->prev_pilot_phases.clear();

                // Reset adaptive equalizer
                std::fill(impl_->lms_weights.begin(), impl_->lms_weights.end(), Complex(1, 0));
                std::fill(impl_->last_decisions.begin(), impl_->last_decisions.end(), Complex(0, 0));
                std::fill(impl_->rls_P.begin(), impl_->rls_P.end(), 1.0f);

                impl_->dbpsk_prev_equalized.clear();
                impl_->carrier_erasure_flags_.clear();
                impl_->differential_prev_erased_.clear();
                impl_->carrier_eq_mag_ema_.clear();
                impl_->carrier_eq_mag_var_.clear();
                if (!is_differential || impl_->config.use_pilots) {
                    impl_->carrier_phase_initialized = false;
                    impl_->carrier_phase_correction = Complex(1, 0);
                }
                impl_->dqpsk_skip_first_symbol = false;
                impl_->timing_offset_samples = 0.0f;
                LOG_SYNC(INFO, "Schmidl-Cox sync complete, CFO=%.1f Hz, ready for data", coarse_cfo);
            }
        } else {
            // No preamble found - trim old data
            if (impl_->rx_buffer.size() > OVERLAP_SAMPLES * 2) {
                size_t trim = impl_->rx_buffer.size() - OVERLAP_SAMPLES;
                impl_->rx_buffer.erase(impl_->rx_buffer.begin(),
                                       impl_->rx_buffer.begin() + trim);
            }
        }
    }

    // ========================================================================
    // SYNCED STATE
    // ========================================================================
    if (impl_->state.load() == Impl::State::SYNCED) {
        // Check for new preamble (mid-frame detection)
        bool should_check_preamble = impl_->synced_symbol_count.load() > 0 &&
                                      impl_->idle_call_count.load() >= 2 &&
                                      impl_->soft_bits.empty();

        if (should_check_preamble && impl_->rx_buffer.size() >= preamble_total_len) {
            size_t search_limit = std::min(impl_->rx_buffer.size() - preamble_total_len,
                                           impl_->symbol_samples * 2);
            constexpr size_t STEP = 8;

            for (size_t offset = 0; offset <= search_limit; offset += STEP) {
                if (!impl_->hasMinimumEnergy(offset, correlation_window)) {
                    continue;
                }

                float corr = impl_->measureCorrelation(offset);
                if (corr > impl_->sync_threshold) {
                    size_t sts_start = offset;
                    size_t refined_lts_start = impl_->refineLTSTiming(sts_start);

                    if (refined_lts_start == SIZE_MAX) {
                        LOG_SYNC(DEBUG, "SYNCED: LTS confirmation failed at offset %zu, continuing", offset);
                        continue;
                    }

                    size_t consume = refined_lts_start + preamble_symbol_len;

                    LOG_SYNC(INFO, "SYNCED preamble: sts=%zu, refined_lts=%zu, consume=%zu",
                             sts_start, refined_lts_start, consume);

                    float coarse_cfo = impl_->estimateCoarseCFO(sts_start);
                    impl_->freq_offset_hz = coarse_cfo;
                    impl_->freq_offset_filtered = coarse_cfo;
                    impl_->freq_correction_phase = 0.0f;
                    impl_->symbols_since_sync = 0;

                    impl_->rx_buffer.erase(impl_->rx_buffer.begin(),
                                           impl_->rx_buffer.begin() + consume);

                    LOG_SYNC(WARN, "Mid-frame preamble detected at offset %zu (had %zu soft bits)! Clearing state.",
                             sts_start, impl_->soft_bits.size());
                    impl_->soft_bits.clear();
                    impl_->synced_symbol_count.store(0);
                    impl_->idle_call_count.store(0);
                    impl_->mixer.reset();

                    std::fill(impl_->channel_estimate.begin(), impl_->channel_estimate.end(), Complex(1, 0));
                    impl_->snr_symbol_count = 0;
                    impl_->estimated_snr_linear = 1.0f;
                    impl_->noise_variance = 0.1f;
                    impl_->prev_pilot_phases.clear();

                    std::fill(impl_->lms_weights.begin(), impl_->lms_weights.end(), Complex(1, 0));
                    std::fill(impl_->last_decisions.begin(), impl_->last_decisions.end(), Complex(0, 0));
                    std::fill(impl_->rls_P.begin(), impl_->rls_P.end(), 1.0f);

                    impl_->dbpsk_prev_equalized.clear();
                    impl_->carrier_erasure_flags_.clear();
                    impl_->differential_prev_erased_.clear();
                    impl_->carrier_eq_mag_ema_.clear();
                    impl_->carrier_eq_mag_var_.clear();
                    impl_->carrier_phase_initialized = false;
                    impl_->carrier_phase_correction = Complex(1, 0);

                    break;
                }
            }
        }

        size_t soft_bits_before = impl_->soft_bits.size();
        LOG_DEMOD(DEBUG, "SYNCED: buffer=%zu samples, symbol_samples=%zu, can process %zu symbols",
                  impl_->rx_buffer.size(), impl_->symbol_samples, impl_->rx_buffer.size() / impl_->symbol_samples);

        // Process all complete symbols
        size_t symbols_processed = 0;
        while (impl_->rx_buffer.size() >= impl_->symbol_samples) {
            SampleSpan sym_samples(impl_->rx_buffer.data(), impl_->symbol_samples);
            const auto& baseband = impl_->toBaseband(sym_samples);
            const auto& freq_domain = impl_->extractSymbol(baseband, 0);

            impl_->updateChannelEstimate(freq_domain);

            const auto& equalized = impl_->equalize(freq_domain);
            impl_->demodulateSymbol(equalized, impl_->config.modulation);

            impl_->rx_buffer.erase(impl_->rx_buffer.begin(),
                                   impl_->rx_buffer.begin() + impl_->symbol_samples);
            ++symbols_processed;

            impl_->updateQuality();

            // Break early once we have enough for a codeword
            // This prevents processing multiple frames in one call
            if (impl_->soft_bits.size() >= LDPC_BLOCK_SIZE) {
                LOG_DEMOD(DEBUG, "Have %zu soft bits (>= %zu), returning early",
                          impl_->soft_bits.size(), LDPC_BLOCK_SIZE);
                break;
            }

            int sym_count = ++impl_->synced_symbol_count;
            if (sym_count > MAX_SYMBOLS_BEFORE_TIMEOUT) {
                LOG_SYNC(WARN, "Sync timeout after %d symbols (%zu soft bits accumulated), resetting to SEARCHING",
                         sym_count, impl_->soft_bits.size());
                impl_->state.store(Impl::State::SEARCHING);
                impl_->synced_symbol_count.store(0);
                impl_->idle_call_count.store(0);
                return impl_->soft_bits.size() >= LDPC_BLOCK_SIZE;
            }
        }

        if (symbols_processed > 0) {
            float snr_db = (impl_->estimated_snr_linear > 0)
                ? 10.0f * std::log10(impl_->estimated_snr_linear) : 0.0f;
            LOG_DEMOD(INFO, "Processed %zu symbols, soft_bits=%zu (+%zu), SNR=%.1f dB, CFO=%.1f Hz",
                      symbols_processed, impl_->soft_bits.size(),
                      impl_->soft_bits.size() - soft_bits_before,
                      snr_db, impl_->freq_offset_hz);
        }

        // Track idle calls
        if (impl_->soft_bits.size() == soft_bits_before) {
            int idle = ++impl_->idle_call_count;
            if (idle > MAX_IDLE_CALLS_BEFORE_RESET) {
                LOG_SYNC(DEBUG, "Idle timeout after %d calls (%zu soft bits), resetting to SEARCHING",
                         idle, impl_->soft_bits.size());
                impl_->state.store(Impl::State::SEARCHING);
                impl_->synced_symbol_count.store(0);
                impl_->idle_call_count.store(0);
                return impl_->soft_bits.size() >= LDPC_BLOCK_SIZE;
            }
        } else {
            impl_->idle_call_count.store(0);
        }

        bool has_codeword = impl_->soft_bits.size() >= LDPC_BLOCK_SIZE;

        // Frame completion detection
        bool truly_idle = samples.empty() && symbols_processed == 0;
        if (!has_codeword && impl_->synced_symbol_count.load() > 0 && truly_idle) {
            LOG_DEMOD(INFO, "Frame complete (only %zu leftover bits, truly idle), resetting to SEARCHING",
                      impl_->soft_bits.size());
            impl_->state.store(Impl::State::SEARCHING);
            impl_->synced_symbol_count.store(0);
            impl_->idle_call_count.store(0);
            impl_->soft_bits.clear();
        }

        LOG_DEMOD(DEBUG, "process: soft_bits=%zu, returning %s",
                  impl_->soft_bits.size(), has_codeword ? "true" : "false");
        return has_codeword;
    }

    LOG_DEMOD(DEBUG, "process: not synced, returning false");
    return false;
}

Bytes OFDMDemodulator::getData() {
    Bytes data;
    uint8_t byte = 0;
    int bit_count = 0;

    for (float llr : impl_->soft_bits) {
        uint8_t bit = (llr > 0) ? 1 : 0;
        byte = (byte << 1) | bit;
        ++bit_count;

        if (bit_count == 8) {
            data.push_back(byte);
            byte = 0;
            bit_count = 0;
        }
    }

    impl_->soft_bits.clear();
    return data;
}

std::vector<float> OFDMDemodulator::getSoftBits() {
    LOG_DEMOD(DEBUG, "getSoftBits: buffer has %zu soft bits", impl_->soft_bits.size());

    if (g_log_level >= LogLevel::TRACE && g_log_categories.demod && impl_->soft_bits.size() >= 24) {
        char buf[256];
        int pos = 0;
        for (size_t i = 0; i < 24 && pos < 240; ++i) {
            int bit = (impl_->soft_bits[i] < 0) ? 1 : 0;
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%+.1f(%d) ", impl_->soft_bits[i], bit);
            if ((i + 1) % 6 == 0) pos += snprintf(buf + pos, sizeof(buf) - pos, "| ");
        }
        LOG_DEMOD(TRACE, "First 24 LLRs: %s", buf);
    }

    if (impl_->soft_bits.size() <= LDPC_BLOCK_SIZE) {
        auto bits = std::move(impl_->soft_bits);
        impl_->soft_bits.clear();
        return bits;
    } else {
        std::vector<float> bits(impl_->soft_bits.begin(),
                                impl_->soft_bits.begin() + LDPC_BLOCK_SIZE);
        impl_->soft_bits.erase(impl_->soft_bits.begin(),
                               impl_->soft_bits.begin() + LDPC_BLOCK_SIZE);
        return bits;
    }
}

ChannelQuality OFDMDemodulator::getChannelQuality() const {
    return impl_->quality;
}

float OFDMDemodulator::getEstimatedSNR() const {
    return 10.0f * std::log10(impl_->estimated_snr_linear);
}

float OFDMDemodulator::getFrequencyOffset() const {
    return impl_->freq_offset_hz;
}

float OFDMDemodulator::getLastTimingOffsetSamples() const {
    return impl_->timing_offset_samples;
}

float OFDMDemodulator::getFadingIndex() const {
    // Compute coefficient of variation of per-carrier channel estimate magnitudes
    // Uses data_carrier_indices to get magnitudes of active carriers only
    const auto& indices = impl_->data_carrier_indices;
    if (indices.empty()) return 0.0f;

    // Collect carrier magnitudes
    std::vector<float> magnitudes;
    magnitudes.reserve(indices.size());
    for (int idx : indices) {
        float mag = std::abs(impl_->channel_estimate[idx]);
        magnitudes.push_back(mag);
    }

    // Calculate mean
    float sum = 0.0f;
    for (float m : magnitudes) sum += m;
    float mean = sum / magnitudes.size();

    if (mean < 0.001f) return 0.0f;  // No signal

    // Calculate standard deviation
    float var_sum = 0.0f;
    for (float m : magnitudes) {
        float diff = m - mean;
        var_sum += diff * diff;
    }
    float std_dev = std::sqrt(var_sum / magnitudes.size());

    // Coefficient of variation (normalized std dev)
    return std_dev / mean;
}

float OFDMDemodulator::getLastLTSSignalPower() const {
    return impl_->last_lts_signal_power;
}

float OFDMDemodulator::getLastLTSChannelMagnitude() const {
    return impl_->last_lts_channel_magnitude;
}

float OFDMDemodulator::getLastLTSResidualCFOHz() const {
    return impl_->last_lts_residual_cfo_hz;
}

void OFDMDemodulator::setRXCarrierErasureEnabled(bool enabled) {
    impl_->rx_carrier_erasure_enabled_ = enabled;
}

void OFDMDemodulator::setFrequencyOffset(float cfo_hz) {
    LOG_DEMOD(INFO, "setFrequencyOffset: CFO=%.2f Hz (was %.2f Hz)", cfo_hz, impl_->freq_offset_hz);
    impl_->freq_offset_hz = cfo_hz;
    impl_->freq_offset_filtered = cfo_hz;
    // Reset correction phase so it starts from 0 with the new offset
    impl_->freq_correction_phase = 0.0f;
    // Mark that CFO was explicitly provided (e.g., from chirp detection)
    // This tells processPresynced() to trust this value instead of re-estimating
    impl_->chirp_cfo_estimated = true;
}

void OFDMDemodulator::setFrequencyOffsetWithPhase(float cfo_hz, float initial_phase_rad) {
    LOG_DEMOD(INFO, "setFrequencyOffsetWithPhase: CFO=%.2f Hz, initial_phase=%.1f° (was CFO=%.2f Hz)",
              cfo_hz, initial_phase_rad * 180.0f / M_PI, impl_->freq_offset_hz);
    impl_->freq_offset_hz = cfo_hz;
    impl_->freq_offset_filtered = cfo_hz;
    // Set initial correction phase to match accumulated CFO phase at this point
    impl_->freq_correction_phase = initial_phase_rad;
    // Mark that CFO was explicitly provided
    impl_->chirp_cfo_estimated = true;
}

Symbol OFDMDemodulator::getConstellationSymbols() const {
    std::lock_guard<std::mutex> lock(impl_->constellation_mutex);
    return impl_->constellation_symbols;
}

bool OFDMDemodulator::isSynced() const {
    return impl_->state.load() == Impl::State::SYNCED;
}

bool OFDMDemodulator::hasPendingData() const {
    if (impl_->state.load() != Impl::State::SYNCED) {
        return false;
    }
    if (!impl_->soft_bits.empty()) {
        return true;
    }
    return impl_->rx_buffer.size() >= impl_->symbol_samples;
}

size_t OFDMDemodulator::getLastSyncOffset() const {
    return impl_->last_sync_offset;
}

void OFDMDemodulator::setTimingOffset(int offset) {
    impl_->manual_timing_offset = offset;
}

bool OFDMDemodulator::processPresynced(SampleSpan samples, int training_symbols) {
    // Process samples after external sync (chirp preamble)
    //
    // ROBUST ACQUISITION SEQUENCE:
    // 1. First 'training_symbols' are known LTS sequence - use for channel estimation
    // 2. Remaining symbols are data - demodulate them
    //
    // This mirrors the Schmidl-Cox approach: LTS for channel est, then data.
    // The chirp replaces STS for more robust timing sync at low SNR.

    if (samples.size() < impl_->symbol_samples) {
        return false;
    }

    // Reset state but preserve config
    impl_->soft_bits.clear();
    impl_->demod_data.clear();
    impl_->rx_buffer.clear();
    impl_->synced_symbol_count.store(0);
    impl_->idle_call_count.store(0);
    impl_->mixer.reset();

    // Reset channel estimate to unity
    std::fill(impl_->channel_estimate.begin(), impl_->channel_estimate.end(), Complex(1, 0));
    impl_->snr_symbol_count = 0;
    impl_->estimated_snr_linear = 1.0f;
    impl_->noise_variance = 0.1f;

    // Preserve pre-set CFO and phase (e.g., from chirp-based estimation)
    // If CFO was explicitly set via setFrequencyOffsetWithPhase(), the phase
    // contains the accumulated CFO phase at processing start - DON'T reset it!
    // impl_->freq_offset_hz = 0.0f;  // KEEP the pre-set value!
    // impl_->freq_offset_filtered = 0.0f;  // KEEP the pre-set value!
    // impl_->freq_correction_phase = 0.0f;  // KEEP the pre-set value if explicitly set!
    LOG_SYNC(INFO, "processPresynced: pre-set CFO=%.2f Hz, initial_phase=%.1f°",
             impl_->freq_offset_hz, impl_->freq_correction_phase * 180.0f / M_PI);
    impl_->symbols_since_sync = 0;
    impl_->prev_pilot_phases.clear();
    impl_->pilot_phase_correction = Complex(1, 0);

    // Reset adaptive equalizer state
    std::fill(impl_->lms_weights.begin(), impl_->lms_weights.end(), Complex(1, 0));
    std::fill(impl_->last_decisions.begin(), impl_->last_decisions.end(), Complex(0, 0));
    std::fill(impl_->rls_P.begin(), impl_->rls_P.end(), 1.0f);

    impl_->dbpsk_prev_equalized.clear();
    impl_->carrier_erasure_flags_.clear();
    impl_->differential_prev_erased_.clear();
    impl_->carrier_eq_mag_ema_.clear();
    impl_->carrier_eq_mag_var_.clear();
    impl_->carrier_phase_initialized = false;
    impl_->carrier_phase_correction = Complex(1, 0);
    impl_->lts_phase_offset = Complex(1, 0);  // Will be updated by estimateChannelFromLTS

    // Clear constellation symbols so we only show the current frame's data
    {
        std::lock_guard<std::mutex> lock(impl_->constellation_mutex);
        impl_->constellation_symbols.clear();
    }

    // Set state to SYNCED
    impl_->state.store(Impl::State::SYNCED);

    const float* ptr = samples.data();
    size_t remaining = samples.size();

    // === PHASE 1a: CFO handling ===
    // If chirp-based CFO was provided, TRUST it completely.
    // The training symbol correlation doesn't work well because:
    // 1. Training symbols are identical in freq domain but not time domain
    // 2. Mixer phase advancement between symbols causes spurious phase
    //
    // TODO: Implement proper two-stage CFO using cyclic prefix correlation
    // or frequency-domain phase estimation after initial CFO correction.
    if (impl_->chirp_cfo_estimated) {
        LOG_SYNC(INFO, "Using chirp CFO: %.1f Hz (trusted)", impl_->freq_offset_hz);
    } else if (training_symbols >= 2 && std::abs(impl_->freq_offset_hz) < 0.1f) {
        // No chirp CFO available - try training estimation (may be inaccurate)
        float cfo = impl_->estimateCFOFromTraining(ptr, training_symbols, 0.0f);
        impl_->freq_offset_hz = cfo;
        impl_->freq_offset_filtered = cfo;
        LOG_SYNC(INFO, "CFO from training: %.1f Hz (no chirp available)", cfo);
    } else {
        LOG_SYNC(INFO, "Using pre-set CFO: %.1f Hz", impl_->freq_offset_hz);
    }

    // === PHASE 1b: Process training symbols for channel estimation ===
    // Even for differential modulation (DQPSK), we need channel estimation on
    // fading channels where different carriers experience different attenuation.
    //
    // estimateChannelFromLTS uses toBaseband() which advances the mixer,
    // so we don't need to advance it separately here.
    if (training_symbols > 0) {
        size_t training_samples_count = training_symbols * impl_->symbol_samples;

        // Bounds check: a too-short input would underflow `remaining` (size_t)
        // when we subtract training_samples_count below, leading to a billion-
        // sample vector::insert and a std::length_error crash. Seen on real
        // hardware with small SDL2 audio buffers when a partial chirp lock
        // produced LTS estimates with NaN/inf values and a 1509-sample input.
        if (training_samples_count > remaining) {
            LOG_DEMOD(WARN,
                "processPresynced: short input — got %zu samples but training "
                "alone needs %zu; aborting",
                remaining, training_samples_count);
            return false;
        }

        // Use training symbols for channel estimation (this advances the mixer)
        {
            timing::ScopedTimer _profile_(timing::globalDecoderProfile().lts_channel_estimate);
            impl_->estimateChannelFromLTS(ptr, training_symbols);
        }

        ptr += training_samples_count;
        remaining -= training_samples_count;
        impl_->synced_symbol_count = training_symbols;
    }

    // Initialize reference for differential demodulation to (1,0)
    // Same as Schmidl-Cox path does
    impl_->dbpsk_prev_equalized.clear();  // Will be initialized in demodulateSymbol
    impl_->differential_prev_erased_.clear();

    LOG_SYNC(INFO, "processPresynced: skipped %d training symbols, %zu samples remaining",
             training_symbols, remaining);

    // === PHASE 2: Process data symbols ===
    LOG_DEMOD(DEBUG, "DATA phase: first_sample=%.6f, remaining=%zu", *ptr, remaining);
    size_t data_offset = 0;

    while (remaining - data_offset >= impl_->symbol_samples) {
        timing::ScopedTimer _profile_(timing::globalDecoderProfile().data_symbol_loop);

        SampleSpan sym_samples(ptr + data_offset, impl_->symbol_samples);
        const auto& bb = impl_->toBaseband(sym_samples);
        const auto& fd = impl_->extractSymbol(bb, 0);

        // Per-symbol pilot tracking: update channel estimate from pilot observations.
        // For differential modes: update only |H| (magnitude) to track fading depth,
        // while keeping phase frozen from LTS. This gives MMSE accurate amplitude
        // scaling without corrupting the differential phase relationship.
        // For coherent modes: full H update (magnitude + phase).
        if (!impl_->pilot_carrier_indices.empty()) {
            impl_->updateChannelEstimate(fd);
        }
        const auto& eq = impl_->equalize(fd);
        impl_->demodulateSymbol(eq, impl_->config.modulation);

        data_offset += impl_->symbol_samples;
        ++impl_->synced_symbol_count;
        impl_->updateQuality();
    }

    if (data_offset < remaining) {
        impl_->rx_buffer.assign(ptr + data_offset, ptr + remaining);
    } else {
        impl_->rx_buffer.clear();
    }

    // Debug: count positive vs negative soft bits to check for all-zero demod issue
    {
        int pos_count = 0, neg_count = 0, zero_count = 0;
        float min_abs = 999, max_abs = 0;
        for (size_t i = 0; i < std::min(impl_->soft_bits.size(), size_t(648)); i++) {
            float v = impl_->soft_bits[i];
            if (v > 0.1f) pos_count++;
            else if (v < -0.1f) neg_count++;
            else zero_count++;
            float av = std::abs(v);
            if (av < min_abs) min_abs = av;
            if (av > max_abs) max_abs = av;
        }
        LOG_SYNC(INFO, "processPresynced: %d symbols, %zu soft bits. CW0 stats: pos=%d neg=%d zero=%d min_abs=%.2f max_abs=%.2f first8=[%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f]",
                 impl_->synced_symbol_count.load(), impl_->soft_bits.size(),
                 pos_count, neg_count, zero_count, min_abs, max_abs,
                 impl_->soft_bits.size() > 0 ? impl_->soft_bits[0] : 0,
                 impl_->soft_bits.size() > 1 ? impl_->soft_bits[1] : 0,
                 impl_->soft_bits.size() > 2 ? impl_->soft_bits[2] : 0,
                 impl_->soft_bits.size() > 3 ? impl_->soft_bits[3] : 0,
                 impl_->soft_bits.size() > 4 ? impl_->soft_bits[4] : 0,
                 impl_->soft_bits.size() > 5 ? impl_->soft_bits[5] : 0,
                 impl_->soft_bits.size() > 6 ? impl_->soft_bits[6] : 0,
                 impl_->soft_bits.size() > 7 ? impl_->soft_bits[7] : 0);
    }

    LOG_DEMOD(DEBUG, "OFDM processPresynced: %d symbols, %zu soft bits, need %d",
              impl_->synced_symbol_count.load(), impl_->soft_bits.size(), LDPC_BLOCK_SIZE);

    return impl_->soft_bits.size() >= LDPC_BLOCK_SIZE;
}

void OFDMDemodulator::reset() {
    impl_->state.store(Impl::State::SEARCHING);
    impl_->synced_symbol_count.store(0);
    impl_->idle_call_count.store(0);
    impl_->rx_buffer.clear();
    impl_->soft_bits.clear();
    impl_->demod_data.clear();
    std::fill(impl_->channel_estimate.begin(), impl_->channel_estimate.end(), Complex(1, 0));
    impl_->snr_symbol_count = 0;
    impl_->estimated_snr_linear = 1.0f;
    impl_->noise_variance = 0.1f;
    impl_->last_lts_signal_power = 1.0f;
    impl_->last_lts_channel_magnitude = 1.0f;
    impl_->last_lts_residual_cfo_hz = 0.0f;

    impl_->freq_offset_hz = 0.0f;
    impl_->freq_offset_filtered = 0.0f;
    impl_->freq_correction_phase = 0.0f;
    impl_->chirp_cfo_estimated = false;
    impl_->symbols_since_sync = 0;
    impl_->prev_pilot_phases.clear();
    impl_->pilot_phase_correction = Complex(1, 0);
    impl_->lts_phase_offset = Complex(1, 0);

    // Reset mixer phase - critical for OFDM_CHIRP which calls reset() between frames
    impl_->mixer.reset();
    impl_->dbpsk_prev_equalized.clear();
    impl_->carrier_erasure_flags_.clear();
    impl_->differential_prev_erased_.clear();
    impl_->carrier_eq_mag_ema_.clear();
    impl_->carrier_eq_mag_var_.clear();
    impl_->carrier_phase_initialized = false;
    impl_->carrier_phase_correction = Complex(1, 0);

    std::fill(impl_->lms_weights.begin(), impl_->lms_weights.end(), Complex(1, 0));
    std::fill(impl_->last_decisions.begin(), impl_->last_decisions.end(), Complex(0, 0));
    std::fill(impl_->rls_P.begin(), impl_->rls_P.end(), 1.0f);
}

} // namespace ultra
