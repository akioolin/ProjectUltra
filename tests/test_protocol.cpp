/**
 * Protocol Layer Test Suite
 *
 * Tests the ARQ protocol implementation by simulating two stations
 * communicating with each other. No actual audio/modem involved -
 * TX output from one station feeds directly into RX of the other.
 */

#include "protocol/protocol_engine.hpp"
#include "protocol/frame_v2.hpp"
#include "protocol/file_transfer.hpp"
#include "ultra/types.hpp"
#include "protocol/compression.hpp"
#include "helpers/temp_dir.hpp"
#include <iostream>
#include <cassert>
#include <thread>
#include <queue>
#include <fstream>
#include <cstdio>
#include <filesystem>
#include <system_error>

using namespace ultra::protocol;
using ultra::Bytes;

// Test counters
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { std::cout << "  Testing " << name << "... " << std::flush; tests_run++; } while(0)

#define PASS() \
    do { std::cout << "PASS\n"; tests_passed++; } while(0)

#define FAIL(msg) \
    do { std::cout << "FAIL: " << msg << "\n"; return false; } while(0)

// ============================================================================
// v2 Frame Tests
// ============================================================================

bool test_control_frame_serialization() {
    TEST("v2 Control frame serialization/deserialization");

    // Create a PROBE frame
    auto probe = v2::ControlFrame::makeProbe("VA2MVR", "VE3ABC");
    Bytes serialized = probe.serialize();

    // Should be exactly 20 bytes (ControlFrame::SIZE)
    if (serialized.size() != v2::ControlFrame::SIZE) {
        FAIL("Serialized control frame wrong size");
    }

    // Deserialize
    auto parsed = v2::ControlFrame::deserialize(serialized);
    if (!parsed) {
        FAIL("Failed to deserialize valid control frame");
    }

    // Verify fields
    if (parsed->type != v2::FrameType::PROBE) FAIL("Type mismatch");
    if (parsed->src_hash != v2::hashCallsign("VA2MVR")) FAIL("Source hash mismatch");
    if (parsed->dst_hash != v2::hashCallsign("VE3ABC")) FAIL("Dest hash mismatch");

    PASS();
    return true;
}

bool test_data_frame_serialization() {
    TEST("v2 Data frame serialization/deserialization");

    // Create a DATA frame
    Bytes payload(100, 0x42);  // 100 bytes of 'B'
    auto frame = v2::DataFrame::makeData("VA2MVR", "VE3ABC", 5, payload);
    Bytes serialized = frame.serialize();

    // Deserialize
    auto parsed = v2::DataFrame::deserialize(serialized);
    if (!parsed) {
        FAIL("Failed to deserialize valid data frame");
    }

    // Verify fields
    if (parsed->type != v2::FrameType::DATA) FAIL("Type mismatch");
    if (parsed->seq != 5) FAIL("Sequence mismatch");
    if (parsed->payload != payload) FAIL("Payload mismatch");

    PASS();
    return true;
}

bool test_frame_crc() {
    TEST("v2 Frame CRC validation");

    auto probe = v2::ControlFrame::makeProbe("TEST1", "TEST2");
    Bytes data = probe.serialize();

    // Valid frame should parse
    auto valid = v2::ControlFrame::deserialize(data);
    if (!valid) FAIL("Valid frame rejected");

    // Corrupt one byte in middle
    data[10] ^= 0xFF;
    auto corrupt = v2::ControlFrame::deserialize(data);
    if (corrupt) FAIL("Corrupt frame accepted");

    PASS();
    return true;
}

