#pragma once

#include "frame_v2.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ultra {
namespace protocol {
namespace selective_repeat_arq_policy {

inline constexpr size_t kMaxWindow = 16;
inline constexpr int kMaxFastRetransmitsPerHole = 1;
inline constexpr int kMinHoleConfirmations = 2;
inline constexpr int kMaxHoleProbeRetransmits = 1;

inline size_t clampWindowSize(size_t size, size_t max_window = kMaxWindow) {
    return std::clamp<size_t>(size, 1, max_window);
}

inline uint32_t clampAckBatchSize(uint32_t ack_batch_size,
                                  size_t window_size,
                                  size_t max_window = kMaxWindow) {
    ack_batch_size = std::min<uint32_t>(ack_batch_size, static_cast<uint32_t>(max_window));
    if (ack_batch_size > 0 && ack_batch_size > static_cast<uint32_t>(window_size)) {
        return static_cast<uint32_t>(window_size);
    }
    return ack_batch_size;
}

inline uint32_t effectiveAckBatchThreshold(uint32_t ack_batch_size, size_t window_size) {
    return ack_batch_size > 0 ? ack_batch_size : static_cast<uint32_t>(window_size);
}

inline uint32_t decodeSackBitmap(const uint8_t* payload) {
    if (!payload) {
        return 0;
    }

    const uint32_t bitmap = v2::NackPayload::decode(payload).cw_bitmap;

    // Compatibility with the legacy 8-bit SACK encoding used before the
    // wider-window path: makeNack() placed the bitmap in payload[5], then the
    // ARQ code also copied it into payload[2] and read only payload[2].
    const uint8_t legacy = payload[2];
    const uint32_t legacy_shape = (static_cast<uint32_t>(legacy) << 24) | legacy;
    if (legacy != 0 && bitmap == legacy_shape) {
        return legacy;
    }

    return bitmap;
}

inline bool seqInWindow(uint16_t seq, uint16_t base_seq, size_t window_size) {
    const uint16_t diff = (seq - base_seq) & 0xFFFF;
    return diff < window_size;
}

enum class AckFreshness {
    Accept,
    Stale,
    Future,
};

inline AckFreshness classifyAckFreshness(uint16_t ack_seq,
                                         uint16_t tx_base_seq,
                                         size_t window_size) {
    const uint16_t ack_base = (tx_base_seq - 1) & 0xFFFF;
    const uint16_t back = (ack_base - ack_seq) & 0xFFFF;
    if (back > 0 && back < 0x8000) {
        return AckFreshness::Stale;
    }

    const uint16_t forward = (ack_seq - ack_base) & 0xFFFF;
    if (forward > window_size + 1 && forward < 0x8000) {
        return AckFreshness::Future;
    }

    return AckFreshness::Accept;
}

inline bool shouldSuppressDuplicateAck(bool last_signature_valid,
                                       uint32_t dedup_timer_ms,
                                       uint16_t last_seq,
                                       uint32_t last_bitmap,
                                       uint16_t seq,
                                       uint32_t bitmap) {
    return last_signature_valid &&
           dedup_timer_ms > 0 &&
           last_seq == seq &&
           last_bitmap == bitmap;
}

inline uint32_t ackDedupWindowMs(uint32_t ack_repeat_delay_ms) {
    return std::clamp(ack_repeat_delay_ms + 40u, 80u, 500u);
}

inline uint32_t sackTimerForFrame(uint32_t current_timer_ms,
                                  uint32_t sack_delay_ms,
                                  uint32_t sack_delay_short_ms,
                                  bool frame_more_frag) {
    uint32_t pick_ms = sack_delay_ms;
    if (sack_delay_short_ms != 0 && !frame_more_frag) {
        pick_ms = sack_delay_short_ms;
    }

    if (current_timer_ms == 0) {
        return pick_ms;
    }
    return std::min(current_timer_ms, pick_ms);
}

inline bool isAlignedBaseHoleAck(uint16_t ack_seq,
                                 uint16_t tx_base_seq,
                                 uint32_t bitmap) {
    const bool is_aligned = (ack_seq == ((tx_base_seq - 1) & 0xFFFF));
    const bool has_hole = (bitmap & 0x01u) == 0 && (bitmap & ~0x01u) != 0;
    return is_aligned && has_hole;
}

inline uint32_t holeProbeInitialTimerMs(uint32_t ack_timeout_ms) {
    return std::clamp(ack_timeout_ms / 2, 1200u, 2500u);
}

inline uint32_t holeProbeNextTimerMs(uint32_t ack_timeout_ms) {
    return std::clamp((ack_timeout_ms * 2) / 3, 1800u, 3000u);
}

inline uint32_t fastRetransmitCooldownMs(uint32_t ack_timeout_ms) {
    return std::clamp(ack_timeout_ms / 6, 300u, 1200u);
}

inline bool shouldFastRetransmitHole(int hole_ack_count,
                                     int fast_retx_count,
                                     uint32_t fast_retx_cooldown_ms) {
    return hole_ack_count >= kMinHoleConfirmations &&
           fast_retx_count < kMaxFastRetransmitsPerHole &&
           fast_retx_cooldown_ms == 0;
}

inline uint32_t ackRepeatDelayForCopy(uint32_t ack_repeat_delay_ms, int copy_index) {
    if (copy_index <= 2) {
        return ack_repeat_delay_ms;
    }
    return ack_repeat_delay_ms * 3;
}

inline int ackRepeatJitterMs(uint16_t base_seq, uint32_t bitmap, int copy_index) {
    uint32_t h = static_cast<uint32_t>(base_seq);
    h = (h * 1103515245u + 12345u) ^ (bitmap << 8) ^ (bitmap >> 16)
        ^ static_cast<uint32_t>(copy_index * 7919);
    return static_cast<int>(h % 61u) - 30;
}

}  // namespace selective_repeat_arq_policy
}  // namespace protocol
}  // namespace ultra
