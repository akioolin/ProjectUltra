#include "protocol_engine.hpp"
#include "gui/startup_trace.hpp"
#include "ultra/logging.hpp"
#include <algorithm>

namespace ultra {
namespace protocol {

ProtocolEngine::ProtocolEngine(const ConnectionConfig& config)
    : connection_(config)
{
    ultra::gui::startupTrace("ProtocolEngine", "ctor-enter");
    // Wire up Connection callbacks
    connection_.setTransmitCallback([this](const Bytes& data) {
        handleTxFrame(data);
    });

    connection_.setConnectedCallback([this]() {
        if (on_connection_changed_) {
            on_connection_changed_(ConnectionState::CONNECTED, connection_.getRemoteCallsign());
        }
    });

    connection_.setDisconnectedCallback([this](const std::string& reason) {
        if (on_connection_changed_) {
            on_connection_changed_(ConnectionState::DISCONNECTED, reason);
        }
    });

    connection_.setMessageReceivedCallback([this](const std::string& text) {
        if (on_message_received_) {
            on_message_received_(connection_.getRemoteCallsign(), text);
        }
    });

    connection_.setDataReceivedCallback([this](const Bytes& data, bool more_data) {
        if (on_data_received_) {
            on_data_received_(data, more_data);
        }
    });

    connection_.setIncomingCallCallback([this](const std::string& from) {
        if (on_incoming_call_) {
            on_incoming_call_(from);
        }
    });

    // Forward internal state changes (like PROBING → CONNECTING)
    connection_.setStateChangedCallback([this](ConnectionState state, const std::string& info) {
        if (on_connection_changed_) {
            on_connection_changed_(state, info);
        }
    });
    ultra::gui::startupTrace("ProtocolEngine", "ctor-exit");
}

void ProtocolEngine::setLocalCallsign(const std::string& call) {
#ifdef _WIN32
    ultra::gui::startupTrace("ProtocolEngine", "setLocalCallsign-enter");
    // Win10 startup hardening: avoid std::mutex path in early GUI bring-up.
    // Called from UI thread during app construction.
    connection_.setLocalCallsign(call);
    ultra::gui::startupTrace("ProtocolEngine", "setLocalCallsign-exit");
#else
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setLocalCallsign(call);
#endif
}

std::string ProtocolEngine::getLocalCallsign() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getLocalCallsign();
}

void ProtocolEngine::setAutoAccept(bool auto_accept) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setAutoAccept(auto_accept);
}

bool ProtocolEngine::getAutoAccept() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getAutoAccept();
}

void ProtocolEngine::setTxDataCallback(TxDataCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    on_tx_data_ = std::move(cb);
}

void ProtocolEngine::setMessageReceivedCallback(MessageReceivedCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    on_message_received_ = std::move(cb);
}

void ProtocolEngine::setConnectionChangedCallback(ConnectionChangedCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    on_connection_changed_ = std::move(cb);
}

void ProtocolEngine::setIncomingCallCallback(IncomingCallCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    on_incoming_call_ = std::move(cb);
}

void ProtocolEngine::setDataReceivedCallback(DataReceivedCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    on_data_received_ = std::move(cb);
}

bool ProtocolEngine::connect(const std::string& remote_call) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    bool result = connection_.connect(remote_call);
    if (result && on_connection_changed_) {
        // Connection now starts with PROBING state (fast presence check)
        on_connection_changed_(ConnectionState::PROBING, remote_call);
    }
    return result;
}

void ProtocolEngine::acceptCall() {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.acceptCall();
}

void ProtocolEngine::rejectCall() {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.rejectCall();
}

void ProtocolEngine::disconnect() {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.disconnect();
    if (on_connection_changed_) {
        on_connection_changed_(ConnectionState::DISCONNECTING, "");
    }
}

void ProtocolEngine::abortTxNow() {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.abortTxNow();
    tx_queue_.clear();
    defer_tx_ = false;
}

bool ProtocolEngine::sendMessage(const std::string& text) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.sendMessage(text);
}

bool ProtocolEngine::sendBinary(const Bytes& data) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.sendBinary(data);
}

bool ProtocolEngine::sendMessages(const std::vector<std::string>& texts) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.sendMessages(texts);
}

bool ProtocolEngine::isReadyToSend() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.isReadyToSend();
}

size_t ProtocolEngine::getTxBacklogBytes() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getTxBacklogBytes();
}

// --- File Transfer ---

bool ProtocolEngine::sendFile(const std::string& filepath) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.sendFile(filepath);
}

