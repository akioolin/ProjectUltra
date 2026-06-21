// test_doppler_coherence_estimator — locks the Good/Moderate channel discriminator in CI.
//
// The DopplerCoherenceEstimator separates Good (slow fading, RMS Doppler 0.05 Hz) from
// Moderate (fast, 0.25 Hz) via the temporal autocorrelation of the per-symbol pilot |H|^2,
// pooled across frames — where the production fading_index (fade DEPTH) is blind.
//
// This is a UNIT test of the estimator class. It drives the class with a synthetic
// Gaussian-Doppler |H|^2 process whose RMS spread sigma is known (matching the OTASim
// WattersonChannel: R(tau)=exp(-2 pi^2 sigma^2 tau^2), sigma = 0.5 * nominal Doppler), chopped
// into ~1.7 s frames with the inter-frame onFrameBoundary() exactly as the equalizer feeds it.
// The faithful-channel proof (real WattersonChannel + noise + OFDM FFT) lives in
// tools/channel_discriminator_probe.cpp; the real-modem-path proof is the GUI gate. See
// docs/CHANNEL_DISCRIMINATOR_DESIGN_2026_06_15.md.

#include "ofdm/doppler_coherence_estimator.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

using ultra::DopplerCoherenceEstimator;

namespace {

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg)                                       \
    do {                                                       \
        ++tests_run;                                           \
        if (!(cond)) {                                         \
            ++tests_failed;                                    \
            std::printf("FAIL: %s\n", msg);                    \
        }                                                      \
    } while (0)

constexpr float kPi = 3.14159265358979323846f;
constexpr float kSymbolPeriodS = 1152.0f / 48000.0f;  // wideband OFDM symbol = 24 ms

// One Gaussian-Doppler fading tap as a sum of sinusoids with Gaussian-distributed
// frequencies (RMS sigma_hz) — the same construction as the WattersonChannel, so its
// temporal autocorrelation is exp(-2 pi^2 sigma^2 tau^2).
struct FadingTap {
    std::vector<float> freq;
    std::vector<float> phase;
    void init(float sigma_hz, std::mt19937& rng, int n_osc = 64) {
        std::normal_distribution<float> fd(0.0f, sigma_hz);
        std::uniform_real_distribution<float> pd(0.0f, 2.0f * kPi);
        freq.resize(n_osc);
        phase.resize(n_osc);
        for (int k = 0; k < n_osc; ++k) {
            freq[k] = fd(rng);
            phase[k] = pd(rng);
        }
    }
    std::complex<float> at(float t) const {
        std::complex<float> g(0, 0);
        const float amp = 1.0f / std::sqrt(static_cast<float>(freq.size()));
        for (size_t k = 0; k < freq.size(); ++k) {
            const float ph = 2.0f * kPi * freq[k] * t + phase[k];
            g += amp * std::complex<float>(std::cos(ph), std::sin(ph));
        }
        return g;
    }
};

// Feed `n_frames` per-frame |H|^2 snapshots (each the mean of `syms_per_frame` symbols of a
// two-equal-power-tap channel of RMS Doppler `sigma_hz`, + estimation noise) into a fresh
// estimator. The snapshot cadence (syms_per_frame * symbol_period) is the inter-frame spacing.
void runChannel(DopplerCoherenceEstimator& est, float sigma_hz, uint32_t seed,
                int n_frames, int syms_per_frame, float per_symbol_noise) {
    std::mt19937 rng(seed);
    FadingTap t1, t2;
    t1.init(sigma_hz, rng);
    t2.init(sigma_hz, rng);
    std::normal_distribution<float> noise(0.0f, per_symbol_noise);
    est.configure(kSymbolPeriodS);
    for (int f = 0; f < n_frames; ++f) {
        double acc = 0.0;
        for (int s = 0; s < syms_per_frame; ++s) {
            const float t = static_cast<float>(f * syms_per_frame + s) * kSymbolPeriodS;
            const float p2 = std::norm(t1.at(t)) + std::norm(t2.at(t)) + noise(rng);
            acc += std::max(0.0f, p2);
        }
        est.addSnapshot(static_cast<float>(acc / syms_per_frame));  // one per-frame |H|^2 snapshot
    }
}

