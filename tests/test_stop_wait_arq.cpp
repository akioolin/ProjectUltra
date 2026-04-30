#include "protocol/arq.hpp"
#include "protocol/arq_interface.hpp"
#include "protocol/frame_v2.hpp"

#include <iostream>
#include <queue>
#include <string>
#include <vector>

using ultra::Bytes;
using namespace ultra::protocol;

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

class ByteQueue {
public:
    void send(const Bytes& data) { queue_.push(data); }
    bool empty() const { return queue_.empty(); }
    size_t size() const { return queue_.size(); }
    Bytes pop() {
        Bytes data = queue_.front();
        queue_.pop();
        return data;
    }

private:
    std::queue<Bytes> queue_;
};

void test_factory_and_ack_completion() {
    auto arq = createARQController(ARQMode::STOP_AND_WAIT);
    CHECK(arq->getMode() == ARQMode::STOP_AND_WAIT, "factory should create stop-and-wait ARQ");
    CHECK(arqModeToString(ARQMode::STOP_AND_WAIT) == std::string("Stop-and-Wait"),
          "mode string should be stable");

    ByteQueue tx_queue;
    int completions = 0;
    bool last_success = false;

    arq->setCallsigns("TX1", "RX1");
    arq->setTransmitCallback([&](const Bytes& data) { tx_queue.send(data); });
    arq->setSendCompleteCallback([&](bool success) {
        ++completions;
        last_success = success;
    });

    CHECK(arq->isReadyToSend(), "new ARQ should be ready");
    CHECK(arq->getAvailableSlots() == 1, "stop-and-wait should expose one slot");
    CHECK(arq->sendData(Bytes{0x10, 0x20, 0x30}), "send should accept data");
    CHECK(!arq->isReadyToSend(), "ARQ should wait for ACK after send");
    CHECK(arq->getAvailableSlots() == 0, "slot should be consumed while waiting");
    CHECK(!arq->sendData(Bytes{0xFF}), "busy stop-and-wait should reject a second send");
    CHECK(tx_queue.size() == 1, "one DATA frame should be transmitted");

    auto data_frame = v2::DataFrame::deserialize(tx_queue.pop());
    CHECK(static_cast<bool>(data_frame), "transmitted DATA frame should parse");
    CHECK(data_frame->seq == 0, "first DATA sequence should be zero");
    CHECK(data_frame->payload == Bytes({0x10, 0x20, 0x30}), "payload should round-trip into frame");

    auto wrong_ack = v2::ControlFrame::makeAck("RX1", "TX1", 3);
    arq->onFrameReceived(wrong_ack.serialize());
    CHECK(completions == 0, "wrong ACK sequence should be ignored");
    CHECK(!arq->isReadyToSend(), "wrong ACK must not free the slot");

    auto ack = v2::ControlFrame::makeAck("RX1", "TX1", 0);
    arq->onFrameReceived(ack.serialize());
    CHECK(completions == 1, "matching ACK should complete the send");
    CHECK(last_success, "matching ACK should report success");
    CHECK(arq->isReadyToSend(), "matching ACK should free the slot");

    auto stats = arq->getStats();
    CHECK(stats.frames_sent == 1, "frames_sent should count original send only");
    CHECK(stats.acks_received == 1, "acks_received should count matching ACK only");
}

