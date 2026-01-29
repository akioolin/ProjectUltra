#include "codec_factory.hpp"
#include "ldpc_codec.hpp"
#include <algorithm>
#include <stdexcept>
#include <cctype>

namespace ultra {
namespace fec {

namespace {

// Convert string to lowercase for case-insensitive comparison
std::string toLower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

// All known codec definitions
const std::vector<CodecInfo>& getCodecRegistry() {
    static const std::vector<CodecInfo> registry = {
        {
            CodecType::LDPC,
            "ldpc",
            "802.11n LDPC",
            "Low-Density Parity-Check code from IEEE 802.11n/ac. "
            "648-bit codewords, excellent performance within 0.5dB of Shannon limit.",
            {CodeRate::R1_4, CodeRate::R1_2, CodeRate::R2_3, CodeRate::R3_4, CodeRate::R5_6},
            648,
            true,   // supports soft decode
            true    // implemented
        },
        {
            CodecType::LDPC_5G,
            "ldpc5g",
            "5G NR LDPC",
            "LDPC codes from 3GPP 5G NR standard. Variable block sizes, "
            "optimized for high throughput.",
            {CodeRate::R1_4, CodeRate::R1_3, CodeRate::R1_2, CodeRate::R2_3, CodeRate::R3_4, CodeRate::R5_6},
            0,      // variable codeword size
            true,
            false   // not implemented
        },
        {
            CodecType::CONVOLUTIONAL,
            "conv",
            "K=7 Convolutional",
            "Constraint length 7 convolutional code with Viterbi decoder. "
            "G1=0171, G2=0133. Good for streaming, lower latency than block codes.",
            {CodeRate::R1_2, CodeRate::R2_3, CodeRate::R3_4},
            0,      // streaming (no fixed block)
            true,
            false   // not implemented
        },
        {
            CodecType::TURBO,
            "turbo",
            "3GPP Turbo",
            "Parallel concatenated convolutional code from 3GPP LTE. "
            "Excellent performance at very low SNR, higher complexity.",
            {CodeRate::R1_3, CodeRate::R1_2, CodeRate::R2_3, CodeRate::R3_4},
            0,
            true,
            false   // not implemented
        },
        {
            CodecType::POLAR,
            "polar",
            "5G Polar",
            "Polar codes from 3GPP 5G NR for control channels. "
            "Theoretically optimal for short blocks, uses successive cancellation.",
            {CodeRate::R1_4, CodeRate::R1_2, CodeRate::R2_3, CodeRate::R3_4},
            0,
            true,
            false   // not implemented
        },
        {
            CodecType::REED_SOLOMON,
            "rs",
            "Reed-Solomon",
            "RS(255,223) byte-oriented code. Best as outer code in concatenated "
            "schemes for burst error correction.",
            {CodeRate::R1_2, CodeRate::R2_3, CodeRate::R3_4},
            0,
            false,  // hard decision only
            false   // not implemented
        },
    };
    return registry;
}

} // anonymous namespace

// ============================================================================
// Creation
// ============================================================================

CodecPtr CodecFactory::create(CodecType type, CodeRate rate) {
    switch (type) {
        case CodecType::LDPC:
            return std::make_unique<LDPCCodec>(rate);

        case CodecType::LDPC_5G:
        case CodecType::CONVOLUTIONAL:
        case CodecType::TURBO:
        case CodecType::POLAR:
        case CodecType::REED_SOLOMON:
            throw std::runtime_error("Codec type '" + typeToName(type) + "' is not yet implemented");

        default:
            throw std::runtime_error("Unknown codec type");
    }
}

CodecPtr CodecFactory::createByName(const std::string& name, CodeRate rate) {
    return create(nameToType(name), rate);
}

// ============================================================================
// Information
// ============================================================================

std::vector<CodecInfo> CodecFactory::getAllCodecs() {
    return getCodecRegistry();
}

std::vector<CodecInfo> CodecFactory::getAvailableCodecs() {
    std::vector<CodecInfo> available;
    for (const auto& info : getCodecRegistry()) {
        if (info.is_implemented) {
            available.push_back(info);
        }
    }
    return available;
}

CodecInfo CodecFactory::getCodecInfo(CodecType type) {
    for (const auto& info : getCodecRegistry()) {
        if (info.type == type) {
            return info;
        }
    }
    throw std::runtime_error("Unknown codec type");
}

bool CodecFactory::isImplemented(CodecType type) {
    for (const auto& info : getCodecRegistry()) {
        if (info.type == type) {
            return info.is_implemented;
        }
    }
    return false;
}

// ============================================================================
// Utility
// ============================================================================

std::string CodecFactory::typeToName(CodecType type) {
    for (const auto& info : getCodecRegistry()) {
        if (info.type == type) {
            return info.name;
        }
    }
    return "unknown";
}

CodecType CodecFactory::nameToType(const std::string& name) {
    std::string lower_name = toLower(name);

    for (const auto& info : getCodecRegistry()) {
        if (toLower(info.name) == lower_name) {
            return info.type;
        }
    }

    // Also check display names
    for (const auto& info : getCodecRegistry()) {
        if (toLower(info.display_name) == lower_name) {
            return info.type;
        }
    }

    throw std::runtime_error("Unknown codec name: " + name);
}

} // namespace fec
} // namespace ultra
