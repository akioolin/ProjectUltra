#include "protocol/selective_repeat_arq_policy.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>

using namespace ultra::protocol::selective_repeat_arq_policy;

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

void test_window_and_batch_clamps() {
    CHECK(clampWindowSize(0, 16) == 1, "window should clamp to minimum 1");
    CHECK(clampWindowSize(4, 16) == 4, "window should preserve valid value");
    CHECK(clampWindowSize(99, 16) == 16, "window should clamp to maximum");

    CHECK(clampAckBatchSize(0, 4, 16) == 0, "ack batch sentinel should be preserved");
    CHECK(clampAckBatchSize(2, 4, 16) == 2, "valid ack batch should be preserved");
    CHECK(clampAckBatchSize(8, 4, 16) == 4, "ack batch should clamp to window");
    CHECK(clampAckBatchSize(99, 32, 16) == 16, "ack batch should clamp to max window");
    CHECK(effectiveAckBatchThreshold(0, 4) == 4, "sentinel batch should track window");
    CHECK(effectiveAckBatchThreshold(2, 4) == 2, "explicit batch should be used");
}

void test_sack_bitmap_decode() {
    uint8_t payload[6] = {};
    ultra::protocol::v2::NackPayload np;
    np.frame_seq = 123;
    np.cw_bitmap = 0x00000400u;
    np.encode(payload);
    CHECK(decodeSackBitmap(payload) == 0x00000400u,
          "wide SACK bitmap should preserve bits beyond legacy byte");

    uint8_t legacy_payload[6] = {};
    legacy_payload[2] = 0x05;
    legacy_payload[5] = 0x05;
    CHECK(decodeSackBitmap(legacy_payload) == 0x05u,
          "legacy repeated 8-bit SACK shape should decode to legacy byte");

    CHECK(decodeSackBitmap(nullptr) == 0u, "null SACK payload should be safe");
}

void test_sequence_window_policy() {
    CHECK(seqInWindow(10, 10, 4), "base sequence should be in window");
    CHECK(seqInWindow(13, 10, 4), "last sequence in window");
    CHECK(!seqInWindow(14, 10, 4), "first sequence outside window");
    CHECK(seqInWindow(0, 65534, 4), "sequence window should wrap across 65535");
    CHECK(!seqInWindow(2, 65534, 4), "wrapped sequence outside window");
}

void test_ack_freshness() {
    CHECK(classifyAckFreshness(9, 10, 4) == AckFreshness::Accept,
          "base-1 ACK should be accepted");
    CHECK(classifyAckFreshness(8, 10, 4) == AckFreshness::Stale,
          "ACK older than base-1 should be stale");
    CHECK(classifyAckFreshness(14, 10, 4) == AckFreshness::Accept,
          "ACK within window+1 should be accepted");
    CHECK(classifyAckFreshness(15, 10, 4) == AckFreshness::Future,
          "ACK too far ahead should be future");
    CHECK(classifyAckFreshness(65535, 0, 4) == AckFreshness::Accept,
          "wrapped base-1 ACK should be accepted");
    CHECK(classifyAckFreshness(65534, 0, 4) == AckFreshness::Stale,
          "wrapped stale ACK should be rejected");
}

void test_ack_dedup_policy() {
    CHECK(!shouldSuppressDuplicateAck(false, 100, 1, 0x10, 1, 0x10),
          "missing prior signature should not suppress");
    CHECK(!shouldSuppressDuplicateAck(true, 0, 1, 0x10, 1, 0x10),
          "expired dedup timer should not suppress");
    CHECK(shouldSuppressDuplicateAck(true, 100, 1, 0x10, 1, 0x10),
          "matching ACK signature within timer should suppress");
    CHECK(!shouldSuppressDuplicateAck(true, 100, 1, 0x10, 2, 0x10),
          "different ACK seq should not suppress");
    CHECK(!shouldSuppressDuplicateAck(true, 100, 1, 0x10, 1, 0x20),
          "different ACK bitmap should not suppress");

    CHECK(ackDedupWindowMs(1) == 80, "dedup window should clamp low");
    CHECK(ackDedupWindowMs(100) == 140, "dedup window should add guard time");
    CHECK(ackDedupWindowMs(1000) == 500, "dedup window should clamp high");
}

