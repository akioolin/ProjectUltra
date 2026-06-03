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
#include <algorithm>

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
        { v2::ControlFrame::makeFileCancel("A", "B"), v2::FrameType::FILE_CANCEL },
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
            observeControlFrame(data, true);
            if (shouldDropNextFileCancel(data, true)) {
                return;
            }
            if (!drop_a_to_b_) {
                pending_b_.push(data);
            }
        });

        stationB_.setTxDataCallback([this](const Bytes& data, bool) {
            if (verbose_) {
                std::cout << "    [B->A] " << data.size() << " bytes\n";
            }
            tx_count_b_++;
            observeControlFrame(data, false);
            if (shouldDropNextFileCancel(data, false)) {
                return;
            }
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
    void dropNextFileCancelAtoB() { drop_next_file_cancel_a_to_b_ = true; }
    void dropNextFileCancelBtoA() { drop_next_file_cancel_b_to_a_ = true; }
    void setVerbose(bool v) { verbose_ = v; }

    int getTxCountA() const { return tx_count_a_; }
    int getTxCountB() const { return tx_count_b_; }
    int getTurnoverCountA() const { return turnover_count_a_; }
    int getTurnoverCountB() const { return turnover_count_b_; }
    int getTurnRequestCountA() const { return turn_request_count_a_; }
    int getTurnRequestCountB() const { return turn_request_count_b_; }
    int getFileCancelCountA() const { return file_cancel_count_a_; }
    int getFileCancelCountB() const { return file_cancel_count_b_; }

private:
    bool shouldDropNextFileCancel(const Bytes& data, bool from_a) {
        auto ctrl = v2::ControlFrame::deserialize(data);
        if (!ctrl || ctrl->type != v2::FrameType::FILE_CANCEL) {
            return false;
        }
        bool& drop_next = from_a ? drop_next_file_cancel_a_to_b_
                                 : drop_next_file_cancel_b_to_a_;
        if (!drop_next) {
            return false;
        }
        drop_next = false;
        return true;
    }

    void observeControlFrame(const Bytes& data, bool from_a) {
        auto ctrl = v2::ControlFrame::deserialize(data);
        if (!ctrl) {
            return;
        }
        if (ctrl->type == v2::FrameType::TURNOVER) {
            if (from_a) {
                turnover_count_a_++;
            } else {
                turnover_count_b_++;
            }
        } else if (ctrl->type == v2::FrameType::TURN_REQUEST) {
            if (from_a) {
                turn_request_count_a_++;
            } else {
                turn_request_count_b_++;
            }
        } else if (ctrl->type == v2::FrameType::FILE_CANCEL) {
            if (from_a) {
                file_cancel_count_a_++;
            } else {
                file_cancel_count_b_++;
            }
        }
    }

    ProtocolEngine& stationA_;
    ProtocolEngine& stationB_;

    std::queue<Bytes> pending_a_;
    std::queue<Bytes> pending_b_;

    bool drop_a_to_b_ = false;
    bool drop_b_to_a_ = false;
    bool drop_next_file_cancel_a_to_b_ = false;
    bool drop_next_file_cancel_b_to_a_ = false;
    bool verbose_ = false;

    int tx_count_a_ = 0;
    int tx_count_b_ = 0;
    int turnover_count_a_ = 0;
    int turnover_count_b_ = 0;
    int turn_request_count_a_ = 0;
    int turn_request_count_b_ = 0;
    int file_cancel_count_a_ = 0;
    int file_cancel_count_b_ = 0;
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

bool test_message_tx_status_callbacks() {
    TEST("Message TX status uses ARQ sequence IDs and cumulative ACK delivery");

    ConnectionConfig config;
    config.auto_accept = true;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    std::vector<ProtocolEngine::MessageTxStatusEvent> tx_events;
    std::vector<std::string> received_at_b;

    stationA.setMessageTxStatusCallback(
        [&](const ProtocolEngine::MessageTxStatusEvent& event) {
            tx_events.push_back(event);
        });
    stationB.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received_at_b.push_back(text);
    });

    SimulatedChannel channel(stationA, stationB);

    stationA.connect("K2DEF");
    channel.run(40, 100);

    if (!stationA.isConnected() || !stationB.isConnected()) {
        FAIL("Connection not established");
    }

    if (!stationA.sendMessage("short operator message")) {
        FAIL("short sendMessage() returned false");
    }
    channel.run(60, 100);

    if (received_at_b.size() != 1 || received_at_b[0] != "short operator message") {
        FAIL("Short message not received exactly once");
    }
    if (tx_events.size() < 2) {
        FAIL("Short message did not emit submitted+delivered callbacks");
    }
    if (tx_events[0].status != ProtocolEngine::MessageTxStatus::SUBMITTED ||
        tx_events[1].status != ProtocolEngine::MessageTxStatus::DELIVERED) {
        FAIL("Short message status order was not SUBMITTED then DELIVERED");
    }
    if (tx_events[0].first_seq != tx_events[1].first_seq ||
        tx_events[1].first_seq != tx_events[1].last_seq) {
        FAIL("Short message should use one ARQ sequence id");
    }

    tx_events.clear();
    received_at_b.clear();

    std::string long_message;
    for (int i = 0; i < 120; ++i) {
        long_message += "Long operator message segment " + std::to_string(i) + ". ";
    }

    if (!stationA.sendMessage(long_message)) {
        FAIL("long sendMessage() returned false");
    }
    channel.run(300, 100);

    if (received_at_b.size() != 1 || received_at_b[0] != long_message) {
        FAIL("Long fragmented message not received exactly once");
    }
    if (tx_events.size() < 2) {
        FAIL("Long message did not emit submitted+delivered callbacks");
    }
    if (tx_events[0].status != ProtocolEngine::MessageTxStatus::SUBMITTED ||
        tx_events.back().status != ProtocolEngine::MessageTxStatus::DELIVERED) {
        FAIL("Long message status order was not SUBMITTED then DELIVERED");
    }
    if (tx_events[0].first_seq != tx_events.back().first_seq) {
        FAIL("Long message delivery did not preserve first ARQ sequence id");
    }
    if (tx_events.back().last_seq == tx_events.back().first_seq) {
        FAIL("Long message should span multiple ARQ sequence ids");
    }

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
    channel.run(120, 100);

    if (received_at_b.size() != 1 || received_at_b[0] != "Hello from Alice") {
        FAIL("B didn't receive Alice's message");
    }

    if (received_at_a.size() != 1 || received_at_a[0] != "Hello from Bob") {
        FAIL("A didn't receive Bob's message");
    }

    PASS();
    return true;
}

