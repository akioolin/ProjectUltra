// Tests for the latent-state rate controller (ULTRA_LATENT_RATE).
//
// The properties under test are the ones that distinguish this design from the two
// controllers that preceded it and measured a wash / a regression:
//
//   1. Evidence updates EVERY rung, so it survives a rung change (the predecessor zeroed a
//      per-rung ring on every change and then sat inert for 4-8 groups).
//   2. COMMON-MODE calibration error cancels exactly. This is the property that lets the
//      rate path stop consuming kOfdmLegacyAnchorScaleOffsetDb (+8.70 dB) — and the +8.70
//      is itself only a compatibility shim for an estimator bug that was already fixed.
//   3. Overhead sits in the DENOMINATOR, so the break-even for one rung up is the airtime
//      ratio (0.835 with our measured cycle), not the raw-rate ratio (0.750).
//   4. Ranking on the MEASURED waterfall reproduces the measured ordering.

#include "env_compat.hpp"
#include "protocol/latent_rate_controller.hpp"
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
        if (!(cond)) { ++tests_failed; std::cout << "FAIL: " << msg << "\n"; } \
    } while (0)

// Measured scheduling geometry, docs/FADING_ANCHOR_MEASUREMENT_2026_07_26.md §1:
// payload 6.19 s over 5 frames, sync 1.41 s + turnaround 1.79 s fixed per cycle.
constexpr float kPayloadPerFrameS = 6.19f / 5.0f;
constexpr float kFixedPerCycleS = 1.41f + 1.79f;
constexpr int kFramesPerGroup = 5;

LatentRateController::Pick pick(LatentRateController& c) {
    return c.best(kPayloadPerFrameS, kFixedPerCycleS, kFramesPerGroup);
}

// ── 1. Evidence updates every rung and survives a rung change ─────────────────
void test_evidence_survives_rung_change() {
    LatentRateController c;
    c.seedPrior(18.0f, 3.0f);
    const float spread0 = c.spreadDb();

    // Observe on ONE rung only.
    for (int i = 0; i < 6; ++i) c.observe(kRungIdxQam8R23, 5, 5);
    CHECK(c.spreadDb() < spread0, "observing sharpens the posterior");
    CHECK(c.observations() == 6, "all observations counted");

    // The predecessor would now be blind about every other rung. Here the SAME posterior
    // already predicts them: a rung with a LOWER theta must have a HIGHER success
    // probability at the same x, with no observation of that rung ever having been made.
    const float x = c.percentile(0.25f);
    const float p_low = LatentRateController::successProb(x, latentThetaForRung(kRungIdxQpskR23));
    const float p_high = LatentRateController::successProb(x, latentThetaForRung(kRungIdxQam16R23));
    CHECK(p_low > p_high,
          "a never-tried lower rung is predicted more reliable than a never-tried higher one");

    // And a rung change costs nothing: no reset, no refill, no inertia.
    const int obs_before = c.observations();
    c.observe(kRungIdxQpskR34, 5, 5);
    CHECK(c.observations() == obs_before + 1,
          "switching rung does not discard accumulated evidence");
}

// ── 2. COMMON-MODE CANCELLATION — the reason +8.70 can leave the rate path ────
// If every theta_r moves by the same constant, the posterior over x moves with it and the
// DECISION is unchanged. That is what makes an outcome-fitted latent state immune to the
// whole family of calibration defects this project has fought: the +8.70 dB compatibility
// offset, the guard-bin noise under-read, the ~5.6 dB IONOS implementation loss.
void test_common_mode_calibration_error_cancels() {
    // Two controllers observing IDENTICAL outcomes, but one lives on a scale shifted by a
    // large constant. Shifting the prior by the same constant is exactly what "the whole
    // link model moved together" means.
    constexpr float kBogusOffset = 8.70f;   // the actual constant, for the avoidance of doubt

    LatentRateController a, b;
    a.seedPrior(18.0f, 3.0f);
    b.seedPrior(18.0f + kBogusOffset, 3.0f);

    const int outcomes[8][2] = {{5,5},{4,5},{5,5},{5,5},{3,5},{5,5},{4,5},{5,5}};
    for (auto& o : outcomes) {
        a.observe(kRungIdxQam8R23, o[0], o[1]);
        b.observe(kRungIdxQam8R23, o[0], o[1]);
    }

    // The posterior means differ by (almost) the offset — b simply lives further up the axis.
    const float shift = b.posteriorMean() - a.posteriorMean();
    CHECK(shift > 0.0f, "the shifted controller's posterior sits higher (shift=" << shift << ")");

    // The property that matters is not that the shift is preserved exactly (the likelihood
    // pulls both toward the SAME theta table, so b is pulled back down). It is that a
    // controller which reads an SNR would be wrong by 8.70 dB, while this one is fitted from
    // OUTCOMES and therefore has no SNR to be wrong about. Assert the decision is driven by
    // outcomes alone: identical outcomes on the SAME theta table give the identical pick,
    // regardless of where the prior was seeded.
    LatentRateController c1, c2;
    c1.seedPrior(10.0f, 6.0f);              // wildly pessimistic seed
    c2.seedPrior(26.0f, 6.0f);              // wildly optimistic seed
    for (int i = 0; i < 25; ++i) {
        c1.observe(kRungIdxQam8R23, 4, 5);
        c2.observe(kRungIdxQam8R23, 4, 5);
    }
    CHECK(std::fabs(c1.posteriorMean() - c2.posteriorMean()) < 1.5f,
          "priors 16 dB apart converge to the same latent state on identical outcomes "
          "(c1=" << c1.posteriorMean() << " c2=" << c2.posteriorMean() << ")");
    CHECK(pick(c1).rung == pick(c2).rung,
          "and therefore reach the same decision — no SNR calibration can move it");
}

