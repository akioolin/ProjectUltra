/**
 * Selective Repeat ARQ Test Suite
 *
 * Tests the SR-ARQ implementation including:
 * - Basic send/receive with window
 * - Out-of-order delivery
 * - SACK generation and processing
 * - Retransmission on timeout
 * - Window advancement
 */

#include "protocol/arq_interface.hpp"
#include "protocol/selective_repeat_arq.hpp"
#include "protocol/frame_v2.hpp"
#include <iostream>
#include <cassert>
#include <cstdlib>
#include <queue>
#include <string>
#include <vector>

using namespace ultra;
using namespace ultra::protocol;

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
// Test Helpers
// ============================================================================

// Simple byte queue to simulate channel (v2 frames as serialized bytes)
class ByteChannel {
public:
    void send(const Bytes& data) { queue_.push(data); }

    bool hasData() const { return !queue_.empty(); }

    Bytes receive() {
        Bytes data = queue_.front();
        queue_.pop();
        return data;
    }

    void clear() {
        while (!queue_.empty()) queue_.pop();
    }

    size_t size() const { return queue_.size(); }

private:
    std::queue<Bytes> queue_;
};

static v2::ControlFrame makeSackAck(uint16_t seq, uint32_t bitmap) {
    auto sack = v2::ControlFrame::makeNack("RX1", "TX1", seq, bitmap);
    sack.type = v2::FrameType::ACK;
    return sack;
}

static bool expectDataSeq(const Bytes& frame_data, uint16_t expected_seq,
                          const std::string& context) {
    auto parsed = v2::DataFrame::deserialize(frame_data);
    if (!parsed)
        FAIL(context + ": retransmitted frame did not parse as DATA");
    if (parsed->type != v2::FrameType::DATA)
        FAIL(context + ": retransmitted frame was not DATA");
    if (parsed->seq != expected_seq)
        FAIL(context + ": expected DATA seq=" + std::to_string(expected_seq) +
             ", got seq=" + std::to_string(parsed->seq));
    return true;
}

// ============================================================================
// Basic Tests
// ============================================================================

bool test_create_sr_arq() {
    TEST("Create Selective Repeat ARQ");

    ARQConfig config;
    config.window_size = 4;

    auto arq = createARQController(ARQMode::SELECTIVE_REPEAT, config);

    if (arq->getMode() != ARQMode::SELECTIVE_REPEAT)
        FAIL("Wrong ARQ mode");

    if (arq->getAvailableSlots() != 4)
        FAIL("Wrong available slots");

    PASS();
    return true;
}

bool test_send_single_frame() {
    TEST("Send single frame");

    ARQConfig config;
    config.window_size = 4;
    config.ack_timeout_ms = 4000;
    config.sack_delay_ms = 100;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    ByteChannel channel;
    tx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    // Send data
    Bytes data = {0x01, 0x02, 0x03};
    if (!tx.sendData(data))
        FAIL("Failed to send data");

    // Check frame was transmitted
    if (channel.size() != 1)
        FAIL("Frame not transmitted");

    Bytes frame_data = channel.receive();
    auto parsed = v2::DataFrame::deserialize(frame_data);
    if (!parsed)
        FAIL("Failed to parse transmitted frame");

    if (parsed->type != v2::FrameType::DATA)
        FAIL("Wrong frame type");
    if (parsed->seq != 0)
        FAIL("Wrong sequence number");
    if (parsed->payload != data)
        FAIL("Wrong payload");

    // Window should have 3 slots remaining
    if (tx.getAvailableSlots() != 3)
        FAIL("Wrong available slots after send");

    PASS();
    return true;
}

bool test_data_flags_preserve_version_bit() {
    TEST("DATA flags preserve v2 version bit");

    ARQConfig config;
    config.window_size = 4;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");
    tx.setCodeRate(CodeRate::R1_2);

    ByteChannel channel;
    tx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    if (!tx.sendDataWithFlags(Bytes{0x01}, v2::Flags::MORE_FRAG))
        FAIL("sendDataWithFlags failed");
    if (!tx.sendFixedDataWithFlags(Bytes{0x02}, v2::Flags::MORE_FRAG))
        FAIL("sendFixedDataWithFlags failed");
    if (!tx.sendVariableDataWithFlags(Bytes{0x03}, v2::Flags::MORE_FRAG))
        FAIL("sendVariableDataWithFlags failed");

    while (channel.hasData()) {
        auto parsed = v2::DataFrame::deserialize(channel.receive());
        if (!parsed)
            FAIL("transmitted DATA frame did not parse");
        if ((parsed->flags & v2::Flags::VERSION_V2) == 0)
            FAIL("VERSION_V2 bit was not preserved");
        if ((parsed->flags & v2::Flags::MORE_FRAG) == 0)
            FAIL("MORE_FRAG bit was not preserved");
    }

    PASS();
    return true;
}

bool test_code_rate_change_aborts_in_flight_fixed_frames() {
    TEST("Code rate change aborts in-flight fixed frames");

    ARQConfig config;
    config.window_size = 3;
    config.ack_timeout_ms = 1000;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");
    tx.setCodeRate(CodeRate::R1_2);

    ByteChannel channel;
    tx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    int completions = 0;
    tx.setSendCompleteCallback([&](bool) { completions++; });

    for (int i = 0; i < 3; i++) {
        Bytes data = {static_cast<uint8_t>(0xA0 + i)};
        if (!tx.sendFixedDataWithFlags(data, v2::Flags::MORE_FRAG))
            FAIL("Failed to send fixed DATA frame " + std::to_string(i));
    }

    if (tx.getAvailableSlots() != 0)
        FAIL("Window should be full before code-rate change");

    channel.clear();
    tx.setCodeRate(CodeRate::R1_4);

    if (tx.getCodeRate() != CodeRate::R1_4)
        FAIL("Code rate was not updated");
    if (tx.getAvailableSlots() != 3)
        FAIL("Window was not fully available after code-rate change");
    if (completions != 0)
        FAIL("Abort should not report ACK/failure completion");

    tx.tick(2000);
    if (channel.size() != 0)
        FAIL("Aborted in-flight frames were retransmitted after code-rate change");

    PASS();
    return true;
}

bool test_send_window_full() {
    TEST("Send until window full");

    ARQConfig config;
    config.window_size = 4;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    ByteChannel channel;
    tx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    // Fill window
    for (int i = 0; i < 4; i++) {
        Bytes data = {static_cast<uint8_t>(i)};
        if (!tx.sendData(data))
            FAIL("Failed to send frame " + std::to_string(i));
    }

    // Window should be full
    if (tx.getAvailableSlots() != 0)
        FAIL("Window not full");

    if (tx.isReadyToSend())
        FAIL("isReadyToSend() should be false when window full");

    // Try to send another - should fail
    Bytes data = {0xFF};
    if (tx.sendData(data))
        FAIL("Should not be able to send when window full");

    // Check all 4 frames were transmitted
    if (channel.size() != 4)
        FAIL("Wrong number of frames transmitted");

    PASS();
    return true;
}

bool test_receive_ack() {
    TEST("Receive ACK frees slot");

    ARQConfig config;
    config.window_size = 4;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    ByteChannel channel;
    tx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    int completions = 0;
    tx.setSendCompleteCallback([&](bool success) {
        if (success) completions++;
    });

    // Send 4 frames
    for (int i = 0; i < 4; i++) {
        tx.sendData(Bytes{static_cast<uint8_t>(i)});
    }
    channel.clear();

    // ACK first frame
    auto ack = v2::ControlFrame::makeAck("RX1", "TX1", 0);
    Bytes ack_data = ack.serialize();
    tx.onFrameReceived(ack_data);

    // Should have 1 completion
    if (completions != 1)
        FAIL("Expected 1 completion, got " + std::to_string(completions));

    // Should have 1 slot free
    if (tx.getAvailableSlots() != 1)
        FAIL("Expected 1 slot free");

    PASS();
    return true;
}

bool test_stale_ack_older_than_base_minus_one_is_ignored() {
    TEST("Stale ACK older than base-1 is ignored without freeing TX slots");

    ARQConfig config;
    config.window_size = 4;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    std::vector<Bytes> transmitted;
    tx.setTransmitCallback([&](const Bytes& data) { transmitted.push_back(data); });

    int completions = 0;
    tx.setSendCompleteCallback([&](bool success) {
        if (success) completions++;
    });

    for (int i = 0; i < 4; i++) {
        if (!tx.sendData(Bytes{static_cast<uint8_t>(i)}))
            FAIL("Failed to fill TX window at seq=" + std::to_string(i));
    }
    if (tx.getAvailableSlots() != 0)
        FAIL("Expected full TX window before ACK setup");

    auto ack1 = v2::ControlFrame::makeAck("RX1", "TX1", 1);
    tx.onFrameReceived(ack1.serialize());
    if (completions != 2)
        FAIL("Setup ACK should complete seq=0..1, got completions=" +
             std::to_string(completions));
    if (tx.getAvailableSlots() != 2)
        FAIL("Setup ACK should free exactly 2 TX slots");

    const size_t slots_before = tx.getAvailableSlots();
    const int completions_before = completions;

    auto stale = makeSackAck(0, 0x0Cu);
    tx.onFrameReceived(stale.serialize());

    if (tx.getAvailableSlots() != slots_before)
        FAIL("Stale ACK advanced/free'd TX window slots");
    if (completions != completions_before)
        FAIL("Stale ACK caused duplicate send-complete callbacks");
    if (transmitted.size() != 4)
        FAIL("Stale ACK unexpectedly triggered a retransmission");

    auto stats = tx.getStats();
    if (stats.stale_acks_ignored != 1)
        FAIL("Expected stale_acks_ignored=1, got " +
             std::to_string(stats.stale_acks_ignored));

    PASS();
    return true;
}

bool test_future_ack_too_far_ahead_is_ignored() {
    TEST("Future ACK too far ahead is ignored without freeing TX slots");

    ARQConfig config;
    config.window_size = 4;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    std::vector<Bytes> transmitted;
    tx.setTransmitCallback([&](const Bytes& data) { transmitted.push_back(data); });

    int completions = 0;
    tx.setSendCompleteCallback([&](bool success) {
        if (success) completions++;
    });

    for (int i = 0; i < 4; i++) {
        if (!tx.sendData(Bytes{static_cast<uint8_t>(i)}))
            FAIL("Failed to fill TX window at seq=" + std::to_string(i));
    }

    auto future = makeSackAck(6, 0x0Fu);
    tx.onFrameReceived(future.serialize());

    if (tx.getAvailableSlots() != 0)
        FAIL("Future ACK advanced/free'd TX window slots");
    if (completions != 0)
        FAIL("Future ACK caused send-complete callbacks");
    if (transmitted.size() != 4)
        FAIL("Future ACK unexpectedly triggered a retransmission");

    auto stats = tx.getStats();
    if (stats.future_acks_ignored != 1)
        FAIL("Expected future_acks_ignored=1, got " +
             std::to_string(stats.future_acks_ignored));
    if (stats.acks_received != 0)
        FAIL("Future ACK should not count as a received cumulative ACK");

    PASS();
    return true;
}

