#include "audio/channel_busy_detector.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ultra::audio {

namespace {

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

    if (local_rx_blackout) {
        rms_window_.clear();
        rms_window_sum_ = 0.0;
        last_busy_at_ = now;
        quiet_since_ = TimePoint{};
        cv_.notify_all();
        return;
    }

    rms_window_.emplace_back(now, current_rms_);
    rms_window_sum_ += current_rms_;
    pruneWindowLocked(now);

    const float window_rms = rms_window_.empty()
        ? current_rms_
        : static_cast<float>(rms_window_sum_ / static_cast<double>(rms_window_.size()));

    if (window_rms > config_.quiet_rms_threshold) {
        last_busy_at_ = now;
        quiet_since_ = TimePoint{};
        cv_.notify_all();
        return;
    }

    if (quiet_since_ == TimePoint{}) {
        quiet_since_ = now;
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
    const TimePoint deadline = Clock::now() + max_wait;

    while (!idleForLocked(Clock::now(), guard)) {
        const TimePoint ready_at = idleReadyAtLocked(guard);
        const TimePoint wait_until = ready_at == TimePoint{}
            ? deadline
            : std::min(ready_at, deadline);

        if (cv_.wait_until(lock, wait_until) == std::cv_status::timeout &&
            Clock::now() >= deadline) {
            return idleForLocked(Clock::now(), guard);
        }
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

}  // namespace ultra::audio