bool test_control_frame_types() {
    TEST("All v2 control frame types");

    // Test ControlFrames (1 codeword)
    std::vector<std::pair<v2::ControlFrame, v2::FrameType>> control_cases = {
        { v2::ControlFrame::makeProbe("A", "B"), v2::FrameType::PROBE },
        { v2::ControlFrame::makeAck("A", "B", 1), v2::FrameType::ACK },
        { v2::ControlFrame::makeNack("A", "B", 1, 0), v2::FrameType::NACK },
        { v2::ControlFrame::makeDisconnect("A", "B"), v2::FrameType::DISCONNECT },
        { v2::ControlFrame::makeKeepalive("A", "B"), v2::FrameType::KEEPALIVE },
        { v2::ControlFrame::makeModeChange("A", "B", 1, ultra::Modulation::QAM16, ultra::CodeRate::R2_3, 20.0f, 0.50f, 0), v2::FrameType::MODE_CHANGE },
    };

    for (const auto& [frame, expected_type] : control_cases) {
        Bytes data = frame.serialize();
        auto parsed = v2::ControlFrame::deserialize(data);
        if (!parsed) FAIL("Failed to parse control frame");
        if (parsed->type != expected_type) FAIL("Type mismatch");
    }

    // Test ConnectFrames (3 codewords)
    std::vector<std::pair<v2::ConnectFrame, v2::FrameType>> connect_cases = {
        { v2::ConnectFrame::makeConnect("CALLSIGN1", "CALLSIGN2", 0x07, 0), v2::FrameType::CONNECT },
        { v2::ConnectFrame::makeConnectAck("CALLSIGN1", "CALLSIGN2", 0, ultra::Modulation::DQPSK, ultra::CodeRate::R1_4, 15.0f, 0.60f, 4), v2::FrameType::CONNECT_ACK },
        { v2::ConnectFrame::makeConnectNak("CALLSIGN1", "CALLSIGN2"), v2::FrameType::CONNECT_NAK },
        { v2::ConnectFrame::makeDisconnect("CALLSIGN1", "CALLSIGN2"), v2::FrameType::DISCONNECT },
    };

    for (const auto& [frame, expected_type] : connect_cases) {
        Bytes data = frame.serialize();
        auto parsed = v2::ConnectFrame::deserialize(data);
        if (!parsed) FAIL("Failed to parse connect frame");
        if (parsed->type != expected_type) FAIL("Type mismatch");
    }

    PASS();
    return true;
}

bool test_callsign_validation() {
    TEST("Callsign validation");

    // Valid callsigns
    if (!isValidCallsign("VA2MVR")) FAIL("Valid callsign rejected");
    if (!isValidCallsign("W1AW")) FAIL("Valid callsign rejected");
    if (!isValidCallsign("VE3ABC")) FAIL("Valid callsign rejected");

    // Invalid callsigns
    if (isValidCallsign("")) FAIL("Empty callsign accepted");
    if (isValidCallsign("AB")) FAIL("Too short callsign accepted");

    // Sanitization
    if (sanitizeCallsign("va2mvr") != "VA2MVR") FAIL("Sanitize should uppercase");

    PASS();
    return true;
}

bool test_callsign_hash() {
    TEST("Callsign hash");

    // Hash should be deterministic
    uint32_t h1 = v2::hashCallsign("VA2MVR");
    uint32_t h2 = v2::hashCallsign("VA2MVR");
    if (h1 != h2) FAIL("Hash not deterministic");

    // Different callsigns should (likely) have different hashes
    uint32_t h3 = v2::hashCallsign("VE3ABC");
    if (h1 == h3) FAIL("Different callsigns same hash");

    // Hash should be 24-bit
    if (h1 > 0xFFFFFF) FAIL("Hash exceeds 24 bits");

    PASS();
    return true;
}

// ============================================================================
// Two-Station Simulation
// ============================================================================

/**
 * Simulated link between two stations
 * TX from one goes to RX of the other (with optional delay/loss)
 */
class SimulatedChannel {
public:
    SimulatedChannel(ProtocolEngine& stationA, ProtocolEngine& stationB)
        : stationA_(stationA), stationB_(stationB) {

        stationA_.setTxDataCallback([this](const Bytes& data, bool) {
            if (verbose_) {
                std::cout << "    [A->B] " << data.size() << " bytes\n";
            }
            tx_count_a_++;
            if (!drop_a_to_b_) {
                pending_b_.push(data);
            }
        });

        stationB_.setTxDataCallback([this](const Bytes& data, bool) {
            if (verbose_) {
                std::cout << "    [B->A] " << data.size() << " bytes\n";
            }
            tx_count_b_++;
            if (!drop_b_to_a_) {
                pending_a_.push(data);
            }
        });
    }

    void deliver() {
        while (!pending_a_.empty()) {
            stationA_.onRxData(pending_a_.front());
            pending_a_.pop();
        }
        while (!pending_b_.empty()) {
            stationB_.onRxData(pending_b_.front());
            pending_b_.pop();
        }
    }

    void tick(uint32_t ms) {
        stationA_.tick(ms);
        stationB_.tick(ms);
    }

    void run(int cycles, uint32_t tick_ms = 100) {
        for (int i = 0; i < cycles; i++) {
            deliver();
            tick(tick_ms);
        }
    }

    void setDropAtoB(bool drop) { drop_a_to_b_ = drop; }
    void setDropBtoA(bool drop) { drop_b_to_a_ = drop; }
    void setVerbose(bool v) { verbose_ = v; }

    int getTxCountA() const { return tx_count_a_; }
    int getTxCountB() const { return tx_count_b_; }

private:
    ProtocolEngine& stationA_;
    ProtocolEngine& stationB_;

