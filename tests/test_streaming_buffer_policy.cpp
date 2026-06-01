#include "gui/modem/streaming_buffer_policy.hpp"
#include "sync/frame_arrival_policy.hpp"

#include <cmath>
#include <iostream>

using namespace ultra::gui::streaming_buffer_policy;
namespace arrival_policy = ultra::sync::frame_arrival_policy;

namespace {

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg) \
    do { \
        ++tests_run; \
        if (!(cond)) { \
            ++tests_failed; \
            std::cout << "FAIL: " << msg << "\n"; \
            return; \
        } \
    } while (0)

void test_ring_distance() {
    CHECK(ringDistance(10, 40, 100) == 30, "ring distance without wrap");
    CHECK(ringDistance(90, 10, 100) == 20, "ring distance with wrap");
    CHECK(ringDistance(10, 10, 100) == 0, "ring distance same position");
    CHECK(ringDistance(110, 240, 100) == 30, "ring distance should normalize inputs");
    CHECK(ringDistance(10, 40, 0) == 0, "zero-capacity ring should be safe");
}

void test_no_overflow() {
    auto d = planOverflowRecovery(
        4000, 1000, 4000, 1000,
        10000, 1000, 2500);

    CHECK(!d.pointer_drift_detected, "normal buffer should not detect drift");
    CHECK(!d.overflow, "normal buffer should not overflow");
    CHECK(d.initial_unsearched == 3000, "normal unsearched count");
    CHECK(d.final_unsearched == 3000, "normal final unsearched count");
    CHECK(d.samples_to_drop == 0, "normal path should not drop");
    CHECK(d.new_correlation_pos == 1000, "normal path should keep correlation position");
}

void test_overflow_drop_without_wrap() {
    auto d = planOverflowRecovery(
        8000, 1000, 8000, 3000,
        10000, 1000, 2500);

    CHECK(d.overflow, "overflow should be detected");
    CHECK(!d.pointer_drift_detected, "ordinary overflow is not pointer drift");
    CHECK(d.initial_unsearched == 7000, "overflow initial unsearched");
    CHECK(d.samples_to_drop == 7000, "overflow should drop available backlog when target needs more");
    CHECK(d.final_unsearched == 0, "helper reports post-drop unsearched before write");
    CHECK(d.new_correlation_pos == 8000, "correlation position should advance by dropped samples");
    CHECK(d.target_after_write == 2500, "recovery target should respect configured keep size");
}

void test_overflow_drop_with_wrap() {
    auto d = planOverflowRecovery(
        6000, 8000, 16000, 2500,
        10000, 1000, 3000);

    CHECK(d.overflow, "wrapped overflow should be detected");
    CHECK(d.initial_unsearched == 8000, "wrapped unsearched count");
    CHECK(d.samples_to_drop == 7500, "wrapped overflow drop count");
    CHECK(d.new_correlation_pos == 5500, "wrapped correlation position should wrap");
}

void test_pointer_drift_guard() {
    auto d = planOverflowRecovery(
        1000, 1100, 12000, 100,
        10000, 1000, 3000);

    CHECK(d.pointer_drift_detected, "near-full unsearched after wrap should be treated as pointer drift");
    CHECK(!d.overflow, "small incoming chunk after drift reset should not overflow");
    CHECK(d.initial_unsearched == 9900, "drift initial unsearched count");
    CHECK(d.final_unsearched == 0, "drift reset should clear unsearched backlog");
    CHECK(d.new_correlation_pos == 1000, "drift reset should snap to write position");
}

void test_backlog_snapshot() {
    auto b = computeBacklog(9600, 0, 9600, 480000, 48000.0f);
    CHECK(b.unsearched_samples == 9600, "backlog unsearched samples");
    CHECK(b.used_samples == 9600, "backlog used samples before buffer fills");
    CHECK(std::abs(b.backlog_ms - 200.0f) < 0.001f, "backlog milliseconds");
    CHECK(std::abs(b.fill_percent - 2.0f) < 0.001f, "buffer fill percent");

    auto wrapped = computeBacklog(1000, 470000, 481000, 480000, 48000.0f);
    CHECK(wrapped.unsearched_samples == 11000, "wrapped backlog unsearched samples");
    CHECK(wrapped.used_samples == 480000, "used samples should clamp at capacity");

    auto invalid = computeBacklog(1000, 0, 1000, 0, 48000.0f);
    CHECK(invalid.unsearched_samples == 0, "invalid capacity should be safe");
}

void test_frame_arrival_prediction_tracks_cadence() {
    constexpr size_t tight_window = 960;  // 20 ms at 48 kHz
    constexpr size_t frame_samples = 11520;
    constexpr size_t gap_samples = 480;   // 10 ms

    bool have_prediction = false;
    size_t expected = 0;
    float confidence = 0.0f;
    int misses = 0;
    size_t actual_start = 240000;

    for (int i = 0; i < 50; ++i) {
        if (have_prediction) {
            const int jitter = ((i % 5) - 2) * 120;  // +/-2.5 ms max
            actual_start = jitter >= 0
                ? expected + static_cast<size_t>(jitter)
                : expected - static_cast<size_t>(-jitter);
        }

        const auto update = arrival_policy::updateOnSuccessfulFrame(
            have_prediction, expected, confidence,
            actual_start, actual_start + frame_samples,
            gap_samples, tight_window);

        CHECK(update.within_tight_window, "predicted frame arrival should stay inside 20 ms window");
        if (update.has_arrival_error) {
            CHECK(arrival_policy::absSampleError(update.arrival_error_samples) <= tight_window,
                  "arrival error should remain bounded by tight window");
        }
        CHECK(update.next_expected_frame_sample == actual_start + frame_samples + gap_samples,
              "next prediction should be last frame end plus configured gap");
        CHECK(update.consecutive_sync_misses == 0,
              "successful frame should clear consecutive sync misses");

        have_prediction = true;
        expected = update.next_expected_frame_sample;
        confidence = update.confidence;
        misses = update.consecutive_sync_misses;
    }

    CHECK(misses == 0, "sequential successes should not accumulate misses");
    CHECK(confidence > 0.75f, "arrival confidence should build over stable cadence");
}

