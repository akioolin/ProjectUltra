// OFDM Demodulator - Main state machine
// Sync detection: ofdm_sync.cpp
// Channel estimation/equalization: channel_equalizer.cpp
// Soft demapping: soft_demap.hpp

#define _USE_MATH_DEFINES
#include <cmath>
#include "ultra/ofdm.hpp"
#include "ultra/dsp.hpp"
#include "ultra/logging.hpp"
#include "ultra/timing_profiler.hpp"
#include "demodulator_impl.hpp"
#include "demodulator_constants.hpp"
#include "soft_demap.hpp"
#include <algorithm>
#include <numeric>
#include <random>

namespace ultra {

using namespace demod_constants;

namespace {

size_t bitsPerCarrier(Modulation mod) {
    switch (mod) {
        case Modulation::DBPSK:
        case Modulation::BPSK:
            return 1;
        case Modulation::DQPSK:
        case Modulation::QPSK:
            return 2;
        case Modulation::D8PSK:
            return 3;
        case Modulation::QAM16:
            return 4;
        case Modulation::QAM32:
            return 5;
        case Modulation::QAM64:
            return 6;
        case Modulation::QAM256:
            return 8;
        default:
            return 2;
    }
}

void appendErasureLLRs(std::vector<float>& soft_bits, Modulation mod) {
    soft_bits.insert(soft_bits.end(), bitsPerCarrier(mod), 0.0f);
}

bool isDifferentialModulation(Modulation mod) {
    return mod == Modulation::DBPSK ||
           mod == Modulation::DQPSK ||
           mod == Modulation::D8PSK;
}

} // namespace

// =============================================================================
// IMPL CONSTRUCTOR AND INITIALIZATION
// =============================================================================

OFDMDemodulator::Impl::Impl(const ModemConfig& cfg)
    : config(cfg)
    , fft(cfg.fft_size)
    , mixer(cfg.center_freq, cfg.sample_rate)
    , sync_threshold(cfg.sync_threshold)
{
    symbol_samples = cfg.getSymbolDuration();
    channel_estimate.resize(cfg.fft_size, Complex(1, 0));

    // Initialize adaptive equalizer state
    lms_weights.resize(cfg.fft_size, Complex(1, 0));
    last_decisions.resize(cfg.fft_size, Complex(0, 0));
    rls_P.resize(cfg.fft_size, 1.0f);

    setupCarriers();
    generateSequences();
    buildInterpTable();
}

void OFDMDemodulator::Impl::setupCarriers() {
    // Must match modulator exactly
    int neg_limit = config.num_carriers / 2;
    int pos_limit = (config.num_carriers + 1) / 2;

    // Build all-carrier ordered list AND separate pilot/data lists
    all_carrier_fft_indices.clear();
    is_pilot_logical.clear();

    int pilot_count = 0;
    for (int i = -neg_limit; i <= pos_limit; ++i) {
        if (i == 0) continue;

        int fft_idx = (i + config.fft_size) % config.fft_size;
        bool is_pilot = config.use_pilots && (pilot_count % config.pilot_spacing == 0);

        all_carrier_fft_indices.push_back(fft_idx);
        is_pilot_logical.push_back(is_pilot);

        if (!config.use_pilots) {
            data_carrier_indices.push_back(fft_idx);
        } else {
            if (is_pilot) {
                pilot_carrier_indices.push_back(fft_idx);
            } else {
                data_carrier_indices.push_back(fft_idx);
            }
        }
        ++pilot_count;
    }

    }

void OFDMDemodulator::Impl::generateSequences() {
    // Zadoff-Chu sequence for sync
    size_t N = config.num_carriers;
    size_t u = 1;

    sync_sequence.resize(N);
    for (size_t n = 0; n < N; ++n) {
        float phase = -M_PI * u * n * (n + 1) / N;
        sync_sequence[n] = Complex(std::cos(phase), std::sin(phase));
    }

    // Pilot sequence (must match modulator)
    pilot_sequence.resize(pilot_carrier_indices.size());
    std::mt19937 rng(PILOT_RNG_SEED);
    for (size_t i = 0; i < pilot_sequence.size(); ++i) {
        pilot_sequence[i] = (rng() & 1) ? Complex(1, 0) : Complex(-1, 0);
    }

    LOG_DEMOD(DEBUG, "Demod pilot config: %zu pilots, %zu data carriers",
              pilot_carrier_indices.size(), data_carrier_indices.size());
    if (pilot_carrier_indices.size() >= 3) {
        LOG_DEMOD(DEBUG, "Demod pilot indices[0-2]: %d, %d, %d",
                  pilot_carrier_indices[0], pilot_carrier_indices[1], pilot_carrier_indices[2]);
    }
    if (pilot_sequence.size() >= 3) {
        LOG_DEMOD(DEBUG, "Demod pilot seq[0-2]: (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f)",
                  pilot_sequence[0].real(), pilot_sequence[0].imag(),
                  pilot_sequence[1].real(), pilot_sequence[1].imag(),
                  pilot_sequence[2].real(), pilot_sequence[2].imag());
    }

    // Generate LTS time-domain reference for fine timing
    std::vector<Complex> lts_freq(config.fft_size, Complex(0, 0));

    for (size_t i = 0; i < data_carrier_indices.size(); ++i) {
        lts_freq[data_carrier_indices[i]] = sync_sequence[i % sync_sequence.size()];
    }
    for (size_t i = 0; i < pilot_carrier_indices.size(); ++i) {
        lts_freq[pilot_carrier_indices[i]] = pilot_sequence[i];
    }

    std::vector<Complex> lts_complex;
    fft.inverse(lts_freq, lts_complex);

    size_t cp_len = config.getCyclicPrefix();
    std::vector<Complex> lts_baseband(cp_len + config.fft_size);
    for (size_t i = 0; i < cp_len; ++i) {
        lts_baseband[i] = lts_complex[config.fft_size - cp_len + i];
    }
    for (size_t i = 0; i < config.fft_size; ++i) {
        lts_baseband[cp_len + i] = lts_complex[i];
    }

    // Convert to passband templates
    lts_passband_I.resize(lts_baseband.size());
    lts_passband_Q.resize(lts_baseband.size());

    NCO template_nco(config.center_freq, config.sample_rate);
    for (size_t i = 0; i < lts_baseband.size(); ++i) {
        Complex osc = template_nco.next();
        Complex mixed = lts_baseband[i] * osc;
        lts_passband_I[i] = mixed.real();
        lts_passband_Q[i] = mixed.imag();
    }

    LOG_DEMOD(DEBUG, "LTS passband templates generated: %zu samples", lts_passband_I.size());
}

void OFDMDemodulator::Impl::buildInterpTable() {
    // Pre-compute interpolation weights for each data carrier
    struct CarrierInfo {
        int fft_idx;
        bool is_pilot;
    };
    std::vector<CarrierInfo> carriers;
    int neg_limit = config.num_carriers / 2;
    int pos_limit = (config.num_carriers + 1) / 2;
    int pilot_count = 0;

    for (int i = -neg_limit; i <= pos_limit; ++i) {
        if (i == 0) continue;
        int fft_idx = (i + config.fft_size) % config.fft_size;
        bool is_pilot = (pilot_count % config.pilot_spacing == 0);
        carriers.push_back({fft_idx, is_pilot});
        ++pilot_count;
    }

    interp_table.clear();
    interp_table.reserve(data_carrier_indices.size());

    for (size_t ci = 0; ci < carriers.size(); ++ci) {
        if (carriers[ci].is_pilot) continue;

        InterpInfo info;
        info.fft_idx = carriers[ci].fft_idx;
        info.lower_pilot = -1;
        info.upper_pilot = -1;
        info.alpha = 0.5f;

        int lower_ci = -1;
        for (int j = (int)ci - 1; j >= 0; --j) {
            if (carriers[j].is_pilot) {
                info.lower_pilot = carriers[j].fft_idx;
                lower_ci = j;
                break;
            }
        }

        int upper_ci = -1;
        for (size_t j = ci + 1; j < carriers.size(); ++j) {
            if (carriers[j].is_pilot) {
                info.upper_pilot = carriers[j].fft_idx;
                upper_ci = (int)j;
                break;
            }
        }

        if (lower_ci >= 0 && upper_ci >= 0) {
            float total_dist = (float)(upper_ci - lower_ci);
            info.alpha = (total_dist > 0) ? (float)((int)ci - lower_ci) / total_dist : 0.5f;
        }

        interp_table.push_back(info);
    }
}

// =============================================================================
// SYMBOL DEMODULATION
// =============================================================================

void OFDMDemodulator::Impl::demodulateSymbol(const std::vector<Complex>& equalized, Modulation mod) {
    // Constellation symbols collected during demodulation (differential decoded for DPSK modes)
    std::vector<Complex> constellation_update;

    // Phase inversion detection (disabled - raw data can have extreme bias)
    if (soft_bits.empty() && !equalized.empty()) {
        int pos_count = 0, neg_count = 0;
        for (const auto& s : equalized) {
            if (s.real() > 0) pos_count++;
            else neg_count++;
        }
        LOG_DEMOD(DEBUG, "First symbol stats: %zu carriers, %d positive, %d negative, first 3: (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f)",
                equalized.size(), pos_count, neg_count,
                equalized[0].real(), equalized[0].imag(),
                equalized.size() > 1 ? equalized[1].real() : 0.0f,
                equalized.size() > 1 ? equalized[1].imag() : 0.0f,
                equalized.size() > 2 ? equalized[2].real() : 0.0f,
                equalized.size() > 2 ? equalized[2].imag() : 0.0f);

        llr_sign_flip = false;
        float neg_ratio = float(neg_count) / equalized.size();
        LOG_DEMOD(DEBUG, "Symbol polarity: %.0f%% negative, %.0f%% positive (no flip)",
                  neg_ratio * 100, (1.0f - neg_ratio) * 100);
    }

    // Get channel estimation error margin
    float ce_error_margin = soft_demap::getCEErrorMargin(mod);

    // PER-CARRIER ADAPTIVE LLR SCALING (replaces old global fading_scale):
    // Track |equalized| EMA per carrier across symbols within a frame.
    // Carriers with high magnitude variance are fading mid-frame → inflate their noise.
    // Stable carriers keep full LLR confidence. On AWGN, all carriers stable → no scaling.
    constexpr float MAG_EMA_ALPHA = 0.3f;
    if (carrier_eq_mag_ema_.size() != equalized.size()) {
        // First symbol in frame: initialize EMA to current magnitudes, zero variance
        carrier_eq_mag_ema_.resize(equalized.size());
        carrier_eq_mag_var_.resize(equalized.size(), 0.0f);
        for (size_t i = 0; i < equalized.size(); ++i)
            carrier_eq_mag_ema_[i] = std::abs(equalized[i]);
    } else {
        for (size_t i = 0; i < equalized.size(); ++i) {
            float mag = std::abs(equalized[i]);
            float delta = mag - carrier_eq_mag_ema_[i];
            carrier_eq_mag_ema_[i] += MAG_EMA_ALPHA * delta;
            carrier_eq_mag_var_[i] += MAG_EMA_ALPHA * (delta * delta - carrier_eq_mag_var_[i]);
        }
    }

    float llr_sign = llr_sign_flip ? -1.0f : 1.0f;

    // Initialize DQPSK/D8PSK reference
    // TX initializes dbpsk_prev_symbols to (1,0) in generateTrainingSymbols(), so first data
    // symbol is encoded relative to (1,0), NOT relative to the training symbol (sync_sequence).
    //
    // MMSE equalization: eq = rx × conj(H) / (|H|² + σ²) = TX × |H|² / (|H|² + σ²)
    // This produces a REAL-valued scaling (no phase rotation when H_est ≈ H_true).
    // So the equalized first data symbol ≈ TX × real_scale = dqpsk_phase × real_scale.
    //
    // The correct reference is (1,0) because:
    //   TX first data: (1,0) × DQPSK_phase
    //   RX equalized:  DQPSK_phase × real_scale  (MMSE removes channel phase)
    //   RX reference:  (1,0)
    //   diff = eq × conj(ref) = DQPSK_phase × real_scale  ✓
    //
    // NOTE: The old code used conj(H)/|H| as reference, which was derived assuming ZF
    // equalization (eq = rx/H = TX × e^{-jφ}). With MMSE, equalization already removes
    // the channel phase, so adding conj(H)/|H| re-introduces arg(H) into the differential,
    // causing the constellation to show a circle instead of DQPSK clusters.
    const bool is_differential = isDifferentialModulation(mod);
    if (is_differential && dbpsk_prev_equalized.empty()) {
        dbpsk_prev_equalized.assign(equalized.size(), Complex(1, 0));
        differential_prev_erased_.assign(equalized.size(), 0);
        dqpsk_skip_first_symbol = true;  // First diff uses synthetic ref → skip constellation
        LOG_DEMOD(DEBUG, "DPSK: Reference initialized to (1,0) for %zu carriers", equalized.size());
    } else if (is_differential && differential_prev_erased_.size() != equalized.size()) {
        differential_prev_erased_.assign(equalized.size(), 0);
    }

    // Two-pass D8PSK decoding: use embedded DQPSK grid to estimate common phase error
    // and correct it before decoding. Only activates on fading channels.
    // NOTE: Use last_fading_index (from pilot variance), NOT computeFadingIndex()
    // because channel_estimate is reset to unity after sync.
    if (mod == Modulation::D8PSK && d8psk_two_pass_enabled_) {
        float fading_index = last_fading_index;
        if (fading_index > TWO_PASS_FADING_THRESHOLD) {
            LOG_DEMOD(DEBUG, "D8PSK two-pass: fading=%.3f > %.3f, applying correction",
                      fading_index, TWO_PASS_FADING_THRESHOLD);
            demodulateD8PSKTwoPass(equalized, noise_variance);
            snr_symbol_count++;
            dqpsk_skip_first_symbol = false;
            return;  // Two-pass handled everything (constellation updated inside)
        }
    }

    // Two-pass DQPSK decoding: DISABLED.
    // Testing shows it compresses LLR dynamic range too much at SNR=20,
    // reducing scale from ~20 to 5-13. The normal path with per-carrier
    // noise_var + fading scaling + perturbation retry works better.
    // if (mod == Modulation::DQPSK && dqpsk_two_pass_enabled_) {
    //     float fading_index = last_fading_index;
    //     if (fading_index > TWO_PASS_FADING_THRESHOLD) {
    //         demodulateDQPSKTwoPass(equalized, noise_variance);
    //         snr_symbol_count++;
    //         dqpsk_skip_first_symbol = false;
    //         return;
    //     }
    // }

    // Debug: log modulation value once per symbol
    static int mod_log_once = 0;
    if (mod_log_once++ < 5) {
        LOG_DEMOD(DEBUG, "demodulateSymbol: mod=%d (DQPSK=%d), carriers=%zu, snr_count=%d",
                  static_cast<int>(mod), static_cast<int>(Modulation::DQPSK),
                  equalized.size(), snr_symbol_count);
    }

    std::vector<Complex> differential_symbols;
    std::vector<float> differential_signal_power;
    if (mod == Modulation::DQPSK || mod == Modulation::D8PSK) {
        differential_symbols.assign(equalized.size(), Complex(0, 0));
        differential_signal_power.assign(equalized.size(), 0.0f);
    }

    for (size_t i = 0; i < equalized.size(); ++i) {
        const auto& sym = equalized[i];
        float base_nv = (i < carrier_noise_var.size()) ? carrier_noise_var[i] : noise_variance;
        float nv = base_nv * ce_error_margin;
        const bool carrier_erased =
            i < carrier_erasure_flags_.size() && carrier_erasure_flags_[i] != 0;
        const bool prev_erased =
            is_differential &&
            i < differential_prev_erased_.size() &&
            differential_prev_erased_[i] != 0;
        const bool erase_llrs = carrier_erased || prev_erased;

        // Per-carrier adaptive: inflate noise for carriers with unstable |eq|
        if (carrier_eq_mag_var_.size() == equalized.size()) {
            float mean_sq = carrier_eq_mag_ema_[i] * carrier_eq_mag_ema_[i] + 1e-6f;
            float norm_var = carrier_eq_mag_var_[i] / mean_sq;
            nv *= (1.0f + CARRIER_ADAPTIVE_K * norm_var);
        }

        if (erase_llrs) {
            appendErasureLLRs(soft_bits, mod);
            if (is_differential) {
                dbpsk_prev_equalized[i] = sym;
                differential_prev_erased_[i] = carrier_erased ? 1 : 0;
            }
            continue;
        }

        switch (mod) {
            case Modulation::DBPSK: {
                if (dbpsk_prev_equalized.empty()) {
                    dbpsk_prev_equalized.assign(equalized.size(), Complex(1, 0));
                    differential_prev_erased_.assign(equalized.size(), 0);
                    dqpsk_skip_first_symbol = true;
                }
                Complex prev_sym = dbpsk_prev_equalized[i];
                float llr = soft_demap::demapDBPSK(sym, prev_sym, nv);
                soft_bits.push_back(llr);
                if (!dqpsk_skip_first_symbol) {
                    Complex diff = sym * std::conj(prev_sym);
                    constellation_update.push_back(diff);
                }
                dbpsk_prev_equalized[i] = sym;
                differential_prev_erased_[i] = 0;
                break;
            }
            case Modulation::DQPSK: {
                Complex prev_sym = dbpsk_prev_equalized[i];
                Complex diff = sym * std::conj(prev_sym);
                differential_symbols[i] = diff;
                differential_signal_power[i] = std::abs(sym) * std::abs(prev_sym);
                auto llrs = soft_demap::demapDQPSK(sym, prev_sym, nv);
                soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());

                if (!dqpsk_skip_first_symbol) {
                    constellation_update.push_back(diff);
                }
                dbpsk_prev_equalized[i] = sym;
                differential_prev_erased_[i] = 0;
                break;
            }
            case Modulation::D8PSK: {
                Complex prev_sym = dbpsk_prev_equalized[i];
                Complex diff = sym * std::conj(prev_sym);
                differential_symbols[i] = diff;
                differential_signal_power[i] = std::abs(sym) * std::abs(prev_sym);
                auto llrs = soft_demap::demapD8PSK(sym, prev_sym, nv);
                soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());
                if (!dqpsk_skip_first_symbol) {
                    constellation_update.push_back(diff);
                }
                dbpsk_prev_equalized[i] = sym;
                differential_prev_erased_[i] = 0;
                break;
            }
            case Modulation::BPSK:
                soft_bits.push_back(soft_demap::demapBPSK(sym, nv) * llr_sign);
                constellation_update.push_back(sym);
                break;
            case Modulation::QPSK: {
                auto llrs = soft_demap::demapQPSK(sym, nv);
                for (auto& llr : llrs) llr *= llr_sign;
                soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());
                constellation_update.push_back(sym);
                break;
            }
            case Modulation::QAM16: {
                auto llrs = soft_demap::demapQAM16(sym, nv);
                for (auto& llr : llrs) llr *= llr_sign;
                soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());
                break;
            }
            case Modulation::QAM32: {
                auto llrs = soft_demap::demapQAM32(sym, nv);
                for (auto& llr : llrs) llr *= llr_sign;
                soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());
                break;
            }
            case Modulation::QAM64: {
                auto llrs = soft_demap::demapQAM64(sym, nv);
                for (auto& llr : llrs) llr *= llr_sign;
                soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());
                break;
            }
            case Modulation::QAM256: {
                auto llrs = soft_demap::demapQAM256(sym, nv);
                for (auto& llr : llrs) llr *= llr_sign;
                soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());
                break;
            }
            default: {
                auto llrs = soft_demap::demapQPSK(sym, nv);
                for (auto& llr : llrs) llr *= llr_sign;
                soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());
            }
        }
    }

    // Decision-directed tracking for differential modes without pilots
    // Two-stage tracking:
    // 1. Per-carrier channel tracking: update channel_estimate[] for frequency-selective fading
    // 2. Common phase tracking: update pilot_phase_correction for overall drift
    if ((mod == Modulation::DQPSK || mod == Modulation::D8PSK) &&
        differential_symbols.size() == equalized.size()) {
        // Skip first symbol to let differential decoding establish reference
        if (snr_symbol_count >= 1) {
            Complex phase_error_sum(0, 0);
            int valid_count = 0;

            // Per-carrier tracking: update channel_estimate based on decoded symbol
            // This handles frequency-selective fading where each carrier drifts differently
            float dd_alpha = (snr_symbol_count < 3) ? 0.3f : 0.15f;  // Faster initial, then slower

            for (size_t i = 0; i < equalized.size(); ++i) {
                int idx = data_carrier_indices[i];
                float signal_power = differential_signal_power[i];

                // Only track strong carriers
                if (signal_power > 0.1f) {
                    Complex diff = differential_symbols[i];
                    float phase = std::atan2(diff.imag(), diff.real());

                    // Map to nearest constellation point
                    float expected_phase;
                    if (mod == Modulation::DQPSK) {
                        int quadrant = (int)std::round(phase * 2.0f / M_PI);
                        quadrant = ((quadrant % 4) + 4) % 4;
                        expected_phase = quadrant * M_PI / 2.0f;
                    } else {
                        // D8PSK constellation has 22.5° offset: 22.5°, 67.5°, 112.5°, etc.
                        // Must account for this offset when finding nearest constellation point
                        float phase_minus_offset = phase - M_PI / 8.0f;  // Subtract 22.5°
                        int octant = (int)std::round(phase_minus_offset * 4.0f / M_PI);
                        octant = ((octant % 8) + 8) % 8;
                        expected_phase = octant * M_PI / 4.0f + M_PI / 8.0f;  // Add offset back
                    }

                    // Phase error for this carrier
                    float phase_error = phase - expected_phase;
                    while (phase_error > M_PI) phase_error -= 2 * M_PI;
                    while (phase_error < -M_PI) phase_error += 2 * M_PI;

                    // Per-carrier channel update: rotate channel_estimate to correct the error
                    // Only update if error is small (likely correct decoding)
                    float max_error_rad = (mod == Modulation::DQPSK) ? 0.7f : 0.35f;  // ~40° for DQPSK
                    if (std::abs(phase_error) < max_error_rad) {
                        Complex phase_correction = Complex(std::cos(phase_error * dd_alpha),
                                                           std::sin(phase_error * dd_alpha));
                        channel_estimate[idx] *= phase_correction;
                    }

                    // Accumulate for common phase tracking
                    phase_error_sum += signal_power * Complex(std::cos(phase_error), std::sin(phase_error));
                    valid_count++;
                }
            }

            // Common phase tracking: update pilot_phase_correction
            if (valid_count >= 5) {
                float avg_phase_error = std::atan2(phase_error_sum.imag(), phase_error_sum.real());
                Complex correction = Complex(std::cos(-avg_phase_error), std::sin(-avg_phase_error));

                float alpha = (snr_symbol_count < 5) ? 0.5f : 0.2f;
                pilot_phase_correction = pilot_phase_correction *
                    std::pow(std::abs(correction), alpha) *
                    Complex(std::cos(alpha * std::arg(correction)),
                            std::sin(alpha * std::arg(correction)));

                float mag = std::abs(pilot_phase_correction);
                if (mag > 0.01f) pilot_phase_correction /= mag;

                if (snr_symbol_count < 10) {
                    LOG_DEMOD(DEBUG, "DD tracking: avg_err=%.1f°, valid=%d",
                              avg_phase_error * 180.0f / M_PI, valid_count);
                }
            }
        }
    }

    // Store constellation symbols (differential decoded for DPSK, raw equalized for coherent)
    if (!constellation_update.empty()) {
        std::lock_guard<std::mutex> lock(constellation_mutex);
        constellation_symbols.insert(constellation_symbols.end(),
                                     constellation_update.begin(), constellation_update.end());
        if (constellation_symbols.size() > MAX_CONSTELLATION_SYMBOLS) {
            constellation_symbols.erase(constellation_symbols.begin(),
                constellation_symbols.begin() + (constellation_symbols.size() - MAX_CONSTELLATION_SYMBOLS));
        }
    }

    // Clear skip flag so subsequent symbols show in constellation
    dqpsk_skip_first_symbol = false;
}

