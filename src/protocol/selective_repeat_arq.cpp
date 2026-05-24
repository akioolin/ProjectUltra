#include "selective_repeat_arq.hpp"
#include "selective_repeat_arq_policy.hpp"
#include "ultra/logging.hpp"
#include "ultra/phy_diagnostics.hpp"
#include <sstream>

namespace ultra {
namespace protocol {

namespace arq_policy = selective_repeat_arq_policy;

namespace {

uint8_t dataFrameFlags(uint8_t flags) {
    return static_cast<uint8_t>(v2::Flags::VERSION_V2 | flags);
}

const char* boolDigit(bool value) {
    return value ? "1" : "0";
}

}  // namespace

SelectiveRepeatARQ::SelectiveRepeatARQ(const ARQConfig& config)
    : config_(config)
{
    config_.window_size = arq_policy::clampWindowSize(config_.window_size, MAX_WINDOW);
    config_.ack_batch_size = arq_policy::clampAckBatchSize(
        config_.ack_batch_size, config_.window_size, MAX_WINDOW);
    adaptive_ack_timeout_ms_ = config_.ack_timeout_ms;
}

void SelectiveRepeatARQ::setCallsigns(const std::string& local, const std::string& remote) {
    local_call_ = sanitizeCallsign(local);
    remote_call_ = sanitizeCallsign(remote);
}

void SelectiveRepeatARQ::setCodeRate(CodeRate rate) {
    if (rate == code_rate_) {
        return;
    }

    code_rate_ = rate;

    size_t aborted_unacked = 0;
    size_t cleared_acked = 0;
    for (auto& slot : tx_window_) {
        if (!slot.active) {
            continue;
        }

        if (slot.acked) {
            cleared_acked++;
        } else {
            aborted_unacked++;
        }

        slot.active = false;
        slot.acked = false;
        slot.frame_data.clear();
        slot.fixed_frame_codewords = fixed_frame_codewords_;
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
        clearTXSlotRepairState(slot);
    }

    if (aborted_unacked > 0 || cleared_acked > 0 || tx_in_flight_ > 0) {
        tx_next_seq_ = tx_base_seq_;
        tx_in_flight_ = 0;
        last_ack_signature_valid_ = false;
        last_ack_seq_ = 0;
        last_ack_bitmap_ = 0;
        ack_dedup_timer_ms_ = 0;

        LOG_MODEM(WARN,
                  "SR-ARQ: Code rate changed, aborted %zu unACKed in-flight TX slots "
                  "(cleared %zu SACKed); rewound TX seq to %u",
                  aborted_unacked, cleared_acked, tx_next_seq_);
    }

    size_t discarded_rx = 0;
    for (auto& slot : rx_window_) {
        if (!slot.received && !slot.partial) {
            continue;
        }

        slot.received = false;
        clearPartialRXSlot(slot);
        slot.payload.clear();
        slot.flags = 0;
        slot.type = v2::FrameType::DATA;
        discarded_rx++;
    }

    if (discarded_rx > 0) {
        sack_pending_ = false;
        sack_timer_ms_ = 0;
        frames_since_ack_ = 0;
        ack_repeat_jobs_.clear();
        last_sack_base_valid_ = false;
        last_sack_base_ = 0;

        LOG_MODEM(WARN,
                  "SR-ARQ: Code rate changed, discarded %zu buffered RX slots at seq base %u",
                  discarded_rx, rx_base_seq_);
    }
}

void SelectiveRepeatARQ::setFixedFrameCodewords(int cw_count) {
    cw_count = v2::sanitizeFixedFrameCodewords(cw_count);
    if (cw_count == fixed_frame_codewords_) {
        return;
    }

    fixed_frame_codewords_ = cw_count;
    abortPendingTx();
    LOG_MODEM(INFO, "SR-ARQ: Fixed frame CW count set to %d", fixed_frame_codewords_);
}

bool SelectiveRepeatARQ::sendData(const Bytes& data) {
    return sendDataWithFlags(data, v2::Flags::NONE);
}

bool SelectiveRepeatARQ::sendData(const std::string& text) {
    Bytes data(text.begin(), text.end());
    return sendData(data);
}

bool SelectiveRepeatARQ::sendDataWithFlags(const Bytes& data, uint8_t flags) {
    return sendDataWithTypeAndFlags(data, v2::FrameType::DATA, flags);
}

bool SelectiveRepeatARQ::sendDataWithTypeAndFlags(const Bytes& data,
                                                  v2::FrameType frame_type,
                                                  uint8_t flags) {
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

    auto frame = v2::DataFrame::makeData(local_call_, remote_call_, seq, data, code_rate_);
    frame.type = frame_type;
    frame.flags = dataFrameFlags(flags);

    tx_window_[slot].active = true;
    tx_window_[slot].frame_data = frame.serialize();
    tx_window_[slot].info_codewords =
        v2::splitIntoCodewords(tx_window_[slot].frame_data, code_rate_);
    tx_window_[slot].seq = seq;
    tx_window_[slot].fixed_frame_codewords = fixed_frame_codewords_;
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
    tx_window_[slot].last_repair_bitmap = 0;
    tx_window_[slot].repair_cooldown_ms = 0;
    tx_window_[slot].repair_in_flight = false;
    tx_window_[slot].repair_guard_ms = 0;

    // Publish TX state before invoking the callback. Unit tests and future
    // low-latency transports may synchronously deliver the DATA and its ACK
    // before transmitData() returns.
    stats_.frames_sent++;
    tx_next_seq_ = (tx_next_seq_ + 1) & 0xFFFF;
    tx_in_flight_++;

    LOG_MODEM(DEBUG, "SR-ARQ: Sent %s seq=%d slot=%zu, window=[%d,%d)",
              v2::frameTypeToString(frame_type), seq, slot, tx_base_seq_, tx_next_seq_);
    if (ultra::phyDiagnosticsEnabled()) {
        std::ostringstream oss;
        oss << "event=arq_data_tx"
            << " local=" << local_call_
            << " remote=" << remote_call_
            << " seq=" << seq
            << " slot=" << slot
            << " frame_type=" << v2::frameTypeToString(frame_type)
            << " fixed=0"
            << " payload_bytes=" << data.size()
            << " frame_bytes=" << tx_window_[slot].frame_data.size()
            << " in_flight=" << tx_in_flight_
            << " window=" << config_.window_size
            << " timeout_ms=" << tx_window_[slot].timeout_ms;
        ultra::phyDiagLine(oss.str());
    }

    transmitData(tx_window_[slot].frame_data);

    return true;
}

bool SelectiveRepeatARQ::sendFixedDataWithFlags(const Bytes& data, uint8_t flags) {
    return sendFixedDataWithTypeAndFlags(data, v2::FrameType::DATA, flags);
}

bool SelectiveRepeatARQ::sendFixedDataWithTypeAndFlags(const Bytes& data,
                                                       v2::FrameType frame_type,
                                                       uint8_t flags) {
    if (!isReadyToSend()) {
        LOG_MODEM(WARN, "SR-ARQ: Window full, cannot send fixed frame");
        return false;
    }

    if (local_call_.empty() || remote_call_.empty()) {
        LOG_MODEM(ERROR, "SR-ARQ: Callsigns not set");
        return false;
    }

    const uint16_t seq = tx_next_seq_;
    size_t slot = seqToSlot(seq);

    auto frame = v2::makeFixedDataFrame(local_call_, remote_call_, seq, data,
                                        code_rate_, fixed_frame_codewords_);
    frame.type = frame_type;
    frame.flags = dataFrameFlags(flags);

    tx_window_[slot].active = true;
    tx_window_[slot].frame_data = frame.serialize();
    tx_window_[slot].info_codewords.clear();
    tx_window_[slot].seq = seq;
    tx_window_[slot].fixed_frame_codewords = fixed_frame_codewords_;
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
    tx_window_[slot].last_repair_bitmap = 0;
    tx_window_[slot].repair_cooldown_ms = 0;
    tx_window_[slot].repair_in_flight = false;
    tx_window_[slot].repair_guard_ms = 0;

    stats_.frames_sent++;
    tx_next_seq_ = (tx_next_seq_ + 1) & 0xFFFF;
    tx_in_flight_++;

    LOG_MODEM(DEBUG, "SR-ARQ: Sent fixed %s seq=%d slot=%zu cw=%d, window=[%d,%d)",
              v2::frameTypeToString(frame_type), seq, slot, fixed_frame_codewords_,
              tx_base_seq_, tx_next_seq_);
    if (ultra::phyDiagnosticsEnabled()) {
        std::ostringstream oss;
        oss << "event=arq_data_tx"
            << " local=" << local_call_
            << " remote=" << remote_call_
            << " seq=" << seq
            << " slot=" << slot
            << " frame_type=" << v2::frameTypeToString(frame_type)
            << " fixed=1"
            << " payload_bytes=" << data.size()
            << " frame_bytes=" << tx_window_[slot].frame_data.size()
            << " cw=" << fixed_frame_codewords_
            << " in_flight=" << tx_in_flight_
            << " window=" << config_.window_size
            << " timeout_ms=" << tx_window_[slot].timeout_ms;
        ultra::phyDiagLine(oss.str());
    }

    transmitData(tx_window_[slot].frame_data);
    return true;
}

bool SelectiveRepeatARQ::sendVariableDataWithFlags(const Bytes& data, uint8_t flags) {
    if (!isReadyToSend()) {
        LOG_MODEM(WARN, "SR-ARQ: Window full, cannot send variable frame");
        return false;
    }

    if (local_call_.empty() || remote_call_.empty()) {
        LOG_MODEM(ERROR, "SR-ARQ: Callsigns not set");
        return false;
    }

    const uint16_t seq = tx_next_seq_;
    size_t slot = seqToSlot(seq);

    auto frame = v2::DataFrame::makeData(local_call_, remote_call_, seq, data, code_rate_);
    frame.flags = dataFrameFlags(flags);

    tx_window_[slot].active = true;
    tx_window_[slot].frame_data = frame.serialize();
    tx_window_[slot].info_codewords =
        v2::splitIntoCodewords(tx_window_[slot].frame_data, code_rate_);
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
    tx_window_[slot].last_repair_bitmap = 0;
    tx_window_[slot].repair_cooldown_ms = 0;
    tx_window_[slot].repair_in_flight = false;
    tx_window_[slot].repair_guard_ms = 0;

    stats_.frames_sent++;
    tx_next_seq_ = (tx_next_seq_ + 1) & 0xFFFF;
    tx_in_flight_++;

    LOG_MODEM(INFO, "SR-ARQ: Sent variable DATA seq=%d slot=%zu total_cw=%d",
              seq, slot, static_cast<int>(frame.total_cw));
    if (ultra::phyDiagnosticsEnabled()) {
        std::ostringstream oss;
        oss << "event=arq_data_tx"
            << " local=" << local_call_
            << " remote=" << remote_call_
            << " seq=" << seq
            << " slot=" << slot
            << " frame_type=DATA"
            << " fixed=0"
            << " variable=1"
            << " payload_bytes=" << data.size()
            << " frame_bytes=" << tx_window_[slot].frame_data.size()
            << " total_cw=" << static_cast<int>(frame.total_cw)
            << " in_flight=" << tx_in_flight_
            << " window=" << config_.window_size
            << " timeout_ms=" << tx_window_[slot].timeout_ms;
        ultra::phyDiagLine(oss.str());
    }

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

size_t SelectiveRepeatARQ::getTxInFlightBytes() const {
    size_t bytes = 0;
    for (const auto& slot : tx_window_) {
        if (!slot.active || slot.acked) {
            continue;
        }

        auto frame = v2::DataFrame::deserialize(slot.frame_data);
        if (frame) {
            bytes += frame->payload.size();
        }
    }
    return bytes;
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
        if (header.type == v2::FrameType::DATA_REPAIR) {
            auto repair = v2::DataRepairFrame::deserialize(frame_data);
            if (repair) {
                handleDataRepairFrame(*repair);
            }
        } else {
            auto data_frame = v2::DataFrame::deserialize(frame_data);
            if (data_frame) {
                handleDataFrame(*data_frame);
            }
        }
    }
}

void SelectiveRepeatARQ::onPartialFrame(const v2::PartialFrameCodewords& partial) {
    handlePartialFrame(partial);
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
    const bool frame_final = (frame.flags & v2::Flags::FINAL) != 0;

    uint16_t seq = frame.seq;

    if (isInRXWindow(seq)) {
        uint16_t expected_seq = rx_base_seq_;
        size_t slot = seqToSlot(seq);
        bool new_frame = false;
        bool out_of_order = false;

        if (!rx_window_[slot].received) {
            rx_window_[slot].received = true;
            clearPartialRXSlot(rx_window_[slot]);
            rx_window_[slot].seq = seq;
            rx_window_[slot].payload = frame.payload;
            rx_window_[slot].flags = frame.flags;
            rx_window_[slot].type = frame.type;
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

        if (ultra::phyDiagnosticsEnabled()) {
            std::ostringstream oss;
            oss << "event=arq_data_rx"
                << " local=" << local_call_
                << " remote=" << remote_call_
                << " seq=" << seq
                << " expected=" << expected_seq
                << " slot=" << slot
                << " frame_type=" << v2::frameTypeToString(frame.type)
                << " new=" << boolDigit(new_frame)
                << " duplicate=" << boolDigit(!new_frame)
                << " out_of_order=" << boolDigit(out_of_order)
                << " in_window=1"
                << " more_frag=" << boolDigit(frame_more_frag)
                << " final=" << boolDigit(frame_final)
                << " rx_base=" << rx_base_seq_
                << " frames_since_ack=" << frames_since_ack_;
            ultra::phyDiagLine(oss.str());
        }

        // ACK strategy for burst traffic:
        // - Immediate ACK on hole detection (out-of-order) — safety valve, MUST
        //   stay first in the condition.
        // - While MORE_FRAG is set, threshold ACKs stay on the delayed path so
        //   a receiver does not transmit a control frame into the sender's
        //   still-arriving physical burst. MC-DPSK continuous-burst mode can
        //   opt out because decoded frames share one physical preamble and
        //   arrive only as the sample cursor reaches them.
        // - At message/stream tail, threshold ACKs fire immediately; otherwise
        //   only an explicit FINAL marker can use the short delayed timer. A
        //   plain MORE_FRAG=0 boundary may be just one message inside a still
        //   arriving physical burst.
        if (new_frame) {
            frames_since_ack_++;
        }

        const uint32_t batch_threshold = arq_policy::effectiveAckBatchThreshold(
            config_.ack_batch_size, config_.window_size);
        const bool batch_threshold_reached = frames_since_ack_ >= batch_threshold;
        const bool batch_ack_allowed = !frame_more_frag || ack_batch_through_more_frag_;
        if (arq_policy::shouldSendImmediateFrameNackForGap(
                out_of_order, frame_more_frag, frame_final)) {
            sendFrameNack(expected_seq);
        }

        if (out_of_order || (batch_threshold_reached && batch_ack_allowed)) {
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
            // Stream-aware timer: regular frames get the long physical burst
            // delay; explicit FINAL frames can use the short tail delay so the
            // sender's window advances promptly after the actual stream tail.
            // Sentinel sack_delay_short_ms_ = 0 preserves legacy long-delay
            // behavior for every frame.
            sack_timer_ms_ = arq_policy::sackTimerForFrame(
                sack_timer_ms_, config_.sack_delay_ms, sack_delay_short_ms_,
                frame_final);
        }

    } else {
        LOG_MODEM(WARN, "SR-ARQ: DATA seq=%d outside window [%d, %d)",
                  seq, rx_base_seq_, (rx_base_seq_ + config_.window_size) & 0xFFFF);
        if (ultra::phyDiagnosticsEnabled()) {
            std::ostringstream oss;
            oss << "event=arq_data_rx"
                << " local=" << local_call_
                << " remote=" << remote_call_
                << " seq=" << seq
                << " frame_type=" << v2::frameTypeToString(frame.type)
                << " in_window=0"
                << " rx_base=" << rx_base_seq_
                << " window=" << config_.window_size;
            ultra::phyDiagLine(oss.str());
        }
        // Out-of-window: send SACK immediately to help sender recover
        stats_.sack_trigger_out_of_window++;
        sendSack();
        sack_pending_ = false;
        sack_timer_ms_ = 0;
        frames_since_ack_ = 0;
    }
}

void SelectiveRepeatARQ::handlePartialFrame(const v2::PartialFrameCodewords& partial) {
    if (!partial.valid()) {
        return;
    }
    if (!isInRXWindow(partial.seq)) {
        LOG_MODEM(WARN, "SR-ARQ: Partial DATA seq=%d outside window [%d, %d)",
                  partial.seq, rx_base_seq_, (rx_base_seq_ + config_.window_size) & 0xFFFF);
        stats_.sack_trigger_out_of_window++;
        sendSack();
        sack_pending_ = false;
        sack_timer_ms_ = 0;
        frames_since_ack_ = 0;
        return;
    }

    size_t slot_index = seqToSlot(partial.seq);
    RXSlot& slot = rx_window_[slot_index];
    if (slot.received) {
        LOG_MODEM(DEBUG, "SR-ARQ: Partial DATA seq=%d ignored; frame already complete", partial.seq);
        sendSack();
        return;
    }

    if (!slot.partial || slot.seq != partial.seq) {
        if (partial.from_repair && (partial.decoded_bitmap & 0x1u) == 0) {
            LOG_MODEM(DEBUG,
                      "SR-ARQ: DATA_REPAIR seq=%d ignored; no partial slot and CW0 absent",
                      partial.seq);
            return;
        }
        clearPartialRXSlot(slot);
        slot.partial = true;
        slot.seq = partial.seq;
        slot.flags = partial.flags;
        slot.type = partial.type;
        slot.total_cw = partial.total_cw;
        slot.cw_data.assign(partial.total_cw, Bytes{});
        slot.partial_age_ms = 0;
    }

    bool merged = false;
    const uint32_t expected = partial.expectedBitmap();
    for (uint8_t cw = 0; cw < partial.total_cw && cw < 32; ++cw) {
        const uint32_t bit = 1u << cw;
        if ((partial.decoded_bitmap & bit) == 0) {
            continue;
        }
        if (cw >= partial.data.size() || partial.data[cw].empty()) {
            continue;
        }
        if ((slot.cw_bitmap & bit) == 0) {
            merged = true;
            if (partial.from_repair) {
                stats_.data_repair_cws_merged++;
            }
        }
        slot.cw_bitmap |= bit;
        slot.cw_data[cw] = partial.data[cw];
    }

    if (merged) {
        stats_.partial_frames_received++;
    }

    LOG_MODEM(INFO, "SR-ARQ: Partial DATA seq=%d cw=0x%08X/%08X missing=0x%08X",
              partial.seq, slot.cw_bitmap, expected, expected & ~slot.cw_bitmap);

    if (tryCompletePartialRXSlot(slot_index)) {
        return;
    }

    maybeSendCwNack(slot_index, expected & ~slot.cw_bitmap);
}

void SelectiveRepeatARQ::handleDataRepairFrame(const v2::DataRepairFrame& repair) {
    v2::PartialFrameCodewords partial;
    partial.type = v2::FrameType::DATA;
    partial.flags = v2::Flags::VERSION_V2;
    partial.seq = repair.target_seq;
    partial.src_hash = repair.src_hash;
    partial.dst_hash = repair.dst_hash;
    partial.total_cw = repair.original_total_cw;
    partial.decoded_bitmap = repair.repair_bitmap;
    partial.from_repair = true;
    partial.data.assign(repair.original_total_cw, Bytes{});

    auto indices = repair.repairIndices();
    for (size_t i = 0; i < indices.size() && i < repair.repair_codewords.size(); ++i) {
        partial.data[indices[i]] = repair.repair_codewords[i];
    }

    stats_.data_repairs_received++;
    LOG_MODEM(INFO, "SR-ARQ: DATA_REPAIR seq=%d bitmap=0x%04X repair_cw=%d",
              repair.target_seq, repair.repair_bitmap, repair.repair_count);
    handlePartialFrame(partial);
}

void SelectiveRepeatARQ::handleAckFrame(const v2::ControlFrame& frame) {
    uint16_t seq = frame.seq;
    uint32_t bitmap = arq_policy::decodeSackBitmap(frame.payload);

    LOG_MODEM(INFO, "SR-ARQ: ACK seq=%d bitmap=0x%08X (base=%d, in_flight=%zu)",
              seq, bitmap, tx_base_seq_, tx_in_flight_);
    if (ultra::phyDiagnosticsEnabled()) {
        std::ostringstream oss;
        oss << "event=arq_ack_rx"
            << " local=" << local_call_
            << " remote=" << remote_call_
            << " ack_seq=" << seq
            << " bitmap=0x" << std::hex << bitmap << std::dec
            << " tx_base=" << tx_base_seq_
            << " in_flight=" << tx_in_flight_;
        ultra::phyDiagLine(oss.str());
    }
    stats_.sacks_received++;

    // Stale-ACK guard: reject ACKs strictly older than (tx_base_seq_ - 1).
    // seq == (tx_base_seq_ - 1) is valid — it's the common "no new cumulative progress"
    // ACK that still carries a fresh SACK bitmap for the current window.
    const auto ack_freshness = arq_policy::classifyAckFreshness(
        seq, tx_base_seq_, config_.window_size);
    const uint16_t ack_base = (tx_base_seq_ - 1) & 0xFFFF;
    if (ack_freshness == arq_policy::AckFreshness::Stale) {
        stats_.stale_acks_ignored++;
        LOG_MODEM(INFO, "SR-ARQ: Stale ACK seq=%d < base-1=%d, ignoring", seq, ack_base);
        if (ultra::phyDiagnosticsEnabled()) {
            std::ostringstream oss;
            oss << "event=arq_ack_ignore"
                << " local=" << local_call_
                << " ack_seq=" << seq
                << " reason=stale"
                << " tx_base=" << tx_base_seq_
                << " bitmap=0x" << std::hex << bitmap << std::dec;
            ultra::phyDiagLine(oss.str());
        }
        return;
    }

    // Far-future guard: reject ACKs implausibly ahead (> window_size + 1 past base)
    if (ack_freshness == arq_policy::AckFreshness::Future) {
        stats_.future_acks_ignored++;
        LOG_MODEM(INFO, "SR-ARQ: Future ACK seq=%d too far ahead of base=%d, ignoring", seq, tx_base_seq_);
        if (ultra::phyDiagnosticsEnabled()) {
            std::ostringstream oss;
            oss << "event=arq_ack_ignore"
                << " local=" << local_call_
                << " ack_seq=" << seq
                << " reason=future"
                << " tx_base=" << tx_base_seq_
                << " bitmap=0x" << std::hex << bitmap << std::dec;
            ultra::phyDiagLine(oss.str());
        }
        return;
    }

    // ACK-repeat dedup guard: suppress clustered duplicate ACKs carrying
    // identical cumulative+bitmap information.
    if (arq_policy::shouldSuppressDuplicateAck(
            last_ack_signature_valid_, ack_dedup_timer_ms_, last_ack_seq_,
            last_ack_bitmap_, seq, bitmap)) {
        stats_.duplicate_acks_ignored++;
        LOG_MODEM(INFO, "SR-ARQ: Duplicate ACK seq=%d bitmap=0x%08X suppressed", seq, bitmap);
        if (ultra::phyDiagnosticsEnabled()) {
            std::ostringstream oss;
            oss << "event=arq_ack_ignore"
                << " local=" << local_call_
                << " ack_seq=" << seq
                << " reason=duplicate"
                << " tx_base=" << tx_base_seq_
                << " bitmap=0x" << std::hex << bitmap << std::dec;
            ultra::phyDiagLine(oss.str());
        }
        return;
    }
    last_ack_signature_valid_ = true;
    last_ack_seq_ = seq;
    last_ack_bitmap_ = bitmap;
    ack_dedup_timer_ms_ = arq_policy::ackDedupWindowMs(ack_repeat_delay_ms_);

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
            clearTXSlotRepairState(tx_window_[slot]);
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
    if (arq_policy::isAlignedBaseHoleAck(seq, tx_base_seq_, bitmap)) {
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
                s.hole_probe_timer_ms = arq_policy::holeProbeInitialTimerMs(
                    currentAckTimeoutMs());
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
            uint32_t fast_retx_cooldown_ms = arq_policy::fastRetransmitCooldownMs(
                config_.ack_timeout_ms);
            if (arq_policy::shouldFastRetransmitHole(
                    s.hole_ack_count, s.fast_retx_count, s.fast_retx_cooldown_ms)) {
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
                          tx_base_seq_, bitmap, s.fast_retx_count,
                          arq_policy::kMaxFastRetransmitsPerHole,
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
    v2::NackPayload np = v2::NackPayload::decode(frame.payload);
    uint16_t seq = np.frame_seq;
    if (seq != frame.seq) {
        seq = frame.seq;
    }

    LOG_MODEM(INFO, "SR-ARQ: NACK seq=%d missing_cw=0x%08X", seq, np.cw_bitmap);
    if (np.cw_bitmap != 0) {
        stats_.cw_nacks_received++;
    }

    if (isInTXWindow(seq)) {
        size_t slot = seqToSlot(seq);
        if (tx_window_[slot].active && !tx_window_[slot].acked) {
            if (np.cw_bitmap != 0 && sendDataRepair(slot, np.cw_bitmap)) {
                return;
            }
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
            if (ultra::phyDiagnosticsEnabled()) {
                std::ostringstream oss;
                oss << "event=arq_ack_repeat_tx"
                    << " local=" << local_call_
                    << " remote=" << remote_call_
                    << " ack_seq=" << job.base_seq
                    << " bitmap=0x" << std::hex << job.bitmap << std::dec
                    << " copy=" << job.copy_index;
                ultra::phyDiagLine(oss.str());
            }
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
            if (s.repair_cooldown_ms > 0) {
                if (elapsed_ms >= s.repair_cooldown_ms) {
                    s.repair_cooldown_ms = 0;
                } else {
                    s.repair_cooldown_ms -= elapsed_ms;
                }
            }
            if (s.repair_guard_ms > 0) {
                if (elapsed_ms >= s.repair_guard_ms) {
                    s.repair_guard_ms = 0;
                    s.repair_in_flight = false;
                } else {
                    s.repair_guard_ms -= elapsed_ms;
                }
            }

            if (s.fast_retx_cooldown_ms > 0) {
                if (elapsed_ms >= s.fast_retx_cooldown_ms) {
                    s.fast_retx_cooldown_ms = 0;
                } else {
                    s.fast_retx_cooldown_ms -= elapsed_ms;
                }
            }

            if (s.hole_probe_armed) {
                if (elapsed_ms >= s.hole_probe_timer_ms) {
                    if (s.repair_in_flight && s.repair_guard_ms > 0) {
                        LOG_MODEM(INFO,
                                  "SR-ARQ: Suppressed hole-probe full retx seq=%d while DATA_REPAIR in flight (%ums)",
                                  s.seq, s.repair_guard_ms);
                        s.hole_probe_timer_ms = s.repair_guard_ms;
                    } else if (s.hole_probe_count < arq_policy::kMaxHoleProbeRetransmits) {
                        s.hole_probe_count++;
                        s.hole_probe_timer_ms = arq_policy::holeProbeNextTimerMs(
                            currentAckTimeoutMs());
                        LOG_MODEM(INFO,
                                  "SR-ARQ: Hole-probe retransmit seq=%d (%d/%d)",
                                  s.seq, s.hole_probe_count,
                                  arq_policy::kMaxHoleProbeRetransmits);
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
                if (s.repair_in_flight && s.repair_guard_ms > 0) {
                    LOG_MODEM(INFO,
                              "SR-ARQ: Suppressed timeout full retx seq=%d while DATA_REPAIR in flight (%ums)",
                              s.seq, s.repair_guard_ms);
                    s.timeout_ms = s.repair_guard_ms;
                } else {
                    if (ultra::phyDiagnosticsEnabled()) {
                        std::ostringstream oss;
                        oss << "event=arq_timeout"
                            << " local=" << local_call_
                            << " remote=" << remote_call_
                            << " seq=" << s.seq
                            << " slot=" << slot
                            << " retry_count=" << s.retry_count
                            << " in_flight=" << tx_in_flight_
                            << " ack_timeout_ms=" << currentAckTimeoutMs();
                        ultra::phyDiagLine(oss.str());
                    }
                    stats_.timeouts++;
                    retransmitFrame(slot, RetransmitCause::TIMEOUT);
                }
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

    for (auto& slot : rx_window_) {
        if (!slot.partial || slot.received) {
            continue;
        }
        if (slot.cw_nack_cooldown_ms > 0) {
            slot.cw_nack_cooldown_ms = elapsed_ms >= slot.cw_nack_cooldown_ms
                ? 0
                : slot.cw_nack_cooldown_ms - elapsed_ms;
        }
        if (elapsed_ms >= PARTIAL_RX_TTL_MS - std::min(slot.partial_age_ms, PARTIAL_RX_TTL_MS)) {
            LOG_MODEM(WARN, "SR-ARQ: Partial DATA seq=%d expired (cw=0x%08X)",
                      slot.seq, slot.cw_bitmap);
            clearPartialRXSlot(slot);
            stats_.partial_frame_expired++;
        } else {
            slot.partial_age_ms += elapsed_ms;
        }
    }
}

uint32_t SelectiveRepeatARQ::computeRepairGuardMs(const TXSlot& slot,
                                                  size_t repair_frame_codewords) const {
    const uint32_t full_timeout_ms = std::max<uint32_t>(currentAckTimeoutMs(), 1u);
    const size_t original_cw = std::max<size_t>(
        1, slot.info_codewords.empty()
               ? static_cast<size_t>(std::max(slot.fixed_frame_codewords, 1))
               : slot.info_codewords.size());
    const size_t repair_cw = std::max<size_t>(1, repair_frame_codewords);

    const uint64_t scaled_airtime_ms =
        (static_cast<uint64_t>(full_timeout_ms) * repair_cw + original_cw - 1) / original_cw;
    const uint32_t repeat_margin_ms =
        ack_repeat_delay_ms_ * static_cast<uint32_t>(std::max(0, ack_repeat_count_ - 1));
    const uint32_t ack_margin_ms =
        std::max<uint32_t>(1000u, config_.sack_delay_ms + repeat_margin_ms);
    const uint32_t max_guard_ms = std::max(full_timeout_ms, ack_margin_ms);
    const uint64_t guard_ms = scaled_airtime_ms + ack_margin_ms;
    return static_cast<uint32_t>(std::min<uint64_t>(guard_ms, max_guard_ms));
}

bool SelectiveRepeatARQ::suppressFullRetransmitForRepair(size_t slot,
                                                         RetransmitCause cause) {
    TXSlot& s = tx_window_[slot];
    if (!s.repair_in_flight || s.repair_guard_ms == 0) {
        return false;
    }

    const char* cause_str = "unknown";
    switch (cause) {
        case RetransmitCause::TIMEOUT: cause_str = "timeout"; break;
        case RetransmitCause::FAST_HOLE: cause_str = "fast-hole"; break;
        case RetransmitCause::HOLE_PROBE: cause_str = "hole-probe"; break;
        case RetransmitCause::NACK: cause_str = "nack"; break;
    }

    LOG_MODEM(INFO,
              "SR-ARQ: Suppressed %s full retx seq=%d while DATA_REPAIR in flight (%ums)",
              cause_str, s.seq, s.repair_guard_ms);
    if (s.timeout_ms == 0 || s.timeout_ms > s.repair_guard_ms) {
        s.timeout_ms = s.repair_guard_ms;
    }
    return true;
}

void SelectiveRepeatARQ::retransmitFrame(size_t slot, RetransmitCause cause) {
    TXSlot& s = tx_window_[slot];
    if (suppressFullRetransmitForRepair(slot, cause)) {
        return;
    }

    s.repair_in_flight = false;
    s.repair_guard_ms = 0;
    s.last_repair_bitmap = 0;
    s.repair_cooldown_ms = 0;

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
        if (ultra::phyDiagnosticsEnabled()) {
            std::ostringstream oss;
            oss << "event=arq_frame_fail"
                << " local=" << local_call_
                << " remote=" << remote_call_
                << " seq=" << s.seq
                << " retries=" << s.retry_count
                << " max_retries=" << config_.max_retries;
            ultra::phyDiagLine(oss.str());
        }
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

    LOG_MODEM(INFO, "SR-ARQ: Retransmitting seq=%d (attempt %d/%d, cause=%s, cw=%d)",
              s.seq, s.retry_count + 1, config_.max_retries, cause_str,
              s.fixed_frame_codewords);
    if (ultra::phyDiagnosticsEnabled()) {
        std::ostringstream oss;
        oss << "event=arq_retx"
            << " local=" << local_call_
            << " remote=" << remote_call_
            << " seq=" << s.seq
            << " slot=" << slot
            << " attempt=" << (s.retry_count + 1)
            << " max_retries=" << config_.max_retries
            << " cause=" << cause_str
            << " cw=" << s.fixed_frame_codewords
            << " in_flight=" << tx_in_flight_
            << " timeout_ms=" << currentAckTimeoutMs();
        ultra::phyDiagLine(oss.str());
    }

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

bool SelectiveRepeatARQ::sendDataRepair(size_t slot, uint32_t missing_bitmap) {
    TXSlot& s = tx_window_[slot];
    if (s.info_codewords.empty()) {
        return false;
    }
    if (s.info_codewords.size() > v2::DataRepairFrame::MAX_REPAIR_CW) {
        return false;
    }

    uint32_t available_mask = 0;
    for (size_t i = 0; i < s.info_codewords.size() && i < 32; ++i) {
        available_mask |= (1u << i);
    }
    const uint32_t repair_bitmap = missing_bitmap & available_mask & 0xFFFFu;
    if (repair_bitmap == 0) {
        return false;
    }
    if (s.last_repair_bitmap == repair_bitmap && s.repair_cooldown_ms > 0) {
        LOG_MODEM(DEBUG, "SR-ARQ: Suppressed duplicate DATA_REPAIR seq=%d bitmap=0x%04X",
                  s.seq, static_cast<unsigned>(repair_bitmap));
        return true;
    }

    std::vector<Bytes> repair_codewords;
    repair_codewords.reserve(8);
    for (size_t i = 0; i < s.info_codewords.size() && i < 16; ++i) {
        if ((repair_bitmap & (1u << i)) != 0) {
            repair_codewords.push_back(s.info_codewords[i]);
        }
    }
    if (repair_codewords.empty()) {
        return false;
    }

    // Karn's algorithm: repair frames are retransmissions of the original seq.
    s.rtt_sample_eligible = false;
    s.retry_count++;
    if (s.retry_count >= config_.max_retries) {
        LOG_MODEM(ERROR, "SR-ARQ: Frame seq=%d failed after %d repairs/retries",
                  s.seq, config_.max_retries);
        stats_.failed++;
        s.active = false;
        tx_in_flight_--;
        if (on_send_complete_) {
            on_send_complete_(false);
        }
        advanceTXWindow();
        return true;
    }

    auto repair = v2::DataRepairFrame::make(local_call_, remote_call_, s.seq,
                                            static_cast<uint8_t>(s.info_codewords.size()),
                                            repair_bitmap, code_rate_, repair_codewords);
    Bytes repair_data = repair.serialize();
    if (repair_data.empty()) {
        return false;
    }

    stats_.retransmissions++;
    stats_.retransmissions_nack++;
    stats_.data_repairs_sent++;
    stats_.data_repair_cws_sent += static_cast<int>(repair_codewords.size());

    const uint32_t repair_guard_ms = computeRepairGuardMs(s, repair_codewords.size() + 1);
    s.timeout_ms = repair_guard_ms;
    s.last_repair_bitmap = repair_bitmap;
    s.repair_cooldown_ms = repair_guard_ms;
    s.repair_in_flight = true;
    s.repair_guard_ms = repair_guard_ms;

    LOG_MODEM(INFO,
              "SR-ARQ: DATA_REPAIR seq=%d bitmap=0x%04X repair_cw=%zu guard=%ums (attempt %d/%d)",
              s.seq, static_cast<unsigned>(repair_bitmap), repair_codewords.size(),
              repair_guard_ms, s.retry_count + 1, config_.max_retries);
    transmitData(repair_data);
    return true;
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
            clearTXSlotRepairState(tx_window_[slot]);
            tx_in_flight_--;

            if (on_send_complete_) {
                on_send_complete_(true);
            }
        }
        tx_base_seq_ = (tx_base_seq_ + 1) & 0xFFFF;
    }
}

void SelectiveRepeatARQ::advanceRXWindow() {
    const uint16_t base_before = rx_base_seq_;
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
        last_rx_frame_type_ = rx_window_[slot].type;
        if ((rx_window_[slot].flags & v2::Flags::FINAL) != 0) {
            rx_final_delivered_since_sack_ = true;
        }

        if (on_data_received_) {
            on_data_received_(rx_window_[slot].payload);
        }

        rx_window_[slot].received = false;
        clearPartialRXSlot(rx_window_[slot]);
        rx_window_[slot].payload.clear();
        rx_window_[slot].flags = 0;
        rx_window_[slot].type = v2::FrameType::DATA;
        rx_base_seq_ = (rx_base_seq_ + 1) & 0xFFFF;
    }

    if (rx_base_seq_ != base_before && on_rx_window_advanced_) {
        on_rx_window_advanced_(rx_base_seq_, config_.window_size);
    }
}

void SelectiveRepeatARQ::clearPartialRXSlot(RXSlot& slot) {
    slot.partial = false;
    slot.total_cw = 0;
    slot.cw_bitmap = 0;
    slot.partial_age_ms = 0;
    slot.last_cw_nack_bitmap = 0;
    slot.cw_nack_cooldown_ms = 0;
    slot.cw_data.clear();
}

void SelectiveRepeatARQ::clearTXSlotRepairState(TXSlot& slot) {
    slot.info_codewords.clear();
    slot.last_repair_bitmap = 0;
    slot.repair_cooldown_ms = 0;
    slot.repair_in_flight = false;
    slot.repair_guard_ms = 0;
}

bool SelectiveRepeatARQ::tryCompletePartialRXSlot(size_t slot_index) {
    RXSlot& slot = rx_window_[slot_index];
    if (!slot.partial || slot.total_cw == 0 || slot.total_cw > 32) {
        return false;
    }

    const uint32_t expected =
        slot.total_cw >= 32 ? 0xFFFFFFFFu : ((1u << slot.total_cw) - 1u);
    if ((slot.cw_bitmap & expected) != expected) {
        return false;
    }

    v2::CodewordStatus status;
    status.initForFrame(slot.total_cw);
    for (uint8_t i = 0; i < slot.total_cw; ++i) {
        status.decoded[i] = true;
        status.data[i] = slot.cw_data[i];
    }

    Bytes assembled = status.reassemble();
    auto frame = v2::DataFrame::deserialize(assembled);
    if (!frame || frame->seq != slot.seq) {
        LOG_MODEM(WARN, "SR-ARQ: Partial DATA seq=%d had all CWs but frame CRC rejected",
                  slot.seq);
        stats_.partial_frame_crc_failed++;
        const uint16_t seq = slot.seq;
        clearPartialRXSlot(slot);
        sendCwNack(seq, expected);
        return false;
    }

    stats_.partial_frames_completed++;
    LOG_MODEM(INFO, "SR-ARQ: Partial DATA seq=%d completed from CW slots", slot.seq);
    clearPartialRXSlot(slot);
    handleDataFrame(*frame);
    return true;
}

void SelectiveRepeatARQ::sendCwNack(uint16_t seq, uint32_t missing_bitmap) {
    if (missing_bitmap == 0) {
        return;
    }

    auto nack = v2::ControlFrame::makeNack(local_call_, remote_call_, seq, missing_bitmap);
    stats_.cw_nacks_sent++;
    auto data = nack.serialize();
    LOG_MODEM(INFO, "SR-ARQ: Sent CW_NACK seq=%d missing=0x%08X", seq, missing_bitmap);
    transmitData(data);
}

void SelectiveRepeatARQ::maybeSendCwNack(size_t slot_index, uint32_t missing_bitmap) {
    if (missing_bitmap == 0) {
        return;
    }
    RXSlot& slot = rx_window_[slot_index];
    if (slot.last_cw_nack_bitmap == missing_bitmap && slot.cw_nack_cooldown_ms > 0) {
        LOG_MODEM(DEBUG, "SR-ARQ: Suppressed duplicate CW_NACK seq=%d missing=0x%08X",
                  slot.seq, missing_bitmap);
        return;
    }

    sendCwNack(slot.seq, missing_bitmap);
    slot.last_cw_nack_bitmap = missing_bitmap;
    slot.cw_nack_cooldown_ms = std::max<uint32_t>(config_.sack_delay_ms, 1000u);
}

void SelectiveRepeatARQ::sendSack() {
    uint32_t bitmap = buildRXBitmap();
    uint16_t base_seq = (rx_base_seq_ - 1) & 0xFFFF;
    const bool sack_has_final = rx_final_delivered_since_sack_;
    const bool turn_requested = should_request_turn_ && should_request_turn_();

    // Use NACK with bitmap as SACK
    auto sack = v2::ControlFrame::makeNack(local_call_, remote_call_,
                                            base_seq,
                                            bitmap);
    // Override type to ACK for cumulative ack behavior
    sack.type = v2::FrameType::ACK;
    if (sack_has_final) {
        sack.flags |= v2::Flags::FINAL;
    }
    if (turn_requested) {
        sack.flags |= v2::Flags::TURN_REQUEST;
    }

    stats_.sacks_sent++;
    stats_.acks_sent++;

    auto data = sack.serialize();
    rx_final_delivered_since_sack_ = false;

    LOG_MODEM(INFO, "SR-ARQ: Sent SACK base=%d bitmap=0x%08X turn_request=%d",
              base_seq, bitmap, turn_requested ? 1 : 0);
    if (ultra::phyDiagnosticsEnabled()) {
        std::ostringstream oss;
        oss << "event=arq_ack_tx"
            << " local=" << local_call_
            << " remote=" << remote_call_
            << " ack_seq=" << base_seq
            << " bitmap=0x" << std::hex << bitmap << std::dec
            << " final=" << boolDigit(sack_has_final)
            << " turn_request=" << boolDigit(turn_requested)
            << " rx_base=" << rx_base_seq_
            << " frames_since_ack=" << frames_since_ack_
            << " repeat_count=" << ack_repeat_count_;
        ultra::phyDiagLine(oss.str());
    }

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

        const uint32_t base_delay_ms = ackRepeatDelayForCopy(copy_index);
        // Clean non-final cumulative ACK repeats are diversity copies for a
        // state after which the peer may immediately start its next data burst.
        // In half-duplex audio/radio paths, sending those copies immediately can
        // deafen the receiver to that burst. Hole-bearing SACK repeats remain
        // prompt because they are repair feedback, not just ACK diversity.
        const bool guard_half_duplex_repeat = (bitmap == 0) && !sack_has_final;
        uint32_t delay_ms = arq_policy::ackRepeatDelayWithHalfDuplexGuard(
            base_delay_ms, config_.sack_delay_ms, guard_half_duplex_repeat);
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
        if (ultra::phyDiagnosticsEnabled()) {
            std::ostringstream oss;
            oss << "event=arq_ack_repeat_schedule"
                << " local=" << local_call_
                << " remote=" << remote_call_
                << " ack_seq=" << base_seq
                << " bitmap=0x" << std::hex << bitmap << std::dec
                << " copy=" << copy_index
                << " delay_ms=" << static_cast<uint32_t>(scheduled)
                << " queue=" << ack_repeat_jobs_.size()
                << " final=" << boolDigit(sack_has_final);
            ultra::phyDiagLine(oss.str());
        }
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

    uint32_t sample_ms = arq_policy::rttSampleMs(arq_time_ms_, slot.first_tx_ms);
    slot.rtt_sample_eligible = false;
    if (!arq_policy::shouldUseRTTSample(
            true, arq_time_ms_, slot.first_tx_ms)) {
        return;
    }

    // RFC6298-style estimator (Karn-safe: retransmitted slots are marked ineligible).
    const auto rto = arq_policy::updateRTO(
        have_rtt_estimator_, srtt_ms_, rttvar_ms_, sample_ms, config_.ack_timeout_ms);
    srtt_ms_ = rto.srtt_ms;
    rttvar_ms_ = rto.rttvar_ms;
    adaptive_ack_timeout_ms_ = rto.rto_ms;
    have_rtt_estimator_ = true;

    LOG_MODEM(DEBUG, "SR-ARQ: RTT sample=%ums srtt=%.1f rttvar=%.1f rto=%ums",
              sample_ms, srtt_ms_, rttvar_ms_, adaptive_ack_timeout_ms_);
}

void SelectiveRepeatARQ::sendFrameNack(uint16_t seq) {
    auto nack = v2::ControlFrame::makeNack(local_call_, remote_call_, seq, 0);
    auto data = nack.serialize();
    LOG_MODEM(INFO, "SR-ARQ: Sent frame NACK seq=%d", seq);
    if (ultra::phyDiagnosticsEnabled()) {
        std::ostringstream oss;
        oss << "event=arq_nack_tx"
            << " local=" << local_call_
            << " remote=" << remote_call_
            << " seq=" << seq
            << " missing_cw=0x00000000";
        ultra::phyDiagLine(oss.str());
    }
    transmitData(data);
}

uint32_t SelectiveRepeatARQ::ackRepeatDelayForCopy(int copy_index) const {
    return arq_policy::ackRepeatDelayForCopy(ack_repeat_delay_ms_, copy_index);
}

int SelectiveRepeatARQ::ackRepeatJitterMs(uint16_t base_seq, uint32_t bitmap, int copy_index) const {
    return arq_policy::ackRepeatJitterMs(base_seq, bitmap, copy_index);
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
    return arq_policy::seqInWindow(seq, tx_base_seq_, config_.window_size);
}

bool SelectiveRepeatARQ::isInRXWindow(uint16_t seq) const {
    return arq_policy::seqInWindow(seq, rx_base_seq_, config_.window_size);
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
        slot.fixed_frame_codewords = fixed_frame_codewords_;
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
        clearTXSlotRepairState(slot);
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

void SelectiveRepeatARQ::clearPendingAckRepeats() {
    if (!ack_repeat_jobs_.empty()) {
        LOG_MODEM(INFO, "SR-ARQ: Cleared %zu turn-scoped ACK repeat(s)", ack_repeat_jobs_.size());
    }
    ack_repeat_jobs_.clear();
}

void SelectiveRepeatARQ::reset() {
    for (auto& slot : tx_window_) {
        slot.active = false;
        slot.acked = false;
        slot.frame_data.clear();
        slot.fixed_frame_codewords = fixed_frame_codewords_;
        slot.first_tx_ms = 0;
        slot.rtt_sample_eligible = false;
        slot.hole_ack_count = 0;
        slot.fast_retx_count = 0;
        slot.fast_retx_cooldown_ms = 0;
        slot.hole_probe_armed = false;
        slot.hole_probe_timer_ms = 0;
        slot.hole_probe_count = 0;
        clearTXSlotRepairState(slot);
    }
    tx_base_seq_ = 0;
    tx_next_seq_ = 0;
    tx_in_flight_ = 0;

    for (auto& slot : rx_window_) {
        slot.received = false;
        clearPartialRXSlot(slot);
        slot.payload.clear();
        slot.flags = 0;
        slot.type = v2::FrameType::DATA;
    }
    rx_base_seq_ = 0;

    last_rx_more_data_ = false;
    last_rx_flags_ = 0;
    last_rx_frame_type_ = v2::FrameType::DATA;
    rx_final_delivered_since_sack_ = false;

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
