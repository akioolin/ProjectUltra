#pragma once

// DopplerCoherenceEstimator — measures HF channel coherence TIME (Doppler spread) to tell
// Good (slow fading) from Moderate (fast). The production fading_index measures fade DEPTH
// (|H| CV), identical for the equal-gain 2-path CCIR Good (RMS Doppler 0.05 Hz) and Moderate
// (0.25 Hz) presets — chance-level classification. What differs is the fading RATE.
//
// METHOD — PER-FRAME |H|^2 SNAPSHOTS (not within-frame symbols). OFDM wideband uses BURST
// transport: each data frame is only ~9 OFDM symbols (~0.2 s), far too short to see a ~1 s
// Doppler decorrelation WITHIN a frame. But consecutive burst frames are ~1.5-2 s apart (group
// airtime / frames-per-group), and that inter-frame cadence sits squarely in the regime where
// Good and Moderate diverge. So we take ONE carrier-averaged |H|^2 snapshot per frame and
// measure the temporal autocorrelation ACROSS frames at snapshot-lag-1 (and lag-2). The channel
// autocorrelation is Gaussian, A(tau)=exp(-4 pi^2 sigma^2 tau^2); at a ~1.5-2 s lag Good stays
// ~0.5-0.8 correlated while Moderate is ~0 — a wide, robust margin that holds across the whole
// plausible cadence range (0.7-4 s: Good >0.45, Moderate <0.3 throughout), so the decision does
// not depend on knowing the exact cadence. Full physics/proof:
// docs/CHANNEL_DISCRIMINATOR_DESIGN_2026_06_15.md.
//
// DESIGN NOTES (four-tier stack):
//  - MAGNITUDE only (|H|^2): immune to residual CFO / the warm-LTS phase re-anchoring between
//    frames (a complex autocorrelation over second-scale lags would be corrupted by both).
//  - GLOBAL mean over the snapshot window (a Good channel's snapshots are a slowly-drifting
//    near-constant; demeaning per anything shorter would erase exactly that slow signal).
//  - SNR-robust: white per-snapshot estimation noise (already reduced ~sqrt(symbols/frame) by the
//    intra-frame average) inflates only the zero-lag variance — a constant floor that biases
//    lag>=1 down equally and is absorbed by the threshold / the ln-vs-lag^2 slope.
//  - READ-ONLY: observes pilot magnitudes; never alters the channel estimate, equalization, or
//    any decode decision. Consumption is separately gated on valid().
//  - Caveat (documented): snapshot-INDEX lags assume the inter-frame cadence is in the ~1-2 s
//    discrimination zone (true for OFDM burst). A future mode with a very different cadence would
//    want real per-frame timestamps; that hardening is a tracked follow-up.

#include <cmath>
#include <cstddef>
#include <deque>

namespace ultra {

class DopplerCoherenceEstimator {
public:
    // symbol_period_s = (FFT + CP) / sample_rate (used only for the rough Doppler-Hz readout).
    void configure(float symbol_period_s) {
        symbol_period_s_ = (symbol_period_s > 1e-6f && symbol_period_s < 1.0f)
                               ? symbol_period_s
                               : 0.024f;
        reset();
    }

    // Clear all state (per connection / mode).
    void reset() {
        snaps_.clear();
        score_sum_ = 0.0;
        score_n_ = 0;
    }

    // Add ONE per-frame |H|^2 snapshot (the frame's LTS channel power). Caller supplies one per
    // decoded OFDM frame; the inter-frame spacing IS the ~1-2 s discriminating lag. Hosted at a
    // layer that persists across the per-group demodulator recreation (the StreamingDecoder),
    // NOT inside the demodulator (which burst transport rebuilds every group, wiping the pool).
    // Each snapshot extends the sliding window AND folds one lag-1 autocorrelation reading into
    // the cumulative-mean score (see coherenceScore for why the running mean, not a single read).
    void addSnapshot(float abs_h2) {
        if (!(std::isfinite(abs_h2) && abs_h2 >= 0.0f)) return;
        snaps_.push_back(abs_h2);
        while (snaps_.size() > kMaxSnaps) snaps_.pop_front();
        if (snaps_.size() >= kMinSnapsForReading) {
            score_sum_ += static_cast<double>(normAutocov(1));
            ++score_n_;
        }
    }

