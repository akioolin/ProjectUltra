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

#include "env_compat.hpp"
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
    static void fillArq(Connection& c) {
        c.arq_.sendData(Bytes{0x01, 0x02, 0x03});  // one frame in flight = busy
    }
    static constexpr size_t obsRing() { return Connection::kRxAuthObsRing; }
    static constexpr int climbDwell() { return Connection::kRxAuthClimbDwellGroups; }
    // Simulate the sender obeying + the receiver adopting the standing command
    // (the real loop moves `cur`; without this every verdict re-climbs from the
    // same rung and dwell/one-rung assertions measure the wrong thing).
    static void adoptCmd(Connection& c) {
        if (c.rx_authority_cmd_ == kRungIdxNone) return;
        const CoherentPick p = coherentRungFromIndex(c.rx_authority_cmd_);
        c.data_modulation_ = p.mod;
        c.data_code_rate_ = p.rate;
    }
};

}  // namespace protocol
}  // namespace ultra

using TA = ConnectionAdaptiveTestAccess;

// Receiver verdict: a strong calm channel climbs ONE enabled rung per clean
// verdict (F149: multi-rung crest jumps fueled the climb-crater-demote cycle —
// 11 switches / 5.5 min; each rung must now prove itself with a delivered
// group), and repeated clean verdicts still reach the map's top pick. Descents
// keep taking the map directly (bounded by the 2-rung down-limit).
static bool test_verdict_maps_snr_to_rung() {
    TEST("verdict climbs one rung per clean verdict; 13 dB maps straight down");

    Connection c;
    TA::makeConnectedOFDM(c, CodeRate::R3_4, 20.0f, 0.05f, Modulation::QPSK);
    c.setBurstChannelObservation(21.0f, 0.30f, 0.8f, true, 0.2f);
    TA::verdict(c, /*all_ok=*/true, /*quality=*/0.9f);
    if (TA::rxCmd(c) != kRungIdxQam8R23)
        FAIL("first clean 21 dB verdict from QPSK R3/4 was idx " +
             std::to_string(TA::rxCmd(c)) +
             " (want ONE enabled rung up = QAM8 R2/3 = " +
             std::to_string(kRungIdxQam8R23) + ")");
    // Each further step must ITSELF clear anchor + 2.5 dB margin (the old code
    // proved "some climb" then jumped to the raw map target — the crest-jump
    // hole). At 21 dB the ladder tops out at 16QAM R1/2: 16QAM R2/3 (the F149
    // cratering rung) is not margin-proof there and must NOT be commanded.
    TA::adoptCmd(c);
    TA::verdict(c, true, 0.9f);  // idx 5 -> 7 (16QAM R1/2; idx 6 disabled)
    if (TA::rxCmd(c) != kRungIdxQam16R12)
        FAIL("second step was idx " + std::to_string(TA::rxCmd(c)) +
             " (want 16QAM R1/2 = " + std::to_string(kRungIdxQam16R12) + ")");
    TA::adoptCmd(c);
    TA::verdict(c, true, 0.9f);  // idx 7: 16QAM R2/3 not margin-proof at 21 dB
    if (TA::rxCmd(c) != kRungIdxQam16R12)
        FAIL("21 dB over-climbed past 16QAM R1/2 to idx " +
             std::to_string(TA::rxCmd(c)) + " (margin-proof ladder must hold)");

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

// F149/F160 CLIMB DWELL: after a confirmed crater episode, post-episode reality
// must displace the crest-biased reads (kRxAuthClimbDwellGroups clean groups)
// before ANY up-command — a fade
// crest read right after the demote must not re-arm the loop.
static bool test_confirmed_crater_arms_climb_dwell() {
    TEST("confirmed crater blocks re-climb until the ring turns over");

    Connection c;
    TA::makeConnectedOFDM(c, CodeRate::R2_3, 20.0f, 0.05f, Modulation::QAM16);
    c.setBurstChannelObservation(22.0f, 0.20f, 0.9f, true, 0.1f);
    TA::verdict(c, false, 0.0f);  // crater #1 — hold
    TA::verdict(c, false, 0.0f);  // crater #2 — confirmed: demote + dwell armed
    const uint8_t demoted = TA::rxCmd(c);
    if (demoted >= kRungIdxQam16R23)
        FAIL("confirmed crater did not demote");
    TA::adoptCmd(c);  // sender obeys the demote

    // Fade-crest reads right after the demote: climbs must stay blocked for a
    // full ring turnover even though the map screams UP.
    for (int i = 0; i + 1 < TA::climbDwell(); ++i) {
        c.setBurstChannelObservation(32.0f, 0.20f, 0.9f, true, 0.1f);
        TA::verdict(c, true, 0.95f);
        if (TA::rxCmd(c) > demoted)
            FAIL("climb re-armed after only " + std::to_string(i + 1) +
                 " clean groups (dwell must hold " +
                 std::to_string(TA::climbDwell()) + ")");
    }
    // Ring turned over: the next clean verdict may climb (one rung).
    c.setBurstChannelObservation(32.0f, 0.20f, 0.9f, true, 0.1f);
    TA::verdict(c, true, 0.95f);
    if (TA::rxCmd(c) <= demoted)
        FAIL("climb still blocked after the dwell expired");
    if (TA::rxCmd(c) > demoted + 2)
        FAIL("post-dwell climb jumped more than one enabled rung");
    PASS();
    return true;
}

// ── RX-AUTHORITY PREDICTIVE (docs/RX_AUTHORITY_PREDICTIVE_2026_07_07.md) ──

// EESM self-calibration identity: on a FLAT channel the predictor must
// reproduce the anchor table exactly — pass at the anchor, fail below it.
static bool test_prediction_flat_identity() {
    TEST("EESM flat-channel identity vs the anchor table");
    const Modulation m = Modulation::QAM16;
    const CodeRate c = CodeRate::R2_3;
    const float A_db = calibrationAnchorDbFor(m, c);
    if (A_db >= kRungDisabledDb) FAIL("16QAM R2/3 unexpectedly disabled");
    std::vector<float> flat(51, std::pow(10.0f, (A_db + 0.1f) / 10.0f));
    if (!rungPredictedSustainable(flat.data(), flat.size(), m, c, 0.0f))
        FAIL("flat channel 0.1 dB above the anchor must pass at margin 0");
    std::vector<float> below(51, std::pow(10.0f, (A_db - 0.5f) / 10.0f));
    if (rungPredictedSustainable(below.data(), below.size(), m, c, 0.0f))
        FAIL("flat channel 0.5 dB below the anchor must fail");
    // Margin shifts the bar: at-anchor fails once any margin applies.
    std::vector<float> at(51, std::pow(10.0f, A_db / 10.0f));
    if (rungPredictedSustainable(at.data(), at.size(), m, c, 2.5f))
        FAIL("at-anchor flat channel must fail with a 2.5 dB margin");
    PASS();
    return true;
}

// The F149 crest trap: mean SNR reads generous while a parked notch has 20 %
// of carriers dead — dense rungs must FAIL prediction; QPSK must survive.
static bool test_prediction_rejects_notched_channel() {
    TEST("notched channel rejects 16QAM R2/3 while QPSK R2/3 passes");
    std::vector<float> g(51);
    for (size_t i = 0; i < g.size(); ++i) {
        g[i] = (i % 5 == 0) ? std::pow(10.0f, -10.0f / 10.0f)   // 20% at -10 dB
                            : std::pow(10.0f, 24.0f / 10.0f);   // rest at 24 dB
    }
    if (rungPredictedSustainable(g.data(), g.size(), Modulation::QAM16,
                                 CodeRate::R2_3, 2.5f))
        FAIL("16QAM R2/3 must fail on a 20%-notched channel (the crest trap)");
    if (!rungPredictedSustainable(g.data(), g.size(), Modulation::QPSK,
                                  CodeRate::R2_3, 2.5f))
        FAIL("QPSK R2/3 must survive the same notched channel");
    PASS();
    return true;
}

// DIRECT MULTI-RUNG JUMP: with >=2 fresh calm flat snapshots, one clean verdict
// commands the measured-sustainable rung outright (idx 3 -> 8), no laddering.
static bool test_predictive_direct_jump() {
    TEST("2 calm snapshots let one verdict jump QPSK R2/3 -> 16QAM R2/3");
    Connection c;
    TA::makeConnectedOFDM(c, CodeRate::R2_3, 24.0f, 0.05f, Modulation::QPSK);
    std::vector<float> calm(51, std::pow(10.0f, 26.0f / 10.0f));  // 26 dB flat
    c.setBurstCarrierGammas(calm);
    c.setBurstCarrierGammas(calm);
    c.setBurstChannelObservation(24.0f, 0.20f, 0.9f, true, 0.1f);
    TA::verdict(c, true, 0.95f);
    if (TA::rxCmd(c) != kRungIdxQam16R23)
        FAIL("verdict was idx " + std::to_string(TA::rxCmd(c)) +
             " (want direct jump to 16QAM R2/3 = " +
             std::to_string(kRungIdxQam16R23) + ")");

    // Same scalar picture but ONE snapshot is notched: the jump must not fire
    // past what every snapshot proves — 16QAM R2/3 rejected.
    Connection d;
    TA::makeConnectedOFDM(d, CodeRate::R2_3, 24.0f, 0.05f, Modulation::QPSK);
    std::vector<float> notch(51);
    for (size_t i = 0; i < notch.size(); ++i) {
        notch[i] = (i % 5 == 0) ? std::pow(10.0f, -10.0f / 10.0f)
                                : std::pow(10.0f, 24.0f / 10.0f);
    }
    d.setBurstCarrierGammas(calm);
    d.setBurstCarrierGammas(notch);
    d.setBurstChannelObservation(24.0f, 0.20f, 0.9f, true, 0.1f);
    TA::verdict(d, true, 0.95f);
    if (TA::rxCmd(d) >= kRungIdxQam16R23)
        FAIL("a notched snapshot in the window must veto the 16QAM R2/3 jump "
             "(got idx " + std::to_string(TA::rxCmd(d)) + ")");
    PASS();
    return true;
}

// TWO-CRATER rule: a single crater holds the rung (irreducible null — the ARQ's
// job); the SECOND consecutive crater clamps below it whatever the map says.
static bool test_two_crater_rule() {
    TEST("single crater holds; second consecutive crater clamps below");

    Connection c;
    TA::makeConnectedOFDM(c, CodeRate::R2_3, 20.0f, 0.05f, Modulation::QAM16);
    c.setBurstChannelObservation(22.0f, 0.20f, 0.9f, true, 0.1f);  // map says stay high
    const uint8_t cur = coherentRungIndexFor(Modulation::QAM16, CodeRate::R2_3);

    TA::verdict(c, /*all_ok=*/false, /*quality=*/0.0f);  // crater #1
    if (TA::rxCmd(c) != cur)
        FAIL("single crater moved the command (idx " +
             std::to_string(TA::rxCmd(c)) + ", want hold " + std::to_string(cur) + ")");

    TA::verdict(c, false, 0.0f);  // crater #2 — confirmed
    if (TA::rxCmd(c) >= cur)
        FAIL("confirmed crater verdict idx " + std::to_string(TA::rxCmd(c)) +
             " not below current " + std::to_string(cur));
    // F160: demote lands on the FIRST ENABLED rung below — 16QAM R1/2 (idx 7),
    // the rung the ladder proved on the way up. The old cur-2 stride snapped
    // through the QAM8 R3/4 hole to idx 5 = a 3-rung collapse the one-rung
    // climb repaid across 3 switches.
    if (TA::rxCmd(c) != kRungIdxQam16R12)
        FAIL("confirmed crater demoted to idx " + std::to_string(TA::rxCmd(c)) +
             " (want first enabled below = 16QAM R1/2 = " +
             std::to_string(kRungIdxQam16R12) + ")");
    // ENABLED-LADDER pin (F145 deadlock): the raw cur-2 stride from QAM16 R2/3
    // (idx 8) lands on QAM8 R3/4 (idx 6) — a fully-disabled anchor row. The
    // command must snap to an ENABLED rung (idx 5, QAM8 R2/3) or the sender's
    // guard refuses it and the cratering rung pins forever (F145: 50 s, six
    // 0/5 whole-burst resends, four refused DOWN commands).
    {
        const CoherentPick p = coherentRungFromIndex(TA::rxCmd(c));
        if (!coherentRungLocallyEnabled(p.mod, p.rate))
            FAIL("confirmed crater commanded a DISABLED rung (idx " +
                 std::to_string(TA::rxCmd(c)) + ") — unobeyable, deadlocks the link");
    }

    // A clean group resets the streak: the next single crater holds again.
    c.setBurstChannelObservation(22.0f, 0.20f, 0.9f, true, 0.1f);
    TA::verdict(c, true, 0.9f);
    TA::verdict(c, false, 0.0f);
    const uint8_t cur2 = coherentRungIndexFor(c.getDataModulation(), c.getDataCodeRate());
    if (TA::rxCmd(c) < cur2 && cur2 != kRungIdxNone)
        FAIL("post-clean single crater demoted (streak not reset)");
    PASS();
    return true;
}

// Boundary asymmetry: with data in flight, an UP command defers (no mode change);
// a DOWN command obeys immediately.
static bool test_busy_defers_up_not_down() {
    TEST("busy window: climb defers, demote obeys");

    Connection c;
    TA::makeConnectedOFDM(c, CodeRate::R2_3, 20.0f, 0.05f, Modulation::QAM8);
    // Put a frame in flight directly through the friend'd ARQ.
    TA::fillArq(c);
    TA::obey(c, kRungIdxQam16R23);  // UP from QAM8 R2/3
    if (TA::modeChangePending(c)) FAIL("climb fired mid-window");
    TA::obey(c, kRungIdxQpskR23);   // DOWN
    if (!TA::modeChangePending(c)) FAIL("demote deferred mid-window");
    if (TA::pendingModulation(c) != Modulation::QPSK ||
        TA::pendingCodeRate(c) != CodeRate::R2_3)
        FAIL("demote target wrong");
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

// A fixed-rung probe is an operator command, not an advisory. Receiver authority
// must not move the sender while ULTRA_LOCK_RATE is active.
static bool test_operator_lock_overrides_authority() {
    TEST("operator adaptation controls override receiver authority");

    {
        Connection c;
        TA::makeConnectedOFDM(c, CodeRate::R3_4, 20.0f, 0.05f, Modulation::QPSK);
        setenv("ULTRA_LOCK_RATE", "1", 1);
        TA::obey(c, kRungIdxQam8R23);
        unsetenv("ULTRA_LOCK_RATE");
        if (TA::modeChangePending(c)) FAIL("authority bypassed ULTRA_LOCK_RATE");
    }
    {
        Connection c;
        TA::makeConnectedOFDM(c, CodeRate::R3_4, 20.0f, 0.05f, Modulation::QPSK);
        setenv("ULTRA_RATE_ADAPT", "0", 1);
        TA::obey(c, kRungIdxQam8R23);
        unsetenv("ULTRA_RATE_ADAPT");
        if (TA::modeChangePending(c)) FAIL("authority bypassed ULTRA_RATE_ADAPT=0");
    }
    {
        setenv("ULTRA_ADAPTIVE_RATE", "0", 1);
        Connection c;
        unsetenv("ULTRA_ADAPTIVE_RATE");
        TA::makeConnectedOFDM(c, CodeRate::R3_4, 20.0f, 0.05f, Modulation::QPSK);
        TA::obey(c, kRungIdxQam8R23);
        if (TA::modeChangePending(c)) FAIL("authority bypassed ULTRA_ADAPTIVE_RATE=0");
    }
    PASS();
    return true;
}

// ── ULTRA_RX_EMA_HOLD (throughput-ceiling audit lever #1, 2026-07-21) ──
// A confirmed crater must HOLD the rung while the fade-averaged SNR still clears the
// rung's class floor (deep-null brush the ARQ absorbs), but sustained real failure —
// censored samples driving the average down — must still eventually demote. Same
// scenario, knob ON vs OFF, is the A/B: OFF demotes on the 2nd crater, ON holds.
static bool test_ema_hold_absorbs_supported_crater() {
    TEST("EMA-HOLD holds a confirmed crater the fade-averaged SNR still supports");
    // 16QAM R2/3 on Good reads 24 dB (> the 20 dB Good anchor). Seed clean history
    // so the ring average sits above the anchor, THEN two craters (a fade brush).
    auto run = [](bool knob_on) -> uint8_t {
        setenv("ULTRA_RX_EMA_HOLD", knob_on ? "1" : "0", 1);  // explicit (default is ON)
        Connection c;
        TA::makeConnectedOFDM(c, CodeRate::R2_3, 20.0f, 0.20f, Modulation::QAM16);
        for (int i = 0; i < 4; ++i) {  // clean history keeps the ring average high
            c.setBurstChannelObservation(24.0f, 0.20f, 0.9f, true, 0.1f);
            TA::verdict(c, true, 0.95f);
        }
        c.setBurstChannelObservation(24.0f, 0.20f, 0.9f, true, 0.1f);
        TA::verdict(c, false, 0.0f);  // crater #1
        c.setBurstChannelObservation(24.0f, 0.20f, 0.9f, true, 0.1f);
        TA::verdict(c, false, 0.0f);  // crater #2 — confirmed
        const uint8_t cmd = TA::rxCmd(c);
        setenv("ULTRA_RX_EMA_HOLD", "0", 1);  // restore the suite baseline (off)
        return cmd;
    };
    const uint8_t off_cmd = run(false);
    const uint8_t on_cmd = run(true);
    if (off_cmd >= kRungIdxQam16R23)
        FAIL("knob OFF: confirmed crater must still demote (regression guard)");
    if (on_cmd != kRungIdxQam16R23)
        FAIL("knob ON: EMA-supported confirmed crater must HOLD 16QAM R2/3, got idx " +
             std::to_string(on_cmd));
    PASS();
    return true;
}

static bool test_ema_hold_still_demotes_sustained_failure() {
    TEST("EMA-HOLD does NOT latch: sustained failure demotes once the average drops");
    setenv("ULTRA_RX_EMA_HOLD", "1", 1);
    Connection c;
    TA::makeConnectedOFDM(c, CodeRate::R2_3, 20.0f, 0.20f, Modulation::QAM16);
    for (int i = 0; i < 3; ++i) {  // brief clean history
        c.setBurstChannelObservation(24.0f, 0.20f, 0.9f, true, 0.1f);
        TA::verdict(c, true, 0.95f);
    }
    // Sustained craters: each failed group is censored toward the rung floor, so the
    // ring average is dragged down and MUST cross below the anchor within a bounded
    // number of groups (strict '>' breaks the censor==anchor equality latch).
    int demoted_at = -1;
    for (int k = 1; k <= 10; ++k) {
        c.setBurstChannelObservation(24.0f, 0.20f, 0.9f, true, 0.1f);
        TA::verdict(c, false, 0.0f);
        if (TA::rxCmd(c) < kRungIdxQam16R23) { demoted_at = k; break; }
    }
    setenv("ULTRA_RX_EMA_HOLD", "0", 1);  // restore the suite baseline (off)
    if (demoted_at < 0)
        FAIL("sustained crater never demoted — EMA-HOLD latched (bug)");
    if (demoted_at < 2)
        FAIL("demoted on the very first confirmed crater — the hold did not engage");
    PASS();
    return true;
}

int main() {
    // MUST precede any Connection construction (env-latched statics).
    setenv("ULTRA_RX_RATE_AUTHORITY", "1", 1);
    setenv("ULTRA_ENABLE_PSK8_LADDER", "1", 1);
    unsetenv("ULTRA_DESCRIPTOR_MODE_SWITCH");  // legacy path -> pending_* observable
    unsetenv("ULTRA_RX_RATE_CMD");
    // ULTRA_RX_EMA_HOLD is default-OFF (reverted 2026-07-23) — matches the legacy
    // demote-machinery tests (two-crater rule, climb-dwell), which exercise the path that
    // runs when the hold does NOT engage. Set explicitly so the baseline is unambiguous
    // regardless of the default. The two test_ema_hold_* cases toggle it on and restore "0".
    setenv("ULTRA_RX_EMA_HOLD", "0", 1);

    std::cout << "RX-AUTHORITY suite\n";
    bool ok = true;
    ok &= test_verdict_maps_snr_to_rung();
    ok &= test_prediction_flat_identity();
    ok &= test_prediction_rejects_notched_channel();
    ok &= test_predictive_direct_jump();
    ok &= test_two_crater_rule();
    ok &= test_confirmed_crater_arms_climb_dwell();
    ok &= test_ema_hold_absorbs_supported_crater();
    ok &= test_ema_hold_still_demotes_sustained_failure();
    ok &= test_busy_defers_up_not_down();
    ok &= test_clean_never_commands_down();
    ok &= test_no_observation_no_command();
    ok &= test_sender_obeys_and_dedups();
    ok &= test_sender_ignores_none_and_garbage();
    ok &= test_operator_lock_overrides_authority();
    std::cout << tests_passed << "/" << tests_run << " passed\n";
    return (ok && tests_passed == tests_run) ? 0 : 1;
}
