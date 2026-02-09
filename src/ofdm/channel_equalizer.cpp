// Channel Estimation and Equalization for OFDM
// Part of OFDMDemodulator::Impl

#define _USE_MATH_DEFINES
#include <cmath>
#include "demodulator_impl.hpp"
#include "demodulator_constants.hpp"
#include "ultra/logging.hpp"
#include <algorithm>

namespace ultra {

using namespace demod_constants;

// =============================================================================
// BASEBAND CONVERSION
// =============================================================================

std::vector<Complex> OFDMDemodulator::Impl::toBaseband(SampleSpan samples) {
    std::vector<Complex> baseband(samples.size());

    // Phase increment per sample for frequency correction
    float phase_increment = -2.0f * M_PI * freq_offset_hz / config.sample_rate;

    // Log CFO correction
    static int tb_call_count = 0;
    if (std::abs(freq_offset_hz) > 0.01f && tb_call_count < 5) {
        LOG_DEMOD(DEBUG, "toBaseband #%d: CFO=%.2f Hz, phase_inc=%.6f, start_phase=%.2f rad (%.1f deg), samples=%zu",
                  tb_call_count, freq_offset_hz, phase_increment, freq_correction_phase,
                  freq_correction_phase * 180.0f / M_PI, samples.size());
        tb_call_count++;
    }

    for (size_t i = 0; i < samples.size(); ++i) {
        Complex osc = mixer.next();
        Complex mixed = samples[i] * std::conj(osc);

        // Apply frequency offset correction
        if (std::abs(freq_offset_hz) > 0.01f) {
            Complex correction(std::cos(freq_correction_phase),
                               std::sin(freq_correction_phase));
            mixed *= correction;
            freq_correction_phase += phase_increment;

            // Wrap phase
            if (freq_correction_phase > M_PI) {
                freq_correction_phase -= 2.0f * M_PI;
            } else if (freq_correction_phase < -M_PI) {
                freq_correction_phase += 2.0f * M_PI;
            }
        }

        baseband[i] = mixed;
    }

    return baseband;
}

std::vector<Complex> OFDMDemodulator::Impl::extractSymbol(const std::vector<Complex>& baseband, size_t offset) {
    size_t start = offset + config.getCyclicPrefix();

    std::vector<Complex> symbol(config.fft_size);
    for (size_t i = 0; i < config.fft_size && (start + i) < baseband.size(); ++i) {
        symbol[i] = baseband[start + i];
    }

    std::vector<Complex> freq;
    fft.forward(symbol, freq);

    return freq;
}

// =============================================================================
// CHANNEL ESTIMATION
// =============================================================================

void OFDMDemodulator::Impl::estimateChannelFromLTS(const float* training_samples, size_t num_symbols) {
    // Estimate channel response from LTS (Long Training Sequence) symbols
    // This is used by processPresynced() for chirp-synced modes where we have
    // training symbols for initial channel estimation.
    //
    // The LTS carries:
    //   - sync_sequence on data carriers
    //   - pilot_sequence on pilot carriers (if use_pilots=true)
    //
    // We estimate H for BOTH data and pilot carriers so that subsequent
    // updateChannelEstimate() calls can use pilots for tracking.
    //
    // We average over multiple training symbols for robustness.
    // We also estimate noise variance from the variance of H estimates.

    LOG_DEMOD(DEBUG, "estimateChannelFromLTS: num_symbols=%zu, symbol_samples=%zu, first_sample=%.6f",
             num_symbols, symbol_samples, training_samples[0]);

    // Print carrier indices
    {
        char idx_buf[128] = "";
        int pos = 0;
        for (size_t i = 0; i < std::min(size_t(5), data_carrier_indices.size()); ++i) {
            pos += snprintf(idx_buf + pos, sizeof(idx_buf) - pos, "%d ", data_carrier_indices[i]);
        }
        LOG_DEMOD(DEBUG, "LTS RX carrier indices (first 5): %s(total %zu)", idx_buf, data_carrier_indices.size());
    }

    if (num_symbols == 0 || data_carrier_indices.empty()) return;

    // Store per-symbol channel estimates for noise estimation (data carriers only for now)
    std::vector<std::vector<Complex>> h_per_symbol(num_symbols);
    for (auto& v : h_per_symbol) v.resize(data_carrier_indices.size());

    // Accumulate channel estimates from each training symbol
    std::vector<Complex> h_sum_data(data_carrier_indices.size(), Complex(0, 0));
    std::vector<Complex> h_sum_pilot(pilot_carrier_indices.size(), Complex(0, 0));
    size_t valid_symbols = 0;

    // Process each training symbol using the main mixer (it will be advanced)
    const float* ptr = training_samples;
    for (size_t sym = 0; sym < num_symbols; ++sym) {
        // Use toBaseband and extractSymbol like normal demodulation
        SampleSpan sym_span(ptr, symbol_samples);
        auto baseband = toBaseband(sym_span);
        auto freq = extractSymbol(baseband, 0);

        // DEBUG: Print first few freq domain values on first training symbol
        if (sym == 0) {
            char rx_buf[256] = "", tx_buf[256] = "";
            int rp = 0, tp = 0;
            for (size_t i = 0; i < std::min(size_t(5), data_carrier_indices.size()); ++i) {
                int idx = data_carrier_indices[i];
                rp += snprintf(rx_buf + rp, sizeof(rx_buf) - rp, "[%d]=(%.3f,%.3f) ", idx, freq[idx].real(), freq[idx].imag());
                Complex tx = sync_sequence[i % sync_sequence.size()];
                tp += snprintf(tx_buf + tp, sizeof(tx_buf) - tp, "(%.3f,%.3f) ", tx.real(), tx.imag());
            }
            LOG_DEMOD(DEBUG, "LTS RX freq[idx] first 5 carriers: %s", rx_buf);
            LOG_DEMOD(DEBUG, "LTS TX sync_seq first 5 carriers: %s", tx_buf);
        }

        // Estimate H for each data carrier
        for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
            int idx = data_carrier_indices[i];
            Complex rx = freq[idx];
            Complex tx = sync_sequence[i % sync_sequence.size()];

            // H = rx / tx (LS estimate)
            if (std::abs(tx) > 0.01f) {
                Complex h_ls = rx / tx;
                h_sum_data[i] += h_ls;
                h_per_symbol[sym][i] = h_ls;
            }
        }

        // Estimate H for each pilot carrier (LTS includes pilots!)
        for (size_t i = 0; i < pilot_carrier_indices.size(); ++i) {
            int idx = pilot_carrier_indices[i];
            Complex rx = freq[idx];
            Complex tx = pilot_sequence[i];  // Pilots use pilot_sequence, not sync_sequence

            // H = rx / tx (LS estimate)
            if (std::abs(tx) > 0.01f) {
                Complex h_ls = rx / tx;
                h_sum_pilot[i] += h_ls;
            }

            // DEBUG: Log raw pilot values for each training symbol
            if (sym == 0 && i < 4) {
                LOG_DEMOD(DEBUG, "LTS sym=%zu pilot[%zu] idx=%d: rx=(%.4f,%.4f) |rx|=%.4f tx=(%.1f,%.1f)",
                         sym, i, idx, rx.real(), rx.imag(), std::abs(rx), tx.real(), tx.imag());
            }
        }

        valid_symbols++;
        ptr += symbol_samples;
    }