bool test_half_duplex_turn_taking_queues_irs_data() {
    TEST("Half-duplex turn-taking queues IRS DATA until TURNOVER");

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

    if (!stationA.isConnected() || !stationB.isConnected()) FAIL("Connection not established");

    if (!stationA.sendMessage("Alice owns first ISS turn")) FAIL("A sendMessage failed");
    channel.run(30, 100);
    if (received_at_b.size() != 1 || received_at_b[0] != "Alice owns first ISS turn") {
        FAIL("B did not receive A message");
    }

    const int b_tx_before_queue = channel.getTxCountB();
    const int b_turn_requests_before = channel.getTurnRequestCountB();
    if (!stationB.sendMessage("Bob queued while IRS")) FAIL("B sendMessage should queue");
    for (int i = 0; i < 80 && channel.getTurnRequestCountB() == b_turn_requests_before; i++) {
        channel.run(1, 100);
    }
    if (channel.getTurnRequestCountB() != b_turn_requests_before + 1) {
        FAIL("IRS did not emit a bounded TURN_REQUEST for queued DATA");
    }
    if (channel.getTxCountB() <= b_tx_before_queue) {
        FAIL("IRS did not transmit a TURN_REQUEST control before receiving DATA turn");
    }

    channel.run(100, 100);

    if (received_at_a.size() != 1 || received_at_a[0] != "Bob queued while IRS") {
        FAIL("A did not receive queued B message after TURNOVER");
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
    config.mode_capabilities = ModeCapabilities::OFDM_CHIRP;
    config.preferred_mode = WaveformMode::OFDM_CHIRP;

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
    config.mode_capabilities = ModeCapabilities::OFDM_CHIRP;
    config.preferred_mode = WaveformMode::OFDM_CHIRP;

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

std::string createPseudoRandomTestFile(const std::filesystem::path& dir,
                                       const std::string& name,
                                       size_t size) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return "";

    std::string path = (dir / name).string();
    std::ofstream f(path, std::ios::binary);
    if (!f) return "";

    uint32_t state = 0x13579BDFu;
    for (size_t i = 0; i < size; i++) {
        state = state * 1664525u + 1013904223u;
        f.put(static_cast<char>((state >> 24) & 0xFF));
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

bool filesEqual(const std::string& a, const std::string& b) {
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb) return false;

    constexpr size_t kBufSize = 4096;
    char ba[kBufSize];
    char bb[kBufSize];
    do {
        fa.read(ba, kBufSize);
        fb.read(bb, kBufSize);
        if (fa.gcount() != fb.gcount()) return false;
        if (!std::equal(ba, ba + fa.gcount(), bb)) return false;
    } while (fa && fb);

    return fa.eof() && fb.eof();
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

    stationB.setFileReceivedCallback([&](const std::string& path, bool success, const std::string&) {
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
    if (channel.getTurnoverCountA() != 0 || channel.getTurnoverCountB() != 0) {
        FAIL("One-way file transfer emitted an unexpected DATA TURNOVER");
    }

    PASS();
    return true;
}

bool test_file_transfer_queues_during_connect_guard() {
    TEST("File transfer queues during initial ISS guard");

    ConnectionConfig config;
    config.auto_accept = true;
    config.arq.ack_timeout_ms = 200;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    ultra::test::TempDir temp_dir("ultra_protocol_guard_file_test");
    if (!temp_dir.valid()) FAIL("Could not create temp test directory");
    const auto& test_dir = temp_dir.path();

    const size_t FILE_SIZE = 512;
    std::string src_path = createPseudoRandomTestFile(test_dir, "guard_source.bin", FILE_SIZE);
    if (src_path.empty()) FAIL("Could not create guard test file");

    std::string rx_dir = (test_dir / "rx").string();
    std::filesystem::create_directories(rx_dir);
    stationB.setReceiveDirectory(rx_dir);

    bool file_received = false;
    std::string received_path;
    bool receive_success = false;
    stationB.setFileReceivedCallback([&](const std::string& path, bool success, const std::string&) {
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

    for (int i = 0; i < 20 && (!stationA.isConnected() || !stationB.isConnected()); i++) {
        channel.run(1, 100);
    }

    if (!stationA.isConnected() || !stationB.isConnected()) FAIL("Connection not established");
    if (!stationA.sendFile(src_path)) FAIL("sendFile() should queue during initial guard");

    for (int i = 0; i < 500 && (!file_received || !file_sent); i++) {
        channel.run(1, 50);
    }

    if (!file_sent) FAIL("Queued file not sent");
    if (!send_success) FAIL("Queued file send reported failure");
    if (!file_received) FAIL("Queued file not received");
    if (!receive_success) FAIL("Queued file receive reported failure");
    if (!filesEqual(src_path, received_path)) FAIL("Queued file content mismatch");
    if (channel.getTurnoverCountA() != 0 || channel.getTurnoverCountB() != 0) {
        FAIL("One-way queued file emitted an unexpected DATA TURNOVER");
    }

    PASS();
    return true;
}

bool test_queued_file_yields_to_pending_peer_turn_request_before_start() {
    TEST("Queued file yields to pending peer turn request before start");

    ConnectionConfig config;
    config.auto_accept = true;
    config.arq.ack_timeout_ms = 200;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    ultra::test::TempDir temp_dir("ultra_protocol_queued_file_turn_test");
    if (!temp_dir.valid()) FAIL("Could not create temp test directory");
    const auto& test_dir = temp_dir.path();

    const size_t FILE_SIZE = 512;
    std::string src_path = createPseudoRandomTestFile(test_dir, "queued_file.bin", FILE_SIZE);
    if (src_path.empty()) FAIL("Could not create queued file test source");

    std::string rx_dir = (test_dir / "rx").string();
    std::filesystem::create_directories(rx_dir);
    stationB.setReceiveDirectory(rx_dir);

    std::vector<std::string> received_at_a;
    std::vector<std::string> received_at_b;
    stationA.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received_at_a.push_back(text);
    });
    stationB.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received_at_b.push_back(text);
    });

    bool file_received = false;
    bool receive_success = false;
    std::string received_path;
    stationB.setFileReceivedCallback([&](const std::string& path, bool success, const std::string&) {
        file_received = true;
        receive_success = success;
        received_path = path;
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

    if (!stationA.isConnected() || !stationB.isConnected()) FAIL("Connection not established");
    if (!stationA.sendMessage("A first")) FAIL("A sendMessage failed");
    if (!stationB.sendMessage("B queued before file")) FAIL("B sendMessage should queue");

    // Deliver B's TURN_REQUEST and A's DATA, but queue the file before A sees
    // the ACK that clears its in-flight message. This reproduces the GUI
    // stall where queued_file_path_ and peer_data_turn_requested_ blocked each
    // other.
    channel.deliver();
    if (!stationA.sendFile(src_path)) FAIL("A sendFile() should queue behind pending ACK");

    for (int i = 0; i < 400 && received_at_a.empty(); i++) {
        channel.run(1, 50);
    }
    if (channel.getTurnoverCountA() < 1) {
        FAIL("A did not yield DATA turn for peer request while file was queued");
    }
    if (received_at_a.empty() || received_at_a[0] != "B queued before file") {
        FAIL("A did not receive B's queued message before starting queued file");
    }

    for (int i = 0; i < 800 && (!file_received || !file_sent); i++) {
        channel.run(1, 50);
    }

    if (!file_sent) FAIL("Queued file was not sent after peer turn drained");
    if (!send_success) FAIL("Queued file send reported failure");
    if (!file_received) FAIL("Queued file was not received");
    if (!receive_success) FAIL("Queued file receive reported failure");
    if (!filesEqual(src_path, received_path)) FAIL("Queued file content mismatch");

    PASS();
    return true;
}

bool test_queued_file_preempts_deferred_chat_after_current_payload() {
    TEST("Queued file starts before deferred chat after current payload drains");

    ConnectionConfig config;
    config.auto_accept = true;
    config.arq.ack_timeout_ms = 200;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    ultra::test::TempDir temp_dir("ultra_protocol_file_chat_priority_test");
    if (!temp_dir.valid()) FAIL("Could not create temp test directory");
    const auto& test_dir = temp_dir.path();

    const size_t FILE_SIZE = 512;
    std::string src_path = createPseudoRandomTestFile(test_dir, "priority_file.bin", FILE_SIZE);
    if (src_path.empty()) FAIL("Could not create test source file");

    std::string rx_dir = (test_dir / "rx").string();
    std::filesystem::create_directories(rx_dir);
    stationB.setReceiveDirectory(rx_dir);

    std::vector<std::string> b_events;
    stationB.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        b_events.push_back("msg:" + text);
    });

    bool file_received = false;
    bool receive_success = false;
    std::string received_path;
    stationB.setFileReceivedCallback([&](const std::string& path, bool success, const std::string&) {
        file_received = true;
        receive_success = success;
        received_path = path;
        b_events.push_back(success ? "file:ok" : "file:fail");
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

    if (!stationA.isConnected() || !stationB.isConnected()) FAIL("Connection not established");
    if (!stationA.sendMessage("A current")) FAIL("A current sendMessage failed");
    if (!stationA.sendMessage("A deferred chat")) FAIL("A deferred chat should queue");
    if (!stationA.sendFile(src_path)) FAIL("A sendFile() should queue behind current payload");

    for (int i = 0; i < 1200 && (!file_received || !file_sent); i++) {
        channel.run(1, 50);
    }

    if (!file_sent) FAIL("Queued file was not sent");
    if (!send_success) FAIL("Queued file send reported failure");
    if (!file_received) FAIL("Queued file was not received");
    if (!receive_success) FAIL("Queued file receive reported failure");
    if (!filesEqual(src_path, received_path)) FAIL("Queued file content mismatch");

    auto file_it = std::find(b_events.begin(), b_events.end(), "file:ok");
    auto deferred_it = std::find(b_events.begin(), b_events.end(), "msg:A deferred chat");
    if (file_it == b_events.end()) FAIL("File event missing from receive order");
    if (deferred_it != b_events.end() && deferred_it < file_it) {
        FAIL("Deferred chat was delivered before the queued file");
    }

    PASS();
    return true;
}

bool test_file_transfer_holds_link_and_defers_peer_message() {
    TEST("File transfer holds link and defers peer message");

    ConnectionConfig config;
    config.auto_accept = true;
    config.arq.ack_timeout_ms = 200;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    ultra::test::TempDir temp_dir("ultra_protocol_mixed_test");
    if (!temp_dir.valid()) FAIL("Could not create temp test directory");
    const auto& test_dir = temp_dir.path();

    const size_t FILE_SIZE = 32768;
    std::string src_path = createPseudoRandomTestFile(test_dir, "mixed_source.bin", FILE_SIZE);
    if (src_path.empty()) FAIL("Could not create mixed test file");

    std::string rx_dir = (test_dir / "rx").string();
    std::filesystem::create_directories(rx_dir);
    stationB.setReceiveDirectory(rx_dir);

    bool file_received = false;
    std::string received_path;
    bool receive_success = false;
    stationB.setFileReceivedCallback([&](const std::string& path, bool success, const std::string&) {
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

    std::vector<std::string> received_at_a;
    bool message_before_file_complete = false;
    stationA.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        if (!file_received) {
            message_before_file_complete = true;
        }
        received_at_a.push_back(text);
    });

    SimulatedChannel channel(stationA, stationB);
    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected() || !stationB.isConnected()) FAIL("Connection not established");
    if (!stationA.sendFile(src_path)) FAIL("A sendFile() returned false");
    if (!stationB.sendMessage("B message during A file")) FAIL("B sendMessage should queue during received file");

    for (int i = 0; i < 2000 && (!file_received || !file_sent || received_at_a.empty()); i++) {
        channel.run(1, 50);
        if (!file_received &&
            (channel.getTurnoverCountA() != 0 || channel.getTurnoverCountB() != 0)) {
            FAIL("File transfer emitted DATA TURNOVER before completion");
        }
    }

    if (message_before_file_complete) {
        FAIL("Peer message was delivered before file completion");
    }
    if (received_at_a.empty() || received_at_a[0] != "B message during A file") {
        FAIL("A did not receive B's deferred message after file transfer");
    }
    if (!file_sent) FAIL("File not sent (no callback)");
    if (!send_success) FAIL("File send reported failure");
    if (!file_received) FAIL("File not received (no callback)");
    if (!receive_success) FAIL("File receive reported failure");
    if (!filesEqual(src_path, received_path)) FAIL("Mixed file content mismatch");
    if (channel.getTurnoverCountA() < 1) FAIL("File sender did not yield DATA turn after completion");

    const auto a_stats = stationA.getStats().arq;
    const auto b_stats = stationB.getStats().arq;
    if (a_stats.retransmissions != 0 || b_stats.retransmissions != 0 ||
        a_stats.failed != 0 || b_stats.failed != 0) {
        FAIL("Mixed file/message path had ARQ retransmission or failure");
    }

    PASS();
    return true;
}

bool test_file_transfer_receiver_cancel_propagates_and_frees_link() {
    TEST("Receiver file cancel propagates and frees link");

    ConnectionConfig config;
    config.auto_accept = true;
    config.arq.ack_timeout_ms = 200;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    ultra::test::TempDir temp_dir("ultra_protocol_rx_cancel_test");
    if (!temp_dir.valid()) FAIL("Could not create temp test directory");
    const auto& test_dir = temp_dir.path();

    const size_t FILE_SIZE = 32768;
    std::string src_path = createPseudoRandomTestFile(test_dir, "rx_cancel_source.bin", FILE_SIZE);
    if (src_path.empty()) FAIL("Could not create cancel test file");

    std::string rx_dir = (test_dir / "rx").string();
    std::filesystem::create_directories(rx_dir);
    stationB.setReceiveDirectory(rx_dir);

    bool receive_started = false;
    bool receive_cancelled = false;
    bool sender_cancelled = false;
    stationB.setFileProgressCallback([&](const FileTransferProgress& p) {
        if (!p.is_sending && p.total_bytes > 0) {
            receive_started = true;
        }
    });
    stationB.setFileReceivedCallback([&](const std::string&, bool success, const std::string& error) {
        if (!success && error == "Transfer cancelled") {
            receive_cancelled = true;
        }
    });
    stationA.setFileSentCallback([&](bool success, const std::string& error) {
        if (!success && error == "Transfer cancelled") {
            sender_cancelled = true;
        }
    });

    std::vector<std::string> received_at_a;
    stationA.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received_at_a.push_back(text);
    });

    SimulatedChannel channel(stationA, stationB);
    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected() || !stationB.isConnected()) FAIL("Connection not established");
    if (!stationA.sendFile(src_path)) FAIL("A sendFile() returned false");

    for (int i = 0; i < 400 && !receive_started; i++) {
        channel.run(1, 50);
    }
    if (!receive_started) FAIL("B did not start receiving file");
    if (!stationB.sendMessage("B message after receiver cancel")) FAIL("B message should queue during file");

    stationB.cancelFileTransfer();
    for (int i = 0; i < 1200 && (!receive_cancelled || !sender_cancelled || received_at_a.empty()); i++) {
        channel.run(1, 50);
    }

    if (channel.getFileCancelCountB() < 1) FAIL("Receiver did not transmit FILE_CANCEL");
    if (!receive_cancelled) FAIL("Receiver did not report transfer cancelled");
    if (!sender_cancelled) FAIL("Sender did not report transfer cancelled");
    if (received_at_a.empty() || received_at_a[0] != "B message after receiver cancel") {
        FAIL("Queued receiver message did not send after cancel");
    }

    PASS();
    return true;
}

