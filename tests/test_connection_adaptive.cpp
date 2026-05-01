#include "protocol/connection.hpp"
#include "protocol/connection_policy.hpp"
#include "protocol/frame_v2.hpp"

#include <chrono>
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

std::filesystem::path makeTempDir(const std::string& prefix) {
    std::error_code ec;
    auto base = std::filesystem::temp_directory_path(ec);
    if (ec) return {};
    auto dir = base / (prefix + "_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};
    return dir;
}

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

void test_adaptive_upgrade_requires_backlog_and_clean_windows() {
    auto dir = makeTempDir("ultra_adapt_upgrade");
    CHECK(!dir.empty(), "temp dir");
    auto path = createFile(dir, 5000);
    CHECK(!path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R1_2, 15.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, path);

    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "upgrade should wait for clean-window threshold");
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);

    CHECK(ConnectionAdaptiveTestAccess::modeChangePending(c),
          "clean AWGN backlog should request adaptive upgrade");
    CHECK(ConnectionAdaptiveTestAccess::pendingRate(c) == CodeRate::R2_3,
          "adaptive upgrade should target recommended R2/3");
    std::filesystem::remove_all(dir);
}

void test_adaptive_upgrade_skips_small_backlog() {
    auto dir = makeTempDir("ultra_adapt_small");
    CHECK(!dir.empty(), "temp dir");
    auto path = createFile(dir, 32);
    CHECK(!path.empty(), "small test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R1_2, 15.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, path);

    for (int i = 0; i < 4; ++i) {
        ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    }

    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "small backlog should not pay MODE_CHANGE overhead for upgrade");
    std::filesystem::remove_all(dir);
}

void test_adaptive_downgrade_waits_when_more_than_half_full() {
    auto dir = makeTempDir("ultra_adapt_down");
    CHECK(!dir.empty(), "temp dir");
    auto path = createFile(dir, 5000);
    CHECK(!path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R2_3, 15.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, path);
    const size_t window = ConnectionAdaptiveTestAccess::arqWindow(c);
    ConnectionAdaptiveTestAccess::createRetransmissionPressure(c, (window / 2) + 1);
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);

    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "retry pressure should queue an adaptive downgrade");
    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetRate(c) == CodeRate::R1_2,
          "R2/3 retry pressure should step down to R1/2");
    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "downgrade should wait while more than half the ARQ window is occupied");
    std::filesystem::remove_all(dir);
}

void test_adaptive_downgrade_fires_when_window_half_full() {
    auto dir = makeTempDir("ultra_adapt_down_half");
    CHECK(!dir.empty(), "temp dir");
    auto path = createFile(dir, 5000);
    CHECK(!path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R2_3, 15.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, path);
    const size_t window = ConnectionAdaptiveTestAccess::arqWindow(c);
    CHECK(window % 2 == 0, "test expects an even ARQ window");
    ConnectionAdaptiveTestAccess::createRetransmissionPressure(c, window / 2);

    CHECK(ConnectionAdaptiveTestAccess::arqAvailableSlots(c) * 2 == window,
          "test setup should leave exactly half the ARQ window free");
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);

    CHECK(ConnectionAdaptiveTestAccess::modeChangePending(c),
          "half-free downgrade should issue MODE_CHANGE immediately");
    CHECK(ConnectionAdaptiveTestAccess::pendingRate(c) == CodeRate::R1_2,
          "issued downgrade should target R1/2");
    CHECK(!ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "issued downgrade should clear the queued adaptive target");
    std::filesystem::remove_all(dir);
}

void test_adaptive_stuck_downgrade_forces_after_timeout() {
    auto dir = makeTempDir("ultra_adapt_down_force");
    CHECK(!dir.empty(), "temp dir");
    auto path = createFile(dir, 5000);
    CHECK(!path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R2_3, 15.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, path);
    const size_t window = ConnectionAdaptiveTestAccess::arqWindow(c);
    ConnectionAdaptiveTestAccess::createRetransmissionPressure(c, window);

    CHECK(ConnectionAdaptiveTestAccess::arqAvailableSlots(c) == 0,
          "test setup should leave no ARQ slots free");
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
    std::filesystem::remove_all(dir);
}

