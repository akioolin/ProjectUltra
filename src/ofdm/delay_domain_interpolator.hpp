#pragma once

// Delay-domain (CIR) pilot reconstruction — ULTRA_PILOT_DFT_INTERP.
//
// WHY
// ---
// The HF channel is DELAY-SPARSE: ITU-R F.1487 "Good" is two paths 0.5 ms apart,
// so |H(f)| ripples with period 1/tau = 2000 Hz across a 2766 Hz band. Any
// reconstruction filter that does not know the channel is delay-limited (linear
// interpolation being the worst case) mis-reconstructs |H| between pilots, and a
// GAIN error is fatal for amplitude-carrying constellations (16-QAM's ring bit is
// an absolute |I| vs 2/sqrt(10) comparison — see soft_demap.hpp:95) while being
// invisible to 8PSK (its LLRs are scale-invariant). There is no AGC between the
// equalizer and the demapper, so |H_hat| is the sole amplitude reference.
//
// WHAT THIS IS
// ------------
// The textbook fix is "IDFT the pilot estimates to the CIR, keep only the taps
// inside the channel's delay support, DFT back". For a UNIFORM pilot grid that is
// literally an FFT; for our grid it is NOT, for two reasons that both matter:
//
//   1. The carriers skip DC (k = -29..-1, +1..+30), so the pilot LOGICAL index is
//      not proportional to frequency — an FFT over logical index warps the delay
//      axis. The pre-existing (and never-called) Impl::interpolateChannel() makes
//      exactly this mistake.
//   2. Np * spacing != fft_size (8 * 8 = 64 vs 1024), so the pilots are a WINDOWED
//      subsampling of H, not a complete period. A plain Np-point IDFT therefore
//      does not give the CIR; it gives the CIR aliased AND smeared by a Dirichlet
//      kernel, and "truncate to the CP" is then a no-op.
//
// So we do the same operation in closed form instead of via an FFT. Truncating the
// CIR to a symmetric integer-sample window m in [-M, +M] and returning to the
// frequency domain is exactly an MMSE (Wiener) estimator whose frequency
// correlation function is the DIRICHLET KERNEL of that window:
//
//     R(dk) = (1/W) * sum_{m=-M..M} exp(-j*2*pi*dk*m/Nfft)
//           = sin(pi*dk*W/Nfft) / (W * sin(pi*dk/Nfft)),   W = 2M+1
//
// Derivation (flat delay power profile over the window, white pilot noise):
//     h_p = A c + n,  A[p][m] = exp(-j2*pi*k_p*m/Nfft),
//     E[c c^H] = (P_h/W) I,  E[n n^H] = sigma^2 I
//     H_hat(k) = d_k^T (G + lambda I)^-1 h_p
//     G[p][q] = R(k_p - k_q),  d_k[p] = R(k - k_p),  lambda = sigma^2 / P_h
// which is precisely the normalized system ofdm_wiener::estimate1D already solves
// (it loads the diagonal with the observation's noise_norm = sigma^2/P_h). So the
// ONLY things this header supplies are (a) the correct correlation kernel and
// (b) the correct position variable: the PHYSICAL carrier index k, not the logical
// one. Everything else is reused.
//
// Constant-free by construction: W comes from the channel's delay budget and the
// FFT/pilot geometry in scope; the kernel is a pure function of (dk, W, Nfft).
//
// THREE THINGS THIS BUYS OVER THE PRODUCTION sinc PRIOR
//   - correct geometry across the DC gap (physical k, not logical index);
//   - a DISCRETE, integer-sample delay support (the true model) instead of the
//     continuous-rect prior sinc(pi*df*tau) implies;
//   - an explicit, auditable aliasing/CP clamp instead of an implicit one.
//
// AND WHAT IT DOES NOT BUY (honest limits — see the header comment in the test)
//   - It is NOT exact for an arbitrary 2-tap channel when W > Np. With W taps and
//     only Np pilots the system is underdetermined and the estimator returns the
//     minimum-norm/MMSE solution, which reproduces H at the pilots but not exactly
//     between them. Exactness holds when W <= Np and the true taps lie inside the
//     window. See test_ofdm_delay_domain_interpolator.cpp, which pins BOTH cases.
//   - It cannot beat the pilot geometry: 8 pilots spanning 56 carriers resolve
//     delays no finer than Nfft/56 = 18.3 samples (0.38 ms).

