// Connection mode-change / handshake / ARQ-config regression tests.
//
// NOTE (2026-05-30): the adaptive-RATE-LADDER cases (test_adaptive_*, the
// upgrade/downgrade hysteresis + post-downgrade lockout + timeout-repair
// framing) were removed here. They had drifted out of sync with the
// controller (14/281 checks were RED at this exact state AND at the
// pre-session commit c384b6a — i.e. pre-existing legacy drift, not a
// regression) and the adaptive ladder is being reworked. Proper ladder
// coverage will be re-authored against the reworked controller. The cases
// kept below exercise stable connection plumbing, not the ladder.
//
// NOTE (2026-06-09): the old in-Connection adaptive-mode controller
// (updateAdaptiveModeController + adaptive_target_ hysteresis/lockout state)
// has now been DELETED outright — rate adaptation lives in the EMA-smoothed
// RateController (tests/test_rate_controller.cpp) and lands via the
// synchronized requestModeChange() MODE_CHANGE handshake exercised below. The
// dead test accessors for that machinery were removed with it.
#include "env_compat.hpp"
#include "protocol/connection.hpp"
#include "protocol/connection_policy.hpp"
#include "protocol/frame_v2.hpp"
#include "waveform/tone_burst_ack/tone_burst_ack_monitor.hpp"
#include "helpers/temp_dir.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>

using namespace ultra;
using namespace ultra::protocol;

namespace {

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg) \
    do { \
        ++tests_run; \
        if (!(cond)) { \
            ++tests_failed; \
            std::cout << "FAIL: " << msg << "\n"; \
            return; \
        } \
    } while (0)

std::string createFile(const std::filesystem::path& dir, size_t bytes) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};

    const auto path = dir / "payload.bin";
    std::ofstream out(path, std::ios::binary);
    if (!out) return {};
    for (size_t i = 0; i < bytes; ++i) {
        out.put(static_cast<char>((i * 17 + 3) & 0xFF));
    }
    return path.string();
}

struct TempPayloadFile {
    ultra::test::TempDir dir;
    std::string path;

    TempPayloadFile(const std::string& prefix, size_t bytes)
        : dir(prefix) {
        if (dir.valid()) {
            path = createFile(dir.path(), bytes);
        }
    }
};

} // namespace

namespace ultra {
namespace protocol {

struct ConnectionAdaptiveTestAccess {
    static void makeLocalIss(Connection& c) {
        c.local_data_turn_ = true;
        c.peer_data_turn_requested_ = false;
        c.local_turn_request_pending_ = false;
        c.received_peer_data_since_connect_ = false;
        c.data_turn_yield_pending_ = false;
        c.data_turn_payload_bytes_sent_ = 0;
        c.data_turn_contended_ms_ = 0;
        c.data_turn_tx_guard_ms_ = 0;
        c.turn_request_retransmit_ms_ = 0;
        c.turn_request_holdoff_ms_ = 0;
    }

    static void makeConnectedOFDM(Connection& c,
                                  CodeRate rate,
                                  float snr = 15.0f,
                                  float fading = 0.05f,
                                  Modulation modulation = Modulation::DQPSK) {
        c.local_call_ = "W1ABC";
        c.remote_call_ = "K2DEF";
        c.state_ = ConnectionState::CONNECTED;
        c.is_initiator_ = true;
        c.handshake_confirmed_ = true;
        c.negotiated_mode_ = WaveformMode::OFDM_CHIRP;
        c.data_modulation_ = modulation;
        c.data_code_rate_ = rate;
        c.measured_snr_db_ = snr;
        c.fading_index_ = fading;
        makeLocalIss(c);
        c.arq_.setCallsigns(c.local_call_, c.remote_call_);
        c.configureArqForCurrentDataMode();
    }

    static void makeResponderWithConnectAckRescue(Connection& c,
                                                  WaveformMode mode = WaveformMode::OFDM_CHIRP) {
        c.local_call_ = "K2DEF";
        c.remote_call_ = "W1ABC";
        c.state_ = ConnectionState::CONNECTED;
        c.is_initiator_ = false;
        c.handshake_confirmed_ = false;
        c.negotiated_mode_ = mode;
        c.data_modulation_ = Modulation::DQPSK;
        c.data_code_rate_ = CodeRate::R1_4;
        c.connect_ack_frame_ = Bytes{0x55, 0x4C, static_cast<uint8_t>(v2::FrameType::CONNECT_ACK)};
        c.connect_ack_retransmit_ms_ = 1000;
        c.connect_ack_retx_remaining_ = 1;
    }

    static void makeConnectedInitiator(Connection& c, WaveformMode mode) {
        c.local_call_ = "W1ABC";
        c.remote_call_ = "K2DEF";
        c.state_ = ConnectionState::CONNECTED;
        c.is_initiator_ = true;
        c.handshake_confirmed_ = true;
        c.negotiated_mode_ = mode;
        c.data_modulation_ = Modulation::DQPSK;
        c.data_code_rate_ = CodeRate::R1_4;
        makeLocalIss(c);
        c.arq_.setCallsigns(c.local_call_, c.remote_call_);
    }

    static void enterConnected(Connection& c) {
        c.enterConnected();
    }

    static void transmitFrame(Connection& c, const Bytes& frame) {
        c.transmitFrame(frame);
    }

    static void transmitFrameBatch(Connection& c, const std::vector<Bytes>& frames) {
        c.transmitFrameBatch(frames);
    }

    static uint32_t connectRetryInterval(Connection& c) {
        return c.connectRetryIntervalMs();
    }

    static uint32_t connectAckRetransmitMs(Connection& c) {
        return c.connectAckRetransmitMs();
    }

    static uint32_t modeChangeRetryMs(Connection& c) {
        return c.modeChangeRetryMs();
    }

    static int modeChangeMaxRetries() {
        return Connection::MODE_CHANGE_MAX_RETRIES;
    }

    static bool handshakeConfirmed(const Connection& c) {
        return c.handshake_confirmed_;
    }

    static void setResponderHandshakeWait(Connection& c, uint32_t ms) {
        c.responder_handshake_wait_ms_ = ms;
    }

    static void clearConnectAckRetxBudget(Connection& c) {
        c.connect_ack_retx_remaining_ = 0;
    }

    static bool connectAckRescueArmed(const Connection& c) {
        return !c.connect_ack_frame_.empty() || c.connect_ack_retx_remaining_ > 0 ||
               c.connect_ack_retransmit_ms_ > 0;
    }

    static void startFile(Connection& c, const std::string& path) {
        CHECK(c.file_transfer_.startSend(path), "startSend should succeed");
    }

    static void acknowledgeModeChange(Connection& c) {
        auto ack = v2::ControlFrame::makeAck("K2DEF", "W1ABC",
                                             c.getStats().arq.acks_received + 1);
        ack.seq = c.mode_change_seq_;
        c.onFrameReceived(ack.serialize());
    }

    static void fillArqWindow(Connection& c, size_t in_flight_frames) {
        for (size_t i = 0; i < in_flight_frames; ++i) {
            CHECK(c.arq_.sendFixedDataWithFlags(
                      Bytes(16, static_cast<uint8_t>(0x42 + i)), v2::Flags::MORE_FRAG),
                  "seed DATA frame should enter ARQ window");
        }
    }

    static void createRetransmissionPressure(Connection& c, size_t in_flight_frames) {
        c.arq_.setAckTimeout(100);
        fillArqWindow(c, in_flight_frames);
        advanceRetransmissionPressure(c);
    }

    static void advanceRetransmissionPressure(Connection& c) {
        c.arq_.tick(150);
    }

    static void createRetransmissionPressure(Connection& c) {
        createRetransmissionPressure(c, 1);
    }

    static bool modeChangePending(const Connection& c) {
        return c.mode_change_pending_;
    }

    static CodeRate pendingRate(const Connection& c) {
        return c.pending_code_rate_;
    }

    static Modulation pendingModulation(const Connection& c) {
        return c.pending_modulation_;
    }

    static uint16_t modeChangeSeq(const Connection& c) {
        return c.mode_change_seq_;
    }

    static size_t arqWindow(const Connection& c) {
        return c.arq_.getWindowSize();
    }

    static size_t arqAvailableSlots(const Connection& c) {
        return c.arq_.getAvailableSlots();
    }

    static void abortArqPendingTx(Connection& c) {
        c.arq_.abortPendingTx();
    }

