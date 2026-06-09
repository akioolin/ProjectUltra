#include "sync/signal_policy.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

using namespace ultra::sync::signal_policy;

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

    auto warm = lightSyncThresholds(false, false, true, 0, true, 9600, 2176);
    const float expected_warm_threshold =
        deriveNarrowWindowMagnitudeThreshold(0.52f, 9600, 2176);
    CHECK(warm.narrow_expected_window, "warm expected-window threshold should report narrow mode");
    CHECK_CLOSE(warm.false_positive_window_reduction, 9600.0f / 2176.0f, 0.001f,
                "warm threshold should expose window reduction factor");
    CHECK_CLOSE(warm.threshold_reduction_db,
                10.0f * std::log10(9600.0f / 2176.0f), 0.001f,
                "warm threshold should expose dB reduction");
    CHECK_CLOSE(warm.min_confidence, expected_warm_threshold, 0.0001f,
                "warm threshold should be derived from window-size reduction");
    CHECK(warm.min_confidence > 0.24f && warm.min_confidence < 0.26f,
          "warm threshold should land near 0.25 for the current narrow window");

    auto disconnected = lightSyncThresholds(false, false, false, 0);
    CHECK_CLOSE(disconnected.min_confidence, 0.65f, 0.0001f, "disconnected threshold");
    CHECK_CLOSE(disconnected.weak_floor, 0.55f, 0.0001f, "disconnected weak floor");

    auto relaxed = lightSyncThresholds(false, false, true, 5);
    CHECK_CLOSE(relaxed.min_confidence, 0.50f, 0.0001f, "reject streak should relax threshold");

    auto floor = lightSyncThresholds(false, false, true, 99);
    CHECK_CLOSE(floor.min_confidence, 0.40f, 0.0001f, "relaxation should have a hard floor");
    CHECK_CLOSE(floor.weak_floor, 0.35f, 0.0001f,
                "long reject streak should enable connected OFDM rescue floor");
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

    auto rescued = evaluateLightSyncCandidate(
        true, 0.36f, false, true, 8,
        lightSyncThresholds(false, false, true, 8));
    CHECK(rescued.found, "late connected OFDM rescue candidate should be accepted");
    CHECK(rescued.weak_accept, "late connected OFDM rescue should report weak accept");

    auto coherent = evaluateLightSyncCandidate(true, 0.86f, true, true, 10,
                                               LightSyncThresholds{0.90f, 0.85f});
    CHECK(!coherent.found, "coherent modes should not weak-accept below threshold");
    CHECK(coherent.next_reject_streak == 11, "coherent rejection should increment streak");
}

void test_cfo_drift_limit() {
    // High-correlation (trusted) chirps: behaviour is the original drift clamp.
    constexpr float kHi = 1.0f;
    auto disconnected = limitConnectedCFODrift(false, 10.0f, 1.0f, kHi);
    CHECK(!disconnected.clamped, "disconnected trusted CFO should not clamp");
    CHECK_CLOSE(disconnected.accepted_cfo, 10.0f, 0.0001f, "disconnected CFO accepted");

    auto unknown = limitConnectedCFODrift(true, 10.0f, 0.0f, kHi);
    CHECK(!unknown.clamped, "near-zero known + trusted chirp should not clamp");
    CHECK_CLOSE(unknown.accepted_cfo, 10.0f, 0.0001f, "unknown CFO accepted");

    auto small_drift = limitConnectedCFODrift(true, 10.9f, 10.0f, kHi);
    CHECK(!small_drift.clamped, "small CFO drift should not clamp");
    CHECK_CLOSE(small_drift.accepted_cfo, 10.9f, 0.0001f, "small drift accepted");

    auto large_drift = limitConnectedCFODrift(true, 12.0f, 10.0f, kHi);
    CHECK(large_drift.clamped, "large CFO drift should clamp");
    CHECK_CLOSE(large_drift.accepted_cfo, 10.0f, 0.0001f, "large drift should use known CFO");
    CHECK_CLOSE(large_drift.diff_hz, 2.0f, 0.0001f, "CFO diff telemetry");

    // Low-confidence (fade-jittered) chirp: a large jump is rejected to the tracked value at
    // EVERY stage, including the pre-connect PING (so a phantom never establishes in `known`).
    auto phantom_ping = limitConnectedCFODrift(false, -1.25f, 0.0f, 0.73f);
    CHECK(phantom_ping.clamped, "low-corr phantom must clamp even at PING (disconnected)");
    CHECK_CLOSE(phantom_ping.accepted_cfo, 0.0f, 0.0001f, "phantom rejected to tracked 0");

    // Genuine first acquisition: a clean high-corr chirp with a large real dial offset is accepted
    // (no established CFO to clamp to, and the chirp is trusted).
    auto real_acq = limitConnectedCFODrift(false, 30.0f, 0.0f, 0.95f);
    CHECK(!real_acq.clamped, "trusted large real CFO at acquisition must be accepted");
    CHECK_CLOSE(real_acq.accepted_cfo, 30.0f, 0.0001f, "real dial offset acquired");
}

void test_pilot_cfo_update() {
    auto residual_only = combinePilotCFO(0.0f, 0.25f, 0.0f, false);
    CHECK_CLOSE(residual_only.unclamped_cfo, 0.25f, 0.0001f,
                "pilot CFO should use residual when no pre-correction was applied");
    CHECK_CLOSE(residual_only.accepted_cfo, 0.25f, 0.0001f, "unclamped residual accepted");
    CHECK(!residual_only.clamped, "unclamped update should not clamp");

    auto corrected = combinePilotCFO(9.5f, 0.4f, 9.0f, true);
    CHECK_CLOSE(corrected.unclamped_cfo, 9.9f, 0.0001f,
                "pilot CFO should add pre-correction and residual");
    CHECK_CLOSE(corrected.accepted_cfo, 9.9f, 0.0001f, "small pilot CFO drift accepted");
    CHECK(!corrected.clamped, "small pilot CFO drift should not clamp");

    auto positive_clamp = combinePilotCFO(9.5f, 4.0f, 10.0f, true);
    CHECK(positive_clamp.clamped, "large positive pilot CFO drift should clamp");
    CHECK_CLOSE(positive_clamp.unclamped_cfo, 13.5f, 0.0001f,
                "pilot CFO unclamped telemetry");
    CHECK_CLOSE(positive_clamp.accepted_cfo, 12.0f, 0.0001f,
                "positive pilot CFO clamp should step from reference");

    auto negative_clamp = combinePilotCFO(9.5f, -4.0f, 10.0f, true);
    CHECK(negative_clamp.clamped, "large negative pilot CFO drift should clamp");
    CHECK_CLOSE(negative_clamp.accepted_cfo, 8.0f, 0.0001f,
                "negative pilot CFO clamp should step from reference");
}

}  // namespace

int main() {
    test_mean_abs_llr();
    test_presync_llr_quality();
    test_invalid_ofdm_lts_training();
    test_light_sync_thresholds();
    test_light_sync_decision();
    test_cfo_drift_limit();
    test_pilot_cfo_update();

    if (tests_failed != 0) {
        std::cout << "StreamingSignalPolicy: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "StreamingSignalPolicy: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
