#pragma once
// Active Constellation Extension (ACE) PAPR reduction.
//
// WHY ACE (not clipping, not tone reservation) — measured on the rig 2026-07-13:
//   - Crude amplitude clipping recovers ~4.5 dB of average power (real,
//     hardware-only benefit under peak-normalization) but injects EVM that
//     CRATERS clean channels (−58%: it damages a signal the RX decodes fine).
//   - Tone reservation needs ~4-6 spare in-band carriers; our spectrum is
//     packed (59 carriers in ~61 in-band bins, only DC+1 edge spare), so it
//     would require a frame-geometry redesign.
//   ACE fits a packed spectrum: it extends OUTER constellation points OUTWARD
//   (away from their decision boundaries) to cancel time-domain peaks. The RX
//   sees points FARTHER from where it would misread them, so BER is
//   equal-or-better — never worse. No reserved carriers, no frame change, no
//   RX change (the demapper's decision regions are unbounded on the outward
//   side). The two invariants below are what make it safe; the unit test
//   (test_papr_ace) proves both hold.
//
// INVARIANT 1 (safety): every data carrier moves ONLY in its outward-safe
//   region — radially outward for PSK, axis-outward on an outer level for QAM.
//   No point moves toward any decision boundary. Pilots and empty bins are
//   never touched (channel estimation and the reserved bins are preserved).
// INVARIANT 2 (benefit): post-PAPR <= pre-PAPR (peaks only shrink).
//
// Algorithm: clip-and-project (Krongold & Jones 2003). Clip the time signal,
// take the clip residual's spectrum, add back ONLY the outward-safe component
// on data carriers, iterate a few times. The projection is what confines the
// correction to the safe region.

