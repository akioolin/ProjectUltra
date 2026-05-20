#include "protocol/connection.hpp"
#include "protocol/connection_policy.hpp"
#include "protocol/frame_v2.hpp"
#include "helpers/temp_dir.hpp"

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
    static void makeConnectedOFDM(Connection& c,
                                  CodeRate rate,
                                  float snr = 15.0f,
                                  float fading = 0.05f) {
        c.local_call_ = "W1ABC";
        c.remote_call_ = "K2DEF";
        c.state_ = ConnectionState::CONNECTED;
        c.is_initiator_ = true;
        c.handshake_confirmed_ = true;
        c.negotiated_mode_ = WaveformMode::OFDM_CHIRP;
        c.data_modulation_ = Modulation::DQPSK;
        c.data_code_rate_ = rate;
        c.measured_snr_db_ = snr;
        c.fading_index_ = fading;
        c.arq_.setCallsigns(c.local_call_, c.remote_call_);
        c.configureArqForCurrentDataMode();
        c.resetAdaptiveModeController();
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
        c.arq_.setCallsigns(c.local_call_, c.remote_call_);
    }

    static void enterConnected(Connection& c) {
        c.enterConnected();
    }

    static void transmitFrame(Connection& c, const Bytes& frame) {
        c.transmitFrame(frame);
    }

    static bool connectAckRescueArmed(const Connection& c) {
        return !c.connect_ack_frame_.empty() || c.connect_ack_retx_remaining_ > 0 ||
               c.connect_ack_retransmit_ms_ > 0;
    }

    static void startFile(Connection& c, const std::string& path) {
        CHECK(c.file_transfer_.startSend(path), "startSend should succeed");
    }

    static void updateAdaptive(Connection& c, uint32_t ms) {
        c.updateAdaptiveModeController(ms);
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

    static uint16_t modeChangeSeq(const Connection& c) {
        return c.mode_change_seq_;
    }

    static CodeRate adaptiveTargetRate(const Connection& c) {
        return c.adaptive_target_.rate;
    }

    static bool adaptiveTargetPending(const Connection& c) {
        return c.adaptive_target_.pending;
    }

    static uint32_t postDowngradeLockoutMs() {
        return Connection::ADAPTIVE_POST_DOWNGRADE_LOCKOUT_MS;
    }

    static int cleanWindowsForUpgrade() {
        return Connection::ADAPTIVE_CLEAN_WINDOWS_FOR_UPGRADE;
    }

    static uint32_t downgradeForceMs() {
        return Connection::ADAPTIVE_DOWNGRADE_FORCE_MS;
    }

    static uint32_t postDowngradeLockoutRemaining(const Connection& c) {
        return c.adaptive_post_downgrade_lockout_ms_;
    }

    static uint32_t downgradeQueueAge(const Connection& c) {
        return c.adaptive_downgrade_queue_age_ms_;
    }

    static size_t arqWindow(const Connection& c) {
        return c.arq_.getWindowSize();
    }

    static size_t arqAvailableSlots(const Connection& c) {
        return c.arq_.getAvailableSlots();
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

    static uint32_t arqAckTimeout(const Connection& c) {
        return c.arq_.getAckTimeout();
    }

    static void forceCodeRate(Connection& c, CodeRate rate) {
        c.config_.forced_code_rate = rate;
    }
};

} // namespace protocol
} // namespace ultra

namespace {

void test_local_mode_change_ack_reconfigures_arq() {
    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R1_2, 15.0f, 0.30f);
    CHECK(ConnectionAdaptiveTestAccess::arqWindow(c) == connection_policy::kHighThroughputOFDMWindowFrames,
          "R1/2 should start with high-throughput ARQ window");