    static CodeRate arqCodeRate(const Connection& c) {
        return c.arq_.getCodeRate();
    }

    static uint32_t arqSackDelay(const Connection& c) {
        return c.arq_.getSackDelay();
    }

    static uint32_t arqSackDelayShort(const Connection& c) {
        return c.arq_.getSackDelayShort();
    }

    static bool arqSackDelaySlidesOnData(const Connection& c) {
        return c.arq_.getSackDelaySlidesOnData();
    }

    static uint32_t arqAckTimeout(const Connection& c) {
        return c.arq_.getAckTimeout();
    }

    static int arqAckRepeatCount(const Connection& c) {
        return c.arq_.getAckRepeatCount();
    }

    static uint32_t arqAckRepeatDelay(const Connection& c) {
        return c.arq_.getAckRepeatDelay();
    }

    static void forceCodeRate(Connection& c, CodeRate rate) {
        c.config_.forced_code_rate = rate;
    }

    // §RETX-PACING (docs/RETX_PACING_DESIGN_2026_07_03.md) test hooks.
    static void noteRoundOutcome(Connection& c, int progress_frames, const char* origin) {
        c.noteArqRoundOutcome(progress_frames, origin);
    }

    static int zeroProgressRounds(const Connection& c) {
        return c.zero_progress_rounds_;
    }

    static uint32_t paceHoldMs(const Connection& c) {
        return c.retx_pace_hold_ms_;
    }

    static void pollCollapseEscape(Connection& c) {
        c.maybeCollapseEscape();
    }

    // DESC-SWITCH (docs/MODE_SWITCH_PIGGYBACK_DESIGN_2026_07_03.md Phase 1) hooks.
    static void applyFeedback(Connection& c, float quality) {
        c.applyAdaptiveRateFeedback(quality);
    }

    static bool descSwitchFullAnchorArmed(const Connection& c) {
        return c.desc_switch_full_anchor_pending_;
    }

    static uint8_t arqTxMoveEpoch(const Connection& c) {
        return c.arq_.txMoveEpoch();
    }

    static int dataFrameCWCount(const Connection& c) {
        return c.data_frame_cw_count_;
    }

    // RX-RATE-CMD (Phase 2, ULTRA_RX_RATE_CMD) hooks.
    static uint8_t rxRateCmdPending(const Connection& c) {
        return c.rx_rate_cmd_pending_;
    }
};

} // namespace protocol
} // namespace ultra

namespace {

void test_local_mode_change_ack_reconfigures_arq() {
    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R1_2, 15.0f, 0.30f);
    // R1/2 selects the high-throughput window (16). Since the 2026-07-02 8->16 mask widen
    // the tone-burst SACK cap (kToneBurstAckWindowCapFrames=16) exactly covers it, so the
    // window runs uncapped at 16 (pre-widen it was capped 16->8).
    CHECK(ConnectionAdaptiveTestAccess::arqWindow(c) == connection_policy::kHighThroughputOFDMWindowFrames,
          "R1/2 high-throughput window fits the 16-bit tone-burst SACK mask");
    CHECK(ConnectionAdaptiveTestAccess::arqWindow(c) <= connection_policy::kToneBurstAckWindowCapFrames,
          "in-flight window must never exceed the tone-burst SACK mask cap");

    c.requestModeChange(Modulation::DQPSK, CodeRate::R1_4, 12.0f,
                        v2::ModeChangeReason::CHANNEL_DEGRADED);
    auto ack = v2::ControlFrame::makeAck("K2DEF", "W1ABC", c.getStats().arq.acks_received + 1);
    ack.seq = ConnectionAdaptiveTestAccess::modeChangeSeq(c);
    c.onFrameReceived(ack.serialize());

    CHECK(c.getDataCodeRate() == CodeRate::R1_4, "local MODE_CHANGE ACK should apply pending rate");
    CHECK(ConnectionAdaptiveTestAccess::arqCodeRate(c) == CodeRate::R1_4,
          "local MODE_CHANGE ACK should update ARQ code rate");
    // DQPSK R1/4 is not a high-throughput rung -> the recomputed window is the default
    // wide window (8), below the 16-frame SACK cap (which no longer binds here).
    CHECK(ConnectionAdaptiveTestAccess::arqWindow(c) == connection_policy::kWideOFDMWindowFrames,
          "local MODE_CHANGE ACK should recompute ARQ window (default wide window)");
}

void test_local_mode_change_timeout_keeps_current_arq_mode() {
    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(
        c, CodeRate::R2_3, 20.0f, 0.50f, Modulation::QPSK);

    c.requestModeChange(Modulation::QPSK, CodeRate::R1_2, 20.0f,
                        v2::ModeChangeReason::CHANNEL_DEGRADED);
    const uint32_t retry_ms = ConnectionAdaptiveTestAccess::modeChangeRetryMs(c);

    CHECK(ConnectionAdaptiveTestAccess::modeChangePending(c),
          "test setup should leave MODE_CHANGE pending");
    // 2026-07-03 (post rig-bisect W5/W5b/W6): the retry floors at the FULL burst
    // ACK deadline — the peer may not ACK until its whole outstanding burst is
    // decoded, and faster retries key onto the ACK in flight (W5 livelock, W5b
    // stall; W6 at the full deadline ran clean). The deadline is ratiometric
    // (scales with mod/rate/window); ULTRA_MODE_CHANGE_RETRY_MS pins for A/B.
    CHECK(retry_ms >= ConnectionAdaptiveTestAccess::arqAckTimeout(c),
          "MODE_CHANGE retry must cover the full data-burst ACK deadline");
    CHECK(retry_ms >= 1000,
          "MODE_CHANGE retry must cover a control round trip (anchor+ctl x2)");

    for (int i = 0; i < ConnectionAdaptiveTestAccess::modeChangeMaxRetries() + 1; ++i) {
        c.tick(retry_ms);
    }

    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "unresolved one-phase MODE_CHANGE should release the pending control state");
    CHECK(c.getDataCodeRate() == CodeRate::R2_3,
          "unacknowledged MODE_CHANGE must keep the proven shared data rate");
    CHECK(ConnectionAdaptiveTestAccess::arqCodeRate(c) == CodeRate::R2_3,
          "unacknowledged MODE_CHANGE must not reconfigure local ARQ alone");
}

void test_remote_mode_change_reconfigures_arq() {
    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R1_2, 15.0f, 0.30f);

    auto frame = v2::ControlFrame::makeModeChange(
        "K2DEF", "W1ABC", 44, Modulation::DQPSK, CodeRate::R1_4,
        12.0f, 0.80f, v2::ModeChangeReason::CHANNEL_DEGRADED);
    c.onFrameReceived(frame.serialize());

    CHECK(c.getDataCodeRate() == CodeRate::R1_4, "remote MODE_CHANGE should apply requested rate");
    CHECK(ConnectionAdaptiveTestAccess::arqCodeRate(c) == CodeRate::R1_4,
          "remote MODE_CHANGE should update ARQ code rate");
    // DQPSK R1/4 -> default wide window (8); the 16-frame SACK cap no longer binds
    // (it equaled the recomputed window only while the mask was 8 bits).
    CHECK(ConnectionAdaptiveTestAccess::arqWindow(c) == connection_policy::kWideOFDMWindowFrames,
          "remote MODE_CHANGE should recompute ARQ window (default wide window)");
}

