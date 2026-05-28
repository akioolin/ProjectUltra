// tone_burst_payload.hpp — packed payload + CRC-12 + (15,11) Hamming for
// the tone-burst ACK (PHY_ADAPTATION_DESIGN §15).

#pragma once

#include "tone_burst_constants.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace ultra {
namespace waveform {
namespace tone_burst_ack {

// ============================================================================
// ToneBurstAckPayload — semantic view of one tone-burst ACK
// ============================================================================

enum class AckType : uint8_t {
    Ack = 0,
    Nack = 1,
};

struct ToneBurstAckPayload {
    // group_seq: 6-bit (mod-64) burst-group sequence number being ACK'd.
    uint8_t group_seq = 0;

    // frame_mask: 6 bits, one per frame in the group.
    //   bit i = 1 -> frame i delivered OK
    //   bit i = 0 -> frame i FAILED (sender should resend just this frame)
    // For a NACK, mask is the "still missing" mask (sender resends each 0 bit).
    uint8_t frame_mask = 0;

    // rate_hint: 3-bit RateController feedback (§14.43).
    // Encoding: 0=R1/4, 1=R1/3, 2=R1/2, 3=R2/3, 4=R3/4, 5=R5/6, 6=unused, 7=hold.
    uint8_t rate_hint = 0;

    AckType type = AckType::Ack;

    // Sanitize fields to their bit-widths. Returns true if all fields are
    // already within range (no truncation occurred).
    bool clampToWireWidths();
};

// ============================================================================
// Bit-level pack / unpack
// ============================================================================
//
// Layout (LSB first within each field, fields in ascending bit-order):
//   bits  0..5   group_seq
//   bits  6..11  frame_mask
//   bits 12..14  rate_hint
//   bit  15      type (0=ACK, 1=NACK)
//   bits 16..27  crc12
//   bits 28..31  reserved (zero)
//
// The CRC is computed over the 16 "useful" bits (bits 0..15) as a single
// little-endian 16-bit word, using CRC-12-CCITT (poly 0x80F, init 0xFFF).
//
// We use a 12-bit CRC (rather than 16) to keep the packet small: 12 bits at
// ~1 bit/symbol after 4-FSK + (15,11) Hamming means ~3-4 fewer symbols on
// air. CRC-12 still catches all 1-3 bit bursts and >99.97% of random errors
// for our 16-bit message — overkill for ACK semantics.

// Pack 32 raw payload bits (excluding Hamming) into a uint32_t (LSB-first).
uint32_t packPayload(const ToneBurstAckPayload& p);

// Unpack a raw 32-bit payload back to fields. Does NOT validate the CRC;
// use verifyPayloadCRC() for that.
ToneBurstAckPayload unpackPayload(uint32_t raw);

// CRC-12-CCITT (poly 0x80F, init 0xFFF, refin/refout false, xorout 0).
// Computed over `bits` lowest-order bits of `value` (MSB-first into the
// CRC engine).
uint16_t crc12(uint32_t value, uint32_t bits);

// Verify the embedded CRC against the rest of the payload. Returns true if
// the CRC matches.
bool verifyPayloadCRC(uint32_t raw);

// ============================================================================
// (15, 11) Hamming codec
// ============================================================================
//
// Per-block: take 11 info bits, append 4 parity bits, total 15 coded bits.
// We do NOT use the extended SECDED variant (which would add a 16th parity
// bit). For our packet size, the gain from SECDED is modest and costs an
// extra symbol per block.
//
// Encoding scheme (systematic, standard textbook):
//   Bit positions 1..15 (1-indexed):
//     pos 1, 2, 4, 8 = parity bits (cover overlapping bit subsets)
//     pos 3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15 = info bits 0..10
//   Parity i covers all positions whose bit i is set:
//     p1 covers positions {1, 3, 5, 7, 9, 11, 13, 15}
//     p2 covers positions {2, 3, 6, 7, 10, 11, 14, 15}
//     p4 covers positions {4, 5, 6, 7, 12, 13, 14, 15}
//     p8 covers positions {8, 9, 10, 11, 12, 13, 14, 15}
//
// Decoder: compute syndrome (4 bits). If 0, no error. If non-zero, the
// syndrome (read as binary) is the 1-indexed bit position to flip.

// Encode 11 info bits into 15 coded bits. info_bits must be < (1<<11).
// Returns coded bits in bits 0..14 of the uint16_t.
uint16_t hammingEncode15_11(uint16_t info_bits);

// Decode 15 received bits to 11 info bits, correcting up to 1 bit error.
// Sets `corrected_errors` to the number of bits corrected (0 or 1).
// Two-bit errors mis-correct silently (this is the (15,11) limit; the
// outer CRC catches them).
uint16_t hammingDecode15_11(uint16_t coded_bits, int& corrected_errors);

// ============================================================================
// Full packet encode/decode (payload <-> dibits ready for 4-FSK modulation)
// ============================================================================

// Encode a full ToneBurstAckPayload to a sequence of dibits (2-bit values
// 0..3) representing 4-FSK tone indices for the PAYLOAD portion of the
// burst. Length = kPayloadSymbols. Does NOT include the Costas prefix
// (the encoder prepends that).
std::vector<uint8_t> encodePayloadDibits(const ToneBurstAckPayload& p);

// Decode payload dibits back to a ToneBurstAckPayload. Returns std::nullopt
// if the CRC fails after Hamming correction.
struct PayloadDecodeStats {
    int hamming_corrected_blocks = 0;   // count of blocks where Hamming corrected 1 bit
    bool crc_ok = false;
};
std::optional<ToneBurstAckPayload> decodePayloadDibits(
    const std::vector<uint8_t>& dibits, PayloadDecodeStats& stats);

// ============================================================================
// Costas pattern helpers
// ============================================================================

// Get the full Costas sync pattern as dibit indices.
inline std::array<uint8_t, kCostasSymbols> costasDibits() {
    return kCostasPattern;
}

// Build the full on-air dibit sequence: [COSTAS] [PAYLOAD]. Length =
// kTotalSymbols. This is what the modulator consumes.
std::vector<uint8_t> buildOnAirDibits(const ToneBurstAckPayload& p);

}  // namespace tone_burst_ack
}  // namespace waveform
}  // namespace ultra
