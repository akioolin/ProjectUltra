#pragma once

#include "frame_v2.hpp"
#include "arq.hpp"
#include "selective_repeat_arq.hpp"
#include "burst_transport.hpp"
#include "rate_controller.hpp"
#include "file_transfer.hpp"
#include "ultra/types.hpp"
#include "fec/soft_combine.hpp"
#include <cmath>
#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace ultra {
namespace protocol {

// Connection states
enum class ConnectionState {
    DISCONNECTED,
    PROBING,       // Sending PING, waiting for PONG (fast presence check)
    CONNECTING,    // Received PONG, sending full CONNECT
    CONNECTED,
    DISCONNECTING
};

const char* connectionStateToString(ConnectionState state);

// Connection configuration
struct ConnectionConfig {
    ARQConfig arq;
    uint32_t connect_timeout_ms = 60000;  // 60s for DPSK (16s TX + 16s RX + margin)
    uint32_t disconnect_timeout_ms = 30000;
    int connect_retries = 10;  // Robust MC-DPSK control attempts
    bool auto_accept = true;

    uint8_t mode_capabilities = ModeCapabilities::ALL | ModeCapabilities::PHY_MASK_V1;
    WaveformMode preferred_mode = WaveformMode::AUTO;  // Forced waveform (0xFF=AUTO)

    // Forced data mode - operator can override SNR-based selection
    // 0xFF (AUTO) = let responder decide based on SNR
    // Any other value = force that specific mode
    Modulation forced_modulation = Modulation::AUTO;
    CodeRate forced_code_rate = CodeRate::AUTO;
    int fixed_frame_codewords = v2::kDefaultFixedFrameCodewords;
    int mc_dpsk_num_carriers = 8;
    int mc_dpsk_samples_per_symbol = 1024;
    // Initiator-side forced CW override (0 = AUTO, responder picks via
    // recommendCWCount(rate)). When non-zero, the initiator embeds this
    // value in CONNECT.data_frame_cw_count and the responder honors it
    // in CONNECT_ACK + applyDataMode. Set via setForcedFrameCodewords().
    uint8_t forced_cw_count = 0;
};

// Connection statistics
struct ConnectionStats {
    ARQStats arq;
    int connects_initiated = 0;
    int connects_received = 0;
    int connects_failed = 0;
    int disconnects = 0;
    uint32_t connected_time_ms = 0;
};

/**
 * Connection Manager
 *
 * Handles connection establishment and teardown with callsign addressing.
 * Wraps ARQ controller for reliable data transfer once connected.
 * Uses v2 frame format exclusively.
 */
class Connection {
    friend struct ConnectionAdaptiveTestAccess;
public:
    // Callback types - all use serialized Bytes (v2 frames)
    using TransmitCallback = std::function<void(const Bytes&)>;
    using TransmitInfoCallback =
        std::function<void(const Bytes&, bool expect_full_ofdm_anchor_after_tx)>;
    using ConnectedCallback = std::function<void()>;
    using DisconnectedCallback = std::function<void(const std::string& reason)>;
    using MessageReceivedCallback = std::function<void(const std::string& text)>;
    using MessageSentCallback = std::function<void(bool success)>;
    enum class MessageTxStatus {
        SUBMITTED,
        DELIVERED,
        FAILED
    };
    struct MessageTxStatusEvent {
        MessageTxStatus status = MessageTxStatus::SUBMITTED;
        uint16_t first_seq = 0;
        uint16_t last_seq = 0;
        std::string remote_call;
        std::string text;
        uint32_t elapsed_ms = 0;
    };
    using MessageTxStatusCallback = std::function<void(const MessageTxStatusEvent& event)>;
    using IncomingCallCallback = std::function<void(const std::string& remote_call)>;
    using DataReceivedCallback = std::function<void(const Bytes& data, bool more_data)>;

    // Ping/Pong callbacks (for fast presence check before full CONNECT)
    using PingTxCallback = std::function<void()>;  // Request modem to transmit ping
    using PingReceivedCallback = std::function<void()>;  // Called when receiver detects our ping (incoming call)

    // State change callback (for internal state transitions like PROBING → CONNECTING)
    using StateChangedCallback = std::function<void(ConnectionState state, const std::string& info)>;

    // File transfer callbacks
    using FileProgressCallback = FileTransferController::ProgressCallback;
    using FileReceivedCallback = FileTransferController::ReceivedCallback;
    using FileSentCallback = FileTransferController::SentCallback;

    explicit Connection(const ConnectionConfig& config = ConnectionConfig{});

    // --- Configuration ---

    void setLocalCallsign(const std::string& call);
    std::string getLocalCallsign() const { return local_call_; }

    void setAutoAccept(bool auto_accept) { config_.auto_accept = auto_accept; }
    bool getAutoAccept() const { return config_.auto_accept; }

