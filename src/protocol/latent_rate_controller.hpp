#pragma once
// ============================================================================
// LATENT-STATE RATE CONTROLLER  (ULTRA_LATENT_RATE, DEFAULT-ON since 2026-08-01)
// ============================================================================
//
// WHY THIS SHAPE — what every deployed rate controller does and we did not
// -----------------------------------------------------------------------
// Minstrel-HT (Linux mac80211 default), iwlwifi, LTE/NR OLLA and STANAG 5066's DRC all
// share one property we lacked:
//
//     THEY MAINTAIN A VALUE ESTIMATE FOR EVERY RATE AT ALL TIMES, AND THE DECISION IS A
//     STATELESS argmax OVER THAT VECTOR. Reliability enters as a clamped probability
//     INSIDE the value; it is never a penalty applied to the incumbent.
//
// Minstrel contains no dwell, no hysteresis, no ratchet, no penalty state and no
// crater rule. All of its stability lives in the ESTIMATOR; none in the ACTUATOR. Our
// RX-authority path is the exact inverse: a correct selector followed by ~10 actuator-side
// correctives which, measured over 135 rig verdicts, clamp 46% of decisions down a mean of
// 1.61 rungs (the ladder picks 8PSK R2/3 66% of the time; 26% survives to the wire).
//
// The predecessor, goodput_rate_controller.hpp, had the right OBJECTIVE and the wrong
// SHAPE: one ring buffer for the CURRENT rung, zeroed on every rung change, then 4-8 groups
// of "window-filling" before it may act. With 5-8 rung changes per transfer it is inert for
// most of a run — argmax over one entry is a thermostat. That, not statistics, is the most
// likely reason it measured a wash.
//
// WHY NOT JUST COPY MINSTREL
// --------------------------
// Minstrel's per-rate table is fed by probing 5-10% of packets, and its probe is nearly free
// (it rides a retry slot inside a TXOP that was going to be spent anyway). Neither holds here:
//   - Our feedback is ONE group ACK per ~9.5 s. A 10% probe budget is one probe per 95 s =
//     one refresh per 22 coherence times. Stale before use.
//   - A probe costs a WHOLE GROUP (~9.5 s of airtime), because we are half-duplex and
//     rung-committed for the burst.
//   - Frames inside a burst are CORRELATED: burst 8-10 s against Tc = 4.23 s gives
//     n_eff ~ 2.2 independent frames per group, not 5. A usable per-rung estimate then needs
//     ~20 groups ~ 190 s of EXCLUSIVE use per rung; ten rungs is 30-50 minutes. A transfer
//     is ~70 s. Per-rung independent statistics are structurally impossible here.
//
// So we take the cellular/HF route instead: populate all rungs from a SHARED low-dimensional
// model and spend ZERO dedicated probe groups.
//
// THE MODEL
// ---------
// One latent scalar x — the link's effective operating point in dB — with a per-rung
// threshold theta_r and a shared logistic slope:
//
//     P(frame decodes on rung r | x) = logistic(slope * (x - theta_r))
//
// Every group's SACK updates the posterior over x, and therefore updates the predicted
// success probability of ALL TEN RUNGS SIMULTANEOUSLY. Evidence SURVIVES a rung change:
// re-convergence after a switch is zero groups, against 4-8 groups of forced inertia before.
//
// THE PROPERTY THAT MATTERS MOST HERE
// -----------------------------------
// Because x is fitted from OUTCOMES rather than read from an SNR estimator, adding a
// constant to every theta_r is absorbed by x. COMMON-MODE CALIBRATION ERROR CANCELS EXACTLY.
// That retires, for rate selection, the entire class of defect that has cost this project
// the most: kOfdmLegacyAnchorScaleOffsetDb (+8.70 dB, itself a compatibility shim for an
// estimator bug that was already fixed), the guard-bin noise under-read, and the ~5.6 dB
// IONOS implementation loss. None of them can move the decision. Only the DIFFERENTIAL
// spacing theta_r - theta_s matters — which is precisely the quantity
// docs/FADING_ANCHOR_MEASUREMENT_2026_07_26.md measured directly.
//
// This controller therefore consumes NO SNR ESTIMATE AT ALL. It is the reason the offset can
// leave the rate path.
//
// SCOPE. Pure logic, no I/O, no clock, no RNG.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "waveform_selection.hpp"