    c.requestModeChange(Modulation::DQPSK, CodeRate::R1_4, 12.0f,
                        v2::ModeChangeReason::CHANNEL_DEGRADED);
    auto ack = v2::ControlFrame::makeAck("K2DEF", "W1ABC", c.getStats().arq.acks_received + 1);
    ack.seq = ConnectionAdaptiveTestAccess::modeChangeSeq(c);
    c.onFrameReceived(ack.serialize());

    CHECK(c.getDataCodeRate() == CodeRate::R1_4, "local MODE_CHANGE ACK should apply pending rate");
    CHECK(ConnectionAdaptiveTestAccess::arqCodeRate(c) == CodeRate::R1_4,
          "local MODE_CHANGE ACK should update ARQ code rate");
    CHECK(ConnectionAdaptiveTestAccess::arqWindow(c) == connection_policy::kWideOFDMWindowFrames,
          "local MODE_CHANGE ACK should recompute ARQ window");
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
    CHECK(ConnectionAdaptiveTestAccess::arqWindow(c) == connection_policy::kWideOFDMWindowFrames,
          "remote MODE_CHANGE should recompute ARQ window");
}

void test_wide_ofdm_configures_short_tail_sack_delay() {
    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R1_4, 10.0f, 0.05f);

    const uint32_t long_delay = ConnectionAdaptiveTestAccess::arqSackDelay(c);
    const uint32_t tail_delay = ConnectionAdaptiveTestAccess::arqSackDelayShort(c);

    CHECK(tail_delay == connection_policy::wideOFDMSackTailDelayMs(),
          "wide OFDM should use the derived short SACK delay at stream tail");
    CHECK(tail_delay == connection_policy::kCarrierSenseSackCoalesceMs,
          "wide OFDM tail SACK delay should be carrier-sense coalescing only");
    CHECK(long_delay > tail_delay,
          "wide OFDM in-burst physical SACK hold should remain longer than tail delay");
    CHECK(ConnectionAdaptiveTestAccess::arqAckTimeout(c) ==
              connection_policy::computeWideOFDMAckTimeoutMs(
                  Modulation::DQPSK, CodeRate::R1_4,
                  ConnectionAdaptiveTestAccess::arqWindow(c),
                  long_delay, 3, v2::kDefaultFixedFrameCodewords),
          "wide OFDM ACK timeout should remain derived from the long physical SACK hold");
}

void test_accepted_ofdm_data_sync_clears_connect_ack_rescue() {
    Connection c;
    ConnectionAdaptiveTestAccess::makeResponderWithConnectAckRescue(c);

    CHECK(ConnectionAdaptiveTestAccess::connectAckRescueArmed(c),
          "responder CONNECT_ACK rescue should start armed");
    c.onAcceptedOFDMDataSync(0.90f);
    CHECK(!ConnectionAdaptiveTestAccess::connectAckRescueArmed(c),
          "accepted OFDM DATA sync should clear responder CONNECT_ACK rescue");
}

void test_accepted_ofdm_data_sync_does_not_clear_non_ofdm_rescue() {
    Connection c;
    ConnectionAdaptiveTestAccess::makeResponderWithConnectAckRescue(c, WaveformMode::MC_DPSK);

    c.onAcceptedOFDMDataSync(0.90f);
    CHECK(ConnectionAdaptiveTestAccess::connectAckRescueArmed(c),
          "accepted OFDM DATA sync hook should not clear non-OFDM rescue state");
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
    CHECK(immediate_expectations == 0,
          "metadata TX callback should defer full-anchor application to the transport TX edge");

    Connection mcdpsk;
    mcdpsk.setFullOFDMAnchorExpectedCallback([&]() {
        ++full_anchor_expectations;
    });
    ConnectionAdaptiveTestAccess::makeConnectedInitiator(mcdpsk, WaveformMode::MC_DPSK);
    ConnectionAdaptiveTestAccess::transmitFrame(mcdpsk, ack.serialize());
    CHECK(full_anchor_expectations == 1,
          "non-OFDM ACK must not arm full-anchor expectation");
}

