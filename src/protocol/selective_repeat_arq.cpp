#include "selective_repeat_arq.hpp"
#include "ultra/logging.hpp"
#include <cmath>

namespace ultra {
namespace protocol {

namespace {

uint32_t decodeSackBitmap(const uint8_t* payload) {
    const uint32_t bitmap = v2::NackPayload::decode(payload).cw_bitmap;

    // Compatibility with the legacy 8-bit SACK encoding used before the
    // wider-window path: makeNack() placed the bitmap in payload[5], then the
    // ARQ code also copied it into payload[2] and read only payload[2].
    const uint8_t legacy = payload[2];
    const uint32_t legacy_shape = (static_cast<uint32_t>(legacy) << 24) | legacy;
    if (legacy != 0 && bitmap == legacy_shape) {
        return legacy;
    }

    return bitmap;
}

} // namespace

SelectiveRepeatARQ::SelectiveRepeatARQ(const ARQConfig& config)
    : config_(config)
{
    if (config_.window_size > MAX_WINDOW) {
        config_.window_size = MAX_WINDOW;
    }
    if (config_.window_size < 1) {
        config_.window_size = 1;
    }
    if (config_.ack_batch_size > MAX_WINDOW) {
        config_.ack_batch_size = MAX_WINDOW;
    }
    if (config_.ack_batch_size > 0 &&
        config_.ack_batch_size > static_cast<uint32_t>(config_.window_size)) {
        config_.ack_batch_size = static_cast<uint32_t>(config_.window_size);
    }
    adaptive_ack_timeout_ms_ = config_.ack_timeout_ms;
}

void SelectiveRepeatARQ::setCallsigns(const std::string& local, const std::string& remote) {
    local_call_ = sanitizeCallsign(local);
    remote_call_ = sanitizeCallsign(remote);
}

bool SelectiveRepeatARQ::sendData(const Bytes& data) {
    return sendDataWithFlags(data, v2::Flags::NONE);
}

bool SelectiveRepeatARQ::sendData(const std::string& text) {
    Bytes data(text.begin(), text.end());
    return sendData(data);
}

bool SelectiveRepeatARQ::sendDataWithFlags(const Bytes& data, uint8_t flags) {
    if (!isReadyToSend()) {
        LOG_MODEM(WARN, "SR-ARQ: Window full, cannot send");
        return false;
    }

    if (local_call_.empty() || remote_call_.empty()) {
        LOG_MODEM(ERROR, "SR-ARQ: Callsigns not set");
        return false;
    }

    const uint16_t seq = tx_next_seq_;
    size_t slot = seqToSlot(seq);

    // Create and serialize v2 frame
    auto frame = v2::DataFrame::makeData(local_call_, remote_call_, seq, data);
    frame.flags = flags;

    tx_window_[slot].active = true;
    tx_window_[slot].frame_data = frame.serialize();
    tx_window_[slot].seq = seq;
    tx_window_[slot].timeout_ms = currentAckTimeoutMs();
    tx_window_[slot].first_tx_ms = arq_time_ms_;
    tx_window_[slot].rtt_sample_eligible = true;
    tx_window_[slot].retry_count = 0;
    tx_window_[slot].acked = false;
    tx_window_[slot].hole_ack_count = 0;
    tx_window_[slot].fast_retx_count = 0;
    tx_window_[slot].fast_retx_cooldown_ms = 0;
    tx_window_[slot].hole_probe_armed = false;
    tx_window_[slot].hole_probe_timer_ms = 0;
    tx_window_[slot].hole_probe_count = 0;

    // Publish TX state before invoking the callback. Unit tests and future
    // low-latency transports may synchronously deliver the DATA and its ACK
    // before transmitData() returns.
    stats_.frames_sent++;
    tx_next_seq_ = (tx_next_seq_ + 1) & 0xFFFF;
    tx_in_flight_++;

    LOG_MODEM(DEBUG, "SR-ARQ: Sent DATA seq=%d slot=%zu, window=[%d,%d)",
              seq, slot, tx_base_seq_, tx_next_seq_);

    transmitData(tx_window_[slot].frame_data);

    return true;
}

bool SelectiveRepeatARQ::isReadyToSend() const {
    return getAvailableSlots() > 0;
}

size_t SelectiveRepeatARQ::getAvailableSlots() const {
    size_t window = config_.window_size;
    return (tx_in_flight_ < window) ? (window - tx_in_flight_) : 0;
}

void SelectiveRepeatARQ::onFrameReceived(const Bytes& frame_data) {
    if (frame_data.size() < 2) {
        return;
    }

    uint16_t magic = (static_cast<uint16_t>(frame_data[0]) << 8) | frame_data[1];
    if (magic != v2::MAGIC_V2) {
        LOG_MODEM(TRACE, "SR-ARQ: Ignoring frame with wrong magic");
        return;
    }

    auto header = v2::parseHeader(frame_data);
    if (!header.valid) {
        LOG_MODEM(TRACE, "SR-ARQ: Ignoring frame with invalid header");
        return;
    }

    uint32_t our_hash = v2::hashCallsign(local_call_);
    if (header.dst_hash != our_hash && header.dst_hash != 0xFFFFFF) {
        LOG_MODEM(TRACE, "SR-ARQ: Ignoring frame for different station");
        return;
    }

    LOG_MODEM(DEBUG, "SR-ARQ: Received %s seq=%d",
              v2::frameTypeToString(header.type), header.seq);

    if (header.is_control) {
        auto ctrl = v2::ControlFrame::deserialize(frame_data);
        if (ctrl) {
            switch (ctrl->type) {
                case v2::FrameType::ACK:
                    handleAckFrame(*ctrl);
                    break;
                case v2::FrameType::NACK:
                    handleNackFrame(*ctrl);
                    break;
                default:
                    break;
            }
        }
    } else {
        auto data_frame = v2::DataFrame::deserialize(frame_data);
        if (data_frame) {
            handleDataFrame(*data_frame);
        }
    }
}

void SelectiveRepeatARQ::handleDataFrame(const v2::DataFrame& frame) {
    last_rx_flags_ = frame.flags;
    last_rx_more_data_ = (frame.flags & v2::Flags::MORE_FRAG) != 0;

    // Capture the just-arrived frame's MORE_FRAG locally — the member
    // last_rx_more_data_ above will be overwritten by advanceRXWindow()
    // when it delivers buffered frames (line 552 area). The timer-arm
    // logic below needs the value of THIS frame, not whichever frame
    // advanceRXWindow happened to deliver last.
    const bool frame_more_frag = (frame.flags & v2::Flags::MORE_FRAG) != 0;

    uint16_t seq = frame.seq;

    if (isInRXWindow(seq)) {
        uint16_t expected_seq = rx_base_seq_;
        size_t slot = seqToSlot(seq);
        bool new_frame = false;
        bool out_of_order = false;

        if (!rx_window_[slot].received) {
            rx_window_[slot].received = true;
            rx_window_[slot].seq = seq;
            rx_window_[slot].payload = frame.payload;
            rx_window_[slot].flags = frame.flags;
            stats_.frames_received++;
            new_frame = true;

            LOG_MODEM(DEBUG, "SR-ARQ: DATA seq=%d stored in slot %zu", seq, slot);

            if (seq == expected_seq) {
                advanceRXWindow();
            } else {
                out_of_order = true;
                stats_.out_of_order++;
                LOG_MODEM(DEBUG, "SR-ARQ: Out-of-order seq=%d (expected %d)",
                          seq, expected_seq);
            }
        } else {
            LOG_MODEM(DEBUG, "SR-ARQ: Duplicate DATA seq=%d", seq);
        }

        // ACK strategy for OFDM burst traffic:
        // - Immediate ACK on hole detection (out-of-order) — safety valve, MUST
        //   stay first in the condition.
        // - Otherwise threshold-driven coalescing using ack_batch_size; falls
        //   back to window_size when ack_batch_size = 0 (default), preserving
        //   prior behavior bit for bit.
        // - Short delayed timer (sack_delay_ms) covers tail of stream.
        if (new_frame) {
            frames_since_ack_++;
        }

        const uint32_t batch_threshold = config_.ack_batch_size > 0
                                       ? config_.ack_batch_size
                                       : static_cast<uint32_t>(config_.window_size);
        if (out_of_order || frames_since_ack_ >= batch_threshold) {
            // Bump the trigger-reason counter BEFORE sendSack — out_of_order
            // takes priority because it's the immediate safety valve. Each
            // SACK send increments exactly one trigger counter.
            if (out_of_order) {
                stats_.sack_trigger_out_of_order++;
            } else {
                stats_.sack_trigger_threshold++;
            }
            sendSack();
            sack_pending_ = false;
            sack_timer_ms_ = 0;
            frames_since_ack_ = 0;
        } else if (new_frame) {
            sack_pending_ = true;
            // Stream-aware timer: in-burst frames (MORE_FRAG=1) get the
            // long delay (sack_delay_ms); end-of-burst (MORE_FRAG=0) gets
            // the short delay so the sender's window advances promptly.
            // Sentinel sack_delay_short_ms_ = 0 → use sack_delay_ms for
            // both legs (legacy behavior bit-for-bit).
            uint32_t pick_ms;
            if (sack_delay_short_ms_ == 0) {
                pick_ms = config_.sack_delay_ms;
            } else if (frame_more_frag) {
                pick_ms = config_.sack_delay_ms;
            } else {
                pick_ms = sack_delay_short_ms_;
            }

            if (sack_timer_ms_ == 0) {
                sack_timer_ms_ = pick_ms;
            } else {
                sack_timer_ms_ = std::min(sack_timer_ms_, pick_ms);
            }
        }

    } else {
        LOG_MODEM(WARN, "SR-ARQ: DATA seq=%d outside window [%d, %d)",
                  seq, rx_base_seq_, (rx_base_seq_ + config_.window_size) & 0xFFFF);
        // Out-of-window: send SACK immediately to help sender recover
        stats_.sack_trigger_out_of_window++;
        sendSack();
        sack_pending_ = false;
        sack_timer_ms_ = 0;
        frames_since_ack_ = 0;
    }
}

void SelectiveRepeatARQ::handleAckFrame(const v2::ControlFrame& frame) {
    uint16_t seq = frame.seq;
    uint32_t bitmap = decodeSackBitmap(frame.payload);

    LOG_MODEM(INFO, "SR-ARQ: ACK seq=%d bitmap=0x%08X (base=%d, in_flight=%zu)",
              seq, bitmap, tx_base_seq_, tx_in_flight_);
    stats_.sacks_received++;

    // Stale-ACK guard: reject ACKs strictly older than (tx_base_seq_ - 1).
    // seq == (tx_base_seq_ - 1) is valid — it's the common "no new cumulative progress"
    // ACK that still carries a fresh SACK bitmap for the current window.
    uint16_t ack_base = (tx_base_seq_ - 1) & 0xFFFF;
    uint16_t back = (ack_base - seq) & 0xFFFF;
    if (back > 0 && back < 0x8000) {
        stats_.stale_acks_ignored++;
        LOG_MODEM(INFO, "SR-ARQ: Stale ACK seq=%d < base-1=%d, ignoring", seq, ack_base);
        return;
    }

    // Far-future guard: reject ACKs implausibly ahead (> window_size + 1 past base)
    uint16_t forward = (seq - ack_base) & 0xFFFF;
    if (forward > config_.window_size + 1 && forward < 0x8000) {
        stats_.future_acks_ignored++;
        LOG_MODEM(INFO, "SR-ARQ: Future ACK seq=%d too far ahead of base=%d, ignoring", seq, tx_base_seq_);
        return;
    }

    // ACK-repeat dedup guard: suppress clustered duplicate ACKs carrying
    // identical cumulative+bitmap information.
    if (last_ack_signature_valid_ &&
        ack_dedup_timer_ms_ > 0 &&
        last_ack_seq_ == seq &&
        last_ack_bitmap_ == bitmap) {
        stats_.duplicate_acks_ignored++;
        LOG_MODEM(INFO, "SR-ARQ: Duplicate ACK seq=%d bitmap=0x%08X suppressed", seq, bitmap);
        return;
    }
    last_ack_signature_valid_ = true;
    last_ack_seq_ = seq;
    last_ack_bitmap_ = bitmap;
    ack_dedup_timer_ms_ = std::clamp(ack_repeat_delay_ms_ + 40u, 80u, 500u);

    // --- Cumulative ACK: advance base past all frames up to seq ---
    uint16_t base_before_ack = tx_base_seq_;
    while (tx_in_flight_ > 0 && tx_base_seq_ != ((seq + 1) & 0xFFFF)) {
        size_t slot = seqToSlot(tx_base_seq_);
        if (tx_window_[slot].active) {
            maybeSampleRTT(tx_window_[slot]);
            tx_window_[slot].active = false;
            tx_window_[slot].acked = true;
            tx_window_[slot].hole_ack_count = 0;
            tx_window_[slot].fast_retx_count = 0;
            tx_window_[slot].fast_retx_cooldown_ms = 0;
            tx_window_[slot].hole_probe_armed = false;
            tx_window_[slot].hole_probe_timer_ms = 0;
            tx_window_[slot].hole_probe_count = 0;
            tx_in_flight_--;
            stats_.acks_received++;

            if (on_send_complete_) {
                on_send_complete_(true);
            }
        }
        tx_base_seq_ = (tx_base_seq_ + 1) & 0xFFFF;
    }

    // --- Positive-only SACK bitmap: mark frames the receiver confirms it HAS ---
    if (bitmap != 0) {
        bool any_sacked = false;
        for (int i = 0; i < 32 && i < static_cast<int>(config_.window_size); i++) {
            if (!(bitmap & (1u << i))) continue;

            uint16_t sack_seq = (tx_base_seq_ + i) & 0xFFFF;
            size_t slot = seqToSlot(sack_seq);

            if (tx_window_[slot].active && !tx_window_[slot].acked && tx_window_[slot].seq == sack_seq) {
                tx_window_[slot].acked = true;
                any_sacked = true;
                LOG_MODEM(INFO, "SR-ARQ: SACK seq=%d confirmed received (bitmap=0x%08X)", sack_seq, bitmap);
            }
        }

        if (any_sacked) {
            advanceTXWindow();
        }
    }

    // --- Hole-based fast retransmit for base gap frame ---
    // Trigger: ACK aligned to base (seq == tx_base-1), bit0=0, any higher bit set.
    // This means the receiver is missing the base frame but has later frames.
    bool is_aligned = (seq == ((tx_base_seq_ - 1) & 0xFFFF));
    bool has_hole = (bitmap & 0x01u) == 0 && (bitmap & ~0x01u) != 0;

    if (is_aligned && has_hole) {
        size_t base_slot = seqToSlot(tx_base_seq_);
        TXSlot& s = tx_window_[base_slot];

        if (s.active && !s.acked && s.seq == tx_base_seq_) {
            s.hole_ack_count++;
            stats_.hole_events++;
            LOG_MODEM(INFO, "SR-ARQ: Hole detected for base seq=%d (hole_count=%d, bitmap=0x%08X)",
                      tx_base_seq_, s.hole_ack_count, bitmap);

            if (!s.hole_probe_armed) {
                s.hole_probe_armed = true;
                s.hole_probe_count = 0;
                s.hole_probe_timer_ms = std::clamp(currentAckTimeoutMs() / 2, 1200u, 2500u);
                LOG_MODEM(INFO, "SR-ARQ: Armed hole-probe timer for seq=%d (%ums)",
                          s.seq, s.hole_probe_timer_ms);
            }

            // Send one fast repair for a base hole. Additional SACK bitmap updates
            // often arrive before the repair ACK catches up, so repeated fast
            // retransmits mostly create stale out-of-window duplicates.
            //
            // Require TWO hole-confirmation SACKs before retx — protects against
            // false-loss inferences when the original ACK is just delayed in the
            // audio buffer chain. Real-hardware tests showed fast_hole firing
            // before the original ACK had time to traverse the 340ms-each-way
            // soundcard buffers, creating ~25% wasted duplicate retx. With
            // window=4 and ACKs arriving every ~700ms, requiring 2 SACKs adds
            // ~700ms of latency to genuine-loss recovery — acceptable cost for
            // eliminating the spurious retx storm.
            constexpr int MAX_FAST_RETX_PER_HOLE = 1;
            constexpr int MIN_HOLE_CONFIRMATIONS = 2;
            uint32_t fast_retx_cooldown_ms = std::clamp(config_.ack_timeout_ms / 6, 300u, 1200u);
            if (s.hole_ack_count >= MIN_HOLE_CONFIRMATIONS &&
                s.fast_retx_count < MAX_FAST_RETX_PER_HOLE &&
                s.fast_retx_cooldown_ms == 0) {
                s.fast_retx_count++;
                s.fast_retx_cooldown_ms = fast_retx_cooldown_ms;
                // Reset the timeout timer too — we just retx'd, give the new
                // ACK a fresh round-trip window before timer-firing again.
                // Without this, fast_hole + timeout both fire for the same
                // seq, doubling the duplicate count. Likewise disarm the
                // hole_probe — fast_hole already serves that purpose.
                s.timeout_ms = currentAckTimeoutMs();
                s.hole_probe_armed = false;
                s.hole_probe_timer_ms = 0;
                LOG_MODEM(INFO,
                          "SR-ARQ: Fast retransmit base seq=%d (bitmap=0x%08X, fast=%d/%d, cooldown=%ums, confirms=%d)",
                          tx_base_seq_, bitmap, s.fast_retx_count, MAX_FAST_RETX_PER_HOLE,
                          fast_retx_cooldown_ms, s.hole_ack_count);
                retransmitFrame(base_slot, RetransmitCause::FAST_HOLE);
            }
        }
    }

    // Reset hole and fast-retransmit guards when base advances (new gap context).
    if (tx_base_seq_ != base_before_ack) {
        for (size_t i = 0; i < config_.window_size; i++) {
            size_t slot = seqToSlot((tx_base_seq_ + i) & 0xFFFF);
            tx_window_[slot].hole_ack_count = 0;
            tx_window_[slot].fast_retx_count = 0;
            tx_window_[slot].fast_retx_cooldown_ms = 0;
            tx_window_[slot].hole_probe_armed = false;
            tx_window_[slot].hole_probe_timer_ms = 0;
            tx_window_[slot].hole_probe_count = 0;
        }
    }
}

void SelectiveRepeatARQ::handleNackFrame(const v2::ControlFrame& frame) {
    uint16_t seq = frame.seq;

    LOG_MODEM(DEBUG, "SR-ARQ: NACK seq=%d", seq);

    if (isInTXWindow(seq)) {
        size_t slot = seqToSlot(seq);
        if (tx_window_[slot].active && !tx_window_[slot].acked) {
            retransmitFrame(slot, RetransmitCause::NACK);
        }
    }
}

void SelectiveRepeatARQ::tick(uint32_t elapsed_ms) {
    arq_time_ms_ += elapsed_ms;

    if (ack_dedup_timer_ms_ > 0) {
        if (elapsed_ms >= ack_dedup_timer_ms_) {
            ack_dedup_timer_ms_ = 0;
        } else {
            ack_dedup_timer_ms_ -= elapsed_ms;
        }
    }

    // Delayed ACK repeats (time diversity for fading channels).
    // Jobs are one-shot; each queued copy is sent once when its timer expires.
    for (auto it = ack_repeat_jobs_.begin(); it != ack_repeat_jobs_.end();) {
        AckRepeatJob& job = *it;
        if (elapsed_ms >= job.timer_ms) {
            transmitData(job.frame_data);
            stats_.acks_sent++;
            LOG_MODEM(INFO, "SR-ARQ: ACK_REPEAT_SENT copy=%d", job.copy_index);
            it = ack_repeat_jobs_.erase(it);
            continue;
        }

        job.timer_ms -= elapsed_ms;
        ++it;
    }

    // TX side: check for timeouts and retransmit
    for (size_t i = 0; i < config_.window_size; i++) {
        size_t slot = seqToSlot((tx_base_seq_ + i) & 0xFFFF);
        TXSlot& s = tx_window_[slot];

        if (s.active && !s.acked) {
            if (s.fast_retx_cooldown_ms > 0) {
                if (elapsed_ms >= s.fast_retx_cooldown_ms) {
                    s.fast_retx_cooldown_ms = 0;
                } else {
                    s.fast_retx_cooldown_ms -= elapsed_ms;
                }
            }

            if (s.hole_probe_armed) {
                if (elapsed_ms >= s.hole_probe_timer_ms) {
                    constexpr int MAX_HOLE_PROBE_RETX = 1;
                    if (s.hole_probe_count < MAX_HOLE_PROBE_RETX) {
                        s.hole_probe_count++;
                        s.hole_probe_timer_ms = std::clamp((currentAckTimeoutMs() * 2) / 3, 1800u, 3000u);
                        LOG_MODEM(INFO,
                                  "SR-ARQ: Hole-probe retransmit seq=%d (%d/%d)",
                                  s.seq, s.hole_probe_count, MAX_HOLE_PROBE_RETX);
                        retransmitFrame(slot, RetransmitCause::HOLE_PROBE);
                    } else {
                        s.hole_probe_armed = false;
                        s.hole_probe_timer_ms = 0;
                    }
                } else {
                    s.hole_probe_timer_ms -= elapsed_ms;
                }
            }

            if (elapsed_ms >= s.timeout_ms) {
                stats_.timeouts++;
                retransmitFrame(slot, RetransmitCause::TIMEOUT);
            } else {
                s.timeout_ms -= elapsed_ms;
            }
        }
    }

    // RX side: delayed SACK for half-duplex burst handling
    if (sack_pending_) {
        if (elapsed_ms >= sack_timer_ms_) {
            LOG_MODEM(DEBUG, "SR-ARQ: SACK timer expired, sending SACK");
            stats_.sack_trigger_timer++;
            sendSack();
            sack_pending_ = false;
            sack_timer_ms_ = 0;
            frames_since_ack_ = 0;
        } else {
            sack_timer_ms_ -= elapsed_ms;
        }
    }
}

void SelectiveRepeatARQ::retransmitFrame(size_t slot, RetransmitCause cause) {
    TXSlot& s = tx_window_[slot];

    if (cause == RetransmitCause::TIMEOUT) {
        // New timeout epoch: permit another cycle of hole-based fast retransmits.
        s.fast_retx_count = 0;
        s.fast_retx_cooldown_ms = 0;
        s.hole_ack_count = 0;
        s.hole_probe_armed = false;
        s.hole_probe_timer_ms = 0;
        s.hole_probe_count = 0;
    }

    // Karn's algorithm: once retransmitted, do not use this frame for RTT sampling.
    s.rtt_sample_eligible = false;

    s.retry_count++;
    if (s.retry_count >= config_.max_retries) {
        LOG_MODEM(ERROR, "SR-ARQ: Frame seq=%d failed after %d retries",
                  s.seq, config_.max_retries);
        stats_.failed++;

        s.active = false;
        tx_in_flight_--;

        if (on_send_complete_) {
            on_send_complete_(false);
        }

        advanceTXWindow();
        return;
    }

    const char* cause_str = "unknown";
    switch (cause) {
        case RetransmitCause::TIMEOUT: cause_str = "timeout"; break;
        case RetransmitCause::FAST_HOLE: cause_str = "fast-hole"; break;
        case RetransmitCause::HOLE_PROBE: cause_str = "hole-probe"; break;
        case RetransmitCause::NACK: cause_str = "nack"; break;
    }

    LOG_MODEM(INFO, "SR-ARQ: Retransmitting seq=%d (attempt %d/%d, cause=%s)",
              s.seq, s.retry_count + 1, config_.max_retries, cause_str);

    stats_.retransmissions++;
    switch (cause) {
        case RetransmitCause::TIMEOUT: stats_.retransmissions_timeout++; break;
        case RetransmitCause::FAST_HOLE: stats_.retransmissions_fast_hole++; break;
        case RetransmitCause::HOLE_PROBE: stats_.retransmissions_hole_probe++; break;
        case RetransmitCause::NACK: stats_.retransmissions_nack++; break;
    }
    s.timeout_ms = currentAckTimeoutMs();
    transmitData(s.frame_data);
}

void SelectiveRepeatARQ::advanceTXWindow() {
    while (tx_in_flight_ > 0) {
        size_t slot = seqToSlot(tx_base_seq_);
        if (tx_window_[slot].active && !tx_window_[slot].acked) {
            break;
        }
        if (tx_window_[slot].active) {
            maybeSampleRTT(tx_window_[slot]);
            tx_window_[slot].active = false;
            tx_window_[slot].hole_ack_count = 0;
            tx_window_[slot].fast_retx_count = 0;
            tx_window_[slot].fast_retx_cooldown_ms = 0;
            tx_window_[slot].hole_probe_armed = false;
            tx_window_[slot].hole_probe_timer_ms = 0;
            tx_window_[slot].hole_probe_count = 0;
            tx_in_flight_--;

            if (on_send_complete_) {
                on_send_complete_(true);
            }
        }
        tx_base_seq_ = (tx_base_seq_ + 1) & 0xFFFF;
    }
}

void SelectiveRepeatARQ::advanceRXWindow() {
    while (true) {
        size_t slot = seqToSlot(rx_base_seq_);
        if (!rx_window_[slot].received) {
            break;
        }

        LOG_MODEM(DEBUG, "SR-ARQ: Delivering seq=%d", rx_base_seq_);

        // Update flags from the delivered frame's stored flags (not from the
        // last arrived frame). When advanceRXWindow delivers multiple buffered
        // frames in sequence (e.g., after retransmission fills a gap), the
        // Connection layer calls lastRxHadMoreData() to check MORE_FRAG.
        // Without this, it would see the flags from handleDataFrame's last
        // call, which is the gap-filling frame — not the frame being delivered.
        last_rx_flags_ = rx_window_[slot].flags;
        last_rx_more_data_ = (rx_window_[slot].flags & v2::Flags::MORE_FRAG) != 0;

        if (on_data_received_) {
            on_data_received_(rx_window_[slot].payload);
        }

        rx_window_[slot].received = false;
        rx_window_[slot].payload.clear();
        rx_base_seq_ = (rx_base_seq_ + 1) & 0xFFFF;
    }
}

void SelectiveRepeatARQ::sendSack() {
    uint32_t bitmap = buildRXBitmap();
    uint16_t base_seq = (rx_base_seq_ - 1) & 0xFFFF;

    // Use NACK with bitmap as SACK
    auto sack = v2::ControlFrame::makeNack(local_call_, remote_call_,
                                            base_seq,
                                            bitmap);
    // Override type to ACK for cumulative ack behavior
    sack.type = v2::FrameType::ACK;

    stats_.sacks_sent++;
    stats_.acks_sent++;

    auto data = sack.serialize();

    LOG_MODEM(INFO, "SR-ARQ: Sent SACK base=%d bitmap=0x%08X",
              base_seq, bitmap);

    transmitData(data);

    last_sack_base_valid_ = true;
    last_sack_base_ = base_seq;
    bool repeat_ack = ack_repeat_count_ > 1;

    // Coalesce pending repeats:
    // - Keep queued repeats matching current ACK state (base+bitmap).
    // - Drop queued repeats for superseded ACK states.
    bool have_copy_queued[4] = {false, false, false, false};
    size_t removed_jobs = 0;
    for (auto it = ack_repeat_jobs_.begin(); it != ack_repeat_jobs_.end();) {
        if (it->base_seq == base_seq && it->bitmap == bitmap) {
            if (it->copy_index >= 0 && it->copy_index < 4) {
                have_copy_queued[it->copy_index] = true;
            }
            ++it;
        } else {
            it = ack_repeat_jobs_.erase(it);
            removed_jobs++;
        }
    }
    if (removed_jobs > 0) {
        stats_.ack_repeat_jobs_coalesced += static_cast<int>(removed_jobs);
        LOG_MODEM(INFO, "SR-ARQ: ACK_REPEAT coalesced %zu queued jobs", removed_jobs);
    }

    // Schedule delayed repeats for any ACK state when the connection profile
    // requests ACK diversity. Fading tests showed the dominant loss is often a
    // plain cumulative ACK (bitmap=0), especially tail ACKs; repeating only
    // selective SACKs leaves those losses unprotected. Superseded ACK states
    // were coalesced above, and the sender-side stale/duplicate guards make
    // late repeats benign.
    for (int copy_index = 2; repeat_ack && copy_index <= ack_repeat_count_; ++copy_index) {
        if (copy_index < 4 && have_copy_queued[copy_index]) {
            continue;
        }

        uint32_t delay_ms = ackRepeatDelayForCopy(copy_index);
        int jitter_ms = ackRepeatJitterMs(base_seq, bitmap, copy_index);

        int64_t scheduled = static_cast<int64_t>(delay_ms) + jitter_ms;
        if (scheduled < 1) {
            scheduled = 1;
        }

        if (ack_repeat_jobs_.size() >= 16) {
            LOG_MODEM(WARN, "SR-ARQ: ACK_REPEAT queue full, dropping oldest pending repeat");
            ack_repeat_jobs_.pop_front();
            stats_.ack_repeat_jobs_dropped++;
        }

        AckRepeatJob job;
        job.frame_data = data;
        job.base_seq = base_seq;
        job.bitmap = bitmap;
        job.timer_ms = static_cast<uint32_t>(scheduled);
        job.copy_index = copy_index;
        ack_repeat_jobs_.push_back(std::move(job));

        LOG_MODEM(INFO, "SR-ARQ: ACK_REPEAT scheduled copy=%d delay=%ums jitter=%dms enabled=%d queue=%zu",
                  copy_index, static_cast<uint32_t>(scheduled), jitter_ms, repeat_ack ? 1 : 0,
                  ack_repeat_jobs_.size());
    }
}

uint32_t SelectiveRepeatARQ::currentAckTimeoutMs() const {
    if (adaptive_ack_timeout_ms_ > 0) {
        return adaptive_ack_timeout_ms_;
    }
    return config_.ack_timeout_ms;
}

void SelectiveRepeatARQ::maybeSampleRTT(TXSlot& slot) {
    if (!slot.rtt_sample_eligible) {
        return;
    }
    if (arq_time_ms_ < slot.first_tx_ms) {
        return;
    }

    uint32_t sample_ms = static_cast<uint32_t>(
        std::min<uint64_t>(arq_time_ms_ - slot.first_tx_ms, 60000ULL));
    slot.rtt_sample_eligible = false;

    if (sample_ms < 50) {
        return;
    }

    // RFC6298-style estimator (Karn-safe: retransmitted slots are marked ineligible).
    if (!have_rtt_estimator_) {
        srtt_ms_ = static_cast<float>(sample_ms);
        rttvar_ms_ = static_cast<float>(sample_ms) * 0.5f;
        have_rtt_estimator_ = true;
    } else {
        float sample_f = static_cast<float>(sample_ms);
        float err = std::fabs(srtt_ms_ - sample_f);
        rttvar_ms_ = 0.75f * rttvar_ms_ + 0.25f * err;
        srtt_ms_ = 0.875f * srtt_ms_ + 0.125f * sample_f;
    }

    float rto_f = srtt_ms_ + 4.0f * rttvar_ms_;

    // SRTT-aware floor: once we have RTT samples, let the timeout drop close to
    // the actual round trip rather than staying pinned at config_.ack_timeout_ms / 2.
    // For OFDM DQPSK R1/2 with window=4 the static floor is 2250ms, but observed
    // RTTs are 400-700ms — paying 2.25s per lost-ACK recovery accounted for ~30s
    // of unnecessary wait on a 50KB transfer. Bound to [600ms, 2500ms] so the
    // estimator can adapt down on clean channels but stays safe on slow paths.
    uint32_t floor_ms;
    if (have_rtt_estimator_) {
        uint32_t srtt_floor = static_cast<uint32_t>(srtt_ms_ * 1.5f + 0.5f);
        floor_ms = std::clamp(srtt_floor, 600u, 2500u);
    } else {
        floor_ms = std::max(1200u, config_.ack_timeout_ms / 2);
    }
    // Never let the adaptive timeout drop below the static configured value.
    // The static value is sized for the *worst-case burst RTT* including
    // soundcard chain latency (see computeOfdmAckTimeoutMs). Adaptive RTT
    // sampling sees per-frame RTT in the steady state which can be much
    // shorter than the burst-end RTT, so without this clamp the timer fires
    // before the last frame's ACK can return — causing spurious retx storms
    // observed on real Mac<->Pi hardware after the tick-rate fix.
    floor_ms = std::max(floor_ms, config_.ack_timeout_ms);
    const uint32_t ceiling_ms = std::max<uint32_t>(12000u, config_.ack_timeout_ms);
    adaptive_ack_timeout_ms_ = std::clamp(static_cast<uint32_t>(rto_f + 0.5f),
                                          floor_ms, ceiling_ms);

    LOG_MODEM(DEBUG, "SR-ARQ: RTT sample=%ums srtt=%.1f rttvar=%.1f rto=%ums",
              sample_ms, srtt_ms_, rttvar_ms_, adaptive_ack_timeout_ms_);
}

uint32_t SelectiveRepeatARQ::ackRepeatDelayForCopy(int copy_index) const {
    // copy_index 2 = first delayed repeat.
    if (copy_index <= 2) {
        return ack_repeat_delay_ms_;
    }
    // Wider spacing for later copies improves time diversity in fading.
    return ack_repeat_delay_ms_ * 3;
}

int SelectiveRepeatARQ::ackRepeatJitterMs(uint16_t base_seq, uint32_t bitmap, int copy_index) const {
    uint32_t h = static_cast<uint32_t>(base_seq);
    h = (h * 1103515245u + 12345u) ^ (bitmap << 8) ^ (bitmap >> 16)
        ^ static_cast<uint32_t>(copy_index * 7919);
    int jitter = static_cast<int>(h % 61u) - 30;  // [-30, +30] ms
    return jitter;
}

uint32_t SelectiveRepeatARQ::buildRXBitmap() const {
    uint32_t bitmap = 0;

    for (int i = 0; i < 32 && i < static_cast<int>(config_.window_size); i++) {
        size_t slot = seqToSlot((rx_base_seq_ + i) & 0xFFFF);
        if (rx_window_[slot].received) {
            bitmap |= (1u << i);
        }
    }

    return bitmap;
}

size_t SelectiveRepeatARQ::seqToSlot(uint16_t seq) const {
    return seq % MAX_WINDOW;
}

bool SelectiveRepeatARQ::isInTXWindow(uint16_t seq) const {
    uint16_t diff = (seq - tx_base_seq_) & 0xFFFF;
    return diff < config_.window_size;
}

bool SelectiveRepeatARQ::isInRXWindow(uint16_t seq) const {
    uint16_t diff = (seq - rx_base_seq_) & 0xFFFF;
    return diff < config_.window_size;
}

void SelectiveRepeatARQ::transmitData(const Bytes& data) {
    if (on_transmit_) {
        on_transmit_(data);
    }
}

void SelectiveRepeatARQ::setTransmitCallback(TransmitCallback cb) {
    on_transmit_ = std::move(cb);
}

void SelectiveRepeatARQ::setDataReceivedCallback(DataReceivedCallback cb) {
    on_data_received_ = std::move(cb);
}

void SelectiveRepeatARQ::setSendCompleteCallback(SendCompleteCallback cb) {
    on_send_complete_ = std::move(cb);
}

void SelectiveRepeatARQ::abortPendingTx() {
    for (auto& slot : tx_window_) {
        slot.active = false;
        slot.acked = false;
        slot.frame_data.clear();
        slot.timeout_ms = 0;
        slot.first_tx_ms = 0;
        slot.rtt_sample_eligible = false;
        slot.retry_count = 0;
        slot.hole_ack_count = 0;
        slot.fast_retx_count = 0;
        slot.fast_retx_cooldown_ms = 0;
        slot.hole_probe_armed = false;
        slot.hole_probe_timer_ms = 0;
        slot.hole_probe_count = 0;
    }

    tx_base_seq_ = tx_next_seq_;
    tx_in_flight_ = 0;

    // Cancel pending control TX from ARQ side as well.
    sack_pending_ = false;
    sack_timer_ms_ = 0;
    frames_since_ack_ = 0;
    ack_repeat_jobs_.clear();
    ack_dedup_timer_ms_ = 0;

    LOG_MODEM(INFO, "SR-ARQ: Aborted pending TX state");
}

void SelectiveRepeatARQ::reset() {
    for (auto& slot : tx_window_) {
        slot.active = false;
        slot.acked = false;
        slot.frame_data.clear();
        slot.first_tx_ms = 0;
        slot.rtt_sample_eligible = false;
        slot.hole_ack_count = 0;
        slot.fast_retx_count = 0;
        slot.fast_retx_cooldown_ms = 0;
        slot.hole_probe_armed = false;
        slot.hole_probe_timer_ms = 0;
        slot.hole_probe_count = 0;
    }
    tx_base_seq_ = 0;
    tx_next_seq_ = 0;
    tx_in_flight_ = 0;

    for (auto& slot : rx_window_) {
        slot.received = false;
        slot.payload.clear();
    }
    rx_base_seq_ = 0;

    last_rx_more_data_ = false;
    last_rx_flags_ = 0;

    sack_pending_ = false;
    sack_timer_ms_ = 0;
    frames_since_ack_ = 0;

    ack_repeat_jobs_.clear();
    last_sack_base_valid_ = false;
    last_sack_base_ = 0;
    last_ack_signature_valid_ = false;
    last_ack_seq_ = 0;
    last_ack_bitmap_ = 0;
    ack_dedup_timer_ms_ = 0;
    arq_time_ms_ = 0;
    have_rtt_estimator_ = false;
    srtt_ms_ = 0.0f;
    rttvar_ms_ = 0.0f;
    adaptive_ack_timeout_ms_ = config_.ack_timeout_ms;

    LOG_MODEM(DEBUG, "SR-ARQ: Reset");
}

} // namespace protocol
} // namespace ultra
