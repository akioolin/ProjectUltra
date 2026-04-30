#include "ultra/arq.hpp"

#include <chrono>
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

void test_legacy_arq_tx_ack_nack_and_timeout_paths() {
    ModemConfig config;
    config.frame_size = 14;  // 4 data bytes per frame after legacy header/CRC.
    config.arq_timeout_ms = 100;

    FrameParser parser(config);
    ARQController arq(config);

    std::vector<Bytes> sent;
    arq.setSendCallback([&](ByteSpan frame) {
        sent.emplace_back(frame.begin(), frame.end());
    });

    const Bytes payload = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    arq.sendData(payload);

    CHECK(sent.size() == 3, "ARQ splits payload into three legacy frames");
    CHECK(arq.framesInFlight() == 3, "ARQ tracks three frames in flight");

    auto f0 = parser.parse(sent[0]);
    auto f1 = parser.parse(sent[1]);
    auto f2 = parser.parse(sent[2]);
    CHECK(f0.valid && f0.seq_num == 0 && f0.payload == Bytes({0, 1, 2, 3}),
          "ARQ first split frame");
    CHECK(f1.valid && f1.seq_num == 1 && f1.payload == Bytes({4, 5, 6, 7}),
          "ARQ second split frame");
    CHECK(f2.valid && f2.seq_num == 2 && f2.payload == Bytes({8, 9}),
          "ARQ third split frame");

    ChannelQuality remote_quality;
    arq.onAckReceived(1, false, remote_quality);
    CHECK(arq.framesInFlight() == 1, "cumulative ACK removes frames through seq1");

    const size_t before_nack = sent.size();
    arq.onAckReceived(2, true, remote_quality);
    CHECK(sent.size() == before_nack + 1, "NACK retransmits requested frame");
    CHECK(arq.framesPendingRetry() == 1, "NACK increments retry accounting");
    CHECK(parser.parse(sent.back()).seq_num == 2, "NACK retransmits seq2");

    arq.onFrameSent(2);
    const size_t before_timeout = sent.size();
    arq.tick(std::chrono::steady_clock::now() + std::chrono::milliseconds(5000));
    CHECK(sent.size() == before_timeout + 1, "timeout retransmits timed frame");
    CHECK(parser.parse(sent.back()).seq_num == 2, "timeout retransmits seq2");

    arq.reset();
    CHECK(arq.framesInFlight() == 0, "reset clears in-flight frames");
    CHECK(arq.framesPendingRetry() == 0, "reset clears retry accounting");
}

void test_legacy_arq_rx_reorders_and_acks_drained_frames() {
    ModemConfig config;
    FrameParser parser(config);
    ARQController arq(config);

    std::vector<Bytes> delivered;
    arq.setDeliveryCallback([&](Bytes data) {
        delivered.push_back(std::move(data));
    });

    const Bytes second = {'B'};
    const Bytes first = {'A'};
    arq.onDataReceived(1, second);
    CHECK(delivered.empty(), "out-of-order RX frame is buffered");

    arq.onDataReceived(0, first);
    CHECK(delivered.size() == 2, "gap close drains buffered RX frame");
    CHECK(delivered[0] == first, "RX delivers first frame first");
    CHECK(delivered[1] == second, "RX drains buffered second frame");

    auto ack = parser.parse(arq.generateAck());
    CHECK(ack.valid && ack.type == FrameType::ACK && ack.seq_num == 1,
          "ACK advances to last drained in-order frame");

    arq.onDataReceived(0, first);
    CHECK(delivered.size() == 2, "duplicate old RX frame is ignored");
}

}  // namespace

int main() {
    test_data_frame_round_trip_and_crc_rejection();
    test_ack_quality_and_empty_control_frames_parse();
    test_tiny_frame_size_does_not_underflow_or_send();
    test_legacy_arq_tx_ack_nack_and_timeout_paths();
    test_legacy_arq_rx_reorders_and_acks_drained_frames();

    if (tests_failed != 0) {
        std::cout << "LegacyFrameBuilder: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "LegacyFrameBuilder: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
