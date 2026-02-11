#include "selective_repeat_arq.hpp"
#include "ultra/logging.hpp"

namespace ultra {
namespace protocol {

SelectiveRepeatARQ::SelectiveRepeatARQ(const ARQConfig& config)
    : config_(config)
{
    if (config_.window_size > MAX_WINDOW) {
        config_.window_size = MAX_WINDOW;
    }
    if (config_.window_size < 1) {
        config_.window_size = 1;
    }
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

    size_t slot = seqToSlot(tx_next_seq_);

    // Create and serialize v2 frame
    auto frame = v2::DataFrame::makeData(local_call_, remote_call_, tx_next_seq_, data);
    frame.flags = flags;

    tx_window_[slot].active = true;
    tx_window_[slot].frame_data = frame.serialize();
    tx_window_[slot].seq = tx_next_seq_;
    tx_window_[slot].timeout_ms = config_.ack_timeout_ms;
    tx_window_[slot].retry_count = 0;
    tx_window_[slot].acked = false;
    tx_window_[slot].hole_ack_count = 0;
    tx_window_[slot].fast_retransmitted = false;

    transmitData(tx_window_[slot].frame_data);

    LOG_MODEM(DEBUG, "SR-ARQ: Sent DATA seq=%d slot=%zu, window=[%d,%d)",
              tx_next_seq_, slot, tx_base_seq_, (tx_next_seq_ + 1) & 0xFFFF);

    stats_.frames_sent++;
    tx_next_seq_ = (tx_next_seq_ + 1) & 0xFFFF;
    tx_in_flight_++;

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

    uint16_t seq = frame.seq;

    if (isInRXWindow(seq)) {
        size_t slot = seqToSlot(seq);

        if (!rx_window_[slot].received) {
            rx_window_[slot].received = true;
            rx_window_[slot].seq = seq;
            rx_window_[slot].payload = frame.payload;
            rx_window_[slot].flags = frame.flags;
            stats_.frames_received++;

            LOG_MODEM(DEBUG, "SR-ARQ: DATA seq=%d stored in slot %zu", seq, slot);

            if (seq == rx_base_seq_) {
                advanceRXWindow();
            } else {
                stats_.out_of_order++;
                LOG_MODEM(DEBUG, "SR-ARQ: Out-of-order seq=%d (expected %d)",
                          seq, rx_base_seq_);
            }
        } else {
            LOG_MODEM(DEBUG, "SR-ARQ: Duplicate DATA seq=%d", seq);
        }

        // ACK strategy: immediate when burst window is full or last fragment
        frames_since_ack_++;
        if (!last_rx_more_data_ || frames_since_ack_ >= config_.window_size) {
            // Window full or last fragment — ACK immediately
            sendSack();
            sack_pending_ = false;
            sack_timer_ms_ = 0;
            frames_since_ack_ = 0;
        } else {
            // More fragments coming, window not full — short delay as fallback
            sack_pending_ = true;
            sack_timer_ms_ = config_.sack_delay_ms;
        }

    } else {
        LOG_MODEM(WARN, "SR-ARQ: DATA seq=%d outside window [%d, %d)",
                  seq, rx_base_seq_, (rx_base_seq_ + config_.window_size) & 0xFFFF);
        // Out-of-window: send SACK immediately to help sender recover
        sendSack();
    }
}

void SelectiveRepeatARQ::handleAckFrame(const v2::ControlFrame& frame) {
    uint16_t seq = frame.seq;
    uint8_t bitmap = frame.payload[2];

    LOG_MODEM(INFO, "SR-ARQ: ACK seq=%d bitmap=0x%02X (base=%d, in_flight=%zu)",
              seq, bitmap, tx_base_seq_, tx_in_flight_);

    // Stale-ACK guard: reject ACKs strictly older than (tx_base_seq_ - 1).
    // seq == (tx_base_seq_ - 1) is valid — it's the common "no new cumulative progress"
    // ACK that still carries a fresh SACK bitmap for the current window.
    uint16_t ack_base = (tx_base_seq_ - 1) & 0xFFFF;
    uint16_t back = (ack_base - seq) & 0xFFFF;
    if (back > 0 && back < 0x8000) {
        LOG_MODEM(INFO, "SR-ARQ: Stale ACK seq=%d < base-1=%d, ignoring", seq, ack_base);
        return;
    }

    // Far-future guard: reject ACKs implausibly ahead (> window_size + 1 past base)
    uint16_t forward = (seq - ack_base) & 0xFFFF;
    if (forward > config_.window_size + 1 && forward < 0x8000) {
        LOG_MODEM(INFO, "SR-ARQ: Future ACK seq=%d too far ahead of base=%d, ignoring", seq, tx_base_seq_);
        return;
    }

    // --- Cumulative ACK: advance base past all frames up to seq ---
    uint16_t old_base = tx_base_seq_;
    while (tx_in_flight_ > 0 && tx_base_seq_ != ((seq + 1) & 0xFFFF)) {
        size_t slot = seqToSlot(tx_base_seq_);
        if (tx_window_[slot].active) {
            tx_window_[slot].active = false;
            tx_window_[slot].acked = true;
            tx_in_flight_--;
            stats_.acks_received++;

            if (on_send_complete_) {
                on_send_complete_(true);
            }
        }
        tx_base_seq_ = (tx_base_seq_ + 1) & 0xFFFF;
    }

    // Reset fast-retransmit guards when base advances (new gap context)
    if (tx_base_seq_ != old_base) {
        for (size_t i = 0; i < config_.window_size; i++) {
            size_t slot = seqToSlot((tx_base_seq_ + i) & 0xFFFF);
            tx_window_[slot].hole_ack_count = 0;
            tx_window_[slot].fast_retransmitted = false;
        }
    }

    // --- Positive-only SACK bitmap: mark frames the receiver confirms it HAS ---
    if (bitmap != 0) {
        bool any_sacked = false;
        for (int i = 0; i < 8 && i < static_cast<int>(config_.window_size); i++) {
            if (!(bitmap & (1 << i))) continue;

            uint16_t sack_seq = (tx_base_seq_ + i) & 0xFFFF;
            size_t slot = seqToSlot(sack_seq);

            if (tx_window_[slot].active && !tx_window_[slot].acked && tx_window_[slot].seq == sack_seq) {
                tx_window_[slot].acked = true;
                any_sacked = true;
                LOG_MODEM(INFO, "SR-ARQ: SACK seq=%d confirmed received (bitmap=0x%02X)", sack_seq, bitmap);
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
    bool has_hole = (bitmap & 0x01) == 0 && (bitmap & 0xFE) != 0;

    if (is_aligned && has_hole) {
        size_t base_slot = seqToSlot(tx_base_seq_);
        TXSlot& s = tx_window_[base_slot];

        if (s.active && !s.acked && s.seq == tx_base_seq_ && !s.fast_retransmitted) {
            s.hole_ack_count++;
            LOG_MODEM(INFO, "SR-ARQ: Hole detected for base seq=%d (hole_count=%d, bitmap=0x%02X)",
                      tx_base_seq_, s.hole_ack_count, bitmap);

            // Fast retransmit on first hole detection (no duplicate-ACK threshold needed:
            // bitmap already proves receiver has later frames but not base)
            s.fast_retransmitted = true;
            LOG_MODEM(INFO, "SR-ARQ: Fast retransmit base seq=%d (bitmap=0x%02X)", tx_base_seq_, bitmap);
            retransmitFrame(base_slot);
        }
    }
}

void SelectiveRepeatARQ::handleNackFrame(const v2::ControlFrame& frame) {
    uint16_t seq = frame.seq;

    LOG_MODEM(DEBUG, "SR-ARQ: NACK seq=%d", seq);

    if (isInTXWindow(seq)) {
        size_t slot = seqToSlot(seq);
        if (tx_window_[slot].active && !tx_window_[slot].acked) {
            retransmitFrame(slot);
        }
    }
}

void SelectiveRepeatARQ::tick(uint32_t elapsed_ms) {
    // TX side: check for timeouts and retransmit
    for (size_t i = 0; i < config_.window_size; i++) {
        size_t slot = seqToSlot((tx_base_seq_ + i) & 0xFFFF);
        TXSlot& s = tx_window_[slot];

        if (s.active && !s.acked) {
            if (elapsed_ms >= s.timeout_ms) {
                stats_.timeouts++;
                retransmitFrame(slot);
            } else {
                s.timeout_ms -= elapsed_ms;
            }
        }
    }

    // RX side: delayed SACK for half-duplex burst handling
    if (sack_pending_) {
        if (elapsed_ms >= sack_timer_ms_) {
            LOG_MODEM(DEBUG, "SR-ARQ: SACK timer expired, sending SACK");
            sendSack();
            sack_pending_ = false;
            sack_timer_ms_ = 0;
            frames_since_ack_ = 0;
        } else {
            sack_timer_ms_ -= elapsed_ms;
        }
    }
}

void SelectiveRepeatARQ::retransmitFrame(size_t slot) {
    TXSlot& s = tx_window_[slot];

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

    LOG_MODEM(DEBUG, "SR-ARQ: Retransmitting seq=%d (attempt %d/%d)",
              s.seq, s.retry_count + 1, config_.max_retries);

    stats_.retransmissions++;
    s.timeout_ms = config_.ack_timeout_ms;
    transmitData(s.frame_data);
}

void SelectiveRepeatARQ::advanceTXWindow() {
    while (tx_in_flight_ > 0) {
        size_t slot = seqToSlot(tx_base_seq_);
        if (tx_window_[slot].active && !tx_window_[slot].acked) {
            break;
        }
        if (tx_window_[slot].active) {
            tx_window_[slot].active = false;
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
    uint8_t bitmap = buildRXBitmap();

    // Use NACK with bitmap as SACK
    auto sack = v2::ControlFrame::makeNack(local_call_, remote_call_,
                                            (rx_base_seq_ - 1) & 0xFFFF,
                                            bitmap);
    // Override type to ACK for cumulative ack behavior
    sack.type = v2::FrameType::ACK;
    sack.payload[2] = bitmap;  // Store bitmap in payload

    stats_.sacks_sent++;
    stats_.acks_sent++;

    auto data = sack.serialize();

    LOG_MODEM(INFO, "SR-ARQ: Sent SACK base=%d bitmap=0x%02X",
              (rx_base_seq_ - 1) & 0xFFFF, bitmap);

    transmitData(data);

    // Conditional ACK repeat: send a second copy only for critical cases:
    // - hole bitmap (bit0=0, higher bits set) → sender needs gap info urgently
    // - out-of-window triggered ACKs are already immediate (called from handleDataFrame else branch)
    bool has_hole = (bitmap & 0x01) == 0 && (bitmap & 0xFE) != 0;
    if (has_hole) {
        LOG_MODEM(INFO, "SR-ARQ: Repeating SACK (hole bitmap=0x%02X)", bitmap);
        transmitData(data);
    }
}

uint8_t SelectiveRepeatARQ::buildRXBitmap() const {
    uint8_t bitmap = 0;

    for (int i = 0; i < 8 && i < static_cast<int>(config_.window_size); i++) {
        size_t slot = seqToSlot((rx_base_seq_ + i) & 0xFFFF);
        if (rx_window_[slot].received) {
            bitmap |= (1 << i);
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

void SelectiveRepeatARQ::reset() {
    for (auto& slot : tx_window_) {
        slot.active = false;
        slot.acked = false;
        slot.frame_data.clear();
        slot.hole_ack_count = 0;
        slot.fast_retransmitted = false;
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

    LOG_MODEM(DEBUG, "SR-ARQ: Reset");
}

} // namespace protocol
} // namespace ultra
