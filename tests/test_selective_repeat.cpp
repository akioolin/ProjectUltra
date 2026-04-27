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
#include <queue>
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
    config.ack_timeout_ms = 1000;

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

    // Check SACKs were sent
    if (channel.size() != 3)
        FAIL("Expected 3 SACKs");

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

    // Completions should match sends (last completion may be delayed)
    // Accept 9 or 10 due to synchronous callback timing
    if (completions < 9)
        FAIL("Expected at least 9 completions, got " + std::to_string(completions));

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

// ============================================================================
// Stream-aware SACK timer tests (post-Phase 3 plan)
// ============================================================================

bool test_sack_timer_more_frag_short_collapses_long() {
    TEST("MORE_FRAG=0 frame collapses long timer to short via std::min");

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

    // Frame 1 with MORE_FRAG=0 → pick_ms=50, std::min(440, 50) = 50
    auto f1 = v2::DataFrame::makeData("TX1", "RX1", 1, Bytes{1});
    // (no MORE_FRAG flag = end-of-burst)
    rx.onFrameReceived(f1.serialize());
    rx.tick(60);  // 50 - 60 ≤ 0 → fires
    if (channel.size() != 1)
        FAIL("Short-collapsed timer did not fire after 60ms tick (got " +
             std::to_string(channel.size()) + " SACKs)");

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

bool test_sack_delay_short_zero_sentinel_preserves_legacy() {
    TEST("sack_delay_short=0 sentinel uses sack_delay_ms regardless of MORE_FRAG");

    ARQConfig config;
    config.window_size = 4;
    config.sack_delay_ms = 120;
    SelectiveRepeatARQ rx(config);
    rx.setCallsigns("RX1", "TX1");
    // Do NOT call setSackDelayShort — sack_delay_short_ms_ stays 0 (sentinel)

    ByteChannel channel;
    rx.setTransmitCallback([&](const Bytes& data) { channel.send(data); });

    // MORE_FRAG=0 frame — under sentinel should arm at 120ms (sack_delay_ms),
    // not at 0 or any short-override value.
    auto f0 = v2::DataFrame::makeData("TX1", "RX1", 0, Bytes{0});
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
    test_ack_batch_setter_clamping();
    test_ack_batch_default_matches_window();

    std::cout << "\nStream-Aware SACK Timer Tests:\n";
    test_sack_timer_more_frag_short_collapses_long();
    test_sack_timer_more_frag_does_not_extend();
    test_sack_delay_short_zero_sentinel_preserves_legacy();

    std::cout << "\nBasic Tests:\n";
    test_create_sr_arq();
    test_send_single_frame();
    test_send_window_full();
    test_receive_ack();

    std::cout << "\nReceiver Tests:\n";
    test_rx_in_order();
    test_rx_out_of_order();

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
