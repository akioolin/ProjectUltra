#include "ultra/arq.hpp"

#include <cmath>
#include <iostream>
#include <vector>

using namespace ultra;

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

bool nearlyEqual(float a, float b, float tolerance = 0.01f) {
    return std::fabs(a - b) <= tolerance;
}

void test_data_frame_round_trip_and_crc_rejection() {
    ModemConfig config;
    config.frame_size = 32;
    config.modulation = Modulation::QPSK;
    config.code_rate = CodeRate::R1_2;

    FrameBuilder builder(config);
    FrameParser parser(config);

    CHECK(builder.maxPayloadSize() == 22, "max payload subtracts header and CRC");

    const Bytes payload = {1, 2, 3, 4, 5};
    Bytes frame = builder.buildDataFrame(0x1234, payload);
    auto parsed = parser.parse(frame);
    CHECK(parsed.valid, "data frame parses");
    CHECK(parsed.type == FrameType::DATA, "data frame type");
    CHECK(parsed.seq_num == 0x1234, "data frame sequence");
    CHECK(parsed.payload == payload, "data frame payload");

    Bytes bad_header = frame;
    bad_header[1] ^= 0x01;
    CHECK(!parser.parse(bad_header).valid, "header CRC rejects modified sequence");

    Bytes bad_payload = frame;
    bad_payload[8] ^= 0x01;
    CHECK(!parser.parse(bad_payload).valid, "payload CRC rejects modified payload");
}

void test_ack_quality_and_empty_control_frames_parse() {
    ModemConfig config;
    FrameBuilder builder(config);
    FrameParser parser(config);

    ChannelQuality quality;
    quality.snr_db = 12.5f;
    quality.doppler_hz = 1.25f;
    quality.ber_estimate = 0.011f;

    auto ack = parser.parse(builder.buildAckFrame(7, quality));
    CHECK(ack.valid, "ACK frame parses");
    CHECK(ack.type == FrameType::ACK, "ACK frame type");
    CHECK(ack.seq_num == 7, "ACK sequence");
    CHECK(nearlyEqual(ack.remote_quality.snr_db, 12.5f), "ACK SNR quality");
    CHECK(nearlyEqual(ack.remote_quality.doppler_hz, 1.25f), "ACK Doppler quality");
    CHECK(nearlyEqual(ack.remote_quality.ber_estimate, 0.01f, 0.0001f),
          "ACK BER quality");

    auto nack = parser.parse(builder.buildNackFrame(9));
    CHECK(nack.valid && nack.type == FrameType::NACK && nack.seq_num == 9,
          "NACK empty control frame parses");

    auto sync = parser.parse(builder.buildSyncFrame());
    CHECK(sync.valid && sync.type == FrameType::SYNC,
          "SYNC empty control frame parses");

    auto probe = parser.parse(builder.buildProbeFrame());
    CHECK(probe.valid && probe.type == FrameType::PROBE,
          "PROBE empty control frame parses");

    auto disconnect = parser.parse(builder.buildDisconnectFrame());
    CHECK(disconnect.valid && disconnect.type == FrameType::DISCONNECT,
          "DISCONNECT empty control frame parses");
}

void test_tiny_frame_size_does_not_underflow_or_send() {
    ModemConfig config;
    config.frame_size = 8;

    FrameBuilder builder(config);
    CHECK(builder.maxPayloadSize() == 0, "tiny frame size clamps max payload to zero");

    ARQController arq(config);
    bool sent = false;
    arq.setSendCallback([&](ByteSpan) {
        sent = true;
    });

    const Bytes payload = {1, 2, 3};
    arq.sendData(payload);
    CHECK(!sent, "ARQ does not send with zero payload capacity");
    CHECK(arq.framesInFlight() == 0, "ARQ keeps no in-flight frames with zero payload capacity");
}

}  // namespace

int main() {
    test_data_frame_round_trip_and_crc_rejection();
    test_ack_quality_and_empty_control_frames_parse();
    test_tiny_frame_size_does_not_underflow_or_send();

    if (tests_failed != 0) {
        std::cout << "LegacyFrameBuilder: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "LegacyFrameBuilder: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
