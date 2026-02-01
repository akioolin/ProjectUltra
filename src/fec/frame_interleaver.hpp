#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>

namespace ultra {
namespace fec {

/**
 * Frame-level interleaver for fixed 4-codeword frames.
 *
 * Purpose: Spread burst fading errors across all 4 codewords so that each CW
 * sees only 1/4 of the errors instead of one CW being completely corrupted.
 *
 * Operation:
 *   TX: After LDPC encoding, interleave coded bits across all 4 CWs
 *   RX: Before LDPC decoding, deinterleave soft bits back to original order
 *
 * Interleaving pattern (block interleave):
 *   Original order: [CW0: a0..a647][CW1: b0..b647][CW2: c0..c647][CW3: d0..d647]
 *   Interleaved:    [a0 b0 c0 d0 a1 b1 c1 d1 ... a647 b647 c647 d647]
 *
 * After a fading burst corrupting bits 1000-1500:
 *   Without interleave: CW1 completely lost (bits 648-1295)
 *   With interleave: Each CW loses ~125 bits (~19%), all decode
 *
 * R1/4 LDPC can correct up to ~37% bit errors, so 19% is easily recoverable.
 */
class FrameInterleaver {
public:
    // Fixed frame parameters
    static constexpr int NUM_CODEWORDS = 4;
    static constexpr int BITS_PER_CODEWORD = 648;
    static constexpr int TOTAL_FRAME_BITS = NUM_CODEWORDS * BITS_PER_CODEWORD;  // 2592

    /**
     * Interleave coded bits for transmission.
     *
     * Takes 4 codewords (4 × 81 bytes = 324 bytes) and interleaves at bit level.
     * Output is same size, bits reordered for fading resistance.
     *
     * @param coded_bytes Vector of 4 coded codewords (81 bytes each)
     * @return Interleaved bytes (324 bytes total)
     */
    static std::vector<uint8_t> interleave(const std::vector<std::vector<uint8_t>>& coded_codewords);

    /**
     * Deinterleave soft bits for decoding.
     *
     * Takes interleaved soft bits (2592 floats) and restores original order
     * so each codeword's soft bits are grouped together for LDPC decoding.
     *
     * @param interleaved_soft Interleaved soft bits (2592 floats)
     * @return Vector of 4 soft bit vectors (648 floats each)
     */
    static std::vector<std::vector<float>> deinterleave(const std::vector<float>& interleaved_soft);

    /**
     * Interleave soft bits (for testing/simulation).
     * Same pattern as byte interleave but operates on soft bits.
     */
    static std::vector<float> interleaveSoft(const std::vector<std::vector<float>>& soft_codewords);

private:
    // Permutation tables (computed once, used many times)
    static void ensureTablesInitialized();
    static std::vector<int> interleave_table_;  // Original index → interleaved index
    static std::vector<int> deinterleave_table_;  // Interleaved index → original index
    static bool tables_initialized_;
};

}  // namespace fec
}  // namespace ultra
