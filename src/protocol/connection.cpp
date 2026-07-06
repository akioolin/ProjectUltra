// Connection state machine - core logic
// Frame handlers are in connection_handlers.cpp

#include "connection.hpp"
#include "connection_policy.hpp"
#include "waveform_selection.hpp"
#include "ultra/logging.hpp"
#include <algorithm>
#include <cctype>
#include <limits>
#include <filesystem>

namespace ultra {
namespace protocol {

namespace {
constexpr size_t kOFDMFileBlockPayloadLimit = 2300;
constexpr const char* kOFDMBurstPadCallsign = "ULPAD";
constexpr uint16_t kOFDMBurstPadSeq = 0xFFFE;
constexpr size_t kMaxQueuedPayloads = 32;

// TRANSPORT MERGE (step 1): opt-in tone-burst ACK on the interactive SR-ARQ path.
constexpr uint32_t kInteractiveToneAckWindowMs = 8000;  // floor: monitor arm window for the ack
// TRANSPORT MERGE (2026-06-06): the unified arq_ path is now THE OFDM file/message
// transport — one 16-bit seq space, one tone-burst ack, one retransmit window. The
// file still bursts+interleaves via sendNextFileChunk()->flushBurstBuffer(); arq_ owns
// sequencing/dedup/retransmit and the RX delivers through processArqFrame. The legacy
// burst_transport_ group controller (BurstStopAndWaitController) is removed — there is
// exactly ONE group-generation path. These two helpers are now unconditionally true
// (kept as named call sites during the cleanup; inline + delete in a follow-up).
// Proof: /tmp/unified_multiseed.sh — AWGN R3/4+R2/3, Good R2/3 CRC-clean, legacy_calls=0.
bool kInteractiveToneAckEnabled() { return true; }
bool kUnifiedSeqEnabled() { return true; }

Modulation wideOFDMControlModulationForData(Modulation data_modulation) {
    return ofdm_link_adaptation::isCoherentModulation(data_modulation)
        ? Modulation::QPSK
        : Modulation::DQPSK;
}

uint32_t ackRepeatDelayForControlAirtimeMs(uint32_t control_airtime_ms) {
    return control_airtime_ms +
           selective_repeat_arq_policy::kAckRepeatMaxJitterMs +
           connection_policy::kCarrierSenseSackCoalesceMs;
}

Bytes makeOFDMBurstPadPayload(CodeRate rate, int cw_count, size_t pad_index) {
    const size_t capacity = v2::getFixedFramePayloadCapacity(rate, cw_count);
    Bytes payload(capacity);
    if (payload.empty()) {
        return payload;
    }

    // Fill the dummy frame instead of sending an empty DATA payload. Empty pad
    // frames leave most fixed-frame info bits as all-zero LDPC codewords, which
    // are prone to ugly "4/4 CWs OK but frame invalid" tail artifacts in fading.
    uint32_t x = 0xA5C35A7Du ^
                 (static_cast<uint32_t>(rate) << 24) ^
                 static_cast<uint32_t>(pad_index * 0x9E3779B1u);
    for (size_t i = 0; i < payload.size(); ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        payload[i] = static_cast<uint8_t>(x & 0xFF);
    }
    payload[0] = 0x7F;  // reserved dummy discriminator if a hash collision ever delivers it
    return payload;
}

bool shouldUseSingleOFDMFileBlock(float fading_index, float snr_db, CodeRate rate) {
    const auto* descriptor = ofdmCodeRateDescriptor(rate);
    const auto* single_block_floor = ofdmCodeRateDescriptor(CodeRate::R2_3);
    return connection_policy::isNearAwgnOFDM(fading_index, snr_db) &&
           descriptor != nullptr && single_block_floor != nullptr &&
           descriptor->code_rate >= single_block_floor->code_rate;
}

bool shouldPadPartialOFDMBurst(WaveformMode mode,
                               Modulation modulation,
                               CodeRate rate,
                               float fading_index,
                               float snr_db,
                               FileTransferState file_state,
                               size_t burst_frames) {
    if (!isOFDMMode(mode)) {
        return false;
    }
    if (file_state != FileTransferState::SENDING) {
        return connection_policy::isHighThroughputOFDMMode(modulation, rate) &&
               connection_policy::shouldPadBurstInterleaveGroup(burst_frames);
    }

    return connection_policy::shouldPadHighRateFadingBurst(
        modulation,
        rate,
        connection_policy::isNearAwgnOFDM(fading_index, snr_db),
        burst_frames);
}

bool isFinalDataFrame(const Bytes& frame_data) {
    auto hdr = v2::parseHeader(frame_data);
    return hdr.valid &&
           hdr.type == v2::FrameType::DATA &&
           frame_data.size() > 3 &&
           ((frame_data[3] & v2::Flags::FINAL) != 0);
}

bool seqBefore(uint16_t a, uint16_t b) {
    return a != b && static_cast<uint16_t>(b - a) < 0x8000;
}

float modeEfficiency(Modulation mod, CodeRate rate) {
    return estimateWideOFDMRawBps(mod, rate);
}

bool isFasterMode(Modulation candidate_mod, CodeRate candidate_rate,
                  Modulation current_mod, CodeRate current_rate) {
    return modeEfficiency(candidate_mod, candidate_rate) >
           modeEfficiency(current_mod, current_rate) + 0.05f;
}

bool isNormalArqAckFrame(const Bytes& frame_data) {
    if (frame_data.size() < v2::ControlFrame::SIZE ||
        static_cast<v2::FrameType>(frame_data[2]) != v2::FrameType::ACK) {
        return false;
    }

    const uint16_t seq =
        (static_cast<uint16_t>(frame_data[4]) << 8) | frame_data[5];
    return seq != 0xFFFF;
}

bool expectsFullOFDMAnchorAfterTx(const Bytes& frame_data) {
    if (isNormalArqAckFrame(frame_data)) {
        return true;
    }
    if (frame_data.size() < v2::ControlFrame::SIZE) {
        return false;
    }

    const auto type = static_cast<v2::FrameType>(frame_data[2]);
    return type == v2::FrameType::TURNOVER ||
           type == v2::FrameType::TURN_REQUEST ||
           type == v2::FrameType::FILE_CANCEL;
}

}

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
    data_frame_cw_count_ = v2::sanitizeFixedFrameCodewords(config_.fixed_frame_codewords);
    config_.fixed_frame_codewords = data_frame_cw_count_;
    arq_.setFixedFrameCodewords(data_frame_cw_count_);

    // §14.27: burst transport is THE OFDM-wideband file path — UNCONDITIONAL, no env
    // gate (2026-06-02; the ULTRA_BURST_TRANSPORT opt-out was removed — burst is the
    // only valid file method now). `use_burst_transport_` stays initialized true; the
    // legacy windowed-file `!use_burst_transport_` branches are now dead code (R1
    // deletion follow-up). NOTE: burst is itself selective-repeat (GROUP_ACK carries
    // the 16-bit SACK frame_mask) — SelectiveRepeatARQ (`arq_`) still serves MC-DPSK/
    // narrow/control; this is NOT "remove SR-ARQ".
    // §14.36 Phase 5c: per-block decode-headroom quality feedback. Default ON (drives the GUI
    // "Adapt:" bar + diagnostics on sim AND hardware); opt OUT with ULTRA_ADAPTIVE_RATE=0
    // (which also disables the rate-change path). The actual rate CHANGE is separately gated by
    // rateAdaptationActive() — default-ON for connected wideband OFDM since 2026-07-02
    // (fade-riding ladder); ULTRA_RATE_ADAPT=0 / ULTRA_LOCK_RATE=1 opt out.
    if (const char* ar = std::getenv("ULTRA_ADAPTIVE_RATE"); ar && ar[0] == '0') {
        adaptive_rate_enabled_ = false;
    }

    // DESC-SWITCH Phase 1 (docs/MODE_SWITCH_PIGGYBACK_DESIGN_2026_07_03.md §5.1, knob
    // ULTRA_DESCRIPTOR_MODE_SWITCH, read once per Connection like ULTRA_ARQ_MOVE_EPOCH;
    // default OFF = byte-identical): clean-boundary wideband-OFDM ladder moves commit
    // LOCALLY and ride the next BURST_HEADER descriptor instead of the MODE_CHANGE
    // stop-and-wait exchange. SEMANTICS-BREAKING lockstep when ON (both ends must be
    // built + enabled; same increment policy as move-epoch/tone-payload).
    if (const char* ds = std::getenv("ULTRA_DESCRIPTOR_MODE_SWITCH"); !(ds && ds[0] == '0')) {  // DEFAULT-ON 2026-07-05
        descriptor_mode_switch_enabled_ = true;
    }

    // RX-RATE-CMD Phase 2 (design §5.2, knob ULTRA_RX_RATE_CMD, read once per
    // Connection; default OFF = byte-identical — rung_cmd bits stay 0 AND the tone-ACK
    // CRC span stays 28 bits, see tone_burst_payload.cpp rungCmdCrcSpanEnabled()).
    // SEMANTICS-BREAKING lockstep when ON; the descriptor-committed consume path
    // additionally needs ULTRA_DESCRIPTOR_MODE_SWITCH (+ ULTRA_ARQ_MOVE_EPOCH for
    // mid-window) — it falls back to the legacy MODE_CHANGE exchange without them.
    if (const char* rc = std::getenv("ULTRA_RX_RATE_CMD"); !(rc && rc[0] == '0')) {  // DEFAULT-ON 2026-07-05 (voice; demote-cmd inert under authority)
        rx_rate_cmd_enabled_ = true;
    }

    // Wire up ARQ callbacks
    arq_.setTransmitCallback([this](const Bytes& data) {
        transmitFrame(data);
        // A TIMEOUT-RETRANSMIT of a single in-flight DATA frame goes out right here but
        // does NOT hit setTxFrameSubmittedCallback (that fires only on NEW submits), so
        // without this it never (re)arms the tone-burst ack monitor → we can't hear the
        // peer's ack for the resend → we time out and resend AGAIN (observed: a faded
        // message fragment resent 3× while BRAVO acked each time). Arm here too so every
        // transmitted DATA frame — new or retransmit — listens for its tone-burst ack.
        // (Redundant for new burst frames, which also arm via setTxFrameSubmittedCallback;
        // re-arming only extends the window and the monitor auto-disarms on decode.)
        armToneBurstAckListenWindow();
    });

    arq_.setDataReceivedCallback([this](const Bytes& data) {
        handleDataPayload(data, arq_.lastRxHadMoreData(), arq_.lastRxFrameType());
    });

    arq_.setReceiveWindowAdvancedCallback([this](uint16_t base_seq, size_t window_size) {
        soft_combine_harq_.retainOnlySeqWindow(base_seq, window_size);
    });
    arq_.setTxFrameSubmittedCallback([this](uint16_t seq) {
        handleArqFrameSubmitted(seq);
        // TRANSPORT MERGE (step 1): after submitting interactive DATA, arm the
        // tone-burst monitor so we hear the peer's tone-burst ACK (it will emit one
        // instead of a SACK frame). Re-arming on each frame just extends the window;
        // the monitor auto-disarms on a successful decode.
        //
        // The window must cover the FULL round-trip from TX-start, and the ack does NOT
        // arrive until the peer has fully RECEIVED the burst (real-time airtime) AND
        // LDPC-decoded it — for a multi-frame OFDM burst that is many seconds, far past
        // the fixed 8 s floor (which only ever fit a 1-2 frame interactive message). The
        // exact, already-derived bound is the ARQ retransmit timeout: listen for the ack
        // for as long as we'd wait before resending. It is burst-aware (burst airtime +
        // decode-jitter margin + ack tail), so the monitor can never expire mid-round-
        // trip; it auto-disarms the instant the ack decodes. Floor at the interactive
        // value so short MC-DPSK/interactive sends are unchanged.
        armToneBurstAckListenWindow();
    });
    arq_.setTxBaseAdvancedCallback([this](uint16_t base_seq) {
        handleArqTxBaseAdvanced(base_seq);
    });
    arq_.setTxFrameFailedCallback([this](uint16_t seq) {
        handleArqFrameFailed(seq);
    });
    arq_.setTurnRequestCallback([this]() {
        return noteTurnRequestOnAckIfNeeded();
    });

    // TRANSPORT MERGE (step 1, env ULTRA_TONE_ACK_INTERACTIVE): route the interactive
    // SACK through the same tone-burst transport the burst path uses. When enabled, the
    // receiver emits the ack as a tone-burst (low-6 base_seq + the RX bitmap truncated
    // to the 16-bit wire mask) instead of a SACK control frame; the sender arms its
    // monitor (above) and consumes it in onToneBurstAck(). Default off — the legacy
    // SACK-frame path is unchanged.
    if (kInteractiveToneAckEnabled()) {
        arq_.setEmitToneBurstSackCallback(
            [this](uint16_t base_seq, uint32_t bitmap, bool /*has_final*/,
                   uint8_t move_epoch) {
                if (!on_transmit_tone_burst_ack_) {
                    return;
                }
                ultra::waveform::tone_burst_ack::ToneBurstAckPayload tba;
                // frame_mask width tracks the wire layout (16 bits as of 2026-07-02) so the
                // SACK can address a 16-frame in-flight window — see kToneBurstAckWindowCapFrames.
                constexpr uint32_t kFrameMaskWire =
                    (1u << ultra::waveform::tone_burst_ack::kPayloadFrameMaskBits) - 1u;
                tba.group_seq = static_cast<uint8_t>(base_seq & 0x3F);
                tba.frame_mask = static_cast<uint16_t>(bitmap & kFrameMaskWire);
                tba.type = ultra::waveform::tone_burst_ack::AckType::Ack;
                // MOVE-EPOCH echo (ULTRA_ARQ_MOVE_EPOCH, BUG-ARQ-SEQ-COLLISION):
                // payload bits 40-41; always 0 while the knob is OFF (byte-identical).
                tba.move_epoch = move_epoch;
                // RX-RATE-CMD Phase 2 (ULTRA_RX_RATE_CMD): payload bits 42-43 — the
                // standing receiver rung command computed per group event in
                // updateRxRateCommandFromGroup (crater-only DOWN-hard). Only ever
                // non-zero when the knob is ON (byte-identical OFF); re-emitted ACK
                // copies re-carry the same command, the sender dedups by group_seq.
                tba.rung_cmd = rx_rate_cmd_pending_;
                // §14.43: carry the receiver's last measured group decode headroom [0,1] back to
                // the sender, quantized into the 3-bit rate_hint (0..7). The sender de-quantizes it
                // in onToneBurstAck and feeds its RateController. -1 (no sample yet) -> 0.
                tba.rate_hint = (last_group_quality_ >= 0.0f)
                    ? static_cast<uint8_t>(std::lround(
                          std::clamp(last_group_quality_, 0.0f, 1.0f) * 7.0f))
                    : 0;
                // RX-AUTHORITY (ULTRA_RX_RATE_AUTHORITY): reinterpret the SAME five
                // bits [rate_hint(3)|rung_cmd(2)] as the receiver's ABSOLUTE canonical
                // rung command (waveform_selection.hpp kRungIdx*; 0 = no command).
                // Overrides the hint/relative-cmd stamps above — under authority the
                // sender's EMA has no consumer for the hint, and the demote-only
                // relative command is superseded by the absolute one. The WAITING-
                // REBASE voice is untouched (type=NACK path, emitted elsewhere).
                if (rxRateAuthorityEnabled()) {
                    tba.rate_hint = static_cast<uint8_t>(rx_authority_cmd_ & 0x7);
                    tba.rung_cmd = static_cast<uint8_t>((rx_authority_cmd_ >> 3) & 0x3);
                }
                // Software-ALC (BUG-QAM16-RIG-LEVEL-BUDGET): stamp the drive advisory
                // from the per-burst RX level verdict fed via setRxLevelVerdict just
                // before this group's delivery. Down IMMEDIATELY on a clip signature
                // (fast attack); up only after kAlcLowStreakForUp consecutive fresh
                // LOW verdicts (fade hysteresis, slow release). ULTRA_SOFTWARE_ALC=0
                // pins the advisory to hold (the receiver advisory LOG still runs in
                // the decoder). Repeated ACK emits for the same group re-carry the
                // same advisory; the sender dedups by group_seq.
                if (connection_policy::softwareAlcEnabled()) {
                    if (rx_level_clipped_) {
                        tba.drive_advisory =
                            ultra::waveform::tone_burst_ack::kDriveAdvisoryDown;
                    } else if (rx_level_low_streak_ >=
                                   connection_policy::kAlcLowStreakForUp &&
                               last_group_quality_ > 0.0f) {
                        // ALC RUNAWAY GUARD (2026-07-04, F18 forensics): a LOW level
                        // reading is drive evidence ONLY while frames are DECODING at
                        // that level (genuinely level-starved but workable chain). A
                        // LOW reading on a zero-delivery group is a FADE TROUGH — the
                        // ladder owns fades; drive must not chase them. Without this
                        // gate every trough ratcheted the peer's tx_drive up (0.63 ->
                        // the 0.85 cap by t=171), TX compression on the cheap card
                        // then cratered the thin-margin rungs at 30+ dB readings, and
                        // no RX clip signature ever brought the drive back down.
                        tba.drive_advisory =
                            ultra::waveform::tone_burst_ack::kDriveAdvisoryUp;
                    }
                }
                on_transmit_tone_burst_ack_(tba);
            });
    }

