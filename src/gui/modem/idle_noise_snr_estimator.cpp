#include "idle_noise_snr_estimator.hpp"

#include "sim/channel_calibration.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace ultra {
namespace gui {

namespace {

double safePositive(double value, double fallback) {
    return value > 0.0 && std::isfinite(value) ? value : fallback;
}

}  // namespace

IdleNoiseSNREstimator::IdleNoiseSNREstimator()
    : IdleNoiseSNREstimator(Config{}) {}

IdleNoiseSNREstimator::IdleNoiseSNREstimator(const Config& config)
    : config_(config),
      filter_(FIRFilter::bandpass(
          static_cast<size_t>(std::max(3, config.filter_taps)),
          config.band_low_hz,
          config.band_high_hz,
          config.sample_rate)) {
    fir_energy_ = safePositive(coefficientEnergy(filter_), 1.0);
    equivalent_noise_bandwidth_hz_ = 0.5 * static_cast<double>(config_.sample_rate) *
                                     fir_energy_;
}

void IdleNoiseSNREstimator::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    filter_.reset();
    window_sum_sq_ = 0.0;
    window_fill_ = 0;
    valid_ = false;
    smoothed_snr_db_ = 0.0f;
    latest_instant_snr_db_ = 0.0f;
    filtered_noise_rms_ = 0.0f;
    normalized_noise_rms_ = 0.0f;
    windows_observed_ = 0;
}

void IdleNoiseSNREstimator::observeIdleAudio(const float* samples, size_t count) {
    if (!samples || count == 0 || config_.window_samples == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    for (size_t i = 0; i < count; ++i) {
        const float filtered = filter_.process(samples[i]);
        window_sum_sq_ += static_cast<double>(filtered) * static_cast<double>(filtered);
        ++window_fill_;

        if (window_fill_ < config_.window_samples) {
            continue;
        }

        const double filtered_power =
            window_sum_sq_ / static_cast<double>(config_.window_samples);

        // Bandwidth-normalization derivation:
        //
        // The receive chain uses the same windowed FIR bandpass family as the
        // operator audio filter. For idle white receiver noise x[n] with sample
        // variance sigma^2, the FIR output is y[n] = sum_k h[k] x[n-k]. Because
        // white samples are uncorrelated, first principles/Parseval give
        //
        //     E{y^2} = sigma^2 * sum_k h[k]^2 .
        //
        // The actual 101-tap Blackman bandpass configured here is 50-2950 Hz,
        // matching the default input-chain filter around the 2.8 kHz modem
        // band. Its finite-transition coefficient energy is not the ideal
        // 2800/24000 rectangular fraction; with the current coefficients
        // sum(h^2) ~= 0.1086, i.e. ENBW ~= 24000 * sum(h^2) ~= 2.61 kHz.
        // Comparing kModemReferenceRms^2 directly to filtered_power would
        // therefore read about +10*log10(1/sum(h^2)) = +9.64 dB high on the
        // locked SimulatedChannel/ChannelSNRProbe reference, which defines SNR
        // against sigma^2 at the receiver samples. The non-heuristic correction
        // is to divide by the actual coefficient energy:
        //
        //     P_noise_reference = filtered_power / sum_k h[k]^2 .
        //
        // This single factor accounts for both the FIR integration bandwidth and
        // passband gain because it is computed from the exact coefficients being
        // applied to the idle window.
        const double normalized_noise_power =
            filtered_power / safePositive(fir_energy_, 1.0);
        const double bounded_noise_power =
            std::max(normalized_noise_power, std::numeric_limits<double>::min());
        const float snr_db = static_cast<float>(
            10.0 * std::log10(sim::kModemReferencePower / bounded_noise_power));

        latest_instant_snr_db_ = snr_db;
        filtered_noise_rms_ = static_cast<float>(std::sqrt(std::max(0.0, filtered_power)));
        normalized_noise_rms_ = static_cast<float>(std::sqrt(bounded_noise_power));
        if (!valid_) {
            smoothed_snr_db_ = snr_db;
            valid_ = true;
        } else {
            const float alpha = std::clamp(config_.ema_alpha, 0.0f, 1.0f);
            smoothed_snr_db_ = alpha * snr_db + (1.0f - alpha) * smoothed_snr_db_;
        }
        ++windows_observed_;

        window_sum_sq_ = 0.0;
        window_fill_ = 0;
    }
}

bool IdleNoiseSNREstimator::hasEstimate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return valid_;
}

float IdleNoiseSNREstimator::snrDb() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return smoothed_snr_db_;
}

IdleNoiseSNREstimator::Snapshot IdleNoiseSNREstimator::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot out;
    out.valid = valid_;
    out.snr_db = smoothed_snr_db_;
    out.latest_instant_snr_db = latest_instant_snr_db_;
    out.filtered_noise_rms = filtered_noise_rms_;
    out.normalized_noise_rms = normalized_noise_rms_;
    out.fir_energy = fir_energy_;
    out.equivalent_noise_bandwidth_hz = equivalent_noise_bandwidth_hz_;
    out.windows_observed = windows_observed_;
    return out;
}

double IdleNoiseSNREstimator::coefficientEnergy(const FIRFilter& filter) {
    const auto& coeffs = filter.coefficients();
    return std::accumulate(coeffs.begin(), coeffs.end(), 0.0,
                           [](double sum, float h) {
                               return sum + static_cast<double>(h) *
                                            static_cast<double>(h);
                           });
}

}  // namespace gui
}  // namespace ultra
