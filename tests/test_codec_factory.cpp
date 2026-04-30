#include "fec/codec_factory.hpp"
#include "fec/ldpc_codec.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ultra;
using namespace ultra::fec;

namespace {

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg) \
    do { \
        ++tests_run; \
        if (!(cond)) { \
            ++tests_failed; \
            std::cout << "FAIL: " << msg << "\n"; \
            return; \
        } \
    } while (0)

template <typename Fn>
void checkThrows(Fn&& fn, const std::string& msg) {
    ++tests_run;
    try {
        fn();
        ++tests_failed;
        std::cout << "FAIL: " << msg << "\n";
    } catch (const std::runtime_error&) {
    }
}

std::vector<float> bytesToPerfectLlrs(const Bytes& encoded) {
    std::vector<float> llrs;
    llrs.reserve(encoded.size() * 8);
    for (uint8_t byte : encoded) {
        for (int bit = 7; bit >= 0; --bit) {
            bool one = ((byte >> bit) & 1) != 0;
            llrs.push_back(one ? -10.0f : 10.0f);
        }
    }
    return llrs;
}

Bytes makePayload(size_t size) {
    Bytes data(size);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 31u + 0xA5u) & 0xFFu);
    }
    return data;
}

void test_registry_and_lookup() {
    auto all = CodecFactory::getAllCodecs();
    auto available = CodecFactory::getAvailableCodecs();

    CHECK(all.size() == 6, "registry should expose all known codec entries");
    CHECK(available.size() == 1, "only LDPC should be implemented");
    CHECK(available[0].type == CodecType::LDPC, "available codec should be LDPC");
    CHECK(available[0].codeword_bits == 648, "LDPC codeword size should be 648 bits");
    CHECK(available[0].supports_soft_decode, "LDPC should advertise soft decode");

    CHECK(CodecFactory::getDefaultType() == CodecType::LDPC, "default codec type should be LDPC");
    CHECK(CodecFactory::getDefaultName() == "ldpc", "default codec name should be ldpc");
    CHECK(CodecFactory::typeToName(CodecType::LDPC) == "ldpc", "LDPC type name");
    CHECK(CodecFactory::typeToName(static_cast<CodecType>(999)) == "unknown",
          "unknown codec type should stringify as unknown");

    CHECK(CodecFactory::nameToType("LDPC") == CodecType::LDPC, "codec lookup should be case-insensitive");
    CHECK(CodecFactory::nameToType("802.11n ldpc") == CodecType::LDPC,
          "codec lookup should accept display name");
    CHECK(CodecFactory::isImplemented(CodecType::LDPC), "LDPC should be implemented");
    CHECK(!CodecFactory::isImplemented(CodecType::POLAR), "future codecs should not be implemented");
    CHECK(!CodecFactory::isImplemented(static_cast<CodecType>(999)),
          "unknown codec type should not be implemented");

    auto info = CodecFactory::getCodecInfo(CodecType::LDPC);
    CHECK(info.name == "ldpc", "getCodecInfo should return LDPC info");

    checkThrows([] { (void)CodecFactory::nameToType("does-not-exist"); },
                "unknown codec name should throw");
    checkThrows([] { (void)CodecFactory::getCodecInfo(static_cast<CodecType>(999)); },
                "unknown codec info should throw");
}

void test_factory_creation_failures() {
    auto codec = CodecFactory::create(CodecType::LDPC, CodeRate::R1_2);
    CHECK(static_cast<bool>(codec), "LDPC codec should be creatable by type");
    CHECK(codec->getName() == "802.11n LDPC", "LDPC codec name should be stable");

    auto by_name = CodecFactory::createByName("ldpc", CodeRate::R1_4);
    CHECK(by_name->getRate() == CodeRate::R1_4, "createByName should pass requested rate");

    auto def = createDefaultCodec(CodeRate::R2_3);
    CHECK(def->getRate() == CodeRate::R2_3, "createDefaultCodec should pass requested rate");

    checkThrows([] { (void)CodecFactory::create(CodecType::TURBO); },
                "unimplemented codec should throw");
    checkThrows([] { (void)CodecFactory::create(static_cast<CodecType>(999)); },
                "unknown codec create should throw");
    checkThrows([] { (void)CodecFactory::createByName("polar"); },
                "recognized but unimplemented codec name should throw");
}