    arq_.setSendCompleteCallback([this](bool success) {
        if (file_transfer_.getState() == FileTransferState::SENDING) {
            if (success) {
                file_transfer_.onChunkAcked();
                if (arq_callback_defer_refill_) {
                    deferred_file_refill_ = true;
                } else {
                    sendNextFileChunk();
                }
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
                pending_tx_fragment_types_.clear();
                pending_tx_fragment_message_tokens_.clear();
                next_fragment_idx_ = 0;
                acked_fragment_count_ = 0;
                if (on_message_sent_) {
                    on_message_sent_(false);
                }
            } else {
                acked_fragment_count_++;

                if (next_fragment_idx_ < pending_tx_fragments_.size()) {
                    // More fragments to submit to ARQ
                    if (arq_callback_defer_refill_) {
                        deferred_fragment_refill_ = true;
                    } else {
                        sendNextFragment();
                    }
                }

                if (acked_fragment_count_ >= pending_tx_fragments_.size()) {
                    // ALL fragments truly ACKed
                    LOG_MODEM(INFO, "Connection: All %zu fragments sent and ACKed",
                              pending_tx_fragments_.size());
                    pending_tx_fragments_.clear();
                    pending_tx_fragment_flags_.clear();
                    pending_tx_fragment_types_.clear();
                    pending_tx_fragment_message_tokens_.clear();
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
    timeout_remaining_ms_ = pingTimeoutMsForCurrentProfile();
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

    // Use centralized algorithm from waveform_selection.hpp. #58: the selection value
    // is basis-corrected (fade-effective reading vs dial-calibrated anchors). Increment
    // 3: snr_db is the connect-SNR-pool aggregate when ULTRA_CONNECT_SNR_POOL is set
    // (clustered dB-mean of the handshake's data-aided readings — same population the
    // +5 basis was calibrated on, so the basis composes unchanged and is applied ONCE,
    // downstream); knob-off it is exactly the raw measured_snr_db_ scalar.
    const float snr_db = rateSelectionSnrDb();
    // Increment 4: entry-pick fading is pooled like the SNR (single-frame fading
    // scatters 0.24-0.74 across the 0.65 boundary at Watterson Good); knob-off
    // this IS fading_index_, byte-identical.
    const float entry_fading_raw = rateSelectionFadingIndex();
    // Entry classification shrinkage — same rationale as handleConnect (see the
    // helper's provenance comment); knob-off keeps the raw scalar path.
    const float entry_fading =
        connection_policy::connectSnrPoolEnabled()
            ? connection_policy::entryClassificationFadingIndex(
                  entry_fading_raw,
                  connect_snr_pool_.effectiveCount(
                      connectSnrPoolTcMs(), /*handshake_only=*/true,
                      /*max_age_ms=*/UINT64_MAX))
            : entry_fading_raw;
    const bool accept_snr_data_aided = rateSelectionSnrDataAided();
    const float accept_selection_snr_db =
        connection_policy::connectSelectionSnrDb(snr_db, entry_fading,
                                                 accept_snr_data_aided);
    recommendDataMode(accept_selection_snr_db, negotiated_mode_, rec_mod, rec_rate, entry_fading);

    // Bootstrap safety: the connect-time reading can overestimate first OFDM frame
    // quality (historically the chirp snapshot; since #58 it is the data-aided
    // fade-averaged estimate). ULTRA_ENTRY_CAP_R34 (default OFF) lets a data-aided
    // reading clearing the R3/4 anchor by >= 1 sigma enter at R3/4.
    if (isOFDMMode(negotiated_mode_)) {
        CodeRate capped = capInitialOFDMRate(accept_selection_snr_db, entry_fading, rec_rate, rec_mod,
                                             accept_snr_data_aided);
        if (capped != rec_rate) {
            LOG_MODEM(INFO, "Connection: Bootstrap cap %s -> %s for initial OFDM setup (SNR=%.1f (%s), fading=%.2f)",
                      codeRateToString(rec_rate), codeRateToString(capped), snr_db,
                      snrSourceToString(measured_snr_source_), entry_fading);
            rec_rate = capped;
        }
    }

    // ULTRA_ENTRY_QAM16_SNR (experiment): start AT 16QAM R2/3 on a strong Good-class
    // connect instead of QPSK-and-climb (the fade-riding strategy). AFTER the bootstrap
    // cap, BEFORE forced overrides. Mirrors the responder site in connection_handlers.cpp.
    if (isOFDMMode(negotiated_mode_) &&
        entryQam16Promote(accept_selection_snr_db, entry_fading, rec_mod,
                          accept_snr_data_aided)) {
        LOG_MODEM(INFO,
                  "Connection: ENTRY-QAM16 promote %s %s -> 16QAM R2/3 (data-aided SNR=%.1f, fading=%.2f)",
                  modulationToString(rec_mod), codeRateToString(rec_rate),
                  accept_selection_snr_db, entry_fading);
        rec_mod = Modulation::QAM16;
        rec_rate = CodeRate::R2_3;
    }

    // 2026-05-28 experiment (env-gated): the industry leader's tactical ladder
    // picks QPSK R2/3 (~3230 bps net) as its 3000 bps speed slot, not R3/4
    // or R5/6. Stronger FEC -> fewer drop-on-timeout cascades -> higher
    // *effective* e2e throughput. ULTRA_MAX_OFDM_RATE=R2_3 caps both initial
    // selection AND adaptive climb at R2/3 to test this hypothesis. No-op when
    // the env is unset (default ladder unchanged).
    if (const char* env = std::getenv("ULTRA_MAX_OFDM_RATE")) {
        const std::string s(env);
        const CodeRate cap = (s == "R1_2" || s == "r1_2") ? CodeRate::R1_2
                           : (s == "R2_3" || s == "r2_3") ? CodeRate::R2_3
                           : (s == "R3_4" || s == "r3_4") ? CodeRate::R3_4
                           : CodeRate::AUTO;  // anything else = no cap (AUTO sentinel)
        if (cap != CodeRate::AUTO && rec_rate > cap) {
            LOG_MODEM(INFO, "Connection: ULTRA_MAX_OFDM_RATE cap %s -> %s",
                      codeRateToString(rec_rate), codeRateToString(cap));
            rec_rate = cap;
        }
    }

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

    // Pick negotiated CW count (honor initiator's forced value, else auto).
    // Computed BEFORE building CONNECT_ACK so the embedded byte and the
    // initiator's view match what we'll actually use locally.
    int negotiated_cw = (pending_forced_cw_count_ != 0)
        ? v2::sanitizeFixedFrameCodewords(pending_forced_cw_count_)
        : connection_policy::recommendCWCountForChannel(
              rec_mod, rec_rate, negotiated_mode_, entry_fading, snr_db);

    // Clear pending forced modes
    pending_forced_modulation_ = Modulation::AUTO;
    pending_forced_code_rate_ = CodeRate::AUTO;
    pending_forced_cw_count_ = 0;

    LadderRungId rung_id = LadderRungId::UNKNOWN;
    if (negotiated_mode_ == WaveformMode::MC_DPSK) {
        rung_id = connection_policy::rungForMCDPSKConfig(
            rec_mod, config_.mc_dpsk_num_carriers,
            config_.mc_dpsk_samples_per_symbol, negotiated_cw).id;
    } else if (negotiated_mode_ == WaveformMode::OFDM_CHIRP) {
        rung_id = LadderRungId::OFDM_CHIRP;
    } else if (negotiated_mode_ == WaveformMode::OFDM_NARROW) {
        rung_id = LadderRungId::OFDM_NARROW;
    }

    // 2026-05-28: ULTRA_FRAME_CW env override for reliability sweep. Allows
    // pinning cw count per frame in [1, kMaxFixedFrameCodewords]. Default unset
    // = use the negotiated value as before.
    if (const char* env = std::getenv("ULTRA_FRAME_CW")) {
        const int v = std::atoi(env);
        if (v >= v2::kMinFixedFrameCodewords && v <= v2::kMaxFixedFrameCodewords) {
            LOG_MODEM(INFO, "Connection: ULTRA_FRAME_CW override %d -> %d",
                      negotiated_cw, v);
            negotiated_cw = v;
        }
    }

    // Set our local data mode immediately.
    applyDataMode(rec_mod, rec_rate, negotiated_cw, rung_id);

    LOG_MODEM(INFO, "Connection: Accepting call from %s (waveform=%s, data=%s %s, cw=%d)",
              remote_call_.c_str(), waveformModeToString(negotiated_mode_),
              modulationToString(data_modulation_), codeRateToString(data_code_rate_),
              data_frame_cw_count_);

    // CONNECT_ACK wire bytes: the pool aggregates under ULTRA_CONNECT_SNR_POOL (always
    // fresh at this instant by construction — never the stale sentinel), else the raw
    // scalars exactly as before. SNR and fading get the SAME treatment so the
    // initiator's display shows the values that actually drove this pick.
    auto ack = v2::ConnectFrame::makeConnectAck(local_call_, remote_call_,
                                                 static_cast<uint8_t>(negotiated_mode_),
                                                 data_modulation_, data_code_rate_,
                                                 snr_db, entry_fading,
                                                 static_cast<uint8_t>(data_frame_cw_count_),
                                                 rung_id);
    Bytes ack_data = ack.serialize();
    connect_ack_frame_ = ack_data;
    connect_ack_retransmit_ms_ = connectAckRetransmitMs();
    connect_ack_retx_remaining_ =
        negotiated_mode_ == WaveformMode::OFDM_CHIRP ? connectAckRetxBudget() : 0;
    const uint32_t responder_handshake_failsafe_ms = responderHandshakeFailSafeMs();

    LOG_MODEM(INFO, "Connection: Sending CONNECT_ACK (%zu bytes, SNR=%.1f dB (%s))",
              ack_data.size(), snr_db, snrSourceToString(measured_snr_source_));
    transmitFrame(ack_data);

    // We are the responder - we received CONNECT and are sending CONNECT_ACK
    is_initiator_ = false;
    handshake_confirmed_ = false;  // Responder waits for first frame to confirm
    responder_handshake_wait_ms_ = responder_handshake_failsafe_ms;

    enterConnected();

    // Notify application of initial data mode (LOCAL reading — responder pick;
    // entry_fading = the pooled value that drove the pick, raw scalar knob-off)
    notifyDataModeChanged(snr_db, entry_fading, /*snr_is_wire=*/false);
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

        auto disc = v2::ControlFrame::makeDisconnect(local_call_, remote_call_);
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

void Connection::abortTxNow() {
    // Cancel all outbound ARQ activity (data retransmit timers, delayed ACK repeats,
    // delayed SACK, in-flight TX slots) while preserving RX reassembly state.
    arq_.abortPendingTx();

    // Cancel pending local TX assembly/burst state.
    bool had_pending_message = !pending_tx_fragments_.empty();
    pending_tx_fragments_.clear();
    pending_tx_fragment_flags_.clear();
    pending_tx_fragment_types_.clear();
    pending_tx_fragment_message_tokens_.clear();
    next_fragment_idx_ = 0;
    acked_fragment_count_ = 0;
    clearOutboundMessageTracking();
    burst_mode_active_ = false;
    burst_tx_buffer_.clear();
    arq_callback_defer_refill_ = false;
    deferred_file_refill_ = false;
    deferred_fragment_refill_ = false;

    // Cancel file TX if active. Keep RX file state untouched.
    queued_file_path_.reset();
    if (file_transfer_.getState() == FileTransferState::SENDING) {
        file_transfer_.cancel();
    }

    // Cancel pending control-path retries/timeouts.
    mode_change_pending_ = false;
    mode_change_timeout_ms_ = 0;
    mode_change_retry_count_ = 0;
    desc_switch_full_anchor_pending_ = false;
    disconnect_pending_ = false;
    disconnect_pending_ms_ = 0;
    disconnect_ack_retransmit_ms_ = 0;
    disconnect_ack_frame_.clear();
    disconnect_frame_.clear();
    disconnect_retry_count_ = 0;
    disconnect_retransmit_ms_ = 0;
    timeout_remaining_ms_ = 0;
    connect_retry_count_ = 0;
    ping_retry_count_ = 0;
    responder_handshake_wait_ms_ = 0;
    connect_ack_frame_.clear();
    connect_ack_retransmit_ms_ = 0;
    connect_ack_retx_remaining_ = 0;

    // Stop transient connection attempts immediately.
    if (state_ == ConnectionState::PROBING ||
        state_ == ConnectionState::CONNECTING ||
        state_ == ConnectionState::DISCONNECTING) {
        enterDisconnected("TX aborted");
        return;
    }

    // Connected state remains established; only outbound transfer is aborted.
    if (state_ == ConnectionState::CONNECTED && had_pending_message && on_message_sent_) {
        on_message_sent_(false);
    }

    LOG_MODEM(INFO, "Connection: TX abort applied (state=%s)",
              connectionStateToString(state_));
}

void Connection::setForcedFrameCodewords(int cw_count, bool forced) {
    cw_count = v2::sanitizeFixedFrameCodewords(cw_count);
    if (forced) {
        // Operator override: initiator embeds in CONNECT.data_frame_cw_count
        // so responder honors and echoes via CONNECT_ACK. One-sided
        // propagation — caller only needs to set this on one peer.
        config_.forced_cw_count = static_cast<uint8_t>(cw_count);
    }
    // Note: !forced is the boot-time default path (host wiring up encoder/
    // decoder before connection). It MUST NOT touch config_.forced_cw_count
    // or every connect would advertise the default as a forced override
    // and bypass auto-pick on the responder.

    if (cw_count == data_frame_cw_count_) {
        return;
    }

    data_frame_cw_count_ = cw_count;
    config_.fixed_frame_codewords = cw_count;
    arq_.setFixedFrameCodewords(cw_count);

    const bool bounded_variable_mc_dpsk = usesBoundedVariableMCDPSKFrames();
    if (isOFDMMode(negotiated_mode_) || bounded_variable_mc_dpsk) {
        file_transfer_.setMaxChunkPayload(currentDataPayloadCapacity());
        configureArqForCurrentDataMode();
    }

    LOG_MODEM(INFO, "Connection: Fixed data frame CW count set to %d", data_frame_cw_count_);
}

// =============================================================================
// DATA TRANSFER
// =============================================================================

bool Connection::sendMessage(const std::string& text) {
    Bytes data(text.begin(), text.end());
    return sendPayload(data, false);
}

bool Connection::sendBinary(const Bytes& data) {
    return sendPayload(data, true);
}

uint64_t Connection::createOutboundMessageRecord(const Bytes& data) {
    if (data.empty()) {
        return 0;
    }

    OutboundMessageTxRecord record;
    record.token = next_outbound_message_token_++;
    if (next_outbound_message_token_ == 0) {
        next_outbound_message_token_ = 1;
    }
    record.text.assign(data.begin(), data.end());
    outbound_message_tx_records_.push_back(std::move(record));
    return outbound_message_tx_records_.back().token;
}

void Connection::setOutboundMessageExpectedFragments(uint64_t token, size_t fragments) {
    if (token == 0) {
        return;
    }
    for (auto& record : outbound_message_tx_records_) {
        if (record.token == token) {
            record.expected_fragments = fragments;
            return;
        }
    }
}

void Connection::dropOutboundMessageRecord(uint64_t token) {
    if (token == 0) {
        return;
    }
    outbound_message_tx_records_.erase(
        std::remove_if(outbound_message_tx_records_.begin(),
                       outbound_message_tx_records_.end(),
                       [token](const OutboundMessageTxRecord& record) {
                           return record.token == token;
                       }),
        outbound_message_tx_records_.end());
}

void Connection::clearOutboundMessageTracking() {
    outbound_message_tx_records_.clear();
    pending_tx_fragment_message_tokens_.clear();
    arq_submit_message_token_ = 0;
}

bool Connection::sendArqPayloadFrame(const Bytes& chunk,
                                     v2::FrameType frame_type,
                                     uint8_t flags,
                                     bool fixed_frame,
                                     uint64_t message_token) {
    arq_submit_message_token_ = message_token;
    const bool sent = fixed_frame
        ? arq_.sendFixedDataWithTypeAndFlags(chunk, frame_type, flags)
        : arq_.sendDataWithTypeAndFlags(chunk, frame_type, flags);
    arq_submit_message_token_ = 0;
    return sent;
}

void Connection::emitMessageTxStatus(OutboundMessageTxRecord& record,
                                     MessageTxStatus status) {
    if (!on_message_tx_status_) {
        return;
    }

    MessageTxStatusEvent event;
    event.status = status;
    event.first_seq = record.first_seq;
    event.last_seq = record.last_seq;
    event.remote_call = remote_call_;
    event.text = record.text;
    if (record.first_seq_valid) {
        const auto now = std::chrono::steady_clock::now();
        event.elapsed_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - record.submitted_at).count());
    }
    on_message_tx_status_(event);
}

void Connection::handleArqFrameSubmitted(uint16_t seq) {
    const uint64_t token = arq_submit_message_token_;
    if (token == 0) {
        return;
    }

    for (auto& record : outbound_message_tx_records_) {
        if (record.token != token || record.terminal_reported) {
            continue;
        }
        if (!record.first_seq_valid) {
            record.first_seq = seq;
            record.first_seq_valid = true;
            record.submitted_at = std::chrono::steady_clock::now();
        }
        record.last_seq = seq;
        record.assigned_fragments++;
        if (!record.submitted_reported) {
            record.submitted_reported = true;
            emitMessageTxStatus(record, MessageTxStatus::SUBMITTED);
        }
        if (record.expected_fragments != 0 &&
            record.assigned_fragments >= record.expected_fragments) {
            LOG_MODEM(INFO,
                      "Connection: Message TX #%u spans seq=%u..%u (%zu frame%s, %zu bytes)",
                      record.first_seq,
                      record.first_seq,
                      record.last_seq,
                      record.assigned_fragments,
                      record.assigned_fragments == 1 ? "" : "s",
                      record.text.size());
        }
        return;
    }
}

void Connection::handleArqTxBaseAdvanced(uint16_t base_seq) {
    for (auto& record : outbound_message_tx_records_) {
        if (record.terminal_reported ||
            !record.first_seq_valid ||
            record.assigned_fragments < record.expected_fragments) {
            continue;
        }
        if (seqBefore(record.last_seq, base_seq)) {
            record.terminal_reported = true;
            emitMessageTxStatus(record, MessageTxStatus::DELIVERED);
        }
    }

    outbound_message_tx_records_.erase(
        std::remove_if(outbound_message_tx_records_.begin(),
                       outbound_message_tx_records_.end(),
                       [](const OutboundMessageTxRecord& record) {
                           return record.terminal_reported;
                       }),
        outbound_message_tx_records_.end());
}

void Connection::handleArqFrameFailed(uint16_t seq) {
    for (auto& record : outbound_message_tx_records_) {
        if (record.terminal_reported ||
            !record.first_seq_valid ||
            record.assigned_fragments == 0) {
            continue;
        }
        const bool in_record =
            seq == record.first_seq ||
            seq == record.last_seq ||
            (seqBefore(record.first_seq, seq) && seqBefore(seq, record.last_seq));
        if (!in_record) {
            continue;
        }
        record.terminal_reported = true;
        emitMessageTxStatus(record, MessageTxStatus::FAILED);
        break;
    }

    outbound_message_tx_records_.erase(
        std::remove_if(outbound_message_tx_records_.begin(),
                       outbound_message_tx_records_.end(),
                       [](const OutboundMessageTxRecord& record) {
                           return record.terminal_reported;
                       }),
        outbound_message_tx_records_.end());
}

bool Connection::sendPayload(const Bytes& data, bool binary_payload) {
    if (state_ != ConnectionState::CONNECTED) {
        LOG_MODEM(WARN, "Connection: Cannot send, not connected");
        return false;
    }

    const uint64_t message_token = binary_payload ? 0 : createOutboundMessageRecord(data);
    LOG_MODEM(DEBUG, "Connection: B2F-DBG sendPayload %zuB queue=%d (local_turn=%d is_init=%d hs_conf=%d yield_pend=%d peer_req=%d guard=%u)",
              data.size(), shouldQueuePayloadForLinkTurn() ? 1 : 0,
              local_data_turn_ ? 1 : 0, is_initiator_ ? 1 : 0, handshake_confirmed_ ? 1 : 0,
              data_turn_yield_pending_ ? 1 : 0, peer_data_turn_requested_ ? 1 : 0, data_turn_tx_guard_ms_);
    if (shouldQueuePayloadForLinkTurn()) {
        if (queued_payloads_.size() >= kMaxQueuedPayloads) {
            dropOutboundMessageRecord(queued_payloads_.front().message_token);
            queued_payloads_.pop_front();
            LOG_MODEM(WARN, "Connection: Queued payload limit reached, dropping oldest deferred payload");
        }
        queued_payloads_.push_back(QueuedPayload{data, binary_payload, message_token});
        LOG_MODEM(INFO,
                  "Connection: Queued %zu byte %s until local ISS DATA turn (depth=%zu, local_turn=%d, peer_request=%d)",
                  data.size(), binary_payload ? "binary payload" : "message",
                  queued_payloads_.size(), local_data_turn_ ? 1 : 0,
                  peer_data_turn_requested_ ? 1 : 0);
        sendTurnRequestIfNeeded();
        return true;
    }

    const bool started = startPayloadNow(data, binary_payload, message_token);
    if (!started) {
        dropOutboundMessageRecord(message_token);
    }
    return started;
}

bool Connection::hasLocalOutboundDataTurn() const {
    return file_transfer_.getState() == FileTransferState::SENDING ||
           mode_change_pending_ ||
           !pending_tx_fragments_.empty() ||
           arq_.getTxInFlightBytes() > 0;
}

bool Connection::hasLocalInFlightDataTurn() const {
    return mode_change_pending_ ||
           !pending_tx_fragments_.empty() ||
           arq_.getTxInFlightBytes() > 0 ||
           (file_transfer_.getState() == FileTransferState::SENDING &&
            file_transfer_.hasPendingChunks());
}

bool Connection::hasLocalDataWaitingForTurn() const {
    const bool file_waiting =
        file_transfer_.getState() == FileTransferState::SENDING &&
        (file_transfer_.hasMoreChunks() || file_transfer_.hasPendingChunks());

    const bool fragments_waiting =
        !pending_tx_fragments_.empty() &&
        (next_fragment_idx_ < pending_tx_fragments_.size() ||
         acked_fragment_count_ < pending_tx_fragments_.size());

    return queued_file_path_.has_value() ||
           !queued_payloads_.empty() ||
           file_waiting ||
           fragments_waiting ||
           mode_change_pending_;
}

bool Connection::dataTurnFairBudgetMet() const {
    return data_turn_payload_bytes_sent_ >= DATA_TURN_FAIR_BURST_BYTES ||
           (data_turn_contended_ms_ >= DATA_TURN_FAIR_BURST_MS &&
            data_turn_payload_bytes_sent_ >= DATA_TURN_FAIR_MIN_BYTES_FOR_TIME_YIELD);
}

bool Connection::shouldPauseLocalDataForPeerRequest() const {
    if (state_ != ConnectionState::CONNECTED ||
        !local_data_turn_ ||
        !peer_data_turn_requested_ ||
        file_cancel_confirm_pending_ ||
        file_transfer_.getState() == FileTransferState::SENDING ||
        !dataTurnFairBudgetMet()) {
        return false;
    }

    // Do not split an already-started fragmented operator payload. An active
    // file transfer owns the DATA turn until completion or cancel, so file
    // chunks are intentionally not paused for chat turn requests.
    return pending_tx_fragments_.empty();
}

bool Connection::shouldQueuePayloadForLinkTurn() const {
    if (state_ != ConnectionState::CONNECTED) {
        return false;
    }

    // HF ARQ is a half-duplex link. Only the current ISS may originate DATA.
    // Operator payloads from the IRS are queued and announced through a
    // turn-request ACK/control frame; they do not race the peer on the channel.
    const bool responder_handshake_turn =
        !is_initiator_ && !handshake_confirmed_;

    return responder_handshake_turn ||
           data_turn_yield_pending_ ||
           !local_data_turn_ ||
           peer_data_turn_requested_ ||
           file_cancel_confirm_pending_ ||
           data_turn_tx_guard_ms_ > 0 ||
           hasLocalOutboundDataTurn();
}

bool Connection::shouldRequestDataTurnOnAck() const {
    return state_ == ConnectionState::CONNECTED &&
           !local_data_turn_ &&
           !file_transfer_.isBusy() &&
           !file_cancel_confirm_pending_ &&
           hasLocalDataWaitingForTurn() &&
           (is_initiator_ || handshake_confirmed_);
}

bool Connection::noteTurnRequestOnAckIfNeeded() {
    if (!shouldRequestDataTurnOnAck()) {
        return false;
    }

    // A TURN_REQUEST bit riding on an ACK is already an on-air request. The
    // first standalone retransmission must wait long enough for the peer to
    // receive that ACK, finish any ACK-diversity guard, and send TURNOVER back
    // across the half-duplex channel. Otherwise both sides can transmit control
    // bursts into each other at the ownership change.
    local_turn_request_pending_ = true;
    turn_request_retransmit_ms_ = std::max(
        turn_request_retransmit_ms_,
        turnRequestAckEmbeddedRetransmitMs());
    return true;
}

void Connection::resetDataTurnFairness() {
    data_turn_payload_bytes_sent_ = 0;
    data_turn_contended_ms_ = 0;
}

void Connection::noteDataTurnPayloadStarted(size_t payload_bytes) {
    if (local_data_turn_) {
        data_turn_payload_bytes_sent_ += payload_bytes;
    }
}

void Connection::sendTurnRequestIfNeeded() {
    if (state_ != ConnectionState::CONNECTED ||
        local_data_turn_ ||
        file_transfer_.isBusy() ||
        file_cancel_confirm_pending_ ||
        yielded_data_turn_waiting_for_peer_data_ ||
        !hasLocalDataWaitingForTurn() ||
        local_turn_request_pending_ ||
        turn_request_holdoff_ms_ > 0 ||
        (!is_initiator_ && !handshake_confirmed_)) {
        return;
    }

    auto request = v2::ControlFrame::makeTurnRequest(local_call_, remote_call_);
    LOG_MODEM(INFO, "Connection: TX TURN_REQUEST (queued=%zu, backlog=%zu bytes)",
              queued_payloads_.size(), getTxBacklogBytes());
    transmitFrame(request.serialize());
    local_turn_request_pending_ = true;
    turn_request_retransmit_ms_ = turnRequestRetransmitMs();
}

void Connection::armDataTurnTxGuard(uint32_t guard_ms) {
    data_turn_tx_guard_ms_ = std::max(data_turn_tx_guard_ms_, guard_ms);
}

bool Connection::maybeYieldDataTurn() {
    if (state_ != ConnectionState::CONNECTED ||
        !local_data_turn_ ||
        !peer_data_turn_requested_ ||
        file_transfer_.getState() == FileTransferState::SENDING ||
        hasLocalInFlightDataTurn()) {
        return false;
    }

    const bool only_unstarted_file_waiting =
        queued_file_path_.has_value() &&
        queued_payloads_.empty() &&
        file_transfer_.getState() != FileTransferState::SENDING &&
        pending_tx_fragments_.empty() &&
        arq_.getTxInFlightBytes() == 0 &&
        !mode_change_pending_;

    if (!data_turn_yield_pending_ &&
        data_turn_payload_bytes_sent_ > 0 &&
        hasLocalDataWaitingForTurn() &&
        !only_unstarted_file_waiting &&
        !dataTurnFairBudgetMet()) {
        return false;
    }

    if (data_turn_tx_guard_ms_ > 0) {
        data_turn_yield_pending_ = true;
        return false;
    }

    auto turnover = v2::ControlFrame::makeTurnover(local_call_, remote_call_);
    LOG_MODEM(INFO,
              "Connection: TX TURNOVER to %s (peer requested DATA turn, turn_bytes=%llu, contended_ms=%u, backlog=%zu)",
              remote_call_.c_str(),
              static_cast<unsigned long long>(data_turn_payload_bytes_sent_),
              data_turn_contended_ms_, getTxBacklogBytes());
    transmitFrame(turnover.serialize());
    local_data_turn_ = false;
    peer_data_turn_requested_ = false;
    local_turn_request_pending_ = false;
    yielded_data_turn_waiting_for_peer_data_ = true;
    data_turn_yield_pending_ = false;
    turn_request_retransmit_ms_ = 0;
    turn_request_holdoff_ms_ = turnRequestHoldoffAfterDataMs();
    received_peer_data_since_connect_ = false;
    resetDataTurnFairness();
    armDataTurnTxGuard(dataTurnControlGuardMs());
    return true;
}

bool Connection::startPayloadNow(const Bytes& data, bool binary_payload, uint64_t message_token) {
    if (state_ != ConnectionState::CONNECTED) {
        LOG_MODEM(WARN, "Connection: Cannot send, not connected");
        return false;
    }

    bool is_ofdm = isOFDMMode(negotiated_mode_);
    const bool bounded_variable_mc_dpsk = usesBoundedVariableMCDPSKFrames();
    const size_t capacity = currentDataPayloadCapacity();
    auto markPayloadStarted = [this, &data](bool started) {
        if (started) {
            noteDataTurnPayloadStarted(data.size());
        }
        return started;
    };

    if (!is_ofdm && !bounded_variable_mc_dpsk) {
        setOutboundMessageExpectedFragments(message_token, 1);
        if (binary_payload) {
            return markPayloadStarted(
                sendArqPayloadFrame(data, v2::FrameType::DATA_END, v2::Flags::NONE,
                                    false, message_token));
        }
        return markPayloadStarted(
            sendArqPayloadFrame(data, v2::FrameType::DATA, v2::Flags::NONE,
                                false, message_token));
    }

    if (capacity == 0) {
        LOG_MODEM(ERROR, "Connection: Data frame payload capacity is zero for current mode");
        return false;
    }

    if (data.size() <= capacity) {
        setOutboundMessageExpectedFragments(message_token, 1);
        if (binary_payload) {
            return markPayloadStarted(
                is_ofdm
                    ? sendArqPayloadFrame(data, v2::FrameType::DATA_END, v2::Flags::FINAL,
                                          true, message_token)
                    : sendArqPayloadFrame(data, v2::FrameType::DATA_END, v2::Flags::FINAL,
                                          false, message_token));
        }
        return markPayloadStarted(
            sendArqPayloadFrame(data, v2::FrameType::DATA, v2::Flags::FINAL,
                                is_ofdm, message_token));
    }

    // Fragment the message into chunks that fit in one frame each
    LOG_MODEM(INFO, "Connection: Fragmenting %zu byte %s into %zu-byte chunks",
              data.size(), binary_payload ? "binary payload" : "message", capacity);

    pending_tx_fragments_.clear();
    pending_tx_fragment_flags_.clear();
    pending_tx_fragment_types_.clear();
    pending_tx_fragment_message_tokens_.clear();
    next_fragment_idx_ = 0;
    acked_fragment_count_ = 0;

    for (size_t offset = 0; offset < data.size(); offset += capacity) {
        size_t chunk_size = std::min(capacity, data.size() - offset);
        pending_tx_fragments_.emplace_back(data.begin() + offset, data.begin() + offset + chunk_size);
        if (binary_payload) {
            const bool first = (offset == 0);
            const bool last = (offset + chunk_size >= data.size());
            pending_tx_fragment_types_.push_back(
                first ? v2::FrameType::DATA_START :
                (last ? v2::FrameType::DATA_END : v2::FrameType::DATA_CONT));
        }
        const bool last = (offset + chunk_size >= data.size());
        pending_tx_fragment_flags_.push_back(
            last ? v2::Flags::FINAL : v2::Flags::MORE_FRAG);
        pending_tx_fragment_message_tokens_.push_back(message_token);
    }
    setOutboundMessageExpectedFragments(message_token, pending_tx_fragments_.size());

    LOG_MODEM(INFO, "Connection: Split into %zu fragments", pending_tx_fragments_.size());

    sendNextFragment();
    return true;
}

void Connection::transmitFileCancelControl(const char* reason) {
    if (state_ != ConnectionState::CONNECTED) {
        return;
    }

    auto cancel = v2::ControlFrame::makeFileCancel(local_call_, remote_call_);
    LOG_MODEM(INFO, "Connection: TX FILE_CANCEL to %s%s",
              remote_call_.c_str(), reason ? reason : "");
    transmitFrame(cancel.serialize());
}

void Connection::armFileCancelReassertion() {
    file_cancel_reassert_ms_ = FILE_CANCEL_REASSERT_WINDOW_MS;
    file_cancel_reassert_cooldown_ms_ = 0;
}

void Connection::clearFileCancelReassertion() {
    file_cancel_reassert_ms_ = 0;
    file_cancel_reassert_cooldown_ms_ = 0;
}

void Connection::maybeReassertFileCancelForStaleData() {
    if (file_cancel_reassert_ms_ == 0 ||
        file_cancel_reassert_cooldown_ms_ > 0 ||
        state_ != ConnectionState::CONNECTED) {
        return;
    }

    transmitFileCancelControl(" (reassert stale DATA)");
    file_cancel_reassert_cooldown_ms_ = FILE_CANCEL_REASSERT_COOLDOWN_MS;
}

bool Connection::tryStartQueuedFileIfReady() {
    if (!queued_file_path_) {
        return false;
    }
    if (state_ != ConnectionState::CONNECTED) {
        return false;
    }
    if (!local_data_turn_) {
        sendTurnRequestIfNeeded();
        return false;
    }
    if (file_cancel_confirm_pending_) {
        return false;
    }
    if (peer_data_turn_requested_ || data_turn_yield_pending_) {
        maybeYieldDataTurn();
        return false;
    }
    if (data_turn_tx_guard_ms_ > 0 ||
        hasLocalOutboundDataTurn() ||
        !pending_tx_fragments_.empty() ||
        !arq_.isReadyToSend()) {
        return false;
    }

    const std::string path = *queued_file_path_;
    queued_file_path_.reset();
    if (!queued_payloads_.empty()) {
        LOG_MODEM(INFO,
                  "Connection: Starting queued file ahead of %zu deferred chat payload(s) after current DATA turn drained",
                  queued_payloads_.size());
    }
    LOG_MODEM(INFO, "Connection: Starting queued file transfer on local ISS DATA turn: %s",
              path.c_str());
    if (!startFileTransferNow(path)) {
        return false;
    }
    return true;
}

void Connection::sendNextQueuedPayloadIfReady() {
    if (queued_file_path_) {
        tryStartQueuedFileIfReady();
        return;
    }
    if (queued_payloads_.empty()) {
        return;
    }
    if (state_ != ConnectionState::CONNECTED) {
        return;
    }
    if (!is_initiator_ && !handshake_confirmed_) {
        return;
    }
    if (!local_data_turn_) {
        sendTurnRequestIfNeeded();
        return;
    }
    if (file_cancel_confirm_pending_) {
        return;
    }
    if (data_turn_tx_guard_ms_ > 0) {
        return;
    }
    if (data_turn_yield_pending_) {
        maybeYieldDataTurn();
        return;
    }
    if (shouldPauseLocalDataForPeerRequest()) {
        maybeYieldDataTurn();
        return;
    }
    if (hasLocalOutboundDataTurn() || !arq_.isReadyToSend()) {
        return;
    }

    QueuedPayload payload = std::move(queued_payloads_.front());
    queued_payloads_.pop_front();
    LOG_MODEM(INFO,
              "Connection: Sending deferred half-duplex payload (%zu bytes, remaining=%zu)",
              payload.data.size(), queued_payloads_.size());
    startPayloadNow(payload.data, payload.binary_payload, payload.message_token);
}

bool Connection::sendMessages(const std::vector<std::string>& texts) {
    if (state_ != ConnectionState::CONNECTED) {
        LOG_MODEM(WARN, "Connection: Cannot send, not connected");
        return false;
    }
    if (shouldQueuePayloadForLinkTurn()) {
        for (const auto& text : texts) {
            Bytes data(text.begin(), text.end());
            const uint64_t message_token = createOutboundMessageRecord(data);
            if (queued_payloads_.size() >= kMaxQueuedPayloads) {
                dropOutboundMessageRecord(queued_payloads_.front().message_token);
                queued_payloads_.pop_front();
                LOG_MODEM(WARN, "Connection: Queued payload limit reached, dropping oldest deferred message");
            }
            queued_payloads_.push_back(QueuedPayload{data, false, message_token});
        }
        LOG_MODEM(INFO,
                  "Connection: Queued %zu-message batch until local ISS DATA turn (depth=%zu)",
                  texts.size(), queued_payloads_.size());
        sendTurnRequestIfNeeded();
        return !texts.empty();
    }

    bool is_ofdm = isOFDMMode(negotiated_mode_);
    const bool bounded_variable_mc_dpsk = usesBoundedVariableMCDPSKFrames();
    size_t capacity = (is_ofdm || bounded_variable_mc_dpsk) ? currentDataPayloadCapacity() : SIZE_MAX;

    // Pre-fragment all messages into a flat list of frame payloads with flags
    pending_tx_fragments_.clear();
    pending_tx_fragment_flags_.clear();
    pending_tx_fragment_types_.clear();
    pending_tx_fragment_message_tokens_.clear();
    next_fragment_idx_ = 0;
    acked_fragment_count_ = 0;

    for (const auto& text : texts) {
        Bytes data(text.begin(), text.end());
        const uint64_t message_token = createOutboundMessageRecord(data);
        size_t frames_for_message = 0;

        if (data.size() <= capacity) {
            // Single frame — no MORE_FRAG
            pending_tx_fragments_.push_back(data);
            pending_tx_fragment_flags_.push_back(v2::Flags::NONE);
            pending_tx_fragment_message_tokens_.push_back(message_token);
            frames_for_message = 1;
        } else {
            // Fragment this message
            for (size_t offset = 0; offset < data.size(); offset += capacity) {
                size_t chunk_size = std::min(capacity, data.size() - offset);
                Bytes chunk(data.begin() + offset, data.begin() + offset + chunk_size);
                bool is_last = (offset + chunk_size >= data.size());
                pending_tx_fragments_.push_back(chunk);
                pending_tx_fragment_flags_.push_back(
                    is_last ? v2::Flags::NONE : v2::Flags::MORE_FRAG);
                pending_tx_fragment_message_tokens_.push_back(message_token);
                frames_for_message++;
            }
        }
        setOutboundMessageExpectedFragments(message_token, frames_for_message);
    }

    if (!pending_tx_fragment_flags_.empty()) {
        pending_tx_fragment_flags_.back() |= v2::Flags::FINAL;
    }

    LOG_MODEM(INFO, "Connection: Batch queued %zu messages as %zu frames",
              texts.size(), pending_tx_fragments_.size());

    // Send first window-worth via sendNextFragment() (handles burst buffering)
    sendNextFragment();
    return !pending_tx_fragments_.empty();
}

bool Connection::isReadyToSend() const {
    return state_ == ConnectionState::CONNECTED && arq_.isReadyToSend() &&
           local_data_turn_ && !peer_data_turn_requested_ &&
           !file_cancel_confirm_pending_ &&
           data_turn_tx_guard_ms_ == 0 && !file_transfer_.isBusy() &&
           !queued_file_path_.has_value();
}

size_t Connection::getTxBacklogBytes() const {
    size_t bytes = arq_.getTxInFlightBytes();

    if (next_fragment_idx_ < pending_tx_fragments_.size()) {
        for (size_t i = next_fragment_idx_; i < pending_tx_fragments_.size(); ++i) {
            bytes += pending_tx_fragments_[i].size();
        }
    }

    for (const auto& payload : queued_payloads_) {
        bytes += payload.data.size();
    }

    if (queued_file_path_) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(*queued_file_path_, ec);
        bytes += ec ? 1 : static_cast<size_t>(size);
    }

    if (file_transfer_.getState() == FileTransferState::SENDING) {
        bytes += file_transfer_.remainingTxBytes();
    }

    return bytes;
}

// =============================================================================
// FILE TRANSFER
// =============================================================================

bool Connection::sendFile(const std::string& filepath) {
    LOG_MODEM(WARN, "Connection::sendFile() called path=%s state=%d use_burst=%d",
              filepath.c_str(), static_cast<int>(state_), use_burst_transport_ ? 1 : 0);
    if (state_ != ConnectionState::CONNECTED) {
        LOG_MODEM(WARN, "Connection: Cannot send file, not connected");
        return false;
    }

    if (file_transfer_.isBusy() || queued_file_path_) {
        LOG_MODEM(WARN, "Connection: File transfer already in progress");
        return false;
    }

    // 2026-05-28: bypass the legacy ISS-turn-taking gate when burst transport
    // is active. Burst transport is one-way sender-driven (design §14.27).
    // 2026-06-03: but NOT in half-duplex INTERACTIVE mode (TNC / Winlink B2F).
    // There BOTH stations alternately transmit; the bypass would let them key up
    // uncoordinated and collide. Keep the turn gate below so the burst only starts
    // when this station holds the DATA turn (else queue + TURN_REQUEST; the peer
    // yields TURNOVER and the directions serialize).
    if (use_burst_transport_ && isOFDMMode(negotiated_mode_) && !half_duplex_interactive_) {
        return startFileTransferNow(filepath);
    }

    if (!local_data_turn_ ||
        peer_data_turn_requested_ ||
        file_cancel_confirm_pending_ ||
        data_turn_tx_guard_ms_ > 0 ||
        hasLocalOutboundDataTurn() ||
        !pending_tx_fragments_.empty() ||
        !queued_payloads_.empty() ||
        !arq_.isReadyToSend()) {
        queued_file_path_ = filepath;
        LOG_MODEM(INFO,
                  "Connection: Queued file transfer until local ISS DATA turn is clear (path=%s, local_turn=%d, peer_request=%d, guard_ms=%u)",
                  filepath.c_str(), local_data_turn_ ? 1 : 0,
                  peer_data_turn_requested_ ? 1 : 0, data_turn_tx_guard_ms_);
        sendTurnRequestIfNeeded();
        maybeYieldDataTurn();
        return true;
    }

    return startFileTransferNow(filepath);
}

bool Connection::startFileTransferNow(const std::string& filepath) {
    if (!arq_.isReadyToSend()) {
        LOG_MODEM(INFO, "Connection: ARQ busy, cannot start file transfer");
        return false;
    }

    // Set chunk size to match frame capacity for bounded frame geometries.
    bool is_ofdm = isOFDMMode(negotiated_mode_);
    const bool bounded_variable_mc_dpsk = usesBoundedVariableMCDPSKFrames();
    if (is_ofdm || bounded_variable_mc_dpsk) {
        size_t capacity = currentDataPayloadCapacity();
        if (capacity <= FileTransferController::FILE_DATA_OVERHEAD) {
            LOG_MODEM(ERROR,
                      "Connection: File chunk payload capacity %zu is too small for FILE_DATA overhead",
                      capacity);
            return false;
        }
        file_transfer_.setMaxChunkPayload(capacity);
        LOG_MODEM(INFO, "Connection: File chunk payload limited to %zu bytes (%s %s, cw=%d)",
                  capacity,
                  is_ofdm ? "OFDM fixed-frame" : "MC-DPSK variable-frame",
                  codeRateToString(data_code_rate_),
                  data_frame_cw_count_);
    }

    LOG_MODEM(INFO, "Connection: Starting file transfer: %s", filepath.c_str());

    if (!file_transfer_.startSend(filepath)) {
        LOG_MODEM(ERROR, "Connection: Failed to start file transfer");
        return false;
    }

    // [LADDER] per-transfer telemetry (time-in-rung + move count, logged at completion).
    ladderTelemetryStart();

    // TRANSPORT MERGE (2026-06-06): the unified arq_ path is the only OFDM file transport —
    // it bursts + interleaves via sendNextFileChunk() -> flushBurstBuffer() over ONE 16-bit
    // seq space, one tone-burst ack, one retransmit window (legacy burst_transport_ removed).
    // #58 increment 3: selection-flavored consumer — the pool aggregate (knob-gated)
    // replaces the single-snapshot scalar so one trough reading can't flip the block
    // strategy for the whole transfer. Knob-off: exactly measured_snr_db_.
    if (is_ofdm && shouldUseSingleOFDMFileBlock(fading_index_, rateSelectionSnrDb(), data_code_rate_)) {
        Bytes block = file_transfer_.getSingleBlockPayload(kOFDMFileBlockPayloadLimit);
        if (!block.empty()) {
            LOG_MODEM(INFO, "Connection: Sending file as single OFDM block (%zu bytes payload)",
                      block.size());
            if (!arq_.sendVariableDataWithFlags(block, v2::Flags::FINAL)) {
                file_transfer_.onSendFailed();
                return false;
            }
            noteDataTurnPayloadStarted(block.size());
            return true;
        }
    } else if (is_ofdm) {
        LOG_MODEM(INFO, "Connection: Using interleaved OFDM chunks for file (SNR=%.1f (%s), fading=%.2f, rate=%s)",
                  measured_snr_db_, snrSourceToString(measured_snr_source_),
                  fading_index_, codeRateToString(data_code_rate_));
    }

    sendNextFileChunk();
    return true;
}

void Connection::setReceiveDirectory(const std::string& dir) {
    file_transfer_.setReceiveDirectory(dir);
}

void Connection::clearFileTransferArqState() {
    deferred_file_refill_ = false;
    burst_mode_active_ = false;
    burst_tx_buffer_.clear();
    arq_.reset();
    soft_combine_harq_.clear();
    mode_change_pending_ = false;
    mode_change_timeout_ms_ = 0;
    mode_change_retry_count_ = 0;
    pending_ladder_rung_id_ = LadderRungId::UNKNOWN;
    desc_switch_full_anchor_pending_ = false;
}

void Connection::cancelFileTransfer() {
    const bool had_active_transfer = file_transfer_.isBusy();
    const bool was_local_iss = local_data_turn_;
    if (state_ == ConnectionState::CONNECTED && had_active_transfer) {
        transmitFileCancelControl(" (local cancel)");
        armFileCancelReassertion();
    }

    queued_file_path_.reset();
    if (had_active_transfer) {
        file_transfer_.cancel("Transfer cancelled");
        clearFileTransferArqState();
        file_cancel_rx_drain_ms_ = FILE_CANCEL_RX_DRAIN_MS;
        armDataTurnTxGuard(fileCancelTxGuardMs());
        file_cancel_confirm_pending_ = false;
    }

    if (state_ == ConnectionState::CONNECTED) {
        data_turn_yield_pending_ = false;
        resetDataTurnFairness();
        if (!was_local_iss) {
            local_turn_request_pending_ = false;
            turn_request_retransmit_ms_ = 0;
            sendTurnRequestIfNeeded();
        }
        maybeYieldDataTurn();
        sendNextQueuedPayloadIfReady();
    }
}

bool Connection::isFileTransferInProgress() const {
    return file_transfer_.isBusy() || queued_file_path_.has_value();
}

FileTransferProgress Connection::getFileProgress() const {
    FileTransferProgress p = file_transfer_.getProgress();
    // UNIFIED PATH: out-of-order frames are buffered inside the ARQ rx window and not yet
    // handed to file_transfer_ (which assembles in order). Add them to received_bytes so
    // the GUI's "received" count + progress bar advance on ANY frame, not just contiguous
    // ones — matching the old offset-assembler feedback (received_bytes >= transferred).
    // Estimate by frame count × the data-chunk payload size (uniform except the tail).
    if (kUnifiedSeqEnabled() && p.is_sending == false && p.total_bytes > 0) {
        const size_t buffered_frames = arq_.bufferedRxFrameCount();
        if (buffered_frames > 0) {
            const size_t cap = currentDataPayloadCapacity();
            const size_t chunk_bytes =
                (cap > FileTransferController::FILE_DATA_OVERHEAD)
                    ? cap - FileTransferController::FILE_DATA_OVERHEAD
                    : 0;
            const uint32_t buffered_bytes =
                static_cast<uint32_t>(buffered_frames * chunk_bytes);
            p.received_bytes = std::min(p.total_bytes,
                                        p.transferred_bytes + buffered_bytes);
        }
    }
    return p;
}

void Connection::sendNextFileChunk() {
    if (file_transfer_.getState() != FileTransferState::SENDING) {
        return;
    }

    bool is_ofdm = isOFDMMode(negotiated_mode_);

    const bool is_mc_dpsk = negotiated_mode_ == WaveformMode::MC_DPSK;

    // Enable burst buffering for OFDM and MC-DPSK data-window mode.
    if ((is_ofdm || is_mc_dpsk) && on_transmit_burst_) {
        burst_mode_active_ = true;
        burst_tx_buffer_.clear();
    }

    // Bound ONE burst (key-down) to the half-duplex airtime ceiling: cap the frames
    // submitted this turn to burstAirtimeBudgetFrames(window) so a single transmission
    // can't run too long (PA duty, ack latency, and — the bug this fixes — a burst
    // outliving its own tone-burst ack window). DERIVED per rung (mod/rate/cw/fading),
    // not a fixed count. OFDM only (the airtime model is OFDM); MC-DPSK keeps its own
    // timing-derived window. Gated to the unified path for now (default build unchanged).
    const size_t burst_frame_cap = prepareUnifiedBurstWindow();

    // STOP-AND-WAIT, keep-the-pipe-full: this burst = [in-flight holes] + [new chunks],
    // filled to the budget, as ONE group. First RESEND the frames the receiver is still
    // missing (holes), then top up with new chunks. A partial burst's surviving frames
    // were SACKed, so the holes are few — they ride the next group instead of going out
    // as a lonely 1-frame resend, and the rest of the key-down carries new data. (Unified
    // OFDM only; the resend buffers into the open burst via transmitFrame.)
    if (is_ofdm && kUnifiedSeqEnabled() && burst_mode_active_) {
        arq_.retransmitInFlightUnacked(burst_frame_cap);
    }

    // Fill the remainder of the budget with new chunks. The budget counts the whole
    // group, so burst_tx_buffer_.size() (holes already buffered) is the running total.
    while (arq_.isReadyToSend() && file_transfer_.hasMoreChunks()) {
        const size_t in_burst =
            burst_mode_active_ ? burst_tx_buffer_.size() : 0;
        if (in_burst >= burst_frame_cap) {
            break;
        }
        Bytes chunk = file_transfer_.getNextChunk();
        if (chunk.empty()) {
            break;
        }

        // MORE_FRAG indicates more data remaining in file (not burst). FINAL
        // marks the actual stream tail for the short SACK timer.
        const bool has_more = file_transfer_.hasMoreChunks();
        uint8_t flags = has_more ? v2::Flags::MORE_FRAG : v2::Flags::FINAL;
        bool sent = false;
        if (is_ofdm) {
            sent = arq_.sendFixedDataWithFlags(chunk, flags);
        } else {
            sent = arq_.sendDataWithFlags(chunk, flags);
        }
        if (sent) {
            noteDataTurnPayloadStarted(chunk.size());
        }
    }

    // Flush burst buffer
    if ((is_ofdm || is_mc_dpsk) && on_transmit_burst_) {
        burst_mode_active_ = false;
        flushBurstBuffer();
    }
}

// §15 step 4d-ii: receiver-side tone-burst ACK handoff. Mirrors the OFDM
// GROUP_ACK arrival path at connection.cpp:2516, but takes a ToneBurstAckDetection
// (already decoded by the StreamingDecoder's monitor — no frame parse needed).
//
// Quality mapping for the rate controller:
//   - ACK  -> quality 1.0 (clean decode; the receiver couldn't have CRC-verified
//             a tone-burst payload otherwise). The 3-bit rate_hint field in the
//             payload is NOT consumed yet — that's a future refinement; for v1
//             the ACK/NACK semantic is all we use.
//   - NACK -> quality 0.0 (receiver couldn't decode -> step rate down).
//
// group_seq is 6 bits on the wire (mod 64). For files with <= 64 groups (~1 MB
// at QPSK R3/4) this matches the in-flight group exactly. Beyond that, the
// caller must extend the wire format; not gated for v1.
bool Connection::onToneBurstAck(
    const ultra::waveform::tone_burst_ack::ToneBurstAckDetection& detection) {
    if (state_ != ConnectionState::CONNECTED) return false;

    // WAITING-REBASE voice (BUG-UNANCHORED-SILENCE-ESCAPE, design §5.3, gated on
    // ULTRA_RX_RATE_CMD): rung_cmd==3 is NOT an ack — it is the unanchored peer's
    // only utterance ("I am alive, forward data is arriving, but the era-base frame
    // keeps dying — resend it"). Consume it FIRST and consume it WHOLE: its mask is
    // meaningless (must not reach the ARQ as a SACK) and it must not feed the rate
    // controller a 0 (the voice is proof the forward link WORKS — treating it as
    // demote evidence would re-manufacture the exact collapse it exists to prevent).
    if (rx_rate_cmd_enabled_ &&
        detection.payload.rung_cmd ==
            ultra::waveform::tone_burst_ack::kRungCmdReserved) {
        const int seen = static_cast<int>(detection.payload.group_seq);
        // Reset the collapse evidence every voice copy: silence-while-unanchored is
        // by design, not a forward crater (the E1/D1/D3 manufactured demotes).
        zero_progress_rounds_ = 0;
        if (seen != rx_rebase_voice_seq_seen_) {
            rx_rebase_voice_seq_seen_ = seen;
            LOG_MODEM(WARN,
                      "Connection: WAITING-REBASE voice from peer (seq=%d) — "
                      "re-sending era base standalone; zero-progress evidence reset",
                      seen);
            arq_.expireBaseSlotTimerForRebase();
        }
        return true;  // consumed whole — no ARQ sack, no controller feed, no advisory
    }

    // FOREIGN-SEMANTICS gate (BUG-TONEACK-FABRICATION, F116 2026-07-05): nothing on
    // the unified path emits a Nack-TYPED tone burst — a crater'd group still acks as
    // an Ack-typed no-progress SACK (sendSack: base = rx_base-1, holes in the mask),
    // and the only real Nack producer is the WAITING-REBASE voice, whose group_seq is
    // a BURST-GROUP ordinal in a DIFFERENT sequence space (consumed above when
    // ULTRA_RX_RATE_CMD is on). A Nack-typed detection reaching this point is foreign
    // or corrupt (F116: a stale-audio re-decode at the wrong symbol_ms rung that
    // fluked Costas+Hamming+CRC-12) — its fields must NEVER be indexed into the ARQ
    // window, the rate controller, or the drive advisory. Consume it whole; worst
    // case is one lost real ack, which the RTO covers.
    if (detection.payload.type == ultra::waveform::tone_burst_ack::AckType::Nack) {
        LOG_MODEM(WARN,
                  "Connection: Nack-typed tone-burst detection dropped whole "
                  "(group_seq=%u mask=0x%X) — foreign or corrupt",
                  detection.payload.group_seq,
                  detection.payload.frame_mask);
        return true;
    }

    // TRANSPORT MERGE (step 1): when interactive tone-burst acks are enabled and we have
    // interactive SR-ARQ frames in flight (no active burst owns this ack), route it to
    // the ARQ window. The ARQ reconstructs the seq and drives its normal ack path.
    //
    // COALESCE THE REFILL: a cumulative ack of N frames fires on_send_complete_ N times,
    // each of which would otherwise call sendNextFileChunk() immediately — fragmenting
    // the next window into N tiny bursts (the "3+1+1+1" / shorter-burst chains observed).
    // Bracket the ack with the SAME arq_callback_defer_refill_ guard processArqFrame uses
    // so all N completions defer to ONE runDeferredArqRefill() → ONE budget-sized burst.
    if (kInteractiveToneAckEnabled() && arq_.getTxInFlightBytes() > 0) {
        const bool outermost = !arq_callback_defer_refill_;
        if (outermost) arq_callback_defer_refill_ = true;
        // §RETX-PACING §1.1: re-arm the progress sentinel FIRST so the reading below is
        // exactly what THIS ack produced — a leftover value from any non-round ack path
        // (e.g. a control-frame SACK through processArqFrame) must not leak into round
        // accounting when this ack gets dedup/stale-dropped inside handleAckFrame.
        arq_.consumeAckProgress();
        // MOVE-EPOCH: hand the payload's era echo (bits 40-41) to the ARQ, which
        // folds it into the synthetic SACK and gates retirement on it (no-op OFF).
        arq_.onToneBurstAck(detection.payload.group_seq, detection.payload.frame_mask,
                            detection.payload.move_epoch);
        // §14.43: feed the RateController the RECEIVER's GRADED decode headroom carried in
        // rate_hint (0..7 -> [0,1]), not a binary ack/nack — restoring the closed loop the
        // unification cut. A NACK (group lost) still feeds 0. (Replaces the legacy GROUP_ACK
        // quality byte; same controller, now on the unified tone-burst path.)
        // RX-RATE-CMD Phase 2: snapshot the mode BEFORE the controller runs — one ACK is
        // ONE piece of channel evidence, so if the EMA/QAM16 machinery already moved the
        // rung on this ACK, the piggybacked command (same evidence, measured receiver-side)
        // must not fire a second move (maybeApplyRxRateCommand compares against these).
        const Modulation mod_at_ack = data_modulation_;
        const CodeRate rate_at_ack = data_code_rate_;
        // RX-AUTHORITY (ULTRA_RX_RATE_AUTHORITY): the receiver commands the rung
        // outright — the ACK's [rate_hint|rung_cmd] bits are its ABSOLUTE canonical
        // rung index, and the sender's own mid-transfer drivers (the EMA walk, the
        // dense demote/crest walks, the climb hop, trough amnesty, the relative
        // rung command) are INERT: one decision-maker, sitting where the channel is
        // actually measured. Ack-SILENCE safety rails (collapse escape, stuck-frame
        // escape, RTO machinery) stay live — no command crosses a blackout.
        const bool rx_authority = rxRateAuthorityEnabled();
        if (rx_authority) {
            const uint8_t cmd_idx = static_cast<uint8_t>(
                (detection.payload.rate_hint & 0x7) |
                ((detection.payload.rung_cmd & 0x3) << 3));
            maybeObeyAuthorityCommand(cmd_idx);
        } else {
            const float fed_quality =
                detection.payload.type ==
                        ultra::waveform::tone_burst_ack::AckType::Nack
                    ? 0.0f
                    : static_cast<float>(detection.payload.rate_hint) / 7.0f;
            applyAdaptiveRateFeedback(fed_quality);
        }
        // Software-ALC sender side (BUG-QAM16-RIG-LEVEL-BUDGET): the receiver's
        // drive advisory rides bits [30..31] of this ACK. Hand a non-hold advisory
        // to the host (the host owns tx_drive), which applies at most ONE step per
        // ACKed group (dedup by group_seq), clamped to [configured baseline, 0.85].
        // Advisory 3 (reserved) is treated as hold. ACK loss is stateless-safe:
        // the advisory simply doesn't arrive and the next group's ACK re-derives it.
        {
            const uint8_t advisory = detection.payload.drive_advisory;
            if (connection_policy::softwareAlcEnabled() && on_drive_advisory_ &&
                (advisory == ultra::waveform::tone_burst_ack::kDriveAdvisoryUp ||
                 advisory == ultra::waveform::tone_burst_ack::kDriveAdvisoryDown)) {
                on_drive_advisory_(advisory, detection.payload.group_seq);
            }
        }
        // RX-RATE-CMD Phase 2 (ULTRA_RX_RATE_CMD): consume the receiver's rung command
        // (bits 42-43) — ADVISORY input through the sender's own guards, never a blind
        // obey. Runs INSIDE the defer-refill bracket so a committed demote's refill
        // coalesces into the single outermost runDeferredArqRefill below (which then
        // sends the [holes]+[new] burst at the NEW rung). Hard no-op while OFF.
        // RX-AUTHORITY supersedes it: bits 42-43 are then command bits already
        // consumed above, not a relative demote.
        if (!rx_authority) {
            maybeApplyRxRateCommand(detection.payload.rung_cmd,
                                    detection.payload.group_seq,
                                    mod_at_ack, rate_at_ack);
        }
        if (outermost) {
            arq_callback_defer_refill_ = false;
            // §RETX-PACING §1.1 round boundary: every tone-burst ack ends a resend round.
            // Read the ARQ's identity-agnostic progress (base advance + new SACK bits;
            // −1 = the ack was dedup/stale-dropped ⇒ NOT a round) exactly once. Progress
            // resets the streak + releases any hold; a zero-progress round counts toward
            // the collapse escape and (knob-gated) arms the trough deferral BEFORE the
            // refill below, so the refill latches instead of re-blasting into the trough.
            const int round_progress = arq_.lastAckProgressFrames();
            arq_.consumeAckProgress();
            noteArqRoundOutcome(round_progress, "toneburst-ack");
            // TROUGH AMNESTY: progress after a zero-progress episode = the null ended;
            // restore the pre-episode rung (see maybeTroughAmnesty). Inside the same
            // defer-refill bracket, so the restored rung rides the very next refill.
            // RX-AUTHORITY: inert — the receiver re-commands the right rung on the
            // first post-trough ACK; a second restorer would double-drive.
            if (!rx_authority) {
                maybeTroughAmnesty(round_progress, detection.payload.rung_cmd);
            }
            // STOP-AND-WAIT: every tone-burst ack is a TURN boundary — it's now our turn
            // to send the next burst (resend remaining holes + new frames). Trigger the
            // refill even when the cumulative base did NOT advance: a SACK with a hole at
            // base (e.g. "I have 4,5, missing 3") doesn't fire on_send_complete_, but it's
            // still our turn to re-send the hole. sendNextFileChunk() coalesces
            // [holes]+[new] into one budget burst; an empty turn (nothing to send) no-ops.
            if (file_transfer_.getState() == FileTransferState::SENDING) {
                deferred_file_refill_ = true;
            }
            runDeferredArqRefill();
        }
        return true;
    }

    // Ack arrived with no in-flight ARQ bytes (nothing to advance) → nothing to do.
    return false;
}

// Retransmit depth at which a stuck in-flight frame triggers a one-rung ESCAPE-drop. 5 is well
// above the 1-3 retx a frame needs at a SUSTAINABLE rate on a fading channel, and well below
// max_retries (15) — so a frame the current (over-climbed) rate genuinely cannot push through
// drops to a robust rung before it dies, but ordinary Moderate retx don't trip it.
// ULTRA_STUCK_ESCAPE_RETX [2..10] (2026-07-02 campaign A/B knob): the Phase-0 forensics measured
// the 5-retx trigger costing ~84 s of frozen-base blind re-blast per 16QAM collapse (sim g43 and
// live rig MPG@20 both) — each retx round at 672 ms/frame x8 + RTO is a whole group-time spent
// re-sending into the same trough. Lower = earlier demote at the cost of occasionally fleeing a
// rung a lucky retx would have salvaged.
static int stuckRetransmitEscape() {
    static const int v = [] {
        if (const char* e = std::getenv("ULTRA_STUCK_ESCAPE_RETX")) {
            const int n = std::atoi(e);
            if (n >= 2 && n <= 10) return n;
        }
        return 5;
    }();
    return v;
}

// ═══════ Retx trough pacing + collapse-conditioned escape (docs/RETX_PACING_DESIGN_2026_07_03.md) ═══════
// ULTRA_RETX_TROUGH_PACING (default OFF ⇒ byte-identical): master switch for the §1
// trough-aware resend deferral — after a ZERO-progress round (whole key-down failed ⇒
// trough-conditioned), hold the next resend ~Tc so the channel decorrelates from the state
// that just killed it, instead of re-blasting the same frames into the same trough (the
// measured 6.3-retx/delivered collapse waste). Read ONCE (static).
static bool retxTroughPacingEnabled() {
    static const bool v = [] {
        const char* e = std::getenv("ULTRA_RETX_TROUGH_PACING");
        return e == nullptr || std::atoi(e) != 0;  // DEFAULT-ON 2026-07-05
    }();
    return v;
}

// ULTRA_TROUGH_DEFER_TC_FRAC [0.25..4.0] (default 1.0): `frac` in
// T_defer(n) = clamp(frac·Tc·2^(n−1) − elapsed, 0, T_cycle/2) — §1.2. Out-of-range/garbage
// values fall back to 1.0. Read ONCE (static).
static float troughDeferTcFrac() {
    static const float v = [] {
        if (const char* e = std::getenv("ULTRA_TROUGH_DEFER_TC_FRAC")) {
            const float f = static_cast<float>(std::atof(e));
            if (std::isfinite(f) && f >= 0.25f && f <= 4.0f) return f;
        }
        return 1.0f;
    }();
    return v;
}

// ULTRA_COLLAPSE_ESCAPE_ROUNDS 0 (OFF, default ⇒ byte-identical) or [2..8]: N consecutive
// zero-progress rounds at the current rung (with ≥ half the burst budget in flight) ⇒
// escape-drop the rung (§2). Zero-DELIVERED evidence, not retry depth — the rejected
// ULTRA_STUCK_ESCAPE_RETX=3 hair-trigger fled rungs that were delivering (g42 −28%); the
// round condition cannot trip on a lone straggler amid deliveries (§2.3). The existing
// 5-retx per-frame backstop (stuckRetransmitEscape above) stays untouched. Read ONCE (static).
static int collapseEscapeRounds() {
    static const int v = [] {
        if (const char* e = std::getenv("ULTRA_COLLAPSE_ESCAPE_ROUNDS")) {
            const int n = std::atoi(e);
            if (n >= 2 && n <= 8) return n;
        }
        return 2;  // DEFAULT 2026-07-05 (campaign standing value; 0 opts out)
    }();
    return v;
}

// QAM16 R2/3 cross-modulation climb (ULTRA_QAM16_CLIMB). Climb to QAM16 only after this many
// CONSECUTIVE clean groups (quality >= climb_above) while pinned at the QPSK R3/4 top rung — a
// modulation hop costs a full MODE_CHANGE (T/R + re-anchor) and QAM16 is fragile. Default 2
// (history 8 -> 4 on 2026-06-17, 4 -> 2 on 2026-07-02 fade-riding ladder: the crests of a
// Good ~10-20 s fade cycle only last a few ~8 s groups, so a 4-group streak forfeited most of
// each crest; 2 clean groups is the fastest gate that still requires a SUSTAINED reading, and
// the immediate demote + re-climb cooldown are the safety net). Env-tunable for rig sweeps via
// ULTRA_QAM16_CLIMB_STREAK [1..64]; lower = faster switch but a weaker Good-vs-Moderate proxy.
// Demote off QAM16 IMMEDIATELY on a bad group (kQam16DemoteBadStreak=1, was 2 — fade-riding
// needs the trough exit to be as prompt as the cliff NACK exit) or on a NACK.
// The ULTRA_QAM16_R34 crest-rung walk (QAM16 R2/3 -> R3/4, default-OFF) reuses this same
// streak length as its climb gate — one clean-evidence constant for both upward moves.
// MID-RUNG demote landing (ULTRA_QAM16_DEMOTE_MIDRUNG, default OFF; F102 finding):
// with the 16QAM R1/2 rung ladder-enabled, a crater/escape at 16QAM R2/3+ lands at
// 16QAM R1/2 (raw ~2.45k, ~2x the FEC margin) instead of skipping to QPSK R3/4
// (~2.05k raw). The channel that craters R2/3 often still carries R1/2 — that is
// the margin argument the epoch-stats exposed (broadband ~24 dB, damage-limited).
// If R1/2 craters too, the NEXT command lands QPSK R3/4 (one extra ~12 s step).
// Requires ULTRA_ENABLE_QAM16_LADDER (the rung must be selectable at all).
static bool qam16DemoteMidrungEnabled() {
    static const bool v = [] {
        const char* a = std::getenv("ULTRA_QAM16_DEMOTE_MIDRUNG");
        const char* b = std::getenv("ULTRA_ENABLE_QAM16_LADDER");
        return !(a && a[0] == '0') && !(b && b[0] == '0');  // DEFAULT-ON 2026-07-05
    }();
    return v;
}

static int qam16ClimbStreak() {
    static const int v = [] {
        if (const char* e = std::getenv("ULTRA_QAM16_CLIMB_STREAK")) {
            const int n = std::atoi(e);
            if (n >= 1 && n <= 64) return n;
        }
        return 1;  // DEFAULT 2026-07-05 (campaign standing value, was 2)
    }();
    return v;
}
static constexpr int kQam16DemoteBadStreak = 1;

// Re-climb cooldown BASE after a QAM16 demote, in CLEAN groups (quality >= climb_above).
// ULTRA_QAM16_RECLIMB_COOLDOWN [0..64]; 0 = no cooldown. Doubles per demote this connection
// (cap x4) via noteQam16Demoted — see the member comment in connection.hpp and the move-
// overhead arithmetic in CHANGELOG 2026-07-02.
static int qam16ReclimbCooldownBase() {
    static const int v = [] {
        if (const char* e = std::getenv("ULTRA_QAM16_RECLIMB_COOLDOWN")) {
            const int n = std::atoi(e);
            if (n >= 0 && n <= 64) return n;
        }
        return 1;  // DEFAULT 2026-07-05 (campaign standing value, was 3)
    }();
    return v;
}

// Register a QAM16 demote and arm the re-climb cooldown. weight=1 for the ack-driven soft
// demote; weight=2 for the escape-drop (a frame nearly DIED at QAM16 — stronger evidence, so
// the backoff advances two steps). Cooldown = base << min(demotes-1, 2): 3, 6, 12, 12...
void Connection::noteQam16Demoted(int weight) {
    qam16_demote_count_ = std::min(qam16_demote_count_ + weight, 8);
    const int scale = 1 << std::min(qam16_demote_count_ - 1, 2);
    qam16_reclimb_cooldown_ = qam16ReclimbCooldownBase() * scale;
    qam16_clean_streak_ = 0;
    qam16_r34_clean_streak_ = 0;  // leaving QAM16 — any pending crest-rung walk dies with it
}

bool Connection::rateAdaptationActive() const {
    if (!adaptive_rate_enabled_) return false;
    const char* l = std::getenv("ULTRA_LOCK_RATE");
    if (l != nullptr && std::atoi(l) != 0) return false;  // operator pin always wins
    const char* e = std::getenv("ULTRA_RATE_ADAPT");
    if (e != nullptr) return std::atoi(e) != 0;  // explicit override (any OFDM mode, as before)
    // DEFAULT (2026-07-02, fade-riding ladder): ON for connected wideband OFDM — the
    // burst-transport file path whose clean-boundary gate (06-10), synchronized
    // MODE_CHANGE (06-09), ssthresh ceiling (06-11) and QAM16 climb/demote machinery
    // (06-17) are GUI-proven. MC-DPSK and OFDM_NARROW keep their fixed negotiated rate
    // unless explicitly enabled (their adaptation is unvalidated on the faithful gate).
    return negotiated_mode_ == WaveformMode::OFDM_CHIRP;
}

// Shared escape ACTION (§RETX_PACING_DESIGN_2026_07_03 §2.2 — refactored out of
// maybeEscapeStuckFrame so the collapse-conditioned round escape REUSES it, not forks it).
// All guards (rateAdaptationActive, CONNECTED, mode_change_pending_, floor, in-flight)
// stay with the CALLERS — they keep first refusal exactly as before.
//
// DESC-SWITCH Phase-1 scope gate (docs/MODE_SWITCH_PIGGYBACK_DESIGN_2026_07_03.md §7):
// the escape drops fire MID-WINDOW (frames in flight, bypassing the clean-boundary gate
// BY DESIGN) — a descriptor-committed regrid here is only era-safe under the move-epoch
// machinery (ULTRA_ARQ_MOVE_EPOCH). These sender-initiated escapes stay on the legacy
// requestModeChange exchange in EVERY knob state — DELIBERATELY, Phase 2 included: they
// fire on zero-ACK evidence, i.e. precisely when the tone-burst control plane from the
// peer has gone silent, so a descriptor-only (announce-and-hope) commit would be aimed
// at a receiver that is demonstrably not confirming reception — the synchronized
// exchange doubles as the deaf-peer escalation (§6.5/§6 row 7). The RECEIVER-commanded
// demote (RX-RATE-CMD Phase 2, maybeApplyRxRateCommand below) is the mid-window case
// that DOES ride the descriptor commit: a command in hand proves the reverse control
// channel is alive, and the ARQ abort inside commitLocalModeSwitch bumps the epoch
// (setCodeRate rate-abort, or the abortPendingTx payload-drop bump for same-rate regrids — 2026-07-04 fix) for era safety.
void Connection::executeEscapeDrop(const char* trigger) {
    if (data_modulation_ == Modulation::QAM16 ||
        data_modulation_ == Modulation::QAM8) {
        // 8PSK revival (2026-07-05): QAM8 takes the same dense-constellation escape
        // EXIT as QAM16 (never the rate walk — the probe measured the walk sliding
        // QAM8 to the strictly-dominated R1/4). Differences handled below: no
        // noteQam16Demoted (QAM8 has no climb-in cooldown to meter) and no midrung
        // landing (that lever targets 16QAM R1/2).
        const bool esc_is_qam16 = data_modulation_ == Modulation::QAM16;
        // QAM16 top-gear stuck on a fade. When QAM16 craters off the decodability cliff it may emit
        // NO tone-burst ack at all, so the ack-driven demote in applyAdaptiveRateFeedback never sees
        // it — this escape path is the only one that fires. Demote STRAIGHT to the robust QPSK R3/4
        // home gear (not a QAM16 code-rate step — this applies from EITHER QAM16 rate, so the
        // ULTRA_QAM16_R34 crest rung takes the same straight exit) and arm a DOUBLE-weight re-climb
        // cooldown (a frame nearly died — stronger evidence than a soft demote, but not a permanent
        // forfeit of the next fade crest). requestModeChange re-anchors both stations at a clean
        // boundary.
        // MID-RUNG landing (ULTRA_QAM16_DEMOTE_MIDRUNG): the escape lands at 16QAM
        // R1/2 (2x margin, stays on QAM16 ⇒ no reclimb cooldown) unless already
        // there — a second escape then takes the QPSK R3/4 exit below.
        const bool midrung_exit = esc_is_qam16 &&
            qam16DemoteMidrungEnabled() && data_code_rate_ != CodeRate::R1_2;
        const Modulation esc_mod =
            midrung_exit ? Modulation::QAM16 : Modulation::QPSK;
        const CodeRate esc_rate =
            midrung_exit ? CodeRate::R1_2 : CodeRate::R3_4;
        if (!midrung_exit && esc_is_qam16) noteQam16Demoted(2);
        // PHASE-3 (2026-07-04): the FIRST escape of a silent stretch commits via the
        // descriptor (mid-window era-safe under move-epoch; the wire self-describes,
        // so a briefly-deaf peer re-syncs on the first descriptor it decodes, and the
        // waiting-rebase voice covers by-design unanchored silence). A SECOND escape
        // with still-zero ACK progress falls back to the legacy synchronized
        // exchange — the deaf-peer escalation ladder stays reachable. F1 measured
        // 20-30 s per legacy escape exchange in troughs; the descriptor commit is
        // ~free airtime.
        const bool desc_escape_ok = descriptor_mode_switch_enabled_ &&
                                    arq_.moveEpochEnabled() &&
                                    consecutive_escape_drops_ == 0;
        ++consecutive_escape_drops_;
        bool desc_committed = false;
        if (desc_escape_ok) {
            desc_committed = tryDescriptorModeSwitch(
                esc_mod, esc_rate, wireSnrDb(),
                v2::ModeChangeReason::CHANNEL_DEGRADED);
        }
        LOG_MODEM(WARN, "Connection: ESCAPE-drop %s %s -> %s %s (%s) via %s",
                  modulationToString(esc_is_qam16 ? Modulation::QAM16
                                                  : Modulation::QAM8),
                  codeRateToString(data_code_rate_), modulationToString(esc_mod),
                  codeRateToString(esc_rate), trigger,
                  desc_committed ? "DESC-SWITCH" : "MODE_CHANGE");
        if (!desc_committed) {
            // Wire SNR embed: freshness-gated pool aggregate under ULTRA_WIRE_SNR_FRESH
            // (stale sentinel -10 when nothing < 3*Tc), else the raw scalar (unchanged).
            requestModeChange(esc_mod, esc_rate, wireSnrDb(),
                              v2::ModeChangeReason::CHANNEL_DEGRADED);
        }
        return;
    }

    // A frame/window the current (over-climbed) rate genuinely cannot push through: the fade
    // troughs are killing it. It produces NO group ACK, so the ack-driven RateController never sees
    // it, and the clean-boundary gate defers any change because the stuck frame keeps the window
    // busy — so without this it grinds to max_retries and fails the whole transfer (Moderate@18:
    // adapt climbed R1/4->R3/4, a frame stuck 248 retx and died; locked R1/4 completed). Force a
    // ONE-rung drop to a more robust rate so the frame can punch through. requestModeChange
    // re-anchors both stations; setCodeRate re-sends the in-flight frames at the new rate KEEPING
    // their seqs (tx_next=tx_base) so there is no receiver seq-hole; ssthresh (noteRungFailed) stops
    // the controller from immediately climbing back into the rung that just failed.
    const CodeRate robust = rate_controller_.moreRobustRung(data_code_rate_);
    if (robust == data_code_rate_) return;
    rate_controller_.noteRungFailed(data_code_rate_);
    // PHASE-3 (2026-07-04): same first-escape-via-descriptor policy as the QAM16
    // branch above (see that comment); second consecutive silent escape -> legacy.
    const bool desc_escape_ok = descriptor_mode_switch_enabled_ &&
                                arq_.moveEpochEnabled() &&
                                consecutive_escape_drops_ == 0;
    ++consecutive_escape_drops_;
    bool desc_committed = false;
    if (desc_escape_ok) {
        desc_committed = tryDescriptorModeSwitch(
            data_modulation_, robust, wireSnrDb(),
            v2::ModeChangeReason::CHANNEL_DEGRADED);
    }
    LOG_MODEM(WARN, "Connection: ESCAPE-drop %s -> %s (%s) via %s",
              codeRateToString(data_code_rate_), codeRateToString(robust), trigger,
              desc_committed ? "DESC-SWITCH" : "MODE_CHANGE");
    if (!desc_committed) {
        requestModeChange(data_modulation_, robust, wireSnrDb(),
                          v2::ModeChangeReason::CHANNEL_DEGRADED);
    }
}

// ═══════ RX-RATE-CMD Phase 2 — receiver rung command in the tone-burst ACK ═══════
// docs/MODE_SWITCH_PIGGYBACK_DESIGN_2026_07_03.md §5.2 as amended (knob
// ULTRA_RX_RATE_CMD, default OFF = byte-identical; SEMANTICS-BREAKING lockstep when
// ON: live payload bits 42-43 + the widened tone-ACK CRC span). Motivation (the
// Phase-1 D3 rig finding): with climbs ~free under the descriptor commit, the ESCAPE
// side became the measured bottleneck — the RECEIVER sees a 16QAM crater IMMEDIATELY
// (failed group decode) while the sender only learns after ~2 zero-progress rounds
// (~2×RTO); 4 climb/escape cycles in 90 s on a Moderate epoch. The command closes
// that gap: the verdict rides the cratered group's own tone-burst ACK — the 4-FSK
// control plane whose detection floor out-survives every OFDM waveform by ~15-20 dB
// on fading (§1.2), exactly the channel a trough verdict must cross.
//
// DELIBERATE deviations from the design doc's §5.2 sketch (stated in-doc too):
//   - NO UP command (the sketch's value 1 = STEP-UP): climbs stay sender-side where
//     the quality EMA + ssthresh + climb streaks live — a second upward driver would
//     double-drive the control loop (two integrators, one plant) and re-open the
//     2026-06-09 churn arm. Demote-only keeps the command channel single-purpose:
//     trough-escape latency. Wire encoding: 0 none / 1 DOWN-one / 2 DOWN-hard /
//     3 reserved-hold (tone_burst_constants.hpp kRungCmd*).
//   - Emit policy is CRATER-ONLY DOWN-hard: the receiver has NO quality EMA/streak
//     machinery (the RateController runs sender-side only) and building a parallel
//     estimator is an explicit anti-goal. DOWN-one is defined on the wire and
//     consumed (forward-compatible), but nothing emits it yet.

// RECEIVER side (called from onBurstGroupReceived, before the group's ACK emits).
// Crater predicate — both arms derived from EXISTING policy quantizations, no new
// numeric constants:
//   frame_mask == 0  ⇔ ZERO frames delivered — the decoder's whole-fail/fast-NACK
//                      signature and the same zero-progress evidence class the
//                      sender's collapse escape counts. No finer grade exists on
//                      this axis: the decoder assigns quality EXACTLY 0.0 to any
//                      !all_ok group (streaming_burst_interleave.cpp), so the
//                      3-bit rate_hint quantization already saturates at 0 for
//                      every partial-fail — mask==0 is the only receiver-side
//                      distinction between "cliff" and "ordinary fade losses".
//   QAM16 only       — the modulation whose demote-on-a-single-bad-group policy is
//                      already codified sender-side (kQam16DemoteBadStreak = 1: the
//                      decodability-cliff asymmetry). At QPSK rungs a zero group is
//                      indistinguishable from an irreducible deep null (fading loss
//                      is irreducible at ANY rate); commanding DOWN there would
//                      bypass the deliberately EMA-smoothed policy and re-introduce
//                      the 2026-06-09 single-NACK ratchet-to-R1/4.
void Connection::updateRxRateCommandFromGroup(bool all_ok, uint16_t frame_mask) {
    using ultra::waveform::tone_burst_ack::kRungCmdDownHard;
    using ultra::waveform::tone_burst_ack::kRungCmdDownOne;
    using ultra::waveform::tone_burst_ack::kRungCmdNone;
    if (all_ok) {
        // Clean group — the bad stretch (if any) ended; nothing may keep riding.
        qam16_rx_bad_streak_ = 0;
        rx_rate_cmd_pending_ = kRungCmdNone;
        return;
    }
    if (data_modulation_ != Modulation::QAM16) {
        qam16_rx_bad_streak_ = 0;
        return;  // deep null at a robust rung: irreducible fading, not a rate signal
    }
    ++qam16_rx_bad_streak_;
    if (frame_mask != 0) {
        // PARTIAL failure (frames delivered, group not clean). A single partial is a
        // fade brush — clear any standing command (the base advances, fresh seqs, the
        // dedup would no longer swallow it). But TWO consecutive failed groups at
        // 16QAM = the rung is under water even without a total crater — and the
        // sender's own detection of that state costs ~40 s of zero-progress rounds
        // (F27 paid it twice; partials never froze the base so no crater command
        // fired). Command DOWN-ONE once per bad pair; the next group event either
        // clears it (delivered) or re-arms it (still failing — a new seq, and the
        // sender's per-ACK mode-snapshot guard keeps it to one rung per ACK).
        rx_rate_cmd_pending_ =
            (qam16_rx_bad_streak_ >= 2) ? kRungCmdDownOne : kRungCmdNone;
        return;
    }
    // Total crater at the high-order mode → command a demote. Idempotent across
    // consecutive craters: the SAME command keeps riding every re-emitted ACK until
    // the sender's move is observed (applyDataMode clears the latch on a real
    // mod/rate change — the once-per-committed-move rule). That re-carry is
    // ACK-loss diversity for free and storm-safe: a crater freezes the ARQ base,
    // so every copy carries ONE group_seq and the sender acts once.
    //
    // CREST-RUNG graded landing (2026-07-04, F10 finding): a crater AT 16QAM R3/4
    // commands DOWN-ONE (→ 16QAM R2/3, the sender's DownOne mapping) — R2/3's
    // requirement sits ~1.5 dB below R3/4's, so a trough that kills the crest rung
    // by inches usually leaves R2/3 alive (F10: the pre-hop R2/3 groups ran 0.85-0.98
    // in the same epoch). One rung per evidence quantum; if R2/3 craters too, the
    // NEXT command is DOWN-hard to the QPSK home gear. R2/3 craters keep the
    // straight-to-QPSK exit (the decodability-cliff asymmetry).
    rx_rate_cmd_pending_ =
        (data_code_rate_ == CodeRate::R3_4)
            ? ultra::waveform::tone_burst_ack::kRungCmdDownOne
            : kRungCmdDownHard;
}

// ═══════════ RX-AUTHORITY receiver verdict (ULTRA_RX_RATE_AUTHORITY) ═══════════
// Called per delivered/failed burst group, BEFORE the group's ACK emits. Maps the
// receiver's FRESH channel observation (broadband SNR EMA + coherence-adjusted
// fading, fed via setBurstChannelObservation from the decoder's per-frame atomics)
// through selectCoherentOFDM — the SAME anchor tables used at connect — into an
// absolute canonical rung command. Multi-rung moves in both directions are the
// point: the map's input is already alpha-smoothed, so stability lives in the
// measurement, not in streak counters. Two decode-evidence overrides keep the
// verdict honest when the meter and the decoder disagree:
//   - CRATER (quality <= 0): never command AT or ABOVE the rung that just failed —
//     clamp to one canonical step below the current rung, whatever the SNR map
//     says (the map's fading class lags a fresh trough).
//   - CLEAN group: never command BELOW the current rung (the rung is proven
//     working THIS group; a stale-low SNR reading must not thrash it downward).
void Connection::updateRxAuthorityCommand(bool all_ok, float quality) {
    if (state_ != ConnectionState::CONNECTED ||
        negotiated_mode_ != WaveformMode::OFDM_CHIRP) {
        rx_authority_cmd_ = kRungIdxNone;
        return;
    }
    if (burst_obs_snr_db_ < 0.0f) {
        // No fresh observation yet this connection — command nothing rather than
        // steer on handshake-stale state.
        rx_authority_cmd_ = kRungIdxNone;
        return;
    }
    // FADE-AVERAGED verdict SNR (2026-07-05, first-probe finding): the per-frame
    // broadband EMA swung 16.9..26.8 dB on a dial-20 Good channel — a fade
    // snapshot. Commanding on it aliases the fade cycle (crest reading -> climb ->
    // trough crater -> demote -> repeat; 12 moves/300 s measured). The rung anchors
    // are calibrated on dial-equivalent SNR, so the verdict input is the dB mean of
    // the last few group observations (~30 s ≈ many Tc). Decode-evidence overrides
    // below keep the fast reactions (crater = instant clamp).
    const float inst_fading = connection_policy::coherenceAdjustedFadingIndex(
        (burst_obs_fading_ >= 0.0f) ? burst_obs_fading_ : fading_index_,
        burst_obs_coh_score_, burst_obs_coh_valid_);
    rx_auth_obs_db_[rx_auth_obs_next_] = burst_obs_snr_db_;
    rx_auth_fading_ring_[rx_auth_obs_next_] = inst_fading;
    rx_auth_obs_age_ms_[rx_auth_obs_next_] = 0;
    rx_auth_obs_next_ = (rx_auth_obs_next_ + 1) % kRxAuthObsRing;
    if (rx_auth_obs_count_ < kRxAuthObsRing) ++rx_auth_obs_count_;
    float snr_sum = 0.0f;
    float fading_sum = 0.0f;
    int snr_n = 0;
    for (size_t i = 0; i < rx_auth_obs_count_; ++i) {
        if (rx_auth_obs_age_ms_[i] <= kRxAuthObsMaxAgeMs) {
            snr_sum += rx_auth_obs_db_[i];
            fading_sum += rx_auth_fading_ring_[i];
            ++snr_n;
        }
    }
    const float snr_avg = (snr_n > 0) ? (snr_sum / static_cast<float>(snr_n))
                                      : burst_obs_snr_db_;
    const float fading_avg = (snr_n > 0)
        ? (fading_sum / static_cast<float>(snr_n)) : inst_fading;
    // STICKY CLASS (F123): the anchor table's three fading columns are cliffs and
    // the class boundary is intrinsically fuzzy — a column switch must be an epoch
    // verdict, not a snapshot. Adopt a new class only when the SMOOTHED fading's
    // class persists 2 consecutive group verdicts; until then keep feeding the map
    // the last in-class fading value.
    const int raw_class = static_cast<int>(classifyFading(fading_avg));
    float eff_fading = fading_avg;
    if (raw_class == rx_auth_class_sticky_) {
        rx_auth_class_streak_ = 0;
        rx_auth_fading_passed_ = fading_avg;
    } else if (++rx_auth_class_streak_ >= 2) {
        rx_auth_class_sticky_ = raw_class;
        rx_auth_class_streak_ = 0;
        rx_auth_fading_passed_ = fading_avg;
        LOG_MODEM(INFO, "Connection: RX-AUTHORITY fading class -> %d (fading=%.2f)",
                  raw_class, fading_avg);
    } else {
        eff_fading = rx_auth_fading_passed_;  // unconfirmed flap: hold the column
    }
    const CoherentPick mapped = selectCoherentOFDM(snr_avg, eff_fading);
    uint8_t cmd = coherentRungIndexFor(mapped.mod, mapped.rate);
    const uint8_t cur = coherentRungIndexFor(data_modulation_, data_code_rate_);
    // TWO-CRATER rule + CRATER-MARGIN memory (F122: 10 moves/283 s — a single
    // crater at a ~10 s decision quantum vs Tc 2-4 s is an irreducible deep null
    // the ARQ absorbs, NOT rate evidence; chasing singles paid the full-anchor +
    // requeue-rewind tax every ~28 s). Only CONSECUTIVE craters demote and charge
    // the rung's margin memory (+2 dB, cap 6; posterior beats prior). Every clean
    // group decays all penalties (0.25 dB) — the fade epoch ends, evidence expires.
    const bool crater = (quality <= 0.0f && !all_ok);
    if (crater) ++rx_auth_crater_streak_;
    else if (all_ok) rx_auth_crater_streak_ = 0;
    const bool crater_confirmed = crater && rx_auth_crater_streak_ >= 2;
    if (cur != kRungIdxNone && cur < kRungIdxCount) {
        if (crater_confirmed) {
            rx_auth_rung_penalty_db_[cur] =
                std::min(6.0f, rx_auth_rung_penalty_db_[cur] + 2.0f);
        } else if (all_ok) {
            for (size_t i = 0; i < kRungIdxCount; ++i) {
                rx_auth_rung_penalty_db_[i] =
                    std::max(0.0f, rx_auth_rung_penalty_db_[i] - 0.25f);
            }
        }
    }
    if (cur != kRungIdxNone) {
        if (cmd > cur) {
            // CLIMB HYSTERESIS + crater margin: an up-command must survive a
            // haircut — the base 2.5 dB guards against slow swells; the target
            // rung's crater penalty demands the channel PROVE headroom the anchors
            // only assumed. Descents take the map directly (down is safe).
            constexpr float kClimbMarginDb = 2.5f;
            const float haircut = kClimbMarginDb +
                ((cmd < kRungIdxCount) ? rx_auth_rung_penalty_db_[cmd] : 0.0f);
            const CoherentPick guarded =
                selectCoherentOFDM(snr_avg - haircut, eff_fading);
            const uint8_t guarded_idx =
                coherentRungIndexFor(guarded.mod, guarded.rate);
            if (guarded_idx <= cur) cmd = cur;  // not a margin-proof climb: hold
        }
        if (crater_confirmed) {
            // Confirmed-crater override: two in a row is the rung failing, not a
            // null — command below whatever the (lagging) map says.
            const uint8_t below = static_cast<uint8_t>(cur > 1 ? cur - 1 : 1);
            if (cmd >= cur) cmd = below;
        } else if (crater && cmd > cur) {
            // Single crater: hold the rung (the ARQ resends through the null) —
            // but never climb ON a crater either.
            cmd = cur;
        } else if (all_ok && cmd < cur) {
            // Clean-group override: this rung just WORKED end to end.
            cmd = cur;
        }
        if (!crater_confirmed && cmd < cur) {
            // DOWN RATE-LIMIT (F123): a map-driven demote without confirmed decode
            // failure steps at most 2 canonical rungs per verdict — a residual
            // input swing must never crash the ladder to the basement in one
            // command (measured: QPSK R1/2 commanded at a steady 24 dB when a
            // fading-class flap switched anchor columns). Confirmed craters and
            // repeated verdicts still reach any depth, one bounded step at a time.
            const uint8_t floor_step = static_cast<uint8_t>(cur > 2 ? cur - 2 : 1);
            if (cmd < floor_step) cmd = floor_step;
        }
    }
    // Canonical indices stay < 24 so bits [rung_cmd] never equal kRungCmdReserved
    // (3) — the WAITING-REBASE voice encoding stays unambiguous (it is also
    // type=NACK, but keep the value space disjoint regardless).
    static_assert(kRungIdxCount <= 24, "rung index would alias the rebase voice");
    rx_authority_cmd_ = cmd;
    if (cmd != cur) {
        LOG_MODEM(INFO,
                  "Connection: RX-AUTHORITY verdict %s %s (idx %u -> %u) "
                  "snr_avg=%.1f (inst=%.1f n=%d) fading=%.2f coh=%.2f q=%.2f",
                  modulationToString(mapped.mod), codeRateToString(mapped.rate),
                  cur, cmd, snr_avg, burst_obs_snr_db_, snr_n, eff_fading,
                  burst_obs_coh_score_, quality);
    }
}

// SENDER side of RX-AUTHORITY: obey a non-zero absolute rung command from the
// receiver's ACK. Dedup by target (repeat ACK copies re-carry the same command);
// clamp to the locally-enabled ladder (env knobs may differ across ends); commit
// via the descriptor (mid-window era-safe) with the legacy MODE_CHANGE fallback.
void Connection::maybeObeyAuthorityCommand(uint8_t cmd_idx) {
    if (cmd_idx == kRungIdxNone || cmd_idx >= kRungIdxCount) return;
    if (state_ != ConnectionState::CONNECTED ||
        negotiated_mode_ != WaveformMode::OFDM_CHIRP) return;
    if (mode_change_pending_) return;  // a move is already in flight — obey later copies
    const CoherentPick pick = coherentRungFromIndex(cmd_idx);
    Modulation mod = pick.mod;
    CodeRate rate = pick.rate;
    if (!coherentRungLocallyEnabled(mod, rate)) {
        // Local ladder doesn't know the rung (knob mismatch) — take the local map's
        // pick for the receiver-reported situation instead of an unvalidated rung.
        LOG_MODEM(WARN,
                  "Connection: RX-AUTHORITY command idx=%u (%s %s) not locally "
                  "enabled — holding current rung",
                  cmd_idx, modulationToString(mod), codeRateToString(rate));
        return;
    }
    if (mod == data_modulation_ && rate == data_code_rate_) {
        tx_authority_last_obeyed_ = cmd_idx;  // already there — arm dedup anyway
        return;
    }
    if (cmd_idx == tx_authority_last_obeyed_) {
        // Same target as last obeyed but we're not there yet (descriptor commit
        // still propagating / legacy exchange in flight) — don't re-fire on every
        // repeated ACK copy carrying the same command.
        return;
    }
    const bool faster = isFasterMode(mod, rate, data_modulation_, data_code_rate_);
    if (faster) {
        // UP-commands defer to a CLEAN send boundary (F122): a mid-window switch
        // discards the receiver's buffered frames and rewinds the file cursor by
        // the whole in-flight window — pure gain-chasing must never pay that
        // redundant-airtime tax. The receiver re-stamps the command on every ACK,
        // so the deferred climb re-asserts free at the next full-ack tick.
        // DOWN-commands obey immediately: a failing window never drains (waiting
        // would deadlock), and its frames need resending anyway — the rewind is
        // free information-wise; move-epoch makes it era-safe.
        const bool busy =
            arq_.getTxInFlightBytes() > 0 ||
            (file_transfer_.getState() == FileTransferState::SENDING &&
             file_transfer_.hasPendingChunks());
        if (busy) {
            LOG_MODEM(DEBUG,
                      "Connection: RX-AUTHORITY hold climb idx=%u for clean boundary",
                      cmd_idx);
            return;  // do NOT arm dedup — the re-carried command must retry
        }
    }
    const uint8_t reason = faster ? v2::ModeChangeReason::CHANNEL_IMPROVED
                                  : v2::ModeChangeReason::CHANNEL_DEGRADED;
    const char* old_mod = modulationToString(data_modulation_);
    const char* old_rate = codeRateToString(data_code_rate_);
    const bool desc_committed =
        tryDescriptorModeSwitch(mod, rate, wireSnrDb(), reason);
    if (!desc_committed) {
        requestModeChange(mod, rate, wireSnrDb(), reason);
    }
    tx_authority_last_obeyed_ = cmd_idx;
    LOG_MODEM(INFO, "Connection: RX-AUTHORITY obey %s %s -> %s %s via %s",
              old_mod, old_rate, modulationToString(mod), codeRateToString(rate),
              desc_committed ? "DESC-SWITCH" : "MODE_CHANGE");
}

// SENDER side (called from onToneBurstAck, inside the defer-refill bracket, AFTER
// applyAdaptiveRateFeedback). The command is ADVISORY: it routes through the sender's
// own guards, ladder tables, caps and cooldowns — the receiver is authoritative about
// what it could not decode; the sender stays authoritative about what it transmits
// (design §4.1 arbitration).
void Connection::maybeApplyRxRateCommand(uint8_t cmd, uint8_t group_seq,
                                         Modulation mod_at_ack, CodeRate rate_at_ack) {
    using ultra::waveform::tone_burst_ack::kRungCmdDownHard;
    using ultra::waveform::tone_burst_ack::kRungCmdDownOne;
    if (!rx_rate_cmd_enabled_) return;  // knob OFF: byte-identical (bits ignored)
    if (cmd != kRungCmdDownOne && cmd != kRungCmdDownHard) {
        return;  // 0 = no command; 3 = reserved, treat as hold (forward compat)
    }
    if (state_ != ConnectionState::CONNECTED) return;
    if (negotiated_mode_ != WaveformMode::OFDM_CHIRP) return;  // wideband ladder only
    // One action attempt per command episode: a crater freezes the ARQ base, so every
    // re-emitted copy of the command carries the same group_seq (the drive_advisory
    // dedup pattern, connection.cpp:1718-1731 class). Recorded BEFORE the policy
    // guards below — a transiently-blocked command is dropped, not retried later
    // under the same seq (fail-soft to the sender's own escape backstops, which
    // remain armed and unchanged).
    if (static_cast<int>(group_seq) == rx_rate_cmd_seq_seen_) return;
    rx_rate_cmd_seq_seen_ = group_seq;
    if (!rateAdaptationActive()) return;  // ULTRA_LOCK_RATE / rate-adapt-off wins
    if (mode_change_pending_) return;     // a legacy exchange is already re-anchoring
    if (mod_at_ack != data_modulation_ || rate_at_ack != data_code_rate_) {
        // The EMA/QAM16 machinery already moved the rung on THIS ack (the command and
        // the quality byte are the same channel evidence measured two ways) — one ACK
        // may move the ladder at most once.
        return;
    }

    // Target selection — the sender's OWN ladder tables, never a receiver-named rung:
    // DOWN-hard mirrors executeEscapeDrop exactly (crater semantics: QAM16 → straight
    // to the robust QPSK R3/4 home gear + double-weight re-climb cooldown; below QAM16
    // → one rung more robust + noteRungFailed so ssthresh remembers the cratered
    // rung). DOWN-one mirrors the soft ack-driven demote ladder (single-weight).
    Modulation target_mod = data_modulation_;
    CodeRate target_rate = data_code_rate_;
    if (data_modulation_ == Modulation::QAM16) {
        if (cmd == kRungCmdDownOne && qam16R34Enabled() &&
            data_code_rate_ == CodeRate::R3_4) {
            target_rate = CodeRate::R2_3;  // crest-rung step-down: stays on QAM16
        } else if (qam16DemoteMidrungEnabled() &&
                   data_code_rate_ != CodeRate::R1_2) {
            // MID-RUNG landing: stay on 16QAM at R1/2 (2x margin) instead of the
            // QPSK R3/4 exit. Stays-on-QAM16 ⇒ no noteQam16Demoted (mirrors the
            // crest step-down above); a further crater lands QPSK via the else.
            target_rate = CodeRate::R1_2;
        } else {
            target_mod = Modulation::QPSK;
            target_rate = CodeRate::R3_4;
            noteQam16Demoted(cmd == kRungCmdDownHard ? 2 : 1);
        }
    } else {
        const CodeRate robust = rate_controller_.moreRobustRung(data_code_rate_);
        if (robust == data_code_rate_) {
            return;  // already the most robust rung — irreducible (floor guard)
        }
        if (cmd == kRungCmdDownHard) {
            rate_controller_.noteRungFailed(data_code_rate_);
        }
        target_rate = robust;
    }

    // COMMIT. Mid-window is the whole point of the command (the crater keeps the
    // window busy, so the clean boundary the Phase-1 sites wait for never comes):
    // a descriptor commit there regrids live seqs → only era-safe under the
    // move-epoch machinery, so gate on ULTRA_ARQ_MOVE_EPOCH and fall back to the
    // legacy synchronized exchange without it (exactly executeEscapeDrop's commit).
    // At a clean boundary no epoch is needed (Phase-1 invariant: empty window ⇒
    // nothing to abort). tryDescriptorModeSwitch re-checks the Phase-1 knob and
    // scope (incl. the descriptor-bearing >1-frame-remaining guard) and returns
    // false out of scope — the guards compose, never duplicate.
    const bool busy =
        file_transfer_.getState() == FileTransferState::SENDING &&
        (file_transfer_.hasPendingChunks() || arq_.getTxInFlightBytes() > 0);
    bool desc_committed = false;
    if (!busy || arq_.moveEpochEnabled()) {
        desc_committed = tryDescriptorModeSwitch(
            target_mod, target_rate, wireSnrDb(),
            v2::ModeChangeReason::CHANNEL_DEGRADED);
    }
    if (!desc_committed) {
        requestModeChange(target_mod, target_rate, wireSnrDb(),
                          v2::ModeChangeReason::CHANNEL_DEGRADED);
    }
    char buf[112];
    std::snprintf(buf, sizeof(buf), "RX-RATE-CMD %s: %s %s -> %s %s via %s (seq=%u)",
                  cmd == kRungCmdDownHard ? "down-hard" : "down-one",
                  modulationToString(mod_at_ack), codeRateToString(rate_at_ack),
                  modulationToString(target_mod), codeRateToString(target_rate),
                  desc_committed ? "DESC-SWITCH" : "MODE_CHANGE",
                  static_cast<unsigned>(group_seq));
    LOG_MODEM(WARN, "Connection: %s", buf);
    last_adaptive_action_ = buf;
}

void Connection::maybeEscapeStuckFrame() {
    if (!rateAdaptationActive()) return;
    if (state_ != ConnectionState::CONNECTED) return;
    if (mode_change_pending_) return;             // a rate change is already in flight
    if (!isOFDMMode(negotiated_mode_)) return;    // burst-transport rate ladder only
    if (arq_.getTxInFlightBytes() == 0) return;   // nothing in flight to be stuck
    if (rate_controller_.isAtFloor(data_code_rate_)) return;  // already most robust — irreducible
    if (arq_.maxInFlightRetryCount() < stuckRetransmitEscape()) return;

    // The pathological-single-frame 5-retx backstop (ULTRA_STUCK_ESCAPE_RETX), kept verbatim
    // (§RETX_PACING_DESIGN_2026_07_03 §2.3). During genuine collapses the round-conditioned
    // maybeCollapseEscape typically preempts this (2 rounds ≈ retry 2-3 < 5); on healthy
    // windows only this backstop can fire — exactly the pre-pacing behavior.
    char trigger[64];
    std::snprintf(trigger, sizeof(trigger), "frame stuck, %d retx at current rate",
                  arq_.maxInFlightRetryCount());
    executeEscapeDrop(trigger);
}

// Collapse-conditioned escape (§RETX_PACING_DESIGN_2026_07_03 §2, behind
// ULTRA_COLLAPSE_ESCAPE_ROUNDS, default OFF). Polled from the CONNECTED tick beside
// maybeEscapeStuckFrame — NEVER fired from inside an ARQ callback (an RTO-batch round
// increments the counter inside the ARQ transmit callback; the escape lands one tick
// later in a proven-safe context). Zero-DELIVERED evidence: only a whole-window zero
// streak (the g43/rig frozen-base signature) trips it; any progress reset the counter
// (noteArqRoundOutcome), so a lone straggler amid deliveries cannot (g42-protective).
void Connection::maybeCollapseEscape() {
    const int rounds_needed = collapseEscapeRounds();
    if (rounds_needed <= 0) return;               // knob OFF (default) — byte-identical
    if (zero_progress_rounds_ < rounds_needed) return;
    if (!rateAdaptationActive()) return;
    if (state_ != ConnectionState::CONNECTED) return;
    if (mode_change_pending_) return;             // a rate change is already in flight
    if (negotiated_mode_ != WaveformMode::OFDM_CHIRP) return;  // scope gate (§1.3/§6.2)
    if (arq_.getTxInFlightBytes() == 0) return;   // nothing in flight to be collapsing
    if (rate_controller_.isAtFloor(data_code_rate_)) return;  // already most robust — irreducible

    // ESCAPE EPISODE CAP (ULTRA_ESCAPE_EPISODE_CAP, default 0 = OFF = unlimited/legacy).
    // A fade null is TIME-bounded (~Tc-scale), not rate-bounded: during the null NO rung
    // delivers (SNR is -inf in the null regardless of code rate), so zero-progress
    // evidence measures time-stuck, not rate error. ONE drop hedges the post-null
    // decode; further drops buy zero delivery during the null and cost minutes of
    // under-rated cruise after (F89: one trough cascaded R3/4->R2/3->R1/2->R1/4 while
    // the deliveries BETWEEN stalls ran q=0.94-0.99). Saturate at the cap per silent
    // episode (consecutive_escape_drops_ resets on ANY ack progress — the episode
    // boundary); the trough-pacing hold owns the time axis, and a genuinely over-
    // climbed rung post-null still demotes via the ack-driven RateController (partial
    // deliveries feed it) and the stuck-frame escape (frame-death evidence, uncapped).
    static const int kEscapeEpisodeCap = [] {
        if (const char* e = std::getenv("ULTRA_ESCAPE_EPISODE_CAP")) {
            const int n = std::atoi(e);
            if (n >= 1 && n <= 8) return n;
        }
        return 1;  // DEFAULT 2026-07-05 (campaign standing value; 0 = unlimited cascade)
    }();
    if (kEscapeEpisodeCap > 0 && consecutive_escape_drops_ >= kEscapeEpisodeCap) {
        LOG_MODEM(INFO,
                  "Connection: COLLAPSE-escape SATURATED (episode cap %d, drops %d) — "
                  "holding rung through the null",
                  kEscapeEpisodeCap, consecutive_escape_drops_);
        zero_progress_rounds_ = 0;  // consume the evidence; re-accumulates before the next poll
        return;
    }

    // §2.1 window-collapse evidence: ≥⌈burst_cap/2⌉ frames pending at the current rung.
    // in_flight ≥ ceil(cap/2) ⇔ 2·in_flight ≥ cap (integer).
    const size_t burst_cap = burstAirtimeBudgetFrames(arq_.getWindowSize());
    const size_t in_flight_frames = arq_.getWindowSize() - arq_.getAvailableSlots();
    if (2 * in_flight_frames < burst_cap) return;

    LOG_MODEM(WARN, "Connection: COLLAPSE-escape (%d zero rounds)", zero_progress_rounds_);
    char trigger[64];
    std::snprintf(trigger, sizeof(trigger), "collapse, %d zero-progress rounds",
                  zero_progress_rounds_);
    // Reset the era before the drop — requestModeChange sets mode_change_pending_ and the
    // commit path (applyDataMode) starts a new era anyway; this keeps the counter from
    // double-firing if the MODE_CHANGE round-trip is slow.
    zero_progress_rounds_ = 0;
    retx_pace_hold_ms_ = 0;
    executeEscapeDrop(trigger);
}

// §RETX_PACING_DESIGN_2026_07_03 §1.3 scope gate: CONNECTED **wideband** OFDM
// (OFDM_CHIRP only) on the unified tone-burst burst path, file SENDING with bytes in
// flight — the mirror of the escape's guards. MC-DPSK and OFDM_NARROW are explicitly OUT
// of scope (§6.2): their ACK timers were just re-derived (BUG-MCDPSK-ACK-COLLISION /
// BUG-MCDPSK-FILE-COMPLETION) and their RTT already dwarfs any Tc.
bool Connection::retxPacingScopeActive() const {
    return state_ == ConnectionState::CONNECTED &&
           negotiated_mode_ == WaveformMode::OFDM_CHIRP &&
           use_burst_transport_ && kUnifiedSeqEnabled() &&
           file_transfer_.getState() == FileTransferState::SENDING &&
           arq_.getTxInFlightBytes() > 0;
}

// §RETX-PACING: record the modeled END of an OFDM data-burst key-down (flush time +
// airtime derived from the SAME wideOFDMBurstAirtimeMs model the budget/RTO use). This is
// the reference point for T_defer's t_since_last_tx_end subtraction (§1.2): by the time
// the sender LEARNS a round was zero-progress it has already spent part of Tc listening —
// ~3-4 s on the fast-NACK path (deferral bites), ~10+ s on the RTO path (deferral ≈ 0 at
// Good, correct: the RTO already over-paces that path). Recording is unconditional and
// behavior-free (a clock read + member store); every DECISION stays knob-gated.
void Connection::noteDataBurstKeydown(size_t frame_count) {
    if (negotiated_mode_ != WaveformMode::OFDM_CHIRP || frame_count == 0) {
        return;
    }
    const uint32_t reanchor_ms =
        connection_policy::shouldUseWideOFDMShortReanchor(
            negotiated_mode_, data_modulation_, fading_index_)
            ? connection_policy::wideOFDMShortReanchorChirpDurationMs()
            : 0;
    const uint32_t airtime_ms = connection_policy::wideOFDMBurstAirtimeMs(
        data_modulation_, data_code_rate_, frame_count, data_frame_cw_count_,
        reanchor_ms, selectBurstLiftingZ());
    last_data_burst_end_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(airtime_ms);
    last_data_burst_end_valid_ = true;
}

uint32_t Connection::elapsedSinceLastDataBurstEndMs() const {
    if (!last_data_burst_end_valid_) {
        return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now <= last_data_burst_end_) {
        return 0;  // still (modeled as) keyed down — no listening time elapsed yet
    }
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - last_data_burst_end_).count();
    return static_cast<uint32_t>(std::min<long long>(ms, 0xFFFFFFFFll));
}

// §RETX_PACING_DESIGN_2026_07_03 §1.1/§1.2: one call per ROUND boundary.
//   progress_frames > 0  → the channel just proved it delivers: reset the zero-round
//                          streak and EARLY-RELEASE any armed hold (never wait out a hold
//                          when a late/duplicate SACK finally decodes) — §2.3
//                          g42-protective property.
//   progress_frames == 0 → a fully-failed round (fresh ack with no base advance and no
//                          new SACK bit, or a slot-RTO batch — a timeout IS the absence
//                          of an ack): count it; knob-gated, arm the channel-derived
//                          deferral on BOTH resend triggers (§1.3).
//   progress_frames < 0  → no fresh ack was processed (duplicate/stale/future — the ARQ
//                          ack-signature dedup): NOT a round, touch nothing.
// Partial-SACK rounds land here with progress > 0 and therefore resend immediately
// (status quo) — only zero-progress rounds ever defer (§3).
// TROUGH AMNESTY (ULTRA_TROUGH_AMNESTY, default OFF = byte-identical). A fade null is
// TIME-bounded: a rung proven clean seconds before it is not invalidated by it. Yet the
// down-side (collapse escape + quality-EMA demote) and the up-side (ssthresh pins +
// per-rung climb streaks) both treat trough evidence as RATE evidence — F89/F91 measured
// a ~20 s bidirectional/forward null demoting three rungs and then sitting at R1/4 for
// minutes while delivering 8/8 q=0.9+. When the episode ends (the first progress-bearing
// ack), restore the pre-episode rung directly: if the channel genuinely worsened, one
// crater at the restored rung re-demotes in ~3.5 s (receiver rung command) — bounded
// downside, minutes of upside. Ordering with the receiver's rung command: the command
// applies BEFORE the round outcome in onToneBurstAck, so (a) an episode that starts on a
// crater ack snapshots the POST-crater rung (crater evidence kept), and (b) amnesty
// SKIPS when this ack carries a fresh DOWN command (receiver evidence wins).
static int coherentRungOrdinal(Modulation m, CodeRate r) {
    const int mod_rank = (m == Modulation::QAM16) ? 1 : 0;
    return mod_rank * 16 + static_cast<int>(ofdmCodeRateValue(r) * 12.0f + 0.5f);
}
void Connection::maybeTroughAmnesty(int progress_frames, uint8_t rung_cmd) {
    if (!trough_episode_active_ || progress_frames <= 0) return;
    trough_episode_active_ = false;  // the episode is over either way
    static const bool kAmnestyOn = [] {
        const char* e = std::getenv("ULTRA_TROUGH_AMNESTY");
        return !(e && e[0] == '0');  // DEFAULT-ON 2026-07-05 (inert under authority)
    }();
    if (!kAmnestyOn) return;
    if (rung_cmd != ultra::waveform::tone_burst_ack::kRungCmdNone) return;
    if (state_ != ConnectionState::CONNECTED) return;
    if (negotiated_mode_ != WaveformMode::OFDM_CHIRP) return;
    if (!rateAdaptationActive() || mode_change_pending_) return;
    if (coherentRungOrdinal(data_modulation_, data_code_rate_) >=
        coherentRungOrdinal(pre_episode_mod_, pre_episode_rate_)) {
        return;  // nothing was lost to the trough
    }
    LOG_MODEM(WARN,
              "Connection: TROUGH-AMNESTY restore %s %s -> %s %s (null ended; "
              "trough demotes are not rate evidence)",
              modulationToString(data_modulation_), codeRateToString(data_code_rate_),
              modulationToString(pre_episode_mod_), codeRateToString(pre_episode_rate_));
    // Same commit envelope as maybeApplyRxRateCommand (we are inside the ack's
    // defer-refill bracket): descriptor commit when era-safe, legacy exchange fallback.
    bool desc_committed = false;
    const bool busy =
        file_transfer_.getState() == FileTransferState::SENDING &&
        (file_transfer_.hasPendingChunks() || arq_.getTxInFlightBytes() > 0);
    if (!busy || arq_.moveEpochEnabled()) {
        desc_committed = tryDescriptorModeSwitch(
            pre_episode_mod_, pre_episode_rate_, wireSnrDb(),
            v2::ModeChangeReason::CHANNEL_IMPROVED);
    }
    if (!desc_committed) {
        requestModeChange(pre_episode_mod_, pre_episode_rate_, wireSnrDb(),
                          v2::ModeChangeReason::CHANNEL_IMPROVED);
    }
}

void Connection::noteArqRoundOutcome(int progress_frames, const char* origin) {
    if (progress_frames > 0) {
        if (retx_pace_hold_ms_ > 0) {
            LOG_MODEM(INFO,
                      "Connection: TROUGH-PACING early release (%d frames progressed, %s)",
                      progress_frames, origin);
        }
        zero_progress_rounds_ = 0;
        retx_pace_hold_ms_ = 0;
        consecutive_escape_drops_ = 0;  // PHASE-3: channel proved alive — descriptor escapes re-enabled
        return;
    }
    if (progress_frames < 0) {
        return;  // duplicate/stale ack — never a phantom round (§1.1 dedup)
    }
    if (!retxPacingScopeActive()) {
        // Out of scope (MC-DPSK / OFDM_NARROW / no file in flight): never accumulate
        // rounds or holds here — their timers must never see this machinery (§6.2).
        zero_progress_rounds_ = 0;
        retx_pace_hold_ms_ = 0;
        return;
    }

    ++zero_progress_rounds_;
    // TROUGH AMNESTY: snapshot the rung active as the episode BEGINS. Runs after
    // maybeApplyRxRateCommand in the ack path, so a crater rung-command landing on
    // this same ack has already applied — the snapshot is the post-crater rung
    // (legitimate receiver evidence is kept; only trough-driven drops are amnestied).
    if (zero_progress_rounds_ == 1) {
        trough_episode_active_ = true;
        pre_episode_mod_ = data_modulation_;
        pre_episode_rate_ = data_code_rate_;
    }
    LOG_MODEM(INFO, "Connection: zero-progress ARQ round %d (%s)",
              zero_progress_rounds_, origin);
    // The §2 collapse escape is POLLED from the CONNECTED tick (maybeCollapseEscape), not
    // fired here — this function runs inside ack processing AND inside the ARQ transmit
    // callback (RTO batch), and requestModeChange must not re-enter the ARQ from the latter.
    if (!retxTroughPacingEnabled()) {
        return;  // §5.1 master knob OFF (default) — counting above is inert bookkeeping
    }
    if (mode_change_pending_) {
        return;  // a rate change is already re-anchoring the era; don't stack a hold on it
    }
    const uint32_t elapsed_ms = elapsedSinceLastDataBurstEndMs();
    const float doppler_hz = coherence_valid_ ? coherence_doppler_hz_ : 0.0f;
    const uint32_t hold_ms = connection_policy::retxTroughDeferMs(
        doppler_hz, fading_index_, coherence_score_, coherence_valid_,
        zero_progress_rounds_, elapsed_ms, troughDeferTcFrac());
    if (hold_ms == 0) {
        return;  // e.g. the ~18 s RTO path already out-waited Tc (§1.2) — add nothing
    }
    retx_pace_hold_ms_ = hold_ms;                 // trigger #1: turn refill (runDeferredArqRefill)
    arq_.deferPendingRetransmits(hold_ms);        // trigger #2: per-slot RTO (one state, both)
    const float tc_s = static_cast<float>(connection_policy::coherenceTimeMsForDoppler(
        connection_policy::retxTroughDopplerHz(doppler_hz, fading_index_,
                                               coherence_score_, coherence_valid_))) /
        1000.0f;
    LOG_MODEM(WARN,
              "Connection: TROUGH-PACING defer %ums (round %d, Tc=%.2fs, elapsed=%ums, %s)",
              hold_ms, zero_progress_rounds_, tc_s, elapsed_ms, origin);
    // Label the pause for the operator (2 AM waterfall rule, §4): a visible "Adapt:" text,
    // not a silent hang. Also the A/B grep hook.
    char buf[96];
    std::snprintf(buf, sizeof(buf), "pace-hold %ums (zero-progress round %d)",
                  hold_ms, zero_progress_rounds_);
    last_adaptive_action_ = buf;
}

void Connection::applyAdaptiveRateFeedback(float quality) {
    // §14.36 Phase 5c: the SENDER runs the rate controller on the receiver's
    // decode-headroom feedback (carried on the GROUP_ACK; a GROUP_NACK feeds 0).
    // It owns the rate, so data_code_rate_ is the rate the just-acked group used.
    if (!adaptive_rate_enabled_ || !use_burst_transport_) {
        return;
    }
    if (quality < 0.0f) {
        return;  // no feedback byte (old peer / adaptation off on the far end)
    }
    // GUI "Adapt:" headroom bar reads lastGroupQuality(). Record EVERY valid sample here —
    // before the lock-rate / rate-change branches below each return — or the bar stays at the
    // init -1.0 forever ("waiting for first group..."). The unification dropped this assignment
    // (only last_adaptive_action_ text was kept), so the bar never populated mid-transfer.
    // NOTE: on the unified tone-burst path `quality` is currently the binary ack(1.0)/nack(0.0)
    // signal; a graded decode-headroom would need the tone-burst rate_hint field wired through
    // (TODO) — but binary already drives the green/red bar instead of "waiting" all run.
    last_group_quality_ = quality;
    // Rate ADAPTATION is DEFAULT-ON for connected wideband OFDM (2026-07-02, fade-riding
    // ladder; was default-OFF 2026-06-07..07-01). The §14.43 closed-loop quality feedback is
    // wired end-to-end (receiver LDPC headroom -> rate_hint -> here). Opt out with
    // ULTRA_RATE_ADAPT=0 or pin with ULTRA_LOCK_RATE=1 — then the graded quality still drives
    // the GUI "Adapt:" bar (last_group_quality_, set above) + the action text, so we can SEE
    // what the controller WOULD do without it moving the rate.
    if (!rateAdaptationActive()) {
        const bool rate_locked = [] {
            const char* l = std::getenv("ULTRA_LOCK_RATE");
            return l != nullptr && std::atoi(l) != 0;
        }();
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s %s (q=%.2f)",
                      rate_locked ? "lock" : "off", codeRateToString(data_code_rate_), quality);
        last_adaptive_action_ = buf;
        return;
    }

    // ───────── QAM16 R2/3 cross-modulation climb (ULTRA_QAM16_CLIMB, default-ON) ─────────
    // Default-ON since 2026-07-02 (fade-riding ladder): above QPSK R3/4 the next throughput
    // step is the QAM16 R2/3 modulation hop, and riding the Good fade crests requires taking
    // it. "0" opts out.
    const bool qam16_climb_enabled = [] {
        const char* e = std::getenv("ULTRA_QAM16_CLIMB");
        if (e == nullptr || e[0] == '\0') return true;
        return std::atoi(e) != 0;
    }();
    if (data_modulation_ == Modulation::QAM16 ||
        data_modulation_ == Modulation::QAM8) {
        // While on a DENSE-constellation gear we do NOT walk the QPSK code-rate ladder
        // (the RateController is QPSK-blind — its R2/3 index is a QPSK rung). QAM16 is
        // fragile on frequency-selective fading (the decodability cliff: 55-70% loss /
        // link-death — fable_07); QAM8's 45° boundaries fail the same way, just later
        // (+3.6 dB margin). 8PSK revival probe (2026-07-05): letting the EMA walk QAM8
        // down its own rate ladder slid R2/3 -> R1/2 -> R1/4 (a strictly-dominated
        // rung: tight boundaries AND low rate) and finished at 990 vs the pinned-rate
        // 2040 — dense mods need the EXIT semantics, not the walk. So: HOLD or a
        // prompt asymmetric demote to the robust QPSK R3/4 home gear (QAM16's
        // re-climb cooldown machinery applies to QAM16 only; QAM8 has no mid-stream
        // climb-in yet — it is entry-selected — so no cooldown to meter).
        // ULTRA_QAM16_R34 crest semantics generalize: a bad group at the mod's R3/4
        // rung steps down ONE rung to its validated R2/3 and STAYS on the modulation;
        // a further bad group takes the QPSK exit.
        // RAW quality (not the EMA): a cliff demands a prompt reaction.
        const bool is_qam16 = data_modulation_ == Modulation::QAM16;
        const float drop_below = rate_controller_.config().drop_below;
        const bool nack = quality <= 0.0f;  // group fully lost — the cliff signature
        if (quality < drop_below) ++qam16_bad_streak_; else qam16_bad_streak_ = 0;
        char buf[96];
        if (nack || qam16_bad_streak_ >= kQam16DemoteBadStreak) {
            qam16_bad_streak_ = 0;
            qam16_r34_clean_streak_ = 0;  // a bad group also aborts any pending R3/4 walk
            // Crest-rung exit is one step at a time: R3/4 demotes to the validated R2/3, and
            // STAYS on the dense mod — noteQam16Demoted meters the QPSK->QAM16 re-entry
            // cooldown, which this is not. A further bad group at R2/3 exits to QPSK below.
            // (QAM8 R3/4 is a ladder rung when the psk8 ladder is on — same one-step-down.)
            const bool r34_step_down =
                (is_qam16 ? qam16R34Enabled() : true) &&
                data_code_rate_ == CodeRate::R3_4;
            const bool busy =
                file_transfer_.getState() == FileTransferState::SENDING &&
                (file_transfer_.hasPendingChunks() || arq_.getTxInFlightBytes() > 0);
            if (busy) {
                std::snprintf(buf, sizeof(buf),
                              "hold %s %s (demote->%s at clean boundary, q=%.2f)",
                              modulationToString(data_modulation_),
                              codeRateToString(data_code_rate_),
                              r34_step_down ? "R2/3 (same mod)" : "QPSK R3/4", quality);
            } else if (r34_step_down) {
                const Modulation step_mod = data_modulation_;  // stays on the dense mod
                const bool desc_committed = tryDescriptorModeSwitch(
                    step_mod, CodeRate::R2_3, wireSnrDb(),
                    v2::ModeChangeReason::CHANNEL_DEGRADED);
                if (!desc_committed) {
                    requestModeChange(step_mod, CodeRate::R2_3, wireSnrDb(),
                                      v2::ModeChangeReason::CHANNEL_DEGRADED);
                }
                std::snprintf(buf, sizeof(buf),
                              "%s R3/4 -> R2/3 demote via %s (q=%.2f)",
                              modulationToString(step_mod),
                              desc_committed ? "DESC-SWITCH" : "MODE_CHANGE", quality);
                LOG_MODEM(INFO, "Connection: adaptive %s", buf);
            } else {
                // Count the demote + arm the re-climb cooldown only when the MODE_CHANGE actually
                // fires (a busy-held decision re-asserts on the next bad group; counting the hold
                // would double-charge the backoff for one logical demote). QAM16 only: QAM8 has
                // no mid-stream climb-in, so there is no re-entry cooldown to meter.
                if (is_qam16) noteQam16Demoted(1);
                // Old-mode strings captured BEFORE the commit: a descriptor commit applies
                // the new mode immediately (data_code_rate_ mutates), unlike the pending
                // MODE_CHANGE path which holds it until the ACK.
                const char* old_mod_str = modulationToString(data_modulation_);
                const char* old_rate_str = codeRateToString(data_code_rate_);
                const bool desc_committed = tryDescriptorModeSwitch(
                    Modulation::QPSK, CodeRate::R3_4, wireSnrDb(),
                    v2::ModeChangeReason::CHANNEL_DEGRADED);
                if (!desc_committed) {
                    requestModeChange(Modulation::QPSK, CodeRate::R3_4, wireSnrDb(),
                                      v2::ModeChangeReason::CHANNEL_DEGRADED);
                }
                std::snprintf(buf, sizeof(buf),
                              "%s %s -> QPSK R3/4 demote via %s (q=%.2f)",
                              old_mod_str, old_rate_str,
                              desc_committed ? "DESC-SWITCH" : "MODE_CHANGE", quality);
                LOG_MODEM(INFO, "Connection: adaptive %s", buf);
            }
        } else if (is_qam16 && qam16R34Enabled() && data_code_rate_ == CodeRate::R2_3) {
            // Crest-rung walk: QAM16 R2/3 -> R3/4 after qam16ClimbStreak() CONSECUTIVE clean
            // groups (quality >= climb_above; a sub-threshold group resets the streak — same
            // gate as the QPSK->QAM16 hop). Fires only at a clean send boundary; when the
            // window is busy the streak is KEPT so the walk re-asserts on a later
            // clean-boundary ack, mirroring the modulation hop's deferred re-assert.
            const float climb_above = rate_controller_.config().climb_above;
            if (quality >= climb_above) ++qam16_r34_clean_streak_;
            else qam16_r34_clean_streak_ = 0;
            // FAST-CREST (2026-07-04, F9 finding, knob ULTRA_R34_FAST_CREST default-OFF):
            // the 2-group streak TRAILS the crest — F9's hop confirmed at fading 0.17
            // but fired at 0.51 (the window had closed; the excursion caught the tail,
            // 4/9 + 0/9, ~40 s lost). With descriptor commits + the ~4 s receiver-command
            // demote, a wrong hop is now cheap — so when the receiver's quality hint
            // SATURATES (>= 0.99 = the 3-bit quantizer's top bin: decode headroom far
            // beyond the R2/3 requirement), ONE such group arms the walk. Exit speed
            // funds entry speed.
            static const bool kFastCrest = [] {
                const char* e = std::getenv("ULTRA_R34_FAST_CREST");
                return !(e && e[0] == '0');  // DEFAULT-ON 2026-07-05
            }();
            const int walk_streak_needed =
                (kFastCrest && quality >= 0.99f) ? 1 : qam16ClimbStreak();
            // CALM-GATE (2026-07-04, F3-replication + crest A/B, knob
            // ULTRA_R34_CALM_FADING default-OFF byte-identical): the R3/4 walk fired
            // on `quality` alone — a BACKWARD-looking signal (the group that just
            // decoded had headroom). But R3/4 must survive the NEXT ~9 s, which is a
            // channel-COHERENCE property, not a decode-headroom one. In a chop
            // realization a clean group is routinely followed by a crater, so blind
            // probing pays 1 cratered group + a re-climb per probe (afternoon A/B:
            // crest median 1.25 vs crest-OFF 1.43/1.62 in the SAME chop; F3's 2.50
            // was a zero-crater 16QAM cruise that NEVER probed). Gate the walk on the
            // coherence-adjusted fading index: probe only in AWGN/low-Doppler calm —
            // exactly F3's cruise condition — otherwise stay at 16QAM R2/3 and cruise.
            // Set the knob to a threshold (e.g. 0.30) to enable; absent = legacy.
            static const float kR34CalmFading = [] {
                const char* e = std::getenv("ULTRA_R34_CALM_FADING");
                return (e && e[0]) ? std::strtof(e, nullptr) : 0.30f;  // DEFAULT 0.30 2026-07-05; <0 = off
            }();
            const bool calm_gate_ok =
                kR34CalmFading < 0.0f ||
                connection_policy::coherenceAdjustedFadingIndex(
                    fading_index_, coherence_score_, coherence_valid_) <= kR34CalmFading;
            if (calm_gate_ok && qam16_r34_clean_streak_ >= walk_streak_needed) {
                const bool busy =
                    file_transfer_.getState() == FileTransferState::SENDING &&
                    (file_transfer_.hasPendingChunks() || arq_.getTxInFlightBytes() > 0);
                if (busy) {
                    std::snprintf(buf, sizeof(buf),
                                  "hold QAM16 R2/3 for clean boundary (want QAM16 R3/4, q=%.2f)",
                                  quality);
                } else {
                    qam16_r34_clean_streak_ = 0;  // walk FIRED at a clean boundary -> reset
                    const bool desc_committed = tryDescriptorModeSwitch(
                        Modulation::QAM16, CodeRate::R3_4, wireSnrDb(),
                        v2::ModeChangeReason::CHANNEL_IMPROVED);
                    if (!desc_committed) {
                        requestModeChange(Modulation::QAM16, CodeRate::R3_4, wireSnrDb(),
                                          v2::ModeChangeReason::CHANNEL_IMPROVED);
                    }
                    std::snprintf(buf, sizeof(buf),
                                  "QAM16 R2/3 -> QAM16 R3/4 climb via %s (q=%.2f)",
                                  desc_committed ? "DESC-SWITCH" : "MODE_CHANGE", quality);
                    LOG_MODEM(INFO, "Connection: adaptive %s", buf);
                }
            } else {
                std::snprintf(buf, sizeof(buf), "hold QAM16 R2/3 (q=%.2f)", quality);
            }
        } else if (!is_qam16 && psk8LadderEnabled() &&
                   data_code_rate_ == CodeRate::R2_3) {
            // QAM8 R2/3 -> QAM16 R2/3 modulation crest-step (2026-07-05, F121 finding:
            // QAM8 was entry-only — no upward walk existed). Same gates as the QAM16
            // R3/4 crest walk above: qam16ClimbStreak() CONSECUTIVE clean groups
            // (>= climb_above; one sub-threshold group resets), fire only at a clean
            // send boundary, streak KEPT on a busy hold so the step re-asserts.
            // Reuses qam16_r34_clean_streak_ (identical lifecycle, mutually exclusive
            // with the QAM16 walk via the is_qam16 arm; rename tracked in the cleanup
            // register). +1 bit/symbol at -3.6 dB margin — strictly a crest move, the
            // streak is the calm gate.
            const float climb_above = rate_controller_.config().climb_above;
            if (quality >= climb_above) ++qam16_r34_clean_streak_;
            else qam16_r34_clean_streak_ = 0;
            if (qam16_r34_clean_streak_ >= qam16ClimbStreak() &&
                qam16_reclimb_cooldown_ == 0) {
                const bool busy =
                    file_transfer_.getState() == FileTransferState::SENDING &&
                    (file_transfer_.hasPendingChunks() || arq_.getTxInFlightBytes() > 0);
                if (busy) {
                    std::snprintf(buf, sizeof(buf),
                                  "hold QAM8 R2/3 for clean boundary (want QAM16 R2/3, q=%.2f)",
                                  quality);
                } else {
                    qam16_r34_clean_streak_ = 0;  // step FIRED at a clean boundary -> reset
                    const bool desc_committed = tryDescriptorModeSwitch(
                        Modulation::QAM16, CodeRate::R2_3, wireSnrDb(),
                        v2::ModeChangeReason::CHANNEL_IMPROVED);
                    if (!desc_committed) {
                        requestModeChange(Modulation::QAM16, CodeRate::R2_3, wireSnrDb(),
                                          v2::ModeChangeReason::CHANNEL_IMPROVED);
                    }
                    std::snprintf(buf, sizeof(buf),
                                  "QAM8 R2/3 -> QAM16 R2/3 climb via %s (q=%.2f)",
                                  desc_committed ? "DESC-SWITCH" : "MODE_CHANGE", quality);
                    LOG_MODEM(INFO, "Connection: adaptive %s", buf);
                }
            } else {
                std::snprintf(buf, sizeof(buf), "hold QAM8 R2/3 (q=%.2f)", quality);
            }
        } else {
            std::snprintf(buf, sizeof(buf), "hold %s %s (q=%.2f)",
                          modulationToString(data_modulation_),
                          codeRateToString(data_code_rate_), quality);
        }
        last_adaptive_action_ = buf;
        return;
    }

    const CodeRate prev = data_code_rate_;
    CodeRate next = rate_controller_.update(prev, quality);
    // 2026-05-28 experiment: ULTRA_MAX_OFDM_RATE caps climbs at a chosen rung.
    if (const char* env = std::getenv("ULTRA_MAX_OFDM_RATE")) {
        const std::string s(env);
        const CodeRate cap = (s == "R1_2" || s == "r1_2") ? CodeRate::R1_2
                           : (s == "R2_3" || s == "r2_3") ? CodeRate::R2_3
                           : (s == "R3_4" || s == "r3_4") ? CodeRate::R3_4
                           : CodeRate::AUTO;  // anything else = no cap (AUTO sentinel)
        if (cap != CodeRate::AUTO && next > cap) {
            next = cap;
        }
    }
    // Per-modulation validated-rate cap (2026-06-12, Phase 1). The RateController is
    // modulation-blind: it would climb the connect-time 16QAM R1/2 up into the measured
    // damage-bound 16QAM R2/3/R3/4 (Phase 0a) before the REACTIVE ssthresh can cap it —
    // taking a frame into a fade at the over-climbed rung (the seed-7 R3/4 FAIL mode).
    // Cap the CLIMB at the highest GUI-validated rate for the active modulation. Drops
    // (escape-drop / ssthresh) go DOWN and are unaffected. No-op for QPSK (cap = R3/4, ladder top).
    const CodeRate mod_cap = maxValidatedCoherentRate(data_modulation_);
    if (next > mod_cap) {
        next = mod_cap;
    }

    // QPSK R3/4 -> QAM16 R2/3 cross-modulation CLIMB. Above the QPSK top rung (R3/4) the next
    // throughput step is a MODULATION step, not a thinner code (R5/6 retired — a measured loser).
    // Climb ONLY after qam16ClimbStreak() CONSECUTIVE clean groups (quality >= climb_above) while
    // pinned at QPSK R3/4: a single sub-threshold group breaks the streak, so the streak doubles
    // as a low-variance Good-vs-Moderate gate (a Moderate channel's fades keep resetting it) — the
    // sender-side proxy for "confirmed-Good" that needs no wire change. The LTS coherence disc (the
    // sharper gate) lives on the RECEIVER, which is deaf to its own channel while sending, and its
    // HW threshold is not yet validated, so disc-gating is a deliberate later add. After a demote,
    // the re-climb COOLDOWN (noteQam16Demoted) must first be spent in CLEAN groups before the
    // streak may accrue again — the fade-riding replacement for the 06-17 sticky no-reclimb.
    Modulation target_mod = data_modulation_;
    CodeRate target_rate = next;
    bool qam16_hop = false;
    // LADDER-AWARE dense climb (2026-07-05, F121 finding): with the 8PSK ladder on,
    // the first dense rung above QPSK R3/4 is QAM8 R2/3 (constant-envelope, ~3.6 dB
    // more constellation margin than 16QAM). Without this the hop was hardcoded
    // QPSK->QAM16, making QAM8 ENTRY-ONLY: any exit mid-transfer stranded the run on
    // the QPSK<->QAM16 loop (F121 never revisited 8PSK after an 18 dB connect
    // snapshot). The streak / cooldown / calm-gate machinery below is unchanged —
    // only the hop TARGET generalizes. The QAM8 R2/3 -> QAM16 R2/3 upward step lives
    // in the dense-constellation branch above (this code is unreachable while on a
    // dense mod — that branch returns). (The qam16_* names now cover the generic
    // dense-climb machinery; rename tracked in the cleanup register.)
    const Modulation climb_to_mod =
        psk8LadderEnabled() ? Modulation::QAM8 : Modulation::QAM16;
    if (qam16_climb_enabled &&
        data_modulation_ == Modulation::QPSK && prev == CodeRate::R3_4 &&
        next == CodeRate::R3_4) {
        const float climb_above = rate_controller_.config().climb_above;
        if (quality >= climb_above) {
            if (qam16_reclimb_cooldown_ > 0) {
                --qam16_reclimb_cooldown_;  // a clean group spends cooldown; streak stays 0
                qam16_clean_streak_ = 0;
            } else {
                ++qam16_clean_streak_;
            }
        } else {
            qam16_clean_streak_ = 0;
        }
        // 16QAM CALM-GATE (2026-07-04, crater-frequency lever, knob
        // ULTRA_QAM16_CALM_FADING default-OFF byte-identical): the climb fires on
        // backward-looking `quality`; 16QAM R2/3 sits at ZERO fade-headroom, so in a
        // choppy realization it climbs into a trough and craters (afternoon 8/29 vs
        // F3's calm 1/18). Now that crater RECOVERY is fast (self-echo fix, ~3.5 s),
        // the residual tax is crater FREQUENCY. Gate the hop on the coherence-adjusted
        // fading index — climb to 16QAM only when the channel is calm (fade-riding:
        // 16QAM on crests), else hold the robust QPSK R3/4 and cruise. The streak is
        // preserved when the gate blocks (same deferral semantics as a busy window),
        // so the hop re-asserts the moment calm returns. Mirror of the R3/4 calm-gate.
        static const float kQam16CalmFading = [] {
            const char* e = std::getenv("ULTRA_QAM16_CALM_FADING");
            return (e && e[0]) ? std::strtof(e, nullptr) : -1.0f;  // <0 = gate off
        }();
        const bool qam16_calm_ok =
            kQam16CalmFading < 0.0f ||
            connection_policy::coherenceAdjustedFadingIndex(
                fading_index_, coherence_score_, coherence_valid_) <= kQam16CalmFading;
        if (qam16_clean_streak_ >= qam16ClimbStreak() && qam16_calm_ok) {
            target_mod = climb_to_mod;
            target_rate = CodeRate::R2_3;
            qam16_hop = true;
            // NOTE: do NOT reset the streak here. If the change is DEFERRED below (busy send
            // window — the common case mid-transfer), keeping the streak >= qam16ClimbStreak() lets
            // the hop RE-ASSERT on the next clean-boundary ack (mirroring how the QPSK rate change
            // re-asserts via rate_controller_.update). The streak resets only when the hop actually
            // FIRES, or when a sub-climb_above group breaks it (the channel degraded — abort).
        }
    } else {
        qam16_clean_streak_ = 0;  // not pinned at the QPSK top rung / disabled -> reset the streak
    }

    char buf[96];
    const bool changed = (target_mod != data_modulation_) || (target_rate != prev);
    if (changed) {
        // OPTION A (2026-06-10): a mid-FILE rate/mod change must land at a CLEAN send boundary —
        // one with no in-flight/pending file chunks. applyDataMode() re-encodes pending chunks
        // via requeuePendingChunks(), which REWINDS the file send cursor by the whole in-flight
        // window and re-sends already-delivered data. On a clean channel that's just wasted
        // airtime (survivable), but coincident with a fade the redundant re-sends starve the
        // real remaining chunks and the transfer strands (~75%, Good@20 seeds 2/99, 2026-06-09).
        // requestModeChange() holds the rate until BRAVO ACKs and runDeferredArqRefill() is gated
        // on mode_change_pending_, so NO new chunks submit between issue and apply — a change
        // ISSUED at a clean boundary is also APPLIED at one (the window stays drained until the
        // ACK -> requeuePendingChunks() is a no-op -> nothing re-sent). When the window is busy,
        // HOLD: applyAdaptiveRateFeedback runs on every group ack, and the EMA controller
        // re-asserts the decision on a later ack that lands at a clean boundary (a full-ack tick).
        // This also correctly THROTTLES rate churn during a fade (partial acks keep us busy).
        const bool file_send_window_busy =
            file_transfer_.getState() == FileTransferState::SENDING &&
            (file_transfer_.hasPendingChunks() || arq_.getTxInFlightBytes() > 0);
        if (file_send_window_busy) {
            std::snprintf(buf, sizeof(buf), "hold %s %s for clean boundary (want %s %s, q=%.2f)",
                          modulationToString(data_modulation_), codeRateToString(prev),
                          modulationToString(target_mod), codeRateToString(target_rate), quality);
        } else {
            // Route the adaptive rate/mod change through the SYNCHRONIZED MODE_CHANGE handshake:
            // requestModeChange() holds the local rate until BRAVO ACKs, so both stations switch
            // TOGETHER and re-anchor cleanly. The former unilateral `data_code_rate_ = next` flip
            // (in-band BURST_HEADER descriptor) desynced the pilot/carrier geometry between sender
            // and receiver — stale warm-sync -> |H| garbage -> 0/8 CWs forever -> churn to R1/4
            // (2026-06-09). The EMA RateController owns WHEN to change; the MODE_CHANGE owns HOW.
            const uint8_t reason =
                isFasterMode(target_mod, target_rate, data_modulation_, prev)
                    ? v2::ModeChangeReason::CHANNEL_IMPROVED
                    : v2::ModeChangeReason::CHANNEL_DEGRADED;
            // Old-mod string captured BEFORE the commit: a descriptor commit applies the
            // new mode immediately (data_modulation_ mutates), unlike the pending
            // MODE_CHANGE path which holds it until the ACK.
            const char* old_mod_str = modulationToString(data_modulation_);
            // DESC-SWITCH Phase 1 (knob-ON): skip the MODE_CHANGE round-trip — commit
            // locally and let the next burst's BURST_HEADER descriptor announce the move
            // (§5.1; the pilot/carrier-geometry desync arm of the 2026-06-09 failure is
            // closed by the mandatory full-anchor one-shot + RX warm-handoff demotion).
            // Falls back to the synchronized exchange when out of scope.
            const bool desc_committed =
                tryDescriptorModeSwitch(target_mod, target_rate, wireSnrDb(), reason);
            if (!desc_committed) {
                requestModeChange(target_mod, target_rate, wireSnrDb(), reason);
            }
            if (qam16_hop) qam16_clean_streak_ = 0;  // hop FIRED at a clean boundary -> reset
            std::snprintf(buf, sizeof(buf), "%s %s -> %s %s via %s (q=%.2f)",
                          old_mod_str, codeRateToString(prev),
                          modulationToString(target_mod), codeRateToString(target_rate),
                          desc_committed ? "DESC-SWITCH" : "MODE_CHANGE", quality);
            LOG_MODEM(INFO, "Connection: adaptive %s", buf);
        }
    } else {
        std::snprintf(buf, sizeof(buf), "hold %s (q=%.2f)", codeRateToString(prev), quality);
    }
    last_adaptive_action_ = buf;
}

void Connection::setRxLevelVerdict(int verdict, uint32_t seq) {
    if (seq == rx_level_verdict_seq_seen_) {
        return;  // stale re-feed — no fresh per-burst measurement since last time
    }
    rx_level_verdict_seq_seen_ = seq;
    using connection_policy::RxLevelVerdict;
    switch (static_cast<RxLevelVerdict>(verdict)) {
        case RxLevelVerdict::CLIPPED:
            rx_level_clipped_ = true;
            rx_level_low_streak_ = 0;
            break;
        case RxLevelVerdict::LOW:
            rx_level_clipped_ = false;
            ++rx_level_low_streak_;
            break;
        default:
            rx_level_clipped_ = false;
            rx_level_low_streak_ = 0;
            break;
    }
}

// Responder handshake confirmation (BUG-RESPONDER-HANDSHAKE-NEVER-CONFIRMS,
// 2026-07-04): the single site that flips a responder's handshake_confirmed_ and
// fires on_handshake_confirmed_() (→ the modem switches TX onto the negotiated OFDM
// data waveform instead of the handshake last-RX-waveform mirror). Historically it
// ran only in onFrameReceived — the CLASSIC frame path — because in the legacy
// control plane the initiator's first MODE_CHANGE frame always arrived there. The
// descriptor-committed control plane (Phase 1/2) eliminated classic frames BY
// DESIGN, so a burst-only session never confirmed: the responder's modem sat in
// handshake TX-routing all session and its rare classic control TXs (frame NACKs)
// went out as 3.1 s MC-DPSK DBPSK full-preamble frames (last_rx_waveform_ = the
// CONNECT-phase MC-DPSK) — the "MC-DPSK at the end of the run" the operator saw.
// A DELIVERED BURST GROUP is equally hard evidence the initiator heard our
// CONNECT_ACK (it only sends data after it), so the group path confirms too.
void Connection::maybeConfirmResponderHandshake(const char* evidence) {
    if (state_ != ConnectionState::CONNECTED || is_initiator_ || handshake_confirmed_) {
        return;
    }
    LOG_MODEM(INFO, "Connection: Handshake confirmed (%s)", evidence);
    handshake_confirmed_ = true;
    responder_handshake_wait_ms_ = 0;
    if (on_handshake_confirmed_) {
        on_handshake_confirmed_();
    }
    // Initial data mode is already carried in CONNECT_ACK.
}

void Connection::onBurstGroupReceived(uint16_t group_seq, const std::vector<Bytes>& frames,
                                      bool all_ok, float quality, uint16_t frame_mask,
                                      bool interleaved, uint8_t group_size) {
    if (!use_burst_transport_) {
        return;
    }
    // Descriptor-era handshake evidence (see maybeConfirmResponderHandshake): a
    // burst-only session must confirm here — classic frames may never arrive.
    maybeConfirmResponderHandshake("first delivered burst group from initiator");

    // TRANSPORT MERGE (increment 1): one seq space END-TO-END. The sender formed these
    // frames through arq_ (unified TX), so feed each decoded REAL frame back through the
    // ARQ window (processArqFrame) instead of the burst-group delivery + group_seq ack.
    // arq_ dedups/reorders by the frames' own seqs, delivers in order, and emits the
    // tone-burst ack itself — which the sender's arq_ consumes (seqs match), driving
    // selective repeat over the unified space. Skips the burst controller entirely.
    if (kUnifiedSeqEnabled()) {
        (void)all_ok; (void)frame_mask; (void)interleaved; (void)group_seq;
        // §14.43 closed-loop rate feedback + RECEIVER GUI "Adapt:" bar. THIS callback is where the
        // graded per-group decode headroom [0,1] is MEASURED (streaming_burst_interleave:
        // 1 - worst_CW_LDPC_iters/80). Record it so (a) BRAVO's Adapt bar shows it — the sender's
        // bar is fed via applyAdaptiveRateFeedback(), which the RECEIVER never runs — and (b) the
        // next tone-burst ack carries it back to ALPHA in rate_hint (the loop the unification cut).
        if (quality >= 0.0f) {
            last_group_quality_ = quality;
        }
        // RX-RATE-CMD Phase 2 (ULTRA_RX_RATE_CMD): refresh the receiver's standing rung
        // command BEFORE the ARQ emits this group's tone-burst ACK (endGroupReceiveAndAck
        // below) so the verdict rides THIS ACK — the crater is visible to the sender one
        // whole escape-detection cycle (~2×RTO) earlier than its own zero-progress
        // evidence. Hard no-op while the knob is OFF.
        if (rx_rate_cmd_enabled_) {
            updateRxRateCommandFromGroup(all_ok, frame_mask);
        }
        // RX-AUTHORITY (ULTRA_RX_RATE_AUTHORITY): compute the receiver's ABSOLUTE
        // rung verdict for this group BEFORE its ACK emits — the command rides THIS
        // ACK. Hard no-op while the knob is OFF.
        if (rxRateAuthorityEnabled()) {
            updateRxAuthorityCommand(all_ok, quality);
        }
        // ALC RUNAWAY GUARD (2026-07-04, F18): a fully-failed group invalidates any
        // accumulated LOW-level streak — those readings were fade-trough artifacts,
        // not drive-starvation evidence (see the SACK-emit advisory gate).
        if (!all_ok && frame_mask == 0) {
            rx_level_low_streak_ = 0;
        }
        // BURST-AWARE ACK: this callback IS the group boundary the operator pointed to —
        // "whatever ALPHA sends as a burst, BRAVO must ack, it knows the group ended."
        // Bracket the group's frames so arq_ suppresses its per-frame ack heuristic
        // (which can't ack a sub-window burst) and emits EXACTLY ONE tone-burst ack for
        // the whole burst at the end — cumulative base + hole bitmap, every burst.
        // Live "incoming burst" status for the GUI — the flashing partial-group
        // indicator (group #, X decoded / Y group size from frame_mask). Set this BEFORE
        // processing the frames: if this group completes the file, the file-received
        // callback (setFileReceivedCallback -> burst_activity_ = {}) fires DURING
        // processArqFrame and must WIN. Previously this block ran after the loop and
        // re-activated the indicator post-completion, so it kept flashing "received
        // group X/Y" after the "File Received" toast (looked like an extra/late group).
        {
            unsigned decoded = 0;
            for (uint16_t m = frame_mask; m; m &= (m - 1)) ++decoded;      // popcount = X
            // Y = the REAL group size from the descriptor (modem passes burst_group_size).
            // The old bit-length-of-frame_mask trick UNDERCOUNTS when the group's TRAILING
            // frame(s) fail (a failed frame is a 0 bit, so it's invisible to bit-length) —
            // e.g. a 3-frame group with seq 2 faded showed "2/2" instead of "2/3". Fall
            // back to that heuristic only if group_size wasn't supplied (group_size==0).
            unsigned group_bits = 0;
            for (uint16_t m = frame_mask; m; m >>= 1) ++group_bits;        // bit-length
            unsigned Y = group_size > 0
                             ? group_size
                             : (group_bits > decoded ? group_bits : decoded);
            if (Y < decoded) Y = decoded;
            burst_activity_.active = true;
            burst_activity_.group_seq = group_seq;
            burst_activity_.frames_decoded = static_cast<uint8_t>(decoded);
            burst_activity_.frames_in_group = static_cast<uint8_t>(Y);
            ++burst_activity_.groups_seen;
        }
        arq_.beginGroupReceive();
        for (const auto& frame : frames) {
            auto hdr = v2::parseHeader(frame);
            if (hdr.valid && !v2::isAddressedToCallsign(hdr, local_call_)) {
                continue;  // burst pad — addressed to the pad callsign
            }
            processArqFrame(frame);  // a file-completing frame clears burst_activity_ (wins)
        }
        arq_.endGroupReceiveAndAck();
        // WAITING-REBASE voice (BUG-UNANCHORED-SILENCE-ESCAPE, design §5.3, gated on
        // ULTRA_RX_RATE_CMD): checked AFTER the frames processed — if THIS group carried
        // the era base, the interregnum just ended and no voice is needed. While it
        // holds, the ARQ suppressed the group ack above (total ack silence by design),
        // so this is the receiver's ONLY utterance: a tone burst with rung_cmd=3 whose
        // mask/type the sender never parses as an ack (consumed whole in
        // onToneBurstAck). It tells the sender "alive + forward link works + resend
        // the era base" — the anti-manufactured-collapse signal, on the 4-FSK plane
        // that out-survives every OFDM waveform in the trough.
        if (rx_rate_cmd_enabled_ && arq_.rxWaitingRebase() &&
            kInteractiveToneAckEnabled() && on_transmit_tone_burst_ack_) {
            ultra::waveform::tone_burst_ack::ToneBurstAckPayload voice;
            voice.group_seq = static_cast<uint8_t>(group_seq & 0x3F);
            voice.frame_mask = 0;  // meaningless — consumed whole, never as a SACK
            voice.type = ultra::waveform::tone_burst_ack::AckType::Nack;
            voice.rung_cmd = ultra::waveform::tone_burst_ack::kRungCmdReserved;  // 3
            LOG_MODEM(WARN,
                      "Connection: WAITING-REBASE — voicing unanchored state to the "
                      "sender (group_seq=%u)",
                      group_seq);
            on_transmit_tone_burst_ack_(voice);
        }
        return;
    }
}

void Connection::sendNextFragment() {
    bool is_ofdm = isOFDMMode(negotiated_mode_);
    const bool pipeline_fragments = is_ofdm;

    // Enable burst buffering for OFDM mode
    if (is_ofdm && on_transmit_burst_) {
        burst_mode_active_ = true;
        burst_tx_buffer_.clear();
    }

    // UNIFIED PATH: bound a message burst to the SAME airtime budget as a file burst
    // (one budget-sized group per key-down, ack timeout sized to it) — so a large
    // message and a file transfer key down identically. SIZE_MAX (no cap) off the
    // unified OFDM path, preserving legacy fill-the-window message behavior.
    const size_t burst_frame_cap = prepareUnifiedBurstWindow();
    size_t submitted_this_call = 0;
    while (arq_.isReadyToSend() && next_fragment_idx_ < pending_tx_fragments_.size()) {
        if (submitted_this_call >= burst_frame_cap) {
            break;  // one budget-sized group per burst (unified path)
        }
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
        v2::FrameType frame_type = v2::FrameType::DATA;
        if (next_fragment_idx_ < pending_tx_fragment_types_.size()) {
            frame_type = pending_tx_fragment_types_[next_fragment_idx_];
        }
        uint64_t message_token = 0;
        if (next_fragment_idx_ < pending_tx_fragment_message_tokens_.size()) {
            message_token = pending_tx_fragment_message_tokens_[next_fragment_idx_];
        }

        LOG_MODEM(DEBUG, "Connection: Sending fragment %zu/%zu (%zu bytes, type=%s, flags=0x%02X)",
                  next_fragment_idx_ + 1, pending_tx_fragments_.size(), chunk.size(),
                  v2::frameTypeToString(frame_type), flags);

        bool sent = false;
        sent = sendArqPayloadFrame(chunk, frame_type, flags, is_ofdm, message_token);
        if (sent) {
            noteDataTurnPayloadStarted(chunk.size());
        }
        next_fragment_idx_++;
        submitted_this_call++;

        // MC-DPSK bulk-file transfer uses sendNextFileChunk() and is safe to
        // pipeline. The message-fragment path still shares OFDM-oriented
        // MORE_FRAG/SACK semantics, so keep it stop-and-wait until it has its
        // own hardware-validated burst pacing.
        if (!pipeline_fragments && submitted_this_call >= 1) {
            break;
        }
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

    const bool duplicate_connect_retry = header.type == v2::FrameType::CONNECT;

    // Any post-CONNECT frame from the initiator means our CONNECT_ACK got
    // through — stop proactive ACK retx regardless of whether the formal
    // handshake-confirmed bit has flipped. A duplicate CONNECT means the
    // opposite: the initiator is still CONNECTING because CONNECT_ACK was lost,
    // so keep the cached ACK for handleConnect() to re-send.
    if (state_ == ConnectionState::CONNECTED && !is_initiator_ &&
        !duplicate_connect_retry && !connect_ack_frame_.empty()) {
        connect_ack_frame_.clear();
        connect_ack_retx_remaining_ = 0;
    }

    // Responder handshake confirmation: first valid protocol frame after CONNECT_ACK
    // means the initiator received our ACK and switched to data/control exchange.
    if (!duplicate_connect_retry) {
        maybeConfirmResponderHandshake("first valid classic frame from initiator");
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

    // DISCONNECT now uses control-frame encoding (20 bytes) for hardened
    // 1-CW handling. Keep ConnectFrame fallback for legacy peers/log replay.
    if (header.type == v2::FrameType::DISCONNECT) {
        if (auto ctrl = v2::ControlFrame::deserialize(frame_data)) {
            handleDisconnect(*ctrl, src_call);
            return;
        }

        if (auto conn = v2::ConnectFrame::deserialize(frame_data)) {
            std::string frame_src_call = conn->getSrcCallsign();
            if (!frame_src_call.empty()) {
                src_call = frame_src_call;
            }
            handleDisconnectFrame(*conn, src_call);
            return;
        }

        LOG_MODEM(WARN, "Connection: Failed to parse DISCONNECT frame");
        return;
    }

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
                default:
                    break;
            }
        }
    } else if (v2::isControlFrame(header.type)) {
        // Control frames (DISCONNECT, ACK, NACK, etc) - parse as ControlFrame
        auto ctrl = v2::ControlFrame::deserialize(frame_data);
        if (ctrl) {
            switch (ctrl->type) {
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
                            commitPendingModeChange("ACKed");
                        } else {
                            // Regular data ACK
                            if (local_data_turn_) {
                                armDataTurnTxGuard(dataTurnAckDiversityGuardMs(*ctrl));
                                if ((ctrl->flags & v2::Flags::TURN_REQUEST) != 0) {
                                    peer_data_turn_requested_ = true;
                                    LOG_MODEM(INFO,
                                              "Connection: Peer requested DATA turn on ACK seq=%u",
                                              ctrl->seq);
                                }
                            }
                            processArqFrame(frame_data);
                            maybeYieldDataTurn();
                        }
                    }
                    break;
                case v2::FrameType::NACK:
                    if (state_ == ConnectionState::CONNECTED) {
                        processArqFrame(frame_data);
                    }
                    break;
                case v2::FrameType::MODE_CHANGE:
                    handleModeChange(*ctrl, src_call);
                    break;
                case v2::FrameType::TURNOVER:
                    handleTurnover(*ctrl, src_call);
                    break;
                case v2::FrameType::TURN_REQUEST:
                    handleTurnRequest(*ctrl, src_call);
                    break;
                case v2::FrameType::FILE_CANCEL:
                    handleFileCancel(*ctrl, src_call);
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
            if (file_cancel_rx_drain_ms_ > 0 || file_cancel_reassert_ms_ > 0) {
                LOG_MODEM(INFO,
                          "Connection: Dropping stale DATA seq=%u during FILE_CANCEL drain/reassert (drain=%ums reassert=%ums)",
                          header.seq, file_cancel_rx_drain_ms_, file_cancel_reassert_ms_);
                if (file_cancel_rx_drain_ms_ == 0) {
                    file_cancel_rx_drain_ms_ = FILE_CANCEL_RX_DRAIN_MS;
                }
                maybeReassertFileCancelForStaleData();
                return;
            }
            processArqFrame(frame_data);
        }
    }
}