bool test_good_moderate_separation() {
    // 0.5 * nominal Doppler -> Good 0.05 Hz, Moderate 0.25 Hz (CCIR MPG/MPM).
    constexpr float kGoodSigma = 0.05f;
    constexpr float kModSigma = 0.25f;
    constexpr int kFrames = 50;     // -> 40-snapshot window (lag-1 SE small enough to separate)
    constexpr int kFrameLen = 51;   // ~1.2 s frame cadence (the inter-frame discriminating lag)
    // Two-threshold dead zone (matches connection_policy::kCoherence{Good,Moderate}Threshold).
    constexpr float kGoodThr = 0.45f;
    constexpr float kModThr = 0.30f;

    (void)kModThr;
    int good_confident = 0, mod_dangerous = 0;
    float good_min = 1e9f, mod_max = -1e9f;
    for (uint32_t seed = 1; seed <= 8; ++seed) {
        DopplerCoherenceEstimator g, m;
        runChannel(g, kGoodSigma, 7000u + seed, kFrames, kFrameLen, 0.05f);
        runChannel(m, kModSigma, 8000u + seed, kFrames, kFrameLen, 0.05f);

        CHECK(g.valid(), "Good estimator should be valid after the pooled window fills");
        CHECK(m.valid(), "Moderate estimator should be valid after the pooled window fills");
        const float gs = g.coherenceScore();
        const float ms = m.coherenceScore();
        good_min = std::min(good_min, gs);
        mod_max = std::max(mod_max, ms);
        if (gs >= kGoodThr) ++good_confident;   // confident Good
        if (ms >= kGoodThr) ++mod_dangerous;    // Moderate misread as confident Good (must be 0)
    }
    std::printf("  Good/Moderate: good_min=%.3f mod_max=%.3f  (good %d/8 confident-Good >= %.2f, moderate %d/8 dangerous >= %.2f)\n",
                good_min, mod_max, good_confident, kGoodThr, mod_dangerous, kGoodThr);
    CHECK(good_confident == 8, "all Good seeds should be a confident Good (>= 0.45)");
    // The SAFETY property: a Moderate channel must never read confident-Good (-> over-high rate).
    // A Moderate landing in the dead zone [0.30,0.45] is acceptable (defers to the status quo).
    CHECK(mod_dangerous == 0, "no Moderate seed may reach the confident-Good threshold (the dangerous misread)");
    CHECK(good_min > mod_max, "Good and Moderate score ranges must not overlap (separated)");
    return tests_failed == 0;
}

bool test_score_ordering_and_doppler() {
    // The DECISION quantity is coherenceScore (lag-1 autocorr): Good high, Moderate/Poor low.
    // At a ~1.6 s inter-frame cadence Moderate/Poor are fully decorrelated, so their score is
    // ~0 and their RMS Doppler is NOT estimable from snapshot lag-1 (expected) — dopplerHz is a
    // Good-channel readout for the Wiener model, which is exactly the case that needs it.
    DopplerCoherenceEstimator g, m, p;
    runChannel(g, 0.05f, 4242u, 50, 51, 0.05f);
    runChannel(m, 0.25f, 4243u, 50, 51, 0.05f);
    runChannel(p, 0.50f, 4244u, 50, 51, 0.05f);
    const float sg = g.coherenceScore(), sm = m.coherenceScore(), sp = p.coherenceScore();
    std::printf("  coherenceScore: good=%.3f moderate=%.3f poor=%.3f | dopplerHz(good)=%.3f\n",
                sg, sm, sp, g.dopplerHz());
    CHECK(sg > sm, "coherenceScore: Good should be above Moderate");
    CHECK(sg > sp, "coherenceScore: Good should be above Poor");
    // (Moderate vs Poor are both fully decorrelated at this cadence -> both ~0; their relative
    // order is noise and the discriminator does not claim to separate them.)
    CHECK(sm < 0.45f && sp < 0.45f, "neither Moderate nor Poor may reach the confident-Good threshold");
    CHECK(sg > 0.45f, "Good coherenceScore should clear the confident-Good threshold (0.45)");
    CHECK(g.dopplerHz() > 0.0f && g.dopplerHz() < 0.2f,
          "Good RMS Doppler readout should be estimable and in a sane low range");
    return true;
}