// =============================================================================
// TWO-PASS D8PSK DECODING
// =============================================================================

float OFDMDemodulator::Impl::computeFadingIndex() const {
    // Coefficient of variation of channel estimate magnitudes
    if (data_carrier_indices.empty()) return 0.0f;

    float sum = 0.0f;
    for (int idx : data_carrier_indices) {
        sum += std::abs(channel_estimate[idx]);
    }
    float mean = sum / data_carrier_indices.size();
    if (mean < 0.001f) return 0.0f;

    float var_sum = 0.0f;
    for (int idx : data_carrier_indices) {
        float diff = std::abs(channel_estimate[idx]) - mean;
        var_sum += diff * diff;
    }
    return std::sqrt(var_sum / data_carrier_indices.size()) / mean;
}

bool OFDMDemodulator::Impl::demodulateD8PSKTwoPass(
    const std::vector<Complex>& equalized,
    float base_noise_variance)
{
    // Two-pass D8PSK: Use embedded DQPSK grid (45°, 135°, 225°, 315°) to estimate
    // common phase drift, then apply correction before D8PSK decoding.
    // DQPSK has 45° margins vs D8PSK's 22.5°, so decisions are more robust.

    float ce_margin = soft_demap::getCEErrorMargin(Modulation::D8PSK);

    // PASS 1: Estimate common phase error using DQPSK decisions
    float sin_sum = 0.0f, cos_sum = 0.0f, weight_sum = 0.0f;

    for (size_t i = 0; i < equalized.size(); ++i) {
        const bool carrier_erased =
            i < carrier_erasure_flags_.size() && carrier_erasure_flags_[i] != 0;
        const bool prev_erased =
            i < differential_prev_erased_.size() && differential_prev_erased_[i] != 0;
        if (carrier_erased || prev_erased) {
            continue;
        }

        Complex prev_sym = dbpsk_prev_equalized[i];
        float signal_power = std::abs(equalized[i]) * std::abs(prev_sym);

        if (signal_power > 0.1f) {
            Complex diff = equalized[i] * std::conj(prev_sym);
            float phase = std::atan2(diff.imag(), diff.real());

            // Find nearest DQPSK point (45°, 135°, 225°, 315°)
            float phase_minus_offset = phase - M_PI / 4.0f;
            int quadrant = (int)std::round(phase_minus_offset * 2.0f / M_PI);
            quadrant = ((quadrant % 4) + 4) % 4;
            float expected = quadrant * M_PI / 2.0f + M_PI / 4.0f;

            float error = phase - expected;
            while (error > M_PI) error -= 2 * M_PI;
            while (error < -M_PI) error += 2 * M_PI;

            // Weighted circular mean
            sin_sum += signal_power * std::sin(error);
            cos_sum += signal_power * std::cos(error);
            weight_sum += signal_power;
        }
    }

    // Compute mean phase error
    float mean_error = (weight_sum > 0.1f) ? std::atan2(sin_sum, cos_sum) : 0.0f;

    // Only apply partial correction (50%) to avoid over-correction
    // Only if error is significant (> 3°) but not too large (< 15°)
    Complex phase_correction(1.0f, 0.0f);
    float correction_factor = 0.5f;  // Apply only half the estimated error
    if (std::abs(mean_error) > 0.05f && std::abs(mean_error) < 0.26f) {
        float corrected_error = mean_error * correction_factor;
        phase_correction = Complex(std::cos(-corrected_error), std::sin(-corrected_error));
        LOG_DEMOD(DEBUG, "D8PSK two-pass: err=%.1f°, applying %.1f°",
                  mean_error * 180.0f / M_PI, corrected_error * 180.0f / M_PI);
    }

    // PASS 2: Apply correction and decode
    std::vector<Complex> constellation_update;
    if (differential_prev_erased_.size() != equalized.size()) {
        differential_prev_erased_.assign(equalized.size(), 0);
    }
    for (size_t i = 0; i < equalized.size(); ++i) {
        const bool carrier_erased =
            i < carrier_erasure_flags_.size() && carrier_erasure_flags_[i] != 0;
        const bool prev_erased = differential_prev_erased_[i] != 0;
        if (carrier_erased || prev_erased) {
            appendErasureLLRs(soft_bits, Modulation::D8PSK);
            dbpsk_prev_equalized[i] = equalized[i] * phase_correction;
            differential_prev_erased_[i] = carrier_erased ? 1 : 0;
            continue;
        }

        Complex prev_sym = dbpsk_prev_equalized[i];
        float nv = (i < carrier_noise_var.size()) ? carrier_noise_var[i] : base_noise_variance;
        nv *= ce_margin;

        // Per-carrier adaptive: inflate noise for carriers with unstable |eq|
        if (carrier_eq_mag_var_.size() == equalized.size()) {
            float mean_sq = carrier_eq_mag_ema_[i] * carrier_eq_mag_ema_[i] + 1e-6f;
            float norm_var = carrier_eq_mag_var_[i] / mean_sq;
            nv *= (1.0f + CARRIER_ADAPTIVE_K * norm_var);
        }

        Complex corrected_sym = equalized[i] * phase_correction;
        auto llrs = soft_demap::demapD8PSK(corrected_sym, prev_sym, nv);
        soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());

        if (!dqpsk_skip_first_symbol) {
            Complex diff = corrected_sym * std::conj(prev_sym);
            constellation_update.push_back(diff);
        }

        // Update reference with corrected symbol
        dbpsk_prev_equalized[i] = corrected_sym;
        differential_prev_erased_[i] = 0;
    }

    // Store differential symbols for constellation display
    {
        std::lock_guard<std::mutex> lock(constellation_mutex);
        constellation_symbols.insert(constellation_symbols.end(),
                                     constellation_update.begin(), constellation_update.end());
        if (constellation_symbols.size() > MAX_CONSTELLATION_SYMBOLS) {
            constellation_symbols.erase(constellation_symbols.begin(),
                constellation_symbols.begin() + (constellation_symbols.size() - MAX_CONSTELLATION_SYMBOLS));
        }
    }

    return true;
}

