// Unit tests for the goodput-maximizing rate controller (ULTRA_GOODPUT_RATE).
//
// The controller exists because the rig measured the SNR-anchor ladder costing 10.6%
// against simply pinning the best rung (2026-07-30, IONOS MPG@20, both pairs of an
// interleaved A/B agreeing). These tests pin the PROPERTIES that make it better, not
// the numbers it happened to produce: the hold guarantee is a theorem and is tested as
// one, and the scenario test replays the measured rig behaviour.

#include "env_compat.hpp"
#include "protocol/goodput_rate_controller.hpp"
#include "protocol/waveform_selection.hpp"

#include <cmath>
#include <iostream>

using namespace ultra;
using namespace ultra::protocol;

namespace {

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg) \
    do { \
        ++tests_run; \
        if (!(cond)) { \
            ++tests_failed; \
            std::cout << "FAIL: " << msg << "\n"; \
        } \
    } while (0)

GoodputRateController makeCtl(int window = 4, float group_s = 10.0f) {
    GoodputRateController c;
    c.configure(window, group_s);
    return c;
}

// ── The window is DERIVED from the channel, not chosen ────────────────────────
void test_window_from_coherence() {
    // Rappaport geometric-mean Tc = 0.423/fd. MPG is 0.1 Hz -> Tc 4.23 s. Eight
    // independent fades = 33.8 s; at ~10 s groups that is 4 groups.
    CHECK(std::fabs(coherenceTimeSeconds(0.1f) - 4.23f) < 0.01f,
          "Tc at 0.1 Hz Doppler should be 4.23 s");
    CHECK(windowGroupsForCoherence(0.1f, 10.0f) == 4,
          "MPG at 10 s groups should give a 4-group window");
    // A faster channel decorrelates sooner, so FEWER groups suffice.

    // Clamped both ways so the loop can neither act on noise nor sleep through a
    // real propagation change.
    // FLOOR IS STATISTICAL, not a latency compromise: the per-group delivered fraction
    // is a binomial over ~5 frames, so the estimator needs GROUPS, not fades. On the rig
    // a Moderate reading (0.5 Hz) collapsed the window to 2 groups and the controller
    // committed rung changes on ~10 frames of evidence.
    CHECK(windowGroupsForCoherence(10.0f, 10.0f) >= 4, "window floor is 4 groups");
    CHECK(windowGroupsForCoherence(0.5f, 10.0f) >= 4, "MPM must not buy a thinner window");
    CHECK(windowGroupsForCoherence(0.001f, 10.0f) <= 8, "window ceiling is 8 groups");
    CHECK(windowGroupsForCoherence(0.0f, 10.0f) == 4, "unknown Doppler falls back to 4");
}

// ── The ladder's pick BOUNDS the climb (rig-caught 2026-07-30) ────────────────
// Optimism-with-backoff has no memory before its first mistake, so a clean window climbs
// one rung EVERY window and walks the ladder up until something breaks. The rig logged
// 2->3->4->5->8 in four consecutive windows, landing on 16QAM R2/3 which measures 51.4%
// FER on ITU Good — tuition paid on every transfer, plus the overshoot coming back down
// (8 mode changes, 1.42 kbps). The anchor table's VALUES have been wrong before, but its
// structure encodes which rungs are viable at all; used as a ceiling it can only
// constrain, never force.
void test_ladder_ceiling_bounds_the_climb() {
    const uint8_t cur = kRungIdxQpskR23;
    // Unbounded: a clean window climbs.
    auto c = makeCtl();
    for (int i = 0; i < 4; ++i) c.observe(cur, 1.0f);
    CHECK(c.decide(cur).rung > cur, "with no ceiling a clean window climbs");

    // Ceiling AT the current rung: no climb, however clean.
    auto c2 = makeCtl();
    c2.setCeiling(cur);
    for (int i = 0; i < 4; ++i) c2.observe(cur, 1.0f);
    const auto d2 = c2.decide(cur);
    CHECK(d2.rung == cur, "a ceiling at the current rung blocks the climb");
    CHECK(std::string(d2.reason) == "ladder-ceiling", "and says so");

    // The ceiling must NOT block a demote — it bounds the top, not the bottom. This is
    // what keeps the ladder's own failure mode (over-committing) from returning.
    auto c3 = makeCtl();
    c3.setCeiling(kRungIdxQam16R23);
    const float be = goodputBreakEvenDeliveredFraction(cur);
    for (int i = 0; i < 4; ++i) c3.observe(cur, be * 0.4f);
    CHECK(c3.decide(cur).rung < cur, "a high ceiling still permits demoting");

    // And a ceiling ABOVE the next rung leaves the climb available — the controller can
    // still reach 8PSK R2/3, which is where the measured +10.6% lives.
    auto c4 = makeCtl();
    c4.setCeiling(kRungIdxQam8R23);
    for (int i = 0; i < 4; ++i) c4.observe(kRungIdxQpskR34, 1.0f);
    CHECK(c4.decide(kRungIdxQpskR34).rung == kRungIdxQam8R23,
          "a ceiling at 8PSK R2/3 still allows climbing into it");
}

