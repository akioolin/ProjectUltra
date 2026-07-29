// Phase 5: unit tests for the resource-element link abstraction.
//
// These test the MATH and the API contracts, independent of any fitted calibration
// — the fit is validated separately against held-out sweep data. What matters here
// is that the primitives behave correctly at the limits and that the selector
// cannot be tricked into commanding a rung it has no evidence for.

#include "protocol/link_abstraction.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

namespace la = ultra::protocol::link_abstraction;
using ultra::CodeRate;
using ultra::Modulation;

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

la::RungCalibration cal(Modulation m, CodeRate r, double beta, double mid,
                        double slope, bool valid = true) {
    la::RungCalibration c;
    c.mod = m;
    c.rate = r;
    c.beta = beta;
    c.midpoint_db = mid;
    c.slope = slope;
    c.valid = valid;
    c.n_frames = 1000;
    return c;
}

}  // namespace

int main() {
    std::printf("=== link abstraction (Phase 5) ===\n");

    // ---- EESM limits.
    {
        // A FLAT grid must return the common value regardless of beta: with every
        // gamma_k equal, EESM is exactly that value for any beta. This is the one
        // case with a closed form, so it pins the implementation.
        const std::vector<float> flat(51, 100.0f);
        bool flat_ok = true;
        for (double beta : {0.5, 5.0, 50.0, 500.0}) {
            const double eff = la::effectiveSnrEesm(flat, beta);
            if (std::abs(eff - 100.0) > 1e-3) {
                flat_ok = false;
            }
        }
        check(flat_ok, "flat grid returns the common gamma for every beta");
    }
    {
        // Large beta -> arithmetic mean. Small beta -> minimum. This is what makes
        // beta the compression knob, and getting the direction backwards would
        // silently make the predictor optimistic on selective channels.
        std::vector<float> g(51, 200.0f);
        for (int i = 0; i < 5; ++i) {
            g[i] = 2.0f;  // a few deep carriers
        }
        const double mean = [&] {
            double s = 0;
            for (float x : g) s += x;
            return s / g.size();
        }();
        const double big = la::effectiveSnrEesm(g, 1e6);
        const double small = la::effectiveSnrEesm(g, 0.05);
        std::printf("   mean=%.2f  eesm(beta=1e6)=%.2f  eesm(beta=0.05)=%.2f  min=2\n",
                    mean, big, small);
        check(std::abs(big - mean) / mean < 0.02,
              "beta -> infinity recovers the arithmetic mean");
        check(small < 4.0, "beta -> 0 approaches the minimum carrier");
        check(small < big, "EESM is monotonic in beta (compression direction correct)");
    }
    {
        // A deep grid must not underflow to a perfect score. With a naive
        // implementation exp(-gamma/beta) is 0 for every carrier, log(0) = -inf and
        // gamma_eff comes out +inf -- i.e. "this channel is flawless" at the exact
        // moment it is worst. The log-domain form must survive it.
        const std::vector<float> deep(51, 5000.0f);
        const double eff = la::effectiveSnrEesm(deep, 0.5);
        check(std::isfinite(eff) && eff > 0.0,
              "very high gamma with small beta stays finite (no underflow to perfect)");
        const std::vector<float> empty;
        check(la::effectiveSnrEesm(empty, 5.0) == 0.0, "empty grid returns 0, not NaN");
        check(la::effectiveSnrEesm(deep, 0.0) == 0.0, "beta <= 0 returns 0, not NaN");
    }

    // ---- PER curve.
    {
        const auto c = cal(Modulation::QPSK, CodeRate::R1_2, 10.0, 15.0, 0.5);
        check(std::abs(la::predictPer(c, 15.0) - 0.5) < 1e-6,
              "PER at the midpoint is exactly 0.5");
        check(la::predictPer(c, 40.0) < 1e-4, "PER -> 0 well above the midpoint");
        check(la::predictPer(c, -10.0) > 0.99, "PER -> 1 well below the midpoint");
        check(la::predictPer(c, 20.0) < la::predictPer(c, 10.0),
              "PER decreases with increasing gamma_eff (sign is not inverted)");
        const auto invalid = cal(Modulation::QPSK, CodeRate::R1_2, 10, 15, 0.5, false);
        check(la::predictPer(invalid, 100.0) == 1.0,
              "an INVALID calibration predicts PER = 1 at any SNR (never optimistic)");
    }

    // ---- Efficiency.
    {
        check(std::abs(la::bitsPerCarrier(Modulation::QPSK, CodeRate::R1_2) - 1.0) < 1e-9,
              "QPSK R1/2 is 1.0 info bits/carrier");
        check(std::abs(la::bitsPerCarrier(Modulation::QAM16, CodeRate::R3_4) - 3.0) < 1e-9,
              "16QAM R3/4 is 3.0 info bits/carrier");
        check(la::bitsPerCarrier(Modulation::QAM8, CodeRate::R2_3) >
                  la::bitsPerCarrier(Modulation::QPSK, CodeRate::R3_4),
              "8PSK R2/3 (2.0) beats QPSK R3/4 (1.5)");
    }

    // ---- Selection under a reliability constraint.
    {
        la::CalibrationTable t;
        t.version = la::kCalibrationVersion;
        // A fast rung that drops ~30% of frames, and a slower one that holds at ~2%.
        // This is the 16QAM R2/3 situation the anchor table actually shipped: the
        // ladder selected a rung measuring 51.4% FER because it was nominally faster.
        // Midpoints are chosen so that on the grid below the PERs land at those
        // values -- the point of the test is the DECISION, so the inputs must
        // actually construct the dilemma rather than a degenerate case.
        const std::vector<float> grid(51, 80.0f);  // 19.03 dB flat
        t.rungs.push_back(cal(Modulation::QAM16, CodeRate::R2_3, 10.0, 17.34, 0.5));
        t.rungs.push_back(cal(Modulation::QPSK, CodeRate::R3_4, 10.0, 11.25, 0.5));
        check(t.usable(), "table with the current version is usable");

        const auto scored = la::scoreAll(t, grid);
        check(scored.size() == 2, "every rung is scored, none silently dropped");

        // Confirm the fixture really does pose the dilemma before asserting on it.
        double per16 = 0.0, per4 = 0.0;
        for (const auto& s : scored) {
            if (s.mod == Modulation::QAM16) per16 = s.predicted_per;
            if (s.mod == Modulation::QPSK) per4 = s.predicted_per;
        }
        std::printf("   fixture: 16QAM R2/3 PER=%.3f (eff 2.67)  QPSK R3/4 PER=%.3f (eff 1.5)\n",
                    per16, per4);
        check(per16 > 0.2 && per16 < 0.4 && per4 < 0.05,
              "fixture poses the real dilemma: fast-but-lossy vs slow-but-solid");

        const auto* strict = la::selectUnderReliability(scored, 0.10);
        check(strict != nullptr && strict->mod == Modulation::QPSK,
              "under a 10% PER ceiling the RELIABLE rung wins, not the fastest");

        const auto* loose = la::selectUnderReliability(scored, 0.50);
        check(loose != nullptr && loose->mod == Modulation::QAM16,
              "under a loose ceiling the higher-efficiency rung wins on expected goodput");

        const auto* none = la::selectUnderReliability(scored, 1e-9);
        check(none == nullptr,
              "when nothing clears the ceiling the selector returns nullptr "
              "(caller must hold, not pick the fastest anyway)");
    }
    {
        // An uncalibrated rung must never be selectable, however attractive.
        la::CalibrationTable t;
        t.version = la::kCalibrationVersion;
        t.rungs.push_back(cal(Modulation::QAM16, CodeRate::R3_4, 10, 5.0, 0.5, false));
        const std::vector<float> grid(51, 1000.0f);  // 30 dB, would look perfect
        const auto scored = la::scoreAll(t, grid);
        check(scored.size() == 1 && !scored[0].calibrated,
              "uncalibrated rung is reported, flagged, and given PER 1");
        check(la::selectUnderReliability(scored, 0.5) == nullptr,
              "an UNCALIBRATED rung is never selected even on a perfect grid");
    }
    {
        // A stale table version must invalidate the whole table: the gamma
        // definition can change underneath it, and a table fitted against the old
        // definition would command confidently wrong rungs.
        la::CalibrationTable t;
        t.version = la::kCalibrationVersion - 1;
        t.rungs.push_back(cal(Modulation::QPSK, CodeRate::R1_2, 10, 15, 0.5));
        check(!t.usable(), "a stale calibration version makes the table unusable");
    }
    {
        // Empty grid: no measurement means no prediction, not a default guess.
        la::CalibrationTable t;
        t.version = la::kCalibrationVersion;
        t.rungs.push_back(cal(Modulation::QPSK, CodeRate::R1_2, 10, 15, 0.5));
        const auto scored = la::scoreAll(t, {});
        check(scored.size() == 1 && scored[0].predicted_per == 1.0,
              "empty gamma grid yields PER 1, never an optimistic default");
    }

    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
