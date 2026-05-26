#pragma once

#include "frame_v2.hpp"
#include "waveform_selection.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cmath>

namespace ultra {
namespace protocol {
namespace connection_policy {

inline constexpr uint32_t kOFDMSampleRate = 48000;
inline constexpr uint32_t kWideOFDMFFTSamples = 1024;
inline constexpr uint32_t kWideOFDMLongCPSamples = 128;
inline constexpr uint32_t kWideOFDMSymbolSamples = kWideOFDMFFTSamples + kWideOFDMLongCPSamples;
inline constexpr uint32_t kWideOFDMCarriers = 59;
inline constexpr uint32_t kWideOFDMChirpDurationMs = 500;
inline constexpr uint32_t kWideOFDMChirpGapMs = 100;
inline constexpr uint32_t kWideOFDMFullAnchorExtraSamples =
    2 * ((kOFDMSampleRate * kWideOFDMChirpDurationMs) / 1000) +
    2 * ((kOFDMSampleRate * kWideOFDMChirpGapMs) / 1000);
inline constexpr uint32_t kWideOFDMFullAnchorExtraMs =
    (kWideOFDMFullAnchorExtraSamples * 1000 + kOFDMSampleRate - 1) / kOFDMSampleRate;
inline constexpr uint32_t kWideOFDMShortReanchorDefaultMs = 100;
inline constexpr uint32_t kWideOFDMShortReanchorMinMs = 100;
inline constexpr uint32_t kWideOFDMShortReanchorMaxMs = 300;
inline constexpr float kGoodHFDesignDopplerHz = 0.5f;
inline constexpr float kClarkeCoherenceNumerator = 0.423f;
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
inline constexpr uint32_t kMCDPSKDualChirpPreambleMs = 1200;
inline constexpr uint32_t kMCDPSKInterFrameGuardMs = 100;
inline constexpr uint32_t kMCDPSKRobustLowAckTimeoutFloorMs = 36000;
inline constexpr uint32_t kCarrierSenseSackCoalesceMs = 30;
inline constexpr int kCarrierSenseAckRepeatCount = 1;
inline constexpr uint32_t kWideOFDMAckTimeoutFloorMs = 8000;

struct OFDMFrameTiming {
    uint32_t data_symbols = 0;
    uint32_t ack_symbols = 0;
    uint32_t data_ms = 0;
    uint32_t ack_ms = 0;
};

struct MCDPSKFrameTiming {
    uint32_t overhead_symbols = 0;
    uint32_t data_only_symbols = 0;
    uint32_t data_symbols = 0;
    uint32_t ack_symbols = 0;
    uint32_t overhead_ms = 0;
    uint32_t data_only_ms = 0;
    uint32_t data_ms = 0;
    uint32_t ack_ms = 0;
};

inline const char* fadingLabel(float fading) {
    if (fading < 0.15f) return "AWGN";
    if (fading < 0.65f) return "Good";
    if (fading < 1.10f) return "Moderate";
    return "Poor";
}

enum class ChannelClassification {
    AWGN,
    GOOD,
    MODERATE,
    POOR,
};

inline ChannelClassification classifyChannel(float fading) {
    if (fading < 0.15f) return ChannelClassification::AWGN;
    if (fading < 0.65f) return ChannelClassification::GOOD;
    if (fading < 1.10f) return ChannelClassification::MODERATE;
    return ChannelClassification::POOR;
}

struct LadderRung {
    LadderRungId id = LadderRungId::UNKNOWN;
    const char* name = "Unknown";
    WaveformMode waveform = WaveformMode::MC_DPSK;
    Modulation modulation = Modulation::DQPSK;
    CodeRate code_rate = CodeRate::R1_4;
    int num_carriers = 0;
    int samples_per_symbol = 0;
    int cw_count = v2::kDefaultFixedFrameCodewords;
};

inline LadderRung ladderRungForId(LadderRungId id) {
    switch (id) {
        case LadderRungId::ROBUST_LOW:
            return {id, "Robust-Low", WaveformMode::MC_DPSK,
                    Modulation::DBPSK, CodeRate::R1_4, 8, 2048, 3};
        case LadderRungId::ROBUST_MID:
            return {id, "Robust-Mid", WaveformMode::MC_DPSK,
                    Modulation::DBPSK, CodeRate::R1_4, 8, 1024, 3};
        case LadderRungId::ROBUST:
            return {id, "Robust", WaveformMode::MC_DPSK,
                    Modulation::DQPSK, CodeRate::R1_4, 8, 1024,
                    v2::kDefaultFixedFrameCodewords};
        case LadderRungId::STANDARD:
            return {id, "Standard", WaveformMode::MC_DPSK,
                    Modulation::DQPSK, CodeRate::R1_4, 8, 512,
                    v2::kDefaultFixedFrameCodewords};
        case LadderRungId::OFDM_CHIRP:
            return {id, "OFDM_CHIRP", WaveformMode::OFDM_CHIRP,
                    Modulation::DQPSK, CodeRate::R1_4, 0, 0,
                    v2::kDefaultFixedFrameCodewords};
        case LadderRungId::OFDM_NARROW:
            return {id, "OFDM_NARROW", WaveformMode::OFDM_NARROW,
                    Modulation::DQPSK, CodeRate::R1_4, 0, 0,
                    v2::kDefaultFixedFrameCodewords};
        case LadderRungId::UNKNOWN:
        default:
            return {};
    }
}

inline LadderRung rungForMCDPSKConfig(Modulation modulation,
                                      int num_carriers,
                                      int samples_per_symbol,
                                      int cw_count) {
    if (num_carriers != 8) {
        return {};
    }
    if (modulation == Modulation::DBPSK && samples_per_symbol >= 2048 && cw_count == 3) {
        return ladderRungForId(LadderRungId::ROBUST_LOW);
    }
    if (modulation == Modulation::DBPSK && samples_per_symbol == 1024 && cw_count == 3) {
        return ladderRungForId(LadderRungId::ROBUST_MID);
    }
    if (modulation == Modulation::DQPSK && samples_per_symbol == 1024 &&
        cw_count == v2::kDefaultFixedFrameCodewords) {
        return ladderRungForId(LadderRungId::ROBUST);
    }
    if (modulation == Modulation::DQPSK && samples_per_symbol == 512 &&
        cw_count == v2::kDefaultFixedFrameCodewords) {
        return ladderRungForId(LadderRungId::STANDARD);
    }
    return {};
}

inline LadderRung selectLadderRung(float snr_db, ChannelClassification channel) {
    float ofdm_floor = 10.0f;
    float robust_floor = 13.0f;
    float robust_mid_floor = 5.0f;

    switch (channel) {
        case ChannelClassification::AWGN:
            ofdm_floor = 10.0f;
            robust_floor = 13.0f;
            robust_mid_floor = 5.0f;
            break;
        case ChannelClassification::GOOD:
            ofdm_floor = 12.0f;
            robust_floor = 14.0f;
            robust_mid_floor = 6.0f;
            break;
        case ChannelClassification::MODERATE:
            ofdm_floor = 14.0f;
            robust_floor = 15.0f;
            robust_mid_floor = 7.0f;
            break;
        case ChannelClassification::POOR:
            ofdm_floor = 18.0f;
            robust_floor = 17.0f;
            robust_mid_floor = 9.0f;
            break;
    }

    if (snr_db >= ofdm_floor) {
        return ladderRungForId(LadderRungId::OFDM_CHIRP);
    }
    if (snr_db >= robust_floor) {
        return ladderRungForId(LadderRungId::ROBUST);
    }
    if (snr_db >= robust_mid_floor) {
        return ladderRungForId(LadderRungId::ROBUST_MID);
    }
    return ladderRungForId(LadderRungId::ROBUST_LOW);
}

inline LadderRung selectLadderRung(float snr_db, float fading_index) {
    return selectLadderRung(snr_db, classifyChannel(fading_index));
}

inline uint8_t modeToCapabilityBit(WaveformMode mode) {
    switch (mode) {
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
    return fading_index < 0.15f && snr_db >= 25.0f;
}

inline bool isHighThroughputOFDM(float fading_index, float snr_db) {
    return fading_index < 0.65f && snr_db >= 25.0f;
}

inline bool isHighThroughputOFDMMode(Modulation mod, CodeRate rate) {
    // High-throughput predicate gates window=16 selective-repeat
    // (vs window=8 default). DQPSK at R1/2+ uses bigger window because
    // fading correlation across an 8-frame burst is tolerable; D8PSK
    // gets the same treatment because the 2026-05-04 D8PSK gate only
    // fires when the channel is good enough to support it (in-band SNR>=20
    // fading<0.65 minimum), which is the same precondition the larger
    // window assumes.
    if (mod == Modulation::DQPSK || mod == Modulation::D8PSK) {
        const auto* descriptor = ofdmCodeRateDescriptor(rate);
        const auto* floor = ofdmCodeRateDescriptor(CodeRate::R1_2);
        return descriptor != nullptr && floor != nullptr &&
               descriptor->code_rate >= floor->code_rate;
    }
    return false;
}

inline bool isSpeculativeHighRateOFDM(Modulation mod, CodeRate rate) {
    // R2/3 and R3/4 are speculative (window=16 only on near-AWGN);
    // R1/2 is non-speculative (window=16 always when fading channel
    // is good). Both DQPSK and D8PSK follow the same logic.
    const auto* descriptor = ofdmCodeRateDescriptor(rate);
    const auto* floor = ofdmCodeRateDescriptor(CodeRate::R1_2);
    const bool risky_rate = descriptor != nullptr && floor != nullptr &&
                            descriptor->code_rate > floor->code_rate;
    return risky_rate && (mod == Modulation::DQPSK || mod == Modulation::D8PSK);
}

inline bool isBurstInterleavedOFDMMode(Modulation mod, CodeRate rate) {
    // Coherent 8PSK/16-QAM need the per-frame chirp/LTS channel reference while
    // the 2026-05 burst-accumulation path is still being reworked. Round-5 Good
    // fading A/B showed burst groups 2/4/8 lose to independent coherent frames
    // because continuation timing failures dominate the diversity gain.
    return isSpeculativeHighRateOFDM(mod, rate);
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

inline bool shouldPadBurstInterleaveGroup(size_t burst_frames) {
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

inline uint32_t wideOFDMShortReanchorChirpDurationMs() {
    static const uint32_t duration_ms = [] {
        const char* value = std::getenv("ULTRA_SHORT_REANCHOR_CHIRP_MS");
        if (!value || value[0] == '\0') {
            return kWideOFDMShortReanchorDefaultMs;
        }

        char* end = nullptr;
        const float parsed = std::strtof(value, &end);
        if (end == value || !std::isfinite(parsed)) {
            return kWideOFDMShortReanchorDefaultMs;
        }
        const uint32_t rounded = static_cast<uint32_t>(parsed + 0.5f);
        return std::clamp<uint32_t>(rounded,
                                    kWideOFDMShortReanchorMinMs,
                                    kWideOFDMShortReanchorMaxMs);
    }();
    return duration_ms;
}

inline bool shouldUseWideOFDMShortReanchor(WaveformMode waveform,
                                           Modulation modulation,
                                           float fading_index) {
    if (waveform != WaveformMode::OFDM_CHIRP ||
        !ofdm_link_adaptation::isCoherentModulation(modulation) ||
        !std::isfinite(fading_index)) {
        return false;
    }

    return fading_index >= kQAM16AwgnFadingMax;
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

inline uint32_t wideOFDMBurstAirtimeMs(Modulation mod,
                                       CodeRate rate,
                                       size_t frame_count,
                                       int cw_count = v2::kDefaultFixedFrameCodewords,
                                       uint32_t continuation_reanchor_ms = 0) {
    if (frame_count == 0) {
        return 0;
    }

    const OFDMFrameTiming timing = wideOFDMFrameTiming(mod, rate, cw_count);
    uint64_t burst_ms = static_cast<uint64_t>(frame_count) * timing.data_ms;
    if (frame_count > 1) {
        // StreamingEncoder::encodeBurstLight() emits a full chirp anchor on the
        // first OFDM burst frame, then either light LTS-only preambles or
        // adaptive short chirp+LTS reanchors for continuations.
        burst_ms += kWideOFDMFullAnchorExtraMs;
        burst_ms += static_cast<uint64_t>(frame_count - 1) *
                    static_cast<uint64_t>(continuation_reanchor_ms);
    }
    return static_cast<uint32_t>(std::min<uint64_t>(burst_ms, 0xFFFFFFFFull));
}

inline uint32_t wideOFDMSackDelayMs(Modulation mod,
                                    CodeRate rate,
                                    size_t window_size,
                                    int cw_count = v2::kDefaultFixedFrameCodewords,
                                    uint32_t continuation_reanchor_ms = 0) {
    const uint32_t burst_ms = wideOFDMBurstAirtimeMs(
        mod, rate, std::max<size_t>(1, window_size), cw_count,
        continuation_reanchor_ms);
    return burst_ms + kCarrierSenseSackCoalesceMs;
}

inline uint32_t wideOFDMSackTailDelayMs() {
    return kCarrierSenseSackCoalesceMs;
}

inline uint32_t wideOFDMSlidingSackDelayMs(
        Modulation mod,
        CodeRate rate,
        int cw_count = v2::kDefaultFixedFrameCodewords) {
    const OFDMFrameTiming timing = wideOFDMFrameTiming(mod, rate, cw_count);
    // This timer is re-armed on every decoded DATA frame, so it is a quiet
    // interval after the observed burst tail, not a full-window hold. It covers
    // one selected-rate DATA frame cadence plus one selected-rate ACK/control
    // frame and the carrier-sense coalescing guard.
    const uint64_t quiet_interval_ms =
        static_cast<uint64_t>(timing.data_ms) +
        static_cast<uint64_t>(timing.ack_ms) +
        static_cast<uint64_t>(kCarrierSenseSackCoalesceMs);
    return static_cast<uint32_t>(std::min<uint64_t>(quiet_interval_ms, 0xFFFFFFFFull));
}

inline uint32_t coherenceTimeMsForDoppler(float doppler_hz) {
    if (!std::isfinite(doppler_hz) || doppler_hz <= 0.0f) {
        return UINT32_MAX;
    }
    // Clarke/Jakes 50%-correlation coherence-time approximation: Tc ~= 0.423/fD.
    const float tc_ms = (kClarkeCoherenceNumerator * 1000.0f) / doppler_hz;
    return static_cast<uint32_t>(std::max(1.0f, tc_ms) + 0.5f);
}

// Recommend fixed-frame CW count for a given OFDM data rate + waveform.
// Inputs are deterministic and shared by both peers (rate is negotiated;
// waveform is negotiated too) so both peers compute the same CW count
// without risk of one reading a different SNR/fading and picking a
// different CW geometry.
//
// Wide OFDM (OFDM_CHIRP):
//   R1/2, R2/3, R3/4 → 8 (hardware A/B Mac↔Pi5 5KB DQPSK R1/2 SNR=15
//   good fading: CW=4 → 1077 bps 2 retx; CW=8 → 1615 bps 0 retx, +50%.
//   D8PSK R3/4 SNR=27 AWGN ceiling: CW=8 → 3127 bps.)
//   R1/4 stays at default 4 (low-SNR robustness, no measured win wider).
//
// Narrow OFDM (OFDM_NARROW): always 4. Narrow R1/2 frames are already
// ~6 s at CW=8 with the 21-carrier geometry; window=3 burst would be
// ~18 s — longer than typical narrow good-fading coherence (~10 s).
// 3-seed sim A/B at SNR=8 good fading R1/2 (2 KB): CW=8 1/3 FAIL with
// 240 s timeout, 2/3 PASS at 124-172 bps; CW=4 baseline 3/3 PASS at
// 116-149 bps. The bigger frame becomes a single fade event's victim.
inline int recommendCWCount(CodeRate rate, WaveformMode waveform) {
    if (waveform == WaveformMode::OFDM_NARROW) {
        return v2::kDefaultFixedFrameCodewords;  // 4 — fade-coherence cap
    }
    if (const auto* descriptor = ofdmCodeRateDescriptor(rate)) {
        return descriptor->wide_cw_count;
    }
    return v2::kDefaultFixedFrameCodewords;
}

// Modulation-aware data-frame CW policy for waveforms whose fade exposure is
// dominated by frame duration. OFDM keeps the fixed-frame policy above.
// Robust-Low MC-DPSK DBPSK uses variable LDPC frames; with R1/4, 3 CW carries
// a 37-byte ARQ payload, i.e. 32 file bytes after FILE_DATA overhead. 1 CW
// cannot carry file data and 2 CW is too slow to be operationally useful.
inline int recommendCWCount(Modulation mod, CodeRate rate, WaveformMode waveform) {
    if (waveform == WaveformMode::MC_DPSK && mod == Modulation::DBPSK) {
        (void)rate;
        return 3;
    }
    return recommendCWCount(rate, waveform);
}

inline int recommendCWCountForChannel(Modulation mod,
                                      CodeRate rate,
                                      WaveformMode waveform,
                                      float fading_index,
                                      float snr_db,
                                      float doppler_hz = kGoodHFDesignDopplerHz) {
    const int baseline = recommendCWCount(mod, rate, waveform);
    if (waveform != WaveformMode::OFDM_CHIRP ||
        !ofdm_link_adaptation::isCoherentModulation(mod) ||
        isNearAwgnOFDM(fading_index, snr_db)) {
        return baseline;
    }

    const auto* descriptor = ofdmCodeRateDescriptor(rate);
    const auto* coherence_floor = ofdmCodeRateDescriptor(CodeRate::R1_2);
    if (descriptor == nullptr || coherence_floor == nullptr ||
        descriptor->code_rate < coherence_floor->code_rate) {
        return baseline;
    }

    const uint32_t coherence_ms = coherenceTimeMsForDoppler(doppler_hz);
    int selected = v2::kDefaultFixedFrameCodewords;
    for (int candidate = baseline; candidate >= v2::kDefaultFixedFrameCodewords; --candidate) {
        const auto timing = wideOFDMFrameTiming(mod, rate, candidate);
        if (timing.data_ms <= coherence_ms) {
            selected = candidate;
            break;
        }
    }
    return v2::sanitizeFixedFrameCodewords(selected);
}

inline uint32_t computeWideOFDMAckTimeoutMs(Modulation mod,
                                            CodeRate rate,
                                            size_t window_size,
                                            uint32_t sack_delay_ms,
                                            int ack_repeat_count,
                                            int cw_count = v2::kDefaultFixedFrameCodewords,
                                            uint32_t continuation_reanchor_ms = 0) {
    const int sanitized_cw_count = v2::sanitizeFixedFrameCodewords(cw_count);
    const OFDMFrameTiming timing = wideOFDMFrameTiming(mod, rate, sanitized_cw_count);

    const uint32_t ack_copies = static_cast<uint32_t>(std::clamp(ack_repeat_count, 1, 3));
    const size_t window_frames = std::max<size_t>(1, window_size);
    const uint32_t tx_burst_ms = wideOFDMBurstAirtimeMs(
        mod, rate, window_frames, sanitized_cw_count,
        continuation_reanchor_ms);
    const uint32_t physical_sack_hold_ms = std::max<uint32_t>(
        sack_delay_ms,
        wideOFDMSackDelayMs(mod, rate, window_frames, sanitized_cw_count,
                            continuation_reanchor_ms));
    const uint32_t ack_path_ms = ack_copies * timing.ack_ms + physical_sack_hold_ms;

    constexpr uint32_t audio_chain_rtt_margin_ms = 700;
    const uint32_t decode_jitter_margin_ms = std::max<uint32_t>(700, timing.data_ms / 2)
                                             + audio_chain_rtt_margin_ms;

    // tx_burst_ms spans the actual multi-frame OFDM burst, including the full
    // chirp anchor on the first frame. The SACK path must also include that
    // receiver holdoff because a half-duplex peer cannot ACK until the burst
    // has physically cleared.
    const uint32_t timeout_ms = tx_burst_ms + ack_path_ms + decode_jitter_margin_ms;

    return std::max<uint32_t>(timeout_ms, kWideOFDMAckTimeoutFloorMs);
}

inline uint32_t bitsPerMCDPSKCarrier(Modulation mod) {
    switch (mod) {
        case Modulation::DBPSK: return 1;
        case Modulation::D8PSK: return 3;
        case Modulation::DQPSK:
        default: return 2;
    }
}

inline uint32_t mcDpskSymbolsToMs(uint32_t symbols, int samples_per_symbol) {
    const int sps = std::clamp(samples_per_symbol, 1, 8192);
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(symbols) * static_cast<uint64_t>(sps) * 1000ULL +
         kOFDMSampleRate / 2) / kOFDMSampleRate);
}

inline MCDPSKFrameTiming mcDpskFrameTiming(Modulation mod,
                                           int num_carriers,
                                           int samples_per_symbol,
                                           int data_cw_count = v2::kDefaultFixedFrameCodewords) {
    const int carriers = std::clamp(num_carriers, 1, 64);
    const int sps = std::clamp(samples_per_symbol, 1, 8192);
    const int cw_count = v2::sanitizeFixedFrameCodewords(data_cw_count);
    const uint32_t bits_per_symbol = static_cast<uint32_t>(carriers) * bitsPerMCDPSKCarrier(mod);
    const uint32_t data_symbols_per_cw =
        (kLDPCBitsPerCodeword + bits_per_symbol - 1) / bits_per_symbol;

    constexpr uint32_t kMCDPSKTrainingSymbols = 8;
    constexpr uint32_t kMCDPSKReferenceSymbols = 1;
    const uint32_t overhead_symbols = kMCDPSKTrainingSymbols + kMCDPSKReferenceSymbols;

    MCDPSKFrameTiming timing;
    timing.overhead_symbols = overhead_symbols;
    timing.data_only_symbols = static_cast<uint32_t>(cw_count) * data_symbols_per_cw;
    timing.data_symbols = overhead_symbols + timing.data_only_symbols;
    timing.ack_symbols = overhead_symbols + data_symbols_per_cw;
    timing.overhead_ms = mcDpskSymbolsToMs(timing.overhead_symbols, sps);
    timing.data_only_ms = mcDpskSymbolsToMs(timing.data_only_symbols, sps);
    timing.data_ms = mcDpskSymbolsToMs(timing.data_symbols, sps);
    timing.ack_ms = mcDpskSymbolsToMs(timing.ack_symbols, sps);
    return timing;
}

inline uint32_t mcDpskBurstAirtimeMs(const MCDPSKFrameTiming& timing,
                                     size_t window_size) {
    if (window_size == 0) return 0;
    const uint64_t burst_ms =
        static_cast<uint64_t>(kMCDPSKDualChirpPreambleMs) +
        static_cast<uint64_t>(timing.overhead_ms) +
        static_cast<uint64_t>(window_size) * timing.data_only_ms;
    return static_cast<uint32_t>(
        std::min<uint64_t>(burst_ms, static_cast<uint64_t>(UINT32_MAX)));
}

inline size_t mcDpskWindowSizeForTiming(const MCDPSKFrameTiming& timing) {
    if (timing.data_ms == 0 || timing.data_only_ms == 0) return 1;

    constexpr uint32_t kTargetContinuousBurstMs = 19000;
    constexpr size_t kMaxAuditedContinuousMCDPSKWindow = 5;
    size_t selected = 1;
    for (size_t candidate = 2; candidate <= kMaxAuditedContinuousMCDPSKWindow; ++candidate) {
        if (mcDpskBurstAirtimeMs(timing, candidate) > kTargetContinuousBurstMs) {
            break;
        }
        selected = candidate;
    }
    return selected;
}

inline size_t mcDpskWindowSizeForTiming(uint32_t data_frame_ms) {
    if (data_frame_ms == 0) return 1;

    constexpr uint32_t kTargetBurstMs = 19000;
    constexpr size_t kMaxAuditedMCDPSKWindow = 5;
    const size_t by_burst = std::max<size_t>(1, kTargetBurstMs / data_frame_ms);
    return std::clamp<size_t>(by_burst, 1, kMaxAuditedMCDPSKWindow);
}

inline uint32_t computeMCDPSKAckTimeoutMs(const MCDPSKFrameTiming& timing,
                                          size_t window_size,
                                          uint32_t sack_delay_ms,
                                          int ack_repeat_count) {
    const uint32_t ack_copies = static_cast<uint32_t>(std::clamp(ack_repeat_count, 1, 3));
    const uint32_t tx_burst_ms = static_cast<uint32_t>(window_size) * timing.data_ms;
    const uint32_t ack_path_ms = ack_copies * timing.ack_ms + sack_delay_ms;

    // MC-DPSK uses full audio frames and a streaming decoder, so keep a larger
    // wall-clock margin than OFDM. This prevents a legal window burst from
    // self-timing-out before the receiver can batch and send its SACK.
    constexpr uint32_t kStreamingDecoderMarginMs = 12000;
    const uint32_t timeout_ms = tx_burst_ms + ack_path_ms + kStreamingDecoderMarginMs;
    return std::clamp(timeout_ms, 18000u, 72000u);
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
                                              int cw_count = v2::kDefaultFixedFrameCodewords,
                                              size_t window_size = 1) {
    const OFDMFrameTiming timing = narrowOFDMFrameTiming(mod, cw_count);
    // For window>1 selective-repeat, we hold the ARQ window full during
    // the burst, so the ACK timeout has to cover the full TX burst plus
    // ACK turnaround plus a generous decode margin. Without this scale
    // the timer fires while later frames are still on the wire.
    const uint32_t tx_burst_ms = timing.data_ms *
                                 static_cast<uint32_t>(std::max<size_t>(1, window_size));
    const uint32_t timeout_ms = tx_burst_ms + 2 * timing.ack_ms + 120 +
                                std::max<uint32_t>(700, timing.data_ms / 2);
    // Cap scales with window: 4.5s..14s for window=1, 4.5s..22s for window=2,
    // 4.5s..30s for window=3 etc. Avoids pathologically long single-window
    // values while letting large windows actually wait for their bursts.
    const uint32_t upper = 14000u + 8000u * static_cast<uint32_t>(
                               std::max<size_t>(1, window_size) - 1);
    return std::clamp(timeout_ms, 4500u, upper);
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
        // No overlap in advertised capabilities: fall back to MC-DPSK, the
        // universal robust floor every station implements.
        return WaveformMode::MC_DPSK;
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

    if (common & ModeCapabilities::OFDM_CHIRP) return WaveformMode::OFDM_CHIRP;
    if (common & ModeCapabilities::OFDM_NARROW) return WaveformMode::OFDM_NARROW;
    if (common & ModeCapabilities::MC_DPSK) return WaveformMode::MC_DPSK;

    // Last resort: MC-DPSK, the universal robust floor.
    return WaveformMode::MC_DPSK;
}

}  // namespace connection_policy
}  // namespace protocol
}  // namespace ultra
