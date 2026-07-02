#pragma once

// Phase 5c — BER-driven per-block rate adaptation (design §14.36).
//
// Pure decision state machine: feed it one burst-group's normalized decode
// "headroom" (quality in [0,1], 0 = the group failed to decode) and the rate
// that group was sent at; it returns the rate to use for the NEXT burst.
//
// It runs on the RECEIVER (which is the only station that sees channel quality —
// half-duplex: the sender is deaf while transmitting). The receiver stamps the
// returned rate onto the GROUP_ACK; the sender obeys it on the next burst.
//
// Steering signal is pre-FEC BER / iteration headroom, NOT SNR (SNR lies on a
// frequency-selective channel — see §14.33) and NOT post-FEC BER (binary, no
// graduation). The metric->quality mapping lives at the measurement site; this
// class only owns the climb/drop policy.
//
// Policy (asymmetric, because costs are asymmetric — a failed burst loses the
// whole burst + a retx, while running one rung slow costs only a little rate):
//   * the steering quality is EMA-SMOOTHED (sustained channel state, not a single
//     transient group) — this is the churn fix (2026-06-09). The pre-smoothing
//     policy dropped on ONE bad sample; on a fading channel every fade is a NACK,
//     so it ratcheted monotonically to R1/4 and never climbed back (each fade dropped
//     a rung while a climb needed 3 consecutive clean groups). On the production
//     tone-burst path `quality` is binary ack(1.0)/nack(0.0) = the FER signal, so a
//     single fade must NOT move the rate — only a sustained run of NACKs (ARQ actually
//     losing) should.
//   * DOWN:  ema_quality < drop_below            -> step down one rung
//   * UP:    ema_quality >= climb_above for K     -> step up one rung
//   * hysteresis gap (drop_below << climb_above) + the EMA inertia + the K-streak +
//     reset-to-midpoint after any change kill thrash.
//
// No PHY/audio/protocol dependencies — unit-tested in tests/test_rate_controller.cpp.

#include "ultra/types.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace ultra {
namespace protocol {

class RateController {
public:
    struct Config {
        // quality in [0,1]: 1 = lots of decode headroom, 0 = group failed.
        float drop_below = 0.25f;   // below this -> step down immediately
        float climb_above = 0.70f;  // at/above this for climb_streak groups -> step up
        // Climbs are deliberately SLOWER than drops. History: 2 -> 3 (2026-05-28)
        // after the g4+adapt seed-2 thrash — but that was PRE-EMA (the raw quality
        // drove the policy). 3 -> 2 (2026-07-02, fade-riding ladder): the ladder now
        // RIDES the Good-channel fade cycle (~10-20 s crests at ~8 s group cadence),
        // so reaction within ~2 ACKed groups is the requirement, and the 2026-06-09
        // EMA + reset-to-midpoint already supplies the sustained-evidence inertia the
        // old 3-streak existed for (post-change the EMA needs ~2 more groups to
        // re-reach climb_above, so an effective climb still needs ~4 groups of clean
        // evidence end-to-end; the ssthresh ceiling below still suppresses the
        // bounce-back-into-a-failed-rung oscillation). Env ULTRA_RATE_CLIMB_STREAK
        // [1..16] overrides for sweeps.
        int climb_streak = 2;
        // EMA weight for the quality low-pass (the churn fix). alpha=0.4 => a single
        // bad group only pulls ema down ~40% (1.0 -> 0.6, still above drop_below), so a
        // transient fade does NOT drop the rung; ~3 consecutive bad groups are needed to
        // cross drop_below. Lower alpha = more inertia (slower, steadier); higher = twitchier.
        float ema_alpha = 0.4f;
        // ssthresh re-probe (2026-06-11): after a DROP caps the ceiling just below the rung that
        // failed, the controller may climb back UP to (and past) that rung only after this many
        // climb-eligible events while pinned at the ceiling. With climb_streak=3 that is ~6 good
        // groups per re-probe — so a rung that keeps failing on a fading channel is retried
        // occasionally, not every 3 groups. This killed the R3/4<->R5/6 oscillation that, ungated,
        // thrashed the rate ~15x in one transfer (Good@20 seed 7/42) and burned the whole budget.
        // R5/6 is now retired from the ladder (the top rung is R3/4, the Good@20 sweet spot), so
        // this guard is mostly inert on the default ladder — it still protects the R2/3<->R3/4
        // boundary on a fading channel that can't hold R3/4.
        int ceiling_reprobe_climbs = 2;
        // Ordered ladder of SUPPORTED rates, lowest throughput first. If left empty
        // the controller fills it with the production OFDM ladder (skips the
        // unsupported R1_3/R7_8 enum holes, and R5_6 — retired 2026-06-17, see ctor).
        std::vector<CodeRate> ladder;
    };

