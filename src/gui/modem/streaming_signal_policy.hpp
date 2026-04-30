#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ultra {
namespace gui {
namespace streaming_signal_policy {

inline constexpr float kMinLLRForSingleCWDecode = 1.5f;
inline constexpr float kMinLLRForEscalation = 1.5f;
inline constexpr float kMinPreSyncLLR = 1.5f;
inline constexpr float kNearZeroLLR = 0.1f;
inline constexpr float kMaxErasureLikeFraction = 0.30f;
inline constexpr float kMinLTSSignalPower = 1.0e-3f;
inline constexpr float kMinLTSChannelMagnitude = 5.0e-2f;
inline constexpr float kMaxSyncCFODriftHz = 1.0f;
inline constexpr float kKnownCFOEpsilonHz = 0.01f;

inline float meanAbsLLR(const float* bits, size_t count) {
    if (!bits || count == 0) {
        return 0.0f;
    }

    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        sum += std::abs(bits[i]);
    }
    return sum / static_cast<float>(count);
}

struct LLRQuality {
    size_t count = 0;
    size_t near_zero_count = 0;
    float mean_abs = 0.0f;
    float near_zero_fraction = 1.0f;
    bool reject_as_false_lock = true;
};

inline LLRQuality evaluatePreSyncLLR(const float* bits,
                                     size_t available_count,
                                     size_t max_count,
                                     float min_mean_abs = kMinPreSyncLLR,
                                     float near_zero_threshold = kNearZeroLLR,
                                     float max_near_zero_fraction = kMaxErasureLikeFraction) {
    LLRQuality quality;
    quality.count = std::min(available_count, max_count);
    if (!bits || quality.count == 0) {
        return quality;
    }

    float sum = 0.0f;
    for (size_t i = 0; i < quality.count; ++i) {
        const float llr_abs = std::abs(bits[i]);
        sum += llr_abs;
        if (llr_abs <= near_zero_threshold) {
            ++quality.near_zero_count;
        }
    }

    quality.mean_abs = sum / static_cast<float>(quality.count);
    quality.near_zero_fraction = static_cast<float>(quality.near_zero_count) /
                                 static_cast<float>(quality.count);
    quality.reject_as_false_lock = quality.mean_abs < min_mean_abs ||
                                   quality.near_zero_fraction > max_near_zero_fraction;
    return quality;
}

inline bool invalidOFDMLTSTraining(bool is_ofdm,
                                   bool connected,
                                   float lts_signal_power,
                                   float lts_channel_mag) {
    if (!is_ofdm || !connected) {
        return false;
    }

    if (!std::isfinite(lts_signal_power) || !std::isfinite(lts_channel_mag)) {
        return true;
    }

    return lts_signal_power < kMinLTSSignalPower &&
           lts_channel_mag < kMinLTSChannelMagnitude;
}

struct LightSyncThresholds {
    float min_confidence = 0.65f;
    float weak_floor = 0.55f;
};

inline LightSyncThresholds lightSyncThresholds(bool is_coherent,
                                               bool is_narrowband,
                                               bool connected,
                                               uint64_t sync_reject_streak) {
    LightSyncThresholds thresholds;
    if (is_coherent) {
        thresholds.min_confidence = 0.90f;
        thresholds.weak_floor = 0.85f;
    } else if (is_narrowband) {
        thresholds.min_confidence = 0.50f;
        thresholds.weak_floor = 0.40f;
    } else if (connected) {
        thresholds.min_confidence = 0.52f;
        thresholds.weak_floor = 0.45f;
    }

    if (!is_coherent && connected && sync_reject_streak >= 5) {
        const float extra_relax = std::min(
            0.10f, 0.02f * static_cast<float>(sync_reject_streak - 4));
        thresholds.min_confidence = std::max(0.45f, thresholds.min_confidence - extra_relax);
    }

    return thresholds;
}

struct LightSyncDecision {
    bool found = false;
    bool weak_accept = false;
    bool rejected = false;
    uint64_t next_reject_streak = 0;
};

inline LightSyncDecision evaluateLightSyncCandidate(bool found,
                                                    float correlation,
                                                    bool is_coherent,
                                                    bool connected,
                                                    uint64_t sync_reject_streak,
                                                    LightSyncThresholds thresholds) {
    LightSyncDecision decision;
    decision.found = found;
    decision.next_reject_streak = sync_reject_streak;

    if (!found) {
        return decision;
    }

    if (correlation < thresholds.min_confidence) {
        const bool allow_weak_accept =
            !is_coherent &&
            connected &&
            sync_reject_streak >= 3 &&
            correlation >= std::max(thresholds.weak_floor,
                                    thresholds.min_confidence - 0.08f);

        if (!allow_weak_accept) {
            decision.found = false;
            decision.rejected = true;
            decision.next_reject_streak = sync_reject_streak + 1;
            return decision;
        }

        decision.weak_accept = true;
        decision.next_reject_streak = std::max<uint64_t>(1, sync_reject_streak / 2);
        return decision;
    }

    decision.next_reject_streak = 0;
    return decision;
}

struct CFODriftDecision {
    float accepted_cfo = 0.0f;
    float diff_hz = 0.0f;
    bool clamped = false;
};

inline CFODriftDecision limitConnectedCFODrift(bool connected,
                                               float measured_cfo,
                                               float known_cfo,
                                               float max_drift_hz = kMaxSyncCFODriftHz,
                                               float known_cfo_epsilon_hz = kKnownCFOEpsilonHz) {
    CFODriftDecision decision;
    decision.accepted_cfo = measured_cfo;
    decision.diff_hz = measured_cfo - known_cfo;

    if (connected &&
        std::abs(known_cfo) > known_cfo_epsilon_hz &&
        std::abs(decision.diff_hz) > max_drift_hz) {
        decision.accepted_cfo = known_cfo;
        decision.clamped = true;
    }

    return decision;
}

}  // namespace streaming_signal_policy
}  // namespace gui
}  // namespace ultra