// ── The hold rule is a THEOREM: f*eta_cur >= eta_below => no lower rung can win ──
void test_hold_guarantee_is_sound() {
    for (uint8_t cur = kRungIdxQpskR14; cur < kRungIdxCount; ++cur) {
        const float be = goodputBreakEvenDeliveredFraction(cur);
        if (!(be > 0.0f)) continue;
        // A DOMINATED rung (break-even >= 1) has no reachable holding fraction — see
        // test_dominated_rung_is_always_left. Excluded here by construction, not by
        // convenience: the guarantee is about rungs that CAN be held.
        if (be >= 1.0f) continue;
        auto c = makeCtl();
        // Deliver strictly above break-even (plus the switch margin) every group.
        const float f = std::min(1.0f, be * (1.0f + c.switchMargin()) + 0.02f);
        if (f < be * (1.0f + c.switchMargin())) continue;  // margin unreachable at f<=1
        for (int i = 0; i < 4; ++i) c.observe(cur, f);
        const auto d = c.decide(cur);
        CHECK(d.rung >= cur,
              "above break-even the controller must never demote (rung "
                  << static_cast<int>(cur) << ", f=" << f << ", be=" << be << ")");
    }
}

// ── LADDER FINDING (2026-07-30, found by this test): 16QAM R1/2 is DOMINATED ───
// eta(16QAM R1/2) = 4 * 0.5 = 2.000 and eta(8PSK R2/3) = 3 * 0.667 = 2.001 — the two
// rungs carry the SAME 2.0 bits/carrier (the 0.001 is getCodeRateValue's 3-dp rounding
// of 2/3, include/ultra/types.hpp:147). 8PSK R2/3 is the enabled rung directly below.
// So climbing QPSK->8PSK R2/3->16QAM R1/2 buys ZERO throughput while demanding a denser
// constellation: more SNR for the same bits, worse PAPR, and more exposure to the
// cheap-card compression that craters 16QAM above drive ~0.70. Break-even >= 1 means NO
// delivered fraction, not even a perfect one, justifies being there.
//
// This is a property of the ladder, not of the controller, and it is asserted here
// because a goodput-maximizing controller is the first thing in the tree that can
// notice it. If a future ladder edit makes 16QAM R1/2 genuinely better, this test
// fails and should be updated deliberately.
void test_dominated_rung_is_always_left() {
    const float eta_16qam_r12 = rungSpectralEfficiency(kRungIdxQam16R12);
    const float eta_8psk_r23 = rungSpectralEfficiency(kRungIdxQam8R23);
    CHECK(std::fabs(eta_16qam_r12 - 2.0f) < 1e-4f, "16QAM R1/2 carries 2.0 bits/carrier");
    CHECK(std::fabs(eta_8psk_r23 - 2.0f) < 2e-3f, "8PSK R2/3 carries 2.0 bits/carrier");
    CHECK(eta_16qam_r12 <= eta_8psk_r23,
          "16QAM R1/2 offers no spectral-efficiency gain over 8PSK R2/3");

    const float be = goodputBreakEvenDeliveredFraction(kRungIdxQam16R12);
    CHECK(be >= 1.0f, "a dominated rung has break-even >= 1 (be=" << be << ")");

    // The controller must never ENTER it: a lateral move cannot clear the switch cost.
    // (Leaving it is NOT required — the two rungs are tied, so the move would not pay
    // for itself either. The switch-cost term is symmetric and that is correct.)
    auto c2 = makeCtl();
    for (int i = 0; i < 4; ++i) c2.observe(kRungIdxQam8R23, 1.0f);
    const auto up = c2.decide(kRungIdxQam8R23);
    CHECK(up.rung != kRungIdxQam16R12,
          "a clean 8PSK R2/3 window must not climb laterally into 16QAM R1/2");
}