void test_adaptive_upgrade_not_forced_after_timeout() {
    auto dir = makeTempDir("ultra_adapt_up_no_force");
    CHECK(!dir.empty(), "temp dir");
    auto path = createFile(dir, 5000);
    CHECK(!path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R1_2, 15.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, path);
    const size_t window = ConnectionAdaptiveTestAccess::arqWindow(c);
    ConnectionAdaptiveTestAccess::fillArqWindow(c, window);

    CHECK(ConnectionAdaptiveTestAccess::arqAvailableSlots(c) == 0,
          "test setup should leave no ARQ slots free");
    for (int i = 0; i < 3; ++i) {
        ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    }

    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "clean backlog should queue an adaptive upgrade");
    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetRate(c) == CodeRate::R2_3,
          "queued upgrade should target R2/3");
    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "full ARQ window should block normal upgrade boundary");

    ConnectionAdaptiveTestAccess::updateAdaptive(
        c, ConnectionAdaptiveTestAccess::downgradeForceMs() + 1000);

    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "upgrade should not force MODE_CHANGE after downgrade timeout");
    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "blocked upgrade should remain queued");
    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetRate(c) == CodeRate::R2_3,
          "blocked upgrade should keep its target rate");
    std::filesystem::remove_all(dir);
}

void test_adaptive_post_downgrade_lockout_blocks_upgrade() {
    auto dir = makeTempDir("ultra_adapt_down_lockout");
    CHECK(!dir.empty(), "temp dir");
    auto path = createFile(dir, 5000);
    CHECK(!path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R2_3, 15.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, path);
    ConnectionAdaptiveTestAccess::createRetransmissionPressure(
        c, ConnectionAdaptiveTestAccess::arqWindow(c) / 2);
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
    std::filesystem::remove_all(dir);
}

void test_adaptive_post_downgrade_lockout_expires() {
    auto dir = makeTempDir("ultra_adapt_down_lockout_expire");
    CHECK(!dir.empty(), "temp dir");
    auto path = createFile(dir, 5000);
    CHECK(!path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R2_3, 15.0f, 0.05f);
    ConnectionAdaptiveTestAccess::startFile(c, path);
    ConnectionAdaptiveTestAccess::createRetransmissionPressure(
        c, ConnectionAdaptiveTestAccess::arqWindow(c) / 2);
    ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    ConnectionAdaptiveTestAccess::acknowledgeModeChange(c);

    for (int i = 0; i < 3; ++i) {
        ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    }
    ConnectionAdaptiveTestAccess::updateAdaptive(
        c, ConnectionAdaptiveTestAccess::postDowngradeLockoutMs() - 3000 + 1);

    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetPending(c),
          "upgrade should queue once post-downgrade lockout expires");
    CHECK(ConnectionAdaptiveTestAccess::adaptiveTargetRate(c) == CodeRate::R2_3,
          "expired post-downgrade lockout should allow recommended R2/3 upgrade");
    std::filesystem::remove_all(dir);
}

void test_forced_rate_disables_adaptive_controller() {
    auto dir = makeTempDir("ultra_adapt_forced");
    CHECK(!dir.empty(), "temp dir");
    auto path = createFile(dir, 5000);
    CHECK(!path.empty(), "large test file");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedOFDM(c, CodeRate::R1_2, 15.0f, 0.05f);
    ConnectionAdaptiveTestAccess::forceCodeRate(c, CodeRate::R1_2);
    ConnectionAdaptiveTestAccess::startFile(c, path);

    for (int i = 0; i < 4; ++i) {
        ConnectionAdaptiveTestAccess::updateAdaptive(c, 1000);
    }

    CHECK(!ConnectionAdaptiveTestAccess::modeChangePending(c),
          "forced code rate should suppress adaptive MODE_CHANGE");
    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    test_local_mode_change_ack_reconfigures_arq();
    test_remote_mode_change_reconfigures_arq();
    test_adaptive_upgrade_requires_backlog_and_clean_windows();
    test_adaptive_upgrade_skips_small_backlog();
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
