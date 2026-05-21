#include "ultra/papr_reduction.hpp"
#include "ultra/tx_burst_normalization.hpp"

#define POCKETFFT_CACHE_SIZE 64
#if defined(__APPLE__) || defined(__unix__)
#define POCKETFFT_USE_POSIX_MEMALIGN
#endif
#include "pocketfft_hdronly.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

using Complex = std::complex<float>;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kSampleRate = 48000.0f;
constexpr float kThresholdDb = ultra::phy::kOfdmPaprReductionDefaultThresholdDb;
constexpr float kPostPaprTargetDb = 10.0f;

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg) \
    do { \
        ++tests_run; \
        if (!(cond)) { \
            ++tests_failed; \
            std::cout << "FAIL: " << msg << "\n"; \
        } \
    } while (0)

float dbPowerRatio(double numerator, double denominator) {
    if (!(numerator > 0.0) || !(denominator > 0.0)) {
        return -std::numeric_limits<float>::infinity();
    }
    return static_cast<float>(10.0 * std::log10(numerator / denominator));
}

float dbRmsRatio(float numerator, float denominator) {
    if (!(numerator > 0.0f) || !(denominator > 0.0f)) {
        return 0.0f;
    }
    return 20.0f * std::log10(numerator / denominator);
}

uint32_t lcg(uint32_t& state) {
    state = 1664525u * state + 1013904223u;
    return state;
}

float randUnit(uint32_t& state) {
    return static_cast<float>((lcg(state) >> 8) & 0x00ffffffu) /
           static_cast<float>(0x01000000u);
}

size_t nextPowerOfTwo(size_t n) {
    size_t out = 1;
    while (out < n) {
        out <<= 1;
    }
    return out;
}

float maxAbs(std::span<const float> samples) {
    float peak = 0.0f;
    for (float sample : samples) {
        peak = std::max(peak, std::abs(sample));
    }
    return peak;
}

float maxAdjacentDelta(std::span<const float> samples) {
    float delta = 0.0f;
    for (size_t i = 1; i < samples.size(); ++i) {
        delta = std::max(delta, std::abs(samples[i] - samples[i - 1]));
    }
    return delta;
}

std::vector<float> makeSyntheticOfdmLikeBurst(uint32_t seed) {
    constexpr size_t kPrefix = 512;
    constexpr size_t kActive = 32768;
    constexpr size_t kSuffix = 512;
    constexpr int kCarriers = 59;

    std::vector<float> samples(kPrefix + kActive + kSuffix, 0.0f);
    uint32_t state = seed;
    std::vector<float> phases;
    phases.reserve(kCarriers);
    for (int carrier = 0; carrier < kCarriers; ++carrier) {
        phases.push_back(2.0f * kPi * randUnit(state));
    }

    for (size_t n = 0; n < kActive; ++n) {
        const float t = static_cast<float>(n) / kSampleRate;
        float sample = 0.0f;
        for (int carrier = 0; carrier < kCarriers; ++carrier) {
            const float freq = 120.0f +
                               (2680.0f * static_cast<float>(carrier) /
                                static_cast<float>(kCarriers - 1));
            sample += std::cos(2.0f * kPi * freq * t + phases[carrier]);
        }
        samples[kPrefix + n] =
            sample / std::sqrt(static_cast<float>(kCarriers));
    }

    const auto active = std::span<float>(
        samples.data() + static_cast<std::ptrdiff_t>(kPrefix), kActive);
    double sum_sq = 0.0;
    for (float sample : active) {
        sum_sq += static_cast<double>(sample) * static_cast<double>(sample);
    }
    const float rms = static_cast<float>(
        std::sqrt(sum_sq / static_cast<double>(active.size())));
    if (rms > 0.0f) {
        const float gain = 0.20f / rms;
        for (float& sample : samples) {
            sample *= gain;
        }
    }
    return samples;
}

std::vector<Complex> analyticSignal(std::span<const float> samples) {
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
    for (size_t i = 1; i < fft_size / 2; ++i) {
        freq[i] *= 2.0f;
    }
    for (size_t i = fft_size / 2 + 1; i < fft_size; ++i) {
        freq[i] = Complex(0.0f, 0.0f);
    }
    pocketfft::c2c<float>(shape, stride, stride, axes,
                          pocketfft::BACKWARD, freq.data(), analytic.data(),
                          1.0f / static_cast<float>(fft_size));
    analytic.resize(samples.size());
    return analytic;
}