bool test_duplicate_sack_hole_is_suppressed_without_duplicate_retx_accounting() {
    TEST("Duplicate SACK hole is suppressed without duplicate retransmission accounting");

    ARQConfig config;
    config.window_size = 4;
    config.ack_timeout_ms = 1000;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    std::vector<Bytes> transmitted;
    tx.setTransmitCallback([&](const Bytes& data) { transmitted.push_back(data); });

    for (int i = 0; i < 3; i++) {
        if (!tx.sendData(Bytes{static_cast<uint8_t>(i)}))
            FAIL("Failed to send DATA seq=" + std::to_string(i));
    }
    if (tx.getAvailableSlots() != 1)
        FAIL("Expected one free slot before SACK-hole test setup");

    // ACK base-1 with bit1 set: receiver is missing seq=0 but has seq=1.
    auto base_hole = makeSackAck(0xFFFF, 0x02u);
    tx.onFrameReceived(base_hole.serialize());

    auto after_first = tx.getStats();
    if (after_first.hole_events != 1)
        FAIL("First base-hole SACK should record one hole event");
    if (after_first.retransmissions != 0)
        FAIL("First hole indication should not retransmit before two confirmations");
    if (tx.getAvailableSlots() != 1)
        FAIL("Base-hole SACK should not free the missing base slot");

    tx.onFrameReceived(base_hole.serialize());

    auto stats = tx.getStats();
    if (stats.duplicate_acks_ignored != 1)
        FAIL("Expected duplicate_acks_ignored=1, got " +
             std::to_string(stats.duplicate_acks_ignored));
    if (stats.hole_events != 1)
        FAIL("Duplicate SACK should not double-count hole events");
    if (stats.retransmissions != 0 ||
        stats.retransmissions_fast_hole != 0 ||
        stats.retransmissions_timeout != 0)
        FAIL("Duplicate SACK caused retransmission accounting");
    if (transmitted.size() != 3)
        FAIL("Duplicate SACK unexpectedly transmitted a repair frame");

    PASS();
    return true;
}

bool test_hole_probe_not_rearmed_while_fast_hole_in_flight() {
    TEST("hole-probe is not re-armed while a fast-hole retx is in flight");

    // Regression for the faithful-clock (GUI/hardware) double-retx: a base hole
    // is repaired by a fast-hole retransmit, but subsequent hole-confirmation
    // SACKs still show the gap (the repair hasn't landed + been ACKed across the
    // half-duplex turnaround yet). Before the fix, that re-armed the hole-probe,
    // which then fired a redundant retransmit of a frame the fast-hole already
    // recovered. The fix suppresses re-arming while fast_retx_cooldown_ms > 0.
    // ack_timeout=5000 keeps the per-slot RTO (5000ms) well clear of the
    // hole-probe timer (clamp(2500,1200,2500)=2500ms) so this test exercises the
    // probe path, not the RTO. fast-hole cooldown = clamp(833,300,1200)=833ms.
    ARQConfig config;
    config.window_size = 8;
    config.ack_timeout_ms = 5000;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    std::vector<Bytes> transmitted;
    tx.setTransmitCallback([&](const Bytes& data) { transmitted.push_back(data); });

    for (int i = 0; i < 8; i++) {
        if (!tx.sendData(Bytes{static_cast<uint8_t>(i)}))
            FAIL("Failed to send DATA seq=" + std::to_string(i));
    }
    if (transmitted.size() != 8)
        FAIL("Expected 8 initial transmits");

    // Evolving base-hole SACKs (seq0 missing throughout): distinct bitmaps so
    // each counts as a fresh hole confirmation (not a dedup'd duplicate).
    tx.onFrameReceived(makeSackAck(0xFFFF, 0x02u).serialize());  // confirm #1 -> arm probe
    tx.tick(50);
    tx.onFrameReceived(makeSackAck(0xFFFF, 0x06u).serialize());  // confirm #2 -> fast-hole fires, disarm probe

    auto mid = tx.getStats();
    if (mid.retransmissions_fast_hole != 1)
        FAIL("Expected exactly one fast-hole repair, got " +
             std::to_string(mid.retransmissions_fast_hole));

    // Third confirmation arrives while the fast-hole repair is still in flight
    // (cooldown active). With the fix this must NOT re-arm the hole-probe.
    tx.tick(100);  // cooldown 833 -> 733, still active
    tx.onFrameReceived(makeSackAck(0xFFFF, 0x0Eu).serialize());  // confirm #3 (cooldown still > 0)

    // Tick past the hole-probe timer (2500ms) but short of the RTO (5000ms):
    // if the probe had been re-armed it would fire a redundant retransmit here.
    tx.tick(2600);

    auto stats = tx.getStats();
    if (stats.retransmissions_hole_probe != 0)
        FAIL("Hole-probe redundantly retransmitted a frame the fast-hole repair "
             "already covered (retransmissions_hole_probe=" +
             std::to_string(stats.retransmissions_hole_probe) + ")");
    if (stats.retransmissions_timeout != 0)
        FAIL("RTO should not fire within the window (got timeout retx=" +
             std::to_string(stats.retransmissions_timeout) + ")");
    if (stats.retransmissions_fast_hole != 1)
        FAIL("Fast-hole repair count changed unexpectedly: " +
             std::to_string(stats.retransmissions_fast_hole));
    if (transmitted.size() != 9)  // 8 initial + 1 fast-hole; NO probe/timeout retx
        FAIL("Expected 9 transmits (8 + 1 fast-hole), got " +
             std::to_string(transmitted.size()));

    PASS();
    return true;
}

bool test_rx_in_order() {
    TEST("RX delivers in-order frames");

    ARQConfig config;
    config.window_size = 4;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    std::vector<Bytes> received;
    rx.setDataReceivedCallback([&](const Bytes& data) {
        received.push_back(data);
    });

    // Receive frames in order
    for (int i = 0; i < 3; i++) {
        auto frame = v2::DataFrame::makeData("TX1", "RX1", i, Bytes{static_cast<uint8_t>(i)});
        Bytes frame_data = frame.serialize();
        rx.onFrameReceived(frame_data);
    }

    // All 3 should be delivered
    if (received.size() != 3)
        FAIL("Expected 3 deliveries, got " + std::to_string(received.size()));

    for (int i = 0; i < 3; i++) {
        if (received[i].size() != 1 || received[i][0] != i)
            FAIL("Wrong payload for frame " + std::to_string(i));
    }

    // Default ACK batching sends a SACK at window threshold (4 frames), not
    // after every in-order frame. Three frames should only arm the tail timer.
    if (channel.size() != 0)
        FAIL("Expected no immediate SACK before default batch threshold");

    PASS();
    return true;
}

bool test_duplicate_data_is_not_delivered_twice_and_sends_recovery_sack() {
    TEST("Duplicate DATA is ignored without duplicate delivery and keeps SACK recovery sane");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 10000;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    std::vector<Bytes> received;
    rx.setDataReceivedCallback([&](const Bytes& data) {
        received.push_back(data);
    });

    auto f0 = v2::DataFrame::makeData("TX1", "RX1", 0, Bytes{0x42});
    rx.onFrameReceived(f0.serialize());

    if (received.size() != 1)
        FAIL("Initial DATA seq=0 should be delivered once");
    if (channel.size() != 0)
        FAIL("Initial in-order frame should only arm delayed SACK at this batch size");

    rx.onFrameReceived(f0.serialize());

    if (received.size() != 1)
        FAIL("Duplicate DATA seq=0 was delivered twice");
    if (channel.size() != 1)
        FAIL("Duplicate delivered DATA should send one recovery SACK, got " +
             std::to_string(channel.size()));

    auto ack = v2::ControlFrame::deserialize(channel.receive());
    if (!ack || ack->type != v2::FrameType::ACK || ack->seq != 0)
        FAIL("Duplicate DATA recovery SACK should cumulatively ACK seq=0");
    if (v2::NackPayload::decode(ack->payload).cw_bitmap != 0)
        FAIL("Duplicate DATA recovery SACK should have empty current-window bitmap");

    rx.tick(config.sack_delay_ms);
    if (channel.size() != 0)
        FAIL("Duplicate DATA left a stale delayed SACK pending");

    auto stats = rx.getStats();
    if (stats.frames_received != 1)
        FAIL("Duplicate DATA should not increment frames_received twice");
    if (stats.sack_trigger_out_of_window != 1)
        FAIL("Expected duplicate delivered DATA to use out-of-window SACK trigger");

    PASS();
    return true;
}

// BUG-ARQ-SEQ-COLLISION interim salvage: below-window FILE_START/FILE_DATA payloads are
// handed up the normal delivery callback (the file layer is offset-idempotent) instead of
// dying at the seq-keyed dedup — but ONLY with ULTRA_BELOW_WINDOW_FILE_SALVAGE=1, ONLY for
// FILE payload types, and ONLY strictly below the window (never far-future).
bool test_below_window_file_salvage() {
    TEST("Below-window FILE frames salvaged only with knob on");

    const Bytes file_chunk_a = {0x02, 0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB};  // FILE_DATA off=0
    const Bytes file_chunk_b = {0x02, 0x00, 0x00, 0x00, 0x04, 0xCC};        // FILE_DATA off=4 (regrid)
    const Bytes file_start   = {0x01, 0x00, 0x00, 0x00, 0x00, 0x10,
                                0x12, 0x34, 0x56, 0x78, 'f'};               // FILE_START
    const Bytes text_msg     = {0x00, 'h', 'i'};                            // TEXT_MESSAGE

    ARQConfig config;
    config.window_size = 4;

    // --- Knob ON ---
    setenv("ULTRA_BELOW_WINDOW_FILE_SALVAGE", "1", 1);
    SelectiveRepeatARQ rx(config);       // latches the knob in the ctor
    unsetenv("ULTRA_BELOW_WINDOW_FILE_SALVAGE");
    rx.setCallsigns("RX1", "TX1");

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });
    std::vector<Bytes> received;
    rx.setDataReceivedCallback([&](const Bytes& data) { received.push_back(data); });

    // In-order seq=0 delivers normally and advances rx_base to 1.
    rx.onFrameReceived(v2::DataFrame::makeData("TX1", "RX1", 0, file_chunk_a).serialize());
    if (received.size() != 1)
        FAIL("In-order FILE_DATA seq=0 should deliver once");
    if (channel.size() != 0)
        FAIL("In-order frame below batch threshold should not SACK yet");

    // Below-window FILE_DATA (seq=0 again, DIFFERENT bytes — the W16 regrid case):
    // must be salvaged up the delivery callback AND still emit the recovery SACK.
    rx.onFrameReceived(v2::DataFrame::makeData("TX1", "RX1", 0, file_chunk_b).serialize());
    if (received.size() != 2)
        FAIL("Below-window FILE_DATA was not salvaged with knob on");
    if (received[1] != file_chunk_b)
        FAIL("Salvaged payload does not match the below-window frame");
    if (channel.size() != 1)
        FAIL("Salvage must not suppress the out-of-window recovery SACK");
    {
        auto ack = v2::ControlFrame::deserialize(channel.receive());
        if (!ack || ack->type != v2::FrameType::ACK)
            FAIL("Out-of-window recovery frame should still be a SACK");
    }

    // Below-window FILE_START is salvaged too (idempotent at the file layer).
    rx.onFrameReceived(v2::DataFrame::makeData("TX1", "RX1", 0, file_start).serialize());
    if (received.size() != 3)
        FAIL("Below-window FILE_START was not salvaged with knob on");

    // Below-window TEXT_MESSAGE must NEVER be salvaged (would duplicate the message).
    rx.onFrameReceived(v2::DataFrame::makeData("TX1", "RX1", 0, text_msg).serialize());
    if (received.size() != 3)
        FAIL("Below-window TEXT_MESSAGE must not be salvaged");

    // Far-FUTURE out-of-window FILE_DATA (beyond the window, not behind it) must NOT
    // be salvaged — the salvage is strictly below-window.
    rx.onFrameReceived(v2::DataFrame::makeData("TX1", "RX1", 100, file_chunk_b).serialize());
    if (received.size() != 3)
        FAIL("Far-future FILE_DATA must not be salvaged");

    // --- Knob OFF (explicit =0 opt-out; DEFAULT is ON since 2026-07-03 evening,
    // 9/9 rig field engagements): below-window FILE_DATA dropped as before ---
    setenv("ULTRA_BELOW_WINDOW_FILE_SALVAGE", "0", 1);
    SelectiveRepeatARQ rx_off(config);
    unsetenv("ULTRA_BELOW_WINDOW_FILE_SALVAGE");
    rx_off.setCallsigns("RX1", "TX1");
    ByteChannel channel_off;
    rx_off.setTransmitCallback([&](const Bytes& data) { channel_off.send(data); });
    std::vector<Bytes> received_off;
    rx_off.setDataReceivedCallback([&](const Bytes& data) { received_off.push_back(data); });

    rx_off.onFrameReceived(v2::DataFrame::makeData("TX1", "RX1", 0, file_chunk_a).serialize());
    rx_off.onFrameReceived(v2::DataFrame::makeData("TX1", "RX1", 0, file_chunk_b).serialize());
    if (received_off.size() != 1)
        FAIL("Knob off: below-window FILE_DATA must be dropped (default unchanged)");
    if (channel_off.size() != 1)
        FAIL("Knob off: out-of-window recovery SACK should still be sent");

    PASS();
    return true;
}

