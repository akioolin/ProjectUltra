#pragma once

// FrequencySelectivity — a FIRST-FRAME Good/Moderate/Poor channel-class discriminator measured
// from ONE LTS channel estimate.
//
// WHY THIS EXISTS. Two prior estimators cannot do this job:
//   * The production fading index is the across-carrier COEFFICIENT OF VARIATION of |H|
//     (channel_equalizer_lts.cpp). For a Rayleigh channel that converges to the CONSTANT
//     sqrt(2-pi/2)/sqrt(pi/2) = 0.5227 REGARDLESS of delay spread or Doppler — it answers
//     "is this Rayleigh?", not "how dispersive". Measured against operator-confirmed IONOS
//     ground truth (MPG 0.1 Hz/0.5 ms Good vs MPM 0.5 Hz/1 ms Moderate — 5x Doppler, 2x delay):
//     0.354 vs ~0.35, no separation, single-frame ordering even BACKWARDS. See
//     BUG-FADING-INDEX-BLIND.
//   * DopplerCoherenceEstimator measures the fading RATE and DOES work (verified good +0.606 vs
//     moderate -0.285 on the faithful gate) — but it needs kMinSnapsForReading(8) +
//     kMinReadings(24) = 31 per-frame snapshots, i.e. ~40-60 s. For an ARQ system whose whole
//     cost model is "over-commit for a minute = craters + re-anchor", a verdict that arrives
//     after the first half of a transfer is a post-mortem, not a discriminator. It remains the
//     slow/confirming path; this is the fast one.
//
// THE PHYSICS. For an equal-gain 2-path channel with differential delay D:
//     H(f)   = a1 + a2*exp(-j*2*pi*f*D)
//     |H(f)|^2 = |a1|^2 + |a2|^2 + 2*Re(a1*conj(a2)*exp(-j*2*pi*f*D))
// Subtracting the across-carrier MEAN leaves exactly the ripple 2|a1||a2|cos(2*pi*f*D + phi),
// whose normalized autocorrelation at carrier lag L is
//     E[S(L)] ∝ cos(2*pi * L * df * D)
// Two properties make this the right statistic:
//   1. TWICE the frequency sensitivity of the complex-H autocorrelation (whose kernel is
//      cos(pi*L*df*D)) — the |H|^2 ripple runs at twice the rate of the H ripple.
//   2. Normalizing by the RIPPLE's own energy divides out 2|a1||a2|, eliminating the random
//      tap-power split r = |a1|^2/(|a1|^2+|a2|^2) ~ U(0,1). That split is the dominant nuisance
//      and is why the complex form ceilings at d' ~ 1.9 even at INFINITE SNR.
// It is MAGNITUDE-ONLY, hence exactly invariant to timing offsets (a linear phase ramp),
// residual CFO, and warm-LTS phase re-anchoring. AWGN attenuates it multiplicatively without
// moving its ZERO, so a fixed sign threshold stays optimal at every SNR.
//
// CONSTANT-FREE BY CONSTRUCTION. The lags come from CCIR geometry (log-midpoints between the
// standard channel delays) and the confidence gate comes from the null distribution of a sample
// autocorrelation (a probability constant, the same class as the project's -ln(0.05) chi-sq
// radius) — nothing is bench-fitted, so it transfers across radios unchanged.

#include <cmath>
#include <cstddef>

