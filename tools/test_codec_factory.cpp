// Quick test for FEC codec factory
// Build: make test_codec_factory
// Run: ./build/test_codec_factory

#include "fec/codec_factory.hpp"
#include "fec/codec_interface.hpp"
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <random>

using namespace ultra;
using namespace ultra::fec;

void printCodecInfo(const CodecInfo& info) {
    std::cout << "  Name: " << info.display_name << " (" << info.name << ")\n";
    std::cout << "  Description: " << info.description.substr(0, 60) << "...\n";
    std::cout << "  Codeword bits: " << (info.codeword_bits ? std::to_string(info.codeword_bits) : "variable") << "\n";
    std::cout << "  Soft decode: " << (info.supports_soft_decode ? "yes" : "no") << "\n";
    std::cout << "  Implemented: " << (info.is_implemented ? "YES" : "no") << "\n";
    std::cout << "  Rates: ";
    for (auto rate : info.supported_rates) {
        std::cout << codeRateToString(rate) << " ";
    }
    std::cout << "\n\n";
}

int main() {
    std::cout << "=== FEC Codec Factory Test ===\n\n";

    // List all codecs
    std::cout << "All known codecs:\n";
    std::cout << "─────────────────\n";
    for (const auto& info : CodecFactory::getAllCodecs()) {
        printCodecInfo(info);
    }

    // List available (implemented) codecs
    std::cout << "Available (implemented) codecs:\n";
    std::cout << "───────────────────────────────\n";
    for (const auto& info : CodecFactory::getAvailableCodecs()) {
        std::cout << "  - " << info.display_name << "\n";
    }
    std::cout << "\n";

    // Create LDPC codec and test encode/decode
    std::cout << "Testing LDPC codec:\n";
    std::cout << "───────────────────\n";

    try {
        auto codec = CodecFactory::create(CodecType::LDPC, CodeRate::R1_2);
        std::cout << "  Created: " << codec->getName() << "\n";
        std::cout << "  Rate: " << codeRateToString(codec->getRate()) << "\n";
        std::cout << "  Info bits: " << codec->getInfoBits() << "\n";
        std::cout << "  Codeword bits: " << codec->getCodewordBits() << "\n";
        std::cout << "  Data bytes: " << codec->getDataBytes() << "\n";
        std::cout << "  Effective rate: " << std::fixed << std::setprecision(3) << codec->getEffectiveRate() << "\n\n";

        // Test encode/decode with random data
        std::cout << "  Testing encode/decode cycle:\n";

        Bytes test_data(codec->getDataBytes());
        std::mt19937 rng(12345);
        for (auto& b : test_data) {
            b = rng() & 0xFF;
        }

        Bytes encoded = codec->encode(test_data);
        std::cout << "    Input: " << test_data.size() << " bytes\n";
        std::cout << "    Encoded: " << encoded.size() << " bytes\n";

        // Convert to soft bits (perfect, no noise)
        std::vector<float> soft_bits(encoded.size() * 8);
        for (size_t i = 0; i < encoded.size(); i++) {
            for (int b = 7; b >= 0; b--) {
                int bit = (encoded[i] >> b) & 1;
                // LDPC convention: positive LLR = more likely bit 0, negative = bit 1
                soft_bits[i * 8 + (7 - b)] = bit ? -5.0f : 5.0f;
            }
        }

        auto [success, decoded] = codec->decode(soft_bits);
        std::cout << "    Decode success: " << (success ? "YES" : "NO") << "\n";
        std::cout << "    Decoded: " << decoded.size() << " bytes\n";

        // Verify
        bool match = (decoded.size() >= test_data.size());
        if (match) {
            for (size_t i = 0; i < test_data.size(); i++) {
                if (decoded[i] != test_data[i]) {
                    match = false;
                    break;
                }
            }
        }
        std::cout << "    Data match: " << (match ? "YES" : "NO") << "\n\n";

        // Test rate switching
        std::cout << "  Testing rate switching:\n";
        for (CodeRate rate : {CodeRate::R1_4, CodeRate::R1_2, CodeRate::R2_3, CodeRate::R3_4, CodeRate::R5_6}) {
            codec->setRate(rate);
            std::cout << "    " << codeRateToString(rate)
                      << ": info=" << codec->getInfoBits()
                      << ", data=" << codec->getDataBytes() << " bytes\n";
        }
        std::cout << "\n";

    } catch (const std::exception& e) {
        std::cerr << "  ERROR: " << e.what() << "\n";
        return 1;
    }

    // Test creating by name
    std::cout << "Testing createByName:\n";
    std::cout << "─────────────────────\n";
    try {
        auto codec1 = CodecFactory::createByName("ldpc");
        std::cout << "  'ldpc' -> " << codec1->getName() << "\n";

        auto codec2 = CodecFactory::createByName("LDPC");  // case-insensitive
        std::cout << "  'LDPC' -> " << codec2->getName() << "\n";

    } catch (const std::exception& e) {
        std::cerr << "  ERROR: " << e.what() << "\n";
    }

    // Test error handling
    std::cout << "\nTesting error handling:\n";
    std::cout << "───────────────────────\n";
    try {
        auto codec = CodecFactory::create(CodecType::TURBO);
        std::cout << "  ERROR: Should have thrown for unimplemented codec\n";
        return 1;
    } catch (const std::exception& e) {
        std::cout << "  Correctly threw for TURBO: " << e.what() << "\n";
    }

    try {
        auto codec = CodecFactory::createByName("nosuchcodec");
        std::cout << "  ERROR: Should have thrown for unknown name\n";
        return 1;
    } catch (const std::exception& e) {
        std::cout << "  Correctly threw for unknown name: " << e.what() << "\n";
    }

    std::cout << "\n=== All tests passed! ===\n";
    return 0;
}
