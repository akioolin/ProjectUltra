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
        switch (rate) {
            case CodeRate::R3_4: return 8;
            case CodeRate::R2_3:
            case CodeRate::R1_2:
            case CodeRate::R1_4:
            case CodeRate::R1_3:
            default:
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
        if (fading_index >= 0.45f) return 6;
        return 4;
    }
    return 4;
}

inline int sanitizeBurstGroupSize(int value) {
    return std::clamp(value, 2, 8);
}

} // namespace ofdm_link_adaptation
} // namespace ultra