namespace ultra {
namespace ofdm {

// CCIR/ITU-R F.1487 differential delays for the standard HF channels (seconds).
inline constexpr double kCcirDelayGoodS     = 0.5e-3;
inline constexpr double kCcirDelayModerateS = 1.0e-3;
inline constexpr double kCcirDelayPoorS     = 2.0e-3;

// Decision delays = the LOG-midpoints between adjacent classes. A log-midpoint (geometric mean)
// is the scale-free split between two multiplicatively-spaced hypotheses — the same reasoning
// that makes an ML boundary sit at the midpoint of two cluster centres, applied on the axis the
// classes are actually spaced on (each CCIR class doubles the delay of the previous one).
inline double decisionDelayGoodModerateS() {
    return std::sqrt(kCcirDelayGoodS * kCcirDelayModerateS);      // 0.7071 ms
}
inline double decisionDelayModeratePoorS() {
    return std::sqrt(kCcirDelayModerateS * kCcirDelayPoorS);      // 1.4142 ms
}

// Carrier lag whose cos() kernel has its ZERO exactly at the decision delay:
//   cos(2*pi*L*df*D) = 0  <=>  L*df*D = 1/4  <=>  L = 1 / (4*df*D)
// So S(L) > 0 means "delay below the boundary" and S(L) < 0 means "above" — the sign IS the
// classifier, with no magnitude threshold to calibrate. df is derived from the live OFDM
// geometry (sample_rate / fft_size), so OFDM_NARROW gets its own lags automatically.
inline int decisionLagForDelay(double carrier_spacing_hz, double decision_delay_s) {
    if (!(carrier_spacing_hz > 0.0) || !(decision_delay_s > 0.0)) return 0;
    const double lag = 1.0 / (4.0 * carrier_spacing_hz * decision_delay_s);
    if (!std::isfinite(lag) || lag < 1.0) return 0;
    return static_cast<int>(std::lround(lag));
}

struct FrequencySelectivity {
    float s_gm = 0.0f;      // normalized autocorrelation at the Good/Moderate lag
    float s_mp = 0.0f;      // ... at the Moderate/Poor lag
    int   lag_gm = 0;
    int   lag_mp = 0;
    size_t carriers = 0;    // total carriers used this frame (across all segments)
    // Effective degrees of freedom = number of lag-L PAIRS actually summed, per lag. With a
    // single run this is (N - lag); with the production two-segment grid (DC gap) it is
    // sum_i (n_i - lag). The confidence gate needs the pair count, not the carrier count.
    size_t dof_gm = 0;
    size_t dof_mp = 0;
    bool  valid = false;
};

// Normalized lag-L autocorrelation of the MEAN-REMOVED |H|^2 sequence.
//
//   S(L) = sum_k Ptilde_k * Ptilde_{k+L}
//          / sqrt( sum_k Ptilde_k^2 * sum_{k>=L} Ptilde_k^2 )
//
// `power` must be |H|^2 in ASCENDING FREQUENCY ORDER on a UNIFORM grid (use the full
// data+pilot carrier list — a pilot-gapped list breaks the lag->frequency mapping).
//
// DO NOT DETREND beyond removing the mean. A moving-average detrend wide enough to track real
// TX/RX shaping roll-off also eats the Good-channel ripple (period 1/D = 2000 Hz ~ 43 carriers);
// measured, that collapses d' from 6.16 to 2.69. Plain mean removal plus a small accepted
// positive bias is the correct trade.
inline float normalizedPowerAutocorr(const float* power, size_t n, int lag) {
    if (power == nullptr || lag <= 0 || n <= static_cast<size_t>(lag) + 1) return 0.0f;
    double mean = 0.0;
    for (size_t i = 0; i < n; ++i) mean += static_cast<double>(power[i]);
    mean /= static_cast<double>(n);

    const size_t m = n - static_cast<size_t>(lag);
    double num = 0.0, e_lo = 0.0, e_hi = 0.0;
    for (size_t k = 0; k < m; ++k) {
        const double a = static_cast<double>(power[k]) - mean;
        const double b = static_cast<double>(power[k + static_cast<size_t>(lag)]) - mean;
        num += a * b;
        e_lo += a * a;
        e_hi += b * b;
    }
    const double den = std::sqrt(e_lo * e_hi);
    if (!(den > 0.0) || !std::isfinite(den)) return 0.0f;
    const double s = num / den;
    if (!std::isfinite(s)) return 0.0f;
    return static_cast<float>(s < -1.0 ? -1.0 : (s > 1.0 ? 1.0 : s));
}

// SEGMENTED form — the production case. The OFDM carrier grid straddles DC with the DC bin
// unused, so the active carriers form TWO uniform runs (negative and positive frequencies)
// rather than one. Taking only the longest run wastes half the band: measured on the rig it
// gave N=30 of 59, which lifts the single-frame confidence threshold from 1.96/sqrt(51)=0.274
// to 1.96/sqrt(22)=0.418 — right on top of the real MPM readings (-0.35..-0.46), so genuine
// Moderate frames fell back to "not confident" (rig: only ~50% of frames reached a MODERATE
// verdict even though every pooled run was correctly negative).
//
// Accumulating the sums ACROSS segments is exact, not an approximation: each segment
// contributes its own lag-L pairs on the same uniform grid, and the estimator is a ratio of
// sums. The mean is removed GLOBALLY (one channel, one mean). Effective dof is
// sum_i (n_i - lag), which is what selectivityConfident() must be told.
inline float normalizedPowerAutocorrSegmented(const float* const* segments,
                                              const size_t* lengths, size_t seg_count,
                                              int lag, size_t* dof_out = nullptr) {
    if (dof_out) *dof_out = 0;
    if (segments == nullptr || lengths == nullptr || lag <= 0 || seg_count == 0) return 0.0f;
    double sum = 0.0;
    size_t total = 0;
    for (size_t s = 0; s < seg_count; ++s) {
        for (size_t i = 0; i < lengths[s]; ++i) sum += static_cast<double>(segments[s][i]);
        total += lengths[s];
    }
    if (total == 0) return 0.0f;
    const double mean = sum / static_cast<double>(total);

    double num = 0.0, e_lo = 0.0, e_hi = 0.0;
    size_t dof = 0;
    for (size_t s = 0; s < seg_count; ++s) {
        const size_t n = lengths[s];
        if (n <= static_cast<size_t>(lag)) continue;   // too short to contribute this lag
        const size_t m = n - static_cast<size_t>(lag);
        for (size_t k = 0; k < m; ++k) {
            const double a = static_cast<double>(segments[s][k]) - mean;
            const double b = static_cast<double>(segments[s][k + static_cast<size_t>(lag)]) - mean;
            num += a * b;
            e_lo += a * a;
            e_hi += b * b;
        }
        dof += m;
    }
    if (dof_out) *dof_out = dof;
    const double den = std::sqrt(e_lo * e_hi);
    if (!(den > 0.0) || !std::isfinite(den)) return 0.0f;
    const double s = num / den;
    if (!std::isfinite(s)) return 0.0f;
    return static_cast<float>(s < -1.0 ? -1.0 : (s > 1.0 ? 1.0 : s));
}

// Segmented entry point — USE THIS in production. `segments`/`lengths` describe the maximal
// uniformly-spaced runs of active carriers in ascending frequency (the wideband grid has two:
// negative and positive frequencies either side of the unused DC bin).
inline FrequencySelectivity measureFrequencySelectivitySegmented(
    const float* const* segments, const size_t* lengths, size_t seg_count,
    double carrier_spacing_hz) {
    FrequencySelectivity out;
    out.lag_gm = decisionLagForDelay(carrier_spacing_hz, decisionDelayGoodModerateS());
    out.lag_mp = decisionLagForDelay(carrier_spacing_hz, decisionDelayModeratePoorS());
    if (out.lag_gm <= 0 || out.lag_mp <= 0 || seg_count == 0) return out;
    for (size_t i = 0; i < seg_count; ++i) out.carriers += lengths[i];
    out.s_gm = normalizedPowerAutocorrSegmented(segments, lengths, seg_count, out.lag_gm,
                                                &out.dof_gm);
    out.s_mp = normalizedPowerAutocorrSegmented(segments, lengths, seg_count, out.lag_mp,
                                                &out.dof_mp);
    out.valid = (out.dof_gm > 0 && out.dof_mp > 0);
    return out;
}

inline FrequencySelectivity measureFrequencySelectivity(const float* power, size_t n,
                                                        double carrier_spacing_hz) {
    const float* segs[1] = {power};
    const size_t lens[1] = {n};
    return measureFrequencySelectivitySegmented(segs, lens, 1, carrier_spacing_hz);
}

// CONFIDENCE from the NULL distribution — no fitted constant.
// Under H0 (flat channel: AWGN, or a momentary single-path fade) the mean-removed sequence is
// driven purely by per-carrier estimation noise, i.i.d. across carriers. The lag-L sample
// autocorrelation of N i.i.d. points is asymptotically N(0, 1/(N-L)), so
//     sigma0 = 1 / sqrt(N - L)
// (predicted 0.139 at N=59,L=7; measured 0.137 in simulation — the null model is exact).
// Pooling M frames divides the variance by M. z = 1.96 is the two-sided alpha = 0.05 point.
inline constexpr float kSelectivityZ = 1.96f;

// `pair_dof` = lag-L pairs summed for THIS lag in ONE frame (FrequencySelectivity::dof_*).
inline bool selectivityConfidentDof(float s, size_t pair_dof, size_t frames_pooled = 1) {
    if (pair_dof == 0 || frames_pooled == 0) return false;
    const double dof = static_cast<double>(pair_dof) * static_cast<double>(frames_pooled);
    if (!(dof > 0.0)) return false;
    return std::fabs(static_cast<double>(s)) >= kSelectivityZ / std::sqrt(dof);
}

// Convenience overload for a single contiguous run of `carriers` carriers.
inline bool selectivityConfident(float s, size_t carriers, int lag, size_t frames_pooled = 1) {
    if (lag <= 0 || carriers <= static_cast<size_t>(lag)) return false;
    return selectivityConfidentDof(s, carriers - static_cast<size_t>(lag), frames_pooled);
}

enum class SelectivityClass { UNDETERMINED, FLAT_OR_GOOD, GOOD, MODERATE, POOR };

// SIGN THERMOMETER. The Moderate/Poor lag is tested FIRST, and that ordering is a CORRECTNESS
// requirement, not a preference: cos(2*pi*L*df*D) is periodic, so the Good/Moderate statistic
// swings positive again for D in roughly (2.3, 3.8) ms — a 3 ms channel would read "Good" on
// that lag alone. The Moderate/Poor lag's first zero is at 1.33 ms and its next at 4.0 ms, so it
// is NEGATIVE across that whole window and vetoes the alias.
//
// A flat channel (both lags inside the noise band) reports FLAT_OR_GOOD: AWGN and a single-path
// fade genuinely have no frequency selectivity, and treating them as Good is both correct and
// the safe direction (it is the class the ladder already assumes).
inline SelectivityClass classifySelectivity(const FrequencySelectivity& fs,
                                            size_t frames_pooled = 1) {
    if (!fs.valid) return SelectivityClass::UNDETERMINED;
    const bool c_mp = selectivityConfidentDof(fs.s_mp, fs.dof_mp, frames_pooled);
    const bool c_gm = selectivityConfidentDof(fs.s_gm, fs.dof_gm, frames_pooled);
    if (!c_mp && !c_gm) return SelectivityClass::FLAT_OR_GOOD;
    if (c_mp && fs.s_mp < 0.0f) return SelectivityClass::POOR;
    if (c_gm && fs.s_gm < 0.0f) return SelectivityClass::MODERATE;
    if (c_gm && fs.s_gm >= 0.0f) return SelectivityClass::GOOD;
    return SelectivityClass::UNDETERMINED;
}

inline const char* selectivityClassName(SelectivityClass c) {
    switch (c) {
        case SelectivityClass::FLAT_OR_GOOD: return "FLAT/GOOD";
        case SelectivityClass::GOOD:         return "GOOD";
        case SelectivityClass::MODERATE:     return "MODERATE";
        case SelectivityClass::POOR:         return "POOR";
        default:                             return "UNDETERMINED";
    }
}

}  // namespace ofdm
}  // namespace ultra