void test_remote_mode_change_ack_repeats_use_ofdm_ack_diversity() {
    Connection c;
    std::vector<Bytes> tx_frames;
    c.setTransmitCallback([&](const Bytes& data) {
        tx_frames.push_back(data);
    });
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(
        c, CodeRate::R2_3, 20.0f, 0.48f, Modulation::QPSK);

    // DATA acks: ONE prompt tone-burst ack per burst (lost acks are backstopped by the
    // sender's ARQ retransmit). But the MODE_CHANGE ACK rides the fragile 1-CW control
    // path and gates every rate move — since 2026-07-03 it gets FADING-AWARE repeats
    // (3 staggered copies on a fading channel; rig measured 5 receptions per climb at
    // single-copy). This Connection has fading 0.48 -> expect 3 MC-ACK copies while the
    // ARQ data-ack count stays 1.
    CHECK(ConnectionAdaptiveTestAccess::arqAckRepeatCount(c) == 1,
          "tone-burst OFDM path uses a single prompt DATA ack (no diversity chain)");
    const int repeat_count = 3;  // fading-aware MC-ACK repeat count at fading >= 0.15

    auto frame = v2::ControlFrame::makeModeChange(
        "K2DEF", "W1ABC", 44, Modulation::QPSK, CodeRate::R1_2,
        19.8f, 0.48f, v2::ModeChangeReason::CHANNEL_DEGRADED);
    c.onFrameReceived(frame.serialize());

    CHECK(tx_frames.size() == 1,
          "remote MODE_CHANGE should send the first ACK immediately");

    const uint32_t drain_ms =
        selective_repeat_arq_policy::ackRepeatDelayForCopy(
            ConnectionAdaptiveTestAccess::arqAckRepeatDelay(c), repeat_count) +
        selective_repeat_arq_policy::kAckRepeatMaxJitterMs;
    c.tick(drain_ms);

    CHECK(tx_frames.size() == static_cast<size_t>(repeat_count),
          "remote MODE_CHANGE ACK should reuse the active OFDM ACK repeat count");
    for (const auto& data : tx_frames) {
        auto ack = v2::ControlFrame::deserialize(data);
        CHECK(ack && ack->type == v2::FrameType::ACK && ack->seq == 44,
              "every MODE_CHANGE ACK diversity copy should acknowledge the request seq");
    }
}

// BUG-MC-RETRY-SPURIOUS fix 1: the MODE_CHANGE retry deadline HOLDS while our own
// TX is keyed — half-duplex means keyed time cannot be ACK-loss evidence (rig E1:
// the frame rode the tail of a ~10.6 s bundled key-down, so the request-anchored
// 18.2 s deadline lost to the 21-30 s pipeline and retried spuriously EVERY trough
// exchange). With the host provider reporting keyed, ticks must neither decrement
// nor fire the deadline; on key-up the deadline resumes from where it held.
void test_mode_change_retry_holds_while_tx_keyed() {
    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(
        c, CodeRate::R2_3, 20.0f, 0.50f, Modulation::QPSK);

    bool tx_keyed = true;
    c.setTxActiveProvider([&tx_keyed] { return tx_keyed; });

    std::vector<Bytes> tx_frames;
    c.setTransmitCallback([&](const Bytes& d) { tx_frames.push_back(d); });

    c.requestModeChange(Modulation::QPSK, CodeRate::R1_2, 20.0f,
                        v2::ModeChangeReason::CHANNEL_DEGRADED);
    const uint32_t retry_ms = ConnectionAdaptiveTestAccess::modeChangeRetryMs(c);
    const size_t frames_after_request = tx_frames.size();

    // Keyed: three full deadlines elapse — the deadline must HOLD (no retry).
    for (int i = 0; i < 3; ++i) c.tick(retry_ms);
    CHECK(tx_frames.size() == frames_after_request,
          "keyed TX must hold the MODE_CHANGE retry deadline (no spurious retry)");
    CHECK(ConnectionAdaptiveTestAccess::modeChangePending(c),
          "the exchange must stay pending across the hold");

    // Key-up: the deadline resumes and one full deadline fires exactly one retry.
    tx_keyed = false;
    c.tick(retry_ms);
    CHECK(tx_frames.size() == frames_after_request + 1,
          "after key-up one elapsed deadline must fire exactly one retry");
}

// BUG-MC-RETRY-SPURIOUS fix 3: a re-arriving copy of an ALREADY-APPLIED MODE_CHANGE
// (sender diversity copy or its request-time-anchored spurious retry — rig E1/D1/D3:
// a retry fired EVERY trough exchange although copy #1 was ACKed) must not re-apply
// the mode, must not re-notify the GUI (the operator-visible duplicate [MODE] lines),
// and must not schedule a fresh fading-aware repeat set. The duplicate still carries
// information — the sender may have missed our ACKs — so the calibrated response is
// exactly ONE re-ACK copy per duplicate reception.
void test_duplicate_mode_change_single_reack_no_reapply() {
    Connection c;
    std::vector<Bytes> tx_frames;
    c.setTransmitCallback([&](const Bytes& data) {
        tx_frames.push_back(data);
    });
    int notify_count = 0;
    c.setDataModeChangedCallback([&](Modulation, CodeRate, int, float, float,
                                     int, int, bool) {
        ++notify_count;
    });
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(
        c, CodeRate::R2_3, 20.0f, 0.48f, Modulation::QPSK);

    const int repeat_count = 3;  // fading-aware MC-ACK repeat count at fading >= 0.15
    const uint32_t drain_ms =
        selective_repeat_arq_policy::ackRepeatDelayForCopy(
            ConnectionAdaptiveTestAccess::arqAckRepeatDelay(c), repeat_count) +
        selective_repeat_arq_policy::kAckRepeatMaxJitterMs;

    auto frame = v2::ControlFrame::makeModeChange(
        "K2DEF", "W1ABC", 44, Modulation::QPSK, CodeRate::R1_2,
        19.8f, 0.48f, v2::ModeChangeReason::CHANNEL_DEGRADED);

    // First copy: full behavior — apply + one GUI notify + full fading-aware ACK set.
    c.onFrameReceived(frame.serialize());
    CHECK(c.getDataCodeRate() == CodeRate::R1_2,
          "first MODE_CHANGE copy should apply the requested rate");
    CHECK(notify_count == 1, "first MODE_CHANGE copy should notify the GUI once");
    CHECK(tx_frames.size() == 1, "first MODE_CHANGE copy should ACK immediately");
    c.tick(drain_ms);
    CHECK(tx_frames.size() == static_cast<size_t>(repeat_count),
          "first MODE_CHANGE copy should emit the full fading-aware ACK set");

    // Duplicate copy (same seq, same mod/rate): exactly ONE re-ACK, nothing else.
    c.onFrameReceived(frame.serialize());
    CHECK(tx_frames.size() == static_cast<size_t>(repeat_count) + 1,
          "duplicate MODE_CHANGE should emit exactly one re-ACK copy");
    auto dup_ack = v2::ControlFrame::deserialize(tx_frames.back());
    CHECK(dup_ack && dup_ack->type == v2::FrameType::ACK && dup_ack->seq == 44,
          "the duplicate's re-ACK should acknowledge the request seq");
    CHECK(notify_count == 1, "duplicate MODE_CHANGE must not re-notify the GUI");
    CHECK(c.getDataCodeRate() == CodeRate::R1_2,
          "duplicate MODE_CHANGE must leave the applied mode untouched");
    CHECK(ConnectionAdaptiveTestAccess::arqCodeRate(c) == CodeRate::R1_2,
          "duplicate MODE_CHANGE must not re-run applyDataMode/ARQ reconfig");
    c.tick(drain_ms);
    CHECK(tx_frames.size() == static_cast<size_t>(repeat_count) + 1,
          "duplicate MODE_CHANGE must not schedule a fresh ACK repeat set");

    // Every further duplicate reception earns one more diversity re-ACK.
    c.onFrameReceived(frame.serialize());
    CHECK(tx_frames.size() == static_cast<size_t>(repeat_count) + 2,
          "each duplicate reception should earn exactly one re-ACK copy");
    CHECK(notify_count == 1, "repeated duplicates must stay notify-silent");

    // A genuinely NEW request (fresh seq) applies normally again.
    const size_t before_new = tx_frames.size();
    auto next = v2::ControlFrame::makeModeChange(
        "K2DEF", "W1ABC", 45, Modulation::QPSK, CodeRate::R2_3,
        20.2f, 0.48f, v2::ModeChangeReason::CHANNEL_IMPROVED);
    c.onFrameReceived(next.serialize());
    CHECK(c.getDataCodeRate() == CodeRate::R2_3,
          "a new MODE_CHANGE seq should apply normally after a dedup");
    CHECK(notify_count == 2, "a new MODE_CHANGE seq should notify the GUI again");
    CHECK(tx_frames.size() == before_new + 1,
          "a new MODE_CHANGE seq should ACK immediately");

    // Seq-reuse guard: SAME seq but a DIFFERENT (mod, rate) tuple is NOT a duplicate
    // (a restarted peer restarts its seq counter) — it must be applied, not deduped.
    auto reused_seq = v2::ControlFrame::makeModeChange(
        "K2DEF", "W1ABC", 45, Modulation::QPSK, CodeRate::R1_2,
        18.0f, 0.48f, v2::ModeChangeReason::CHANNEL_DEGRADED);
    c.onFrameReceived(reused_seq.serialize());
    CHECK(c.getDataCodeRate() == CodeRate::R1_2,
          "same seq with a different mode tuple must apply (seq-reuse guard)");
    CHECK(notify_count == 3,
          "same seq with a different mode tuple must notify (it is a new request)");
}

