#pragma once

#include <cmath>

namespace ultra::sim {

// Empirically measured from StreamingEncoder::encodePing() output on
// 2026-05-14: 62208 samples, RMS 0.318072406640.
inline constexpr float kModemReferenceRms = 0.3180724f;
inline constexpr double kModemReferencePower =
    static_cast<double>(kModemReferenceRms) *
    static_cast<double>(kModemReferenceRms);

// Equivalent noise bandwidth of the 101-tap 50-2950 Hz receive FIR used by
// IdleNoiseSNREstimator. White broadband noise at the channel input leaves this
// fraction of its power inside the modem band. The operator/rate-selection SNR
// convention is in-band SNR, so older broadband-equivalent estimates add this
// correction before publication.
inline constexpr double kModemInBandNoisePowerFraction = 0.10858718;
inline constexpr double kModemBroadbandToInBandSnrOffsetDb = 9.64221445;

inline float broadbandToInBandSnrDb(float broadband_snr_db) {
    return broadband_snr_db +
           static_cast<float>(kModemBroadbandToInBandSnrOffsetDb);
}

inline float modemReferenceNoiseStddev(float snr_db) {
    return kModemReferenceRms *
           std::pow(10.0f, -snr_db / 20.0f);
}

}  // namespace ultra::sim