void Connection::processArqFrame(const Bytes& frame_data) {
    // TEST HOOK (env ULTRA_DROP_RX_SEQ=N): drop the FIRST receipt of the DATA frame with
    // seq=N to prove SELECTIVE repeat — the ack bitmap must then show only that hole and
    // the sender must resend ONLY that frame. One-shot: the resend is accepted.
    static const long kDropRxSeq = [] {
        const char* e = std::getenv("ULTRA_DROP_RX_SEQ");
        return e ? std::atol(e) : -1L;
    }();
    if (kDropRxSeq >= 0) {
        static bool dropped_once = false;
        auto hdr = v2::parseHeader(frame_data);
        if (!dropped_once && hdr.valid && !hdr.is_control &&
            hdr.seq == static_cast<uint16_t>(kDropRxSeq)) {
            dropped_once = true;
            LOG_MODEM(WARN, "Connection: TEST-DROP RX DATA seq=%u (one-shot, proving selective repeat)",
                      hdr.seq);
            return;
        }
    }

    const bool outermost = !arq_callback_defer_refill_;
    if (outermost) {
        arq_callback_defer_refill_ = true;
    }

    arq_.onFrameReceived(frame_data);

    if (outermost) {
        arq_callback_defer_refill_ = false;
        runDeferredArqRefill();
    }
}