void test_sack_timer_policy() {
    CHECK(sackTimerForFrame(0, 500, 0, false) == 500,
          "zero short sentinel should use normal delay");
    CHECK(sackTimerForFrame(0, 500, 50, true) == 500,
          "MORE_FRAG should use long delay");
    CHECK(sackTimerForFrame(0, 500, 50, false) == 50,
          "tail frame should use short delay");
    CHECK(sackTimerForFrame(440, 500, 50, false) == 50,
          "tail frame should collapse existing long timer");
    CHECK(sackTimerForFrame(50, 500, 0, true) == 50,
          "new in-burst frame should not extend existing timer");
}

void test_hole_and_repeat_policy() {
    CHECK(isAlignedBaseHoleAck(9, 10, 0x02),
          "aligned SACK with higher bit and missing bit0 should indicate base hole");
    CHECK(!isAlignedBaseHoleAck(10, 10, 0x02),
          "unaligned SACK should not indicate base hole");
    CHECK(!isAlignedBaseHoleAck(9, 10, 0x01),
          "bit0 present should not indicate base hole");
    CHECK(!isAlignedBaseHoleAck(9, 10, 0x00),
          "empty bitmap should not indicate base hole");

    CHECK(holeProbeInitialTimerMs(1000) == 1200, "hole probe initial timer low clamp");
    CHECK(holeProbeInitialTimerMs(4000) == 2000, "hole probe initial timer midpoint");
    CHECK(holeProbeInitialTimerMs(8000) == 2500, "hole probe initial timer high clamp");
    CHECK(holeProbeNextTimerMs(2000) == 1800, "hole probe next timer low clamp");
    CHECK(holeProbeNextTimerMs(3900) == 2600, "hole probe next timer midpoint");
    CHECK(holeProbeNextTimerMs(6000) == 3000, "hole probe next timer high clamp");
    CHECK(fastRetransmitCooldownMs(1000) == 300, "fast retx cooldown low clamp");
    CHECK(fastRetransmitCooldownMs(3600) == 600, "fast retx cooldown midpoint");
    CHECK(fastRetransmitCooldownMs(9000) == 1200, "fast retx cooldown high clamp");

    CHECK(!shouldFastRetransmitHole(1, 0, 0),
          "single hole confirmation should not fast retransmit");
    CHECK(shouldFastRetransmitHole(2, 0, 0),
          "two hole confirmations should fast retransmit");
    CHECK(!shouldFastRetransmitHole(2, 1, 0),
          "fast retransmit count should cap repeats");
    CHECK(!shouldFastRetransmitHole(2, 0, 1),
          "cooldown should block fast retransmit");
}

void test_ack_repeat_policy() {
    CHECK(ackRepeatDelayForCopy(100, 2) == 100,
          "first delayed ACK repeat should use configured delay");
    CHECK(ackRepeatDelayForCopy(100, 3) == 300,
          "later ACK repeat should use wider spacing");

    const int jitter = ackRepeatJitterMs(7, 0x1234, 2);
    CHECK(jitter >= -30 && jitter <= 30, "ACK repeat jitter should stay bounded");
    CHECK(ackRepeatJitterMs(7, 0x1234, 2) == jitter,
          "ACK repeat jitter should be deterministic");
}

}  // namespace

int main() {
    test_window_and_batch_clamps();
    test_sack_bitmap_decode();
    test_sequence_window_policy();
    test_ack_freshness();
    test_ack_dedup_policy();
    test_sack_timer_policy();
    test_hole_and_repeat_policy();
    test_ack_repeat_policy();

    if (tests_failed != 0) {
        std::cout << "SelectiveRepeatPolicy: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "SelectiveRepeatPolicy: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
