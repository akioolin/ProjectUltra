#include "ultra/tx_burst_normalization.hpp"

#include "ota_channel_core/models.hpp"
#include "ultra/logging.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ultra::sim {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr size_t kReferenceBandFirTaps = 101;
constexpr float kReferenceBandLowHz = 50.0f;
constexpr float kReferenceBandHighHz = 2950.0f;
constexpr float kReferenceSampleRate = 48000.0f;

std::vector<float> makeReferenceBandFirCoefficients() {
    std::vector<float> coeffs(kReferenceBandFirTaps);
    const float fc_low = kReferenceBandLowHz / kReferenceSampleRate;
    const float fc_high = kReferenceBandHighHz / kReferenceSampleRate;
    const int midpoint = static_cast<int>((kReferenceBandFirTaps - 1) / 2);

    for (int n = 0; n < static_cast<int>(kReferenceBandFirTaps); ++n) {
        if (n == midpoint) {
            coeffs[static_cast<size_t>(n)] = 2.0f * (fc_high - fc_low);
        } else {
            const float x = kPi * static_cast<float>(n - midpoint);
            coeffs[static_cast<size_t>(n)] =
                (std::sin(2.0f * fc_high * x) -
                 std::sin(2.0f * fc_low * x)) / x;
        }
        const float w = 2.0f * kPi * static_cast<float>(n) /
                        static_cast<float>(kReferenceBandFirTaps - 1);
        coeffs[static_cast<size_t>(n)] *=
            0.42f - 0.5f * std::cos(w) + 0.08f * std::cos(2.0f * w);
    }
    return coeffs;
}

float processFirSample(float sample,
                       const std::vector<float>& coeffs,
                       std::vector<float>& delay_line,
                       size_t& delay_idx) {
    delay_line[delay_idx] = sample;

    float out = 0.0f;
    size_t j = delay_idx;
    for (size_t i = 0; i < coeffs.size(); ++i) {
        out += coeffs[i] * delay_line[j];
        if (j == 0) {
            j = coeffs.size();
        }
        --j;
    }

    delay_idx = (delay_idx + 1) % coeffs.size();
    return out;
}

void updatePeakStats(TxBurstRmsMeasurement& result,
                     std::span<const float> samples) {
    result.peak_after_gain = 0.0f;
    result.peak_warning_samples = 0;
    result.peak_clip_samples = 0;
    for (float sample : samples) {
        const float after_gain = std::abs(sample * result.gain_to_reference);
        result.peak_after_gain =
            std::max(result.peak_after_gain, after_gain);
        if (after_gain > kTxBurstPeakWarningThreshold) {
            ++result.peak_warning_samples;
        }
        if (after_gain > kTxBurstPeakClipThreshold) {
            ++result.peak_clip_samples;
        }
    }
    result.peak_warning = result.peak_warning_samples > 0;
    result.peak_clip_error = result.peak_clip_samples > 0;
}

}  // namespace

const std::vector<float>& referenceBandFirCoefficients() {
    static const std::vector<float> coeffs = makeReferenceBandFirCoefficients();
    return coeffs;
}

TxBurstRmsMeasurement measureTxBurstInBandRms(std::span<const float> samples) {
    TxBurstRmsMeasurement result;
    if (samples.empty()) {
        return result;
    }

    size_t begin = 0;
    while (begin < samples.size() &&
           std::abs(samples[begin]) <= kTxBurstActiveThreshold) {
        ++begin;
    }

    size_t end = samples.size();
    while (end > begin &&
           std::abs(samples[end - 1]) <= kTxBurstActiveThreshold) {
        --end;
    }

    result.active_begin = begin;
    result.active_end = end;
    result.active_samples = end > begin ? end - begin : 0;
    result.burst_fragment_warning =
        result.active_samples > 0 &&
        result.active_samples < kTxBurstMinimumActiveSamples;

    if (result.active_samples == 0) {
        updatePeakStats(result, samples);
        return result;
    }

    const auto& coeffs = referenceBandFirCoefficients();
    std::vector<float> delay_line(coeffs.size(), 0.0f);
    size_t delay_idx = 0;
    double broadband_sum_sq = 0.0;
    double in_band_sum_sq = 0.0;

    for (size_t i = 0; i < end; ++i) {
        const float sample = samples[i];
        const float filtered =
            processFirSample(sample, coeffs, delay_line, delay_idx);
        if (i >= begin) {
            broadband_sum_sq += static_cast<double>(sample) *
                                static_cast<double>(sample);
            in_band_sum_sq += static_cast<double>(filtered) *
                              static_cast<double>(filtered);
        }
    }

    result.broadband_rms = static_cast<float>(
        std::sqrt(broadband_sum_sq / static_cast<double>(result.active_samples)));
    result.in_band_rms = static_cast<float>(
        std::sqrt(in_band_sum_sq / static_cast<double>(result.active_samples)));

    if (result.in_band_rms > std::numeric_limits<float>::min()) {
        result.gain_to_reference =
            ota_channel_core::kModemReferenceInBandRms / result.in_band_rms;
    }

    updatePeakStats(result, samples);
    return result;
}

TxBurstRmsMeasurement normalizeTxBurstToReference(std::vector<float>& samples) {
    const TxBurstRmsMeasurement result = measureTxBurstInBandRms(samples);
    if (result.burst_fragment_warning) {
        LOG_WARN("SIM",
                 "TX burst normalization called on fragment: active=%zu "
                 "minimum=%zu samples=%zu",
                 result.active_samples,
                 kTxBurstMinimumActiveSamples,
                 samples.size());
    }
    if (samples.empty() || result.active_samples == 0 ||
        !(result.gain_to_reference > 0.0f) ||
        !std::isfinite(result.gain_to_reference)) {
        return result;
    }

    for (float& sample : samples) {
        sample *= result.gain_to_reference;
    }
    return result;
}

}  // namespace ultra::sim
