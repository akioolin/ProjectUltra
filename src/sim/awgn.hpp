#pragma once

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace ultra::sim::awgn {

constexpr float kActiveSampleEpsilon = 1.0e-6f;

inline double activeSignalPower(const std::vector<float>& samples,
                                float epsilon = kActiveSampleEpsilon) {
    double sum_sq = 0.0;
    size_t count = 0;
    for (float s : samples) {
        if (std::abs(s) > epsilon) {
            sum_sq += static_cast<double>(s) * static_cast<double>(s);
            ++count;
        }
    }
    return count > 0 ? sum_sq / static_cast<double>(count) : 0.0;
}

inline double noisePowerForSNR(double signal_power, float snr_db) {
    if (signal_power <= 0.0) {
        return 0.0;
    }
    const double snr_linear = std::pow(10.0, static_cast<double>(snr_db) / 10.0);
    return signal_power / std::max(snr_linear, 1.0e-12);
}

inline float noiseStddevForSNR(double signal_power, float snr_db) {
    return static_cast<float>(std::sqrt(noisePowerForSNR(signal_power, snr_db)));
}

template <typename Rng>
inline void addAWGN(std::vector<float>& samples,
                    float snr_db,
                    Rng& rng,
                    float no_noise_snr_db = 80.0f,
                    bool clamp_output = false) {
    if (samples.empty() || snr_db >= no_noise_snr_db) {
        return;
    }

    const float sigma = noiseStddevForSNR(activeSignalPower(samples), snr_db);
    if (sigma <= 0.0f) {
        return;
    }

    std::normal_distribution<float> noise(0.0f, sigma);
    for (float& s : samples) {
        s += noise(rng);
        if (clamp_output) {
            s = std::clamp(s, -1.0f, 1.0f);
        }
    }
}

}  // namespace ultra::sim::awgn
