// OFDM symbol demapping
// Part of OFDMDemodulator::Impl

#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "ultra/ofdm.hpp"
#include "ultra/dsp.hpp"
#include "ultra/logging.hpp"
#include "demodulator_impl.hpp"
#include "demodulator_constants.hpp"
#include "soft_demap.hpp"

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
        case Modulation::QAM8:
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

constexpr float MIN_DIFFERENTIAL_DISPLAY_POWER = 0.2f;

Complex normalizePhaseOnlyForDisplay(Complex symbol) {
    const float mag = std::abs(symbol);
    if (mag < 1.0e-6f) {
        return symbol;
    }
    return symbol / mag;
}

const char* constellationDiagModName(Modulation mod) {
    switch (mod) {
        case Modulation::DBPSK: return "DBPSK";
        case Modulation::BPSK: return "BPSK";
        case Modulation::DQPSK: return "DQPSK";
        case Modulation::QPSK: return "QPSK";
        case Modulation::D8PSK: return "D8PSK";
        case Modulation::QAM8: return "QAM8";
        case Modulation::QAM16: return "16QAM";
        case Modulation::QAM32: return "QAM32";
        case Modulation::QAM64: return "QAM64";
        case Modulation::QAM256: return "QAM256";
        default: return "UNKNOWN";
    }
}

bool constellationDiagEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ULTRA_CONSTELLATION_DIAG");
        if (!value) return false;
        return value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

struct ConstellationDiagStats {
    size_t count = 0;
    size_t below_0p2 = 0;
    float min_mag = 0.0f;
    float avg_mag = 0.0f;
    float max_mag = 0.0f;
};

struct ConstellationClusterStats {
    size_t occupied = 0;
    float rms_evm = 0.0f;
    float max_evm = 0.0f;
};

ConstellationDiagStats constellationDiagStats(const std::vector<Complex>& symbols) {
    ConstellationDiagStats stats;
    stats.count = symbols.size();
    if (symbols.empty()) {
        return stats;
    }

    stats.min_mag = std::numeric_limits<float>::max();
    double sum = 0.0;
    for (const auto& symbol : symbols) {
        const float mag = std::abs(symbol);
        stats.min_mag = std::min(stats.min_mag, mag);
        stats.max_mag = std::max(stats.max_mag, mag);
        sum += mag;
        if (mag < 0.2f) {
            ++stats.below_0p2;
        }
    }
    stats.avg_mag = static_cast<float>(sum / static_cast<double>(symbols.size()));
    return stats;
}

std::vector<Complex> constellationReferencePoints(Modulation mod) {
    static const float pi = 3.14159265358979f;
    std::vector<Complex> points;

    if (mod == Modulation::DBPSK || mod == Modulation::BPSK) {
        points = {Complex(-1, 0), Complex(1, 0)};
    } else if (mod == Modulation::DQPSK) {
        for (int i = 0; i < 4; ++i) {
            const float angle = i * pi / 2.0f;
            points.emplace_back(std::cos(angle), std::sin(angle));
        }
    } else if (mod == Modulation::QPSK) {
        for (int i = 0; i < 4; ++i) {
            const float angle = pi / 4.0f + i * pi / 2.0f;
            points.emplace_back(std::cos(angle), std::sin(angle));
        }
    } else if (mod == Modulation::D8PSK || mod == Modulation::QAM8) {
        const float offset = (mod == Modulation::D8PSK) ? pi / 8.0f : pi / 8.0f;
        for (int i = 0; i < 8; ++i) {
            const float angle = offset + i * pi / 4.0f;
            points.emplace_back(std::cos(angle), std::sin(angle));
        }
    } else if (mod == Modulation::QAM16 || mod == Modulation::QAM64 ||
               mod == Modulation::QAM256) {
        int points_per_axis = 4;
        if (mod == Modulation::QAM64) {
            points_per_axis = 8;
        } else if (mod == Modulation::QAM256) {
            points_per_axis = 16;
        }
        const float norm = std::sqrt((2.0f / 3.0f) *
                                     (points_per_axis * points_per_axis - 1.0f));
        points.reserve(static_cast<size_t>(points_per_axis * points_per_axis));
        for (int yi = 0; yi < points_per_axis; ++yi) {
            for (int xi = 0; xi < points_per_axis; ++xi) {
                const float x = (2.0f * xi - points_per_axis + 1) / norm;
                const float y = (2.0f * yi - points_per_axis + 1) / norm;
                points.emplace_back(x, y);
            }
        }
    }
    return points;
}