    // --- Connection Control ---

    bool connect(const std::string& remote_call);
    void acceptCall();
    void rejectCall();
    void disconnect();
    void abortTxNow();

    // --- Data Transfer ---

    bool sendMessage(const std::string& text);
    bool sendBinary(const Bytes& data);
    bool sendMessages(const std::vector<std::string>& texts);  // Batch: burst-interleaved
    bool isReadyToSend() const;
    size_t getTxBacklogBytes() const;

    // --- File Transfer ---

    bool sendFile(const std::string& filepath);
    void setReceiveDirectory(const std::string& dir);
    void cancelFileTransfer();
    bool isFileTransferInProgress() const;
    FileTransferProgress getFileProgress() const;

    // --- Frame Processing ---

    // Process received frame data (v2 serialized bytes)
    void onFrameReceived(const Bytes& frame_data);
    void onMCDPSKPartialFrame(const v2::PartialFrameCodewords& partial);
    void onAcceptedOFDMDataSync(float sync_correlation);

    // §14.27: a decoded interleaved burst delivered as a UNIT by the decoder
    // (group_seq, ordered serialized DATA frames, all-logical-frames-decoded).
    // Drops pad frames (addressed to the burst-pad callsign) and, only when the
    // whole group decoded, hands the real frames to the group stop-and-wait
    // controller (deliver-once + single GROUP_ACK). A partial group is dropped
    // so the sender whole-burst-resends. Inert unless use_burst_transport_.
    void onBurstGroupReceived(uint16_t group_seq, const std::vector<Bytes>& frames,
                              bool all_ok, float quality);

    // §14.36 Phase 5c GUI observability: decode headroom of the most recent burst
    // group [0,1] (<0 = none yet) and a short human-readable adaptive action
    // ("rate R3/4 -> R2/3 (q=0.18)" / "hold R3/4 (q=0.85)"). Empty when off.
    bool adaptiveRateEnabled() const { return adaptive_rate_enabled_; }
    float lastGroupQuality() const { return last_group_quality_; }
    const std::string& lastAdaptiveAction() const { return last_adaptive_action_; }

    void tick(uint32_t elapsed_ms);

    // --- Callbacks ---

    void setTransmitCallback(TransmitCallback cb);
    void setTransmitInfoCallback(TransmitInfoCallback cb);

    // Burst mode TX callback - transmits multiple frames as single audio burst.
    // Used for OFDM connected mode and MC-DPSK DATA file-window bursts.
    // group_seq stamps the burst descriptor (§14.27) for whole-burst GROUP_ACK;
    // 0 for the legacy arq_ burst path / single-shot.
    using TransmitBurstCallback = std::function<void(const std::vector<Bytes>&, uint16_t group_seq)>;
    void setTransmitBurstCallback(TransmitBurstCallback cb);

    void setConnectedCallback(ConnectedCallback cb);
    void setDisconnectedCallback(DisconnectedCallback cb);
    void setMessageReceivedCallback(MessageReceivedCallback cb);
    void setMessageSentCallback(MessageSentCallback cb);
    void setMessageTxStatusCallback(MessageTxStatusCallback cb);
    void setIncomingCallCallback(IncomingCallCallback cb);
    void setDataReceivedCallback(DataReceivedCallback cb);

    // Ping/Pong (fast presence check)
    void setPingTxCallback(PingTxCallback cb) { on_ping_tx_ = cb; }
    void setPingReceivedCallback(PingReceivedCallback cb) { on_ping_received_ = cb; }
    void setStateChangedCallback(StateChangedCallback cb) { on_state_changed_ = cb; }
    void onPongReceived();  // Call when modem detects response to our PING

    void setFileProgressCallback(FileProgressCallback cb);
    void setFileReceivedCallback(FileReceivedCallback cb);
    void setFileSentCallback(FileSentCallback cb);

    // --- State ---

    ConnectionState getState() const { return state_; }
    std::string getRemoteCallsign() const { return remote_call_; }
    bool isConnected() const { return state_ == ConnectionState::CONNECTED; }
    ConnectionStats getStats() const;
    void resetStats();

    // --- Waveform Mode ---

    WaveformMode getNegotiatedMode() const { return negotiated_mode_; }
    bool isInitiator() const { return is_initiator_; }
    bool isHandshakeConfirmed() const { return handshake_confirmed_; }
    void setPreferredMode(WaveformMode mode) { config_.preferred_mode = mode; }
    void setModeCapabilities(uint8_t caps) { config_.mode_capabilities = caps; }
    bool isPhyMaskV1Negotiated() const { return phy_mask_v1_negotiated_; }