    if (valid_symbols == 0) return;

    // === RESIDUAL CFO ESTIMATION FROM LTS ===
    // Even when chirp-based CFO is applied, fading can cause chirp peak position errors
    // that result in a wrong CFO estimate (e.g., -1.4 Hz when actual is 0).
    // The toBaseband() above applied this wrong CFO, so we can detect the residual
    // by measuring the phase rotation between training symbols.
    //
    // If H[sym0] and H[sym1] differ by a consistent phase across carriers,
    // that phase = residual_CFO × T_symbol.
    if (valid_symbols >= 2) {
        Complex phase_diff_sum(0, 0);
        int cfo_valid = 0;

        for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
            Complex h0 = h_per_symbol[0][i];
            Complex h1 = h_per_symbol[1][i];
            if (std::abs(h0) > 0.01f && std::abs(h1) > 0.01f) {
                Complex diff = h1 * std::conj(h0);
                float mag = std::abs(diff);
                if (mag > 1e-6f) {
                    phase_diff_sum += diff / mag;  // Normalized unit vector
                    cfo_valid++;
                }
            }
        }

        if (cfo_valid > 10) {
            float avg_phase = std::atan2(phase_diff_sum.imag(), phase_diff_sum.real());
            float symbol_duration = static_cast<float>(symbol_samples) / config.sample_rate;
            float residual_cfo = avg_phase / (2.0f * M_PI * symbol_duration);

            // Only correct if residual is significant (> 0.3 Hz) but sane (< 5 Hz)
            if (std::abs(residual_cfo) > 0.3f && std::abs(residual_cfo) < 5.0f) {
                float old_cfo = freq_offset_hz;
                freq_offset_hz += residual_cfo;
                freq_offset_filtered = freq_offset_hz;

                LOG_DEMOD(WARN, "LTS residual CFO: %.2f Hz detected (chirp gave %.2f, corrected to %.2f Hz)",
                          residual_cfo, old_cfo, freq_offset_hz);

                // Re-process training symbols with corrected CFO for accurate channel estimate
                // Reset mixer to start position and re-run
                mixer.reset();
                freq_correction_phase = 0.0f;

                // Recompute phase increment with corrected CFO
                const float* ptr2 = training_samples;
                for (auto& v : h_per_symbol) std::fill(v.begin(), v.end(), Complex(0, 0));
                std::fill(h_sum_data.begin(), h_sum_data.end(), Complex(0, 0));
                std::fill(h_sum_pilot.begin(), h_sum_pilot.end(), Complex(0, 0));

                for (size_t sym = 0; sym < num_symbols; ++sym) {
                    SampleSpan sym_span(ptr2, symbol_samples);
                    auto baseband = toBaseband(sym_span);
                    auto freq = extractSymbol(baseband, 0);

                    for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
                        int idx = data_carrier_indices[i];
                        Complex rx = freq[idx];
                        Complex tx = sync_sequence[i % sync_sequence.size()];
                        if (std::abs(tx) > 0.01f) {
                            Complex h_ls = rx / tx;
                            h_sum_data[i] += h_ls;
                            h_per_symbol[sym][i] = h_ls;
                        }
                    }
                    for (size_t i = 0; i < pilot_carrier_indices.size(); ++i) {
                        int idx = pilot_carrier_indices[i];
                        Complex rx = freq[idx];
                        Complex tx = pilot_sequence[i];
                        if (std::abs(tx) > 0.01f) {
                            h_sum_pilot[i] += rx / tx;
                        }
                    }
                    ptr2 += symbol_samples;
                }

                LOG_DEMOD(INFO, "LTS re-processed with corrected CFO=%.2f Hz", freq_offset_hz);
            } else if (std::abs(residual_cfo) > 0.1f) {
                LOG_DEMOD(DEBUG, "LTS residual CFO: %.2f Hz (below correction threshold)", residual_cfo);
            }
        }
    }

    // For data carriers: use LAST symbol's H estimate
    // This is closest in time to the first data symbol, minimizing CFO-induced phase mismatch
    // Using the first symbol caused decode failures at CFO=30 Hz due to 2-symbol phase drift
    if (valid_symbols > 0) {
        for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
            int idx = data_carrier_indices[i];
            // Use last symbol's H estimate (minimize phase mismatch with first data symbol)
            channel_estimate[idx] = h_per_symbol[num_symbols - 1][i];
        }
    }

    // Average and store channel estimate for pilot carriers
    if (valid_symbols > 0) {
        float inv_count = 1.0f / valid_symbols;
        for (size_t i = 0; i < pilot_carrier_indices.size(); ++i) {
            int idx = pilot_carrier_indices[i];
            channel_estimate[idx] = h_sum_pilot[i] * inv_count;
        }
    }

    LOG_DEMOD(INFO, "LTS channel estimate: %zu data + %zu pilot carriers",
              data_carrier_indices.size(), pilot_carrier_indices.size());

    // Compute average channel response for logging
    Complex h_avg(0, 0);
    float h_mag_sum = 0;
    for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
        int idx = data_carrier_indices[i];
        h_avg += channel_estimate[idx];
        h_mag_sum += std::abs(channel_estimate[idx]);
    }
    h_avg /= float(data_carrier_indices.size());
    float h_mag_avg = h_mag_sum / data_carrier_indices.size();

    // Estimate noise variance from LTS training symbols
    // With 2 training symbols, noise = (H1 - H2) / 2, variance = E[|H1-H2|²] / 4
    // At 0.1 Hz Doppler, channel barely changes between training symbols (~0.001 coherence),
    // so H1-H2 is almost entirely noise.
    if (valid_symbols >= 2) {
        float noise_sum = 0.0f;
        float signal_sum = 0.0f;
        int count = 0;
        for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
            Complex h0 = h_per_symbol[0][i];
            Complex h1 = h_per_symbol[1][i];
            if (std::abs(h0) > 1e-6f && std::abs(h1) > 1e-6f) {
                Complex diff = h1 - h0;
                noise_sum += std::norm(diff);  // |H1-H2|²
                signal_sum += (std::norm(h0) + std::norm(h1)) / 2.0f;
                count++;
            }
        }
        if (count > 0) {
            // noise_variance = |H1-H2|²/4 per carrier (each H has noise variance σ²)
            float lts_noise_var = noise_sum / (4.0f * count);
            float lts_signal_power = signal_sum / count;

            // Clamp SNR estimate to reasonable range (5 dB to 40 dB)
            float lts_snr = lts_signal_power / std::max(lts_noise_var, 1e-10f);
            lts_snr = std::max(3.16f, std::min(10000.0f, lts_snr));

            noise_variance = lts_noise_var;
            estimated_snr_linear = lts_snr;

            LOG_DEMOD(INFO, "LTS SNR estimate: %.1f dB (measured noise_var=%.6f, signal=%.4f)",
                      10.0f * std::log10(estimated_snr_linear), noise_variance, lts_signal_power);
        }
    } else if (h_mag_avg > 1e-6f) {
        // Fallback: only 1 training symbol, use default assumption
        float signal_power = h_mag_avg * h_mag_avg;
        noise_variance = signal_power / DEFAULT_SNR_LINEAR;
        estimated_snr_linear = DEFAULT_SNR_LINEAR;
        LOG_DEMOD(INFO, "LTS SNR estimate: %.1f dB (fallback, 1 training symbol)",
                  10.0f * std::log10(estimated_snr_linear));
    }

    LOG_DEMOD(INFO, "LTS channel estimate: %zu symbols, |H|_avg=%.3f, phase_avg=%.1f°",
              valid_symbols, h_mag_avg, std::arg(h_avg) * 180.0f / M_PI);

    // DEBUG: Print first few channel estimates from first and last training symbols
    {
        char h0_buf[128] = "", hn_buf[128] = "";
        int p0 = 0, pn = 0;
        for (size_t i = 0; i < std::min(size_t(5), data_carrier_indices.size()); ++i) {
            p0 += snprintf(h0_buf + p0, sizeof(h0_buf) - p0, "%.0f deg ", std::arg(h_per_symbol[0][i]) * 180.0f / M_PI);
            pn += snprintf(hn_buf + pn, sizeof(hn_buf) - pn, "%.0f deg ", std::arg(h_per_symbol[num_symbols - 1][i]) * 180.0f / M_PI);
        }
        LOG_DEMOD(DEBUG, "LTS H from sym0 (first 5): %s", h0_buf);
        LOG_DEMOD(DEBUG, "LTS H from sym%zu (last, first 5): %s", num_symbols - 1, hn_buf);
    }

    // === DQPSK PER-CARRIER PHASE REFERENCES ===
    // TX sends LTS with sync_sequence (Zadoff-Chu), BUT initializes dbpsk_prev_symbols to (1,0).
    // So the first data symbol is encoded as: TX_data = (1,0) × DQPSK_phase, NOT sync_seq × DQPSK.
    //
    // With CFO and timing errors, different carriers have different phase offsets (φ) in H_est.
    // RX needs a reference that has the SAME phase error as the equalized data symbol.
    //
    // Derivation:
    //   1. TX first data: (1,0) × DQPSK_phase
    //   2. RX received: (1,0) × DQPSK_phase × H
    //   3. RX equalized: (1,0) × DQPSK_phase × H / H_est = (1,0) × DQPSK_phase × e^{-jφ}
    //   4. RX reference: (1,0) × e^{-jφ} = conj(H) / |H| = conj(h_unit)
    //   5. Differential: diff = eq_data × conj(eq_ref)
    //      = (1,0) × DQPSK_phase × e^{-jφ} × conj((1,0) × e^{-jφ})
    //      = DQPSK_phase  ✓  (phase errors cancel!)
    //
    // CRITICAL: The RX reference must be (1,0) × e^{-jφ} = conj(h_unit), NOT sync_seq × e^{-jφ}.
    // This was a bug that caused first symbol decode errors when interleaving was enabled.

    lts_carrier_phases.resize(data_carrier_indices.size());

    // Compute the DQPSK reference for each carrier.
    //
    // Key insight: channel_estimate now uses LAST training symbol's H for consistency.
    // The first DATA symbol is 1 symbol after the last training symbol.
    // With CFO correction, there's a phase advance of 1 symbol between them.
    //
    // The equalized first data symbol has:
    //   eq_data = FFT(corrected_data0) / H_last
    //   = TX_data × H × e^{j×φ_data0} / (H × e^{j×φ_last})
    //   = TX_data × e^{j×(φ_data0 - φ_last)}
    //   = TX_data × e^{j×phase_per_symbol}  (1 symbol of phase advance)
    //
    // For DQPSK to work, the reference must have the same phase:
    //   eq_ref = (1,0) × e^{j×phase_per_symbol}

    float phase_inc = -2.0f * M_PI * freq_offset_hz / config.sample_rate;
    float phase_per_symbol = phase_inc * symbol_samples;

    // Phase advance from last training to first data: 1 symbol
    Complex phase_advance(std::cos(phase_per_symbol), std::sin(phase_per_symbol));

    LOG_DEMOD(DEBUG, "LTS CFO=%.1f Hz, phase_per_sym=%.0f deg, phase_advance=(%.3f,%.3f)",
              freq_offset_hz, phase_per_symbol * 180.0f / M_PI,
              phase_advance.real(), phase_advance.imag());

    for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
        // For DQPSK with pilots, the differential reference must account for
        // per-carrier phase errors in the channel estimate.
        //
        // Equalization produces: eq = rx × conj(H_est) / |H_est|²
        // If H_est has phase error φ (from noise, timing, etc.):
        //   eq = TX × H_true × conj(H_est) / |H_est|²
        //      = TX × |H_true| × e^{jθ} × |H_est| × e^{-j(θ+φ)} / |H_est|²
        //      ≈ TX × e^{-jφ}  (approximately, when |H_true| ≈ |H_est|)
        //
        // For differential decoding: diff = eq[n] × conj(ref)
        // If ref = (1,0), then diff = TX × e^{-jφ} which has the wrong phase!
        //
        // Fix: Set ref to match the phase error that will appear in equalized symbols.
        // ref[i] = unit_phase(H_est[i])^* = conj(H_est[i]) / |H_est[i]|
        //
        // Then: diff = TX × e^{-jφ} × conj(e^{-jφ}) = TX × e^{-jφ} × e^{+jφ} = TX ✓
        //
        int idx = data_carrier_indices[i];
        Complex h = channel_estimate[idx];
        float h_mag = std::abs(h);
        if (h_mag > 0.01f) {
            // Reference = unit vector in direction of conj(H)
            // This matches the phase that equalization will produce
            lts_carrier_phases[i] = std::conj(h) / h_mag;
        } else {
            lts_carrier_phases[i] = Complex(1.0f, 0.0f);
        }

        // Debug: log first 3 carrier H phases
        if (i < 3) {
            LOG_DEMOD(DEBUG, "LTS carrier %zu: H=%.1f∠%.0f° -> ref=%.0f°",
                      i, h_mag, std::arg(h) * 180.0f / M_PI,
                      std::arg(lts_carrier_phases[i]) * 180.0f / M_PI);
        }
    }

    // Also compute a single phase offset for backwards compatibility
    // (used if lts_carrier_phases is empty)
    Complex avg_h(0, 0);
    for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
        int idx = data_carrier_indices[i];
        avg_h += channel_estimate[idx] / std::abs(channel_estimate[idx] + Complex(1e-10f, 0));
    }
    avg_h /= static_cast<float>(data_carrier_indices.size());
    lts_phase_offset = avg_h / std::abs(avg_h + Complex(1e-10f, 0));

    {
        char phase_buf[128] = "";
        int pp = 0;
        for (size_t i = 0; i < std::min(size_t(5), lts_carrier_phases.size()); ++i) {
            pp += snprintf(phase_buf + pp, sizeof(phase_buf) - pp, "%.0f deg ", std::arg(lts_carrier_phases[i]) * 180.0f / M_PI);
        }
        LOG_DEMOD(DEBUG, "LTS DQPSK ref phases (first 5): %s(H avg phase=%.0f deg)", phase_buf, std::arg(lts_phase_offset) * 180.0f / M_PI);
    }

    // DON'T set carrier_phase_initialized here - let updateChannelEstimate() do it
    // on the first data symbol. This ensures we use fresh pilot data for phase
    // recovery instead of potentially noisy LTS estimates.
    //
    // The LTS channel estimates provide magnitude and approximate phase.
    // The first data symbol's pilots will refine the common phase offset.

    // Compute fading index from LTS channel estimate
    // This is critical for differential modes (DQPSK, DBPSK, D8PSK) which skip
    // updateChannelEstimate() — without this, last_fading_index stays at 0 and
    // LLR fading scaling + two-pass decoding never activate on fading channels.
    {
        float h_mag_mean = 0.0f;
        for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
            h_mag_mean += std::abs(channel_estimate[data_carrier_indices[i]]);
        }
        h_mag_mean /= data_carrier_indices.size();

        float h_mag_var = 0.0f;
        for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
            float diff = std::abs(channel_estimate[data_carrier_indices[i]]) - h_mag_mean;
            h_mag_var += diff * diff;
        }
        h_mag_var /= data_carrier_indices.size();

        last_fading_index = (h_mag_mean > 0.01f) ? std::sqrt(h_mag_var) / h_mag_mean : 0.0f;
        LOG_DEMOD(INFO, "LTS fading index: %.3f (threshold: LLR>0.15, two-pass>0.30)", last_fading_index);
    }

    // Mark that we have a valid channel estimate (for smoothing factor selection)
    snr_symbol_count = num_symbols;
}