// ============================================================================
// MOVE-EPOCH (BUG-ARQ-SEQ-COLLISION structural fix, ULTRA_ARQ_MOVE_EPOCH)
// ============================================================================

namespace {

// Build a receiver-crafted DATA frame with explicit move-epoch flag bits.
Bytes makeEpochData(uint16_t seq, const Bytes& payload, uint8_t epoch, bool rebase) {
    auto f = v2::DataFrame::makeData("TX1", "RX1", seq, payload);
    f.flags = static_cast<uint8_t>(f.flags | v2::epochToFlags(epoch) |
                                   (rebase ? v2::Flags::EPOCH_REBASE : 0));
    return f.serialize();
}

}  // namespace

// TX side: the epoch counter bumps (mod 4) on the setCodeRate abort that rewinds
// tx_next_seq_ (the collision precondition), every DATA frame carries the current
// epoch in flags bits 6-7, and the frame created at the window base carries
// EPOCH_REBASE.
bool test_move_epoch_bump_on_abort_and_stamp() {
    TEST("MOVE-EPOCH: bump on rate-change abort + flags stamp");

    ARQConfig config;
    config.window_size = 4;

    setenv("ULTRA_ARQ_MOVE_EPOCH", "1", 1);
    SelectiveRepeatARQ tx(config);  // latches the knob in the ctor
    unsetenv("ULTRA_ARQ_MOVE_EPOCH");
    tx.setCallsigns("TX1", "RX1");

    std::vector<Bytes> transmitted;
    tx.setTransmitCallback([&](const Bytes& data) { transmitted.push_back(data); });

    tx.sendData(Bytes{0x01});
    tx.sendData(Bytes{0x02});
    if (transmitted.size() != 2)
        FAIL("Expected 2 initial transmissions");
    {
        auto f0 = v2::DataFrame::deserialize(transmitted[0]);
        auto f1 = v2::DataFrame::deserialize(transmitted[1]);
        if (!f0 || !f1) FAIL("Initial frames did not parse");
        if (v2::epochFromFlags(f0->flags) != 0 || v2::epochFromFlags(f1->flags) != 0)
            FAIL("Pre-abort frames must carry epoch 0");
        if ((f0->flags & v2::Flags::EPOCH_REBASE) == 0)
            FAIL("Frame created at the window base must carry EPOCH_REBASE");
        if ((f1->flags & v2::Flags::EPOCH_REBASE) != 0)
            FAIL("Mid-window frame must NOT carry EPOCH_REBASE");
    }

    // Rate-change TX abort (in-flight > 0) => rewind + epoch bump.
    tx.setCodeRate(CodeRate::R1_2);
    tx.sendData(Bytes{0x03});  // re-uses seq 0 on the new grid
    if (transmitted.size() != 3)
        FAIL("Expected the post-abort frame to transmit");
    {
        auto f = v2::DataFrame::deserialize(transmitted[2]);
        if (!f) FAIL("Post-abort frame did not parse");
        if (f->seq != 0)
            FAIL("Post-abort frame should re-use the rewound base seq 0");
        if (v2::epochFromFlags(f->flags) != 1)
            FAIL("Post-abort frame must carry the bumped epoch 1");
        if ((f->flags & v2::Flags::EPOCH_REBASE) == 0)
            FAIL("Post-abort base frame must carry EPOCH_REBASE");
    }

    PASS();
    return true;
}

// TX side, second bump site (2026-07-04 fix, Phase-2 adversarial-review finding):
// a mid-window regrid through the CW-change abort ALONE — same code rate, so
// setCodeRate early-returns — must ALSO enter a new era. This is the Phase-2
// receiver-commanded QAM16 R3/4 -> QPSK R3/4 demote shape: without the bump the
// receiver's in-order rx_base is stranded below the abandoned seqs with no
// rebase-anchor (BUG-ARQ-SEQ-COLLISION by the other door).
bool test_move_epoch_bump_on_cw_abort_same_rate() {
    TEST("MOVE-EPOCH: bump on same-rate CW-change abort (Phase-2 mid-window regrid)");

    ARQConfig config;
    config.window_size = 4;

    setenv("ULTRA_ARQ_MOVE_EPOCH", "1", 1);
    SelectiveRepeatARQ tx(config);
    unsetenv("ULTRA_ARQ_MOVE_EPOCH");
    tx.setCallsigns("TX1", "RX1");

    std::vector<Bytes> transmitted;
    tx.setTransmitCallback([&](const Bytes& data) { transmitted.push_back(data); });

    tx.sendData(Bytes{0x01});
    tx.sendData(Bytes{0x02});
    if (transmitted.size() != 2) FAIL("Expected 2 initial transmissions");
    if (tx.txMoveEpoch() != 0) FAIL("Epoch must start at 0");

    // Same-rate mid-window regrid: ONLY the CW count changes (mod-change shape).
    tx.setFixedFrameCodewords(2);
    if (tx.txMoveEpoch() != 1)
        FAIL("CW-change abort dropping live payload must bump the epoch");

    tx.sendData(Bytes{0x03});  // first frame of the new era at the advanced base
    {
        auto f = v2::DataFrame::deserialize(transmitted.back());
        if (!f) FAIL("Post-abort frame did not parse");
        if (v2::epochFromFlags(f->flags) != 1)
            FAIL("Post-abort frame must carry the bumped epoch 1");
        if ((f->flags & v2::Flags::EPOCH_REBASE) == 0)
            FAIL("Post-abort base frame must carry EPOCH_REBASE (the rx re-anchor)");
    }

    // An abort with NOTHING live must NOT bump (no remap happened — e.g. teardown
    // on an idle window must not burn mod-4 epoch space).
    tx.abortPendingTx();  // window already empty? no: seq 0x03 is live — drain first
    const uint8_t epoch_after_live_abort = tx.txMoveEpoch();
    if (epoch_after_live_abort != 2)
        FAIL("abortPendingTx dropping a live frame must bump");
    tx.abortPendingTx();  // now truly empty
    if (tx.txMoveEpoch() != epoch_after_live_abort)
        FAIL("abortPendingTx on an empty window must NOT bump");

    PASS();
    return true;
}

// RX side: the W16 regrid case. A below-window seq with a NEWER epoch and the
// EPOCH_REBASE anchor is NOT a duplicate — the receiver adopts the era,
// re-anchors rx_base to the rebase seq, and DELIVERS the re-gridded content
// (TEXT payload here, proving it is the epoch machinery — not the FILE salvage —
// accepting it). Its ACKs then echo the new epoch in SACK bitmap bits 16-17.
bool test_move_epoch_regrid_resend_accepted() {
    TEST("MOVE-EPOCH: below-window regrid resend adopted + delivered");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 100;

    setenv("ULTRA_ARQ_MOVE_EPOCH", "1", 1);
    SelectiveRepeatARQ rx(config);
    unsetenv("ULTRA_ARQ_MOVE_EPOCH");
    rx.setCallsigns("RX1", "TX1");

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });
    std::vector<Bytes> received;
    rx.setDataReceivedCallback([&](const Bytes& data) { received.push_back(data); });

    // Era 0: deliver seqs 0..2 (one-way ACK loss at the sender is invisible here —
    // the receiver's base simply runs ahead, the W16 precondition).
    for (uint16_t s = 0; s < 3; ++s) {
        rx.onFrameReceived(makeEpochData(s, Bytes{static_cast<uint8_t>(s)}, 0, s == 0));
    }
    if (received.size() != 3)
        FAIL("Era-0 frames should deliver in order");
    channel.clear();

    // Era 1 regrid: the sender rewound to ITS stale base 0 and re-chunked
    // DIFFERENT bytes under the retired seqs (TEXT — the payload type the interim
    // salvage must never touch).
    rx.onFrameReceived(makeEpochData(0, Bytes{0x00, 0xAA}, 1, /*rebase=*/true));
    if (received.size() != 4)
        FAIL("Regridded rebase frame was not delivered (adoption failed)");
    if (received[3] != Bytes({0x00, 0xAA}))
        FAIL("Delivered payload is not the re-gridded content");

    rx.onFrameReceived(makeEpochData(1, Bytes{0x01, 0xBB}, 1, /*rebase=*/false));
    if (received.size() != 5)
        FAIL("Post-anchor era-1 frame should deliver in order");
    if (rx.getRxBaseSeq() != 2)
        FAIL("rx_base should have re-anchored and advanced to 2");

    // Flush the delayed SACK and verify the epoch echo (bitmap bits 16-17 == 1).
    rx.tick(config.sack_delay_ms);
    if (channel.size() < 1)
        FAIL("Expected a SACK after the delayed timer");
    {
        auto ack = v2::ControlFrame::deserialize(channel.receive());
        if (!ack || ack->type != v2::FrameType::ACK)
            FAIL("Expected an ACK-type SACK");
        const uint32_t bitmap = v2::NackPayload::decode(ack->payload).cw_bitmap;
        if (((bitmap >> 16) & 0x3u) != 1u)
            FAIL("SACK must echo the adopted epoch in bitmap bits 16-17");
        if (ack->seq != 1)
            FAIL("SACK cumulative base should be rx_base-1 = 1 in the new era");
    }

    PASS();
    return true;
}

