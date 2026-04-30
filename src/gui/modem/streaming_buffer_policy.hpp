#pragma once

#include <algorithm>
#include <cstddef>

namespace ultra {
namespace gui {
namespace streaming_buffer_policy {

inline size_t ringDistance(size_t from, size_t to, size_t capacity) {
    if (capacity == 0) return 0;
    from %= capacity;
    to %= capacity;
    return (to >= from) ? (to - from) : (capacity - from + to);
}

struct OverflowDecision {
    size_t initial_unsearched = 0;
    size_t final_unsearched = 0;
    size_t samples_to_drop = 0;
    size_t new_correlation_pos = 0;
    size_t target_after_write = 0;
    bool pointer_drift_detected = false;
    bool overflow = false;
};

inline OverflowDecision planOverflowRecovery(size_t write_pos,
                                             size_t correlation_pos,
                                             size_t total_fed,
                                             size_t incoming_count,
                                             size_t capacity,
                                             size_t invariant_guard,
                                             size_t recovery_keep) {
    OverflowDecision decision;
    if (capacity == 0) {
        return decision;
    }

    write_pos %= capacity;
    correlation_pos %= capacity;
    decision.new_correlation_pos = correlation_pos;
    decision.initial_unsearched = ringDistance(correlation_pos, write_pos, capacity);
    decision.final_unsearched = decision.initial_unsearched;

    if (total_fed >= capacity &&
        decision.final_unsearched > (capacity - std::min(invariant_guard, capacity))) {
        decision.pointer_drift_detected = true;
        decision.new_correlation_pos = write_pos;
        decision.final_unsearched = 0;
    }

    if (decision.final_unsearched + incoming_count >= capacity) {
        decision.overflow = true;
        const size_t incoming_total = decision.final_unsearched + incoming_count;
        decision.target_after_write = std::min(recovery_keep, capacity - 1);
        if (incoming_total > decision.target_after_write) {
            decision.samples_to_drop = std::min(decision.final_unsearched,
                                                incoming_total - decision.target_after_write);
        }
        decision.new_correlation_pos =
            (decision.new_correlation_pos + decision.samples_to_drop) % capacity;
        decision.final_unsearched -= decision.samples_to_drop;
    }

    return decision;
}

struct BacklogSnapshot {
    size_t unsearched_samples = 0;
    size_t used_samples = 0;
    float backlog_ms = 0.0f;
    float fill_percent = 0.0f;
};

inline BacklogSnapshot computeBacklog(size_t write_pos,
                                      size_t correlation_pos,
                                      size_t total_fed,
                                      size_t capacity,
                                      float sample_rate_hz) {
    BacklogSnapshot snapshot;
    if (capacity == 0 || sample_rate_hz <= 0.0f) {
        return snapshot;
    }

    snapshot.unsearched_samples = ringDistance(correlation_pos, write_pos, capacity);
    snapshot.used_samples = std::min(total_fed, capacity);
    snapshot.backlog_ms = (static_cast<float>(snapshot.unsearched_samples) * 1000.0f) /
                          sample_rate_hz;
    snapshot.fill_percent = 100.0f * static_cast<float>(snapshot.used_samples) /
                            static_cast<float>(capacity);
    return snapshot;
}

}  // namespace streaming_buffer_policy
}  // namespace gui
}  // namespace ultra