    std::queue<Bytes> pending_a_;
    std::queue<Bytes> pending_b_;

    bool drop_a_to_b_ = false;
    bool drop_b_to_a_ = false;
    bool verbose_ = false;

    int tx_count_a_ = 0;
    int tx_count_b_ = 0;
};

bool test_connection_establishment() {
    TEST("Connection establishment");

    ConnectionConfig config;
    config.auto_accept = true;
    config.connect_timeout_ms = 5000;
    config.connect_retries = 3;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("TEST1A");
    stationB.setLocalCallsign("TEST2B");

    bool a_connected = false;
    bool b_connected = false;

    stationA.setConnectionChangedCallback([&](ConnectionState state, const std::string&) {
        if (state == ConnectionState::CONNECTED) a_connected = true;
    });

    stationB.setConnectionChangedCallback([&](ConnectionState state, const std::string&) {
        if (state == ConnectionState::CONNECTED) b_connected = true;
    });

    SimulatedChannel channel(stationA, stationB);

    if (!stationA.connect("TEST2B")) {
        FAIL("Connect() returned false");
    }

    for (int i = 0; i < 50 && (!a_connected || !b_connected); i++) {
        channel.run(1, 100);
    }

    if (!a_connected) FAIL("Station A did not connect");
    if (!b_connected) FAIL("Station B did not connect");

    PASS();
    return true;
}

bool test_nonphysical_snr_sources_do_not_drive_negotiation() {
    TEST("Non-physical SNR sources do not drive negotiation");

    ConnectionConfig config;
    config.auto_accept = true;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("TEST1A");
    stationB.setLocalCallsign("TEST2B");

    stationB.setMeasuredSNR(30.0f, ultra::SNRSource::SYNC_QUALITY);
    stationB.setMeasuredSNR(35.0f, ultra::SNRSource::OFDM_INTERNAL);

    SimulatedChannel channel(stationA, stationB);

    if (!stationA.connect("TEST2B")) {
        FAIL("Connect() returned false");
    }

    channel.run(50, 100);

    if (!stationA.isConnected()) FAIL("Station A did not connect");
    if (!stationB.isConnected()) FAIL("Station B did not connect");
    if (stationB.getMeasuredSNRSource() == ultra::SNRSource::SYNC_QUALITY) {
        FAIL("SYNC_QUALITY was stored as rate-selection SNR");
    }
    if (stationB.getMeasuredSNRSource() == ultra::SNRSource::OFDM_INTERNAL) {
        FAIL("OFDM_INTERNAL was stored as rate-selection SNR");
    }
    if (stationB.getNegotiatedMode() != WaveformMode::MC_DPSK) {
        FAIL("non-physical SNR promoted responder out of MC-DPSK fallback");
    }
    if (stationA.getNegotiatedMode() != WaveformMode::MC_DPSK) {
        FAIL("non-physical SNR promoted initiator out of MC-DPSK fallback");
    }

    PASS();
    return true;
}

bool test_data_transfer() {
    TEST("Data transfer with ACK");

    ConnectionConfig config;
    config.auto_accept = true;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    std::vector<std::string> received_at_b;

    stationB.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received_at_b.push_back(text);
    });

    SimulatedChannel channel(stationA, stationB);

    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected() || !stationB.isConnected()) {
        FAIL("Connection not established");
    }

    if (!stationA.sendMessage("Hello Bob!")) {
        FAIL("sendMessage() returned false");
    }

    channel.run(30, 100);

    if (received_at_b.empty()) FAIL("No message received at B");
    if (received_at_b[0] != "Hello Bob!") FAIL("Message content mismatch");

    PASS();
    return true;
}

bool test_bidirectional_transfer() {
    TEST("Bidirectional data transfer");

    ConnectionConfig config;
    config.auto_accept = true;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    std::vector<std::string> received_at_a;
    std::vector<std::string> received_at_b;

    stationA.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received_at_a.push_back(text);
    });

    stationB.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received_at_b.push_back(text);
    });

    SimulatedChannel channel(stationA, stationB);

    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected()) FAIL("Not connected");

    stationA.sendMessage("Hello from Alice");
    channel.run(30, 100);

    stationB.sendMessage("Hello from Bob");
    channel.run(30, 100);

    if (received_at_b.size() != 1 || received_at_b[0] != "Hello from Alice") {
        FAIL("B didn't receive Alice's message");
    }

    if (received_at_a.size() != 1 || received_at_a[0] != "Hello from Bob") {
        FAIL("A didn't receive Bob's message");
    }

    PASS();
    return true;
}

