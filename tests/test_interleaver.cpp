#include "ultra/fec.hpp"
#include "fec/burst_interleaver.hpp"
#include "fec/frame_interleaver.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <random>
#include <stdexcept>

using namespace ultra;

// Generate random bytes for testing
Bytes generateRandomBytes(size_t count, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    Bytes data(count);
    for (size_t i = 0; i < count; ++i) {
        data[i] = static_cast<uint8_t>(rng() & 0xFF);
    }
    return data;
}

// Test 1: Round-trip - interleave then deinterleave should return original
bool testRoundTrip() {
    std::cout << "Test 1: Round-trip (bytes)..." << std::flush;

    Interleaver interleaver(32, 32);  // 32x32 = 1024 bits = 128 bytes

    // Test with exactly one block
    Bytes original = generateRandomBytes(128);

    Bytes interleaved = interleaver.interleave(original);

    // Interleaved should be different from original
    bool is_shuffled = false;
    for (size_t i = 0; i < original.size(); ++i) {
        if (interleaved[i] != original[i]) {
            is_shuffled = true;
            break;
        }
    }
    if (!is_shuffled) {
        std::cout << " FAILED (not shuffled)\n";
        return false;
    }

    Bytes recovered = interleaver.deinterleave(interleaved);

    // Recovered should match original
    for (size_t i = 0; i < original.size(); ++i) {
        if (recovered[i] != original[i]) {
            std::cout << " FAILED at byte " << i << "\n";
            return false;
        }
    }

    std::cout << " OK\n";
    return true;
}

// Test 2: Soft bits round-trip
bool testSoftBitsRoundTrip() {
    std::cout << "Test 2: Round-trip (soft bits)..." << std::flush;

    Interleaver interleaver(32, 32);  // 32x32 = 1024 soft bits

    // Generate soft bits (LLRs)
    std::vector<float> original(1024);
    for (int i = 0; i < 1024; ++i) {
        original[i] = (i - 512) * 0.01f;  // Range: -5.12 to +5.11
    }

    auto interleaved = interleaver.interleave(std::span<const float>(original));

    // Check it's shuffled
    bool is_shuffled = false;
    for (size_t i = 0; i < original.size(); ++i) {
        if (std::abs(interleaved[i] - original[i]) > 1e-6f) {
            is_shuffled = true;
            break;
        }
    }
    if (!is_shuffled) {
        std::cout << " FAILED (not shuffled)\n";
        return false;
    }

    auto recovered = interleaver.deinterleave(std::span<const float>(interleaved));

    // Check exact match
    for (size_t i = 0; i < original.size(); ++i) {
        if (std::abs(recovered[i] - original[i]) > 1e-6f) {
            std::cout << " FAILED at position " << i
                      << " (expected " << original[i] << ", got " << recovered[i] << ")\n";
            return false;
        }
    }

    std::cout << " OK\n";
    return true;
}

// Test 3: Verify burst error spreading
bool testBurstErrorSpreading() {
    std::cout << "Test 3: Burst error spreading..." << std::flush;

    Interleaver interleaver(32, 32);

    // Simulate soft bits with a burst error at the start
    // All "good" bits are +5.0, burst error bits are -5.0
    std::vector<float> with_burst(1024, 5.0f);

    // Introduce a 32-bit burst error (positions 0-31)
    for (int i = 0; i < 32; ++i) {
        with_burst[i] = -5.0f;
    }

    // Deinterleave (this is what RX does - errors come in, get spread out)
    auto spread = interleaver.deinterleave(std::span<const float>(with_burst));

    // Count gaps between errors
    // With 32x32 interleaver, errors at positions 0,1,2,...,31 (column 0)
    // After deinterleave, they should be at positions 0,32,64,96,... (every 32 positions)
    std::vector<int> error_positions;
    for (int i = 0; i < 1024; ++i) {
        if (spread[i] < 0) {
            error_positions.push_back(i);
        }
    }

    if (error_positions.size() != 32) {
        std::cout << " FAILED (expected 32 errors, got " << error_positions.size() << ")\n";
        return false;
    }

    // Check that errors are spread apart (minimum gap should be ~32)
    int min_gap = 1024;
    for (size_t i = 1; i < error_positions.size(); ++i) {
        int gap = error_positions[i] - error_positions[i-1];
        if (gap < min_gap) min_gap = gap;
    }

    // With proper interleaving, errors should be 32 positions apart
    if (min_gap < 30) {  // Allow some tolerance
        std::cout << " FAILED (min gap = " << min_gap << ", expected >= 30)\n";
        return false;
    }

    std::cout << " OK (min gap = " << min_gap << ")\n";
    return true;
}

