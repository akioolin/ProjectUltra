#pragma once
// ============================================================================
// GOODPUT-MAXIMIZING RATE CONTROLLER  (ULTRA_GOODPUT_RATE, default OFF)
// ============================================================================
//
// WHY THIS EXISTS — the measurement, 2026-07-30, IONOS rig at MPG@20
// -------------------------------------------------------------------
// Interleaved same-epoch A/B, 50 KB, both arms CRC-clean:
//
//     pair | forced 8PSK R2/3 | auto ladder | delta  | ladder mode changes
//        1 |      2.06 kbps   |  1.86 kbps  | +10.8% |  5
//        2 |      1.59 kbps   |  1.44 kbps  | +10.4% |  8
//
// The epoch roughened ~23% between pairs (both arms fell together) and the RELATIVE
// gain held. The forced arm never demoted — not even on the rough epoch — and still
// won. So 8PSK R2/3 was holdable throughout and EVERY demote the ladder made was
// unnecessary. Adaptation cost ~10.6%, and cost more where it churned more.
//
// Sender-clock accounting the same day showed why this is the whole prize: airtime is
// 81-84% of wall clock and runs at 90-99% of the rung's OWN raw ceiling, so airtime is
// not being wasted on retransmission. The loss is which rung is selected.
//
// WHAT IS WRONG WITH THE EXISTING CONTROLLER
// ------------------------------------------
// updateRxAuthorityCommand decides from selectCoherentOFDM(snr_avg, fading) — an
// SNR-versus-anchor-table comparison — and then layers ~10 correctives on top (censored
// ring feed, sticky class, asymmetric class persistence, decode-evidence veto,
// two-crater rule, goodput regrade, dense fast demote, EMA hold, ratcheting penalties,
// climb dwell). Every one of those exists to suppress a wrong answer from the primary
// variable. Two independent defects:
//
//   1. WRONG TIMESCALE. At MPG the Doppler is 0.1 Hz, so Tc ~ 4 s. The controller acts
//      on one or two groups (~10-20 s), which is INSIDE the Rayleigh process it is
//      reacting to. CLAUDE.md: "you always equalize with a past estimate". A rate loop
//      cannot track fast fading; it can only chase it. Adaptation on HF must track the
//      SLOW component (propagation, QSB over minutes), so the averaging window must sit
//      ABOVE the coherence time and BELOW the propagation timescale.
//
//   2. WRONG DECISION VARIABLE. A crater is read as "the rung is too high". On a
//      Rayleigh channel that is a category error — CLAUDE.md: "Fading loss is
//      irreducible ... 'zero retx on fading' is unphysical". Every rung craters. The
//      right rung is not the one that stops cratering, it is the one that maximizes
//      delivered goodput. docs/FADING_ANCHOR_MEASUREMENT_2026_07_26.md measures exactly
//      that and says 8PSK R2/3 wins on Good@20 at 2450 bps INCLUDING its crater rate,
//      beating QPSK R3/4's 2066. The ladder had the answer and walked away from it.
//
// WHAT THIS DOES INSTEAD
// ----------------------
// Decides on MEASURED DELIVERED GOODPUT over a coherence-derived window, and never
// reads an SNR. That second property is not incidental: the SNR chain has been the
// single largest source of wrong answers in this project (the +8.70 dB anchor offset,
// the guard-bin noise over-read, the 51.4%-FER Good anchor). A controller that never
// consumes an SNR cannot be wrong about one.
//
// The airtime derivation is the one already documented at
// waveform_selection.hpp::goodputBreakEvenDeliveredFraction: on rung r with delivered
// fraction f, a frame costs airtime ~1/(f*eta_r), so the larger (f*eta) wins. That
// function is currently used only to REGRADE a single cratered group. Here the same
// quantity becomes the PRIMARY decision, measured over a window.
//
// The HOLD rule is a genuine guarantee, not a heuristic. Since f_below <= 1 always,
// eta_below is an upper bound on the lower rung's goodput. So
//
//     f_cur * eta_cur  >=  eta_below      =>      NO rung below can beat us
//
// is sound: when it holds, demoting is provably wrong. The converse is not sound, so
// falling below the bound only makes a demote PERMITTED — it must then also clear
// persistence and the switch cost.
//
// Climbing cannot be derived this way (f_above is unobservable until tried), so it is
// an explicit bet: optimism when the current rung shows headroom, with exponential
// backoff per rung on a failed probe. That is standard optimism-under-uncertainty, and
// the backoff is what stops the F125/F126 oscillation from reappearing.
//
// SCOPE / SAFETY. Pure logic, no I/O, no clock. Default OFF. When off, nothing in this
// header is reachable from a production path.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "connection_policy.hpp"
#include "waveform_selection.hpp"