void test_wide_ofdm_configures_short_tail_sack_delay() {
    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R1_4, 10.0f, 0.05f);

    const uint32_t sliding_delay = ConnectionAdaptiveTestAccess::arqSackDelay(c);
    const uint32_t tail_delay = ConnectionAdaptiveTestAccess::arqSackDelayShort(c);
    const uint32_t physical_delay = connection_policy::wideOFDMSackDelayMs(
        Modulation::DQPSK, CodeRate::R1_4,
        ConnectionAdaptiveTestAccess::arqWindow(c), v2::kDefaultFixedFrameCodewords);

    CHECK(tail_delay == connection_policy::wideOFDMSackTailDelayMs(),
          "wide OFDM should use the derived short SACK delay at stream tail");
    CHECK(tail_delay == connection_policy::kCarrierSenseSackCoalesceMs,
          "wide OFDM tail SACK delay should be carrier-sense coalescing only");
    CHECK(sliding_delay == connection_policy::wideOFDMSlidingSackDelayMs(
                               Modulation::DQPSK, CodeRate::R1_4),
          "wide OFDM in-stream SACK delay should be a data/ACK-airtime-derived quiet interval");
    CHECK(ConnectionAdaptiveTestAccess::arqSackDelaySlidesOnData(c),
          "wide OFDM should re-arm the SACK quiet timer on each decoded DATA frame");
    CHECK(physical_delay > sliding_delay && sliding_delay > tail_delay,
          "wide OFDM should retain physical RTO coverage while ACKing burst tails promptly");
    CHECK(ConnectionAdaptiveTestAccess::arqAckTimeout(c) ==
              connection_policy::computeWideOFDMAckTimeoutMs(
                  Modulation::DQPSK, CodeRate::R1_4,
                  ConnectionAdaptiveTestAccess::arqWindow(c),
                  sliding_delay,
                  ConnectionAdaptiveTestAccess::arqAckRepeatCount(c),
                  v2::kDefaultFixedFrameCodewords),
          "wide OFDM ACK timeout should remain derived from the long physical SACK hold");
}

void test_accepted_ofdm_data_sync_keeps_connect_ack_rescue_armed() {
    Connection c;
    ConnectionAdaptiveTestAccess::makeResponderWithConnectAckRescue(c);

    CHECK(ConnectionAdaptiveTestAccess::connectAckRescueArmed(c),
          "responder CONNECT_ACK rescue should start armed");
    c.onAcceptedOFDMDataSync(0.90f);
    CHECK(ConnectionAdaptiveTestAccess::connectAckRescueArmed(c),
          "accepted OFDM DATA sync alone must not clear CONNECT_ACK rescue before a decoded initiator frame");
}

void test_accepted_ofdm_data_sync_does_not_clear_non_ofdm_rescue() {
    Connection c;
    ConnectionAdaptiveTestAccess::makeResponderWithConnectAckRescue(c, WaveformMode::MC_DPSK);

    c.onAcceptedOFDMDataSync(0.90f);
    CHECK(ConnectionAdaptiveTestAccess::connectAckRescueArmed(c),
          "accepted OFDM DATA sync hook should not clear non-OFDM rescue state");
}

void test_duplicate_connect_replays_cached_connect_ack_without_confirming() {
    Connection c;
    std::vector<Bytes> tx_frames;
    c.setTransmitCallback([&](const Bytes& data) {
        tx_frames.push_back(data);
    });
    ConnectionAdaptiveTestAccess::makeResponderWithConnectAckRescue(c);

    auto duplicate_connect = v2::ConnectFrame::makeConnect(
        "W1ABC", "K2DEF",
        ModeCapabilities::ALL | ModeCapabilities::PHY_MASK_V1,
        static_cast<uint8_t>(WaveformMode::AUTO));

    c.onFrameReceived(duplicate_connect.serialize());

    CHECK(tx_frames.size() == 1,
          "duplicate CONNECT from the same unconfirmed peer should replay CONNECT_ACK");
    CHECK(ConnectionAdaptiveTestAccess::connectAckRescueArmed(c),
          "duplicate CONNECT must not clear cached CONNECT_ACK rescue state");
    CHECK(!ConnectionAdaptiveTestAccess::handshakeConfirmed(c),
          "duplicate CONNECT means CONNECT_ACK was lost and must not confirm responder handshake");
}

void test_connect_retry_interval_is_control_airtime_derived() {
    Connection c;
    c.setLocalCallsign("W1ABC");
    std::vector<Bytes> tx_frames;
    c.setTransmitCallback([&](const Bytes& data) {
        tx_frames.push_back(data);
    });

    CHECK(c.connect("K2DEF"), "connect should start without a ping callback");
    CHECK(tx_frames.size() == 1, "connect fallback should send initial CONNECT");

    const uint32_t retry_ms = ConnectionAdaptiveTestAccess::connectRetryInterval(c);
    const uint32_t ack_retx_ms = ConnectionAdaptiveTestAccess::connectAckRetransmitMs(c);
    CHECK(retry_ms > 0, "CONNECT retry interval should be derived from control airtime");
    CHECK(ack_retx_ms > 0 && ack_retx_ms < retry_ms,
          "responder CONNECT_ACK rescue should be airtime-derived and faster than full initiator retry");

    c.tick(retry_ms - 1);
    CHECK(tx_frames.size() == 1, "CONNECT should not retry before the airtime-derived interval");

    c.tick(1);
    CHECK(tx_frames.size() == 2, "CONNECT should retry at the airtime-derived interval");
}

void test_responder_handshake_timer_does_not_false_confirm() {
    Connection c;
    int confirmed_callbacks = 0;
    c.setHandshakeConfirmedCallback([&]() {
        ++confirmed_callbacks;
    });
    ConnectionAdaptiveTestAccess::makeResponderWithConnectAckRescue(c);
    ConnectionAdaptiveTestAccess::clearConnectAckRetxBudget(c);
    ConnectionAdaptiveTestAccess::setResponderHandshakeWait(c, 1);

    c.tick(1);

    CHECK(!ConnectionAdaptiveTestAccess::handshakeConfirmed(c),
          "responder timer alone must not confirm a half-connected handshake");
    CHECK(confirmed_callbacks == 0,
          "responder timer must not switch TX to connected waveform without an initiator frame");
}

// §RETX-PACING round accounting (docs/RETX_PACING_DESIGN_2026_07_03.md §1.1/§2.3), with
// both knobs pinned OFF in main(): counting is inert bookkeeping — no hold armed, no ARQ
// timer touched, no escape fired (the knob-off byte-identical contract) — and the
// g42-PROTECTIVE property holds: ANY progress resets the zero-round streak, and a
// duplicate/stale ack (progress −1) is never a round.
void test_zero_progress_round_counter_knob_off_and_g42_protective() {
    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(
        c, CodeRate::R2_3, 20.0f, 0.40f, Modulation::QPSK);
    TempPayloadFile payload("retx_pacing_rounds", 4096);
    CHECK(payload.dir.valid() && !payload.path.empty(),
          "temp payload file should be created");
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);
    ConnectionAdaptiveTestAccess::fillArqWindow(c, 4);

    const uint32_t rto_before = ConnectionAdaptiveTestAccess::arqAckTimeout(c);

    // Two consecutive zero-progress rounds accumulate (scope: CONNECTED wideband OFDM,
    // file SENDING, in-flight bytes — all true here).
    ConnectionAdaptiveTestAccess::noteRoundOutcome(c, 0, "test");
    ConnectionAdaptiveTestAccess::noteRoundOutcome(c, 0, "test");
    CHECK(ConnectionAdaptiveTestAccess::zeroProgressRounds(c) == 2,
          "two zero-progress rounds should accumulate");
    CHECK(ConnectionAdaptiveTestAccess::paceHoldMs(c) == 0,
          "ULTRA_RETX_TROUGH_PACING=0: no pacing hold may be armed (byte-identical)");
    CHECK(ConnectionAdaptiveTestAccess::arqAckTimeout(c) == rto_before,
          "knob-off: the ARQ ack timeout base must be untouched");

    // ULTRA_COLLAPSE_ESCAPE_ROUNDS=0: polling the escape must never fire a MODE_CHANGE.
    ConnectionAdaptiveTestAccess::pollCollapseEscape(c);
    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "ULTRA_COLLAPSE_ESCAPE_ROUNDS=0: no collapse escape may fire (byte-identical)");
    CHECK(c.getDataCodeRate() == CodeRate::R2_3,
          "knob-off: the data rate must not move");

    // g42-protective (§2.3): a round in which ANY frame progressed resets the streak —
    // a lone straggler retrying amid deliveries can never accumulate rounds.
    ConnectionAdaptiveTestAccess::noteRoundOutcome(c, 0, "test");
    ConnectionAdaptiveTestAccess::noteRoundOutcome(c, 1, "test");
    CHECK(ConnectionAdaptiveTestAccess::zeroProgressRounds(c) == 0,
          "any delivered/SACKed frame must reset the zero-round streak");

    // §1.1 dedup: progress −1 (duplicate/stale ack — no fresh ack processed) is NOT a round.
    ConnectionAdaptiveTestAccess::noteRoundOutcome(c, 0, "test");
    ConnectionAdaptiveTestAccess::noteRoundOutcome(c, -1, "test");
    CHECK(ConnectionAdaptiveTestAccess::zeroProgressRounds(c) == 1,
          "a duplicate/stale ack (progress -1) must not create or reset a round");
}

