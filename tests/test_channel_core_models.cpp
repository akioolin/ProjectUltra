#include "ota_channel_core/models.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <iostream>
#include <numeric>
#include <vector>

using ultra::ota_channel_core::AWGNChannelModel;
using ultra::ota_channel_core::ChannelConfig;
using ultra::ota_channel_core::ChannelType;
using ultra::ota_channel_core::PassthroughChannelModel;
using ultra::ota_channel_core::RngRoot;
using ultra::ota_channel_core::WattersonChannel;
using ultra::ota_channel_core::createChannelModel;

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr uint32_t kSampleRate = ultra::ota_channel_core::kDefaultSampleRate;

void assertNear(const std::vector<float>& a,
                const std::vector<float>& b,
                float eps = 1.0e-6f) {
    assert(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        assert(std::abs(a[i] - b[i]) <= eps);
    }
}

void assertNear(float actual, float expected, float eps) {
    const float tolerance = eps * std::max(1.0f, std::abs(expected));
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << "assertNear failed actual=" << actual
                  << " expected=" << expected
                  << " tolerance=" << tolerance << "\n";
    }
    assert(std::abs(actual - expected) <= tolerance);
}

double meanOf(const std::vector<float>& samples) {
    return std::accumulate(samples.begin(), samples.end(), 0.0) /
           static_cast<double>(samples.size());
}

double varianceOf(const std::vector<float>& samples, double mean) {
    double sum = 0.0;
    for (float sample : samples) {
        const double centered = static_cast<double>(sample) - mean;
        sum += centered * centered;
    }
    return sum / static_cast<double>(samples.size());
}

std::vector<float> modemBandpassFirCoefficients() {
    constexpr size_t taps = 101;
    constexpr float low_hz = 50.0f;
    constexpr float high_hz = 2950.0f;
    std::vector<float> coeffs(taps);
    const float fc_low = low_hz / static_cast<float>(kSampleRate);
    const float fc_high = high_hz / static_cast<float>(kSampleRate);
    const int midpoint = static_cast<int>((taps - 1) / 2);

    for (int n = 0; n < static_cast<int>(taps); ++n) {
        if (n == midpoint) {
            coeffs[static_cast<size_t>(n)] = 2.0f * (fc_high - fc_low);
        } else {
            const float x = kPi * static_cast<float>(n - midpoint);
            coeffs[static_cast<size_t>(n)] =
                (std::sin(2.0f * fc_high * x) -
                 std::sin(2.0f * fc_low * x)) / x;
        }
        const float w = 2.0f * kPi * static_cast<float>(n) /
                        static_cast<float>(taps - 1);
        coeffs[static_cast<size_t>(n)] *=
            0.42f - 0.5f * std::cos(w) + 0.08f * std::cos(2.0f * w);
    }
    return coeffs;
}

double inBandPowerOf(const std::vector<float>& samples) {
    const std::vector<float> coeffs = modemBandpassFirCoefficients();
    std::vector<float> delay_line(coeffs.size(), 0.0f);
    size_t delay_idx = 0;
    double sum_sq = 0.0;
    size_t measured = 0;
    for (size_t n = 0; n < samples.size(); ++n) {
        delay_line[delay_idx] = samples[n];
        float out = 0.0f;
        size_t j = delay_idx;
        for (float coeff : coeffs) {
            out += coeff * delay_line[j];
            if (j == 0) {
                j = coeffs.size();
            }
            --j;
        }
        delay_idx = (delay_idx + 1) % coeffs.size();
        if (n >= coeffs.size()) {
            sum_sq += static_cast<double>(out) * static_cast<double>(out);
            ++measured;
        }
    }
    return measured == 0 ? 0.0 : sum_sq / static_cast<double>(measured);
}

double averageDftBinPower(const std::vector<float>& samples,
                          const std::vector<int>& bins,
                          size_t block_size,
                          size_t blocks) {
    double sum_power = 0.0;
    size_t count = 0;
    for (size_t block = 0; block < blocks; ++block) {
        const size_t offset = block * block_size;
        for (int bin : bins) {
            std::complex<double> acc(0.0, 0.0);
            for (size_t n = 0; n < block_size; ++n) {
                const double phase = -2.0 * static_cast<double>(kPi) *
                                     static_cast<double>(bin) *
                                     static_cast<double>(n) /
                                     static_cast<double>(block_size);
                acc += static_cast<double>(samples[offset + n]) *
                       std::complex<double>(std::cos(phase), std::sin(phase));
            }
            sum_power += std::norm(acc) / static_cast<double>(block_size);
            ++count;
        }
    }
    return sum_power / static_cast<double>(count);
}

double rmsOf(const std::vector<float>& samples, size_t begin = 0, size_t end = 0) {
    if (end == 0 || end > samples.size()) {
        end = samples.size();
    }
    double sum_sq = 0.0;
    for (size_t i = begin; i < end; ++i) {
        sum_sq += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    }
    return std::sqrt(sum_sq / static_cast<double>(end - begin));
}