void test_receive_data_duplicate_and_out_of_order() {
    StopAndWaitARQ rx;
    rx.setCallsigns("RX1", "TX1");

    ByteQueue ack_queue;
    std::vector<Bytes> delivered;
    rx.setTransmitCallback([&](const Bytes& data) { ack_queue.send(data); });
    rx.setDataReceivedCallback([&](const Bytes& data) { delivered.push_back(data); });

    auto frame0 = v2::DataFrame::makeData("TX1", "RX1", 0, Bytes{0xA0, 0xA1});
    frame0.flags |= v2::Flags::MORE_FRAG;
    rx.onFrameReceived(frame0.serialize());

    CHECK(delivered.size() == 1, "in-order DATA should be delivered once");
    CHECK(delivered[0] == Bytes({0xA0, 0xA1}), "delivered payload should match");
    CHECK(rx.lastRxHadMoreData(), "MORE_FRAG should be latched");
    CHECK((rx.lastRxFlags() & v2::Flags::MORE_FRAG) != 0, "last flags should expose MORE_FRAG");
    CHECK(ack_queue.size() == 1, "in-order DATA should generate ACK");
    auto ack0 = v2::ControlFrame::deserialize(ack_queue.pop());
    CHECK(ack0 && ack0->type == v2::FrameType::ACK && ack0->seq == 0,
          "in-order DATA should ACK seq0");

    rx.onFrameReceived(frame0.serialize());
    CHECK(delivered.size() == 1, "duplicate DATA must not be delivered twice");
    CHECK(ack_queue.size() == 1, "duplicate DATA should be re-ACKed");
    auto dup_ack = v2::ControlFrame::deserialize(ack_queue.pop());
    CHECK(dup_ack && dup_ack->type == v2::FrameType::ACK && dup_ack->seq == 0,
          "duplicate DATA should re-ACK last accepted sequence");

    auto frame2 = v2::DataFrame::makeData("TX1", "RX1", 2, Bytes{0xB2});
    rx.onFrameReceived(frame2.serialize());
    CHECK(delivered.size() == 1, "out-of-order DATA should not be delivered");
    CHECK(ack_queue.size() == 1, "out-of-order DATA should generate NACK");
    auto nack = v2::ControlFrame::deserialize(ack_queue.pop());
    CHECK(nack && nack->type == v2::FrameType::NACK && nack->seq == 1,
          "out-of-order DATA should NACK the missing expected sequence");

    auto stats = rx.getStats();
    CHECK(stats.frames_received == 1, "only the in-order DATA should count as received");
    CHECK(stats.acks_sent == 2, "ACK count should include original and duplicate ACK");
}

void test_timeout_retries_then_failure() {
    ARQConfig config;
    config.ack_timeout_ms = 100;
    config.max_retries = 3;

    StopAndWaitARQ tx(config);
    tx.setCallsigns("TX1", "RX1");

    ByteQueue tx_queue;
    int completions = 0;
    bool last_success = true;
    tx.setTransmitCallback([&](const Bytes& data) { tx_queue.send(data); });
    tx.setSendCompleteCallback([&](bool success) {
        ++completions;
        last_success = success;
    });

    CHECK(tx.sendData(Bytes{0x01}), "initial send should succeed");
    CHECK(tx_queue.size() == 1, "initial send should transmit once");

    tx.tick(99);
    CHECK(tx_queue.size() == 1, "timeout should not fire early");

    tx.tick(1);
    CHECK(tx_queue.size() == 2, "first timeout should retransmit");

    tx.tick(100);
    CHECK(tx_queue.size() == 3, "second timeout should retransmit");

    tx.tick(100);
    CHECK(tx_queue.size() == 3, "final timeout should fail instead of retransmitting");
    CHECK(completions == 1, "failure should invoke completion once");
    CHECK(!last_success, "failure completion should report false");
    CHECK(tx.isReadyToSend(), "failed frame should release stop-and-wait state");

    auto stats = tx.getStats();
    CHECK(stats.timeouts == 3, "all three timeout expirations should be counted");
    CHECK(stats.retransmissions == 2, "max_retries=3 allows two retransmissions after original");
    CHECK(stats.failed == 1, "failure should be counted after retry budget is exhausted");
}

void test_nack_retransmits_immediately() {
    StopAndWaitARQ tx;
    tx.setCallsigns("TX1", "RX1");

    ByteQueue tx_queue;
    tx.setTransmitCallback([&](const Bytes& data) { tx_queue.send(data); });

    CHECK(tx.sendData(Bytes{0x44}), "initial send should succeed");
    CHECK(tx_queue.size() == 1, "initial send should transmit once");

    auto nack = v2::ControlFrame::makeNack("RX1", "TX1", 0, 0);
    tx.onFrameReceived(nack.serialize());

    CHECK(tx_queue.size() == 2, "NACK should trigger immediate retransmission");
    auto stats = tx.getStats();
    CHECK(stats.retransmissions == 1, "NACK retransmission should be counted");
}

}  // namespace

int main() {
    test_factory_and_ack_completion();
    test_receive_data_duplicate_and_out_of_order();
    test_timeout_retries_then_failure();
    test_nack_retransmits_immediately();

    if (tests_failed != 0) {
        std::cout << "StopWaitARQ: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "StopWaitARQ: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