// ─────────── DESC-SWITCH (docs/MODE_SWITCH_PIGGYBACK_DESIGN_2026_07_03.md Phase 1) ───────────

int countModeChangeFrames(const std::vector<Bytes>& frames) {
    int n = 0;
    for (const auto& f : frames) {
        auto hdr = v2::parseHeader(f);
        if (hdr.valid && hdr.type == v2::FrameType::MODE_CHANGE) ++n;
    }
    return n;
}

// Knob-OFF identity pin: ULTRA_DESCRIPTOR_MODE_SWITCH=0 (the baseline pinned in main)
// must route a clean-boundary ladder move through the legacy synchronized MODE_CHANGE
// exchange — pending state armed, mode held until ACK, exactly one MODE_CHANGE frame on
// the wire, zero descriptor commits — and the RX-side notification must be a no-op.
void test_descriptor_switch_knob_off_is_byte_identical() {
    Connection c;
    std::vector<Bytes> tx_frames;
    c.setTransmitCallback([&](const Bytes& d) { tx_frames.push_back(d); });
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(
        c, CodeRate::R2_3, 20.0f, 0.30f, Modulation::QAM16);
    TempPayloadFile payload("desc_switch_off", 60 * 1024);
    CHECK(payload.dir.valid() && !payload.path.empty(),
          "temp payload file should be created");
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);

    // QAM16 NACK (quality 0) at a clean send boundary -> prompt demote decision.
    ConnectionAdaptiveTestAccess::applyFeedback(c, 0.0f);

    CHECK(ConnectionAdaptiveTestAccess::modeChangePending(c),
          "knob-off: the demote must arm the MODE_CHANGE stop-and-wait");
    CHECK(c.getDataModulation() == Modulation::QAM16 &&
              c.getDataCodeRate() == CodeRate::R2_3,
          "knob-off: the local mode must be HELD until the peer ACKs");
    CHECK(countModeChangeFrames(tx_frames) == 1,
          "knob-off: exactly one MODE_CHANGE control frame goes on the wire");
    CHECK(c.getStats().descriptor_mode_switches == 0,
          "knob-off: no descriptor commit may be counted");
    CHECK(!ConnectionAdaptiveTestAccess::descSwitchFullAnchorArmed(c),
          "knob-off: the descriptor-switch full-anchor one-shot must stay unarmed");

    // RX-side notification is a hard no-op with the knob off.
    Connection r;
    std::vector<Bytes> r_tx;
    r.setTransmitCallback([&](const Bytes& d) { r_tx.push_back(d); });
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(
        r, CodeRate::R2_3, 20.0f, 0.30f, Modulation::QPSK);
    r.onDescriptorModeChange(Modulation::QAM16, CodeRate::R2_3, 8);
    CHECK(r.getDataModulation() == Modulation::QPSK,
          "knob-off: onDescriptorModeChange must not touch the data mode");
    CHECK(r_tx.empty(), "knob-off: onDescriptorModeChange must transmit nothing");
    CHECK(r.getStats().descriptor_mode_switches == 0,
          "knob-off: no adopt may be counted");
}

// Knob-ON sender commit: a clean-boundary ladder move commits LOCALLY — mode applied
// immediately (ARQ included), NO mode_change_pending_, NO MODE_CHANGE frame on the
// wire, the full-anchor one-shot armed for the next burst group, and no epoch bump
// (an EMPTY window has nothing to abort — the descriptor + EPOCH_REBASE flag suffice).
void test_descriptor_switch_commits_locally_at_clean_boundary() {
    setenv("ULTRA_DESCRIPTOR_MODE_SWITCH", "1", 1);
    Connection c;  // ctor latches knob ON for this instance
    setenv("ULTRA_DESCRIPTOR_MODE_SWITCH", "0", 1);  // restore the pinned baseline

    std::vector<Bytes> tx_frames;
    c.setTransmitCallback([&](const Bytes& d) { tx_frames.push_back(d); });
    std::vector<bool> burst_full_anchor;
    std::vector<size_t> burst_sizes;
    std::vector<uint8_t> burst_reasons;
    c.setTransmitBurstCallback([&](const std::vector<Bytes>& frames, uint16_t /*seq*/,
                                   uint8_t anchor_reason) {
        burst_sizes.push_back(frames.size());
        // Any non-None reason still means "this burst carries a full anchor"; the
        // reason only distinguishes WHY (resend vs config switch) for the skip streak.
        burst_full_anchor.push_back(anchor_reason != Connection::kAnchorReasonNone);
        burst_reasons.push_back(anchor_reason);
    });
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(
        c, CodeRate::R2_3, 20.0f, 0.30f, Modulation::QAM16);
    TempPayloadFile payload("desc_switch_on", 60 * 1024);
    CHECK(payload.dir.valid() && !payload.path.empty(),
          "temp payload file should be created");
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);
    const uint8_t epoch_before = ConnectionAdaptiveTestAccess::arqTxMoveEpoch(c);

    // QAM16 NACK at a clean boundary -> demote decision -> descriptor commit.
    ConnectionAdaptiveTestAccess::applyFeedback(c, 0.0f);

    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "commit must not arm the MODE_CHANGE stop-and-wait (no TX freeze)");
    CHECK(c.getDataModulation() == Modulation::QPSK &&
              c.getDataCodeRate() == CodeRate::R3_4,
          "commit applies the new mode immediately (sender-local)");
    CHECK(ConnectionAdaptiveTestAccess::arqCodeRate(c) == CodeRate::R3_4,
          "commit must reconfigure the ARQ to the new rate");
    CHECK(countModeChangeFrames(tx_frames) == 0,
          "knob-on: NO MODE_CHANGE control frame may go on the wire");
    CHECK(c.getStats().descriptor_mode_switches == 1,
          "commit must count in descriptor_mode_switches");
    CHECK(ConnectionAdaptiveTestAccess::arqTxMoveEpoch(c) == epoch_before,
          "clean-boundary commit: EMPTY window -> no ARQ abort -> no epoch bump");
    // F163 REVERSAL (2026-07-06): warm state still carries within-grid, but the
    // switch DESCRIPTOR itself must ride a full anchor — three light-descriptor
    // switches were missed on fading (25 s adoption latency each, ~130 s of the
    // 424 s transfer). Every commit now arms the full anchor.
    // 2026-07-26 REASON PLUMBING: a descriptor mode/rate switch must be reported as
    // kAnchorReasonModeSwitch, NOT as a resend. The encoder uses the distinction to
    // decide whether the #69 anchor-skip clean streak (DELIVERY evidence) recools; a
    // config switch needs the chirp but is not evidence the channel stopped syncing.
    // Reporting it as a resend is what made rate changes the dominant streak-resetter.
    for (size_t i = 0; i < burst_reasons.size(); ++i) {
        if (burst_reasons[i] != Connection::kAnchorReasonNone) {
            CHECK(burst_reasons[i] == Connection::kAnchorReasonModeSwitch,
                  "a descriptor switch's full anchor must carry kAnchorReasonModeSwitch "
                  "(got a resend reason -> the skip streak would recool on a config event)");
        }
    }
    bool full_anchor_armed_or_consumed =
        ConnectionAdaptiveTestAccess::descSwitchFullAnchorArmed(c);
    for (size_t i = 0; i < burst_full_anchor.size(); ++i) {
        if (burst_sizes[i] >= 2 && burst_full_anchor[i]) {
            full_anchor_armed_or_consumed = true;
        }
    }
    CHECK(full_anchor_armed_or_consumed,
          "every DESC-SWITCH commit must arm the full anchor (F163: missed "
          "light switch descriptors cost 25 s each)");
}