#include "ultra/types.hpp"
#include "ultra/dsp.hpp"
#include "ofdm/demodulator_constants.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ultra {
namespace ofdm {
namespace papr_ace {

struct AceResult {
    bool applied = false;
    float pre_papr_db = 0.0f;
    float post_papr_db = 0.0f;
    int iterations = 0;
    size_t data_carriers_extended = 0;
};

// Outer-level magnitude (in scaled constellation units) per QAM axis: the
// point is "outer" on an axis if its coordinate sits beyond the last inner
// decision boundary, so extending it further out crosses no boundary.
// 16QAM I/Q levels {-3,-1,1,3}·s → outer at ±3·s, inner/outer boundary at 2·s.
// 32QAM (cross): I {-3..3}·s, Q {-7..7}·s → outer boundary 2·s (I) / 6·s (Q).
struct QamAxisSpec {
    float scale = 0.0f;         // constellation scale (1/sqrt(Es))
    float i_outer_boundary = 0.0f;  // |I| beyond this ⇒ outer on I
    float q_outer_boundary = 0.0f;  // |Q| beyond this ⇒ outer on Q
    bool valid = false;
};

// QAM16_SCALE lives in modulator.cpp (not the shared demod constants); mirror
// the value here (1/sqrt(10)) so ACE and the TX constellation agree exactly.
inline constexpr float kAceQam16Scale = 0.3162277660168379f;  // 1/sqrt(10)

inline QamAxisSpec qamAxisSpec(Modulation mod) {
    using namespace demod_constants;
    QamAxisSpec s;
    if (mod == Modulation::QAM16) {
        s.scale = kAceQam16Scale;
        s.i_outer_boundary = 2.0f * kAceQam16Scale;  // between level 1 and 3
        s.q_outer_boundary = 2.0f * kAceQam16Scale;
        s.valid = true;
    } else if (mod == Modulation::QAM32) {
        s.scale = QAM32_SCALE;
        s.i_outer_boundary = 2.0f * QAM32_SCALE;  // I levels ±1,±3 → outer ±3
        s.q_outer_boundary = 6.0f * QAM32_SCALE;  // Q levels ±1..±7 → outer ±7
        s.valid = true;
    }
    return s;
}

inline bool isPskMod(Modulation mod) {
    // All constellations whose decisions are by phase sector (points on a
    // circle) → safe extension is radially outward. QAM8 is the 8PSK
    // constellation (types.hpp:69), so it belongs here, not with rectangular QAM.
    return mod == Modulation::DBPSK || mod == Modulation::BPSK ||
           mod == Modulation::DQPSK || mod == Modulation::QPSK ||
           mod == Modulation::D8PSK || mod == Modulation::QAM8;
}

// Project a desired correction `delta` for a data symbol `s` onto its
// outward-safe region. Returns the allowed part (added to s stays decodable).
inline Complex aceProject(Complex s, Complex delta, Modulation mod,
                          const QamAxisSpec& qam) {
    if (isPskMod(mod)) {
        // PSK decisions are by phase sector: safe = radially OUTWARD only
        // (increase magnitude, never rotate). Project delta onto the unit
        // vector s/|s| and keep only the outward (positive-radial) part.
        const float mag = std::abs(s);
        if (mag < 1e-9f) return Complex(0.0f, 0.0f);
        const Complex u = s / mag;
        const float radial = delta.real() * u.real() + delta.imag() * u.imag();
        if (radial <= 0.0f) return Complex(0.0f, 0.0f);
        return u * radial;
    }
    if (qam.valid) {
        // QAM decisions are per-axis at integer half-levels: safe = extend an
        // OUTER-level coordinate further out on that axis only. Inner
        // coordinates are pinned (moving them crosses a boundary).
        float dI = 0.0f, dQ = 0.0f;
        if (s.real() > qam.i_outer_boundary && delta.real() > 0.0f) dI = delta.real();
        else if (s.real() < -qam.i_outer_boundary && delta.real() < 0.0f) dI = delta.real();
        if (s.imag() > qam.q_outer_boundary && delta.imag() > 0.0f) dQ = delta.imag();
        else if (s.imag() < -qam.q_outer_boundary && delta.imag() < 0.0f) dQ = delta.imag();
        return Complex(dI, dQ);
    }
    return Complex(0.0f, 0.0f);  // unknown mod → no extension (safe no-op)
}

inline float paprDb(const std::vector<Complex>& x) {
    double sum_sq = 0.0, peak_sq = 0.0;
    for (const auto& v : x) {
        const double p = static_cast<double>(v.real()) * v.real() +
                         static_cast<double>(v.imag()) * v.imag();
        sum_sq += p;
        if (p > peak_sq) peak_sq = p;
    }
    if (sum_sq <= 0.0) return 0.0f;
    const double mean = sum_sq / static_cast<double>(x.size());
    return static_cast<float>(10.0 * std::log10(peak_sq / mean));
}

// In-place ACE on the freq-domain symbol. `clip_ratio` is the peak/RMS target
// in linear (e.g. 10^(target_db/20)); `mu` is the per-iteration step (0..1);
// `max_iters` typically 3-4. Data carriers only; pilots/empty bins untouched.
inline AceResult applyAce(std::vector<Complex>& freq_domain,
                          const std::vector<int>& data_indices,
                          Modulation mod, FFT& fft,
                          float clip_ratio, int max_iters, float mu) {
    AceResult r;
    const size_t N = freq_domain.size();
    if (N == 0 || data_indices.empty() || clip_ratio <= 0.0f) return r;
    const QamAxisSpec qam = qamAxisSpec(mod);
    if (!isPskMod(mod) && !qam.valid) return r;  // unsupported mod

    std::vector<Complex> x(N), excess(N), E(N);
    fft.inverse(freq_domain.data(), x.data());
    r.pre_papr_db = paprDb(x);

    // FIXED clip level from the INITIAL RMS: ACE extends points outward, which
    // raises RMS — recomputing A each iteration would chase the rising RMS and
    // stop clipping (self-defeating; measured 0.1 dB). Holding A at the target
    // peak level lets successive iterations keep pushing the peak down toward it.
    double init_sq = 0.0;
    for (const auto& v : x) init_sq += static_cast<double>(v.real()) * v.real() +
                                       static_cast<double>(v.imag()) * v.imag();
    const float init_rms = std::sqrt(static_cast<float>(init_sq / static_cast<double>(N)));
    const float A = clip_ratio * init_rms;
    if (A <= 0.0f) return r;

    for (int it = 0; it < max_iters; ++it) {
        bool any = false;
        for (size_t i = 0; i < N; ++i) {
            const float m = std::abs(x[i]);
            if (m > A) {
                excess[i] = x[i] * ((m - A) / m);  // the part above the clip level
                any = true;
            } else {
                excess[i] = Complex(0.0f, 0.0f);
            }
        }
        if (!any) break;

        // X-domain rep of the time residual: forward(excess). This FFT
        // convention has forward∘inverse = identity (the modulator uses
        // inverse for X→time), so no 1/N — E[bin] is already in freq_domain's
        // units, ready to add to a data carrier.
        fft.forward(excess.data(), E.data());
        size_t extended = 0;
        for (int bin : data_indices) {
            const Complex s = freq_domain[bin];
            // Subtract the residual on this carrier (reduces the peak), but only
            // the outward-safe component.
            const Complex delta_desired = E[bin] * (-mu);
            const Complex allowed = aceProject(s, delta_desired, mod, qam);
            if (allowed.real() != 0.0f || allowed.imag() != 0.0f) {
                freq_domain[bin] = s + allowed;
                ++extended;
            }
        }
        r.data_carriers_extended = std::max(r.data_carriers_extended, extended);
        fft.inverse(freq_domain.data(), x.data());
        r.iterations = it + 1;
        if (extended == 0) break;  // nothing safe left to do
    }

    r.post_papr_db = paprDb(x);
    r.applied = r.iterations > 0;
    return r;
}

}  // namespace papr_ace
}  // namespace ofdm
}  // namespace ultra