bool test_retransmission() {
    TEST("Retransmission on timeout");

    ConnectionConfig config;
    config.auto_accept = true;
    config.arq.ack_timeout_ms = 500;
    config.arq.max_retries = 3;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    std::vector<std::string> received_at_b;
    stationB.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received_at_b.push_back(text);
    });

    SimulatedChannel channel(stationA, stationB);

    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected()) FAIL("Not connected");

    channel.setDropBtoA(true);

    stationA.sendMessage("Test retransmit");

    int initial_tx = channel.getTxCountA();
    channel.run(20, 100);

    if (channel.getTxCountA() <= initial_tx + 1) {
        channel.setDropBtoA(false);
        channel.run(20, 100);
    }

    channel.setDropBtoA(false);
    channel.run(30, 100);

    if (received_at_b.empty()) FAIL("Message never received despite retransmits");

    PASS();
    return true;
}

bool test_disconnect() {
    TEST("Graceful disconnect");

    ConnectionConfig config;
    config.auto_accept = true;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    bool a_disconnected = false;
    bool b_disconnected = false;

    stationA.setConnectionChangedCallback([&](ConnectionState state, const std::string&) {
        if (state == ConnectionState::DISCONNECTED) a_disconnected = true;
    });

    stationB.setConnectionChangedCallback([&](ConnectionState state, const std::string&) {
        if (state == ConnectionState::DISCONNECTED) b_disconnected = true;
    });

    SimulatedChannel channel(stationA, stationB);

    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected()) FAIL("Not connected");

    stationA.disconnect();
    channel.run(20, 100);

    // The responder intentionally stays connected during a disconnect grace
    // period so it can re-send the final ACK if the initiator retransmits the
    // DISCONNECT. Advance beyond that production grace window before asserting.
    channel.run(60, 100);

    if (!a_disconnected && stationA.isConnected()) FAIL("A not disconnected");
    if (!b_disconnected && stationB.isConnected()) FAIL("B not disconnected");

    PASS();
    return true;
}

bool test_manual_accept() {
    TEST("Manual call accept/reject");

    ConnectionConfig config;
    config.auto_accept = false;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    bool incoming_call = false;
    std::string incoming_from;

    stationB.setIncomingCallCallback([&](const std::string& from) {
        incoming_call = true;
        incoming_from = from;
    });

    SimulatedChannel channel(stationA, stationB);

    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!incoming_call) FAIL("No incoming call notification");

    stationB.acceptCall();
    channel.run(30, 100);

    if (!stationA.isConnected()) FAIL("A not connected after accept");
    if (!stationB.isConnected()) FAIL("B not connected after accept");

    PASS();
    return true;
}

bool test_multiple_messages() {
    TEST("Multiple sequential messages");

    ConnectionConfig config;
    config.auto_accept = true;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    std::vector<std::string> received;
    stationB.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received.push_back(text);
    });

    SimulatedChannel channel(stationA, stationB);

    stationA.connect("K2DEF");
    channel.run(30, 100);

    const int NUM_MESSAGES = 5;
    for (int i = 0; i < NUM_MESSAGES; i++) {
        int attempts = 0;
        while (!stationA.isReadyToSend() && attempts < 50) {
            channel.run(1, 100);
            attempts++;
        }

        std::string msg = "Message " + std::to_string(i + 1);
        stationA.sendMessage(msg);
        channel.run(20, 100);
    }

    channel.run(30, 100);

    if (received.size() != NUM_MESSAGES) {
        std::cout << "(received " << received.size() << "/" << NUM_MESSAGES << ") ";
        FAIL("Not all messages received");
    }

    for (int i = 0; i < NUM_MESSAGES; i++) {
        std::string expected = "Message " + std::to_string(i + 1);
        if (received[i] != expected) FAIL("Message order/content wrong");
    }

    PASS();
    return true;
}

bool test_quick_brown_fox() {
    TEST("Quick Brown Fox test message");

    ConnectionConfig config;
    config.auto_accept = true;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    std::string received_msg;

    stationB.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received_msg = text;
    });

    SimulatedChannel channel(stationA, stationB);

    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected()) FAIL("Not connected");

    const std::string fox_msg = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 1234567890";
    stationA.sendMessage(fox_msg);
    channel.run(30, 100);

    if (received_msg.empty()) FAIL("No message received");
    if (received_msg != fox_msg) {
        std::cout << "\n    Expected: " << fox_msg << "\n";
        std::cout << "    Received: " << received_msg << "\n";
        FAIL("Message content mismatch");
    }

    PASS();
    return true;
}