    // Session-scoped narrowband override (cleared on disconnect/reset)
    // Set when responder detects narrowband chirp — overrides config_.preferred_mode for this session only
    void setNarrowbandOverride(WaveformMode mode) { narrowband_override_ = mode; }

    // Forced data mode - operator can override SNR-based selection
    void setForcedModulation(Modulation mod) { config_.forced_modulation = mod; }
    void setForcedCodeRate(CodeRate rate) { config_.forced_code_rate = rate; }
    void setMCDPSKConfig(int num_carriers, int samples_per_symbol);
    // forced=true marks this as an operator override: the initiator will
    // embed it in CONNECT.data_frame_cw_count and the responder will
    // honor + echo it. forced=false is the boot-time default path used
    // by host wiring (encoder/decoder bootstrap) — does NOT mark forced,
    // so the responder still gets to auto-pick via recommendCWCount(rate).
    void setForcedFrameCodewords(int cw_count, bool forced = true);
    Modulation getForcedModulation() const { return config_.forced_modulation; }
    CodeRate getForcedCodeRate() const { return config_.forced_code_rate; }
    int getForcedFrameCodewords() const { return data_frame_cw_count_; }

    void setSoftCombiningHARQ(bool enable);
    bool getSoftCombiningHARQ() const { return soft_combine_harq_.enabled(); }
    fec::SoftCombineBuffer* softCombineBuffer() { return &soft_combine_harq_; }
    std::optional<fec::SoftCombineBuffer::ProvisionalContext>
    harqProvisionalContext() const;

    using ModeNegotiatedCallback = std::function<void(WaveformMode mode)>;
    void setModeNegotiatedCallback(ModeNegotiatedCallback cb) { on_mode_negotiated_ = cb; }

    // Callback when handshake is confirmed (safe to switch to negotiated waveform)
    // For initiator: called immediately after CONNECT_ACK received
    // For responder: called when first frame received after sending CONNECT_ACK
    using HandshakeConfirmedCallback = std::function<void()>;
    void setHandshakeConfirmedCallback(HandshakeConfirmedCallback cb) { on_handshake_confirmed_ = cb; }

    // Callback when the protocol has just emitted an OFDM ACK that makes the
    // peer's next DATA turn likely to begin with a full OFDM anchor.
    using FullOFDMAnchorExpectedCallback = std::function<void()>;
    void setFullOFDMAnchorExpectedCallback(FullOFDMAnchorExpectedCallback cb) {
        on_full_ofdm_anchor_expected_ = std::move(cb);
    }

    // Callback when the connection-attempt waveform changes.
    using ConnectWaveformChangedCallback = std::function<void(WaveformMode mode)>;
    void setConnectWaveformChangedCallback(ConnectWaveformChangedCallback cb) { on_connect_waveform_changed_ = cb; }

    using PhyMaskV1NegotiatedCallback = std::function<void(bool enabled)>;
    void setPhyMaskV1NegotiatedCallback(PhyMaskV1NegotiatedCallback cb) {
        on_phy_mask_v1_negotiated_ = cb;
    }

    // Get current waveform being used for connection attempts
    WaveformMode getConnectWaveform() const { return connect_waveform_; }

    // Set initial waveform for next connection tests.
    void setInitialConnectWaveform(WaveformMode mode) { connect_waveform_ = mode; }

    // --- Data Mode (modulation + code rate) ---

    Modulation getDataModulation() const { return data_modulation_; }
    CodeRate getDataCodeRate() const { return data_code_rate_; }

    // Set measured SNR from modem layer (call this when decoding frames)
    void setMeasuredSNR(float snr_db, SNRSource source = SNRSource::NONE) {
        if (!std::isfinite(snr_db) || !acceptsRateSelectionSNR(source)) {
            return;
        }
        measured_snr_db_ = snr_db;
        measured_snr_source_ = source;
        measured_snr_valid_ = true;
    }
    float getMeasuredSNR() const { return measured_snr_db_; }
    SNRSource getMeasuredSNRSource() const { return measured_snr_source_; }

    // Set channel quality including fading detection
    // fading_index: combined freq_cv + temporal_cv, where > 0.65 indicates significant fading
    void setChannelQuality(float snr_db, float fading_index,
                           SNRSource source = SNRSource::NONE) {
        if (!std::isfinite(snr_db) || !acceptsRateSelectionSNR(source)) {
            return;
        }
        measured_snr_db_ = snr_db;
        measured_snr_source_ = source;
        measured_snr_valid_ = true;
        if (std::isfinite(fading_index)) {
            fading_index_ = fading_index;
        }
    }
    float getFadingIndex() const { return fading_index_; }
    bool isFading() const { return fading_index_ > 0.65f; }