#include "wiener_interpolator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ultra {
namespace ofdm_cir {

// Symmetric integer-sample delay window the reconstruction projects onto.
struct DelayWindow {
    int taps = 0;             // W = 2*half_taps + 1 (always odd)
    int half_taps = 0;        // M
    bool alias_clamped = false;  // clamped by the pilot grid's unambiguous range
    bool cp_clamped = false;     // clamped by the cyclic prefix (the ISI budget)

    bool valid() const { return taps >= 1; }
};

// Choose the delay window from physics + geometry. No tuned constants:
//   - span_s          : the channel delay support the receiver assumes, TWO-SIDED.
//                       Symmetric because the OFDM timing point (and the LTS phase
//                       de-slope applied before interpolation) can land anywhere
//                       inside the CIR — with two equal-power paths the sync peak
//                       is ambiguous, so the CIR can sit at {0,+tau} or {-tau,0}.
//   - cyclic_prefix   : a tap beyond the CP is ISI, not a channel the OFDM model
//                       can represent at all. Hard physical cap.
//   - fft/pilot grid  : two delays differing by fft_size/pilot_spacing produce
//                       IDENTICAL values at every pilot (exp(-j2*pi*k_p/d) = 1 when
//                       k_p is a multiple of d), so they are unresolvable. The
//                       window must not contain such a pair => W <= Nfft/d. This is
//                       the "unambiguous delay range = 1/(pilot spacing in Hz)".
inline DelayWindow chooseDelayWindow(float span_s,
                                     uint32_t sample_rate,
                                     uint32_t fft_size,
                                     uint32_t pilot_spacing,
                                     uint32_t cyclic_prefix) {
    DelayWindow win;
    if (fft_size == 0 || sample_rate == 0) {
        return win;
    }

    long requested = std::lround(static_cast<double>(std::max(0.0f, span_s)) *
                                 static_cast<double>(sample_rate));
    if (requested < 1) {
        requested = 1;
    }

    const long alias_cap = (pilot_spacing > 0)
        ? static_cast<long>(fft_size / pilot_spacing)
        : static_cast<long>(fft_size);
    const long cp_cap = (cyclic_prefix > 0)
        ? static_cast<long>(cyclic_prefix)
        : static_cast<long>(fft_size);

    long taps = requested;
    if (taps > alias_cap) {
        taps = alias_cap;
        win.alias_clamped = true;
    }
    if (taps > cp_cap) {
        taps = cp_cap;
        win.cp_clamped = true;
        win.alias_clamped = false;  // the tighter cap is the one that bound
    }
    if (taps > static_cast<long>(fft_size)) {
        taps = static_cast<long>(fft_size);
    }
    if (taps < 1) {
        taps = 1;
    }
    // Odd width keeps the kernel real, even in dk, and fft_size-periodic.
    if ((taps % 2) == 0) {
        --taps;
    }

    win.taps = static_cast<int>(taps);
    win.half_taps = (win.taps - 1) / 2;
    return win;
}

// Normalized Dirichlet kernel: the frequency correlation of a flat, symmetric,
// integer-sample delay window of `taps` samples. R(0) = 1, real and even in
// carrier_delta, period fft_size (for odd `taps`).
inline float carrierCorrelation(float carrier_delta, int taps, uint32_t fft_size) {
    if (taps <= 1 || fft_size == 0) {
        return 1.0f;
    }
    const double W = static_cast<double>(taps);
    const double x = M_PI * static_cast<double>(carrier_delta) /
                     static_cast<double>(fft_size);
    const double sx = std::sin(x);
    // Only dk == 0 (mod fft_size) is singular; |dk| < fft_size here by construction.
    if (std::abs(sx) < 1.0e-7) {
        // sin(Wx)/(W sin x) -> 1 - (W^2 - 1) x^2 / 6
        return static_cast<float>(1.0 - (W * W - 1.0) * x * x / 6.0);
    }
    return static_cast<float>(std::sin(W * x) / (W * sx));
}

// Symmetric-system solve in DOUBLE. Deliberately not ofdm_wiener::solveRealSystem
// (float32): the delay-window Gram matrix is far more ill-conditioned than the sinc
// one — for the sp8/59-carrier geometry det(G) ~ 1e-22 — because a narrow delay
// window makes neighbouring carriers almost perfectly correlated. At realistic
// loading (lambda = sigma^2/P_h >= 1e-3 at 30 dB) float32 is fine, but the float
// path costs ~4e-3 of relative accuracy at small lambda and would silently degrade
// on a clean channel. An 8..16 element solve in double is free on this hot path.
// Keeping it local also leaves the shipped Wiener path bit-for-bit unchanged.
inline bool solveSymmetricDouble(std::vector<double> a,
                                 std::vector<double> b,
                                 std::vector<double>& x) {
    const size_t n = b.size();
    if (a.size() != n * n || n == 0) {
        return false;
    }
    for (size_t col = 0; col < n; ++col) {
        size_t pivot = col;
        double pivot_abs = std::abs(a[col * n + col]);
        for (size_t row = col + 1; row < n; ++row) {
            const double cand = std::abs(a[row * n + col]);
            if (cand > pivot_abs) {
                pivot = row;
                pivot_abs = cand;
            }
        }
        if (!(pivot_abs > 1.0e-300) || !std::isfinite(pivot_abs)) {
            return false;
        }
        if (pivot != col) {
            for (size_t k = col; k < n; ++k) {
                std::swap(a[col * n + k], a[pivot * n + k]);
            }
            std::swap(b[col], b[pivot]);
        }
        const double diag = a[col * n + col];
        for (size_t row = col + 1; row < n; ++row) {
            const double factor = a[row * n + col] / diag;
            if (factor == 0.0) {
                continue;
            }
            a[row * n + col] = 0.0;
            for (size_t k = col + 1; k < n; ++k) {
                a[row * n + k] -= factor * a[col * n + k];
            }
            b[row] -= factor * b[col];
        }
    }
    x.assign(n, 0.0);
    for (size_t i = n; i-- > 0;) {
        double sum = b[i];
        for (size_t k = i + 1; k < n; ++k) {
            sum -= a[i * n + k] * x[k];
        }
        const double diag = a[i * n + i];
        if (!(std::abs(diag) > 1.0e-300)) {
            return false;
        }
        x[i] = sum / diag;
    }
    return true;
}

// MMSE reconstruction of H at `target_carrier` (PHYSICAL signed carrier index)
// from pilot observations whose `position` field is also the physical carrier
// index. Returns ofdm_wiener::Estimate: .value = H_hat, .error_var = the
// NORMALIZED MMSE residual 1 - d^T (G+lambda I)^-1 d in [0,1].
//
// Same observation-selection policy as ofdm_wiener::estimate1D (the
// `max_observations` nearest by |position - target|), so the two reconstruction
// filters differ ONLY in kernel and position variable.
//
// Band edges are handled by construction: this is a MODEL EVALUATION, not an
// interpolation between bracketing pilots, so a carrier outside the pilot span is
// simply extrapolated under the same delay-support prior — no DFT wrap, no
// discontinuity, no edge-hold. The cost of extrapolation shows up honestly as a
// LARGER error_var, which the callers already consume (DD Kalman pilot variance,
// per_carrier_h_error_var_ / eps_H LLR term).
inline ofdm_wiener::Estimate reconstruct(
        const std::vector<ofdm_wiener::Observation1D>& observations,
        float target_carrier,
        int taps,
        uint32_t fft_size,
        size_t max_observations) {
    if (observations.empty()) {
        return ofdm_wiener::Estimate{};
    }

    std::vector<size_t> order(observations.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return std::abs(observations[a].position - target_carrier) <
               std::abs(observations[b].position - target_carrier);
    });
    const size_t n = std::min(order.size(),
                              std::max<size_t>(1, max_observations));