// Test 4: Multiple block sizes
bool testDifferentSizes() {
    std::cout << "Test 4: Different interleaver sizes..." << std::flush;

    struct TestCase {
        int rows, cols;
    };

    TestCase cases[] = {
        {8, 8},    // 64 bits = 8 bytes
        {16, 16},  // 256 bits = 32 bytes
        {32, 32},  // 1024 bits = 128 bytes
        {16, 32},  // 512 bits = 64 bytes (non-square)
    };

    for (const auto& tc : cases) {
        Interleaver interleaver(tc.rows, tc.cols);
        int block_bytes = (tc.rows * tc.cols) / 8;

        Bytes original = generateRandomBytes(block_bytes);
        Bytes interleaved = interleaver.interleave(original);
        Bytes recovered = interleaver.deinterleave(interleaved);

        for (size_t i = 0; i < original.size(); ++i) {
            if (recovered[i] != original[i]) {
                std::cout << " FAILED (" << tc.rows << "x" << tc.cols << ")\n";
                return false;
            }
        }
    }

    std::cout << " OK\n";
    return true;
}

bool testFrameInterleaverSoftRoundTrip() {
    std::cout << "Test 5: Frame interleaver soft-bit round-trip..." << std::flush;

    std::vector<std::vector<float>> original(
        ultra::fec::FrameInterleaver::NUM_CODEWORDS,
        std::vector<float>(ultra::fec::FrameInterleaver::BITS_PER_CODEWORD));

    for (int cw = 0; cw < ultra::fec::FrameInterleaver::NUM_CODEWORDS; ++cw) {
        for (int bit = 0; bit < ultra::fec::FrameInterleaver::BITS_PER_CODEWORD; ++bit) {
            original[cw][bit] = static_cast<float>(cw * 1000 + bit) * 0.125f;
        }
    }

    auto interleaved = ultra::fec::FrameInterleaver::interleaveSoft(original);
    auto recovered = ultra::fec::FrameInterleaver::deinterleave(interleaved);

    if (recovered.size() != original.size()) {
        std::cout << " FAILED (wrong codeword count)\n";
        return false;
    }

    for (size_t cw = 0; cw < original.size(); ++cw) {
        if (recovered[cw].size() != original[cw].size()) {
            std::cout << " FAILED (wrong CW size)\n";
            return false;
        }
        for (size_t bit = 0; bit < original[cw].size(); ++bit) {
            if (std::abs(recovered[cw][bit] - original[cw][bit]) > 1e-6f) {
                std::cout << " FAILED at cw=" << cw << " bit=" << bit << "\n";
                return false;
            }
        }
    }

    std::cout << " OK\n";
    return true;
}

bool testFrameInterleaverByteMapping() {
    std::cout << "Test 6: Frame interleaver byte-to-soft inverse..." << std::flush;

    std::vector<std::vector<uint8_t>> codewords(
        ultra::fec::FrameInterleaver::NUM_CODEWORDS,
        std::vector<uint8_t>(ultra::fec::FrameInterleaver::BITS_PER_CODEWORD / 8));

    for (size_t cw = 0; cw < codewords.size(); ++cw) {
        for (size_t byte = 0; byte < codewords[cw].size(); ++byte) {
            codewords[cw][byte] = static_cast<uint8_t>((cw * 53 + byte * 17 + 0x39) & 0xFF);
        }
    }

    auto interleaved_bytes = ultra::fec::FrameInterleaver::interleave(codewords);
    if (interleaved_bytes.size() != ultra::fec::FrameInterleaver::TOTAL_FRAME_BITS / 8) {
        std::cout << " FAILED (wrong interleaved byte count)\n";
        return false;
    }

    std::vector<float> interleaved_soft;
    interleaved_soft.reserve(ultra::fec::FrameInterleaver::TOTAL_FRAME_BITS);
    for (uint8_t byte : interleaved_bytes) {
        for (int bit = 7; bit >= 0; --bit) {
            interleaved_soft.push_back(static_cast<float>((byte >> bit) & 1));
        }
    }

    auto recovered = ultra::fec::FrameInterleaver::deinterleave(interleaved_soft);
    for (size_t cw = 0; cw < codewords.size(); ++cw) {
        for (size_t byte = 0; byte < codewords[cw].size(); ++byte) {
            for (int bit = 0; bit < 8; ++bit) {
                bool expected = ((codewords[cw][byte] >> (7 - bit)) & 1) != 0;
                bool actual = recovered[cw][byte * 8 + bit] > 0.5f;
                if (actual != expected) {
                    std::cout << " FAILED at cw=" << cw
                              << " byte=" << byte << " bit=" << bit << "\n";
                    return false;
                }
            }
        }
    }

    std::cout << " OK\n";
    return true;
}