bool test_phy_mask_v1_negotiation() {
    TEST("PHY_MASK_V1 negotiation gates CarrierLDPC");

    ConnectionConfig modern;
    modern.auto_accept = true;

    ProtocolEngine stationA(modern);
    ProtocolEngine stationB(modern);
    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    bool a_enabled = false;
    bool b_enabled = false;
    stationA.setPhyMaskV1NegotiatedCallback([&](bool enabled) { a_enabled = enabled; });
    stationB.setPhyMaskV1NegotiatedCallback([&](bool enabled) { b_enabled = enabled; });

    SimulatedChannel channel(stationA, stationB);
    stationA.connect("K2DEF");
    channel.run(50, 100);

    if (!stationA.isConnected() || !stationB.isConnected()) FAIL("modern stations did not connect");
    if (!stationA.isPhyMaskV1Negotiated() || !stationB.isPhyMaskV1Negotiated()) {
        FAIL("modern stations did not negotiate PHY_MASK_V1");
    }
    if (!a_enabled || !b_enabled) FAIL("PHY_MASK_V1 callback not raised on both sides");

    ConnectionConfig legacy = modern;
    legacy.mode_capabilities = ModeCapabilities::ALL;

    ProtocolEngine stationC(modern);
    ProtocolEngine stationD(legacy);
    stationC.setLocalCallsign("W3ABC");
    stationD.setLocalCallsign("K4DEF");

    bool legacy_enabled = false;
    stationC.setPhyMaskV1NegotiatedCallback([&](bool enabled) { legacy_enabled = legacy_enabled || enabled; });
    stationD.setPhyMaskV1NegotiatedCallback([&](bool enabled) { legacy_enabled = legacy_enabled || enabled; });

    SimulatedChannel legacy_channel(stationC, stationD);
    stationC.connect("K4DEF");
    legacy_channel.run(50, 100);

    if (!stationC.isConnected() || !stationD.isConnected()) FAIL("legacy pair did not connect");
    if (stationC.isPhyMaskV1Negotiated() || stationD.isPhyMaskV1Negotiated()) {
        FAIL("PHY_MASK_V1 negotiated with legacy peer");
    }
    if (legacy_enabled) FAIL("PHY_MASK_V1 callback enabled with legacy peer");

    PASS();
    return true;
}

// ============================================================================
// Binary stream / TNC-facing API Tests
// ============================================================================

bool test_binary_fragment_reassembly_single_callback() {
    TEST("Binary fragment reassembly emits one data callback");

    ConnectionConfig config;
    config.auto_accept = true;
    config.mode_capabilities = ModeCapabilities::OFDM_COX;
    config.preferred_mode = WaveformMode::OFDM_COX;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    int data_callbacks = 0;
    int message_callbacks = 0;
    bool saw_more_data = false;
    Bytes received;

    stationB.setDataReceivedCallback([&](const Bytes& data, bool more_data) {
        data_callbacks++;
        saw_more_data = saw_more_data || more_data;
        received = data;
    });
    stationB.setMessageReceivedCallback([&](const std::string&, const std::string&) {
        message_callbacks++;
    });

    SimulatedChannel channel(stationA, stationB);

    stationA.connect("K2DEF");
    channel.run(50, 100);

    if (!stationA.isConnected() || !stationB.isConnected()) FAIL("Connection not established");

    Bytes payload(1024);
    payload[0] = static_cast<uint8_t>(PayloadType::FILE_START);
    payload[1] = static_cast<uint8_t>(PayloadType::FILE_DATA);
    payload[2] = static_cast<uint8_t>(PayloadType::FILE_BLOCK);
    for (size_t i = 3; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    }

    if (!stationA.sendBinary(payload)) FAIL("sendBinary() returned false");

    for (int i = 0; i < 300 && data_callbacks == 0; ++i) {
        channel.run(1, 50);
    }
    channel.run(60, 50);

    if (data_callbacks != 1) {
        std::cout << "(callbacks " << data_callbacks << ") ";
        FAIL("Expected exactly one reassembled data callback");
    }
    if (saw_more_data) FAIL("Data callback should only receive complete payloads");
    if (message_callbacks != 0) FAIL("Binary payload should not be delivered as text");
    if (received != payload) FAIL("Reassembled binary payload mismatch");

    PASS();
    return true;
}

