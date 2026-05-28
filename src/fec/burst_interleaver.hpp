#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>

namespace ultra {
namespace fec {

/**
 * Burst-level interleaver for spreading coded bytes across N physical frames.
 *
 * Purpose: Spread each logical frame's coded bytes across multiple physical frames
 * so that a single frame loss only destroys 1/N of each codeword's bits.
 * With N=4 and R1/2 LDPC (50% redundancy), losing one frame means each CW
 * loses 25% of bits — well within correction capacity.
 *
 * Permutation (byte-level row-column block interleave):
 *   flat_pos = N * b + f    (b = byte index within frame, f = frame index)
 *   physical_frame = flat_pos / B
 *   physical_byte  = flat_pos % B
 *
 * where B = coded bytes per OFDM frame (CW count × 81 bytes).
 *
 * TX: interleave coded bytes across N frames
 * RX: deinterleave soft bits (operates on byte-groups of 8 floats)
 */
class BurstInterleaver {
public:
    static constexpr int DEFAULT_CODEWORDS_PER_FRAME = 4;
    static constexpr int CODEWORD_BYTES = 81;        // N=648 default
    static constexpr int CODEWORD_BITS = 648;
    static constexpr int BYTES_PER_FRAME = DEFAULT_CODEWORDS_PER_FRAME * CODEWORD_BYTES;
    static constexpr int BITS_PER_FRAME = DEFAULT_CODEWORDS_PER_FRAME * CODEWORD_BITS;

    static int sanitizeCodewordCount(int codeword_count);
    // Backwards-compat: bytes_per_cw defaults to 81 (N=648). Pass 243 for N=1944.
    static int bytesPerFrame(int codeword_count, int bytes_per_cw = CODEWORD_BYTES);
    static int bitsPerFrame(int codeword_count, int bytes_per_cw = CODEWORD_BYTES);

    /**
     * TX: Interleave coded bytes across N physical frames.
     *
     * Input:  N logical frames, each BYTES_PER_FRAME bytes
     * Output: N physical frames, each BYTES_PER_FRAME bytes
     * N is determined from input.size().
     */
    static std::vector<std::vector<uint8_t>> interleave(
        const std::vector<std::vector<uint8_t>>& logical_frames,
        int codeword_count,
        int bytes_per_cw = CODEWORD_BYTES);
    static std::vector<std::vector<uint8_t>> interleave(
        const std::vector<std::vector<uint8_t>>& logical_frames);

    /**
     * RX: Deinterleave soft bits back to logical frame order.
     *
     * Input:  N physical frames of soft bits, each BITS_PER_FRAME floats
     * Output: N logical frames of soft bits, each BITS_PER_FRAME floats
     * Operates on byte-groups of 8 soft bits to match byte-level TX interleaving.
     */
    static std::vector<std::vector<float>> deinterleave(
        const std::vector<std::vector<float>>& physical_soft,
        int codeword_count,
        int bytes_per_cw = CODEWORD_BYTES);
    static std::vector<std::vector<float>> deinterleave(
        const std::vector<std::vector<float>>& physical_soft);
};

}  // namespace fec
}  // namespace ultra
