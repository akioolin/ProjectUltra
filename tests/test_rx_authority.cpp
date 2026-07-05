/**
 * RX-AUTHORITY regression suite (ULTRA_RX_RATE_AUTHORITY, 2026-07-05)
 *
 * The receiver measures, the receiver decides: per burst group it maps its fresh
 * channel observation through selectCoherentOFDM and commands the sender's next
 * rung (absolute canonical index in the ACK's [rate_hint|rung_cmd] bits). The
 * sender obeys; its own mid-transfer drivers are inert under the knob.
 *
 * Own binary: rxRateAuthorityEnabled()/psk8LadderEnabled() latch env at first
 * call, so setenv() runs first in main().
 */

#include "protocol/connection.hpp"
#include "protocol/frame_v2.hpp"
#include "protocol/waveform_selection.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

using namespace ultra;
using namespace ultra::protocol;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { std::cout << "  Testing " << name << "... " << std::flush; tests_run++; } while (0)
#define PASS() \
    do { std::cout << "PASS\n"; tests_passed++; } while (0)
#define FAIL(msg) \
    do { std::cout << "FAIL: " << msg << "\n"; return false; } while (0)

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
    static void makeConnectedOFDM(Connection& c, CodeRate rate, float snr,
                                  float fading, Modulation modulation) {
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
    static void verdict(Connection& c, bool all_ok, float quality) {
        c.updateRxAuthorityCommand(all_ok, quality);
    }
    static uint8_t rxCmd(const Connection& c) { return c.rx_authority_cmd_; }
    static void obey(Connection& c, uint8_t idx) { c.maybeObeyAuthorityCommand(idx); }
    static bool modeChangePending(const Connection& c) { return c.mode_change_pending_; }
    static Modulation pendingModulation(const Connection& c) { return c.pending_modulation_; }
    static CodeRate pendingCodeRate(const Connection& c) { return c.pending_code_rate_; }
};

}  // namespace protocol
}  // namespace ultra

using TA = ConnectionAdaptiveTestAccess;

// Receiver verdict: strong calm channel maps to the top enabled rung; the command
// index is the canonical one.
static bool test_verdict_maps_snr_to_rung() {
    TEST("verdict maps 21 dB calm to 16QAM R2/3, 13 dB to QPSK R1/2");

    Connection c;
    TA::makeConnectedOFDM(c, CodeRate::R3_4, 20.0f, 0.05f, Modulation::QPSK);
    c.setBurstChannelObservation(21.0f, 0.30f, 0.8f, true, 0.2f);
    TA::verdict(c, /*all_ok=*/true, /*quality=*/0.9f);
    if (TA::rxCmd(c) != kRungIdxQam16R23)
        FAIL("21 dB Good verdict was idx " + std::to_string(TA::rxCmd(c)) +
             " (want QAM16 R2/3 = " + std::to_string(kRungIdxQam16R23) + ")");

    Connection d;
    TA::makeConnectedOFDM(d, CodeRate::R1_2, 13.0f, 0.05f, Modulation::QPSK);
    d.setBurstChannelObservation(13.0f, 0.30f, 0.8f, true, 0.2f);
    TA::verdict(d, true, 0.9f);
    if (TA::rxCmd(d) != kRungIdxQpskR12)
        FAIL("13 dB Good verdict was idx " + std::to_string(TA::rxCmd(d)) +
             " (want QPSK R1/2)");
    PASS();
    return true;
}

// Crater override: a failed group must never command at/above the current rung,
// whatever the (lagging) SNR map says.
static bool test_crater_never_commands_up() {
    TEST("crater clamps the command below the failed rung");

    Connection c;
    TA::makeConnectedOFDM(c, CodeRate::R2_3, 20.0f, 0.05f, Modulation::QAM16);
    c.setBurstChannelObservation(22.0f, 0.20f, 0.9f, true, 0.1f);  // map says stay high
    TA::verdict(c, /*all_ok=*/false, /*quality=*/0.0f);
    const uint8_t cur = coherentRungIndexFor(Modulation::QAM16, CodeRate::R2_3);
    if (TA::rxCmd(c) >= cur)
        FAIL("crater verdict idx " + std::to_string(TA::rxCmd(c)) +
             " not below current " + std::to_string(cur));
    PASS();
    return true;
}

