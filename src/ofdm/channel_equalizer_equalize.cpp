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

namespace {

constexpr float kQam16MinPosteriorOdds = 2.1972246f;   // ln(9): >= 9:1 bit odds.
constexpr float kQam16FullPosteriorOdds = 4.5951199f;  // ln(99): near-certain DD.
constexpr float kQam16DecisionChiSq95 = 2.9957323f;    // 95% radius for 2-D Gaussian EVM.

float clamp01(float x) {
    return std::max(0.0f, std::min(1.0f, x));
}

float qam16MinAbsLLRNoClip(Complex sym, float noise_var) {
    const float scale = 2.0f / std::max(noise_var, MIN_CARRIER_NOISE_VAR);
    const float i_abs = std::abs(sym.real());
    const float q_abs = std::abs(sym.imag());
    const float llr_i_sign = std::abs(scale * sym.real());
    const float llr_i_ring = std::abs(scale * (i_abs - QAM16_THRESHOLD));
    const float llr_q_sign = std::abs(scale * sym.imag());
    const float llr_q_ring = std::abs(scale * (q_abs - QAM16_THRESHOLD));
    return std::min(std::min(llr_i_sign, llr_i_ring),
                    std::min(llr_q_sign, llr_q_ring));
}

} // namespace

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
                if (x < -QAM16_THRESHOLD) return -0.9487f;
                if (x < 0.0f) return -0.3162f;
                if (x < QAM16_THRESHOLD) return 0.3162f;
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

    // QAM16 decision-directed channel observations. The noise reference is the
    // post-equalizer carrier noise variance used by the QAM16 LLRs, inflated by
    // the same CE margin as demapping. A carrier is accepted only when:
    // - its EVM is inside the 95% 2-D Gaussian noise radius,
    // - it remains inside half the QAM16 decision-cell spacing, and
    // - every bit has at least 9:1 posterior odds.
    //
    // The actual H update is consumed in updateChannelEstimate() after the next
    // symbol's pilots have re-anchored the channel, which matches the receiver
    // loop order: pilot update -> equalize -> demap/decision.
    if (mod == Modulation::QAM16 && data_carrier_indices.size() == equalized.size()) {
        dd_qam16_channel_observations_.assign(data_carrier_indices.size(), Complex(0, 0));
        dd_qam16_measurement_var_.assign(data_carrier_indices.size(), 0.0f);
        dd_qam16_reliability_.assign(data_carrier_indices.size(), 0.0f);

        float norm_evm_sum = 0.0f;
        size_t norm_evm_count = 0;
        size_t reliable_count = 0;
        constexpr float kDecisionCellGuard2 =
            0.25f * QAM16_THRESHOLD * QAM16_THRESHOLD;

        for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
            if (i < carrier_erasure_flags_.size() && carrier_erasure_flags_[i] != 0) {
                continue;
            }

            const int idx = data_carrier_indices[i];
            const Complex decision = hardDecision(equalized[i], mod);
            const float decision_power = std::norm(decision);
            if (decision_power <= 1.0e-6f) {
                continue;
            }

            const float effective_noise =
                std::max(carrier_noise_var[i] * CE_MARGIN_QAM16, MIN_CARRIER_NOISE_VAR);
            const float evm2 = std::norm(equalized[i] - decision);
            const float norm_evm = evm2 / effective_noise;
            norm_evm_sum += norm_evm;
            ++norm_evm_count;

            const float min_abs_llr = qam16MinAbsLLRNoClip(equalized[i], effective_noise);
            const bool inside_noise_model = norm_evm <= kQam16DecisionChiSq95;
            const bool inside_decision_cell = evm2 <= kDecisionCellGuard2;
            const bool enough_posterior_margin = min_abs_llr >= kQam16MinPosteriorOdds;
            if (!inside_noise_model || !inside_decision_cell || !enough_posterior_margin) {
                continue;
            }

            const float evm_reliability = clamp01(1.0f - norm_evm / kQam16DecisionChiSq95);
            const float llr_reliability = clamp01(
                (min_abs_llr - kQam16MinPosteriorOdds) /
                (kQam16FullPosteriorOdds - kQam16MinPosteriorOdds));
            const float reliability = std::min(evm_reliability, llr_reliability);
            if (reliability <= 0.0f) {
                continue;
            }

            dd_qam16_channel_observations_[i] = freq_domain[idx] / decision;
            dd_qam16_measurement_var_[i] =
                std::max(noise_variance / decision_power, MIN_CARRIER_NOISE_VAR);
            dd_qam16_reliability_[i] = reliability;
            ++reliable_count;
        }

        const float mean_norm_evm = (norm_evm_count > 0)
            ? norm_evm_sum / static_cast<float>(norm_evm_count)
            : kQam16DecisionChiSq95 + 1.0f;
        const size_t min_reliable_carriers = std::max(
            pilot_carrier_indices.size(),
            static_cast<size_t>(1));

        // If the whole symbol is outside the documented noise model and the
        // number of reliable decisions is below the pilot anchor density, freeze
        // DD for this symbol and let the next update use pilot interpolation.
        if (mean_norm_evm > kQam16DecisionChiSq95 &&
            reliable_count < min_reliable_carriers) {
            std::fill(dd_qam16_reliability_.begin(), dd_qam16_reliability_.end(), 0.0f);
        }
    } else if (mod != Modulation::QAM16) {
        dd_qam16_channel_observations_.clear();
        dd_qam16_measurement_var_.clear();
        dd_qam16_reliability_.clear();
        dd_qam16_channel_var_.clear();
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