// Knob-ON receiver adopt: a mode-hop descriptor notification runs the RX-relevant
// subset of applyDataMode (mode + CW + ARQ reconfig + GUI notify) and must NOT send
// any ACK (no MODE_CHANGE ACK machinery); a re-announced descriptor is idempotent,
// and a receiver with its OWN data in flight skips the adopt (ISS asymmetry guard).
void test_descriptor_adopt_reconfigures_receiver_without_ack() {
    setenv("ULTRA_DESCRIPTOR_MODE_SWITCH", "1", 1);
    Connection c;  // ctor latches knob ON for this instance
    setenv("ULTRA_DESCRIPTOR_MODE_SWITCH", "0", 1);  // restore the pinned baseline

    std::vector<Bytes> tx_frames;
    c.setTransmitCallback([&](const Bytes& d) { tx_frames.push_back(d); });
    int notify_count = 0;
    Modulation notified_mod = Modulation::AUTO;
    CodeRate notified_rate = CodeRate::AUTO;
    c.setDataModeChangedCallback([&](Modulation mod, CodeRate rate, int /*cw*/,
                                     float /*snr*/, float /*fading*/, int /*carriers*/,
                                     int /*sps*/, bool /*wire*/) {
        ++notify_count;
        notified_mod = mod;
        notified_rate = rate;
    });
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(
        c, CodeRate::R2_3, 20.0f, 0.30f, Modulation::QPSK);

    c.onDescriptorModeChange(Modulation::QAM16, CodeRate::R2_3, 8);

    CHECK(c.getDataModulation() == Modulation::QAM16 &&
              c.getDataCodeRate() == CodeRate::R2_3,
          "adopt must apply the descriptor's announced mode at the protocol layer");
    CHECK(ConnectionAdaptiveTestAccess::dataFrameCWCount(c) == 8,
          "adopt must apply the descriptor's announced CW count");
    CHECK(tx_frames.empty(),
          "adopt must transmit NOTHING (no MODE_CHANGE ACK machinery)");
    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "adopt must not arm any pending mode-change state");
    CHECK(c.getStats().descriptor_mode_switches == 1, "adopt must count once");
    CHECK(notify_count == 1 && notified_mod == Modulation::QAM16 &&
              notified_rate == CodeRate::R2_3,
          "adopt must fire the data-mode-changed notify (GUI/modem follow-through)");

    // Idempotent: the resend's re-announced descriptor must be a no-op.
    c.onDescriptorModeChange(Modulation::QAM16, CodeRate::R2_3, 8);
    CHECK(c.getStats().descriptor_mode_switches == 1,
          "re-announced descriptor must not double-adopt");
    CHECK(notify_count == 1, "re-announced descriptor must not re-notify");

    // ISS asymmetry guard: with our OWN data in flight the adopt is skipped.
    ConnectionAdaptiveTestAccess::fillArqWindow(c, 1);
    c.onDescriptorModeChange(Modulation::QPSK, CodeRate::R3_4, 8);
    CHECK(c.getDataModulation() == Modulation::QAM16 &&
              c.getDataCodeRate() == CodeRate::R2_3,
          "adopt with local DATA in flight must be skipped (per-direction rungs)");
    CHECK(c.getStats().descriptor_mode_switches == 1,
          "skipped adopt must not count");
}

// ─────────── RX-RATE-CMD (docs/MODE_SWITCH_PIGGYBACK_DESIGN_2026_07_03.md Phase 2) ───────────

using ultra::waveform::tone_burst_ack::AckType;
using ultra::waveform::tone_burst_ack::ToneBurstAckDetection;
using ultra::waveform::tone_burst_ack::ToneBurstAckPayload;
using ultra::waveform::tone_burst_ack::kRungCmdDownHard;
using ultra::waveform::tone_burst_ack::kRungCmdNone;

ToneBurstAckDetection makeRungCmdDetection(uint8_t group_seq, uint8_t rung_cmd) {
    ToneBurstAckDetection d;
    d.payload.group_seq = group_seq;
    d.payload.frame_mask = 0;               // total crater: nothing delivered
    d.payload.rate_hint = 0;                // quality 0 (the crater's quantized grade)
    d.payload.type = AckType::Ack;
    d.payload.move_epoch = 0;               // matches the sender's initial TX epoch
    d.payload.rung_cmd = rung_cmd;
    return d;
}

// Knob-OFF identity pin (ULTRA_RX_RATE_CMD=0, the baseline pinned in main): the
// receiver's emitted tone-ACK carries rung_cmd 0 even for a QAM16 crater, and the
// sender ignores an incoming DOWN-hard command outright — no mode move, no
// MODE_CHANGE frame, no descriptor commit.
void test_rx_rate_cmd_knob_off_is_byte_identical() {
    // Receiver emit side: a total QAM16 crater must NOT set the command.
    Connection r;
    std::vector<ToneBurstAckPayload> acks;
    r.setTransmitToneBurstAckCallback(
        [&](const ToneBurstAckPayload& p) { acks.push_back(p); });
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(
        r, CodeRate::R2_3, 20.0f, 0.30f, Modulation::QAM16);
    r.onBurstGroupReceived(3, {}, /*all_ok=*/false, /*quality=*/0.0f,
                           /*frame_mask=*/0, /*interleaved=*/true, /*group_size=*/8);
    CHECK(!acks.empty(), "crater group must still emit its tone-burst ACK");
    CHECK(acks.back().rung_cmd == kRungCmdNone,
          "knob-off: the emitted rung_cmd bits must stay 0 (byte-identical wire)");
    CHECK(ConnectionAdaptiveTestAccess::rxRateCmdPending(r) == 0,
          "knob-off: no standing command may be latched");

    // Sender consume side: an incoming DOWN-hard command must be ignored.
    Connection c;
    std::vector<Bytes> tx_frames;
    c.setTransmitCallback([&](const Bytes& d) { tx_frames.push_back(d); });
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(
        c, CodeRate::R2_3, 20.0f, 0.30f, Modulation::QAM16);
    TempPayloadFile payload("rx_rate_cmd_off", 60 * 1024);
    CHECK(payload.dir.valid() && !payload.path.empty(),
          "temp payload file should be created");
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);
    ConnectionAdaptiveTestAccess::fillArqWindow(c, 4);

    c.onToneBurstAck(makeRungCmdDetection(/*group_seq=*/1, kRungCmdDownHard));

    CHECK(c.getDataModulation() == Modulation::QAM16 &&
              c.getDataCodeRate() == CodeRate::R2_3,
          "knob-off: an incoming rung command must not move the mode");
    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "knob-off: an incoming rung command must not arm a MODE_CHANGE");
    CHECK(countModeChangeFrames(tx_frames) == 0,
          "knob-off: no MODE_CHANGE frame may go on the wire");
    CHECK(c.getStats().descriptor_mode_switches == 0,
          "knob-off: no descriptor commit may be counted");
}