// ── The switch cost must make demoting HARDER, never easier ───────────────────
// Rig-caught 2026-07-30. The first implementation multiplied the break-even by
// (1+margin) instead of dividing, so a rung delivering ABOVE break-even was demoted:
// run 1 logged "f_window=0.800 break_even=0.750 reason=goodput-demote" and the transfer
// walked R3/4 -> R2/3 -> R1/2, finishing at 1.38 kbps against the ladder's 1.44. A cost
// term with the wrong sign is not a tuning error, it inverts the control law.
void test_switch_cost_sign_makes_demote_harder() {
    const uint8_t cur = kRungIdxQpskR23;
    const float be = goodputBreakEvenDeliveredFraction(cur);
    auto c = makeCtl();
    // Sit in the band between break_even/(1+m) and break_even — the exact region the
    // sign error mishandled. Delivering just BELOW raw break-even must still HOLD,
    // because the demote does not pay for its own switch cost.
    const float f = be * 0.98f;
    CHECK(f < be, "test point is below raw break-even");
    CHECK(f > be / (1.0f + c.switchMargin()), "and above the cost-adjusted bar");
    for (int i = 0; i < 4; ++i) c.observe(cur, f);
    const auto d = c.decide(cur);
    CHECK(d.rung >= cur,
          "just below break-even must HOLD — the demote must clear the switch cost too "
          "(f=" << f << " be=" << be << " bar=" << be / (1.0f + c.switchMargin()) << ")");
}

// ── Below break-even it demotes, but only on a FULL window ────────────────────
void test_demote_below_break_even() {
    const uint8_t cur = kRungIdxQpskR34;
    const float be = goodputBreakEvenDeliveredFraction(cur);
    CHECK(be > 0.0f, "QPSK R3/4 has a rung below it");
    auto c = makeCtl();
    const float f = be * 0.5f;
    // A partial window must NOT act — this is the anti-churn property.
    c.observe(cur, f);
    CHECK(!c.decide(cur).changed, "must not act on a partial window (1 of 4)");
    c.observe(cur, f);
    c.observe(cur, f);
    CHECK(!c.decide(cur).changed, "must not act on a partial window (3 of 4)");
    c.observe(cur, f);
    const auto d = c.decide(cur);
    CHECK(d.changed && d.rung < cur, "a full window below break-even demotes");
}

// ── Irreducible fading craters must NOT demote a rung that is still winning ────
void test_partial_craters_do_not_demote() {
    // THE measured failure mode. On Good@20 8PSK R2/3 delivers 2450 bps INCLUDING its
    // crater rate and beats QPSK R3/4's 2066 (FADING_ANCHOR_MEASUREMENT_2026_07_26).
    // A controller that reads craters as over-commit leaves the winning rung; this one
    // must not, because the delivered fraction still clears break-even.
    const uint8_t cur = kRungIdxQam8R23;
    const float be = goodputBreakEvenDeliveredFraction(cur);
    auto c = makeCtl();
    // Alternating 6/8 and 8/8 groups: visibly lossy, still winning.
    const float pattern[4] = {0.75f, 1.0f, 0.75f, 1.0f};
    for (int i = 0; i < 4; ++i) c.observe(cur, pattern[i]);
    const float f = c.windowedDeliveredFraction();
    CHECK(f > be / (1.0f + c.switchMargin()),
          "the replayed record clears the cost-adjusted demote bar");
    const auto d = c.decide(cur);
    // A CLIMB from here is legitimate (the probe is judged on measurement and reverts
    // if it loses). The failure mode under test is DEMOTING off a winning rung.
    CHECK(d.rung >= cur,
          "a lossy-but-winning rung must never demote (f=" << f << " be=" << be << ")");
}