void test_adaptive_upgrade_requires_backlog_and_clean_windows() {
    TempPayloadFile payload("ultra_adapt_upgrade", 5000);
    CHECK(payload.dir.valid(), "temp dir");
    CHECK(!payload.path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R1_2, 25.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);

    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "upgrade should wait for clean-window threshold");
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);

    CHECK(ConnectionAdaptiveTestAccess::modeChangePending(c),
          "clean AWGN backlog should request adaptive upgrade");
    CHECK(ConnectionAdaptiveTestAccess::pendingRate(c) == CodeRate::R3_4,
          "adaptive upgrade should target recommended R3/4 (post-Item-3-calibration)");
}

void test_adaptive_upgrade_skips_small_backlog() {
    TempPayloadFile payload("ultra_adapt_small", 32);
    CHECK(payload.dir.valid(), "temp dir");
    CHECK(!payload.path.empty(), "small test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R1_2, 25.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);

    for (int i = 0; i < 4; ++i) {
        ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    }

    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "small backlog should not pay MODE_CHANGE overhead for upgrade");
}

void test_adaptive_downgrade_hysteresis_and_short_lockout_upgrade() {
    TempPayloadFile payload("ultra_adapt_hysteresis", 5000);
    CHECK(payload.dir.valid(), "temp dir");
    CHECK(!payload.path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R1_2, 25.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);
    ConnectionAdaptiveTestAccess::createRetransmissionPressure(
        c, ConnectionAdaptiveTestAccess::arqWindow(c) / 2);
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);

    CHECK(!ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "single retry-pressure window should not queue downgrade target");
    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "single retry-pressure window should not issue downgrade MODE_CHANGE");

    ConnectionAdaptiveTestAccess::advanceRetransmissionPressure(c);
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);

    CHECK(ConnectionAdaptiveTestAccess::modeChangePending(c),
          "two consecutive retry-pressure windows should issue downgrade");
    CHECK(ConnectionAdaptiveTestAccess::pendingRate(c) == CodeRate::R1_4,
          "R1/2 retry pressure should step down to R1/4");
    ConnectionAdaptiveTestAccess::acknowledgeModeChange(c);
    CHECK(c.getDataCodeRate() == CodeRate::R1_4,
          "acknowledged downgrade should apply R1/4");
    CHECK(ConnectionAdaptiveTestAccess::postDowngradeLockoutMs() == 5000,
          "post-downgrade lockout should be 5000ms");

    ConnectionAdaptiveTestAccess::updateAdaptive(
        c, ConnectionAdaptiveTestAccess::postDowngradeLockoutMs());
    for (int i = 0; i < ConnectionAdaptiveTestAccess::cleanWindowsForUpgrade(); ++i) {
        ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    }

    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "clean windows after short lockout should queue upgrade");
    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetRate(c) == CodeRate::R3_4,
          "short-lockout upgrade should target recommended R3/4");
}

void test_adaptive_downgrade_waits_when_more_than_half_full() {
    TempPayloadFile payload("ultra_adapt_down", 5000);
    CHECK(payload.dir.valid(), "temp dir");
    CHECK(!payload.path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R2_3, 25.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);
    const size_t window = ConnectionAdaptiveTestAccess::arqWindow(c);
    ConnectionAdaptiveTestAccess::createRetransmissionPressure(c, (window / 2) + 1);
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    ConnectionAdaptiveTestAccess::advanceRetransmissionPressure(c);
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);

    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "retry pressure should queue an adaptive downgrade");
    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetRate(c) == CodeRate::R1_2,
          "R2/3 retry pressure should step down to R1/2");
    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "downgrade should wait while more than half the ARQ window is occupied");
}

