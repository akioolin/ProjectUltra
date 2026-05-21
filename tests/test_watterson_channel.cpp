#include "ota_channel_core/models.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <numeric>
#include <span>
#include <vector>

using ultra::ota_channel_core::WattersonChannel;

namespace {

using Complex = std::complex<double>;

constexpr double kPi = 3.141592653589793238462643383279502884;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "require failed: " << message << "\n";
        std::abort();
    }
}

double rms(std::span<const float> samples, size_t begin = 0, size_t end = 0) {
    if (end == 0 || end > samples.size()) {
        end = samples.size();
    }
    double sum_sq = 0.0;
    for (size_t i = begin; i < end; ++i) {
        sum_sq += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    }
    return std::sqrt(sum_sq / static_cast<double>(end - begin));
}

std::vector<float> makeTone(double hz,
                            double rms_level,
                            uint32_t sample_rate,
                            size_t sample_count) {
    std::vector<float> tone(sample_count);
    const double amplitude = rms_level * std::sqrt(2.0);
    for (size_t i = 0; i < tone.size(); ++i) {
        tone[i] = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * hz *
                                 static_cast<double>(i) /
                                 static_cast<double>(sample_rate)));
    }
    return tone;
}

WattersonChannel::Config diagnosticConfig(uint32_t sample_rate,
                                          float doppler_hz) {
    WattersonChannel::Config cfg;
    cfg.snr_db = 100.0f;
    cfg.delay_spread_ms = 0.5f;
    cfg.doppler_spread_hz = doppler_hz;
    cfg.path1_gain = 0.707f;
    cfg.path2_gain = 0.707f;
    cfg.sample_rate = sample_rate;
    cfg.fading_enabled = true;
    cfg.multipath_enabled = true;
    cfg.noise_enabled = false;
    cfg.cfo_enabled = false;
    return cfg;
}

struct FadingSeries {
    std::vector<Complex> h1;
    std::vector<Complex> h2;
    double alpha = 0.0;
};

FadingSeries sampleFading(uint32_t sample_rate,
                          float doppler_hz,
                          size_t warmup,
                          size_t count) {
    auto cfg = diagnosticConfig(sample_rate, doppler_hz);
    cfg.multipath_enabled = false;
    WattersonChannel channel(cfg, 0x5eed1234u);

    FadingSeries series;
    series.h1.reserve(count);
    series.h2.reserve(count);
    series.alpha = channel.fadingAlphaForDiagnostics();

    for (size_t i = 0; i < warmup + count; ++i) {
        channel.stepFadingForDiagnostics();
        if (i >= warmup) {
            const auto h1 = channel.fadingTap1ForDiagnostics();
            const auto h2 = channel.fadingTap2ForDiagnostics();
            series.h1.emplace_back(static_cast<double>(h1.real()),
                                   static_cast<double>(h1.imag()));
            series.h2.emplace_back(static_cast<double>(h2.real()),
                                   static_cast<double>(h2.imag()));
        }
    }
    return series;
}

double meanReal(const std::vector<Complex>& values, bool imag) {
    double sum = 0.0;
    for (const Complex& value : values) {
        sum += imag ? value.imag() : value.real();
    }
    return sum / static_cast<double>(values.size());
}

double varianceReal(const std::vector<Complex>& values,
                    bool imag,
                    double mean) {
    double sum = 0.0;
    for (const Complex& value : values) {
        const double x = (imag ? value.imag() : value.real()) - mean;
        sum += x * x;
    }
    return sum / static_cast<double>(values.size());
}

void checkMoment(const std::vector<Complex>& values,
                 const char* label,
                 double expected_variance) {
    const double mean_i = meanReal(values, false);
    const double mean_q = meanReal(values, true);
    const double var_i = varianceReal(values, false, mean_i);
    const double var_q = varianceReal(values, true, mean_q);
    const double sigma = std::sqrt(expected_variance);

    std::cout << "watterson_rayleigh " << label
              << " mean_i=" << mean_i
              << " mean_q=" << mean_q
              << " var_i=" << var_i
              << " var_q=" << var_q
              << " expected_var=" << expected_variance
              << "\n";

    require(std::abs(mean_i) < 0.05 * sigma, "tap I mean near zero");
    require(std::abs(mean_q) < 0.05 * sigma, "tap Q mean near zero");
    require(std::abs(var_i - expected_variance) / expected_variance < 0.05,
            "tap I variance matches CN(0,1) AR(1) moment");
    require(std::abs(var_q - expected_variance) / expected_variance < 0.05,
            "tap Q variance matches CN(0,1) AR(1) moment");
}