namespace ultra {
namespace protocol {

inline bool goodputRateControllerEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("ULTRA_GOODPUT_RATE");
        return e != nullptr && e[0] == '1' && e[1] == '\0';
    }();
    return on;
}

// Coherence time of a Rayleigh channel, Clarke/Rappaport geometric-mean definition
// Tc ~ 0.423 / f_d. The numerator is SHARED with connection_policy.hpp rather than
// re-declared — a second copy of a physical constant is how the two drift apart.
inline float coherenceTimeSeconds(float doppler_hz) {
    if (!(doppler_hz > 0.0f)) return 0.0f;
    return connection_policy::kClarkeCoherenceNumerator / doppler_hz;
}

// Groups needed for the delivered-fraction estimate. Two requirements, and the window
// must satisfy BOTH:
//
//   (a) span enough independent fades that the estimate is not one fade's luck, and
//   (b) contain enough GROUPS that the delivered-fraction estimate itself is stable.
//
// The first version only imposed (a), via kIndependentFades / Tc. That is the right
// reasoning for estimating a mean over a continuous process, but the quantity here is a
// per-group delivered fraction — a binomial over ~5 frames — so the estimator's variance
// is driven by the NUMBER OF GROUPS, not by how many fades each group happens to span.
// Faster fading must not buy a thinner window. On the rig it did exactly that: a fading
// index reading Moderate set f_d = 0.5 Hz -> Tc 0.85 s -> one group -> clamped to 2, and
// the controller started committing rung changes on ~10 frames of evidence
// ("window=2g/9.5s doppler=0.50" immediately before a climb into a rung that measures
// 51.4% FER). kMinWindow is therefore a STATISTICAL floor, not a latency compromise.
inline int windowGroupsForCoherence(float doppler_hz, float group_seconds) {
    constexpr float kIndependentFades = 8.0f;
    constexpr int kMinWindow = 4;   // (b): ~20 frames — below this the fraction is noise
    constexpr int kMaxWindow = 8;   // above this a real propagation change is missed
    const float tc = coherenceTimeSeconds(doppler_hz);
    if (tc <= 0.0f || !(group_seconds > 0.0f)) return kMinWindow;
    const float need_s = kIndependentFades * tc;
    const int w = static_cast<int>(std::ceil(need_s / group_seconds));
    return std::clamp(w, kMinWindow, kMaxWindow);
}

// First ENABLED rung strictly above idx, or kRungIdxNone. Mirrors
// snapRungIndexDownToEnabled — disabled anchor rows are HOLES and raw arithmetic on
// rung indices is a known deadlock source (F145).
inline uint8_t snapRungIndexUpToEnabled(uint8_t idx) {
    for (int r = static_cast<int>(idx) + 1; r < static_cast<int>(kRungIdxCount); ++r) {
        const CoherentPick p = coherentRungFromIndex(static_cast<uint8_t>(r));
        if (coherentRungLocallyEnabled(p.mod, p.rate)) return static_cast<uint8_t>(r);
    }
    return kRungIdxNone;
}

