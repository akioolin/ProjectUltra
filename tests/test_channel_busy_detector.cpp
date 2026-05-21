#include "audio/channel_busy_detector.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

using ultra::audio::ChannelBusyDetector;
using ultra::audio::ChannelBusyDetectorConfig;

namespace {

using Clock = ChannelBusyDetector::Clock;

ChannelBusyDetectorConfig testConfig() {
    ChannelBusyDetectorConfig config;
    config.quiet_rms_threshold = 0.005f;
    config.quiet_hold_ms = 30;
    config.rms_window_ms = 30;
    config.max_wait_for_idle_ms = 50;
    return config;
}

Clock::time_point syntheticBase() {
    static const Clock::time_point base = Clock::now() - std::chrono::seconds(10);
    return base;
}

Clock::time_point atMs(int ms) {
    return syntheticBase() + std::chrono::milliseconds(ms);
}

void observe(ChannelBusyDetector& detector,
             float rms,
             int ms,
             bool local_tx = false) {
    detector.observeRms(rms, local_tx, atMs(ms));
}

bool isIdleAtMs(const ChannelBusyDetector& detector, int ms) {
    return detector.isIdleFor(std::chrono::milliseconds(0), atMs(ms));
}

void testInitialStateIsIdle() {
    ChannelBusyDetector detector(testConfig());
    assert(detector.isIdle());
    assert(detector.isIdleFor(std::chrono::milliseconds(50)));
}

void testBusyThenQuietHold() {
    ChannelBusyDetector detector(testConfig());

    observe(detector, 0.100f, 0);
    assert(!detector.isIdle());

    observe(detector, 0.000f, 10);
    assert(!detector.isIdle());
    observe(detector, 0.000f, 20);
    assert(!detector.isIdle());
    observe(detector, 0.000f, 30);
    assert(!detector.isIdle());

    observe(detector, 0.000f, 40);
    assert(detector.timeSinceQuiet(Clock::now()).count() >= 0);
    assert(detector.isIdle());
}

void testMovingWindowAbsorbsShortDropout() {
    ChannelBusyDetector detector(testConfig());

    observe(detector, 0.080f, 0);
    observe(detector, 0.080f, 10);
    observe(detector, 0.000f, 20);
    observe(detector, 0.080f, 30);
    assert(!detector.isIdle());

    observe(detector, 0.000f, 70);
    observe(detector, 0.000f, 80);
    observe(detector, 0.000f, 90);
    observe(detector, 0.000f, 100);
    assert(detector.isIdle());
}

void testLocalTxBlackoutIsBusyRegardlessOfRms() {
    ChannelBusyDetector detector(testConfig());

    observe(detector, 0.000f, 0, true);
    assert(!isIdleAtMs(detector, 0));

    observe(detector, 0.000f, 10, false);
    observe(detector, 0.000f, 20, false);
    observe(detector, 0.000f, 30, false);
    assert(!isIdleAtMs(detector, 30));

    observe(detector, 0.000f, 40, false);
    assert(isIdleAtMs(detector, 40));
}

void testAdaptiveNoiseFloorTreatsAwgnFloorAsIdle() {
    ChannelBusyDetectorConfig config = testConfig();
    config.min_noise_floor_observations = 5;
    ChannelBusyDetector detector(config);

    for (int i = 0; i < 10; ++i) {
        observe(detector, 0.090f, i * 10);
    }
    assert(detector.isIdle());

    observe(detector, 0.360f, 200);
    observe(detector, 0.360f, 210);
    observe(detector, 0.360f, 220);
    assert(!detector.isIdle());

    for (int i = 0; i < 5; ++i) {
        observe(detector, 0.090f, 300 + i * 10);
    }
    assert(detector.isIdle());
}

void testAdaptiveThresholdRejectsFadedCarrierDips() {
    ChannelBusyDetectorConfig config = testConfig();
    config.min_noise_floor_observations = 5;
    ChannelBusyDetector detector(config);

    for (int i = 0; i < 20; ++i) {
        observe(detector, 0.050f, i * 10);
    }
    assert(detector.isIdle());

    observe(detector, 0.220f, 300);
    observe(detector, 0.180f, 310);
    observe(detector, 0.120f, 320);
    observe(detector, 0.095f, 330);
    observe(detector, 0.090f, 340);
    observe(detector, 0.105f, 350);
    observe(detector, 0.130f, 360);
    assert(!detector.isIdle());
    assert(!detector.isIdleFor(std::chrono::milliseconds(50)));
}

void testSignalDominatedHistoryDoesNotRaiseNoiseFloor() {
    ChannelBusyDetectorConfig config = testConfig();
    config.min_noise_floor_observations = 5;
    config.noise_floor_window_ms = 5000;
    ChannelBusyDetector detector(config);

    for (int i = 0; i < 20; ++i) {
        observe(detector, 0.060f, i * 10);
    }

    const float seeded_threshold = detector.quietThreshold();
    assert(seeded_threshold > 0.085f);
    assert(seeded_threshold < 0.100f);
    assert(detector.isIdle());

    for (int i = 0; i < 650; ++i) {
        const float signal_rms =
            (i % 3 == 0) ? 0.180f : ((i % 3 == 1) ? 0.260f : 0.300f);
        observe(detector, signal_rms, 500 + i * 10);
        assert(detector.quietThreshold() < 0.120f);
    }

    assert(!detector.isIdle());
    observe(detector, 0.181f, 7100);
    assert(!detector.isIdle());

    for (int i = 0; i < 10; ++i) {
        observe(detector, 0.060f, 8000 + i * 10);
    }

    const float recovered_threshold = detector.quietThreshold();
    assert(recovered_threshold > 0.085f);
    assert(recovered_threshold < 0.100f);
    assert(detector.isIdle());
}

void testWaitUntilIdleTimeoutAndGuard() {
    ChannelBusyDetector detector(testConfig());
    observe(detector, 0.050f, 0);

    const auto start = Clock::now();
    const bool timed_out = detector.waitUntilIdle(
        std::chrono::milliseconds(10),
        std::chrono::milliseconds(5));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start);
    assert(!timed_out);
    assert(elapsed >= std::chrono::milliseconds(4));

    detector.reset();
    assert(detector.waitUntilIdle(std::chrono::milliseconds(10),
                                  std::chrono::milliseconds(20)));
}

}  // namespace

int main() {
    testInitialStateIsIdle();
    testBusyThenQuietHold();
    testMovingWindowAbsorbsShortDropout();
    testLocalTxBlackoutIsBusyRegardlessOfRms();
    testAdaptiveNoiseFloorTreatsAwgnFloorAsIdle();
    testAdaptiveThresholdRejectsFadedCarrierDips();
    testSignalDominatedHistoryDoesNotRaiseNoiseFloor();
    testWaitUntilIdleTimeoutAndGuard();

    std::cout << "ChannelBusyDetector tests passed\n";
    return 0;
}