bool test_send_binary_roundtrip_arbitrary_bytes() {
    TEST("sendBinary roundtrip preserves arbitrary bytes");

    ConnectionConfig config;
    config.auto_accept = true;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");
    stationB.setMeasuredSNR(25.0f);  // Keep this byte-preservation test on OFDM.

    Bytes received;
    stationB.setDataReceivedCallback([&](const Bytes& data, bool more_data) {
        if (!more_data) {
            received = data;
        }
    });

    SimulatedChannel channel(stationA, stationB);

    stationA.connect("K2DEF");
    channel.run(50, 100);

    if (!stationA.isConnected() || !stationB.isConnected()) FAIL("Connection not established");

    Bytes payload;
    payload.reserve(260);
    payload.push_back(static_cast<uint8_t>(PayloadType::FILE_START));
    payload.push_back(0x00);
    payload.push_back(0xFF);
    payload.push_back(static_cast<uint8_t>(PayloadType::FILE_DATA));
    payload.push_back(static_cast<uint8_t>(PayloadType::FILE_BLOCK));
    for (int i = 0; i < 255; ++i) {
        payload.push_back(static_cast<uint8_t>(i));
    }

    if (!stationA.sendBinary(payload)) FAIL("sendBinary() returned false");

    for (int i = 0; i < 360 && received.empty(); ++i) {
        channel.run(1, 50);
    }

    if (received != payload) FAIL("Binary roundtrip payload mismatch");

    PASS();
    return true;
}

bool test_tx_backlog_bytes_snapshot() {
    TEST("TX backlog byte snapshot");

    ConnectionConfig config;
    config.auto_accept = true;
    config.mode_capabilities = ModeCapabilities::OFDM_COX;
    config.preferred_mode = WaveformMode::OFDM_COX;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    Bytes received;
    stationB.setDataReceivedCallback([&](const Bytes& data, bool more_data) {
        if (!more_data) {
            received = data;
        }
    });

    SimulatedChannel channel(stationA, stationB);

    stationA.connect("K2DEF");
    channel.run(50, 100);

    if (!stationA.isConnected() || !stationB.isConnected()) FAIL("Connection not established");
    if (stationA.getTxBacklogBytes() != 0) FAIL("Idle backlog should be zero");

    const size_t capacity = v2::getFixedFramePayloadCapacity(
        stationA.getDataCodeRate(), stationA.getForcedFrameCodewords());
    if (capacity == 0) FAIL("Invalid fixed-frame capacity");

    Bytes payload(capacity * 5 + 13);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>((i * 19 + 7) & 0xFF);
    }

    if (!stationA.sendBinary(payload)) FAIL("sendBinary() returned false");

    const size_t backlog_after_queue = stationA.getTxBacklogBytes();
    if (backlog_after_queue != payload.size()) {
        std::cout << "(expected " << payload.size()
                  << ", got " << backlog_after_queue << ") ";
        FAIL("Queued backlog should match payload bytes");
    }

    for (int i = 0; i < 360 && (received.empty() || stationA.getTxBacklogBytes() != 0); ++i) {
        channel.run(1, 50);
    }

    if (received != payload) FAIL("Backlog test payload not received");
    if (stationA.getTxBacklogBytes() != 0) FAIL("Backlog should return to zero after ACK drain");

    PASS();
    return true;
}

// ============================================================================
// Protocol Rate Adaptation Tests
// ============================================================================

bool test_protocol_rate_upgrade() {
    TEST("Automatic MODE_CHANGE on good channel");

    ConnectionConfig config;
    config.auto_accept = true;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    // Station B measures good in-band SNR - should trigger MODE_CHANGE.
    // In-band SNR=32 + AWGN promotes to D8PSK while staying below the
    // initial R3/4 cap.
    stationB.setMeasuredSNR(32.0f);

    bool a_mode_changed = false;
    ultra::Modulation a_new_mod = ultra::Modulation::DQPSK;
    ultra::CodeRate a_new_rate = ultra::CodeRate::R1_4;

    stationA.setDataModeChangedCallback([&](ultra::Modulation mod, ultra::CodeRate rate,
                                             int cw_count, float snr, float peer_fading,
                                             int mc_dpsk_num_carriers,
                                             int mc_dpsk_samples_per_symbol) {
        (void)cw_count;
        (void)snr;
        (void)peer_fading;
        (void)mc_dpsk_num_carriers;
        (void)mc_dpsk_samples_per_symbol;
        a_mode_changed = true;
        a_new_mod = mod;
        a_new_rate = rate;
    });

    SimulatedChannel channel(stationA, stationB);

    // Connect - should trigger automatic MODE_CHANGE from B
    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected()) FAIL("Not connected");
    if (!stationB.isConnected()) FAIL("B not connected");

    // Verify MODE_CHANGE was received
    if (!a_mode_changed) FAIL("No MODE_CHANGE received at A");
    if (a_new_mod != ultra::Modulation::D8PSK) {
        std::cout << "(got " << ultra::modulationToString(a_new_mod) << ") ";
        FAIL("Expected D8PSK at 32 dB in-band SNR");
    }
    // Bootstrap rate cap: capInitialOFDMRate keeps R3/4 only at in-band SNR>=34
    // AND fading<0.05. At SNR=32 the cap returns R2/3, so the expected
    // outcome is D8PSK R2/3 not D8PSK R3/4. (See waveform_selection.hpp
    // line 64.)
    if (a_new_rate != ultra::CodeRate::R2_3) {
        std::cout << "(got " << ultra::codeRateToString(a_new_rate) << ") ";
        FAIL("Expected R2/3 at 32 dB in-band SNR");
    }

    // Verify both stations report same mode
    if (stationA.getDataCodeRate() != ultra::CodeRate::R2_3) FAIL("A has wrong code rate");
    if (stationB.getDataCodeRate() != ultra::CodeRate::R2_3) FAIL("B has wrong code rate");

    PASS();
    return true;
}