void OFDMDemodulator::Impl::updateChannelEstimate(const std::vector<Complex>& freq_domain) {
    // Smoothing factor for channel estimate update:
    // - First symbol: alpha=1.0 (use pilot estimate directly)
    // - Subsequent symbols: depends on modulation type and pilot availability
    //
    // For DIFFERENTIAL modes (DQPSK, D8PSK, DBPSK):
    //   WITHOUT pilots: Use low alpha (0.1) to keep H stable
    //   WITH pilots: Use higher alpha (0.5) to track fading - pilots provide
    //                reliable per-symbol phase reference that makes differential
    //                decoding robust even with faster tracking
    //
    // For COHERENT modes (QPSK, QAM):
    //   Use high alpha (0.9) to track channel changes quickly.
    //
    bool is_differential = (config.modulation == Modulation::DBPSK ||
                            config.modulation == Modulation::DQPSK ||
                            config.modulation == Modulation::D8PSK);
    bool has_pilots = !pilot_carrier_indices.empty();

    // Use soft_bits.empty() to detect first DATA symbol (snr_symbol_count may be > 0 from LTS)
    bool is_first_data_symbol = soft_bits.empty();

    float alpha;
    if (is_first_data_symbol) {
        alpha = 1.0f;  // First data symbol: use pilot estimate directly (channel changed since LTS)
    } else if (is_differential) {
        // Magnitude-only tracking: updates |H| to track fading depth for MMSE,
        // while phase stays frozen from LTS. Alpha=0.5 balances fading tracking
        // vs noise smoothing (fading at 0.1Hz Doppler changes ~5%/symbol).
        alpha = has_pilots ? 0.5f : 0.1f;
    } else {
        alpha = 0.9f;  // Coherent: track channel changes
    }

    // First pass: compute all LS estimates and their average
    std::vector<Complex> h_ls_all(pilot_carrier_indices.size());
    Complex h_sum(0, 0);

    for (size_t i = 0; i < pilot_carrier_indices.size(); ++i) {
        int idx = pilot_carrier_indices[i];
        Complex rx = freq_domain[idx];
        Complex tx = pilot_sequence[i];
        h_ls_all[i] = rx / tx;
        h_sum += h_ls_all[i];
    }

    // Carrier phase recovery: compute average phase offset on first symbol
    if (!carrier_phase_initialized && !pilot_carrier_indices.empty()) {
        Complex h_avg = h_sum / float(pilot_carrier_indices.size());
        float avg_mag = std::abs(h_avg);
        if (avg_mag > 0.01f) {
            carrier_phase_correction = std::conj(h_avg) / avg_mag;
            carrier_phase_initialized = true;
            LOG_DEMOD(DEBUG, "Carrier phase recovery: avg_phase=%.1f°, applying correction",
                      std::arg(h_avg) * 180.0f / M_PI);
        }
    }

    // Apply carrier phase correction to all H estimates
    for (size_t i = 0; i < h_ls_all.size(); ++i) {
        h_ls_all[i] *= carrier_phase_correction;
    }
    h_sum *= carrier_phase_correction;

    // DEBUG: Log first symbol's pilot analysis
    if (soft_bits.empty()) {
        LOG_DEMOD(DEBUG, "=== First DATA symbol pilot analysis (snr_symbol_count=%d) ===", snr_symbol_count);
        for (size_t i = 0; i < pilot_carrier_indices.size(); ++i) {
            int idx = pilot_carrier_indices[i];
            LOG_DEMOD(DEBUG, "DATA pilot[%zu] idx=%d: rx=(%.4f,%.4f) |rx|=%.4f tx=(%.1f,%.1f) H=(%.2f,%.2f) |H|=%.2f",
                      i, idx,
                      freq_domain[idx].real(), freq_domain[idx].imag(), std::abs(freq_domain[idx]),
                      pilot_sequence[i].real(), pilot_sequence[i].imag(),
                      h_ls_all[i].real(), h_ls_all[i].imag(),
                      std::abs(h_ls_all[i]));
        }
        LOG_DEMOD(DEBUG, "H avg: (%.2f,%.2f), |H|=%.2f, phase=%.1f deg",
                  (h_sum / float(pilot_carrier_indices.size())).real(),
                  (h_sum / float(pilot_carrier_indices.size())).imag(),
                  std::abs(h_sum / float(pilot_carrier_indices.size())),
                  std::arg(h_sum / float(pilot_carrier_indices.size())) * 180.0f / M_PI);
    }

    // Compute average signal power from pilots
    float signal_power_sum = 0.0f;
    for (size_t i = 0; i < pilot_carrier_indices.size(); ++i) {
        signal_power_sum += std::norm(h_ls_all[i]);
    }
    float signal_power = signal_power_sum / pilot_carrier_indices.size();

    // Measure noise using TEMPORAL comparison
    float noise_power_sum = 0.0f;
    size_t noise_count = 0;

    for (size_t i = 0; i < pilot_carrier_indices.size(); ++i) {
        int idx = pilot_carrier_indices[i];

        if (!prev_pilot_phases.empty() && i < prev_pilot_phases.size()) {
            Complex prev_h = prev_pilot_phases[i];
            Complex curr_h = h_ls_all[i];

            if (std::norm(prev_h) > 1e-6f && std::norm(curr_h) > 1e-6f) {
                Complex diff = curr_h - prev_h;
                noise_power_sum += std::norm(diff);
                noise_count++;
            }
        }

        // Update smoothed channel estimate
        Complex h_old = channel_estimate[idx];
        if (is_differential) {
            // Magnitude-only update: track fading depth for MMSE scaling
            // while preserving the LTS phase that differential decoding needs.
            // new_mag = alpha * |H_pilot| + (1-alpha) * |H_old|
            float new_mag = alpha * std::abs(h_ls_all[i]) + (1.0f - alpha) * std::abs(h_old);
            float old_phase = std::arg(h_old);
            channel_estimate[idx] = std::polar(new_mag, old_phase);
        } else {
            channel_estimate[idx] = alpha * h_ls_all[i] + (1.0f - alpha) * h_old;
        }
    }

    // First symbol fallback: assume 15 dB SNR
    if (noise_count == 0) {
        noise_power_sum = signal_power / DEFAULT_SNR_LINEAR;
        noise_count = 1;
    }

    // === Frequency offset estimation from pilot phase differences ===
    // Skip for differential modes: fading-induced phase changes get misattributed to CFO,
    // causing the estimate to drift. Differential modes use chirp/LTS CFO only.
    if (!is_differential && !prev_pilot_phases.empty() && prev_pilot_phases.size() == h_ls_all.size()) {
        Complex phase_diff_sum(0, 0);
        int valid_count = 0;

        for (size_t i = 0; i < h_ls_all.size(); ++i) {
            Complex diff = h_ls_all[i] * std::conj(prev_pilot_phases[i]);

            if (std::norm(prev_pilot_phases[i]) > 1e-6f &&
                std::norm(h_ls_all[i]) > 1e-6f) {
                float mag = std::abs(diff);
                if (mag > 1e-6f) {
                    phase_diff_sum += diff / mag;
                    valid_count++;
                }
            }
        }

        if (valid_count > 0) {
            Complex avg_diff = phase_diff_sum / static_cast<float>(valid_count);
            float avg_phase_diff = std::atan2(avg_diff.imag(), avg_diff.real());

            pilot_phase_correction = Complex(std::cos(-avg_phase_diff), std::sin(-avg_phase_diff));

            float symbol_duration = static_cast<float>(config.getSymbolDuration()) /
                                   static_cast<float>(config.sample_rate);
            float residual_cfo = avg_phase_diff / (2.0f * M_PI * symbol_duration);
            float total_cfo = freq_offset_hz + residual_cfo;

            // Adaptive alpha for CFO tracking
            float adaptive_alpha = FREQ_OFFSET_ALPHA;
            if (symbols_since_sync < CFO_ACQUISITION_SYMBOLS) {
                float progress = static_cast<float>(symbols_since_sync) / CFO_ACQUISITION_SYMBOLS;
                adaptive_alpha = 0.9f * (1.0f - progress) + FREQ_OFFSET_ALPHA * progress;
            }
            if (std::abs(residual_cfo) > 10.0f) {
                adaptive_alpha = std::max(adaptive_alpha, 0.9f);
            }
            symbols_since_sync++;

            freq_offset_filtered = adaptive_alpha * total_cfo +
                                  (1.0f - adaptive_alpha) * freq_offset_filtered;

            freq_offset_hz = std::max(-MAX_CFO_HZ, std::min(MAX_CFO_HZ, freq_offset_filtered));

            LOG_DEMOD(TRACE, "Freq offset: residual=%.2f Hz, total=%.2f Hz, filtered=%.2f Hz",
                     residual_cfo, total_cfo, freq_offset_hz);
        }
    } else {
        pilot_phase_correction = Complex(1, 0);
    }

    // === Symbol timing recovery from pilot phase slope ===
    // Skip for differential modes (same reason as CFO — fading corrupts phase slope)
    if (!is_differential && snr_symbol_count >= 3) {
        float sum_k = 0, sum_k2 = 0, sum_phase = 0, sum_k_phase = 0;
        int timing_valid_count = 0;

        for (size_t i = 0; i < h_ls_all.size(); ++i) {
            if (std::norm(h_ls_all[i]) < 1e-6f) continue;

            int k = pilot_carrier_indices[i];
            if (k > (int)config.fft_size / 2) k -= config.fft_size;

            float phase = std::arg(h_ls_all[i]);

            sum_k += k;
            sum_k2 += k * k;
            sum_phase += phase;
            sum_k_phase += k * phase;
            timing_valid_count++;
        }

        if (timing_valid_count >= 3) {
            float n = timing_valid_count;
            float denom = n * sum_k2 - sum_k * sum_k;
            if (std::abs(denom) > 1e-6f) {
                float slope = (n * sum_k_phase - sum_k * sum_phase) / denom;
                float instantaneous_timing = slope * config.fft_size / (2.0f * M_PI);

                timing_offset_samples = TIMING_ALPHA * instantaneous_timing +
                                       (1.0f - TIMING_ALPHA) * timing_offset_samples;

                float max_timing = 50.0f * (config.fft_size / 512.0f);
                timing_offset_samples = std::max(-max_timing, std::min(max_timing, timing_offset_samples));

                LOG_DEMOD(DEBUG, "Timing recovery: instant=%.2f samp, filtered=%.2f samp (slope=%.4f rad/bin)",
                         instantaneous_timing, timing_offset_samples, slope);
            }
        }
    }

    // Store current pilots for next symbol
    prev_pilot_phases = h_ls_all;

    // === COHERENT MODE FIX: Remove/add timing phase for correct interpolation ===
    bool is_coherent = (config.modulation != Modulation::DBPSK &&
                       config.modulation != Modulation::DQPSK &&
                       config.modulation != Modulation::D8PSK);

    if (is_coherent && snr_symbol_count < 3) {
        LOG_DEMOD(INFO, "Coherent timing fix: is_coherent=%d, timing_offset=%.2f, sym=%d",
                  is_coherent ? 1 : 0, timing_offset_samples, snr_symbol_count);
    }

    // Step 1: Remove timing phase from pilots before interpolation
    if (is_coherent && std::abs(timing_offset_samples) > 0.1f) {
        for (int idx : pilot_carrier_indices) {
            int k = idx;
            if (k > (int)config.fft_size / 2) k -= config.fft_size;
            float timing_phase = 2.0f * M_PI * k * timing_offset_samples / config.fft_size;
            Complex timing_removal = std::exp(Complex(0, -timing_phase));

            if (snr_symbol_count == 0 && k >= -2 && k <= 3) {
                Complex before = channel_estimate[idx];
                Complex after = before * timing_removal;
                LOG_DEMOD(DEBUG, "TimingFix k=%d: before=(%.2f,%.2f) phase=%.1f° "
                          "remove=%.1f° after=(%.2f,%.2f) phase=%.1f°",
                          k, before.real(), before.imag(),
                          std::arg(before) * 180.0f / M_PI,
                          -timing_phase * 180.0f / M_PI,
                          after.real(), after.imag(),
                          std::arg(after) * 180.0f / M_PI);
            }

            channel_estimate[idx] *= timing_removal;
        }
    }

    // Interpolate between pilots
    if (is_differential) {
        // For differential modes: interpolate MAGNITUDES only to data carriers,
        // preserving existing phases. This tracks fading depth for MMSE scaling
        // without corrupting the phase relationship that differential decoding needs.
        // DFT interpolation would change data carrier phases (producing time-varying
        // H that introduces artificial differential phase errors).
        for (size_t dc = 0; dc < interp_table.size(); ++dc) {
            const auto& info = interp_table[dc];
            float interp_mag = 0.0f;
            if (info.lower_pilot >= 0 && info.upper_pilot >= 0) {
                float m1 = std::abs(channel_estimate[info.lower_pilot]);
                float m2 = std::abs(channel_estimate[info.upper_pilot]);
                interp_mag = (1.0f - info.alpha) * m1 + info.alpha * m2;
            } else if (info.lower_pilot >= 0) {
                interp_mag = std::abs(channel_estimate[info.lower_pilot]);
            } else if (info.upper_pilot >= 0) {
                interp_mag = std::abs(channel_estimate[info.upper_pilot]);
            }
            float old_phase = std::arg(channel_estimate[info.fft_idx]);
            channel_estimate[info.fft_idx] = std::polar(interp_mag, old_phase);
        }
    } else {
        interpolateChannel();
    }

    // Step 2: Add back correct timing phase for each carrier
    if (is_coherent && std::abs(timing_offset_samples) > 0.1f) {
        for (int idx : pilot_carrier_indices) {
            int k = idx;
            if (k > (int)config.fft_size / 2) k -= config.fft_size;
            float timing_phase = 2.0f * M_PI * k * timing_offset_samples / config.fft_size;
            Complex timing_correction = std::exp(Complex(0, timing_phase));
            channel_estimate[idx] *= timing_correction;
        }
        for (int idx : data_carrier_indices) {
            int k = idx;
            if (k > (int)config.fft_size / 2) k -= config.fft_size;
            float timing_phase = 2.0f * M_PI * k * timing_offset_samples / config.fft_size;
            Complex timing_correction = std::exp(Complex(0, timing_phase));
            channel_estimate[idx] *= timing_correction;
        }
    }

    // Initialize adaptive equalizer weights from pilot-based estimate
    if (config.adaptive_eq_enabled) {
        for (int idx : data_carrier_indices) {
            if (snr_symbol_count < 3) {
                lms_weights[idx] = channel_estimate[idx];
            }
        }
        for (int idx : pilot_carrier_indices) {
            if (snr_symbol_count < 3) {
                lms_weights[idx] = channel_estimate[idx];
            }
        }
    }

    // Update noise variance and SNR
    // Note: noise_count == 1 means first symbol fallback (no prev_pilot_phases yet)
    // In that case, noise_power_sum = signal_power / DEFAULT_SNR_LINEAR is already the variance
    //
    // CRITICAL FIX FOR FADING CHANNELS:
    // On fading channels, H[n] - H[n-1] includes BOTH noise AND fading variation.
    // The temporal comparison cannot distinguish them, causing noise_variance to be
    // massively overestimated → compressed LLRs → LDPC decode failure.
    //
    // Solution: Detect fading from pilot magnitude variance and use LTS-based SNR
    // estimate when fading is significant. The pilots still track the channel
    // (for equalization), but we don't let fading contaminate noise_variance.

    // Compute fading index from pilot magnitude variance
    // High variance across carriers indicates frequency-selective fading
    float h_mag_mean = 0.0f;
    for (size_t i = 0; i < h_ls_all.size(); ++i) {
        h_mag_mean += std::abs(h_ls_all[i]);
    }
    h_mag_mean /= h_ls_all.size();

    float h_mag_variance = 0.0f;
    for (size_t i = 0; i < h_ls_all.size(); ++i) {
        float diff = std::abs(h_ls_all[i]) - h_mag_mean;
        h_mag_variance += diff * diff;
    }
    h_mag_variance /= h_ls_all.size();

    // Fading index: normalized magnitude variance (0 = flat, >0.1 = fading)
    float fading_index = (h_mag_mean > 0.01f) ? std::sqrt(h_mag_variance) / h_mag_mean : 0.0f;

    // Store for external access (GUI display, rate adaptation)
    last_fading_index = fading_index;

    if (noise_count > 0 && noise_power_sum > 0.0f) {
        // CRITICAL FIX FOR DIFFERENTIAL MODES ON FADING CHANNELS:
        //
        // The temporal H difference (curr_h - prev_h) includes BOTH noise AND fading variation.
        // We cannot distinguish them, so for DIFFERENTIAL modes (DQPSK, DBPSK, D8PSK):
        //   - Only use the FIRST symbol's SNR estimate (based on DEFAULT_SNR_LINEAR assumption)
        //   - Don't update noise_variance from temporal comparison (it would include fading)
        //
        // This works because:
        //   1. Differential decoding is robust to slow phase changes (fading)
        //   2. R1/4 LDPC is very robust - noise_variance just needs to be in the right ballpark
        //   3. The equalization uses pilots to track H, which is separate from noise estimation
        //
        // For COHERENT modes, we still use temporal comparison (they need accurate noise estimate).

        if (noise_count == 1 && !is_differential) {
            // First symbol, coherent mode: use the fallback SNR estimate
            noise_variance = noise_power_sum;
            if (noise_variance < 1e-6f) noise_variance = 1e-6f;

            float instantaneous_snr = signal_power / noise_variance;
            instantaneous_snr = std::max(0.1f, std::min(10000.0f, instantaneous_snr));
            estimated_snr_linear = instantaneous_snr;  // First symbol: no smoothing

            LOG_DEMOD(DEBUG, "First symbol: noise_var=%.6f, SNR=%.1f dB, fading_idx=%.3f",
                      noise_variance, 10.0f * std::log10(estimated_snr_linear), fading_index);
        } else if (!is_differential) {
            // Coherent mode: use temporal comparison (they need it)
            noise_variance = noise_power_sum / (noise_count - 1);
            if (noise_variance < 1e-6f) noise_variance = 1e-6f;

            float instantaneous_snr = signal_power / noise_variance;
            instantaneous_snr = std::max(0.1f, std::min(10000.0f, instantaneous_snr));
            estimated_snr_linear = snr_alpha * instantaneous_snr + (1.0f - snr_alpha) * estimated_snr_linear;
        }
        // For differential modes after first symbol: DON'T update noise_variance
        // This prevents fading from corrupting the noise estimate
    }

    snr_symbol_count++;
}