    // Diagnostics (for logging): number of per-frame autocorrelation readings averaged so far.
    size_t snapshotCount() const { return score_n_; }

    // True once enough readings have been averaged for a trustworthy verdict.
    bool valid() const { return score_n_ >= kMinReadings; }

    // CUMULATIVE-MEAN normalized |H|^2 autocorrelation at the ~1-2 s inter-frame cadence, in
    // ~[-1, 1]. High => Good (long coherence time); near zero => Moderate/Poor. The running mean
    // over the transfer is used deliberately: a SINGLE 40-snapshot autocorrelation has ~0.16
    // standard error (measured: Moderate single-reads scatter to ~0.45), too noisy for a tight
    // threshold; the mean is what cleanly separates the classes (GUI multi-seed: Good >=0.38,
    // Moderate <=0.25). 0 until valid().
    float coherenceScore() const {
        return (score_n_ > 0) ? static_cast<float>(score_sum_ / static_cast<double>(score_n_))
                              : 0.0f;
    }

    // Diagnostic: the assumed inter-frame cadence used for the Doppler-Hz readout (seconds).
    float refLagSeconds() const { return kNominalCadenceS; }

    // Rough RMS Doppler (Hz) from the cumulative-mean coherence, assuming the inter-frame cadence
    // is ~kNominalCadenceS. SECONDARY/approximate (the Good/Moderate decision uses coherenceScore);
    // precise Hz needs per-frame timestamps (tracked follow-up). 0 if not estimable.
    float dopplerHz() const {
        if (!valid()) return 0.0f;
        const float r = coherenceScore();
        if (!(r > 0.02f) || r >= 1.0f) return 0.0f;  // decorrelated/degenerate -> not estimable here
        // A(tau)=exp(-4 pi^2 sigma^2 tau^2) -> sigma = sqrt(-ln r / (4 pi^2 tau^2)).
        const double tau = kNominalCadenceS;
        const double sigma2 = -std::log(static_cast<double>(r)) /
                              (4.0 * kPi * kPi * tau * tau);
        return (sigma2 > 0.0) ? static_cast<float>(std::sqrt(sigma2)) : 0.0f;
    }

private:
    // Biased normalized autocovariance of the snapshot series at the given integer lag,
    // detrended against the global window mean.
    float normAutocov(int lag) const {
        const int n = static_cast<int>(snaps_.size());
        if (n <= lag + 1) return 0.0f;
        double mu = 0.0;
        for (float v : snaps_) mu += v;
        mu /= static_cast<double>(n);
        double num = 0.0, den = 0.0;
        for (int i = 0; i < n; ++i) {
            const double e = static_cast<double>(snaps_[static_cast<size_t>(i)]) - mu;
            den += e * e;
            if (i + lag < n) {
                num += e * (static_cast<double>(snaps_[static_cast<size_t>(i + lag)]) - mu);
            }
        }
        if (!(den > 1e-20)) return 0.0f;  // negated-positive form rejects NaN too
        return static_cast<float>(num / den);
    }

    static constexpr double kPi = 3.14159265358979323846;
    static constexpr size_t kMaxSnaps = 40;          // sliding window for each lag-1 reading
    static constexpr size_t kMinSnapsForReading = 8; // need a few snapshots before a reading is meaningful
    static constexpr size_t kMinReadings = 24;       // average >=24 readings before the verdict is trusted
    static constexpr float kNominalCadenceS = 1.6f;  // approx OFDM burst inter-frame spacing (Hz readout only)

    float symbol_period_s_ = 0.024f;  // retained for API symmetry / future timestamp-based cadence
    std::deque<float> snaps_;         // per-frame |H|^2, sliding window
    double score_sum_ = 0.0;          // sum of per-frame lag-1 autocorrelation readings
    size_t score_n_ = 0;              // number of readings (cumulative-mean denominator)
};

}  // namespace ultra
