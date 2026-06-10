// Unit tests for RateController — the Phase 5c per-block rate-adaptation policy
// (design §14.36). Pure state machine; no PHY/protocol dependency.
//
// 2026-06-09: the steering quality is now EMA-SMOOTHED so a single transient fade
// (one NACK on the binary ack/nack path) cannot move the rate — this is the churn
// fix. The pre-smoothing policy dropped a rung on EVERY bad sample, so on a fading
// channel it ratcheted monotonically to R1/4 and never climbed back.

#include "protocol/rate_controller.hpp"
#include "ultra/types.hpp"

#include <iostream>

using namespace ultra;
using namespace ultra::protocol;

namespace {

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg)                                  \
    do {                                                  \
        ++tests_run;                                      \
        if (!(cond)) {                                    \
            ++tests_failed;                               \
            std::cout << "FAIL: " << msg << "\n";         \
        }                                                 \
    } while (0)

// With the default EMA (alpha=0.4) and reset-to-midpoint(0.475) after a change, the
// thresholds are drop_below=0.25, climb_above=0.70, climb_streak=3.

void test_first_bad_group_acts_no_history() {
    // The very first sample has no history -> ema = quality directly, so a failed
    // first group still drops immediately (best evidence available).
    RateController c;
    CHECK(c.update(CodeRate::R3_4, 0.0f) == CodeRate::R2_3,
          "first group (no history) at q=0 drops R3/4 -> R2/3");
}

void test_single_transient_fade_does_not_drop() {
    // THE CHURN FIX. Warm up the EMA at the top rung with good groups, then inject
    // fades: ONE (and even TWO) consecutive fades must HOLD; only a sustained run
    // (3rd consecutive) finally drops a rung.
    RateController c;
    for (int i = 0; i < 4; ++i) c.update(CodeRate::R5_6, 1.0f);  // ema -> ~1.0
    CHECK(c.update(CodeRate::R5_6, 0.0f) == CodeRate::R5_6,
          "1 fade after a healthy run: HOLD (no churn)");
    CHECK(c.update(CodeRate::R5_6, 0.0f) == CodeRate::R5_6,
          "2 consecutive fades: still HOLD");
    CHECK(c.update(CodeRate::R5_6, 0.0f) == CodeRate::R3_4,
          "3 consecutive fades (sustained): NOW drop one rung");
}

void test_periodic_fade_does_not_ratchet_to_floor() {
    // THE regression that motivated the fix: a fade every 4th group (75% good) must
    // NEVER ratchet below the starting rung. Pre-fix, each binary NACK dropped a rung
    // and it walked straight to R1/4.
    RateController c;
    CodeRate r = CodeRate::R2_3;
    bool ratcheted_below_start = false;
    for (int i = 0; i < 60; ++i) {
        const float q = (i % 4 == 3) ? 0.0f : 1.0f;  // one fade per four groups
        r = c.update(r, q);
        if (r == CodeRate::R1_2 || r == CodeRate::R1_4) ratcheted_below_start = true;
    }
    CHECK(!ratcheted_below_start,
          "75%-good periodic fade never ratchets below the R2/3 start (no churn)");
    CHECK(r == CodeRate::R3_4 || r == CodeRate::R5_6 || r == CodeRate::R2_3,
          "ends at or above the start rung");
}

void test_sustained_failure_descends_to_floor() {
    // Unrelenting failure (the rung really IS over capacity) must still descend all
    // the way to the R1/4 floor — smoothing slows it, it does not stop it.
    RateController c;
    CodeRate r = CodeRate::R5_6;
    for (int i = 0; i < 40; ++i) r = c.update(r, 0.0f);
    CHECK(r == CodeRate::R1_4, "unrelenting failure descends to the R1/4 floor");
}

void test_climb_is_slow_and_deliberate() {
    RateController c;  // climb_streak default 3
    CHECK(c.update(CodeRate::R1_2, 1.0f) == CodeRate::R1_2, "1 good group: hold");
    CHECK(c.update(CodeRate::R1_2, 1.0f) == CodeRate::R1_2, "2 good groups: hold");
    CHECK(c.update(CodeRate::R1_2, 1.0f) == CodeRate::R2_3, "3 consecutive good: climb");
    // post-climb the EMA is reset to midpoint, so the next climb takes LONGER than 3
    // (the EMA must first re-reach climb_above) — deliberately slow.
    CHECK(c.update(CodeRate::R2_3, 1.0f) == CodeRate::R2_3, "post-climb hold (ema reset below climb_above)");
    // sustained perfect quality eventually reaches the top rung.
    CodeRate r = CodeRate::R1_4;
    for (int i = 0; i < 40; ++i) r = c.update(r, 1.0f);
    CHECK(r == CodeRate::R5_6, "sustained perfect quality climbs to the top rung");
}

void test_no_thrash_in_hysteresis_gap() {
    RateController c;
    // steady mid-quality between drop_below(0.25) and climb_above(0.70): never moves.
    for (int i = 0; i < 20; ++i) {
        CHECK(c.update(CodeRate::R2_3, 0.50f) == CodeRate::R2_3,
              "steady mid-quality never changes rate (no thrash)");
    }
}

void test_quality_from_iterations() {
    CHECK(RateController::qualityFromIterations(false, 0, 60) == 0.0f, "failed decode -> 0");
    CHECK(RateController::qualityFromIterations(true, 0, 60) == 1.0f, "0 iters -> full headroom");
    const float q = RateController::qualityFromIterations(true, 30, 60);
    CHECK(q > 0.49f && q < 0.51f, "half max iters -> ~0.5 headroom");
    CHECK(RateController::qualityFromIterations(true, 60, 60) == 0.0f, "max iters -> 0 headroom");
}

void test_off_ladder_rate_passes_through() {
    RateController c;
    // R1_3 isn't on the default ladder -> pass through unchanged, no crash.
    CHECK(c.update(CodeRate::R1_3, 0.0f) == CodeRate::R1_3, "off-ladder rate passes through");
}

}  // namespace

int main() {
    test_first_bad_group_acts_no_history();
    test_single_transient_fade_does_not_drop();
    test_periodic_fade_does_not_ratchet_to_floor();
    test_sustained_failure_descends_to_floor();
    test_climb_is_slow_and_deliberate();
    test_no_thrash_in_hysteresis_gap();
    test_quality_from_iterations();
    test_off_ladder_rate_passes_through();

    if (tests_failed != 0) {
        std::cout << "RateController: " << (tests_run - tests_failed) << "/" << tests_run
                  << " passed\n";
        return 1;
    }
    std::cout << "RateController: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