class GoodputRateController {
public:
    static constexpr int kMaxWindow = 8;
    // A group delivering at or above this has headroom worth probing upward with.
    // Not a fitted value: it is "the ARQ is absorbing essentially nothing", i.e. the
    // rung is not the binding constraint.
    static constexpr float kHeadroomFraction = 0.95f;
    // Consecutive TOTAL craters (0 of N) that bypass the window. A partial crater is
    // ARQ doing its job; two consecutive ZERO-delivery groups is a link that is not
    // working, and waiting a full window there is expensive.
    static constexpr int kCatastrophicCraters = 2;
    // Groups a climb probe is judged over. Two is the minimum that can distinguish a
    // rung failing systematically from one unlucky fade, and it bounds the tuition a
    // bad probe costs to ~20 s instead of a full window.
    static constexpr int kProbeGroups = 2;
    // Measured 2026-07-30 on the rig: turnaround 1.85 s + post-switch re-acquisition.
    // A switch must pay for itself before it is worth making.
    static constexpr float kSwitchCostSeconds = 3.0f;

    struct Decision {
        uint8_t rung = kRungIdxNone;
        const char* reason = "hold";
        bool changed = false;
    };

    // The window size is LATCHED for the duration of a rung's evidence window. Applying
    // a freshly recomputed size every group corrupted the ring on the rig (2026-07-30
    // run 1 logged "n=2/3" — the size shrank from 4 to 3 mid-fill because the measured
    // group cadence moved, so next_ % window_ then aliased occupied slots). A window is
    // a measurement; its length cannot change while it is being filled.
    void configure(int window_groups, float group_seconds) {
        const int w = std::clamp(window_groups, 1, kMaxWindow);
        group_seconds_ = (group_seconds > 0.0f) ? group_seconds : 10.0f;
        if (count_ == 0) window_ = w;      // between windows: adopt freely
        else pending_window_ = w;          // mid-window: defer to the next reset
    }

    // One group outcome. delivered_fraction in [0,1]; negative means "not measurable"
    // and is ignored rather than guessed at.
    void observe(uint8_t rung, float delivered_fraction) {
        if (delivered_fraction < 0.0f) return;
        if (rung != cur_rung_) {   // rung changed: the old window describes a different rung
            cur_rung_ = rung;
            count_ = 0;
            next_ = 0;
            zero_streak_ = 0;
            if (pending_window_ > 0) { window_ = pending_window_; pending_window_ = 0; }
        }
        ring_[next_] = std::clamp(delivered_fraction, 0.0f, 1.0f);
        next_ = (next_ + 1) % static_cast<size_t>(window_);
        if (count_ < window_) ++count_;
        if (delivered_fraction <= 0.0f) ++zero_streak_;
        else zero_streak_ = 0;
    }

    // Fraction of the ladder-relative gain a switch must exceed to be worth its cost.
    float switchMargin() const {
        const float window_s = static_cast<float>(window_) * group_seconds_;
        return (window_s > 0.0f) ? (kSwitchCostSeconds / window_s) : 0.0f;
    }

    float windowedDeliveredFraction() const {
        if (count_ <= 0) return -1.0f;
        float s = 0.0f;
        for (int i = 0; i < count_; ++i) s += ring_[static_cast<size_t>(i)];
        return s / static_cast<float>(count_);
    }

    // CEILING: the highest rung the caller's own ladder considers plausible right now.
    // kRungIdxNone = unbounded (unit tests, or no ladder opinion available).
    //
    // WHY A PRIOR IS NEEDED (rig-measured 2026-07-30). The first design deliberately read
    // nothing from the anchor table, on the grounds that its VALUES have repeatedly been
    // wrong. That over-corrected. Optimism-with-backoff has no memory before its first
    // mistake, so a clean window climbs one rung EVERY window and the ladder is walked
    // upward until something breaks: the rig logged 2->3->4->5->8 in four consecutive
    // windows (~40 s), landing on 16QAM R2/3 — a rung docs/FADING_ANCHOR_MEASUREMENT
    // _2026_07_26.md measures at 51.4% FER on ITU Good. The probe reverted correctly on
    // f=0.450, but that tuition is paid on every transfer, and the overshoot continues
    // downward afterwards (8 mode changes, 1.42 kbps).
    //
    // The anchor table's THRESHOLDS may be mis-calibrated, but its structure encodes real
    // measured knowledge about which rungs are viable at all. Used as a CEILING it can
    // only constrain, never force — so the ladder's known failure mode (over-committing,
    // and under-using 8PSK R2/3) is not reintroduced: the controller may still sit
    // anywhere at or below the pick, which is exactly where the measured +10.6% lives.
    void setCeiling(uint8_t rung) { ceiling_ = rung; }