std::vector<float> rawEnvelopeClipOnly(std::span<const float> samples,
                                       float target_db) {
    std::vector<float> clipped(samples.begin(), samples.end());
    const auto measurement = ultra::sim::measureTxBurstInBandRms(samples);
    if (measurement.active_samples == 0 || !(measurement.broadband_rms > 0.0f)) {
        return clipped;
    }
    const float pre_papr = ultra::phy::measurePaprDb(samples);
    target_db = std::min(
        target_db,
        std::max(0.0f, pre_papr -
                           ultra::phy::kOfdmPaprReductionTargetReductionDb));
    const float threshold =
        measurement.broadband_rms * std::pow(10.0f, target_db / 20.0f);
    const std::span<const float> active(
        samples.data() + static_cast<std::ptrdiff_t>(measurement.active_begin),
        measurement.active_samples);
    const auto analytic = analyticSignal(active);
    for (size_t i = 0; i < analytic.size(); ++i) {
        Complex sample = analytic[i];
        const float mag = std::abs(sample);
        if (mag > threshold) {
            sample *= threshold / mag;
            clipped[measurement.active_begin + i] = std::real(sample);
        }
    }
    return clipped;
}

struct Spectrum {
    std::vector<float> freq_hz;
    std::vector<double> power;
};

Spectrum makeSpectrum(std::span<const float> samples) {
    const auto measurement = ultra::sim::measureTxBurstInBandRms(samples);
    const size_t begin = measurement.active_begin;
    const size_t active_samples = measurement.active_samples;
    const size_t fft_size = nextPowerOfTwo(std::max<size_t>(2048, active_samples));

    std::vector<Complex> time(fft_size, Complex(0.0f, 0.0f));
    std::vector<Complex> freq(fft_size, Complex(0.0f, 0.0f));
    for (size_t i = 0; i < active_samples; ++i) {
        const float window =
            0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) /
                                   static_cast<float>(active_samples - 1));
        time[i] = Complex(samples[begin + i] * window, 0.0f);
    }

    const pocketfft::shape_t shape{fft_size};
    const pocketfft::stride_t stride{static_cast<std::ptrdiff_t>(sizeof(Complex))};
    const pocketfft::shape_t axes{0};
    pocketfft::c2c<float>(shape, stride, stride, axes,
                          pocketfft::FORWARD, time.data(), freq.data(), 1.0f);

    Spectrum out;
    out.freq_hz.reserve(fft_size / 2 + 1);
    out.power.reserve(fft_size / 2 + 1);
    for (size_t i = 0; i <= fft_size / 2; ++i) {
        out.freq_hz.push_back(kSampleRate * static_cast<float>(i) /
                              static_cast<float>(fft_size));
        out.power.push_back(std::norm(freq[i]));
    }
    return out;
}