void checkRayleighMomentsAndAutocorrelation() {
    constexpr uint32_t sample_rate = 1000;
    constexpr float doppler_hz = 5.0f;
    constexpr size_t warmup = 5000;
    constexpr size_t count = 262144;
    const FadingSeries series =
        sampleFading(sample_rate, doppler_hz, warmup, count);

    const double expected_variance = 1.0 / (2.0 - series.alpha);
    checkMoment(series.h1, "h1", expected_variance);
    checkMoment(series.h2, "h2", expected_variance);

    constexpr size_t lag = 25;
    Complex numerator(0.0, 0.0);
    double denominator = 0.0;
    for (size_t i = 0; i + lag < series.h1.size(); ++i) {
        numerator += series.h1[i] * std::conj(series.h1[i + lag]);
        denominator += std::norm(series.h1[i]);
    }
    const Complex normalized = numerator / denominator;
    const double expected = std::pow(1.0 - series.alpha, static_cast<double>(lag));

    std::cout << "watterson_autocorrelation lag=" << lag
              << " measured_real=" << normalized.real()
              << " measured_imag=" << normalized.imag()
              << " expected=" << expected
              << " alpha=" << series.alpha
              << "\n";

    require(std::abs(normalized.real() - expected) / expected < 0.05,
            "complex tap autocorrelation matches AR(1)");
    require(std::abs(normalized.imag()) < 0.03,
            "complex tap autocorrelation has negligible quadrature bias");
}

std::vector<double> welchPsd(const std::vector<Complex>& samples,
                             size_t segment_size,
                             size_t max_bin) {
    const size_t segments = samples.size() / segment_size;
    std::vector<double> psd(max_bin + 1, 0.0);
    Complex mean(0.0, 0.0);
    for (const Complex& sample : samples) {
        mean += sample;
    }
    mean /= static_cast<double>(samples.size());

    for (size_t segment = 0; segment < segments; ++segment) {
        const size_t offset = segment * segment_size;
        for (size_t bin = 0; bin <= max_bin; ++bin) {
            Complex acc(0.0, 0.0);
            for (size_t n = 0; n < segment_size; ++n) {
                const double window =
                    0.5 - 0.5 * std::cos(2.0 * kPi *
                                          static_cast<double>(n) /
                                          static_cast<double>(segment_size - 1));
                const double phase = -2.0 * kPi *
                                     static_cast<double>(bin) *
                                     static_cast<double>(n) /
                                     static_cast<double>(segment_size);
                acc += (samples[offset + n] - mean) * window *
                       Complex(std::cos(phase), std::sin(phase));
            }
            psd[bin] += std::norm(acc);
        }
    }

    for (double& value : psd) {
        value /= static_cast<double>(segments);
    }
    return psd;
}

std::vector<double> welchEnvelopePsd(const std::vector<Complex>& samples,
                                     size_t segment_size,
                                     size_t max_bin) {
    std::vector<Complex> envelope(samples.size());
    double mean = 0.0;
    for (const Complex& sample : samples) {
        mean += std::abs(sample);
    }
    mean /= static_cast<double>(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        envelope[i] = Complex(std::abs(samples[i]) - mean, 0.0);
    }
    return welchPsd(envelope, segment_size, max_bin);
}

double firstHalfPowerHz(const std::vector<double>& psd,
                        double bin_hz,
                        double reference_power) {
    const double threshold = 0.5 * reference_power;
    for (size_t bin = 1; bin < psd.size(); ++bin) {
        if (psd[bin] <= threshold) {
            return static_cast<double>(bin) * bin_hz;
        }
    }
    return static_cast<double>(psd.size() - 1) * bin_hz;
}

