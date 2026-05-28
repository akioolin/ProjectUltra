// Unit tests for RateController — the Phase 5c per-block rate-adaptation policy
// (design §14.36). Pure state machine; no PHY/protocol dependency.

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

void test_failed_group_drops_immediately() {
    RateController c;
    // quality 0 (failed decode) at R3/4 -> step down to R2/3 next burst.
    CHECK(c.update(CodeRate::R3_4, 0.0f) == CodeRate::R2_3,
          "failed group steps R3/4 -> R2/3 immediately");
    CHECK(c.update(CodeRate::R2_3, 0.0f) == CodeRate::R1_2,
          "failed group steps R2/3 -> R1/2");
}

void test_drop_is_one_rung_at_a_time() {
    RateController c;
    CHECK(c.update(CodeRate::R5_6, 0.10f) == CodeRate::R3_4, "from top: R5/6 -> R3/4");
    CHECK(c.update(CodeRate::R3_4, 0.10f) == CodeRate::R2_3, "low quality -> one rung down");
    CHECK(c.update(CodeRate::R2_3, 0.10f) == CodeRate::R1_2, "again one rung down");
    CHECK(c.update(CodeRate::R1_2, 0.10f) == CodeRate::R1_4, "again one rung down");
    CHECK(c.update(CodeRate::R1_4, 0.10f) == CodeRate::R1_4, "floor: cannot drop below R1/4");
}

void test_climb_is_slow_needs_consecutive_good() {
    RateController c;  // climb_streak default 3; ladder {R1/4,R1/2,R2/3,R3/4,R5/6}
    // 1st and 2nd comfortable groups: hold (streak < 3)
    CHECK(c.update(CodeRate::R1_2, 0.95f) == CodeRate::R1_2, "1 good group: hold");
    CHECK(c.update(CodeRate::R1_2, 0.95f) == CodeRate::R1_2, "2 good groups: still hold");
    // 3rd consecutive comfortable group -> climb
    CHECK(c.update(CodeRate::R1_2, 0.95f) == CodeRate::R2_3, "3 good groups: climb R1/2 -> R2/3");
    CHECK(c.update(CodeRate::R2_3, 0.95f) == CodeRate::R2_3, "streak reset after climb: hold");
    CHECK(c.update(CodeRate::R2_3, 0.95f) == CodeRate::R2_3, "2 good after reset: still hold");
    CHECK(c.update(CodeRate::R2_3, 0.95f) == CodeRate::R3_4, "3 good: climb R2/3 -> R3/4");
    CHECK(c.update(CodeRate::R3_4, 0.95f) == CodeRate::R3_4, "1 good at R3/4: hold");
    CHECK(c.update(CodeRate::R3_4, 0.95f) == CodeRate::R3_4, "2 good at R3/4: hold");
    CHECK(c.update(CodeRate::R3_4, 0.95f) == CodeRate::R5_6, "3 good: climb R3/4 -> R5/6 (top)");
    CHECK(c.update(CodeRate::R5_6, 0.95f) == CodeRate::R5_6, "ceiling: cannot climb above R5/6");
    CHECK(c.update(CodeRate::R5_6, 0.95f) == CodeRate::R5_6, "ceiling: still R5/6");
}

void test_midzone_holds_and_breaks_climb_streak() {
    RateController c;  // climb_streak default 3
    CHECK(c.update(CodeRate::R1_2, 0.95f) == CodeRate::R1_2, "1 good (streak=1)");
    CHECK(c.update(CodeRate::R1_2, 0.95f) == CodeRate::R1_2, "2 good (streak=2)");
    // a mid-zone group (decoding fine, no margin) must RESET the climb streak
    CHECK(c.update(CodeRate::R1_2, 0.50f) == CodeRate::R1_2, "mid-zone holds, streak reset");
    CHECK(c.update(CodeRate::R1_2, 0.95f) == CodeRate::R1_2, "1 good after reset (streak=1, no climb)");
    CHECK(c.update(CodeRate::R1_2, 0.95f) == CodeRate::R1_2, "2 good after reset (streak=2, no climb)");
    CHECK(c.update(CodeRate::R1_2, 0.95f) == CodeRate::R2_3, "3 consecutive good -> climb");
}

void test_no_thrash_in_hysteresis_gap() {
    RateController c;
    // quality sitting between drop_below(0.25) and climb_above(0.70): never moves.
    for (int i = 0; i < 20; ++i) {
        CHECK(c.update(CodeRate::R2_3, 0.50f) == CodeRate::R2_3,
              "steady mid-quality never changes rate (no thrash)");
    }
}

void test_drop_resets_climb_progress() {
    RateController c;
    c.update(CodeRate::R2_3, 0.95f);                 // streak = 1
    CHECK(c.climbStreak() == 1, "one good group banked");
    CHECK(c.update(CodeRate::R2_3, 0.0f) == CodeRate::R1_2, "fail drops a rung");
    CHECK(c.climbStreak() == 0, "drop wipes climb progress");
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
    test_failed_group_drops_immediately();
    test_drop_is_one_rung_at_a_time();
    test_climb_is_slow_needs_consecutive_good();
    test_midzone_holds_and_breaks_climb_streak();
    test_no_thrash_in_hysteresis_gap();
    test_drop_resets_climb_progress();
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