// Knob-ON receiver emit: a TOTAL crater (frame_mask == 0) at QAM16 sets DOWN-hard on
// the group's ACK; the SAME command re-rides subsequent ACKs of the unchanged state
// (ACK-loss diversity, sender dedups by group_seq); the observed adoption (a real
// mod/rate change through applyDataMode) clears it; a group that delivers frames
// clears it; a crater at a non-QAM16 mode commands nothing (deep null at a robust
// rung is irreducible fading, not a rate signal — the 2026-06-09 ratchet guard).
void test_rx_rate_cmd_receiver_emits_crater_down_hard_once_per_move() {
    setenv("ULTRA_RX_RATE_CMD", "1", 1);
    setenv("ULTRA_DESCRIPTOR_MODE_SWITCH", "1", 1);  // adoption path for the clear
    Connection r;  // ctor latches both knobs ON for this instance
    setenv("ULTRA_DESCRIPTOR_MODE_SWITCH", "0", 1);  // restore the pinned baseline
    setenv("ULTRA_RX_RATE_CMD", "0", 1);

    std::vector<ToneBurstAckPayload> acks;
    r.setTransmitToneBurstAckCallback(
        [&](const ToneBurstAckPayload& p) { acks.push_back(p); });
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(
        r, CodeRate::R2_3, 20.0f, 0.30f, Modulation::QAM16);

    // Total crater at QAM16 -> DOWN-hard rides this group's ACK.
    r.onBurstGroupReceived(5, {}, false, 0.0f, 0, true, 8);
    CHECK(!acks.empty() && acks.back().rung_cmd == kRungCmdDownHard,
          "QAM16 total crater must command DOWN-hard on the group's own ACK");

    // Unchanged state -> the SAME standing command re-rides (idempotent diversity;
    // the ARQ base is frozen during a crater so the sender dedups all copies).
    r.onBurstGroupReceived(5, {}, false, 0.0f, 0, true, 8);
    CHECK(acks.back().rung_cmd == kRungCmdDownHard,
          "repeated crater ACKs re-carry the same standing command");

    // Sender's adoption observed (descriptor announces QPSK R3/4) -> latch clears.
    r.onDescriptorModeChange(Modulation::QPSK, CodeRate::R3_4, 8);
    CHECK(ConnectionAdaptiveTestAccess::rxRateCmdPending(r) == 0,
          "an applied mod/rate change is the adoption — the command latch must clear");
    r.onBurstGroupReceived(6, {}, false, 0.0f, 0, true, 8);
    CHECK(acks.back().rung_cmd == kRungCmdNone,
          "post-adoption crater at QPSK must NOT command (non-QAM16 = fade physics)");

    // A delivering group ends any crater state (fresh QAM16 instance).
    setenv("ULTRA_RX_RATE_CMD", "1", 1);
    Connection r2;
    setenv("ULTRA_RX_RATE_CMD", "0", 1);
    std::vector<ToneBurstAckPayload> acks2;
    r2.setTransmitToneBurstAckCallback(
        [&](const ToneBurstAckPayload& p) { acks2.push_back(p); });
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(
        r2, CodeRate::R2_3, 20.0f, 0.30f, Modulation::QAM16);
    r2.onBurstGroupReceived(9, {}, false, 0.0f, 0, true, 8);
    CHECK(ConnectionAdaptiveTestAccess::rxRateCmdPending(r2) == kRungCmdDownHard,
          "crater must latch the command");
    // PARTIAL-CRATER policy (2026-07-04, F27): a failed-but-partial group after a
    // crater is the SECOND consecutive bad group — the rung is under water; the
    // command steps to DOWN-ONE (one rung per evidence quantum) instead of clearing.
    // Only a CLEAN group ends the bad stretch.
    r2.onBurstGroupReceived(10, {}, false, 0.0f, /*frame_mask=*/0x3, true, 8);
    CHECK(ConnectionAdaptiveTestAccess::rxRateCmdPending(r2) == ultra::waveform::tone_burst_ack::kRungCmdDownOne,
          "second consecutive failed group (partial) must command DOWN-ONE");
    r2.onBurstGroupReceived(11, {}, true, 0.95f, /*frame_mask=*/0xFF, true, 8);
    CHECK(ConnectionAdaptiveTestAccess::rxRateCmdPending(r2) == 0,
          "a CLEAN group ends the bad stretch — no stale command may ride");
}

// Knob-ON sender consume, MID-WINDOW (the Phase-2 case): a DOWN-hard command with
// frames in flight routes the escape target (QAM16 -> QPSK R3/4) through the
// DESCRIPTOR commit — no MODE_CHANGE frame, mode applied immediately, and the ARQ
// abort bumps the move-epoch (era safety; requires ULTRA_ARQ_MOVE_EPOCH ON). A
// duplicate of the same command (same group_seq) before adoption is a no-op.
void test_rx_rate_cmd_down_hard_mid_window_commits_via_descriptor_with_epoch() {
    setenv("ULTRA_ARQ_MOVE_EPOCH", "1", 1);
    setenv("ULTRA_DESCRIPTOR_MODE_SWITCH", "1", 1);
    setenv("ULTRA_RX_RATE_CMD", "1", 1);
    Connection c;  // ctor latches all three knobs ON for this instance
    setenv("ULTRA_RX_RATE_CMD", "0", 1);  // restore the pinned baselines
    setenv("ULTRA_DESCRIPTOR_MODE_SWITCH", "0", 1);
    unsetenv("ULTRA_ARQ_MOVE_EPOCH");

    std::vector<Bytes> tx_frames;
    c.setTransmitCallback([&](const Bytes& d) { tx_frames.push_back(d); });
    std::vector<bool> burst_full_anchor;
    std::vector<size_t> burst_sizes;
    c.setTransmitBurstCallback([&](const std::vector<Bytes>& frames, uint16_t /*seq*/,
                                   bool force_full_preamble) {
        burst_sizes.push_back(frames.size());
        burst_full_anchor.push_back(force_full_preamble);
    });
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(
        c, CodeRate::R2_3, 20.0f, 0.30f, Modulation::QAM16);
    TempPayloadFile payload("rx_rate_cmd_mid", 60 * 1024);
    CHECK(payload.dir.valid() && !payload.path.empty(),
          "temp payload file should be created");
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);
    ConnectionAdaptiveTestAccess::fillArqWindow(c, 4);  // MID-window: frames in flight
    CHECK(ConnectionAdaptiveTestAccess::arqTxMoveEpoch(c) == 0,
          "fresh connection starts at TX move-epoch 0");

    // The crater group's ACK arrives carrying DOWN-hard (epoch echo 0 = current era).
    c.onToneBurstAck(makeRungCmdDetection(/*group_seq=*/1, kRungCmdDownHard));

    // ULTRA_QAM16_DEMOTE_MIDRUNG DEFAULT-ON (2026-07-05): the first DOWN-hard from
    // 16QAM R2/3 lands at the 16QAM R1/2 midrung (2x margin, stays on the mod); a
    // second escape takes the QPSK R3/4 exit.
    CHECK(c.getDataModulation() == Modulation::QAM16 &&
              c.getDataCodeRate() == CodeRate::R1_2,
          "DOWN-hard at QAM16 must commit the midrung landing (16QAM R1/2) immediately");
    CHECK(ConnectionAdaptiveTestAccess::arqCodeRate(c) == CodeRate::R1_2,
          "the ARQ must be reconfigured to the committed rate");
    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "the descriptor commit must not arm the MODE_CHANGE stop-and-wait");
    CHECK(countModeChangeFrames(tx_frames) == 0,
          "NO MODE_CHANGE control frame may go on the wire (that was the dead-air)");
    CHECK(c.getStats().descriptor_mode_switches == 1,
          "the commit must count as a descriptor switch");
    CHECK(ConnectionAdaptiveTestAccess::arqTxMoveEpoch(c) == 1,
          "the MID-WINDOW regrid must bump the TX move-epoch (era safety)");
    bool full_anchor_armed_or_consumed =
        ConnectionAdaptiveTestAccess::descSwitchFullAnchorArmed(c);
    for (size_t i = 0; i < burst_full_anchor.size(); ++i) {
        if (burst_sizes[i] >= 2 && burst_full_anchor[i]) {
            full_anchor_armed_or_consumed = true;
        }
    }
    // F163 REVERSAL (2026-07-06): every commit arms the full anchor — the
    // midrung landing included (see the first-switch comment above).
    CHECK(full_anchor_armed_or_consumed,
          "every DESC-SWITCH commit must arm the full anchor (F163)");

    // Rate-limit: the SAME command (same group_seq — the base was frozen by the
    // crater) re-arriving before the receiver's adoption is deduped: no second
    // demote (QPSK R3/4 would otherwise step to R2/3), no new commit.
    c.onToneBurstAck(makeRungCmdDetection(/*group_seq=*/1, kRungCmdDownHard));
    // Midrung default (2026-07-05): the first DOWN-hard landed at 16QAM R1/2; the
    // DUPLICATE (same group_seq) must not move it again.
    CHECK(c.getDataModulation() == Modulation::QAM16 &&
              c.getDataCodeRate() == CodeRate::R1_2,
          "a duplicate command (same group_seq) before adoption must be a no-op");
    CHECK(c.getStats().descriptor_mode_switches == 1,
          "a duplicate command must not commit again");
    CHECK(countModeChangeFrames(tx_frames) == 0,
          "a duplicate command must not fall back to MODE_CHANGE either");
}

