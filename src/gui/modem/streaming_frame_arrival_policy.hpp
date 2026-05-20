#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <limits>

namespace ultra {
namespace gui {
namespace streaming_frame_arrival_policy {

static constexpr size_t kSampleRateHz = 48000;
static constexpr size_t kDefaultTightWindowSamples = kSampleRateHz / 50; // 20 ms

struct SuccessfulFrameUpdate {
    bool had_previous_prediction = false;
    bool within_tight_window = true;
    bool has_arrival_error = false;
    int64_t arrival_error_samples = 0;
    size_t next_expected_frame_sample = 0;
    float confidence = 0.0f;
    int consecutive_sync_misses = 0;
};

inline int64_t signedSampleError(size_t actual_sample, size_t expected_sample) {
    const size_t max_i64 = static_cast<size_t>(std::numeric_limits<int64_t>::max());
    if (actual_sample >= expected_sample) {
        const size_t diff = actual_sample - expected_sample;
        return diff > max_i64 ? std::numeric_limits<int64_t>::max()
                              : static_cast<int64_t>(diff);
    }

    const size_t diff = expected_sample - actual_sample;
    return diff > max_i64 ? std::numeric_limits<int64_t>::min()
                          : -static_cast<int64_t>(diff);
}

inline size_t absSampleError(int64_t error_samples) {
    if (error_samples >= 0) {
        return static_cast<size_t>(error_samples);
    }
    if (error_samples == std::numeric_limits<int64_t>::min()) {
        return static_cast<size_t>(std::numeric_limits<int64_t>::max());
    }
    return static_cast<size_t>(-error_samples);
}

inline float clampConfidence(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

inline SuccessfulFrameUpdate updateOnSuccessfulFrame(
    bool has_previous_prediction,
    size_t previous_expected_frame_sample,
    float previous_confidence,
    size_t actual_frame_start_sample,
    size_t actual_frame_end_sample,
    size_t expected_gap_samples,
    size_t tight_window_samples = kDefaultTightWindowSamples) {

    SuccessfulFrameUpdate update;
    update.had_previous_prediction = has_previous_prediction;

    if (actual_frame_end_sample < actual_frame_start_sample) {
        actual_frame_end_sample = actual_frame_start_sample;
    }

    if (has_previous_prediction) {
        update.has_arrival_error = true;
        update.arrival_error_samples =
            signedSampleError(actual_frame_start_sample, previous_expected_frame_sample);

        const size_t abs_error = absSampleError(update.arrival_error_samples);
        update.within_tight_window = abs_error <= tight_window_samples;

        const float error_fraction = tight_window_samples == 0
            ? (abs_error == 0 ? 0.0f : 1.0f)
            : std::min(1.0f, static_cast<float>(abs_error) /
                                static_cast<float>(tight_window_samples));
        const float observed_confidence = 1.0f - error_fraction;
        update.confidence = clampConfidence(0.75f * previous_confidence +
                                            0.25f * observed_confidence);
    } else {
        update.confidence = 0.35f;
    }

    update.next_expected_frame_sample = actual_frame_end_sample + expected_gap_samples;
    update.consecutive_sync_misses = 0;
    return update;
}

inline int incrementSyncMisses(int current_misses) {
    if (current_misses == std::numeric_limits<int>::max()) {
        return current_misses;
    }
    return current_misses + 1;
}

inline float confidenceAfterSyncMiss(float previous_confidence) {
    return clampConfidence(previous_confidence * 0.65f);
}

}  // namespace streaming_frame_arrival_policy
}  // namespace gui
}  // namespace ultra
