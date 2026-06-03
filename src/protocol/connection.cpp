// Connection state machine - core logic
// Frame handlers are in connection_handlers.cpp

#include "connection.hpp"
#include "connection_policy.hpp"
#include "gui/startup_trace.hpp"
#include "waveform_selection.hpp"
#include "ultra/logging.hpp"
#include <algorithm>
#include <filesystem>

namespace ultra {
namespace protocol {

namespace {
constexpr size_t kOFDMFileBlockPayloadLimit = 2300;
constexpr const char* kOFDMBurstPadCallsign = "ULPAD";
constexpr uint16_t kOFDMBurstPadSeq = 0xFFFE;
constexpr size_t kMaxQueuedPayloads = 32;

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

int statDelta(int now, int before) {
    return std::max(0, now - before);
}

struct AdaptivePressure {
    bool present = false;
    bool severe = false;
};

AdaptivePressure getAdaptiveRetryPressure(const ARQStats& now,
                                          const ARQStats& before,
                                          size_t window_size) {
    const int retransmissions =
        statDelta(now.retransmissions, before.retransmissions);
    const int timeouts = statDelta(now.timeouts, before.timeouts);
    const int failed = statDelta(now.failed, before.failed);
    const int holes = statDelta(now.hole_events, before.hole_events);

    AdaptivePressure pressure;
    pressure.present =
        timeouts > 0 || failed > 0 || retransmissions >= 2 || holes >= 2;

    // A whole ARQ window timing out is not ordinary fading noise; it means the
    // current mode has stopped delivering a usable feedback loop. Step down one
    // rung immediately, then let the post-change dwell prove recovery.
    const int full_window = static_cast<int>(std::max<size_t>(1, window_size));
    pressure.severe = failed > 0 || timeouts >= full_window;
    return pressure;
}

bool hasCleanAdaptiveWindow(const ARQStats& now,
                            const ARQStats& before,
                            bool tx_window_idle) {
    const bool no_error_activity =
        statDelta(now.retransmissions, before.retransmissions) == 0 &&
        statDelta(now.timeouts, before.timeouts) == 0 &&
        statDelta(now.failed, before.failed) == 0 &&
        statDelta(now.hole_events, before.hole_events) == 0;

    if (!no_error_activity) {
        return false;
    }

    // No new ARQ event while DATA is still outstanding is not a clean receive
    // window; it is an unresolved fade/ACK wait. Count clean time only when the
    // peer ACKs progress or when the transmit window is genuinely idle.
    return statDelta(now.acks_received, before.acks_received) > 0 ||
           tx_window_idle;
}

bool isFasterRate(CodeRate candidate, CodeRate current) {
    return ofdmCodeRateValue(candidate) > ofdmCodeRateValue(current);
}

bool isMoreRobustRate(CodeRate candidate, CodeRate current) {
    return ofdmCodeRateValue(candidate) < ofdmCodeRateValue(current);
}

float modeEfficiency(Modulation mod, CodeRate rate) {
    return estimateWideOFDMRawBps(mod, rate);
}

bool isFasterMode(Modulation candidate_mod, CodeRate candidate_rate,
                  Modulation current_mod, CodeRate current_rate) {
    return modeEfficiency(candidate_mod, candidate_rate) >
           modeEfficiency(current_mod, current_rate) + 0.05f;
}

int adaptiveModulationRank(Modulation mod) {
    switch (mod) {
        case Modulation::DQPSK: return 0;
        case Modulation::QPSK:  return 1;
        case Modulation::D8PSK:
        case Modulation::QAM8:  return 2;
        case Modulation::QAM16: return 3;
        case Modulation::QAM32: return 4;
        case Modulation::QAM64: return 5;
        case Modulation::QAM256: return 6;
        default: return 0;
    }
}

bool isMoreRobustMode(Modulation candidate_mod, CodeRate candidate_rate,
                      Modulation current_mod, CodeRate current_rate) {
    const float candidate_eff = modeEfficiency(candidate_mod, candidate_rate);
    const float current_eff = modeEfficiency(current_mod, current_rate);
    if (candidate_eff < current_eff - 0.05f) {
        return true;
    }
    if (candidate_eff > current_eff + 0.05f) {
        return false;
    }
    return adaptiveModulationRank(candidate_mod) <
           adaptiveModulationRank(current_mod);
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

CodeRate oneStepMoreRobust(CodeRate rate) {
    if (const auto* previous = previousOFDMRateDescriptor(rate)) {
        return previous->rate;
    }
    return rate;
}

struct AdaptiveMode {
    Modulation modulation = Modulation::DQPSK;
    CodeRate rate = CodeRate::R1_4;
};

AdaptiveMode oneStepMoreRobustMode(Modulation mod, CodeRate rate) {
    if (isFasterRate(rate, fallbackOFDMCodeRateDescriptor().rate)) {
        return {mod, oneStepMoreRobust(rate)};
    }

    switch (mod) {
        case Modulation::QAM256:
        case Modulation::QAM64:
        case Modulation::QAM32:
            return {Modulation::QAM16, rate};
        case Modulation::QAM16:
            return {mod, rate};
        case Modulation::D8PSK:
        case Modulation::QAM8:
            return {Modulation::QPSK, rate};
        case Modulation::QPSK:
            return {Modulation::DQPSK, rate};
        default:
            break;
    }

    if (isFasterRate(rate, fallbackOFDMCodeRateDescriptor().rate)) {
        return {mod, oneStepMoreRobust(rate)};
    }
    return {mod, rate};
}

AdaptiveMode oneStepFasterToward(Modulation current_mod, CodeRate current_rate,
                                  Modulation recommended_mod, CodeRate recommended_rate) {
    if (!isFasterMode(recommended_mod, recommended_rate, current_mod, current_rate)) {
        return {current_mod, current_rate};
    }

    if (const auto* next = nextOFDMRateDescriptorToward(current_rate, recommended_rate);
        next != nullptr && next->rate != current_rate) {
        return {current_mod, next->rate};
    }

    if (current_mod == Modulation::DQPSK &&
        adaptiveModulationRank(recommended_mod) >= adaptiveModulationRank(Modulation::D8PSK)) {
        return {Modulation::D8PSK, current_rate};
    }
    if (current_mod == Modulation::QPSK &&
        adaptiveModulationRank(recommended_mod) >= adaptiveModulationRank(Modulation::D8PSK)) {
        return {Modulation::D8PSK, current_rate};
    }
    if ((current_mod == Modulation::D8PSK || current_mod == Modulation::QAM8) &&
        adaptiveModulationRank(recommended_mod) >= adaptiveModulationRank(Modulation::QAM16)) {
        return {Modulation::QAM16, recommended_rate};
    }

    return {recommended_mod, recommended_rate};
}

bool canDowngradeMode(Modulation mod, CodeRate rate) {
    const AdaptiveMode target = oneStepMoreRobustMode(mod, rate);
    return target.modulation != mod || target.rate != rate;
}

bool downgradeRequiresSeverePressure(Modulation current_mod,
                                      CodeRate current_rate,
                                      const AdaptiveMode& target) {
    // D8PSK R1/2 is the differential high-throughput landing for GOOD fading.
    // Sparse ARQ pressure there is usually a faded ACK/data pocket, not enough
    // evidence to abandon a mode with 8PSK throughput for same-rate coherent
    // QPSK. Only a full-window/failed-frame event proves the rung is unusable.
    return current_mod == Modulation::D8PSK &&
           target.modulation == Modulation::QPSK &&
           target.rate == current_rate;
}

bool csiSupportsFasterSameConstellation(Modulation recommended_mod,
                                        CodeRate recommended_rate,
                                        Modulation current_mod,
                                        CodeRate current_rate) {
    return recommended_mod == current_mod &&
           isFasterRate(recommended_rate, current_rate);
}

bool isSameConstellationOrderChange(Modulation recommended_mod,
                                    Modulation current_mod) {
    return recommended_mod != current_mod &&
           getBitsPerSymbol(recommended_mod) == getBitsPerSymbol(current_mod);
}

bool csiOnlyRequestsSameOrderRegimeChange(Modulation recommended_mod,
                                          CodeRate recommended_rate,
                                          Modulation current_mod,
                                          CodeRate current_rate) {
    return isSameConstellationOrderChange(recommended_mod, current_mod) &&
           !isMoreRobustRate(recommended_rate, current_rate);
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
    ultra::gui::startupTrace("Connection", "ctor-enter");
    data_frame_cw_count_ = v2::sanitizeFixedFrameCodewords(config_.fixed_frame_codewords);
    config_.fixed_frame_codewords = data_frame_cw_count_;
    arq_.setFixedFrameCodewords(data_frame_cw_count_);

    // §14.27: burst transport is THE OFDM-wideband file path — UNCONDITIONAL, no env
    // gate (2026-06-02; the ULTRA_BURST_TRANSPORT opt-out was removed — burst is the
    // only valid file method now). `use_burst_transport_` stays initialized true; the
    // legacy windowed-file `!use_burst_transport_` branches are now dead code (R1
    // deletion follow-up). NOTE: burst is itself selective-repeat (GROUP_ACK carries
    // the 6-bit SACK frame_mask) — SelectiveRepeatARQ (`arq_`) still serves MC-DPSK/
    // narrow/control; this is NOT "remove SR-ARQ".
    // §14.36 Phase 5c: BER-driven per-block rate adaptation. Default OFF; opt in via
    // ULTRA_ADAPTIVE_RATE=1. Only meaningful on the burst transport file path.
    if (const char* ar = std::getenv("ULTRA_ADAPTIVE_RATE"); ar && ar[0] == '1') {
        adaptive_rate_enabled_ = true;
    }

    // Wire up ARQ callbacks
    arq_.setTransmitCallback([this](const Bytes& data) {
        transmitFrame(data);
    });

    arq_.setDataReceivedCallback([this](const Bytes& data) {
        handleDataPayload(data, arq_.lastRxHadMoreData(), arq_.lastRxFrameType());
    });

    arq_.setReceiveWindowAdvancedCallback([this](uint16_t base_seq, size_t window_size) {
        soft_combine_harq_.retainOnlySeqWindow(base_seq, window_size);
    });
    arq_.setTxFrameSubmittedCallback([this](uint16_t seq) {
        handleArqFrameSubmitted(seq);
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

    // §14.27: wire the one-way burst stop-and-wait controller callbacks.
    // Burst transport is the DEFAULT OFDM-wideband file path as of 2026-05-30
    // (`use_burst_transport_ = true`). NOTE: burst is itself selective-repeat (GROUP_ACK
    // carries the 6-bit SACK frame_mask; resend-failed-only + refill) — it is NOT "non-ARQ".
    // What is transitional / slated for removal is only the LEGACY wideband-file ROUTING:
    // the `!use_burst_transport_` branches that push a wideband file through the windowed
    // `arq_` instead of `burst_transport_`, plus the `ULTRA_BURST_TRANSPORT=0` opt-out.
    // The `SelectiveRepeatARQ arq_` controller stays — it still serves MC-DPSK / OFDM_NARROW
    // data and all control ACKs. See docs/REMOVAL_BACKLOG.md.
    burst_transport_.setTransmitGroup(
        [this](uint16_t group_seq, const BurstStopAndWaitController::Group& frames) {
            if (on_transmit_burst_) {
                // -> ModemEngine::transmitBurst(frames, group_seq); group_seq is
                // stamped into the descriptor so the RX whole-burst-ACKs this group.
                // Legacy tx_group_ path does not carry is_resend; the active
                // chunk-at-rate path (formAndSendBurstGroup) sets the resend anchor.
                on_transmit_burst_(frames, group_seq, /*force_full_preamble=*/false);
            }
            // §15 step 4d-late: arm the receiver-side tone-burst monitor
            // for the expected ACK. The window = burst_transport_'s
            // ack_timeout (already computed from rung timing in
            // applyAdaptiveRateFeedback / applyDataMode / startBurstFileTransfer).
            // The monitor wakes up, runs detection at a tight cadence, and
            // disarms automatically on a successful decode or window expiry.
            // Outside this window the monitor idles — no audio-thread CPU.
            if (on_arm_tone_burst_ack_monitor_) {
                const uint32_t window_ms = burst_transport_.ackTimeoutMs();
                on_arm_tone_burst_ack_monitor_(window_ms);
            }
        });
    burst_transport_.setSendGroupAck([this](uint16_t group_seq) {
        // §15 step 4d-iv: tone-burst ACK is now the SOLE GROUP_ACK transport
        // for the burst-transport file path. The OFDM 1-CW GROUP_ACK frame
        // is no longer emitted here.
        //
        // The previous parallel-emit dual-path approach (4d-iii) had an
        // ordering race: the OFDM ACK was queued first, took ~1.5 s on the
        // wire, arrived at the sender before the tone-burst, and advanced
        // burst_transport_.next_group_. The tone-burst then arrived with the
        // older seq and was silently rejected by the "seq != next_group_"
        // guard inside BurstTransport::onGroupAck. Result: tone-burst was
        // proven to detect (10/10 in smoke test) but never won the race.
        //
        // Fix: drop the OFDM ACK emit. Goodput improvement is ~1.5 s saved
        // per group ACK = ~830 ms net (after subtracting the tone-burst's
        // 675 ms airtime). Verified on the GUI: the receiver's tone-burst
        // monitor fires reliably and the protocol path consumes it via
        // Connection::onToneBurstAck (step 4d-ii).
        //
        // §14.36 quality feedback: the rate-controller signal previously
        // rode the OFDM ACK's quality byte. The tone-burst payload's 3-bit
        // rate_hint carries the same information at lower fidelity (8
        // bins vs 256). On the sender side, Connection::onToneBurstAck
        // currently maps ACK -> quality 1.0, NACK -> 0.0; consuming the
        // rate_hint as a continuous quality signal is a follow-up.
        const uint8_t quality_q = pending_ack_quality_q_;

        // A/B-comparison knob (TEMPORARY, will be removed once §15 is
        // validated multi-seed). ULTRA_LEGACY_OFDM_GROUP_ACK=1 emits the
        // pre-4d-iv OFDM 1-CW GROUP_ACK frame instead of the tone-burst.
        // Used to measure apples-to-apples goodput delta between the two
        // ACK transports at the same HEAD commit.
        const char* legacy_env = std::getenv("ULTRA_LEGACY_OFDM_GROUP_ACK");
        const bool use_legacy_ofdm_ack =
            (legacy_env && std::atoi(legacy_env) != 0);
        if (use_legacy_ofdm_ack) {
            transmitFrame(
                v2::ControlFrame::makeGroupAck(local_call_, remote_call_,
                                                group_seq, quality_q)
                    .serialize());
            return;
        }

        if (on_transmit_tone_burst_ack_) {
            ultra::waveform::tone_burst_ack::ToneBurstAckPayload tba;
            tba.group_seq = static_cast<uint8_t>(group_seq & 0x3F);
            tba.frame_mask = 0x3F;  // cumulative ACK (all frames in group OK)
            tba.type =
                ultra::waveform::tone_burst_ack::AckType::Ack;
            // Quantize the §14.36 quality byte (0..254, 255 = none) into a
            // 3-bit rate_hint (0..7). Informational only for now.
            if (quality_q == 0xFF) {
                tba.rate_hint = 0;
            } else {
                tba.rate_hint = static_cast<uint8_t>(
                    (static_cast<uint32_t>(quality_q) * 7u) / 254u);
            }
            on_transmit_tone_burst_ack_(tba);
        } else {
            // Defensive: this should never happen in production (app.cpp
            // installs the callback at startup). If it does, the sender's
            // burst_transport_.next_group_ will not advance and the file
            // transfer will stall until ack_timeout fires a resend. Log
            // loudly so the failure is obvious in the field.
            LOG_MODEM(ERROR,
                      "Connection: GROUP_ACK emit skipped (no tone-burst ACK callback installed); "
                      "file transfer will rely on ack_timeout-driven retransmits");
        }
    });
    burst_transport_.setGroupDelivered(
        [this](uint16_t /*group_seq*/, const BurstStopAndWaitController::Group& frames) {
            // Each group frame is a serialized fixed DATA frame; strip the header
            // and feed the payload to the SAME reassembly path as SR-ARQ DATA
            // frames (handleDataPayload). more_data follows MORE_FRAG; the stream
            // tail carries FINAL (no MORE_FRAG) so the reassembler finalizes.
            for (const auto& frame : frames) {
                auto df = v2::DataFrame::deserialize(frame);
                if (!df) {
                    continue;
                }
                const bool more_data = (df->flags & v2::Flags::MORE_FRAG) != 0;
                handleDataPayload(df->payload, more_data, df->type);
            }
        });
    burst_transport_.setTransferDone([this](bool success) {
        if (file_transfer_.getState() != FileTransferState::SENDING) {
            return;
        }
        if (success) {
            // startBurstFileTransfer() drained every chunk up front (so
            // hasMoreChunks() is already false). Drive the chunk-ack count to
            // chunks_sent_ so FileTransferController fires its sent-callback once
            // for the whole burst transfer and resets TX state (resetTxState()).
            while (file_transfer_.getState() == FileTransferState::SENDING &&
                   file_transfer_.hasPendingChunks()) {
                file_transfer_.onChunkAcked();
            }
        } else {
            // Link declared dead after max group retries.
            file_transfer_.onSendFailed();
        }
    });

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
    ultra::gui::startupTrace("Connection", "ctor-exit");
}

// =============================================================================
// CONFIGURATION
// =============================================================================

void Connection::setLocalCallsign(const std::string& call) {
    ultra::gui::startupTrace("Connection", "setLocalCallsign-enter");
    local_call_ = sanitizeCallsign(call);
    ultra::gui::startupTrace("Connection", "setLocalCallsign-exit");
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

    // Use centralized algorithm from waveform_selection.hpp
    recommendDataMode(measured_snr_db_, negotiated_mode_, rec_mod, rec_rate, fading_index_);

    // Bootstrap safety: chirp SNR can overestimate first OFDM frame quality.
    if (isOFDMMode(negotiated_mode_)) {
        CodeRate capped = capInitialOFDMRate(measured_snr_db_, fading_index_, rec_rate, rec_mod);
        if (capped != rec_rate) {
            LOG_MODEM(INFO, "Connection: Bootstrap cap %s -> %s for initial OFDM setup (SNR=%.1f (%s), fading=%.2f)",
                      codeRateToString(rec_rate), codeRateToString(capped), measured_snr_db_,
                      snrSourceToString(measured_snr_source_), fading_index_);
            rec_rate = capped;
        }
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
                           : CodeRate::R5_6;  // anything else = no cap
        if (cap != CodeRate::R5_6 && rec_rate > cap) {
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
              rec_mod, rec_rate, negotiated_mode_, fading_index_, measured_snr_db_);

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

    auto ack = v2::ConnectFrame::makeConnectAck(local_call_, remote_call_,
                                                 static_cast<uint8_t>(negotiated_mode_),
                                                 data_modulation_, data_code_rate_,
                                                 measured_snr_db_, fading_index_,
                                                 static_cast<uint8_t>(data_frame_cw_count_),
                                                 rung_id);
    Bytes ack_data = ack.serialize();
    connect_ack_frame_ = ack_data;
    connect_ack_retransmit_ms_ = connectAckRetransmitMs();
    connect_ack_retx_remaining_ =
        negotiated_mode_ == WaveformMode::OFDM_CHIRP ? connectAckRetxBudget() : 0;
    const uint32_t responder_handshake_failsafe_ms = responderHandshakeFailSafeMs();

    LOG_MODEM(INFO, "Connection: Sending CONNECT_ACK (%zu bytes, SNR=%.1f dB (%s))",
              ack_data.size(), measured_snr_db_, snrSourceToString(measured_snr_source_));
    transmitFrame(ack_data);

    // We are the responder - we received CONNECT and are sending CONNECT_ACK
    is_initiator_ = false;
    handshake_confirmed_ = false;  // Responder waits for first frame to confirm
    responder_handshake_wait_ms_ = responder_handshake_failsafe_ms;

    enterConnected();

    // Notify application of initial data mode
    notifyDataModeChanged(measured_snr_db_, fading_index_);
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
    resetAdaptiveModeController();

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
    LOG_MODEM(WARN, "sendFile: about to check ISS bypass use_burst=%d ofdm=%d mode=%d interactive=%d",
              use_burst_transport_ ? 1 : 0,
              isOFDMMode(negotiated_mode_) ? 1 : 0,
              static_cast<int>(negotiated_mode_),
              half_duplex_interactive_ ? 1 : 0);
    if (use_burst_transport_ && isOFDMMode(negotiated_mode_) && !half_duplex_interactive_) {
        LOG_MODEM(WARN, "sendFile: ISS-bypass taken (one-way), calling startFileTransferNow");
        return startFileTransferNow(filepath);
    }
    LOG_MODEM(WARN, "sendFile: turn-gated path (interactive or non-burst)");

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
    LOG_MODEM(WARN, "startFileTransferNow ENTER path=%s use_burst=%d ofdm=%d data_rate=%s cw=%d",
              filepath.c_str(), use_burst_transport_ ? 1 : 0,
              isOFDMMode(negotiated_mode_) ? 1 : 0,
              codeRateToString(data_code_rate_), data_frame_cw_count_);
    LOG_MODEM(WARN, "TRACE A: about to call arq_.isReadyToSend()");
    if (!arq_.isReadyToSend()) {
        LOG_MODEM(WARN, "Connection: ARQ busy, cannot start file transfer");
        return false;
    }
    LOG_MODEM(WARN, "TRACE B: arq ready, checking ofdm mode");

    // Set chunk size to match frame capacity for bounded frame geometries.
    bool is_ofdm = isOFDMMode(negotiated_mode_);
    const bool bounded_variable_mc_dpsk = usesBoundedVariableMCDPSKFrames();
    LOG_MODEM(WARN, "TRACE C: is_ofdm=%d bounded_var_mc=%d", is_ofdm ? 1 : 0, bounded_variable_mc_dpsk ? 1 : 0);
    if (is_ofdm || bounded_variable_mc_dpsk) {
        LOG_MODEM(WARN, "TRACE D: about to call currentDataPayloadCapacity()");
        size_t capacity = currentDataPayloadCapacity();
        LOG_MODEM(WARN, "TRACE E: capacity = %zu", capacity);
        if (capacity <= FileTransferController::FILE_DATA_OVERHEAD) {
            LOG_MODEM(ERROR,
                      "Connection: File chunk payload capacity %zu is too small for FILE_DATA overhead",
                      capacity);
            return false;
        }
        LOG_MODEM(WARN, "TRACE F: about to call file_transfer_.setMaxChunkPayload(%zu)", capacity);
        file_transfer_.setMaxChunkPayload(capacity);
        LOG_MODEM(WARN, "TRACE G: setMaxChunkPayload returned");
        LOG_MODEM(INFO, "Connection: File chunk payload limited to %zu bytes (%s %s, cw=%d)",
                  capacity,
                  is_ofdm ? "OFDM fixed-frame" : "MC-DPSK variable-frame",
                  codeRateToString(data_code_rate_),
                  data_frame_cw_count_);
    }

    LOG_MODEM(WARN, "TRACE H: about to call file_transfer_.startSend()");
    LOG_MODEM(INFO, "Connection: Starting file transfer: %s", filepath.c_str());

    if (!file_transfer_.startSend(filepath)) {
        LOG_MODEM(ERROR, "Connection: Failed to start file transfer");
        return false;
    }
    LOG_MODEM(WARN, "TRACE I: file_transfer_.startSend returned true");

    // §14.27: one-way burst stop-and-wait path (flag-gated, OFDM only). Drains
    // the whole file into interleaved groups and runs group-level ARQ — no
    // SR-ARQ window, no SACK. Default OFF until GUI-proven.
    if (use_burst_transport_ && is_ofdm) {
        LOG_MODEM(WARN, "TRACE J: about to call startBurstFileTransfer()");
        if (startBurstFileTransfer()) {
            LOG_MODEM(WARN, "TRACE K: startBurstFileTransfer returned true");
            return true;
        }
        LOG_MODEM(WARN, "Connection: Burst file transfer start failed; falling back to SR-ARQ");
    }

    if (is_ofdm && shouldUseSingleOFDMFileBlock(fading_index_, measured_snr_db_, data_code_rate_)) {
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
    resetAdaptiveModeController();
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
    return file_transfer_.getProgress();
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

    // Fill the ARQ window with as many chunks as possible
    // Selective Repeat ARQ can have multiple frames in flight
    while (arq_.isReadyToSend() && file_transfer_.hasMoreChunks()) {
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
    if (!use_burst_transport_) return false;

    const uint8_t tba_seq6 = detection.payload.group_seq;
    const size_t expected_next = burst_transport_.groupsAcked();
    const uint16_t expected_seq16 = static_cast<uint16_t>(expected_next);
    const uint8_t expected_seq6 =
        static_cast<uint8_t>(expected_seq16 & 0x3F);

    if (tba_seq6 != expected_seq6) {
        LOG_MODEM(DEBUG,
                  "Connection: tone-burst ACK ignored: seq6=%u != expected_seq6=%u (next_group=%u type=%s)",
                  static_cast<unsigned>(tba_seq6),
                  static_cast<unsigned>(expected_seq6),
                  static_cast<unsigned>(expected_seq16),
                  detection.payload.type ==
                          ultra::waveform::tone_burst_ack::AckType::Nack
                      ? "NACK"
                      : "ACK");
        return false;
    }

    const bool is_nack = (detection.payload.type ==
                          ultra::waveform::tone_burst_ack::AckType::Nack);
    LOG_MODEM(INFO,
              "Connection: tone-burst %s matched group_seq=%u "
              "(frame_mask=0x%02X rate_hint=%u peak=%.1f symbol_ms=%u)",
              is_nack ? "NACK" : "ACK",
              static_cast<unsigned>(expected_seq16),
              static_cast<unsigned>(detection.payload.frame_mask),
              static_cast<unsigned>(detection.payload.rate_hint),
              detection.correlation_peak,
              static_cast<unsigned>(detection.symbol_ms_used));

    // §SR-ARQ: interpret the per-frame mask. Re-queue the 0-bit REAL frames (skip
    // pads) of the in-flight burst for resend, then advance + refill if ANY real
    // frame was delivered. A fully-dead burst (0 real acked) is a NACK so the
    // controller's max_retries liveness still declares a dead link.
    if (burst_interleave_off_) {
        const uint8_t mask = detection.payload.frame_mask;
        int real_total = 0, real_acked = 0;
        for (size_t i = 0; i < burst_inflight_frames_.size(); ++i) {
            const bool pad = (i < burst_inflight_is_pad_.size()) && burst_inflight_is_pad_[i];
            if (pad) continue;
            ++real_total;
            const bool acked = (i < 8) && (((mask >> i) & 1u) != 0);
            if (acked) {
                ++real_acked;
            } else {
                burst_resend_frames_.push_back(burst_inflight_frames_[i]);
            }
        }
        LOG_MODEM(INFO,
                  "Connection: SR-ARQ ACK seq=%u mask=0x%02X real_acked=%d/%d resend_queue=%zu",
                  static_cast<unsigned>(expected_seq16), static_cast<unsigned>(mask),
                  real_acked, real_total, burst_resend_frames_.size());
        if (real_acked > 0) {
            applyAdaptiveRateFeedback(1.0f);
            burst_transport_.onGroupAck(expected_seq16);   // advance -> form next (resends+new)
        } else {
            applyAdaptiveRateFeedback(0.0f);
            burst_transport_.onGroupNack(expected_seq16);  // retry same burst (liveness)
        }
        return true;
    }

    if (is_nack) {
        applyAdaptiveRateFeedback(0.0f);
        burst_transport_.onGroupNack(expected_seq16);
    } else {
        applyAdaptiveRateFeedback(1.0f);
        burst_transport_.onGroupAck(expected_seq16);
    }
    return true;
}

bool Connection::startBurstFileTransfer() {
    // §14.27: drain the whole file into serialized fixed DATA frames, slice into
    // BURST_GROUP_SIZE-frame interleaved groups, and hand them to the group
    // stop-and-wait controller. No SR-ARQ window, no per-frame SACK — one group
    // is the ARQ unit (whole-burst resend on group-ACK timeout).
    LOG_MODEM(WARN, "TRACE BF1: startBurstFileTransfer ENTER");
    if (!on_transmit_burst_) {
        LOG_MODEM(ERROR, "Connection: Burst file transfer requires the burst callback");
        return false;
    }

    // 2026-05-28: when the long-LDPC z=81 opt-in is active, the encoder will use
    // cw_per_frame=2 (the architectural coupling: each frame carries 2x1944 coded
    // bits = ~75% of the legacy 8x648 frame). The chunk sizer downstream
    // (file_transfer_.getNextChunk via data_frame_cw_count_) MUST match so the
    // frames it produces are sized for the encoder's CW count — otherwise the
    // LDPC-encoded bytes don't match BurstInterleaver's expected B = cw*bytes_per_cw
    // and the interleaver throws "frame size mismatch". Push cw=2 down to the
    // connection layer here so getFixedFramePayloadCapacity returns the right
    // value before the file is drained into chunks.
    if (isOFDMMode(negotiated_mode_)) {
        if (selectBurstLiftingZ() == 81) {
            if (data_frame_cw_count_ != 2) {
                LOG_MODEM(WARN, "Connection: z=81 opt-in active -> coercing data_frame_cw_count %d -> 2 for burst transfer",
                          data_frame_cw_count_);
                setForcedFrameCodewords(2, /*forced=*/false);
            }
            // 2026-05-28: REFRESH the file_transfer chunker's max payload at the
            // z=81 capacity (which currentDataPayloadCapacity now returns when
            // ULTRA_LDPC_Z=81 is set). Without this the chunker keeps the value
            // it was given at handshake (Z=27 sized) and zero-pads 70% of every
            // burst. setForcedFrameCodewords above also calls this internally,
            // but only when it actually changes cw_count — the resize-only case
            // wouldn't trigger it, so refresh unconditionally here.
            const size_t z81_capacity = currentDataPayloadCapacity();
            file_transfer_.setMaxChunkPayload(z81_capacity);
            LOG_MODEM(WARN, "Connection: z=81 chunker refresh -> setMaxChunkPayload(%zu B/frame) "
                            "(was using Z=27 capacity ~%zu)",
                      z81_capacity,
                      v2::getFixedFramePayloadCapacity(data_code_rate_, data_frame_cw_count_));
        }
    }
    LOG_MODEM(WARN, "TRACE BF2: on_transmit_burst_ is set");
    const size_t group_size = connection_policy::burstInterleaveGroupFrames();
    LOG_MODEM(WARN, "TRACE BF3: group_size=%zu", group_size);

    // Drain all chunk payloads first (so we can mark FINAL on the last real
    // frame and know the total chunk count for completion accounting).
    std::vector<Bytes> chunk_payloads;
    size_t total_payload_bytes = 0;
    int chunk_iter = 0;
    while (file_transfer_.hasMoreChunks()) {
        chunk_iter++;
        if (chunk_iter <= 3 || chunk_iter % 20 == 0) {
            LOG_MODEM(WARN, "TRACE BF4: iter=%d about to getNextChunk()", chunk_iter);
        }
        Bytes chunk = file_transfer_.getNextChunk();
        if (chunk_iter <= 3 || chunk_iter % 20 == 0) {
            LOG_MODEM(WARN, "TRACE BF5: iter=%d got chunk size=%zu", chunk_iter, chunk.size());
        }
        if (chunk.empty()) {
            break;
        }
        total_payload_bytes += chunk.size();
        chunk_payloads.push_back(std::move(chunk));
    }
    LOG_MODEM(WARN, "TRACE BF6: drained %zu chunks, total=%zu B (iter=%d)",
              chunk_payloads.size(), total_payload_bytes, chunk_iter);
    if (chunk_payloads.empty()) {
        LOG_MODEM(WARN, "Connection: Burst file transfer produced no chunks");
        return false;
    }

    // §14.36 chunk-at-rate path: if adaptive rate is on, do NOT pre-frame at the
    // start rate. Strip the 5 B TYPE+OFFSET headers from each drained chunk to
    // recover the raw file payload bytes, hand them to formAndSendBurstGroup which
    // re-chunks at the CURRENT rate on every (re)send. Required for adaptation
    // because a mid-transfer rate change would otherwise overflow the in-flight
    // group's R3/4-sized frames into a smaller (R1/2 / R1/4) frame capacity.
    LOG_MODEM(WARN, "TRACE BF7: about to enter adaptive_rate path, adaptive_rate_enabled_=%d",
              adaptive_rate_enabled_ ? 1 : 0);
    if (adaptive_rate_enabled_) {
        LOG_MODEM(WARN, "TRACE BF8: in adaptive path, clearing burst_file_payload_");
        // The first chunk(s) from file_transfer_ are FILE_START (metadata: TYPE +
        // FLAGS + SIZE + CRC32 + NAME — a DIFFERENT header from FILE_DATA's
        // TYPE+OFFSET). They must arrive intact so the receiver enters RECEIVING
        // state; only THEN does processFileData store FILE_DATA bytes. Strip-5-
        // bytes works for FILE_DATA only — for FILE_START we send the chunk as-is
        // via a normal DATA frame before the burst transport starts. burst_file_
        // payload_ holds only file-body bytes (FILE_DATA data portions).
        burst_file_payload_.clear();
        burst_file_payload_.reserve(total_payload_bytes);
        std::vector<Bytes> metadata_chunks;
        for (const auto& chunk : chunk_payloads) {
            if (chunk.empty()) continue;
            const uint8_t ptype = chunk[0];
            if (ptype == static_cast<uint8_t>(PayloadType::FILE_DATA) && chunk.size() > 5) {
                burst_file_payload_.insert(burst_file_payload_.end(),
                                           chunk.begin() + 5, chunk.end());
            } else {
                // FILE_START / FILE_BLOCK / etc. — keep as-is to send before the burst.
                metadata_chunks.push_back(chunk);
            }
        }

        // Hand the metadata chunks to the form fn so they ride the FIRST burst
        // group's frames (one chunk per frame slot) on the SAME interleaved path
        // as file data. Sending them as separate non-burst frames RACED the
        // burst descriptor in the audio queue and lost — embedding them in the
        // burst itself is deterministic and reuses the descriptor-driven decode.
        burst_metadata_queue_.assign(metadata_chunks.begin(), metadata_chunks.end());
        burst_pending_metadata_consumed_ = 0;
        burst_file_cursor_ = 0;
        burst_pending_advance_ = 0;
        burst_chunk_seq_ = 0;
        // §SR-ARQ profile: interleave OFF -> per-frame Selective-Repeat (the default now,
        // for ALL modulations). ONE source of truth shared with the encoder + the on-wire
        // descriptor bit (connection_policy::burstCrossFrameInterleaveOn) so the TX ARQ
        // semantics, the encoder byte-interleave, and the descriptor bi bit can never
        // disagree — the QAM16 offset-skip bug was exactly that disagreement.
        burst_interleave_off_ = !connection_policy::burstCrossFrameInterleaveOn();
        burst_resend_frames_.clear();
        burst_inflight_frames_.clear();
        burst_inflight_is_pad_.clear();
        burst_transport_.setFormAndSendGroup(
            [this](uint16_t group_seq, bool is_resend) {
                // SR form drains the resend queue + refills; on a TIMEOUT resend
                // (is_resend with an empty resend queue — no ACK came back to populate
                // it) it must re-queue the un-acked in-flight burst instead of advancing
                // the cursor. The whole-group form re-creates the group from the cursor.
                return burst_interleave_off_ ? formAndSendBurstGroupSR(group_seq, is_resend)
                                             : formAndSendBurstGroup(group_seq, is_resend);
            });
        // 2026-05-28: previously the timeout handler called
        // applyAdaptiveRateFeedback(0.0f) — "treat silence as quality=0 so the
        // controller steps down before the timeout-driven resend". In practice
        // this fires false alarms on Good@20: the ACK is actually arriving ~2-4s
        // AFTER the timer (e.g. ACK at 42.3s vs timeout at 38.8s) but bravo did
        // decode the group cleanly (quality=0.99). One false alarm per group
        // drops the rate R3/4 → R2/3 → R1/2 → R1/4 over three groups even though
        // every group is delivered, and the lower rates compound the problem
        // (slower TX → ACK arrives even later → another false-alarm drop).
        //
        // Now we DO NOT step the rate down on timeout. Resend stays at the same
        // rate; if the ACK never arrives the burst_transport's max_retries gate
        // still declares the link dead. If the ACK is just late, the next group
        // proceeds at the original rate and the rate-up logic in
        // applyAdaptiveRateFeedback (on actual decoded quality) handles climbs.
        // No callback wired → BurstStopAndWaitController skips the feedback.
        // §14.36 group-aware ACK timeout. 2026-05-28 corrected: the previous
        // linear scaling `base * group_size / 8` (from SR-ARQ window=8) was
        // wrong because it shrunk the FIXED overheads (ack TX time, decode
        // jitter margin, audio chain RTT) proportionally to the burst size.
        // Those don't shrink. With group=6 vs window=8, a base of 12464ms
        // scaled to 9348ms — ~3s short of the real round-trip — and alpha
        // was resending every group BEFORE bravo's GROUP_ACK could arrive,
        // causing the 17-per-transfer dup-delivery cycle observed across all
        // seeds. Recompute correctly: call the same airtime-derived formula
        // with window=group_size so tx_burst_ms uses the actual burst length
        // and the fixed overheads stay fixed.
        const size_t group_size = connection_policy::burstInterleaveGroupFrames();
        const uint32_t base_timeout = arq_.getAckTimeout();
        // Include the group-start chirp+LTS re-anchor airtime (~1.4s) that
        // alpha emits at the start of each burst — without it, the formula
        // under-estimates by exactly that amount. Plus a 2-second safety
        // margin to cover real-world fade slowdowns (bravo's LTS decode
        // may take an extra symbol or two on the bad-fade groups).
        const uint32_t continuation_reanchor_ms =
            connection_policy::wideOFDMShortReanchorChirpDurationMs();
        const uint32_t burst_timeout = connection_policy::computeWideOFDMAckTimeoutMs(
            data_modulation_, data_code_rate_, group_size,
            arq_.getSackDelay(), arq_.getAckRepeatCount(),
            data_frame_cw_count_,
            continuation_reanchor_ms);
        // Add ~2s safety margin — empirical fade-luck shows GROUP_ACK can
        // arrive 1-1.5s after a clean formula-derived timeout, esp. on
        // marginal-SNR groups. Better to over-wait than to fire a wasted
        // resend that doubles airtime per group.
        const uint32_t timeout_ms = std::max<uint32_t>(burst_timeout + 2000u, 14000u);
        burst_transport_.setAckTimeoutMs(timeout_ms);
        LOG_MODEM(WARN, "TRACE BF9: setAckTimeoutMs(%u) returned, calling startTransfer", timeout_ms);
        LOG_MODEM(INFO,
                  "Connection: Burst file transfer (chunk-at-rate): %zu metadata chunks + "
                  "%zu B raw payload, ack_timeout=%ums (scaled from %ums for group=%zu), "
                  "start_rate=%s",
                  metadata_chunks.size(), burst_file_payload_.size(),
                  burst_transport_.ackTimeoutMs(), base_timeout, group_size,
                  codeRateToString(data_code_rate_));
        noteDataTurnPayloadStarted(total_payload_bytes);
        burst_transport_.startTransfer();
        LOG_MODEM(WARN, "TRACE BF10: startTransfer returned, returning true");
        return true;
    }

    // Wrap each payload into a serialized fixed DATA frame (same construction the
    // SR-ARQ path uses, minus the window bookkeeping). FINAL marks the stream tail.
    std::vector<Bytes> frames;
    frames.reserve(chunk_payloads.size());
    uint16_t seq = 0;
    for (size_t i = 0; i < chunk_payloads.size(); ++i) {
        const bool is_last = (i + 1 == chunk_payloads.size());
        auto frame = v2::makeFixedDataFrame(local_call_, remote_call_, seq++,
                                            chunk_payloads[i], data_code_rate_,
                                            data_frame_cw_count_, selectBurstLiftingZ());
        frame.type = v2::FrameType::DATA;
        frame.flags = is_last ? v2::Flags::FINAL : v2::Flags::MORE_FRAG;
        frames.push_back(frame.serialize());
    }

    // The encoder only interleaves + emits a descriptor for FULL groups
    // (encoded_frames.size() / BURST_GROUP_SIZE). Pad the final partial group to
    // group_size so it forms a full interleaved burst. Pad frames are addressed
    // to kOFDMBurstPadCallsign, which the RX address filter drops before
    // reassembly — so they cost airtime but never corrupt the file.
    if (frames.size() % group_size != 0) {
        const size_t pad = group_size - (frames.size() % group_size);
        for (size_t i = 0; i < pad; ++i) {
            frames.push_back(v2::makeFixedDataFrame(
                local_call_, kOFDMBurstPadCallsign,
                static_cast<uint16_t>(kOFDMBurstPadSeq - i),
                makeOFDMBurstPadPayload(data_code_rate_, data_frame_cw_count_, i),
                data_code_rate_, data_frame_cw_count_).serialize());
        }
    }

    // Slice into groups of exactly group_size frames.
    std::vector<BurstStopAndWaitController::Group> groups;
    groups.reserve(frames.size() / group_size);
    for (size_t i = 0; i < frames.size(); i += group_size) {
        groups.emplace_back(frames.begin() + i, frames.begin() + i + group_size);
    }

    LOG_MODEM(INFO,
              "Connection: Burst file transfer: %zu chunks (%zu B) -> %zu groups x %zu frames "
              "(cw=%d, rate=%s)",
              chunk_payloads.size(), total_payload_bytes, groups.size(), group_size,
              data_frame_cw_count_, codeRateToString(data_code_rate_));

    // Match the group-ACK timeout to the SR-ARQ window=8 burst timeout: one group
    // IS that 8-frame burst, so the same burst-airtime + turnaround + ACK budget
    // applies. The hardcoded 14 s default is shorter than a single QPSK R3/4 burst
    // (~11 s) + turnaround, collapsing the listen window so the sender resends
    // before the GROUP_ACK can land.
    burst_transport_.setAckTimeoutMs(arq_.getAckTimeout());

    LOG_MODEM(INFO, "Connection: Burst group-ACK timeout=%ums (from SR-ARQ burst budget)",
              burst_transport_.ackTimeoutMs());

    noteDataTurnPayloadStarted(total_payload_bytes);
    burst_transport_.startTransfer(std::move(groups));
    return true;
}

bool Connection::formAndSendBurstGroup(uint16_t group_seq, bool is_resend) {
    // §14.36 chunk-at-rate: build the in-flight burst group's frames at the CURRENT
    // data_code_rate_ from the raw file payload + cursor. On a NEW group (is_resend=
    // false) the cursor advances by the previous group's consumed bytes; on a resend
    // we re-form from the SAME cursor at whatever rate the controller picked since
    // (so a dropped rate produces correctly-sized smaller frames).
    if (!on_transmit_burst_) return false;
    if (!is_resend) {
        // Commit the previous group's progress: file cursor + any metadata chunks
        // we drained in the last form. (Resends re-form from the same state, so
        // these advances are skipped — the new rate may consume different bytes.)
        burst_file_cursor_ += burst_pending_advance_;
        burst_pending_advance_ = 0;
        for (size_t i = 0; i < burst_pending_metadata_consumed_ &&
                            !burst_metadata_queue_.empty(); ++i) {
            burst_metadata_queue_.pop_front();
        }
        burst_pending_metadata_consumed_ = 0;
    }
    if (burst_metadata_queue_.empty() &&
        burst_file_cursor_ >= burst_file_payload_.size()) {
        return false;  // file fully drained -> transport will mark done(success)
    }

    const size_t group_size = connection_policy::burstInterleaveGroupFrames();
    // Z-aware capacity. At z=81 each codeword carries 3x as many info bytes as at
    // z=27 (R3/4 cw=2: 96 B legacy -> 345 B at z=81). The burst chunker must size
    // frames at the active z (selectBurstLiftingZ) or the encoder pads 70% zeros.
    const int active_z = selectBurstLiftingZ();
    const size_t frame_cap = (active_z == 81)
        ? v2::getFixedFramePayloadCapacityZ(data_code_rate_, data_frame_cw_count_, 81)
        : v2::getFixedFramePayloadCapacity(data_code_rate_, data_frame_cw_count_);
    constexpr size_t kFileDataOverhead = 5;  // TYPE(1) + OFFSET(4)
    if (frame_cap <= kFileDataOverhead) return false;
    const size_t per_frame_data = frame_cap - kFileDataOverhead;

    BurstStopAndWaitController::Group frames;
    frames.reserve(group_size);
    size_t consumed = 0;
    size_t md_taken = 0;  // metadata chunks consumed in THIS form attempt
    for (size_t i = 0; i < group_size; ++i) {
        // 1) Metadata chunks (FILE_START / FILE_BLOCK) ride the first frame slots
        //    of the first burst group, as-is — the receiver's handleDataPayload
        //    routes them via file_transfer_.processPayload which enters RECEIVING
        //    before any FILE_DATA arrives. Same path the legacy pre-chunked file
        //    transfer used; reusing it keeps the receiver code unchanged.
        if (md_taken < burst_metadata_queue_.size()) {
            const Bytes& chunk = burst_metadata_queue_[md_taken];
            const uint16_t seq = burst_chunk_seq_++;
            auto frame = v2::makeFixedDataFrame(local_call_, remote_call_, seq, chunk,
                                                data_code_rate_, data_frame_cw_count_,
                                                selectBurstLiftingZ());
            frame.type = v2::FrameType::DATA;
            frame.flags = v2::Flags::MORE_FRAG;  // metadata is never the file tail
            frames.push_back(frame.serialize());
            ++md_taken;
            continue;
        }
        const size_t offset_in_file = burst_file_cursor_ + consumed;
        const size_t total = burst_file_payload_.size();
        if (offset_in_file >= total) {
            // File drained mid-group — pad the rest of the group.
            frames.push_back(v2::makeFixedDataFrame(
                local_call_, kOFDMBurstPadCallsign,
                static_cast<uint16_t>(kOFDMBurstPadSeq - i),
                makeOFDMBurstPadPayload(data_code_rate_, data_frame_cw_count_, i),
                data_code_rate_, data_frame_cw_count_).serialize());
            continue;
        }
        const size_t this_data = std::min(per_frame_data, total - offset_in_file);
        Bytes chunk;
        chunk.reserve(kFileDataOverhead + this_data);
        chunk.push_back(static_cast<uint8_t>(PayloadType::FILE_DATA));
        const uint32_t off32 = static_cast<uint32_t>(offset_in_file);
        chunk.push_back(static_cast<uint8_t>((off32 >> 24) & 0xFF));
        chunk.push_back(static_cast<uint8_t>((off32 >> 16) & 0xFF));
        chunk.push_back(static_cast<uint8_t>((off32 >> 8) & 0xFF));
        chunk.push_back(static_cast<uint8_t>(off32 & 0xFF));
        chunk.insert(chunk.end(),
                     burst_file_payload_.begin() + offset_in_file,
                     burst_file_payload_.begin() + offset_in_file + this_data);
        const uint16_t seq = burst_chunk_seq_++;
        auto frame = v2::makeFixedDataFrame(local_call_, remote_call_, seq, chunk,
                                            data_code_rate_, data_frame_cw_count_,
                                            selectBurstLiftingZ());
        frame.type = v2::FrameType::DATA;
        const bool finishes_file = (offset_in_file + this_data >= total);
        frame.flags = finishes_file ? v2::Flags::FINAL : v2::Flags::MORE_FRAG;
        frames.push_back(frame.serialize());
        consumed += this_data;
    }
    // 2026-05-28: KEEP pending_advance from the FIRST form for this group.
    // Resends at a lower rate cover FEWER bytes than the original (e.g. R3/4
    // form: 1700 B; R2/3 resend: 1500 B). If the original form's burst is what
    // bravo actually delivered (the common case — first attempt usually wins
    // and the ACK chasing comes later), advancing by the resend's smaller
    // value leaves the alpha cursor 200 B behind what bravo wrote. From then
    // on alpha emits chunks at offsets before bravo's expected cursor, bravo
    // buffers them as out-of-order forever, and the file never assembles.
    //
    // By preserving the first form's `consumed` we advance correctly when the
    // first attempt delivered. The rare case where a later RESEND is what
    // delivered (original lost) is handled later by the receiver buffering
    // the extra bytes; bravo's offset-keyed assembler can write them either
    // way. The right long-term fix is to put the byte range in BURST_HEADER
    // so bravo can self-report exact coverage on GROUP_ACK.
    if (!is_resend) {
        burst_pending_advance_ = consumed;
    }
    burst_pending_metadata_consumed_ = md_taken;
    LOG_MODEM(INFO,
              "Connection: form group_seq=%u rate=%s capacity=%zuB/frame consumed=%zuB"
              " cursor=%zu/%zu pending_advance=%zu%s",
              group_seq, codeRateToString(data_code_rate_), per_frame_data, consumed,
              burst_file_cursor_, burst_file_payload_.size(),
              burst_pending_advance_,
              is_resend ? " [RESEND]" : "");
    // §16.4 escalation: a RESEND means the previous attempt did not deliver —
    // pay for a full chirp+LTS group-start anchor so bravo re-acquires
    // deterministically even when warm-sync is enabled. First attempts stay
    // light (warm-handoff goodput); only retries carry the anchor. When the
    // warm-handoff knob is OFF the group-start is already full, so this is a
    // no-op there.
    on_transmit_burst_(frames, group_seq, /*force_full_preamble=*/is_resend);
    // §15 step 4d-late: arm the receiver-side tone-burst monitor for the
    // ACK that should arrive after this group's airtime + T/R + ACK airtime.
    // Path covers BOTH burst_transport_.setTransmitGroup (legacy tx_group_)
    // and the §14.36 chunk-at-rate form_send_ path that fires
    // formAndSendBurstGroup directly. Arming HERE — at TX dispatch — gives
    // the monitor the full burst-airtime window to be in armed state when
    // BRAVO's tone-burst lands. During ALPHA's TX, its own RX is muted by
    // OTASim's mixer (own-TX excluded), so the cadence runs on silence and
    // costs ~zero CPU until ACK arrival.
    if (on_arm_tone_burst_ack_monitor_) {
        const uint32_t window_ms = burst_transport_.ackTimeoutMs();
        on_arm_tone_burst_ack_monitor_(window_ms);
    }
    return true;
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
    // 2026-05-28: ULTRA_LOCK_RATE=1 holds the data rate fixed for the whole
    // transfer. Used to validate the end-to-end burst transport path without
    // the rate ladder muddying the diagnosis. The chunk-at-rate path stays on
    // (it just re-chunks at the SAME rate on resend).
    if (const char* lock = std::getenv("ULTRA_LOCK_RATE"); lock && std::atoi(lock) != 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "lock %s (q=%.2f)",
                      codeRateToString(data_code_rate_), quality);
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
                           : CodeRate::R5_6;
        if (cap != CodeRate::R5_6 && next > cap) {
            next = cap;
        }
    }
    char buf[96];
    if (next != prev) {
        data_code_rate_ = next;
        // 2026-05-28: recompute burst ack_timeout for the new rate. Lower
        // rates take longer per frame; if we leave the old (faster-rate)
        // timeout in place, alpha will fire premature resends every group
        // and waste airtime on dup-deliveries (same bug the initial
        // 9348ms→14000ms fix solved at startup, but stickily across rate
        // changes too).
        if (use_burst_transport_) {
            const size_t group_size = connection_policy::burstInterleaveGroupFrames();
            const uint32_t continuation_reanchor_ms =
                connection_policy::wideOFDMShortReanchorChirpDurationMs();
            const uint32_t burst_timeout = connection_policy::computeWideOFDMAckTimeoutMs(
                data_modulation_, data_code_rate_, group_size,
                arq_.getSackDelay(), arq_.getAckRepeatCount(),
                data_frame_cw_count_, continuation_reanchor_ms);
            const uint32_t timeout_ms = std::max<uint32_t>(burst_timeout + 2000u, 14000u);
            burst_transport_.setAckTimeoutMs(timeout_ms);
            LOG_MODEM(INFO,
                      "Connection: ack_timeout recomputed for rate change: %ums "
                      "(rate=%s, group_size=%zu)",
                      timeout_ms, codeRateToString(data_code_rate_), group_size);
        }
        std::snprintf(buf, sizeof(buf), "rate %s -> %s (q=%.2f)",
                      codeRateToString(prev), codeRateToString(next), quality);
        LOG_MODEM(INFO, "Connection: adaptive %s", buf);
        notifyDataModeChanged(measured_snr_db_, fading_index_);
    } else {
        std::snprintf(buf, sizeof(buf), "hold %s (q=%.2f)", codeRateToString(prev), quality);
    }
    last_adaptive_action_ = buf;
}

void Connection::onBurstGroupReceived(uint16_t group_seq, const std::vector<Bytes>& frames,
                                      bool all_ok, float quality, uint8_t frame_mask,
                                      bool interleaved) {
    if (!use_burst_transport_) {
        return;
    }

    // Live "incoming burst" status for the GUI. Updated for EVERY group (SR or
    // interleaved), so the operator sees the modem working — group # advancing, X/Y
    // frames decoding — even before FILE_START establishes the file. frames.size() is
    // the group size (failed slots are present but undeserializable); frame_mask is the
    // decoded bitmap.
    {
        unsigned decoded = 0;
        for (uint8_t m = frame_mask; m; m &= (m - 1)) ++decoded;  // popcount (X)
        // Group size (Y) = bit-length of the decode mask: the decoder builds an N-bit
        // mask for the N-frame group (failed frames are 0-bits), so the highest slot
        // gives N. frames.size() is decoded-only (== X), so NOT the total. Robust for
        // full and partial-last groups; only cosmetically low if a group's *trailing*
        // frame fails (rare). For frame 0 (FILE_START) missing, mask=0x3E -> Y=6.
        unsigned group_bits = 0;
        for (uint8_t m = frame_mask; m; m >>= 1) ++group_bits;
        burst_activity_.active = true;
        burst_activity_.group_seq = group_seq;
        burst_activity_.frames_decoded = static_cast<uint8_t>(decoded);
        burst_activity_.frames_in_group =
            static_cast<uint8_t>(group_bits > decoded ? group_bits : decoded);
        ++burst_activity_.groups_seen;
    }

    // §14.36 Phase 5c: stash the decode headroom so the GROUP_ACK for this group
    // carries it back to the sender (which runs the rate controller). Quantize
    // [0,1] -> 0..254; 0xFF means "no feedback" when adaptation is off.
    last_group_quality_ = quality;
    pending_ack_quality_q_ =
        adaptive_rate_enabled_
            ? static_cast<uint8_t>(std::lround(std::clamp(quality, 0.0f, 1.0f) * 254.0f))
            : 0xFF;

    // §SR-ARQ (channel-adaptive): an interleave-OFF group is a set of INDEPENDENTLY
    // decodable frames. Deliver every frame that decoded (the offset-keyed assembler
    // handles reorder + dedup) and ACK the TRUE per-frame frame_mask so the sender
    // resends only the 0-bit frames + refills — no whole-group resend on a partial fade.
    if (!interleaved) {
        onBurstGroupReceivedSR(group_seq, frames, frame_mask);
        return;
    }
    // A partial group cannot be deinterleaved/reassembled, so it is dropped and
    // the sender whole-burst-resends (stop-and-wait, no SACK). Only a fully
    // decoded group is ACKed + delivered.
    if (!all_ok) {
        // §14.30 fast-NACK: the descriptor decoded (so group_seq is known) but the
        // interleaved group failed (0/8) — usually a deep fade across the burst.
        // Tell the sender to resend NOW instead of letting it wait out the ~27 s
        // group-ACK timeout (which on Good fading costs ~54 s over two blind
        // cycles). Only NACK the group we are actually still waiting for, so a
        // failed duplicate of an already-delivered group is not NACKed.
        if (group_seq == burst_transport_.rxExpectedGroupSeq()) {
            // 2026-05-29 fix: emit the NACK over the §15 TONE-BURST path, same as
            // the GROUP_ACK. The old transmitFrame(makeGroupNack) sent a 20-byte
            // control frame over the CURRENT waveform — and on group 0
            // handshake_complete_ is still false (it is set later, in the all_ok
            // path this NACK branch returns before reaching), so the frame went
            // out as the MC-DPSK HANDSHAKE waveform: a ~3.1 s MC-DPSK burst on the
            // air (the "weird MC-DPSK signal" on the waterfall), ~4.6× slower than
            // the 675 ms tone-burst and bypassing §15 entirely. The sender's
            // onToneBurstAck already maps a NACK-type tone-burst to
            // burst_transport_.onGroupNack (resend now), so the whole receive path
            // already exists — only the emit was wrong.
            if (on_transmit_tone_burst_ack_) {
                LOG_MODEM(INFO,
                          "Connection: Burst group_seq=%u failed (0/8); sending tone-burst GROUP_NACK (resend now)",
                          group_seq);
                ultra::waveform::tone_burst_ack::ToneBurstAckPayload tba;
                tba.group_seq = static_cast<uint8_t>(group_seq & 0x3F);
                tba.frame_mask = 0x00;  // whole group missing (0/8); sender whole-burst-resends
                tba.type = ultra::waveform::tone_burst_ack::AckType::Nack;
                tba.rate_hint = 0;      // quality 0 (group failed)
                on_transmit_tone_burst_ack_(tba);
            } else {
                LOG_MODEM(INFO,
                          "Connection: Burst group_seq=%u failed (0/8); sending GROUP_NACK (resend now)",
                          group_seq);
                transmitFrame(
                    v2::ControlFrame::makeGroupNack(local_call_, remote_call_, group_seq).serialize());
            }
        } else {
            // BUG-FINACK-001: this is a resend of an ALREADY-DELIVERED group — our
            // prior GROUP_ACK was fade-lost, so the sender is stuck resending the
            // final group forever and the transfer never cleanly closes (esp. at
            // low SNR / heavy fading, where the final ACK is frequently nulled).
            // Re-ACK it DECODE-INDEPENDENTLY: route it into the controller's existing
            // duplicate path (onGroupReceived → seqLess → re-emit GROUP_ACK, no
            // re-delivery), which does NOT need the (failed) data frames. Previously
            // this branch just dropped the duplicate, so the re-ACK never fired.
            LOG_MODEM(INFO,
                      "Connection: Burst group_seq=%u failed but already delivered "
                      "(expected %u); re-ACKing decode-independently (FINACK close)",
                      group_seq, burst_transport_.rxExpectedGroupSeq());
            burst_transport_.onGroupReceived(group_seq, {});
        }
        return;
    }
    // Drop pad frames (addressed to the burst-pad callsign): keep only frames
    // addressed to us for reassembly. Pads exist only to fill a partial group so
    // the encoder forms a full interleaved burst.
    std::vector<Bytes> real_frames;
    real_frames.reserve(frames.size());
    for (const auto& frame : frames) {
        auto hdr = v2::parseHeader(frame);
        if (hdr.valid && !v2::isAddressedToCallsign(hdr, local_call_)) {
            continue;
        }
        real_frames.push_back(frame);
    }
    // Responder handshake confirmation: the burst group is the first valid
    // connected frame the responder decodes from the initiator. The normal
    // per-frame RX path (onFrameReceived) confirms the handshake here, but the
    // burst group-as-unit path bypasses it — so confirm it now, BEFORE the
    // GROUP_ACK is emitted. Without this, handshake_complete_ stays false and the
    // GROUP_ACK is transmitted with the handshake (MC-DPSK) waveform, which the
    // OFDM-mode initiator cannot decode → the sender never advances past group 0.
    if (state_ == ConnectionState::CONNECTED && !is_initiator_ && !handshake_confirmed_) {
        LOG_MODEM(INFO, "Connection: Handshake confirmed (first burst group from initiator)");
        handshake_confirmed_ = true;
        responder_handshake_wait_ms_ = 0;
        if (on_handshake_confirmed_) {
            on_handshake_confirmed_();
        }
    }
    // Disarm the CONNECT_ACK rescue: a decoded burst group proves the initiator
    // got our CONNECT_ACK and moved to data. onFrameReceived does this on the first
    // decoded frame, but the burst group-as-unit path bypasses it — so the rescue
    // stayed armed and bravo kept blasting an 8.3 s MC-DPSK CONNECT_ACK every ~17 s,
    // COLLIDING with the initiator's in-flight group bursts (half-duplex violation:
    // both stations transmitting at once). That collision corrupts the initiator's
    // GROUP_ACK reception and wastes airtime — the dominant cause of the group-0 ACK
    // latency. Mirrors onFrameReceived's rescue clear.
    if (state_ == ConnectionState::CONNECTED && !is_initiator_ &&
        !connect_ack_frame_.empty()) {
        LOG_MODEM(INFO, "Connection: CONNECT_ACK rescue disarmed (burst group decoded)");
        connect_ack_frame_.clear();
        connect_ack_retx_remaining_ = 0;
    }

    LOG_MODEM(INFO, "Connection: Burst group_seq=%u complete: %zu real frames -> deliver+GROUP_ACK",
              group_seq, real_frames.size());
    // Controller delivers once + emits one GROUP_ACK (dedup re-ACKs a duplicate
    // group whose prior ACK was lost without re-delivering).
    burst_transport_.onGroupReceived(group_seq, real_frames);
}

void Connection::onBurstGroupReceivedSR(uint16_t group_seq,
                                        const std::vector<Bytes>& frames,
                                        uint8_t frame_mask) {
    // Responder handshake confirmation + CONNECT_ACK rescue disarm — the burst
    // group-as-unit path bypasses onFrameReceived, so mirror it here (identical to
    // the whole-group all_ok path) on the first decoded burst.
    if (state_ == ConnectionState::CONNECTED && !is_initiator_ && !handshake_confirmed_) {
        LOG_MODEM(INFO, "Connection: Handshake confirmed (first SR burst from initiator)");
        handshake_confirmed_ = true;
        responder_handshake_wait_ms_ = 0;
        if (on_handshake_confirmed_) on_handshake_confirmed_();
    }
    if (state_ == ConnectionState::CONNECTED && !is_initiator_ &&
        !connect_ack_frame_.empty()) {
        LOG_MODEM(INFO, "Connection: CONNECT_ACK rescue disarmed (SR burst decoded)");
        connect_ack_frame_.clear();
        connect_ack_retx_remaining_ = 0;
    }

    // Deliver every decoded REAL frame (drop pads addressed to the burst-pad
    // callsign). FileTransferController's assembler writes each chunk at its
    // embedded offset, buffers out-of-order, dedups overlaps, and finalizes by
    // byte count (not the FINAL flag) — so partial/reordered delivery is already
    // correct, including a FINAL-tail frame that arrives before earlier gaps fill.
    int delivered = 0;
    for (const auto& frame : frames) {
        auto df = v2::DataFrame::deserialize(frame);
        if (!df) continue;
        auto hdr = v2::parseHeader(frame);
        if (hdr.valid && !v2::isAddressedToCallsign(hdr, local_call_)) continue;  // pad
        const bool more_data = (df->flags & v2::Flags::MORE_FRAG) != 0;
        handleDataPayload(df->payload, more_data, df->type);
        ++delivered;
    }

    // Correctness guard (the seed-44 offset-0 loss): the assembler drops FILE_DATA
    // delivered before FILE_START establishes RECEIVING. The decoder's frame_mask
    // says "decoded", but un-RECEIVING delivery DROPS it — so ack DELIVERY, not
    // DECODE. Until the stream is established we NACK the whole burst (the sender
    // resends metadata + data together); once established, every decoded frame is
    // safely written/buffered/deduped so the true mask is the right ACK. A late
    // duplicate after finalization (ever-receiving but no longer RECEIVING) is acked
    // so the sender completes instead of resending to death.
    const bool receiving_now =
        (file_transfer_.getState() == FileTransferState::RECEIVING);
    if (receiving_now) {
        burst_rx_ever_receiving_ = true;
    }
    // Pre-RECEIVING, decoded FILE_DATA is now STAGED (not dropped) by the file
    // controller — the BURST_HEADER already told us a bulk burst is inbound — so it is
    // safe to ACK the decode mask as soon as we have a live file context: RECEIVING,
    // ever-received, OR healthy staged data. Staged frames survive until FILE_START
    // drains them, so this can no longer ACK data that gets silently dropped (the
    // seed-44 hazard). A burst that retained NOTHING (no staging, never receiving — or
    // staging overflowed) is still NACKed so the sender's liveness fires. This is what
    // lets group 0 ACK its decoded data frames and re-send only the missing FILE_START
    // frame, instead of re-sending the whole group.
    const bool safe_to_ack =
        receiving_now || burst_rx_ever_receiving_ ||
        file_transfer_.hasHealthyStagedData();
    const uint8_t ack_mask = safe_to_ack ? frame_mask : 0;

    // ACK the delivery-safe mask. type=Ack when any frame is acked (forward
    // progress); Nack only on a fully-undelivered burst (0/N) so the sender's
    // max_retries liveness still fires on a genuinely dead link.
    if (on_transmit_tone_burst_ack_) {
        ultra::waveform::tone_burst_ack::ToneBurstAckPayload tba;
        tba.group_seq = static_cast<uint8_t>(group_seq & 0x3F);
        tba.frame_mask = ack_mask;
        tba.type = (ack_mask == 0)
                       ? ultra::waveform::tone_burst_ack::AckType::Nack
                       : ultra::waveform::tone_burst_ack::AckType::Ack;
        const uint8_t q = pending_ack_quality_q_;
        tba.rate_hint = (q == 0xFF)
                            ? 0
                            : static_cast<uint8_t>((static_cast<uint32_t>(q) * 7u) / 254u);
        on_transmit_tone_burst_ack_(tba);
        LOG_MODEM(INFO,
                  "Connection: SR-ARQ burst group_seq=%u delivered %d frame(s); "
                  "tone-burst %s ack_mask=0x%02X (decode_mask=0x%02X receiving=%d)",
                  group_seq, delivered, ack_mask == 0 ? "NACK" : "ACK",
                  static_cast<unsigned>(ack_mask), static_cast<unsigned>(frame_mask),
                  receiving_now ? 1 : 0);
    }
}

bool Connection::formOneNewBurstFrame(Bytes& out_frame, bool& is_pad,
                                      size_t per_frame_data) {
    is_pad = false;
    constexpr size_t kFileDataOverhead = 5;  // TYPE(1) + OFFSET(4)
    // Metadata chunks (FILE_START / FILE_BLOCK) ride the first new slots, as-is.
    if (!burst_metadata_queue_.empty()) {
        const Bytes& chunk = burst_metadata_queue_.front();
        const uint16_t seq = burst_chunk_seq_++;
        auto frame = v2::makeFixedDataFrame(local_call_, remote_call_, seq, chunk,
                                            data_code_rate_, data_frame_cw_count_,
                                            selectBurstLiftingZ());
        frame.type = v2::FrameType::DATA;
        frame.flags = v2::Flags::MORE_FRAG;  // metadata is never the file tail
        out_frame = frame.serialize();
        burst_metadata_queue_.pop_front();
        return true;
    }
    const size_t total = burst_file_payload_.size();
    if (burst_file_cursor_ >= total) {
        return false;  // file + metadata drained
    }
    const size_t offset_in_file = burst_file_cursor_;
    const size_t this_data = std::min(per_frame_data, total - offset_in_file);
    Bytes chunk;
    chunk.reserve(kFileDataOverhead + this_data);
    chunk.push_back(static_cast<uint8_t>(PayloadType::FILE_DATA));
    const uint32_t off32 = static_cast<uint32_t>(offset_in_file);
    chunk.push_back(static_cast<uint8_t>((off32 >> 24) & 0xFF));
    chunk.push_back(static_cast<uint8_t>((off32 >> 16) & 0xFF));
    chunk.push_back(static_cast<uint8_t>((off32 >> 8) & 0xFF));
    chunk.push_back(static_cast<uint8_t>(off32 & 0xFF));
    chunk.insert(chunk.end(), burst_file_payload_.begin() + offset_in_file,
                 burst_file_payload_.begin() + offset_in_file + this_data);
    const uint16_t seq = burst_chunk_seq_++;
    auto frame = v2::makeFixedDataFrame(local_call_, remote_call_, seq, chunk,
                                        data_code_rate_, data_frame_cw_count_,
                                        selectBurstLiftingZ());
    frame.type = v2::FrameType::DATA;
    const bool finishes_file = (offset_in_file + this_data >= total);
    frame.flags = finishes_file ? v2::Flags::FINAL : v2::Flags::MORE_FRAG;
    out_frame = frame.serialize();
    burst_file_cursor_ += this_data;
    return true;
}

bool Connection::formAndSendBurstGroupSR(uint16_t group_seq, bool is_resend) {
    if (!on_transmit_burst_) return false;
    // Timeout resend (no ACK came back): the failed frames were never re-queued by
    // onToneBurstAck, so re-queue the un-acked in-flight burst's REAL frames now —
    // otherwise the refill below would advance the cursor and skip this group's
    // bytes (the seed-7 stall: a missed group 3 became "group 4 sent as seq 3" and
    // group 3's bytes were lost). A NACK-driven resend has already populated the
    // resend queue, so the empty-queue check distinguishes the two without double-
    // queuing.
    if (is_resend && burst_resend_frames_.empty() && !burst_inflight_frames_.empty()) {
        for (size_t i = 0; i < burst_inflight_frames_.size(); ++i) {
            const bool pad = (i < burst_inflight_is_pad_.size()) && burst_inflight_is_pad_[i];
            if (!pad) burst_resend_frames_.push_back(burst_inflight_frames_[i]);
        }
        LOG_MODEM(INFO,
                  "Connection: SR timeout resend group_seq=%u — re-queued %zu in-flight frame(s)",
                  group_seq, burst_resend_frames_.size());
    }
    const size_t group_size = connection_policy::burstInterleaveGroupFrames();
    const int active_z = selectBurstLiftingZ();
    const size_t frame_cap = (active_z == 81)
        ? v2::getFixedFramePayloadCapacityZ(data_code_rate_, data_frame_cw_count_, 81)
        : v2::getFixedFramePayloadCapacity(data_code_rate_, data_frame_cw_count_);
    constexpr size_t kFileDataOverhead = 5;
    if (frame_cap <= kFileDataOverhead) return false;
    const size_t per_frame_data = frame_cap - kFileDataOverhead;

    // Done: nothing queued to resend AND the file+metadata are fully drained.
    const bool drained = burst_resend_frames_.empty() &&
                         burst_metadata_queue_.empty() &&
                         burst_file_cursor_ >= burst_file_payload_.size();
    if (drained) return false;  // transport marks done(success)

    BurstStopAndWaitController::Group frames;
    std::vector<bool> is_pad;
    frames.reserve(group_size);
    is_pad.reserve(group_size);

    // 1) Resends first (identical serialized bytes — no offset/length/rate
    //    re-derivation, so the receiver's offset-keyed assembler always lines up).
    int resent = 0;
    while (frames.size() < group_size && !burst_resend_frames_.empty()) {
        frames.push_back(std::move(burst_resend_frames_.front()));
        burst_resend_frames_.pop_front();
        is_pad.push_back(false);
        ++resent;
    }
    // 2) Refill the rest with NEW frames from the cursor (advanced immediately;
    //    failed frames are tracked in burst_resend_frames_, not via the cursor).
    int new_frames = 0;
    while (frames.size() < group_size) {
        Bytes nf;
        bool pad = false;
        if (!formOneNewBurstFrame(nf, pad, per_frame_data)) break;
        frames.push_back(std::move(nf));
        is_pad.push_back(pad);
        if (!pad) ++new_frames;
    }
    // 3) Pad the remainder to a full burst (filler; never re-queued on NACK).
    while (frames.size() < group_size) {
        const size_t i = frames.size();
        frames.push_back(v2::makeFixedDataFrame(
            local_call_, kOFDMBurstPadCallsign,
            static_cast<uint16_t>(kOFDMBurstPadSeq - i),
            makeOFDMBurstPadPayload(data_code_rate_, data_frame_cw_count_, i),
            data_code_rate_, data_frame_cw_count_).serialize());
        is_pad.push_back(true);
    }

    // Record the in-flight burst so the next frame_mask maps positions -> frames.
    burst_inflight_frames_ = frames;
    burst_inflight_is_pad_ = is_pad;

    LOG_MODEM(INFO,
              "Connection: SR form group_seq=%u rate=%s cap=%zuB/frame resent=%d new=%d "
              "resend_left=%zu cursor=%zu/%zu",
              group_seq, codeRateToString(data_code_rate_), per_frame_data, resent,
              new_frames, burst_resend_frames_.size(), burst_file_cursor_,
              burst_file_payload_.size());

    // A burst carrying resends follows a fade — pay the full chirp+LTS anchor so
    // bravo re-acquires deterministically (mirrors the whole-group resend anchor).
    on_transmit_burst_(frames, group_seq, /*force_full_preamble=*/resent > 0);
    if (on_arm_tone_burst_ack_monitor_) {
        on_arm_tone_burst_ack_monitor_(burst_transport_.ackTimeoutMs());
    }
    return true;
}

void Connection::collectBurstGroupFrame(uint16_t group_seq, const Bytes& frame_data,
                                        bool group_complete) {
    // RX group assembly for the burst transport. Decoded burst frames arrive in
    // order; group_complete marks the last frame of an interleaved group. On a
    // complete group we hand the accumulated frames to the controller, which
    // delivers (once) and emits a single GROUP_ACK.
    if (!use_burst_transport_) {
        return;
    }
    if (!burst_rx_group_open_ || group_seq != burst_rx_group_seq_) {
        burst_rx_group_open_ = true;
        burst_rx_group_seq_ = group_seq;
        burst_rx_group_frames_.clear();
    }
    burst_rx_group_frames_.push_back(frame_data);

    if (group_complete) {
        burst_transport_.onGroupReceived(group_seq, burst_rx_group_frames_);
        burst_rx_group_open_ = false;
        burst_rx_group_frames_.clear();
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

    size_t submitted_this_call = 0;
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
    if (state_ == ConnectionState::CONNECTED && !is_initiator_ && !handshake_confirmed_ &&
        !duplicate_connect_retry) {
        LOG_MODEM(INFO, "Connection: Handshake confirmed (received first valid frame from initiator)");
        handshake_confirmed_ = true;
        responder_handshake_wait_ms_ = 0;
        if (on_handshake_confirmed_) {
            on_handshake_confirmed_();
        }
        // Initial data mode is already carried in CONNECT_ACK.
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
                case v2::FrameType::GROUP_ACK:
                    // §14.27: whole-burst ACK for the one-way burst transport.
                    // Advances the group stop-and-wait sender by one group; no
                    // SACK, no per-frame retransmit. Only meaningful when the
                    // burst transport drives the file path.
                    if (state_ == ConnectionState::CONNECTED && use_burst_transport_) {
                        const uint16_t group_seq = ctrl->getGroupAckSeq();
                        LOG_MODEM(INFO, "Connection: GROUP_ACK group_seq=%u", group_seq);
                        if (local_data_turn_) {
                            armDataTurnTxGuard(dataTurnAckDiversityGuardMs(*ctrl));
                        }
                        // §14.36: the receiver fed back its decode headroom; run the
                        // rate controller (sender owns rate) and pick the next rate.
                        applyAdaptiveRateFeedback(ctrl->getGroupAckQuality());
                        burst_transport_.onGroupAck(group_seq);
                    }
                    break;
                case v2::FrameType::GROUP_NACK:
                    // §14.30: receiver couldn't decode the in-flight group — resend
                    // it immediately instead of waiting out the group-ACK timeout.
                    if (state_ == ConnectionState::CONNECTED && use_burst_transport_) {
                        const uint16_t group_seq = ctrl->getGroupNackSeq();
                        LOG_MODEM(INFO, "Connection: GROUP_NACK group_seq=%u (fast resend)", group_seq);
                        if (local_data_turn_) {
                            armDataTurnTxGuard(dataTurnAckDiversityGuardMs(*ctrl));
                        }
                        // §14.36: a NACK is a failed group -> quality 0 -> controller
                        // steps the rate down before the resend.
                        applyAdaptiveRateFeedback(0.0f);
                        burst_transport_.onGroupNack(group_seq);
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

    if (tryIssueAdaptiveModeChangeAtBoundary()) {
        return;
    }

    const bool refill_file = deferred_file_refill_;
    const bool refill_fragments = deferred_fragment_refill_;
    deferred_file_refill_ = false;
    deferred_fragment_refill_ = false;

    if (state_ != ConnectionState::CONNECTED) {
        return;
    }
    if (!local_data_turn_ || file_cancel_confirm_pending_ || data_turn_tx_guard_ms_ > 0) {
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

void Connection::resetAdaptiveModeController() {
    adaptive_target_ = AdaptiveModeTarget{};
    adaptive_last_stats_ = arq_.getStats();
    adaptive_eval_elapsed_ms_ = 0;
    adaptive_cooldown_ms_ = ADAPTIVE_MODE_CHANGE_COOLDOWN_MS;
    adaptive_post_downgrade_lockout_ms_ = 0;
    adaptive_downgrade_queue_age_ms_ = 0;
    adaptive_clean_windows_ = 0;
    adaptive_pressure_windows_ = 0;
}

bool Connection::canIssueAdaptiveModeChange(bool is_downgrade) const {
    if (state_ != ConnectionState::CONNECTED || !isOFDMMode(negotiated_mode_)) {
        return false;
    }
    if (!local_data_turn_ || peer_data_turn_requested_ || data_turn_tx_guard_ms_ > 0) {
        return false;
    }
    if (config_.forced_modulation != Modulation::AUTO ||
        config_.forced_code_rate != CodeRate::AUTO) {
        return false;
    }
    if (mode_change_pending_ || disconnect_pending_) {
        return false;
    }
    if (file_transfer_.getState() != FileTransferState::SENDING) {
        return false;
    }
    if (!is_initiator_ && !handshake_confirmed_) {
        return false;
    }
    if (!pending_tx_fragments_.empty()) {
        return false;
    }

    const size_t available_slots = arq_.getAvailableSlots();
    const size_t window_size = arq_.getWindowSize();
    if (is_downgrade) {
        const bool target_changes_modulation =
            adaptive_target_.pending &&
            adaptive_target_.modulation != data_modulation_;
        if (target_changes_modulation) {
            // Changing modulation also changes the demodulator, pilot layout,
            // and coherent-vs-differential control profile. Keep those regime
            // changes on a fully drained ARQ boundary; only same-regime
            // code-rate downgrades may use the half-window recovery path.
            return available_slots == window_size;
        }
        // Downgrades are recovery-oriented for sustained backlog, but a file
        // tail smaller than one ARQ window should drain in the current mode.
        // Injecting MODE_CHANGE into a short repair tail steals the ACK/DATA
        // turn and can starve the last few frames. The boundary is derived from
        // the active ARQ window, not from an SNR/rate constant.
        const CodeRate target_rate =
            adaptive_target_.pending ? adaptive_target_.rate : data_code_rate_;
        if (adaptiveBacklogFrames(target_rate) < window_size) {
            return available_slots == window_size;
        }
        return available_slots * 2 >= window_size;
    }

    // Upgrades keep the strict boundary so in-flight DATA at the old, more
    // robust rate clears before switching to a faster rate.
    return available_slots == window_size;
}

size_t Connection::adaptiveBacklogFrames(CodeRate rate) const {
    size_t frames = 0;
    const size_t capacity = v2::getFixedFramePayloadCapacity(rate, data_frame_cw_count_);
    const size_t file_data_payload =
        capacity > FileTransferController::FILE_DATA_OVERHEAD
            ? capacity - FileTransferController::FILE_DATA_OVERHEAD
            : capacity;

    if (file_transfer_.getState() == FileTransferState::SENDING) {
        frames += file_transfer_.pendingChunkCount();

        const size_t remaining = file_transfer_.remainingTxBytes();
        if (remaining > 0 && file_data_payload > 0) {
            frames += (remaining + file_data_payload - 1) / file_data_payload;
        } else if (file_transfer_.hasMoreChunks()) {
            frames += 1;
        }
    }

    if (!pending_tx_fragments_.empty() &&
        acked_fragment_count_ < pending_tx_fragments_.size()) {
        frames += pending_tx_fragments_.size() - acked_fragment_count_;
    }

    return frames;
}

bool Connection::hasAdaptiveUpgradeBacklog(CodeRate target_rate) const {
    if (file_transfer_.getState() != FileTransferState::SENDING) {
        return false;
    }
    return adaptiveBacklogFrames(target_rate) >=
           arq_.getWindowSize() * ADAPTIVE_UPGRADE_BACKLOG_WINDOWS;
}

bool Connection::tryIssueAdaptiveModeChangeAtBoundary() {
    if (!adaptive_target_.pending) {
        return false;
    }

    const bool is_downgrade =
        isMoreRobustMode(adaptive_target_.modulation, adaptive_target_.rate,
                         data_modulation_, data_code_rate_);
    const bool downgrade_stuck =
        is_downgrade &&
        adaptive_downgrade_queue_age_ms_ >= ADAPTIVE_DOWNGRADE_FORCE_MS;
    const bool boundary_ready = canIssueAdaptiveModeChange(is_downgrade);
    const size_t backlog_frames = adaptiveBacklogFrames(adaptive_target_.rate);
    const bool backlog_can_absorb_mode_change = backlog_frames >= arq_.getWindowSize();
    const bool target_changes_modulation =
        adaptive_target_.modulation != data_modulation_;
    const bool force_downgrade =
        downgrade_stuck &&
        backlog_can_absorb_mode_change &&
        !target_changes_modulation &&
        !boundary_ready &&
        state_ == ConnectionState::CONNECTED &&
        isOFDMMode(negotiated_mode_) &&
        config_.forced_modulation == Modulation::AUTO &&
        config_.forced_code_rate == CodeRate::AUTO &&
        !mode_change_pending_ &&
        !disconnect_pending_ &&
        file_transfer_.getState() == FileTransferState::SENDING &&
        (is_initiator_ || handshake_confirmed_) &&
        pending_tx_fragments_.empty();
    if (!boundary_ready && !force_downgrade) {
        return false;
    }

    if (backlog_frames == 0) {
        adaptive_target_ = AdaptiveModeTarget{};
        adaptive_downgrade_queue_age_ms_ = 0;
        return false;
    }

    const bool is_upgrade =
        isFasterMode(adaptive_target_.modulation, adaptive_target_.rate,
                     data_modulation_, data_code_rate_);
    if (is_upgrade && !hasAdaptiveUpgradeBacklog(adaptive_target_.rate)) {
        adaptive_target_ = AdaptiveModeTarget{};
        adaptive_downgrade_queue_age_ms_ = 0;
        return false;
    }

    if (adaptive_target_.modulation == data_modulation_ &&
        adaptive_target_.rate == data_code_rate_) {
        adaptive_target_ = AdaptiveModeTarget{};
        adaptive_downgrade_queue_age_ms_ = 0;
        return false;
    }

    if (force_downgrade) {
        LOG_MODEM(WARN,
                  "Connection: Forced downgrade after %ums queue age (sustained backlog): %s %s -> %s %s",
                  adaptive_downgrade_queue_age_ms_,
                  modulationToString(data_modulation_),
                  codeRateToString(data_code_rate_),
                  modulationToString(adaptive_target_.modulation),
                  codeRateToString(adaptive_target_.rate));
    }

    LOG_MODEM(INFO, "Connection: Adaptive MODE_CHANGE at TX boundary: %s %s -> %s %s (SNR=%.1f (%s), fading=%.2f, backlog=%zu frames)",
              modulationToString(data_modulation_), codeRateToString(data_code_rate_),
              modulationToString(adaptive_target_.modulation),
              codeRateToString(adaptive_target_.rate),
              measured_snr_db_, snrSourceToString(measured_snr_source_), fading_index_,
              backlog_frames);

    requestModeChange(adaptive_target_.modulation,
                      adaptive_target_.rate,
                      measured_snr_db_,
                      adaptive_target_.reason);
    adaptive_target_ = AdaptiveModeTarget{};
    adaptive_downgrade_queue_age_ms_ = 0;
    adaptive_cooldown_ms_ = ADAPTIVE_MODE_CHANGE_COOLDOWN_MS;
    adaptive_clean_windows_ = 0;
    adaptive_pressure_windows_ = 0;
    if (is_downgrade && mode_change_pending_) {
        adaptive_post_downgrade_lockout_ms_ = ADAPTIVE_POST_DOWNGRADE_LOCKOUT_MS;
    }
    return mode_change_pending_;
}

void Connection::updateAdaptiveModeController(uint32_t elapsed_ms) {
    // DISABLED for the one-way burst file design (§14.20/§14.25). Mid-transfer
    // adaptive rate renegotiation (MODE_CHANGE on backlog) is old interactive-era
    // machinery that actively FIGHTS the target: it downgraded R3/4 -> R2/3 on a
    // transient burst backlog, defeating the whole point of the burst-interleaver
    // rework (make R3/4 survive Good fading). The negotiated rate is now fixed for
    // the session; the self-describing burst descriptor declares per-burst params,
    // and deep-fade bursts are recovered by interleave depth + whole-burst ARQ, NOT
    // by retreating in rate. Reliability on R3/4 is a PHY/interleave problem, not a
    // rate-selection one.
    (void)elapsed_ms;
    resetAdaptiveModeController();
    return;

    if (adaptive_cooldown_ms_ > 0) {
        if (elapsed_ms >= adaptive_cooldown_ms_) {
            adaptive_cooldown_ms_ = 0;
        } else {
            adaptive_cooldown_ms_ -= elapsed_ms;
        }
    }
    if (adaptive_post_downgrade_lockout_ms_ > 0) {
        if (elapsed_ms >= adaptive_post_downgrade_lockout_ms_) {
            adaptive_post_downgrade_lockout_ms_ = 0;
        } else {
            adaptive_post_downgrade_lockout_ms_ -= elapsed_ms;
        }
    }

    if (state_ != ConnectionState::CONNECTED || !isOFDMMode(negotiated_mode_)) {
        resetAdaptiveModeController();
        return;
    }
    if (config_.forced_modulation != Modulation::AUTO ||
        config_.forced_code_rate != CodeRate::AUTO) {
        adaptive_last_stats_ = arq_.getStats();
        adaptive_target_ = AdaptiveModeTarget{};
        adaptive_post_downgrade_lockout_ms_ = 0;
        adaptive_downgrade_queue_age_ms_ = 0;
        adaptive_clean_windows_ = 0;
        adaptive_pressure_windows_ = 0;
        return;
    }
    if (file_transfer_.getState() != FileTransferState::SENDING) {
        adaptive_last_stats_ = arq_.getStats();
        adaptive_target_ = AdaptiveModeTarget{};
        adaptive_post_downgrade_lockout_ms_ = 0;
        adaptive_downgrade_queue_age_ms_ = 0;
        adaptive_clean_windows_ = 0;
        adaptive_pressure_windows_ = 0;
        return;
    }
    if (mode_change_pending_) {
        adaptive_last_stats_ = arq_.getStats();
        adaptive_target_ = AdaptiveModeTarget{};
        adaptive_downgrade_queue_age_ms_ = 0;
        adaptive_clean_windows_ = 0;
        adaptive_pressure_windows_ = 0;
        return;
    }

    if (adaptive_target_.pending &&
        isMoreRobustMode(adaptive_target_.modulation, adaptive_target_.rate,
                         data_modulation_, data_code_rate_)) {
        adaptive_downgrade_queue_age_ms_ += elapsed_ms;
        if (adaptive_downgrade_queue_age_ms_ >= ADAPTIVE_DOWNGRADE_FORCE_MS &&
            tryIssueAdaptiveModeChangeAtBoundary()) {
            return;
        }
    } else {
        adaptive_downgrade_queue_age_ms_ = 0;
    }

    adaptive_eval_elapsed_ms_ += elapsed_ms;
    if (adaptive_eval_elapsed_ms_ < ADAPTIVE_EVAL_INTERVAL_MS) {
        return;
    }
    adaptive_eval_elapsed_ms_ = 0;

    const ARQStats current_stats = arq_.getStats();
    const AdaptivePressure retry_pressure =
        getAdaptiveRetryPressure(current_stats, adaptive_last_stats_,
                                 arq_.getWindowSize());
    const bool tx_window_idle =
        arq_.getAvailableSlots() == arq_.getWindowSize();
    const bool clean_window =
        hasCleanAdaptiveWindow(current_stats, adaptive_last_stats_, tx_window_idle);
    adaptive_last_stats_ = current_stats;
    if (retry_pressure.present) {
        adaptive_pressure_windows_++;
    } else if (clean_window) {
        adaptive_pressure_windows_ = 0;
    }

    Modulation recommended_mod = data_modulation_;
    CodeRate recommended_rate = data_code_rate_;
    recommendDataMode(measured_snr_db_, negotiated_mode_,
                      recommended_mod, recommended_rate, fading_index_);

    if (adaptive_target_.pending) {
        const bool target_is_downgrade =
            isMoreRobustMode(adaptive_target_.modulation, adaptive_target_.rate,
                             data_modulation_, data_code_rate_);
        if (!target_is_downgrade && retry_pressure.present) {
            LOG_MODEM(INFO,
                      "Connection: Adaptive upgrade canceled by retry pressure before boundary: %s %s -> %s %s",
                      modulationToString(data_modulation_),
                      codeRateToString(data_code_rate_),
                      modulationToString(adaptive_target_.modulation),
                      codeRateToString(adaptive_target_.rate));
            adaptive_target_ = AdaptiveModeTarget{};
            adaptive_clean_windows_ = 0;
            adaptive_downgrade_queue_age_ms_ = 0;
            return;
        }

        tryIssueAdaptiveModeChangeAtBoundary();
        return;
    }

    if (adaptive_cooldown_ms_ == 0 &&
        retry_pressure.present &&
        canDowngradeMode(data_modulation_, data_code_rate_)) {
        AdaptiveMode target = oneStepMoreRobustMode(data_modulation_, data_code_rate_);
        const bool severe_required =
            downgradeRequiresSeverePressure(data_modulation_, data_code_rate_, target) ||
            csiSupportsFasterSameConstellation(recommended_mod, recommended_rate,
                                               data_modulation_, data_code_rate_) ||
            (target.modulation == data_modulation_ &&
             csiOnlyRequestsSameOrderRegimeChange(recommended_mod, recommended_rate,
                                                  data_modulation_, data_code_rate_));
        const bool downgrade_pressure_ready =
            retry_pressure.severe ||
            (!severe_required &&
             adaptive_pressure_windows_ >= ADAPTIVE_PRESSURE_WINDOWS_FOR_DOWNGRADE);
        if (!downgrade_pressure_ready) {
            if (severe_required) {
                LOG_MODEM(INFO,
                          "Connection: Holding %s %s despite retry pressure (recommended=%s %s, severe=%d, pressure_windows=%d); waiting for severe evidence before overriding CSI",
                          modulationToString(data_modulation_),
                          codeRateToString(data_code_rate_),
                          modulationToString(recommended_mod),
                          codeRateToString(recommended_rate),
                          retry_pressure.severe ? 1 : 0,
                          adaptive_pressure_windows_);
            }
            return;
        }
        const bool recommended_is_safe_downgrade =
            isMoreRobustMode(recommended_mod, recommended_rate,
                             data_modulation_, data_code_rate_) &&
            getBitsPerSymbol(recommended_mod) <= getBitsPerSymbol(data_modulation_);
        if (recommended_is_safe_downgrade) {
            AdaptiveMode recommended_target{recommended_mod, recommended_rate};
            const bool preserve_d8psk_rate_ladder =
                data_modulation_ == Modulation::D8PSK &&
                target.modulation == data_modulation_ &&
                recommended_target.modulation != data_modulation_;
            const bool preserve_same_order_code_rate_step =
                !retry_pressure.severe &&
                target.modulation == data_modulation_ &&
                isSameConstellationOrderChange(recommended_target.modulation,
                                               data_modulation_);
            if (!preserve_d8psk_rate_ladder &&
                !preserve_same_order_code_rate_step &&
                !isMoreRobustMode(recommended_target.modulation, recommended_target.rate,
                                  target.modulation, target.rate)) {
                target = recommended_target;
            }
        }

        adaptive_clean_windows_ = 0;
        adaptive_target_.pending = true;
        adaptive_target_.modulation = target.modulation;
        adaptive_target_.rate = target.rate;
        adaptive_target_.reason = v2::ModeChangeReason::CHANNEL_DEGRADED;
        LOG_MODEM(INFO, "Connection: Adaptive downgrade queued: %s %s -> %s %s (recommended=%s %s, SNR=%.1f (%s), fading=%.2f, severe=%d, pressure_windows=%d)",
                  modulationToString(data_modulation_),
                  codeRateToString(data_code_rate_),
                  modulationToString(adaptive_target_.modulation),
                  codeRateToString(adaptive_target_.rate),
                  modulationToString(recommended_mod),
                  codeRateToString(recommended_rate),
                  measured_snr_db_, snrSourceToString(measured_snr_source_),
                  fading_index_,
                  retry_pressure.severe ? 1 : 0,
                  adaptive_pressure_windows_);
        tryIssueAdaptiveModeChangeAtBoundary();
        return;
    }

    if (clean_window) {
        adaptive_clean_windows_++;
    } else {
        adaptive_clean_windows_ = 0;
    }

    if (adaptive_cooldown_ms_ == 0 &&
        adaptive_post_downgrade_lockout_ms_ == 0 &&
        !adaptive_target_.pending &&
        adaptive_clean_windows_ >= ADAPTIVE_CLEAN_WINDOWS_FOR_UPGRADE &&
        isFasterMode(recommended_mod, recommended_rate,
                     data_modulation_, data_code_rate_) &&
        canIssueAdaptiveModeChange(false) &&
        hasAdaptiveUpgradeBacklog(recommended_rate)) {
        AdaptiveMode target =
            oneStepFasterToward(data_modulation_, data_code_rate_,
                                recommended_mod, recommended_rate);
        if (target.modulation == data_modulation_ &&
            target.rate == data_code_rate_) {
            return;
        }
        adaptive_target_.pending = true;
        adaptive_target_.modulation = target.modulation;
        adaptive_target_.rate = target.rate;
        adaptive_target_.reason = v2::ModeChangeReason::CHANNEL_IMPROVED;
        LOG_MODEM(INFO, "Connection: Adaptive upgrade queued: %s %s -> %s %s (recommended=%s %s, SNR=%.1f (%s), fading=%.2f, clean=%d, backlog=%zu frames)",
                  modulationToString(data_modulation_),
                  codeRateToString(data_code_rate_),
                  modulationToString(adaptive_target_.modulation),
                  codeRateToString(adaptive_target_.rate),
                  modulationToString(recommended_mod),
                  codeRateToString(recommended_rate),
                  measured_snr_db_, snrSourceToString(measured_snr_source_), fading_index_,
                  adaptive_clean_windows_,
                  adaptiveBacklogFrames(recommended_rate));
        tryIssueAdaptiveModeChangeAtBoundary();
    }
}

// =============================================================================
// TIMER / TICK
// =============================================================================

void Connection::tick(uint32_t elapsed_ms) {
    soft_combine_harq_.tick(elapsed_ms);

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

            tickModeChangeAckRepeats(elapsed_ms);

            // Handle MODE_CHANGE timeout
            if (mode_change_pending_) {
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
                        adaptive_cooldown_ms_ = ADAPTIVE_MODE_CHANGE_COOLDOWN_MS;
                        adaptive_target_ = AdaptiveModeTarget{};
                        adaptive_downgrade_queue_age_ms_ = 0;
                        adaptive_clean_windows_ = 0;
                        adaptive_pressure_windows_ = 0;
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
            if (use_burst_transport_) {
                // §14.27 Stage 2: drive the burst stop-and-wait controller clock
                // (ACK-timeout -> whole-burst resend). Inert unless the file path
                // activated it; arq_ still ticks for messages.
                burst_transport_.tick(elapsed_ms);
            }
            updateAdaptiveModeController(elapsed_ms);
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
    const uint32_t arq_timeout_ms = arq_.getAckTimeout();
    if (arq_timeout_ms > 0) {
        return arq_timeout_ms;
    }

    const uint64_t control_round_trip_ms =
        2ULL * static_cast<uint64_t>(dataTurnControlGuardMs()) +
        static_cast<uint64_t>(connection_policy::kCarrierSenseSackCoalesceMs);
    return static_cast<uint32_t>(
        std::min<uint64_t>(control_round_trip_ms, 0xFFFFFFFFull));
}

void Connection::scheduleModeChangeAckRepeats(const Bytes& ack_data, uint16_t ack_seq) {
    if (!isOFDMMode(negotiated_mode_)) {
        return;
    }

    const int repeat_count = arq_.getAckRepeatCount();
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
        uint32_t ack_timeout_ms = connection_policy::computeMCDPSKAckTimeoutMs(
            timing, window_size, arq_.getSackDelay(),
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
            data_modulation_, data_frame_cw_count_, kNarrowWindow);
        arq_.setAckTimeout(timeout_ms);

        LOG_MODEM(INFO, "Connection: ARQ window=%zu, timeout=%.2fs (data=%ums, ack=%ums), carrier_sense_sack_coalesce=%ums, ack_repeat=%d, cw=%d (OFDM_NARROW %s %s)",
                  kNarrowWindow, timeout_ms / 1000.0f, timing.data_ms, timing.ack_ms,
                  arq_.getSackDelay(),
                  connection_policy::kCarrierSenseAckRepeatCount,
                  data_frame_cw_count_,
                  modulationToString(data_modulation_), codeRateToString(data_code_rate_));
    } else {
        const bool near_awgn_ofdm =
            connection_policy::isNearAwgnOFDM(fading_index_, measured_snr_db_);
        arq_.setWindowSize(connection_policy::ofdmWindowSizeForChannel(
            data_modulation_, data_code_rate_, fading_index_, measured_snr_db_));
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
        constexpr int kWideOFDMAckRepeatCount = 3;
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
    if (isOFDMMode(negotiated_mode_) &&
        use_burst_transport_ &&
        file_transfer_.getState() == FileTransferState::SENDING) {
        return 81;
    }
    return 27;
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

void Connection::notifyDataModeChanged(float snr_db, float peer_fading_index) {
    if (!on_data_mode_changed_) {
        return;
    }
    const bool mc_dpsk = negotiated_mode_ == WaveformMode::MC_DPSK;
    on_data_mode_changed_(data_modulation_, data_code_rate_, data_frame_cw_count_,
                          snr_db, peer_fading_index,
                          mc_dpsk ? config_.mc_dpsk_num_carriers : 0,
                          mc_dpsk ? config_.mc_dpsk_samples_per_symbol : 0);
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
    // Pending chunks must be re-encoded if rate OR CW changed: the ARQ payload
    // capacity depends on both, and chunks queued under the old geometry will
    // overflow / mis-align under the new one.
    const bool requeue_file =
        (rate_changed || cw_changed) &&
        file_transfer_.getState() == FileTransferState::SENDING &&
        file_transfer_.hasPendingChunks();
    const bool refill_file =
        (rate_changed || cw_changed) &&
        file_transfer_.getState() == FileTransferState::SENDING;
    if (requeue_file) {
        file_transfer_.requeuePendingChunks();
    }

    data_modulation_ = mod;
    data_code_rate_ = rate;
    data_frame_cw_count_ = new_cw;
    config_.fixed_frame_codewords = new_cw;
    data_ladder_rung_id_ = (rung_id != LadderRungId::UNKNOWN)
        ? rung_id
        : currentLadderRungId();
    configureArqForCurrentDataMode();
    if (rate_changed || cw_changed) {
        soft_combine_harq_.clear();
        // 2026-05-28: recompute burst ack_timeout for the new mode (same
        // formula as startup / applyAdaptiveRateFeedback). MODE_CHANGE
        // negotiations can land on a slower rate where the original
        // timeout no longer covers the burst+ack round-trip; without
        // this, the next burst at the new rate will fire premature
        // resends every group.
        if (use_burst_transport_) {
            const size_t group_size = connection_policy::burstInterleaveGroupFrames();
            const uint32_t continuation_reanchor_ms =
                connection_policy::wideOFDMShortReanchorChirpDurationMs();
            const uint32_t burst_timeout = connection_policy::computeWideOFDMAckTimeoutMs(
                data_modulation_, data_code_rate_, group_size,
                arq_.getSackDelay(), arq_.getAckRepeatCount(),
                data_frame_cw_count_, continuation_reanchor_ms);
            const uint32_t timeout_ms = std::max<uint32_t>(burst_timeout + 2000u, 14000u);
            burst_transport_.setAckTimeoutMs(timeout_ms);
            LOG_MODEM(INFO,
                      "Connection: ack_timeout recomputed for MODE_CHANGE: %ums "
                      "(rate=%s mod=%s cw=%d)",
                      timeout_ms, codeRateToString(data_code_rate_),
                      modulationToString(data_modulation_), data_frame_cw_count_);
        }
    }
    resetAdaptiveModeController();

    if (refill_file) {
        deferred_file_refill_ = true;
    }
}

void Connection::commitPendingModeChange(const char* outcome) {
    if (!mode_change_pending_) {
        return;
    }

    const bool was_downgrade =
        isMoreRobustMode(pending_modulation_, pending_code_rate_,
                         data_modulation_, data_code_rate_);
    LOG_MODEM(INFO, "Connection: MODE_CHANGE %s, applying %s %s",
              outcome,
              modulationToString(pending_modulation_),
              codeRateToString(pending_code_rate_));
    applyDataMode(pending_modulation_, pending_code_rate_,
                  pending_cw_count_, pending_ladder_rung_id_);
    if (was_downgrade) {
        adaptive_post_downgrade_lockout_ms_ =
            ADAPTIVE_POST_DOWNGRADE_LOCKOUT_MS;
        adaptive_clean_windows_ = 0;
        adaptive_pressure_windows_ = 0;
    }
    mode_change_pending_ = false;
    mode_change_timeout_ms_ = 0;
    mode_change_retry_count_ = 0;
    pending_ladder_rung_id_ = LadderRungId::UNKNOWN;

    notifyDataModeChanged(pending_snr_db_, pending_fading_index_);
    runDeferredArqRefill();
}

void Connection::enterConnected() {
    state_ = ConnectionState::CONNECTED;
    connected_time_ms_ = 0;
    // Half-duplex INTERACTIVE (TNC/Winlink-B2F): the RESPONDER speaks first (it sends
    // the SID banner), so it must be able to transmit immediately. The normal one-way
    // flow makes the responder wait for the initiator's first DATA frame to confirm the
    // handshake (handshake_confirmed_) — but in B2F that never comes first, so the
    // responder deadlocks (it can't even request the DATA turn). The CONNECT/CONNECT_ACK
    // exchange already validated BOTH directions, so pre-confirm the handshake here.
    if (half_duplex_interactive_ && !is_initiator_) {
        handshake_confirmed_ = true;
    }
    interactive_initiator_yield_done_ = false;
    interactive_yield_log_throttle_ms_ = 0;
    local_data_turn_ = is_initiator_;
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
    resetAdaptiveModeController();
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
    mode_change_ack_repeat_jobs_.clear();
    disconnect_frame_.clear();
    disconnect_pending_ = false;
    disconnect_ack_frame_.clear();
    burst_mode_active_ = false;
    burst_tx_buffer_.clear();
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
    resetAdaptiveModeController();
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
        // Single frame, no burst needed
        on_transmit_(burst_tx_buffer_[0]);
    } else if (on_transmit_burst_) {
        LOG_MODEM(INFO, "Connection: Flushing burst of %zu frames", burst_tx_buffer_.size());
        on_transmit_burst_(burst_tx_buffer_, /*group_seq=*/0,
                           /*force_full_preamble=*/false);  // legacy arq_ burst path
    } else if (on_transmit_) {
        // Fallback: send individually
        for (const auto& frame : burst_tx_buffer_) {
            on_transmit_(frame);
        }
    }
    burst_tx_buffer_.clear();
}

void Connection::transmitFrameBatch(const std::vector<Bytes>& frame_data_list) {
    if (frame_data_list.empty()) {
        return;
    }

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
    LOG_MODEM(INFO, "Connection: Resending ARQ timeout-repair as re-interleaved burst of %zu frames",
              frame_data_list.size());
    on_transmit_burst_(frame_data_list, /*group_seq=*/0,
                       /*force_full_preamble=*/false);  // legacy arq_ repair burst
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
    resetAdaptiveModeController();
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
    resetAdaptiveModeController();
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
    LOG_MODEM(DEBUG, "Connection: Full reset");
}

} // namespace protocol
} // namespace ultra