    // Callback when remote station requests mode change
    // Data-mode-changed callback. cw_count is the negotiated fixed-frame CW
    // count for the new rate (1..8) — host updates encoder/decoder from this
    // value directly. Host MUST NOT call back into ProtocolEngine from this
    // callback (mutex held; re-entry will deadlock).
    using DataModeChangedCallback = std::function<void(Modulation mod, CodeRate rate,
                                                        int cw_count,
                                                        float snr_db, float peer_fading_index,
                                                        int mc_dpsk_num_carriers,
                                                        int mc_dpsk_samples_per_symbol)>;
    void setDataModeChangedCallback(DataModeChangedCallback cb) { on_data_mode_changed_ = cb; }

    // Request mode change to remote station
    void requestModeChange(Modulation new_mod, CodeRate new_rate, float measured_snr, uint8_t reason);

    void reset();

private:
    void setPhyMaskV1Negotiated(bool enabled);
    static bool acceptsRateSelectionSNR(SNRSource source) {
        return source == SNRSource::NONE ||
               source == SNRSource::IDLE_IN_BAND ||
               source == SNRSource::OFDM_BROADBAND ||
               source == SNRSource::MCDPSK_IN_BAND;
    }

    ConnectionConfig config_;
    ConnectionState state_ = ConnectionState::DISCONNECTED;

    // Callsigns
    std::string local_call_;
    std::string remote_call_;
    std::string pending_remote_call_;

    // Remote station hashes (for routing when callsign unknown)
    uint32_t remote_hash_ = 0;
    uint32_t pending_remote_hash_ = 0;

    // Pending forced modes from incoming CONNECT (for manual accept flow)
    Modulation pending_forced_modulation_ = Modulation::AUTO;
    CodeRate pending_forced_code_rate_ = CodeRate::AUTO;
    uint8_t pending_forced_cw_count_ = 0;  // 0 = AUTO (responder chooses)

    // Waveform mode
    WaveformMode narrowband_override_ = WaveformMode::AUTO;  // Session-scoped, cleared on disconnect/reset
    WaveformMode negotiated_mode_ = WaveformMode::OFDM_CHIRP;
    uint8_t remote_capabilities_ = ModeCapabilities::OFDM_CHIRP;
    WaveformMode remote_preferred_ = WaveformMode::OFDM_CHIRP;
    bool phy_mask_v1_negotiated_ = false;

    // Data modulation and code rate (adaptive)
    Modulation data_modulation_ = Modulation::DQPSK;
    CodeRate data_code_rate_ = CodeRate::R1_4;
    int data_frame_cw_count_ = v2::kDefaultFixedFrameCodewords;
    LadderRungId data_ladder_rung_id_ = LadderRungId::UNKNOWN;
    uint16_t mode_change_seq_ = 0;  // Sequence number for MODE_CHANGE frames
    float measured_snr_db_ = 15.0f;  // Routed SNR measured by modem (see source).
    SNRSource measured_snr_source_ = SNRSource::NONE;
    bool measured_snr_valid_ = false;
    float fading_index_ = 0.0f;      // Fading index (0-2, > 0.65 = significant fading)

    // MODE_CHANGE timeout/retry tracking
    bool mode_change_pending_ = false;
    uint32_t mode_change_timeout_ms_ = 0;
    int mode_change_retry_count_ = 0;
    Modulation pending_modulation_ = Modulation::DQPSK;
    CodeRate pending_code_rate_ = CodeRate::R1_4;
    uint8_t pending_cw_count_ = 0;  // 0 = use applyDataMode's default
    LadderRungId pending_ladder_rung_id_ = LadderRungId::UNKNOWN;
    float pending_snr_db_ = 15.0f;
    float pending_fading_index_ = 0.0f;
    uint8_t pending_reason_ = 0;
    static constexpr int MODE_CHANGE_MAX_RETRIES = 2;
    struct ModeChangeAckRepeatJob {
        Bytes frame_data;
        uint16_t seq = 0;
        uint32_t timer_ms = 0;
        int copy_index = 0;
    };
    std::deque<ModeChangeAckRepeatJob> mode_change_ack_repeat_jobs_;

    // ARQ for reliable data transfer (Selective Repeat for higher throughput)
    SelectiveRepeatARQ arq_;
    fec::SoftCombineBuffer soft_combine_harq_;

    // File transfer controller
    FileTransferController file_transfer_;
    std::optional<std::string> queued_file_path_;

