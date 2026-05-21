#include "ultra/papr_reduction.hpp"

#include "ultra/tx_burst_normalization.hpp"

#define POCKETFFT_CACHE_SIZE 64
#if defined(__APPLE__) || defined(__unix__)
#define POCKETFFT_USE_POSIX_MEMALIGN
#endif

#include "pocketfft/pocketfft_hdronly.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>

namespace ultra::phy {
namespace {

using Complex = std::complex<float>;

float dbRmsRatio(float numerator, float denominator) {
    if (!(numerator > 0.0f) || !(denominator > 0.0f)) {
        return 0.0f;
    }
    return 20.0f * std::log10(numerator / denominator);
}

float paprDb(float peak, float rms) {
    if (!(peak > 0.0f) || !(rms > 0.0f)) {
        return 0.0f;
    }
    return 20.0f * std::log10(peak / rms);
}

float maxAbs(std::span<const float> samples) {
    float peak = 0.0f;
    for (float sample : samples) {
        peak = std::max(peak, std::abs(sample));
    }
    return peak;
}

size_t nextPowerOfTwo(size_t n) {
    size_t out = 1;
    while (out < n) {
        out <<= 1;
    }
    return out;
}

std::vector<Complex> analyticSignal(std::span<const float> samples) {
    if (samples.empty()) {
        return {};
    }

    const size_t fft_size = nextPowerOfTwo(std::max<size_t>(2, samples.size() * 2));
    std::vector<Complex> time(fft_size, Complex(0.0f, 0.0f));
    std::vector<Complex> freq(fft_size, Complex(0.0f, 0.0f));
    std::vector<Complex> analytic(fft_size, Complex(0.0f, 0.0f));
    for (size_t i = 0; i < samples.size(); ++i) {
        time[i] = Complex(samples[i], 0.0f);
    }

    const pocketfft::shape_t shape{fft_size};
    const pocketfft::stride_t stride{static_cast<std::ptrdiff_t>(sizeof(Complex))};
    const pocketfft::shape_t axes{0};
    pocketfft::c2c<float>(shape, stride, stride, axes,
                          pocketfft::FORWARD, time.data(), freq.data(), 1.0f);

    if (fft_size >= 2) {
        for (size_t i = 1; i < fft_size / 2; ++i) {
            freq[i] *= 2.0f;
        }
        for (size_t i = fft_size / 2 + 1; i < fft_size; ++i) {
            freq[i] = Complex(0.0f, 0.0f);
        }
    }

    pocketfft::c2c<float>(shape, stride, stride, axes,
                          pocketfft::BACKWARD, freq.data(), analytic.data(),
                          1.0f / static_cast<float>(fft_size));
    analytic.resize(samples.size());
    return analytic;
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

std::vector<float> referenceBandLimitAligned(std::span<const float> samples) {
    const auto& coeffs = sim::referenceBandFirCoefficients();
    if (samples.empty() || coeffs.empty()) {
        return std::vector<float>(samples.begin(), samples.end());
    }

    const size_t delay = coeffs.size() / 2;
    std::vector<float> out(samples.size(), 0.0f);
    std::vector<float> delay_line(coeffs.size(), 0.0f);
    size_t delay_idx = 0;

    for (size_t i = 0; i < samples.size() + delay; ++i) {
        const float sample = i < samples.size() ? samples[i] : 0.0f;
        const float filtered =
            processFirSample(sample, coeffs, delay_line, delay_idx);
        if (i >= delay) {
            out[i - delay] = filtered;
        }
    }
    return out;
}

std::vector<float> idealReferenceBandLimit(std::span<const float> samples) {
    if (samples.empty()) {
        return {};
    }

    const size_t fft_size = nextPowerOfTwo(std::max<size_t>(2, samples.size() * 2));
    std::vector<Complex> time(fft_size, Complex(0.0f, 0.0f));
    std::vector<Complex> freq(fft_size, Complex(0.0f, 0.0f));
    std::vector<Complex> limited(fft_size, Complex(0.0f, 0.0f));
    for (size_t i = 0; i < samples.size(); ++i) {
        time[i] = Complex(samples[i], 0.0f);
    }

    const pocketfft::shape_t shape{fft_size};
    const pocketfft::stride_t stride{static_cast<std::ptrdiff_t>(sizeof(Complex))};
    const pocketfft::shape_t axes{0};
    pocketfft::c2c<float>(shape, stride, stride, axes,
                          pocketfft::FORWARD, time.data(), freq.data(), 1.0f);

    for (size_t bin = 0; bin < fft_size; ++bin) {
        const float frequency =
            bin <= fft_size / 2
                ? sim::kReferenceSampleRateHz * static_cast<float>(bin) /
                      static_cast<float>(fft_size)
                : sim::kReferenceSampleRateHz *
                      static_cast<float>(static_cast<std::ptrdiff_t>(bin) -
                                         static_cast<std::ptrdiff_t>(fft_size)) /
                      static_cast<float>(fft_size);
        const float abs_frequency = std::abs(frequency);
        if (abs_frequency < sim::kReferenceBandLowHz ||
            abs_frequency > sim::kReferenceBandHighHz) {
            freq[bin] = Complex(0.0f, 0.0f);
        }
    }

    pocketfft::c2c<float>(shape, stride, stride, axes,
                          pocketfft::BACKWARD, freq.data(), limited.data(),
                          1.0f / static_cast<float>(fft_size));

    std::vector<float> out(samples.size(), 0.0f);
    for (size_t i = 0; i < samples.size(); ++i) {
        out[i] = std::real(limited[i]);
    }
    return out;
}

void fillBroadbandStats(PaprReductionMeasurement& result,
                        std::span<const float> samples,
                        bool post) {
    const auto measurement = sim::measureTxBurstInBandRms(samples);
    const size_t begin = measurement.active_begin;
    const size_t end = measurement.active_end;
    const std::span<const float> active =
        end > begin ? samples.subspan(begin, end - begin) : std::span<const float>{};
    const float peak = maxAbs(active);
    const float rms = measurement.broadband_rms;

    if (post) {
        result.post_peak = peak;
        result.post_rms = rms;
        result.post_papr_db = paprDb(peak, rms);
        result.post_in_band_rms = measurement.in_band_rms;
    } else {
        result.active_begin = begin;
        result.active_end = end;
        result.active_samples = measurement.active_samples;
        result.pre_peak = peak;
        result.pre_rms = rms;
        result.pre_papr_db = paprDb(peak, rms);
        result.pre_in_band_rms = measurement.in_band_rms;
    }
}

float softSaturateMagnitude(float magnitude, float threshold) {
    if (!(magnitude > 0.0f) || !(threshold > 0.0f)) {
        return magnitude;
    }
    const float knee = 0.75f * threshold;
    if (magnitude <= knee) {
        return magnitude;
    }
    const float span = threshold - knee;
    return knee + span * std::tanh((magnitude - knee) / span);
}

}  // namespace

float measurePaprDb(std::span<const float> samples) {
    PaprReductionMeasurement result;
    fillBroadbandStats(result, samples, false);
    return result.pre_papr_db;
}

PaprReductionMeasurement applyPaprReduction(std::vector<float>& samples,
                                            float threshold_db,
                                            bool enable) {
    PaprReductionMeasurement result;
    result.enabled = enable;
    if (!std::isfinite(threshold_db) || !(threshold_db > 0.0f)) {
        threshold_db = kOfdmPaprReductionDefaultThresholdDb;
    }
    fillBroadbandStats(result, samples, false);
    if (!enable || samples.empty() || result.active_samples == 0 ||
        !(result.pre_rms > 0.0f)) {
        result.threshold_db = threshold_db;
        fillBroadbandStats(result, samples, true);
        return result;
    }
    if (result.pre_papr_db <= kOfdmPaprReductionMinPrePaprDb) {
        result.threshold_db = threshold_db;
        fillBroadbandStats(result, samples, true);
        return result;
    }

    const float adaptive_threshold_db =
        std::max(0.0f, result.pre_papr_db -
                            kOfdmPaprReductionTargetReductionDb);
    threshold_db = std::min(threshold_db, adaptive_threshold_db);
    result.threshold_db = threshold_db;

    const float crest = std::pow(10.0f, threshold_db / 20.0f);
    result.threshold_amplitude = result.pre_rms * crest;
    if (!(result.threshold_amplitude > 0.0f) ||
        result.pre_papr_db <= threshold_db) {
        fillBroadbandStats(result, samples, true);
        return result;
    }

    std::vector<float> working(samples);
    float last_takeback_gain = 1.0f;
    for (int iteration = 0; iteration < 1; ++iteration) {
        const auto current_measure = sim::measureTxBurstInBandRms(working);
        if (current_measure.active_samples == 0 ||
            !(current_measure.broadband_rms > 0.0f)) {
            break;
        }

        const float threshold_amplitude =
            current_measure.broadband_rms * crest;
        if (iteration == 0) {
            result.threshold_amplitude = threshold_amplitude;
        }

        const std::span<const float> active(
            working.data() + static_cast<std::ptrdiff_t>(current_measure.active_begin),
            current_measure.active_samples);
        const std::vector<Complex> analytic = analyticSignal(active);
        std::vector<float> clipped(working);
        size_t iteration_clips = 0;
        for (size_t i = 0; i < analytic.size(); ++i) {
            Complex sample = analytic[i];
            const float mag = std::abs(sample);
            const float saturated_mag =
                softSaturateMagnitude(mag, threshold_amplitude);
            if (saturated_mag < mag) {
                sample *= saturated_mag / mag;
                clipped[current_measure.active_begin + i] = std::real(sample);
                ++iteration_clips;
            }
        }

        if (iteration_clips == 0) {
            break;
        }
        result.clipped_samples += iteration_clips;

        std::vector<float> filtered = idealReferenceBandLimit(
            referenceBandLimitAligned(
                referenceBandLimitAligned(referenceBandLimitAligned(clipped))));
        const auto active_filtered = sim::measureTxBurstInBandRms(filtered);
        if (active_filtered.active_samples > 0) {
            double mean = 0.0;
            for (size_t i = active_filtered.active_begin;
                 i < active_filtered.active_end && i < filtered.size(); ++i) {
                mean += filtered[i];
            }
            mean /= static_cast<double>(active_filtered.active_samples);
            for (size_t i = active_filtered.active_begin;
                 i < active_filtered.active_end && i < filtered.size(); ++i) {
                filtered[i] -= static_cast<float>(mean);
            }
        }
        const auto filtered_measure = sim::measureTxBurstInBandRms(filtered);
        if (result.pre_in_band_rms > 0.0f &&
            filtered_measure.in_band_rms > 0.0f &&
            std::isfinite(result.pre_in_band_rms) &&
            std::isfinite(filtered_measure.in_band_rms)) {
            last_takeback_gain =
                result.pre_in_band_rms / filtered_measure.in_band_rms;
            for (float& sample : filtered) {
                sample *= last_takeback_gain;
            }
        }

        working = std::move(filtered);

        PaprReductionMeasurement current;
        fillBroadbandStats(current, working, false);
        if (current.pre_papr_db <= threshold_db + 0.2f) {
            break;
        }
    }

    if (result.clipped_samples == 0) {
        fillBroadbandStats(result, samples, true);
        return result;
    }

    result.rms_takeback_gain = last_takeback_gain;
    samples = std::move(working);
    result.applied = true;
    fillBroadbandStats(result, samples, true);
    result.in_band_rms_delta_db =
        dbRmsRatio(result.post_in_band_rms, result.pre_in_band_rms);
    return result;
}

}  // namespace ultra::phy