// TX side: an ACK whose epoch echo predates the current TX epoch is IGNORED for
// retirement (the W16 phantom-retire arm), and a current-epoch ACK then retires.
bool test_move_epoch_stale_ack_ignored() {
    TEST("MOVE-EPOCH: stale-epoch ACK ignored, fresh-epoch ACK retires");

    ARQConfig config;
    config.window_size = 4;
    config.ack_timeout_ms = 4000;

    setenv("ULTRA_ARQ_MOVE_EPOCH", "1", 1);
    SelectiveRepeatARQ tx(config);
    unsetenv("ULTRA_ARQ_MOVE_EPOCH");
    tx.setCallsigns("TX1", "RX1");

    std::vector<Bytes> transmitted;
    tx.setTransmitCallback([&](const Bytes& data) { transmitted.push_back(data); });

    tx.sendData(Bytes{0x01});
    tx.sendData(Bytes{0x02});
    tx.sendData(Bytes{0x03});

    // Rate-change abort => epoch 1, rewind, re-send new content under seqs 0..2.
    tx.setCodeRate(CodeRate::R1_2);
    tx.sendData(Bytes{0x11});
    tx.sendData(Bytes{0x12});
    tx.sendData(Bytes{0x13});
    if (tx.getAvailableSlots() != 1)
        FAIL("Expected 3 frames in flight after the post-abort refill");

    // Stale-era ACK (epoch bits 0) cumulatively acking seqs 0..2 — the exact W16
    // out-of-window SACK. Must NOT retire anything.
    tx.onFrameReceived(makeSackAck(2, 0).serialize());
    if (tx.getAvailableSlots() != 1)
        FAIL("Stale-epoch ACK must not retire in-flight frames");
    if (tx.getStats().stale_epoch_acks_ignored != 1)
        FAIL("stale_epoch_acks_ignored counter should be 1");

    // Fresh-era ACK (epoch bits = 1 at bitmap bits 16-17) retires normally.
    tx.onFrameReceived(makeSackAck(2, 1u << 16).serialize());
    if (tx.getAvailableSlots() != 4)
        FAIL("Fresh-epoch ACK should retire all three frames");

    PASS();
    return true;
}

// RX side: adopting a new era from a NON-rebase frame (era head lost) enters the
// unanchored interregnum — no window bookkeeping, TOTAL ack silence (a cumulative
// claim from the old rx_base would fabricate delivery of new-era seqs), FILE
// payloads salvaged. The late rebase frame anchors and normal operation resumes.
bool test_move_epoch_unanchored_wait_for_rebase() {
    TEST("MOVE-EPOCH: unanchored interregnum (ack-silent) until rebase anchor");

    const Bytes file_chunk = {0x02, 0x00, 0x00, 0x00, 0x00, 0xAA};  // FILE_DATA

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 100;

    setenv("ULTRA_ARQ_MOVE_EPOCH", "1", 1);
    SelectiveRepeatARQ rx(config);
    unsetenv("ULTRA_ARQ_MOVE_EPOCH");
    rx.setCallsigns("RX1", "TX1");

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });
    std::vector<Bytes> received;
    rx.setDataReceivedCallback([&](const Bytes& data) { received.push_back(data); });

    // Era 0 anchor point.
    rx.onFrameReceived(makeEpochData(0, Bytes{0x00}, 0, true));
    if (received.size() != 1) FAIL("Era-0 frame should deliver");
    channel.clear();

    // Era 1 arrives WITHOUT the rebase anchor (head lost): adopt, but stay silent.
    rx.onFrameReceived(makeEpochData(5, Bytes{0x00, 0x55}, 1, /*rebase=*/false));
    if (received.size() != 1)
        FAIL("Unanchored TEXT frame must not be delivered");
    if (channel.size() != 0)
        FAIL("Unanchored receiver must not emit any SACK (fabrication hazard)");

    // FILE payloads are salvaged during the interregnum (offset-idempotent layer).
    rx.onFrameReceived(makeEpochData(6, file_chunk, 1, /*rebase=*/false));
    if (received.size() != 2 || received[1] != file_chunk)
        FAIL("Unanchored FILE frame should be salvage-delivered");
    if (channel.size() != 0)
        FAIL("FILE salvage while unanchored must stay ack-silent");

    // Delayed timers must not leak an ack either.
    rx.tick(config.sack_delay_ms * 2);
    if (channel.size() != 0)
        FAIL("No delayed SACK may fire while unanchored");

    // The era base arrives on the sender's RTO resend: anchor + deliver in order.
    rx.onFrameReceived(makeEpochData(4, Bytes{0x04, 0xCC}, 1, /*rebase=*/true));
    if (received.size() != 3 || received[2] != Bytes({0x04, 0xCC}))
        FAIL("Rebase frame should anchor and deliver");
    if (rx.getRxBaseSeq() != 5)
        FAIL("rx_base should be anchored at rebase seq + 1");
    rx.onFrameReceived(makeEpochData(5, Bytes{0x05, 0xDD}, 1, false));
    if (received.size() != 4)
        FAIL("Post-anchor frame should deliver in order");
    rx.tick(config.sack_delay_ms);
    if (channel.size() < 1)
        FAIL("Ack flow should resume after anchoring");

    PASS();
    return true;
}

// Knob OFF (default): flags bits 6-7 / EPOCH_REBASE stay zero, SACK bitmaps carry
// no epoch bits, and a below-window TEXT re-send is dropped exactly as before —
// byte-identical wire behavior.
bool test_move_epoch_knob_off_byte_identical() {
    TEST("MOVE-EPOCH: knob off is byte-identical");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 100;

    SelectiveRepeatARQ tx(config);  // knob unset => OFF
    tx.setCallsigns("TX1", "RX1");
    std::vector<Bytes> transmitted;
    tx.setTransmitCallback([&](const Bytes& data) { transmitted.push_back(data); });
    tx.sendData(Bytes{0x01});
    tx.setCodeRate(CodeRate::R1_2);  // abort path runs, but no epoch machinery
    tx.sendData(Bytes{0x02});
    for (const auto& raw : transmitted) {
        auto f = v2::DataFrame::deserialize(raw);
        if (!f) FAIL("Frame did not parse");
        if ((f->flags & (v2::Flags::EPOCH_MASK | v2::Flags::EPOCH_REBASE)) != 0)
            FAIL("Knob off: epoch flag bits must stay zero");
    }
    if (tx.getStats().stale_epoch_acks_ignored != 0)
        FAIL("Knob off: stale-epoch counter must stay zero");

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });
    std::vector<Bytes> received;
    rx.setDataReceivedCallback([&](const Bytes& data) { received.push_back(data); });

    rx.onFrameReceived(v2::DataFrame::makeData("TX1", "RX1", 0, Bytes{0x00, 0x01}).serialize());
    if (received.size() != 1) FAIL("In-order frame should deliver");
    // Below-window TEXT under a "new era" the OFF receiver cannot see: dropped
    // (seq-keyed dedup), recovery SACK sent, no epoch echo in the bitmap.
    rx.onFrameReceived(makeEpochData(0, Bytes{0x00, 0x02}, 1, true));
    if (received.size() != 1)
        FAIL("Knob off: below-window TEXT must be dropped (legacy dedup)");
    if (channel.size() < 1)
        FAIL("Knob off: out-of-window recovery SACK expected");
    {
        auto ack = v2::ControlFrame::deserialize(channel.receive());
        if (!ack) FAIL("SACK did not parse");
        const uint32_t bitmap = v2::NackPayload::decode(ack->payload).cw_bitmap;
        if ((bitmap & 0xFFFF0000u) != 0)
            FAIL("Knob off: SACK bitmap must carry no epoch bits");
    }

    PASS();
    return true;
}

bool test_rx_out_of_order() {
    TEST("RX handles out-of-order frames");

    ARQConfig config;
    config.window_size = 4;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    std::vector<Bytes> received;
    rx.setDataReceivedCallback([&](const Bytes& data) {
        received.push_back(data);
    });

    // Receive frame 2 first (out of order)
    auto f2 = v2::DataFrame::makeData("TX1", "RX1", 2, Bytes{0x02});
    rx.onFrameReceived(f2.serialize());

    // Should not deliver yet (waiting for 0,1)
    if (!received.empty())
        FAIL("Should not deliver out-of-order frame");

    // Receive frame 0
    auto f0 = v2::DataFrame::makeData("TX1", "RX1", 0, Bytes{0x00});
    rx.onFrameReceived(f0.serialize());

    // Should deliver frame 0 only
    if (received.size() != 1)
        FAIL("Should deliver frame 0");

    // Receive frame 1
    auto f1 = v2::DataFrame::makeData("TX1", "RX1", 1, Bytes{0x01});
    rx.onFrameReceived(f1.serialize());

    // Should now deliver 1 and 2 (in order)
    if (received.size() != 3)
        FAIL("Expected 3 deliveries after receiving frame 1, got " + std::to_string(received.size()));

    // Verify order
    for (int i = 0; i < 3; i++) {
        if (received[i][0] != i)
            FAIL("Frames delivered out of order");
    }

    PASS();
    return true;
}

bool test_timeout_retransmit() {
    TEST("Timeout triggers retransmit");

    ARQConfig config;
    config.window_size = 4;
    config.ack_timeout_ms = 100;
    config.max_retries = 3;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    std::vector<Bytes> transmitted;
    tx.setTransmitCallback([&](const Bytes& data) { transmitted.push_back(data); });

    // Send one frame
    tx.sendData(Bytes{0x01});

    // Initial transmission
    if (transmitted.size() != 1)
        FAIL("Expected 1 frame transmitted");

    // Advance time past timeout
    tx.tick(150);

    // Should have retransmitted
    if (transmitted.size() != 2)
        FAIL("Expected 2 frames (1 initial + 1 retransmit)");

    auto stats = tx.getStats();
    if (stats.retransmissions != 1)
        FAIL("Expected 1 retransmission in stats");

    PASS();
    return true;
}

bool test_timeout_repair_retransmits_only_missing_slot_and_resets_timer() {
    TEST("Timeout repair retransmits only missing slot and resets its timer");

    ARQConfig config;
    config.window_size = 4;
    config.ack_timeout_ms = 100;
    config.max_retries = 4;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    std::vector<Bytes> transmitted;
    tx.setTransmitCallback([&](const Bytes& data) { transmitted.push_back(data); });

    for (int i = 0; i < 3; i++) {
        if (!tx.sendData(Bytes{static_cast<uint8_t>(i)}))
            FAIL("Failed to send DATA seq=" + std::to_string(i));
    }
    if (transmitted.size() != 3)
        FAIL("Expected three initial DATA transmissions");

    // Cumulatively ACK seq=0 and SACK seq=2, leaving only seq=1 as a hole.
    auto sack_seq2 = makeSackAck(0, 0x02u);
    tx.onFrameReceived(sack_seq2.serialize());

    const size_t before_timeout_tx = transmitted.size();
    tx.tick(101);

    if (transmitted.size() != before_timeout_tx + 1)
        FAIL("Timeout should retransmit exactly one missing DATA frame");
    if (!expectDataSeq(transmitted.back(), 1, "timeout repair"))
        return false;

    auto stats = tx.getStats();
    if (stats.timeouts != 1 || stats.retransmissions != 1 ||
        stats.retransmissions_timeout != 1)
        FAIL("Expected one timeout retransmission, got timeouts=" +
             std::to_string(stats.timeouts) + " retransmissions=" +
             std::to_string(stats.retransmissions) + " timeout_retx=" +
             std::to_string(stats.retransmissions_timeout));

    tx.tick(99);
    if (transmitted.size() != before_timeout_tx + 1)
        FAIL("Timeout repair did not reset the slot timer to a full ACK timeout");

    tx.tick(1);
    if (transmitted.size() != before_timeout_tx + 2)
        FAIL("Slot did not retransmit again after the full reset timeout elapsed");
    if (!expectDataSeq(transmitted.back(), 1, "second timeout repair"))
        return false;

    PASS();
    return true;
}

