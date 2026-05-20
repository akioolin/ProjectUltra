#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <utility>

namespace ultra::audio {

struct ChannelBusyDetectorConfig {
    float quiet_rms_threshold = 0.005f;
    uint32_t quiet_hold_ms = 30;
    uint32_t rms_window_ms = 30;
    bool adaptive_noise_floor = true;
    uint32_t noise_floor_window_ms = 5000;
    uint32_t min_noise_floor_observations = 10;
    float noise_floor_percentile = 0.10f;
    float quiet_noise_multiplier = 1.5f;
    float noise_floor_bootstrap_rms_ceiling = 0.11f;
    uint32_t max_wait_for_idle_ms = 15000;
};

class ChannelBusyDetector {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit ChannelBusyDetector(ChannelBusyDetectorConfig config = {});

    void reset(TimePoint now = Clock::now());

    void observeSamples(std::span<const float> samples,
                        bool local_rx_blackout,
                        TimePoint now = Clock::now());
    void observeRms(float rms,
                    bool local_rx_blackout,
                    TimePoint now = Clock::now());

    bool isIdle() const;
    bool isIdleFor(std::chrono::milliseconds guard) const;
    TimePoint lastBusyAt() const;
    std::chrono::milliseconds timeSinceQuiet(TimePoint now = Clock::now()) const;
    float currentRms() const;

    bool waitUntilIdle(std::chrono::milliseconds guard);
    bool waitUntilIdle(std::chrono::milliseconds guard,
                       std::chrono::milliseconds max_wait);
    float quietThreshold() const;

    const ChannelBusyDetectorConfig& config() const { return config_; }

private:
    bool idleForLocked(TimePoint now, std::chrono::milliseconds guard) const;
    std::chrono::milliseconds requiredQuietDuration(std::chrono::milliseconds guard) const;
    TimePoint idleReadyAtLocked(std::chrono::milliseconds guard) const;
    void pruneWindowLocked(TimePoint now);
    void pruneNoiseFloorLocked(TimePoint now);
    bool hasNoiseFloorEstimateLocked() const;
    float noiseFloorEstimateLocked() const;
    bool shouldRecordNoiseFloorSampleLocked(float rms) const;
    float quietThresholdLocked() const;

    ChannelBusyDetectorConfig config_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<TimePoint, float>> rms_window_;
    std::deque<std::pair<TimePoint, float>> noise_floor_window_;
    double rms_window_sum_ = 0.0;
    float current_rms_ = 0.0f;
    TimePoint last_busy_at_{};
    TimePoint quiet_since_{};
};

}  // namespace ultra::audio
