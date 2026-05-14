#pragma once

#include "ultra/dsp.hpp"

#include <cstddef>
#include <mutex>

namespace ultra {
namespace gui {

class IdleNoiseSNREstimator {
public:
    struct Config {
        float sample_rate = 48000.0f;
        size_t window_samples = 9600;  // 200 ms at 48 kHz
        float ema_alpha = 0.30f;
        int filter_taps = 101;
        float band_low_hz = 50.0f;
        float band_high_hz = 2950.0f;
    };

    struct Snapshot {
        bool valid = false;
        float snr_db = 0.0f;
        float latest_instant_snr_db = 0.0f;
        float filtered_noise_rms = 0.0f;
        float normalized_noise_rms = 0.0f;
        double fir_energy = 0.0;
        double equivalent_noise_bandwidth_hz = 0.0;
        size_t windows_observed = 0;
    };

    IdleNoiseSNREstimator();
    explicit IdleNoiseSNREstimator(const Config& config);

    void reset();
    void observeIdleAudio(const float* samples, size_t count);

    bool hasEstimate() const;
    float snrDb() const;
    Snapshot snapshot() const;

private:
    static double coefficientEnergy(const FIRFilter& filter);

    Config config_;
    FIRFilter filter_;
    double fir_energy_ = 1.0;
    double equivalent_noise_bandwidth_hz_ = 0.0;

    mutable std::mutex mutex_;
    double window_sum_sq_ = 0.0;
    size_t window_fill_ = 0;
    bool valid_ = false;
    float smoothed_snr_db_ = 0.0f;
    float latest_instant_snr_db_ = 0.0f;
    float filtered_noise_rms_ = 0.0f;
    float normalized_noise_rms_ = 0.0f;
    size_t windows_observed_ = 0;
};

}  // namespace gui
}  // namespace ultra