void test_ofdm_connected_entry_does_not_emit_unsolicited_timing_anchor() {
    Connection c;
    std::vector<Bytes> tx_frames;
    c.setTransmitCallback([&](const Bytes& data) {
        tx_frames.push_back(data);
    });
    ConnectionAdaptiveTestAccess::makeConnectedInitiator(c, WaveformMode::OFDM_CHIRP);

    ConnectionAdaptiveTestAccess::enterConnected(c);
    CHECK(tx_frames.empty(),
          "connected OFDM entry should not emit an unsolicited KEEPALIVE anchor");
}

void test_normal_ofdm_ack_arms_full_anchor_expectation() {
    Connection c;
    std::vector<Bytes> tx_frames;
    int full_anchor_expectations = 0;
    c.setTransmitCallback([&](const Bytes& data) {
        tx_frames.push_back(data);
    });
    c.setFullOFDMAnchorExpectedCallback([&]() {
        ++full_anchor_expectations;
    });
    ConnectionAdaptiveTestAccess::makeConnectedInitiator(c, WaveformMode::OFDM_CHIRP);

    auto ack = v2::ControlFrame::makeAck("W1ABC", "K2DEF", 7);
    ConnectionAdaptiveTestAccess::transmitFrame(c, ack.serialize());
    CHECK(tx_frames.size() == 1, "normal ACK should still transmit through Connection");
    CHECK(full_anchor_expectations == 1,
          "normal connected OFDM ACK should arm full-anchor expectation");

    auto connect_ack_sentinel = v2::ControlFrame::makeAck("W1ABC", "K2DEF", 0xFFFF);
    ConnectionAdaptiveTestAccess::transmitFrame(c, connect_ack_sentinel.serialize());
    CHECK(full_anchor_expectations == 1,
          "CONNECT_ACK sentinel seq=65535 must not arm full-anchor expectation");

    auto turnover = v2::ControlFrame::makeTurnover("W1ABC", "K2DEF");
    ConnectionAdaptiveTestAccess::transmitFrame(c, turnover.serialize());
    CHECK(full_anchor_expectations == 2,
          "connected OFDM TURNOVER should arm full-anchor expectation before listening");

    auto turn_request = v2::ControlFrame::makeTurnRequest("W1ABC", "K2DEF");
    ConnectionAdaptiveTestAccess::transmitFrame(c, turn_request.serialize());
    CHECK(full_anchor_expectations == 3,
          "connected OFDM TURN_REQUEST should arm full-anchor expectation before listening");

    Connection metadata;
    int metadata_expectations = 0;
    int immediate_expectations = 0;
    metadata.setTransmitInfoCallback([&](const Bytes&, bool expect_full_anchor_after_tx) {
        if (expect_full_anchor_after_tx) {
            ++metadata_expectations;
        }
    });
    metadata.setFullOFDMAnchorExpectedCallback([&]() {
        ++immediate_expectations;
    });
    ConnectionAdaptiveTestAccess::makeConnectedInitiator(metadata, WaveformMode::OFDM_CHIRP);
    ConnectionAdaptiveTestAccess::transmitFrame(metadata, ack.serialize());
    CHECK(metadata_expectations == 1,
          "Connection should attach full-anchor expectation metadata to normal OFDM ACK");
    ConnectionAdaptiveTestAccess::transmitFrame(metadata, turnover.serialize());
    CHECK(metadata_expectations == 2,
          "Connection should attach full-anchor expectation metadata to TURNOVER");
    CHECK(immediate_expectations == 0,
          "metadata TX callback should defer full-anchor application to the transport TX edge");

    Connection mcdpsk;
    int mcdpsk_expectations = 0;
    mcdpsk.setFullOFDMAnchorExpectedCallback([&]() {
        ++mcdpsk_expectations;
    });
    ConnectionAdaptiveTestAccess::makeConnectedInitiator(mcdpsk, WaveformMode::MC_DPSK);
    ConnectionAdaptiveTestAccess::transmitFrame(mcdpsk, ack.serialize());
    CHECK(mcdpsk_expectations == 0,
          "non-OFDM ACK must not arm full-anchor expectation");
}

} // namespace

int main() {
    // §RETX-PACING A/B knobs are latched ONCE via function-local statics — pin BOTH to
    // their disabled defaults BEFORE any Connection call so this binary deterministically
    // tests the byte-identical baseline (no hold armed, no collapse escape fired), per the
    // setenv-in-main pattern used by test_connection_policy.
    setenv("ULTRA_RETX_TROUGH_PACING", "0", 1);
    setenv("ULTRA_COLLAPSE_ESCAPE_ROUNDS", "0", 1);
    // RX-AUTHORITY is DEFAULT-ON since 2026-07-05 and supersedes the machinery this
    // binary tests (EMA feedback, RX-RATE-CMD demotes, descriptor escapes on the
    // legacy drivers) — pin it OFF so the legacy/fallback paths stay testable.
    setenv("ULTRA_RX_RATE_AUTHORITY", "0", 1);
    // #58 increment 3: pin the connect-SNR-pool knobs to their disabled defaults so
    // every Connection built here deterministically runs the byte-identical scalar
    // path (rateSelectionSnrDb/wireSnrDb == measured_snr_db_, no pick defer).
    setenv("ULTRA_CONNECT_SNR_POOL", "0", 1);
    setenv("ULTRA_CONNECT_PICK_DEFER", "0", 1);
    setenv("ULTRA_WIRE_SNR_FRESH", "0", 1);
    // DESC-SWITCH (docs/MODE_SWITCH_PIGGYBACK_DESIGN_2026_07_03.md Phase 1): pin the
    // knob to its byte-identical default. Unlike the function-local-static knobs above,
    // this one is latched PER Connection in its ctor — the knob-ON tests flip the env
    // around a single construction and restore it, so ordering here is baseline-only.
    setenv("ULTRA_DESCRIPTOR_MODE_SWITCH", "0", 1);
    // RX-RATE-CMD (design Phase 2): same per-Connection-ctor latch pattern. NOTE: the
    // tone_burst_payload CRC-span binding reads this env ONCE process-wide, but this
    // binary never routes payloads through the parameter-less codec overloads, so the
    // per-test flips below cannot skew the wire span (payload tests use explicit spans).
    setenv("ULTRA_RX_RATE_CMD", "0", 1);

    test_local_mode_change_ack_reconfigures_arq();
    test_local_mode_change_timeout_keeps_current_arq_mode();
    test_remote_mode_change_reconfigures_arq();
    test_remote_mode_change_ack_repeats_use_ofdm_ack_diversity();
    test_mode_change_retry_holds_while_tx_keyed();
    test_duplicate_mode_change_single_reack_no_reapply();
    test_wide_ofdm_configures_short_tail_sack_delay();
    test_accepted_ofdm_data_sync_keeps_connect_ack_rescue_armed();
    test_accepted_ofdm_data_sync_does_not_clear_non_ofdm_rescue();
    test_duplicate_connect_replays_cached_connect_ack_without_confirming();
    test_connect_retry_interval_is_control_airtime_derived();
    test_responder_handshake_timer_does_not_false_confirm();
    test_zero_progress_round_counter_knob_off_and_g42_protective();
    test_descriptor_switch_knob_off_is_byte_identical();
    test_descriptor_switch_commits_locally_at_clean_boundary();
    test_descriptor_adopt_reconfigures_receiver_without_ack();
    test_rx_rate_cmd_knob_off_is_byte_identical();
    test_rx_rate_cmd_receiver_emits_crater_down_hard_once_per_move();
    test_rx_rate_cmd_down_hard_mid_window_commits_via_descriptor_with_epoch();
    test_ofdm_connected_entry_does_not_emit_unsolicited_timing_anchor();
    test_normal_ofdm_ack_arms_full_anchor_expectation();

    if (tests_failed != 0) {
        std::cout << "ConnectionAdaptive: " << tests_failed << "/" << tests_run
                  << " failed\n";
        return 1;
    }

    std::cout << "ConnectionAdaptive: " << tests_run << "/" << tests_run
              << " passed\n";
    return 0;
}
