#include "gui/modem/streaming_signal_policy.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

using namespace ultra::gui::streaming_signal_policy;

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

#define CHECK_CLOSE(actual, expected, tol, msg) \
    CHECK(std::abs((actual) - (expected)) <= (tol), msg)

void test_mean_abs_llr() {
    CHECK(meanAbsLLR(nullptr, 3) == 0.0f, "null LLR pointer should be safe");
    CHECK(meanAbsLLR(nullptr, 0) == 0.0f, "empty LLR span should be safe");

    const float values[] = {1.0f, -2.0f, 3.0f};
    CHECK_CLOSE(meanAbsLLR(values, 3), 2.0f, 0.0001f, "mean absolute LLR");
}

void test_presync_llr_quality() {
    std::vector<float> strong(648, 2.0f);
    auto good = evaluatePreSyncLLR(strong.data(), strong.size(), 648);
    CHECK(good.count == 648, "presync LLR should cap at first CW");
    CHECK(good.near_zero_count == 0, "strong LLR should have no near-zero samples");
    CHECK_CLOSE(good.mean_abs, 2.0f, 0.0001f, "strong LLR mean");
    CHECK(!good.reject_as_false_lock, "strong LLR should not reject");

    std::vector<float> weak(648, 1.0f);
    auto weak_quality = evaluatePreSyncLLR(weak.data(), weak.size(), 648);
    CHECK(weak_quality.reject_as_false_lock, "low mean LLR should reject");

    std::vector<float> erased(648, 2.0f);
    for (size_t i = 0; i < 220; ++i) {
        erased[i] = 0.05f;
    }
    auto erasure_quality = evaluatePreSyncLLR(erased.data(), erased.size(), 648);
    CHECK(erasure_quality.reject_as_false_lock, "high near-zero fraction should reject");
    CHECK(erasure_quality.near_zero_fraction > kMaxErasureLikeFraction,
          "near-zero fraction should exceed policy limit");

    auto empty = evaluatePreSyncLLR(nullptr, 0, 648);
    CHECK(empty.reject_as_false_lock, "empty LLR quality should reject");
    CHECK(empty.near_zero_fraction == 1.0f, "empty LLR quality should be fully erasure-like");
}

void test_invalid_ofdm_lts_training() {
    CHECK(!invalidOFDMLTSTraining(false, true, 0.0f, 0.0f),
          "non-OFDM path should not apply LTS gate");
    CHECK(!invalidOFDMLTSTraining(true, false, 0.0f, 0.0f),
          "disconnected path should not apply LTS gate");
    CHECK(invalidOFDMLTSTraining(true, true,
                                 std::numeric_limits<float>::quiet_NaN(), 1.0f),
          "NaN LTS signal should reject");
    CHECK(invalidOFDMLTSTraining(true, true, 1.0f,
                                 std::numeric_limits<float>::infinity()),
          "infinite channel magnitude should reject");
    CHECK(invalidOFDMLTSTraining(true, true, 0.0005f, 0.01f),
          "both weak LTS metrics should reject");
    CHECK(!invalidOFDMLTSTraining(true, true, 0.0100f, 0.01f),
          "adequate LTS signal should not reject by itself");
    CHECK(!invalidOFDMLTSTraining(true, true, 0.0005f, 0.20f),
          "adequate channel magnitude should not reject by itself");
}

void test_light_sync_thresholds() {
    auto coherent = lightSyncThresholds(true, false, true, 99);
    CHECK_CLOSE(coherent.min_confidence, 0.90f, 0.0001f,
                "coherent threshold should stay strict");
    CHECK_CLOSE(coherent.weak_floor, 0.85f, 0.0001f, "coherent weak floor");

    auto narrow = lightSyncThresholds(false, true, true, 0);
    CHECK_CLOSE(narrow.min_confidence, 0.50f, 0.0001f, "narrowband threshold");
    CHECK_CLOSE(narrow.weak_floor, 0.40f, 0.0001f, "narrowband weak floor");

    auto connected = lightSyncThresholds(false, false, true, 0);
    CHECK_CLOSE(connected.min_confidence, 0.52f, 0.0001f, "connected threshold");
    CHECK_CLOSE(connected.weak_floor, 0.45f, 0.0001f, "connected weak floor");

    auto disconnected = lightSyncThresholds(false, false, false, 0);
    CHECK_CLOSE(disconnected.min_confidence, 0.65f, 0.0001f, "disconnected threshold");
    CHECK_CLOSE(disconnected.weak_floor, 0.55f, 0.0001f, "disconnected weak floor");

    auto relaxed = lightSyncThresholds(false, false, true, 5);
    CHECK_CLOSE(relaxed.min_confidence, 0.50f, 0.0001f, "reject streak should relax threshold");

    auto floor = lightSyncThresholds(false, false, true, 99);
    CHECK_CLOSE(floor.min_confidence, 0.45f, 0.0001f, "relaxation should have a hard floor");
}

