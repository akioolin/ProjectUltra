#include "frame_interleaver.hpp"
#include <stdexcept>
#include <string>

namespace ultra {
namespace fec {

// Static member definitions
std::vector<int> FrameInterleaver::interleave_table_;
std::vector<int> FrameInterleaver::deinterleave_table_;
bool FrameInterleaver::tables_initialized_ = false;

void FrameInterleaver::ensureTablesInitialized() {
    if (tables_initialized_) return;

    // Build permutation tables for rotating round-robin interleaving
    //
    // For DQPSK, each carrier produces 2 bits with different reliability:
    //   MSB (sin-based): ±90° decision margin → more reliable
    //   LSB (cos-based): ±45° decision margin → less reliable
    //
    // Simple round-robin (bit*4 + cw) assigns CW0,CW2 always to MSB and
    // CW1,CW3 always to LSB, creating a systematic reliability imbalance.
    //
    // Fix: rotate CW assignment by LDPC bit position:
    //   interleaved_idx = bit * 4 + (cw + bit) % 4
    //
    // This ensures each CW gets 50% MSB + 50% LSB positions over every
    // 4 LDPC bit positions, equalizing reliability across all codewords.
    //
    // interleave_table_[original_idx] = interleaved_idx
    // deinterleave_table_[interleaved_idx] = original_idx

    interleave_table_.resize(TOTAL_FRAME_BITS);
    deinterleave_table_.resize(TOTAL_FRAME_BITS);

    for (int cw = 0; cw < NUM_CODEWORDS; ++cw) {
        for (int bit = 0; bit < BITS_PER_CODEWORD; ++bit) {
            int original_idx = cw * BITS_PER_CODEWORD + bit;
            int interleaved_idx = bit * NUM_CODEWORDS + (cw + bit) % NUM_CODEWORDS;

            interleave_table_[original_idx] = interleaved_idx;
            deinterleave_table_[interleaved_idx] = original_idx;
        }
    }

    tables_initialized_ = true;
}

std::vector<uint8_t> FrameInterleaver::interleave(
    const std::vector<std::vector<uint8_t>>& coded_codewords) {

    if (coded_codewords.size() != NUM_CODEWORDS) {
        throw std::invalid_argument("FrameInterleaver: expected 4 codewords, got " +
                                    std::to_string(coded_codewords.size()));
    }

    ensureTablesInitialized();

    // Convert all codewords to a single bit vector
    std::vector<uint8_t> original_bits(TOTAL_FRAME_BITS);
    size_t bit_idx = 0;

    for (int cw = 0; cw < NUM_CODEWORDS; ++cw) {
        const auto& cw_bytes = coded_codewords[cw];
        // Each coded codeword should be 81 bytes (648 bits)
        for (size_t byte_idx = 0; byte_idx < cw_bytes.size() && bit_idx < TOTAL_FRAME_BITS; ++byte_idx) {
            uint8_t byte = cw_bytes[byte_idx];
            for (int b = 7; b >= 0 && bit_idx < static_cast<size_t>((cw + 1) * BITS_PER_CODEWORD); --b) {
                original_bits[bit_idx++] = (byte >> b) & 1;
            }
        }
        // Ensure we hit exactly BITS_PER_CODEWORD bits per CW
        while (bit_idx < static_cast<size_t>((cw + 1) * BITS_PER_CODEWORD)) {
            original_bits[bit_idx++] = 0;  // Zero-pad if short
        }
    }

    // Apply interleaving permutation
    std::vector<uint8_t> interleaved_bits(TOTAL_FRAME_BITS);
    for (int i = 0; i < TOTAL_FRAME_BITS; ++i) {
        interleaved_bits[interleave_table_[i]] = original_bits[i];
    }

    // Convert back to bytes
    std::vector<uint8_t> output((TOTAL_FRAME_BITS + 7) / 8);
    for (int i = 0; i < TOTAL_FRAME_BITS; ++i) {
        if (interleaved_bits[i]) {
            output[i / 8] |= (1 << (7 - (i % 8)));
        }
    }

    return output;
}

std::vector<std::vector<float>> FrameInterleaver::deinterleave(
    const std::vector<float>& interleaved_soft) {

    if (interleaved_soft.size() < TOTAL_FRAME_BITS) {
        throw std::invalid_argument("FrameInterleaver: expected " +
                                    std::to_string(TOTAL_FRAME_BITS) +
                                    " soft bits, got " +
                                    std::to_string(interleaved_soft.size()));
    }

    ensureTablesInitialized();

    // Apply deinterleaving permutation
    std::vector<float> original_soft(TOTAL_FRAME_BITS);
    for (int i = 0; i < TOTAL_FRAME_BITS; ++i) {
        original_soft[deinterleave_table_[i]] = interleaved_soft[i];
    }

    // Split into 4 codewords
    std::vector<std::vector<float>> result(NUM_CODEWORDS);
    for (int cw = 0; cw < NUM_CODEWORDS; ++cw) {
        result[cw].resize(BITS_PER_CODEWORD);
        for (int bit = 0; bit < BITS_PER_CODEWORD; ++bit) {
            result[cw][bit] = original_soft[cw * BITS_PER_CODEWORD + bit];
        }
    }

    return result;
}

std::vector<float> FrameInterleaver::interleaveSoft(
    const std::vector<std::vector<float>>& soft_codewords) {

    if (soft_codewords.size() != NUM_CODEWORDS) {
        throw std::invalid_argument("FrameInterleaver: expected 4 codewords");
    }

    ensureTablesInitialized();

    // Flatten to single vector
    std::vector<float> original_soft(TOTAL_FRAME_BITS);
    for (int cw = 0; cw < NUM_CODEWORDS; ++cw) {
        for (int bit = 0; bit < BITS_PER_CODEWORD && bit < static_cast<int>(soft_codewords[cw].size()); ++bit) {
            original_soft[cw * BITS_PER_CODEWORD + bit] = soft_codewords[cw][bit];
        }
    }

    // Apply interleaving
    std::vector<float> interleaved(TOTAL_FRAME_BITS);
    for (int i = 0; i < TOTAL_FRAME_BITS; ++i) {
        interleaved[interleave_table_[i]] = original_soft[i];
    }

    return interleaved;
}

}  // namespace fec
}  // namespace ultra
