// OFDM LTS channel estimation
// Part of OFDMDemodulator::Impl

#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "demodulator_impl.hpp"
#include "demodulator_constants.hpp"
#include "sim/channel_calibration.hpp"
#include "ultra/logging.hpp"

namespace ultra {

using namespace demod_constants;

void OFDMDemodulator::Impl::updateLastSNREstimate(float signal_power,
                                                  float noise_power,
                                                  size_t independent_bins,
                                                  float alpha,
                                                  bool fitted_common_gain,
                                                  bool noise_reference_only,
                                                  float noise_power_reference_scale) {
    if (signal_power <= 0.0f || noise_power <= 0.0f || independent_bins == 0) {
        return;
    }

    // LTS/pilot residuals are measured in FFT-bin channel-estimate units.
    // Near-AWGN pilot residuals use signal/noise so real signal fades remain
    // visible. Under selective fading, temporal pilot residuals are not a clean
    // noise proxy, so the caller can keep the LTS residual and reference it to
    // the calibrated broadband noise floor instead.
    //
    // Absolute SNR calibration derivation:
    //
    //   TX mapping in src/ofdm/modulator.cpp:
    //     - data, LTS, and pilots are unit-magnitude complex subcarrier symbols
    //       (DQPSK differential states, Zadoff-Chu LTS, and BPSK pilots all have
    //       |X_k| = 1), so the pilot-vs-data power ratio is 1.0 (0 dB).
    //     - FFT::inverse() applies 1/N, and FFT::forward() applies 1.0.
    //     - real passband modulation makes each received positive-frequency bin
    //       carry output_scale/2, but that signal factor cancels in the LS
    //       residual R_k / X_k used here.
    //
    //   AWGN broadband reference:
    //     - SimulatedChannel sizes real white audio noise as
    //       sigma^2 = kModemReferencePower / SNR_broadband.
    //     - after downconversion, an unnormalized N-point FFT gives each active
    //       complex bin noise power N * sigma^2.
    //     - therefore SNR_broadband = N * kModemReferencePower / noise_bin.
    //     - operator/rate-selection SNR is in-band, matching the idle-noise
    //       estimator. For white noise, in-band noise is the 50-2950 Hz FIR
    //       output power, so SNR_in_band = SNR_broadband / FIR_energy.
    //
    //   Two-LTS residual normalization:
    //     - estimateChannelFromLTS() forms noise_power = E|H1 - H0|^2 / 4.
    //     - for independent per-symbol FFT-bin noises, E|H1 - H0|^2 = 2N*sigma^2.
    //     - so the stored noise_power is N*sigma^2/2, i.e. 3.0103 dB too small
    //       for a single-symbol FFT-bin noise reference.
    //
    // The caller-supplied reference scale converts its residual to a
    // single-symbol FFT-bin noise reference. For the repeated-LTS difference
    // estimator this scale is 2.0, which accounts for the observed +2.71 dB
    // AWGN bias without a fitted offset; the remaining ~0.3 dB is
    // finite-sample/channel-search variance. Guard-bin FFT noise estimates
    // already use single-symbol bin power, so they use scale 1.0.
    if (noise_reference_only) {
        const float corrected_noise_power =
            std::max(noise_power_reference_scale, 1.0e-6f) * noise_power;
        const float broadband_snr_db = 10.0f * std::log10(
            static_cast<float>(config.fft_size * sim::kModemReferencePower) /
            std::max(corrected_noise_power, 1.0e-12f));
        const float in_band_snr_db =
            sim::broadbandToInBandSnrDb(broadband_snr_db);

        if (!std::isfinite(in_band_snr_db)) {
            return;
        }

        if (!last_snr_db_estimate_valid) {
            last_snr_db_estimate = in_band_snr_db;
            last_snr_db_estimate_valid = true;
        } else {
            const float a = std::clamp(alpha, 0.0f, 1.0f);
            last_snr_db_estimate =
                a * in_band_snr_db + (1.0f - a) * last_snr_db_estimate;
        }
        return;
    }

    // signal/noise is a frequency-bin SNR. Convert it through the modem's
    // calibrated broadband-audio reference and then to the shared in-band
    // operator/rate-selection SNR convention:
    //
    //   TX real passband carrier amplitude at the FFT bin is output_scale/2.
    //   White audio noise is integrated by the N-point FFT.
    //   The configured/reference SNR uses kModemReferencePower over the full
    //   OFDM symbol duration including CP.
    //
    // This subtracts OFDM measurement gain while retaining true signal fades in
    // signal_power. Averaging bins reduces estimator variance; it is not RF
    // gain. The temporal pilot path also fits one common complex gain, removing
    // one complex degree of freedom, so undo that small residual-noise bias.
    const float snr_per_carrier_db =
        10.0f * std::log10(signal_power / std::max(noise_power, 1.0e-12f));
    const double carrier_signal_power =
        static_cast<double>(config.output_scale) *
        static_cast<double>(config.output_scale) * 0.25;
    const double fft_noise_reference =
        static_cast<double>(config.fft_size) * sim::kModemReferencePower;
    const double cp_reference =
        static_cast<double>(config.fft_size + config.getCyclicPrefix()) /
        static_cast<double>(config.fft_size);
    double measurement_gain = carrier_signal_power / fft_noise_reference;
    measurement_gain *= cp_reference;
    if (fitted_common_gain && independent_bins > 1) {
        measurement_gain *= static_cast<double>(independent_bins) /
                            static_cast<double>(independent_bins - 1);
    }

    if (measurement_gain <= 0.0) {
        return;
    }

    const float broadband_snr_db =
        snr_per_carrier_db - 10.0f * std::log10(static_cast<float>(measurement_gain));
    const float in_band_snr_db =
        sim::broadbandToInBandSnrDb(broadband_snr_db);

    if (!std::isfinite(in_band_snr_db)) {
        return;
    }

    // Temporal pilot residuals can contain residual channel motion even when
    // the magnitude-variance gate says "near AWGN". Do not let that path
    // overrule the same-frame LTS noise estimate with a large downward jump.
    if (fitted_common_gain && last_snr_db_estimate_valid &&
        in_band_snr_db < last_snr_db_estimate - 3.0f) {
        return;
    }

    if (!last_snr_db_estimate_valid) {
        last_snr_db_estimate = in_band_snr_db;
        last_snr_db_estimate_valid = true;
    } else {
        const float a = std::clamp(alpha, 0.0f, 1.0f);
        last_snr_db_estimate =
            a * in_band_snr_db + (1.0f - a) * last_snr_db_estimate;
    }
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

    // Store per-symbol data-carrier channel estimates for LTS difference noise
    // and residual-CFO estimation.
    std::vector<std::vector<Complex>> h_per_symbol(num_symbols);
    for (auto& v : h_per_symbol) v.resize(data_carrier_indices.size());

    // Accumulate channel estimates from each training symbol
    std::vector<Complex> h_sum_data(data_carrier_indices.size(), Complex(0, 0));
    std::vector<Complex> h_sum_pilot(pilot_carrier_indices.size(), Complex(0, 0));
    std::vector<Complex> h_last_pilot(pilot_carrier_indices.size(), Complex(0, 0));
    size_t valid_symbols = 0;
    const float phase_at_training_start = freq_correction_phase;

    double guard_noise_power_sum = 0.0;
    size_t guard_noise_bin_count = 0;

    auto accumulate_guard_noise = [&](const std::vector<Complex>& freq_domain) {
        // After downconversion, the desired OFDM signal occupies active positive
        // bins +1..+ceil(Ncarriers/2). The real-passband image lands on the
        // negative side near -2*center_freq, so the adjacent positive guard bins
        // are signal-free FFT-bin noise samples with the same N*sigma^2 scaling
        // as active-carrier LS residuals.
        const int positive_active_edge = static_cast<int>((config.num_carriers + 1) / 2);
        const int guard_start = positive_active_edge + 2;  // one-bin cushion for residual CFO
        const int nyquist_bin = static_cast<int>(config.fft_size / 2);
        const int guard_stop = std::min(
            nyquist_bin - 1,
            guard_start + static_cast<int>(config.num_carriers) - 1);
        for (int bin = guard_start; bin <= guard_stop; ++bin) {
            if (bin >= 0 && static_cast<size_t>(bin) < freq_domain.size()) {
                guard_noise_power_sum += static_cast<double>(std::norm(freq_domain[bin]));
                ++guard_noise_bin_count;
            }
        }
    };

    // Process each training symbol using the main mixer (it will be advanced)
    const float* ptr = training_samples;
    for (size_t sym = 0; sym < num_symbols; ++sym) {
        // Use toBaseband and extractSymbol like normal demodulation
        SampleSpan sym_span(ptr, symbol_samples);
        const auto& baseband = toBaseband(sym_span);
        const auto& freq = extractSymbol(baseband, 0);

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

        accumulate_guard_noise(freq);

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
                h_last_pilot[i] = h_ls;  // Keep last symbol's estimate
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
    last_lts_residual_cfo_hz = 0.0f;
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
            last_lts_residual_cfo_hz = residual_cfo;

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
                // Preserve original phase baseline at training start.
                // Resetting to zero here breaks phase consistency when processing
                // starts at non-zero absolute sample positions.
                freq_correction_phase = phase_at_training_start;

                // Recompute phase increment with corrected CFO
                const float* ptr2 = training_samples;
                for (auto& v : h_per_symbol) std::fill(v.begin(), v.end(), Complex(0, 0));
                std::fill(h_sum_data.begin(), h_sum_data.end(), Complex(0, 0));
                std::fill(h_sum_pilot.begin(), h_sum_pilot.end(), Complex(0, 0));
                guard_noise_power_sum = 0.0;
                guard_noise_bin_count = 0;

                for (size_t sym = 0; sym < num_symbols; ++sym) {
                    SampleSpan sym_span(ptr2, symbol_samples);
                    const auto& baseband = toBaseband(sym_span);
                    const auto& freq = extractSymbol(baseband, 0);

                    accumulate_guard_noise(freq);

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
                            Complex h_ls = rx / tx;
                            h_sum_pilot[i] += h_ls;
                            h_last_pilot[i] = h_ls;
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

    // Store last training symbol's channel estimate for pilot carriers
    // Must use LAST symbol (same as data carriers) so pilot and data H are phase-consistent.
    // Using the average causes phase mismatch when there's any residual CFO, because
    // data carriers use last-symbol H. This mismatch breaks complex interpolation.
    if (valid_symbols > 0) {
        for (size_t i = 0; i < pilot_carrier_indices.size(); ++i) {
            int idx = pilot_carrier_indices[i];
            channel_estimate[idx] = h_last_pilot[i];
        }
    }

    // Estimate per-carrier phase slope from LTS (timing offset effect).
    // On AWGN, H[k] = A × exp(-j × 2π × k × Δn/N_fft), giving a linear phase slope.
    // This slope is ~19°/carrier for a typical ~54-sample timing offset.
    // Knowing this slope allows de-sloping before pilot interpolation (removing the
    // timing contribution so that only channel-phase variation remains, which is
    // smooth enough for 10-carrier pilot spacing).
    {
        int neg_limit = config.num_carriers / 2;
        Complex slope_sum(0, 0);
        int slope_count = 0;
        // Use ALL carrier H (data + pilot) for a robust slope estimate
        for (size_t i = 0; i + 1 < all_carrier_fft_indices.size(); ++i) {
            int idx0 = all_carrier_fft_indices[i];
            int idx1 = all_carrier_fft_indices[i + 1];
            Complex h0 = channel_estimate[idx0];
            Complex h1 = channel_estimate[idx1];
            if (std::abs(h0) > 0.01f && std::abs(h1) > 0.01f) {
                Complex diff = h1 * std::conj(h0);
                float mag = std::abs(diff);
                if (mag > 1e-6f) {
                    slope_sum += diff / mag;  // Normalized to unit magnitude
                    slope_count++;
                }
            }
        }
        if (slope_count > 0) {
            lts_phase_slope = std::arg(slope_sum / static_cast<float>(slope_count));
            timing_offset_samples = -lts_phase_slope * config.fft_size / (2.0f * M_PI);
            LOG_DEMOD(INFO, "LTS phase slope: %.2f°/carrier (timing offset ~%.1f samples)",
                      lts_phase_slope * 180.0f / M_PI,
                      timing_offset_samples);
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
    last_lts_channel_magnitude = h_mag_avg;
    last_lts_signal_power = h_mag_avg * h_mag_avg;

    const float guard_noise_var = guard_noise_bin_count > 0
        ? static_cast<float>(guard_noise_power_sum /
                             static_cast<double>(guard_noise_bin_count))
        : 0.0f;
    const bool guard_noise_valid =
        std::isfinite(guard_noise_var) && guard_noise_var > 0.0f;

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
            last_lts_signal_power = lts_signal_power;

            // Clamp SNR estimate to reasonable range (5 dB to 40 dB)
            float lts_snr = lts_signal_power / std::max(lts_noise_var, 1e-10f);
            lts_snr = std::max(3.16f, std::min(10000.0f, lts_snr));

            noise_variance = lts_noise_var;
            estimated_snr_linear = lts_snr;
            const float meter_noise_var =
                guard_noise_valid ? guard_noise_var : lts_noise_var;
            const float meter_noise_reference_scale =
                guard_noise_valid ? 1.0f : 2.0f;

            updateLastSNREstimate(lts_signal_power, meter_noise_var,
                                  static_cast<size_t>(count), 1.0f,
                                  false, true, meter_noise_reference_scale);

            LOG_DEMOD(INFO, "LTS SNR estimate: internal=%.1f dB in_band=%.1f dB "
                      "(measured noise_var=%.6f, meter_noise=%.6f source=%s, signal=%.4f)",
                      10.0f * std::log10(estimated_snr_linear),
                      last_snr_db_estimate_valid ? last_snr_db_estimate : 0.0f,
                      noise_variance, meter_noise_var,
                      guard_noise_valid ? "guard" : "time",
                      lts_signal_power);
        }
    } else if (h_mag_avg > 1e-6f) {
        // Fallback: only 1 training symbol, use default assumption
        float signal_power = h_mag_avg * h_mag_avg;
        last_lts_signal_power = signal_power;
        noise_variance = signal_power / DEFAULT_SNR_LINEAR;
        estimated_snr_linear = DEFAULT_SNR_LINEAR;
        updateLastSNREstimate(signal_power, noise_variance,
                              std::max<size_t>(1, data_carrier_indices.size()),
                              1.0f);
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
        LOG_DEMOD(INFO, "LTS fading index: %.3f in_band_snr=%.1f dB "
                  "(threshold: LLR>0.15, two-pass>0.30)",
                  last_fading_index,
                  last_snr_db_estimate_valid ? last_snr_db_estimate : 0.0f);
    }

    // Mark that we have a valid channel estimate (for smoothing factor selection)
    snr_symbol_count = num_symbols;
}


} // namespace ultra