namespace ultra {
namespace protocol {

// DEFAULT-ON since 2026-08-01. Opt out with ULTRA_LATENT_RATE=0, which restores the legacy
// SNR-anchor ladder and its corrective stack.
//
// EVIDENCE FOR THE FLIP:
//   - Rig A/B, 8 interleaved pairs at IONOS MPG@20: +14.5% mean, median +18.4%, 7/8
//     positive, 1.64 vs 1.45 kbps. Paired t = 2.92, df=7 -> p = 0.022. The ONLY result in
//     this campaign to clear p<0.05 (the others: +8.0% goodput ctl, -13.8% dwell, +20.7%
//     trust-pick, +26.2% bundle, +5.2% forced rung — none significant).
//   - Faithful gate PASSES on BOTH classes: Good@20 and Moderate@20, CRC clean.
//   - Moderate is the one that mattered, because theta_r was measured on ITU Good ONLY. It
//     works anyway: on Moderate the posterior settled to x ~ 11-12 against 15-17 on Good and
//     held QPSK R1/2-R2/3 instead of 8PSK. A worse channel simply produces a lower x, so the
//     argmax lands correctly without a per-class table. That is the common-mode absorption
//     doing real work, not merely cancelling a calibration constant.
//
// HONEST CAVEATS carried into the release:
//   - Significant by the t-test, NOT by the sign test (7/8, p=0.070). Established, not
//     settled.
//   - The tie-break probe (kTieBreakMarginFrac / kTieBreakPeriod) raises the mean slightly
//     and DOUBLES the variance (+16.5% sd 27.3% over 7 pairs, losing significance). It is
//     compiled in and fires, and its bimodal signature is documented in the CHANGELOG; if a
//     regression appears, that is the first thing to gate on posterior confidence.
inline bool latentRateControllerEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("ULTRA_LATENT_RATE");
        return !(e != nullptr && e[0] == '0' && e[1] == '\0');  // DEFAULT-ON
    }();
    return on;
}

// ── LINK MODEL ───────────────────────────────────────────────────────────────
//
// theta_r = the operating point at which rung r decodes half its frames, in the SAME
// arbitrary units as the latent x. The absolute scale is IRRELEVANT (it cancels); only the
// spacing carries information.
//
// MEASURED rungs come from the ITU-Good FER waterfall in
// docs/FADING_ANCHOR_MEASUREMENT_2026_07_26.md §1 (6 seeds x 24 frames x 5 SNR points),
// fitted as a logistic in dB over the 20-70% FER band — outside that band the curve flattens
// into the fading error floor, which a logistic cannot represent and must not be fitted to:
//
//     rung          fitted slope   theta      (research cross-check)
//     QPSK R3/4        0.403       13.0            12.5
//     8PSK R2/3        0.449       15.9            15.8
//     8PSK R3/4        0.148       20.5            20.1
//     16QAM R2/3       0.219       20.3            20.1
//
// DERIVED rungs (QPSK R1/4..R2/3) were not in that sweep. Their spacing is taken from the
// ladder's own within-QPSK anchor differences (R3/4 20.0, R2/3 15.0, R1/2 10.0 on Good —
// internally consistent, measured by measure_ack_fer sweeps), anchored to the MEASURED
// QPSK R3/4 theta. Any error common to all of them cancels; only their spacing matters, and
// that spacing is the measured part.
inline constexpr float kLatentSlopePerDb = 0.35f;  // shared; per-rung fits 0.15-0.45, and
                                                   // the tails are error-floor artefacts