void ProtocolEngine::setReceiveDirectory(const std::string& dir) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setReceiveDirectory(dir);
}

void ProtocolEngine::cancelFileTransfer() {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.cancelFileTransfer();
}

bool ProtocolEngine::isFileTransferInProgress() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.isFileTransferInProgress();
}

FileTransferProgress ProtocolEngine::getFileProgress() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getFileProgress();
}

void ProtocolEngine::setFileProgressCallback(FileProgressCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setFileProgressCallback(std::move(cb));
}

void ProtocolEngine::setFileReceivedCallback(FileReceivedCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setFileReceivedCallback(std::move(cb));
}

void ProtocolEngine::setFileSentCallback(FileSentCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setFileSentCallback(std::move(cb));
}

void ProtocolEngine::onRxData(const Bytes& data) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);

    LOG_MODEM(INFO, "[%s] Protocol RX: %zu bytes from modem (buffer now %zu)",
              connection_.getLocalCallsign().c_str(), data.size(), rx_buffer_.size() + data.size());

    rx_buffer_.insert(rx_buffer_.end(), data.begin(), data.end());

    defer_tx_ = true;
    processRxBuffer();
    defer_tx_ = false;
}

void ProtocolEngine::onMCDPSKPartialFrame(const v2::PartialFrameCodewords& partial) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);

    defer_tx_ = true;
    connection_.onMCDPSKPartialFrame(partial);
    defer_tx_ = false;
}

