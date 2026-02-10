#include "burst_interleaver.hpp"
#include <cassert>
#include <stdexcept>

namespace ultra {
namespace fec {

std::vector<std::vector<uint8_t>> BurstInterleaver::interleave(
    const std::vector<std::vector<uint8_t>>& logical_frames)
{
    const int N = static_cast<int>(logical_frames.size());
    if (N < 2) return logical_frames;  // Nothing to interleave

    const int B = BYTES_PER_FRAME;

    // Validate input sizes
    for (int f = 0; f < N; f++) {
        if (static_cast<int>(logical_frames[f].size()) != B) {
            throw std::invalid_argument("BurstInterleaver::interleave: frame size mismatch");
        }
    }

    // Allocate output
    std::vector<std::vector<uint8_t>> physical(N, std::vector<uint8_t>(B, 0));

    // Permutation: flat_pos = N * b + f, pf = flat_pos / B, pb = flat_pos % B
    for (int f = 0; f < N; f++) {
        for (int b = 0; b < B; b++) {
            int flat_pos = N * b + f;
            int pf = flat_pos / B;
            int pb = flat_pos % B;
            physical[pf][pb] = logical_frames[f][b];
        }
    }

    return physical;
}

std::vector<std::vector<float>> BurstInterleaver::deinterleave(
    const std::vector<std::vector<float>>& physical_soft)
{
    const int N = static_cast<int>(physical_soft.size());
    if (N < 2) return physical_soft;  // Nothing to deinterleave

    const int B = BYTES_PER_FRAME;
    const int BITS = BITS_PER_FRAME;

    // Validate input sizes
    for (int pf = 0; pf < N; pf++) {
        if (static_cast<int>(physical_soft[pf].size()) < BITS) {
            throw std::invalid_argument("BurstInterleaver::deinterleave: soft bits size mismatch");
        }
    }

    // Allocate output
    std::vector<std::vector<float>> logical(N, std::vector<float>(BITS, 0.0f));

    // Inverse permutation: for each logical[f][b*8..b*8+7], find it in physical
    // TX did: physical[pf][pb] = logical[f][b] where flat = N*b + f, pf = flat/B, pb = flat%B
    // RX: for each (f, b), compute flat = N*b + f, pf = flat/B, pb = flat%B
    //     then logical[f][8*b..8*b+7] = physical[pf][8*pb..8*pb+7]
    for (int f = 0; f < N; f++) {
        for (int b = 0; b < B; b++) {
            int flat_pos = N * b + f;
            int pf = flat_pos / B;
            int pb = flat_pos % B;

            // Copy 8 soft bits (one byte's worth)
            int src_offset = pb * 8;
            int dst_offset = b * 8;
            for (int bit = 0; bit < 8; bit++) {
                logical[f][dst_offset + bit] = physical_soft[pf][src_offset + bit];
            }
        }
    }

    return logical;
}

}  // namespace fec
}  // namespace ultra
