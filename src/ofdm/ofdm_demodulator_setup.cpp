// OFDM demodulator setup and carrier tables
// Part of OFDMDemodulator::Impl

#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>
#include <random>
#include "ultra/ofdm.hpp"
#include "ultra/dsp.hpp"
#include "ultra/logging.hpp"
#include "demodulator_impl.hpp"
#include "demodulator_constants.hpp"

namespace ultra {

using namespace demod_constants;

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
    baseband_scratch.resize(symbol_samples);
    symbol_scratch.resize(cfg.fft_size);
    freq_domain_scratch.resize(cfg.fft_size);
    equalized_scratch.resize(static_cast<size_t>(cfg.num_carriers));
    constellation_update_scratch.reserve(static_cast<size_t>(cfg.num_carriers));
    differential_symbols_scratch.resize(static_cast<size_t>(cfg.num_carriers));
    differential_signal_power_scratch.resize(static_cast<size_t>(cfg.num_carriers));
    d8psk_constellation_update_scratch.reserve(static_cast<size_t>(cfg.num_carriers));
    dqpsk_constellation_update_scratch.reserve(static_cast<size_t>(cfg.num_carriers));
    dqpsk_valid_errors_scratch.reserve(static_cast<size_t>(cfg.num_carriers));
    interp_h_full_scratch.resize(static_cast<size_t>(cfg.num_carriers));
    interp_h_cir_scratch.resize(static_cast<size_t>(cfg.num_carriers));
    interp_h_clean_scratch.resize(static_cast<size_t>(cfg.num_carriers));
    interp_pilot_logical_pos_scratch.reserve(static_cast<size_t>(cfg.num_carriers));
    channel_estimate.resize(cfg.fft_size, Complex(1, 0));

    // Initialize adaptive equalizer state
    lms_weights.resize(cfg.fft_size, Complex(1, 0));
    last_decisions.resize(cfg.fft_size, Complex(0, 0));
    rls_P.resize(cfg.fft_size, 1.0f);

    setupCarriers();
    generateSequences();
    buildInterpTable();
    buildInterpolationPhasors();
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

void OFDMDemodulator::Impl::buildInterpolationPhasors() {
    const size_t N = all_carrier_fft_indices.size();
    interp_idft_phasors.resize(N * N);
    interp_dft_phasors.resize(N * N);

    if (N == 0) {
        return;
    }

    const float inv_N = 1.0f / static_cast<float>(N);
    for (size_t n = 0; n < N; ++n) {
        for (size_t k = 0; k < N; ++k) {
            const float idft_phase = 2.0f * M_PI * k * n * inv_N;
            interp_idft_phasors[n * N + k] =
                Complex(std::cos(idft_phase), std::sin(idft_phase));

            const float dft_phase = -2.0f * M_PI * k * n * inv_N;
            interp_dft_phasors[k * N + n] =
                Complex(std::cos(dft_phase), std::sin(dft_phase));
        }
    }
}

} // namespace ultra
