// OFDM equalization helpers
// Part of OFDMDemodulator::Impl

#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "demodulator_impl.hpp"
#include "demodulator_constants.hpp"
#include "ultra/logging.hpp"

namespace ultra {

using namespace demod_constants;

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

const std::vector<Complex>& OFDMDemodulator::Impl::equalize(const std::vector<Complex>& freq_domain, Modulation mod) {
    auto& equalized = equalized_scratch;
    equalized.resize(data_carrier_indices.size());
    carrier_noise_var.resize(data_carrier_indices.size());
    carrier_erasure_flags_.assign(data_carrier_indices.size(), 0);

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
        // Noise variance for MMSE equalization and LLR computation
        // Uses global average from LTS (per-carrier estimates are too noisy with only 2 samples)
        float scaled_noise_var = noise_variance;
        if (scaled_noise_var < 1e-6f) {
            scaled_noise_var = avg_h_power / DEFAULT_SNR_LINEAR;
        }
        scaled_noise_var = std::max(scaled_noise_var, MIN_CARRIER_NOISE_VAR);

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

            // RX-local hard erasure: below the configured per-carrier gamma
            // floor, the demapper must contribute no evidence to LDPC.
            if (rx_carrier_erasure_enabled_ &&
                h_power < RX_ERASURE_GAMMA_FLOOR_LINEAR * scaled_noise_var) {
                carrier_erasure_flags_[i] = 1;
                carrier_noise_var[i] = MAX_CARRIER_NOISE_VAR;
            }

            carrier_noise_var[i] = std::max(MIN_CARRIER_NOISE_VAR, std::min(MAX_CARRIER_NOISE_VAR, carrier_noise_var[i]));
        }
        eq_log_count++;
        return equalized;
    }

    bool use_adaptive = config.adaptive_eq_enabled;
    const float reliability_noise_var = std::max(noise_variance, MIN_CARRIER_NOISE_VAR);

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
                // MMSE post-equalization noise variance: σ²/(|H|²+σ²)
                carrier_noise_var[i] = noise_variance / mmse_denom;
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
                // MMSE post-equalization noise variance: σ²/(|H|²+σ²)
                carrier_noise_var[i] = noise_variance / mmse_denom;
                carrier_noise_var[i] = std::max(MIN_CARRIER_NOISE_VAR, std::min(MAX_CARRIER_NOISE_VAR, carrier_noise_var[i]));
            }
        }

        float h_power = std::norm(channel_estimate[idx]);
        if (rx_carrier_erasure_enabled_ &&
            h_power < RX_ERASURE_GAMMA_FLOOR_LINEAR * reliability_noise_var) {
            carrier_erasure_flags_[i] = 1;
            carrier_noise_var[i] = MAX_CARRIER_NOISE_VAR;
        }
    }

    // Decision-directed per-carrier phase tracking for coherent modes.
    // Store the current symbol's phase correction for next symbol's updateChannelEstimate()
    // to apply AFTER interpolation. Each symbol gets a fresh estimate (no accumulation,
    // which would diverge due to positive feedback from the correction-measure loop).
    if ((mod == Modulation::QPSK || mod == Modulation::BPSK) && snr_symbol_count >= 2) {
        dd_phase_corrections.resize(data_carrier_indices.size(), 0.0f);

        for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
            float eq_mag = std::abs(equalized[i]);
            if (eq_mag < 0.3f) {
                dd_phase_corrections[i] = 0.0f;
                continue;
            }

            Complex decision = hardDecision(equalized[i], mod);
            Complex error_ratio = equalized[i] * std::conj(decision);
            float phase_err = std::arg(error_ratio);

            // Only store correction if hard decision is likely correct
            if (std::abs(phase_err) < 0.61f) {
                dd_phase_corrections[i] = -phase_err;
            } else {
                dd_phase_corrections[i] = 0.0f;
            }
        }
    }

    return equalized;
}

const std::vector<Complex>& OFDMDemodulator::Impl::equalize(const std::vector<Complex>& freq_domain) {
    const auto& result = equalize(freq_domain, config.modulation);

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