void Connection::onMCDPSKPartialFrame(const v2::PartialFrameCodewords& partial) {
    if (state_ != ConnectionState::CONNECTED || negotiated_mode_ != WaveformMode::MC_DPSK) {
        return;
    }
    if (!partial.valid()) {
        return;
    }
    const uint32_t our_hash = v2::hashCallsign(local_call_);
    if (partial.dst_hash != our_hash && partial.dst_hash != 0xFFFFFF) {
        return;
    }

    const bool outermost = !arq_callback_defer_refill_;
    if (outermost) {
        arq_callback_defer_refill_ = true;
    }

    arq_.onPartialFrame(partial);

    if (outermost) {
        arq_callback_defer_refill_ = false;
        runDeferredArqRefill();
    }
}

void Connection::onAcceptedOFDMDataSync(float sync_correlation) {
    if (state_ != ConnectionState::CONNECTED || is_initiator_ ||
        !isOFDMMode(negotiated_mode_)) {
        return;
    }
    if (connect_ack_frame_.empty() && connect_ack_retx_remaining_ <= 0) {
        return;
    }

    // §14.27: in the one-way burst transport, an ACCEPTED OFDM data sync from the
    // initiator proves it received our CONNECT_ACK — it only transmits OFDM data
    // after the handshake completes. Disarm the CONNECT_ACK rescue NOW (before the
    // first full group decodes ~5 s into the burst), so it cannot fire an 8.3 s
    // MC-DPSK / OFDM CONNECT_ACK blast INTO the initiator's in-flight group burst.
    // That collision (both stations keyed at once) summed on the medium and
    // corrupted/serialized-behind the GROUP_ACK round trip — the root cause of the
    // group-0 ACK latency. Gated on use_burst_transport_ so the normal OFDM
    // handshake (which keeps the rescue armed until a decoded frame) is unchanged.
    if (use_burst_transport_) {
        LOG_MODEM(INFO,
                  "Connection: Accepted OFDM DATA sync (corr=%.2f); disarming CONNECT_ACK rescue (burst transport)",
                  sync_correlation);
        connect_ack_frame_.clear();
        connect_ack_retx_remaining_ = 0;
        return;
    }

    LOG_MODEM(INFO,
              "Connection: Accepted OFDM DATA sync (corr=%.2f); keeping CONNECT_ACK rescue armed until decoded initiator frame",
              sync_correlation);
}

