#pragma once

#include "ota_channel_core/rng.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <vector>

namespace ultra::ota_channel_core {

inline constexpr uint32_t kDefaultSampleRate = 48000;
inline constexpr float kModemReferenceBroadbandRms = 0.3180724f;
inline constexpr float kModemReferenceInBandRms = 0.30482664f;
inline constexpr float kModemReferenceRms = kModemReferenceInBandRms;
inline constexpr double kModemReferencePower =
    static_cast<double>(kModemReferenceRms) *
    static_cast<double>(kModemReferenceRms);
inline constexpr double kModemInBandNoisePowerFraction = 0.10858718;
inline constexpr double kModemBroadbandToInBandSnrOffsetDb = 9.64221445;

enum class ChannelType {
    PASSTHROUGH,
    AWGN,
    GOOD,
    MODERATE,
    POOR,
    FLUTTER,
    REAL_HF_LOOP
};

struct ChannelConfig {
    ChannelType type = ChannelType::PASSTHROUGH;
    float snr_db = 20.0f;
    uint64_t seed = 42;
    uint32_t sample_rate = kDefaultSampleRate;
    std::shared_ptr<const std::vector<float>> real_hf_loop_noise;
};

float modemReferenceNoiseStddev(float snr_db);
const char* channelTypeName(ChannelType type);
std::optional<ChannelType> parseChannelType(std::string_view value);
std::vector<float> loadRealHfLoopNoiseBedWav(std::string_view path);

class IChannelModel {
public:
    virtual ~IChannelModel() = default;
    virtual void reset() = 0;
    virtual void process(std::span<const float> input,
                         uint64_t start_sample,
                         std::vector<float>& output) = 0;

    void process(std::span<const float> input, std::vector<float>& output) {
        process(input, 0, output);
    }

    std::vector<float> process(std::span<const float> input) {
        std::vector<float> output;
        process(input, output);
        return output;
    }

    std::vector<float> process(std::span<const float> input, uint64_t start_sample) {
        std::vector<float> output;
        process(input, start_sample, output);
        return output;
    }
};

class PassthroughChannelModel final : public IChannelModel {
public:
    using IChannelModel::process;

    void reset() override {}
    void process(std::span<const float> input,
                 uint64_t start_sample,
                 std::vector<float>& output) override;
};

class AWGNChannelModel final : public IChannelModel {
public:
    using IChannelModel::process;

    AWGNChannelModel(float snr_db, uint32_t seed);

    void reset() override {}
    void setSNR(float snr_db);
    void process(std::span<const float> input,
                 uint64_t start_sample,
                 std::vector<float>& output) override;

private:
    float noise_stddev_ = 0.0f;
    uint32_t seed_ = 0;
};

class RealHfLoopChannelModel final : public IChannelModel {
public:
    using IChannelModel::process;

    RealHfLoopChannelModel(float snr_db,
                           std::vector<float> normalized_loop,
                           uint64_t seed_for_phase = 0);
    RealHfLoopChannelModel(float snr_db,
                           std::shared_ptr<const std::vector<float>> normalized_loop,
                           uint64_t seed_for_phase = 0);

    void reset() override;
    void setSNR(float snr_db);
    void process(std::span<const float> input,
                 uint64_t start_sample,
                 std::vector<float>& output) override;

private:
    float scale_ = 0.0f;
    double loop_in_band_power_ = 0.0;
    std::shared_ptr<const std::vector<float>> loop_;
    size_t start_position_ = 0;
    size_t position_ = 0;
};

class WattersonChannel {
public:
    struct Config {
        float snr_db = 15.0f;
        float delay_spread_ms = 2.0f;
        float doppler_spread_hz = 1.0f;
        float cfo_hz = 0.0f;
        float random_cfo_max_hz = 0.0f;
        float path1_gain = 0.707f;
        float path2_gain = 0.707f;
        uint32_t sample_rate = kDefaultSampleRate;
        bool fading_enabled = true;
        bool multipath_enabled = true;
        bool noise_enabled = true;
        bool cfo_enabled = true;
    };

    explicit WattersonChannel(const Config& config, uint64_t seed = 42);

    void reset();
    void setSNR(float snr_db);
    void process(std::span<const float> input, std::vector<float>& output);
    std::vector<float> process(std::span<const float> input);

    float actualCFO() const { return actual_cfo_hz_; }
    float fadingMagnitude() const;
    std::complex<float> fadingTap1ForDiagnostics() const { return fading1_; }
    std::complex<float> fadingTap2ForDiagnostics() const { return fading2_; }
    float fadingAlphaForDiagnostics() const { return fading_alpha_; }
    void stepFadingForDiagnostics();
    void setFadingTapsForDiagnostics(std::complex<float> tap1,
                                      std::complex<float> tap2);
    static std::vector<std::complex<float>> applyComplexMultipathForDiagnostics(
        std::span<const std::complex<float>> input,
        size_t delay_samples,
        std::complex<float> tap1,
        std::complex<float> tap2,
        float path1_gain,
        float path2_gain);
    const Config& config() const { return config_; }

private:
    void initializeHilbert();
    std::complex<float> analyticSample(float sample);
    void processWithoutFading(std::span<const float> input,
                              std::vector<float>& output);
    void processWithComplexFading(std::span<const float> input,
                                  std::vector<float>& output);
    void updateFading();
    void applyCFO(std::vector<float>& samples);

    Config config_;
    std::mt19937 rng_;
    std::normal_distribution<float> gaussian_{0.0f, 1.0f};
    std::deque<float> delay_line_;
    std::deque<std::complex<float>> analytic_delay_line_;
    std::vector<float> hilbert_coeffs_;
    std::vector<float> hilbert_delay_line_;
    size_t delay_samples_ = 0;
    size_t hilbert_delay_idx_ = 0;
    size_t hilbert_delay_samples_ = 0;
    float noise_stddev_ = 0.0f;
    float fading_alpha_ = 0.0f;
    std::complex<float> fading1_{1.0f, 0.0f};
    std::complex<float> fading2_{1.0f, 0.0f};
    float cfo_phase_ = 0.0f;
    float cfo_phase_inc_ = 0.0f;
    float actual_cfo_hz_ = 0.0f;
};

class WattersonChannelModel final : public IChannelModel {
public:
    using IChannelModel::process;

    WattersonChannelModel(const WattersonChannel::Config& config, uint64_t seed);

    void reset() override;
    void setSNR(float snr_db);
    void process(std::span<const float> input,
                 uint64_t start_sample,
                 std::vector<float>& output) override;

    const WattersonChannel::Config& config() const;

private:
    WattersonChannel channel_;
};

namespace itu_r_f1487 {
WattersonChannel::Config good(float snr_db = 20.0f);
WattersonChannel::Config moderate(float snr_db = 20.0f);
WattersonChannel::Config poor(float snr_db = 20.0f);
WattersonChannel::Config flutter(float snr_db = 20.0f);
WattersonChannel::Config awgn(float snr_db = 20.0f);
}  // namespace itu_r_f1487

WattersonChannel::Config configForWatterson(ChannelType type,
                                            float snr_db,
                                            uint32_t sample_rate = kDefaultSampleRate);
std::unique_ptr<IChannelModel> createChannelModel(const ChannelConfig& config,
                                                  const RngRoot& rng_root,
                                                  std::string_view stream_name);

}  // namespace ultra::ota_channel_core
