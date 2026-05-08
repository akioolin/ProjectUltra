// Connection frame handlers
// Split from connection.cpp for maintainability

#include "connection.hpp"
#include "connection_policy.hpp"
#include "waveform_selection.hpp"  // Shared waveform/rate selection algorithm
#include "ultra/logging.hpp"

namespace ultra {
namespace protocol {

// Recommend data mode based on SNR, fading, and the NEGOTIATED waveform
// Uses shared recommendDataMode() algorithm from waveform_selection.hpp
// IMPORTANT: waveform should be the negotiated/forced waveform, NOT auto-selected
static void recommendDataModeForWaveform(float snr_db, float fading_index,
                                          WaveformMode waveform,
                                          Modulation& mod, CodeRate& rate) {
    // Use shared algorithm for modulation and rate selection
    recommendDataMode(snr_db, waveform, mod, rate, fading_index);

    LOG_MODEM(INFO, "recommendDataModeForWaveform: SNR=%.1f, fading=%.2f, waveform=%s -> %s %s",
              snr_db, fading_index,
              waveformModeToString(waveform), modulationToString(mod), codeRateToString(rate));
}

// =============================================================================
// PING/PONG HANDLING
// =============================================================================

void Connection::onPongReceived() {
    if (state_ != ConnectionState::PROBING) {
        // Not in probing state - this might be an incoming ping from someone else
        // Notify via ping_received callback (they're calling us)
        if (state_ == ConnectionState::DISCONNECTED && on_ping_received_) {
            LOG_MODEM(INFO, "Connection: Received incoming PING while disconnected");
            on_ping_received_();
        }
        return;
    }

    // We were probing and got a PONG - remote station exists!
    LOG_MODEM(INFO, "Connection: PONG received, sending full CONNECT");
    sendFullConnect();
}

void Connection::sendFullConnect() {
    // Transition to CONNECTING and send full CONNECT frame
    state_ = ConnectionState::CONNECTING;
    connect_retry_count_ = 0;
    timeout_remaining_ms_ = config_.connect_timeout_ms;

    // Notify about state change (PROBING -> CONNECTING)
    if (on_state_changed_) {
        on_state_changed_(ConnectionState::CONNECTING, remote_call_);
    }

    auto connect_frame = v2::ConnectFrame::makeConnect(local_call_, remote_call_,
                                                        config_.mode_capabilities,
                                                        static_cast<uint8_t>(config_.preferred_mode),
                                                        static_cast<uint8_t>(config_.forced_modulation),
                                                        static_cast<uint8_t>(config_.forced_code_rate),
                                                        config_.forced_cw_count);
    Bytes connect_data = connect_frame.serialize();

    LOG_MODEM(INFO, "Connection: Sending CONNECT via %s (%zu bytes, forced_mod=%d, forced_rate=%d, forced_cw=%d)",
              waveformModeToString(connect_waveform_), connect_data.size(),
              static_cast<int>(config_.forced_modulation), static_cast<int>(config_.forced_code_rate),
              static_cast<int>(config_.forced_cw_count));
    transmitFrame(connect_data);
}

// =============================================================================
// CONNECT FRAME HANDLERS
// =============================================================================

void Connection::handleConnect(const v2::ConnectFrame& frame, const std::string& src_call) {
    if (state_ != ConnectionState::DISCONNECTED) {
        LOG_MODEM(WARN, "Connection: Rejecting CONNECT (busy, state=%s)",
                  connectionStateToString(state_));
        auto nak = v2::ConnectFrame::makeConnectNakByHash(local_call_, frame.src_hash);
        transmitFrame(nak.serialize());
        return;
    }

    // Get capabilities from ConnectFrame
    uint8_t remote_caps = frame.mode_capabilities;
    WaveformMode remote_pref = static_cast<WaveformMode>(frame.negotiated_mode);

    LOG_MODEM(INFO, "Connection: Incoming call from %s (caps=0x%02X, pref=%s)",
              src_call.c_str(), remote_caps, waveformModeToString(remote_pref));
    stats_.connects_received++;

    remote_capabilities_ = remote_caps;
    remote_preferred_ = remote_pref;
    setPhyMaskV1Negotiated(hasPhyMaskV1Capability(config_.mode_capabilities) &&
                           hasPhyMaskV1Capability(remote_caps));

    // Default channel estimates for mode selection
    float snr_db = 15.0f;
    float delay_spread_ms = 1.0f;
    float doppler_spread_hz = 0.5f;

    if (config_.auto_accept) {
        // Store callsign if known, otherwise use placeholder with hash for display
        remote_call_ = src_call.empty() ? "REMOTE" : src_call;
        remote_hash_ = frame.src_hash;  // Store hash for routing

        // Use measured SNR from modem (set via setMeasuredSNR)
        snr_db = measured_snr_db_;

        // Let negotiateMode() handle AUTO mode with SNR-based selection
        // Don't override remote_pref here - negotiateMode() has the correct SNR thresholds
        negotiated_mode_ = negotiateMode(remote_caps, remote_pref);

        LOG_MODEM(INFO, "Connection: Negotiated waveform mode: %s",
                  waveformModeToString(negotiated_mode_));

        // We are the responder - we received CONNECT and are sending CONNECT_ACK
        is_initiator_ = false;
        handshake_confirmed_ = false;  // Responder waits for first frame to confirm
        responder_handshake_wait_ms_ = RESPONDER_HANDSHAKE_FAILSAFE_MS;

        // Check if initiator forced specific modes (0xFF = AUTO, else forced)
        Modulation forced_mod = static_cast<Modulation>(frame.initial_modulation);
        CodeRate forced_rate = static_cast<CodeRate>(frame.initial_code_rate);

        Modulation rec_mod;
        CodeRate rec_rate;

        // If waveform is AUTO, select based on SNR/fading first
        if (negotiated_mode_ == WaveformMode::AUTO) {
            auto rec = recommendWaveformAndRate(snr_db, fading_index_);
            negotiated_mode_ = rec.waveform;
            LOG_MODEM(INFO, "Connection: Auto-selected waveform %s based on SNR=%.1f, fading=%.2f",
                      waveformModeToString(negotiated_mode_), snr_db, fading_index_);
        }

        // Get recommended mode based on SNR, fading AND the negotiated waveform
        // This ensures MC-DPSK uses R1/4, OFDM uses appropriate rate, etc.
        recommendDataModeForWaveform(snr_db, fading_index_, negotiated_mode_, rec_mod, rec_rate);

        // Bootstrap safety: chirp SNR can overestimate first OFDM frame quality.
        // Start one step more robust when channel is borderline.
        if (isOFDMMode(negotiated_mode_)) {
            CodeRate capped = capInitialOFDMRate(snr_db, fading_index_, rec_rate);
            if (capped != rec_rate) {
                LOG_MODEM(INFO, "Connection: Bootstrap cap %s -> %s for initial OFDM setup (SNR=%.1f, fading=%.2f)",
                          codeRateToString(rec_rate), codeRateToString(capped), snr_db, fading_index_);
                rec_rate = capped;
            }
        }

        // Override with forced values if specified
        if (forced_mod != Modulation::AUTO) {
            rec_mod = forced_mod;
            LOG_MODEM(INFO, "Connection: Using FORCED modulation %s from initiator",
                      modulationToString(rec_mod));
        }

        if (forced_rate != CodeRate::AUTO) {
            rec_rate = forced_rate;
            LOG_MODEM(INFO, "Connection: Using FORCED code rate %s from initiator",
                      codeRateToString(rec_rate));
        }

        LOG_MODEM(INFO, "Connection: Initial data mode %s %s (SNR=%.1f dB, forced_mod=%d, forced_rate=%d)",
                  modulationToString(rec_mod), codeRateToString(rec_rate), snr_db,
                  static_cast<int>(forced_mod), static_cast<int>(forced_rate));

        // Set our local data mode immediately
        data_modulation_ = rec_mod;
        data_code_rate_ = rec_rate;
        arq_.setCodeRate(data_code_rate_);  // Update ARQ for correct total_cw calculation

        // Pick negotiated CW count. Honor initiator's forced value if it sent
        // one (frame.data_frame_cw_count != 0), else auto-pick from rate.
        // We store the result in data_frame_cw_count_ here BEFORE building
        // the CONNECT_ACK and before computing the retry delay so both reflect
        // the value the initiator will see on the wire — Codex finding 4.
        int negotiated_cw = (frame.data_frame_cw_count != 0)
            ? v2::sanitizeFixedFrameCodewords(frame.data_frame_cw_count)
            : connection_policy::recommendCWCount(rec_rate, negotiated_mode_);
        data_frame_cw_count_ = negotiated_cw;
        config_.fixed_frame_codewords = negotiated_cw;
        const uint8_t cw_byte = static_cast<uint8_t>(negotiated_cw);

        // Prefer full-callsign CONNECT_ACK when the initiator callsign is known,
        // fallback to hash-only ACK when we only have src_hash.
        Bytes ack_data;
        if (!src_call.empty()) {
            auto ack = v2::ConnectFrame::makeConnectAck(local_call_, src_call,
                                                        static_cast<uint8_t>(negotiated_mode_),
                                                        rec_mod, rec_rate, snr_db, fading_index_,
                                                        cw_byte);
            if (phy_mask_v1_negotiated_) {
                v2::setPhyMaskV1Capability(ack);
            }
            ack_data = ack.serialize();
        } else {
            auto ack = v2::ConnectFrame::makeConnectAckByHash(local_call_, frame.src_hash,
                                                               static_cast<uint8_t>(negotiated_mode_),
                                                               rec_mod, rec_rate, snr_db, fading_index_,
                                                               cw_byte);
            if (phy_mask_v1_negotiated_) {
                v2::setPhyMaskV1Capability(ack);
            }
            ack_data = ack.serialize();
        }
        transmitFrame(ack_data);

        // Cache the ACK for proactive retransmission. ALPHA can fail to decode
        // the single MC-DPSK CONNECT_ACK on faded seeds, leaving handshake stuck.
        // We re-send periodically until handshake is confirmed by first frame.
        connect_ack_frame_ = ack_data;
        connect_ack_retransmit_interval_ms_ =
            connection_policy::connectAckRetransmitDelayMs(
                negotiated_mode_, rec_mod, rec_rate, data_frame_cw_count_);
        connect_ack_retransmit_ms_ = connect_ack_retransmit_interval_ms_;
        connect_ack_retx_remaining_ = CONNECT_ACK_MAX_RETX;
        LOG_MODEM(INFO, "Connection: CONNECT_ACK rescue retry armed in %.2fs (%d remaining)",
                  connect_ack_retransmit_interval_ms_ / 1000.0f,
                  connect_ack_retx_remaining_);

        enterConnected();
        // NOTE: Don't call on_handshake_confirmed_ yet - wait for first frame from initiator

        // Notify application of initial data mode
        if (on_data_mode_changed_) {
            on_data_mode_changed_(data_modulation_, data_code_rate_, data_frame_cw_count_,
                                  snr_db, fading_index_);
        }
    } else {
        pending_remote_call_ = src_call.empty() ? "REMOTE" : src_call;
        pending_remote_hash_ = frame.src_hash;  // Store hash for later
        // Store forced modes from initiator for later use in acceptCall()
        pending_forced_modulation_ = static_cast<Modulation>(frame.initial_modulation);
        pending_forced_code_rate_ = static_cast<CodeRate>(frame.initial_code_rate);
        pending_forced_cw_count_ = frame.data_frame_cw_count;  // 0 = AUTO
        if (on_incoming_call_) {
            on_incoming_call_(pending_remote_call_);
        }
    }
}

void Connection::handleConnectAck(const v2::ConnectFrame& frame, const std::string& src_call) {
    if (state_ != ConnectionState::CONNECTING) {
        LOG_MODEM(DEBUG, "Connection: Ignoring CONNECT_ACK (state=%s)",
                  connectionStateToString(state_));
        return;
    }

    // Get negotiated waveform mode from ConnectFrame
    WaveformMode mode = static_cast<WaveformMode>(frame.negotiated_mode);
    negotiated_mode_ = mode;
    setPhyMaskV1Negotiated(hasPhyMaskV1Capability(config_.mode_capabilities) &&
                           v2::hasPhyMaskV1Capability(frame));

    // Get initial data mode from CONNECT_ACK (eliminates separate MODE_CHANGE)
    Modulation init_mod = static_cast<Modulation>(frame.initial_modulation);
    CodeRate init_rate = static_cast<CodeRate>(frame.initial_code_rate);
    float snr_db = v2::decodeSNR(frame.measured_snr);
    float peer_fading = v2::decodeFadingIndex(frame.mode_capabilities);

    // Negotiated CW count from responder. Falls back to recommendCWCount(rate)
    // if responder advertised 0 (interoperability with un-upgraded peer that
    // hasn't been built since the protocol change — defensive only).
    int negotiated_cw = (frame.data_frame_cw_count != 0)
        ? v2::sanitizeFixedFrameCodewords(frame.data_frame_cw_count)
        : connection_policy::recommendCWCount(init_rate, negotiated_mode_);

    // Apply the initial data mode immediately
    data_modulation_ = init_mod;
    data_code_rate_ = init_rate;
    data_frame_cw_count_ = negotiated_cw;
    config_.fixed_frame_codewords = negotiated_cw;
    arq_.setCodeRate(data_code_rate_);  // Update ARQ for correct total_cw calculation

    // Update remote callsign if we got it from the frame
    if (!src_call.empty() && (remote_call_.empty() || remote_call_ == "REMOTE")) {
        remote_call_ = src_call;
    }

    LOG_MODEM(INFO, "Connection: Connected to %s (waveform=%s, data=%s %s, SNR=%.1f dB, peer_fading=%.2f)",
              remote_call_.c_str(), waveformModeToString(negotiated_mode_),
              modulationToString(data_modulation_), codeRateToString(data_code_rate_), snr_db, peer_fading);

    // We are the initiator - we sent CONNECT and received CONNECT_ACK
    is_initiator_ = true;
    handshake_confirmed_ = true;  // Handshake complete for initiator
    responder_handshake_wait_ms_ = 0;

    enterConnected();

    // Initiator can switch to negotiated waveform immediately
    if (on_handshake_confirmed_) {
        on_handshake_confirmed_();
    }

    // Notify application of initial data mode
    if (on_data_mode_changed_) {
        on_data_mode_changed_(data_modulation_, data_code_rate_, data_frame_cw_count_,
                              snr_db, peer_fading);
    }
}

void Connection::handleConnectNak(const v2::ConnectFrame& frame, const std::string& src_call) {
    if (state_ != ConnectionState::CONNECTING) {
        return;
    }

    LOG_MODEM(WARN, "Connection: Connection rejected by %s", remote_call_.c_str());
    stats_.connects_failed++;
    enterDisconnected("Connection rejected");
}

// =============================================================================
// DISCONNECT HANDLERS
// =============================================================================

void Connection::handleDisconnect(const v2::ControlFrame& frame, const std::string& src_call) {
    if (state_ == ConnectionState::DISCONNECTED) {
        return;
    }

    LOG_MODEM(INFO, "Connection: Disconnect from %s", remote_call_.c_str());

    auto ack = v2::ControlFrame::makeAck(local_call_, remote_call_, frame.seq);
    disconnect_ack_frame_ = ack.serialize();
    transmitFrame(disconnect_ack_frame_);

    if (disconnect_pending_) {
        // Already in grace period from a previous DISCONNECT — re-sent ACK, reset timer
        LOG_MODEM(INFO, "Connection: Re-sent disconnect ACK (retransmit detected)");
        disconnect_pending_ms_ = DISCONNECT_GRACE_MS;
        disconnect_ack_retransmit_ms_ = DISCONNECT_ACK_RETRANSMIT_MS;
        return;
    }

    // Enter grace period: stay connected so we can re-send ACK if initiator retransmits
    stats_.disconnects++;
    disconnect_pending_ = true;
    disconnect_pending_ms_ = DISCONNECT_GRACE_MS;
    disconnect_ack_retransmit_ms_ = DISCONNECT_ACK_RETRANSMIT_MS;
    LOG_MODEM(INFO, "Connection: Disconnect ACK sent, grace period %dms (re-send ACK every %dms)",
              DISCONNECT_GRACE_MS, DISCONNECT_ACK_RETRANSMIT_MS);
}

void Connection::handleDisconnectFrame(const v2::ConnectFrame& frame, const std::string& src_call) {
    if (state_ == ConnectionState::DISCONNECTED) {
        return;
    }

    LOG_MODEM(INFO, "Connection: Disconnect from %s", src_call.c_str());

    // Send ACK for the disconnect
    auto ack = v2::ControlFrame::makeAck(local_call_, remote_call_, frame.seq);
    disconnect_ack_frame_ = ack.serialize();
    transmitFrame(disconnect_ack_frame_);

    if (disconnect_pending_) {
        // Already in grace period — re-sent ACK, reset timer
        LOG_MODEM(INFO, "Connection: Re-sent disconnect ACK (retransmit detected)");
        disconnect_pending_ms_ = DISCONNECT_GRACE_MS;
        disconnect_ack_retransmit_ms_ = DISCONNECT_ACK_RETRANSMIT_MS;
        return;
    }

    // Enter grace period: stay connected so we can re-send ACK if initiator retransmits
    stats_.disconnects++;
    disconnect_pending_ = true;
    disconnect_pending_ms_ = DISCONNECT_GRACE_MS;
    disconnect_ack_retransmit_ms_ = DISCONNECT_ACK_RETRANSMIT_MS;
    LOG_MODEM(INFO, "Connection: Disconnect ACK sent, grace period %dms (re-send ACK every %dms)",
              DISCONNECT_GRACE_MS, DISCONNECT_ACK_RETRANSMIT_MS);
}

// =============================================================================
// MODE CHANGE HANDLING
// =============================================================================

void Connection::handleModeChange(const v2::ControlFrame& frame, const std::string& src_call) {
    if (state_ != ConnectionState::CONNECTED) {
        LOG_MODEM(DEBUG, "Connection: Ignoring MODE_CHANGE (not connected)");
        return;
    }

    // Parse MODE_CHANGE payload
    auto info = frame.getModeChangeInfo();

    const char* reason_str = "unknown";
    switch (info.reason) {
        case v2::ModeChangeReason::CHANNEL_IMPROVED: reason_str = "channel improved"; break;
        case v2::ModeChangeReason::CHANNEL_DEGRADED: reason_str = "channel degraded"; break;
        case v2::ModeChangeReason::USER_REQUEST:     reason_str = "user request"; break;
        case v2::ModeChangeReason::INITIAL_SETUP:    reason_str = "initial setup"; break;
    }

    LOG_MODEM(INFO, "Connection: MODE_CHANGE from %s: %s %s (SNR=%.1f dB, fading=%.2f, reason=%s)",
              remote_call_.c_str(),
              modulationToString(info.modulation),
              codeRateToString(info.code_rate),
              info.snr_db,
              info.fading_index,
              reason_str);

    // Update local state and refresh the ARQ profile for the new fixed-frame
    // capacity/window/timing. The requester waits for this ACK before sending
    // more DATA in the new mode. The cw_count field in MODE_CHANGE is the
    // requester's chosen value — receiver applies it directly so both peers
    // stay in lockstep on frame geometry.
    applyDataMode(info.modulation, info.code_rate, info.data_frame_cw_count);

    // Send ACK for the MODE_CHANGE
    auto ack = v2::ControlFrame::makeAck(local_call_, remote_call_, frame.seq);
    transmitFrame(ack.serialize());

    // Notify application of mode change
    if (on_data_mode_changed_) {
        on_data_mode_changed_(info.modulation, info.code_rate, data_frame_cw_count_,
                              info.snr_db, info.fading_index);
    }

    runDeferredArqRefill();
}

void Connection::requestModeChange(Modulation new_mod, CodeRate new_rate,
                                    float measured_snr, uint8_t reason) {
    if (state_ != ConnectionState::CONNECTED) {
        LOG_MODEM(WARN, "Connection: Cannot request mode change (not connected)");
        return;
    }

    // Don't send new MODE_CHANGE if one is already pending
    if (mode_change_pending_) {
        LOG_MODEM(DEBUG, "Connection: MODE_CHANGE already pending, ignoring request");
        return;
    }

    LOG_MODEM(INFO, "Connection: Requesting MODE_CHANGE to %s %s (SNR=%.1f dB)",
              modulationToString(new_mod), codeRateToString(new_rate), measured_snr);

    // Store pending mode change parameters for retry
    pending_modulation_ = new_mod;
    pending_code_rate_ = new_rate;
    pending_snr_db_ = measured_snr;
    pending_fading_index_ = fading_index_;
    pending_reason_ = reason;
    mode_change_pending_ = true;
    mode_change_retry_count_ = 0;
    mode_change_timeout_ms_ = MODE_CHANGE_TIMEOUT_MS;

    // Pick the CW count for the new rate. If the operator forced a CW
    // count via --cw-count (config_.forced_cw_count != 0), preserve that
    // override across mode changes — otherwise auto-pick from rate.
    // Without this, a later MODE_CHANGE would silently drop the user's
    // override (caught by Codex, 2026-05-04).
    pending_cw_count_ = static_cast<uint8_t>((config_.forced_cw_count != 0)
        ? v2::sanitizeFixedFrameCodewords(config_.forced_cw_count)
        : connection_policy::recommendCWCount(new_rate, negotiated_mode_));

    mode_change_seq_++;
    auto frame = v2::ControlFrame::makeModeChange(local_call_, remote_call_,
                                                   mode_change_seq_, new_mod, new_rate,
                                                   measured_snr, fading_index_, reason,
                                                   pending_cw_count_);
    transmitFrame(frame.serialize());

    // NOTE: Don't update local mode until ACK is received
    // This prevents mode mismatch if the remote doesn't receive our MODE_CHANGE
}

// =============================================================================
// DATA PAYLOAD HANDLING
// =============================================================================

void Connection::handleDataPayload(const Bytes& payload, bool more_data, v2::FrameType frame_type) {
    if (payload.empty()) {
        return;
    }

    const bool binary_payload =
        frame_type == v2::FrameType::DATA_START ||
        frame_type == v2::FrameType::DATA_CONT ||
        frame_type == v2::FrameType::DATA_END;

    if (!binary_payload && file_transfer_.processPayload(payload, more_data)) {
        LOG_MODEM(DEBUG, "Connection: Processed file transfer payload (%zu bytes)",
                  payload.size());
        return;
    }

    if (more_data) {
        // Fragment with MORE_FRAG - accumulate
        rx_reassembly_buffer_.insert(rx_reassembly_buffer_.end(), payload.begin(), payload.end());
        LOG_MODEM(DEBUG, "Connection: Accumulated fragment (%zu bytes, buffer now %zu bytes)",
                  payload.size(), rx_reassembly_buffer_.size());
        return;
    }

    // Final or single frame
    Bytes complete_payload;
    if (!rx_reassembly_buffer_.empty()) {
        // Last fragment - stitch into a fresh buffer to avoid aliasing/overflow false positives.
        complete_payload.reserve(rx_reassembly_buffer_.size() + payload.size());
        complete_payload.insert(complete_payload.end(),
                                rx_reassembly_buffer_.begin(),
                                rx_reassembly_buffer_.end());
        complete_payload.insert(complete_payload.end(), payload.begin(), payload.end());
        rx_reassembly_buffer_.clear();
        LOG_MODEM(INFO, "Connection: Reassembled %zu-byte %s from fragments",
                  complete_payload.size(), binary_payload ? "binary payload" : "message");
    } else {
        // Single-frame message (backwards compatible)
        complete_payload = payload;
    }

    // Strip TEXT_MESSAGE type prefix if present
    size_t start = 0;
    if (!binary_payload &&
        !complete_payload.empty() &&
        complete_payload[0] == static_cast<uint8_t>(PayloadType::TEXT_MESSAGE)) {
        start = 1;
    }

    if (binary_payload) {
        if (on_data_received_) {
            on_data_received_(complete_payload, false);
        }
        return;
    }

    std::string text;
    if (start < complete_payload.size()) {
        text.assign(complete_payload.begin() + static_cast<std::ptrdiff_t>(start),
                    complete_payload.end());
    }

    if (on_message_received_) {
        on_message_received_(text);
    }

    if (on_data_received_) {
        on_data_received_(complete_payload, false);
    }
}

// =============================================================================
// WAVEFORM MODE NEGOTIATION
// =============================================================================

WaveformMode Connection::negotiateMode(uint8_t remote_caps, WaveformMode remote_pref) {
    uint8_t common = config_.mode_capabilities & remote_caps;
    float snr = measured_snr_db_;
    WaveformMode selected = connection_policy::selectNegotiatedMode(
        config_.mode_capabilities,
        remote_caps,
        remote_pref,
        narrowband_override_,
        config_.preferred_mode,
        snr,
        fading_index_);

    if (common == 0) {
        LOG_MODEM(WARN, "Connection: No common waveform modes! Falling back to OFDM");
        return selected;
    }

    // If remote has explicit preference, honor it if we support it
    if (remote_pref != WaveformMode::AUTO) {
        uint8_t pref_bit = connection_policy::modeToCapabilityBit(remote_pref);
        if ((common & pref_bit) && selected == remote_pref) {
            LOG_MODEM(INFO, "Connection: Using remote preferred mode: %s",
                      waveformModeToString(remote_pref));
            return selected;
        }
    }

    // If narrowband chirp was detected this session, override for this connection
    if (narrowband_override_ != WaveformMode::AUTO) {
        uint8_t pref_bit = connection_policy::modeToCapabilityBit(narrowband_override_);
        if ((common & pref_bit) && selected == narrowband_override_) {
            LOG_MODEM(INFO, "Connection: Using narrowband override: %s",
                      waveformModeToString(narrowband_override_));
            return selected;
        }
    }

    // If we have explicit preference, use it if remote supports it
    if (config_.preferred_mode != WaveformMode::AUTO) {
        uint8_t pref_bit = connection_policy::modeToCapabilityBit(config_.preferred_mode);
        if ((common & pref_bit) && selected == config_.preferred_mode) {
            LOG_MODEM(INFO, "Connection: Using local preferred mode: %s",
                      waveformModeToString(config_.preferred_mode));
            return selected;
        }
    }

    // AUTO mode: Use shared algorithm from waveform_selection.hpp
    // This ensures negotiateMode and recommendDataModeWithFading use same logic
    LOG_MODEM(INFO, "Connection: AUTO mode selection, SNR=%.1f dB, fading_index=%.2f (%s)",
              snr, fading_index_, connection_policy::fadingLabel(fading_index_));

    auto rec = recommendWaveformAndRate(snr, fading_index_);

    // Check if selected mode is supported by both sides
    if (selected == rec.waveform &&
        (common & connection_policy::modeToCapabilityBit(selected))) {
        LOG_MODEM(INFO, "Connection: Selected %s (SNR=%.1f, fading=%.2f %s)",
                  waveformModeToString(selected),
                  snr,
                  fading_index_,
                  connection_policy::fadingLabel(fading_index_));
        return selected;
    }

    // Fallback if selected mode is not supported by both peers.
    // Production priority excludes reserved OTFS/MFSK values.
    return selected;
}

} // namespace protocol
} // namespace ultra
