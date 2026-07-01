// tone_burst_constants.hpp — wire-format constants for the tone-burst ACK
//
// Tone-burst ACK is a narrowband 4-FSK control plane that replaces the
// OFDM 1-CW group ACK in burst transport (PHY_ADAPTATION_DESIGN §15).
//
// Design constraints (all four perspectives):
//
// PHY theorist
//   - 4-FSK gives 2 bits/symbol; 27-bit payload + (15,11) Hamming + 5-symbol
//     Costas sync = ~19 symbols total = ~475 ms at 40 baud (25 ms/sym).
//   - Tones spaced 75 Hz apart in a 300 Hz subband. 75 Hz > Δf for orthogonal
//     FSK at this baud (orthogonality threshold = 1/T_sym = 40 Hz).
//   - Costas pattern at start (5 tones across the 4 frequencies in a
//     specific order) lets the receiver verify "an ACK is arriving" with a
//     known-pattern matched filter — distinguishes the burst from noise that
//     happens to have energy in our band.
//
// DSP systems engineer
//   - All tone frequencies are integer multiples of 25 Hz (sub-band
//     anchored at 2400 Hz, sample rate 48000). 48000 / 25 = 1920 samples
//     per "tone cycle" — clean alignment with sample boundaries.
//   - Symbol duration 25 ms = 1200 samples at 48 kHz. FFT bin resolution
//     for a 1200-sample window is 40 Hz; tone separation 75 Hz puts each
//     tone in a distinct bin with ample margin.
//   - Receiver runs a sliding 1200-sample FFT focused on the four ACK
//     tone bins (4 complex multiplies + 4 magnitudes per sample = trivial).
//
// HF veteran operator
//   - 2400-2700 Hz is above the OFDM data band (300-2400 Hz for 51 carriers
//     centered ~1500 Hz). SSB radios pass 2400-2700 cleanly; rolloff steepens
//     past 2700.
//   - Voice operators use tone bursts at these frequencies routinely
//     (CTCSS, repeater identification). Real-radio proven path.
//
// First principles
//   - Processing gain for matched-filter detection on a known tone in a
//     narrow subband: 10·log₁₀(2·T·BW) = 10·log₁₀(2·0.025·40) = +3 dB per
//     symbol. Over 19 symbols → +16 dB cumulative integration. With 4-FSK
//     discrimination (~2 dB extra cost over 2-FSK), net detection floor
//     ~-2 dB in-band SNR for the 475 ms baseline duration. Longer
//     integration extends the floor lower (§15.5 staircase).

#pragma once

#include <array>
#include <cstdint>