void checkDopplerPsd() {
    constexpr uint32_t sample_rate = 1000;
    constexpr float doppler_hz = 5.0f;
    constexpr size_t warmup = 5000;
    constexpr size_t count = 262144;
    constexpr size_t segment_size = 4096;
    constexpr size_t max_bin = 96;
    const FadingSeries series =
        sampleFading(sample_rate, doppler_hz, warmup, count);

    const std::vector<double> tap_psd =
        welchPsd(series.h1, segment_size, max_bin);
    const std::vector<double> envelope_psd =
        welchEnvelopePsd(series.h1, segment_size, max_bin);
    const double bin_hz =
        static_cast<double>(sample_rate) / static_cast<double>(segment_size);
    const size_t peak_bin = static_cast<size_t>(
        std::distance(tap_psd.begin(),
                      std::max_element(tap_psd.begin(), tap_psd.end())));
    const size_t envelope_peak_bin = static_cast<size_t>(
        std::distance(envelope_psd.begin(),
                      std::max_element(envelope_psd.begin(),
                                       envelope_psd.end())));
    const double half_power_hz =
        firstHalfPowerHz(tap_psd, bin_hz, tap_psd[0]);
    const double envelope_half_power_hz =
        firstHalfPowerHz(envelope_psd, bin_hz, envelope_psd[0]);

    std::cout << "watterson_doppler_psd doppler_hz=" << doppler_hz
              << " bin_hz=" << bin_hz
              << " peak_hz=" << static_cast<double>(peak_bin) * bin_hz
              << " half_power_hz=" << half_power_hz
              << " envelope_peak_hz="
              << static_cast<double>(envelope_peak_bin) * bin_hz
              << " envelope_half_power_hz=" << envelope_half_power_hz
              << "\n";

    require(static_cast<double>(peak_bin) * bin_hz <= 2.0 * bin_hz,
            "tap Doppler PSD peaks at DC");
    require(half_power_hz >= 0.5 * static_cast<double>(doppler_hz) &&
                half_power_hz <= 1.5 * static_cast<double>(doppler_hz),
            "tap Doppler PSD bandwidth matches configured Doppler");
    require(static_cast<double>(envelope_peak_bin) * bin_hz <=
                static_cast<double>(doppler_hz),
            "Rayleigh envelope PSD is centered near DC");
    require(envelope_half_power_hz > static_cast<double>(doppler_hz) &&
                envelope_half_power_hz < 3.0 * static_cast<double>(doppler_hz),
            "Rayleigh envelope PSD remains Doppler-limited");
}

void checkComplexMultipathImpulse() {
    constexpr size_t delay = 7;
    const std::complex<float> h1(0.25f, -0.75f);
    const std::complex<float> h2(-0.5f, 0.125f);
    std::vector<std::complex<float>> impulse(delay + 4,
                                             std::complex<float>(0.0f, 0.0f));
    impulse[0] = std::complex<float>(1.0f, 0.0f);

    const auto out = WattersonChannel::applyComplexMultipathForDiagnostics(
        impulse, delay, h1, h2, 1.0f, 1.0f);

    for (size_t i = 0; i < out.size(); ++i) {
        const std::complex<float> expected =
            i == 0 ? h1 :
            i == delay ? h2 :
            std::complex<float>(0.0f, 0.0f);
        require(std::abs(out[i] - expected) <= 1.0e-7f,
                "complex multipath impulse response");
    }
    std::cout << "watterson_complex_impulse delay_samples=" << delay
              << " h1=" << h1
              << " h2=" << h2
              << "\n";
}

void checkComplexFadingProcessEscapesAmplitudeOnlyNull() {
    constexpr uint32_t sample_rate = 48000;
    constexpr double tone_hz = 1000.0;
    constexpr size_t sample_count = sample_rate * 2;
    auto cfg = diagnosticConfig(sample_rate, 1.0e-3f);
    cfg.path1_gain = 1.0f;
    cfg.path2_gain = 1.0f;
    WattersonChannel channel(cfg, 0xabcdu);
    channel.setFadingTapsForDiagnostics(std::complex<float>(1.0f, 0.0f),
                                        std::complex<float>(0.0f, 1.0f));

    const auto tone = makeTone(tone_hz, 1.0, sample_rate, sample_count);
    const auto out = channel.process(tone);
    constexpr size_t edge = 8192;
    const double ratio = rms(out, edge, out.size() - edge) /
                         rms(tone, edge, tone.size() - edge);
    const double expected = std::sqrt(2.0);

    std::cout << "watterson_complex_process_notch_escape tone_hz=" << tone_hz
              << " rms_ratio=" << ratio
              << " expected_ratio=" << expected
              << "\n";
    require(std::abs(ratio - expected) < 0.08,
            "complex fading keeps phase and escapes amplitude-only null");
}

double notchHzForTaps(std::complex<float> tap1,
                      std::complex<float> tap2,
                      double delay_seconds) {
    double best_hz = 0.0;
    double best_mag = std::numeric_limits<double>::infinity();
    for (double hz = 500.0; hz <= 2500.0; hz += 5.0) {
        const Complex phase = std::polar(
            1.0, -2.0 * kPi * hz * delay_seconds);
        const Complex h =
            0.707 * Complex(tap1.real(), tap1.imag()) +
            0.707 * Complex(tap2.real(), tap2.imag()) * phase;
        const double mag = std::abs(h);
        if (mag < best_mag) {
            best_mag = mag;
            best_hz = hz;
        }
    }
    return best_hz;
}