    RateController() : RateController(Config{}) {}
    explicit RateController(Config cfg) : cfg_(std::move(cfg)) {
        // Sweep knob for the climb reaction time (see climb_streak above). Read here so
        // every construction site (Connection) honors it; tests construct with env unset.
        if (const char* e = std::getenv("ULTRA_RATE_CLIMB_STREAK")) {
            const int n = std::atoi(e);
            if (n >= 1 && n <= 16) cfg_.climb_streak = n;
        }
        if (cfg_.ladder.empty()) {
            // R5/6 RETIRED from the auto ladder (2026-06-17). It was added (2026-05-28)
            // as a "toward-3000" climb target above R3/4, but multi-anchor measurement
            // (docs/RATE_LADDER_ANCHORS.md): QPSK R5/6 Good@20 = 1480 bps / 33% frame
            // damage / it_max 17 — it LOSES to R3/4 (1630 bps / 8%). 17% FEC redundancy
            // sits BELOW Good's ~23% fade-erasure → the rung is under the reliability
            // cliff and the raw-rate gain is eaten by resends. It also forced the
            // ssthresh machinery below to exist purely to suppress the R3/4<->R5/6
            // oscillation that burned the whole airtime budget (Good@20 seed 7/42).
            // The top auto rung is now R3/4 (the measured Good@20 sweet spot); R5_6 is
            // still a valid enum + LDPC rate, reachable only as an explicit
            // ULTRA_FORCE_DATA_RATE probe. The next throughput rung above R3/4 is a
            // *modulation* step (QAM16 R2/3), handled in the connection's adaptive
            // layer, not a thinner QPSK code.
            cfg_.ladder = {CodeRate::R1_4, CodeRate::R1_2, CodeRate::R2_3,
                           CodeRate::R3_4};
        }
        ceiling_idx_ = static_cast<int>(cfg_.ladder.size()) - 1;  // start unrestricted
    }

    // Feed one decoded group's outcome; returns the rate for the NEXT burst.
    // `current` is the rate the just-decoded group was sent at.
    CodeRate update(CodeRate current, float quality) {
        const int idx = ladderIndex(current);
        if (idx < 0) {
            // current rate isn't on the ladder — don't adapt, just pass it through.
            good_streak_ = 0;
            return current;
        }
        quality = std::clamp(quality, 0.0f, 1.0f);

        // Low-pass the per-group quality into a SUSTAINED channel estimate. A single
        // transient fade (one NACK = 0.0 on the binary path) only dents the EMA; it
        // takes a run of bad groups to cross drop_below. This is the churn fix.
        ema_quality_ = ema_initialized_
                           ? cfg_.ema_alpha * quality + (1.0f - cfg_.ema_alpha) * ema_quality_
                           : quality;
        ema_initialized_ = true;

        const int top = static_cast<int>(cfg_.ladder.size()) - 1;

        if (ema_quality_ < cfg_.drop_below) {
            good_streak_ = 0;
            const int next = std::max(0, idx - 1);
            if (next != idx) {
                // ssthresh: rung `idx` just proved too aggressive for the channel. Cap the ceiling
                // just below it so the climb branch won't bounce straight back into it; the ceiling
                // re-probes upward only after a sustained good run (below). A rung that keeps
                // failing is thus retried ~once per (ceiling_reprobe_climbs x climb_streak) good
                // groups instead of every climb_streak — the fix for the R3/4<->R5/6 oscillation.
                ceiling_idx_ = std::max(0, idx - 1);
                reprobe_credit_ = 0;
                resetSmoothingAfterChange();
            }
            return cfg_.ladder[static_cast<size_t>(next)];
        }
        if (ema_quality_ >= cfg_.climb_above) {
            if (++good_streak_ >= cfg_.climb_streak) {
                good_streak_ = 0;
                if (idx + 1 <= ceiling_idx_) {
                    // headroom below the ssthresh ceiling — climb normally.
                    reprobe_credit_ = 0;
                    const int next = std::min(top, idx + 1);
                    if (next != idx) resetSmoothingAfterChange();
                    return cfg_.ladder[static_cast<size_t>(next)];
                }
                // Pinned at the ceiling but the channel is comfortable — earn re-probe credit, and
                // only after enough of it lift the ceiling one rung and climb (cautiously retry a
                // rung that previously failed). A STICKY ceiling (set by an escape-drop) never
                // re-probes — see noteRungFailed.
                if (!ceiling_sticky_ && ceiling_idx_ < top &&
                    ++reprobe_credit_ >= cfg_.ceiling_reprobe_climbs) {
                    reprobe_credit_ = 0;
                    ceiling_idx_ = std::min(top, ceiling_idx_ + 1);
                    const int next = std::min(top, idx + 1);
                    if (next != idx) resetSmoothingAfterChange();
                    return cfg_.ladder[static_cast<size_t>(next)];
                }
                return current;  // capped at the ceiling
            }
            return current;  // comfortable, but not enough consecutive good yet
        }
        // mid-zone: decoding fine but no margin to climb — hold, drop climb credit.
        good_streak_ = 0;
        return current;
    }