void test_ldpc_codec_parameters_and_rate_changes() {
    LDPCCodec codec(CodeRate::R1_2);

    CHECK(codec.getCodewordBits() == 648, "LDPC codeword bits");
    CHECK(codec.getCodewordBytes() == 81, "LDPC codeword bytes");
    CHECK(codec.getInfoBits() == 324, "R1/2 info bits");
    CHECK(codec.getParityBits() == 324, "R1/2 parity bits");
    CHECK(codec.getDataBytes() == 40, "R1/2 data bytes should floor to full bytes");
    CHECK(std::abs(codec.getEffectiveRate() - 0.5f) < 1e-6f, "R1/2 effective rate");
    CHECK(codec.getMaxIterations() == LDPCCodec::getRecommendedIterations(CodeRate::R1_2),
          "constructor should use recommended iterations");

    codec.setMaxIterations(7);
    CHECK(codec.getMaxIterations() == 7, "manual max iterations should be stored");

    codec.setRate(CodeRate::R3_4);
    CHECK(codec.getRate() == CodeRate::R3_4, "setRate should update rate");
    CHECK(codec.getInfoBits() == 486, "R3/4 info bits");
    CHECK(codec.getDataBytes() == 60, "R3/4 data bytes");
    CHECK(codec.getMaxIterations() == LDPCCodec::getRecommendedIterations(CodeRate::R3_4),
          "setRate should restore recommended iterations");

    codec.setRate(CodeRate::R1_4);
    CHECK(codec.getInfoBits() == 162, "R1/4 info bits");
    CHECK(codec.getDataBytes() == 20, "R1/4 data bytes");
}

void test_ldpc_codec_round_trip() {
    for (CodeRate rate : {CodeRate::R1_4, CodeRate::R1_2, CodeRate::R2_3, CodeRate::R3_4}) {
        LDPCCodec codec(rate);
        Bytes payload = makePayload(codec.getDataBytes());
        Bytes encoded = codec.encode(payload);
        CHECK(encoded.size() == codec.getCodewordBytes(), "encoded LDPC codeword byte size");

        auto llrs = bytesToPerfectLlrs(encoded);
        auto [success, decoded] = codec.decode(llrs);
        CHECK(success, "perfect LDPC LLR decode should succeed");
        CHECK(decoded.size() >= payload.size(), "decoded payload should include original bytes");
        decoded.resize(payload.size());
        CHECK(decoded == payload, "LDPC codec payload should round-trip");

        auto extended = codec.decodeExtended(llrs);
        CHECK(extended.success, "extended decode should report success");
        CHECK(extended.iterations >= 0, "extended decode should report non-negative iterations");
        CHECK(extended.ber_estimate >= 0.0f && extended.ber_estimate <= 0.5f,
              "extended decode BER estimate should be bounded");
    }
}

void test_ldpc_codec_failure_observability() {
    LDPCCodec codec(CodeRate::R1_2);
    std::vector<float> neutral(codec.getCodewordBits(), 0.0f);

    auto result = codec.decodeExtended(neutral);
    CHECK(result.data.size() <= codec.getDataBytes() + 1,
          "neutral decode should stay within one data codeword");
    CHECK(result.ber_estimate >= 0.0f && result.ber_estimate <= 0.5f,
          "neutral decode BER estimate should be bounded");

    std::vector<float> too_short(10, 1.0f);
    auto short_result = codec.decodeExtended(too_short);
    CHECK(!short_result.success, "short LLR vector should fail decode");
    CHECK(short_result.ber_estimate == 0.5f, "failed decode should report high BER estimate");
}

}  // namespace

int main() {
    test_registry_and_lookup();
    test_factory_creation_failures();
    test_ldpc_codec_parameters_and_rate_changes();
    test_ldpc_codec_round_trip();
    test_ldpc_codec_failure_observability();

    if (tests_failed != 0) {
        std::cout << "CodecFactory: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "CodecFactory: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
