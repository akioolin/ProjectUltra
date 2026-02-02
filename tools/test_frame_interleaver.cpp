// Test frame-level interleaving for fixed 4-CW frames
//
// Verifies that burst errors spread across all codewords after deinterleaving,
// allowing LDPC to correct what would otherwise be uncorrectable.

#include "protocol/frame_v2.hpp"
#include "../src/fec/frame_interleaver.hpp"
#include "ultra/fec.hpp"
#include <cstdio>
#include <cstring>
#include <random>
#include <cmath>

using namespace ultra;
using namespace ultra::protocol::v2;
using namespace ultra::fec;

// Simulate burst error by corrupting a contiguous range of bits
void applyBurstError(std::vector<float>& soft_bits, size_t start, size_t length) {
    for (size_t i = start; i < start + length && i < soft_bits.size(); ++i) {
        // Flip the soft bit (invert sign and reduce magnitude)
        soft_bits[i] = -soft_bits[i] * 0.3f;  // Weak wrong decision
    }
}

// Convert hard bits to soft bits (+1.0 for 0, -1.0 for 1)
std::vector<float> bytesToSoftBits(const std::vector<uint8_t>& bytes) {
    std::vector<float> soft;
    soft.reserve(bytes.size() * 8);
    for (uint8_t byte : bytes) {
        for (int b = 7; b >= 0; --b) {
            int bit = (byte >> b) & 1;
            soft.push_back(bit ? -1.0f : 1.0f);  // BPSK: 0→+1, 1→-1
        }
    }
    return soft;
}

// Add AWGN noise to soft bits
void addNoise(std::vector<float>& soft_bits, float snr_db, std::mt19937& rng) {
    float snr_linear = std::pow(10.0f, snr_db / 10.0f);
    float noise_std = 1.0f / std::sqrt(2.0f * snr_linear);
    std::normal_distribution<float> noise(0.0f, noise_std);

    for (float& s : soft_bits) {
        s += noise(rng);
    }
}

