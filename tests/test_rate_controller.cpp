// Unit tests for RateController — the Phase 5c per-block rate-adaptation policy
// (design §14.36). Pure state machine; no PHY/protocol dependency.
//
// 2026-06-09: the steering quality is now EMA-SMOOTHED so a single transient fade
// (one NACK on the binary ack/nack path) cannot move the rate — this is the churn
// fix. The pre-smoothing policy dropped a rung on EVERY bad sample, so on a fading
// channel it ratcheted monotonically to R1/4 and never climbed back.

#include "protocol/rate_controller.hpp"
#include "ultra/types.hpp"

#include <cmath>
#include <cstdlib>
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
// thresholds are drop_below=0.25, climb_above=0.70, climb_streak=2 (3 -> 2 on
// 2026-07-02 for the fade-riding ladder: reaction within ~2 ACKed groups; the EMA +
// reset-to-midpoint keep the sustained-evidence inertia).

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
    for (int i = 0; i < 4; ++i) c.update(CodeRate::R3_4, 1.0f);  // ema -> ~1.0
    CHECK(c.update(CodeRate::R3_4, 0.0f) == CodeRate::R3_4,
          "1 fade after a healthy run: HOLD (no churn)");
    CHECK(c.update(CodeRate::R3_4, 0.0f) == CodeRate::R3_4,
          "2 consecutive fades: still HOLD");
    CHECK(c.update(CodeRate::R3_4, 0.0f) == CodeRate::R2_3,
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
    CHECK(r == CodeRate::R3_4 || r == CodeRate::R2_3,
          "ends at or above the start rung");
}

void test_sustained_failure_descends_to_floor() {
    // Unrelenting failure (the rung really IS over capacity) must still descend all
    // the way to the R1/4 floor — smoothing slows it, it does not stop it.
    RateController c;
    CodeRate r = CodeRate::R3_4;
    for (int i = 0; i < 40; ++i) r = c.update(r, 0.0f);
    CHECK(r == CodeRate::R1_4, "unrelenting failure descends to the R1/4 floor");
}

