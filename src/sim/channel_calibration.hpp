#pragma once

#include <cmath>

namespace ultra::sim {

// Empirically measured from StreamingEncoder::encodePing() output on
// 2026-05-14: 62208 samples, broadband RMS 0.318072406640.
inline constexpr float kModemReferenceBroadbandRms = 0.3180724f;

// The operator-facing SNR convention is receiver in-band signal/noise. Measured
// by passing the same PING through the 101-tap 50-2950 Hz receive FIR:
// in-band RMS 0.304826641347, i.e. -0.369461 dB versus broadband.
inline constexpr float kModemReferenceInBandRms = 0.30482664f;
inline constexpr float kModemReferenceRms = kModemReferenceInBandRms;
inline constexpr double kModemReferencePower =
    static_cast<double>(kModemReferenceRms) *
    static_cast<double>(kModemReferenceRms);

// Equivalent noise bandwidth of the 101-tap 50-2950 Hz receive FIR used by
// IdleNoiseSNREstimator. White broadband noise at the channel input leaves this
// fraction of its power inside the modem band.
inline constexpr double kModemInBandNoisePowerFraction = 0.10858718;
inline constexpr double kModemBroadbandToInBandSnrOffsetDb = 9.64221445;

inline float broadbandToInBandSnrDb(float broadband_snr_db) {
    return broadband_snr_db +
           static_cast<float>(kModemBroadbandToInBandSnrOffsetDb);
}

inline float inBandToBroadbandSnrDb(float in_band_snr_db) {
    return in_band_snr_db -
           static_cast<float>(kModemBroadbandToInBandSnrOffsetDb);
}

inline float modemReferenceBroadbandNoiseStddev(float broadband_snr_db) {
    return kModemReferenceRms *
           std::pow(10.0f, -broadband_snr_db / 20.0f);
}

// Configured channel SNR is operator-facing in-band SNR. For white noise
// generated over the full audio stream, size the broadband sigma so the
// receiver FIR leaves the requested in-band noise power.
inline float modemReferenceNoiseStddev(float snr_db) {
    return modemReferenceBroadbandNoiseStddev(inBandToBroadbandSnrDb(snr_db));
}

}  // namespace ultra::sim
