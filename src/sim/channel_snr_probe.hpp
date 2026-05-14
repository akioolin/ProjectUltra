#pragma once

#include "sim/channel_calibration.hpp"
#include "sim/simulated_station.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace ultra::sim {

struct ChannelSNRProbeConfig {
    float snr_db = 15.0f;
    ::ChannelType channel_type = ::ChannelType::AWGN;
    uint32_t seed = 42;
    float tx_cfo_hz = 0.0f;
    size_t prefix_samples = 48000;
    size_t trailing_samples = 48000;
};

struct ChannelSNRProbeResult {
    float configured_snr_db = 0.0f;
    float measured_snr_db = 0.0f;
    float delta_db = 0.0f;
    float measured_signal_rms = kModemReferenceRms;
    float measured_noise_rms = 0.0f;
    float signal_window_rms = 0.0f;
    float signal_component_rms = 0.0f;
    size_t signal_samples = 0;
    size_t noise_samples = 0;
    ::ChannelType channel_type = ::ChannelType::AWGN;
};

inline const char* channelSNRProbeName(::ChannelType type) {
    switch (type) {
        case ::ChannelType::AWGN:     return "AWGN";
        case ::ChannelType::GOOD:     return "GOOD";
        case ::ChannelType::MODERATE: return "MODERATE";
        case ::ChannelType::POOR:     return "POOR";
        case ::ChannelType::FLUTTER:  return "FLUTTER";
        default:                      return "UNKNOWN";
    }
}

namespace detail {

struct PowerAccumulator {
    double sum_sq = 0.0;
    size_t count = 0;

    void add(float sample) {
        sum_sq += static_cast<double>(sample) * static_cast<double>(sample);
        ++count;
    }

    void addRange(const std::vector<float>& samples, size_t begin, size_t end) {
        begin = std::min(begin, samples.size());
        end = std::min(end, samples.size());
        if (begin >= end) return;
        for (size_t i = begin; i < end; ++i) {
            add(samples[i]);
        }
    }

    double power() const {
        return count > 0 ? sum_sq / static_cast<double>(count) : 0.0;
    }

    float rms() const {
        return static_cast<float>(std::sqrt(power()));
    }
};

inline std::pair<size_t, size_t> activeBounds(const std::vector<float>& samples) {
    constexpr float kActiveEpsilon = 1.0e-6f;
    size_t begin = 0;
    while (begin < samples.size() && std::abs(samples[begin]) <= kActiveEpsilon) {
        ++begin;
    }
    size_t end = samples.size();
    while (end > begin && std::abs(samples[end - 1]) <= kActiveEpsilon) {
        --end;
    }
    return {begin, end};
}

inline float dbRatio(float numerator, float denominator) {
    if (denominator <= 0.0f) {
        return numerator > 0.0f ? std::numeric_limits<float>::infinity()
                                : -std::numeric_limits<float>::infinity();
    }
    return 20.0f * std::log10(numerator / denominator);
}

}  // namespace detail

inline ChannelSNRProbeResult runChannelSNRProbe(const ChannelSNRProbeConfig& cfg) {
    ultra::gui::StreamingEncoder encoder;
    std::vector<float> burst = encoder.encodePing();

    std::vector<float> tx(cfg.prefix_samples + burst.size() + cfg.trailing_samples, 0.0f);
    std::copy(burst.begin(), burst.end(),
              tx.begin() + static_cast<std::ptrdiff_t>(cfg.prefix_samples));

    ::SimulatedChannel channel;
    channel.setSeed(cfg.seed);
    channel.setTxCFO(cfg.tx_cfo_hz);
    channel.configure(cfg.snr_db, cfg.channel_type);
    channel.transmitFromA(tx);
    std::vector<float> rx = channel.receiveForB(tx.size());

    auto [signal_begin, signal_end] = detail::activeBounds(tx);
    if (signal_begin >= signal_end) {
        signal_begin = cfg.prefix_samples;
        signal_end = cfg.prefix_samples + burst.size();
    }

    constexpr size_t kTailGuardSamples = 4800;  // 100 ms, beyond all F.1487 delay spreads here.

    detail::PowerAccumulator noise;
    noise.addRange(rx, 0, signal_begin);
    noise.addRange(rx, signal_end + kTailGuardSamples, rx.size());

    detail::PowerAccumulator signal_window;
    signal_window.addRange(rx, signal_begin, signal_end);

    const double noise_power = noise.power();
    const double signal_plus_noise_power = signal_window.power();
    const double signal_component_power =
        std::max(0.0, signal_plus_noise_power - noise_power);

    ChannelSNRProbeResult result;
    result.configured_snr_db = cfg.snr_db;
    result.channel_type = cfg.channel_type;
    result.measured_signal_rms = kModemReferenceRms;
    result.measured_noise_rms = noise.rms();
    result.signal_window_rms = signal_window.rms();
    result.signal_component_rms =
        static_cast<float>(std::sqrt(signal_component_power));
    result.signal_samples = signal_window.count;
    result.noise_samples = noise.count;
    result.measured_snr_db =
        detail::dbRatio(result.measured_signal_rms, result.measured_noise_rms);
    result.delta_db = result.measured_snr_db - result.configured_snr_db;
    return result;
}

}  // namespace ultra::sim
