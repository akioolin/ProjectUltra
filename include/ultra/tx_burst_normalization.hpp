#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace ultra::sim {

inline constexpr float kTxBurstActiveThreshold = 1.0e-6f;
inline constexpr float kTxBurstPeakWarningThreshold = 0.95f;
inline constexpr float kTxBurstPeakClipThreshold = 1.0f;
// Empirical lower bound from the current operational modem TX paths locked by
// regression: the shortest valid burst is the light OFDM ACK active interval
// at 10080 samples. Shorter active intervals are callback/packet fragments,
// not PHY bursts, and must be moved upstream before normalization.
inline constexpr size_t kTxBurstMinimumActiveSamples = 10080;

// Simulator reference-band filter contract:
//   - Bandwidth: 50-2950 Hz, 48 kHz sample rate
//   - 101-tap Blackman-windowed FIR
//   - Power-normalized convention: a unit-RMS passband tone remains unit-RMS
//   - Used to define kModemReferenceInBandRms; receiver code may share these
//     coefficients, but the calibration contract is owned by the simulator
const std::vector<float>& referenceBandFirCoefficients();

struct TxBurstRmsMeasurement {
    size_t active_begin = 0;       // First active-region sample (|x| > threshold)
    size_t active_end = 0;         // One past last active-region sample
    size_t active_samples = 0;     // active_end - active_begin
    float broadband_rms = 0.0f;    // RMS over [active_begin, active_end)
    float in_band_rms = 0.0f;      // RMS after referenceBandFir over active region
    float gain_to_reference = 1.0f; // kModemReferenceInBandRms / in_band_rms
    float peak_after_gain = 0.0f;  // Max |x * gain| over the full burst
    size_t peak_warning_samples = 0; // Full-burst samples with |x * gain| > 0.95
    size_t peak_clip_samples = 0;  // Full-burst samples with |x * gain| > 1.0
    bool peak_warning = false;     // peak_after_gain > 0.95
    bool peak_clip_error = false;  // peak_after_gain > 1.0
    bool burst_fragment_warning = false; // active_samples < kTxBurstMinimumActiveSamples
};

// Measure only. Does not modify the samples.
TxBurstRmsMeasurement measureTxBurstInBandRms(std::span<const float> samples);

// Active in-band signal-power normalization to the simulator reference level
// before channel/noise injection. The gain is derived from the active-region
// RMS, then applied to the whole burst including leading/trailing/embedded
// silence. Clipping after normalization is expected for high-PAPR bursts and
// matches real-radio PA overdrive behavior; it does not invalidate the SNR
// delivery contract because active in-band RMS energy is preserved within
// tolerance. A future expert absolute-noise-density mode can bypass the normal
// SNR-to-reference-noise formula without changing this measurement contract.
TxBurstRmsMeasurement normalizeTxBurstToReference(std::vector<float>& samples);

}  // namespace ultra::sim