void Connection::runDeferredArqRefill() {
    if (arq_callback_defer_refill_) {
        return;
    }

    if (mode_change_pending_) {
        return;
    }

    const bool refill_file = deferred_file_refill_;
    const bool refill_fragments = deferred_fragment_refill_;
    deferred_file_refill_ = false;
    deferred_fragment_refill_ = false;

    if (state_ != ConnectionState::CONNECTED) {
        return;
    }
    // §RETX-PACING §1.3 trigger #1: retx_pace_hold_ms_ > 0 blocks the turn refill exactly
    // like the existing guards — the deferred-refill flags RE-LATCH below, so the refill
    // fires automatically (same [holes]+[new] coalescing, untouched) when the hold expires
    // in the CONNECTED tick. Default-off knob ⇒ the hold is never armed ⇒ byte-identical.
    if (!local_data_turn_ || file_cancel_confirm_pending_ || data_turn_tx_guard_ms_ > 0 ||
        retx_pace_hold_ms_ > 0) {
        deferred_file_refill_ = refill_file || deferred_file_refill_;
        deferred_fragment_refill_ = refill_fragments || deferred_fragment_refill_;
        return;
    }
    if (shouldPauseLocalDataForPeerRequest()) {
        deferred_file_refill_ = refill_file || deferred_file_refill_;
        deferred_fragment_refill_ = refill_fragments || deferred_fragment_refill_;
        maybeYieldDataTurn();
        return;
    }

    if (refill_file && file_transfer_.getState() == FileTransferState::SENDING) {
        sendNextFileChunk();
    }

    if (refill_fragments &&
        !pending_tx_fragments_.empty() &&
        next_fragment_idx_ < pending_tx_fragments_.size()) {
        sendNextFragment();
    }

    sendNextQueuedPayloadIfReady();
}