bool test_adaptive_data_transfer() {
    TEST("Data transfer after mode upgrade");

    ConnectionConfig config;
    config.auto_accept = true;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    // Good fading at in-band SNR 28 should choose DQPSK R1/2. With default fading=0
    // this is classified as near-AWGN and can safely use R2/3.
    stationB.setChannelQuality(28.0f, 0.30f);

    std::vector<std::string> received_at_b;
    stationB.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received_at_b.push_back(text);
    });

    SimulatedChannel channel(stationA, stationB);

    // Connect with automatic MODE_CHANGE
    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected()) FAIL("Not connected");

    // Verify mode upgrade happened
    if (stationA.getDataCodeRate() != ultra::CodeRate::R1_2) {
        std::cout << "(got " << ultra::codeRateToString(stationA.getDataCodeRate()) << ") ";
        FAIL("Expected R1/2 at 28 dB in-band SNR");
    }

    // Send message - should work with upgraded mode
    if (!stationA.sendMessage("Hello at higher rate!")) {
        FAIL("sendMessage() failed");
    }
    channel.run(30, 100);

    if (received_at_b.empty()) FAIL("No message received");
    if (received_at_b[0] != "Hello at higher rate!") FAIL("Message content mismatch");

    PASS();
    return true;
}

bool test_adaptive_bidirectional() {
    TEST("Bidirectional transfer with adaptive mode");

    ConnectionConfig config;
    config.auto_accept = true;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    // Excellent near-AWGN in-band SNR should use high-rate DPSK. Coherent QAM is no
    // longer the automatic HF default.
    stationB.setMeasuredSNR(37.0f);

    std::vector<std::string> received_at_a;
    std::vector<std::string> received_at_b;

    stationA.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received_at_a.push_back(text);
    });
    stationB.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received_at_b.push_back(text);
    });

    SimulatedChannel channel(stationA, stationB);

    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected()) FAIL("Not connected");

    // Verify high-rate mode. D8PSK ladder re-enabled 2026-05-04
    // (see waveform_selection.hpp): high-SNR AWGN picks D8PSK R3/4
    // (recommendDataMode result), but bootstrap cap (capInitialOFDMRate)
    // requires in-band SNR>=34 to keep R3/4. SNR=37 with fading=0 satisfies
    // both, so the expected outcome is D8PSK R3/4.
    if (stationA.getDataModulation() != ultra::Modulation::D8PSK) {
        std::cout << "(got " << ultra::modulationToString(stationA.getDataModulation()) << ") ";
        FAIL("Expected D8PSK at 37 dB in-band SNR");
    }
    if (stationA.getDataCodeRate() != ultra::CodeRate::R3_4) {
        std::cout << "(got " << ultra::codeRateToString(stationA.getDataCodeRate()) << ") ";
        FAIL("Expected R3/4 at 37 dB in-band SNR");
    }

    // Bidirectional transfer
    stationA.sendMessage("High-speed from A");
    channel.run(30, 100);

    stationB.sendMessage("High-speed from B");
    channel.run(30, 100);

    if (received_at_b.empty() || received_at_b[0] != "High-speed from A") {
        FAIL("B didn't receive A's message");
    }
    if (received_at_a.empty() || received_at_a[0] != "High-speed from B") {
        FAIL("A didn't receive B's message");
    }

    PASS();
    return true;
}

// ============================================================================
// File Transfer Tests
// ============================================================================

std::string createTestFile(const std::filesystem::path& dir, const std::string& name, size_t size) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return "";

    std::string path = (dir / name).string();
    std::ofstream f(path, std::ios::binary);
    if (!f) return "";

    for (size_t i = 0; i < size; i++) {
        f.put(static_cast<char>((i * 7 + 13) & 0xFF));
    }
    f.close();
    return path;
}