double estimateToneFrequencyHz(const std::vector<float>& samples,
                               size_t begin,
                               size_t end) {
    int crossings = 0;
    for (size_t i = begin + 1; i < end; ++i) {
        if (samples[i - 1] < 0.0f && samples[i] >= 0.0f) {
            ++crossings;
        }
    }
    const double seconds = static_cast<double>(end - begin) /
                           static_cast<double>(kSampleRate);
    return static_cast<double>(crossings) / seconds;
}

std::vector<float> runModel(ChannelType type, uint64_t seed) {
    const std::vector<float> input{0.0f, 0.1f, -0.2f, 0.3f, -0.4f, 0.0f, 0.25f, -0.15f};
    RngRoot root(seed);
    auto model = createChannelModel(
        ChannelConfig{.type = type, .snr_db = 18.0f, .seed = seed},
        root,
        "model:test");
    return model->process(input);
}

void checkRepeat(ChannelType type) {
    const auto first = runModel(type, 0xfeedbeefu);
    const auto second = runModel(type, 0xfeedbeefu);
    assertNear(first, second, 0.0f);
}

void checkDelaySpread(ChannelType type, size_t expected_delay) {
    auto cfg = ultra::ota_channel_core::configForWatterson(type, 80.0f);
    cfg.noise_enabled = false;
    cfg.fading_enabled = false;
    WattersonChannel channel(cfg, 123);

    std::vector<float> impulse(expected_delay + 8, 0.0f);
    impulse[0] = 1.0f;
    const auto out = channel.process(impulse);

    assert(std::abs(out[0] - cfg.path1_gain) <= 1.0e-6f);
    assert(std::abs(out[expected_delay] - cfg.path2_gain) <= 1.0e-6f);
}

void checkAwgnStatisticsAndSpectrum() {
    constexpr float snr_db = 18.0f;
    constexpr size_t sample_count = 65536;
    RngRoot root(0x123456u);
    AWGNChannelModel model(snr_db, root.stream("awgn:statistics"));
    const auto noise = model.process(std::vector<float>(sample_count, 0.0f));

    const float sigma = ultra::ota_channel_core::modemReferenceNoiseStddev(snr_db);
    const double mean = meanOf(noise);
    const double variance = varianceOf(noise, mean);
    assert(std::abs(mean) < static_cast<double>(sigma) * 0.02);
    assert(std::abs(variance - static_cast<double>(sigma) * sigma) /
           (static_cast<double>(sigma) * sigma) < 0.015);

    constexpr size_t block_size = 1024;
    constexpr size_t blocks = 64;
    const double low = averageDftBinPower(noise, {4, 8, 12, 16}, block_size, blocks);
    const double mid = averageDftBinPower(noise, {64, 96, 128, 160}, block_size, blocks);
    const double high = averageDftBinPower(noise, {256, 320, 384, 448}, block_size, blocks);
    const double max_band = std::max({low, mid, high});
    const double min_band = std::min({low, mid, high});
    const double flatness_db = 10.0 * std::log10(max_band / min_band);
    std::cout << "awgn_statistics snr_db=" << snr_db
              << " mean=" << mean
              << " variance=" << variance
              << " expected_variance=" << static_cast<double>(sigma) * sigma
              << " spectrum_flatness_db=" << flatness_db
              << "\n";
    assert(flatness_db < 1.0);
}

void checkWattersonNoiseAndFading() {
    constexpr float snr_db = 18.0f;
    constexpr size_t sample_count = 65536;
    auto noise_cfg = ultra::ota_channel_core::itu_r_f1487::awgn(snr_db);
    noise_cfg.cfo_enabled = false;
    WattersonChannel noise_channel(noise_cfg, 0xabcdefu);
    const auto noise = noise_channel.process(std::vector<float>(sample_count, 0.0f));
    const float sigma = ultra::ota_channel_core::modemReferenceNoiseStddev(snr_db);
    const double mean = meanOf(noise);
    const double variance = varianceOf(noise, mean);
    std::cout << "watterson_noise snr_db=" << snr_db
              << " mean=" << mean
              << " variance=" << variance
              << " expected_variance=" << static_cast<double>(sigma) * sigma
              << "\n";
    assert(std::abs(mean) < static_cast<double>(sigma) * 0.02);
    assert(std::abs(variance - static_cast<double>(sigma) * sigma) /
           (static_cast<double>(sigma) * sigma) < 0.015);

    auto flutter_cfg = ultra::ota_channel_core::itu_r_f1487::flutter(80.0f);
    flutter_cfg.noise_enabled = false;
    flutter_cfg.multipath_enabled = false;
    flutter_cfg.cfo_enabled = false;
    WattersonChannel flutter(flutter_cfg, 0x13579u);
    const auto envelope = flutter.process(std::vector<float>(sample_count, 1.0f));
    const double mean_square_gain =
        std::inner_product(envelope.begin() + 4096, envelope.end(),
                           envelope.begin() + 4096, 0.0) /
        static_cast<double>(envelope.size() - 4096);
    std::cout << "watterson_flutter mean_square_gain=" << mean_square_gain
              << " doppler_hz=" << flutter_cfg.doppler_spread_hz
              << "\n";
    assert(mean_square_gain > 0.80 && mean_square_gain < 1.20);
}