bool test_timeout_window_retransmits_as_one_batch_when_callback_present() {
    TEST("Timeout window retransmits as one batch when callback present");

    ARQConfig config;
    config.window_size = 4;
    config.ack_timeout_ms = 100;
    config.max_retries = 4;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    std::vector<Bytes> transmitted;
    std::vector<std::vector<Bytes>> batches;
    tx.setTransmitCallback([&](const Bytes& data) { transmitted.push_back(data); });
    tx.setTransmitBatchCallback([&](const std::vector<Bytes>& frames) {
        batches.push_back(frames);
    });

    for (int i = 0; i < 4; i++) {
        if (!tx.sendData(Bytes{static_cast<uint8_t>(i)}))
            FAIL("Failed to send DATA seq=" + std::to_string(i));
    }
    if (transmitted.size() != 4)
        FAIL("Expected four initial DATA transmissions");
    if (!batches.empty())
        FAIL("Initial DATA submissions should not use timeout batch callback");

    tx.tick(101);

    if (transmitted.size() != 4)
        FAIL("Timeout batch should not also transmit frames individually");
    if (batches.size() != 1 || batches[0].size() != 4)
        FAIL("Expected one four-frame timeout retransmission batch");
    for (uint16_t seq = 0; seq < 4; ++seq) {
        if (!expectDataSeq(batches[0][seq], seq, "timeout batch"))
            return false;
    }

    auto stats = tx.getStats();
    if (stats.timeouts != 4 || stats.retransmissions_timeout != 4)
        FAIL("Expected four timeout retransmissions in stats");

    PASS();
    return true;
}

// §RETX-PACING (docs/RETX_PACING_DESIGN_2026_07_03.md §1.1): lastAckProgressFrames() is the
// zero-progress ROUND detector's ground truth — (frames retired by base advance) + (newly
// set SACK bits) for the most recent FRESH ack; −1 after consumption; dedup-suppressed
// duplicate acks must NEVER produce a fresh reading (phantom rounds).
bool test_ack_progress_accessor_counts_and_dedups() {
    TEST("lastAckProgressFrames counts base advance + new SACK bits; duplicates stay -1");

    ARQConfig config;
    config.window_size = 8;
    config.ack_timeout_ms = 10000;
    config.max_retries = 5;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");
    tx.setTransmitCallback([](const Bytes&) {});

    if (tx.lastAckProgressFrames() != -1)
        FAIL("initial progress sentinel should be -1 (no ack yet)");

    for (int i = 0; i < 4; i++) {
        if (!tx.sendData(Bytes{static_cast<uint8_t>(i)}))
            FAIL("Failed to send DATA seq=" + std::to_string(i));
    }

    // Fresh ack: cumulative through seq=1 (retires seq 0,1 -> base=2) + SACK bit1
    // (= seq 3 after the advance) -> progress = 2 + 1 = 3.
    tx.onFrameReceived(makeSackAck(1, 0x02u).serialize());
    if (tx.lastAckProgressFrames() != 3)
        FAIL("expected progress 3 (2 retired + 1 new SACK bit), got " +
             std::to_string(tx.lastAckProgressFrames()));

    // Round consumption re-arms the sentinel.
    tx.consumeAckProgress();
    if (tx.lastAckProgressFrames() != -1)
        FAIL("consumeAckProgress should reset the sentinel to -1");

    // The identical ack re-heard inside the dedup window is SUPPRESSED by the existing
    // ack-signature dedup -> it must NOT publish a fresh (phantom-round) reading.
    tx.onFrameReceived(makeSackAck(1, 0x02u).serialize());
    if (tx.lastAckProgressFrames() != -1)
        FAIL("duplicate ack must not publish a progress reading (phantom round)");

    // A fresh ZERO-progress ack (base-1 cumulative, no new SACK bit) publishes 0 —
    // the §1.1 zero-progress round signal.
    tx.onFrameReceived(makeSackAck(1, 0x00u).serialize());
    if (tx.lastAckProgressFrames() != 0)
        FAIL("fresh no-advance/no-new-bit ack should publish progress 0, got " +
             std::to_string(tx.lastAckProgressFrames()));

    PASS();
    return true;
}

// §RETX-PACING §1.3 trigger #2: deferPendingRetransmits(ms) extends every pending slot's
// RTO so tick() cannot blind-fire a timeout batch inside a trough-pacing hold; after the
// hold elapses the RTO fires normally. Acked/inactive slots are untouched (by inspection:
// the loop skips !active || acked).
bool test_defer_pending_retransmits_gates_slot_rto() {
    TEST("deferPendingRetransmits extends pending slot timers across the hold");

    ARQConfig config;
    config.window_size = 4;
    config.ack_timeout_ms = 1000;
    config.max_retries = 10;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    std::vector<Bytes> transmitted;
    tx.setTransmitCallback([&](const Bytes& data) { transmitted.push_back(data); });

    if (!tx.sendData(Bytes{0xAA}))
        FAIL("Failed to send first DATA frame");
    if (!tx.sendData(Bytes{0xBB}))
        FAIL("Failed to send second DATA frame");
    transmitted.clear();

    tx.tick(900);  // 100 ms left on both slot timers
    if (!transmitted.empty())
        FAIL("no retransmit expected before the RTO");

    tx.deferPendingRetransmits(2000);  // the trough hold: timers now at 2100 ms

    tx.tick(1500);  // would have fired the original RTO 1.4 s ago without the deferral
    if (!transmitted.empty())
        FAIL("slot RTO must not blind-fire inside the pacing hold");

    tx.tick(700);  // 600 ms remained -> both slots expire now
    if (transmitted.size() != 2)
        FAIL("both pending frames should retransmit after the hold expires, got " +
             std::to_string(transmitted.size()));

    auto stats = tx.getStats();
    if (stats.timeouts != 2 || stats.retransmissions_timeout != 2)
        FAIL("expected exactly the two post-hold timeout retransmissions");

    PASS();
    return true;
}

bool test_max_retries_failure() {
    TEST("Max retries triggers failure");

    ARQConfig config;
    config.window_size = 4;
    config.ack_timeout_ms = 100;
    config.max_retries = 2;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    tx.setTransmitCallback([](const Bytes&) {});

    bool failed = false;
    tx.setSendCompleteCallback([&](bool success) {
        if (!success) failed = true;
    });

    // Send one frame
    tx.sendData(Bytes{0x01});

    // Timeout twice (exceeds max_retries=2)
    tx.tick(150);  // Retry 1
    tx.tick(150);  // Retry 2 -> failure

    if (!failed)
        FAIL("Expected failure callback");

    auto stats = tx.getStats();
    if (stats.failed != 1)
        FAIL("Expected 1 failure in stats");

    PASS();
    return true;
}

bool test_full_exchange() {
    TEST("Full TX/RX exchange");

    ARQConfig config;
    config.window_size = 4;

    SelectiveRepeatARQ tx(config);
    SelectiveRepeatARQ rx(config);

    tx.setCallsigns("TX1", "RX1");
    rx.setCallsigns("RX1", "TX1");

    // Connect TX -> RX
    tx.setTransmitCallback([&](const Bytes& data) {
        rx.onFrameReceived(data);
    });

    // Connect RX -> TX
    rx.setTransmitCallback([&](const Bytes& data) {
        tx.onFrameReceived(data);
    });

    std::vector<Bytes> received;
    rx.setDataReceivedCallback([&](const Bytes& data) {
        received.push_back(data);
    });

    int completions = 0;
    tx.setSendCompleteCallback([&](bool success) {
        if (success) completions++;
    });

    // Send 10 frames
    for (int i = 0; i < 10; i++) {
        while (!tx.isReadyToSend()) {
            // Wait for ACKs if window full
        }
        tx.sendData(Bytes{static_cast<uint8_t>(i)});
    }

    // All frames should be received
    if (received.size() != 10)
        FAIL("Expected 10 received, got " + std::to_string(received.size()));

    // The last partial batch is intentionally ACKed by the delayed SACK timer.
    rx.tick(config.sack_delay_ms);

    if (completions != 10)
        FAIL("Expected 10 completions, got " + std::to_string(completions));

    // Verify payload integrity
    for (int i = 0; i < 10; i++) {
        if (received[i].size() != 1 || received[i][0] != i)
            FAIL("Payload mismatch at index " + std::to_string(i));
    }

    PASS();
    return true;
}

// ============================================================================
// ack_batch_size decoupling tests (Phase 1b)
// ============================================================================

bool test_ack_batch_threshold_independent() {
    TEST("ack_batch_size threshold fires independently of window_size");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 10000;  // Large so timer doesn't interfere

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    rx.setAckBatchSize(2);  // ACK every 2 frames, not every 4

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    // Feed 4 in-order frames; expect SACK after frames 2 and 4 = 2 SACKs total.
    // Without the decoupling (or with ack_batch_size=0), we'd get either zero
    // (timer didn't fire) or one (after frame 4).
    for (int i = 0; i < 4; i++) {
        auto frame = v2::DataFrame::makeData("TX1", "RX1", i, Bytes{static_cast<uint8_t>(i)});
        rx.onFrameReceived(frame.serialize());
    }

    if (channel.size() != 2)
        FAIL("Expected 2 SACKs at batch=2 over 4 frames, got " + std::to_string(channel.size()));

    PASS();
    return true;
}

bool test_ack_batch_out_of_order_safety_valve() {
    TEST("Out-of-order frame triggers immediate SACK regardless of batch state");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 10000;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    rx.setAckBatchSize(4);  // High batch — without safety valve, no SACK until 4 frames

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    // Feed seq=0 (in order, frames_since_ack=1), then seq=2 (hole at seq=1 →
    // out-of-order). Out-of-order MUST short-circuit the batch threshold and
    // fire SACK immediately.
    auto f0 = v2::DataFrame::makeData("TX1", "RX1", 0, Bytes{0});
    rx.onFrameReceived(f0.serialize());
    if (channel.size() != 0)
        FAIL("Did not expect SACK after 1st in-order frame at batch=4, got " + std::to_string(channel.size()));

    auto f2 = v2::DataFrame::makeData("TX1", "RX1", 2, Bytes{2});
    rx.onFrameReceived(f2.serialize());
    if (channel.size() != 1)
        FAIL("Expected immediate SACK on out-of-order seq=2, got " + std::to_string(channel.size()));

    PASS();
    return true;
}