void test_adaptive_downgrade_fires_when_window_half_full() {
    TempPayloadFile payload("ultra_adapt_down_half", 5000);
    CHECK(payload.dir.valid(), "temp dir");
    CHECK(!payload.path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R2_3, 25.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);
    const size_t window = ConnectionAdaptiveTestAccess::arqWindow(c);
    CHECK(window % 2 == 0, "test expects an even ARQ window");
    ConnectionAdaptiveTestAccess::createRetransmissionPressure(c, window / 2);

    CHECK(ConnectionAdaptiveTestAccess::arqAvailableSlots(c) * 2 == window,
          "test setup should leave exactly half the ARQ window free");
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    ConnectionAdaptiveTestAccess::advanceRetransmissionPressure(c);
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);

    CHECK(ConnectionAdaptiveTestAccess::modeChangePending(c),
          "half-free downgrade should issue MODE_CHANGE immediately");
    CHECK(ConnectionAdaptiveTestAccess::pendingRate(c) == CodeRate::R1_2,
          "issued downgrade should target R1/2");
    CHECK(!ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "issued downgrade should clear the queued adaptive target");
}

void test_adaptive_stuck_downgrade_forces_after_timeout() {
    TempPayloadFile payload("ultra_adapt_down_force", 5000);
    CHECK(payload.dir.valid(), "temp dir");
    CHECK(!payload.path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R2_3, 25.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);
    const size_t window = ConnectionAdaptiveTestAccess::arqWindow(c);
    ConnectionAdaptiveTestAccess::createRetransmissionPressure(c, window);

    CHECK(ConnectionAdaptiveTestAccess::arqAvailableSlots(c) == 0,
          "test setup should leave no ARQ slots free");
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    ConnectionAdaptiveTestAccess::advanceRetransmissionPressure(c);
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);

    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "retry pressure should queue a downgrade");
    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "full ARQ window should block normal downgrade boundary");

    ConnectionAdaptiveTestAccess::updateAdaptive(
        c, ConnectionAdaptiveTestAccess::downgradeForceMs() + 1);

    CHECK(ConnectionAdaptiveTestAccess::modeChangePending(c),
          "stuck downgrade should force MODE_CHANGE after timeout");
    CHECK(ConnectionAdaptiveTestAccess::pendingRate(c) == CodeRate::R1_2,
          "forced downgrade should target R1/2");
    CHECK(!ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "forced downgrade should clear the queued adaptive target");
    CHECK(ConnectionAdaptiveTestAccess::downgradeQueueAge(c) == 0,
          "forced downgrade should reset queue age");
}

void test_adaptive_upgrade_not_forced_after_timeout() {
    TempPayloadFile payload("ultra_adapt_up_no_force", 5000);
    CHECK(payload.dir.valid(), "temp dir");
    CHECK(!payload.path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R1_2, 25.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);
    const size_t window = ConnectionAdaptiveTestAccess::arqWindow(c);
    ConnectionAdaptiveTestAccess::fillArqWindow(c, window);

    CHECK(ConnectionAdaptiveTestAccess::arqAvailableSlots(c) == 0,
          "test setup should leave no ARQ slots free");
    for (int i = 0; i < 3; ++i) {
        ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    }

    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "clean backlog should queue an adaptive upgrade");
    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetRate(c) == CodeRate::R3_4,
          "queued upgrade should target R3/4");
    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "full ARQ window should block normal upgrade boundary");

    ConnectionAdaptiveTestAccess::updateAdaptive(
        c, ConnectionAdaptiveTestAccess::downgradeForceMs() + 1000);

    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "upgrade should not force MODE_CHANGE after downgrade timeout");
    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "blocked upgrade should remain queued");
    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetRate(c) == CodeRate::R3_4,
          "blocked upgrade should keep its target rate");
}

