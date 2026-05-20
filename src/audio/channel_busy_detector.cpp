#include "audio/channel_busy_detector.hpp"

#include "ultra/logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <vector>

namespace ultra::audio {

namespace {

// ULTRA_CARRIER_SENSE_DEBUG=1 enables per-event logging at WARN level
// for state transitions and waits. Off by default (production silent).
// Matches the pattern of ULTRA_HARQ_DEBUG_LOG / ULTRA_CFO_DEBUG_LOG.
bool csDebugEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("ULTRA_CARRIER_SENSE_DEBUG");
        return env && env[0] != '\0' && !(env[0] == '0' && env[1] == '\0');
    }();
    return enabled;
}

float rmsOf(std::span<const float> samples) {
    if (samples.empty()) {
        return 0.0f;
    }

    double sum_sq = 0.0;
    for (float sample : samples) {
        sum_sq += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return static_cast<float>(std::sqrt(sum_sq / static_cast<double>(samples.size())));
}

}  // namespace

ChannelBusyDetector::ChannelBusyDetector(ChannelBusyDetectorConfig config)
    : config_(config) {
    reset();
}

void ChannelBusyDetector::reset(TimePoint now) {
    std::lock_guard<std::mutex> lock(mutex_);
    rms_window_.clear();
    noise_floor_window_.clear();
    rms_window_sum_ = 0.0;
    current_rms_ = 0.0f;
    const auto initial_quiet_age =
        std::chrono::hours(24) +
        std::chrono::milliseconds(config_.quiet_hold_ms + config_.max_wait_for_idle_ms);
    quiet_since_ = now - initial_quiet_age;
    last_busy_at_ = quiet_since_;
    cv_.notify_all();
}

void ChannelBusyDetector::observeSamples(std::span<const float> samples,
                                         bool local_rx_blackout,
                                         TimePoint now) {
    observeRms(rmsOf(samples), local_rx_blackout, now);
}

void ChannelBusyDetector::observeRms(float rms,
                                     bool local_rx_blackout,
                                     TimePoint now) {
    std::lock_guard<std::mutex> lock(mutex_);

    current_rms_ = std::max(0.0f, rms);
    const bool was_quiet = (quiet_since_ != TimePoint{});

    if (local_rx_blackout) {
        rms_window_.clear();
        rms_window_sum_ = 0.0;
        last_busy_at_ = now;
        quiet_since_ = TimePoint{};
        cv_.notify_all();
        if (was_quiet && csDebugEnabled()) {
            LOG_MODEM(WARN, "CS quiet->busy (local TX blackout) rms=%.4f", current_rms_);
        }
        return;
    }

    rms_window_.emplace_back(now, current_rms_);
    rms_window_sum_ += current_rms_;
    noise_floor_window_.emplace_back(now, current_rms_);
    pruneWindowLocked(now);
    pruneNoiseFloorLocked(now);

    const float window_rms = rms_window_.empty()
        ? current_rms_
        : static_cast<float>(rms_window_sum_ / static_cast<double>(rms_window_.size()));
    const float threshold = quietThresholdLocked();

    if (window_rms > threshold) {
        last_busy_at_ = now;
        quiet_since_ = TimePoint{};
        cv_.notify_all();
        if (was_quiet && csDebugEnabled()) {
            LOG_MODEM(WARN, "CS quiet->busy window_rms=%.4f thresh=%.4f",
                      window_rms, threshold);
        }
        return;
    }

    if (quiet_since_ == TimePoint{}) {
        quiet_since_ = now;
        if (csDebugEnabled()) {
            LOG_MODEM(WARN, "CS busy->quiet window_rms=%.4f thresh=%.4f",
                      window_rms, threshold);
        }
    }
    cv_.notify_all();
}

bool ChannelBusyDetector::isIdle() const {
    return isIdleFor(std::chrono::milliseconds(0));
}

bool ChannelBusyDetector::isIdleFor(std::chrono::milliseconds guard) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return idleForLocked(Clock::now(), guard);
}

ChannelBusyDetector::TimePoint ChannelBusyDetector::lastBusyAt() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_busy_at_;
}

std::chrono::milliseconds ChannelBusyDetector::timeSinceQuiet(TimePoint now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (quiet_since_ == TimePoint{} || now < quiet_since_) {
        return std::chrono::milliseconds(0);
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - quiet_since_);
}