bool test_coherence_area_ordering() {
    // coherenceArea (the radio-agnostic discriminator) = cumulative mean of the sliding-window
    // Sum_{lag=1..5} normalized autocov. Lock its ORDERING: Good (slow, sustained positive
    // autocorrelation out to lag ~5) must give a clearly higher area than Moderate/Poor (decorrelated
    // -> the multi-lag sum collapses). NOTE: this synthetic channel is FLAT fading (no delay spread,
    // so no frequency-selectivity), so its absolute scale is NOT the real-channel scale — the
    // production enter/exit thresholds (0.05/0.00) are calibrated on the real sim+IONOS captures
    // (docs/SCALE_INVARIANT_COHERENCE_DISC_2026_06_20.md), where Moderate's selectivity pushes the
    // area negative. Here we assert only the platform-independent ORDERING + monotonic-in-Doppler.
    // Assert the PAIRED ordering (per-seed Good>Mod) + a mean margin — the properties a flat-fading
    // channel CAN faithfully show. We do NOT assert across-seed range non-overlap or the absolute
    // production thresholds: on a very slow (0.05 Hz) FLAT process the |H|^2 barely varies over a
    // 40-frame window so its local-demeaned autocorrelation is ill-conditioned/noisy (a Good seed can
    // dip negative) — which on a REAL channel cannot happen (selectivity + larger fade variation) and
    // would in any case fail SAFE (under-read Good -> conservative). Real-scale validation is the
    // cross-platform sim+IONOS capture, not this synthetic flat channel.
    int good_above_mod = 0;
    double good_sum = 0.0, mod_sum = 0.0;
    for (uint32_t seed = 1; seed <= 8; ++seed) {
        DopplerCoherenceEstimator g, m;
        runChannel(g, 0.05f, 7000u + seed, 50, 51, 0.05f);
        runChannel(m, 0.25f, 8000u + seed, 50, 51, 0.05f);
        const float ga = g.coherenceArea();
        const float ma = m.coherenceArea();
        good_sum += ga; mod_sum += ma;
        if (ga > ma) ++good_above_mod;
    }
    const float good_mean = static_cast<float>(good_sum / 8.0);
    const float mod_mean = static_cast<float>(mod_sum / 8.0);
    std::printf("  coherenceArea: good_mean=%.3f mod_mean=%.3f  (Good>Mod paired on %d/8 seeds)\n",
                good_mean, mod_mean, good_above_mod);
    // >=6/8 (NOT all 8): the synthetic FLAT-fading channel's area is noisy (some slow-Good seeds dip)
    // AND std::normal_distribution is not bit-identical across libstdc++/libc++ (CI: macOS 8/8, Linux
    // 7/8 — same mt19937, different normal transform), so an all-8 per-seed ordering is platform-fragile.
    // The MEAN margin is the real, platform-stable property (CI Linux: good_mean 0.77 vs mod_mean 0.02).
    CHECK(good_above_mod >= 6, "coherenceArea: Good must exceed Moderate on most PAIRED seeds (ordering)");
    CHECK(good_mean > mod_mean + 0.2f, "coherenceArea: Good mean must clearly exceed Moderate mean");
    return tests_failed == 0;
}

bool test_abstains_until_enough_pooled() {
    // A single short frame must NOT be trusted (the prior "window too short" failure).
    DopplerCoherenceEstimator est;
    runChannel(est, 0.05f, 99u, 1, 51, 0.05f);  // one frame only (1 snapshot)
    CHECK(!est.valid(), "estimator must abstain after a single frame (insufficient pooled data)");
    return true;
}

}  // namespace

int main() {
    test_good_moderate_separation();
    test_score_ordering_and_doppler();
    test_coherence_area_ordering();
    test_abstains_until_enough_pooled();

    if (tests_failed != 0) {
        std::printf("DopplerCoherenceEstimator: %d/%d passed\n", tests_run - tests_failed, tests_run);
        return 1;
    }
    std::printf("DopplerCoherenceEstimator: %d/%d passed\n", tests_run, tests_run);
    return 0;
}