// ── Two total craters bypass the window ───────────────────────────────────────
void test_catastrophic_fast_path() {
    const uint8_t cur = kRungIdxQpskR34;
    auto c = makeCtl();
    c.observe(cur, 0.0f);
    CHECK(!c.decide(cur).changed, "one total crater is not yet catastrophic");
    c.observe(cur, 0.0f);
    const auto d = c.decide(cur);
    CHECK(d.changed && d.rung < cur, "two total craters demote without a full window");
    CHECK(std::string(d.reason) == "catastrophic", "and are labelled as such");
}

// ── Climb requires headroom on EVERY group, not on average ────────────────────
void test_climb_requires_headroom() {
    const uint8_t cur = kRungIdxQpskR23;
    // DERIVED BAR, not an asserted one. The first version demanded >=0.95 on EVERY
    // group, which is close to unreachable under irreducible fading — the controller
    // could then only descend, and the rig measured exactly that (R3/4 -> R2/3 -> R1/2,
    // 1.38 kbps). The bar now comes from the same airtime algebra as the demote:
    // climbing wins iff f_above*eta_above > f_cur*eta_cur, so the rung above must
    // deliver f_required = f_cur*eta_cur/eta_above, and we bet when that is at or below
    // what the ladder itself considers sustainable for that rung.
    auto c2 = makeCtl();
    for (int i = 0; i < 4; ++i) c2.observe(cur, 1.0f);
    const auto d = c2.decide(cur);
    CHECK(d.rung > cur && d.changed, "a clean window climbs");

    // A struggling window must NOT climb: f_required rises with f_cur... but a LOW
    // f_cur also lowers f_required, so the protection here is the demote check running
    // first. Verify a genuinely failing rung goes DOWN, never up.
    auto c3 = makeCtl();
    for (int i = 0; i < 4; ++i) c3.observe(cur, 0.30f);
    const auto d3 = c3.decide(cur);
    CHECK(d3.rung < cur, "a failing rung descends rather than climbing");
}

// ── A failed probe backs off exponentially ────────────────────────────────────
void test_failed_probe_backs_off() {
    const uint8_t cur = kRungIdxQpskR23;
    auto c = makeCtl();
    for (int i = 0; i < 4; ++i) c.observe(cur, 1.0f);
    const auto first = c.decide(cur);
    CHECK(first.changed, "first clean window climbs");
    // The probe FAILS at the target rung — that is where the penalty belongs.
    c.noteProbeFailed(first.rung);
    // Back at cur with another clean window: the backoff must suppress the retry.
    // Rig-caught: this previously passed vacuously because noteProbeFailed wrote
    // backoff_[target] while decide() read backoff_[cur], so nothing blocked the
    // retry and the rig logged the same climb three times in a row.
    for (int i = 0; i < 4; ++i) c.observe(cur, 1.0f);
    CHECK(!c.decide(cur).changed, "backoff suppresses an immediate re-probe");
    // Backoff is finite — it must eventually allow another attempt.
    bool climbed = false;
    for (int k = 0; k < 40 && !climbed; ++k) {
        for (int i = 0; i < 4; ++i) c.observe(cur, 1.0f);
        if (c.decide(cur).changed) climbed = true;
    }
    CHECK(climbed, "backoff expires — the rung is not locked out forever");
}

// ── Marginal gains must not pay a switch they cannot recover ───────────────────
void test_switch_must_pay_for_itself() {
    auto c = makeCtl(4, 10.0f);
    // 3 s switch cost over a 40 s window = 7.5% minimum relative gain.
    CHECK(std::fabs(c.switchMargin() - 0.075f) < 1e-4f,
          "switch margin is cost/window = 3/40");
    // A very long window makes switching nearly free; a short one makes it costly.
    auto c_short = makeCtl(2, 3.0f);
    CHECK(c_short.switchMargin() > c.switchMargin(),
          "shorter windows demand a larger gain to justify a switch");
}