    void reset() {
        good_streak_ = 0;
        ema_quality_ = 0.0f;
        ema_initialized_ = false;
        ceiling_idx_ = static_cast<int>(cfg_.ladder.size()) - 1;
        reprobe_credit_ = 0;
        ceiling_sticky_ = false;
    }
    int climbStreak() const { return good_streak_; }
    float emaQuality() const { return ema_quality_; }
    // Current ssthresh ceiling rung (highest rate the controller will climb to right now).
    CodeRate ceilingRate() const { return cfg_.ladder[static_cast<size_t>(ceiling_idx_)]; }
    const Config& config() const { return cfg_; }

    // One rung MORE ROBUST than `r` (lower throughput), or `r` itself if already at the floor /
    // off-ladder. Used by the connection's stuck-frame escape-drop.
    CodeRate moreRobustRung(CodeRate r) const {
        const int idx = ladderIndex(r);
        if (idx <= 0) return r;  // floor or off-ladder
        return cfg_.ladder[static_cast<size_t>(idx - 1)];
    }
    bool isAtFloor(CodeRate r) const { return ladderIndex(r) <= 0; }

    // EXTERNAL ssthresh signal: a rung was forced down out-of-band (the stuck-frame escape-drop
    // in the connection, which bypasses update()). Cap the ceiling just below the failed rung so
    // the controller does not immediately climb the rate back into the fade that just killed a
    // frame — same effect a normal update()-driven drop has, but triggered from the ARQ/tick path.
    void noteRungFailed(CodeRate failed_rung) {
        const int idx = ladderIndex(failed_rung);
        if (idx < 0) return;
        ceiling_idx_ = std::min(ceiling_idx_, std::max(0, idx - 1));
        reprobe_credit_ = 0;
        // STICKY: an escape-drop means a frame literally DIED-but-for-the-escape at this rung on a
        // fading channel. Unlike a normal update()-driven drop, do NOT re-probe back up into it —
        // R1/4 always looks great (q~1.0) so the re-probe would climb straight back and re-stall,
        // oscillating R1/4<->R1/2 forever (Moderate@18). Stay robust for the rest of the connection
        // (reset() clears it); reliability beats reclaiming a rung that just killed a frame.
        ceiling_sticky_ = true;
    }

    // Convenience: map a successful-decode iteration count to a quality in [0,1].
    // 0 iters = max headroom (1.0); at/above max_iters or a failed decode = 0.0.
    // (v1 metric; refine to 1 - prefec_ber/correctable_ber(rate) later, §14.36.)
    static float qualityFromIterations(bool decoded_ok, int iterations,
                                       int max_iterations) {
        if (!decoded_ok || max_iterations <= 0) return 0.0f;
        const float frac = static_cast<float>(iterations) /
                           static_cast<float>(max_iterations);
        return std::clamp(1.0f - frac, 0.0f, 1.0f);
    }

private:
    int ladderIndex(CodeRate r) const {
        for (size_t i = 0; i < cfg_.ladder.size(); ++i)
            if (cfg_.ladder[i] == r) return static_cast<int>(i);
        return -1;
    }

    // After a rung actually changes, forget the old rung's quality history and start
    // neutral (the threshold midpoint) so neither bound is immediately within reach —
    // the new rung must earn fresh evidence before the next move (hysteresis).
    void resetSmoothingAfterChange() {
        ema_quality_ = 0.5f * (cfg_.drop_below + cfg_.climb_above);
        ema_initialized_ = true;
    }

    Config cfg_;
    int good_streak_ = 0;
    float ema_quality_ = 0.0f;
    bool ema_initialized_ = false;
    // ssthresh ceiling: the highest ladder index the controller will climb to right now. Lowered
    // on a drop (to one below the rung that failed), re-probed upward after a sustained good run.
    int ceiling_idx_ = 0;            // set to top-of-ladder in the constructor / reset()
    int reprobe_credit_ = 0;         // climb-eligible events accrued while pinned at the ceiling
    bool ceiling_sticky_ = false;    // an escape-drop set the ceiling — never re-probe back up
};

}  // namespace protocol
}  // namespace ultra
