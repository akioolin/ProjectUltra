#pragma once

#include "ultra/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace ultra {
namespace protocol {

// Group-level stop-and-wait transport for the one-way file path
// (design PHY_ADAPTATION_DESIGN_2026_05_26.md §14.15/§14.16).
//
// The ARQ unit is one interleaved BURST = a group of N frames (~6 s at QPSK
// R3/4, group size kBurstInterleaveGroupFrames). The sender transmits one group,
// waits for a single group-ACK, and resends the WHOLE group on timeout. There is
// no per-frame selective retransmit: the receiver needs every frame of the group
// to deinterleave the cross-frame block, so a partial group is undecodable and
// whole-burst resend is mandatory (not merely simpler than SACK).
//
// This is deliberately modem-agnostic and callback-driven so it unit-tests
// without the protocol/modem/waveform. The protocol layer wires:
//   TX:  setTransmitGroup -> ModemEngine::transmitBurst(group)
//        onGroupAck       <- decoded group-ACK control frame
//        tick             <- session clock
//   RX:  onGroupReceived  <- a fully-decoded burst (deinterleaved group)
//        setSendGroupAck  -> emit a group-ACK control frame
//        setGroupDelivered-> reassemble file bytes
class BurstStopAndWaitController {
public:
    using Frame = Bytes;
    using Group = std::vector<Frame>;

    using TransmitGroupFn = std::function<void(uint16_t group_seq, const Group& frames)>;
    using TransferDoneFn = std::function<void(bool success)>;  // false => HF link dead
    using SendGroupAckFn = std::function<void(uint16_t group_seq)>;
    using GroupDeliveredFn = std::function<void(uint16_t group_seq, const Group& frames)>;

    // §14.36 chunk-at-rate: form (or re-form) the next group at the connection's
    // CURRENT data rate and transmit it. The transport doesn't store frames; it
    // just runs the stop-and-wait state machine and calls back when it needs a
    // (re)send. Returns false when there is no more payload to send (transfer
    // complete). is_resend=true when this is a retry of the in-flight group
    // (NACK or timeout) — the connection re-forms from the SAME file offset
    // (the rate may have changed since the last send, so the byte count may
    // differ). The transport tracks "in-flight / retries / done"; the connection
    // owns the file cursor and advances it only on ACK.
    using FormAndSendGroupFn = std::function<bool(uint16_t group_seq, bool is_resend)>;

    struct Config {
        // One burst is ~6 s; allow the burst airtime + T/R turnaround + the ACK
        // burst before declaring the group lost. Tuned per channel later.
        uint32_t ack_timeout_ms = 14000;
        // Resend attempts for one group before declaring the link dead. The
        // session then returns to idle (no hung station).
        uint32_t max_retries = 10;
    };

    BurstStopAndWaitController() = default;
    explicit BurstStopAndWaitController(Config cfg) : cfg_(cfg) {}

    // Set the group-ACK timeout. One group is a full interleaved burst (~11 s at
    // QPSK R3/4), so this must cover burst airtime + T/R turnaround + the GROUP_ACK
    // airtime + decode margin. The protocol layer feeds the same burst-aware value
    // the SR-ARQ window=8 path computes; too small and the sender resends before
    // the ACK can land (its listen window collapses) — wasting whole-burst airtime.
    void setAckTimeoutMs(uint32_t timeout_ms) {
        if (timeout_ms > 0) cfg_.ack_timeout_ms = timeout_ms;
    }
    uint32_t ackTimeoutMs() const { return cfg_.ack_timeout_ms; }

    // ----------------------------------------------------------------- sender
    void setTransmitGroup(TransmitGroupFn fn) { tx_group_ = std::move(fn); }
    void setFormAndSendGroup(FormAndSendGroupFn fn) { form_send_ = std::move(fn); }
    void setTransferDone(TransferDoneFn fn) { tx_done_ = std::move(fn); }

    // Begin a transfer: the file pre-chunked into groups (each group = the frame
    // payloads of one interleaved burst). Empty transfer completes immediately.
    void startTransfer(std::vector<Group> groups) {
        groups_ = std::move(groups);
        chunk_at_rate_ = false;
        next_group_ = 0;
        retries_ = 0;
        elapsed_since_tx_ms_ = 0;
        sending_ = !groups_.empty();
        if (sending_) {
            transmitCurrent();
        } else if (tx_done_) {
            tx_done_(true);
        }
    }

    // §14.36 chunk-at-rate transfer: the connection forms each group at the current
    // rate via setFormAndSendGroup; the transport just tracks in-flight/retries/done.
    // The form fn returns false when the file payload is fully drained -> transfer
    // completes successfully. Used by the adaptive-rate path (default OFF).
    void startTransfer() {
        groups_.clear();
        chunk_at_rate_ = true;
        next_group_ = 0;
        retries_ = 0;
        elapsed_since_tx_ms_ = 0;
        sending_ = true;
        if (!transmitCurrent()) {
            // Form fn returned false immediately -> empty payload -> done.
            sending_ = false;
            if (tx_done_) tx_done_(true);
        }
    }

