#include <iostream>
#include <cassert>
#include <cstring>
#include <cmath>
#include <initializer_list>
#include <utility>
#include <vector>
#include "../src/protocol/frame_v2.hpp"

using namespace ultra::protocol;
using namespace ultra::protocol::v2;
using ultra::Bytes;
using ultra::CodeRate;
using ultra::Modulation;

int tests_passed = 0;
int tests_failed = 0;

#define TEST(name) \
    std::cout << "Testing " << name << "... "; \
    try

#define PASS() \
    std::cout << "PASS\n"; \
    tests_passed++;

#define FAIL(msg) \
    std::cout << "FAIL: " << msg << "\n"; \
    tests_failed++;

static std::vector<float> bytesToSoftBits(const Bytes& encoded) {
    std::vector<float> soft_bits;
    soft_bits.reserve(encoded.size() * 8);
    for (uint8_t byte : encoded) {
        for (int b = 7; b >= 0; --b) {
            int bit = (byte >> b) & 1;
            soft_bits.push_back(bit ? -5.0f : 5.0f);
        }
    }
    return soft_bits;
}

static uint64_t activeMaskWithErased(std::initializer_list<uint8_t> erased_carriers) {
    uint64_t active = PHYMaskHeader::ACTIVE_CARRIER_MASK;
    for (uint8_t carrier : erased_carriers) {
        active &= ~(uint64_t{1} << carrier);
    }
    return active;
}

static PHYMaskHeader makeValidPHYMaskHeader(uint64_t active_mask, uint8_t mask_count) {
    PHYMaskHeader h;
    h.payload_profile = PHYMaskHeader::packPayloadProfile(4, 1, 1);
    h.mask_count = mask_count;
    h.active_mask = active_mask;
    return h;
}

