#pragma once

#include "ultra/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <vector>

namespace ultra {
namespace ofdm_wiener {

constexpr float kPi = 3.14159265358979323846f;

struct Observation1D {
    float position = 0.0f;
    Complex value = Complex(0, 0);
    float noise_norm = 0.0f;
};

struct Estimate {
    Complex value = Complex(0, 0);
    float error_var = 1.0f;
    bool valid = false;
};

inline float besselJ0(float x) {
    // Cephes/Numerical-Recipes style J0 approximation. This keeps the time
    // correlation model self-contained on standard libraries without cyl_bessel_j.
    const float ax = std::abs(x);
    if (ax < 8.0f) {
        const double y = static_cast<double>(x) * static_cast<double>(x);
        const double ans1 =
            57568490574.0 +
            y * (-13362590354.0 +
            y * (651619640.7 +
            y * (-11214424.18 +
            y * (77392.33017 +
            y * (-184.9052456)))));
        const double ans2 =
            57568490411.0 +
            y * (1029532985.0 +
            y * (9494680.718 +
            y * (59272.64853 +
            y * (267.8532712 +
            y))));
        return static_cast<float>(ans1 / ans2);
    }

    const double z = 8.0 / static_cast<double>(ax);
    const double y = z * z;
    const double xx = static_cast<double>(ax) - 0.785398164;
    const double ans1 =
        1.0 +
        y * (-0.1098628627e-2 +
        y * (0.2734510407e-4 +
        y * (-0.2073370639e-5 +
        y * 0.2093887211e-6)));
    const double ans2 =
        -0.1562499995e-1 +
        y * (0.1430488765e-3 +
        y * (-0.6911147651e-5 +
        y * (0.7621095161e-6 -
        y * 0.934945152e-7)));
    return static_cast<float>(
        std::sqrt(0.636619772 / static_cast<double>(ax)) *
        (std::cos(xx) * ans1 - z * std::sin(xx) * ans2));
}

inline float sinc(float x) {
    if (std::abs(x) < 1.0e-5f) {
        return 1.0f - (x * x) / 6.0f;
    }
    return std::sin(x) / x;
}

inline float timeCorrelation(float symbol_delta,
                             float symbol_period_s,
                             float doppler_hz) {
    const float x = 2.0f * kPi *
                    std::max(0.0f, doppler_hz) *
                    std::max(0.0f, symbol_period_s) *
                    std::abs(symbol_delta);
    return besselJ0(x);
}

inline float frequencyCorrelation(float logical_delta,
                                  float carrier_spacing_hz,
                                  float delay_spread_s) {
    const float df = std::abs(logical_delta) * std::max(0.0f, carrier_spacing_hz);
    const float x = kPi * df * std::max(0.0f, delay_spread_s);
    return sinc(x);
}

inline bool solveRealSystem(std::vector<float> a,
                            std::vector<float> b,
                            std::vector<float>& x) {
    const size_t n = b.size();
    if (a.size() != n * n || n == 0) {
        return false;
    }

    for (size_t col = 0; col < n; ++col) {
        size_t pivot = col;
        float pivot_abs = std::abs(a[col * n + col]);
        for (size_t row = col + 1; row < n; ++row) {
            const float candidate = std::abs(a[row * n + col]);
            if (candidate > pivot_abs) {
                pivot = row;
                pivot_abs = candidate;
            }
        }
        if (pivot_abs < 1.0e-8f || !std::isfinite(pivot_abs)) {
            return false;
        }
        if (pivot != col) {
            for (size_t k = col; k < n; ++k) {
                std::swap(a[col * n + k], a[pivot * n + k]);
            }
            std::swap(b[col], b[pivot]);
        }

        const float diag = a[col * n + col];
        for (size_t row = col + 1; row < n; ++row) {
            const float factor = a[row * n + col] / diag;
            if (factor == 0.0f) {
                continue;
            }
            a[row * n + col] = 0.0f;
            for (size_t k = col + 1; k < n; ++k) {
                a[row * n + k] -= factor * a[col * n + k];
            }
            b[row] -= factor * b[col];
        }
    }

    x.assign(n, 0.0f);
    for (int row = static_cast<int>(n) - 1; row >= 0; --row) {
        float sum = b[static_cast<size_t>(row)];
        for (size_t k = static_cast<size_t>(row) + 1; k < n; ++k) {
            sum -= a[static_cast<size_t>(row) * n + k] * x[k];
        }
        const float diag = a[static_cast<size_t>(row) * n + static_cast<size_t>(row)];
        if (std::abs(diag) < 1.0e-8f) {
            return false;
        }
        x[static_cast<size_t>(row)] = sum / diag;
    }
    return true;
}

template <typename CorrFn>
inline Estimate estimate1D(const std::vector<Observation1D>& observations,
                           float target_position,
                           size_t max_observations,
                           CorrFn corr) {
    if (observations.empty()) {
        return Estimate{};
    }

    std::vector<size_t> order(observations.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const float da = std::abs(observations[a].position - target_position);
        const float db = std::abs(observations[b].position - target_position);
        return da < db;
    });
    const size_t n = std::min(order.size(), std::max<size_t>(1, max_observations));

    std::vector<float> a(n * n, 0.0f);
    std::vector<float> r(n, 0.0f);
    for (size_t row = 0; row < n; ++row) {
        const Observation1D& oi = observations[order[row]];
        r[row] = corr(std::abs(oi.position - target_position));
        for (size_t col = 0; col < n; ++col) {
            const Observation1D& oj = observations[order[col]];
            a[row * n + col] = corr(std::abs(oi.position - oj.position));
        }
        a[row * n + row] += std::max(oi.noise_norm, 1.0e-5f);
    }

    std::vector<float> weights;
    if (!solveRealSystem(a, r, weights)) {
        return Estimate{observations[order[0]].value, 1.0f, true};
    }

    Complex value(0, 0);
    float explained = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        value += weights[i] * observations[order[i]].value;
        explained += weights[i] * r[i];
    }
    const float error_var = std::clamp(1.0f - explained, 0.0f, 1.0f);
    return Estimate{value, error_var, true};
}

}  // namespace ofdm_wiener
}  // namespace ultra