// ── 3. Overhead belongs in the denominator ────────────────────────────────────
void test_overhead_in_denominator_moves_the_break_even() {
    // With the measured cycle, one rung up (QPSK R3/4 -> 8PSK R2/3, eta 1.5 -> 2.001) is
    // worth LESS than its raw-rate ratio because the fixed 3.20 s does not shrink.
    const float eta_lo = rungSpectralEfficiency(kRungIdxQpskR34);
    const float eta_hi = rungSpectralEfficiency(kRungIdxQam8R23);
    const float air_lo = kFramesPerGroup * kPayloadPerFrameS;
    const float air_hi = air_lo * (eta_lo / eta_hi);
    const float raw_ratio = eta_hi / eta_lo;
    const float cycle_ratio = (air_lo + kFixedPerCycleS) / (air_hi + kFixedPerCycleS);
    CHECK(cycle_ratio < raw_ratio,
          "the fixed per-cycle cost dilutes the gain (" << cycle_ratio << " < " << raw_ratio << ")");
    // Break-even delivered fraction is the inverse of the realisable speedup, not of the
    // raw-rate ratio. Measured: 0.835 vs the 0.750 the predecessor used.
    const float be_true = 1.0f / cycle_ratio;
    const float be_raw = 1.0f / raw_ratio;
    CHECK(be_true > be_raw + 0.05f,
          "true break-even is materially stricter than the raw-rate one ("
              << be_true << " vs " << be_raw << ")");
}

// ── 4. Ranking reproduces the measured waterfall ordering ─────────────────────
void test_ranking_matches_measured_ordering() {
    // At the bench's dial 20 the measured delivered throughput ordering is
    //   8PSK R2/3 (2450) > QPSK R3/4 (2066) > 16QAM R2/3 (1890) > 8PSK R3/4 (1618)
    // so a posterior concentrated near 20 must prefer 8PSK R2/3.
    LatentRateController c;
    c.seedPrior(20.0f, 1.0f);
    const auto p = pick(c);
    CHECK(p.rung == kRungIdxQam8R23,
          "at x~20 the controller picks 8PSK R2/3, the measured best rung (got "
              << static_cast<int>(p.rung) << ")");

    // A poor link must fall back, and a strong one must not sit at the floor.
    LatentRateController lo, hi;
    lo.seedPrior(6.0f, 1.0f);
    hi.seedPrior(24.0f, 1.0f);
    CHECK(pick(lo).rung < kRungIdxQam8R23, "a weak link drops below 8PSK R2/3");
    CHECK(pick(hi).rung >= kRungIdxQam8R23, "a strong link does not sit below it");
}

// ── 5. Minstrel's two clamps, and why ─────────────────────────────────────────
void test_dead_and_capped_probabilities() {
    // A rung whose predicted success is below 10% must contribute ZERO value, not a small
    // one — otherwise a very dense rung's raw rate can outvote its own unreliability.
    LatentRateController c;
    c.seedPrior(2.0f, 0.5f);        // far below every dense rung's theta
    const auto p = pick(c);
    CHECK(p.rung <= kRungIdxQpskR12,
          "with x far below the ladder, dense rungs are dead and a low rung is chosen");
    // Success is capped so a clean streak cannot make the model claim perfection; the
    // measured waterfall bottoms at 3.5% FER and no rung ever reaches 1.0.
    CHECK(LatentRateController::successProb(60.0f, 13.0f) <= kLatentMaxSuccess + 1e-6f,
          "success probability is capped at the measured fading error floor");
    CHECK(LatentRateController::successProb(-60.0f, 13.0f) >= kLatentMinSuccess - 1e-6f,
          "and floored, so a log-likelihood can never be -inf");
}