bool testBurstInterleaverRoundTrip() {
    std::cout << "Test 7: Burst interleaver byte-to-soft inverse..." << std::flush;

    constexpr int frame_count = 4;
    std::vector<std::vector<uint8_t>> logical(
        frame_count,
        std::vector<uint8_t>(ultra::fec::BurstInterleaver::BYTES_PER_FRAME));

    for (int frame = 0; frame < frame_count; ++frame) {
        for (int byte = 0; byte < ultra::fec::BurstInterleaver::BYTES_PER_FRAME; ++byte) {
            logical[frame][byte] = static_cast<uint8_t>((frame * 71 + byte * 29 + 0x11) & 0xFF);
        }
    }

    auto physical = ultra::fec::BurstInterleaver::interleave(logical);
    if (physical.size() != logical.size()) {
        std::cout << " FAILED (wrong physical frame count)\n";
        return false;
    }

    std::vector<std::vector<float>> physical_soft(
        frame_count,
        std::vector<float>(ultra::fec::BurstInterleaver::BITS_PER_FRAME));

    for (int frame = 0; frame < frame_count; ++frame) {
        for (int byte = 0; byte < ultra::fec::BurstInterleaver::BYTES_PER_FRAME; ++byte) {
            for (int bit = 0; bit < 8; ++bit) {
                physical_soft[frame][byte * 8 + bit] =
                    static_cast<float>((physical[frame][byte] >> (7 - bit)) & 1);
            }
        }
    }

    auto recovered = ultra::fec::BurstInterleaver::deinterleave(physical_soft);
    for (int frame = 0; frame < frame_count; ++frame) {
        for (int byte = 0; byte < ultra::fec::BurstInterleaver::BYTES_PER_FRAME; ++byte) {
            for (int bit = 0; bit < 8; ++bit) {
                bool expected = ((logical[frame][byte] >> (7 - bit)) & 1) != 0;
                bool actual = recovered[frame][byte * 8 + bit] > 0.5f;
                if (actual != expected) {
                    std::cout << " FAILED at frame=" << frame
                              << " byte=" << byte << " bit=" << bit << "\n";
                    return false;
                }
            }
        }
    }

    std::cout << " OK\n";
    return true;
}

bool testFrameAndBurstInterleaverValidation() {
    std::cout << "Test 8: Frame/burst interleaver validation..." << std::flush;

    bool frame_threw = false;
    try {
        ultra::fec::FrameInterleaver::interleave({std::vector<uint8_t>(81)});
    } catch (const std::invalid_argument&) {
        frame_threw = true;
    }

    bool burst_threw = false;
    try {
        ultra::fec::BurstInterleaver::interleave({
            std::vector<uint8_t>(ultra::fec::BurstInterleaver::BYTES_PER_FRAME),
            std::vector<uint8_t>(ultra::fec::BurstInterleaver::BYTES_PER_FRAME - 1),
        });
    } catch (const std::invalid_argument&) {
        burst_threw = true;
    }

    if (!frame_threw || !burst_threw) {
        std::cout << " FAILED (expected invalid_argument)\n";
        return false;
    }

    std::cout << " OK\n";
    return true;
}

int main() {
    std::cout << "\nTesting Interleaver implementation...\n\n";

    int failures = 0;

    if (!testRoundTrip()) failures++;
    if (!testSoftBitsRoundTrip()) failures++;
    if (!testBurstErrorSpreading()) failures++;
    if (!testDifferentSizes()) failures++;
    if (!testFrameInterleaverSoftRoundTrip()) failures++;
    if (!testFrameInterleaverByteMapping()) failures++;
    if (!testBurstInterleaverRoundTrip()) failures++;
    if (!testFrameAndBurstInterleaverValidation()) failures++;

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "All interleaver tests passed!\n";
        return 0;
    } else {
        std::cout << failures << " test(s) FAILED\n";
        return 1;
    }
}