inline float latentThetaForRung(uint8_t idx) {
    switch (idx) {
        case kRungIdxQpskR14: return -2.0f;   // derived: R1/2 - 5 dB (ladder spacing)
        case kRungIdxQpskR12: return  3.0f;   // derived: R3/4 - 10 dB
        case kRungIdxQpskR23: return  8.0f;   // derived: R3/4 - 5 dB
        case kRungIdxQpskR34: return 13.0f;   // MEASURED
        case kRungIdxQam8R23: return 15.9f;   // MEASURED
        case kRungIdxQam8R34: return 20.5f;   // MEASURED — and DOMINATED, see below
        case kRungIdxQam16R12: return 17.0f;  // derived: between 8PSK R2/3 and 8PSK R3/4
        case kRungIdxQam16R23: return 20.3f;  // MEASURED
        case kRungIdxQam16R34: return 26.0f;  // derived: unmeasured, deliberately pessimistic
        default: return 99.0f;
    }
}

// FADING ERROR FLOOR. On a Rayleigh channel no rung reaches P=1: deep nulls destroy frames
// regardless of margin (CLAUDE.md: "fading loss is irreducible"). The measured waterfall
// bottoms at 3.5% FER, so the model must cap success at 0.965 — a logistic that approaches
// 1.0 would let the posterior conclude "this rung never fails" from a clean run and then be
// unable to explain the first crater except by moving x a long way.
inline constexpr float kLatentMaxSuccess = 0.965f;
inline constexpr float kLatentMinSuccess = 0.035f;

// ── POSTERIOR ────────────────────────────────────────────────────────────────
class LatentRateController {
public:
    static constexpr int kBins = 61;          // 0.5 dB resolution over [-5, +25]
    static constexpr float kBinLo = -5.0f;
    static constexpr float kBinStep = 0.5f;

    // Minstrel's two decision constants, used verbatim and for its reasons:
    //   below kDead the rate is treated as having ZERO value, not merely low value —
    //     otherwise a 5%-success rung with a huge raw rate can win the argmax;
    //   above kCap the probability is clamped so a lucky clean streak cannot make a
    //     too-high rung look better than a rung that is genuinely reliable.
    static constexpr float kDeadProb = 0.10f;
    static constexpr float kCapProb = 0.90f;

    // TIE-BREAK UPWARD — the counterweight the pessimism above requires.
    //
    // Deciding from the 25th percentile is right (over-committing costs a cratered group
    // ~9.5 s; under-committing costs one group's rate delta) but it is NOT free: on the rig
    // it cost 1.2 dB (posterior mean 16.2 against x_p25 15.0), which was exactly the gap to
    // the QPSK R2/3 -> 8PSK R2/3 crossover at ~15.5. The controller therefore sat one rung
    // low for whole transfers and still beat the ladder by 14.5% on stability alone
    // (8PSK changes 0.4/run against baseline's 1.2 — it WON while using the good rung LESS).
    //
    // The second reason it sticks is structural: the likelihood SATURATES. QPSK R2/3
    // (theta 8.0) caps at p = kLatentMaxSuccess once x >= 17.5, after which a clean 5/5 group
    // carries no information at all — a rung that always succeeds tells you only "x is above
    // my threshold", never how far above. No amount of further evidence lifts x past that.
    //
    // So when the higher rung is predicted within kTieBreakMarginFrac of the best, take it
    // one decision in kTieBreakPeriod. This is Minstrel's INC bucket, and its expected cost
    // is bounded BY CONSTRUCTION: at most margin/period = 15%/4 = 3.75% of one group's
    // goodput, against unlocking a rung worth ~+20%. Crucially the probe is not wasted even
    // when it loses — the outcome updates the posterior, which is the only way to learn
    // about the saturated region at all.
    static constexpr float kTieBreakMarginFrac = 0.15f;
    static constexpr int kTieBreakPeriod = 4;

    // Pessimism. LTS uses min{sampled theta, posterior mean}; the asymmetry is real here too
    // — over-committing costs a cratered group (~9.5 s) while under-committing costs the
    // rate delta on one group. Decide from the 25th percentile of the posterior, not its
    // mean.
    static constexpr float kDecilePessimism = 0.25f;

    LatentRateController() { reset(); }

    void reset() {
        for (int i = 0; i < kBins; ++i) logp_[i] = 0.0f;
        normalise();
        have_prior_ = false;
    }

