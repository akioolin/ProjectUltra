// test_tone_burst_ack_payload.cpp — unit tests for tone-burst ACK payload,
// CRC-12, and (15,11) Hamming codec. PHY_ADAPTATION_DESIGN §15.7-8 step 1.

#include "waveform/tone_burst_ack/tone_burst_constants.hpp"
#include "waveform/tone_burst_ack/tone_burst_payload.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using namespace ultra::waveform::tone_burst_ack;

namespace {

int g_failures = 0;

#define EXPECT(cond)                                                            \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                       \
    } while (0)

#define EXPECT_EQ(a, b)                                                         \
    do {                                                                        \
        const auto _va = (a);                                                   \
        const auto _vb = (b);                                                   \
        if (!(_va == _vb)) {                                                    \
            ++g_failures;                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s != %s (%lld vs %lld)\n",       \
                         __FILE__, __LINE__, #a, #b,                            \
                         static_cast<long long>(_va),                           \
                         static_cast<long long>(_vb));                          \
        }                                                                       \
    } while (0)

// ============================================================================
// Constants sanity
// ============================================================================

void test_constants_sanity() {
    std::printf("[test] constants_sanity\n");
    // Costas pattern uses only valid dibits.
    for (uint8_t d : kCostasPattern) EXPECT(d < kNumTones);
    // Tone frequencies are inside the subband.
    for (float f : kToneFreqHz) {
        EXPECT(f >= static_cast<float>(kSubbandLowHz));
        EXPECT(f <= static_cast<float>(kSubbandHighHz));
    }
    // Total symbols == Costas + payload.
    EXPECT_EQ(kTotalSymbols, kCostasSymbols + kPayloadSymbols);
    // 23 payload symbols * 2 bits/sym = 46 bit-slots, holds 45 coded bits.
    EXPECT(kPayloadSymbols * kBitsPerSymbol >= kHammingCodedBitsTotal);
    // Tone frequencies are integer multiples of 25 Hz for clean alignment.
    for (float f : kToneFreqHz) {
        const float quotient = f / 25.0f;
        EXPECT_EQ(static_cast<int>(quotient * 100), static_cast<int>(quotient) * 100);
    }
}

// ============================================================================
// Pack/unpack round-trip
// ============================================================================

void test_pack_unpack_round_trip() {
    std::printf("[test] pack_unpack_round_trip\n");

    std::vector<ToneBurstAckPayload> cases;
    {
        ToneBurstAckPayload p;
        p.group_seq = 0; p.frame_mask = 0; p.rate_hint = 0; p.type = AckType::Ack;
        cases.push_back(p);
    }
    {
        ToneBurstAckPayload p;
        p.group_seq = 63; p.frame_mask = 63; p.rate_hint = 7; p.type = AckType::Nack;
        cases.push_back(p);
    }
    {
        ToneBurstAckPayload p;
        p.group_seq = 42; p.frame_mask = 0b101010; p.rate_hint = 4; p.type = AckType::Ack;
        cases.push_back(p);
    }
    {
        ToneBurstAckPayload p;
        p.group_seq = 13; p.frame_mask = 0b010101; p.rate_hint = 2; p.type = AckType::Nack;
        cases.push_back(p);
    }

    for (const auto& orig : cases) {
        const uint32_t raw = packPayload(orig);
        EXPECT(verifyPayloadCRC(raw));
        const auto rt = unpackPayload(raw);
        EXPECT_EQ(rt.group_seq, orig.group_seq);
        EXPECT_EQ(rt.frame_mask, orig.frame_mask);
        EXPECT_EQ(rt.rate_hint, orig.rate_hint);
        EXPECT_EQ(static_cast<int>(rt.type), static_cast<int>(orig.type));
    }
}

void test_pack_clamps_out_of_range_fields() {
    std::printf("[test] pack_clamps_out_of_range_fields\n");
    ToneBurstAckPayload p;
    p.group_seq = 200;          // > 63
    p.frame_mask = 200;         // > 63
    p.rate_hint = 15;           // > 7
    p.type = AckType::Ack;
    const uint32_t raw = packPayload(p);
    EXPECT(verifyPayloadCRC(raw));
    const auto rt = unpackPayload(raw);
    // Clamped via mask.
    EXPECT_EQ(rt.group_seq, static_cast<uint8_t>(200 & 0x3F));
    EXPECT_EQ(rt.frame_mask, static_cast<uint8_t>(200 & 0x3F));
    EXPECT_EQ(rt.rate_hint, static_cast<uint8_t>(15 & 0x07));
}

// ============================================================================
// CRC-12 sanity
// ============================================================================

void test_crc12_detects_single_bit_flips() {
    std::printf("[test] crc12_detects_single_bit_flips\n");
    ToneBurstAckPayload p;
    p.group_seq = 17; p.frame_mask = 0b110001; p.rate_hint = 3; p.type = AckType::Ack;
    const uint32_t raw = packPayload(p);
    EXPECT(verifyPayloadCRC(raw));
    // Flip each of the 16 useful bits and confirm CRC catches it.
    int caught = 0;
    for (uint32_t i = 0; i < kPayloadUsefulBits; ++i) {
        const uint32_t flipped = raw ^ (1u << i);
        if (!verifyPayloadCRC(flipped)) ++caught;
    }
    EXPECT_EQ(caught, static_cast<int>(kPayloadUsefulBits));
}

void test_crc12_detects_two_bit_flips_in_useful_bits() {
    std::printf("[test] crc12_detects_two_bit_flips_in_useful_bits\n");
    ToneBurstAckPayload p;
    p.group_seq = 25; p.frame_mask = 0b011110; p.rate_hint = 5; p.type = AckType::Nack;
    const uint32_t raw = packPayload(p);
    int caught = 0;
    int total = 0;
    for (uint32_t i = 0; i < kPayloadUsefulBits; ++i) {
        for (uint32_t j = i + 1; j < kPayloadUsefulBits; ++j) {
            const uint32_t flipped = raw ^ (1u << i) ^ (1u << j);
            ++total;
            if (!verifyPayloadCRC(flipped)) ++caught;
        }
    }
    // CRC-12 detects all 1-2 bit errors over <= 12-bit windows; over 16 bits
    // it doesn't guarantee 100% but should be very close. Require >= 95%.
    const double rate = static_cast<double>(caught) / static_cast<double>(total);
    std::printf("  2-bit detection: %d/%d (%.1f%%)\n", caught, total, rate * 100.0);
    EXPECT(rate >= 0.95);
}

// ============================================================================
// (15,11) Hamming codec
// ============================================================================

void test_hamming_round_trip_all_codewords() {
    std::printf("[test] hamming_round_trip_all_codewords\n");
    for (uint16_t info = 0; info < (1u << kHammingInfoBitsPerBlock); ++info) {
        const uint16_t coded = hammingEncode15_11(info);
        int corrected = 0;
        const uint16_t decoded = hammingDecode15_11(coded, corrected);
        EXPECT_EQ(decoded, info);
        EXPECT_EQ(corrected, 0);
    }
}

void test_hamming_corrects_single_bit_errors() {
    std::printf("[test] hamming_corrects_single_bit_errors\n");
    int errors_recovered = 0;
    int total = 0;
    // Sample a few hundred random codewords for speed.
    std::mt19937 rng(0xC0DEBEEFu);
    std::uniform_int_distribution<uint32_t> info_dist(0, (1u << kHammingInfoBitsPerBlock) - 1u);
    for (int trial = 0; trial < 500; ++trial) {
        const uint16_t info = static_cast<uint16_t>(info_dist(rng));
        const uint16_t coded = hammingEncode15_11(info);
        for (uint32_t bit = 0; bit < kHammingCodedBitsPerBlock; ++bit) {
            const uint16_t corrupted = coded ^ static_cast<uint16_t>(1u << bit);
            int corrected = 0;
            const uint16_t decoded = hammingDecode15_11(corrupted, corrected);
            ++total;
            if (decoded == info && corrected == 1) ++errors_recovered;
        }
    }
    std::printf("  single-bit recovery: %d/%d\n", errors_recovered, total);
    EXPECT_EQ(errors_recovered, total);
}

// ============================================================================
// Full payload -> dibits -> payload (clean channel)
// ============================================================================

void test_payload_dibits_round_trip_clean() {
    std::printf("[test] payload_dibits_round_trip_clean\n");
    ToneBurstAckPayload p;
    p.group_seq = 21; p.frame_mask = 0b111100; p.rate_hint = 4; p.type = AckType::Ack;
    const auto dibits = encodePayloadDibits(p);
    EXPECT_EQ(dibits.size(), static_cast<size_t>(kPayloadSymbols));
    for (uint8_t d : dibits) EXPECT(d < kNumTones);

    PayloadDecodeStats stats;
    const auto recovered = decodePayloadDibits(dibits, stats);
    EXPECT(recovered.has_value());
    EXPECT(stats.crc_ok);
    EXPECT_EQ(stats.hamming_corrected_blocks, 0);
    if (recovered) {
        EXPECT_EQ(recovered->group_seq, p.group_seq);
        EXPECT_EQ(recovered->frame_mask, p.frame_mask);
        EXPECT_EQ(recovered->rate_hint, p.rate_hint);
        EXPECT_EQ(static_cast<int>(recovered->type), static_cast<int>(p.type));
    }
}

void test_on_air_includes_costas_prefix() {
    std::printf("[test] on_air_includes_costas_prefix\n");
    ToneBurstAckPayload p;
    p.group_seq = 7; p.frame_mask = 0b001110; p.rate_hint = 3; p.type = AckType::Nack;
    const auto on_air = buildOnAirDibits(p);
    EXPECT_EQ(on_air.size(), static_cast<size_t>(kTotalSymbols));
    for (uint32_t i = 0; i < kCostasSymbols; ++i) {
        EXPECT_EQ(on_air[i], kCostasPattern[i]);
    }
}

// ============================================================================
// Single-dibit error: one dibit flip = at most 2 coded-bit flips in ONE block
// -> at least one block sees a 2-bit error which (15,11) Hamming can't
// correct. Outer CRC must catch.
// ============================================================================

void test_single_dibit_error_recovery() {
    std::printf("[test] single_dibit_error_recovery\n");
    ToneBurstAckPayload p;
    p.group_seq = 34; p.frame_mask = 0b110011; p.rate_hint = 2; p.type = AckType::Ack;
    auto dibits = encodePayloadDibits(p);

    int decoded_ok = 0;
    int detected_corrupt = 0;
    int total = 0;
    for (size_t i = 0; i < dibits.size(); ++i) {
        for (uint8_t alt = 0; alt < kNumTones; ++alt) {
            if (alt == dibits[i]) continue;
            auto corrupted = dibits;
            corrupted[i] = alt;
            PayloadDecodeStats stats;
            const auto recovered = decodePayloadDibits(corrupted, stats);
            ++total;
            // Two possibilities, both acceptable: Hamming corrects + CRC ok
            // (with the original payload), OR CRC fails and we report no
            // decode. The unacceptable outcome is "CRC ok but wrong payload".
            if (recovered.has_value()) {
                const bool matches_original =
                    recovered->group_seq == p.group_seq &&
                    recovered->frame_mask == p.frame_mask &&
                    recovered->rate_hint == p.rate_hint &&
                    recovered->type == p.type;
                if (matches_original) ++decoded_ok;
                else {
                    // CRC ok but mis-decoded -> Hamming mis-correction that
                    // happened to produce a CRC collision. Should be vanishingly
                    // rare with CRC-12. Count as "undetected" — must be 0.
                    EXPECT(false);
                }
            } else {
                ++detected_corrupt;
            }
        }
    }
    std::printf("  single-dibit results: decoded_ok=%d, detected_corrupt=%d, total=%d\n",
                decoded_ok, detected_corrupt, total);
    // Either outcome is fine; the test gate is "no undetected mis-decode" (above).
    EXPECT_EQ(decoded_ok + detected_corrupt, total);
}

// ============================================================================
// Costas pattern autocorrelation properties
// ============================================================================

void test_costas_autocorrelation_peak() {
    std::printf("[test] costas_autocorrelation_peak\n");
    // For a Costas-style time-frequency pattern, the "thumbtack" auto-
    // ambiguity surface should have a strong peak at (0,0) and low side-
    // lobes elsewhere. We compute the discrete 2D autocorrelation over
    // (time_shift, freq_shift) where freq_shift is in tone-index units.

    const auto& p = kCostasPattern;
    const int N = static_cast<int>(p.size());
    const int F = static_cast<int>(kNumTones);

    // Compute peak (zero shift) and max side-lobe.
    auto matches_at = [&](int dt, int df) {
        int hits = 0;
        for (int i = 0; i < N; ++i) {
            const int j = i + dt;
            if (j < 0 || j >= N) continue;
            const int fi = p[i];
            const int fj = (p[j] + df + F) % F;
            // Pattern correlation: count time-frequency matches.
            if (fi == fj) ++hits;
        }
        return hits;
    };

    const int peak = matches_at(0, 0);
    int max_sidelobe = 0;
    for (int dt = -N + 1; dt <= N - 1; ++dt) {
        for (int df = -F + 1; df <= F - 1; ++df) {
            if (dt == 0 && df == 0) continue;
            const int h = matches_at(dt, df);
            if (h > max_sidelobe) max_sidelobe = h;
        }
    }
    std::printf("  peak=%d, max_sidelobe=%d, ratio=%.2f\n",
                peak, max_sidelobe,
                max_sidelobe ? static_cast<double>(peak) / max_sidelobe : 0.0);
    EXPECT_EQ(peak, N);
    EXPECT(max_sidelobe < N / 2);  // sidelobes must be substantially lower
}

}  // namespace

int main() {
    test_constants_sanity();
    test_pack_unpack_round_trip();
    test_pack_clamps_out_of_range_fields();
    test_crc12_detects_single_bit_flips();
    test_crc12_detects_two_bit_flips_in_useful_bits();
    test_hamming_round_trip_all_codewords();
    test_hamming_corrects_single_bit_errors();
    test_payload_dibits_round_trip_clean();
    test_on_air_includes_costas_prefix();
    test_single_dibit_error_recovery();
    test_costas_autocorrelation_peak();
    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d test assertion(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll tone-burst ACK payload tests PASSED\n");
    return 0;
}
