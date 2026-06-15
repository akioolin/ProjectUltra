#pragma once

#include "protocol/frame_v2.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ultra {
namespace gui {
namespace streaming_frame_policy {

inline constexpr size_t kPingTrainingSkipSamples = 4608;
inline constexpr size_t kPingRMSCheckSamples = 5000;
inline constexpr float kMinTrainingRMSForPingRatio = 0.001f;
inline constexpr float kPingMaxDataToTrainingRMSRatio = 0.5f;
inline constexpr float kPingChirpLockMaxDataRMS = 0.16f;
inline constexpr float kPingCorrFloor = 0.30f;
inline constexpr float kPingMaxGapError = 1000.0f;
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

struct PingFrameDecision {
    float training_rms = 0.0f;
    float data_rms = 0.0f;
    float ratio = 0.0f;
    float chirp_corr = 0.0f;
    float gap_error_samples = std::numeric_limits<float>::infinity();
    bool ldpc_decode_attempted = false;
    bool ldpc_decode_succeeded = false;
    bool ldpc_magic_valid = false;
    bool ping_by_silence = false;
    bool ping_by_chirp_lock = false;
    bool is_ping = true;
};

using PingRMSDecision = PingFrameDecision;

inline PingFrameDecision evaluatePingFrame(
    const float* samples, size_t count,
    size_t training_skip_samples = kPingTrainingSkipSamples,
    size_t rms_check_samples = kPingRMSCheckSamples,
    float chirp_corr = 0.0f,
    float gap_error_samples = std::numeric_limits<float>::infinity(),
    bool ldpc_decode_succeeded = false,
    bool ldpc_magic_valid = false,
    bool ldpc_decode_attempted = false) {
    PingFrameDecision decision;

    const size_t train_len = std::min(training_skip_samples, count);
    decision.training_rms = rms(samples, train_len);

    const size_t check_start = std::min(training_skip_samples, count);
    const size_t check_len = std::min(count - check_start, rms_check_samples);
    decision.data_rms = rms(samples ? samples + check_start : nullptr, check_len);

    decision.ratio = (decision.training_rms > kMinTrainingRMSForPingRatio)
        ? decision.data_rms / decision.training_rms
        : 0.0f;
    decision.chirp_corr = chirp_corr;
    decision.gap_error_samples = gap_error_samples;
    decision.ldpc_decode_attempted = ldpc_decode_attempted;
    decision.ldpc_decode_succeeded = ldpc_decode_succeeded;
    decision.ldpc_magic_valid = ldpc_magic_valid;

    decision.ping_by_silence = decision.ratio < kPingMaxDataToTrainingRMSRatio;
    const bool chirp_signature_real =
        chirp_corr >= kPingCorrFloor &&
        std::abs(gap_error_samples) <= kPingMaxGapError;
    const bool no_valid_frame = !ldpc_decode_succeeded || !ldpc_magic_valid;
    // PATH2 is for chirp-only probes whose payload area was too weak/noisy to
    // justify an LDPC decode. Once a full LDPC attempt has been made on a
    // data-bearing MC-DPSK frame, a failure is a frame failure, not evidence
    // that the peer sent a PING. This keeps faded CONNECT frames from eliciting
    // false PONGs while preserving low-SNR PING acquisition.
    //
    // NOTE: this is the POST-4-CW-decode ping classifier (streaming_ofdm_decode
    // PATH2, after the full fixed CONNECT decode has already FAILED). At that
    // point an absolute data_rms floor is acceptable as a last-resort tie-break
    // for a low-level noisy ping whose 4-CW decode produced nothing. The
    // *pre*-decode WAIT decision must NOT use this absolute floor (it would skip
    // the 4-CW CONNECT decode for a low-level-but-real CONNECT) — it gates on
    // ping_by_silence (ratiometric) only. See streaming_ofdm_decode.cpp ~1290.
    const bool payload_energy_absent =
        decision.ping_by_silence || decision.data_rms <= kPingChirpLockMaxDataRMS;
    decision.ping_by_chirp_lock =
        chirp_signature_real && no_valid_frame &&
        (!ldpc_decode_attempted || payload_energy_absent);
    decision.is_ping = decision.ping_by_silence || decision.ping_by_chirp_lock;
    return decision;
}

inline PingRMSDecision evaluatePingRMS(
    const float* samples, size_t count,
    size_t training_skip_samples = kPingTrainingSkipSamples,
    size_t rms_check_samples = kPingRMSCheckSamples) {
    return evaluatePingFrame(samples, count, training_skip_samples, rms_check_samples,
                             0.0f, std::numeric_limits<float>::infinity(),
                             true, true);
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
