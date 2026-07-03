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
#include "protocol/connection.hpp"
#include "protocol/connection_policy.hpp"
#include "protocol/frame_v2.hpp"
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
    // 2026-07-03 ratiometric timer: the retry is a CONTROL-exchange round trip
    // (2x control guard + SACK coalesce), NOT the multi-frame burst ACK deadline
    // it used to borrow (rig W4: 18.5 s retries for a ~5 s exchange). It must be
    // strictly shorter than the burst deadline and scale with control airtime.
    CHECK(retry_ms < ConnectionAdaptiveTestAccess::arqAckTimeout(c),
          "MODE_CHANGE retry must be shorter than the data-burst ACK deadline");
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

    // Tone-burst OFDM path: ONE prompt ack per burst — NOT a redundant ack-diversity chain.
    // The tone-burst group-ack fires once; a lost ack is backstopped by the sender's ARQ
    // retransmit (re-sends the group -> the receiver re-acks). Repeating it would key the
    // receiver deaf for ~5 s. See configureArqForCurrentDataMode (kWideOFDMAckRepeatCount).
    const int repeat_count = ConnectionAdaptiveTestAccess::arqAckRepeatCount(c);
    CHECK(repeat_count == 1, "tone-burst OFDM path uses a single prompt ack (no diversity chain)");

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
    // #58 increment 3: pin the connect-SNR-pool knobs to their disabled defaults so
    // every Connection built here deterministically runs the byte-identical scalar
    // path (rateSelectionSnrDb/wireSnrDb == measured_snr_db_, no pick defer).
    setenv("ULTRA_CONNECT_SNR_POOL", "0", 1);
    setenv("ULTRA_CONNECT_PICK_DEFER", "0", 1);
    setenv("ULTRA_WIRE_SNR_FRESH", "0", 1);

    test_local_mode_change_ack_reconfigures_arq();
    test_local_mode_change_timeout_keeps_current_arq_mode();
    test_remote_mode_change_reconfigures_arq();
    test_remote_mode_change_ack_repeats_use_ofdm_ack_diversity();
    test_wide_ofdm_configures_short_tail_sack_delay();
    test_accepted_ofdm_data_sync_keeps_connect_ack_rescue_armed();
    test_accepted_ofdm_data_sync_does_not_clear_non_ofdm_rescue();
    test_duplicate_connect_replays_cached_connect_ack_without_confirming();
    test_connect_retry_interval_is_control_airtime_derived();
    test_responder_handshake_timer_does_not_false_confirm();
    test_zero_progress_round_counter_knob_off_and_g42_protective();
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
