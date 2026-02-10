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
 * where B = BYTES_PER_FRAME (coded bytes per OFDM frame = 4 CWs × 81 bytes = 324).
 *
 * TX: interleave coded bytes across N frames
 * RX: deinterleave soft bits (operates on byte-groups of 8 floats)
 */
class BurstInterleaver {
public:
    static constexpr int BYTES_PER_FRAME = 324;   // 4 CWs × 81 bytes
    static constexpr int BITS_PER_FRAME = 2592;   // 4 CWs × 648 bits

    /**
     * TX: Interleave coded bytes across N physical frames.
     *
     * Input:  N logical frames, each BYTES_PER_FRAME bytes
     * Output: N physical frames, each BYTES_PER_FRAME bytes
     * N is determined from input.size().
     */
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
        const std::vector<std::vector<float>>& physical_soft);
};

}  // namespace fec
}  // namespace ultra
