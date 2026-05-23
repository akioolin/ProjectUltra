#pragma once

#include "ultra/dsp.hpp"

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
    // Optional absolute ceiling for samples admitted to the adaptive noise
    // floor. Values <= 0 disable the cap. This is separate from the bootstrap
    // ceiling so calibrated noisy-idle virtual channels can seed at low SNR
    // without ever training on a near-reference modem carrier.
    float noise_floor_estimate_rms_ceiling = 0.0f;
    bool receive_band_rms = true;
    float sample_rate_hz = 48000.0f;
    uint32_t receive_band_filter_taps = 101;
    float receive_band_low_hz = 50.0f;
    float receive_band_high_hz = 2950.0f;
    uint32_t max_wait_for_idle_ms = 15000;
};

// Ratiometric, level-independent carrier-sense calibration for HF channels.
// Every knob here is dimensionless, so the busy decision works at any absolute
// noise level / AF-gain setting:
//   - median floor (percentile 0.50) tracks the typical noise, robust to bursty
//     real-HF excursions and to a transient signal (minority of the window);
//   - busy threshold = floor x 2.0 (+6 dB);
//   - a real signal is kept out of the floor by the detector's ratiometric
//     admission gate (samples > floor x multiplier are not learned as noise), so
//     no absolute estimate ceiling is needed (0 = disabled);
//   - bootstrap admits any startup level (ceiling ~ full scale): assume idle at
//     startup / listen-before-talk, so the floor seeds to whatever the noise is.
// Shared by ModemEngine (GUI) and exercised by test_channel_busy_detector so the
// calibration cannot drift between the production path and its test.
inline ChannelBusyDetectorConfig ratiometricHfCarrierSenseConfig() {
    ChannelBusyDetectorConfig config;
    config.noise_floor_percentile = 0.50f;
    config.quiet_noise_multiplier = 2.0f;
    config.noise_floor_bootstrap_rms_ceiling = 2.0f;
    config.noise_floor_estimate_rms_ceiling = 0.0f;
    return config;
}

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
    void setNoiseFloorEstimateRmsCeiling(float ceiling);

    bool isIdle() const;
    bool isIdleFor(std::chrono::milliseconds guard) const;
    bool isIdleFor(std::chrono::milliseconds guard, TimePoint now) const;
    TimePoint lastBusyAt() const;
    std::chrono::milliseconds timeSinceQuiet(TimePoint now = Clock::now()) const;
    float currentRms() const;

    bool waitUntilIdle(std::chrono::milliseconds guard);
    bool waitUntilIdle(std::chrono::milliseconds guard,
                       std::chrono::milliseconds max_wait);
    float quietThreshold() const;

    const ChannelBusyDetectorConfig& config() const { return config_; }

private:
    void observeRmsLocked(float rms, bool local_rx_blackout, TimePoint now);
    float receiveBandRmsLocked(std::span<const float> samples);
    bool idleForLocked(TimePoint now, std::chrono::milliseconds guard) const;
    std::chrono::milliseconds requiredQuietDuration(std::chrono::milliseconds guard) const;
    TimePoint idleReadyAtLocked(std::chrono::milliseconds guard) const;
    void pruneWindowLocked(TimePoint now);
    void pruneNoiseFloorLocked(TimePoint now);
    bool hasNoiseFloorEstimateLocked() const;
    float noiseFloorEstimateLocked() const;
    float noiseFloorEstimateCeilingLocked() const;
    bool shouldRecordNoiseFloorSampleLocked(float rms) const;
    float quietThresholdLocked() const;

    ChannelBusyDetectorConfig config_;
    FIRFilter receive_band_filter_;

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