// =============================================================================
// TWO-PASS DQPSK DECODING
// =============================================================================

void OFDMDemodulator::Impl::demodulateDQPSKTwoPass(
    const std::vector<Complex>& equalized,
    float base_noise_variance)
{
    // Two-pass DQPSK with phase-variance-based LLR scaling.
    //
    // Problem: MMSE carrier_noise_var gives very different values for peak vs faded
    // carriers (5-50× range). Combined with signal_power in the demapper, peak carriers
    // produce LLRs of 10-15 while faded carriers produce 2-5. At high SNR, the peaks
    // dominate and LDPC can't correct wrong bits from faded carriers.
    //
    // Solution: Use the observed phase noise VARIANCE (from pass 1) as a uniform
    // LLR scale for all carriers in this symbol. This naturally adapts to actual
    // channel quality: clean symbols get high scale, faded symbols get low scale.
    // Per-carrier discrimination comes from the differential PHASE (data information),
    // not from amplitude/noise estimates.

    static const float PI = 3.14159265358979f;

    // PASS 1: Estimate per-carrier phase errors using hard decisions
    std::vector<float> valid_errors;
    valid_errors.reserve(equalized.size());

    for (size_t i = 0; i < equalized.size(); ++i) {
        Complex prev_sym = dbpsk_prev_equalized[i];
        float sp = std::abs(equalized[i]) * std::abs(prev_sym);

        if (sp > 0.1f) {
            float err = soft_demap::computeDQPSKPhaseError(equalized[i], prev_sym);
            valid_errors.push_back(err);
        }
    }

    // Compute median for common phase correction (robust to outliers)
    float median_error = 0.0f;
    if (!valid_errors.empty()) {
        std::sort(valid_errors.begin(), valid_errors.end());
        size_t mid = valid_errors.size() / 2;
        if (valid_errors.size() % 2 == 0) {
            median_error = (valid_errors[mid - 1] + valid_errors[mid]) / 2.0f;
        } else {
            median_error = valid_errors[mid];
        }
    }

    // Compute phase noise variance from valid errors
    // This directly measures differential demodulation quality for this symbol
    float phase_var = 0.05f;  // Default: moderate quality
    if (valid_errors.size() > 5) {
        float sum_sq = 0.0f;
        for (float e : valid_errors) {
            float d = e - median_error;
            sum_sq += d * d;
        }
        phase_var = sum_sq / valid_errors.size();
        phase_var = std::max(0.002f, phase_var);  // Floor to prevent infinite scale
    }

    // PASS 2: Compute LLRs using phase-variance-based uniform scaling
    // Scale = 2/sqrt(phase_var), same principle as MC-DPSK two-pass
    // phase_var already captures channel quality — no additional fading factor needed
    // (fading increases phase errors → higher phase_var → lower scale automatically)
    float correction = -median_error;

    float scale = 2.0f / std::sqrt(phase_var);
    scale = std::min(scale, MAX_LLR);  // Cap scale at MAX_LLR

    if (snr_symbol_count < 5) {
        LOG_DEMOD(INFO, "DQPSK two-pass sym=%d: median_err=%.1f°, phase_var=%.4f, scale=%.1f, valid=%zu/%zu",
                  snr_symbol_count, median_error * 180.0f / PI, phase_var, scale,
                  valid_errors.size(), equalized.size());
    }

    Complex phase_corr(std::cos(correction), std::sin(correction));

    std::vector<Complex> constellation_update;
    for (size_t i = 0; i < equalized.size(); ++i) {
        Complex prev_sym = dbpsk_prev_equalized[i];
        float sp = std::abs(equalized[i]) * std::abs(prev_sym);

        std::array<float, 2> llrs;
        if (sp < 0.05f) {
            // Deep fade — erasure (near-zero LLRs)
            llrs = {MIN_LLR_MAG, MIN_LLR_MAG};
        } else {
            // Apply median correction and compute corrected differential phase
            Complex corrected = equalized[i] * phase_corr;
            Complex diff = corrected * std::conj(prev_sym);
            float phase = std::atan2(diff.imag(), diff.real());

            // Uniform LLR scale from phase_var — all carriers weighted equally
            llrs[0] = soft_demap::clipLLR(scale * std::sin(phase + PI / 4.0f));
            llrs[1] = soft_demap::clipLLR(scale * std::cos(2.0f * phase));
        }

        soft_bits.push_back(llrs[0]);
        soft_bits.push_back(llrs[1]);

        // Constellation display
        if (!dqpsk_skip_first_symbol) {
            Complex diff = equalized[i] * phase_corr * std::conj(prev_sym);
            constellation_update.push_back(diff);
        }

        // Update reference with ORIGINAL symbol (not corrected)
        dbpsk_prev_equalized[i] = equalized[i];
    }

    // Store differential symbols for constellation display
    {
        std::lock_guard<std::mutex> lock(constellation_mutex);
        constellation_symbols.insert(constellation_symbols.end(),
                                     constellation_update.begin(), constellation_update.end());
        if (constellation_symbols.size() > MAX_CONSTELLATION_SYMBOLS) {
            constellation_symbols.erase(constellation_symbols.begin(),
                constellation_symbols.begin() + (constellation_symbols.size() - MAX_CONSTELLATION_SYMBOLS));
        }
    }

    // DEBUG: log first few differential phases to diagnose constellation
    if (snr_symbol_count < 3 && !constellation_update.empty()) {
        char buf[256] = "";
        int bp = 0;
        for (size_t i = 0; i < std::min(size_t(8), constellation_update.size()); ++i) {
            float phase_deg = std::atan2(constellation_update[i].imag(), constellation_update[i].real()) * 180.0f / M_PI;
            bp += snprintf(buf + bp, sizeof(buf) - bp, "%.0f° ", phase_deg);
        }
        LOG_DEMOD(DEBUG, "DQPSK two-pass constellation sym=%d: %s(correction=%.1f°)",
                  snr_symbol_count, buf, correction * 180.0f / M_PI);
    }
}

