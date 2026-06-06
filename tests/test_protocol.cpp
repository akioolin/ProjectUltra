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

// ============================================================================
// Capability Negotiation Tests
// ============================================================================

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

    SimulatedChannel channel(stationA, stationB);
    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected() || !stationB.isConnected()) FAIL("Connection not established");
    if (!stationA.sendFile(src_path)) FAIL("A sendFile() returned false");

    for (int i = 0; i < 400 && !receive_started; i++) {
        channel.run(1, 50);
    }
    if (!receive_started) FAIL("B did not start receiving file");

    stationB.cancelFileTransfer();
    for (int i = 0; i < 1200 && (!receive_cancelled || !sender_cancelled); i++) {
        channel.run(1, 50);
    }

    if (channel.getFileCancelCountB() < 1) FAIL("Receiver did not transmit FILE_CANCEL");
    if (!receive_cancelled) FAIL("Receiver did not report transfer cancelled");
    if (!sender_cancelled) FAIL("Sender did not report transfer cancelled");

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

    SimulatedChannel channel(stationA, stationB);
    stationA.connect("K2DEF");
    channel.run(30, 100);

    if (!stationA.isConnected() || !stationB.isConnected()) FAIL("Connection not established");
    if (!stationA.sendFile(src_path)) FAIL("A sendFile() returned false");

    for (int i = 0; i < 400 && !receive_started; i++) {
        channel.run(1, 50);
    }
    if (!receive_started) FAIL("B did not start receiving file");

    stationA.cancelFileTransfer();
    for (int i = 0; i < 1200 && (!receive_cancelled || !sender_cancelled); i++) {
        channel.run(1, 50);
    }

    if (channel.getFileCancelCountA() < 1) FAIL("Sender did not transmit FILE_CANCEL");
    if (!sender_cancelled) FAIL("Sender did not report transfer cancelled");
    if (!receive_cancelled) FAIL("Receiver did not report transfer cancelled");

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
    test_disconnect();
    test_manual_accept();

    std::cout << "\nCapability Negotiation:\n";
    test_phy_mask_v1_negotiation();

    std::cout << "\nBinary Stream / TNC API Tests:\n";
    test_binary_fragment_reassembly_single_callback();
    test_send_binary_roundtrip_arbitrary_bytes();

    std::cout << "\nFile Transfer Tests:\n";
    // NOTE (TRANSPORT MERGE 2026-06-06): the in-process SimulatedChannel file/binary-send
    // tests (small-file, queue-during-guard, tx-backlog, receiver-cancel-retains-turn) were
    // removed — the unified path bursts through the modem (on_transmit_burst_ ->
    // encodeBurstLight -> RX burst_group_callback_), which a frame-level SimulatedChannel
    // cannot carry. File transfer is gated on the faithful GUI/OTASim path (gui_qso_scenario.sh).
    test_file_transfer_receiver_cancel_propagates_and_frees_link();
    test_file_transfer_sender_cancel_propagates_and_frees_link();

    std::cout << "\nCompression Tests:\n";
    test_compression_basic();

    std::cout << "\n=== Results: " << tests_passed << "/" << tests_run << " passed ===\n";

    return (tests_passed == tests_run) ? 0 : 1;
}