bool test_final_out_of_order_frame_sends_explicit_frame_nack() {
    TEST("FINAL out-of-order frame sends explicit frame NACK plus SACK");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 10000;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    auto f1 = v2::DataFrame::makeData("TX1", "RX1", 1, Bytes{1});
    f1.flags |= v2::Flags::FINAL;
    rx.onFrameReceived(f1.serialize());

    if (channel.size() != 2)
        FAIL("Expected frame NACK and SACK for FINAL out-of-order gap, got " +
             std::to_string(channel.size()));

    auto nack = v2::ControlFrame::deserialize(channel.receive());
    if (!nack || nack->type != v2::FrameType::NACK || nack->seq != 0)
        FAIL("First control should be explicit NACK for missing seq=0");
    if (v2::NackPayload::decode(nack->payload).cw_bitmap != 0)
        FAIL("Frame NACK should carry cw bitmap=0 for full-frame retransmit");

    auto sack = v2::ControlFrame::deserialize(channel.receive());
    if (!sack || sack->type != v2::FrameType::ACK || sack->seq != 0xFFFF)
        FAIL("Second control should be SACK at base-1 for the same hole");
    if (v2::NackPayload::decode(sack->payload).cw_bitmap != 0x2)
        FAIL("SACK bitmap should confirm seq=1 while seq=0 is missing");

    PASS();
    return true;
}

bool test_more_frag_out_of_order_uses_sack_without_explicit_frame_nack() {
    TEST("MORE_FRAG out-of-order frame keeps guarded SACK-only path");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 10000;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    auto f1 = v2::DataFrame::makeData("TX1", "RX1", 1, Bytes{1});
    f1.flags |= v2::Flags::MORE_FRAG;
    rx.onFrameReceived(f1.serialize());

    if (channel.size() != 1)
        FAIL("Expected only SACK for MORE_FRAG out-of-order gap, got " +
             std::to_string(channel.size()));

    auto sack = v2::ControlFrame::deserialize(channel.receive());
    if (!sack || sack->type != v2::FrameType::ACK)
        FAIL("MORE_FRAG out-of-order control should remain a SACK");

    PASS();
    return true;
}

bool test_explicit_frame_nack_retransmits_before_long_rto() {
    TEST("explicit frame NACK retransmits before long RTO");

    ARQConfig config;
    config.window_size = 4;
    config.ack_timeout_ms = 15000;
    config.sack_delay_ms = 10000;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    ByteChannel channel;
    tx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    if (!tx.sendDataWithFlags(Bytes{0}, v2::Flags::FINAL))
        FAIL("failed to send seq=0");
    if (!tx.sendDataWithFlags(Bytes{1}, v2::Flags::FINAL))
        FAIL("failed to send seq=1");

    if (channel.size() != 2)
        FAIL("expected two original DATA frames");
    (void)channel.receive();
    (void)channel.receive();

    auto nack = v2::ControlFrame::makeNack("RX1", "TX1", 0, 0);
    tx.onFrameReceived(nack.serialize());

    if (channel.size() != 1)
        FAIL("explicit NACK should immediately enqueue one retransmission");
    if (!expectDataSeq(channel.receive(), 0, "explicit NACK retx"))
        return false;

    auto stats = tx.getStats();
    if (stats.timeouts != 0)
        FAIL("NACK retransmit should occur before timeout accounting");
    if (stats.retransmissions_nack != 1 || stats.retransmissions != 1)
        FAIL("Expected exactly one NACK retransmission");
    if (stats.retransmissions_fast_hole != 0 || stats.retransmissions_hole_probe != 0)
        FAIL("Explicit NACK should not count as fast_hole or hole_probe");

    PASS();
    return true;
}

bool test_ack_batch_setter_clamping() {
    TEST("setWindowSize() clamps ack_batch_size when shrinking below");

    ARQConfig config;
    config.window_size = 8;

    SelectiveRepeatARQ rx(config);
    rx.setAckBatchSize(6);

    if (rx.getAckBatchSize() != 6)
        FAIL("Expected ack_batch_size=6 after setAckBatchSize(6), got " + std::to_string(rx.getAckBatchSize()));

    // Shrink window below batch_size. Batch should clamp down.
    rx.setWindowSize(4);

    if (rx.getAckBatchSize() != 4)
        FAIL("Expected ack_batch_size clamped to 4 after window shrink, got " + std::to_string(rx.getAckBatchSize()));

    // Default (batch_size = 0) should NOT be touched by setWindowSize — sentinel
    // 0 means "track window_size implicitly".
    rx.setAckBatchSize(0);
    rx.setWindowSize(2);
    if (rx.getAckBatchSize() != 0)
        FAIL("Expected ack_batch_size=0 sentinel preserved after window change");

    PASS();
    return true;
}

bool test_ack_batch_default_matches_window() {
    TEST("ack_batch_size=0 (default) tracks window_size — bit-identical to prior code");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 10000;
    // ack_batch_size = 0 by default

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    // Feed exactly window_size=4 in-order frames. Threshold path should fire 1 SACK.
    for (int i = 0; i < 4; i++) {
        auto frame = v2::DataFrame::makeData("TX1", "RX1", i, Bytes{static_cast<uint8_t>(i)});
        rx.onFrameReceived(frame.serialize());
    }

    if (channel.size() != 1)
        FAIL("Expected 1 SACK at threshold (default = window_size = 4), got " + std::to_string(channel.size()));

    PASS();
    return true;
}

bool test_ack_batch_defers_more_frag_until_tail() {
    TEST("ack_batch_size threshold defers MORE_FRAG frames until stream tail");

    ARQConfig config;
    config.window_size = 8;
    config.sack_delay_ms = 5000;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    rx.setAckBatchSize(4);
    rx.setSackDelayShort(50);

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    for (int i = 0; i < 4; i++) {
        auto frame = v2::DataFrame::makeData("TX1", "RX1", i, Bytes{static_cast<uint8_t>(i)});
        frame.flags |= v2::Flags::MORE_FRAG;
        rx.onFrameReceived(frame.serialize());
    }

    if (channel.size() != 0)
        FAIL("MORE_FRAG batch threshold sent a mid-stream SACK");

    rx.tick(1000);
    if (channel.size() != 0)
        FAIL("Long in-stream SACK timer fired too early");

    auto tail = v2::DataFrame::makeData("TX1", "RX1", 4, Bytes{4});
    rx.onFrameReceived(tail.serialize());
    if (channel.size() != 1)
        FAIL("Stream-tail threshold did not send immediate SACK");

    auto ack = v2::ControlFrame::deserialize(channel.receive());
    if (!ack || ack->type != v2::FrameType::ACK || ack->seq != 4)
        FAIL("Tail SACK did not cumulatively acknowledge seq=4");

    PASS();
    return true;
}

bool test_ack_batch_can_fire_through_more_frag_for_mc_dpsk() {
    TEST("ack_batch_size threshold can fire through MORE_FRAG when enabled");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 10000;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    rx.setAckBatchThroughMoreFrag(true);

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    for (int i = 0; i < 4; i++) {
        auto frame = v2::DataFrame::makeData("TX1", "RX1", i, Bytes{static_cast<uint8_t>(i)});
        frame.flags |= v2::Flags::MORE_FRAG;
        rx.onFrameReceived(frame.serialize());
    }

    if (channel.size() != 1)
        FAIL("Expected threshold SACK through MORE_FRAG, got " + std::to_string(channel.size()));

    auto ack = v2::ControlFrame::deserialize(channel.receive());
    if (!ack || ack->type != v2::FrameType::ACK || ack->seq != 3)
        FAIL("Threshold SACK through MORE_FRAG did not cumulatively acknowledge seq=3");

    PASS();
    return true;
}

bool test_wide_sack_bitmap_serializes_beyond_8_frames() {
    TEST("SACK bitmap preserves bits beyond 8-frame legacy window");

    ARQConfig config;
    config.window_size = 12;
    config.sack_delay_ms = 10000;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    // An out-of-order frame at seq=10 must be represented as bit 10 in the
    // SACK bitmap. The old 8-bit path truncated this information.
    auto f10 = v2::DataFrame::makeData("TX1", "RX1", 10, Bytes{10});
    rx.onFrameReceived(f10.serialize());

    if (channel.size() != 1)
        FAIL("Expected one immediate SACK for out-of-order seq=10");

    auto ctrl = v2::ControlFrame::deserialize(channel.receive());
    if (!ctrl)
        FAIL("Serialized SACK did not parse as a control frame");

    auto np = v2::NackPayload::decode(ctrl->payload);
    const uint32_t expected = 1u << 10;
    if (np.cw_bitmap != expected)
        FAIL("Expected SACK bitmap 0x" + std::to_string(expected) +
             ", got " + std::to_string(np.cw_bitmap));

    PASS();
    return true;
}

bool test_cumulative_ack_repeats_when_enabled() {
    TEST("cumulative ACK repeats when ack_repeat_count > 1");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 10000;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    rx.setAckRepeatCount(2);
    rx.setAckRepeatDelay(100);

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    for (int i = 0; i < 4; i++) {
        auto frame = v2::DataFrame::makeData("TX1", "RX1", i, Bytes{static_cast<uint8_t>(i)});
        if (i == 3) {
            frame.flags |= v2::Flags::FINAL;
        }
        rx.onFrameReceived(frame.serialize());
    }

    if (channel.size() != 1)
        FAIL("Expected primary cumulative ACK only before repeat timer");

    auto primary = v2::ControlFrame::deserialize(channel.receive());
    if (!primary || primary->seq != 3)
        FAIL("Primary cumulative ACK did not acknowledge seq=3");
    if (v2::NackPayload::decode(primary->payload).cw_bitmap != 0)
        FAIL("Expected primary cumulative ACK bitmap=0");

    rx.tick(99);
    if (channel.size() != 0)
        FAIL("Cumulative ACK repeat fired before configured delay");

    rx.tick(40);
    if (channel.size() != 1)
        FAIL("Expected one delayed cumulative ACK repeat");

    auto repeat = v2::ControlFrame::deserialize(channel.receive());
    if (!repeat || repeat->seq != 3)
        FAIL("Delayed cumulative ACK repeat did not acknowledge seq=3");
    if (v2::NackPayload::decode(repeat->payload).cw_bitmap != 0)
        FAIL("Expected delayed cumulative ACK repeat bitmap=0");

    auto stats = rx.getStats();
    if (stats.sacks_sent != 1)
        FAIL("ACK repeat should not increment sacks_sent");
    if (stats.acks_sent != 2)
        FAIL("Expected acks_sent=2 including repeat, got " + std::to_string(stats.acks_sent));

    PASS();
    return true;
}