    // Message fragmentation (TX) - splits long messages across multiple ARQ frames
    struct QueuedPayload {
        Bytes data;
        bool binary_payload = false;
        uint64_t message_token = 0;
    };
    std::deque<QueuedPayload> queued_payloads_;
    std::vector<Bytes> pending_tx_fragments_;
    std::vector<uint8_t> pending_tx_fragment_flags_;  // Per-fragment flags (for sendMessages batch)
    std::vector<v2::FrameType> pending_tx_fragment_types_;  // DATA_START/CONT/END for binary streams
    std::vector<uint64_t> pending_tx_fragment_message_tokens_;
    size_t next_fragment_idx_ = 0;
    size_t acked_fragment_count_ = 0;  // Actual ACKs received (vs next_fragment_idx_ = submitted)
    struct OutboundMessageTxRecord {
        uint64_t token = 0;
        std::string text;
        size_t expected_fragments = 0;
        size_t assigned_fragments = 0;
        uint16_t first_seq = 0;
        uint16_t last_seq = 0;
        bool first_seq_valid = false;
        bool submitted_reported = false;
        bool terminal_reported = false;
        std::chrono::steady_clock::time_point submitted_at{};
    };
    std::deque<OutboundMessageTxRecord> outbound_message_tx_records_;
    uint64_t next_outbound_message_token_ = 1;
    uint64_t arq_submit_message_token_ = 0;

    // Message reassembly (RX) - accumulates fragments into complete messages
    Bytes rx_reassembly_buffer_;

    // Connection timing
    uint32_t timeout_remaining_ms_ = 0;
    int connect_retry_count_ = 0;
    uint32_t connected_time_ms_ = 0;

    // Disconnect retransmission (initiator side)
    Bytes disconnect_frame_;                // Cached DISCONNECT frame for retransmission
    int disconnect_retry_count_ = 0;
    uint32_t disconnect_retransmit_ms_ = 0; // Time until next retransmit
    static constexpr uint32_t DISCONNECT_RETRANSMIT_INTERVAL_MS = 5000;
    static constexpr int DISCONNECT_MAX_RETRIES = 3;

    // Disconnect grace period (responder side)
    // After receiving DISCONNECT, stay connected briefly and re-send ACK
    // periodically to ensure the initiator gets it (fading can lose frames)
    bool disconnect_pending_ = false;
    uint32_t disconnect_pending_ms_ = 0;
    uint32_t disconnect_ack_retransmit_ms_ = 0; // Time until next ACK re-send
    Bytes disconnect_ack_frame_;            // Cached ACK for re-sending
    static constexpr uint32_t DISCONNECT_GRACE_MS = 5000;            // 5s total grace period
    static constexpr uint32_t DISCONNECT_ACK_RETRANSMIT_MS = 2000;   // Re-send ACK every 2s

    // Calling waveform for PING/CONNECT control frames.
    WaveformMode connect_waveform_ = WaveformMode::MC_DPSK;

    // Statistics
    ConnectionStats stats_;

    // Burst mode TX buffering (OFDM and MC-DPSK DATA)
    std::vector<Bytes> burst_tx_buffer_;
    bool burst_mode_active_ = false;
    TransmitBurstCallback on_transmit_burst_;

    // One-way file-path group stop-and-wait transport (design §14.16). Parallel
    // to the SR-ARQ file path: inert until use_burst_transport_ is enabled and
    // the file TX/RX is routed through it, so it cannot affect the existing modem
    // until GUI-proven. Group-ACK reuses the ACK control frame (seq=group_seq).
    BurstStopAndWaitController burst_transport_;
    bool use_burst_transport_ = false;

    // §14.36 Phase 5c BER-driven per-block rate adaptation. Env ULTRA_ADAPTIVE_RATE=1,
    // default OFF. The SENDER runs the controller on the receiver's per-group decode
    // headroom (carried on the GROUP_ACK; a GROUP_NACK feeds quality 0 -> step down).
    void applyAdaptiveRateFeedback(float quality);
    // Chunk-at-rate: form (or re-form) the in-flight burst group's frames at the
    // CURRENT data_code_rate_ from the raw file payload + cursor. The cursor only
    // advances on a fresh group (after the previous one was acked); a resend
    // re-forms at the new rate from the same cursor.
    bool formAndSendBurstGroup(uint16_t group_seq, bool is_resend);
    bool adaptive_rate_enabled_ = false;
    RateController rate_controller_;
    uint8_t pending_ack_quality_q_ = 0xFF;  // RX: byte to stamp on the next GROUP_ACK
    float last_group_quality_ = -1.0f;      // GUI: most recent group decode headroom
    std::string last_adaptive_action_;      // GUI: short human-readable action
    // Raw file payload (TYPE+OFFSET headers stripped) + a byte cursor used by the
    // chunk-at-rate form fn. Populated by startBurstFileTransfer when adaptive
    // rate is on; the cursor advances by burst_pending_advance_ only on ACK.
    std::vector<uint8_t> burst_file_payload_;
    size_t burst_file_cursor_ = 0;
    size_t burst_pending_advance_ = 0;
    uint16_t burst_chunk_seq_ = 0;
    // FILE_START / FILE_BLOCK metadata chunks held intact (different header from
    // FILE_DATA's TYPE+OFFSET — must NOT be byte-stripped or chunked at-rate).
    // Drained into the FIRST burst group's frame slots (one chunk per frame) so
    // the receiver picks them up via the same burst-deinterleave path as file
    // data — file_transfer_ enters RECEIVING before any FILE_DATA arrives.
    std::deque<Bytes> burst_metadata_queue_;
    size_t burst_pending_metadata_consumed_ = 0;
    // RX group assembly for the burst transport: frames of the in-flight group
    // accumulate here (keyed by the descriptor group_seq) until the group is
    // complete, then are handed to burst_transport_.onGroupReceived().
    uint16_t burst_rx_group_seq_ = 0;
    bool burst_rx_group_open_ = false;
    std::vector<Bytes> burst_rx_group_frames_;