void test_light_sync_decision() {
    LightSyncThresholds thresholds{0.52f, 0.45f};

    auto not_found = evaluateLightSyncCandidate(false, 0.0f, false, true, 7, thresholds);
    CHECK(!not_found.found, "not-found input should stay not found");
    CHECK(not_found.next_reject_streak == 7, "not-found input should preserve reject streak");

    auto strong = evaluateLightSyncCandidate(true, 0.80f, false, true, 7, thresholds);
    CHECK(strong.found, "strong candidate should be accepted");
    CHECK(!strong.weak_accept, "strong candidate should not be weak accept");
    CHECK(strong.next_reject_streak == 0, "strong candidate should clear reject streak");

    auto rejected = evaluateLightSyncCandidate(true, 0.43f, false, true, 0, thresholds);
    CHECK(!rejected.found, "low-correlation candidate should reject");
    CHECK(rejected.rejected, "low-correlation candidate should set rejected flag");
    CHECK(rejected.next_reject_streak == 1, "rejection should increment streak");

    auto weak = evaluateLightSyncCandidate(true, 0.46f, false, true, 3, thresholds);
    CHECK(weak.found, "weak candidate should be accepted after reject streak");
    CHECK(weak.weak_accept, "weak candidate should report weak accept");
    CHECK(weak.next_reject_streak == 1, "weak accept should decay reject streak");

    auto coherent = evaluateLightSyncCandidate(true, 0.86f, true, true, 10,
                                               LightSyncThresholds{0.90f, 0.85f});
    CHECK(!coherent.found, "coherent modes should not weak-accept below threshold");
    CHECK(coherent.next_reject_streak == 11, "coherent rejection should increment streak");
}

void test_cfo_drift_limit() {
    auto disconnected = limitConnectedCFODrift(false, 10.0f, 1.0f);
    CHECK(!disconnected.clamped, "disconnected CFO should not clamp");
    CHECK_CLOSE(disconnected.accepted_cfo, 10.0f, 0.0001f, "disconnected CFO accepted");

    auto unknown = limitConnectedCFODrift(true, 10.0f, 0.0f);
    CHECK(!unknown.clamped, "near-zero known CFO should not clamp");
    CHECK_CLOSE(unknown.accepted_cfo, 10.0f, 0.0001f, "unknown CFO accepted");

    auto small_drift = limitConnectedCFODrift(true, 10.9f, 10.0f);
    CHECK(!small_drift.clamped, "small CFO drift should not clamp");
    CHECK_CLOSE(small_drift.accepted_cfo, 10.9f, 0.0001f, "small drift accepted");

    auto large_drift = limitConnectedCFODrift(true, 12.0f, 10.0f);
    CHECK(large_drift.clamped, "large CFO drift should clamp");
    CHECK_CLOSE(large_drift.accepted_cfo, 10.0f, 0.0001f, "large drift should use known CFO");
    CHECK_CLOSE(large_drift.diff_hz, 2.0f, 0.0001f, "CFO diff telemetry");
}

}  // namespace

int main() {
    test_mean_abs_llr();
    test_presync_llr_quality();
    test_invalid_ofdm_lts_training();
    test_light_sync_thresholds();
    test_light_sync_decision();
    test_cfo_drift_limit();

    if (tests_failed != 0) {
        std::cout << "StreamingSignalPolicy: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "StreamingSignalPolicy: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