void test_frame_arrival_miss_accounting() {
    int misses = 0;
    misses = arrival_policy::incrementSyncMisses(misses);
    misses = arrival_policy::incrementSyncMisses(misses);
    CHECK(misses == 2, "sync miss counter should increment");

    const float confidence = arrival_policy::confidenceAfterSyncMiss(0.8f);
    CHECK(confidence < 0.8f && confidence > 0.0f,
          "sync miss should reduce but not zero arrival confidence");
}

void test_warm_search_window_planning() {
    constexpr size_t symbol_samples = 1152;
    constexpr size_t step_samples = 4800;
    constexpr size_t expected = 240000;
    constexpr size_t half_window = 960;

    auto active = arrival_policy::planWarmSearchWindow(
        true, true, true, expected, 0.6f, 0,        expected + 6000, expected - 20000,
        true, expected - 4000,
        expected - 1200,
        symbol_samples, step_samples);

    CHECK(active.active, "warm window should activate when prediction is available and current step intersects it");
    CHECK(active.lower_threshold, "warm window should use lowered threshold");
    CHECK(active.search_start_abs == expected - half_window,
          "warm window should start half-window before expected arrival");
    CHECK(active.candidate_span_samples ==
              half_window * 2 + arrival_policy::kWarmSearchSlackSamples,
          "warm window candidate span should be independent of LTS tail samples");
    CHECK(active.search_size_samples ==
              half_window * 2 + arrival_policy::kWarmSearchSlackSamples + symbol_samples * 2,
          "warm window should include candidate span plus two LTS symbols");

    auto wait = arrival_policy::planWarmSearchWindow(
        true, true, true, expected, 0.6f, 0,        expected + 1000, expected - 20000,
        true, expected - 4000,
        expected - 1200,
        symbol_samples, step_samples);
    CHECK(!wait.active && wait.wait_for_more_samples,
          "warm window should wait until enough post-LTS samples are buffered");

    // §7 collapse: DEGRADED is derived from misses>=kWarmSyncMissesBeforeDegraded (2), not a
    // phase arg — so drive it with misses=2 (the old call forced phase=DEGRADED at misses=1).
    auto degraded = arrival_policy::planWarmSearchWindow(
        true, true, true, expected, 0.2f, 2,
        expected + 6000, expected - 20000,
        true, expected - 4000,
        expected - 1200,
        symbol_samples, step_samples);
    CHECK(degraded.active && !degraded.lower_threshold,
          "degraded warm-sync should keep timed search at low confidence without lowered threshold");
    CHECK(degraded.candidate_span_samples ==
              arrival_policy::kDegradedWindowSamples * 2 +
              arrival_policy::kWarmSearchSlackSamples,
          "degraded warm-sync should widen the timed window");

    // §7 collapse: RECOVERY ⟺ !warm_sync_active (the controller clears active at misses>=4),
    // so drive it with active=false — the old call forced phase=RECOVERY while passing active=true.
    auto recovery = arrival_policy::planWarmSearchWindow(
        true, false, true, expected, 0.2f, 4,
        expected + 6000, expected - 20000,
        true, expected - 4000,
        expected - 1200,
        symbol_samples, step_samples);
    CHECK(!recovery.active && !recovery.wait_for_more_samples,
          "recovery state should fall back to cold wide search");

    auto far = arrival_policy::planWarmSearchWindow(
        true, true, true, expected, 0.6f, 0,        expected + 6000, expected - 20000,
        true, expected - 4000,
        expected + 12000,
        symbol_samples, step_samples);
    CHECK(!far.active && !far.wait_for_more_samples,
          "warm window should not activate when the current search step is outside the expected window");
}

void test_warm_sync_phase_transitions() {
    CHECK(arrival_policy::phaseAfterSuccessfulFrame() == arrival_policy::WarmSyncPhase::WARM,
          "successful frame should enter warm sync");
    CHECK(arrival_policy::phaseAfterSyncMiss(1) == arrival_policy::WarmSyncPhase::DEGRADED,
          "first missed expected window should degrade warm sync");
    CHECK(arrival_policy::phaseAfterSyncMiss(3) == arrival_policy::WarmSyncPhase::DEGRADED,
          "short outages should remain in degraded warm sync");
    CHECK(arrival_policy::phaseAfterSyncMiss(4) == arrival_policy::WarmSyncPhase::RECOVERY,
          "long outages should enter recovery");
}

}  // namespace

int main() {
    test_ring_distance();
    test_no_overflow();
    test_overflow_drop_without_wrap();
    test_overflow_drop_with_wrap();
    test_pointer_drift_guard();
    test_backlog_snapshot();
    test_frame_arrival_prediction_tracks_cadence();
    test_frame_arrival_miss_accounting();
    test_warm_search_window_planning();
    test_warm_sync_phase_transitions();

    if (tests_failed != 0) {
        std::cout << "StreamingBufferPolicy: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "StreamingBufferPolicy: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