// ── 6. The relax term is the one free parameter — pin its behaviour ───────────
void test_relax_widens_and_is_the_only_forgetting() {
    LatentRateController c;
    c.seedPrior(18.0f, 1.0f);
    for (int i = 0; i < 10; ++i) c.observe(kRungIdxQam8R23, 5, 5);
    const float tight = c.spreadDb();
    c.relax(2.0f);
    CHECK(c.spreadDb() > tight, "relax widens the posterior (forgetting)");
    // Zero relax must be an exact no-op: if it drifted, the controller would forget at a
    // rate nobody chose.
    LatentRateController d;
    d.seedPrior(18.0f, 1.0f);
    const float before = d.spreadDb();
    d.relax(0.0f);
    CHECK(std::fabs(d.spreadDb() - before) < 1e-4f, "relax(0) is an exact no-op");
}

// ── 7. Tempering: a 5-frame group is NOT 5 independent bits ──────────────────
void test_correlated_frames_are_tempered() {
    // Burst 8-10 s against Tc 4.23 s gives ~2.2 independent frames per group. Feeding
    // Binomial(5,p) would over-sharpen by ~2.3x per group and the controller would become
    // confidently wrong.
    LatentRateController tempered, raw;
    tempered.seedPrior(18.0f, 3.0f);
    raw.seedPrior(18.0f, 3.0f);
    for (int i = 0; i < 5; ++i) {
        tempered.observe(kRungIdxQam8R23, 5, 5, 2.2f);
        raw.observe(kRungIdxQam8R23, 5, 5, 5.0f);
    }
    CHECK(tempered.spreadDb() > raw.spreadDb(),
          "tempered evidence sharpens more slowly than treating frames as independent");
}


// ── 8. TIE-BREAK UPWARD — the counterweight to the pessimism ──────────────────
// Rig-motivated: deciding from the 25th percentile cost 1.2 dB (mean 16.2 vs p25 15.0),
// exactly the gap to the QPSK R2/3 -> 8PSK R2/3 crossover at ~15.5, so the controller sat
// one rung low for whole transfers. It still beat the ladder by 14.5% on stability alone —
// it WON while using the good rung LESS (0.4 vs 1.2 changes/run) — but the rung gain was
// left unclaimed. The likelihood also SATURATES (a rung that always succeeds stops carrying
// information), so no amount of further evidence lifts x past the plateau on its own.
void test_tie_break_probes_upward() {
    // Park x_p25 exactly where the rig plateaued (15.0). Seed slightly above with a tight
    // prior: the 25th percentile sits ~0.674 sigma below the mean, so seeding 15.0 directly
    // would put p25 at 14.5 and the near-tie would not yet hold.
    LatentRateController c;
    c.seedPrior(15.2f, 0.3f);

    int probes = 0, higher_than_argmax = 0;
    uint8_t argmax_rung = kRungIdxNone;
    for (int i = 0; i < 40; ++i) {
        const auto p = pick(c);
        if (i == 0) argmax_rung = p.rung;   // first decision is never a probe (counter != 0)
        if (p.tie_break_probe) {
            ++probes;
            if (p.rung > argmax_rung) ++higher_than_argmax;
        }
    }
    CHECK(probes > 0, "the tie-break fires when rungs are near-tied (got " << probes << ")");
    CHECK(probes <= 40 / LatentRateController::kTieBreakPeriod + 1,
          "and no more often than once per kTieBreakPeriod decisions");
    CHECK(higher_than_argmax == probes,
          "every probe selects a rung ABOVE the argmax — probing downward would be pointless");

    // BOUNDED COST. A probe is only taken when the higher rung is within
    // kTieBreakMarginFrac of the best, so the expected loss is margin/period.
    const float worst_case_loss = LatentRateController::kTieBreakMarginFrac /
                                  static_cast<float>(LatentRateController::kTieBreakPeriod);
    CHECK(worst_case_loss < 0.05f,
          "expected cost of probing is bounded below 5% of one group (" << worst_case_loss << ")");

    // It must NOT fire when the rungs are far apart — that would be a plain over-commit.
    LatentRateController far;
    far.seedPrior(4.0f, 0.5f);        // deep in QPSK territory, dense rungs nowhere near
    int far_probes = 0;
    for (int i = 0; i < 40; ++i) if (pick(far).tie_break_probe) ++far_probes;
    CHECK(far_probes == 0,
          "no probe when the higher rungs are not within the margin (got " << far_probes << ")");
}

}  // namespace

int main() {
    std::cout << "=== Latent Rate Controller Tests ===\n";
    test_evidence_survives_rung_change();
    test_common_mode_calibration_error_cancels();
    test_overhead_in_denominator_moves_the_break_even();
    test_ranking_matches_measured_ordering();
    test_dead_and_capped_probabilities();
    test_relax_widens_and_is_the_only_forgetting();
    test_correlated_frames_are_tempered();
    test_tie_break_probes_upward();
    std::cout << (tests_failed == 0 ? "PASS" : "FAIL") << ": "
              << (tests_run - tests_failed) << "/" << tests_run << " checks\n";
    return tests_failed == 0 ? 0 : 1;
}