void ProtocolEngine::processRxBuffer() {
    // Look for v2 frame magic (2 bytes: 0x554C = "UL")
    while (!rx_buffer_.empty()) {
        constexpr uint8_t magic_bytes[2] = {
            static_cast<uint8_t>((v2::MAGIC_V2 >> 8) & 0xFF),  // 'U' = 0x55
            static_cast<uint8_t>(v2::MAGIC_V2 & 0xFF)          // 'L' = 0x4C
        };

        auto it = std::search(rx_buffer_.begin(), rx_buffer_.end(),
                              std::begin(magic_bytes), std::end(magic_bytes));
        if (it == rx_buffer_.end()) {
            if (rx_buffer_.size() > 1) {
                rx_buffer_.erase(rx_buffer_.begin(), rx_buffer_.end() - 1);
            }
            return;
        }

        if (it != rx_buffer_.begin()) {
            rx_buffer_.erase(rx_buffer_.begin(), it);
        }

        // Check if we have minimum v2 frame size
        if (rx_buffer_.size() < v2::ControlFrame::SIZE) {
            return;
        }

        // Parse header to determine frame type and size
        auto header = v2::parseHeader(rx_buffer_);
        if (!header.valid) {
            LOG_MODEM(TRACE, "Protocol: Invalid v2 header, skipping 1 byte");
            rx_buffer_.erase(rx_buffer_.begin());
            continue;
        }

        // Determine frame size based on type
        size_t frame_size = 0;
        bool frame_is_control = false;
        bool frame_is_connect = false;
        if (header.type == v2::FrameType::DISCONNECT) {
            // Prefer modern control-frame DISCONNECT (20B) but accept legacy
            // ConnectFrame DISCONNECT (44B) for compatibility.
            constexpr size_t LEGACY_CONNECT_DISCONNECT_SIZE =
                v2::DataFrame::HEADER_SIZE + v2::ConnectFrame::PAYLOAD_SIZE + v2::DataFrame::CRC_SIZE;

            frame_size = v2::ControlFrame::SIZE;
            frame_is_control = true;

            if (rx_buffer_.size() >= frame_size) {
                Bytes ctrl_candidate(rx_buffer_.begin(), rx_buffer_.begin() + frame_size);
                if (!v2::ControlFrame::deserialize(ctrl_candidate)) {
                    if (rx_buffer_.size() < LEGACY_CONNECT_DISCONNECT_SIZE) {
                        return;  // Might be partial legacy frame, wait for more bytes.
                    }

                    Bytes conn_candidate(rx_buffer_.begin(),
                                         rx_buffer_.begin() + LEGACY_CONNECT_DISCONNECT_SIZE);
                    if (v2::ConnectFrame::deserialize(conn_candidate)) {
                        frame_size = LEGACY_CONNECT_DISCONNECT_SIZE;
                        frame_is_control = false;
                        frame_is_connect = true;
                    } else {
                        LOG_MODEM(WARN, "Protocol: Invalid DISCONNECT framing, skipping 1 byte");
                        rx_buffer_.erase(rx_buffer_.begin());
                        continue;
                    }
                }
            }
        } else if (v2::isControlFrame(header.type)) {
            // Control frames (PROBE, ACK, etc): 20 bytes
            frame_size = v2::ControlFrame::SIZE;
            frame_is_control = true;
        } else if (v2::isConnectFrame(header.type)) {
            // Connect frames: header + 25B payload + 2B CRC = 44 bytes
            frame_size = v2::DataFrame::HEADER_SIZE + v2::ConnectFrame::PAYLOAD_SIZE + v2::DataFrame::CRC_SIZE;
            frame_is_connect = true;
        } else if (header.type == v2::FrameType::DATA_REPAIR) {
            auto repair_header = v2::DataRepairFrame::parseHeader(rx_buffer_);
            if (!repair_header) {
                LOG_MODEM(WARN, "Protocol: Invalid DATA_REPAIR header, skipping 1 byte");
                rx_buffer_.erase(rx_buffer_.begin());
                continue;
            }
            frame_size = static_cast<size_t>(repair_header->repair_count + 1) *
                         v2::getBytesPerCodeword(repair_header->rate);
        } else {
            // Data frame - need to read payload length from header
            // Header layout: [0-1] magic, [2] type, [3] flags, [4-5] seq,
            //                [6-8] src_hash, [9-11] dst_hash, [12] total_cw,
            //                [13-14] payload_len, [15-16] header_crc
            if (rx_buffer_.size() < v2::DataFrame::HEADER_SIZE) {
                return;  // Need more data
            }
            uint16_t payload_len = (static_cast<uint16_t>(rx_buffer_[13]) << 8) | rx_buffer_[14];
            frame_size = v2::DataFrame::HEADER_SIZE + payload_len + v2::DataFrame::CRC_SIZE;
        }

        if (rx_buffer_.size() < frame_size) {
            return;  // Need more data
        }

        // Extract frame bytes
        Bytes frame_data(rx_buffer_.begin(), rx_buffer_.begin() + frame_size);

        // Verify CRC
        bool crc_ok = false;
        if (frame_is_control) {
            auto ctrl = v2::ControlFrame::deserialize(frame_data);
            crc_ok = ctrl.has_value();
        } else if (frame_is_connect) {
            auto conn = v2::ConnectFrame::deserialize(frame_data);
            crc_ok = conn.has_value();
        } else if (header.type == v2::FrameType::DATA_REPAIR) {
            auto repair = v2::DataRepairFrame::deserialize(frame_data);
            crc_ok = repair.has_value();
        } else {
            auto data_frame = v2::DataFrame::deserialize(frame_data);
            crc_ok = data_frame.has_value();
        }

        if (crc_ok) {
            const std::string local_call = connection_.getLocalCallsign();
            if (!v2::isAddressedToCallsign(header, local_call)) {
                LOG_MODEM(TRACE,
                          "[%s] RX << %s seq=%d for different station, dropping",
                          local_call.c_str(),
                          v2::frameTypeToString(header.type),
                          header.seq);
                rx_buffer_.erase(rx_buffer_.begin(), rx_buffer_.begin() + frame_size);
                continue;
            }

            LOG_MODEM(INFO, "[%s] RX << %s seq=%d (%zu bytes)",
                      local_call.c_str(),
                      v2::frameTypeToString(header.type), header.seq, frame_size);
            rx_buffer_.erase(rx_buffer_.begin(), rx_buffer_.begin() + frame_size);
            connection_.onFrameReceived(frame_data);
        } else {
            LOG_MODEM(WARN, "Protocol: CRC failed, skipping frame");
            rx_buffer_.erase(rx_buffer_.begin(), rx_buffer_.begin() + frame_size);
        }
    }
}

void ProtocolEngine::tick(uint32_t elapsed_ms) {
    std::vector<Bytes> to_send;
    {
        std::lock_guard<ProtocolEngineMutex> lock(mutex_);
        to_send = std::move(tx_queue_);
        tx_queue_.clear();
    }

    for (const auto& tx_data : to_send) {
        if (on_tx_data_) {
            on_tx_data_(tx_data);
        }
    }

    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.tick(elapsed_ms);
}

ConnectionState ProtocolEngine::getState() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getState();
}

std::string ProtocolEngine::getRemoteCallsign() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getRemoteCallsign();
}

bool ProtocolEngine::isConnected() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.isConnected();
}

ConnectionStats ProtocolEngine::getStats() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getStats();
}

void ProtocolEngine::resetStats() {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.resetStats();
}