bool test_file_transfer_receiver_cancel_sender_retains_turn() {
    TEST("Receiver file cancel leaves sender ISS turn intact");

    ConnectionConfig config;
    config.auto_accept = true;
    config.arq.ack_timeout_ms = 200;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    ultra::test::TempDir temp_dir("ultra_protocol_rx_cancel_iss_test");
    if (!temp_dir.valid()) FAIL("Could not create temp test directory");
    const auto& test_dir = temp_dir.path();

    const size_t FILE_SIZE = 32768;
    std::string src_path = createPseudoRandomTestFile(test_dir, "rx_cancel_iss_source.bin", FILE_SIZE);
    if (src_path.empty()) FAIL("Could not create cancel ISS test file");

    std::string rx_dir = (test_dir / "rx").string();
    std::filesystem::create_directories(rx_dir);
    stationB.setReceiveDirectory(rx_dir);

    bool receive_started = false;
    bool receive_cancelled = false;
    bool sender_cancelled = false;
    stationB.setFileProgressCallback([&](const FileTransferProgress& p) {
        if (!p.is_sending && p.total_bytes > 0) {
            receive_started = true;
        }
    });
    stationB.setFileReceivedCallback([&](const std::string&, bool success, const std::string& error) {
        if (!success && error == "Transfer cancelled") {
            receive_cancelled = true;
        }
    });
    stationA.setFileSentCallback([&](bool success, const std::string& error) {
        if (!success && error == "Transfer cancelled") {
            sender_cancelled = true;
        }
    });

    std::vector<std::string> received_at_b;
    stationB.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received_at_b.push_back(text);
    });

    SimulatedChannel channel(stationA, stationB);
    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected() || !stationB.isConnected()) FAIL("Connection not established");
    if (!stationA.sendFile(src_path)) FAIL("A sendFile() returned false");

    for (int i = 0; i < 400 && !receive_started; i++) {
        channel.run(1, 50);
    }
    if (!receive_started) FAIL("B did not start receiving file");

    const int a_turnovers_before_cancel = channel.getTurnoverCountA();
    channel.dropNextFileCancelBtoA();
    stationB.cancelFileTransfer();
    for (int i = 0; i < 1200 && (!receive_cancelled || !sender_cancelled); i++) {
        channel.run(1, 50);
    }

    if (channel.getFileCancelCountB() < 2) {
        FAIL("Receiver did not reassert FILE_CANCEL after the first cancel was missed");
    }
    if (!receive_cancelled) FAIL("Receiver did not report transfer cancelled");
    if (!sender_cancelled) FAIL("Sender did not report transfer cancelled");
    if (channel.getTurnoverCountA() != a_turnovers_before_cancel) {
        FAIL("Sender emitted TURNOVER after peer FILE_CANCEL");
    }

    // FILE_CANCEL drains any already-launched file DATA before the retained ISS
    // turn may carry new operator payloads. This wait is a local drain guard, not
    // a peer turn grant.
    channel.run(120, 50);
    const int a_tx_before_message = channel.getTxCountA();
    if (!stationA.sendMessage("A message after receiver cancel")) {
        FAIL("A message after receiver cancel should be accepted");
    }
    for (int i = 0; i < 160 && channel.getTxCountA() <= a_tx_before_message; i++) {
        channel.run(1, 50);
    }
    if (channel.getTxCountA() <= a_tx_before_message) {
        FAIL("Sender did not transmit post-cancel message after bounded cancel guard");
    }

    for (int i = 0; i < 40 && received_at_b.empty(); i++) {
        channel.run(1, 50);
    }
    if (received_at_b.empty() || received_at_b[0] != "A message after receiver cancel") {
        FAIL("Post-cancel sender message did not deliver promptly");
    }

    PASS();
    return true;
}

