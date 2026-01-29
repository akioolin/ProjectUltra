#include "ldpc_codec.hpp"
#include <stdexcept>

namespace ultra {
namespace fec {

// Info bits per code rate (matches ldpc_encoder.cpp)
size_t LDPCCodec::getInfoBitsForRate(CodeRate rate) {
    switch (rate) {
        case CodeRate::R1_4: return 162;
        case CodeRate::R1_2: return 324;
        case CodeRate::R2_3: return 432;
        case CodeRate::R3_4: return 486;
        case CodeRate::R5_6: return 540;
        default: return 324;
    }
}

LDPCCodec::LDPCCodec(CodeRate rate)
    : rate_(rate)
    , max_iterations_(50)
{
    encoder_ = std::make_unique<LDPCEncoder>(rate);
    decoder_ = std::make_unique<LDPCDecoder>(rate);
    decoder_->setMaxIterations(max_iterations_);
}

LDPCCodec::~LDPCCodec() = default;

LDPCCodec::LDPCCodec(LDPCCodec&&) noexcept = default;
LDPCCodec& LDPCCodec::operator=(LDPCCodec&&) noexcept = default;

// ============================================================================
// Configuration
// ============================================================================

void LDPCCodec::setRate(CodeRate rate) {
    rate_ = rate;
    encoder_->setRate(rate);
    decoder_->setRate(rate);
}

CodeRate LDPCCodec::getRate() const {
    return rate_;
}

void LDPCCodec::setMaxIterations(int iterations) {
    max_iterations_ = iterations;
    decoder_->setMaxIterations(iterations);
}

int LDPCCodec::getMaxIterations() const {
    return max_iterations_;
}

// ============================================================================
// Encoding
// ============================================================================

Bytes LDPCCodec::encode(const Bytes& data) {
    ByteSpan span(data.data(), data.size());
    return encoder_->encode(span);
}

// ============================================================================
// Decoding
// ============================================================================

std::pair<bool, Bytes> LDPCCodec::decode(const std::vector<float>& soft_bits) {
    std::span<const float> span(soft_bits.data(), soft_bits.size());
    Bytes decoded = decoder_->decodeSoft(span);
    bool success = decoder_->lastDecodeSuccess();
    return {success, decoded};
}

DecodeResult LDPCCodec::decodeExtended(const std::vector<float>& soft_bits) {
    DecodeResult result;

    std::span<const float> span(soft_bits.data(), soft_bits.size());
    result.data = decoder_->decodeSoft(span);
    result.success = decoder_->lastDecodeSuccess();
    result.iterations = decoder_->lastIterations();

    // Estimate BER from iterations (heuristic)
    // More iterations typically means more errors
    if (result.success) {
        result.ber_estimate = static_cast<float>(result.iterations) / (max_iterations_ * 10.0f);
    } else {
        result.ber_estimate = 0.5f;  // Failed decode = high BER
    }

    return result;
}

// ============================================================================
// Parameters
// ============================================================================

size_t LDPCCodec::getInfoBits() const {
    return getInfoBitsForRate(rate_);
}

size_t LDPCCodec::getParityBits() const {
    return CODEWORD_BITS - getInfoBits();
}

size_t LDPCCodec::getDataBytes() const {
    return getInfoBits() / 8;
}

float LDPCCodec::getEffectiveRate() const {
    return static_cast<float>(getInfoBits()) / static_cast<float>(CODEWORD_BITS);
}

} // namespace fec
} // namespace ultra