void ProtocolEngine::reset() {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.reset();
    rx_buffer_.clear();
    tx_queue_.clear();
    defer_tx_ = false;
}

void ProtocolEngine::handleTxFrame(const Bytes& frame_data) {
    LOG_MODEM(INFO, "[%s] Protocol TX: %zu bytes -> modem%s",
              connection_.getLocalCallsign().c_str(), frame_data.size(),
              defer_tx_ ? " (queued)" : "");

    if (defer_tx_) {
        tx_queue_.push_back(frame_data);
    } else if (on_tx_data_) {
        on_tx_data_(frame_data);
    }
}

// --- Waveform Mode ---

WaveformMode ProtocolEngine::getNegotiatedMode() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getNegotiatedMode();
}

void ProtocolEngine::setPreferredMode(WaveformMode mode) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setPreferredMode(mode);
}

void ProtocolEngine::setModeCapabilities(uint8_t caps) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setModeCapabilities(caps);
}

bool ProtocolEngine::isPhyMaskV1Negotiated() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.isPhyMaskV1Negotiated();
}

void ProtocolEngine::setNarrowbandOverride(WaveformMode mode) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setNarrowbandOverride(mode);
}

void ProtocolEngine::setForcedModulation(Modulation mod) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setForcedModulation(mod);
}

void ProtocolEngine::setForcedCodeRate(CodeRate rate) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setForcedCodeRate(rate);
}

void ProtocolEngine::setMCDPSKConfig(int num_carriers, int samples_per_symbol) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setMCDPSKConfig(num_carriers, samples_per_symbol);
}

void ProtocolEngine::setForcedFrameCodewords(int cw_count, bool forced) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setForcedFrameCodewords(cw_count, forced);
}

Modulation ProtocolEngine::getForcedModulation() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getForcedModulation();
}

CodeRate ProtocolEngine::getForcedCodeRate() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getForcedCodeRate();
}

int ProtocolEngine::getForcedFrameCodewords() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getForcedFrameCodewords();
}

void ProtocolEngine::setSoftCombiningHARQ(bool enable) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setSoftCombiningHARQ(enable);
}

fec::SoftCombineBuffer* ProtocolEngine::softCombineBuffer() {
    return connection_.softCombineBuffer();
}

void ProtocolEngine::setModeNegotiatedCallback(ModeNegotiatedCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setModeNegotiatedCallback(std::move(cb));
}

void ProtocolEngine::setConnectWaveformChangedCallback(ConnectWaveformChangedCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setConnectWaveformChangedCallback(std::move(cb));
}

void ProtocolEngine::setPhyMaskV1NegotiatedCallback(PhyMaskV1NegotiatedCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setPhyMaskV1NegotiatedCallback(std::move(cb));
}

void ProtocolEngine::setHandshakeConfirmedCallback(HandshakeConfirmedCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setHandshakeConfirmedCallback(std::move(cb));
}

WaveformMode ProtocolEngine::getConnectWaveform() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getConnectWaveform();
}

void ProtocolEngine::setInitialConnectWaveform(WaveformMode mode) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setInitialConnectWaveform(mode);
}

// --- Adaptive Data Mode ---

void ProtocolEngine::setMeasuredSNR(float snr_db) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setMeasuredSNR(snr_db);
}

float ProtocolEngine::getMeasuredSNR() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getMeasuredSNR();
}

void ProtocolEngine::setChannelQuality(float snr_db, float fading_index) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setChannelQuality(snr_db, fading_index);
}

float ProtocolEngine::getFadingIndex() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getFadingIndex();
}

Modulation ProtocolEngine::getDataModulation() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getDataModulation();
}

CodeRate ProtocolEngine::getDataCodeRate() const {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    return connection_.getDataCodeRate();
}

void ProtocolEngine::setDataModeChangedCallback(DataModeChangedCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setDataModeChangedCallback(std::move(cb));
}

void ProtocolEngine::setTransmitBurstCallback(TransmitBurstCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setTransmitBurstCallback(std::move(cb));
}

void ProtocolEngine::setPingTxCallback(PingTxCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setPingTxCallback(std::move(cb));
}

void ProtocolEngine::setPingReceivedCallback(PingReceivedCallback cb) {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.setPingReceivedCallback(std::move(cb));
}

void ProtocolEngine::onPingReceived() {
    std::lock_guard<ProtocolEngineMutex> lock(mutex_);
    connection_.onPongReceived();
}

} // namespace protocol
} // namespace ultra
