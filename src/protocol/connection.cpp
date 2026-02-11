// Connection state machine - core logic
// Frame handlers are in connection_handlers.cpp

#include "connection.hpp"
#include "waveform_selection.hpp"
#include "ultra/logging.hpp"
#include <algorithm>

namespace ultra {
namespace protocol {

namespace {

constexpr uint32_t OFDM_SAMPLE_RATE = 48000;
constexpr uint32_t OFDM_FFT_SIZE = 1024;
constexpr uint32_t OFDM_LONG_CP_SAMPLES = 128;  // LONG CP at 1024 FFT
constexpr uint32_t OFDM_SYMBOL_SAMPLES = OFDM_FFT_SIZE + OFDM_LONG_CP_SAMPLES;
constexpr uint32_t OFDM_NUM_CARRIERS = 59;
constexpr uint32_t LDPC_BITS_PER_CODEWORD = 648;
constexpr uint32_t FIXED_FRAME_CODEWORDS = 4;

bool isCoherentModulation(Modulation mod) {
    switch (mod) {
        case Modulation::BPSK:
        case Modulation::QPSK:
        case Modulation::QAM8:
        case Modulation::QAM16:
        case Modulation::QAM32:
        case Modulation::QAM64:
        case Modulation::QAM256:
            return true;
        default:
            return false;
    }
}

int pilotSpacingForRate(Modulation mod, CodeRate rate) {
    bool coherent = isCoherentModulation(mod);
    switch (rate) {
        case CodeRate::R3_4:
            return coherent ? 8 : 15;
        case CodeRate::R2_3:
        case CodeRate::R1_2:
            return coherent ? 5 : 10;
        case CodeRate::R1_4:
        case CodeRate::R1_3:
        default:
            return coherent ? 5 : 10;
    }
}

uint32_t symbolsForCodewords(Modulation mod, CodeRate rate, int codewords) {
    int pilot_spacing = pilotSpacingForRate(mod, rate);
    int pilot_count = (OFDM_NUM_CARRIERS + pilot_spacing - 1) / pilot_spacing;
    int data_carriers = std::max(1, static_cast<int>(OFDM_NUM_CARRIERS) - pilot_count);
    uint32_t bits_per_symbol = static_cast<uint32_t>(data_carriers) * getBitsPerSymbol(mod);
    uint32_t frame_bits = static_cast<uint32_t>(codewords) * LDPC_BITS_PER_CODEWORD;
    uint32_t data_symbols = (frame_bits + bits_per_symbol - 1) / bits_per_symbol;

    // 2 training symbols in light preamble + data symbols.
    return 2 + data_symbols;
}

uint32_t computeOfdmAckTimeoutMs(Modulation mod, CodeRate rate, size_t window_size,
                                 uint32_t sack_delay_ms, int ack_repeat_count) {
    constexpr float symbol_ms = (1000.0f * OFDM_SYMBOL_SAMPLES) / OFDM_SAMPLE_RATE;  // 24ms

    uint32_t data_symbols = symbolsForCodewords(mod, rate, FIXED_FRAME_CODEWORDS);
    uint32_t ack_symbols = symbolsForCodewords(mod, rate, 1);
    uint32_t data_frame_ms = static_cast<uint32_t>(data_symbols * symbol_ms + 0.5f);
    uint32_t ack_frame_ms = static_cast<uint32_t>(ack_symbols * symbol_ms + 0.5f);

    uint32_t ack_copies = static_cast<uint32_t>(std::clamp(ack_repeat_count, 1, 3));
    uint32_t tx_burst_ms = static_cast<uint32_t>(window_size) * data_frame_ms;
    uint32_t ack_path_ms = ack_copies * ack_frame_ms + sack_delay_ms;
    uint32_t decode_jitter_margin_ms = std::max<uint32_t>(700, data_frame_ms / 2);

    // Timeout should cover a full burst RTT + decode/jitter margin.
    uint32_t timeout_ms = tx_burst_ms + ack_path_ms + decode_jitter_margin_ms;
    return std::clamp(timeout_ms, 4500u, 14000u);
}

} // namespace

const char* connectionStateToString(ConnectionState state) {
    switch (state) {
        case ConnectionState::DISCONNECTED:  return "DISCONNECTED";
        case ConnectionState::PROBING:       return "PROBING";
        case ConnectionState::CONNECTING:    return "CONNECTING";
        case ConnectionState::CONNECTED:     return "CONNECTED";
        case ConnectionState::DISCONNECTING: return "DISCONNECTING";
        default: return "UNKNOWN";
    }
}

// =============================================================================
// CONSTRUCTOR
// =============================================================================

Connection::Connection(const ConnectionConfig& config)
    : config_(config)
    , arq_(config.arq)
{
    // Wire up ARQ callbacks
    arq_.setTransmitCallback([this](const Bytes& data) {
        transmitFrame(data);
    });

    arq_.setDataReceivedCallback([this](const Bytes& data) {
        handleDataPayload(data, arq_.lastRxHadMoreData());
    });

    arq_.setSendCompleteCallback([this](bool success) {
        if (file_transfer_.getState() == FileTransferState::SENDING) {
            if (success) {
                file_transfer_.onChunkAcked();
                sendNextFileChunk();
            } else {
                file_transfer_.onSendFailed();
            }
        } else if (!pending_tx_fragments_.empty()) {
            if (!success) {
                // Fragment send failed - abort remaining fragments
                LOG_MODEM(WARN, "Connection: Fragment send failed, aborting remaining %zu fragments",
                          pending_tx_fragments_.size() - next_fragment_idx_);
                pending_tx_fragments_.clear();
                pending_tx_fragment_flags_.clear();
                next_fragment_idx_ = 0;
                acked_fragment_count_ = 0;
                if (on_message_sent_) {
                    on_message_sent_(false);
                }
            } else {
                acked_fragment_count_++;

                if (next_fragment_idx_ < pending_tx_fragments_.size()) {
                    // More fragments to submit to ARQ
                    sendNextFragment();
                }

                if (acked_fragment_count_ >= pending_tx_fragments_.size()) {
                    // ALL fragments truly ACKed
                    LOG_MODEM(INFO, "Connection: All %zu fragments sent and ACKed",
                              pending_tx_fragments_.size());
                    pending_tx_fragments_.clear();
                    pending_tx_fragment_flags_.clear();
                    next_fragment_idx_ = 0;
                    acked_fragment_count_ = 0;
                    if (on_message_sent_) {
                        on_message_sent_(true);
                    }
                }
            }
        } else {
            if (on_message_sent_) {
                on_message_sent_(success);
            }
        }
    });
}

// =============================================================================
// CONFIGURATION
// =============================================================================

void Connection::setLocalCallsign(const std::string& call) {
    local_call_ = sanitizeCallsign(call);
    LOG_MODEM(INFO, "Connection: Local callsign set to %s", local_call_.c_str());
}

// =============================================================================
// CONNECTION CONTROL
// =============================================================================

bool Connection::connect(const std::string& remote_call) {
    if (state_ != ConnectionState::DISCONNECTED) {
        LOG_MODEM(WARN, "Connection: Cannot connect, state=%s",
                  connectionStateToString(state_));
        return false;
    }

    if (local_call_.empty()) {
        LOG_MODEM(ERROR, "Connection: Local callsign not set");
        return false;
    }

    remote_call_ = sanitizeCallsign(remote_call);
    if (remote_call_.empty() || !isValidCallsign(remote_call_)) {
        LOG_MODEM(ERROR, "Connection: Invalid remote callsign: %s", remote_call.c_str());
        return false;
    }

    LOG_MODEM(INFO, "Connection: Connecting to %s (starting with PING probe)", remote_call_.c_str());

    // Use current connect_waveform_ (can be pre-set via setInitialConnectWaveform)
    // Notify the modem of the waveform to use
    if (on_connect_waveform_changed_) {
        on_connect_waveform_changed_(connect_waveform_);
    }

    // Start with PROBING state - send PING for fast presence check
    state_ = ConnectionState::PROBING;
    ping_retry_count_ = 0;
    timeout_remaining_ms_ = PING_TIMEOUT_MS;
    stats_.connects_initiated++;

    // Send PING (modem will generate preamble + "ULTR")
    if (on_ping_tx_) {
        LOG_MODEM(INFO, "Connection: Sending PING via %s",
                  waveformModeToString(connect_waveform_));
        on_ping_tx_();
    } else {
        // Fallback: no ping callback, send CONNECT directly
        LOG_MODEM(WARN, "Connection: No ping callback, sending CONNECT directly");
        sendFullConnect();
    }

    return true;
}

void Connection::acceptCall() {
    if (state_ != ConnectionState::DISCONNECTED || pending_remote_call_.empty()) {
        LOG_MODEM(WARN, "Connection: No pending call to accept");
        return;
    }

    remote_call_ = pending_remote_call_;
    pending_remote_call_.clear();

    negotiated_mode_ = negotiateMode(remote_capabilities_, remote_preferred_);

    // Check if initiator forced specific modes (0xFF = AUTO, else forced)
    Modulation rec_mod;
    CodeRate rec_rate;

    // Use centralized algorithm from waveform_selection.hpp
    recommendDataMode(measured_snr_db_, negotiated_mode_, rec_mod, rec_rate, fading_index_);

    if (pending_forced_modulation_ != Modulation::AUTO) {
        // Initiator forced a specific modulation - honor it
        rec_mod = pending_forced_modulation_;
        LOG_MODEM(INFO, "Connection: Using FORCED modulation %s from initiator",
                  modulationToString(rec_mod));
    }

    if (pending_forced_code_rate_ != CodeRate::AUTO) {
        // Initiator forced a specific code rate - honor it
        rec_rate = pending_forced_code_rate_;
        LOG_MODEM(INFO, "Connection: Using FORCED code rate %s from initiator",
                  codeRateToString(rec_rate));
    }

    // Clear pending forced modes
    pending_forced_modulation_ = Modulation::AUTO;
    pending_forced_code_rate_ = CodeRate::AUTO;

    // Set our local data mode immediately
    data_modulation_ = rec_mod;
    data_code_rate_ = rec_rate;

    LOG_MODEM(INFO, "Connection: Accepting call from %s (waveform=%s, data=%s %s)",
              remote_call_.c_str(), waveformModeToString(negotiated_mode_),
              modulationToString(data_modulation_), codeRateToString(data_code_rate_));

    auto ack = v2::ConnectFrame::makeConnectAck(local_call_, remote_call_,
                                                 static_cast<uint8_t>(negotiated_mode_),
                                                 rec_mod, rec_rate, measured_snr_db_);
    Bytes ack_data = ack.serialize();

    LOG_MODEM(INFO, "Connection: Sending CONNECT_ACK (%zu bytes)", ack_data.size());
    transmitFrame(ack_data);

    enterConnected();

    // We are the responder - we received CONNECT and are sending CONNECT_ACK
    is_initiator_ = false;
    handshake_confirmed_ = false;  // Responder waits for first frame to confirm

    // Notify application of initial data mode
    if (on_data_mode_changed_) {
        on_data_mode_changed_(data_modulation_, data_code_rate_, measured_snr_db_);
    }
}

void Connection::rejectCall() {
    if (pending_remote_call_.empty()) {
        return;
    }

    LOG_MODEM(INFO, "Connection: Rejecting call from %s", pending_remote_call_.c_str());

    auto nak = v2::ConnectFrame::makeConnectNak(local_call_, pending_remote_call_);
    Bytes nak_data = nak.serialize();

    LOG_MODEM(INFO, "Connection: Sending CONNECT_NAK (%zu bytes)", nak_data.size());
    transmitFrame(nak_data);

    pending_remote_call_.clear();
}

void Connection::disconnect() {
    if (state_ == ConnectionState::DISCONNECTED) {
        return;
    }

    if (state_ == ConnectionState::CONNECTING) {
        enterDisconnected("Cancelled");
        return;
    }

    if (state_ == ConnectionState::CONNECTED) {
        LOG_MODEM(INFO, "Connection: Disconnecting from %s", remote_call_.c_str());

        auto disc = v2::ConnectFrame::makeDisconnect(local_call_, remote_call_);
        disconnect_frame_ = disc.serialize();

        LOG_MODEM(INFO, "Connection: Sending DISCONNECT (%zu bytes)", disconnect_frame_.size());
        transmitFrame(disconnect_frame_);

        state_ = ConnectionState::DISCONNECTING;
        timeout_remaining_ms_ = config_.disconnect_timeout_ms;
        disconnect_retry_count_ = 0;
        disconnect_retransmit_ms_ = DISCONNECT_RETRANSMIT_INTERVAL_MS;
        stats_.disconnects++;
    }
}

// =============================================================================
// DATA TRANSFER
// =============================================================================

bool Connection::sendMessage(const std::string& text) {
    if (state_ != ConnectionState::CONNECTED) {
        LOG_MODEM(WARN, "Connection: Cannot send, not connected");
        return false;
    }

    // MC-DPSK uses variable-length codewords (no fixed frame limit), so no fragmentation needed.
    // OFDM uses fixed 4-CW frames with a hard capacity limit that requires fragmentation.
    bool is_ofdm = (negotiated_mode_ == WaveformMode::OFDM_CHIRP ||
                    negotiated_mode_ == WaveformMode::OFDM_COX);

    Bytes data(text.begin(), text.end());

    if (!is_ofdm || data.size() <= v2::getFixedFramePayloadCapacity(data_code_rate_)) {
        // Single frame - MC-DPSK can handle any size, OFDM fits in one frame
        return arq_.sendData(data);
    }

    size_t capacity = v2::getFixedFramePayloadCapacity(data_code_rate_);

    // Fragment the message into chunks that fit in one frame each
    LOG_MODEM(INFO, "Connection: Fragmenting %zu byte message into %zu-byte chunks",
              data.size(), capacity);

    pending_tx_fragments_.clear();
    pending_tx_fragment_flags_.clear();
    next_fragment_idx_ = 0;
    acked_fragment_count_ = 0;

    for (size_t offset = 0; offset < data.size(); offset += capacity) {
        size_t chunk_size = std::min(capacity, data.size() - offset);
        pending_tx_fragments_.emplace_back(data.begin() + offset, data.begin() + offset + chunk_size);
    }

    LOG_MODEM(INFO, "Connection: Split into %zu fragments", pending_tx_fragments_.size());

    sendNextFragment();
    return true;
}

bool Connection::sendMessages(const std::vector<std::string>& texts) {
    if (state_ != ConnectionState::CONNECTED) {
        LOG_MODEM(WARN, "Connection: Cannot send, not connected");
        return false;
    }

    bool is_ofdm = (negotiated_mode_ == WaveformMode::OFDM_CHIRP ||
                    negotiated_mode_ == WaveformMode::OFDM_COX);
    size_t capacity = is_ofdm ? v2::getFixedFramePayloadCapacity(data_code_rate_) : SIZE_MAX;

    // Pre-fragment all messages into a flat list of frame payloads with flags
    pending_tx_fragments_.clear();
    pending_tx_fragment_flags_.clear();
    next_fragment_idx_ = 0;
    acked_fragment_count_ = 0;

    for (const auto& text : texts) {
        Bytes data(text.begin(), text.end());

        if (data.size() <= capacity) {
            // Single frame — no MORE_FRAG
            pending_tx_fragments_.push_back(data);
            pending_tx_fragment_flags_.push_back(v2::Flags::NONE);
        } else {
            // Fragment this message
            for (size_t offset = 0; offset < data.size(); offset += capacity) {
                size_t chunk_size = std::min(capacity, data.size() - offset);
                Bytes chunk(data.begin() + offset, data.begin() + offset + chunk_size);
                bool is_last = (offset + chunk_size >= data.size());
                pending_tx_fragments_.push_back(chunk);
                pending_tx_fragment_flags_.push_back(
                    is_last ? v2::Flags::NONE : v2::Flags::MORE_FRAG);
            }
        }
    }

    LOG_MODEM(INFO, "Connection: Batch queued %zu messages as %zu frames",
              texts.size(), pending_tx_fragments_.size());

    // Send first window-worth via sendNextFragment() (handles burst buffering)
    sendNextFragment();
    return !pending_tx_fragments_.empty();
}

bool Connection::isReadyToSend() const {
    return state_ == ConnectionState::CONNECTED && arq_.isReadyToSend() &&
           !file_transfer_.isBusy();
}

// =============================================================================
// FILE TRANSFER
// =============================================================================

bool Connection::sendFile(const std::string& filepath) {
    if (state_ != ConnectionState::CONNECTED) {
        LOG_MODEM(WARN, "Connection: Cannot send file, not connected");
        return false;
    }

    if (file_transfer_.isBusy()) {
        LOG_MODEM(WARN, "Connection: File transfer already in progress");
        return false;
    }

    if (!arq_.isReadyToSend()) {
        LOG_MODEM(WARN, "Connection: ARQ busy, cannot start file transfer");
        return false;
    }

    // Set chunk size to match frame capacity for current mode
    bool is_ofdm = (negotiated_mode_ == WaveformMode::OFDM_CHIRP ||
                    negotiated_mode_ == WaveformMode::OFDM_COX);
    if (is_ofdm) {
        size_t capacity = v2::getFixedFramePayloadCapacity(data_code_rate_);
        file_transfer_.setMaxChunkPayload(capacity);
        LOG_MODEM(INFO, "Connection: File chunk payload limited to %zu bytes (OFDM %s)",
                  capacity, codeRateToString(data_code_rate_));
    }

    LOG_MODEM(INFO, "Connection: Starting file transfer: %s", filepath.c_str());

    if (!file_transfer_.startSend(filepath)) {
        LOG_MODEM(ERROR, "Connection: Failed to start file transfer");
        return false;
    }

    sendNextFileChunk();
    return true;
}

void Connection::setReceiveDirectory(const std::string& dir) {
    file_transfer_.setReceiveDirectory(dir);
}

void Connection::cancelFileTransfer() {
    file_transfer_.cancel();
}

bool Connection::isFileTransferInProgress() const {
    return file_transfer_.isBusy();
}

FileTransferProgress Connection::getFileProgress() const {
    return file_transfer_.getProgress();
}

void Connection::sendNextFileChunk() {
    if (file_transfer_.getState() != FileTransferState::SENDING) {
        return;
    }

    bool is_ofdm = (negotiated_mode_ == WaveformMode::OFDM_CHIRP ||
                    negotiated_mode_ == WaveformMode::OFDM_COX);

    // Enable burst buffering for OFDM mode
    if (is_ofdm && on_transmit_burst_) {
        burst_mode_active_ = true;
        burst_tx_buffer_.clear();
    }

    // Fill the ARQ window with as many chunks as possible
    // Selective Repeat ARQ can have multiple frames in flight
    while (arq_.isReadyToSend() && file_transfer_.hasMoreChunks()) {
        Bytes chunk = file_transfer_.getNextChunk();
        if (chunk.empty()) {
            break;
        }

        // MORE_FRAG indicates more data remaining in file (not burst)
        uint8_t flags = file_transfer_.hasMoreChunks() ? v2::Flags::MORE_FRAG : v2::Flags::NONE;
        arq_.sendDataWithFlags(chunk, flags);
    }

    // Flush burst buffer
    if (is_ofdm && on_transmit_burst_) {
        burst_mode_active_ = false;
        flushBurstBuffer();
    }
}

void Connection::sendNextFragment() {
    bool is_ofdm = (negotiated_mode_ == WaveformMode::OFDM_CHIRP ||
                    negotiated_mode_ == WaveformMode::OFDM_COX);

    // Enable burst buffering for OFDM mode
    if (is_ofdm && on_transmit_burst_) {
        burst_mode_active_ = true;
        burst_tx_buffer_.clear();
    }

    while (arq_.isReadyToSend() && next_fragment_idx_ < pending_tx_fragments_.size()) {
        const Bytes& chunk = pending_tx_fragments_[next_fragment_idx_];

        // Use pre-computed flags if available (from sendMessages batch),
        // otherwise derive from position (single-message fragmentation)
        uint8_t flags;
        if (next_fragment_idx_ < pending_tx_fragment_flags_.size()) {
            flags = pending_tx_fragment_flags_[next_fragment_idx_];
        } else {
            bool is_last = (next_fragment_idx_ + 1 == pending_tx_fragments_.size());
            flags = is_last ? v2::Flags::NONE : v2::Flags::MORE_FRAG;
        }

        LOG_MODEM(DEBUG, "Connection: Sending fragment %zu/%zu (%zu bytes, flags=0x%02X)",
                  next_fragment_idx_ + 1, pending_tx_fragments_.size(), chunk.size(), flags);

        arq_.sendDataWithFlags(chunk, flags);
        next_fragment_idx_++;
    }

    // Flush burst buffer
    if (is_ofdm && on_transmit_burst_) {
        burst_mode_active_ = false;
        flushBurstBuffer();
    }
}

// =============================================================================
// FRAME DISPATCHING
// =============================================================================

void Connection::onFrameReceived(const Bytes& frame_data) {
    if (frame_data.size() < 2) {
        return;
    }

    // Responder handshake confirmation: when we receive first frame after CONNECT_ACK
    // This means the initiator got our CONNECT_ACK and the handshake is complete
    if (state_ == ConnectionState::CONNECTED && !is_initiator_ && !handshake_confirmed_) {
        LOG_MODEM(INFO, "Connection: Handshake confirmed (received first frame from initiator)");
        handshake_confirmed_ = true;
        if (on_handshake_confirmed_) {
            on_handshake_confirmed_();
        }
        // NOTE: Initial data mode is now carried in CONNECT_ACK, no separate MODE_CHANGE needed
    }

    // Check v2 magic
    uint16_t magic = (static_cast<uint16_t>(frame_data[0]) << 8) | frame_data[1];
    if (magic != v2::MAGIC_V2) {
        LOG_MODEM(TRACE, "Connection: Ignoring frame with wrong magic");
        return;
    }

    auto header = v2::parseHeader(frame_data);
    if (!header.valid) {
        LOG_MODEM(TRACE, "Connection: Ignoring frame with invalid header");
        return;
    }

    // Check if frame is for us
    uint32_t our_hash = v2::hashCallsign(local_call_);
    if (header.dst_hash != our_hash && header.dst_hash != 0xFFFFFF) {
        LOG_MODEM(TRACE, "Connection: Ignoring frame for different station");
        return;
    }

    // Resolve source callsign from hash if possible
    std::string src_call;
    if (!remote_call_.empty() && v2::hashCallsign(remote_call_) == header.src_hash) {
        src_call = remote_call_;
    } else if (!pending_remote_call_.empty() && v2::hashCallsign(pending_remote_call_) == header.src_hash) {
        src_call = pending_remote_call_;
    }

    LOG_MODEM(DEBUG, "Connection: Received %s seq=%d from hash 0x%06X",
              v2::frameTypeToString(header.type), header.seq, header.src_hash);

    // Check frame type category and dispatch accordingly
    if (v2::isConnectFrame(header.type)) {
        // CONNECT/CONNECT_ACK/CONNECT_NAK - parse as ConnectFrame (carries full callsigns)
        auto conn = v2::ConnectFrame::deserialize(frame_data);
        if (conn) {
            // Extract callsign from frame if available
            std::string frame_src_call = conn->getSrcCallsign();
            if (!frame_src_call.empty()) {
                src_call = frame_src_call;  // Use verified callsign from frame
            }

            switch (conn->type) {
                case v2::FrameType::CONNECT:
                    handleConnect(*conn, src_call);
                    break;
                case v2::FrameType::CONNECT_ACK:
                    handleConnectAck(*conn, src_call);
                    break;
                case v2::FrameType::CONNECT_NAK:
                    handleConnectNak(*conn, src_call);
                    break;
                case v2::FrameType::DISCONNECT:
                    handleDisconnectFrame(*conn, src_call);
                    break;
                default:
                    break;
            }
        }
    } else if (v2::isControlFrame(header.type)) {
        // Control frames (DISCONNECT, ACK, NACK, etc) - parse as ControlFrame
        auto ctrl = v2::ControlFrame::deserialize(frame_data);
        if (ctrl) {
            switch (ctrl->type) {
                case v2::FrameType::DISCONNECT:
                    handleDisconnect(*ctrl, src_call);
                    break;
                case v2::FrameType::ACK:
                    if (state_ == ConnectionState::DISCONNECTING) {
                        if (ctrl->seq == v2::DISCONNECT_SEQ) {
                            LOG_MODEM(INFO, "Connection: Disconnect acknowledged (seq=0x%04X)", ctrl->seq);
                            enterDisconnected("Disconnect complete");
                        } else {
                            LOG_MODEM(DEBUG, "Connection: Ignoring stale data ACK seq=%d while disconnecting", ctrl->seq);
                        }
                    } else if (state_ == ConnectionState::CONNECTED) {
                        // Check if this ACK is for our pending MODE_CHANGE
                        if (mode_change_pending_ && ctrl->seq == mode_change_seq_) {
                            LOG_MODEM(INFO, "Connection: MODE_CHANGE acknowledged, applying %s %s",
                                      modulationToString(pending_modulation_),
                                      codeRateToString(pending_code_rate_));
                            data_modulation_ = pending_modulation_;
                            data_code_rate_ = pending_code_rate_;
                            arq_.setCodeRate(data_code_rate_);  // Update ARQ for correct total_cw
                            mode_change_pending_ = false;

                            // Notify application of mode change
                            if (on_data_mode_changed_) {
                                on_data_mode_changed_(data_modulation_, data_code_rate_, pending_snr_db_);
                            }
                        } else {
                            // Regular data ACK
                            arq_.onFrameReceived(frame_data);
                        }
                    }
                    break;
                case v2::FrameType::NACK:
                    if (state_ == ConnectionState::CONNECTED) {
                        arq_.onFrameReceived(frame_data);
                    }
                    break;
                case v2::FrameType::MODE_CHANGE:
                    handleModeChange(*ctrl, src_call);
                    break;
                case v2::FrameType::PROBE:
                case v2::FrameType::PROBE_ACK:
                    // PROBE not used - ignore (or could respond with CONNECT_NAK)
                    LOG_MODEM(DEBUG, "Connection: Ignoring PROBE (not supported)");
                    break;
                default:
                    break;
            }
        }
    } else {
        // Data frame - pass to ARQ
        if (state_ == ConnectionState::CONNECTED) {
            arq_.onFrameReceived(frame_data);
        }
    }
}

// =============================================================================
// TIMER / TICK
// =============================================================================

void Connection::tick(uint32_t elapsed_ms) {
    switch (state_) {
        case ConnectionState::PROBING:
            // Fast presence check via PING/PONG
            if (elapsed_ms >= timeout_remaining_ms_) {
                ping_retry_count_++;
                if (ping_retry_count_ >= MAX_PING_RETRIES) {
                    // No response after all PINGs - give up
                    LOG_MODEM(INFO, "Connection: No response after %d PINGs, giving up",
                              MAX_PING_RETRIES);
                    stats_.connects_failed++;
                    enterDisconnected("No response");
                } else {
                    LOG_MODEM(INFO, "Connection: PING timeout, retrying (%d/%d)",
                              ping_retry_count_, MAX_PING_RETRIES);
                    if (on_ping_tx_) {
                        on_ping_tx_();
                    }
                    timeout_remaining_ms_ = PING_TIMEOUT_MS;
                }
            } else {
                timeout_remaining_ms_ -= elapsed_ms;
            }
            break;

        case ConnectionState::CONNECTING:
            if (elapsed_ms >= timeout_remaining_ms_) {
                connect_retry_count_++;
                if (connect_retry_count_ >= config_.connect_retries) {
                    LOG_MODEM(ERROR, "Connection: Connect failed after %d attempts",
                              config_.connect_retries);
                    stats_.connects_failed++;
                    char reason[64];
                    snprintf(reason, sizeof(reason), "Connection timeout after %d attempts", config_.connect_retries);
                    enterDisconnected(reason);
                } else {
                    // Check if we need to fall back to MFSK after DPSK_ATTEMPTS
                    if (connect_retry_count_ == DPSK_ATTEMPTS &&
                        connect_waveform_ == WaveformMode::MC_DPSK) {
                        connect_waveform_ = WaveformMode::MFSK;
                        LOG_MODEM(WARN, "Connection: Falling back to MFSK after %d DPSK attempts",
                                  DPSK_ATTEMPTS);
                        if (on_connect_waveform_changed_) {
                            on_connect_waveform_changed_(connect_waveform_);
                        }
                    }

                    LOG_MODEM(WARN, "Connection: Connect timeout, retrying via %s (%d/%d)",
                              waveformModeToString(connect_waveform_),
                              connect_retry_count_ + 1, config_.connect_retries);
                    auto connect_frame = v2::ConnectFrame::makeConnect(local_call_, remote_call_,
                                                                        config_.mode_capabilities,
                                                                        static_cast<uint8_t>(config_.preferred_mode),
                                                                        static_cast<uint8_t>(config_.forced_modulation),
                                                                        static_cast<uint8_t>(config_.forced_code_rate));
                    transmitFrame(connect_frame.serialize());
                    timeout_remaining_ms_ = config_.connect_timeout_ms;
                }
            } else {
                timeout_remaining_ms_ -= elapsed_ms;
            }
            break;

        case ConnectionState::CONNECTED:
            connected_time_ms_ += elapsed_ms;
            stats_.connected_time_ms = connected_time_ms_;

            // Handle MODE_CHANGE timeout
            if (mode_change_pending_) {
                if (elapsed_ms >= mode_change_timeout_ms_) {
                    mode_change_retry_count_++;
                    if (mode_change_retry_count_ > MODE_CHANGE_MAX_RETRIES) {
                        LOG_MODEM(WARN, "Connection: MODE_CHANGE failed after %d attempts, keeping current mode",
                                  MODE_CHANGE_MAX_RETRIES);
                        mode_change_pending_ = false;
                        // Stay at current mode - don't change anything
                    } else {
                        LOG_MODEM(WARN, "Connection: MODE_CHANGE timeout, retrying (%d/%d)",
                                  mode_change_retry_count_, MODE_CHANGE_MAX_RETRIES);
                        // Resend MODE_CHANGE with same parameters
                        auto frame = v2::ControlFrame::makeModeChange(local_call_, remote_call_,
                                                                       mode_change_seq_, pending_modulation_,
                                                                       pending_code_rate_, pending_snr_db_,
                                                                       pending_reason_);
                        transmitFrame(frame.serialize());
                        mode_change_timeout_ms_ = MODE_CHANGE_TIMEOUT_MS;
                    }
                } else {
                    mode_change_timeout_ms_ -= elapsed_ms;
                }
            }

            // Disconnect grace period (responder side): stay connected and
            // proactively re-send ACK until initiator confirms (goes silent)
            if (disconnect_pending_) {
                if (elapsed_ms >= disconnect_pending_ms_) {
                    disconnect_pending_ = false;
                    disconnect_ack_frame_.clear();
                    enterDisconnected("Remote disconnected");
                    break;
                }
                disconnect_pending_ms_ -= elapsed_ms;

                // Proactively re-send ACK periodically (fading may have lost it)
                if (!disconnect_ack_frame_.empty()) {
                    if (elapsed_ms >= disconnect_ack_retransmit_ms_) {
                        disconnect_ack_retransmit_ms_ = DISCONNECT_ACK_RETRANSMIT_MS;
                        LOG_MODEM(INFO, "Connection: Re-sending disconnect ACK (proactive, %dms remaining)",
                                  disconnect_pending_ms_);
                        transmitFrame(disconnect_ack_frame_);
                    } else {
                        disconnect_ack_retransmit_ms_ -= elapsed_ms;
                    }
                }
            }

            arq_.tick(elapsed_ms);
            break;

        case ConnectionState::DISCONNECTING:
            if (elapsed_ms >= timeout_remaining_ms_) {
                LOG_MODEM(INFO, "Connection: Disconnect timeout, forcing disconnect");
                enterDisconnected("Disconnect timeout");
            } else {
                timeout_remaining_ms_ -= elapsed_ms;

                // Retransmit DISCONNECT periodically (fading can lose the frame)
                if (elapsed_ms >= disconnect_retransmit_ms_) {
                    disconnect_retransmit_ms_ = DISCONNECT_RETRANSMIT_INTERVAL_MS;
                    if (disconnect_retry_count_ < DISCONNECT_MAX_RETRIES && !disconnect_frame_.empty()) {
                        disconnect_retry_count_++;
                        LOG_MODEM(INFO, "Connection: Retransmitting DISCONNECT (%d/%d)",
                                  disconnect_retry_count_, DISCONNECT_MAX_RETRIES);
                        transmitFrame(disconnect_frame_);
                    }
                } else {
                    disconnect_retransmit_ms_ -= elapsed_ms;
                }
            }
            break;

        default:
            break;
    }
}

// =============================================================================
// STATE TRANSITIONS
// =============================================================================

void Connection::transmitFrame(const Bytes& frame_data) {
    LOG_MODEM(DEBUG, "Connection: TX %zu bytes", frame_data.size());

    // If burst mode is active, buffer instead of transmitting immediately
    if (burst_mode_active_ && on_transmit_burst_) {
        burst_tx_buffer_.push_back(frame_data);
        return;
    }

    if (on_transmit_) {
        on_transmit_(frame_data);
    }
}

void Connection::enterConnected() {
    state_ = ConnectionState::CONNECTED;
    connected_time_ms_ = 0;

    arq_.setCallsigns(local_call_, remote_call_);
    arq_.reset();

    // MC-DPSK uses stop-and-wait (window=1), OFDM uses sliding window (window=4)
    // Timeout must exceed round-trip time: TX frame + RX decode + ACK TX + ACK decode
    //   MC-DPSK: full chirp preamble (1.3s) + data (~0.6s) + search buffer (~2.5s) each way → RTT ~13s
    //   OFDM:    light preamble + data (~1.2s) + LTS search (~0.5s) each way → RTT ~3.5s
    if (negotiated_mode_ == WaveformMode::MC_DPSK) {
        arq_.setWindowSize(1);
        arq_.setAckTimeout(18000);
        arq_.setSackDelay(2000);
        arq_.setAckRepeatCount(1);  // Single ACK (stop-and-wait, no benefit from repeat)
        LOG_MODEM(INFO, "Connection: ARQ window=1, timeout=18s, ack_repeat=1 (MC-DPSK)");
    } else {
        // Keep OFDM in-flight burst shorter to reduce ACK-lag hole amplification
        // on fading channels. A window of 4 matches burst-interleave grouping and
        // has shown better control-path stability than 8 for file transfer.
        arq_.setWindowSize(4);
        arq_.setMaxRetries(15);     // More attempts compensate for ACK loss on fading
        arq_.setSackDelay(120);     // Short coalescing delay for ACK/SACK control traffic

        int ack_repeat_count = 2;
        uint32_t ack_repeat_delay_ms = 220;

        // D8PSK R1/2 in fading benefits from stronger control-frame diversity.
        if (data_modulation_ == Modulation::D8PSK && data_code_rate_ == CodeRate::R1_2) {
            ack_repeat_count = 3;
            ack_repeat_delay_ms = 250;
        }

        arq_.setAckRepeatCount(ack_repeat_count);
        arq_.setAckRepeatDelay(ack_repeat_delay_ms);

        uint32_t ack_timeout_ms = computeOfdmAckTimeoutMs(
            data_modulation_, data_code_rate_, arq_.getWindowSize(), arq_.getSackDelay(), ack_repeat_count);
        arq_.setAckTimeout(ack_timeout_ms);

        constexpr float symbol_ms = (1000.0f * OFDM_SYMBOL_SAMPLES) / OFDM_SAMPLE_RATE;
        uint32_t data_symbols = symbolsForCodewords(data_modulation_, data_code_rate_, FIXED_FRAME_CODEWORDS);
        uint32_t ack_symbols = symbolsForCodewords(data_modulation_, data_code_rate_, 1);
        uint32_t data_ms = static_cast<uint32_t>(data_symbols * symbol_ms + 0.5f);
        uint32_t ack_ms = static_cast<uint32_t>(ack_symbols * symbol_ms + 0.5f);

        LOG_MODEM(INFO,
                  "Connection: ARQ window=%zu, timeout=%.2fs (data=%ums, ack=%ums x%d), max_retries=%d, sack_delay=%ums, ack_repeat=%d/%ums (OFDM %s %s)",
                  arq_.getWindowSize(),
                  ack_timeout_ms / 1000.0f,
                  data_ms,
                  ack_ms,
                  ack_repeat_count,
                  arq_.getMaxRetries(),
                  arq_.getSackDelay(),
                  ack_repeat_count,
                  ack_repeat_delay_ms,
                  modulationToString(data_modulation_),
                  codeRateToString(data_code_rate_));
    }

    LOG_MODEM(INFO, "Connection: Now CONNECTED to %s (mode=%s)",
              remote_call_.c_str(), waveformModeToString(negotiated_mode_));

    if (on_mode_negotiated_) {
        on_mode_negotiated_(negotiated_mode_);
    }

    if (on_connected_) {
        on_connected_();
    }
}

void Connection::enterDisconnected(const std::string& reason) {
    state_ = ConnectionState::DISCONNECTED;
    std::string old_remote = remote_call_;
    remote_call_.clear();
    pending_remote_call_.clear();
    mode_change_pending_ = false;
    disconnect_frame_.clear();
    disconnect_pending_ = false;
    disconnect_ack_frame_.clear();
    burst_mode_active_ = false;
    burst_tx_buffer_.clear();
    arq_.reset();
    file_transfer_.cancel();
    pending_tx_fragments_.clear();
    pending_tx_fragment_flags_.clear();
    next_fragment_idx_ = 0;
    acked_fragment_count_ = 0;
    rx_reassembly_buffer_.clear();

    // Reset connect waveform to DPSK for next connection attempt
    connect_waveform_ = WaveformMode::MC_DPSK;

    LOG_MODEM(INFO, "Connection: Disconnected from %s (%s)",
              old_remote.c_str(), reason.c_str());

    if (on_disconnected_) {
        on_disconnected_(reason);
    }
}

// =============================================================================
// CALLBACKS
// =============================================================================

void Connection::setTransmitCallback(TransmitCallback cb) {
    on_transmit_ = std::move(cb);
}

void Connection::setTransmitBurstCallback(TransmitBurstCallback cb) {
    on_transmit_burst_ = std::move(cb);
}

void Connection::flushBurstBuffer() {
    if (burst_tx_buffer_.empty()) return;

    if (burst_tx_buffer_.size() == 1 && on_transmit_) {
        // Single frame, no burst needed
        on_transmit_(burst_tx_buffer_[0]);
    } else if (on_transmit_burst_) {
        LOG_MODEM(INFO, "Connection: Flushing burst of %zu frames", burst_tx_buffer_.size());
        on_transmit_burst_(burst_tx_buffer_);
    } else if (on_transmit_) {
        // Fallback: send individually
        for (const auto& frame : burst_tx_buffer_) {
            on_transmit_(frame);
        }
    }
    burst_tx_buffer_.clear();
}

void Connection::setConnectedCallback(ConnectedCallback cb) {
    on_connected_ = std::move(cb);
}

void Connection::setDisconnectedCallback(DisconnectedCallback cb) {
    on_disconnected_ = std::move(cb);
}

void Connection::setMessageReceivedCallback(MessageReceivedCallback cb) {
    on_message_received_ = std::move(cb);
}

void Connection::setMessageSentCallback(MessageSentCallback cb) {
    on_message_sent_ = std::move(cb);
}

void Connection::setIncomingCallCallback(IncomingCallCallback cb) {
    on_incoming_call_ = std::move(cb);
}

void Connection::setDataReceivedCallback(DataReceivedCallback cb) {
    on_data_received_ = std::move(cb);
}

void Connection::setFileProgressCallback(FileProgressCallback cb) {
    file_transfer_.setProgressCallback(std::move(cb));
}

void Connection::setFileReceivedCallback(FileReceivedCallback cb) {
    file_transfer_.setReceivedCallback(std::move(cb));
}

void Connection::setFileSentCallback(FileSentCallback cb) {
    file_transfer_.setSentCallback(std::move(cb));
}

// =============================================================================
// STATS & RESET
// =============================================================================

ConnectionStats Connection::getStats() const {
    ConnectionStats s = stats_;
    s.arq = arq_.getStats();
    return s;
}

void Connection::resetStats() {
    stats_ = ConnectionStats{};
    arq_.resetStats();
}

void Connection::reset() {
    state_ = ConnectionState::DISCONNECTED;
    remote_call_.clear();
    pending_remote_call_.clear();
    timeout_remaining_ms_ = 0;
    connect_retry_count_ = 0;
    connected_time_ms_ = 0;
    negotiated_mode_ = WaveformMode::OFDM_COX;
    remote_capabilities_ = ModeCapabilities::OFDM_COX;
    remote_preferred_ = WaveformMode::OFDM_COX;
    mode_change_pending_ = false;
    mode_change_timeout_ms_ = 0;
    mode_change_retry_count_ = 0;
    data_modulation_ = Modulation::DQPSK;
    data_code_rate_ = CodeRate::R1_4;
    connect_waveform_ = WaveformMode::MC_DPSK;  // Reset to DPSK for next connect attempt
    burst_mode_active_ = false;
    burst_tx_buffer_.clear();
    arq_.reset();
    file_transfer_.cancel();
    pending_tx_fragments_.clear();
    pending_tx_fragment_flags_.clear();
    next_fragment_idx_ = 0;
    acked_fragment_count_ = 0;
    rx_reassembly_buffer_.clear();
    LOG_MODEM(DEBUG, "Connection: Full reset");
}

} // namespace protocol
} // namespace ultra