ConstellationClusterStats constellationClusterStats(
        const std::vector<Complex>& symbols, Modulation mod) {
    ConstellationClusterStats stats;
    const auto refs = constellationReferencePoints(mod);
    if (symbols.empty() || refs.empty()) {
        return stats;
    }

    float power = 0.0f;
    for (const auto& symbol : symbols) {
        power += std::norm(symbol);
    }
    const float rms = std::sqrt(power / static_cast<float>(symbols.size()));
    const float scale = (rms > 1.0e-6f) ? (1.0f / rms) : 1.0f;

    std::vector<size_t> hits(refs.size(), 0);
    double evm_sq_sum = 0.0;
    for (const auto& symbol : symbols) {
        const Complex normalized = symbol * scale;
        size_t best = 0;
        float best_dist = std::numeric_limits<float>::max();
        for (size_t i = 0; i < refs.size(); ++i) {
            const float dist = std::norm(normalized - refs[i]);
            if (dist < best_dist) {
                best_dist = dist;
                best = i;
            }
        }
        ++hits[best];
        evm_sq_sum += best_dist;
        stats.max_evm = std::max(stats.max_evm, std::sqrt(best_dist));
    }

    for (size_t hit : hits) {
        if (hit > 0) {
            ++stats.occupied;
        }
    }
    stats.rms_evm = std::sqrt(
        static_cast<float>(evm_sq_sum / static_cast<double>(symbols.size())));
    return stats;
}

} // namespace
// =============================================================================
// SYMBOL DEMODULATION
// =============================================================================

void OFDMDemodulator::Impl::appendConstellationSymbols(
        const std::vector<Complex>& update, Modulation mod) {
    if (update.empty()) return;
    std::lock_guard<std::mutex> lock(constellation_mutex);
    // Reset the display buffer when the modulation changes so the GUI never
    // overlays different constellations (e.g. 16QAM data + DQPSK ACKs) into one
    // smeared cloud. Each batch is homogeneous in modulation.
    if (mod != constellation_mod_) {
        constellation_symbols.clear();
        constellation_mod_ = mod;
    }
    constellation_symbols.insert(constellation_symbols.end(),
                                 update.begin(), update.end());
    if (constellation_symbols.size() > MAX_CONSTELLATION_SYMBOLS) {
        constellation_symbols.erase(
            constellation_symbols.begin(),
            constellation_symbols.begin() +
                (constellation_symbols.size() - MAX_CONSTELLATION_SYMBOLS));
    }

    if (constellationDiagEnabled()) {
        static int append_log_count = 0;
        if (append_log_count < 80) {
            const auto update_stats = constellationDiagStats(update);
            const auto buffer_stats = constellationDiagStats(constellation_symbols);
            const auto update_clusters = constellationClusterStats(update, mod);
            const auto buffer_clusters = constellationClusterStats(constellation_symbols, mod);
            LOG_DEMOD(INFO,
                      "CONSTELLATION_DIAG stage=append mod=%s update_count=%zu "
                      "update_mag=[%.4f,%.4f,%.4f] update_lt0p2=%zu/%zu "
                      "update_clusters=%zu update_evm_rms=%.4f update_evm_max=%.4f "
                      "buffer_count=%zu buffer_mag=[%.4f,%.4f,%.4f] "
                      "buffer_lt0p2=%zu/%zu buffer_clusters=%zu "
                      "buffer_evm_rms=%.4f buffer_evm_max=%.4f",
                      constellationDiagModName(mod),
                      update_stats.count,
                      update_stats.min_mag,
                      update_stats.avg_mag,
                      update_stats.max_mag,
                      update_stats.below_0p2,
                      update_stats.count,
                      update_clusters.occupied,
                      update_clusters.rms_evm,
                      update_clusters.max_evm,
                      buffer_stats.count,
                      buffer_stats.min_mag,
                      buffer_stats.avg_mag,
                      buffer_stats.max_mag,
                      buffer_stats.below_0p2,
                      buffer_stats.count,
                      buffer_clusters.occupied,
                      buffer_clusters.rms_evm,
                      buffer_clusters.max_evm);
            ++append_log_count;
        }
    }
}