    Decision decide(uint8_t cur) {
        Decision d;
        d.rung = cur;
        if (cur == kRungIdxNone || cur >= kRungIdxCount) {
            d.reason = "no-rung";
            return d;
        }

        // ── Catastrophic fast path ────────────────────────────────────────────
        // The only case where waiting for the window is the wrong call.
        if (zero_streak_ >= kCatastrophicCraters) {
            const uint8_t below = snapRungIndexDownToEnabled(
                (cur > 0) ? static_cast<uint8_t>(cur - 1) : cur);
            if (below != kRungIdxNone && below < cur) {
                d.rung = below;
                d.reason = "catastrophic";
                d.changed = true;
                noteProbeFailed(cur);
                return d;
            }
        }

        const float eta_cur = rungSpectralEfficiency(cur);
        if (!(eta_cur > 0.0f)) {
            d.reason = "no-eta";
            return d;
        }

        // ── PROBE EVALUATION: a climb is judged against a MEASUREMENT, not a bound ──
        // A climb is the one decision that cannot be derived (f_above is unobservable
        // until tried). But once tried it becomes observable, and we still hold a
        // measurement of the rung we came from — so the probe is settled by a DIRECT
        // comparison rather than by waiting out a full window against a bound.
        //
        // This matters because the enabled ladder can contain rungs that are terrible
        // at the current SNR: 16QAM R2/3 measures 51.4% FER on ITU Good
        // (FADING_ANCHOR_MEASUREMENT_2026_07_26) yet is "enabled". Judging that probe on
        // a full 4-group window costs ~40 s of a ~265 s transfer. Two groups is enough
        // to see a rung failing half its frames, and the exponential backoff stops it
        // being re-tried on a loop.
        if (probe_active_ && count_ >= kProbeGroups && pre_probe_goodput_ > 0.0f) {
            float s = 0.0f;
            for (int i = 0; i < count_; ++i) s += ring_[static_cast<size_t>(i)];
            const float probe_goodput = (s / static_cast<float>(count_)) * eta_cur;
            if (probe_goodput < pre_probe_goodput_ * (1.0f + switchMargin())) {
                const uint8_t back = probe_from_;
                probe_active_ = false;
                probe_settled_ = false;
                if (back != kRungIdxNone && back < cur) {
                    noteProbeFailed(cur);
                    d.rung = back;
                    d.reason = "probe-reverted";
                    d.changed = true;
                    return d;
                }
            } else {
                // The probe is beating the rung it came from — bank it.
                probe_active_ = false;
                probe_settled_ = false;
                pre_probe_goodput_ = 0.0f;
                backoff_[cur] = 0;
            }
        }

        if (count_ < window_) {         // not enough evidence yet — never act on a partial window
            d.reason = "window-filling";
            return d;
        }

        const float f = windowedDeliveredFraction();

        // ── DEMOTE: only when the rung below provably wins by more than the
        //    switch cost. break_even = eta_below / eta_cur (already derived and
        //    unit-tested in waveform_selection.hpp).
        //
        //    SIGN (rig-caught 2026-07-30): demoting pays iff
        //        eta_below  >  f * eta_cur * (1 + margin)
        //    i.e.  f < break_even / (1 + margin). Dividing makes the switch cost make
        //    demotion HARDER, which is the whole intent. The first implementation
        //    MULTIPLIED, which made it EASIER and walked the ladder down: run 1 logged
        //    "f_window=0.800 break_even=0.750 reason=goodput-demote" — a demote from a
        //    rung that was winning by 5 points — and finished at 1.38 kbps against the
        //    ladder's 1.44. The direction of a cost term is not a detail.
        const float break_even = goodputBreakEvenDeliveredFraction(cur);
        if (break_even > 0.0f && f < break_even / (1.0f + switchMargin())) {
            const uint8_t below = snapRungIndexDownToEnabled(
                static_cast<uint8_t>(cur - 1));
            if (below != kRungIdxNone && below < cur) {
                d.rung = below;
                d.reason = "goodput-demote";
                d.changed = true;
                noteProbeFailed(cur);
                return d;
            }
        }

        // ── CLIMB: an explicit bet, gated on the TARGET rung's backoff.
        const uint8_t above = snapRungIndexUpToEnabled(cur);
        if (above == kRungIdxNone) {
            d.reason = "at-ceiling";
            return d;
        }
        // The ladder's prior bounds the search; measurement selects within it.
        if (ceiling_ != kRungIdxNone && above > ceiling_) {
            d.reason = "ladder-ceiling";
            return d;
        }
        // BACKOFF IS A PROPERTY OF THE TARGET (rig-caught 2026-07-30). The first version
        // checked backoff_[cur] — the rung being climbed FROM — while noteProbeFailed
        // recorded the failure against the rung being climbed INTO. The two never met,
        // so a failed probe placed no obstacle in front of a retry: run 3 logged the
        // SAME "idx 4 -> 5" climb three times and finished at 1.25 kbps with 9 mode
        // changes, churning worse than the ladder it replaces. A penalty recorded
        // against one index and read from another is not a weak penalty, it is no
        // penalty at all.
        if (backoff_[above] > 0) {
            --backoff_[above];
            d.reason = "climb-backoff";
            return d;
        }
        // The switch must pay for itself: the rung above must offer more raw rate than
        // the cost of getting there. This is what refuses the lateral 8PSK R2/3 ->
        // 16QAM R1/2 move (identical 2.0 bits/carrier, denser constellation).
        const float eta_above = rungSpectralEfficiency(above);
        if (!(eta_above > eta_cur * (1.0f + switchMargin()))) {
            d.reason = "gain-below-switch-cost";
            return d;
        }
        // HEADROOM, derived rather than asserted. The first version demanded >= 0.95 on
        // EVERY group in the window. On a Rayleigh channel that is close to unreachable
        // — CLAUDE.md: fading loss is irreducible — so the controller could essentially
        // only descend, which is what the rig measured (run 1: R3/4 -> R2/3 -> R1/2,
        // 1.38 kbps). The honest bar comes from the same airtime algebra as the demote:
        // climbing wins iff f_above * eta_above > f_cur * eta_cur, so the rung above
        // must deliver at least
        //     f_required = f_cur * eta_cur / eta_above
        // Betting is reasonable when that required fraction is one the LADDER ITSELF
        // considers sustainable for that rung — its own break-even. No new constant.
        const float f_required = f * eta_cur / eta_above;
        const float above_break_even = goodputBreakEvenDeliveredFraction(above);
        const float sustainable = (above_break_even > 0.0f) ? above_break_even : kHeadroomFraction;
        if (f_required > sustainable) {
            d.reason = "climb-bar-unmet";
            return d;
        }
        d.rung = above;
        d.reason = "goodput-climb";
        d.changed = true;
        probe_active_ = true;
        probe_settled_ = false;
        probe_from_ = cur;
        pre_probe_goodput_ = f * eta_cur;   // the measurement the probe must beat
        return d;
    }

    // Exponential backoff on the rung we just left, so a failed probe is not retried
    // immediately. This is what replaces the ratcheting dB penalties.
    void noteProbeFailed(uint8_t rung) {
        if (rung >= kRungIdxCount) return;
        const int prev = backoff_[rung];
        backoff_[rung] = std::min(32, (prev <= 0) ? 2 : prev * 2);
        probe_active_ = false;
        probe_settled_ = false;
        (void)probe_from_;
    }

    int windowGroups() const { return window_; }
    int observations() const { return count_; }

private:
    float ring_[kMaxWindow] = {};
    int window_ = 4;
    int pending_window_ = 0;
    float group_seconds_ = 10.0f;
    int count_ = 0;
    size_t next_ = 0;
    int zero_streak_ = 0;
    uint8_t cur_rung_ = kRungIdxNone;
    uint8_t ceiling_ = kRungIdxNone;
    int backoff_[kRungIdxCount] = {};
    bool probe_active_ = false;
    bool probe_settled_ = false;
    float pre_probe_goodput_ = 0.0f;
    uint8_t probe_from_ = kRungIdxNone;
};

}  // namespace protocol
}  // namespace ultra