float ChannelBusyDetector::currentRms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_rms_;
}

bool ChannelBusyDetector::waitUntilIdle(std::chrono::milliseconds guard) {
    return waitUntilIdle(
        guard,
        std::chrono::milliseconds(std::max<uint32_t>(1, config_.max_wait_for_idle_ms)));
}

bool ChannelBusyDetector::waitUntilIdle(std::chrono::milliseconds guard,
                                        std::chrono::milliseconds max_wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    const TimePoint start = Clock::now();
    const TimePoint deadline = start + max_wait;
    const bool debug = csDebugEnabled();
    const bool was_idle = idleForLocked(start, guard);

    if (debug && !was_idle) {
        LOG_MODEM(WARN, "CS wait_until_idle: blocking guard=%dms max_wait=%dms rms=%.4f",
                  static_cast<int>(guard.count()),
                  static_cast<int>(max_wait.count()),
                  current_rms_);
    }

    while (!idleForLocked(Clock::now(), guard)) {
        const TimePoint ready_at = idleReadyAtLocked(guard);
        const TimePoint wait_until = ready_at == TimePoint{}
            ? deadline
            : std::min(ready_at, deadline);

        if (cv_.wait_until(lock, wait_until) == std::cv_status::timeout &&
            Clock::now() >= deadline) {
            const bool final_idle = idleForLocked(Clock::now(), guard);
            if (debug) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - start).count();
                LOG_MODEM(WARN, "CS wait_until_idle: timeout after %dms (idle=%d)",
                          static_cast<int>(elapsed), final_idle ? 1 : 0);
            }
            return final_idle;
        }
    }

    if (debug && !was_idle) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start).count();
        LOG_MODEM(WARN, "CS wait_until_idle: released after %dms", static_cast<int>(elapsed));
    }
    return true;
}

bool ChannelBusyDetector::idleForLocked(TimePoint now,
                                        std::chrono::milliseconds guard) const {
    if (quiet_since_ == TimePoint{} || now < quiet_since_) {
        return false;
    }
    return now - quiet_since_ >= requiredQuietDuration(guard);
}

std::chrono::milliseconds ChannelBusyDetector::requiredQuietDuration(
    std::chrono::milliseconds guard) const {
    return std::chrono::milliseconds(config_.quiet_hold_ms) + std::max(guard, std::chrono::milliseconds(0));
}

ChannelBusyDetector::TimePoint ChannelBusyDetector::idleReadyAtLocked(
    std::chrono::milliseconds guard) const {
    if (quiet_since_ == TimePoint{}) {
        return TimePoint{};
    }
    return quiet_since_ + requiredQuietDuration(guard);
}

void ChannelBusyDetector::pruneWindowLocked(TimePoint now) {
    const auto window = std::chrono::milliseconds(std::max<uint32_t>(1, config_.rms_window_ms));
    while (!rms_window_.empty() && now - rms_window_.front().first > window) {
        rms_window_sum_ -= rms_window_.front().second;
        rms_window_.pop_front();
    }

    if (rms_window_.empty()) {
        rms_window_sum_ = 0.0;
    }
}

void ChannelBusyDetector::pruneNoiseFloorLocked(TimePoint now) {
    const auto window =
        std::chrono::milliseconds(std::max<uint32_t>(1, config_.noise_floor_window_ms));
    while (!noise_floor_window_.empty() && now - noise_floor_window_.front().first > window) {
        noise_floor_window_.pop_front();
    }
}

float ChannelBusyDetector::quietThresholdLocked() const {
    float threshold = std::max(0.0f, config_.quiet_rms_threshold);
    if (!config_.adaptive_noise_floor ||
        noise_floor_window_.size() < config_.min_noise_floor_observations) {
        return threshold;
    }

    std::vector<float> values;
    values.reserve(noise_floor_window_.size());
    for (const auto& entry : noise_floor_window_) {
        values.push_back(entry.second);
    }

    const float percentile = std::clamp(config_.noise_floor_percentile, 0.0f, 1.0f);
    const size_t index = std::min(
        values.size() - 1,
        static_cast<size_t>(percentile * static_cast<float>(values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index),
                     values.end());

    const float noise_floor = std::max(0.0f, values[index]);
    const float adaptive_threshold =
        noise_floor * std::max(1.0f, config_.quiet_noise_multiplier);
    return std::max(threshold, adaptive_threshold);
}

}  // namespace ultra::audio