void checkRealHfLoopScaling() {
    constexpr float snr_db = 12.0f;
    constexpr size_t sample_count = 96000;
    std::vector<float> loop(sample_count);
    const double amplitude = std::sqrt(2.0);
    for (size_t i = 0; i < loop.size(); ++i) {
        loop[i] = static_cast<float>(
            amplitude * std::sin(2.0 * static_cast<double>(kPi) * 1000.0 *
                                 static_cast<double>(i) /
                                 static_cast<double>(kSampleRate)));
    }

    ultra::ota_channel_core::RealHfLoopChannelModel model(snr_db, loop, 0);
    const auto output = model.process(std::vector<float>(sample_count, 0.0f));
    const double measured_power = inBandPowerOf(output);
    const double expected_power =
        ultra::ota_channel_core::kModemReferencePower *
        std::pow(10.0, -static_cast<double>(snr_db) / 10.0);
    const double error_db = 10.0 * std::log10(measured_power / expected_power);
    std::cout << "real_hf_loop_scaling snr_db=" << snr_db
              << " measured_in_band_power=" << measured_power
              << " expected_in_band_power=" << expected_power
              << " error_db=" << error_db
              << "\n";
    assert(std::abs(error_db) < 0.10);
}

void checkCfoInjectionPreservesBandEdgeTone() {
    constexpr size_t sample_count = 65536;
    constexpr float cfo_hz = 37.0f;
    const double tone_hz = 3800.0 * static_cast<double>(kSampleRate) /
                           static_cast<double>(sample_count);
    std::vector<float> tone(sample_count);
    for (size_t i = 0; i < tone.size(); ++i) {
        tone[i] = static_cast<float>(
            std::sin(2.0 * static_cast<double>(kPi) * tone_hz *
                     static_cast<double>(i) /
                     static_cast<double>(kSampleRate)));
    }

    ultra::ota_channel_core::WattersonChannel::Config cfg;
    cfg.snr_db = 80.0f;
    cfg.delay_spread_ms = 0.0f;
    cfg.doppler_spread_hz = 0.0f;
    cfg.cfo_hz = cfo_hz;
    cfg.path1_gain = 1.0f;
    cfg.path2_gain = 0.0f;
    cfg.sample_rate = kSampleRate;
    cfg.fading_enabled = false;
    cfg.multipath_enabled = false;
    cfg.noise_enabled = false;
    cfg.cfo_enabled = true;

    WattersonChannel channel(cfg, 0x2468u);
    const auto shifted = channel.process(tone);

    constexpr size_t edge = 4096;
    const double rms_ratio =
        rmsOf(shifted, edge, shifted.size() - edge) /
        rmsOf(tone, edge, tone.size() - edge);
    const double estimated_hz =
        estimateToneFrequencyHz(shifted, edge, shifted.size() - edge);
    std::cout << "cfo_injection tone_hz=" << tone_hz
              << " cfo_hz=" << cfo_hz
              << " estimated_hz=" << estimated_hz
              << " rms_ratio=" << rms_ratio
              << "\n";
    assertNear(static_cast<float>(rms_ratio), 1.0f, 0.03f);
    assertNear(static_cast<float>(estimated_hz),
               static_cast<float>(tone_hz + cfo_hz),
               0.003f);
}

}  // namespace

int main() {
    {
        PassthroughChannelModel model;
        const std::vector<float> input{0.0f, 1.0f, -0.5f, 0.25f};
        assertNear(model.process(input), input, 0.0f);
    }

    {
        RngRoot root(0xabcdefu);
        AWGNChannelModel a(15.0f, root.stream("awgn"));
        AWGNChannelModel b(15.0f, root.stream("awgn"));
        const std::vector<float> zeros(16, 0.0f);
        assertNear(a.process(zeros), b.process(zeros), 0.0f);
    }

    checkRepeat(ChannelType::AWGN);
    checkRepeat(ChannelType::GOOD);
    checkRepeat(ChannelType::MODERATE);
    checkRepeat(ChannelType::POOR);

    checkDelaySpread(ChannelType::GOOD, 24);
    checkDelaySpread(ChannelType::MODERATE, 48);
    checkDelaySpread(ChannelType::POOR, 96);

    checkAwgnStatisticsAndSpectrum();
    checkWattersonNoiseAndFading();
    checkRealHfLoopScaling();
    checkCfoInjectionPreservesBandEdgeTone();

    const auto awgn = runModel(ChannelType::AWGN, 0x1234u);
    const std::vector<float> expected_awgn{
        -0.138502685f, 0.105445761f, -0.395952753f, 0.354813922f,
        -0.163206630f, -0.104314167f, 0.146983050f, -0.067175503f};
    assertNear(awgn, expected_awgn, 1.0e-6f);

    std::cout << "channel core models deterministic\n";
    return 0;
}
