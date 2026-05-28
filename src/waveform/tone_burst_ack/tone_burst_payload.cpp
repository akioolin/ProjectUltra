// tone_burst_payload.cpp — implementation of pack/unpack + CRC-12 + (15,11)
// Hamming + payload encode/decode for the tone-burst ACK.

#include "tone_burst_payload.hpp"

#include <array>
#include <cassert>

namespace ultra {
namespace waveform {
namespace tone_burst_ack {

namespace {

// Extract `bits` bits starting at position `offset` (LSB-first) from `value`.
inline uint32_t getBits(uint32_t value, uint32_t offset, uint32_t bits) {
    const uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    return (value >> offset) & mask;
}

// Insert `bits` bits of `field` starting at position `offset` (LSB-first)
// into `value` (sets, does not clear other bits — caller must ensure target
// bits are 0).
inline uint32_t putBits(uint32_t value, uint32_t offset, uint32_t bits, uint32_t field) {
    const uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    return value | ((field & mask) << offset);
}

}  // namespace

// ============================================================================
// ToneBurstAckPayload
// ============================================================================

bool ToneBurstAckPayload::clampToWireWidths() {
    const uint8_t group_seq_max = (1u << kPayloadGroupSeqBits) - 1u;
    const uint8_t frame_mask_max = (1u << kPayloadFrameMaskBits) - 1u;
    const uint8_t rate_hint_max = (1u << kPayloadRateHintBits) - 1u;

    bool clean = true;
    if (group_seq > group_seq_max) { group_seq &= group_seq_max; clean = false; }
    if (frame_mask > frame_mask_max) { frame_mask &= frame_mask_max; clean = false; }
    if (rate_hint > rate_hint_max) { rate_hint &= rate_hint_max; clean = false; }
    return clean;
}

// ============================================================================
// Pack / unpack
// ============================================================================

uint32_t packPayload(const ToneBurstAckPayload& p) {
    ToneBurstAckPayload sanitized = p;
    sanitized.clampToWireWidths();

    uint32_t raw = 0;
    raw = putBits(raw, kBitOffsetGroupSeq, kPayloadGroupSeqBits, sanitized.group_seq);
    raw = putBits(raw, kBitOffsetFrameMask, kPayloadFrameMaskBits, sanitized.frame_mask);
    raw = putBits(raw, kBitOffsetRateHint, kPayloadRateHintBits, sanitized.rate_hint);
    raw = putBits(raw, kBitOffsetType, kPayloadTypeBits,
                  static_cast<uint32_t>(sanitized.type));

    const uint32_t useful = raw & ((1u << kPayloadUsefulBits) - 1u);
    const uint16_t crc = crc12(useful, kPayloadUsefulBits);
    raw = putBits(raw, kBitOffsetCRC, kPayloadCRCBits, crc);
    // Reserved bits stay zero.
    return raw;
}

ToneBurstAckPayload unpackPayload(uint32_t raw) {
    ToneBurstAckPayload p;
    p.group_seq = static_cast<uint8_t>(getBits(raw, kBitOffsetGroupSeq, kPayloadGroupSeqBits));
    p.frame_mask = static_cast<uint8_t>(getBits(raw, kBitOffsetFrameMask, kPayloadFrameMaskBits));
    p.rate_hint = static_cast<uint8_t>(getBits(raw, kBitOffsetRateHint, kPayloadRateHintBits));
    p.type = static_cast<AckType>(getBits(raw, kBitOffsetType, kPayloadTypeBits));
    return p;
}

// ============================================================================
// CRC-12-CCITT (poly 0x80F, init 0xFFF)
// ============================================================================

uint16_t crc12(uint32_t value, uint32_t bits) {
    // Standard CRC-12-CCITT polynomial: x^12 + x^11 + x^3 + x^2 + x + 1
    // = 0x80F (without the implicit x^12 term).
    constexpr uint16_t kPoly = 0x80F;
    constexpr uint16_t kInit = 0xFFF;
    constexpr uint16_t kCrcMask = 0xFFF;  // 12 bits

    uint16_t crc = kInit;
    // Process MSB-first within the message bits.
    for (uint32_t i = bits; i > 0; --i) {
        const uint16_t input_bit = static_cast<uint16_t>((value >> (i - 1)) & 1u);
        const uint16_t top_bit = (crc >> 11) & 1u;
        crc = static_cast<uint16_t>((crc << 1) & kCrcMask);
        if (top_bit ^ input_bit) {
            crc ^= kPoly;
        }
    }
    return static_cast<uint16_t>(crc & kCrcMask);
}

bool verifyPayloadCRC(uint32_t raw) {
    const uint32_t useful = raw & ((1u << kPayloadUsefulBits) - 1u);
    const uint16_t expected = crc12(useful, kPayloadUsefulBits);
    const uint16_t observed = static_cast<uint16_t>(getBits(raw, kBitOffsetCRC, kPayloadCRCBits));
    return expected == observed;
}

// ============================================================================
// (15, 11) Hamming codec
// ============================================================================

namespace {

// 1-indexed position of each info bit in the 15-bit codeword.
// Positions 1, 2, 4, 8 are parity; everything else is info.
//   info 0 -> pos  3
//   info 1 -> pos  5
//   info 2 -> pos  6
//   info 3 -> pos  7
//   info 4 -> pos  9
//   info 5 -> pos 10
//   info 6 -> pos 11
//   info 7 -> pos 12
//   info 8 -> pos 13
//   info 9 -> pos 14
//   info 10-> pos 15
constexpr std::array<uint8_t, kHammingInfoBitsPerBlock> kInfoPos = {{
    3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15
}};

}  // namespace

uint16_t hammingEncode15_11(uint16_t info_bits) {
    // Place info bits at their codeword positions (positions are 1-indexed;
    // we store the codeword in bits 0..14 corresponding to positions 1..15).
    uint16_t codeword = 0;
    for (uint32_t i = 0; i < kHammingInfoBitsPerBlock; ++i) {
        const uint32_t info_bit = (info_bits >> i) & 1u;
        const uint32_t pos = kInfoPos[i];  // 1..15
        codeword |= static_cast<uint16_t>(info_bit << (pos - 1));
    }
    // Compute the 4 parity bits.
    //   p1 at pos 1 covers positions with bit 0 set: 1,3,5,7,9,11,13,15
    //   p2 at pos 2 covers positions with bit 1 set: 2,3,6,7,10,11,14,15
    //   p4 at pos 4 covers positions with bit 2 set: 4,5,6,7,12,13,14,15
    //   p8 at pos 8 covers positions with bit 3 set: 8,9,10,11,12,13,14,15
    for (uint32_t p = 0; p < 4; ++p) {
        const uint32_t parity_pos_mask = 1u << p;  // 1, 2, 4, 8
        uint32_t parity = 0;
        for (uint32_t pos = 1; pos <= 15; ++pos) {
            if (pos == parity_pos_mask) continue;  // skip the parity bit itself
            if ((pos & parity_pos_mask) != 0) {
                parity ^= ((codeword >> (pos - 1)) & 1u);
            }
        }
        codeword |= static_cast<uint16_t>(parity << (parity_pos_mask - 1));
    }
    return static_cast<uint16_t>(codeword & 0x7FFF);  // 15 bits
}

uint16_t hammingDecode15_11(uint16_t coded_bits, int& corrected_errors) {
    corrected_errors = 0;
    uint16_t cw = static_cast<uint16_t>(coded_bits & 0x7FFF);

    // Compute syndrome (4 bits). Each parity bit's syndrome is XOR of all
    // positions covered by it (INCLUDING the parity bit itself, which is at
    // position == 1<<p — that XOR should give 0 if no error).
    uint32_t syndrome = 0;
    for (uint32_t p = 0; p < 4; ++p) {
        const uint32_t parity_pos_mask = 1u << p;
        uint32_t s = 0;
        for (uint32_t pos = 1; pos <= 15; ++pos) {
            if ((pos & parity_pos_mask) != 0) {
                s ^= ((cw >> (pos - 1)) & 1u);
            }
        }
        syndrome |= (s << p);
    }

    // Non-zero syndrome -> 1-indexed position of the bit error.
    if (syndrome != 0 && syndrome <= 15) {
        cw ^= static_cast<uint16_t>(1u << (syndrome - 1));
        corrected_errors = 1;
    }
    // Extract info bits from their codeword positions.
    uint16_t info = 0;
    for (uint32_t i = 0; i < kHammingInfoBitsPerBlock; ++i) {
        const uint32_t pos = kInfoPos[i];
        const uint32_t bit = (cw >> (pos - 1)) & 1u;
        info |= static_cast<uint16_t>(bit << i);
    }
    return info;
}

// ============================================================================
// Full payload encode/decode (32-bit payload <-> dibits)
// ============================================================================

namespace {

// Split a 33-bit (one zero-pad) info stream into 3 blocks of 11 bits each.
std::array<uint16_t, kHammingNumBlocks> splitToHammingBlocks(uint64_t info_33) {
    std::array<uint16_t, kHammingNumBlocks> blocks{};
    for (uint32_t b = 0; b < kHammingNumBlocks; ++b) {
        blocks[b] = static_cast<uint16_t>(
            (info_33 >> (b * kHammingInfoBitsPerBlock)) &
            ((1u << kHammingInfoBitsPerBlock) - 1u));
    }
    return blocks;
}

// Merge 3 11-bit blocks back to a 33-bit value (drop the top pad bit).
uint64_t mergeFromHammingBlocks(const std::array<uint16_t, kHammingNumBlocks>& blocks) {
    uint64_t info_33 = 0;
    for (uint32_t b = 0; b < kHammingNumBlocks; ++b) {
        info_33 |= (static_cast<uint64_t>(blocks[b] &
                    ((1u << kHammingInfoBitsPerBlock) - 1u))
                    << (b * kHammingInfoBitsPerBlock));
    }
    return info_33;
}

// Convert a stream of 45 coded bits to 23 dibits (4-FSK symbols, 2 bits each).
// Last dibit's high bit is zero-padded (45 bits -> 23 dibits = 46 bit-slots).
std::vector<uint8_t> codedBitsToDibits(uint64_t coded_45) {
    std::vector<uint8_t> dibits;
    dibits.reserve(kPayloadSymbols);
    for (uint32_t s = 0; s < kPayloadSymbols; ++s) {
        const uint32_t shift = s * kBitsPerSymbol;
        const uint8_t dibit = static_cast<uint8_t>((coded_45 >> shift) & 0x3u);
        dibits.push_back(dibit);
    }
    return dibits;
}

uint64_t dibitsToCodedBits(const std::vector<uint8_t>& dibits) {
    uint64_t coded = 0;
    const size_t n = std::min<size_t>(dibits.size(), kPayloadSymbols);
    for (size_t s = 0; s < n; ++s) {
        coded |= (static_cast<uint64_t>(dibits[s] & 0x3u) << (s * kBitsPerSymbol));
    }
    return coded;
}

}  // namespace

std::vector<uint8_t> encodePayloadDibits(const ToneBurstAckPayload& p) {
    const uint32_t raw_32 = packPayload(p);
    // Pad to 33 bits (top bit = 0) for clean 3-block split.
    const uint64_t info_33 = static_cast<uint64_t>(raw_32);

    auto blocks = splitToHammingBlocks(info_33);
    uint64_t coded_45 = 0;
    for (uint32_t b = 0; b < kHammingNumBlocks; ++b) {
        const uint16_t coded = hammingEncode15_11(blocks[b]);
        coded_45 |= (static_cast<uint64_t>(coded &
                     ((1u << kHammingCodedBitsPerBlock) - 1u))
                     << (b * kHammingCodedBitsPerBlock));
    }
    return codedBitsToDibits(coded_45);
}

std::optional<ToneBurstAckPayload> decodePayloadDibits(
    const std::vector<uint8_t>& dibits, PayloadDecodeStats& stats) {
    stats.hamming_corrected_blocks = 0;
    stats.crc_ok = false;

    if (dibits.size() < kPayloadSymbols) {
        return std::nullopt;
    }

    const uint64_t coded_45 = dibitsToCodedBits(dibits);

    std::array<uint16_t, kHammingNumBlocks> info_blocks{};
    for (uint32_t b = 0; b < kHammingNumBlocks; ++b) {
        const uint16_t coded = static_cast<uint16_t>(
            (coded_45 >> (b * kHammingCodedBitsPerBlock)) &
            ((1u << kHammingCodedBitsPerBlock) - 1u));
        int corrected = 0;
        info_blocks[b] = hammingDecode15_11(coded, corrected);
        if (corrected > 0) ++stats.hamming_corrected_blocks;
    }

    const uint64_t info_33 = mergeFromHammingBlocks(info_blocks);
    const uint32_t raw_32 = static_cast<uint32_t>(info_33 & 0xFFFFFFFFu);

    if (!verifyPayloadCRC(raw_32)) {
        return std::nullopt;
    }
    stats.crc_ok = true;
    return unpackPayload(raw_32);
}

std::vector<uint8_t> buildOnAirDibits(const ToneBurstAckPayload& p) {
    std::vector<uint8_t> on_air;
    on_air.reserve(kTotalSymbols);
    // Costas prefix.
    for (uint8_t d : kCostasPattern) on_air.push_back(d);
    // Payload.
    auto payload = encodePayloadDibits(p);
    on_air.insert(on_air.end(), payload.begin(), payload.end());
    assert(on_air.size() == kTotalSymbols);
    return on_air;
}

}  // namespace tone_burst_ack
}  // namespace waveform
}  // namespace ultra