    // ARQ ACK callbacks can acknowledge several slots from one cumulative ACK.
    // Defer window refill until ARQ finishes freeing all slots so OFDM stays
    // burst-oriented instead of collapsing into one-frame steady-state TX.
    bool arq_callback_defer_refill_ = false;
    bool deferred_file_refill_ = false;
    bool deferred_fragment_refill_ = false;

    // In-QSO HF ARQ DATA turn ownership. Exactly one peer is ISS (allowed to
    // originate DATA) at a time; the other queues operator payloads and requests
    // a deterministic changeover via ACK flag or TURN_REQUEST control.
    bool local_data_turn_ = false;
    bool peer_data_turn_requested_ = false;
    bool local_turn_request_pending_ = false;
    bool received_peer_data_since_connect_ = false;
    bool yielded_data_turn_waiting_for_peer_data_ = false;
    bool data_turn_yield_pending_ = false;
    uint64_t data_turn_payload_bytes_sent_ = 0;
    uint32_t data_turn_contended_ms_ = 0;
    uint32_t data_turn_tx_guard_ms_ = 0;
    uint32_t turn_request_retransmit_ms_ = 0;
    uint32_t turn_request_holdoff_ms_ = 0;
    uint32_t file_cancel_rx_drain_ms_ = 0;
    uint32_t file_cancel_reassert_ms_ = 0;
    uint32_t file_cancel_reassert_cooldown_ms_ = 0;
    bool file_cancel_confirm_pending_ = false;
    static constexpr uint32_t DATA_TURN_ACK_DIVERSITY_GUARD_FLOOR_MS = 250;
    static constexpr uint32_t DATA_TURN_CONNECT_GUARD_FLOOR_MS = 500;
    static constexpr uint32_t DATA_TURN_CONTROL_GUARD_FLOOR_MS = 500;
    static constexpr uint32_t TURN_REQUEST_HOLDOFF_FLOOR_MS = 2000;
    static constexpr uint32_t TURN_REQUEST_RETRANSMIT_FLOOR_MS = 2500;
    static constexpr uint32_t FILE_CANCEL_TX_GUARD_FLOOR_MS = 1500;
    static constexpr uint32_t FILE_CANCEL_CONFIRM_DATA_GUARD_FLOOR_MS = 1500;
    static constexpr uint32_t FILE_CANCEL_RX_DRAIN_MS = 5000;
    static constexpr uint32_t FILE_CANCEL_REASSERT_WINDOW_MS = 30000;
    static constexpr uint32_t FILE_CANCEL_REASSERT_COOLDOWN_MS = 1500;
    static constexpr uint64_t DATA_TURN_FAIR_BURST_BYTES = 4096;
    static constexpr uint64_t DATA_TURN_FAIR_MIN_BYTES_FOR_TIME_YIELD = 1024;
    static constexpr uint32_t DATA_TURN_FAIR_BURST_MS = 24000;
    uint32_t currentDataFrameAirtimeMs() const;
    uint32_t currentControlFrameAirtimeMs() const;
    uint32_t currentBurstAnchorAirtimeMs() const;
    uint32_t dataTurnAckDiversityGuardMs(const v2::ControlFrame& ack) const;
    uint32_t dataTurnConnectGuardMs() const;
    uint32_t dataTurnControlGuardMs() const;
    uint32_t turnRequestHoldoffAfterDataMs() const;
    uint32_t turnRequestRetransmitMs() const;
    uint32_t turnRequestAckEmbeddedRetransmitMs() const;
    uint32_t fileCancelTxGuardMs() const;
    uint32_t fileCancelConfirmDataGuardMs() const;
    uint32_t modeChangeRetryMs() const;
    void scheduleModeChangeAckRepeats(const Bytes& ack_data, uint16_t ack_seq);
    void tickModeChangeAckRepeats(uint32_t elapsed_ms);
    uint32_t connectControlFrameAirtimeMs() const;
    uint32_t connectRetryIntervalMs() const;
    uint32_t connectAckRetransmitMs() const;
    int connectAckRetxBudget() const;
    uint32_t responderHandshakeFailSafeMs() const;

