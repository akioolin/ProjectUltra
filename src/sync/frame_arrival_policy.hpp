#pragma once

// Frame-arrival / warm-sync timing policy — PURE helpers (no I/O, no gui deps).
//
// Relocated 2026-05-31 from src/gui/modem/streaming_frame_arrival_policy.hpp into src/sync as
// part of the SyncController refactor (docs/SYNC_ACQUISITION_FIX_PLAN_2026_05_31.md §7.4): this is
// sync logic, and it must be reachable from src/sync/sync_controller.{hpp,cpp} (which cannot include
// gui/modem). The content is byte-identical to the old gui header; only the namespace/location moved
// (ultra::gui::streaming_frame_arrival_policy -> ultra::sync::frame_arrival_policy). The gui call
// sites keep their `namespace arrival_policy = sync::frame_arrival_policy;` alias, so the ~50
// `arrival_policy::` use-sites are unchanged.

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <limits>

namespace ultra {
namespace sync {
namespace frame_arrival_policy {

static constexpr size_t kSampleRateHz = 48000;
static constexpr size_t kDefaultTightWindowSamples = kSampleRateHz / 50; // 20 ms
static constexpr size_t kDegradedWindowSamples = kSampleRateHz / 20; // 50 ms
static constexpr size_t kWarmSearchSlackSamples = 256;
static constexpr float kMinWarmWindowConfidence = 0.25f;
static constexpr float kMinDegradedWindowConfidence = 0.05f;
// HYSTERESIS (2026-05-29, shipped from the SyncV2 stabilization). The old thresholds
// (degrade after 1 miss, recover after 4) collapsed WARM on a SINGLE miss; a partial
// success flipped it back -> the WARM<->DEGRADED oscillation that stalled/killed 40KB
// Good@20 QPSK file transfers (a faded group stuck 0/6 for ~90s, or a dead transfer;
// 20KB masked it). A faded group is NORMAL — ARQ resends it and the descriptor chirp
// re-anchors every group — so a transient decode/search miss must NOT collapse warm
// timing. Tolerate sustained misses before degrading; reserve RECOVERY for genuinely
// lost sync. (40KB Good@20 3/3 seeds: thrash -> 0; dead->1260, stalled->1810, 930->1390.)
// 2/4 (not the old 1/4, not 4/8): 2 still kills the single-miss WARM<->DEGRADED bounce,
// but stays RESPONSIVE to a genuine multi-miss sync loss (and the burst-refresh resets
// the counter on every acquired group, so it rarely climbs anyway).
static constexpr int kWarmSyncMissesBeforeDegraded = 2;
static constexpr int kWarmSyncMissesBeforeRecovery = 4;

enum class WarmSyncPhase {
    COLD,
    WARM,
    DEGRADED,
    RECOVERY,
};

inline const char* warmSyncPhaseName(WarmSyncPhase phase) {
    switch (phase) {
        case WarmSyncPhase::COLD: return "COLD";
        case WarmSyncPhase::WARM: return "WARM";
        case WarmSyncPhase::DEGRADED: return "DEGRADED";
        case WarmSyncPhase::RECOVERY: return "RECOVERY";
    }
    return "UNKNOWN";
}

inline WarmSyncPhase phaseAfterSuccessfulFrame() {
    return WarmSyncPhase::WARM;
}

inline WarmSyncPhase phaseAfterSyncMiss(int consecutive_misses) {
    if (consecutive_misses >= kWarmSyncMissesBeforeRecovery) {
        return WarmSyncPhase::RECOVERY;
    }
    if (consecutive_misses >= kWarmSyncMissesBeforeDegraded) {
        return WarmSyncPhase::DEGRADED;
    }
    return WarmSyncPhase::WARM;
}

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

struct WarmSearchWindowPlan {
    bool active = false;
    bool wait_for_more_samples = false;
    bool lower_threshold = false;
    WarmSyncPhase phase = WarmSyncPhase::COLD;
    size_t search_start_abs = 0;
    size_t candidate_span_samples = 0;
    size_t search_size_samples = 0;
    size_t search_end_abs = 0;
};

inline WarmSearchWindowPlan planWarmSearchWindow(
    bool use_light_search,
    bool warm_sync_active,
    bool has_prediction,
    size_t next_expected_frame_sample,
    float frame_arrival_confidence,
    int consecutive_sync_misses,
    WarmSyncPhase warm_sync_phase,
    size_t total_fed_samples,
    size_t oldest_available_abs,
    bool search_floor_valid,
    size_t search_floor_abs,
    size_t correlation_abs,
    size_t symbol_samples,
    size_t correlation_step_samples) {

    WarmSearchWindowPlan plan;
    plan.phase = warm_sync_phase;
    const size_t safe_symbol_samples = std::max<size_t>(1, symbol_samples);
    const bool degraded = warm_sync_phase == WarmSyncPhase::DEGRADED;
    const size_t half_window_samples =
        degraded ? kDegradedWindowSamples : kDefaultTightWindowSamples;
    const float min_confidence =
        degraded ? kMinDegradedWindowConfidence : kMinWarmWindowConfidence;
    const size_t candidate_span = half_window_samples * 2 + kWarmSearchSlackSamples;
    const size_t tail_samples = safe_symbol_samples * 2;
    plan.candidate_span_samples = candidate_span;
    plan.search_start_abs = next_expected_frame_sample > half_window_samples
        ? next_expected_frame_sample - half_window_samples
        : 0;
    plan.search_size_samples = candidate_span + tail_samples;
    plan.search_end_abs = plan.search_start_abs + plan.search_size_samples;

    if (!use_light_search || !warm_sync_active || !has_prediction ||
        warm_sync_phase == WarmSyncPhase::COLD ||
        warm_sync_phase == WarmSyncPhase::RECOVERY ||
        frame_arrival_confidence < min_confidence) {
        return plan;
    }

    if (plan.search_start_abs < oldest_available_abs) {
        return plan;
    }
    if (search_floor_valid && next_expected_frame_sample < search_floor_abs) {
        return plan;
    }

    const bool current_step_intersects_window =
        correlation_abs + correlation_step_samples >= plan.search_start_abs &&
        correlation_abs <= plan.search_end_abs;
    if (!current_step_intersects_window) {
        return plan;
    }

    if (total_fed_samples < plan.search_end_abs) {
        plan.wait_for_more_samples = true;
        return plan;
    }

    plan.active = true;
    plan.lower_threshold = warm_sync_phase == WarmSyncPhase::WARM &&
        consecutive_sync_misses == 0;
    return plan;
}

}  // namespace frame_arrival_policy
}  // namespace sync
}  // namespace ultra