// Clean override: a clean group must never command below the rung that just worked.
static bool test_clean_never_commands_down() {
    TEST("clean group holds the working rung against a stale-low reading");

    Connection c;
    TA::makeConnectedOFDM(c, CodeRate::R2_3, 20.0f, 0.05f, Modulation::QAM8);
    c.setBurstChannelObservation(11.0f, 0.30f, 0.8f, true, 0.2f);  // stale-low meter
    TA::verdict(c, /*all_ok=*/true, /*quality=*/0.95f);
    const uint8_t cur = coherentRungIndexFor(Modulation::QAM8, CodeRate::R2_3);
    if (TA::rxCmd(c) < cur)
        FAIL("clean-group verdict commanded below the working rung");
    PASS();
    return true;
}

// No observation yet -> no command (never steer on handshake-stale state).
static bool test_no_observation_no_command() {
    TEST("no fresh observation = no command");

    Connection c;
    TA::makeConnectedOFDM(c, CodeRate::R3_4, 20.0f, 0.05f, Modulation::QPSK);
    TA::verdict(c, true, 0.9f);
    if (TA::rxCmd(c) != kRungIdxNone) FAIL("commanded without an observation");
    PASS();
    return true;
}

// Sender obey: a commanded rung asserts the mode move; repeats dedup; arrival at
// the target re-arms.
static bool test_sender_obeys_and_dedups() {
    TEST("sender obeys the command once and dedups repeats");

    Connection c;
    TA::makeConnectedOFDM(c, CodeRate::R3_4, 20.0f, 0.05f, Modulation::QPSK);
    TA::obey(c, kRungIdxQam8R23);
    if (!TA::modeChangePending(c)) FAIL("command did not assert a mode change");
    if (TA::pendingModulation(c) != Modulation::QAM8 ||
        TA::pendingCodeRate(c) != CodeRate::R2_3)
        FAIL("obeyed target was not QAM8 R2/3");
    // A repeated copy while the exchange is pending must be a no-op (guard is the
    // mode_change_pending_ early-return + the last-obeyed dedup).
    TA::obey(c, kRungIdxQam8R23);
    if (!TA::modeChangePending(c)) FAIL("repeat handling clobbered the pending move");
    PASS();
    return true;
}

// Sender obey: command 0 and out-of-range indices are inert.
static bool test_sender_ignores_none_and_garbage() {
    TEST("cmd 0 / out-of-range are inert");

    Connection c;
    TA::makeConnectedOFDM(c, CodeRate::R3_4, 20.0f, 0.05f, Modulation::QPSK);
    TA::obey(c, kRungIdxNone);
    TA::obey(c, 31);
    TA::obey(c, kRungIdxCount);
    if (TA::modeChangePending(c)) FAIL("inert command asserted a mode change");
    PASS();
    return true;
}

int main() {
    // MUST precede any Connection construction (env-latched statics).
    setenv("ULTRA_RX_RATE_AUTHORITY", "1", 1);
    setenv("ULTRA_ENABLE_PSK8_LADDER", "1", 1);
    unsetenv("ULTRA_DESCRIPTOR_MODE_SWITCH");  // legacy path -> pending_* observable
    unsetenv("ULTRA_RX_RATE_CMD");

    std::cout << "RX-AUTHORITY suite\n";
    bool ok = true;
    ok &= test_verdict_maps_snr_to_rung();
    ok &= test_crater_never_commands_up();
    ok &= test_clean_never_commands_down();
    ok &= test_no_observation_no_command();
    ok &= test_sender_obeys_and_dedups();
    ok &= test_sender_ignores_none_and_garbage();
    std::cout << tests_passed << "/" << tests_run << " passed\n";
    return (ok && tests_passed == tests_run) ? 0 : 1;
}