void test_adaptive_post_downgrade_lockout_blocks_upgrade() {
    TempPayloadFile payload("ultra_adapt_down_lockout", 5000);
    CHECK(payload.dir.valid(), "temp dir");
    CHECK(!payload.path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R2_3, 25.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);
    ConnectionAdaptiveTestAccess::createRetransmissionPressure(
        c, ConnectionAdaptiveTestAccess::arqWindow(c) / 2);
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    ConnectionAdaptiveTestAccess::advanceRetransmissionPressure(c);
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    ConnectionAdaptiveTestAccess::acknowledgeModeChange(c);

    CHECK(ConnectionAdaptiveTestAccess::postDowngradeLockoutRemaining(c) ==
              ConnectionAdaptiveTestAccess::postDowngradeLockoutMs(),
          "issued downgrade should arm post-downgrade upgrade lockout");
    for (int i = 0; i < 3; ++i) {
        ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    }

    CHECK(!ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "post-downgrade lockout should suppress immediate upgrade queue");
    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "post-downgrade lockout should suppress immediate upgrade MODE_CHANGE");
}

void test_adaptive_post_downgrade_lockout_expires() {
    TempPayloadFile payload("ultra_adapt_down_lockout_expire", 5000);
    CHECK(payload.dir.valid(), "temp dir");
    CHECK(!payload.path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R2_3, 25.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);
    ConnectionAdaptiveTestAccess::createRetransmissionPressure(
        c, ConnectionAdaptiveTestAccess::arqWindow(c) / 2);
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    ConnectionAdaptiveTestAccess::advanceRetransmissionPressure(c);
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    ConnectionAdaptiveTestAccess::acknowledgeModeChange(c);

    for (int i = 0; i < 3; ++i) {
        ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    }
    ConnectionAdaptiveTestAccess::updateAdaptive(
        c, ConnectionAdaptiveTestAccess::postDowngradeLockoutMs() - 3000 + 1);

    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "upgrade should queue once post-downgrade lockout expires");
    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetRate(c) == CodeRate::R3_4,
          "expired post-downgrade lockout should allow recommended R3/4 upgrade");
}

void test_forced_rate_disables_adaptive_controller() {
    TempPayloadFile payload("ultra_adapt_forced", 5000);
    CHECK(payload.dir.valid(), "temp dir");
    CHECK(!payload.path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R1_2, 25.0f, 0.05f);
    ConnectionAdaptiveTestAccess::forceCodeRate(c, CodeRate::R1_2);
    ConnectionAdaptiveTestAccess::startFile(c, payload.path);

    for (int i = 0; i < 4; ++i) {
        ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    }

    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "forced code rate should suppress adaptive MODE_CHANGE");
}

} // namespace

int main() {
    test_local_mode_change_ack_reconfigures_arq();
    test_remote_mode_change_reconfigures_arq();
    test_wide_ofdm_configures_short_tail_sack_delay();
    test_accepted_ofdm_data_sync_clears_connect_ack_rescue();
    test_accepted_ofdm_data_sync_does_not_clear_non_ofdm_rescue();
    test_ofdm_connected_entry_does_not_emit_unsolicited_timing_anchor();
    test_normal_ofdm_ack_arms_full_anchor_expectation();
    test_adaptive_upgrade_requires_backlog_and_clean_windows();
    test_adaptive_upgrade_skips_small_backlog();
    test_adaptive_downgrade_hysteresis_and_short_lockout_upgrade();
    test_adaptive_downgrade_waits_when_more_than_half_full();
    test_adaptive_downgrade_fires_when_window_half_full();
    test_adaptive_stuck_downgrade_forces_after_timeout();
    test_adaptive_upgrade_not_forced_after_timeout();
    test_adaptive_post_downgrade_lockout_blocks_upgrade();
    test_adaptive_post_downgrade_lockout_expires();
    test_forced_rate_disables_adaptive_controller();

    if (tests_failed != 0) {
        std::cout << "ConnectionAdaptive: " << tests_failed << "/" << tests_run
                  << " failed\n";
        return 1;
    }

    std::cout << "ConnectionAdaptive: " << tests_run << "/" << tests_run
              << " passed\n";
    return 0;
}