    std::vector<double> g(n * n, 0.0);
    std::vector<double> r(n, 0.0);
    for (size_t row = 0; row < n; ++row) {
        const auto& oi = observations[order[row]];
        r[row] = carrierCorrelation(oi.position - target_carrier, taps, fft_size);
        for (size_t col = 0; col < n; ++col) {
            const auto& oj = observations[order[col]];
            g[row * n + col] =
                carrierCorrelation(oi.position - oj.position, taps, fft_size);
        }
        // lambda = sigma^2 / P_h, exactly the MMSE loading derived above.
        g[row * n + row] += std::max(static_cast<double>(oi.noise_norm), 1.0e-9);
    }

    std::vector<double> weights;
    if (!solveSymmetricDouble(g, r, weights)) {
        return ofdm_wiener::Estimate{observations[order[0]].value, 1.0f, true};
    }

    Complex value(0, 0);
    double explained = 0.0;
    for (size_t i = 0; i < n; ++i) {
        value += static_cast<float>(weights[i]) * observations[order[i]].value;
        explained += weights[i] * r[i];
    }
    if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
        return ofdm_wiener::Estimate{observations[order[0]].value, 1.0f, true};
    }
    const float error_var =
        std::clamp(static_cast<float>(1.0 - explained), 0.0f, 1.0f);
    return ofdm_wiener::Estimate{value, error_var, true};
}

}  // namespace ofdm_cir
}  // namespace ultra