namespace ultra {
namespace waveform {
namespace tone_burst_ack {

// ============================================================================
// Audio-band layout
// ============================================================================

inline constexpr uint32_t kSampleRate = 48000;
inline constexpr uint32_t kSubbandLowHz = 2400;   // bottom of the tone subband
inline constexpr uint32_t kSubbandHighHz = 2700;  // top of the tone subband

// 4-FSK tone frequencies, spaced 75 Hz apart, centered in the subband.
// Each tone is at an integer multiple of 25 Hz so 1200-sample symbol
// windows hit exact integer cycles (suppresses leakage in the FFT bin).
inline constexpr std::array<float, 4> kToneFreqHz = {{
    2400.0f,  // tone 0 -> dibit 0b00
    2475.0f,  // tone 1 -> dibit 0b01
    2550.0f,  // tone 2 -> dibit 0b10
    2625.0f,  // tone 3 -> dibit 0b11
}};

inline constexpr uint32_t kToneSeparationHz = 75;
inline constexpr uint32_t kNumTones = 4;
inline constexpr uint32_t kBitsPerSymbol = 2;  // 4-FSK -> 2 bits/sym

// ============================================================================
// Timing
// ============================================================================

// Baseline (Good-channel) symbol duration. The §15.5 staircase scales
// duration with negotiated SNR; this is the default for SNR >= 12 dB.
inline constexpr uint32_t kBaselineSymbolMs = 25;
inline constexpr uint32_t kBaselineSymbolSamples =
    (kSampleRate * kBaselineSymbolMs) / 1000;  // 1200 samples @ 48 kHz
static_assert(kBaselineSymbolSamples * 1000 == kSampleRate * kBaselineSymbolMs,
              "tone-burst symbol duration must be sample-aligned");

// SNR-adaptive durations (§15.5 staircase).
inline constexpr uint32_t kSymbolMsHighSNR = 12;   // SNR >= 18 dB
inline constexpr uint32_t kSymbolMsMidSNR = 25;    // SNR 12-18 dB (baseline)
inline constexpr uint32_t kSymbolMsLowSNR = 50;    // SNR 5-12 dB
inline constexpr uint32_t kSymbolMsMargSNR = 100;  // SNR -5 to 5 dB
inline constexpr uint32_t kSymbolMsWeakSNR = 200;  // SNR < -5 dB

// §15.5 staircase selector: map a measured in-band SNR to the symbol duration.
// Shorter symbols at high SNR shrink the ACK airtime (lower half-duplex
// turnaround); longer symbols at low SNR add matched-filter integration for
// robust detection. The receiver's ACK monitor scans ALL durations, so the
// two ends need not pre-agree. Thresholds mirror the kSymbolMs* comments above;
// monotonic non-increasing, so it only shortens as SNR rises.
//
// fading_present (BUG-ACK-STAIRCASE-FADE-BIN, 2026-07-01): the caller's in-band
// SNR basis is FADE-EFFECTIVE on a fading channel (carries the ~2 dB
// fading/Jensen penalty vs the AWGN-equivalent dial), while the 18 dB edge was
// calibrated in AWGN-equivalent terms against the pre-2026-06-16 (absolute-
// referenced) meter — so the fast rung never engaged on fading (measured
// occupancy: 3 of 4 runs at 0%, incl. 30/30 rig ACKs at 675 ms at MPG@20).
// The 12 ms rung's hardware proof (2026-06-15, MPG@20, 0 retx/15 bursts) is
// the SAME physical operating point that now reads ~16-17 effective, so on a
// fading channel the fast edge is 16 dB — a basis correction back to the
// validated point, not a new one. ONLY the top edge shifts: the lower rungs
// are detection-safety-side and stay put (shifting them would shorten ACKs at
// low SNR — the unsafe direction).
inline constexpr float kFastAckEdgeAwgnDb = 18.0f;
inline constexpr float kFastAckEdgeFadingDb = 16.0f;
inline constexpr uint32_t symbolMsForSNR(float snr_db, bool fading_present = false) {
    const float fast_edge = fading_present ? kFastAckEdgeFadingDb : kFastAckEdgeAwgnDb;
    if (snr_db >= fast_edge) return kSymbolMsHighSNR;  // 12 ms -> 324 ms airtime
    if (snr_db >= 12.0f) return kSymbolMsMidSNR;    // 25 ms -> 675 ms (baseline)
    if (snr_db >= 5.0f)  return kSymbolMsLowSNR;    // 50 ms -> 1350 ms
    if (snr_db >= -5.0f) return kSymbolMsMargSNR;   // 100 ms -> 2700 ms
    return kSymbolMsWeakSNR;                         // 200 ms -> 5400 ms
}

// ============================================================================
// Costas sync pattern (4 symbols, ORDER-4 COSTAS ARRAY)
// ============================================================================
//
// Used at the START of every tone-burst ACK so the receiver can verify "this
// is an ACK arriving" with a known-pattern matched filter, before trying to
// decode the payload.
//
// Mathematical property — a Costas array of order N has the defining property
// that all N*(N-1)/2 pairwise (time, frequency) displacement vectors are
// DISTINCT. This bounds the autocorrelation sidelobe height at ≤ 1 by
// construction: at any non-zero shift, at most one pair of dots aligns. So
// peak=N, max_sidelobe≤1, peak/sidelobe ratio = N (i.e. +10·log₁₀(N) dB).
//
// {0, 1, 3, 2} is a verified order-4 Costas array (displacement vectors
// (1,1), (2,3), (3,2), (1,2), (2,1), (1,-1) are all distinct).
//   - main-lobe energy = 4 (4 symbols match themselves at zero shift)
//   - max side-lobe energy = 1 (Costas property)
//   - peak / side-lobe ratio = 4x = +6 dB
//
// Same family as the JT9/JT65/FT8 Costas sync arrays (those use order-7
// arrays for their longer messages; ours is shorter so order-4 is enough).
inline constexpr std::array<uint8_t, 4> kCostasPattern = {{0, 1, 3, 2}};
inline constexpr uint32_t kCostasSymbols = kCostasPattern.size();

// ============================================================================
// Payload + FEC layout
// ============================================================================
//
// Payload (packed into 32 bits with reserved/padding):
//   bits  0..5   group_seq (6 bits, mod-64 sequence number)
//   bits  6..13  frame_mask (8 bits, 1 = frame OK, 0 = frame FAIL)  [widened 6->8 2026-06-17
//                so a thin-frame cw5 burst can carry 8 frames and fill the PA-duty budget
//                instead of stalling window-bound at 6 — see connection_policy kToneBurstAckWindowCapFrames]
//   bits 14..16  rate_hint (3 bits, RateController feedback per §14.43)
//   bit  17      type (0 = ACK, 1 = NACK; receiver always emits a burst)
//   bits 18..29  crc12 (CRC-12 over the preceding 18 useful bits)
//   bits 30..31  reserved (must be 0)
//
// WIRE-BREAKING (2026-06-17): no version field on the tone-burst payload — both
// stations MUST run the same build (the offset shift moves the CRC field, so a
// mixed-version pair mis-parses every ACK -> CRC fail -> retx storm).
//
// FEC: (15,11) Hamming code applied per nibble-block. 32 payload bits ->
// three 11-bit groups (padded), each encoded to 15 coded bits = 45 coded
// bits = 23 symbols at 4-FSK (rounded up). Hamming corrects 1 bit error
// per block, detects 2.

inline constexpr uint32_t kPayloadBits = 32;       // raw payload incl. CRC
inline constexpr uint32_t kPayloadUsefulBits = 18; // bits before CRC (6+8+3+1)
inline constexpr uint32_t kPayloadGroupSeqBits = 6;
inline constexpr uint32_t kPayloadFrameMaskBits = 8;  // widened 6->8 (2026-06-17): 8-frame SACK window
inline constexpr uint32_t kPayloadRateHintBits = 3;
inline constexpr uint32_t kPayloadTypeBits = 1;
inline constexpr uint32_t kPayloadCRCBits = 12;
inline constexpr uint32_t kPayloadReservedBits = 2;
static_assert(kPayloadUsefulBits == kPayloadGroupSeqBits + kPayloadFrameMaskBits +
              kPayloadRateHintBits + kPayloadTypeBits, "payload bit layout mismatch");
static_assert(kPayloadBits == kPayloadUsefulBits + kPayloadCRCBits +
              kPayloadReservedBits, "payload total mismatch");

// Bit-field offsets in the packed 32-bit payload.
inline constexpr uint32_t kBitOffsetGroupSeq = 0;
inline constexpr uint32_t kBitOffsetFrameMask = 6;
inline constexpr uint32_t kBitOffsetRateHint = 14;
inline constexpr uint32_t kBitOffsetType = 17;
inline constexpr uint32_t kBitOffsetCRC = 18;
inline constexpr uint32_t kBitOffsetReserved = 30;

// (15,11) Hamming: 11 info bits -> 15 coded bits per block.
// 32 payload bits / 11 = 3 blocks (last block partially populated with 0s).
inline constexpr uint32_t kHammingInfoBitsPerBlock = 11;
inline constexpr uint32_t kHammingCodedBitsPerBlock = 15;
inline constexpr uint32_t kHammingNumBlocks = 3;  // ceil(32 / 11)
inline constexpr uint32_t kHammingInfoBitsTotal =
    kHammingNumBlocks * kHammingInfoBitsPerBlock;  // 33 (pad payload by 1 zero)
inline constexpr uint32_t kHammingCodedBitsTotal =
    kHammingNumBlocks * kHammingCodedBitsPerBlock;  // 45
inline constexpr uint32_t kPayloadSymbols =
    (kHammingCodedBitsTotal + kBitsPerSymbol - 1) / kBitsPerSymbol;  // 23

// ============================================================================
// Total burst structure
// ============================================================================
//
// On-air layout:  [COSTAS (4 sym)] [PAYLOAD (23 sym)]
// Total symbols:  27
// Baseline airtime (25 ms/sym): 675 ms
// High-SNR airtime (12 ms/sym): 324 ms
// Low-SNR airtime (50 ms/sym):  1350 ms
//
// Sender's "ACK detection window" after burst end = baseline_duration ± 200 ms.
// If no Costas pattern detected in window, sender treats as ACK-lost.

inline constexpr uint32_t kTotalSymbols = kCostasSymbols + kPayloadSymbols;  // 27
static_assert(kTotalSymbols == 27, "expected 27 total symbols");

inline constexpr uint32_t kBaselineTotalMs = kTotalSymbols * kBaselineSymbolMs;  // 675 ms
inline constexpr uint32_t kBaselineTotalSamples =
    kTotalSymbols * kBaselineSymbolSamples;  // 32400 samples

// Sender's no-ACK guard window around the expected ACK arrival.
inline constexpr uint32_t kAckDetectionGuardMs = 200;

// Number of consecutive ACK-lost events before escalating to OFDM ACK
// fallback (§15.7 question 5).
inline constexpr uint32_t kMissedAcksBeforeEscalation = 3;

// ============================================================================
// Tone amplitude (TX scaling)
// ============================================================================

// Single tone at the soundcard. PAPR is 0 dB (a sine has PAR = 3 dB amplitude
// to RMS, but only one tone is active at a time). Keep peak well below clipping.
inline constexpr float kToneAmplitude = 0.45f;  // peak; 0 dBFS = 1.0

}  // namespace tone_burst_ack
}  // namespace waveform
}  // namespace ultra