    // Seed from the connect-time pick. Minstrel starts from ignorance because at 2000 pkt/s
    // it can afford to; we get one observation per 9.5 s and cannot. sigma is the measured
    // connect-snapshot spread (+-2-3 dB, a single fade snapshot).
    void seedPrior(float x0, float sigma_db = 3.0f) {
        const float s2 = std::max(0.25f, sigma_db * sigma_db);
        for (int i = 0; i < kBins; ++i) {
            const float x = binX(i);
            logp_[i] = -0.5f * (x - x0) * (x - x0) / s2;
        }
        normalise();
        have_prior_ = true;
    }

    // One group outcome: k of M frames delivered on rung r.
    //
    // TEMPERING is not optional. Frames inside a burst are correlated (burst 8-10 s vs
    // Tc 4.23 s), so a 5-frame group carries ~2.2 independent bits, not 5. Feeding
    // Binomial(5,p) would over-sharpen the posterior by ~2.3x per group and the controller
    // would converge to a confident wrong answer and then refuse to move.
    void observe(uint8_t rung, int k, int M, float n_eff_per_group = 2.2f) {
        if (M <= 0 || rung >= kRungIdxCount) return;
        k = std::clamp(k, 0, M);
        const float theta = latentThetaForRung(rung);
        const float w = std::clamp(n_eff_per_group / static_cast<float>(M), 0.05f, 1.0f);
        for (int i = 0; i < kBins; ++i) {
            const float p = successProb(binX(i), theta);
            logp_[i] += w * (static_cast<float>(k) * std::log(p) +
                             static_cast<float>(M - k) * std::log(1.0f - p));
        }
        normalise();
        ++observations_;
    }

    // Widen the posterior once per group: the ionosphere moves and old evidence must decay.
    // THIS IS THE ONE FREE PARAMETER AND IT IS GENUINELY UNKNOWN.
    //   too small -> the posterior collapses and the controller freezes on a rung that
    //     stopped being right. It will look EXCELLENT on OTASim, whose Watterson process is
    //     exactly stationary and models no ionospheric time-variation, and fail on the rig.
    //     That is the simulator-fidelity trap CLAUDE.md warns about, in its purest form.
    //   too large -> the posterior never sharpens, argmax returns the prior, and this whole
    //     controller reduces to the anchor table we already have.
    // Must be MEASURED from our own data, not assumed.
    void relax(float sigma_db) {
        if (!(sigma_db > 0.0f)) return;
        const float s = sigma_db / kBinStep;
        if (s < 0.05f) return;
        float p[kBins], out[kBins];
        float mx = logp_[0];
        for (int i = 1; i < kBins; ++i) mx = std::max(mx, logp_[i]);
        for (int i = 0; i < kBins; ++i) p[i] = std::exp(logp_[i] - mx);
        const int half = std::min(kBins - 1, static_cast<int>(std::ceil(3.0f * s)));
        for (int i = 0; i < kBins; ++i) {
            float acc = 0.0f, wsum = 0.0f;
            for (int d = -half; d <= half; ++d) {
                const int j = i + d;
                if (j < 0 || j >= kBins) continue;
                const float wk = std::exp(-0.5f * static_cast<float>(d) * d / (s * s));
                acc += wk * p[j];
                wsum += wk;
            }
            out[i] = (wsum > 0.0f) ? acc / wsum : p[i];
        }
        for (int i = 0; i < kBins; ++i) logp_[i] = std::log(std::max(out[i], 1e-30f));
        normalise();
    }

    // ── DECISION: stateless argmax over predicted delivered goodput ───────────
    //
    // Overhead goes in the DENOMINATOR (Minstrel's nsecs includes it). Our predecessor's
    // break-even used the raw-rate ratio eta_below/eta_cur, which OMITS the fixed per-cycle
    // cost — with sync 1.41 s + turnaround 1.79 s against a 6.19 s payload, the true
    // break-even for one rung up is 0.835, not 0.750. Getting this wrong makes the
    // controller tolerate ~14 points more loss than it should.
    struct Pick {
        uint8_t rung = kRungIdxNone;
        float goodput = 0.0f;
        float x_used = 0.0f;
        bool tie_break_probe = false;   // this decision took the higher near-tied rung
    };

