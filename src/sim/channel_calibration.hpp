#pragma once

#include "ota_channel_core/models.hpp"

#include <cmath>

namespace ultra::sim {

// Compatibility aliases for simulator code. The canonical modem SNR reference
// constants live in ota_channel_core because that is where channel/noise
// injection is defined.
inline constexpr float kModemReferenceBroadbandRms =
    ota_channel_core::kModemReferenceBroadbandRms;
inline constexpr float kModemReferenceInBandRms =
    ota_channel_core::kModemReferenceInBandRms;
inline constexpr float kModemReferenceRms = kModemReferenceInBandRms;
inline constexpr double kModemReferencePower =
    ota_channel_core::kModemReferencePower;

inline constexpr double kModemInBandNoisePowerFraction =
    ota_channel_core::kModemInBandNoisePowerFraction;
inline constexpr double kModemBroadbandToInBandSnrOffsetDb =
    ota_channel_core::kModemBroadbandToInBandSnrOffsetDb;

inline float broadbandToInBandSnrDb(float broadband_snr_db) {
    return broadband_snr_db +
           static_cast<float>(kModemBroadbandToInBandSnrOffsetDb);
}

inline float inBandToBroadbandSnrDb(float in_band_snr_db) {
    return in_band_snr_db -
           static_cast<float>(kModemBroadbandToInBandSnrOffsetDb);
}

inline float modemReferenceBroadbandNoiseStddev(float broadband_snr_db) {
    return ota_channel_core::kModemReferenceRms *
           std::pow(10.0f, -broadband_snr_db / 20.0f);
}

// Configured channel SNR is operator-facing in-band SNR. For white noise
// generated over the full audio stream, size the broadband sigma so the
// receiver FIR leaves the requested in-band noise power.
inline float modemReferenceNoiseStddev(float snr_db) {
    return modemReferenceBroadbandNoiseStddev(inBandToBroadbandSnrDb(snr_db));
}

}  // namespace ultra::sim
