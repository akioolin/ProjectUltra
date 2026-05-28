#pragma once

#include "types.hpp"
#include <algorithm>

namespace ultra {
namespace ofdm_link_adaptation {

inline bool isCoherentModulation(Modulation mod) {
    switch (mod) {
        case Modulation::BPSK:
        case Modulation::QPSK:
        case Modulation::QAM8:
        case Modulation::QAM16:
        case Modulation::QAM32:
        case Modulation::QAM64:
        case Modulation::QAM256:
            return true;
        default:
            return false;
    }
}

// Deterministic pilot profile (must be identical on TX and RX).
// This intentionally uses only signaled mode/rate to avoid cross-station mismatch.
inline int recommendedPilotSpacing(Modulation mod, CodeRate rate) {
    const bool coherent = isCoherentModulation(mod);

    if (coherent) {
        // 2026-05-28 ADAPTIVE PILOT DENSITY: pilot density is now tied to rate.
        // - High rate (R3/4 + R2/3): try sparser pilots (~6-8) for higher data
        //   carrier count; rate controller already gates these to clean channels.
        // - Low rate (R1/2 + R1/4): drop back to dense pilots (5/12). When the
        //   adaptive controller falls to these rates the channel is rough, so
        //   the pilot-density saving is the WRONG trade — we want robust tracking.
        // Earlier ULTRA_COH_PILOT_SPACING covered all non-R3/4 rates, which
        // meant a rate-drop also lost pilot density on the bumpy channel. Now
        // ULTRA_R23_PILOT_SPACING only applies at R2/3; rates below stay dense.
        const auto envSpacing = [](const char* name, int default_value) {
            if (const char* env = std::getenv(name)) {
                const int v = std::atoi(env);
                if (v >= 5 && v <= 20) return v;
            }
            return default_value;
        };
        switch (rate) {
            case CodeRate::R3_4:
                if (mod == Modulation::QAM32 || mod == Modulation::QAM64) {
                    return 5;
                }
                return envSpacing("ULTRA_R34_PILOT_SPACING", 8);
            case CodeRate::R2_3:
                // Per the industry-leader's L12 (3000 bps slot at R2/3), the
                // production target wants the sparsest pilots that still hold
                // the worst-fade-seed within the rate's FEC margin.
                return envSpacing("ULTRA_R23_PILOT_SPACING", 5);
            case CodeRate::R1_2:
            case CodeRate::R1_4:
            case CodeRate::R1_3:
            default:
                // Rate-adaptation fell to these because channel is rough — dense
                // pilots are mandatory for robust tracking on the bumpy channel.
                return 5;
        }
    }

    // Differential modes: keep DQPSK/DBPSK profile, densify D8PSK.
    if (mod == Modulation::D8PSK) {
        switch (rate) {
            case CodeRate::R3_4: return 8;
            case CodeRate::R2_3:
            case CodeRate::R1_2: return 8;
            case CodeRate::R1_4:
            case CodeRate::R1_3:
            default:
                return 10;
        }
    }

    switch (rate) {
        case CodeRate::R3_4: return 15;
        case CodeRate::R2_3:
        case CodeRate::R1_2:
        case CodeRate::R1_4:
        case CodeRate::R1_3:
        default:
            return 10;
    }
}

inline int pilotCount(int total_carriers, int pilot_spacing) {
    if (pilot_spacing <= 0 || total_carriers <= 0) return 0;
    return (total_carriers + pilot_spacing - 1) / pilot_spacing;
}

inline int dataCarrierCount(int total_carriers, bool use_pilots, int pilot_spacing) {
    if (total_carriers <= 0) return 0;
    if (!use_pilots) return total_carriers;

    const int pilots = pilotCount(total_carriers, pilot_spacing);
    return std::max(1, total_carriers - pilots);
}

inline int bitsPerOFDMSymbol(int total_carriers,
                             bool use_pilots,
                             int pilot_spacing,
                             Modulation mod) {
    const int data_carriers = dataCarrierCount(total_carriers, use_pilots, pilot_spacing);
    if (data_carriers <= 0) return 0;
    return data_carriers * static_cast<int>(getBitsPerSymbol(mod));
}

// Experimental burst interleaver group sizing helper.
inline int recommendedBurstGroupSize(Modulation mod, CodeRate rate, float fading_index = 0.0f) {
    if (mod == Modulation::D8PSK &&
        (rate == CodeRate::R1_2 || rate == CodeRate::R2_3 || rate == CodeRate::R3_4)) {
        if (fading_index >= 0.45f) return 8;
        return 8;
    }
    return 8;
}

inline int sanitizeBurstGroupSize(int value) {
    // Ceiling raised 8 -> 32 (2026-05-26): the burst interleaver is the
    // file-class fade-diversity keystone, and a Good HF fade (Tc ~= 4 s at
    // 0.1 Hz Doppler) needs the group to span MULTIPLE coherence times to
    // convert a slow deep fade into recoverable spread loss. N=8 ~= 1xTc is
    // insufficient (see docs burst-interleaver diagnosis); the genie depth
    // sweep (tests/bench_fade_interleave.cpp) shows the FER prize only opens
    // up at D >= 16-32. Default group size stays 8; deeper groups are
    // opt-in via setBurstInterleaveGroupSize / --burst-group-size.
    return std::clamp(value, 2, 32);
}

// Estimate the maximum payload rate (bits/sec) for the given OFDM geometry,
// modulation, and code rate. Bookkeeping helper kept here (used by tests + any
// future link-adaptation logic). The 0.9 factor is the historical overhead
// reservation for framing/preamble/pilots; it is the legacy nominal value
// inherited from the original Modem API.
inline float calculateMaxDataRate(const ModemConfig& config, Modulation mod, CodeRate rate) {
    const std::size_t bits_per_carrier = getBitsPerSymbol(mod);
    const std::size_t data_carriers    = config.getDataCarriers();
    const std::size_t bits_per_symbol  = data_carriers * bits_per_carrier;
    const float symbol_samples  = static_cast<float>(config.getSymbolDuration());
    const float symbol_duration = symbol_samples / static_cast<float>(config.sample_rate);
    const float raw_bps         = static_cast<float>(bits_per_symbol) / symbol_duration;
    return raw_bps * getCodeRateValue(rate) * 0.9f;
}

} // namespace ofdm_link_adaptation
} // namespace ultra
