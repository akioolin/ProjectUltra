#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace ultra::phy {

inline constexpr bool kPaprReductionDefaultEnabled = true;
inline constexpr float kOfdmPaprReductionDefaultThresholdDb = 10.0f;
inline constexpr float kOfdmPaprReductionReferenceThresholdDb = 8.0f;
inline constexpr float kOfdmPaprReductionTargetReductionDb = 4.0f;
inline constexpr float kOfdmPaprReductionMinPrePaprDb = 7.0f;

struct PaprReductionMeasurement {
    bool enabled = false;
    bool applied = false;
    size_t active_begin = 0;
    size_t active_end = 0;
    size_t active_samples = 0;
    float threshold_db = 0.0f;
    float threshold_amplitude = 0.0f;
    float pre_peak = 0.0f;
    float pre_rms = 0.0f;
    float pre_papr_db = 0.0f;
    float post_peak = 0.0f;
    float post_rms = 0.0f;
    float post_papr_db = 0.0f;
    float pre_in_band_rms = 0.0f;
    float post_in_band_rms = 0.0f;
    float in_band_rms_delta_db = 0.0f;
    float rms_takeback_gain = 1.0f;
    size_t clipped_samples = 0;
};

float measurePaprDb(std::span<const float> samples);

PaprReductionMeasurement applyPaprReduction(
    std::vector<float>& samples,
    float threshold_db = kOfdmPaprReductionDefaultThresholdDb,
    bool enable = kPaprReductionDefaultEnabled);

}  // namespace ultra::phy