void test_callsign_hashing() {
    TEST("callsign hashing") {
        // Hash should be deterministic
        uint32_t h1 = hashCallsign("VA2MVR");
        uint32_t h2 = hashCallsign("VA2MVR");
        assert(h1 == h2);

        // Case insensitive
        uint32_t h3 = hashCallsign("va2mvr");
        assert(h1 == h3);

        // Different callsigns should (usually) have different hashes
        uint32_t h4 = hashCallsign("W1AW");
        assert(h1 != h4);

        // Hash should be 24 bits
        assert(h1 <= 0xFFFFFF);
        assert(h4 <= 0xFFFFFF);

        // Test broadcast hash
        uint32_t hb = hashCallsign("CQ");
        assert(hb <= 0xFFFFFF);

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_callsign_validation_and_ping() {
    TEST("callsign validation and ping frame") {
        assert(sanitizeCallsign("va2mvr/p!?") == "VA2MVR/P");
        assert(sanitizeCallsign("w1aw") == "W1AW");
        assert(isValidCallsign("W1AW"));
        assert(isValidCallsign("VA2MVR/P"));
        assert(!isValidCallsign("AB"));
        assert(!isValidCallsign("TOO-LONG-CALL"));
        assert(!isValidCallsign("BAD?"));

        auto ping = PingFrame::serialize();
        assert(ping.size() == PingFrame::SIZE);
        assert(PingFrame::isPing(ping));
        assert(PingFrame::isPing(ping.data(), ping.size()));

        Bytes short_ping = {0x55, 0x4C, 0x54};
        assert(!PingFrame::isPing(short_ping));

        auto corrupt_ping = ping;
        corrupt_ping[3] ^= 0x01;
        assert(!PingFrame::isPing(corrupt_ping));

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_channel_report_and_quantizers() {
    TEST("channel report and scalar quantizers") {
        ChannelReport report;
        report.snr_db = 12.3f;
        report.delay_spread_ms = 2.4f;
        report.doppler_spread_hz = 1.2f;
        report.recommended_mode = WaveformMode::OFDM_CHIRP;
        report.capabilities = ModeCapabilities::OFDM_CHIRP | ModeCapabilities::MC_DPSK;

        auto encoded = report.encode();
        assert(encoded.size() == 5);
        auto decoded = ChannelReport::decode(encoded);
        assert(std::abs(decoded.snr_db - 12.2f) < 0.001f);
        assert(std::abs(decoded.delay_spread_ms - 2.4f) < 0.001f);
        assert(std::abs(decoded.doppler_spread_hz - 1.2f) < 0.001f);
        assert(decoded.recommended_mode == WaveformMode::OFDM_CHIRP);
        assert(decoded.capabilities == report.capabilities);

        ChannelReport excellent{35.0f, 0.5f, 0.5f, WaveformMode::OFDM_CHIRP, ModeCapabilities::ALL};
        ChannelReport good{28.0f, 1.5f, 1.5f, WaveformMode::OFDM_CHIRP, ModeCapabilities::ALL};
        ChannelReport moderate{20.0f, 5.0f, 5.0f, WaveformMode::OFDM_CHIRP, ModeCapabilities::ALL};
        ChannelReport poor{13.0f, 5.0f, 5.0f, WaveformMode::MC_DPSK, ModeCapabilities::ALL};
        ChannelReport flutter{0.0f, 5.0f, 5.0f, WaveformMode::MC_DPSK, ModeCapabilities::ALL};
        assert(std::strcmp(excellent.getConditionName(), "Excellent") == 0);
        assert(std::strcmp(good.getConditionName(), "Good") == 0);
        assert(std::strcmp(moderate.getConditionName(), "Moderate") == 0);
        assert(std::strcmp(poor.getConditionName(), "Poor") == 0);
        assert(std::strcmp(flutter.getConditionName(), "Flutter") == 0);

        assert(encodeSNR(-99.0f) == 0);
        assert(std::abs(decodeSNR(0) + 10.0f) < 0.001f);
        assert(encodeSNR(99.0f) == 255);
        assert(std::abs(decodeSNR(encodeSNR(15.25f)) - 15.25f) < 0.001f);

        assert(encodeFadingIndex(-0.1f) == 0);
        assert(decodeFadingIndex(0) < 0.0f);
        assert(std::abs(decodeFadingIndex(encodeFadingIndex(0.62f)) - 0.62f) < 0.001f);
        assert(encodeFadingIndex(99.0f) == 255);
        assert(std::abs(decodeFadingIndex(255) - 2.54f) < 0.001f);

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_control_frame_size() {
    TEST("control frame size = 20 bytes") {
        auto probe = ControlFrame::makeProbe("VA2MVR", "W1AW");
        auto serialized = probe.serialize();

        // Must be exactly 20 bytes (1 codeword)
        assert(serialized.size() == 20);
        assert(serialized.size() == ControlFrame::SIZE);
        assert(serialized.size() == BYTES_PER_CODEWORD);

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_control_frame_roundtrip() {
    TEST("control frame serialize/deserialize roundtrip") {
        auto original = ControlFrame::makeProbe("VA2MVR", "W1AW");
        original.seq = 1234;

        auto serialized = original.serialize();
        auto parsed = ControlFrame::deserialize(serialized);

        assert(parsed.has_value());
        assert(parsed->type == original.type);
        assert(parsed->flags == original.flags);
        assert(parsed->seq == original.seq);
        assert(parsed->src_hash == original.src_hash);
        assert(parsed->dst_hash == original.dst_hash);

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_burst_header_roundtrip() {
    TEST("burst header descriptor roundtrip (declares group decode params)") {
        auto original = ControlFrame::makeBurstHeader(
            "VA2MVR", "W1AW", 7, /*group_size=*/8, /*cw_per_frame=*/8,
            Modulation::QPSK, CodeRate::R3_4,
            ControlFrame::BURST_FLAG_INTERLEAVE | ControlFrame::BURST_FLAG_CARRIER_LDPC);

        auto serialized = original.serialize();
        auto parsed = ControlFrame::deserialize(serialized);

        assert(parsed.has_value());
        assert(parsed->type == FrameType::BURST_HEADER);
        auto info = parsed->getBurstHeaderInfo();
        assert(info.group_size == 8);
        assert(info.cw_per_frame == 8);
        assert(info.modulation == Modulation::QPSK);
        assert(info.code_rate == CodeRate::R3_4);
        assert(info.burst_interleave == true);
        assert(info.carrier_ldpc == true);

        // Flags-off variant must read back false.
        auto plain = ControlFrame::makeBurstHeader("VA2MVR", "W1AW", 0, 4, 4,
                                                   Modulation::DQPSK, CodeRate::R1_2, 0);
        auto plain_info = ControlFrame::deserialize(plain.serialize())->getBurstHeaderInfo();
        assert(!plain_info.burst_interleave && !plain_info.carrier_ldpc);
        assert(plain_info.group_size == 4 && plain_info.cw_per_frame == 4);

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_control_frame_crc() {
    TEST("control frame CRC validation") {
        auto probe = ControlFrame::makeProbe("VA2MVR", "W1AW");
        auto serialized = probe.serialize();

        // Valid CRC should parse
        auto valid = ControlFrame::deserialize(serialized);
        assert(valid.has_value());

        // Corrupt a byte - should fail CRC
        serialized[10] ^= 0xFF;
        auto invalid = ControlFrame::deserialize(serialized);
        assert(!invalid.has_value());

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_control_frame_magic() {
    TEST("control frame magic = 0x554C (UL)") {
        auto probe = ControlFrame::makeProbe("VA2MVR", "W1AW");
        auto serialized = probe.serialize();

        // First two bytes should be "UL"
        assert(serialized[0] == 0x55);  // 'U'
        assert(serialized[1] == 0x4C);  // 'L'

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_phy_mask_header_roundtrip_patterns() {
    TEST("PHY mask header roundtrip patterns and byte layout") {
        const std::vector<std::pair<uint64_t, uint8_t>> cases = {
            {activeMaskWithErased({17}), 1},
            {activeMaskWithErased({0, 1, 2, 3, 4, 5, 6, 7}), 8},
            {activeMaskWithErased({0, 58}), 2},
        };

        for (const auto& [active_mask, mask_count] : cases) {
            PHYMaskHeader original = makeValidPHYMaskHeader(active_mask, mask_count);
            Bytes encoded = original.serialize();

            assert(encoded.size() == PHYMaskHeader::SIZE);
            assert(encoded[0] == 0x50);
            assert(encoded[1] == 0x4D);
            assert(encoded[2] == 0x11);
            assert(encoded[3] == 0);
            assert(encoded[4] == PHYMaskHeader::packPayloadProfile(4, 1, 1));
            assert(encoded[5] == PHYMaskHeader::INTERLEAVER_CARRIER_LDPC_V1);
            assert(encoded[6] == mask_count);
            assert(encoded[7] == 0);
            for (size_t i = 0; i < sizeof(active_mask); ++i) {
                assert(encoded[8 + i] == static_cast<uint8_t>((active_mask >> (8 * i)) & 0xFF));
            }

            uint16_t crc = ControlFrame::calculateCRC(encoded.data(), 16);
            uint16_t inv_crc = static_cast<uint16_t>(crc ^ 0xFFFF);
            assert(encoded[16] == static_cast<uint8_t>((crc >> 8) & 0xFF));
            assert(encoded[17] == static_cast<uint8_t>(crc & 0xFF));
            assert(encoded[18] == static_cast<uint8_t>((inv_crc >> 8) & 0xFF));
            assert(encoded[19] == static_cast<uint8_t>(inv_crc & 0xFF));
            assert(PHYMaskHeader::validate(encoded));

            auto decoded = PHYMaskHeader::deserialize(encoded);
            assert(decoded.has_value());
            assert(decoded->version == original.version);
            assert(decoded->scheme == original.scheme);
            assert(decoded->flags == original.flags);
            assert(decoded->payload_profile == original.payload_profile);
            assert(decoded->payloadCWCount() == 4);
            assert(decoded->payloadModId() == 1);
            assert(decoded->payloadRateId() == 1);
            assert(decoded->interleaver_id == original.interleaver_id);
            assert(decoded->mask_count == original.mask_count);
            assert(decoded->reserved == original.reserved);
            assert(decoded->active_mask == original.active_mask);
            assert(decoded->crc16 == crc);
            assert(decoded->inverted_crc16 == inv_crc);
        }

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_phy_mask_header_crc_tampering() {
    TEST("PHY mask header CRC tampering rejection") {
        PHYMaskHeader h = makeValidPHYMaskHeader(activeMaskWithErased({17}), 1);
        Bytes encoded = h.serialize();
        encoded[8] ^= 0x01;
        assert(!PHYMaskHeader::deserialize(encoded).has_value());
        assert(!PHYMaskHeader::validate(encoded));

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_phy_mask_header_inverted_crc_tampering() {
    TEST("PHY mask header inverted CRC tampering rejection") {
        PHYMaskHeader h = makeValidPHYMaskHeader(activeMaskWithErased({17}), 1);
        Bytes encoded = h.serialize();
        encoded[18] ^= 0x01;
        assert(!PHYMaskHeader::deserialize(encoded).has_value());

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_phy_mask_header_unknown_version_scheme() {
    TEST("PHY mask header unknown version, scheme, and interleaver rejection") {
        PHYMaskHeader bad_version = makeValidPHYMaskHeader(activeMaskWithErased({17}), 1);
        bad_version.version = 2;
        assert(!PHYMaskHeader::deserialize(bad_version.serialize()).has_value());

        PHYMaskHeader bad_scheme = makeValidPHYMaskHeader(activeMaskWithErased({17}), 1);
        bad_scheme.scheme = 2;
        assert(!PHYMaskHeader::deserialize(bad_scheme.serialize()).has_value());

        PHYMaskHeader bad_interleaver = makeValidPHYMaskHeader(activeMaskWithErased({17}), 1);
        bad_interleaver.interleaver_id = 1;
        assert(!PHYMaskHeader::deserialize(bad_interleaver.serialize()).has_value());

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_phy_mask_header_reserved_rejection() {
    TEST("PHY mask header flags and reserved-field rejection") {
        PHYMaskHeader bad_flags = makeValidPHYMaskHeader(activeMaskWithErased({17}), 1);
        bad_flags.flags = 1;
        assert(!PHYMaskHeader::deserialize(bad_flags.serialize()).has_value());

        PHYMaskHeader bad_reserved = makeValidPHYMaskHeader(activeMaskWithErased({17}), 1);
        bad_reserved.reserved = 1;
        assert(!PHYMaskHeader::deserialize(bad_reserved.serialize()).has_value());

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_phy_mask_header_mask_count_mismatch() {
    TEST("PHY mask header mask-count mismatch rejection") {
        PHYMaskHeader h = makeValidPHYMaskHeader(activeMaskWithErased({3, 9}), 3);
        assert(!PHYMaskHeader::deserialize(h.serialize()).has_value());

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_phy_mask_header_out_of_range_bits() {
    TEST("PHY mask header out-of-range carrier bit rejection") {
        PHYMaskHeader h = makeValidPHYMaskHeader(activeMaskWithErased({17}), 1);
        h.active_mask |= (uint64_t{1} << 59);
        assert(!PHYMaskHeader::deserialize(h.serialize()).has_value());

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_phy_mask_capability_roundtrip() {
    TEST("PHY mask capability flag roundtrip in CONNECT and CONNECT_ACK") {
        auto connect = ConnectFrame::makeConnect(
            "VA2MVR/P", "W1AW", ModeCapabilities::ALL,
            static_cast<uint8_t>(WaveformMode::OFDM_CHIRP),
            static_cast<uint8_t>(Modulation::DQPSK),
            static_cast<uint8_t>(CodeRate::R1_2));

        assert(!hasPhyMaskV1Capability(connect));
        setPhyMaskV1Capability(connect);
        assert(hasPhyMaskV1Capability(connect));

        auto parsed = ConnectFrame::deserialize(connect.serialize());
        assert(parsed.has_value());
        assert(parsed->type == FrameType::CONNECT);
        assert(hasPhyMaskV1Capability(*parsed));
        assert(ultra::protocol::hasPhyMaskV1Capability(parsed->mode_capabilities));
        assert((parsed->mode_capabilities & ModeCapabilities::ALL) == ModeCapabilities::ALL);

        auto ack = ConnectFrame::makeConnectAck("W1AW", "VA2MVR/P",
                                                static_cast<uint8_t>(WaveformMode::OFDM_CHIRP),
                                                Modulation::DQPSK, CodeRate::R1_2,
                                                15.25f, 0.62f, 4,
                                                LadderRungId::OFDM_CHIRP);
        setPhyMaskV1Capability(ack);

        auto ack_parsed = ConnectFrame::deserialize(ack.serialize());
        assert(ack_parsed.has_value());
        assert(ack_parsed->type == FrameType::CONNECT_ACK);
        assert(hasPhyMaskV1Capability(*ack_parsed));
        assert(ack_parsed->data_frame_cw_count == 4);
        assert(ack_parsed->ladder_rung_id == LadderRungId::OFDM_CHIRP);
        assert(std::abs(decodeFadingIndex(ack_parsed->mode_capabilities) - 0.62f) < 0.001f);

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_connect_frame_roundtrip_and_crc() {
    TEST("connect frame roundtrip and CRC validation") {
        auto connect = ConnectFrame::makeConnect(
            "VA2MVR/P", "W1AW", ModeCapabilities::ALL,
            static_cast<uint8_t>(WaveformMode::OFDM_CHIRP),
            static_cast<uint8_t>(Modulation::DQPSK),
            static_cast<uint8_t>(CodeRate::R1_2));

        auto serialized = connect.serialize();
        assert(serialized.size() == DataFrame::HEADER_SIZE + ConnectFrame::PAYLOAD_SIZE + DataFrame::CRC_SIZE);
        assert(serialized[12] == FIXED_FRAME_CODEWORDS);

        auto parsed = ConnectFrame::deserialize(serialized);
        assert(parsed.has_value());
        assert(parsed->type == FrameType::CONNECT);
        assert(parsed->getSrcCallsign() == "VA2MVR/P");
        assert(parsed->getDstCallsign() == "W1AW");
        assert(parsed->mode_capabilities == ModeCapabilities::ALL);
        assert(parsed->negotiated_mode == static_cast<uint8_t>(WaveformMode::OFDM_CHIRP));
        assert(parsed->initial_modulation == static_cast<uint8_t>(Modulation::DQPSK));
        assert(parsed->initial_code_rate == static_cast<uint8_t>(CodeRate::R1_2));

        auto ack = ConnectFrame::makeConnectAck("W1AW", "VA2MVR/P",
                                                static_cast<uint8_t>(WaveformMode::OFDM_CHIRP),
                                                Modulation::DQPSK, CodeRate::R1_2,
                                                15.25f, 0.62f, 8,
                                                LadderRungId::OFDM_CHIRP);
        auto ack_bytes = ack.serialize();
        auto ack_parsed = ConnectFrame::deserialize(ack_bytes);
        assert(ack_parsed.has_value());
        assert(ack_parsed->type == FrameType::CONNECT_ACK);
        assert(std::abs(decodeSNR(ack_parsed->measured_snr) - 15.25f) < 0.001f);
        assert(std::abs(decodeFadingIndex(ack_parsed->mode_capabilities) - 0.62f) < 0.001f);
        assert(ack_parsed->data_frame_cw_count == 8);
        assert(ack_parsed->ladder_rung_id == LadderRungId::OFDM_CHIRP);

        auto corrupt_header = serialized;
        corrupt_header[5] ^= 0x01;
        assert(!ConnectFrame::deserialize(corrupt_header).has_value());

        auto corrupt_payload = serialized;
        corrupt_payload[DataFrame::HEADER_SIZE] ^= 0x01;
        assert(!ConnectFrame::deserialize(corrupt_payload).has_value());

        auto wrong_type = DataFrame::makeData("VA2MVR", "W1AW", 1, "not a connect").serialize();
        assert(!ConnectFrame::deserialize(wrong_type).has_value());

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_ladder_rung_wire_roundtrip() {
    TEST("ladder rung bits in CONNECT_ACK and MODE_CHANGE") {
        auto ack = ConnectFrame::makeConnectAck("W1AW", "VA2MVR/P",
                                                static_cast<uint8_t>(WaveformMode::MC_DPSK),
                                                Modulation::DBPSK, CodeRate::R1_4,
                                                0.0f, 0.20f, 3,
                                                LadderRungId::ROBUST_MID);
        auto ack_parsed = ConnectFrame::deserialize(ack.serialize());
        assert(ack_parsed.has_value());
        assert(ack_parsed->data_frame_cw_count == 3);
        assert(ack_parsed->ladder_rung_id == LadderRungId::ROBUST_MID);

        auto mode = ControlFrame::makeModeChange("W1AW", "VA2MVR/P", 7,
                                                 Modulation::DQPSK, CodeRate::R1_4,
                                                 6.0f, 0.40f,
                                                 ModeChangeReason::CHANNEL_IMPROVED,
                                                 4, LadderRungId::ROBUST);
        auto parsed = ControlFrame::deserialize(mode.serialize());
        assert(parsed.has_value());
        auto info = parsed->getModeChangeInfo();
        assert(info.data_frame_cw_count == 4);
        assert(info.ladder_rung_id == LadderRungId::ROBUST);

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_data_frame_codeword_count() {
    TEST("data frame codeword calculation") {
        // Empty payload: header(17) + CRC(2) = 19 bytes = 1 codeword
        assert(DataFrame::calculateCodewords(0) == 1);

        // 3 bytes payload: 17 + 3 + 2 = 22 bytes = 2 codewords
        assert(DataFrame::calculateCodewords(3) == 2);

        // 20 bytes payload: 39 bytes total = CW0(20) + 19 bytes in CW1+ = 3 codewords
        assert(DataFrame::calculateCodewords(20) == 3);

        // 21 bytes payload: 40 bytes total = CW0(20) + 20 bytes in CW1+ = 3 codewords
        assert(DataFrame::calculateCodewords(21) == 3);

        // 23 bytes payload: 42 bytes total = CW0(20) + 22 bytes in CW1+ = 3 codewords
        assert(DataFrame::calculateCodewords(23) == 3);

        // 100 bytes payload: 119 bytes total = CW0(20) + 99 bytes in CW1+ = 7 codewords
        assert(DataFrame::calculateCodewords(100) == 7);

        // 256 bytes payload: 275 bytes total = CW0(20) + 255 bytes in CW1+ = 16 codewords
        assert(DataFrame::calculateCodewords(256) == 16);

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_data_frame_roundtrip() {
    TEST("data frame serialize/deserialize roundtrip") {
        std::string message = "Hello, this is a test message from VA2MVR!";
        auto original = DataFrame::makeData("VA2MVR", "W1AW", 42, message);

        auto serialized = original.serialize();
        auto parsed = DataFrame::deserialize(serialized);

        assert(parsed.has_value());
        assert(parsed->type == original.type);
        assert(parsed->seq == original.seq);
        assert(parsed->src_hash == original.src_hash);
        assert(parsed->dst_hash == original.dst_hash);
        assert(parsed->total_cw == original.total_cw);
        assert(parsed->payload_len == original.payload_len);
        assert(parsed->payload == original.payload);
        assert(parsed->payloadAsText() == message);

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_data_frame_crc() {
    TEST("data frame CRC validation") {
        auto data = DataFrame::makeData("VA2MVR", "W1AW", 1, "Test data");
        auto serialized = data.serialize();

        // Valid should parse
        auto valid = DataFrame::deserialize(serialized);
        assert(valid.has_value());

        // Corrupt header - should fail header CRC
        auto corrupt_header = serialized;
        corrupt_header[5] ^= 0xFF;
        auto invalid1 = DataFrame::deserialize(corrupt_header);
        assert(!invalid1.has_value());

        // Corrupt payload - should fail frame CRC
        auto corrupt_payload = serialized;
        corrupt_payload[20] ^= 0xFF;
        auto invalid2 = DataFrame::deserialize(corrupt_payload);
        assert(!invalid2.has_value());

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_split_into_codewords() {
    TEST("split into codewords") {
        std::string message = "Hello, World!";  // 13 bytes
        auto data = DataFrame::makeData("VA2MVR", "W1AW", 1, message);
        auto serialized = data.serialize();

        // 17 + 13 + 2 = 32 bytes = 2 codewords
        auto codewords = splitIntoCodewords(serialized);
        assert(codewords.size() == 2);
        assert(codewords[0].size() == 20);
        assert(codewords[1].size() == 20);  // Padded

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_reassemble_codewords() {
    TEST("reassemble codewords") {
        std::string message = "Hello, World!";  // 13 bytes
        auto data = DataFrame::makeData("VA2MVR", "W1AW", 1, message);
        auto original = data.serialize();

        // Split and reassemble
        auto codewords = splitIntoCodewords(original);
        auto reassembled = reassembleCodewords(codewords, original.size());

        assert(reassembled == original);

        // Should still parse correctly
        auto parsed = DataFrame::deserialize(reassembled);
        assert(parsed.has_value());
        assert(parsed->payloadAsText() == message);

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_malformed_headers_and_status_edges() {
    TEST("malformed headers and CodewordStatus edge cases") {
        assert(!parseHeader(Bytes{}).valid);
        assert(!parseHeader(Bytes{0x55}).valid);

        auto probe = ControlFrame::makeProbe("VA2MVR", "W1AW").serialize();
        auto bad_magic = probe;
        bad_magic[0] = 0x00;
        assert(!parseHeader(bad_magic).valid);

        auto bad_control_crc = probe;
        bad_control_crc[17] ^= 0x01;
        assert(!parseHeader(bad_control_crc).valid);

        auto data = DataFrame::makeData("VA2MVR", "W1AW", 7, "payload").serialize();
        auto bad_header_crc = data;
        bad_header_crc[14] ^= 0x01;
        auto bad_chunks = splitIntoCodewords(bad_header_crc);
        assert(!parseHeader(bad_chunks[0]).valid);

        CodewordStatus empty;
        assert(empty.getExpectedCodewords() == 0);
        assert(empty.reassemble().empty());

        CodewordStatus status;
        status.initForFrame(3);
        assert(status.decoded.size() == 3);
        assert(!status.allSuccess());
        assert(status.countFailures() == 3);
        assert(status.getNackBitmap() == 0b111);
        assert(!status.mergeCodeword(99, Bytes{1, 2, 3}));

        status.decoded[1] = true;
        assert(!status.mergeCodeword(1, Bytes{1, 2, 3}));
        assert(status.mergeCodeword(2, Bytes{4, 5, 6}));
        assert(status.decoded[2]);

        status.initForFrame(2);
        assert(status.decoded.size() == 2);
        assert(!status.decoded[0]);
        assert(!status.decoded[1]);

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_nack_payload() {
    TEST("NACK payload encode/decode") {
        NackPayload original;
        original.frame_seq = 1234;
        original.cw_bitmap = 0b10101010;  // CWs 1,3,5,7 failed

        uint8_t buffer[6];
        original.encode(buffer);

        auto decoded = NackPayload::decode(buffer);
        assert(decoded.frame_seq == original.frame_seq);
        assert(decoded.cw_bitmap == original.cw_bitmap);
        assert(decoded.countFailed() == 4);
        assert(!decoded.isFailed(0));
        assert(decoded.isFailed(1));
        assert(!decoded.isFailed(2));
        assert(decoded.isFailed(3));

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_nack_frame() {
    TEST("NACK control frame") {
        uint32_t bitmap = 0b00000101;  // CWs 0 and 2 failed
        auto nack = ControlFrame::makeNack("W1AW", "VA2MVR", 100, bitmap);

        auto serialized = nack.serialize();
        assert(serialized.size() == 20);  // Still fits in 1 codeword

        auto parsed = ControlFrame::deserialize(serialized);
        assert(parsed.has_value());
        assert(parsed->type == FrameType::NACK);

        // Decode NACK payload
        auto np = NackPayload::decode(parsed->payload);
        assert(np.frame_seq == 100);
        assert(np.cw_bitmap == bitmap);
        assert(np.isFailed(0));
        assert(!np.isFailed(1));
        assert(np.isFailed(2));

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_fixed_frame_helpers() {
    TEST("fixed frame helper policy") {
        assert(getFixedFramePayloadCapacity(CodeRate::R1_4) == 61);
        assert(getFixedFramePayloadCapacity(CodeRate::R1_2) == 141);
        assert(getFixedFramePayloadCapacity(CodeRate::R2_3) == 197);
        assert(getFixedFramePayloadCapacity(CodeRate::R3_4) == 221);

        Bytes oversized(300, 0xA5);
        auto frame = makeFixedDataFrame("VA2MVR", "W1AW", 42, oversized, CodeRate::R1_2);
        assert(frame.total_cw == FIXED_FRAME_CODEWORDS);
        assert(frame.payload.size() == getFixedFramePayloadCapacity(CodeRate::R1_2));
        assert(frame.payload_len == getFixedFramePayloadCapacity(CodeRate::R1_2));

        auto serialized = frame.serialize();
        auto parsed = DataFrame::deserialize(serialized);
        assert(parsed.has_value());
        assert(parsed->total_cw == FIXED_FRAME_CODEWORDS);
        assert(parsed->payload.size() == getFixedFramePayloadCapacity(CodeRate::R1_2));

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_fixed_frame_reassemble_preserves_marker_boundary_byte() {
    TEST("fixed frame reassembly preserves 0xD5 at CW boundary") {
        constexpr CodeRate rate = CodeRate::R2_3;
        const size_t capacity = getFixedFramePayloadCapacity(rate);
        const size_t bytes_per_cw = getBytesPerCodeword(rate);
        const size_t payload_boundary_offset = bytes_per_cw - DataFrame::HEADER_SIZE;

        Bytes payload(capacity, 0x41);
        payload[payload_boundary_offset] = DATA_CW_MARKER;
        payload[payload_boundary_offset + 1] = 0x99;

        auto frame = makeFixedDataFrame("VA2MVR", "W1AW", 77, payload, rate);
        auto serialized = frame.serialize();

        assert(serialized.size() == FIXED_FRAME_CODEWORDS * bytes_per_cw);
        assert(serialized[bytes_per_cw] == DATA_CW_MARKER);
        assert(serialized[bytes_per_cw + 1] == 0x99);

        auto encoded = encodeFixedFrame(serialized, rate, false);
        auto soft_bits = bytesToSoftBits(encoded);
        assert(soft_bits.size() == FIXED_FRAME_CODEWORDS * LDPC_CODEWORD_BITS);

        auto status = decodeFixedFrame(soft_bits, rate, false);
        assert(status.fixed_frame);
        assert(status.allSuccess());

        auto reassembled = status.reassemble();
        assert(reassembled == serialized);
        assert(reassembled[bytes_per_cw] == DATA_CW_MARKER);
        assert(reassembled[bytes_per_cw + 1] == 0x99);

        auto parsed = DataFrame::deserialize(reassembled);
        assert(parsed.has_value());
        assert(parsed->payload == payload);

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_fixed_frame_variable_cw_roundtrip_per_rate() {
    TEST("fixed frame 4/6/8 CW roundtrip per rate") {
        const CodeRate rates[] = {
            CodeRate::R1_4,
            CodeRate::R1_2,
            CodeRate::R2_3,
            CodeRate::R3_4,
        };
        const int cw_counts[] = {4, 6, 8};

        for (CodeRate rate : rates) {
            const size_t bytes_per_cw = getBytesPerCodeword(rate);
            for (int cw_count : cw_counts) {
                const size_t capacity = getFixedFramePayloadCapacity(rate, cw_count);
                Bytes payload(capacity);
                for (size_t i = 0; i < payload.size(); ++i) {
                    payload[i] = static_cast<uint8_t>((i * 37 + cw_count) & 0xFF);
                }

                auto frame = makeFixedDataFrame("VA2MVR", "W1AW",
                                                static_cast<uint16_t>(100 + cw_count),
                                                payload, rate, cw_count);
                assert(frame.total_cw == cw_count);
                assert(frame.payload_len == capacity);

                auto serialized = frame.serialize();
                assert(serialized[12] == cw_count);
                assert(serialized.size() == static_cast<size_t>(cw_count) * bytes_per_cw);

                auto header = parseHeader(serialized);
                assert(header.valid);
                assert(header.total_cw == cw_count);

                auto encoded = encodeFixedFrame(serialized, rate, cw_count, false);
                assert(encoded.size() == static_cast<size_t>(cw_count) * LDPC_CODEWORD_BYTES);

                auto soft_bits = bytesToSoftBits(encoded);
                assert(soft_bits.size() == static_cast<size_t>(cw_count) * LDPC_CODEWORD_BITS);

                auto status = decodeFixedFrame(soft_bits, rate, cw_count, false);
                assert(status.fixed_frame);
                assert(status.decoded.size() == static_cast<size_t>(cw_count));
                assert(status.allSuccess());

                auto reassembled = status.reassemble();
                assert(reassembled == serialized);

                auto parsed = DataFrame::deserialize(reassembled);
                assert(parsed.has_value());
                assert(parsed->total_cw == cw_count);
                assert(parsed->payload == payload);
            }
        }

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_codeword_status() {
    TEST("codeword status tracking") {
        CodewordStatus status;
        status.decoded = {true, false, true, false, true};

        assert(!status.allSuccess());
        assert(status.countFailures() == 2);

        uint32_t bitmap = status.getNackBitmap();
        assert(bitmap == 0b01010);  // Bits 1 and 3 set (failed)

        // All success case
        CodewordStatus success;
        success.decoded = {true, true, true};
        assert(success.allSuccess());
        assert(success.countFailures() == 0);
        assert(success.getNackBitmap() == 0);

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_frame_type_helpers() {
    TEST("frame type helpers") {
        assert(isControlFrame(FrameType::PROBE));
        assert(isControlFrame(FrameType::ACK));
        assert(isControlFrame(FrameType::NACK));
        assert(isControlFrame(FrameType::BEACON));
        assert(!isControlFrame(FrameType::DATA));

        assert(isDataFrame(FrameType::DATA));
        assert(isDataFrame(FrameType::DATA_START));
        assert(isDataFrame(FrameType::DATA_END));
        assert(!isDataFrame(FrameType::PROBE));

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_large_text_message() {
    TEST("large text message (multiple codewords)") {
        // Create a message that requires multiple codewords
        std::string message;
        for (int i = 0; i < 10; i++) {
            message += "This is line " + std::to_string(i) + " of the message. ";
        }

        auto data = DataFrame::makeData("VA2MVR", "W1AW", 999, message);

        std::cout << "\n  Message size: " << message.size() << " bytes\n";
        std::cout << "  Total codewords: " << (int)data.total_cw << "\n";

        auto serialized = data.serialize();
        std::cout << "  Serialized size: " << serialized.size() << " bytes\n";

        // Split into codewords
        auto codewords = splitIntoCodewords(serialized);
        std::cout << "  Codeword count: " << codewords.size() << "\n";
        assert(codewords.size() == data.total_cw);

        // Reassemble and parse
        auto reassembled = reassembleCodewords(codewords, serialized.size());
        auto parsed = DataFrame::deserialize(reassembled);

        assert(parsed.has_value());
        assert(parsed->payloadAsText() == message);
        std::cout << "  ";

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

// ============================================================================
// LDPC Integration Tests
// ============================================================================

void test_ldpc_control_frame_roundtrip() {
    TEST("LDPC encode/decode control frame (1 codeword)") {
        // Create and serialize a PROBE frame
        auto probe = ControlFrame::makeProbe("VA2MVR", "W1AW");
        auto serialized = probe.serialize();
        assert(serialized.size() == 20);

        // LDPC encode
        auto encoded = encodeFrameWithLDPC(serialized);
        assert(encoded.size() == 1);  // 1 codeword
        assert(encoded[0].size() == LDPC_CODEWORD_BYTES);  // 81 bytes

        std::cout << "\n  Frame: 20 bytes → LDPC: " << encoded[0].size() << " bytes\n";

        // Convert to soft bits (perfect LLRs)
        std::vector<std::vector<float>> soft_bits(1);
        for (uint8_t byte : encoded[0]) {
            for (int b = 7; b >= 0; --b) {
                int bit = (byte >> b) & 1;
                soft_bits[0].push_back(bit ? -5.0f : 5.0f);
            }
        }
        assert(soft_bits[0].size() == LDPC_CODEWORD_BITS);

        // LDPC decode
        auto status = decodeCodewordsWithLDPC(soft_bits);
        assert(status.allSuccess());
        assert(status.decoded.size() == 1);
        assert(status.data[0].size() == 20);

        // Verify data matches
        assert(status.data[0] == serialized);

        // Parse and verify
        auto parsed = ControlFrame::deserialize(status.data[0]);
        assert(parsed.has_value());
        assert(parsed->type == FrameType::PROBE);
        assert(parsed->src_hash == probe.src_hash);
        std::cout << "  ";

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_ldpc_data_frame_roundtrip() {
    TEST("LDPC encode/decode data frame (multiple codewords)") {
        std::string message = "Hello, this is a test message for LDPC multi-codeword!";
        auto data = DataFrame::makeData("VA2MVR", "W1AW", 42, message);
        auto serialized = data.serialize();

        std::cout << "\n  Message: " << message.size() << " bytes\n";
        std::cout << "  Frame: " << serialized.size() << " bytes\n";
        std::cout << "  Expected codewords: " << (int)data.total_cw << "\n";

        // LDPC encode
        auto encoded = encodeFrameWithLDPC(serialized);
        assert(encoded.size() == data.total_cw);
        std::cout << "  Encoded: " << encoded.size() << " codewords × "
                  << encoded[0].size() << " bytes\n";

        // Convert all to soft bits
        std::vector<std::vector<float>> soft_bits(encoded.size());
        for (size_t cw = 0; cw < encoded.size(); cw++) {
            for (uint8_t byte : encoded[cw]) {
                for (int b = 7; b >= 0; --b) {
                    int bit = (byte >> b) & 1;
                    soft_bits[cw].push_back(bit ? -5.0f : 5.0f);
                }
            }
        }

        // LDPC decode
        auto status = decodeCodewordsWithLDPC(soft_bits);
        assert(status.allSuccess());
        assert(status.countFailures() == 0);

        // Parse header from first codeword
        auto header = parseHeader(status.data[0]);
        assert(header.valid);
        assert(!header.is_control);
        assert(header.total_cw == data.total_cw);
        assert(header.payload_len == message.size());
        std::cout << "  Header valid, total_cw=" << (int)header.total_cw
                  << ", payload_len=" << header.payload_len << "\n";

        // Reassemble
        auto reassembled = status.reassemble();
        assert(reassembled.size() == serialized.size());
        assert(reassembled == serialized);

        // Parse and verify message
        auto parsed = DataFrame::deserialize(reassembled);
        assert(parsed.has_value());
        assert(parsed->payloadAsText() == message);
        std::cout << "  ";

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_ldpc_simulated_codeword_loss() {
    TEST("LDPC with simulated codeword loss (per-CW recovery)") {
        std::string message = "This message will have codeword 2 corrupted!";
        auto data = DataFrame::makeData("VA2MVR", "W1AW", 100, message);
        auto serialized = data.serialize();

        // LDPC encode
        auto encoded = encodeFrameWithLDPC(serialized);
        assert(encoded.size() >= 3);  // Need at least 3 codewords for this test

        std::cout << "\n  Codewords: " << encoded.size() << "\n";

        // Convert to soft bits, but corrupt codeword 2
        std::vector<std::vector<float>> soft_bits(encoded.size());
        for (size_t cw = 0; cw < encoded.size(); cw++) {
            for (uint8_t byte : encoded[cw]) {
                for (int b = 7; b >= 0; --b) {
                    int bit = (byte >> b) & 1;
                    float llr = bit ? -5.0f : 5.0f;

                    // Corrupt codeword 2 by flipping LLR signs (simulate bit errors)
                    if (cw == 2) {
                        llr = -llr;  // All bits wrong = LDPC will fail
                    }

                    soft_bits[cw].push_back(llr);
                }
            }
        }

        // LDPC decode - codeword 2 should fail
        auto status = decodeCodewordsWithLDPC(soft_bits);

        assert(!status.allSuccess());
        assert(status.decoded[0] == true);   // CW0 (header) OK
        assert(status.decoded[1] == true);   // CW1 OK
        assert(status.decoded[2] == false);  // CW2 FAILED (corrupted)
        if (encoded.size() > 3) {
            assert(status.decoded[3] == true);  // CW3 OK
        }

        // Check NACK bitmap
        uint32_t bitmap = status.getNackBitmap();
        assert(bitmap == 0b00000100);  // Bit 2 set
        std::cout << "  NACK bitmap: 0x" << std::hex << bitmap << std::dec << "\n";
        std::cout << "  Failed codewords: " << status.countFailures() << "\n";

        // Header should still be parseable
        auto header = parseHeader(status.data[0]);
        assert(header.valid);
        assert(header.total_cw == data.total_cw);
        std::cout << "  Header still valid despite CW2 loss\n  ";

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_header_parsing() {
    TEST("header parsing from first codeword") {
        // Test control frame
        auto probe = ControlFrame::makeProbe("VA2MVR", "W1AW");
        auto ctrl_data = probe.serialize();
        auto ctrl_header = parseHeader(ctrl_data);

        assert(ctrl_header.valid);
        assert(ctrl_header.is_control);
        assert(ctrl_header.type == FrameType::PROBE);
        assert(ctrl_header.total_cw == 1);
        assert(ctrl_header.src_hash == hashCallsign("VA2MVR"));
        assert(ctrl_header.dst_hash == hashCallsign("W1AW"));

        // Test data frame
        auto data = DataFrame::makeData("W1AW", "VA2MVR", 1234, "Test payload data");
        auto data_serialized = data.serialize();
        auto chunks = splitIntoCodewords(data_serialized);
        auto data_header = parseHeader(chunks[0]);

        assert(data_header.valid);
        assert(!data_header.is_control);
        assert(data_header.type == FrameType::DATA);
        assert(data_header.total_cw == data.total_cw);
        assert(data_header.payload_len == 17);  // "Test payload data"
        assert(data_header.seq == 1234);

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_full_recovery_cycle() {
    TEST("full NACK→retransmit→merge recovery cycle") {
        std::string message = "This message will be recovered after NACK retransmit!";
        auto data = DataFrame::makeData("VA2MVR", "W1AW", 500, message);
        auto serialized = data.serialize();

        std::cout << "\n  Original message: " << message.size() << " bytes\n";

        // === TX SIDE: LDPC encode all codewords ===
        auto encoded = encodeFrameWithLDPC(serialized);
        std::cout << "  TX encoded: " << encoded.size() << " codewords\n";
        assert(encoded.size() >= 4);

        // === RX SIDE: Initial receive with CW2 corrupted ===
        std::vector<std::vector<float>> rx_soft_bits(encoded.size());
        for (size_t cw = 0; cw < encoded.size(); cw++) {
            for (uint8_t byte : encoded[cw]) {
                for (int b = 7; b >= 0; --b) {
                    int bit = (byte >> b) & 1;
                    float llr = bit ? -5.0f : 5.0f;
                    // Corrupt CW2
                    if (cw == 2) llr = -llr;
                    rx_soft_bits[cw].push_back(llr);
                }
            }
        }

        auto rx_status = decodeCodewordsWithLDPC(rx_soft_bits);
        assert(!rx_status.allSuccess());
        assert(rx_status.decoded[2] == false);
        std::cout << "  RX initial: CW2 failed (as expected)\n";

        // === RX SIDE: Generate NACK ===
        uint32_t nack_bitmap = rx_status.getNackBitmap();
        assert(nack_bitmap == 0x04);  // Bit 2 set
        auto nack = ControlFrame::makeNack("W1AW", "VA2MVR", 500, nack_bitmap);
        auto nack_bytes = nack.serialize();
        std::cout << "  RX sends NACK with bitmap 0x" << std::hex << nack_bitmap << std::dec << "\n";

        // === TX SIDE: Receive NACK, retransmit only CW2 ===
        auto nack_parsed = ControlFrame::deserialize(nack_bytes);
        assert(nack_parsed.has_value());
        assert(nack_parsed->type == FrameType::NACK);

        NackPayload np = NackPayload::decode(nack_parsed->payload);
        assert(np.frame_seq == 500);
        assert(np.isFailed(2));
        std::cout << "  TX received NACK, retransmitting CW2\n";

        // TX retransmits only the failed codeword(s)
        // In practice, this would be sent over the air
        Bytes retransmit_cw2 = encoded[2];

        // === RX SIDE: Receive retransmission, decode and merge ===
        // Convert retransmitted CW2 to soft bits (this time uncorrupted)
        std::vector<float> retx_soft_bits;
        for (uint8_t byte : retransmit_cw2) {
            for (int b = 7; b >= 0; --b) {
                int bit = (byte >> b) & 1;
                retx_soft_bits.push_back(bit ? -5.0f : 5.0f);  // Clean this time
            }
        }

        auto [retx_success, retx_data] = decodeSingleCodeword(retx_soft_bits);
        assert(retx_success);
        std::cout << "  RX decoded retransmitted CW2 successfully\n";

        // Merge into the partial frame
        bool merged = rx_status.mergeCodeword(2, retx_data);
        assert(merged);
        assert(rx_status.allSuccess());
        std::cout << "  RX merged CW2, all codewords now decoded\n";

        // === RX SIDE: Reassemble complete frame ===
        auto reassembled = rx_status.reassemble();
        assert(reassembled.size() == serialized.size());
        assert(reassembled == serialized);

        auto parsed = DataFrame::deserialize(reassembled);
        assert(parsed.has_value());
        assert(parsed->payloadAsText() == message);
        std::cout << "  RX recovered message: \"" << parsed->payloadAsText() << "\"\n  ";

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_codeword_identification() {
    TEST("codeword identification (marker + index)") {
        // Create a data frame that will have multiple codewords
        std::string message = "Test message for codeword identification";
        auto data = DataFrame::makeData("VA2MVR", "W1AW", 100, message);
        auto serialized = data.serialize();
        auto codewords = splitIntoCodewords(serialized);

        assert(codewords.size() >= 3);  // Should have at least 3 CWs

        // CW0 should be identified as HEADER
        auto cw0_info = identifyCodeword(codewords[0]);
        assert(cw0_info.type == CodewordType::HEADER);
        assert(cw0_info.index == 0);
        assert(isHeaderCodeword(codewords[0]));
        assert(!isDataCodeword(codewords[0]));

        // CW1 should be identified as DATA with index 1
        auto cw1_info = identifyCodeword(codewords[1]);
        assert(cw1_info.type == CodewordType::DATA);
        assert(cw1_info.index == 1);
        assert(!isHeaderCodeword(codewords[1]));
        assert(isDataCodeword(codewords[1]));
        assert(getDataCodewordIndex(codewords[1]) == 1);

        // CW2 should be identified as DATA with index 2
        auto cw2_info = identifyCodeword(codewords[2]);
        assert(cw2_info.type == CodewordType::DATA);
        assert(cw2_info.index == 2);
        assert(getDataCodewordIndex(codewords[2]) == 2);

        // Verify CW1+ start with marker 0xD5
        assert(codewords[1][0] == DATA_CW_MARKER);
        assert(codewords[2][0] == DATA_CW_MARKER);

        // Verify reassembly still works
        auto reassembled = reassembleCodewords(codewords, serialized.size());
        assert(reassembled == serialized);

        auto parsed = DataFrame::deserialize(reassembled);
        assert(parsed.has_value());
        assert(parsed->payloadAsText() == message);

        std::cout << "  CW0: HEADER (0x554C magic)\n";
        std::cout << "  CW1: DATA index=1 (0xD5 marker)\n";
        std::cout << "  CW2: DATA index=2 (0xD5 marker)\n  ";

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_codeword_out_of_order_recovery() {
    TEST("out-of-order codeword recovery simulation") {
        // Simulate receiving codewords out of order
        std::string message = "This tests out-of-order codeword reception!";
        auto data = DataFrame::makeData("VA2MVR", "W1AW", 200, message);
        auto serialized = data.serialize();
        auto codewords = splitIntoCodewords(serialized);

        assert(codewords.size() >= 4);

        // Initialize status for the frame
        CodewordStatus status;
        status.initForFrame(codewords.size());

        // Receive CW2 first (out of order)
        auto cw2_info = identifyCodeword(codewords[2]);
        assert(cw2_info.type == CodewordType::DATA);
        status.decoded[cw2_info.index] = true;
        status.data[cw2_info.index] = codewords[2];

        // Receive CW1 second (still out of order)
        auto cw1_info = identifyCodeword(codewords[1]);
        assert(cw1_info.type == CodewordType::DATA);
        status.decoded[cw1_info.index] = true;
        status.data[cw1_info.index] = codewords[1];

        // Not all received yet
        assert(!status.allSuccess());

        // Receive CW3 (if exists)
        if (codewords.size() > 3) {
            auto cw3_info = identifyCodeword(codewords[3]);
            status.decoded[cw3_info.index] = true;
            status.data[cw3_info.index] = codewords[3];
        }

        // Finally receive CW0 (header)
        auto cw0_info = identifyCodeword(codewords[0]);
        assert(cw0_info.type == CodewordType::HEADER);
        status.decoded[0] = true;
        status.data[0] = codewords[0];

        // Now all should be received
        assert(status.allSuccess());

        // Reassemble and verify
        auto reassembled = status.reassemble();
        assert(reassembled == serialized);

        auto parsed = DataFrame::deserialize(reassembled);
        assert(parsed.has_value());
        assert(parsed->payloadAsText() == message);

        std::cout << "  Received order: CW2 → CW1 → CW3 → CW0\n";
        std::cout << "  Successfully reassembled after header arrival\n  ";

        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

int main() {
    std::cout << "=== ULTRA Protocol v2 Frame Tests ===\n\n";

    test_callsign_hashing();
    test_callsign_validation_and_ping();
    test_channel_report_and_quantizers();
    test_control_frame_size();
    test_control_frame_roundtrip();
    test_burst_header_roundtrip();
    test_control_frame_crc();
    test_control_frame_magic();
    test_phy_mask_header_roundtrip_patterns();
    test_phy_mask_header_crc_tampering();
    test_phy_mask_header_inverted_crc_tampering();
    test_phy_mask_header_unknown_version_scheme();
    test_phy_mask_header_reserved_rejection();
    test_phy_mask_header_mask_count_mismatch();
    test_phy_mask_header_out_of_range_bits();
    test_phy_mask_capability_roundtrip();
    test_connect_frame_roundtrip_and_crc();
    test_ladder_rung_wire_roundtrip();
    test_data_frame_codeword_count();
    test_data_frame_roundtrip();
    test_data_frame_crc();
    test_split_into_codewords();
    test_reassemble_codewords();
    test_malformed_headers_and_status_edges();
    test_nack_payload();
    test_nack_frame();
    test_fixed_frame_helpers();
    test_fixed_frame_reassemble_preserves_marker_boundary_byte();
    test_fixed_frame_variable_cw_roundtrip_per_rate();
    test_codeword_status();
    test_frame_type_helpers();
    test_large_text_message();

    // LDPC integration tests
    std::cout << "\n=== LDPC Integration Tests ===\n\n";
    test_ldpc_control_frame_roundtrip();
    test_ldpc_data_frame_roundtrip();
    test_ldpc_simulated_codeword_loss();
    test_header_parsing();
    test_full_recovery_cycle();
    test_codeword_identification();
    test_codeword_out_of_order_recovery();

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << tests_passed << "\n";
    std::cout << "Failed: " << tests_failed << "\n";

    return tests_failed > 0 ? 1 : 0;
}