    void transmitFrameBatch(const std::vector<Bytes>& frame_data_list);
    void flushBurstBuffer();
    void processArqFrame(const Bytes& frame_data);
    void runDeferredArqRefill();
    void configureArqForCurrentDataMode();
    void configureSoftCombineHARQBounds();
    uint32_t pingTimeoutMsForCurrentProfile() const;
    bool usesBoundedVariableMCDPSKFrames() const;
    size_t currentDataPayloadCapacity() const;
    // Apply a new data mode. cw_count: 0 = compute via recommendCWCount(rate),
    // 1..8 = explicit (used when MODE_CHANGE wire byte specifies a value).
    void applyDataMode(Modulation mod, CodeRate rate, int cw_count = 0,
                       LadderRungId rung_id = LadderRungId::UNKNOWN);
    void commitPendingModeChange(const char* outcome);
    void notifyDataModeChanged(float snr_db, float peer_fading_index);
    LadderRungId currentLadderRungId() const;
    void resetAdaptiveModeController();
    void updateAdaptiveModeController(uint32_t elapsed_ms);
    bool tryIssueAdaptiveModeChangeAtBoundary();
    bool canIssueAdaptiveModeChange(bool is_downgrade) const;
    bool hasAdaptiveUpgradeBacklog(CodeRate target_rate) const;
    size_t adaptiveBacklogFrames(CodeRate rate) const;

    struct AdaptiveModeTarget {
        bool pending = false;
        Modulation modulation = Modulation::DQPSK;
        CodeRate rate = CodeRate::R1_4;
        uint8_t reason = v2::ModeChangeReason::CHANNEL_IMPROVED;
    };
    AdaptiveModeTarget adaptive_target_;
    ARQStats adaptive_last_stats_;
    uint32_t adaptive_eval_elapsed_ms_ = 0;
    uint32_t adaptive_cooldown_ms_ = 0;
    uint32_t adaptive_post_downgrade_lockout_ms_ = 0;
    uint32_t adaptive_downgrade_queue_age_ms_ = 0;
    int adaptive_clean_windows_ = 0;
    int adaptive_pressure_windows_ = 0;
    static constexpr uint32_t ADAPTIVE_EVAL_INTERVAL_MS = 1000;
    static constexpr uint32_t ADAPTIVE_MODE_CHANGE_COOLDOWN_MS = 30000;
    static constexpr uint32_t ADAPTIVE_POST_DOWNGRADE_LOCKOUT_MS = 45000;
    static constexpr uint32_t ADAPTIVE_DOWNGRADE_FORCE_MS = 6000;
    static constexpr int ADAPTIVE_CLEAN_WINDOWS_FOR_UPGRADE = 15;
    static constexpr int ADAPTIVE_PRESSURE_WINDOWS_FOR_DOWNGRADE = 2;
    static constexpr size_t ADAPTIVE_UPGRADE_BACKLOG_WINDOWS = 2;

    // Callbacks
    TransmitCallback on_transmit_;
    TransmitInfoCallback on_transmit_info_;
    ConnectedCallback on_connected_;
    DisconnectedCallback on_disconnected_;
    MessageReceivedCallback on_message_received_;
    MessageSentCallback on_message_sent_;
    MessageTxStatusCallback on_message_tx_status_;
    IncomingCallCallback on_incoming_call_;
    DataReceivedCallback on_data_received_;
    ModeNegotiatedCallback on_mode_negotiated_;
    DataModeChangedCallback on_data_mode_changed_;
    ConnectWaveformChangedCallback on_connect_waveform_changed_;
    PhyMaskV1NegotiatedCallback on_phy_mask_v1_negotiated_;
    HandshakeConfirmedCallback on_handshake_confirmed_;
    FullOFDMAnchorExpectedCallback on_full_ofdm_anchor_expected_;
    PingTxCallback on_ping_tx_;
    PingReceivedCallback on_ping_received_;
    StateChangedCallback on_state_changed_;

    // Probing state (PING/PONG fast presence check)
    int ping_retry_count_ = 0;
    static constexpr int MAX_PING_RETRIES = 5;  // Try 5 pings before giving up
    static constexpr uint32_t PING_TIMEOUT_MS = 8000;  // 8 seconds per ping (PING=3.3s + PONG=3.3s + margin)
    static constexpr uint32_t ROBUST_LOW_PING_TIMEOUT_MS = 20000;