// =============================================================================
// CHANNEL INTERPOLATION
// =============================================================================

void OFDMDemodulator::Impl::interpolateChannel() {
    // DFT-based channel interpolation:
    // 1. Build N-point frequency vector: pilot H at known positions, linear interp elsewhere
    // 2. IDFT → channel impulse response (CIR) in delay domain
    // 3. Window: zero taps beyond expected delay spread (noise suppression)
    // 4. DFT back → clean H at every carrier
    //
    // This exploits the finite delay spread of the HF channel:
    // Good fading: 0.5ms delay, Moderate: 1.0ms
    // At 46.875 Hz carrier spacing, bandwidth = 59 × 46.875 = 2766 Hz
    // CIR tap spacing = 1/2766 Hz ≈ 0.36ms → keep ~5 taps for 1.8ms coverage
    //
    // Benefits over linear interpolation:
    // - Noise suppression: zeroing high-delay taps removes pilot estimation noise
    // - Smooth interpolation: DFT naturally produces band-limited frequency response
    // - No phase discontinuity issues at pilot boundaries

    size_t N = all_carrier_fft_indices.size();  // Total carriers (59)
    size_t N_p = pilot_carrier_indices.size();

    if (N_p < 2 || N == 0) {
        // Fallback: linear interpolation
        for (size_t dc = 0; dc < interp_table.size(); ++dc) {
            const auto& info = interp_table[dc];
            if (info.lower_pilot >= 0 && info.upper_pilot >= 0) {
                Complex H1 = channel_estimate[info.lower_pilot];
                Complex H2 = channel_estimate[info.upper_pilot];
                channel_estimate[info.fft_idx] = (1.0f - info.alpha) * H1 + info.alpha * H2;
            } else if (info.lower_pilot >= 0) {
                channel_estimate[info.fft_idx] = channel_estimate[info.lower_pilot];
            } else if (info.upper_pilot >= 0) {
                channel_estimate[info.fft_idx] = channel_estimate[info.upper_pilot];
            }
        }
        return;
    }

    // Step 1: Build N-point H vector with pilot values and linear interp between them
    // This gives IDFT a good starting point (better than zeros at non-pilot positions)
    std::vector<Complex> H_full(N);

    // First, place pilot H values at their logical positions
    // and track pilot logical indices for interpolation
    std::vector<int> pilot_logical_pos;
    for (size_t i = 0; i < N; ++i) {
        if (is_pilot_logical[i]) {
            H_full[i] = channel_estimate[all_carrier_fft_indices[i]];
            pilot_logical_pos.push_back(static_cast<int>(i));
        }
    }

    // Linear interpolation between pilots (as initial fill)
    for (size_t seg = 0; seg < pilot_logical_pos.size(); ++seg) {
        int p1 = pilot_logical_pos[seg];
        int p2 = (seg + 1 < pilot_logical_pos.size())
                     ? pilot_logical_pos[seg + 1]
                     : static_cast<int>(N);  // extrapolate past last pilot
        Complex H1 = H_full[p1];
        Complex H2 = (seg + 1 < pilot_logical_pos.size()) ? H_full[p2] : H1;

        for (int i = p1 + 1; i < p2 && i < static_cast<int>(N); ++i) {
            float t = static_cast<float>(i - p1) / static_cast<float>(p2 - p1);
            H_full[i] = (1.0f - t) * H1 + t * H2;
        }
    }
    // Extrapolate before first pilot
    if (!pilot_logical_pos.empty() && pilot_logical_pos[0] > 0) {
        Complex H0 = H_full[pilot_logical_pos[0]];
        for (int i = 0; i < pilot_logical_pos[0]; ++i) {
            H_full[i] = H0;
        }
    }

    // Step 2: IDFT → CIR (N-point, small enough for direct computation)
    std::vector<Complex> h_cir(N);
    float inv_N = 1.0f / static_cast<float>(N);
    for (size_t n = 0; n < N; ++n) {
        Complex sum(0, 0);
        for (size_t k = 0; k < N; ++k) {
            float phase = 2.0f * M_PI * k * n * inv_N;
            sum += H_full[k] * Complex(std::cos(phase), std::sin(phase));
        }
        h_cir[n] = sum * inv_N;
    }

    // Step 3: Window — keep first L taps and last L-1 taps (symmetric CIR)
    // CIR tap spacing = 1/bandwidth = 1/(N × 46.875 Hz) ≈ 0.36ms
    // Keep L=5 taps → covers ±1.8ms delay spread (enough for moderate fading)
    // Taps [0..L-1] = causal (positive delays), [N-L+1..N-1] = acausal (negative delays)
    size_t L = 5;
    if (L > N / 2) L = N / 2;
    for (size_t n = L; n < N - L + 1; ++n) {
        h_cir[n] = Complex(0, 0);
    }

    // Step 4: DFT → clean interpolated H at all carriers
    std::vector<Complex> H_clean(N);
    for (size_t k = 0; k < N; ++k) {
        Complex sum(0, 0);
        for (size_t n = 0; n < N; ++n) {
            float phase = -2.0f * M_PI * k * n * inv_N;
            sum += h_cir[n] * Complex(std::cos(phase), std::sin(phase));
        }
        H_clean[k] = sum;
    }

    // Step 5: Write clean H to data carrier positions only
    // Pilots keep their direct LS estimates (more accurate at pilot positions)
    for (size_t i = 0; i < N; ++i) {
        if (!is_pilot_logical[i]) {
            channel_estimate[all_carrier_fft_indices[i]] = H_clean[i];
        }
    }
}