    Pick best(float payload_s_per_frame, float fixed_s_per_cycle, int frames_per_group,
              uint8_t ceiling = kRungIdxNone) {
        Pick out;
        float g_by_rung[kRungIdxCount] = {};
        out.x_used = percentile(kDecilePessimism);
        const int M = std::max(1, frames_per_group);
        for (uint8_t r = kRungIdxQpskR14; r < kRungIdxCount; ++r) {
            const CoherentPick cp = coherentRungFromIndex(r);
            if (!coherentRungLocallyEnabled(cp.mod, cp.rate)) continue;
            if (ceiling != kRungIdxNone && r > ceiling) continue;
            float p = successProb(out.x_used, latentThetaForRung(r));
            if (p < kDeadProb) continue;              // dead, not merely bad
            p = std::min(p, kCapProb);                // never reward a lucky streak
            const float eta = rungSpectralEfficiency(r);
            if (!(eta > 0.0f)) continue;
            // Airtime for one group at this rung scales inversely with spectral efficiency;
            // the fixed cost does not scale at all. That asymmetry is why a denser rung wins
            // less than its raw-rate ratio suggests.
            const float air = static_cast<float>(M) * payload_s_per_frame *
                              (rungSpectralEfficiency(kRungIdxQpskR34) / eta);
            const float cycle = air + fixed_s_per_cycle;
            const float g = (cycle > 0.0f) ? (eta * p / cycle) : 0.0f;
            if (g > out.goodput) { out.goodput = g; out.rung = r; }
            if (r < kRungIdxCount) { g_by_rung[r] = g; }
        }

        // TIE-BREAK UPWARD. Walk DOWN from the top so we find the HIGHEST rung that is
        // within the margin, not merely the runner-up: on a flat region several rungs can be
        // near-tied and the informative probe is the highest of them.
        ++decisions_;
        if (out.rung != kRungIdxNone && out.goodput > 0.0f &&
            (decisions_ % kTieBreakPeriod) == 0) {
            for (int r = static_cast<int>(kRungIdxCount) - 1;
                 r > static_cast<int>(out.rung); --r) {
                const float g = g_by_rung[r];
                if (g <= 0.0f) continue;
                if (g >= out.goodput * (1.0f - kTieBreakMarginFrac)) {
                    out.rung = static_cast<uint8_t>(r);
                    out.goodput = g;
                    out.tie_break_probe = true;
                    break;
                }
            }
        }
        return out;
    }

    float posteriorMean() const {
        float m = 0.0f;
        for (int i = 0; i < kBins; ++i) m += binX(i) * std::exp(logp_[i]);
        return m;
    }
    float percentile(float q) const {
        float acc = 0.0f;
        for (int i = 0; i < kBins; ++i) {
            acc += std::exp(logp_[i]);
            if (acc >= q) return binX(i);
        }
        return binX(kBins - 1);
    }
    float spreadDb() const {
        const float m = posteriorMean();
        float v = 0.0f;
        for (int i = 0; i < kBins; ++i) {
            const float d = binX(i) - m;
            v += d * d * std::exp(logp_[i]);
        }
        return std::sqrt(std::max(0.0f, v));
    }
    bool havePrior() const { return have_prior_; }
    int observations() const { return observations_; }
    int decisions() const { return decisions_; }

    static float successProb(float x, float theta) {
        const float z = kLatentSlopePerDb * (x - theta);
        const float p = 1.0f / (1.0f + std::exp(-std::clamp(z, -40.0f, 40.0f)));
        return std::clamp(p, kLatentMinSuccess, kLatentMaxSuccess);
    }
    static float binX(int i) { return kBinLo + kBinStep * static_cast<float>(i); }

private:
    void normalise() {
        float mx = logp_[0];
        for (int i = 1; i < kBins; ++i) mx = std::max(mx, logp_[i]);
        float sum = 0.0f;
        for (int i = 0; i < kBins; ++i) sum += std::exp(logp_[i] - mx);
        const float lz = mx + std::log(std::max(sum, 1e-30f));
        for (int i = 0; i < kBins; ++i) logp_[i] -= lz;
    }

    float logp_[kBins] = {};
    bool have_prior_ = false;
    int observations_ = 0;
    int decisions_ = 0;
};

}  // namespace protocol
}  // namespace ultra