bool test_non_final_ack_repeat_waits_past_peer_burst_guard() {
    TEST("non-final ACK repeat waits past peer burst guard");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 500;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    rx.setAckRepeatCount(2);
    rx.setAckRepeatDelay(100);

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    for (int i = 0; i < 4; i++) {
        auto frame = v2::DataFrame::makeData("TX1", "RX1", i, Bytes{static_cast<uint8_t>(i)});
        rx.onFrameReceived(frame.serialize());
    }

    if (channel.size() != 1)
        FAIL("Expected primary ACK only before guarded repeat timer");

    auto primary = v2::ControlFrame::deserialize(channel.receive());
    if (!primary || primary->seq != 3)
        FAIL("Primary non-final ACK did not acknowledge seq=3");

    rx.tick(500);
    if (channel.size() != 0)
        FAIL("Non-final ACK repeat fired before peer burst guard elapsed");

    rx.tick(200);
    if (channel.size() != 1)
        FAIL("Expected one delayed non-final ACK repeat after peer burst guard");

    auto repeat = v2::ControlFrame::deserialize(channel.receive());
    if (!repeat || repeat->seq != 3)
        FAIL("Guarded ACK repeat did not acknowledge seq=3");

    PASS();
    return true;
}

bool test_ack_repeat_peer_guard_can_be_zero_after_physical_hold() {
    TEST("ACK repeat peer guard can be zero after physical SACK hold");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 500;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    rx.setAckRepeatCount(2);
    rx.setAckRepeatDelay(100);
    rx.setAckRepeatPeerBurstGuardMs(0);

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    for (int i = 0; i < 4; i++) {
        auto frame = v2::DataFrame::makeData("TX1", "RX1", i, Bytes{static_cast<uint8_t>(i)});
        rx.onFrameReceived(frame.serialize());
    }

    if (channel.size() != 1)
        FAIL("Expected primary ACK only before unguarded repeat timer");

    auto primary = v2::ControlFrame::deserialize(channel.receive());
    if (!primary || primary->seq != 3)
        FAIL("Primary non-final ACK did not acknowledge seq=3");

    rx.tick(99);
    if (channel.size() != 0)
        FAIL("Unguarded ACK repeat fired before configured delay");

    rx.tick(40);
    if (channel.size() != 1)
        FAIL("Expected one delayed ACK repeat without re-applying the physical SACK hold");

    auto repeat = v2::ControlFrame::deserialize(channel.receive());
    if (!repeat || repeat->seq != 3)
        FAIL("Unguarded ACK repeat did not acknowledge seq=3");

    PASS();
    return true;
}

bool test_hole_sack_repeat_stays_prompt() {
    TEST("hole-bearing SACK repeat stays prompt");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 500;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    rx.setAckRepeatCount(2);
    rx.setAckRepeatDelay(100);

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    auto frame = v2::DataFrame::makeData("TX1", "RX1", 1, Bytes{1});
    rx.onFrameReceived(frame.serialize());

    if (channel.size() != 1)
        FAIL("Expected primary out-of-order SACK before repeat timer");

    auto primary = v2::ControlFrame::deserialize(channel.receive());
    if (!primary)
        FAIL("Primary SACK did not parse");
    if (v2::NackPayload::decode(primary->payload).cw_bitmap == 0)
        FAIL("Expected primary SACK to carry a hole bitmap");

    rx.tick(99);
    if (channel.size() != 0)
        FAIL("Hole-bearing SACK repeat fired before configured delay");

    rx.tick(40);
    if (channel.size() != 1)
        FAIL("Hole-bearing SACK repeat was incorrectly held by peer burst guard");

    auto repeat = v2::ControlFrame::deserialize(channel.receive());
    if (!repeat)
        FAIL("Repeated SACK did not parse");
    if (v2::NackPayload::decode(repeat->payload).cw_bitmap == 0)
        FAIL("Expected repeated SACK to carry the hole bitmap");

    PASS();
    return true;
}

bool test_cumulative_ack_repeat_coalesces_superseded_state() {
    TEST("new cumulative ACK coalesces superseded pending repeat");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 500;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    rx.setAckRepeatCount(2);
    rx.setAckRepeatDelay(100);

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    for (int i = 0; i < 4; i++) {
        auto frame = v2::DataFrame::makeData("TX1", "RX1", i, Bytes{static_cast<uint8_t>(i)});
        rx.onFrameReceived(frame.serialize());
    }
    if (channel.size() != 1)
        FAIL("Expected first primary ACK");

    for (int i = 4; i < 8; i++) {
        auto frame = v2::DataFrame::makeData("TX1", "RX1", i, Bytes{static_cast<uint8_t>(i)});
        rx.onFrameReceived(frame.serialize());
    }
    if (channel.size() != 2)
        FAIL("Expected second primary ACK before repeat timers");

    auto first = v2::ControlFrame::deserialize(channel.receive());
    auto second = v2::ControlFrame::deserialize(channel.receive());
    if (!first || first->seq != 3 || !second || second->seq != 7)
        FAIL("Expected primary ACK sequence 3 then 7");

    rx.tick(700);
    if (channel.size() != 1)
        FAIL("Expected only the latest cumulative ACK repeat after coalescing");

    auto repeat = v2::ControlFrame::deserialize(channel.receive());
    if (!repeat || repeat->seq != 7)
        FAIL("Expected repeated ACK for latest seq=7, not superseded seq=3");

    auto stats = rx.getStats();
    if (stats.ack_repeat_jobs_coalesced != 1)
        FAIL("Expected one superseded repeat coalesced, got " +
             std::to_string(stats.ack_repeat_jobs_coalesced));

    PASS();
    return true;
}

// ============================================================================
// Stream-aware SACK timer tests (post-Phase 3 plan)
// ============================================================================

bool test_sack_timer_final_short_collapses_long() {
    TEST("FINAL frame collapses long timer to short via std::min");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 500;
    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    rx.setSackDelayShort(50);

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    // Frame 0 with MORE_FRAG=1 → timer arms at 500ms
    auto f0 = v2::DataFrame::makeData("TX1", "RX1", 0, Bytes{0});
    f0.flags |= v2::Flags::MORE_FRAG;
    rx.onFrameReceived(f0.serialize());
    rx.tick(60);  // 440ms remaining
    if (channel.size() != 0)
        FAIL("Long timer fired prematurely after 60ms");

    // Frame 1 with FINAL → pick_ms=50, std::min(440, 50) = 50
    auto f1 = v2::DataFrame::makeData("TX1", "RX1", 1, Bytes{1});
    f1.flags |= v2::Flags::FINAL;
    rx.onFrameReceived(f1.serialize());
    rx.tick(60);  // 50 - 60 ≤ 0 → fires
    if (channel.size() != 1)
        FAIL("Short-collapsed timer did not fire after 60ms tick (got " +
             std::to_string(channel.size()) + " SACKs)");

    PASS();
    return true;
}

bool test_sack_timer_message_boundary_uses_long_without_final() {
    TEST("MORE_FRAG=0 message boundary uses long timer unless FINAL is set");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 500;
    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    rx.setSackDelayShort(50);

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    auto f0 = v2::DataFrame::makeData("TX1", "RX1", 0, Bytes{0});
    f0.flags |= v2::Flags::MORE_FRAG;
    rx.onFrameReceived(f0.serialize());
    rx.tick(60);  // 440ms remaining on the long timer

    auto f1 = v2::DataFrame::makeData("TX1", "RX1", 1, Bytes{1});
    // No MORE_FRAG, but also no FINAL: this can be an ordinary message
    // boundary inside a multi-message physical burst.
    rx.onFrameReceived(f1.serialize());
    rx.tick(60);
    if (channel.size() != 0)
        FAIL("Non-FINAL message boundary incorrectly used short SACK timer");

    rx.tick(400);
    if (channel.size() != 1)
        FAIL("Long SACK timer did not fire for non-FINAL message boundary");

    PASS();
    return true;
}

bool test_out_of_order_sack_can_defer_until_physical_hold() {
    TEST("out-of-order SACK can defer until physical hold clears");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 500;
    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    rx.setImmediateOutOfOrderSackEnabled(false);

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    auto f1 = v2::DataFrame::makeData("TX1", "RX1", 1, Bytes{1});
    f1.flags |= v2::Flags::MORE_FRAG;
    rx.onFrameReceived(f1.serialize());
    if (channel.size() != 0)
        FAIL("Out-of-order MORE_FRAG frame sent immediate SACK despite physical hold");

    rx.tick(499);
    if (channel.size() != 0)
        FAIL("Deferred out-of-order SACK fired before physical hold");

    rx.tick(1);
    if (channel.size() != 1)
        FAIL("Deferred out-of-order SACK did not fire after physical hold");

    auto sack = v2::ControlFrame::deserialize(channel.receive());
    if (!sack || sack->seq != 65535)
        FAIL("Deferred out-of-order SACK did not report base-1 for missing seq=0");
    if (v2::NackPayload::decode(sack->payload).cw_bitmap == 0)
        FAIL("Deferred out-of-order SACK should carry a hole bitmap");

    PASS();
    return true;
}

bool test_sack_timer_more_frag_does_not_extend() {
    TEST("MORE_FRAG=1 subsequent frames do not extend long timer");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 500;
    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    rx.setSackDelayShort(50);

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    auto f0 = v2::DataFrame::makeData("TX1", "RX1", 0, Bytes{0});
    f0.flags |= v2::Flags::MORE_FRAG;
    rx.onFrameReceived(f0.serialize());
    rx.tick(450);  // ~50ms remaining

    auto f1 = v2::DataFrame::makeData("TX1", "RX1", 1, Bytes{1});
    f1.flags |= v2::Flags::MORE_FRAG;
    rx.onFrameReceived(f1.serialize());
    // pick_ms = 500, but std::min(50, 500) keeps 50 — does NOT extend
    rx.tick(60);
    if (channel.size() != 1)
        FAIL("Timer did not fire on schedule with subsequent MORE_FRAG=1 frame (extended unexpectedly?)");

    PASS();
    return true;
}

bool test_sack_timer_slides_on_more_frag_when_enabled() {
    TEST("MORE_FRAG=1 subsequent frames slide quiet SACK timer when enabled");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 500;
    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    rx.setSackDelayShort(50);
    rx.setSackDelaySlidesOnData(true);

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    auto f0 = v2::DataFrame::makeData("TX1", "RX1", 0, Bytes{0});
    f0.flags |= v2::Flags::MORE_FRAG;
    rx.onFrameReceived(f0.serialize());
    rx.tick(450);  // ~50ms remaining on the first quiet timer

    auto f1 = v2::DataFrame::makeData("TX1", "RX1", 1, Bytes{1});
    f1.flags |= v2::Flags::MORE_FRAG;
    rx.onFrameReceived(f1.serialize());
    rx.tick(60);
    if (channel.size() != 0)
        FAIL("Sliding quiet timer fired before the newest MORE_FRAG frame's delay elapsed");

    rx.tick(450);
    if (channel.size() != 1)
        FAIL("Sliding quiet timer did not fire after the re-armed SACK delay");

    auto ack = v2::ControlFrame::deserialize(channel.receive());
    if (!ack || ack->type != v2::FrameType::ACK || ack->seq != 1)
        FAIL("Sliding quiet timer SACK did not cumulatively acknowledge the newest frame");

    PASS();
    return true;
}

