#pragma once

#include "frame_v2.hpp"
#include "waveform_selection.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ultra {
namespace protocol {
namespace connection_policy {

inline constexpr uint32_t kOFDMSampleRate = 48000;
inline constexpr uint32_t kWideOFDMFFTSamples = 1024;
inline constexpr uint32_t kWideOFDMLongCPSamples = 128;
inline constexpr uint32_t kWideOFDMSymbolSamples = kWideOFDMFFTSamples + kWideOFDMLongCPSamples;
inline constexpr uint32_t kWideOFDMCarriers = 59;
inline constexpr uint32_t kNarrowOFDMSymbolSamples = 2240;
inline constexpr uint32_t kNarrowOFDMCarriers = 21;
inline constexpr uint32_t kNarrowOFDMPilotSpacing = 10;
inline constexpr uint32_t kLDPCBitsPerCodeword = 648;
inline constexpr uint32_t kFixedFrameCodewords = v2::kDefaultFixedFrameCodewords;
inline constexpr uint32_t kOFDMBurstAckBatchFrames = 4;
inline constexpr size_t kWideOFDMWindowFrames = 8;
inline constexpr size_t kHighThroughputOFDMWindowFrames = 16;
inline constexpr size_t kBurstInterleaveGroupFrames = 8;
inline constexpr uint32_t kResponderHandshakeFailSafeMs = 2200;
inline constexpr uint32_t kConnectAckLegacyRetransmitMs = 6000;

struct OFDMFrameTiming {
    uint32_t data_symbols = 0;
    uint32_t ack_symbols = 0;
    uint32_t data_ms = 0;
    uint32_t ack_ms = 0;
};

struct SackDelayProfile {
    uint32_t delay_ms = 120;
    uint32_t short_delay_ms = 0;
};

struct AckRepeatProfile {
    int count = 2;
    uint32_t delay_ms = 220;
};

inline const char* fadingLabel(float fading) {
    if (fading < 0.15f) return "AWGN";
    if (fading < 0.65f) return "Good";
    if (fading < 1.10f) return "Moderate";
    return "Poor";
}

inline uint8_t modeToCapabilityBit(WaveformMode mode) {
    switch (mode) {
        case WaveformMode::OFDM_COX:    return ModeCapabilities::OFDM_COX;
        case WaveformMode::OFDM_CHIRP:  return ModeCapabilities::OFDM_CHIRP;
        case WaveformMode::OFDM_NARROW: return ModeCapabilities::OFDM_NARROW;
        case WaveformMode::OTFS_EQ:     return ModeCapabilities::OTFS_EQ;
        case WaveformMode::OTFS_RAW:    return ModeCapabilities::OTFS_RAW;
        case WaveformMode::MFSK:        return ModeCapabilities::MFSK;
        case WaveformMode::MC_DPSK:     return ModeCapabilities::MC_DPSK;
        default: return 0;
    }
}

inline bool isNearAwgnOFDM(float fading_index, float snr_db) {
    return fading_index < 0.15f && snr_db >= 15.0f;
}

inline bool isHighThroughputOFDM(float fading_index, float snr_db) {
    return fading_index < 0.65f && snr_db >= 15.0f;
}

inline bool isHighThroughputOFDMMode(Modulation mod, CodeRate rate) {
    if (mod != Modulation::DQPSK) {
        return false;
    }
    return rate == CodeRate::R1_2 ||
           rate == CodeRate::R2_3 ||
           rate == CodeRate::R3_4;
}

inline bool isSpeculativeHighRateOFDM(Modulation mod, CodeRate rate) {
    return mod == Modulation::DQPSK &&
           (rate == CodeRate::R2_3 || rate == CodeRate::R3_4);
}

inline size_t ofdmWindowSize(Modulation mod, CodeRate rate, bool near_awgn_ofdm) {
    if (!isHighThroughputOFDMMode(mod, rate)) {
        return kWideOFDMWindowFrames;
    }

    if (isSpeculativeHighRateOFDM(mod, rate) && !near_awgn_ofdm) {
        return kWideOFDMWindowFrames;
    }

    return kHighThroughputOFDMWindowFrames;
}

inline size_t ofdmWindowSize(Modulation mod, CodeRate rate) {
    return ofdmWindowSize(mod, rate, true);
}

inline size_t ofdmWindowSizeForChannel(Modulation mod,
                                       CodeRate rate,
                                       float fading_index,
                                       float snr_db) {
    return ofdmWindowSize(mod, rate, isNearAwgnOFDM(fading_index, snr_db));
}

inline bool shouldPadHighRateFadingBurst(Modulation mod,
                                         CodeRate rate,
                                         bool near_awgn_ofdm,
                                         size_t burst_frames) {
    if (!isSpeculativeHighRateOFDM(mod, rate) || near_awgn_ofdm) {
        return false;
    }
    if (burst_frames <= 1) {
        return false;
    }

    return (burst_frames % kBurstInterleaveGroupFrames) != 0;
}

inline uint32_t ofdmAckBatchSize(bool near_awgn_ofdm) {
    (void)near_awgn_ofdm;
    return 0;
}

inline uint32_t bitsPerOFDMSymbol(uint32_t carriers,
                                  bool include_pilots,
                                  int pilot_spacing,
                                  Modulation mod) {
    return static_cast<uint32_t>(ofdm_link_adaptation::bitsPerOFDMSymbol(
        static_cast<int>(carriers), include_pilots, pilot_spacing, mod));
}

inline uint32_t wideOFDMSymbolsForCodewords(Modulation mod, CodeRate rate, int codewords) {
    const int pilot_spacing = ofdm_link_adaptation::recommendedPilotSpacing(mod, rate);
    const uint32_t bits_per_symbol = bitsPerOFDMSymbol(
        kWideOFDMCarriers, true, pilot_spacing, mod);
    const uint32_t frame_bits = static_cast<uint32_t>(codewords) * kLDPCBitsPerCodeword;
    const uint32_t data_symbols = (frame_bits + bits_per_symbol - 1) / bits_per_symbol;
    return 2 + data_symbols;
}

inline OFDMFrameTiming wideOFDMFrameTiming(Modulation mod,
                                           CodeRate rate,
                                           int cw_count = v2::kDefaultFixedFrameCodewords) {
    cw_count = v2::sanitizeFixedFrameCodewords(cw_count);
    constexpr float symbol_ms =
        (1000.0f * static_cast<float>(kWideOFDMSymbolSamples)) /
        static_cast<float>(kOFDMSampleRate);

    OFDMFrameTiming timing;
    timing.data_symbols = wideOFDMSymbolsForCodewords(mod, rate, cw_count);
    timing.ack_symbols = wideOFDMSymbolsForCodewords(mod, rate, 1);
    timing.data_ms = static_cast<uint32_t>(timing.data_symbols * symbol_ms + 0.5f);
    timing.ack_ms = static_cast<uint32_t>(timing.ack_symbols * symbol_ms + 0.5f);
    return timing;
}

inline SackDelayProfile ofdmSackDelays(bool defer_to_burst_tail,
                                       size_t window_size,
                                       uint32_t data_frame_ms) {
    SackDelayProfile profile;
    if (defer_to_burst_tail && window_size >= 8) {
        const size_t deferred_frames = window_size > kBurstInterleaveGroupFrames
            ? window_size - kBurstInterleaveGroupFrames
            : 0;
        const uint64_t burst_tail_ms =
            static_cast<uint64_t>(deferred_frames) * data_frame_ms + 120u;
        profile.delay_ms = static_cast<uint32_t>(
            std::clamp<uint64_t>(burst_tail_ms, 120ULL, 12000ULL));
        profile.short_delay_ms = 120;
    }
    return profile;
}

inline AckRepeatProfile ofdmAckRepeatProfile(Modulation mod,
                                             CodeRate rate,
                                             bool near_awgn_ofdm) {
    (void)mod;
    (void)rate;
    AckRepeatProfile profile;
    if (near_awgn_ofdm) {
        profile.count = 1;
    }
    return profile;
}

inline uint32_t computeWideOFDMAckTimeoutMs(Modulation mod,
                                            CodeRate rate,
                                            size_t window_size,
                                            uint32_t sack_delay_ms,
                                            int ack_repeat_count,
                                            int cw_count = v2::kDefaultFixedFrameCodewords) {
    const int sanitized_cw_count = v2::sanitizeFixedFrameCodewords(cw_count);
    const OFDMFrameTiming timing = wideOFDMFrameTiming(mod, rate, sanitized_cw_count);

    const uint32_t ack_copies = static_cast<uint32_t>(std::clamp(ack_repeat_count, 1, 3));
    const uint32_t tx_burst_ms = static_cast<uint32_t>(window_size) * timing.data_ms;
    const uint32_t ack_path_ms = ack_copies * timing.ack_ms + sack_delay_ms;

    constexpr uint32_t audio_chain_rtt_margin_ms = 700;
    const uint32_t decode_jitter_margin_ms = std::max<uint32_t>(700, timing.data_ms / 2)
                                             + audio_chain_rtt_margin_ms;

    // tx_burst_ms already spans every frame in the sender window, and
    // sack_delay_ms already includes receiver-side burst-tail ACK deferral.
    const uint32_t timeout_ms = tx_burst_ms + ack_path_ms + decode_jitter_margin_ms;

    uint32_t ceiling_ms = 16000;
    if (sanitized_cw_count > v2::kDefaultFixedFrameCodewords) {
        ceiling_ms = static_cast<uint32_t>(
            std::max<uint64_t>(16000ULL, 3ULL * tx_burst_ms));
    }
    return std::clamp(timeout_ms, 8000u, ceiling_ms);
}

inline uint32_t connectAckRetransmitDelayMs(WaveformMode mode,
                                            Modulation mod,
                                            CodeRate rate,
                                            int cw_count = v2::kDefaultFixedFrameCodewords) {
    if (!isOFDMMode(mode)) {
        return kConnectAckLegacyRetransmitMs;
    }

    // CONNECT_ACK is an MC-DPSK full-preamble frame. If the initiator receives
    // it and immediately starts an OFDM burst, the responder cannot deliver a
    // valid DATA frame until the first burst-interleaver group has been
    // collected and deinterleaved. Delay the rescue retransmit until after that
    // success path has had time to clear the cached ACK.
    const OFDMFrameTiming timing = wideOFDMFrameTiming(mod, rate, cw_count);
    const uint64_t first_group_ms =
        static_cast<uint64_t>(timing.data_ms) * kBurstInterleaveGroupFrames;
    const uint64_t delay_ms =
        kResponderHandshakeFailSafeMs + first_group_ms + 3500u;

    return static_cast<uint32_t>(
        std::clamp<uint64_t>(delay_ms, kConnectAckLegacyRetransmitMs, 12000ULL));
}

inline OFDMFrameTiming narrowOFDMFrameTiming(Modulation mod,
                                             int cw_count = v2::kDefaultFixedFrameCodewords) {
    cw_count = v2::sanitizeFixedFrameCodewords(cw_count);
    constexpr float symbol_ms =
        (1000.0f * static_cast<float>(kNarrowOFDMSymbolSamples)) /
        static_cast<float>(kOFDMSampleRate);

    const uint32_t bits_per_symbol = bitsPerOFDMSymbol(
        kNarrowOFDMCarriers, true, static_cast<int>(kNarrowOFDMPilotSpacing), mod);

    const uint32_t data_cw_symbols =
        (static_cast<uint32_t>(cw_count) * kLDPCBitsPerCodeword + bits_per_symbol - 1) / bits_per_symbol;
    const uint32_t ack_cw_symbols =
        (kLDPCBitsPerCodeword + bits_per_symbol - 1) / bits_per_symbol;

    OFDMFrameTiming timing;
    timing.data_symbols = 2 + data_cw_symbols;
    timing.ack_symbols = 2 + ack_cw_symbols;
    timing.data_ms = static_cast<uint32_t>(timing.data_symbols * symbol_ms + 0.5f);
    timing.ack_ms = static_cast<uint32_t>(timing.ack_symbols * symbol_ms + 0.5f);
    return timing;
}

inline uint32_t computeNarrowOFDMAckTimeoutMs(Modulation mod,
                                              int cw_count = v2::kDefaultFixedFrameCodewords) {
    const OFDMFrameTiming timing = narrowOFDMFrameTiming(mod, cw_count);
    const uint32_t timeout_ms = timing.data_ms + 2 * timing.ack_ms + 120 +
                                std::max<uint32_t>(700, timing.data_ms / 2);
    return std::clamp(timeout_ms, 4500u, 14000u);
}

inline WaveformMode selectNegotiatedMode(uint8_t local_caps,
                                         uint8_t remote_caps,
                                         WaveformMode remote_pref,
                                         WaveformMode narrowband_override,
                                         WaveformMode local_pref,
                                         float snr_db,
                                         float fading_index) {
    const uint8_t common = local_caps & remote_caps;
    if (common == 0) {
        return WaveformMode::OFDM_COX;
    }

    if (remote_pref != WaveformMode::AUTO &&
        (common & modeToCapabilityBit(remote_pref))) {
        return remote_pref;
    }

    if (narrowband_override != WaveformMode::AUTO &&
        (common & modeToCapabilityBit(narrowband_override))) {
        return narrowband_override;
    }

    if (local_pref != WaveformMode::AUTO &&
        (common & modeToCapabilityBit(local_pref))) {
        return local_pref;
    }

    const auto rec = recommendWaveformAndRate(snr_db, fading_index);
    if (common & modeToCapabilityBit(rec.waveform)) {
        return rec.waveform;
    }

    if (common & ModeCapabilities::OFDM_COX) return WaveformMode::OFDM_COX;
    if (common & ModeCapabilities::OFDM_CHIRP) return WaveformMode::OFDM_CHIRP;
    if (common & ModeCapabilities::OFDM_NARROW) return WaveformMode::OFDM_NARROW;
    if (common & ModeCapabilities::MC_DPSK) return WaveformMode::MC_DPSK;

    return WaveformMode::OFDM_COX;
}

}  // namespace connection_policy
}  // namespace protocol
}  // namespace ultra