bool verifyFileContent(const std::string& path, size_t expected_size) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    f.seekg(0, std::ios::end);
    size_t actual_size = f.tellg();
    f.seekg(0, std::ios::beg);

    if (actual_size != expected_size) return false;

    for (size_t i = 0; i < expected_size; i++) {
        char c;
        f.get(c);
        uint8_t expected = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
        if (static_cast<uint8_t>(c) != expected) return false;
    }
    return true;
}

bool test_file_transfer_small() {
    TEST("Small file transfer (100 bytes)");

    ConnectionConfig config;
    config.auto_accept = true;
    config.arq.ack_timeout_ms = 200;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    const size_t FILE_SIZE = 100;
    ultra::test::TempDir temp_dir("ultra_protocol_test");
    if (!temp_dir.valid()) FAIL("Could not create temp test directory");
    const auto& test_dir = temp_dir.path();

    std::string src_path = createTestFile(test_dir, "test_small.bin", FILE_SIZE);
    if (src_path.empty()) FAIL("Could not create test file");

    std::string rx_dir = (test_dir / "rx").string();
    std::filesystem::create_directories(rx_dir);
    stationB.setReceiveDirectory(rx_dir);

    bool file_received = false;
    std::string received_path;
    bool receive_success = false;

    stationB.setFileReceivedCallback([&](const std::string& path, bool success) {
        file_received = true;
        received_path = path;
        receive_success = success;
    });

    bool file_sent = false;
    bool send_success = false;

    stationA.setFileSentCallback([&](bool success, const std::string&) {
        file_sent = true;
        send_success = success;
    });

    SimulatedChannel channel(stationA, stationB);

    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected()) FAIL("Not connected");

    if (!stationA.sendFile(src_path)) FAIL("sendFile() returned false");

    for (int i = 0; i < 200 && (!file_received || !file_sent); i++) {
        channel.run(1, 50);
    }

    if (!file_sent) FAIL("File not sent (no callback)");
    if (!send_success) FAIL("File send reported failure");
    if (!file_received) FAIL("File not received (no callback)");
    if (!receive_success) FAIL("File receive reported failure");

    if (!verifyFileContent(received_path, FILE_SIZE)) FAIL("File content mismatch");

    PASS();
    return true;
}

// ============================================================================
// Compression Tests
// ============================================================================

bool test_compression_basic() {
    TEST("Basic compression/decompression");

    std::string text = "Hello World! This is a test of the compression system. ";
    for (int i = 0; i < 10; i++) {
        text += text;
    }
    Bytes input(text.begin(), text.end());

    auto compressed = Compression::compress(input);
    if (!compressed) FAIL("Compression failed");
    if (compressed->size() >= input.size()) FAIL("Compression didn't reduce size");

    auto decompressed = Compression::decompress(*compressed, input.size() * 2);
    if (!decompressed) FAIL("Decompression failed");
    if (*decompressed != input) FAIL("Decompressed data doesn't match original");

    PASS();
    return true;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== Protocol Test Suite (v2 Frames) ===\n\n";

    std::cout << "v2 Frame Tests:\n";
    test_control_frame_serialization();
    test_data_frame_serialization();
    test_frame_crc();
    test_control_frame_types();
    test_callsign_validation();
    test_callsign_hash();

    std::cout << "\nTwo-Station Simulation:\n";
    test_connection_establishment();
    test_nonphysical_snr_sources_do_not_drive_negotiation();
    test_data_transfer();
    test_bidirectional_transfer();
    test_retransmission();
    test_disconnect();
    test_manual_accept();
    test_multiple_messages();

    std::cout << "\nCapability Negotiation:\n";
    test_phy_mask_v1_negotiation();

    std::cout << "\nRadio Test Messages:\n";
    test_quick_brown_fox();

    std::cout << "\nBinary Stream / TNC API Tests:\n";
    test_binary_fragment_reassembly_single_callback();
    test_send_binary_roundtrip_arbitrary_bytes();
    test_tx_backlog_bytes_snapshot();

    std::cout << "\nProtocol Rate Adaptation Tests:\n";
    test_protocol_rate_upgrade();
    test_adaptive_data_transfer();
    test_adaptive_bidirectional();

    std::cout << "\nFile Transfer Tests:\n";
    test_file_transfer_small();

    std::cout << "\nCompression Tests:\n";
    test_compression_basic();

    std::cout << "\n=== Results: " << tests_passed << "/" << tests_run << " passed ===\n";

    return (tests_passed == tests_run) ? 0 : 1;
}
