// Phase 0 (EFFECTIVE_SINR handoff §9): pin the noise-variance contract.
//
// Four quantities circulate in the OFDM RX chain -- per-real time-domain variance,
// per-bin per-symbol complex variance, the M-symbol AVERAGED variance, and the
// per-real-component variance the LLRs are written in. They differ by factors of
// N, 1/M and 1/2, they are all called "noise variance", and they are all stored as
// bare float. That is how a 3.01 dB error lived in the decode path while the meter
// path compensated for it three lines away.
//
// The most load-bearing link is sigma_bin^2 = fft_size * sigma_t^2. It is what
// converts the SIMULATOR's injected noise into the units the equalizer's MMSE
// denominator and LLR scaling actually work in, and it rests entirely on the FFT
// being UNNORMALIZED on the forward transform. That is a property of a third-party
// header (pocketfft, via src/dsp/fft.cpp) which nothing in-tree was checking. If a
// future FFT swap normalizes the forward direction, every noise-referenced
// quantity silently moves by 10*log10(N) = 30.1 dB at N=1024 -- so it is measured
// here rather than assumed.

#include "ofdm/noise_variance_contract.hpp"
#include "ultra/dsp.hpp"
#include "ultra/types.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

namespace {

namespace nv = ultra::ofdm::noise;
using ultra::Complex;

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

}  // namespace

int main() {
    std::printf("=== noise-variance contract (Phase 0) ===\n");

    // ---- The declared conversions are mutually consistent.
    {
        const nv::RealSampleVariance t{0.25f};
        const size_t n = 1024;
        const nv::BinVariancePerSymbol bin = nv::toBinVariance(t, n);
        check(std::abs(bin.v - 0.25f * 1024.0f) < 1e-3f,
              "sigma_bin^2 = fft_size * sigma_t^2");

        const nv::BinVarianceAveraged avg = nv::averagedOver(bin, 2);
        check(std::abs(avg.v - bin.v * 0.5f) < 1e-3f,
              "a 2-symbol averaged estimate has HALF the per-symbol variance");

        const nv::BinVariancePerSymbol back = nv::toPerSymbol(avg);
        check(std::abs(back.v - bin.v) < 1e-3f,
              "toPerSymbol(averagedOver(x, M)) round-trips (this is the missing 2x)");

        const nv::PerRealComponentVariance r = nv::toPerRealComponent(bin);
        check(std::abs(r.v - bin.v * 0.5f) < 1e-3f,
              "sigma_r^2 = sigma_bin^2 / 2 (the max-log LLR convention)");
        check(std::abs(nv::fromPerRealComponent(r).v - bin.v) < 1e-3f,
              "per-real-component conversion round-trips");
    }

    // ---- The FFT is UNNORMALIZED on the forward transform, so bin noise power is
    // N times the per-sample power. Measured against the project's own FFT.
    {
        const size_t n = 1024;
        const float sigma_t = 0.3f;
        const double expected_bin_power =
            static_cast<double>(sigma_t) * sigma_t * static_cast<double>(n);

        ultra::FFT fft(n);
        std::mt19937 rng(20260729u);
        std::normal_distribution<float> gauss(0.0f, sigma_t);

        // Average |X_k|^2 over many independent transforms and over the bins.
        // (One transform is a chi-squared with 2 DOF per bin -- averaging over
        // trials is what reduces the variance, not a longer transform.)
        const int trials = 300;
        double acc = 0.0;
        long count = 0;
        std::vector<Complex> in(n), out(n);
        for (int t = 0; t < trials; ++t) {
            for (size_t i = 0; i < n; ++i) {
                // Real noise rotated by a unit-modulus phasor, exactly as
                // toBaseband does: per-sample power stays sigma_t^2.
                const float s = gauss(rng);
                const float ph = 2.0f * static_cast<float>(M_PI) *
                                 static_cast<float>(i) * 0.1234f;
                in[i] = Complex(s * std::cos(ph), s * std::sin(ph));
            }
            fft.forward(in, out);
            for (size_t k = 0; k < n; ++k) {
                acc += static_cast<double>(std::norm(out[k]));
                ++count;
            }
        }
        const double measured = acc / static_cast<double>(count);
        const double db = 10.0 * std::log10(measured / expected_bin_power);
        std::printf("   measured mean |X_k|^2 = %.2f   expected N*sigma_t^2 = %.2f  (%+.3f dB)\n",
                    measured, expected_bin_power, db);
        check(std::abs(db) < 0.25,
              "forward FFT is UNNORMALIZED: E|X_k|^2 = N * sigma_t^2 (within 0.25 dB)");
    }

    // ---- The production geometry the contract is quoted against.
    {
        const ultra::ModemConfig cfg = ultra::presets::balanced();
        check(cfg.fft_size == 1024,
              "production fft_size is 1024, so sigma_bin^2 = 1024 * sigma_t^2 (+30.1 dB)");
        const double db_gap = 10.0 * std::log10(static_cast<double>(cfg.fft_size));
        std::printf("   time-domain -> bin conversion at production geometry: %+.2f dB\n",
                    db_gap);
    }

    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