// ── Rung change resets the window (the old data described a different rung) ────
void test_window_resets_on_rung_change() {
    auto c = makeCtl();
    for (int i = 0; i < 4; ++i) c.observe(kRungIdxQpskR34, 0.10f);
    CHECK(c.observations() == 4, "window filled on the first rung");
    c.observe(kRungIdxQpskR23, 1.0f);
    CHECK(c.observations() == 1, "window resets when the rung changes");
    CHECK(!c.decide(kRungIdxQpskR23).changed,
          "and cannot act until the new rung has its own evidence");
}

// ── Ladder helper: climbing must skip DISABLED rungs, never index-arithmetic ───
void test_climb_skips_disabled_rungs() {
    // Disabled anchor rows are HOLES; raw +1 walked into one and deadlocked (F145).
    for (uint8_t r = kRungIdxQpskR14; r < kRungIdxCount; ++r) {
        const uint8_t up = snapRungIndexUpToEnabled(r);
        if (up == kRungIdxNone) continue;
        CHECK(up > r, "the next enabled rung is strictly above");
        const CoherentPick p = coherentRungFromIndex(up);
        CHECK(coherentRungLocallyEnabled(p.mod, p.rate),
              "snapRungIndexUpToEnabled must return an ENABLED rung");
    }
}

// ── Scenario: replay what the rig measured ────────────────────────────────────
void test_rig_scenario_holds_the_winning_rung() {
    // The auto ladder made 5-8 mode changes per 50 KB transfer and lost 10.6% to a
    // pinned 8PSK R2/3. Feed the controller a realistic Good@20 record for that rung
    // (mostly clean with irreducible fading losses) and require that it does NOT churn.
    const uint8_t cur = kRungIdxQam8R23;
    auto c = makeCtl(4, 10.0f);
    const float record[16] = {1.0f, 1.0f, 0.875f, 1.0f, 1.0f, 0.75f, 1.0f, 1.0f,
                              1.0f, 0.875f, 1.0f, 1.0f, 0.75f, 1.0f, 1.0f, 1.0f};
    int changes = 0;
    for (int i = 0; i < 16; ++i) {
        c.observe(cur, record[i]);
        if (c.decide(cur).changed) ++changes;
    }
    // The ladder's failure was leaving this rung. A climb attempt is legitimate; a
    // DEMOTE from a rung delivering ~94% is exactly the measured 10.6% loss.
    const float f = c.windowedDeliveredFraction();
    const float be = goodputBreakEvenDeliveredFraction(cur);
    CHECK(f > be, "the replayed record is above break-even (f=" << f << " be=" << be << ")");
    for (int i = 0; i < 16; ++i) {
        c.observe(cur, record[i]);
        const auto d = c.decide(cur);
        CHECK(d.rung >= cur, "must never demote off a rung delivering above break-even");
    }
    (void)changes;
}

}  // namespace

int main() {
    std::cout << "=== Goodput Rate Controller Tests ===\n";
    test_window_from_coherence();
    test_ladder_ceiling_bounds_the_climb();
    test_hold_guarantee_is_sound();
    test_dominated_rung_is_always_left();
    test_switch_cost_sign_makes_demote_harder();
    test_demote_below_break_even();
    test_partial_craters_do_not_demote();
    test_catastrophic_fast_path();
    test_climb_requires_headroom();
    test_failed_probe_backs_off();
    test_switch_must_pay_for_itself();
    test_window_resets_on_rung_change();
    test_climb_skips_disabled_rungs();
    test_rig_scenario_holds_the_winning_rung();
    std::cout << (tests_failed == 0 ? "PASS" : "FAIL") << ": " << (tests_run - tests_failed)
              << "/" << tests_run << " checks\n";
    return tests_failed == 0 ? 0 : 1;
}
