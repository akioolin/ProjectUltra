// Unit tests for BurstStopAndWaitController — the group-level stop-and-wait
// transport for the one-way file path (design §14.15/§14.16). Pure state-machine
// tests; no protocol/modem dependency.

#include "protocol/burst_transport.hpp"
#include "ultra/types.hpp"

#include <iostream>
#include <vector>

using namespace ultra;
using namespace ultra::protocol;

namespace {

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg)                                  \
    do {                                                  \
        ++tests_run;                                      \
        if (!(cond)) {                                    \
            ++tests_failed;                               \
            std::cout << "FAIL: " << msg << "\n";         \
        }                                                 \
    } while (0)

using Group = BurstStopAndWaitController::Group;

Group makeGroup(uint8_t tag, int n = 8) {
    Group g;
    for (int i = 0; i < n; ++i) {
        g.push_back(Bytes{tag, static_cast<uint8_t>(i)});
    }
    return g;
}

void test_clean_transfer() {
    BurstStopAndWaitController c;
    std::vector<uint16_t> sent;
    bool done = false, ok = false;
    c.setTransmitGroup([&](uint16_t seq, const Group&) { sent.push_back(seq); });
    c.setTransferDone([&](bool s) { done = true; ok = s; });

    c.startTransfer({makeGroup(0), makeGroup(1), makeGroup(2)});
    CHECK(sent.size() == 1 && sent[0] == 0, "first group transmitted on start");
    c.onGroupAck(0);
    CHECK(sent.size() == 2 && sent[1] == 1, "ack(0) advances to group 1");
    c.onGroupAck(1);
    CHECK(sent.size() == 3 && sent[2] == 2, "ack(1) advances to group 2");
    c.onGroupAck(2);
    CHECK(done && ok && !c.isSending(), "ack(last) completes transfer with success");
    CHECK(c.groupsAcked() == 3, "all groups acked");
}

void test_empty_transfer() {
    BurstStopAndWaitController c;
    bool done = false, ok = false;
    int tx = 0;
    c.setTransmitGroup([&](uint16_t, const Group&) { ++tx; });
    c.setTransferDone([&](bool s) { done = true; ok = s; });
    c.startTransfer({});
    CHECK(done && ok && tx == 0, "empty transfer completes immediately, no tx");
}

void test_timeout_resend_whole_group() {
    BurstStopAndWaitController::Config cfg;
    cfg.ack_timeout_ms = 1000;
    cfg.max_retries = 3;
    BurstStopAndWaitController c(cfg);
    std::vector<uint16_t> sent;
    c.setTransmitGroup([&](uint16_t seq, const Group&) { sent.push_back(seq); });

    c.startTransfer({makeGroup(0), makeGroup(1)});
    CHECK(sent.size() == 1, "one tx at start");
    c.tick(500);
    CHECK(sent.size() == 1, "no resend before timeout");
    c.tick(600);  // 1100 ms total > timeout
    CHECK(sent.size() == 2 && sent[1] == 0 && c.retriesForCurrentGroup() == 1,
          "whole group 0 resent on ACK timeout");
}

void test_link_dead_after_max_retries() {
    BurstStopAndWaitController::Config cfg;
    cfg.ack_timeout_ms = 100;
    cfg.max_retries = 2;
    BurstStopAndWaitController c(cfg);
    int tx = 0;
    bool done = false, ok = true;
    c.setTransmitGroup([&](uint16_t, const Group&) { ++tx; });
    c.setTransferDone([&](bool s) { done = true; ok = s; });

    c.startTransfer({makeGroup(0)});  // tx=1
    c.tick(150);                      // retry 1 -> tx=2
    c.tick(150);                      // retry 2 -> tx=3
    c.tick(150);                      // retries == max -> link dead
    CHECK(tx == 3, "two retries then stop transmitting");
    CHECK(done && !ok && !c.isSending(), "link declared dead -> done(false)");
}

void test_rx_inorder_delivery_and_duplicate_reack() {
    BurstStopAndWaitController c;
    std::vector<uint16_t> acks;
    std::vector<uint16_t> delivered;
    c.setSendGroupAck([&](uint16_t seq) { acks.push_back(seq); });
    c.setGroupDelivered([&](uint16_t seq, const Group&) { delivered.push_back(seq); });

    c.onGroupReceived(0, makeGroup(0));
    CHECK(delivered.size() == 1 && delivered[0] == 0 && acks.size() == 1 && acks[0] == 0,
          "group 0 delivered + acked");
    c.onGroupReceived(0, makeGroup(0));  // sender resent because its ACK was lost
    CHECK(delivered.size() == 1 && acks.size() == 2 && acks[1] == 0,
          "duplicate group re-acked but NOT re-delivered");
    c.onGroupReceived(1, makeGroup(1));
    CHECK(delivered.size() == 2 && delivered[1] == 1 && acks.size() == 3 && acks[2] == 1,
          "group 1 delivered + acked");
}

void test_stale_ack_ignored() {
    BurstStopAndWaitController c;
    std::vector<uint16_t> sent;
    c.setTransmitGroup([&](uint16_t seq, const Group&) { sent.push_back(seq); });
    c.startTransfer({makeGroup(0), makeGroup(1)});
    c.onGroupAck(5);  // stale/out-of-range ACK
    CHECK(sent.size() == 1 && c.isSending() && c.currentGroupSeq() == 0,
          "stale ACK ignored, still in flight on group 0");
    c.onGroupAck(0);
    CHECK(sent.size() == 2 && sent[1] == 1, "correct ACK advances");
}

}  // namespace

int main() {
    test_clean_transfer();
    test_empty_transfer();
    test_timeout_resend_whole_group();
    test_link_dead_after_max_retries();
    test_rx_inorder_delivery_and_duplicate_reack();
    test_stale_ack_ignored();

    if (tests_failed != 0) {
        std::cout << "BurstTransport: " << (tests_run - tests_failed) << "/" << tests_run
                  << " passed\n";
        return 1;
    }
    std::cout << "BurstTransport: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