// =============================================================================
// HARD DECISION SLICER
// =============================================================================

Complex OFDMDemodulator::Impl::hardDecision(Complex sym, Modulation mod) const {
    switch (mod) {
        case Modulation::BPSK:
            return Complex(sym.real() > 0 ? 1.0f : -1.0f, 0);

        case Modulation::QPSK: {
            float I = sym.real() > 0 ? 0.7071f : -0.7071f;
            float Q = sym.imag() > 0 ? 0.7071f : -0.7071f;
            return Complex(I, Q);
        }

        case Modulation::QAM16: {
            auto slice = [](float x) -> float {
                if (x < -0.4f) return -0.9487f;
                if (x < 0.0f) return -0.3162f;
                if (x < 0.4f) return 0.3162f;
                return 0.9487f;
            };
            return Complex(slice(sym.real()), slice(sym.imag()));
        }

        case Modulation::QAM32: {
            auto slice_i = [](float x) -> float {
                constexpr float d = QAM32_SCALE;
                if (x < -2*d) return -3*d;
                if (x < 0) return -d;
                if (x < 2*d) return d;
                return 3*d;
            };
            auto slice_q = [](float x) -> float {
                constexpr float d = QAM32_SCALE;
                if (x < -6*d) return -7*d;
                if (x < -4*d) return -5*d;
                if (x < -2*d) return -3*d;
                if (x < 0) return -d;
                if (x < 2*d) return d;
                if (x < 4*d) return 3*d;
                if (x < 6*d) return 5*d;
                return 7*d;
            };
            return Complex(slice_i(sym.real()), slice_q(sym.imag()));
        }

        case Modulation::QAM64: {
            auto slice = [](float x) -> float {
                constexpr float d = 0.1543f;
                if (x < -6*d) return -7*d;
                if (x < -4*d) return -5*d;
                if (x < -2*d) return -3*d;
                if (x < 0) return -d;
                if (x < 2*d) return d;
                if (x < 4*d) return 3*d;
                if (x < 6*d) return 5*d;
                return 7*d;
            };
            return Complex(slice(sym.real()), slice(sym.imag()));
        }

        default:
            return Complex(sym.real() > 0 ? 0.7071f : -0.7071f,
                          sym.imag() > 0 ? 0.7071f : -0.7071f);
    }
}