void OFDMDemodulator::Impl::updateQuality() {
    quality.snr_db = 10.0f * std::log10(estimated_snr_linear);
    quality.doppler_hz = 0;
    quality.delay_spread_ms = 0;

    if (quality.snr_db > 15) {
        quality.ber_estimate = 1e-6f;
    } else if (quality.snr_db > 10) {
        quality.ber_estimate = 1e-5f;
    } else if (quality.snr_db > 5) {
        quality.ber_estimate = 1e-3f;
    } else {
        quality.ber_estimate = 1e-1f;
    }
}

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
            auto baseband = impl_->toBaseband(sym_samples);
            auto freq_domain = impl_->extractSymbol(baseband, 0);

            impl_->updateChannelEstimate(freq_domain);

            auto equalized = impl_->equalize(freq_domain);
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
    impl_->rx_buffer.insert(impl_->rx_buffer.end(), ptr, ptr + remaining);

    while (impl_->rx_buffer.size() >= impl_->symbol_samples) {
        timing::ScopedTimer _profile_(timing::globalDecoderProfile().data_symbol_loop);

        SampleSpan sym_samples(impl_->rx_buffer.data(), impl_->symbol_samples);
        auto bb = impl_->toBaseband(sym_samples);
        auto fd = impl_->extractSymbol(bb, 0);

        // Per-symbol pilot tracking: update channel estimate from pilot observations.
        // For differential modes: update only |H| (magnitude) to track fading depth,
        // while keeping phase frozen from LTS. This gives MMSE accurate amplitude
        // scaling without corrupting the differential phase relationship.
        // For coherent modes: full H update (magnitude + phase).
        if (!impl_->pilot_carrier_indices.empty()) {
            impl_->updateChannelEstimate(fd);
        }
        auto eq = impl_->equalize(fd);
        impl_->demodulateSymbol(eq, impl_->config.modulation);

        impl_->rx_buffer.erase(impl_->rx_buffer.begin(),
                               impl_->rx_buffer.begin() + impl_->symbol_samples);
        ++impl_->synced_symbol_count;
        impl_->updateQuality();
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

bool OFDMDemodulator::searchForSync(SampleSpan samples, size_t& out_position, float& out_cfo_hz, float threshold) {
    // Search for Schmidl-Cox sync in samples WITHOUT changing internal state
    // This is used by IWaveform::detectSync() to find preamble position

    if (samples.size() < MIN_SEARCH_SAMPLES) {
        return false;
    }

    // Temporarily store samples for correlation (don't modify rx_buffer)
    std::vector<float> search_buffer(samples.begin(), samples.end());

    // Preamble size constants
    size_t preamble_symbol_len = impl_->config.fft_size + impl_->config.getCyclicPrefix();
    size_t preamble_total_len = preamble_symbol_len * 6;
    size_t correlation_window = preamble_symbol_len * 2;

    if (search_buffer.size() < preamble_total_len + correlation_window) {
        return false;
    }

    // Save original rx_buffer and restore after search
    std::vector<float> saved_buffer = std::move(impl_->rx_buffer);
    impl_->rx_buffer = std::move(search_buffer);

    // Search for preamble
    bool found_sync = false;
    size_t sync_offset = 0;
    size_t refined_lts_offset = 0;
    float sync_cfo = 0.0f;

    size_t search_end = impl_->rx_buffer.size() - preamble_total_len - correlation_window;

    // Use larger step for faster search (64 samples = ~1.3ms at 48kHz)
    // This is a quick search to find candidates, not fine timing
    constexpr size_t QUICK_SEARCH_STEP = 64;

    for (size_t i = 0; i < search_end; i += QUICK_SEARCH_STEP) {
        if (!impl_->hasMinimumEnergy(i, correlation_window)) {
            i += correlation_window / 2 - QUICK_SEARCH_STEP;
            continue;
        }

        float corr = impl_->measureCorrelation(i);

        if (corr > threshold) {
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
                // Verify with LTS
                size_t refined_lts = impl_->refineLTSTiming(peak_pos);
                if (refined_lts != SIZE_MAX) {
                    found_sync = true;
                    sync_offset = peak_pos;
                    refined_lts_offset = refined_lts;
                    sync_cfo = impl_->estimateCoarseCFO(peak_pos);

                    LOG_SYNC(INFO, "searchForSync: found at %zu (LTS=%zu), corr=%.3f, CFO=%.1f Hz",
                             sync_offset, refined_lts_offset, peak_corr, sync_cfo);
                    break;
                }
            }
        }
    }

    if (found_sync) {
        out_cfo_hz = sync_cfo;

        // processPresynced expects samples starting at the FIRST LTS. The LTS
        // template search can peak on either repeated LTS symbol; choose the
        // earlier position only when the preceding two symbols are themselves
        // an LTS pair. Otherwise, subtracting one symbol lands on the final STS
        // and shifts all payload bits by one OFDM symbol.
        out_position = refined_lts_offset;
        if (refined_lts_offset >= preamble_symbol_len) {
            const size_t previous_symbol = refined_lts_offset - preamble_symbol_len;
            const float pair_corr = impl_->measureAnalyticCorrelation(previous_symbol);
            if (pair_corr > 0.85f) {
                out_position = previous_symbol;
            }
        }
    }

    // Restore original buffer (do NOT modify state)
    impl_->rx_buffer = std::move(saved_buffer);

    return found_sync;
}