bool test_file_transfer_sender_cancel_propagates_and_frees_link() {
    TEST("Sender file cancel propagates and frees link");

    ConnectionConfig config;
    config.auto_accept = true;
    config.arq.ack_timeout_ms = 200;

    ProtocolEngine stationA(config);
    ProtocolEngine stationB(config);

    stationA.setLocalCallsign("W1ABC");
    stationB.setLocalCallsign("K2DEF");

    ultra::test::TempDir temp_dir("ultra_protocol_tx_cancel_test");
    if (!temp_dir.valid()) FAIL("Could not create temp test directory");
    const auto& test_dir = temp_dir.path();

    const size_t FILE_SIZE = 32768;
    std::string src_path = createPseudoRandomTestFile(test_dir, "tx_cancel_source.bin", FILE_SIZE);
    if (src_path.empty()) FAIL("Could not create sender-cancel test file");

    std::string rx_dir = (test_dir / "rx").string();
    std::filesystem::create_directories(rx_dir);
    stationB.setReceiveDirectory(rx_dir);

    bool receive_started = false;
    bool receive_cancelled = false;
    bool sender_cancelled = false;
    stationB.setFileProgressCallback([&](const FileTransferProgress& p) {
        if (!p.is_sending && p.total_bytes > 0) {
            receive_started = true;
        }
    });
    stationB.setFileReceivedCallback([&](const std::string&, bool success, const std::string& error) {
        if (!success && error == "Transfer cancelled") {
            receive_cancelled = true;
        }
    });
    stationA.setFileSentCallback([&](bool success, const std::string& error) {
        if (!success && error == "Transfer cancelled") {
            sender_cancelled = true;
        }
    });

    std::vector<std::string> received_at_a;
    stationA.setMessageReceivedCallback([&](const std::string&, const std::string& text) {
        received_at_a.push_back(text);
    });

    SimulatedChannel channel(stationA, stationB);
    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected() || !stationB.isConnected()) FAIL("Connection not established");
    if (!stationA.sendFile(src_path)) FAIL("A sendFile() returned false");

    for (int i = 0; i < 400 && !receive_started; i++) {
        channel.run(1, 50);
    }
    if (!receive_started) FAIL("B did not start receiving file");
    if (!stationB.sendMessage("B message after sender cancel")) FAIL("B message should queue during file");

    stationA.cancelFileTransfer();
    for (int i = 0; i < 1200 && (!receive_cancelled || !sender_cancelled || received_at_a.empty()); i++) {
        channel.run(1, 50);
    }

    if (channel.getFileCancelCountA() < 1) FAIL("Sender did not transmit FILE_CANCEL");
    if (!sender_cancelled) FAIL("Sender did not report transfer cancelled");
    if (!receive_cancelled) FAIL("Receiver did not report transfer cancelled");
    if (received_at_a.empty() || received_at_a[0] != "B message after sender cancel") {
        FAIL("Queued receiver message did not send after sender cancel");
    }

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
    test_message_tx_status_callbacks();
    test_bidirectional_transfer();
    test_half_duplex_turn_taking_queues_irs_data();
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

    std::cout << "\nFile Transfer Tests:\n";
    test_file_transfer_small();
    test_file_transfer_queues_during_connect_guard();
    test_queued_file_yields_to_pending_peer_turn_request_before_start();
    test_queued_file_preempts_deferred_chat_after_current_payload();
    test_file_transfer_holds_link_and_defers_peer_message();
    test_file_transfer_receiver_cancel_propagates_and_frees_link();
    test_file_transfer_receiver_cancel_sender_retains_turn();
    test_file_transfer_sender_cancel_propagates_and_frees_link();

    std::cout << "\nCompression Tests:\n";
    test_compression_basic();

    std::cout << "\n=== Results: " << tests_passed << "/" << tests_run << " passed ===\n";

    return (tests_passed == tests_run) ? 0 : 1;
}
