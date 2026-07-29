#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "demodulator_constants.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/types.hpp"

namespace ultra {
namespace ofdm_pilots {

inline bool scatteredPilotsActive(const ModemConfig& config) {
    return config.scattered_pilots &&
           config.use_pilots &&
           config.pilot_spacing > 0 &&
           ofdm_link_adaptation::isCoherentModulation(config.modulation);
}

inline size_t pilotCount(const ModemConfig& config) {
    if (!config.use_pilots || config.pilot_spacing == 0 ||
        config.num_carriers == 0) {
        return 0;
    }
    return (config.num_carriers + config.pilot_spacing - 1) /
           config.pilot_spacing;
}

inline size_t patternPeriod(const ModemConfig& config) {
    return scatteredPilotsActive(config)
        ? std::max<size_t>(1, static_cast<size_t>(config.pilot_spacing))
        : 1;
}

inline int fftIndexForLogical(const ModemConfig& config, size_t logical) {
    const int neg_limit = static_cast<int>(config.num_carriers / 2);
    int carrier = 0;
    size_t seen = 0;
    for (int k = -neg_limit;
         k <= static_cast<int>((config.num_carriers + 1) / 2);
         ++k) {
        if (k == 0) {
            continue;
        }
        if (seen == logical) {
            carrier = k;
            break;
        }
        ++seen;
    }
    return (carrier + static_cast<int>(config.fft_size)) %
           static_cast<int>(config.fft_size);
}

inline size_t logicalForFftIndex(const ModemConfig& config, int fft_idx) {
    const int neg_limit = static_cast<int>(config.num_carriers / 2);
    size_t logical = 0;
    for (int k = -neg_limit;
         k <= static_cast<int>((config.num_carriers + 1) / 2);
         ++k) {
        if (k == 0) {
            continue;
        }
        const int candidate =
            (k + static_cast<int>(config.fft_size)) %
            static_cast<int>(config.fft_size);
        if (candidate == fft_idx) {
            return logical;
        }
        ++logical;
    }
    return static_cast<size_t>(-1);
}

// ---------------------------------------------------------------------------
// Phase 0 (EFFECTIVE_SINR handoff §9): the carrier -> FREQUENCY contract.
//
// Until 2026-07-29 nothing in the tree converted a carrier to Hz -- the mapping
// lived implicitly in the mixer + IFFT convention and had to be re-derived by
// hand every time. That is precisely the kind of gap that lets a comparison be
// made in the wrong domain without anyone noticing, so it is now a function with
// a test (`OFDMCarrierOrdering`).
//
// THE CONTRACT, verified against buildCarrierPattern() below:
//   * carriers run k = -floor(Nc/2) .. +ceil(Nc/2), SKIPPING k = 0 (the DC bin is
//     never occupied and never carries a channel estimate);
//   * fft_idx = (k + fft_size) % fft_size, so negative k wrap to the FFT's upper
//     half -- bin NUMBER is not monotonic in frequency, but LOGICAL index is;
//   * TX mixes UP by center_freq (modulator.cpp:343, NCO e^{+j2*pi*f/fs}) and RX
//     mixes down by its exact conjugate (channel_equalizer_baseband.cpp:124-125),
//     so bin k is baseband +k*fs/N on both ends;
//   * therefore f_passband = center_freq + k * sample_rate / fft_size.
//
// For odd num_carriers there is one MORE positive-frequency carrier than negative
// (59 -> 29 below, 30 above), so the occupied band is NOT centred on center_freq;
// its midpoint sits half a bin above.
inline int signedCarrierIndex(const ModemConfig& config, int fft_idx) {
    const int n = static_cast<int>(config.fft_size);
    const int k = (fft_idx <= n / 2) ? fft_idx : fft_idx - n;
    return k;
}

inline float carrierFrequencyHz(const ModemConfig& config, int fft_idx) {
    return static_cast<float>(config.center_freq) +
           static_cast<float>(signedCarrierIndex(config, fft_idx)) *
               static_cast<float>(config.sample_rate) /
               static_cast<float>(config.fft_size);
}

inline float carrierFrequencyHzForLogical(const ModemConfig& config, size_t logical) {
    return carrierFrequencyHz(config, fftIndexForLogical(config, logical));
}

inline bool isPilotLogical(const ModemConfig& config,
                           size_t logical,
                           size_t symbol_index) {
    if (!config.use_pilots || config.pilot_spacing == 0) {
        return false;
    }
    if (!scatteredPilotsActive(config)) {
        return (logical % config.pilot_spacing) == 0;
    }

    const size_t n = static_cast<size_t>(config.num_carriers);
    const size_t d = static_cast<size_t>(config.pilot_spacing);
    const size_t count = pilotCount(config);
    const size_t offset = symbol_index % d;
    for (size_t p = 0; p < count; ++p) {
        if ((offset + p * d) % n == logical) {
            return true;
        }
    }
    return false;
}

inline Complex pilotSymbolForLogical(size_t logical) {
    // Deterministic BPSK pilots keyed by logical carrier. Scattered pilots move
    // in time, so the value must not depend on pilot ordinal.
    uint32_t x = demod_constants::PILOT_RNG_SEED ^
                 (static_cast<uint32_t>(logical) * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return (x & 1u) ? Complex(1, 0) : Complex(-1, 0);
}

inline void buildCarrierPattern(const ModemConfig& config,
                                size_t symbol_index,
                                std::vector<int>& all_carrier_fft_indices,
                                std::vector<int>& data_carrier_indices,
                                std::vector<int>& pilot_carrier_indices,
                                std::vector<size_t>& data_logical_indices,
                                std::vector<size_t>& pilot_logical_indices,
                                std::vector<bool>& is_pilot_logical,
                                std::vector<Complex>& pilot_sequence) {
    all_carrier_fft_indices.clear();
    data_carrier_indices.clear();
    pilot_carrier_indices.clear();
    data_logical_indices.clear();
    pilot_logical_indices.clear();
    is_pilot_logical.clear();
    pilot_sequence.clear();

    all_carrier_fft_indices.reserve(config.num_carriers);
    data_carrier_indices.reserve(config.num_carriers);
    pilot_carrier_indices.reserve(pilotCount(config));
    data_logical_indices.reserve(config.num_carriers);
    pilot_logical_indices.reserve(pilotCount(config));
    is_pilot_logical.reserve(config.num_carriers);
    pilot_sequence.reserve(pilotCount(config));

    const bool scattered = scatteredPilotsActive(config);
    const int neg_limit = static_cast<int>(config.num_carriers / 2);
    size_t logical = 0;
    std::mt19937 fixed_rng(demod_constants::PILOT_RNG_SEED);

    for (int k = -neg_limit;
         k <= static_cast<int>((config.num_carriers + 1) / 2);
         ++k) {
        if (k == 0) {
            continue;
        }

        const int fft_idx =
            (k + static_cast<int>(config.fft_size)) %
            static_cast<int>(config.fft_size);
        const bool is_pilot = isPilotLogical(config, logical, symbol_index);

        all_carrier_fft_indices.push_back(fft_idx);
        is_pilot_logical.push_back(is_pilot);
        if (is_pilot) {
            pilot_carrier_indices.push_back(fft_idx);
            pilot_logical_indices.push_back(logical);
            if (scattered) {
                pilot_sequence.push_back(pilotSymbolForLogical(logical));
            } else {
                pilot_sequence.push_back((fixed_rng() & 1)
                                             ? Complex(1, 0)
                                             : Complex(-1, 0));
            }
        } else {
            data_carrier_indices.push_back(fft_idx);
            data_logical_indices.push_back(logical);
        }
        ++logical;
    }
}

}  // namespace ofdm_pilots
}  // namespace ultra