// =============================================================================
// ADAPTIVE EQUALIZER UPDATES
// =============================================================================

void OFDMDemodulator::Impl::lmsUpdate(int idx, Complex received, Complex reference) {
    float mu = config.lms_mu;
    Complex error = received - lms_weights[idx] * reference;
    lms_weights[idx] += mu * std::conj(reference) * error;
}

void OFDMDemodulator::Impl::rlsUpdate(int idx, Complex received, Complex reference) {
    float lambda = config.rls_lambda;
    float P = rls_P[idx];
    float ref_norm = std::norm(reference);

    float k = P / (lambda + P * ref_norm);
    Complex error = received - lms_weights[idx] * reference;

    lms_weights[idx] += k * std::conj(reference) * error;
    rls_P[idx] = (P - k * ref_norm * P) / lambda;
    rls_P[idx] = std::max(ADAPTIVE_EQ_P_MIN, std::min(ADAPTIVE_EQ_P_MAX, rls_P[idx]));
}

// =============================================================================
// EQUALIZATION
// =============================================================================

std::vector<Complex> OFDMDemodulator::Impl::equalize(const std::vector<Complex>& freq_domain, Modulation mod) {
    std::vector<Complex> equalized(data_carrier_indices.size());
    carrier_noise_var.resize(data_carrier_indices.size());

    // For differential modulation: apply pilot_phase_correction to track common phase drift
    // This is updated after each symbol via decision-directed tracking
    bool is_differential = (mod == Modulation::DBPSK || mod == Modulation::DQPSK || mod == Modulation::D8PSK);

    if (is_differential) {
        // For differential modes on fading channels, use MMSE equalization.
        //
        // Key insight: ZF equalization (divide by H) amplifies noise on deeply
        // faded carriers. MMSE adds noise variance to the denominator, limiting
        // noise boost while accepting some signal distortion on weak carriers.
        //
        // MMSE: equalized = conj(H) * rx / (|H|² + σ²)
        // ZF:   equalized = conj(H) * rx / |H|²
        //
        // For deep fades: |H|² << σ², MMSE ≈ conj(H) * rx / σ² (bounded)
        //                           ZF  ≈ conj(H) * rx / tiny  (explodes)

        // First pass: compute average channel power for fade detection
        float avg_h_power = 0.0f;
        for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
            int idx = data_carrier_indices[i];
            avg_h_power += std::norm(channel_estimate[idx]);
        }
        avg_h_power /= data_carrier_indices.size();
        float fade_threshold = FADE_THRESHOLD_RATIO * avg_h_power;

        // Noise variance for MMSE equalization and LLR computation
        // Uses global average from LTS (per-carrier estimates are too noisy with only 2 samples)
        float scaled_noise_var = noise_variance;
        if (scaled_noise_var < 1e-6f) {
            scaled_noise_var = avg_h_power / DEFAULT_SNR_LINEAR;
        }

        // Debug: log first symbol equalization details
        static int eq_log_count = 0;

        for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
            int idx = data_carrier_indices[i];
            Complex received = freq_domain[idx];
            Complex h = channel_estimate[idx];
            float h_power = std::norm(h);

            // MMSE equalization: conj(H) / (|H|² + σ²)
            float mmse_denom = h_power + scaled_noise_var;
            if (mmse_denom < 1e-10f) {
                equalized[i] = Complex(0, 0);
                carrier_noise_var[i] = MAX_CARRIER_NOISE_VAR;
            } else {
                equalized[i] = received * std::conj(h) / mmse_denom;

                if (eq_log_count < 3 && i < 3) {
                    float rx_phase = std::arg(received) * 180.0f / M_PI;
                    float h_phase = std::arg(h) * 180.0f / M_PI;
                    float eq_phase = std::arg(equalized[i]) * 180.0f / M_PI;
                    LOG_DEMOD(INFO, "EQ car %zu: rx=%.1f∠%.0f° H=%.1f∠%.0f° -> eq=%.2f∠%.0f° (rx-H=%.0f°)",
                              i, std::abs(received), rx_phase, std::abs(h), h_phase,
                              std::abs(equalized[i]), eq_phase, rx_phase - h_phase);
                }
                // MMSE output noise variance: σ² / (|H|² + σ²) after equalization
                carrier_noise_var[i] = scaled_noise_var / (h_power + scaled_noise_var);
            }

            // Soft erasure for deeply faded carriers
            // This tells LDPC "don't trust this carrier" by increasing noise variance
            if (h_power < fade_threshold) {
                carrier_noise_var[i] = MAX_CARRIER_NOISE_VAR;
            }

            carrier_noise_var[i] = std::max(MIN_CARRIER_NOISE_VAR, std::min(MAX_CARRIER_NOISE_VAR, carrier_noise_var[i]));
        }
        eq_log_count++;
        return equalized;
    }

    bool use_adaptive = config.adaptive_eq_enabled;

    for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
        int idx = data_carrier_indices[i];
        Complex received = freq_domain[idx];

        if (use_adaptive) {
            Complex h = lms_weights[idx];
            float h_power = std::norm(h);

            // MMSE equalization
            float mmse_denom = h_power + noise_variance;
            if (mmse_denom < 1e-10f) {
                equalized[i] = Complex(0, 0);
                carrier_noise_var[i] = MAX_CARRIER_NOISE_VAR;
            } else {
                equalized[i] = std::conj(h) * received / mmse_denom;
                carrier_noise_var[i] = noise_variance / (h_power + 1e-6f);
            }

            // Decision-directed update
            if (config.decision_directed) {
                Complex decision = hardDecision(equalized[i], mod);

                if (config.adaptive_eq_use_rls) {
                    rlsUpdate(idx, received, decision);
                } else {
                    lmsUpdate(idx, received, decision);
                }

                last_decisions[idx] = decision;
            }
        } else {
            // MMSE equalization with pilot-based channel estimate
            Complex h = channel_estimate[idx];
            float h_power = std::norm(h);

            float mmse_denom = h_power + noise_variance;
            if (mmse_denom < 1e-10f) {
                equalized[i] = Complex(0, 0);
                carrier_noise_var[i] = MAX_CARRIER_NOISE_VAR;
            } else {
                equalized[i] = std::conj(h) * received / mmse_denom;
                carrier_noise_var[i] = noise_variance / (h_power + 1e-6f);
                carrier_noise_var[i] = std::max(MIN_CARRIER_NOISE_VAR, std::min(MAX_CARRIER_NOISE_VAR, carrier_noise_var[i]));
            }
        }
    }

    // Detect deep fades and apply soft erasure
    float avg_h_power = 0.0f;
    for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
        int idx = data_carrier_indices[i];
        avg_h_power += std::norm(channel_estimate[idx]);
    }
    avg_h_power /= data_carrier_indices.size();

    float fade_threshold = FADE_THRESHOLD_RATIO * avg_h_power;
    for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
        int idx = data_carrier_indices[i];
        float h_power = std::norm(channel_estimate[idx]);
        if (h_power < fade_threshold) {
            carrier_noise_var[i] = MAX_CARRIER_NOISE_VAR;  // Soft erasure
        }
    }

    return equalized;
}

std::vector<Complex> OFDMDemodulator::Impl::equalize(const std::vector<Complex>& freq_domain) {
    auto result = equalize(freq_domain, config.modulation);

    // DEBUG: Print first few equalized symbols on first data symbol
    if (soft_bits.empty() && !result.empty()) {
        char eq_buf[256] = "";
        int ep = 0;
        for (size_t i = 0; i < std::min(size_t(5), result.size()); ++i) {
            ep += snprintf(eq_buf + ep, sizeof(eq_buf) - ep, "(%.3f,%.3f) ", result[i].real(), result[i].imag());
        }
        LOG_DEMOD(DEBUG, "EQ first 5 equalized (sym 0): %s", eq_buf);
    }

    return result;
}

} // namespace ultra