void checkGoodPresetNotchSweeps() {
    auto cfg = ultra::ota_channel_core::itu_r_f1487::good(100.0f);
    cfg.noise_enabled = false;
    cfg.cfo_enabled = false;
    WattersonChannel channel(cfg, 42u);

    constexpr double duration_seconds = 120.0;
    constexpr double sample_period_seconds = 10.0;
    const uint64_t total_samples =
        static_cast<uint64_t>(duration_seconds *
                              static_cast<double>(cfg.sample_rate));
    const uint64_t stride_samples =
        static_cast<uint64_t>(sample_period_seconds *
                              static_cast<double>(cfg.sample_rate));

    std::vector<double> notch_hz;
    for (uint64_t sample = 0; sample <= total_samples; ++sample) {
        channel.stepFadingForDiagnostics();
        if ((sample % stride_samples) == 0) {
            notch_hz.push_back(notchHzForTaps(
                channel.fadingTap1ForDiagnostics(),
                channel.fadingTap2ForDiagnostics(),
                static_cast<double>(cfg.delay_spread_ms) / 1000.0));
        }
    }

    const double mean = std::accumulate(notch_hz.begin(), notch_hz.end(), 0.0) /
                        static_cast<double>(notch_hz.size());
    double sum_sq = 0.0;
    for (double hz : notch_hz) {
        const double centered = hz - mean;
        sum_sq += centered * centered;
    }
    const double stddev = std::sqrt(sum_sq / static_cast<double>(notch_hz.size()));

    std::cout << "watterson_good_notch_std_hz=" << stddev
              << " samples_hz=";
    for (double hz : notch_hz) {
        std::cout << hz << ",";
    }
    std::cout << "\n";

    require(stddev > 200.0, "Good preset notch sweeps across the band");
    require(*std::max_element(notch_hz.begin(), notch_hz.end()) -
                *std::min_element(notch_hz.begin(), notch_hz.end()) >
            500.0,
            "Good preset notch has large frequency excursion");
}

void checkInBandRmsAndNoFadingByteExact() {
    constexpr uint32_t sample_rate = 48000;
    constexpr size_t sample_count = sample_rate;
    WattersonChannel::Config cfg;
    cfg.snr_db = 100.0f;
    cfg.sample_rate = sample_rate;
    cfg.fading_enabled = false;
    cfg.multipath_enabled = false;
    cfg.noise_enabled = false;
    cfg.cfo_enabled = false;

    const auto input = makeTone(
        1000.0,
        ultra::ota_channel_core::kModemReferenceInBandRms,
        sample_rate,
        sample_count);
    WattersonChannel channel(cfg, 1234u);
    const auto output = channel.process(input);
    require(output.size() == input.size(), "output size preserved");
    for (size_t i = 0; i < input.size(); ++i) {
        require(output[i] == input[i], "fading-disabled path is byte-exact");
    }

    const double input_rms = rms(input);
    const double output_rms = rms(output);
    const double error_db = 20.0 * std::log10(output_rms / input_rms);
    std::cout << "watterson_in_band_rms input=" << input_rms
              << " output=" << output_rms
              << " error_db=" << error_db
              << "\n";
    require(std::abs(error_db) <= 0.5, "in-band RMS preserved");
}

uint64_t fnv1aSamples(const std::vector<float>& samples) {
    uint64_t hash = 1469598103934665603ull;
    for (float sample : samples) {
        uint32_t bits = 0;
        std::memcpy(&bits, &sample, sizeof(bits));
        for (int byte = 0; byte < 4; ++byte) {
            hash ^= static_cast<uint8_t>((bits >> (8 * byte)) & 0xffu);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

void checkDeterminism() {
    auto cfg = ultra::ota_channel_core::itu_r_f1487::good(35.0f);
    cfg.cfo_enabled = false;
    const auto input = makeTone(1000.0, 0.2, cfg.sample_rate, 65536);

    WattersonChannel first(cfg, 0x4444u);
    WattersonChannel second(cfg, 0x4444u);
    const auto out_a = first.process(input);
    const auto out_b = second.process(input);
    require(out_a.size() == out_b.size(), "determinism output size");
    for (size_t i = 0; i < out_a.size(); ++i) {
        require(out_a[i] == out_b[i], "same seed gives bit-identical samples");
    }
    const uint64_t hash = fnv1aSamples(out_a);
    std::cout << "watterson_determinism_hash=0x" << std::hex << hash
              << std::dec << "\n";
}

}  // namespace

int main() {
    checkRayleighMomentsAndAutocorrelation();
    checkDopplerPsd();
    checkComplexMultipathImpulse();
    checkComplexFadingProcessEscapesAmplitudeOnlyNull();
    checkGoodPresetNotchSweeps();
    checkInBandRmsAndNoFadingByteExact();
    checkDeterminism();
    std::cout << "watterson channel complex fading tests passed\n";
    return 0;
}