// =============================================================================
// CHANNEL ESTIMATOR (standalone class)
// =============================================================================

struct ChannelEstimator::Impl {
    ModemConfig config;
    std::vector<Complex> h_estimate;
    ChannelQuality quality;

    Impl(const ModemConfig& cfg)
        : config(cfg)
        , h_estimate(cfg.fft_size, Complex(1, 0))
    {}
};

ChannelEstimator::ChannelEstimator(const ModemConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

ChannelEstimator::~ChannelEstimator() = default;

void ChannelEstimator::updateFromPilots(const Symbol& received, const Symbol& expected) {
    for (size_t i = 0; i < received.size() && i < expected.size(); ++i) {
        if (std::norm(expected[i]) > 1e-10f) {
            Complex h = received[i] / expected[i];
            impl_->h_estimate[i] = 0.5f * h + 0.5f * impl_->h_estimate[i];
        }
    }
}

Symbol ChannelEstimator::equalize(const Symbol& received) {
    Symbol output(received.size());
    for (size_t i = 0; i < received.size(); ++i) {
        if (std::norm(impl_->h_estimate[i]) > 1e-10f) {
            output[i] = received[i] / impl_->h_estimate[i];
        } else {
            output[i] = received[i];
        }
    }
    return output;
}

ChannelQuality ChannelEstimator::getQuality() const {
    return impl_->quality;
}

void ChannelEstimator::interpolate() {
    // Simple linear interpolation
}

} // namespace ultra
