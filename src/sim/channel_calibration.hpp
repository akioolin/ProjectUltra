#pragma once

#include <cmath>

namespace ultra::sim {

// Empirically measured from StreamingEncoder::encodePing() output on
// 2026-05-14: 62208 samples, RMS 0.318072406640.
inline constexpr float kModemReferenceRms = 0.3180724f;
inline constexpr double kModemReferencePower =
    static_cast<double>(kModemReferenceRms) *
    static_cast<double>(kModemReferenceRms);

inline float modemReferenceNoiseStddev(float snr_db) {
    return kModemReferenceRms *
           std::pow(10.0f, -snr_db / 20.0f);
}

}  // namespace ultra::sim