// =============================================================================
// TIMER / TICK
// =============================================================================

void Connection::tick(uint32_t elapsed_ms) {
    soft_combine_harq_.tick(elapsed_ms);
    // #58 increment 3: age the connect-SNR pool on the modem-time tick (elapsed_ms is
    // the same clock every timer here runs on — never Date/wall-clock). Ticks in ALL
    // states: handshake readings accumulate age while DISCONNECTED/CONNECTING too.
    connect_snr_pool_.tick(elapsed_ms);
    // The fading-freshness clock ages on the same tick (saturating).
    ms_since_fading_update_ =
        (ms_since_fading_update_ > 0x7FFFFFFF - elapsed_ms)
            ? 0x7FFFFFFF
            : ms_since_fading_update_ + elapsed_ms;

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
                    timeout_remaining_ms_ = pingTimeoutMsForCurrentProfile();
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
                    LOG_MODEM(WARN, "Connection: Connect timeout, retrying via %s (%d/%d)",
                              waveformModeToString(connect_waveform_),
                              connect_retry_count_ + 1, config_.connect_retries);
                    auto connect_frame = v2::ConnectFrame::makeConnect(local_call_, remote_call_,
                                                                        config_.mode_capabilities,
                                                                        static_cast<uint8_t>(config_.preferred_mode),
                                                                        static_cast<uint8_t>(config_.forced_modulation),
                                                                        static_cast<uint8_t>(config_.forced_code_rate),
                                                                        config_.forced_cw_count);
                    transmitFrame(connect_frame.serialize());
                    timeout_remaining_ms_ = connectRetryIntervalMs();
                }
            } else {
                timeout_remaining_ms_ -= elapsed_ms;
            }
            break;

        case ConnectionState::CONNECTED:
            connected_time_ms_ += elapsed_ms;
            stats_.connected_time_ms = connected_time_ms_;
            if (data_turn_tx_guard_ms_ > 0) {
                data_turn_tx_guard_ms_ =
                    elapsed_ms >= data_turn_tx_guard_ms_ ? 0 : data_turn_tx_guard_ms_ - elapsed_ms;
            }
            // §RETX-PACING §1.3: tick the trough-pacing hold down BEFORE the
            // runDeferredArqRefill() below, so an expiring hold releases the latched
            // deferred refill in the SAME tick (the deferred-refill flags stay latched
            // while the hold runs — no resubmission logic changes).
            if (retx_pace_hold_ms_ > 0) {
                retx_pace_hold_ms_ =
                    elapsed_ms >= retx_pace_hold_ms_ ? 0 : retx_pace_hold_ms_ - elapsed_ms;
            }
            if (turn_request_holdoff_ms_ > 0) {
                turn_request_holdoff_ms_ =
                    elapsed_ms >= turn_request_holdoff_ms_ ? 0 : turn_request_holdoff_ms_ - elapsed_ms;
            }
            if (file_cancel_rx_drain_ms_ > 0) {
                file_cancel_rx_drain_ms_ =
                    elapsed_ms >= file_cancel_rx_drain_ms_ ? 0 : file_cancel_rx_drain_ms_ - elapsed_ms;
            }
            if (file_cancel_reassert_ms_ > 0) {
                file_cancel_reassert_ms_ =
                    elapsed_ms >= file_cancel_reassert_ms_ ? 0 : file_cancel_reassert_ms_ - elapsed_ms;
            }
            if (file_cancel_reassert_cooldown_ms_ > 0) {
                file_cancel_reassert_cooldown_ms_ =
                    elapsed_ms >= file_cancel_reassert_cooldown_ms_
                        ? 0
                        : file_cancel_reassert_cooldown_ms_ - elapsed_ms;
            }
            if (local_data_turn_ && peer_data_turn_requested_) {
                data_turn_contended_ms_ += elapsed_ms;
            }
            // Half-duplex INTERACTIVE: while we've yielded and are waiting for the peer's
            // first burst, keep the decoder armed to COLD-acquire its full chirp+LTS anchor.
            // The one-shot armed on the yield-TURNOVER gets consumed in the gap (stray
            // detect / warm-sync DEGRADED), so re-arm periodically until the peer's data
            // actually arrives (yielded_data_turn_waiting_for_peer_data_ clears). The peer
            // force-fulls its first frame on turn-acquire, so this meets it (BUG-TNC-B2F-001).
            if (half_duplex_interactive_ && yielded_data_turn_waiting_for_peer_data_ &&
                !local_data_turn_ && on_full_ofdm_anchor_expected_) {
                if (elapsed_ms >= interactive_anchor_rearm_ms_) {
                    interactive_anchor_rearm_ms_ = 400;
                    on_full_ofdm_anchor_expected_();
                } else {
                    interactive_anchor_rearm_ms_ -= elapsed_ms;
                }
            }
            // VARA-HF turnaround: the interactive ISS with an empty TX buffer yields to the
            // IRS so the B2F responder can send its SID. Fire once, ~1.5 s after connect.
            if (half_duplex_interactive_ && is_initiator_ && !interactive_initiator_yield_done_) {
                const bool ready = local_data_turn_ && connected_time_ms_ >= 1500 &&
                                   data_turn_tx_guard_ms_ == 0 &&
                                   !hasLocalDataWaitingForTurn() && !file_transfer_.isBusy() &&
                                   arq_.isReadyToSend();
                if (ready) {
                    // Force a full chirp+LTS anchor on this TURNOVER — it is our first OFDM
                    // frame after the MC-DPSK handshake, so the peer hasn't tracked our OFDM
                    // timing and a light preamble won't decode (BUG-TNC-B2F-001).
                    if (on_data_turn_acquired_) {
                        on_data_turn_acquired_();
                    }
                    auto turnover = v2::ControlFrame::makeTurnover(local_call_, remote_call_);
                    transmitFrame(turnover.serialize());
                    local_data_turn_ = false;
                    yielded_data_turn_waiting_for_peer_data_ = true;
                    received_peer_data_since_connect_ = false;
                    resetDataTurnFairness();
                    armDataTurnTxGuard(dataTurnControlGuardMs());
                    interactive_initiator_yield_done_ = true;
                    LOG_MODEM(INFO, "Connection: interactive ISS yielded first DATA turn to %s (B2F responder speaks first)",
                              remote_call_.c_str());
                } else {
                    interactive_yield_log_throttle_ms_ += elapsed_ms;
                    if (interactive_yield_log_throttle_ms_ >= 2000) {
                        interactive_yield_log_throttle_ms_ = 0;
                        LOG_MODEM(DEBUG, "Connection: interactive yield WAIT: turn=%d conn_ms=%u guard=%u data_waiting=%d file_busy=%d arq_ready=%d",
                                  local_data_turn_ ? 1 : 0, connected_time_ms_, data_turn_tx_guard_ms_,
                                  hasLocalDataWaitingForTurn() ? 1 : 0, file_transfer_.isBusy() ? 1 : 0,
                                  arq_.isReadyToSend() ? 1 : 0);
                    }
                }
            }
            if (file_cancel_confirm_pending_ &&
                data_turn_tx_guard_ms_ == 0 &&
                arq_.isReadyToSend()) {
                transmitFileCancelControl(" (confirm)");
                file_cancel_confirm_pending_ = false;
                armDataTurnTxGuard(fileCancelConfirmDataGuardMs());
            }
            if (!local_data_turn_ && hasLocalDataWaitingForTurn() && !local_turn_request_pending_) {
                sendTurnRequestIfNeeded();
            }
            if (!local_data_turn_ && hasLocalDataWaitingForTurn() && local_turn_request_pending_) {
                if (elapsed_ms >= turn_request_retransmit_ms_) {
                    local_turn_request_pending_ = false;
                    sendTurnRequestIfNeeded();
                } else {
                    turn_request_retransmit_ms_ -= elapsed_ms;
                }
            }

            // Proactive CONNECT_ACK retransmission (responder side, BUG-CTRL-001).
            // ALPHA can miss MC-DPSK ACKs on faded seeds. The cadence is derived
            // from MC-DPSK control airtime and carrier-sense still gates the
            // actual TX edge, so it scales with slower robust profiles without
            // becoming an AWGN-only timeout tweak.
            if (!is_initiator_ &&
                negotiated_mode_ == WaveformMode::OFDM_CHIRP &&
                !connect_ack_frame_.empty() && connect_ack_retx_remaining_ > 0) {
                if (elapsed_ms >= connect_ack_retransmit_ms_) {
                    connect_ack_retransmit_ms_ = connectAckRetransmitMs();
                    connect_ack_retx_remaining_--;
                    LOG_MODEM(INFO, "Connection: Re-sending CONNECT_ACK (proactive, %d retx remaining, carrier-sense gated)",
                              connect_ack_retx_remaining_);
                    transmitFrame(connect_ack_frame_);
                } else {
                    connect_ack_retransmit_ms_ -= elapsed_ms;
                }
            }

            // Responder fail-safe: after the CONNECT_ACK rescue window, keep the
            // responder quiet until a real post-CONNECT frame arrives. A duplicate
            // CONNECT means the initiator is still in MC-DPSK setup, so do not mark
            // the protocol handshake confirmed on a timer.
            if (!is_initiator_ && !handshake_confirmed_ &&
                responder_handshake_wait_ms_ > 0) {
                if (elapsed_ms >= responder_handshake_wait_ms_) {
                    responder_handshake_wait_ms_ = 0;
                    LOG_MODEM(WARN,
                              "Connection: Responder handshake still unconfirmed after CONNECT_ACK rescue window; waiting for initiator frame");
                } else {
                    responder_handshake_wait_ms_ -= elapsed_ms;
                }
            }
            // HALF-OPEN TIMEOUT (2026-07-04, F29: a one-way CONNECT_ACK loss left the
            // responder "Connected" for 31 minutes while the initiator had cleanly
            // given up ~356 s in after its 10 connect retries). If the handshake NEVER
            // confirms — no classic frame, no burst group, nothing — the initiator is
            // provably gone once its whole retry ladder (~340 s) has elapsed: release
            // the session and return to listening. 240,000 ms > 6x the worst normal
            // confirm time observed (~38 s) and inside the initiator give-up bound.
            if (!is_initiator_ && !handshake_confirmed_) {
                responder_half_open_ms_ += elapsed_ms;
                if (responder_half_open_ms_ >= 240000) {
                    LOG_MODEM(WARN,
                              "Connection: handshake never confirmed %u ms after "
                              "CONNECT_ACK — initiator presumed gone (one-way ACK "
                              "loss); releasing the half-open session",
                              responder_half_open_ms_);
                    enterDisconnected("handshake never confirmed (half-open timeout)");
                    return;
                }
            } else {
                responder_half_open_ms_ = 0;
            }

            tickModeChangeAckRepeats(elapsed_ms);

            // Handle MODE_CHANGE timeout.
            // BUG-MC-RETRY-SPURIOUS (2026-07-04, E1 forensics): the deadline HOLDS
            // while our own TX is keyed — half-duplex means we cannot have decoded
            // the peer's MC-ACK during our key-down, so wall time spent transmitting
            // is not evidence of ACK loss. The MODE_CHANGE rides the TAIL of a
            // bundled data key-down (~10.6 s observed), which alone ate most of the
            // old request-anchored 18.2 s deadline; all three observed spurious-retry
            // cycles (21.1/21.3/30.4 s pipelines) become retry-free with the hold.
            // Unwired provider (tests/headless) => legacy behavior.
            if (mode_change_pending_ && tx_active_provider_ && tx_active_provider_()) {
                // keyed: hold the deadline (neither decrement nor fire)
            } else if (mode_change_pending_) {
                if (elapsed_ms >= mode_change_timeout_ms_) {
                    mode_change_retry_count_++;
                    if (mode_change_retry_count_ > MODE_CHANGE_MAX_RETRIES) {
                        LOG_MODEM(WARN,
                                  "Connection: MODE_CHANGE ACK unresolved after %d retries; keeping current %s %s because peer ACK was not proven",
                                  MODE_CHANGE_MAX_RETRIES,
                                  modulationToString(data_modulation_),
                                  codeRateToString(data_code_rate_));
                        mode_change_pending_ = false;
                        mode_change_timeout_ms_ = 0;
                        mode_change_retry_count_ = 0;
                        pending_ladder_rung_id_ = LadderRungId::UNKNOWN;
                        runDeferredArqRefill();
                    } else {
                        LOG_MODEM(WARN, "Connection: MODE_CHANGE timeout, retrying (%d/%d)",
                                  mode_change_retry_count_, MODE_CHANGE_MAX_RETRIES);
                        // Resend MODE_CHANGE with same parameters (incl. CW)
                        auto frame = v2::ControlFrame::makeModeChange(local_call_, remote_call_,
                                                                       mode_change_seq_, pending_modulation_,
                                                                       pending_code_rate_, pending_snr_db_,
                                                                       pending_fading_index_,
                                                                       pending_reason_,
                                                                       pending_cw_count_,
                                                                       pending_ladder_rung_id_);
                        transmitFrame(frame.serialize());
                        mode_change_timeout_ms_ = modeChangeRetryMs();
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
            // RX-AUTHORITY: age the verdict-SNR ring (stale readings must not
            // steer the rung after a quiet stretch).
            for (size_t i = 0; i < kRxAuthObsRing; ++i) {
                if (rx_auth_obs_age_ms_[i] <= kRxAuthObsMaxAgeMs) {
                    rx_auth_obs_age_ms_[i] += elapsed_ms;
                }
            }
            maybeEscapeStuckFrame();
            // §RETX-PACING §2: the collapse-conditioned escape is POLLED here (proven-safe
            // context, same as maybeEscapeStuckFrame) — rounds counted inside the ARQ
            // transmit callback (RTO batch) escape one tick later, never re-entrantly.
            maybeCollapseEscape();
            maybeYieldDataTurn();
            runDeferredArqRefill();
            sendNextQueuedPayloadIfReady();
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
    const bool expect_full_anchor_after_tx =
        negotiated_mode_ == WaveformMode::OFDM_CHIRP &&
        state_ == ConnectionState::CONNECTED &&
        expectsFullOFDMAnchorAfterTx(frame_data);

    // If burst mode is active, buffer instead of transmitting immediately
    if (burst_mode_active_ && on_transmit_burst_) {
        burst_tx_buffer_.push_back(frame_data);
        return;
    }

    if (on_transmit_info_) {
        on_transmit_info_(frame_data, expect_full_anchor_after_tx);
        return;
    }

    if (on_transmit_) {
        on_transmit_(frame_data);
    }

    if (expect_full_anchor_after_tx && on_full_ofdm_anchor_expected_) {
        on_full_ofdm_anchor_expected_();
    }
}

uint32_t Connection::currentDataFrameAirtimeMs() const {
    if (negotiated_mode_ == WaveformMode::OFDM_CHIRP) {
        return connection_policy::wideOFDMFrameTiming(
            data_modulation_, data_code_rate_, data_frame_cw_count_).data_ms;
    }
    if (negotiated_mode_ == WaveformMode::OFDM_NARROW) {
        return connection_policy::narrowOFDMFrameTiming(
            data_modulation_, data_frame_cw_count_).data_ms;
    }
    if (negotiated_mode_ == WaveformMode::MC_DPSK) {
        return connection_policy::mcDpskFrameTiming(
            data_modulation_,
            config_.mc_dpsk_num_carriers,
            config_.mc_dpsk_samples_per_symbol,
            data_frame_cw_count_).data_ms;
    }
    return 1000;
}

uint32_t Connection::currentControlFrameAirtimeMs() const {
    if (negotiated_mode_ == WaveformMode::OFDM_CHIRP) {
        uint32_t control_ms = connection_policy::wideOFDMFrameTiming(
            wideOFDMControlModulationForData(data_modulation_), CodeRate::R1_4).ack_ms;
        if (connection_policy::shouldUseWideOFDMShortReanchor(
                negotiated_mode_, data_modulation_, fading_index_)) {
            control_ms += connection_policy::wideOFDMShortReanchorChirpDurationMs();
        }
        return control_ms;
    }
    if (negotiated_mode_ == WaveformMode::OFDM_NARROW) {
        return connection_policy::narrowOFDMFrameTiming(
            data_modulation_, data_frame_cw_count_).ack_ms;
    }
    if (negotiated_mode_ == WaveformMode::MC_DPSK) {
        return connection_policy::mcDpskFrameTiming(
            data_modulation_,
            config_.mc_dpsk_num_carriers,
            config_.mc_dpsk_samples_per_symbol,
            data_frame_cw_count_).ack_ms;
    }
    return 500;
}

uint32_t Connection::currentBurstAnchorAirtimeMs() const {
    if (negotiated_mode_ == WaveformMode::OFDM_CHIRP) {
        return connection_policy::kWideOFDMFullAnchorExtraMs;
    }
    if (negotiated_mode_ == WaveformMode::MC_DPSK) {
        return connection_policy::kMCDPSKDualChirpPreambleMs;
    }
    return 0;
}

uint32_t Connection::connectControlFrameAirtimeMs() const {
    // The GUI/OTASim cold-call MC-DPSK profile is Robust-Mid DBPSK. Forced
    // MC-DPSK profiles can override this, but AUTO should match the waveform
    // engine's real handshake modulation rather than the connected data default.
    Modulation control_mod = Modulation::DBPSK;
    if (config_.forced_modulation == Modulation::DBPSK ||
        config_.forced_modulation == Modulation::DQPSK ||
        config_.forced_modulation == Modulation::D8PSK) {
        control_mod = config_.forced_modulation;
    }

    const int connect_cw_count = v2::kDefaultFixedFrameCodewords;
    const auto timing = connection_policy::mcDpskFrameTiming(
        control_mod,
        config_.mc_dpsk_num_carriers,
        config_.mc_dpsk_samples_per_symbol,
        connect_cw_count);

    const uint64_t airtime_ms =
        static_cast<uint64_t>(connection_policy::mcDpskBurstAirtimeMs(timing, 1)) +
        2ULL * static_cast<uint64_t>(connection_policy::kMCDPSKInterFrameGuardMs);
    return static_cast<uint32_t>(std::min<uint64_t>(airtime_ms, 0xFFFFFFFFull));
}

uint32_t Connection::connectRetryIntervalMs() const {
    const uint64_t control_ms = std::max<uint32_t>(1, connectControlFrameAirtimeMs());
    const uint64_t interval_ms =
        4ULL * control_ms +
        static_cast<uint64_t>(connection_policy::kCarrierSenseSackCoalesceMs);
    return static_cast<uint32_t>(std::min<uint64_t>(interval_ms, 0xFFFFFFFFull));
}

uint32_t Connection::connectAckRetransmitMs() const {
    const uint64_t control_ms = std::max<uint32_t>(1, connectControlFrameAirtimeMs());
    const uint64_t interval_ms =
        2ULL * control_ms +
        static_cast<uint64_t>(connection_policy::kCarrierSenseSackCoalesceMs);
    return static_cast<uint32_t>(std::min<uint64_t>(interval_ms, 0xFFFFFFFFull));
}

int Connection::connectAckRetxBudget() const {
    return std::max(0, config_.connect_retries - 1);
}

uint32_t Connection::responderHandshakeFailSafeMs() const {
    const uint64_t attempts = static_cast<uint64_t>(connectAckRetxBudget() + 1);
    const uint64_t rescue_window_ms =
        attempts * static_cast<uint64_t>(connectAckRetransmitMs()) +
        static_cast<uint64_t>(connectControlFrameAirtimeMs());
    const uint64_t failsafe_ms = std::max<uint64_t>(
        RESPONDER_HANDSHAKE_FAILSAFE_MS,
        rescue_window_ms);
    return static_cast<uint32_t>(std::min<uint64_t>(failsafe_ms, 0xFFFFFFFFull));
}

uint32_t Connection::dataTurnAckDiversityGuardMs(const v2::ControlFrame& ack) const {
    const uint32_t control_airtime_ms = currentControlFrameAirtimeMs();
    uint64_t guard_ms = static_cast<uint64_t>(control_airtime_ms / 2) +
                        connection_policy::kCarrierSenseSackCoalesceMs;

    const auto ack_payload = v2::NackPayload::decode(ack.payload);
    const bool ack_has_final = (ack.flags & v2::Flags::FINAL) != 0;
    const bool guard_half_duplex_repeat =
        ack_payload.cw_bitmap == 0 && !ack_has_final;
    const uint32_t repeat_tail_ms =
        selective_repeat_arq_policy::ackRepeatTailGuardMs(
            control_airtime_ms,
            arq_.getAckRepeatPeerBurstGuardMs(),
            arq_.getAckRepeatDelay(),
            arq_.getAckRepeatCount(),
            guard_half_duplex_repeat);
    if (repeat_tail_ms > 0) {
        guard_ms = std::max<uint64_t>(
            guard_ms,
            static_cast<uint64_t>(repeat_tail_ms) +
                connection_policy::kCarrierSenseSackCoalesceMs);
    }

    return static_cast<uint32_t>(std::min<uint64_t>(
        std::max<uint64_t>(DATA_TURN_ACK_DIVERSITY_GUARD_FLOOR_MS, guard_ms),
        0xFFFFFFFFull));
}

uint32_t Connection::dataTurnConnectGuardMs() const {
    const uint64_t guard_ms =
        static_cast<uint64_t>(currentBurstAnchorAirtimeMs()) +
        static_cast<uint64_t>(currentControlFrameAirtimeMs()) +
        static_cast<uint64_t>(currentDataFrameAirtimeMs() / 2);
    return static_cast<uint32_t>(
        std::max<uint64_t>(DATA_TURN_CONNECT_GUARD_FLOOR_MS, guard_ms));
}

uint32_t Connection::dataTurnControlGuardMs() const {
    const uint64_t guard_ms =
        static_cast<uint64_t>(currentBurstAnchorAirtimeMs()) +
        static_cast<uint64_t>(currentControlFrameAirtimeMs());
    return static_cast<uint32_t>(
        std::max<uint64_t>(DATA_TURN_CONTROL_GUARD_FLOOR_MS, guard_ms));
}

uint32_t Connection::turnRequestHoldoffAfterDataMs() const {
    const uint64_t guard_ms =
        2ULL * static_cast<uint64_t>(dataTurnControlGuardMs()) +
        static_cast<uint64_t>(currentDataFrameAirtimeMs());
    return static_cast<uint32_t>(
        std::max<uint64_t>(TURN_REQUEST_HOLDOFF_FLOOR_MS, guard_ms));
}

uint32_t Connection::turnRequestRetransmitMs() const {
    const uint64_t guard_ms =
        static_cast<uint64_t>(turnRequestHoldoffAfterDataMs()) +
        static_cast<uint64_t>(currentControlFrameAirtimeMs());
    return static_cast<uint32_t>(
        std::max<uint64_t>(TURN_REQUEST_RETRANSMIT_FLOOR_MS, guard_ms));
}

uint32_t Connection::turnRequestAckEmbeddedRetransmitMs() const {
    const uint64_t guard_ms =
        static_cast<uint64_t>(turnRequestRetransmitMs()) +
        static_cast<uint64_t>(dataTurnControlGuardMs());
    return static_cast<uint32_t>(
        std::min<uint64_t>(guard_ms, 0xFFFFFFFFull));
}

uint32_t Connection::fileCancelTxGuardMs() const {
    const uint64_t guard_ms =
        static_cast<uint64_t>(dataTurnControlGuardMs()) +
        2ULL * static_cast<uint64_t>(currentDataFrameAirtimeMs());
    return static_cast<uint32_t>(
        std::max<uint64_t>(FILE_CANCEL_TX_GUARD_FLOOR_MS, guard_ms));
}

uint32_t Connection::fileCancelConfirmDataGuardMs() const {
    const uint64_t guard_ms =
        static_cast<uint64_t>(dataTurnControlGuardMs()) +
        static_cast<uint64_t>(currentDataFrameAirtimeMs());
    return static_cast<uint32_t>(
        std::max<uint64_t>(FILE_CANCEL_CONFIRM_DATA_GUARD_FLOOR_MS, guard_ms));
}

uint32_t Connection::modeChangeRetryMs() const {
    // RATIOMETRIC control-exchange round trip at the CURRENT waveform (2026-07-03):
    // MODE_CHANGE and its ACK are single control frames — anchor + ctl airtime each
    // way + SACK coalesce — NOT a data-burst object. The old code preferred
    // arq_.getAckTimeout(), the unified multi-frame BURST deadline: 3-4x larger and
    // scaling with the ARQ window (worse at window 16). Rig W4 (IONOS MPG@20):
    // retries spaced 18.5 s for a ~5 s exchange — ~74 s of one 328 s transfer burned
    // waiting on MODE_CHANGE alone. dataTurnControlGuardMs derives from the current
    // mode's anchor/control airtimes, so this scales with waveform automatically
    // (MC-DPSK control is slower -> longer timer, by construction).
    // ULTRA_MODE_CHANGE_RETRY_MS [1000..60000] pins it for A/B.
    static const uint32_t env_pin = [] {
        if (const char* e = std::getenv("ULTRA_MODE_CHANGE_RETRY_MS")) {
            const long n = std::atol(e);
            if (n >= 1000 && n <= 60000) return static_cast<uint32_t>(n);
        }
        return 0u;
    }();
    if (env_pin > 0) {
        return env_pin;
    }

    // Airtime-only RTT (2x anchor+ctl + coalesce) UNDER-budgets the real exchange:
    // the peer cannot ACK until it drains its RX decode backlog (it may be
    // mid-burst-decode when the MODE_CHANGE lands), and each too-early retry keys
    // down half-duplex ON TOP of the ACK in flight — rig W5 (first run at the
    // airtime-only ~5 s timer): 72 MODE_CHANGE receptions, moves never committed,
    // livelock. Floor at HALF the unified burst ACK deadline: that deadline's
    // non-airtime half models exactly the receiver decode/SACK-hold budget, and
    // both terms stay ratiometric (scale with mod/rate/window). Empirically ~9 s
    // wideband — above every clean rig ACK RTT (W1-W4: all ACKs < 18.5 s, most
    // first-try), half the old full-deadline borrow.
    const uint64_t control_round_trip_ms =
        2ULL * static_cast<uint64_t>(dataTurnControlGuardMs()) +
        static_cast<uint64_t>(connection_policy::kCarrierSenseSackCoalesceMs);
    // 2026-07-03 rig bisect W5/W5b/W6: HALF the deadline (~9 s) still stalled
    // transfers (retries keyed onto ACKs still being produced while the peer
    // drained its decode backlog); the FULL deadline ran clean (W6: 1.88 kbps,
    // 5 MC receptions). The full burst deadline IS the ratiometric bound the
    // user asked for — it scales with mod/rate/window — and the peer may
    // legitimately not ACK until the whole outstanding burst is processed.
    // What survives of the fast-timer work: the 4-retry budget and the env pin.
    const uint64_t decode_backlog_floor_ms = arq_.getAckTimeout();
    return static_cast<uint32_t>(std::min<uint64_t>(
        std::max(control_round_trip_ms, decode_backlog_floor_ms), 0xFFFFFFFFull));
}

void Connection::scheduleModeChangeAckRepeats(const Bytes& ack_data, uint16_t ack_seq) {
    if (!isOFDMMode(negotiated_mode_)) {
        return;
    }

    // The MODE_CHANGE ACK is the ONE control ACK that still rides the fragile
    // 1-CW control path AND gates every rate move — but it inherited the DATA
    // ack-repeat count, which the tone-ACK path dials to 1 (kWideOFDMAckRepeat
    // = interactive ? 1 : 3). One lost ~1.4 s copy in a trough = a full-deadline
    // (~18.5 s) sender retry; rig measured up to 5 receptions per climb. Decouple:
    // on a fading channel send staggered repeats (the scheduler below already
    // exists and early-returned at count 1); AWGN-class keeps the single copy.
    // ULTRA_MC_ACK_REPEATS [1..3] pins for A/B.
    static const int env_pin = [] {
        if (const char* e = std::getenv("ULTRA_MC_ACK_REPEATS")) {
            const int n = std::atoi(e);
            if (n >= 1 && n <= 3) return n;
        }
        return 0;
    }();
    const int fading_aware = fading_index_ >= kFadingAwgnMax ? 3 : 1;
    const int repeat_count =
        env_pin > 0 ? env_pin : std::max(arq_.getAckRepeatCount(), fading_aware);
    if (repeat_count <= 1) {
        return;
    }

    for (auto it = mode_change_ack_repeat_jobs_.begin();
         it != mode_change_ack_repeat_jobs_.end();) {
        if (it->seq == ack_seq) {
            it = mode_change_ack_repeat_jobs_.erase(it);
        } else {
            ++it;
        }
    }

    constexpr uint32_t kAckPayloadBitmap = 0;
    for (int copy_index = 2; copy_index <= repeat_count; ++copy_index) {
        const uint32_t base_delay_ms =
            selective_repeat_arq_policy::ackRepeatDelayForCopy(
                arq_.getAckRepeatDelay(), copy_index);
        const int jitter_ms = selective_repeat_arq_policy::ackRepeatJitterMs(
            ack_seq, kAckPayloadBitmap, copy_index);
        int64_t scheduled_ms = static_cast<int64_t>(base_delay_ms) + jitter_ms;
        if (scheduled_ms < 1) {
            scheduled_ms = 1;
        }

        ModeChangeAckRepeatJob job;
        job.frame_data = ack_data;
        job.seq = ack_seq;
        job.timer_ms = static_cast<uint32_t>(scheduled_ms);
        job.copy_index = copy_index;
        mode_change_ack_repeat_jobs_.push_back(std::move(job));

        LOG_MODEM(INFO,
                  "Connection: MODE_CHANGE ACK_REPEAT scheduled seq=%u copy=%d delay=%ums jitter=%dms",
                  static_cast<unsigned>(ack_seq),
                  copy_index,
                  static_cast<unsigned>(scheduled_ms),
                  jitter_ms);
    }
}

void Connection::tickModeChangeAckRepeats(uint32_t elapsed_ms) {
    for (auto it = mode_change_ack_repeat_jobs_.begin();
         it != mode_change_ack_repeat_jobs_.end();) {
        ModeChangeAckRepeatJob& job = *it;
        if (elapsed_ms >= job.timer_ms) {
            LOG_MODEM(INFO,
                      "Connection: MODE_CHANGE ACK_REPEAT sent seq=%u copy=%d",
                      static_cast<unsigned>(job.seq),
                      job.copy_index);
            transmitFrame(job.frame_data);
            it = mode_change_ack_repeat_jobs_.erase(it);
            continue;
        }

        job.timer_ms -= elapsed_ms;
        ++it;
    }
}

void Connection::configureArqForCurrentDataMode() {
    arq_.setCodeRate(data_code_rate_);
    arq_.setFixedFrameCodewords(data_frame_cw_count_);
    arq_.setAckBatchThroughMoreFrag(false);
    arq_.setSackDelaySlidesOnData(false);

    if (isOFDMMode(negotiated_mode_) || usesBoundedVariableMCDPSKFrames()) {
        file_transfer_.setMaxChunkPayload(currentDataPayloadCapacity());
    }

    if (negotiated_mode_ == WaveformMode::MC_DPSK) {
        const auto timing = connection_policy::mcDpskFrameTiming(
            data_modulation_,
            config_.mc_dpsk_num_carriers,
            config_.mc_dpsk_samples_per_symbol,
            data_frame_cw_count_);
        const size_t window_size = connection_policy::mcDpskWindowSizeForTiming(timing);
        arq_.setWindowSize(window_size);
        arq_.setSackDelay(connection_policy::kCarrierSenseSackCoalesceMs);
        arq_.setSackDelayShort(0);
        arq_.setAckBatchThroughMoreFrag(true);
        arq_.setImmediateOutOfOrderSackEnabled(true);
        arq_.setAckRepeatCount(connection_policy::kCarrierSenseAckRepeatCount);
        arq_.setAckRepeatPeerBurstGuardMs(arq_.getSackDelay());
        // BUG-MCDPSK-ACK-COLLISION: the tone-burst partial (hole-bearing) SACK fires this
        // long after the last decoded out-of-order frame. It MUST exceed one MC-DPSK frame
        // airtime (~timing.data_ms), else it lands while the sender is still transmitting a
        // trailing failed frame of the same ~18.7 s window burst -> half-duplex collision ->
        // the sender never hears the NACK -> RTO whole-window resend -> phase-locked
        // livelock -> disconnect. One frame airtime + a T/R/decode margin lands the SACK in
        // the inter-burst gap. Floored at the 1500 ms OFDM default; stays well under the
        // ~31.6 s ACK RTO. (Carrier-sense — defer until the channel is heard idle — is the
        // fully radio-correct generalization; this airtime-scaled guard is the targeted fix.)
        // Compute the receiver tone-burst partial-SACK hold ONCE and feed it to BOTH the
        // receiver (setToneBurstPartialSackDelayMs) AND the sender's ACK RTO budget
        // (computeMCDPSKAckTimeoutMs) — otherwise the sender's deadline omits the hold the
        // receiver actually applies, times out before the ACK round-trip completes, and
        // blind-resends the whole window (BUG-MCDPSK-FILE-COMPLETION: the resulting doubled
        // airtime means the FINAL file chunk is never reached in-session -> never finalizes).
        // Previously the RTO was passed arq_.getSackDelay() = 30 ms carrier-sense coalesce,
        // NOT this ~6.4 s hold.
        const uint32_t sack_hold_ms =
            std::max<uint32_t>(1500u, timing.data_ms + 1000u);
        arq_.setToneBurstPartialSackDelayMs(sack_hold_ms);
        uint32_t ack_timeout_ms = connection_policy::computeMCDPSKAckTimeoutMs(
            timing, window_size, sack_hold_ms,
            connection_policy::kCarrierSenseAckRepeatCount);
        if (data_modulation_ == Modulation::DBPSK &&
            config_.mc_dpsk_samples_per_symbol >= 2048) {
            ack_timeout_ms = std::max<uint32_t>(
                ack_timeout_ms, connection_policy::kMCDPSKRobustLowAckTimeoutFloorMs);
        }
        arq_.setAckTimeout(ack_timeout_ms);
        LOG_MODEM(INFO, "Connection: ARQ window=%zu, timeout=%.1fs (data=%ums, ack=%ums x%d), carrier_sense_sack_coalesce=%ums, cw=%d (MC-DPSK %s %s, carriers=%d, sps=%d)",
                  window_size,
                  ack_timeout_ms / 1000.0f,
                  timing.data_ms,
                  timing.ack_ms,
                  connection_policy::kCarrierSenseAckRepeatCount,
                  arq_.getSackDelay(),
                  data_frame_cw_count_,
                  modulationToString(data_modulation_),
                  codeRateToString(data_code_rate_),
                  config_.mc_dpsk_num_carriers,
                  config_.mc_dpsk_samples_per_symbol);
    } else if (negotiated_mode_ == WaveformMode::OFDM_NARROW) {
        // Selective-repeat window=3 — chosen after A/B in cli_simulator
        // SNR=8 good fading R1/4 7-message test:
        //   window=1 (was): 180 s wall-clock
        //   window=2:        116 s (-36 %)
        //   window=3:         92 s (-49 %, +96 % throughput)
        //   window=4:         69 s (-62 %)
        //   window=8:         87 s (diminishing returns)
        // Settled on 3 because Codex audit explicitly capped at "2, maybe
        // 3 after tests"; window=4 hasn't been audited even though it
        // also passes every documented baseline. 30 m fading coherence
        // on real channels could correlate across a 4-frame burst (each
        // frame ~3.4 s) and turn one fade into 4 retransmits. If real-
        // OTA testing later shows we have headroom, bump to 4 after a
        // fresh audit. If correlated fades chew throughput, drop to 2.
        constexpr size_t kNarrowWindow = 3;
        arq_.setWindowSize(kNarrowWindow);
        arq_.setMaxRetries(15);
        arq_.setSackDelay(connection_policy::kCarrierSenseSackCoalesceMs);
        arq_.setSackDelayShort(0);
        arq_.setImmediateOutOfOrderSackEnabled(true);
        arq_.setAckRepeatCount(connection_policy::kCarrierSenseAckRepeatCount);
        arq_.setAckRepeatPeerBurstGuardMs(arq_.getSackDelay());

        const auto timing = connection_policy::narrowOFDMFrameTiming(
            data_modulation_, data_frame_cw_count_);
        uint32_t timeout_ms = connection_policy::computeNarrowOFDMAckTimeoutMs(
            data_modulation_, data_frame_cw_count_, kNarrowWindow, arq_.getSackDelay());
        arq_.setAckTimeout(timeout_ms);

        LOG_MODEM(INFO, "Connection: ARQ window=%zu, timeout=%.2fs (data=%ums, ack=%ums), carrier_sense_sack_coalesce=%ums, ack_repeat=%d, cw=%d (OFDM_NARROW %s %s)",
                  kNarrowWindow, timeout_ms / 1000.0f, timing.data_ms, timing.ack_ms,
                  arq_.getSackDelay(),
                  connection_policy::kCarrierSenseAckRepeatCount,
                  data_frame_cw_count_,
                  modulationToString(data_modulation_), codeRateToString(data_code_rate_));
    } else {
        // #58 increment 3: selection-flavored consumers (knob-gated pool aggregate) —
        // a lone trough snapshot could deny the window-16 gate for the whole session.
        const float window_snr_db = rateSelectionSnrDb();
        const bool near_awgn_ofdm =
            connection_policy::isNearAwgnOFDM(fading_index_, window_snr_db);
        arq_.setWindowSize(connection_policy::ofdmWindowSizeForChannel(
            data_modulation_, data_code_rate_, fading_index_, window_snr_db));
        // TRANSPORT MERGE (step 1): the tone-burst ack carries a 16-bit frame_mask (widened
        // 6->8 2026-06-17, 8->16 2026-07-02), so cap the in-flight window to 16. An N-frame
        // message then streams as ≤16-frame windows, each fully covered by one tone-burst
        // snapshot — no mask truncation, no spurious resend of frames past the mask. (MC-DPSK
        // 1-5 and OFDM_NARROW 3 are already within it.) The timing math below then sizes
        // timeouts for the capped window.
        if (kInteractiveToneAckEnabled() &&
            arq_.getWindowSize() > connection_policy::kToneBurstAckWindowCapFrames) {
            LOG_MODEM(INFO, "Connection: capped ARQ window %zu -> %zu (tone-burst 16-bit mask)",
                      arq_.getWindowSize(),
                      connection_policy::kToneBurstAckWindowCapFrames);
            arq_.setWindowSize(connection_policy::kToneBurstAckWindowCapFrames);
        }
        arq_.setMaxRetries(15);
        arq_.setAckBatchSize(connection_policy::ofdmAckBatchSize(near_awgn_ofdm));

        const auto timing = connection_policy::wideOFDMFrameTiming(
            data_modulation_, data_code_rate_, data_frame_cw_count_);
        const auto control_timing = connection_policy::wideOFDMFrameTiming(
            wideOFDMControlModulationForData(data_modulation_), CodeRate::R1_4);
        const bool adaptive_short_reanchor =
            connection_policy::shouldUseWideOFDMShortReanchor(
                negotiated_mode_, data_modulation_, fading_index_);
        const uint32_t continuation_reanchor_ms =
            adaptive_short_reanchor
                ? connection_policy::wideOFDMShortReanchorChirpDurationMs()
                : 0;
        const uint32_t control_ack_airtime_ms =
            control_timing.ack_ms + continuation_reanchor_ms;
        const uint32_t burst_airtime_ms = connection_policy::wideOFDMBurstAirtimeMs(
            data_modulation_, data_code_rate_, arq_.getWindowSize(),
            data_frame_cw_count_, continuation_reanchor_ms);
        // ACK diversity (repeat the ack N times) protects a single fade-lost ack on a
        // SACK-frame path. The tone-burst group-ack is different: it fires ONE prompt ack
        // per received burst, and a lost ack is already backstopped by the sender's ARQ
        // retransmit (which re-sends the group → the receiver re-acks). Repeating it just
        // keys the receiver down for ~5 s of redundant acks (deaf to the sender that whole
        // time) — the "4-5 ack chain" the operator saw. So: ONE ack on the tone-burst path.
        const int kWideOFDMAckRepeatCount = kInteractiveToneAckEnabled() ? 1 : 3;
        const uint32_t physical_sack_hold_ms = connection_policy::wideOFDMSackDelayMs(
            data_modulation_, data_code_rate_, arq_.getWindowSize(),
            data_frame_cw_count_, continuation_reanchor_ms);
        const uint32_t sack_delay_ms = connection_policy::wideOFDMSlidingSackDelayMs(
            data_modulation_, data_code_rate_, data_frame_cw_count_);
        arq_.setSackDelay(adaptive_short_reanchor ? physical_sack_hold_ms : sack_delay_ms);
        arq_.setSackDelayShort(connection_policy::wideOFDMSackTailDelayMs());
        arq_.setSackDelaySlidesOnData(!adaptive_short_reanchor);
        arq_.setImmediateOutOfOrderSackEnabled(!adaptive_short_reanchor);
        arq_.setAckRepeatCount(kWideOFDMAckRepeatCount);
        arq_.setAckRepeatPeerBurstGuardMs(
            adaptive_short_reanchor ? 0 : arq_.getSackDelay());
        arq_.setAckRepeatDelay(ackRepeatDelayForControlAirtimeMs(control_ack_airtime_ms));

        uint32_t ack_timeout_ms = connection_policy::computeWideOFDMAckTimeoutMs(
            data_modulation_,
            data_code_rate_,
            arq_.getWindowSize(),
            arq_.getSackDelay(),
            kWideOFDMAckRepeatCount,
            data_frame_cw_count_,
            continuation_reanchor_ms);
        const uint32_t ack_repeat_tail_ms =
            selective_repeat_arq_policy::ackRepeatTailGuardMs(
                control_ack_airtime_ms,
                arq_.getAckRepeatPeerBurstGuardMs(),
                arq_.getAckRepeatDelay(),
                kWideOFDMAckRepeatCount,
                true);
        const uint32_t decode_jitter_margin_ms =
            std::max<uint32_t>(700, timing.data_ms / 2) + 700;
        const uint64_t repeat_covered_timeout_ms =
            static_cast<uint64_t>(burst_airtime_ms) +
            static_cast<uint64_t>(physical_sack_hold_ms) +
            static_cast<uint64_t>(ack_repeat_tail_ms) +
            static_cast<uint64_t>(decode_jitter_margin_ms);
        if (adaptive_short_reanchor) {
            ack_timeout_ms = std::max<uint32_t>(
                ack_timeout_ms,
                static_cast<uint32_t>(
                    std::min<uint64_t>(repeat_covered_timeout_ms, 0xFFFFFFFFull)));
        }
        arq_.setAckTimeout(ack_timeout_ms);

        LOG_MODEM(INFO,
                  "Connection: ARQ window=%zu, timeout=%.2fs (data=%ums, burst=%ums, ack=%ums/control=%ums x%d), max_retries=%d, ack_batch=%u, sack_delay=%ums, sack_slides=%d, physical_sack_hold=%ums, tail_sack=%ums, ack_repeat=%d, ack_repeat_delay=%ums, ack_repeat_guard=%ums, cw=%d, continuation_reanchor=%ums (OFDM %s %s)",
                  arq_.getWindowSize(),
                  ack_timeout_ms / 1000.0f,
                  timing.data_ms,
                  burst_airtime_ms,
                  timing.ack_ms,
                  control_ack_airtime_ms,
                  kWideOFDMAckRepeatCount,
                  arq_.getMaxRetries(),
                  arq_.getAckBatchSize(),
                  arq_.getSackDelay(),
                  arq_.getSackDelaySlidesOnData() ? 1 : 0,
                  physical_sack_hold_ms,
                  arq_.getSackDelayShort(),
                  kWideOFDMAckRepeatCount,
                  arq_.getAckRepeatDelay(),
                  arq_.getAckRepeatPeerBurstGuardMs(),
                  data_frame_cw_count_,
                  continuation_reanchor_ms,
                  modulationToString(data_modulation_),
                  codeRateToString(data_code_rate_));
    }

    configureSoftCombineHARQBounds();
}

void Connection::configureSoftCombineHARQBounds() {
    const size_t max_entries = arq_.getWindowSize() *
        static_cast<size_t>(v2::sanitizeFixedFrameCodewords(data_frame_cw_count_));
    soft_combine_harq_.setMaxEntries(max_entries);
}

uint32_t Connection::pingTimeoutMsForCurrentProfile() const {
    // DBPSK MC-DPSK PING/PONG detection has to wait through the same slower
    // training/ref energy check as CONNECT detection. Keeping the
    // standard timer unchanged avoids slowing normal retries while preventing
    // robust DBPSK probe retries from overlapping the following CONNECT.
    const bool robust_dbpsk_probe =
        connect_waveform_ == WaveformMode::MC_DPSK &&
        (config_.forced_modulation == Modulation::DBPSK ||
         config_.mc_dpsk_samples_per_symbol >= 1024);
    return robust_dbpsk_probe ? ROBUST_LOW_PING_TIMEOUT_MS : PING_TIMEOUT_MS;
}

bool Connection::usesBoundedVariableMCDPSKFrames() const {
    return negotiated_mode_ == WaveformMode::MC_DPSK;
}

int Connection::selectBurstLiftingZ() const {
    // ULTRA_LDPC_Z is the SINGLE discovery override (force long LDPC for sweeps).
    if (const char* env = std::getenv("ULTRA_LDPC_Z")) {
        if (std::atoi(env) == 81) return 81;
    }
    // Traffic-class policy (C-policy): long LDPC (Z=81, n=1944) for bulk/file OFDM
    // bursts — a 1944-bit codeword spans ~1.8x the coherence interval at 0.1 Hz
    // Doppler, buying fade diversity where latency is free. Short (Z=27, n=648) for
    // control, interactive messages, and MC-DPSK (fast ACK turnaround).
    //
    // GATED ON use_burst_transport_: long LDPC needs the burst-group machinery —
    // the BURST_HEADER descriptor (so the RX learns Z=81 off the wire) and the
    // z=81 ⟹ cw=2 coupling. The default SelectiveRepeatARQ file path
    // (use_burst_transport_=false) emits NO descriptor, so it MUST stay Z=27 or the
    // RX can't know Z and the chunker/frame sizes diverge. The app/cli pushes this
    // Z to the TX encoder so its Z matches the chunker.
    // See docs/LDPC_Z_DERIVATION_DESIGN_2026_05_30.md.
    //
    // TRANSPORT MERGE (increment 1, ULTRA_UNIFIED_SEQ): the unified path sends the
    // file as REGULAR arq_ DATA frames (sendNextFileChunk → flushBurstBuffer), NOT
    // through burst_transport_'s long-LDPC group machinery. Those frames are
    // serialized declaring cw=data_frame_cw_count_ at the connection's data geometry;
    // forcing z=81 would re-couple the encoder to cw=2, so the RX reassembles 2
    // codewords where the frame header claims 4 → frame parse fails → 0/N decode.
    // Keeping Z at the DEFAULT 27 makes z/cw consistent end-to-end AND makes the
    // descriptor's z=27 itself the on-wire "regular frames, not a file" signal the RX
    // routes on (operator's keystone — no extra descriptor bit needed).
    if (isOFDMMode(negotiated_mode_) &&
        use_burst_transport_ &&
        !kUnifiedSeqEnabled() &&
        file_transfer_.getState() == FileTransferState::SENDING) {
        return 81;
    }
    return 27;
}

size_t Connection::burstAirtimeBudgetFrames(size_t max_frames) const {
    if (max_frames <= 1) {
        return std::max<size_t>(1, max_frames);
    }
    // Soft half-duplex airtime ceiling for ONE key-down. A real 100 W PA derates on
    // long key-downs and the T/R turnaround must catch air, so a single burst can't run
    // arbitrarily long; it also must not outlive its own tone-burst ack window. SOFT:
    // we just want "not ~10 s straight" — a burst that lands a touch over the nominal
    // 6 s (e.g. one more frame at ~6.8 s) is fine. This is the ONLY fixed number — the
    // frame count is DERIVED from the live per-frame airtime below, so it adapts across
    // modulation (bits/carrier), code rate (pilot spacing — N=648 coded bits is
    // rate-invariant, only pilots move), cw count, and fading (re-anchor) by construction.
    //
    // GROUP SIZE is this airtime ceiling for ONE key-down; the frame count is DERIVED from the
    // live per-frame airtime above. Default 8600 ms = a 5-frame group at the nominal z=27
    // QPSK-R2/3-cw8 rung (1392 ms/frame + 1200 ms dual-chirp anchor: 5 frames = 8560 ms <= 8600;
    // a 6th would be 10052 ms). Codified from a 20-seed Good@16 sweep (2026-06-07): groups 5 and
    // 6 TIE on goodput (~1400 bps, within run-to-run noise) and both deliver reliably with the
    // full-chirp-on-resend fix (maxretry=0; the two genuine failures recovered) — so the smaller
    // group wins on NON-speed grounds: shorter 8.6 s key-down (easier on a real PA than group 6's
    // ~10 s), fewer frames lost per fade, and well below the 16-bit SACK frame_mask ceiling (raised
    // 6->8 on 2026-06-17, 8->16 on 2026-07-02, so thin-frame cw5 bursts can fill the budget — a cw8 burst stays
    // airtime-bound at 5 frames regardless). Replaces the 3-frame 7000 ms default that re-paid the 1.2 s anchor +
    // turnaround every 3 frames. Still env-overridable for sweeps (clamped [5000, 12000]).
    static const uint32_t kMaxBurstAirtimeMs = [] {
        uint32_t v = 8600;  // group 5 (see above); ULTRA_MAX_BURST_AIRTIME_MS overrides
        if (const char* env = std::getenv("ULTRA_MAX_BURST_AIRTIME_MS")) {
            const long parsed = std::strtol(env, nullptr, 10);
            if (parsed >= 5000 && parsed <= 12000) {
                v = static_cast<uint32_t>(parsed);
            }
        }
        return v;
    }();
    const uint32_t reanchor_ms =
        connection_policy::shouldUseWideOFDMShortReanchor(
            negotiated_mode_, data_modulation_, fading_index_)
            ? connection_policy::wideOFDMShortReanchorChirpDurationMs()
            : 0;
    // Grow n until the NEXT frame would breach the ceiling. Uses the same airtime
    // formula the ARQ timeout is derived from, so budget and timeout stay coherent.
    size_t n = 1;
    while (n < max_frames) {
        const uint32_t airtime_ms = connection_policy::wideOFDMBurstAirtimeMs(
            data_modulation_, data_code_rate_, n + 1, data_frame_cw_count_,
            reanchor_ms, selectBurstLiftingZ());
        if (airtime_ms > kMaxBurstAirtimeMs) {
            break;
        }
        ++n;
    }
    return n;
}

uint32_t Connection::unifiedBurstAckTimeoutMs(size_t burst_frames) const {
    const uint32_t reanchor_ms =
        connection_policy::shouldUseWideOFDMShortReanchor(
            negotiated_mode_, data_modulation_, fading_index_)
            ? connection_policy::wideOFDMShortReanchorChirpDurationMs()
            : 0;
    // DATA frames carry the negotiated lifting z (z=81 long-LDPC ≈ 3× the coded bits/
    // codeword → ~3× the per-frame airtime); the control/ack frame is always short (z=27).
    const int data_z = selectBurstLiftingZ();
    // Single source of truth (testable across the whole mod/rate/cw/z matrix):
    // connection_policy::unifiedBurstAckTimeoutMs. It budgets burst airtime + the receiver's
    // SACK-coalesce holdoff (the term the old inline formula OMITTED — see the IONOS MPG E5
    // premature-timeout fix 2026-06-19) + decode jitter + ack-return + turnaround + the §16.4
    // reliability full-anchor reserve. arq_.getSackDelay() is only a FLOOR here — the free function
    // takes max(it, the mod/rate-modeled wideOFDMSackDelayMs), and that model floor is what
    // guarantees coverage of the receiver's full physical hold (the sender's own configured value is
    // its shorter sliding delay, not necessarily the peer's full-burst coalesce hold).
    return connection_policy::unifiedBurstAckTimeoutMs(
        data_modulation_, data_code_rate_, data_frame_cw_count_, burst_frames, data_z,
        wideOFDMControlModulationForData(data_modulation_), arq_.getSackDelay(), reanchor_ms);
}

size_t Connection::prepareUnifiedBurstWindow() {
    if (!(isOFDMMode(negotiated_mode_) && kUnifiedSeqEnabled())) {
        return std::numeric_limits<size_t>::max();  // legacy: fill the whole window
    }
    const size_t cap = burstAirtimeBudgetFrames(arq_.getWindowSize());
    // Size the ARQ retransmit timeout (→ the tone-burst ack-listen window, which floors
    // to it) to THIS burst's frame count under the prompt group-ack model. Set BEFORE
    // the submit loop so the per-frame arm reads the new value.
    arq_.setAckTimeout(unifiedBurstAckTimeoutMs(cap));
    return cap;
}

void Connection::armToneBurstAckListenWindow() {
    if (!(kInteractiveToneAckEnabled() && on_arm_tone_burst_ack_monitor_)) {
        return;
    }
    // Listen for the ack as long as we'd wait before resending — the ARQ ack timeout is
    // burst-aware (it covers the burst airtime + decode jitter + ack return), so the
    // monitor can never expire mid-round-trip. Floor at the interactive value for short
    // MC-DPSK/interactive sends. (Re-armed per (re)send; the monitor keeps the later
    // deadline and auto-disarms the instant an ack decodes.)
    uint32_t window_ms = kInteractiveToneAckWindowMs;
    if (isOFDMMode(negotiated_mode_)) {
        window_ms = std::max(window_ms, arq_.getAckTimeout());
    }
    on_arm_tone_burst_ack_monitor_(window_ms);
}

size_t Connection::currentDataPayloadCapacity() const {
    if (isOFDMMode(negotiated_mode_)) {
        // Z-aware capacity: at z=81 info bytes per codeword scale 3x. Sizing the
        // chunker at the active z (selectBurstLiftingZ) keeps the encoder from
        // zero-padding 70%+ of every burst (the real-world throughput killer).
        if (selectBurstLiftingZ() == 81) {
            return v2::getFixedFramePayloadCapacityZ(
                data_code_rate_, data_frame_cw_count_, 81);
        }
        return v2::getFixedFramePayloadCapacity(data_code_rate_, data_frame_cw_count_);
    }
    if (usesBoundedVariableMCDPSKFrames()) {
        return v2::getVariableFramePayloadCapacity(data_code_rate_, data_frame_cw_count_);
    }
    return SIZE_MAX;
}

LadderRungId Connection::currentLadderRungId() const {
    if (negotiated_mode_ == WaveformMode::OFDM_CHIRP) {
        return LadderRungId::OFDM_CHIRP;
    }
    if (negotiated_mode_ == WaveformMode::OFDM_NARROW) {
        return LadderRungId::OFDM_NARROW;
    }
    if (negotiated_mode_ == WaveformMode::MC_DPSK) {
        return connection_policy::rungForMCDPSKConfig(
            data_modulation_, config_.mc_dpsk_num_carriers,
            config_.mc_dpsk_samples_per_symbol, data_frame_cw_count_).id;
    }
    return LadderRungId::UNKNOWN;
}

void Connection::notifyDataModeChanged(float snr_db, float peer_fading_index,
                                       bool snr_is_wire) {
    if (!on_data_mode_changed_) {
        return;
    }
    const bool mc_dpsk = negotiated_mode_ == WaveformMode::MC_DPSK;
    on_data_mode_changed_(data_modulation_, data_code_rate_, data_frame_cw_count_,
                          snr_db, peer_fading_index,
                          mc_dpsk ? config_.mc_dpsk_num_carriers : 0,
                          mc_dpsk ? config_.mc_dpsk_samples_per_symbol : 0,
                          snr_is_wire);
}

// ─────────────────────── [LADDER] per-transfer telemetry ────────────────────────
// Sender-side observability for the fade-riding ladder: accumulates wall time per
// (modulation, rate) rung between startFileTransferNow and transfer completion, and
// counts mid-stream moves (applyDataMode mod/rate changes). Logged ONCE at completion:
//   [LADDER] qpsk_r34=54% qam16_r23=38% moves=9 (52s, ok)
// Pure telemetry — no control effect; wall clock is fine (the faithful gate and the
// rig both run wall==sample time).

// "QPSK"+"R3/4" -> "qpsk_r34" (lowercase, slash dropped) — grep-stable rung labels.
static std::string ladderRungLabel(Modulation mod, CodeRate rate) {
    std::string label = modulationToString(mod);
    label += '_';
    label += codeRateToString(rate);
    std::string out;
    out.reserve(label.size());
    for (char c : label) {
        if (c == '/') continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

void Connection::ladderTelemetryStart() {
    ladder_telemetry_active_ = true;
    ladder_moves_ = 0;
    ladder_rung_stats_.clear();
    ladder_transfer_start_ = ladder_rung_start_ = std::chrono::steady_clock::now();
    ladder_cur_mod_ = data_modulation_;
    ladder_cur_rate_ = data_code_rate_;
}

void Connection::ladderTelemetryNoteRung(Modulation mod, CodeRate rate, bool count_move) {
    if (!ladder_telemetry_active_) return;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - ladder_rung_start_).count();
    bool found = false;
    for (auto& s : ladder_rung_stats_) {
        if (s.mod == ladder_cur_mod_ && s.rate == ladder_cur_rate_) {
            s.seconds += seconds;
            found = true;
            break;
        }
    }
    if (!found) {
        ladder_rung_stats_.push_back({ladder_cur_mod_, ladder_cur_rate_, seconds});
    }
    ladder_rung_start_ = now;
    ladder_cur_mod_ = mod;
    ladder_cur_rate_ = rate;
    if (count_move) ++ladder_moves_;
}

void Connection::ladderTelemetryFinish(bool success) {
    if (!ladder_telemetry_active_) return;
    ladderTelemetryNoteRung(data_modulation_, data_code_rate_, /*count_move=*/false);
    ladder_telemetry_active_ = false;
    double total_s = 0.0;
    for (const auto& s : ladder_rung_stats_) total_s += s.seconds;
    if (total_s <= 0.0) return;
    std::string line;
    char buf[64];
    for (const auto& s : ladder_rung_stats_) {
        std::snprintf(buf, sizeof(buf), "%s%s=%.0f%%", line.empty() ? "" : " ",
                      ladderRungLabel(s.mod, s.rate).c_str(), 100.0 * s.seconds / total_s);
        line += buf;
    }
    LOG_MODEM(INFO, "Connection: [LADDER] %s moves=%d (%.0fs, %s)",
              line.c_str(), ladder_moves_, total_s, success ? "ok" : "fail");
}

void Connection::applyDataMode(Modulation mod, CodeRate rate, int cw_count,
                               LadderRungId rung_id) {
    if (rung_id != LadderRungId::UNKNOWN) {
        const auto rung = connection_policy::ladderRungForId(rung_id);
        if (rung.id != LadderRungId::UNKNOWN) {
            negotiated_mode_ = rung.waveform;
            if (rung.waveform == WaveformMode::MC_DPSK) {
                config_.mc_dpsk_num_carriers = rung.num_carriers;
                config_.mc_dpsk_samples_per_symbol = rung.samples_per_symbol;
                mod = rung.modulation;
                rate = rung.code_rate;
                if (cw_count == 0) {
                    cw_count = rung.cw_count;
                }
            }
        }
    }

    // Resolve final CW count: explicit value if specified (e.g. from
    // MODE_CHANGE wire byte), else auto-pick from rate.
    const int new_cw = (cw_count > 0)
        ? v2::sanitizeFixedFrameCodewords(cw_count)
        : connection_policy::recommendCWCount(mod, rate, negotiated_mode_);
    const bool rate_changed = rate != data_code_rate_;
    const bool cw_changed = new_cw != data_frame_cw_count_;
    // A MODULATION change (e.g. the QPSK-R3/4 -> QAM16-R2/3 climb) changes the constellation
    // geometry (bits/symbol -> frame airtime; per-CW BYTE capacity is rate/CW-derived,
    // getFixedFramePayloadCapacity, and does NOT change with modulation) — so like a rate/CW
    // change it must trigger the re-encode + HARQ flush. Computed BEFORE data_modulation_ is overwritten
    // below. NOTE: this drives requeuePendingChunks() + the soft_combine_harq_.clear() below (the
    // load-bearing part — stale old-constellation LLRs would corrupt HARQ). The deeper ARQ-window
    // byte-capacity rewind lives in setCodeRate()/setFixedFrameCodewords() (via
    // configureArqForCurrentDataMode), which early-return on an unchanged rate/CW — so a PURE
    // modulation-only transition (same rate AND same CW) is NOT yet fully ARQ-safe. That case does
    // not occur on the DEFAULT path: the QAM16 climb (R3/4->R2/3) and demote (R2/3->R3/4) ALWAYS
    // co-change the rate, so setCodeRate fires and the window rewinds correctly. EXCEPTION
    // (ULTRA_QAM16_R34, default-OFF A/B): the stuck-frame escape QAM16 R3/4 -> QPSK R3/4 is
    // mod-only at the SAME rate (and typically the same CW=8) — the setCodeRate rewind is skipped
    // there. The per-CW BYTE capacity is rate/CW-derived (LDPC K x cw_count), not
    // modulation-derived, so in-flight frame bytes stay geometry-valid and the requeue+HARQ flush
    // above still fire; validate that transition on the faithful gate before the knob graduates.
    const bool mod_changed = mod != data_modulation_;
    // Pending chunks must be re-encoded if rate OR CW OR modulation changed: the ARQ payload
    // capacity depends on all three, and chunks queued under the old geometry will
    // overflow / mis-align under the new one.
    const bool requeue_file =
        (rate_changed || cw_changed || mod_changed) &&
        file_transfer_.getState() == FileTransferState::SENDING &&
        file_transfer_.hasPendingChunks();
    const bool refill_file =
        (rate_changed || cw_changed || mod_changed) &&
        file_transfer_.getState() == FileTransferState::SENDING;
    if (requeue_file) {
        file_transfer_.requeuePendingChunks();
    }

    // [LADDER] telemetry: close the current rung segment on a real mid-transfer move.
    if ((rate_changed || mod_changed) && ladder_telemetry_active_) {
        ladderTelemetryNoteRung(mod, rate, /*count_move=*/true);
    }

    data_modulation_ = mod;
    data_code_rate_ = rate;
    data_frame_cw_count_ = new_cw;
    config_.fixed_frame_codewords = new_cw;
    data_ladder_rung_id_ = (rung_id != LadderRungId::UNKNOWN)
        ? rung_id
        : currentLadderRungId();
    configureArqForCurrentDataMode();
    // §RETX-PACING: a mode/rate change starts a NEW era — the zero-round evidence and any
    // armed hold belong to the rung we just left (§7 checklist: reset on applyDataMode).
    zero_progress_rounds_ = 0;
    trough_episode_active_ = false;  // trough-amnesty episode dies with the era
    retx_pace_hold_ms_ = 0;
    // RX-RATE-CMD Phase 2: an APPLIED mod/rate change is exactly the adoption the
    // receiver's standing rung command was waiting for (descriptor adopt and legacy
    // MODE_CHANGE both funnel through here) — clear the once-per-committed-move latch
    // so the next ACK stops carrying the consumed command. An idempotent re-apply
    // (nothing changed) is NOT an adoption and keeps the latch.
    if (rate_changed || mod_changed) {
        rx_rate_cmd_pending_ = 0;
        // RX-AUTHORITY: any real mode move (an obey, an ack-silence escape, a
        // legacy exchange) starts a new era for the obey-dedup — without this, a
        // safety escape that moved us OFF a previously-obeyed target would block
        // re-obeying that same target when the receiver re-commands it.
        tx_authority_last_obeyed_ = 0;
    }
    if (rate_changed || cw_changed || mod_changed) {
        soft_combine_harq_.clear();  // mod change => old-constellation LLRs would corrupt HARQ
        // 2026-05-28: recompute burst ack_timeout for the new mode (same
        // formula as startup / applyAdaptiveRateFeedback). MODE_CHANGE
        // negotiations can land on a slower rate where the original
        // timeout no longer covers the burst+ack round-trip; without
        // this, the next burst at the new rate will fire premature
        // resends every group.
    }

    if (refill_file) {
        deferred_file_refill_ = true;
    }
}

void Connection::commitPendingModeChange(const char* outcome) {
    if (!mode_change_pending_) {
        return;
    }

    LOG_MODEM(INFO, "Connection: MODE_CHANGE %s, applying %s %s",
              outcome,
              modulationToString(pending_modulation_),
              codeRateToString(pending_code_rate_));
    applyDataMode(pending_modulation_, pending_code_rate_,
                  pending_cw_count_, pending_ladder_rung_id_);
    mode_change_pending_ = false;
    mode_change_timeout_ms_ = 0;
    mode_change_retry_count_ = 0;
    pending_ladder_rung_id_ = LadderRungId::UNKNOWN;

    // pending_snr_db_ is what WE embedded in our MODE_CHANGE request — a LOCAL value.
    notifyDataModeChanged(pending_snr_db_, pending_fading_index_, /*snr_is_wire=*/false);
    runDeferredArqRefill();
}

// ═══════════════ DESC-SWITCH — descriptor-committed rate/mod move ═══════════════
// docs/MODE_SWITCH_PIGGYBACK_DESIGN_2026_07_03.md §5.1, knob ULTRA_DESCRIPTOR_MODE_SWITCH
// (default OFF = byte-identical). The DECISION machinery (RateController EMA + ssthresh +
// QAM16 climb/demote + clean-boundary gate + escape drops) is UNTOUCHED — only the COMMIT
// changes: instead of the MODE_CHANGE stop-and-wait round-trip (2.4-4 s clean, 18.5 s per
// retry × 2-9 receptions in troughs, TX frozen throughout), the sender applies the mode
// locally and the next burst's BURST_HEADER descriptor — control-profile QPSK R1/4,
// already trusted by the RX demod for exactly this reconfiguration — IS the announcement.
//
// Phase-1 scope gate: CLEAN-BOUNDARY wideband-OFDM ladder moves. This function itself
// carries NO in-flight guard — the boundary property comes from its callers. Phase 2
// (RX-RATE-CMD, maybeApplyRxRateCommand) adds the ONE sanctioned mid-window caller: a
// receiver-commanded demote, pre-gated on arq_.moveEpochEnabled() so the resulting
// commitLocalModeSwitch regrid is era-safe (the ARQ abort bumps the move-epoch).
// PHASE-3 (2026-07-04) added the second mid-window caller class: the sender's OWN
// ESCAPE/collapse drops — the FIRST of a silent stretch commits via descriptor (same
// move-epoch pre-gate); a second consecutive silent escape reverts to the legacy
// exchange so the deaf-peer escalation stays reachable (see executeEscapeDrop).
// Excluded BY DESIGN (all keep the legacy MODE_CHANGE exchange, in every knob state):
//   - connect-time INITIAL_SETUP and USER_REQUEST moves,
//   - MC-DPSK rung moves (carriers/sps need ladder_rung_id on the wire) and OFDM_NARROW,
//   - the deaf-peer escalation fallback (§6.5).
bool Connection::tryDescriptorModeSwitch(Modulation mod, CodeRate rate,
                                         float measured_snr, uint8_t reason) {
    if (!descriptor_mode_switch_enabled_) return false;
    if (state_ != ConnectionState::CONNECTED) return false;
    if (negotiated_mode_ != WaveformMode::OFDM_CHIRP) return false;  // wideband ladder only
    if (mode_change_pending_) return false;  // a legacy exchange is already in flight

    // CW pick: IDENTICAL to requestModeChange (operator --cw-count override preserved,
    // else the Doppler-coherence-refined channel recommendation) so the descriptor
    // announces the same frame geometry the old exchange would have negotiated.
    const int cw = (config_.forced_cw_count != 0)
        ? v2::sanitizeFixedFrameCodewords(config_.forced_cw_count)
        : connection_policy::recommendCWCountForChannel(
              mod, rate, negotiated_mode_,
              connection_policy::coherenceAdjustedFadingIndex(
                  fading_index_, coherence_score_, coherence_valid_),
              measured_snr);

    // Descriptor-bearing-burst guard: encodeBurstLight emits NO BURST_HEADER for a
    // single-frame burst (streaming_encoder.cpp:476-489) — the announcement could not
    // ride, and the lone post-switch frame would be undecodable at the peer's old
    // geometry (an RTO grind at the file tail, NOT the §6 row-1 one-lost-group case).
    // Commit via descriptor only when the remaining file payload guarantees a >=2-frame
    // (descriptor-bearing) group at the NEW geometry; the file tail and the non-file
    // (message) path fall back to the legacy exchange, which handles them correctly
    // today. This is also where the mid-transfer dead-air lives, so the fallback costs
    // nothing the design targets.
    if (file_transfer_.getState() != FileTransferState::SENDING) return false;
    const auto progress = file_transfer_.getProgress();
    if (progress.total_bytes <= progress.transferred_bytes) return false;
    const size_t remaining_bytes = progress.total_bytes - progress.transferred_bytes;
    const size_t frame_payload = (selectBurstLiftingZ() == 81)
        ? v2::getFixedFramePayloadCapacityZ(rate, cw, 81)
        : v2::getFixedFramePayloadCapacity(rate, cw);
    const size_t chunk_bytes =
        (frame_payload > FileTransferController::FILE_DATA_OVERHEAD)
            ? frame_payload - FileTransferController::FILE_DATA_OVERHEAD
            : 0;
    if (chunk_bytes == 0 || remaining_bytes <= chunk_bytes) return false;

    commitLocalModeSwitch(mod, rate, cw, measured_snr, reason);
    return true;
}

// The COMMIT half (§5.1 steps 1-3): applyDataMode NOW — no mode_change_pending_, no
// retry timer, no TX freeze; the next burst's descriptor (stamped with the new
// mod/rate/cw/z by transmitBurst → setDataMode) announces the move. Boundary cases:
//   - CLEAN boundary (all Phase-1 callers): the send window is drained, so
//     requeuePendingChunks() and arq_.setCodeRate's abort/rewind are no-ops — an
//     EMPTY-window regrid is collision-free by construction and needs NO epoch bump
//     (when ULTRA_ARQ_MOVE_EPOCH is ON the existing machinery still stamps
//     EPOCH_REBASE on the first frame at the window base and echoes the epoch on the
//     tone-ACK — belt-and-braces, zero extra work here).
//   - MID-WINDOW (Phase 2's receiver-commanded demote, the ONLY such caller, itself
//     gated on arq_.moveEpochEnabled()): frames ARE in flight — applyDataMode
//     requeues the pending chunks and the arq_.setCodeRate abort/rewind fires,
//     bumping the TX move-epoch (setCodeRate rate-abort; same-rate mod/CW regrids bump via the abortPendingTx payload-drop site, 2026-07-04) so the regrid is
//     a recognized new era, and the log line below shows the bumped value.
void Connection::commitLocalModeSwitch(Modulation mod, CodeRate rate, int cw_count,
                                       float measured_snr, uint8_t reason) {
    (void)reason;  // decision telemetry only — nothing rides a control frame on this path
    applyDataMode(mod, rate, cw_count, currentLadderRungId());

    // §2.6-arm-3 mitigation (MANDATORY, §5.1 step 2): the next burst group must carry a
    // full chirp+LTS anchor so the receiver re-derives |H| under the NEW pilot/carrier
    // geometry instead of equalizing with a stale warm-sync estimate (the 2026-06-09
    // unilateral flip's 0/8-forever arm). One-shot; consumed by flushBurstBuffer →
    // on_transmit_burst_(force_full_preamble=true). Same ~1.2 s the MODE_CHANGE ladder
    // already paid per move (kWideOFDMFullAnchorExtraMs).
    desc_switch_full_anchor_pending_ = true;

    ++stats_.descriptor_mode_switches;
    // A/B grep line (§7.2 metric): the epoch is the ARQ TX move-epoch — 0 while
    // ULTRA_ARQ_MOVE_EPOCH is OFF, and a clean-boundary commit never bumps it.
    LOG_MODEM(INFO, "Connection: DESC-SWITCH commit %s %s (epoch %u)",
              modulationToString(mod), codeRateToString(rate),
              static_cast<unsigned>(arq_.txMoveEpoch()));

    // GUI/modem follow-through — the encoder picks up the new mode before the next
    // transmitBurst. Same notify commitPendingModeChange fires; LOCAL reading.
    notifyDataModeChanged(measured_snr, wireFadingIndex(), /*snr_is_wire=*/false);
    // applyDataMode deferred the file refill (rate/CW/mod changed while SENDING);
    // release it now — the next burst goes out at the new rung with NO idle round-trip
    // (the whole point: the mode_change_pending_ TX freeze is gone).
    runDeferredArqRefill();
}

// RX side (§5.1 step 4b): the decoder consumed a mode-hop BURST_HEADER descriptor
// (wired decoder → ModemEngine → frontend binding → ProtocolEngine::onDescriptorModeChange,
// mirroring onBurstGroupReceived — same thread/locking class, §6 row 11). Run the
// RX-relevant subset of a mode change: applyDataMode sets data_modulation_/
// data_code_rate_/data_frame_cw_count_ and configureArqForCurrentDataMode refreshes
// window/timers/chunk capacity — ofdmWindowSize is mod/rate-dependent, so without this a
// receiver holding the old window would below-window-drop the tail of a wider post-hop
// burst. NO MODE_CHANGE ACK machinery fires: confirmation is the switched group's
// tone-burst ACK (implicit and free).
void Connection::onDescriptorModeChange(Modulation mod, CodeRate rate, int cw_per_frame) {
    if (!descriptor_mode_switch_enabled_) return;  // knob-OFF: byte-identical no-op
    if (state_ != ConnectionState::CONNECTED) return;
    if (negotiated_mode_ != WaveformMode::OFDM_CHIRP) return;  // wideband-OFDM scope
    if (mod == data_modulation_ && rate == data_code_rate_ &&
        (cw_per_frame <= 0 || cw_per_frame == data_frame_cw_count_)) {
        return;  // already adopted (re-announced descriptor on a resend) — idempotent
    }
    if (arq_.getTxInFlightBytes() > 0) {
        // Receiver-ISS asymmetry (§6 row 12): WE have our own DATA in flight
        // (half-duplex interactive role overlap). Adopting the peer's TX geometry now
        // would abort OUR send window; per-direction rungs are independent and the
        // peer's next descriptor re-announces. Skip.
        LOG_MODEM(WARN,
                  "Connection: DESC-SWITCH adopt skipped (%s %s) — local DATA in flight",
                  modulationToString(mod), codeRateToString(rate));
        return;
    }
    // §8 checklist 4: clean-boundary invariant — at a boundary switch the RX slots are
    // provably empty (the sender's drained window ⟹ we delivered in-order), so
    // arq_.setCodeRate's RX discard below is a no-op. Non-empty slots + move-epoch OFF
    // mean the sender committed mid-window without era safety — log loudly; the discard
    // still runs (existing setCodeRate semantics) and the sender's ARQ resends cover
    // the loss. With move-epoch ON this is the EXPECTED Phase-2 escape-adopt shape
    // (RX-RATE-CMD mid-window commit: the descriptor arrives ahead of the EPOCH_REBASE
    // frames whose adoption performs the discard) — INFO, not a violation.
    if (arq_.bufferedRxFrameCount() > 0) {
        if (arq_.moveEpochEnabled()) {
            LOG_MODEM(INFO,
                      "Connection: DESC-SWITCH mid-window adopt with %zu buffered RX "
                      "frames — era safety via move-epoch (Phase-2 escape path)",
                      arq_.bufferedRxFrameCount());
        } else {
            LOG_MODEM(WARN,
                      "Connection: DESC-SWITCH adopt with %zu buffered RX frames — "
                      "clean-boundary invariant violated (mid-window regrids belong to "
                      "the move-epoch machinery)",
                      arq_.bufferedRxFrameCount());
        }
    }
    // A/B grep line (§7.2 metric).
    LOG_MODEM(INFO, "Connection: DESC-SWITCH adopt %s %s",
              modulationToString(mod), codeRateToString(rate));
    applyDataMode(mod, rate, cw_per_frame, LadderRungId::UNKNOWN);
    ++stats_.descriptor_mode_switches;
    // GUI/modem follow-through: LOCAL snr reading (no SNR rides a descriptor); peer
    // fading unknown on this path → -1.0 = the existing "n/a" render.
    notifyDataModeChanged(measured_snr_db_, /*peer_fading_index=*/-1.0f,
                          /*snr_is_wire=*/false);
    runDeferredArqRefill();
}

void Connection::enterConnected() {
    state_ = ConnectionState::CONNECTED;
    connected_time_ms_ = 0;
    // #58 increment 3: the handshake spent (or never needed) its one pick-defer;
    // re-arm for the next handshake. Consulted only pre-CONNECT, so this is safe here.
    connect_pick_deferred_once_ = false;
    // Half-duplex INTERACTIVE (TNC/Winlink-B2F): the RESPONDER speaks first (the SID banner).
    // It does NOT need a pre-confirmed handshake to do so — the initiator proactively yields a
    // TURNOVER ~1.5 s after connect (see tick()), and receiving that TURNOVER is the responder's
    // "first valid frame" → it flips handshake_confirmed_ AND fires the modem-waveform switch
    // (onFrameReceived), both at the correct time, then handleTurnover makes it ISS. So the
    // responder enters as a normal IRS and the turn falls to it naturally.
    interactive_initiator_yield_done_ = false;
    interactive_yield_log_throttle_ms_ = 0;
    local_data_turn_ = is_initiator_;
    peer_data_turn_requested_ = false;
    local_turn_request_pending_ = false;
    received_peer_data_since_connect_ = false;
    yielded_data_turn_waiting_for_peer_data_ = false;
    data_turn_yield_pending_ = false;
    resetDataTurnFairness();
    // QAM16 climb state is per-connection (the re-climb cooldown/backoff resets here).
    qam16_clean_streak_ = 0;
    qam16_bad_streak_ = 0;
    qam16_r34_clean_streak_ = 0;
    qam16_reclimb_cooldown_ = 0;
    qam16_demote_count_ = 0;
    // Doppler-coherence verdict is per-connection: setChannelCoherence holds the last VALID
    // verdict while CONNECTED (BUG-DOPPLER-COHERENCE-MODECHANGE-WIPE), so it must start
    // invalid here or a previous connection's channel class would leak into this one.
    coherence_score_ = 0.0f;
    coherence_doppler_hz_ = 0.0f;
    coherence_valid_ = false;
    // §RETX-PACING: trough-pacing / collapse-escape round state is per-connection.
    zero_progress_rounds_ = 0;
    trough_episode_active_ = false;  // trough-amnesty episode dies with the era
    retx_pace_hold_ms_ = 0;
    last_data_burst_end_valid_ = false;
    // Software-ALC receiver-side state is per-connection.
    rx_level_low_streak_ = 0;
    rx_level_clipped_ = false;
    // RX-RATE-CMD Phase 2 state is per-connection (the seq dedup space restarts with
    // the ARQ reset below; a stale standing command must never leak across sessions).
    rx_rate_cmd_pending_ = 0;
    rx_rate_cmd_seq_seen_ = -1;
    rx_rebase_voice_seq_seen_ = -1;
    rx_authority_cmd_ = 0;
    tx_authority_last_obeyed_ = 0;
    rx_auth_obs_count_ = 0;
    rx_auth_obs_next_ = 0;
    rx_auth_crater_streak_ = 0;
    rx_auth_class_sticky_ = 1;
    rx_auth_class_streak_ = 0;
    rx_auth_fading_passed_ = 0.3f;
    for (size_t i = 0; i < kRungIdxCount; ++i) rx_auth_rung_penalty_db_[i] = 0.0f;
    burst_obs_snr_db_ = -1.0f;
    burst_obs_fading_ = -1.0f;
    burst_obs_coh_valid_ = false;
    qam16_rx_bad_streak_ = 0;
    last_applied_mode_change_valid_ = false;  // MC dedup (fix 3) is session-scoped
    data_turn_tx_guard_ms_ = 0;
    turn_request_retransmit_ms_ = 0;
    turn_request_holdoff_ms_ = 0;
    file_cancel_rx_drain_ms_ = 0;
    clearFileCancelReassertion();
    file_cancel_confirm_pending_ = false;
    if (local_data_turn_) {
        armDataTurnTxGuard(dataTurnConnectGuardMs());
    }

    if (is_initiator_ || handshake_confirmed_) {
        responder_handshake_wait_ms_ = 0;
    } else if (responder_handshake_wait_ms_ == 0) {
        responder_handshake_wait_ms_ = responderHandshakeFailSafeMs();
    }

    arq_.setCallsigns(local_call_, remote_call_);
    arq_.reset();
    configureArqForCurrentDataMode();
    mode_change_ack_repeat_jobs_.clear();

    LOG_MODEM(INFO, "Connection: Now CONNECTED to %s (mode=%s, data_turn=%s)",
              remote_call_.c_str(), waveformModeToString(negotiated_mode_),
              local_data_turn_ ? "ISS" : "IRS");
    LOG_MODEM(DEBUG, "Connection: B2F-DBG connect state: is_initiator=%d local_data_turn=%d handshake_confirmed=%d interactive=%d",
              is_initiator_ ? 1 : 0, local_data_turn_ ? 1 : 0,
              handshake_confirmed_ ? 1 : 0, half_duplex_interactive_ ? 1 : 0);

    if (on_mode_negotiated_) {
        on_mode_negotiated_(negotiated_mode_);
    }

    if (on_connected_) {
        on_connected_();
    }

}

void Connection::enterDisconnected(const std::string& reason) {
    state_ = ConnectionState::DISCONNECTED;
    is_initiator_ = false;
    handshake_confirmed_ = false;
    burst_activity_ = BurstActivity{};  // clear the "incoming burst" GUI indicator
    setPhyMaskV1Negotiated(false);
    narrowband_override_ = WaveformMode::AUTO;  // Clear session-scoped narrowband override
    std::string old_remote = remote_call_;
    remote_call_.clear();
    pending_remote_call_.clear();
    mode_change_pending_ = false;
    desc_switch_full_anchor_pending_ = false;
    rx_rate_cmd_pending_ = 0;       // RX-RATE-CMD: session-scoped
    rx_rate_cmd_seq_seen_ = -1;
    rx_rebase_voice_seq_seen_ = -1;
    rx_authority_cmd_ = 0;
    tx_authority_last_obeyed_ = 0;
    rx_auth_obs_count_ = 0;
    rx_auth_obs_next_ = 0;
    rx_auth_crater_streak_ = 0;
    rx_auth_class_sticky_ = 1;
    rx_auth_class_streak_ = 0;
    rx_auth_fading_passed_ = 0.3f;
    for (size_t i = 0; i < kRungIdxCount; ++i) rx_auth_rung_penalty_db_[i] = 0.0f;
    burst_obs_snr_db_ = -1.0f;
    burst_obs_fading_ = -1.0f;
    burst_obs_coh_valid_ = false;
    qam16_rx_bad_streak_ = 0;
    last_applied_mode_change_valid_ = false;  // MC dedup (fix 3) is session-scoped
    mode_change_ack_repeat_jobs_.clear();
    disconnect_frame_.clear();
    disconnect_pending_ = false;
    disconnect_ack_frame_.clear();
    burst_mode_active_ = false;
    burst_tx_buffer_.clear();
    rx_level_low_streak_ = 0;   // software-ALC: per-connection
    rx_level_clipped_ = false;
    responder_handshake_wait_ms_ = 0;
    connect_ack_frame_.clear();
    connect_ack_retransmit_ms_ = 0;
    connect_ack_retx_remaining_ = 0;
    local_data_turn_ = false;
    peer_data_turn_requested_ = false;
    local_turn_request_pending_ = false;
    received_peer_data_since_connect_ = false;
    yielded_data_turn_waiting_for_peer_data_ = false;
    data_turn_yield_pending_ = false;
    resetDataTurnFairness();
    data_turn_tx_guard_ms_ = 0;
    turn_request_retransmit_ms_ = 0;
    turn_request_holdoff_ms_ = 0;
    file_cancel_rx_drain_ms_ = 0;
    clearFileCancelReassertion();
    file_cancel_confirm_pending_ = false;
    data_ladder_rung_id_ = LadderRungId::UNKNOWN;
    pending_ladder_rung_id_ = LadderRungId::UNKNOWN;
    arq_callback_defer_refill_ = false;
    deferred_file_refill_ = false;
    deferred_fragment_refill_ = false;
    arq_.reset();
    soft_combine_harq_.clear();
    file_transfer_.cancel();
    queued_file_path_.reset();
    queued_payloads_.clear();
    pending_tx_fragments_.clear();
    pending_tx_fragment_flags_.clear();
    pending_tx_fragment_types_.clear();
    pending_tx_fragment_message_tokens_.clear();
    next_fragment_idx_ = 0;
    acked_fragment_count_ = 0;
    rx_reassembly_buffer_.clear();
    clearOutboundMessageTracking();
    // #58 increment 3: session boundary — nothing in the connect-SNR pool may leak
    // into the next handshake's entry pick.
    connect_snr_pool_.clear();
    connect_pick_deferred_once_ = false;

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

void Connection::setTransmitInfoCallback(TransmitInfoCallback cb) {
    on_transmit_info_ = std::move(cb);
}

void Connection::setTransmitBurstCallback(TransmitBurstCallback cb) {
    on_transmit_burst_ = std::move(cb);
    if (on_transmit_burst_) {
        arq_.setTransmitBatchCallback([this](const std::vector<Bytes>& frames) {
            transmitFrameBatch(frames);
        });
    } else {
        arq_.setTransmitBatchCallback(SelectiveRepeatARQ::TransmitBatchCallback{});
    }
}

void Connection::setMCDPSKConfig(int num_carriers, int samples_per_symbol) {
    config_.mc_dpsk_num_carriers = std::clamp(num_carriers, 1, 64);
    config_.mc_dpsk_samples_per_symbol = std::clamp(samples_per_symbol, 1, 8192);
}

void Connection::setPhyMaskV1Negotiated(bool enabled) {
    if (phy_mask_v1_negotiated_ == enabled) {
        return;
    }
    phy_mask_v1_negotiated_ = enabled;
    LOG_MODEM(INFO, "Connection: PHY_MASK_V1 %s",
              enabled ? "negotiated" : "disabled");
    if (on_phy_mask_v1_negotiated_) {
        on_phy_mask_v1_negotiated_(enabled);
    }
}

void Connection::flushBurstBuffer() {
    if (burst_tx_buffer_.empty()) return;

    const size_t real_frame_count = burst_tx_buffer_.size();
    const bool final_file_tail =
        file_transfer_.getState() == FileTransferState::SENDING &&
        isFinalDataFrame(burst_tx_buffer_.back());
    if (!final_file_tail &&
        shouldPadPartialOFDMBurst(negotiated_mode_,
                                  data_modulation_,
                                  data_code_rate_,
                                  fading_index_,
                                  measured_snr_db_,
                                  file_transfer_.getState(),
                                  real_frame_count)) {
        const size_t remainder =
            real_frame_count % connection_policy::burstInterleaveGroupFrames();
        const size_t pad_count =
            connection_policy::burstInterleaveGroupFrames() - remainder;
        for (size_t i = 0; i < pad_count; ++i) {
            auto pad_frame = v2::makeFixedDataFrame(
                local_call_,
                kOFDMBurstPadCallsign,
                static_cast<uint16_t>(kOFDMBurstPadSeq - i),
                makeOFDMBurstPadPayload(data_code_rate_, data_frame_cw_count_, i),
                data_code_rate_,
                data_frame_cw_count_).serialize();
            burst_tx_buffer_.push_back(pad_frame);
        }
        LOG_MODEM(INFO,
                  "Connection: Padded OFDM burst %zu -> %zu frames for burst interleaver",
                  real_frame_count,
                  burst_tx_buffer_.size());
    }

    if (burst_tx_buffer_.size() == 1 && on_transmit_) {
        // Single frame, no burst needed. NOTE: desc_switch_full_anchor_pending_ is
        // deliberately NOT consumed here — a single-frame send carries no BURST_HEADER
        // descriptor (encodeBurstLight:476-489), so the one-shot stays armed for the
        // next descriptor-bearing (multi-frame) burst.
        on_transmit_(burst_tx_buffer_[0]);
    } else if (on_transmit_burst_) {
        LOG_MODEM(INFO, "Connection: Flushing burst of %zu frames", burst_tx_buffer_.size());
        // DESC-SWITCH §5.1 step 2: the first burst group after a descriptor-committed
        // mode switch carries the full chirp+LTS anchor (one-shot; the §2.6-arm-3
        // geometry-change mitigation). force_full_preamble routes through the frontend
        // to StreamingEncoder::forceNextBurstGroupStartFullPreamble AND resets the
        // encoder's anchor-skip clean streak (warm_descriptor=false path).
        const bool desc_switch_anchor = desc_switch_full_anchor_pending_;
        desc_switch_full_anchor_pending_ = false;
        on_transmit_burst_(burst_tx_buffer_, /*group_seq=*/0,
                           /*force_full_preamble=*/desc_switch_anchor);  // legacy arq_ burst path
    } else if (on_transmit_) {
        // Fallback: send individually
        for (const auto& frame : burst_tx_buffer_) {
            on_transmit_(frame);
        }
    }
    // §RETX-PACING: stamp the modeled key-down end for this data burst (no-op off wideband
    // OFDM; behavior-free — see noteDataBurstKeydown).
    noteDataBurstKeydown(burst_tx_buffer_.size());
    burst_tx_buffer_.clear();
}

void Connection::transmitFrameBatch(const std::vector<Bytes>& frame_data_list) {
    if (frame_data_list.empty()) {
        return;
    }

    // §RETX-PACING §1.1: this callback fires ONLY as the ARQ slot-RTO batch (arq_.tick →
    // transmitDataBatch), and an RTO round is zero-progress BY DEFINITION — a timeout IS
    // the absence of an ack. Account the round BEFORE stamping this resend's own key-down
    // end time (the elapsed-listening subtraction must reference the PREVIOUS burst). The
    // collapse escape itself is polled from the CONNECTED tick, never fired from inside
    // this ARQ transmit callback.
    noteArqRoundOutcome(0, "rto");
    noteDataBurstKeydown(frame_data_list.size());

    const bool burst_capable_mode =
        isOFDMMode(negotiated_mode_) || negotiated_mode_ == WaveformMode::MC_DPSK;
    if (frame_data_list.size() == 1 || !on_transmit_burst_ || !burst_capable_mode) {
        for (const auto& frame_data : frame_data_list) {
            transmitFrame(frame_data);
        }
        return;
    }

    // A timed-out burst is resent as a WHOLE RE-INTERLEAVED BURST on one anchor —
    // NOT as N standalone full-anchor frames (the old SR-ARQ "standalone repair
    // anchors" path, §14.20 throw). Standalone repair frames threw away the
    // cross-frame burst interleaving (so the resend had no fade diversity and was
    // just as likely to fail on the next fade) AND paid N chirp anchors instead of
    // one — the dominant goodput sink on Good fading. encodeBurstLight re-interleaves
    // the group, so the resend gets fresh fade diversity at ~1/N the preamble cost.
    //
    // RESEND USES A FULL CHIRP+LTS ANCHOR (force_full_preamble=true), not warm light-LTS.
    // A timeout almost always means the receiver MISSED the group's light-LTS acquisition at
    // the warm-handoff boundary — it never completed the group, so it never acked. On that
    // miss the RX side ALREADY arms expect_full_anchor=1 ("waiting for a full chirp on the
    // resend", §16.4 escalation). Resending with light-LTS again just re-misses the same
    // preamble; a full chirp re-acquires deterministically. Costs ~1.4 s extra airtime ONLY
    // on resends (rare) — first-attempt groups keep warm light-LTS (+goodput). Restores the
    // pre-unification reliability coupling the merge dropped when it hardcoded this to false.
    LOG_MODEM(INFO, "Connection: Resending ARQ timeout-repair as re-interleaved burst of %zu frames (full anchor)",
              frame_data_list.size());
    on_transmit_burst_(frame_data_list, /*group_seq=*/0,
                       /*force_full_preamble=*/true);  // full chirp re-anchor: deterministic re-acquire
    // RE-ARM the ack monitor for THIS resend's ack. The initial send arms via the
    // tx-frame-submitted hook, but a timeout RESEND comes through here (arq_ retransmit →
    // transmitDataBatch) and would otherwise leave the sender deaf after the first
    // window expired — the half-duplex phase lock on fading (resend forever, never hear
    // the ack). Mirrors the original burst sender, which re-armed per group (re)send.
    armToneBurstAckListenWindow();
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

void Connection::setMessageTxStatusCallback(MessageTxStatusCallback cb) {
    on_message_tx_status_ = std::move(cb);
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
    // Wrap so the "incoming burst" GUI indicator clears the moment the file finishes
    // (success or fail), not only on disconnect.
    file_transfer_.setReceivedCallback(
        [this, cb = std::move(cb)](const std::string& path, bool success,
                                   const std::string& error) {
            burst_activity_ = BurstActivity{};
            if (cb) cb(path, success, error);
        });
}

void Connection::setFileSentCallback(FileSentCallback cb) {
    // Wrap so the [LADDER] per-transfer telemetry summary is emitted exactly once at
    // completion (success OR failure/cancel — every on_sent_ path), before the app callback.
    file_transfer_.setSentCallback(
        [this, cb = std::move(cb)](bool success, const std::string& error) {
            ladderTelemetryFinish(success);
            if (cb) cb(success, error);
        });
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

void Connection::setSoftCombiningHARQ(bool enable) {
    soft_combine_harq_.setEnabled(enable);
    LOG_MODEM(INFO, "Connection: soft-combining HARQ %s",
              enable ? "ENABLED" : "disabled");
}

std::optional<fec::SoftCombineBuffer::ProvisionalContext>
Connection::harqProvisionalContext() const {
    if (state_ != ConnectionState::CONNECTED ||
        remote_call_.empty() ||
        !soft_combine_harq_.enabled()) {
        return std::nullopt;
    }

    fec::SoftCombineBuffer::ProvisionalContext ctx;
    ctx.sender_hash = v2::hashCallsign(remote_call_);
    ctx.seq = arq_.getRxBaseSeq();
    ctx.window_size = arq_.getWindowSize();
    // The receiver's mirror of the sender's next-burst fill (see
    // predictedIncomingSeqs) — indexed by burst logical position.
    ctx.predicted_seqs = arq_.predictedIncomingSeqs(arq_.getWindowSize());
    if (!ctx.valid()) {
        return std::nullopt;
    }
    return ctx;
}

void Connection::reset() {
    state_ = ConnectionState::DISCONNECTED;
    is_initiator_ = false;
    handshake_confirmed_ = false;
    setPhyMaskV1Negotiated(false);
    remote_call_.clear();
    pending_remote_call_.clear();
    timeout_remaining_ms_ = 0;
    connect_retry_count_ = 0;
    connected_time_ms_ = 0;
    narrowband_override_ = WaveformMode::AUTO;  // Clear session-scoped narrowband override
    negotiated_mode_ = WaveformMode::OFDM_CHIRP;
    remote_capabilities_ = ModeCapabilities::OFDM_CHIRP;
    remote_preferred_ = WaveformMode::OFDM_CHIRP;
    mode_change_pending_ = false;
    mode_change_timeout_ms_ = 0;
    mode_change_retry_count_ = 0;
    pending_ladder_rung_id_ = LadderRungId::UNKNOWN;
    desc_switch_full_anchor_pending_ = false;
    mode_change_ack_repeat_jobs_.clear();
    data_modulation_ = Modulation::DQPSK;
    data_code_rate_ = CodeRate::R1_4;
    data_ladder_rung_id_ = LadderRungId::UNKNOWN;
    connect_waveform_ = WaveformMode::MC_DPSK;  // Reset to DPSK for next connect attempt
    responder_handshake_wait_ms_ = 0;
    connect_ack_frame_.clear();
    connect_ack_retransmit_ms_ = 0;
    connect_ack_retx_remaining_ = 0;
    local_data_turn_ = false;
    peer_data_turn_requested_ = false;
    local_turn_request_pending_ = false;
    received_peer_data_since_connect_ = false;
    yielded_data_turn_waiting_for_peer_data_ = false;
    data_turn_yield_pending_ = false;
    resetDataTurnFairness();
    data_turn_tx_guard_ms_ = 0;
    turn_request_retransmit_ms_ = 0;
    turn_request_holdoff_ms_ = 0;
    file_cancel_rx_drain_ms_ = 0;
    clearFileCancelReassertion();
    file_cancel_confirm_pending_ = false;
    burst_mode_active_ = false;
    burst_tx_buffer_.clear();
    arq_callback_defer_refill_ = false;
    deferred_file_refill_ = false;
    deferred_fragment_refill_ = false;
    arq_.reset();
    soft_combine_harq_.clear();
    file_transfer_.cancel();
    queued_file_path_.reset();
    queued_payloads_.clear();
    pending_tx_fragments_.clear();
    pending_tx_fragment_flags_.clear();
    pending_tx_fragment_types_.clear();
    pending_tx_fragment_message_tokens_.clear();
    next_fragment_idx_ = 0;
    acked_fragment_count_ = 0;
    rx_reassembly_buffer_.clear();
    clearOutboundMessageTracking();
    // Per-connection channel-coherence verdict (see setChannelCoherence hold-last-valid).
    coherence_score_ = 0.0f;
    coherence_doppler_hz_ = 0.0f;
    coherence_valid_ = false;
    // §RETX-PACING: trough-pacing / collapse-escape round state is per-connection.
    zero_progress_rounds_ = 0;
    trough_episode_active_ = false;  // trough-amnesty episode dies with the era
    retx_pace_hold_ms_ = 0;
    last_data_burst_end_valid_ = false;
    ladder_telemetry_active_ = false;
    // #58 increment 3: the connect-SNR pool is per-session (its entry horizon IS the
    // handshake scope); the defer one-shot re-arms for the next handshake.
    connect_snr_pool_.clear();
    connect_pick_deferred_once_ = false;
    LOG_MODEM(DEBUG, "Connection: Full reset");
}

} // namespace protocol
} // namespace ultra