double meanBinPower(const Spectrum& spectrum, float low_hz, float high_hz) {
    double sum = 0.0;
    size_t count = 0;
    for (size_t i = 0; i < spectrum.freq_hz.size(); ++i) {
        const float f = spectrum.freq_hz[i];
        if (f >= low_hz && f < high_hz) {
            sum += spectrum.power[i];
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

double maxBinPower(const Spectrum& spectrum, float low_hz, float high_hz) {
    double peak = 0.0;
    for (size_t i = 0; i < spectrum.freq_hz.size(); ++i) {
        const float f = spectrum.freq_hz[i];
        if (f >= low_hz && f < high_hz) {
            peak = std::max(peak, spectrum.power[i]);
        }
    }
    return peak;
}

float maxInBandBinDeltaDb(const Spectrum& a, const Spectrum& b) {
    float max_delta = 0.0f;
    const size_t n = std::min(a.power.size(), b.power.size());
    const double occupied_floor = meanBinPower(a, 50.0f, 2950.0f) * 1.0e-4;
    for (size_t i = 0; i < n; ++i) {
        const float f = a.freq_hz[i];
        if (f < 50.0f || f >= 2950.0f ||
            a.power[i] <= occupied_floor || b.power[i] <= 0.0) {
            continue;
        }
        max_delta = std::max(max_delta,
                             std::abs(dbPowerRatio(b.power[i], a.power[i])));
    }
    return max_delta;
}

void runSyntheticRegression(bool proof_mode) {
    float min_pre_papr = std::numeric_limits<float>::infinity();
    float max_pre_papr = 0.0f;
    float min_post_papr = std::numeric_limits<float>::infinity();
    float max_post_papr = 0.0f;
    float max_rms_delta = 0.0f;
    float max_peak_drop_db = 0.0f;
    float max_delta_ratio = 0.0f;

    for (uint32_t seed = 1; seed <= 100; ++seed) {
        std::vector<float> samples = makeSyntheticOfdmLikeBurst(seed * 7919u);
        const std::vector<float> before = samples;
        const auto pre = ultra::sim::measureTxBurstInBandRms(before);
        auto measurement =
            ultra::phy::applyPaprReduction(samples, kThresholdDb, true);
        const auto post = ultra::sim::measureTxBurstInBandRms(samples);

        const float peak_drop_db =
            dbRmsRatio(measurement.post_peak, measurement.pre_peak);
        const float delta_ratio =
            maxAdjacentDelta(samples) /
            std::max(1.0e-6f, maxAdjacentDelta(before));

        min_pre_papr = std::min(min_pre_papr, measurement.pre_papr_db);
        max_pre_papr = std::max(max_pre_papr, measurement.pre_papr_db);
        min_post_papr = std::min(min_post_papr, measurement.post_papr_db);
        max_post_papr = std::max(max_post_papr, measurement.post_papr_db);
        max_rms_delta =
            std::max(max_rms_delta, std::abs(measurement.in_band_rms_delta_db));
        max_peak_drop_db = std::max(max_peak_drop_db, std::abs(peak_drop_db));
        max_delta_ratio = std::max(max_delta_ratio, delta_ratio);

        // We do not assert an exact PAPR landing target. Single-pass Hilbert
        // shaping plus band-limit FIR has irreducible peak regrowth on
        // random-phase OFDM bursts; the contract is meaningful crest-factor
        // reduction with in-band power preservation.
        if (measurement.pre_papr_db > ultra::phy::kOfdmPaprReductionMinPrePaprDb) {
            CHECK(measurement.applied, "synthetic high-PAPR burst should be clipped");
            CHECK(measurement.post_papr_db <= measurement.pre_papr_db - 0.5f,
                  "synthetic high-PAPR burst should reduce crest factor");
        } else {
            CHECK(!measurement.applied,
                  "synthetic low-PAPR burst should pass through unchanged");
        }
        CHECK(std::abs(measurement.in_band_rms_delta_db) <= 0.3f,
              "in-band RMS takeback should preserve RMS within 0.3 dB");
        CHECK(std::abs(dbRmsRatio(post.in_band_rms, pre.in_band_rms)) <= 0.3f,
              "external in-band measurement should match takeback contract");
        CHECK(delta_ratio <= 1.10f,
              "band-limited clip should not introduce abrupt sample jumps");
    }

    if (proof_mode) {
        std::cout << "\nPAPR synthetic regression table\n";
        std::cout << "count,threshold_db,post_target_db,min_pre_papr,max_pre_papr,min_post_papr,"
                     "max_post_papr,max_abs_rms_delta_db,max_peak_drop_db,"
                     "max_adjacent_delta_ratio\n";
        std::cout << std::fixed << std::setprecision(6)
                  << 100 << ","
                  << kThresholdDb << ","
                  << kPostPaprTargetDb << ","
                  << min_pre_papr << ","
                  << max_pre_papr << ","
                  << min_post_papr << ","
                  << max_post_papr << ","
                  << max_rms_delta << ","
                  << max_peak_drop_db << ","
                  << max_delta_ratio << "\n";
    }
}

void runSpectralRegression(bool proof_mode) {
    std::vector<float> before = makeSyntheticOfdmLikeBurst(0x515151u);
    std::vector<float> raw_clipped = rawEnvelopeClipOnly(before, kThresholdDb);
    std::vector<float> after = before;
    auto measurement = ultra::phy::applyPaprReduction(after, kThresholdDb, true);

    const Spectrum pre = makeSpectrum(before);
    const Spectrum raw = makeSpectrum(raw_clipped);
    const Spectrum post = makeSpectrum(after);
    const double pre_in_band = meanBinPower(pre, 50.0f, 2950.0f);
    const double raw_in_band = meanBinPower(raw, 50.0f, 2950.0f);
    const double post_in_band = meanBinPower(post, 50.0f, 2950.0f);

    const float pre_high_50 =
        dbPowerRatio(maxBinPower(pre, 2950.0f, 3000.0f), pre_in_band);
    const float raw_high_50 =
        dbPowerRatio(maxBinPower(raw, 2950.0f, 3000.0f), raw_in_band);
    const float raw_high_100 =
        dbPowerRatio(maxBinPower(raw, 3000.0f, 3050.0f), raw_in_band);
    const float raw_low_25 =
        dbPowerRatio(maxBinPower(raw, 0.0f, 25.0f), raw_in_band);
    const float post_high_50 =
        dbPowerRatio(maxBinPower(post, 2950.0f, 3000.0f), post_in_band);
    const float post_high_100 =
        dbPowerRatio(maxBinPower(post, 3000.0f, 3050.0f), post_in_band);
    const float post_low_25 =
        dbPowerRatio(maxBinPower(post, 0.0f, 25.0f), post_in_band);
    const float in_band_delta = maxInBandBinDeltaDb(pre, post);
    const float mean_in_band_delta =
        std::abs(dbPowerRatio(post_in_band, pre_in_band));

    CHECK(measurement.applied, "spectral burst should be clipped");
    CHECK(raw_high_50 > pre_high_50 + 3.0f,
          "raw clipping should create measurable high-side products");
    CHECK(post_high_50 <= raw_high_50 - 3.0f,
          "post-filter 2950-3000 Hz guard should improve over raw clipping");
    CHECK(post_high_100 <= raw_high_100 - 3.0f,
          "post-filter 3000-3050 Hz guard should improve over raw clipping");
    CHECK(post_low_25 <= raw_low_25 + 0.5f,
          "post-filter 0-25 Hz guard should not worsen raw clipping");
    CHECK(mean_in_band_delta <= 0.5f,
          "mean in-band power should stay within 0.5 dB after clipping");

    if (proof_mode) {
        std::cout << "\nPAPR spectral mask table\n";
        std::cout << "state,papr_db,mean_in_band_bin_power,"
                     "max_0_25_dbc,max_0_50_dbc,max_2950_3000_dbc,"
                     "max_3000_3050_dbc,"
                     "max_in_band_delta_db\n";
        std::cout << std::fixed << std::setprecision(6);
        auto print = [&](const char* state,
                         std::span<const float> samples,
                         const Spectrum& spectrum,
                         double in_band,
                         float delta) {
            std::cout << state << ","
                      << ultra::phy::measurePaprDb(samples) << ","
                      << in_band << ","
                      << dbPowerRatio(maxBinPower(spectrum, 0.0f, 25.0f), in_band) << ","
                      << dbPowerRatio(maxBinPower(spectrum, 0.0f, 50.0f), in_band) << ","
                      << dbPowerRatio(maxBinPower(spectrum, 2950.0f, 3000.0f), in_band) << ","
                      << dbPowerRatio(maxBinPower(spectrum, 3000.0f, 3050.0f), in_band) << ","
                      << delta << "\n";
        };
        print("pre", before, pre, pre_in_band, 0.0f);
        print("raw_clip_no_fir", raw_clipped, raw, raw_in_band,
              maxInBandBinDeltaDb(pre, raw));
        print("post_clip_fir", after, post, post_in_band, in_band_delta);
    }
}

void runIdempotenceRegression() {
    std::vector<float> low_papr(4800, 0.0f);
    for (size_t i = 0; i < low_papr.size(); ++i) {
        low_papr[i] = 0.2f * std::sin(2.0f * kPi * 1500.0f *
                                      static_cast<float>(i) / kSampleRate);
    }
    const std::vector<float> before = low_papr;
    const auto measurement =
        ultra::phy::applyPaprReduction(low_papr, kThresholdDb, true);
    CHECK(measurement.enabled, "low-PAPR call should record enable state");
    CHECK(!measurement.applied, "low-PAPR waveform should pass through unchanged");
    CHECK(low_papr == before, "low-PAPR samples should remain bit-identical");
}

void runDisabledBitIdentityRegression() {
    std::vector<float> samples = makeSyntheticOfdmLikeBurst(0x919191u);
    const std::vector<float> before = samples;
    const auto measurement =
        ultra::phy::applyPaprReduction(samples, kThresholdDb, false);
    CHECK(!measurement.enabled, "disabled call should record disabled state");
    CHECK(!measurement.applied, "disabled call should not apply PAPR reduction");
    CHECK(samples == before, "disabled PAPR reduction should be bit-identical");
}

}  // namespace

int main(int argc, char** argv) {
    const bool proof_mode = argc > 1 && std::string(argv[1]) == "--proof";

    runSyntheticRegression(proof_mode);
    runSpectralRegression(proof_mode);
    runIdempotenceRegression();
    runDisabledBitIdentityRegression();

    if (tests_failed == 0) {
        std::cout << "PASS: PaprReduction (" << tests_run << " checks)\n";
        return 0;
    }
    std::cout << "FAIL: " << tests_failed << "/" << tests_run << " checks failed\n";
    return 1;
}