    // Handshake state - responder waits for first frame before confirming
    bool is_initiator_ = false;           // True if we initiated the connection
    bool handshake_confirmed_ = false;    // True after handshake is fully confirmed
    uint32_t responder_handshake_wait_ms_ = 0;  // Fail-safe timer for responder handshake
    static constexpr uint32_t RESPONDER_HANDSHAKE_FAILSAFE_MS = 2200;

    // CONNECT_ACK retransmission (responder side, BUG-CTRL-001)
    // ALPHA can fail to decode MC-DPSK CONNECT_ACK on faded seeds. Retry timing
    // is derived from the current MC-DPSK control-frame airtime so the same rule
    // scales with slower robust profiles and faster audited profiles.
    Bytes connect_ack_frame_;                  // Cached CONNECT_ACK for re-sending
    uint32_t connect_ack_retransmit_ms_ = 0;   // Time until next retransmit
    int connect_ack_retx_remaining_ = 0;       // Retries left (counts down to 0)

    // Internal handlers for v2 frames
    void handleConnect(const v2::ConnectFrame& frame, const std::string& src_call);
    void handleConnectAck(const v2::ConnectFrame& frame, const std::string& src_call);
    void handleConnectNak(const v2::ConnectFrame& frame, const std::string& src_call);
    void handleDisconnect(const v2::ControlFrame& frame, const std::string& src_call);
    void handleDisconnectFrame(const v2::ConnectFrame& frame, const std::string& src_call);
    void handleModeChange(const v2::ControlFrame& frame, const std::string& src_call);
    bool sendPayload(const Bytes& data, bool binary_payload);
    bool startPayloadNow(const Bytes& data, bool binary_payload, uint64_t message_token = 0);
    uint64_t createOutboundMessageRecord(const Bytes& data);
    void setOutboundMessageExpectedFragments(uint64_t token, size_t fragments);
    void dropOutboundMessageRecord(uint64_t token);
    void clearOutboundMessageTracking();
    bool sendArqPayloadFrame(const Bytes& chunk, v2::FrameType frame_type, uint8_t flags,
                             bool fixed_frame, uint64_t message_token);
    void handleArqFrameSubmitted(uint16_t seq);
    void handleArqTxBaseAdvanced(uint16_t base_seq);
    void handleArqFrameFailed(uint16_t seq);
    void emitMessageTxStatus(OutboundMessageTxRecord& record, MessageTxStatus status);
    bool shouldQueuePayloadForLinkTurn() const;
    bool hasLocalOutboundDataTurn() const;
    bool hasLocalInFlightDataTurn() const;
    bool hasLocalDataWaitingForTurn() const;
    bool dataTurnFairBudgetMet() const;
    bool shouldPauseLocalDataForPeerRequest() const;
    bool shouldRequestDataTurnOnAck() const;
    bool noteTurnRequestOnAckIfNeeded();
    void resetDataTurnFairness();
    void noteDataTurnPayloadStarted(size_t payload_bytes);
    void sendTurnRequestIfNeeded();
    bool maybeYieldDataTurn();
    void armDataTurnTxGuard(uint32_t guard_ms);
    void transmitFileCancelControl(const char* reason);
    void armFileCancelReassertion();
    void clearFileCancelReassertion();
    void maybeReassertFileCancelForStaleData();
    bool startFileTransferNow(const std::string& filepath);
    bool tryStartQueuedFileIfReady();
    void sendNextQueuedPayloadIfReady();
    void clearFileTransferArqState();
    void handleDataPayload(const Bytes& payload, bool more_data, v2::FrameType frame_type);
    void handleTurnover(const v2::ControlFrame& frame, const std::string& src_call);
    void handleTurnRequest(const v2::ControlFrame& frame, const std::string& src_call);
    void handleFileCancel(const v2::ControlFrame& frame, const std::string& src_call);

    void transmitFrame(const Bytes& frame_data);
    void enterConnected();
    void enterDisconnected(const std::string& reason);
    void sendFullConnect();  // Send full CONNECT frame after successful PING/PONG
    void cancelOutboundProbe();
    void cancelOutboundConnect();

    WaveformMode negotiateMode(uint8_t remote_caps, WaveformMode remote_pref);
    void sendNextFileChunk();
    void sendNextFragment();

    // One-way burst transport (design §14.27). startBurstFileTransfer drains the
    // whole file into BURST_GROUP_SIZE-frame interleaved groups and hands them to
    // burst_transport_ (group stop-and-wait, no SR-ARQ). collectBurstGroupFrame
    // feeds a decoded burst frame into RX group assembly; on a complete group it
    // calls burst_transport_.onGroupReceived(). Both are gated by
    // use_burst_transport_.
    bool startBurstFileTransfer();
    void collectBurstGroupFrame(uint16_t group_seq, const Bytes& frame_data,
                                bool group_complete);
};

} // namespace protocol
} // namespace ultra
