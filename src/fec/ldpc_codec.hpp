#pragma once

// LDPCCodec - ICodec wrapper for existing LDPC encoder/decoder
//
// This wrapper allows the existing LDPC implementation to be used through
// the ICodec interface, enabling future codec swapping without code changes.
//
// Performance with MC-DPSK (8 carriers, R1/4):
//   AWGN:        100% down to -7 dB
//   Good HF:     100% down to -3 dB
//   Moderate HF: 100% at 3 dB, 60% at 0 dB
//   Poor HF:     100% at 0 dB, 60% at -3 dB
//
// For SNR < 0 dB on fading channels, consider convolutional codes.
// See ldpc_codec.cpp for detailed test results.

#include "codec_interface.hpp"
#include "ultra/fec.hpp"
#include <memory>

namespace ultra {
namespace fec {

class LDPCCodec : public ICodec {
public:
    explicit LDPCCodec(CodeRate rate = CodeRate::R1_2);
    ~LDPCCodec() override;

    // Prevent copying (owns encoder/decoder)
    LDPCCodec(const LDPCCodec&) = delete;
    LDPCCodec& operator=(const LDPCCodec&) = delete;

    // Allow moving
    LDPCCodec(LDPCCodec&&) noexcept;
    LDPCCodec& operator=(LDPCCodec&&) noexcept;

    // ========================================================================
    // ICodec - Identity
    // ========================================================================

    std::string getName() const override { return "802.11n LDPC"; }

    // ========================================================================
    // ICodec - Configuration
    // ========================================================================

    void setRate(CodeRate rate) override;
    CodeRate getRate() const override;

    void setMaxIterations(int iterations) override;
    int getMaxIterations() const override;

    // ========================================================================
    // ICodec - Encoding
    // ========================================================================

    Bytes encode(const Bytes& data) override;

    // ========================================================================
    // ICodec - Decoding
    // ========================================================================

    std::pair<bool, Bytes> decode(const std::vector<float>& soft_bits) override;
    DecodeResult decodeExtended(const std::vector<float>& soft_bits) override;

    // ========================================================================
    // ICodec - Parameters
    // ========================================================================

    size_t getCodewordBits() const override { return CODEWORD_BITS; }
    size_t getInfoBits() const override;
    size_t getParityBits() const override;
    size_t getCodewordBytes() const override { return CODEWORD_BYTES; }
    size_t getDataBytes() const override;
    float getEffectiveRate() const override;

    // ========================================================================
    // LDPC-specific constants
    // ========================================================================

    static constexpr size_t CODEWORD_BITS = 648;
    static constexpr size_t CODEWORD_BYTES = 81;

    // Get recommended max iterations for a code rate
    // Higher rates (less redundancy) need more iterations to converge
    static int getRecommendedIterations(CodeRate rate) {
        switch (rate) {
            case CodeRate::R3_4: return 60;  // Least redundancy, most iterations
            case CodeRate::R2_3: return 70;
            case CodeRate::R1_2: return 80;  // Fading + R1/2 needs most
            case CodeRate::R1_3: return 60;
            case CodeRate::R1_4: return 50;  // Standard iterations (retry with LLR scaling if needed)
            default:            return 50;
        }
    }

private:
    std::unique_ptr<LDPCEncoder> encoder_;
    std::unique_ptr<LDPCDecoder> decoder_;
    CodeRate rate_;
    int max_iterations_ = 50;

    // Info bits per rate (from LDPC implementation)
    static size_t getInfoBitsForRate(CodeRate rate);
};

} // namespace fec
} // namespace ultra