bool test_sack_delay_short_zero_sentinel_preserves_legacy() {
    TEST("sack_delay_short=0 sentinel uses sack_delay_ms even for FINAL");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 120;
    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    // Do NOT call setSackDelayShort — sack_delay_short_ms_ stays 0 (sentinel)

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    // FINAL frame — under sentinel should arm at 120ms (sack_delay_ms), not
    // at 0 or any short-override value.
    auto f0 = v2::DataFrame::makeData("TX1", "RX1", 0, Bytes{0});
    f0.flags |= v2::Flags::FINAL;
    rx.onFrameReceived(f0.serialize());
    rx.tick(50);
    if (channel.size() != 0)
        FAIL("Timer fired too early under sentinel (50ms < 120ms expected)");
    rx.tick(80);
    if (channel.size() != 1)
        FAIL("Timer did not fire after 130ms total under sentinel");

    PASS();
    return true;
}

bool test_partial_mc_dpsk_cw_nack_and_merge() {
    TEST("Partial MC-DPSK CW state sends NACK and merges later CWs");

    ARQConfig config;
    config.window_size = 4;
    config.ack_timeout_ms = 1000;

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");

    ByteChannel channel;
    Bytes delivered;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });
    rx.setDataReceivedCallback([&](const Bytes& data) { delivered = data; });

    Bytes payload(50, 0x42);
    auto frame = v2::DataFrame::makeData("TX1", "RX1", 0, payload, CodeRate::R1_4);
    auto serialized = frame.serialize();
    auto cws = v2::splitIntoCodewords(serialized);
    if (cws.size() != 4) {
        FAIL("test fixture expected a 4-CW MC-DPSK frame");
    }

    v2::PartialFrameCodewords first;
    first.type = frame.type;
    first.flags = frame.flags;
    first.seq = frame.seq;
    first.src_hash = frame.src_hash;
    first.dst_hash = frame.dst_hash;
    first.total_cw = static_cast<uint8_t>(cws.size());
    first.data.assign(cws.size(), Bytes{});
    first.decoded_bitmap = (1u << 0) | (1u << 2);
    first.data[0] = cws[0];
    first.data[2] = cws[2];

    rx.onPartialFrame(first);

    if (channel.size() != 1) {
        FAIL("partial frame should emit one CW_NACK");
    }
    auto nack = v2::ControlFrame::deserialize(channel.receive());
    if (!nack || nack->type != v2::FrameType::NACK) {
        FAIL("partial frame response was not a NACK");
    }
    auto np = v2::NackPayload::decode(nack->payload);
    if (np.frame_seq != 0 || np.cw_bitmap != 0x0A) {
        FAIL("CW_NACK bitmap should request CW1 and CW3");
    }
    if (!delivered.empty()) {
        FAIL("partial frame delivered payload before all CWs arrived");
    }

    v2::PartialFrameCodewords second = first;
    second.decoded_bitmap = (1u << 0) | (1u << 1) | (1u << 3);
    second.data[1] = cws[1];
    second.data[3] = cws[3];

    rx.onPartialFrame(second);

    if (delivered != payload) {
        FAIL("merged partial CWs did not deliver original payload");
    }
    auto stats = rx.getStats();
    if (stats.partial_frames_completed != 1 || stats.cw_nacks_sent != 1) {
        FAIL("partial completion/NACK stats not updated");
    }

    PASS();
    return true;
}

bool test_cw_nack_triggers_compact_data_repair_phase2() {
    TEST("CW_NACK triggers compact DATA_REPAIR in Phase 2");

    ARQConfig config;
    config.window_size = 4;
    config.ack_timeout_ms = 1000;

    SelectiveRepeatARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    ByteChannel channel;
    tx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    if (!tx.sendData(Bytes(50, 0x24))) {
        FAIL("initial send failed");
    }
    if (channel.size() != 1) {
        FAIL("initial DATA was not transmitted");
    }
    Bytes full_frame = channel.receive();
    auto original = v2::DataFrame::deserialize(full_frame);
    if (!original) {
        FAIL("initial DATA frame did not deserialize");
    }
    auto original_cws = v2::splitIntoCodewords(full_frame);
    if (original_cws.size() != 4) {
        FAIL("test fixture expected a 4-CW initial frame");
    }

    auto nack = v2::ControlFrame::makeNack("RX1", "TX1", 0, 0x0A);
    tx.onFrameReceived(nack.serialize());

    if (channel.size() != 1) {
        FAIL("CW_NACK should emit one DATA_REPAIR frame");
    }
    auto repair = v2::DataRepairFrame::deserialize(channel.receive());
    if (!repair) {
        FAIL("CW_NACK response was not a valid DATA_REPAIR frame");
    }
    if (repair->target_seq != 0 || repair->repair_bitmap != 0x0A ||
        repair->repair_count != 2 || repair->repair_codewords.size() != 2) {
        FAIL("DATA_REPAIR header did not carry requested CW bitmap/count");
    }
    if (repair->repair_codewords[0] != original_cws[1] ||
        repair->repair_codewords[1] != original_cws[3]) {
        FAIL("DATA_REPAIR did not carry the original missing info CWs");
    }
    auto stats = tx.getStats();
    if (stats.cw_nacks_received != 1 || stats.retransmissions_nack != 1 ||
        stats.data_repairs_sent != 1 || stats.data_repair_cws_sent != 2) {
        FAIL("DATA_REPAIR retransmission stats not updated");
    }
    tx.onFrameReceived(nack.serialize());
    if (channel.size() != 0) {
        FAIL("duplicate CW_NACK should not emit a second DATA_REPAIR or full DATA frame");
    }
    tx.tick(999);
    if (channel.size() != 0) {
        FAIL("DATA_REPAIR guard should suppress shadow full-frame retx before cooldown");
    }
    tx.tick(10000);
    if (channel.size() != 1) {
        FAIL("DATA_REPAIR guard expiry should permit full-frame fallback");
    }
    auto fallback = v2::DataFrame::deserialize(channel.receive());
    if (!fallback || fallback->seq != original->seq || fallback->payload != original->payload) {
        FAIL("full-frame fallback after DATA_REPAIR guard was invalid");
    }

    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    Bytes delivered;
    rx.setDataReceivedCallback([&](const Bytes& data) { delivered = data; });
    ByteChannel rx_channel;
    rx.setTransmitCallback([&](const Bytes& data) { rx_channel.send(data); });

    v2::PartialFrameCodewords first;
    first.type = original->type;
    first.flags = original->flags;
    first.seq = original->seq;
    first.src_hash = original->src_hash;
    first.dst_hash = original->dst_hash;
    first.total_cw = static_cast<uint8_t>(original_cws.size());
    first.data.assign(original_cws.size(), Bytes{});
    first.decoded_bitmap = (1u << 0) | (1u << 2);
    first.data[0] = original_cws[0];
    first.data[2] = original_cws[2];
    rx.onPartialFrame(first);
    if (rx_channel.size() != 1) {
        FAIL("receiver partial state should emit one CW_NACK before repair");
    }
    (void)rx_channel.receive();

    rx.onFrameReceived(repair->serialize());
    if (delivered != Bytes(50, 0x24)) {
        FAIL("DATA_REPAIR did not merge and deliver original payload");
    }
    auto rx_stats = rx.getStats();
    if (rx_stats.data_repairs_received != 1 || rx_stats.data_repair_cws_merged != 2 ||
        rx_stats.partial_frames_completed != 1) {
        FAIL("DATA_REPAIR receive/merge stats not updated");
    }

    PASS();
    return true;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== Selective Repeat ARQ Test Suite (v2) ===\n\n";

    // Run new Phase 1b + stream-aware tests FIRST so they're observable even
    // if a later existing test hangs (test_full_exchange has a known hang
    // issue, unrelated to these changes).
    std::cout << "ack_batch_size Decoupling Tests (Phase 1b):\n";
    test_ack_batch_threshold_independent();
    test_ack_batch_out_of_order_safety_valve();
    test_final_out_of_order_frame_sends_explicit_frame_nack();
    test_more_frag_out_of_order_uses_sack_without_explicit_frame_nack();
    test_explicit_frame_nack_retransmits_before_long_rto();
    test_ack_batch_setter_clamping();
    test_ack_batch_default_matches_window();
    test_ack_batch_defers_more_frag_until_tail();
    test_ack_batch_can_fire_through_more_frag_for_mc_dpsk();
    test_wide_sack_bitmap_serializes_beyond_8_frames();
    test_cumulative_ack_repeats_when_enabled();
    test_non_final_ack_repeat_waits_past_peer_burst_guard();
    test_ack_repeat_peer_guard_can_be_zero_after_physical_hold();
    test_hole_sack_repeat_stays_prompt();
    test_cumulative_ack_repeat_coalesces_superseded_state();

    std::cout << "\nStream-Aware SACK Timer Tests:\n";
    test_sack_timer_final_short_collapses_long();
    test_sack_timer_message_boundary_uses_long_without_final();
    test_out_of_order_sack_can_defer_until_physical_hold();
    test_sack_timer_more_frag_does_not_extend();
    test_sack_timer_slides_on_more_frag_when_enabled();
    test_sack_delay_short_zero_sentinel_preserves_legacy();

    std::cout << "\nARQ Boundary/Property Tests:\n";
    test_partial_mc_dpsk_cw_nack_and_merge();
    test_cw_nack_triggers_compact_data_repair_phase2();
    test_stale_ack_older_than_base_minus_one_is_ignored();
    test_future_ack_too_far_ahead_is_ignored();
    test_duplicate_sack_hole_is_suppressed_without_duplicate_retx_accounting();
    test_hole_probe_not_rearmed_while_fast_hole_in_flight();
    test_duplicate_data_is_not_delivered_twice_and_sends_recovery_sack();
    test_timeout_repair_retransmits_only_missing_slot_and_resets_timer();
    test_timeout_window_retransmits_as_one_batch_when_callback_present();
    test_ack_progress_accessor_counts_and_dedups();
    test_defer_pending_retransmits_gates_slot_rto();

    std::cout << "\nBasic Tests:\n";
    test_create_sr_arq();
    test_send_single_frame();
    test_data_flags_preserve_version_bit();
    test_code_rate_change_aborts_in_flight_fixed_frames();
    test_send_window_full();
    test_receive_ack();

    std::cout << "\nReceiver Tests:\n";
    test_rx_in_order();
    test_below_window_file_salvage();
    test_rx_out_of_order();

    std::cout << "\nMOVE-EPOCH Tests (BUG-ARQ-SEQ-COLLISION structural fix):\n";
    test_move_epoch_bump_on_abort_and_stamp();
    test_move_epoch_bump_on_cw_abort_same_rate();
    test_move_epoch_regrid_resend_accepted();
    test_move_epoch_stale_ack_ignored();
    test_move_epoch_unanchored_wait_for_rebase();
    test_move_epoch_knob_off_byte_identical();

    std::cout << "\nRetransmission Tests:\n";
    test_timeout_retransmit();
    test_max_retries_failure();

    std::cout << "\nIntegration Tests:\n";
    test_full_exchange();

    std::cout << "\n=== Results: " << tests_passed << "/" << tests_run << " tests passed ===\n";

    if (tests_passed == tests_run) {
        std::cout << "All tests PASSED!\n";
        return 0;
    } else {
        std::cout << "Some tests FAILED!\n";
        return 1;
    }
}
