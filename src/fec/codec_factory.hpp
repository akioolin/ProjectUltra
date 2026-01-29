#pragma once

// CodecFactory - Factory for creating FEC codec instances
//
// Provides a central point for creating codec instances by type or name.
// This enables runtime codec selection via configuration without code changes.
//
// Usage:
//   auto codec = CodecFactory::create(CodecType::LDPC, CodeRate::R1_2);
//   auto codec = CodecFactory::createByName("ldpc", CodeRate::R1_4);

#include "codec_interface.hpp"
#include <string>
#include <vector>

namespace ultra {
namespace fec {

// Supported codec types
enum class CodecType {
    LDPC,           // 802.11n LDPC (default, implemented)
    LDPC_5G,        // 5G NR LDPC (future)
    CONVOLUTIONAL,  // K=7 convolutional + Viterbi (future)
    TURBO,          // 3GPP Turbo codes (future)
    POLAR,          // 5G Polar codes (future)
    REED_SOLOMON,   // RS for outer code in concatenated schemes (future)
};

// Information about a codec type
struct CodecInfo {
    CodecType type;
    std::string name;           // Short name for config (e.g., "ldpc")
    std::string display_name;   // Human-readable name
    std::string description;
    std::vector<CodeRate> supported_rates;
    size_t codeword_bits;       // Bits per codeword (0 = variable)
    bool supports_soft_decode;  // Can decode from LLRs
    bool is_implemented;        // Implementation exists
};

// Codec factory
class CodecFactory {
public:
    // ========================================================================
    // Creation
    // ========================================================================

    // Create a codec by type
    // Throws std::runtime_error if codec type is not implemented
    static CodecPtr create(CodecType type, CodeRate rate = CodeRate::R1_2);

    // Create a codec by name (case-insensitive)
    // Names: "ldpc", "conv", "turbo", "polar", "rs"
    // Throws std::runtime_error if name is not recognized or not implemented
    static CodecPtr createByName(const std::string& name, CodeRate rate = CodeRate::R1_2);

    // ========================================================================
    // Information
    // ========================================================================

    // Get list of all known codecs (including unimplemented)
    static std::vector<CodecInfo> getAllCodecs();

    // Get list of implemented codecs only
    static std::vector<CodecInfo> getAvailableCodecs();

    // Get info for a specific codec type
    static CodecInfo getCodecInfo(CodecType type);

    // Check if a codec type is implemented
    static bool isImplemented(CodecType type);

    // ========================================================================
    // Defaults
    // ========================================================================

    // Get default codec type
    static CodecType getDefaultType() { return CodecType::LDPC; }

    // Get default codec name
    static std::string getDefaultName() { return "ldpc"; }

    // ========================================================================
    // Utility
    // ========================================================================

    // Convert codec type to name
    static std::string typeToName(CodecType type);

    // Convert name to codec type (throws if not found)
    static CodecType nameToType(const std::string& name);
};

// Convenience function for creating default codec
inline CodecPtr createDefaultCodec(CodeRate rate = CodeRate::R1_2) {
    return CodecFactory::create(CodecFactory::getDefaultType(), rate);
}

} // namespace fec
} // namespace ultra