    // Sender received a group-ACK. Advances only on the in-flight group; stale or
    // duplicate ACKs are ignored (stop-and-wait has exactly one group in flight).
    void onGroupAck(uint16_t group_seq) {
        if (!sending_) return;
        if (group_seq != static_cast<uint16_t>(next_group_)) return;
        ++next_group_;
        retries_ = 0;
        elapsed_since_tx_ms_ = 0;
        // Done condition differs by mode: pre-chunked = index past end; chunk-at-rate
        // = form fn returns false (file payload drained).
        if (!chunk_at_rate_ && next_group_ >= groups_.size()) {
            sending_ = false;
            if (tx_done_) tx_done_(true);
            return;
        }
        if (!transmitCurrent()) {
            sending_ = false;
            if (tx_done_) tx_done_(true);
        }
    }

    // Sender received a group-NACK: the receiver decoded the descriptor but the
    // interleaved group failed (0/8). Resend the in-flight group NOW instead of
    // waiting out the full ack_timeout — reclaims the dead silence between the
    // failed burst and the timeout-driven resend (fast fade recovery, §14.30).
    // Counts as a retry so a persistent deep fade still hits max_retries and the
    // link is declared dead rather than resending forever. The ~burst-duration
    // airtime of each resend naturally paces attempts to the fade timescale.
    void onGroupNack(uint16_t group_seq) {
        if (!sending_) return;
        if (group_seq != static_cast<uint16_t>(next_group_)) return;  // stale/dup
        if (retries_ >= cfg_.max_retries) {
            sending_ = false;
            if (tx_done_) tx_done_(false);
            return;
        }
        ++retries_;
        transmitCurrent();  // resets elapsed_since_tx_ms_
    }

    // Advance the session clock. On ACK timeout, resend the whole current group;
    // after max_retries the link is declared dead and the transfer fails.
    void tick(uint32_t elapsed_ms) {
        if (!sending_) return;
        elapsed_since_tx_ms_ += elapsed_ms;
        if (elapsed_since_tx_ms_ < cfg_.ack_timeout_ms) return;
        if (retries_ >= cfg_.max_retries) {
            sending_ = false;
            if (tx_done_) tx_done_(false);
            return;
        }
        ++retries_;
        transmitCurrent();
    }

    bool isSending() const { return sending_; }
    uint16_t currentGroupSeq() const { return static_cast<uint16_t>(next_group_); }
    uint32_t retriesForCurrentGroup() const { return retries_; }
    size_t groupsTotal() const { return groups_.size(); }
    size_t groupsAcked() const { return next_group_; }

    // --------------------------------------------------------------- receiver
    void setSendGroupAck(SendGroupAckFn fn) { ack_fn_ = std::move(fn); }
    void setGroupDelivered(GroupDeliveredFn fn) { delivered_fn_ = std::move(fn); }

    // Feed a fully-decoded burst (the deinterleaved group's frames). The waveform
    // decodes the interleaved burst as a unit, so a group either arrives complete
    // or not at all; we only see (and ACK) complete groups. A duplicate group
    // (the sender resent because its ACK was lost) is re-ACKed but delivered once.
    void onGroupReceived(uint16_t group_seq, const Group& frames) {
        if (group_seq == static_cast<uint16_t>(rx_next_group_)) {
            if (delivered_fn_) delivered_fn_(group_seq, frames);
            ++rx_next_group_;
            if (ack_fn_) ack_fn_(group_seq);
        } else if (seqLess(group_seq, static_cast<uint16_t>(rx_next_group_))) {
            // Already delivered — our prior ACK was lost; re-ACK, do not re-deliver.
            if (ack_fn_) ack_fn_(group_seq);
        }
        // group_seq ahead of expected: impossible under stop-and-wait; ignore so
        // the sender keeps resending the group we actually need.
    }

    uint16_t rxExpectedGroupSeq() const { return static_cast<uint16_t>(rx_next_group_); }

private:
    bool transmitCurrent() {
        const bool is_resend = (retries_ > 0);
        return doTransmit(is_resend);
    }

    bool doTransmit(bool is_resend) {
        elapsed_since_tx_ms_ = 0;
        if (chunk_at_rate_) {
            if (!form_send_) return false;
            return form_send_(static_cast<uint16_t>(next_group_), is_resend);
        }
        if (tx_group_ && next_group_ < groups_.size()) {
            tx_group_(static_cast<uint16_t>(next_group_), groups_[next_group_]);
            return true;
        }
        return false;
    }

    // True if a < b in 16-bit sequence space (no wrap within a single transfer,
    // but kept tolerant for long transfers).
    static bool seqLess(uint16_t a, uint16_t b) {
        return static_cast<uint16_t>(a - b) > 0x8000u;
    }

    Config cfg_;

    // sender state
    std::vector<Group> groups_;          // legacy pre-chunked path (back-compat, tests)
    bool chunk_at_rate_ = false;          // §14.36: form-at-send via form_send_
    size_t next_group_ = 0;
    uint32_t retries_ = 0;
    uint32_t elapsed_since_tx_ms_ = 0;
    bool sending_ = false;
    TransmitGroupFn tx_group_;
    FormAndSendGroupFn form_send_;
    TransferDoneFn tx_done_;

    // receiver state
    size_t rx_next_group_ = 0;
    SendGroupAckFn ack_fn_;
    GroupDeliveredFn delivered_fn_;
};

}  // namespace protocol
}  // namespace ultra
