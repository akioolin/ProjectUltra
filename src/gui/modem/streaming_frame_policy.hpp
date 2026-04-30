#pragma once

#include "protocol/frame_v2.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ultra {
namespace gui {
namespace streaming_frame_policy {

inline constexpr size_t kPingTrainingSkipSamples = 4608;
inline constexpr size_t kPingRMSCheckSamples = 5000;
inline constexpr float kMinTrainingRMSForPingRatio = 0.001f;
inline constexpr float kPingMaxDataToTrainingRMSRatio = 0.5f;
inline constexpr size_t kDefaultFalseLockAdvanceSamples = 1024;
inline constexpr size_t kMinFalseLockAdvanceSamples = 512;
inline constexpr float kMinSyncRecoveryCorrelation = 0.80f;

inline float rms(const float* samples, size_t count) {
    if (!samples || count == 0) {
        return 0.0f;
    }

    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        sum += samples[i] * samples[i];
    }
    return std::sqrt(sum / static_cast<float>(count));
}

struct PingRMSDecision {
    float training_rms = 0.0f;
    float data_rms = 0.0f;
    float ratio = 0.0f;
    bool is_ping = true;
};

inline PingRMSDecision evaluatePingRMS(const float* samples, size_t count) {
    PingRMSDecision decision;

    const size_t train_len = std::min(kPingTrainingSkipSamples, count);
    decision.training_rms = rms(samples, train_len);

    const size_t check_start = std::min(kPingTrainingSkipSamples, count);
    const size_t check_len = std::min(count - check_start, kPingRMSCheckSamples);
    decision.data_rms = rms(samples ? samples + check_start : nullptr, check_len);

    decision.ratio = (decision.training_rms > kMinTrainingRMSForPingRatio)
        ? decision.data_rms / decision.training_rms
        : 0.0f;
    decision.is_ping = decision.ratio < kPingMaxDataToTrainingRMSRatio;
    return decision;
}

inline size_t falseOFDMLockAdvanceSamples(size_t frame_len, int data_preamble_samples) {
    size_t advance = kDefaultFalseLockAdvanceSamples;
    if (data_preamble_samples > 0) {
        advance = std::max(kMinFalseLockAdvanceSamples,
                           static_cast<size_t>(data_preamble_samples) / 2);
    }
    return std::min(advance, frame_len);
}

inline bool shouldRunControlFirstOFDMPeek(int pending_total_cw,
                                          bool is_ofdm,
                                          bool connected,
                                          size_t frame_len,
                                          size_t control_frame_samples) {
    return pending_total_cw == 0 &&
           is_ofdm &&
           connected &&
           frame_len <= control_frame_samples;
}

inline bool allowSyncRecovery(float sync_correlation) {
    return sync_correlation >= kMinSyncRecoveryCorrelation;
}

inline bool isNonDataFrame(bool success, const uint8_t* frame_data, size_t frame_size) {
    if (!success || !frame_data || frame_size < 3) {
        return false;
    }

    const auto type = static_cast<protocol::v2::FrameType>(frame_data[2]);
    return protocol::v2::isControlFrame(type) || protocol::v2::isConnectFrame(type);
}

inline size_t consumedSamplesForDecodedFrame(bool success,
                                             bool is_ofdm,
                                             bool is_non_data_frame,
                                             int actual_codewords,
                                             size_t frame_len,
                                             size_t exact_codeword_samples) {
    if (success &&
        is_ofdm &&
        is_non_data_frame &&
        actual_codewords > 0 &&
        exact_codeword_samples < frame_len) {
        return exact_codeword_samples;
    }
    return frame_len;
}

}  // namespace streaming_frame_policy
}  // namespace gui
}  // namespace ultra