void OFDMDemodulator::Impl::demodulateSymbol(const std::vector<Complex>& equalized, Modulation mod) {
    // Constellation symbols collected during demodulation (differential decoded for DPSK modes)
    auto& constellation_update = constellation_update_scratch;
    constellation_update.clear();

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

    // Two-pass DQPSK decoding is intentionally disabled. Testing showed it
    // compresses LLR dynamic range too much at SNR=20, reducing scale from
    // ~20 to 5-13. The normal path with per-carrier noise_var, fading scaling,
    // and perturbation retry works better.

    // Debug: log modulation value once per symbol
    static int mod_log_once = 0;
    if (mod_log_once++ < 5) {
        LOG_DEMOD(DEBUG, "demodulateSymbol: mod=%d (DQPSK=%d), carriers=%zu, snr_count=%d",
                  static_cast<int>(mod), static_cast<int>(Modulation::DQPSK),
                  equalized.size(), snr_symbol_count);
    }

    auto& differential_symbols = differential_symbols_scratch;
    auto& differential_signal_power = differential_signal_power_scratch;
    if (mod == Modulation::DQPSK || mod == Modulation::D8PSK) {
        differential_symbols.resize(equalized.size());
        differential_signal_power.resize(equalized.size());
        std::fill(differential_symbols.begin(), differential_symbols.end(), Complex(0, 0));
        std::fill(differential_signal_power.begin(), differential_signal_power.end(), 0.0f);
    }

    size_t constellation_padding_skipped = 0;
    size_t constellation_weak_diff_skipped = 0;
    const size_t carrier_bits = bitsPerCarrier(mod);
    for (size_t i = 0; i < equalized.size(); ++i) {
        const size_t air_bit_index = constellation_air_bit_index_;
        constellation_air_bit_index_ += carrier_bits;
        const bool display_payload_carrier =
            air_bit_index < constellation_valid_air_bits_;
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
                    const float signal_power = std::abs(sym) * std::abs(prev_sym);
                    if (!display_payload_carrier) {
                        ++constellation_padding_skipped;
                    } else if (signal_power < MIN_DIFFERENTIAL_DISPLAY_POWER) {
                        ++constellation_weak_diff_skipped;
                    } else {
                        constellation_update.push_back(normalizePhaseOnlyForDisplay(diff));
                    }
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
                    if (!display_payload_carrier) {
                        ++constellation_padding_skipped;
                    } else if (differential_signal_power[i] < MIN_DIFFERENTIAL_DISPLAY_POWER) {
                        ++constellation_weak_diff_skipped;
                    } else {
                        constellation_update.push_back(normalizePhaseOnlyForDisplay(diff));
                    }
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
                    if (!display_payload_carrier) {
                        ++constellation_padding_skipped;
                    } else if (differential_signal_power[i] < MIN_DIFFERENTIAL_DISPLAY_POWER) {
                        ++constellation_weak_diff_skipped;
                    } else {
                        constellation_update.push_back(normalizePhaseOnlyForDisplay(diff));
                    }
                }
                dbpsk_prev_equalized[i] = sym;
                differential_prev_erased_[i] = 0;
                break;
            }
            case Modulation::BPSK:
                soft_bits.push_back(soft_demap::demapBPSK(sym, nv) * llr_sign);
                if (display_payload_carrier) {
                    constellation_update.push_back(sym);
                } else {
                    ++constellation_padding_skipped;
                }
                break;
            case Modulation::QPSK: {
                auto llrs = soft_demap::demapQPSK(sym, nv);
                for (auto& llr : llrs) llr *= llr_sign;
                soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());
                if (display_payload_carrier) {
                    constellation_update.push_back(sym);
                } else {
                    ++constellation_padding_skipped;
                }
                break;
            }
            case Modulation::QAM8: {
                auto llrs = soft_demap::demap8PSK(sym, nv);
                for (auto& llr : llrs) llr *= llr_sign;
                soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());
                if (display_payload_carrier) {
                    constellation_update.push_back(normalizePhaseOnlyForDisplay(sym));
                } else {
                    ++constellation_padding_skipped;
                }
                break;
            }
            case Modulation::QAM16: {
                auto llrs = soft_demap::demapQAM16(sym, nv);
                for (auto& llr : llrs) llr *= llr_sign;
                soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());
                if (display_payload_carrier) {
                    constellation_update.push_back(sym);
                } else {
                    ++constellation_padding_skipped;
                }
                break;
            }
            case Modulation::QAM32: {
                auto llrs = soft_demap::demapQAM32(sym, nv);
                for (auto& llr : llrs) llr *= llr_sign;
                soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());
                if (display_payload_carrier) {
                    constellation_update.push_back(sym);
                } else {
                    ++constellation_padding_skipped;
                }
                break;
            }
            case Modulation::QAM64: {
                auto llrs = soft_demap::demapQAM64(sym, nv);
                for (auto& llr : llrs) llr *= llr_sign;
                soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());
                if (display_payload_carrier) {
                    constellation_update.push_back(sym);
                } else {
                    ++constellation_padding_skipped;
                }
                break;
            }
            case Modulation::QAM256: {
                auto llrs = soft_demap::demapQAM256(sym, nv);
                for (auto& llr : llrs) llr *= llr_sign;
                soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());
                if (display_payload_carrier) {
                    constellation_update.push_back(sym);
                } else {
                    ++constellation_padding_skipped;
                }
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
    if (constellationDiagEnabled()) {
        static int demod_log_count = 0;
        if (demod_log_count < 120) {
            size_t erased = 0;
            for (uint8_t flag : carrier_erasure_flags_) {
                if (flag != 0) {
                    ++erased;
                }
            }
            size_t weak_diff_refs = 0;
            if ((mod == Modulation::DQPSK || mod == Modulation::D8PSK) &&
                differential_signal_power.size() == equalized.size()) {
                for (float signal_power : differential_signal_power) {
                    if (signal_power < MIN_DIFFERENTIAL_DISPLAY_POWER) {
                        ++weak_diff_refs;
                    }
                }
            }
            const auto eq_stats = constellationDiagStats(equalized);
            const auto update_stats = constellationDiagStats(constellation_update);
            LOG_DEMOD(INFO,
                      "CONSTELLATION_DIAG stage=demod mod=%s data_symbol=%zu "
                      "all_carriers=%zu data_carriers=%zu pilots=%zu "
                      "air_bits=%zu/%zu "
                      "equalized_count=%zu equalized_mag=[%.4f,%.4f,%.4f] "
                      "equalized_lt0p2=%zu/%zu erased=%zu "
                      "weak_diff_refs=%zu plotted_update_count=%zu "
                      "plot_skip_padding=%zu plot_skip_weak_diff=%zu "
                      "plotted_mag=[%.4f,%.4f,%.4f] plotted_lt0p2=%zu/%zu",
                      constellationDiagModName(mod),
                      current_data_symbol_index_,
                      all_carrier_fft_indices.size(),
                      data_carrier_indices.size(),
                      pilot_carrier_indices.size(),
                      std::min(constellation_air_bit_index_, constellation_valid_air_bits_),
                      constellation_valid_air_bits_,
                      eq_stats.count,
                      eq_stats.min_mag,
                      eq_stats.avg_mag,
                      eq_stats.max_mag,
                      eq_stats.below_0p2,
                      eq_stats.count,
                      erased,
                      weak_diff_refs,
                      update_stats.count,
                      constellation_padding_skipped,
                      constellation_weak_diff_skipped,
                      update_stats.min_mag,
                      update_stats.avg_mag,
                      update_stats.max_mag,
                      update_stats.below_0p2,
                      update_stats.count);
            ++demod_log_count;
        }
    }
    appendConstellationSymbols(constellation_update, mod);

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
    auto& constellation_update = d8psk_constellation_update_scratch;
    constellation_update.clear();
    if (differential_prev_erased_.size() != equalized.size()) {
        differential_prev_erased_.assign(equalized.size(), 0);
    }
    const size_t carrier_bits = bitsPerCarrier(Modulation::D8PSK);
    for (size_t i = 0; i < equalized.size(); ++i) {
        const size_t air_bit_index = constellation_air_bit_index_;
        constellation_air_bit_index_ += carrier_bits;
        const bool display_payload_carrier =
            air_bit_index < constellation_valid_air_bits_;
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
            const float signal_power = std::abs(corrected_sym) * std::abs(prev_sym);
            if (display_payload_carrier &&
                signal_power >= MIN_DIFFERENTIAL_DISPLAY_POWER) {
                constellation_update.push_back(normalizePhaseOnlyForDisplay(diff));
            }
        }

        // Update reference with corrected symbol
        dbpsk_prev_equalized[i] = corrected_sym;
        differential_prev_erased_[i] = 0;
    }

    // Store differential symbols for constellation display (D8PSK two-pass path)
    appendConstellationSymbols(constellation_update, Modulation::D8PSK);

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
    auto& valid_errors = dqpsk_valid_errors_scratch;
    valid_errors.clear();

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

    auto& constellation_update = dqpsk_constellation_update_scratch;
    constellation_update.clear();
    const size_t carrier_bits = bitsPerCarrier(Modulation::DQPSK);
    for (size_t i = 0; i < equalized.size(); ++i) {
        const size_t air_bit_index = constellation_air_bit_index_;
        constellation_air_bit_index_ += carrier_bits;
        const bool display_payload_carrier =
            air_bit_index < constellation_valid_air_bits_;
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
            if (display_payload_carrier && sp >= MIN_DIFFERENTIAL_DISPLAY_POWER) {
                constellation_update.push_back(normalizePhaseOnlyForDisplay(diff));
            }
        }

        // Update reference with ORIGINAL symbol (not corrected)
        dbpsk_prev_equalized[i] = equalized[i];
    }

    // Store differential symbols for constellation display (DQPSK two-pass path)
    appendConstellationSymbols(constellation_update, Modulation::DQPSK);

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

} // namespace ultra
