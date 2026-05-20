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

void observe(ChannelBusyDetector& detector,
             float rms,
             int ms,
             bool local_tx = false) {
    static const Clock::time_point base = Clock::now();
    detector.observeRms(rms, local_tx, base + std::chrono::milliseconds(ms));
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
    assert(!detector.isIdle());

    observe(detector, 0.000f, 10, false);
    observe(detector, 0.000f, 20, false);
    observe(detector, 0.000f, 30, false);
    assert(!detector.isIdle());

    observe(detector, 0.000f, 40, false);
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
    testWaitUntilIdleTimeoutAndGuard();

    std::cout << "ChannelBusyDetector tests passed\n";
    return 0;
}