void test_climb_is_deliberate() {
    RateController c;  // climb_streak default 2 (fade-riding, 2026-07-02)
    CHECK(c.update(CodeRate::R1_2, 1.0f) == CodeRate::R1_2, "1 good group: hold");
    CHECK(c.update(CodeRate::R1_2, 1.0f) == CodeRate::R2_3, "2 consecutive good: climb");
    // post-climb the EMA is reset to midpoint, so the next climb takes LONGER than 2
    // (the EMA must first re-reach climb_above) — still deliberate, not instant.
    CHECK(c.update(CodeRate::R2_3, 1.0f) == CodeRate::R2_3, "post-climb hold (ema reset below climb_above)");
    // sustained perfect quality eventually reaches the top rung.
    CodeRate r = CodeRate::R1_4;
    for (int i = 0; i < 40; ++i) r = c.update(r, 1.0f);
    CHECK(r == CodeRate::R3_4, "sustained perfect quality climbs to the top rung");
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

void test_ssthresh_ceiling_blocks_bounce_back_into_failed_rung() {
    // THE OSCILLATION FIX (2026-06-11). Without ssthresh, dropping off the top rung on a fade and
    // then climbing straight back into it (every climb_streak good groups) thrashed the rate
    // ~15x in one transfer and burned the whole airtime budget (Good@20 seed 7/42). That was the
    // R3/4<->R5/6 boundary; R5/6 is now retired (2026-06-17) so the guard protects the new top
    // boundary R2/3<->R3/4 with identical mechanics.
    RateController c;
    CodeRate r = CodeRate::R1_4;
    for (int i = 0; i < 40; ++i) r = c.update(r, 1.0f);
    CHECK(r == CodeRate::R3_4, "warm up to the top rung on sustained good");

    // exactly one sustained-fade window drops ONE rung off the top (ema 1.0 -> <drop_below in 3).
    r = c.update(r, 0.0f);
    r = c.update(r, 0.0f);
    r = c.update(r, 0.0f);
    CHECK(r == CodeRate::R2_3, "a sustained fade drops R3/4 -> R2/3");
    CHECK(c.ceilingRate() == CodeRate::R2_3, "the drop caps the ssthresh ceiling at R2/3");

    // good channel again: without ssthresh the controller would re-climb the instant one
    // climb_streak window (EMA warm-up + 2 good groups) passes. ssthresh must HOLD below
    // the failed rung through that FIRST climb-eligible window (no immediate bounce-back);
    // with climb_streak=2 the first re-probe becomes eligible at the ~5th good group.
    for (int i = 0; i < 4; ++i) {
        r = c.update(r, 1.0f);
        CHECK(r == CodeRate::R2_3,
              "ssthresh holds below the failed rung through the first climb window");
    }

    // but a sustained good run DOES eventually re-probe the ceiling upward (channel may
    // recover) — ~ceiling_reprobe_climbs x climb_streak good groups after the drop.
    for (int i = 0; i < 30; ++i) r = c.update(r, 1.0f);
    CHECK(r == CodeRate::R3_4, "after a sustained good run the ceiling re-probes back to the top rung");
}

// ---- Promote EMA carry (ULTRA_PROMOTE_EMA_CARRY, 2026-07-03) ------------------
// The env read is a latch-once static (pinned OFF in main below), so the ON cases
// drive Config::promote_ema_carry directly — the production ctor ORs the env into
// the SAME field, so this exercises the identical policy path.

RateController makeCarryOn() {
    RateController::Config cfg;
    cfg.promote_ema_carry = true;
    return RateController(cfg);
}

float midpoint(const RateController& c) {
    return 0.5f * (c.config().drop_below + c.config().climb_above);
}

void test_promote_carry_off_by_default_midpoint() {
    // Knob OFF (env pinned "0" in main): post-promote EMA resets to the neutral
    // midpoint — byte-identical to the pre-knob policy.
    RateController c;
    c.update(CodeRate::R1_2, 1.0f);
    CHECK(c.update(CodeRate::R1_2, 1.0f) == CodeRate::R2_3, "warm-up promote fires");
    CHECK(std::fabs(c.emaQuality() - midpoint(c)) < 1e-6f,
          "knob OFF: post-promote EMA = midpoint (byte-identical default)");
}

void test_promote_carry_seeds_climb_eligible() {
    // Knob ON: a promote seeds the EMA at climb_above — the new rung starts
    // climb-ELIGIBLE, so the very next clean group counts toward the streak and
    // exactly climb_streak (2) clean groups gate the next promote.
    RateController c = makeCarryOn();
    c.update(CodeRate::R1_2, 1.0f);
    CHECK(c.update(CodeRate::R1_2, 1.0f) == CodeRate::R2_3, "promote R1/2 -> R2/3");
    CHECK(std::fabs(c.emaQuality() - c.config().climb_above) < 1e-6f,
          "knob ON: post-promote EMA seeded at climb_above");
    CHECK(c.update(CodeRate::R2_3, 1.0f) == CodeRate::R2_3,
          "1st clean group at the new rung: hold (streak 1 of 2)");
    CHECK(c.climbStreak() == 1,
          "the first post-promote clean group increments the streak IMMEDIATELY");
    CHECK(c.update(CodeRate::R2_3, 1.0f) == CodeRate::R3_4,
          "2nd clean group: climb — only the streak gates the next move");
}

void test_promote_carry_demote_still_resets_to_midpoint() {
    // Knob ON must not touch the demote side: after a drop the channel proved worse
    // than believed — the midpoint reset is correct and unchanged.
    RateController c = makeCarryOn();
    for (int i = 0; i < 4; ++i) c.update(CodeRate::R3_4, 1.0f);  // ema -> ~1.0
    c.update(CodeRate::R3_4, 0.0f);
    c.update(CodeRate::R3_4, 0.0f);
    CHECK(c.update(CodeRate::R3_4, 0.0f) == CodeRate::R2_3,
          "sustained fade still demotes (3 bad groups, unchanged)");
    CHECK(std::fabs(c.emaQuality() - midpoint(c)) < 1e-6f,
          "knob ON: post-DEMOTE EMA still resets to the midpoint");
}

void test_promote_carry_one_bad_group_kills_eligibility() {
    // No runaway ratchet: ONE bad group right after a carried promote pulls the EMA
    // 0.70 -> 0.42 (alpha 0.4) — climb eligibility gone instantly, but still above
    // drop_below (no panic demote), and the next clean group must NOT climb.
    RateController c = makeCarryOn();
    c.update(CodeRate::R1_2, 1.0f);
    CHECK(c.update(CodeRate::R1_2, 1.0f) == CodeRate::R2_3, "promote R1/2 -> R2/3");
    CHECK(c.update(CodeRate::R2_3, 0.0f) == CodeRate::R2_3,
          "one bad group after the promote: HOLD (no demote)");
    CHECK(c.emaQuality() < c.config().climb_above,
          "one bad group drops the EMA below climb_above (eligibility gone)");
    CHECK(c.emaQuality() > c.config().drop_below,
          "...but not below drop_below (no panic demote)");
    CHECK(c.update(CodeRate::R2_3, 1.0f) == CodeRate::R2_3,
          "next clean group does NOT climb (EMA 0.42 -> 0.652 < 0.70: no runaway)");
    CHECK(c.climbStreak() == 0, "streak stays cleared until the EMA re-earns eligibility");
}

}  // namespace

int main() {
    // Pin the promote-carry knob OFF before ANY RateController is constructed — the
    // env read inside the ctor is a latch-once static-lambda. The knob-ON cases above
    // set Config::promote_ema_carry directly (the env can only OR into that field).
    setenv("ULTRA_PROMOTE_EMA_CARRY", "0", 1);

    test_first_bad_group_acts_no_history();
    test_single_transient_fade_does_not_drop();
    test_periodic_fade_does_not_ratchet_to_floor();
    test_sustained_failure_descends_to_floor();
    test_climb_is_deliberate();
    test_no_thrash_in_hysteresis_gap();
    test_quality_from_iterations();
    test_off_ladder_rate_passes_through();
    test_ssthresh_ceiling_blocks_bounce_back_into_failed_rung();
    test_promote_carry_off_by_default_midpoint();
    test_promote_carry_seeds_climb_eligible();
    test_promote_carry_demote_still_resets_to_midpoint();
    test_promote_carry_one_bad_group_kills_eligibility();

    if (tests_failed != 0) {
        std::cout << "RateController: " << (tests_run - tests_failed) << "/" << tests_run
                  << " passed\n";
        return 1;
    }
    std::cout << "RateController: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
