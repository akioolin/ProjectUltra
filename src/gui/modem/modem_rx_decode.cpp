// ModemEngine RX Decode Implementation
// Frame delivery and notification helpers

#include "modem_engine.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/logging.hpp"

namespace ultra {
namespace gui {

namespace v2 = protocol::v2;

// ============================================================================
// FRAME DELIVERY AND NOTIFICATION
// ============================================================================

void ModemEngine::deliverFrame(const Bytes& frame_data) {
    // Prefer the LIVE local callsign (tracks runtime MYCALL changes); fall back to the logging
    // label only when it was never set (preserves prior GUI behavior where label == callsign).
    // Using the stale label here silently dropped inbound frames addressed to the operator's
    // current callsign when it diverged from the config label (BUG-CALLSIGN-FILTER).
    const std::string local_call =
        !filter_callsign_.empty()
            ? filter_callsign_
            : ((log_prefix_.empty() || log_prefix_ == "MODEM") ? std::string{} : log_prefix_);
    auto header = v2::parseHeader(frame_data);
    if (header.valid && !v2::isAddressedToCallsign(header, local_call)) {
        LOG_MODEM(TRACE, "[%s] deliverFrame: dropping frame for different station",
                  log_prefix_.c_str());
        return;
    }

    LOG_MODEM(INFO, "[%s] deliverFrame: %zu bytes, callback=%s",
              log_prefix_.c_str(), frame_data.size(),
              raw_data_callback_ ? "set" : "NOT SET");

    updateStats([](LoopbackStats& s) {
        s.frames_received++;
        s.synced = true;
    });

    {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        rx_data_queue_.push(frame_data);
    }

    if (raw_data_callback_) {
        raw_data_callback_(frame_data);
    }

    last_rx_complete_time_ = std::chrono::steady_clock::now();
}

void ModemEngine::notifyFrameParsed(const Bytes& frame_data, protocol::v2::FrameType frame_type) {
    if (!status_callback_) return;

    std::string type_str;
    switch (frame_type) {
        case v2::FrameType::PROBE: type_str = "PROBE"; break;
        case v2::FrameType::PROBE_ACK: type_str = "PROBE_ACK"; break;
        case v2::FrameType::CONNECT: type_str = "CONNECT"; break;
        case v2::FrameType::CONNECT_ACK: type_str = "CONNECT_ACK"; break;
        case v2::FrameType::DISCONNECT: type_str = "DISCONNECT"; break;
        case v2::FrameType::DATA: type_str = "DATA"; break;
        case v2::FrameType::ACK: type_str = "ACK"; break;
        case v2::FrameType::NACK: type_str = "NACK"; break;
        default: type_str = "FRAME"; break;
    }

    // Try parsing for more details
    auto connect = v2::ConnectFrame::deserialize(frame_data);
    if (connect) {
        std::string src = connect->getSrcCallsign();
        std::string dst = connect->getDstCallsign();

        if (src.empty()) {
            src = "UNKNOWN";
        }
        if (dst.empty()) {
            // Hash-addressed CONNECT_ACK/NAK may not carry dst callsign.
            // Show local station prefix instead of a blank target in UI.
            if (!log_prefix_.empty() && log_prefix_ != "MODEM") {
                dst = log_prefix_;
            } else {
                dst = "UNKNOWN";
            }
        }

        status_callback_("[" + type_str + "] " + src + " -> " + dst);
        return;
    }

    auto data = v2::DataFrame::deserialize(frame_data);
    if (data && !data->payload.empty()) {
        std::string msg = data->payloadAsText();
        // data_callback_ carries the raw payload onward (protocol / file
        // assembly) for every non-empty frame — unchanged.
        if (!msg.empty() && data_callback_) {
            data_callback_(msg);
        }
        return;
    }

    if (frame_type == v2::FrameType::ACK ||
        frame_type == v2::FrameType::NACK ||
        type_str == "FRAME") {
        return;
    }

    status_callback_("[" + type_str + "] Received");
}

void ModemEngine::updateStats(std::function<void(LoopbackStats&)> updater) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    updater(stats_);
}

} // namespace gui
} // namespace ultra
