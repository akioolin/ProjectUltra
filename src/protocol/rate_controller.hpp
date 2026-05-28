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
//   * fast DOWN: quality < drop_below           -> step down one rung NOW
//   * slow UP:   quality >= climb_above for K    -> step up one rung
//   * hysteresis gap (drop_below << climb_above) + the K-streak kill thrash.
//
// No PHY/audio/protocol dependencies — unit-tested in tests/test_rate_controller.cpp.

#include "ultra/types.hpp"

#include <algorithm>
#include <vector>

namespace ultra {
namespace protocol {

class RateController {
public:
    struct Config {
        // quality in [0,1]: 1 = lots of decode headroom, 0 = group failed.
        float drop_below = 0.25f;   // below this -> step down immediately
        float climb_above = 0.70f;  // at/above this for climb_streak groups -> step up
        // Climbs are deliberately SLOWER than drops. Bumped 2 -> 3 (2026-05-28)
        // after the g4+adapt seed-2 thrash: climb_streak=2 climbed back to R3/4
        // ~17 s after escaping a fade and immediately dropped into the next one;
        // 940 bps net. Three consecutive comfortable groups (~20 s at group=4)
        // gives the recovering channel a longer "rest" before risking a climb
        // back into freshly-arrived fade activity.
        int climb_streak = 3;
        // Ordered ladder of SUPPORTED rates, lowest throughput first. If left empty
        // the controller fills it with the production OFDM ladder (skips the
        // unsupported R1_3/R5_6/R7_8 enum holes).
        std::vector<CodeRate> ladder;
    };

    RateController() : RateController(Config{}) {}
    explicit RateController(Config cfg) : cfg_(std::move(cfg)) {
        if (cfg_.ladder.empty()) {
            // §14.36 toward-3000 ladder: R5/6 added as a climb target above R3/4
            // (2026-05-28). LDPC encoder/decoder fully support R5/6 (802.11n
            // base matrix, 540 info bits / 648 coded). +11% bytes/frame vs R3/4
            // when the channel permits; controller drops back automatically when
            // headroom shrinks. Adds NO risk on faded channels (it just never
            // climbs that high if quality stays low), real win on clean stretches.
            // §14.36 toward-3000 ladder: R5/6 added as a climb target above R3/4
            // (2026-05-28). LDPC encoder/decoder fully support R5/6 (802.11n base
            // matrix, 540 info bits / 648 coded). +11% bytes/frame vs R3/4 when
            // the channel permits; controller drops back automatically when
            // headroom shrinks.
            cfg_.ladder = {CodeRate::R1_4, CodeRate::R1_2, CodeRate::R2_3,
                           CodeRate::R3_4, CodeRate::R5_6};
        }
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

        if (quality < cfg_.drop_below) {
            good_streak_ = 0;
            const int next = std::max(0, idx - 1);
            return cfg_.ladder[static_cast<size_t>(next)];
        }
        if (quality >= cfg_.climb_above) {
            if (++good_streak_ >= cfg_.climb_streak) {
                good_streak_ = 0;
                const int next =
                    std::min(static_cast<int>(cfg_.ladder.size()) - 1, idx + 1);
                return cfg_.ladder[static_cast<size_t>(next)];
            }
            return current;  // comfortable, but not enough consecutive good yet
        }
        // mid-zone: decoding fine but no margin to climb — hold, drop climb credit.
        good_streak_ = 0;
        return current;
    }

    void reset() { good_streak_ = 0; }
    int climbStreak() const { return good_streak_; }
    const Config& config() const { return cfg_; }

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

    Config cfg_;
    int good_streak_ = 0;
};

}  // namespace protocol
}  // namespace ultra