int main(int argc, char* argv[]) {
    printf("=== Frame Interleaver Test ===\n\n");

    CodeRate rate = CodeRate::R1_4;
    size_t bytes_per_cw = getBytesPerCodeword(rate);

    printf("Code rate: R1/4\n");
    printf("Bytes per CW: %zu\n", bytes_per_cw);
    printf("Fixed frame: 4 CWs × %zu bytes = %zu info bytes\n",
           bytes_per_cw, 4 * bytes_per_cw);
    printf("Payload capacity: %zu bytes\n\n", getFixedFramePayloadCapacity(rate));

    // Create test payload
    std::string test_msg = "Hello, this is a test message for frame interleaving!";
    Bytes payload(test_msg.begin(), test_msg.end());

    // Create fixed frame
    auto frame = makeFixedDataFrame("TEST1", "TEST2", 1, payload, rate);
    printf("Created frame: seq=%d, payload=%zu bytes, total_cw=%d\n",
           frame.seq, frame.payload.size(), frame.total_cw);

    // Serialize
    Bytes frame_data = frame.serialize();
    printf("Serialized: %zu bytes\n", frame_data.size());

    // Encode with interleaving
    Bytes interleaved = encodeFixedFrame(frame_data, rate);
    printf("Encoded + interleaved: %zu bytes (%zu bits)\n\n",
           interleaved.size(), interleaved.size() * 8);

    // Convert to soft bits
    std::vector<float> soft_bits = bytesToSoftBits(interleaved);
    printf("Soft bits: %zu\n", soft_bits.size());

    std::mt19937 rng(12345);

    // Test 1: No errors (baseline)
    printf("\n--- Test 1: No errors ---\n");
    {
        auto test_soft = soft_bits;
        auto status = decodeFixedFrame(test_soft, rate);

        int success = 0;
        for (int i = 0; i < 4; ++i) {
            if (status.decoded[i]) success++;
        }
        printf("Decoded: %d/4 CWs\n", success);

        if (status.allSuccess()) {
            Bytes reassembled = status.reassemble();
            auto decoded_frame = DataFrame::deserialize(reassembled);
            if (decoded_frame) {
                printf("Frame decoded: seq=%d, payload='%s'\n",
                       decoded_frame->seq, decoded_frame->payloadAsText().c_str());
            }
        }
    }

    // Test 2: Burst error WITHOUT interleaving (simulated)
    printf("\n--- Test 2: Burst error on CW1 (no interleave protection) ---\n");
    {
        // Encode WITHOUT interleaving to show the problem
        size_t total_info = 4 * bytes_per_cw;
        Bytes padded = frame_data;
        padded.resize(total_info, 0);

        LDPCEncoder encoder(rate);
        std::vector<std::vector<uint8_t>> coded_cws;
        for (int cw = 0; cw < 4; ++cw) {
            Bytes chunk(padded.begin() + cw * bytes_per_cw,
                       padded.begin() + (cw + 1) * bytes_per_cw);
            coded_cws.push_back(encoder.encode(chunk));
        }

        // Flatten (no interleaving)
        std::vector<float> flat_soft;
        for (auto& cw : coded_cws) {
            auto cw_soft = bytesToSoftBits(cw);
            flat_soft.insert(flat_soft.end(), cw_soft.begin(), cw_soft.end());
        }

        // Apply burst error to CW1 (bits 648-1000)
        applyBurstError(flat_soft, 648, 400);  // 400 bits = 62% of CW1

        // Decode each CW separately
        LDPCDecoder decoder(rate);
        int success = 0;
        for (int cw = 0; cw < 4; ++cw) {
            std::vector<float> cw_soft(flat_soft.begin() + cw * 648,
                                       flat_soft.begin() + (cw + 1) * 648);
            decoder.decodeSoft(cw_soft);
            if (decoder.lastDecodeSuccess()) success++;
        }
        printf("Decoded: %d/4 CWs (CW1 likely failed due to 62%% errors)\n", success);
    }

    // Test 3: Same burst error WITH interleaving
    printf("\n--- Test 3: Same burst error WITH interleaving ---\n");
    {
        auto test_soft = soft_bits;  // Already interleaved

        // Apply same burst error (bits 648-1000)
        applyBurstError(test_soft, 648, 400);

        // Decode with deinterleaving
        auto status = decodeFixedFrame(test_soft, rate);

        int success = 0;
        for (int i = 0; i < 4; ++i) {
            if (status.decoded[i]) success++;
        }
        printf("Decoded: %d/4 CWs (errors spread to ~15%% per CW)\n", success);

        if (status.allSuccess()) {
            Bytes reassembled = status.reassemble();
            auto decoded_frame = DataFrame::deserialize(reassembled);
            if (decoded_frame) {
                printf("Frame decoded: seq=%d, payload='%s'\n",
                       decoded_frame->seq, decoded_frame->payloadAsText().c_str());
                printf("SUCCESS: Interleaving saved the frame!\n");
            }
        }
    }

    // Test 4: Larger burst with noise
    printf("\n--- Test 4: 600-bit burst + 10dB noise WITH interleaving ---\n");
    {
        auto test_soft = soft_bits;

        // Add noise first
        addNoise(test_soft, 10.0f, rng);

        // Apply large burst (600 bits = 23% of total, spread to ~6% per CW)
        applyBurstError(test_soft, 500, 600);

        auto status = decodeFixedFrame(test_soft, rate);

        int success = 0;
        for (int i = 0; i < 4; ++i) {
            if (status.decoded[i]) success++;
        }
        printf("Decoded: %d/4 CWs\n", success);

        if (status.allSuccess()) {
            Bytes reassembled = status.reassemble();
            auto decoded_frame = DataFrame::deserialize(reassembled);
            if (decoded_frame) {
                printf("Frame decoded successfully!\n");
            }
        }
    }

    printf("\n=== Test Complete ===\n");
    return 0;
}
