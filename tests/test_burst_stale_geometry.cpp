// test_burst_stale_geometry.cpp
//
// BUG-BURST-STALE-GEOMETRY (2026-07-28) — regression gate for BOTH defects on the
// burst group-start path. They are independent and this file pins each one:
//
//   (A) TRUNCATION / ALIGNMENT — the defect that produces the operator-visible
//       TIMEOUT. checkIfReadyToDecode releases the group-start frame once `available`
//       clears the requirement IT computed; if the requirement then GROWS before
//       decodeCurrentFrame runs (a deferred profile apply at the top of the next
//       processBuffer, a GUI-thread setFixedFrameCodewords, or a commanded-rung
//       change), `frame_len = std::min(frame_len, available)` silently TRUNCATES
//       frame 1. burst_next_pos_ AND burst_data_start_abs_ are both derived from that
//       length, so the whole group lands early; refreshBurstAirEnd() inherits the
//       error and the F176 half-duplex ack interlock opens INSIDE the sender's own
//       key-down. The ack is transmitted into the peer's transmission, is lost, and
//       the sender eats a full ARQ RTO (17.5 s measured; 28.2 s between useful acks).
//       FIX: never arm a burst group from a truncated frame — defer and retry.
//
//   (B) STALE GEOMETRY — the defect that DESTROYS the group (0/N CWs on every frame).
//       A missed BURST_HEADER descriptor after a rung switch leaves the receiver
//       slicing with the PREVIOUS rung's constellation. The receiver is the rate
//       AUTHORITY, so it already commanded the sender's next rung: slice with the
//       COMMANDED rung's learned geometry instead of the latched one.
//
// BOTH fixes live behind ULTRA_COMMANDED_GEOMETRY (DEFAULT-ON, `=0` = exact pre-fix
// behaviour), so each test runs twice in-process: a BROKEN arm with the knob off
// (the fail-before evidence) and a FIXED arm with it on. That is only possible
// because streaming_decode_policy::commandedGeometryEnabled() reads the env FRESH
// rather than latching it in a function-local static — do not "optimize" that.
//
// Harness: real StreamingEncoder -> in-memory samples -> real StreamingDecoder, fed
// in 4800-sample chunks with one processBuffer() per chunk (single-threaded, no audio
// thread), copied from test_ofdm_snr_calibration.cpp. NO channel impairment: these
// are GEOMETRY defects, and a clean channel makes the fail/pass split structural
// rather than statistical.

#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "protocol/frame_v2.hpp"
#include "protocol/waveform_selection.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace ultra;
using namespace ultra::gui;
namespace v2 = ultra::protocol::v2;

namespace {

using Bytes = std::vector<uint8_t>;

int g_failures = 0;

#define EXPECT(cond)                                                            \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                       \
    } while (0)

// Production wideband OFDM geometry (mirrors measure_ack_fer::makeOFDMConfig and
// test_ofdm_snr_calibration::ofdmGeometry): FFT 1024, 59 carriers, LONG CP, pilots
// at the production adaptive spacing for the rung.
ModemConfig ofdmGeometry(Modulation mod, CodeRate rate) {
    ModemConfig cfg;
    cfg.fft_size = 1024;
    cfg.num_carriers = 59;
    cfg.sample_rate = 48000;
    cfg.center_freq = 1500.0f;
    cfg.cp_mode = CyclicPrefixMode::LONG;
    cfg.modulation = mod;
    cfg.code_rate = rate;
    cfg.use_pilots = true;
    cfg.pilot_spacing = ofdm_link_adaptation::recommendedPilotSpacing(mod, rate);
    return cfg;
}

void setKnob(bool on) {
#ifdef _WIN32
    _putenv_s("ULTRA_COMMANDED_GEOMETRY", on ? "1" : "0");
#else
    setenv("ULTRA_COMMANDED_GEOMETRY", on ? "1" : "0", /*overwrite=*/1);
#endif
}

struct BurstResult {
    bool fired = false;
    uint16_t frame_mask = 0;
    bool all_ok = false;
    uint8_t group_size = 0;
    size_t samples_at_callback = 0;   // fed-sample offset when the group finalized
    int frames_recovered = 0;
    uint32_t truncated_holds = 0;     // guard (A) fired this many times
    uint32_t commanded_arms = 0;      // guard (B) armed this many times
};

// Build the group's serialized DATA frames. cw MUST be stamped into the frame
// (byte 12) or the encoder sizes the physical frame from the header instead.
std::vector<Bytes> makeGroupFrames(int group_size, CodeRate rate, int cw) {
    std::vector<Bytes> frames;
    for (int f = 0; f < group_size; ++f) {
        Bytes payload(20, static_cast<uint8_t>(0x40 + f));
        auto frame = v2::makeFixedDataFrame("ALPHA", "BRAVO",
                                            static_cast<uint16_t>(f), payload, rate, cw);
        frames.push_back(frame.serialize());
    }
    return frames;
}

std::vector<float> withSilence(const std::vector<float>& burst) {
    std::vector<float> audio(48000, 0.0f);   // 1 s lead-in for chirp search
    audio.insert(audio.end(), burst.begin(), burst.end());
    audio.resize(audio.size() + 192000, 0.0f);  // 4 s trailing room for the tail frame
    return audio;
}

// ============================================================================
// TEST (A): never arm a burst group from a TRUNCATED group-start frame.
// ============================================================================
// Both stations are at the SAME rung (QPSK R3/4) — the geometry is CORRECT, so this
// isolates the alignment defect from the stale-constellation defect (B). The decoder
// starts LATCHED at a SHORT cw (so checkIfReadyToDecode releases the frame after only
// the short count) and the requirement is grown in the window between the readiness
// check and the decode — exactly what a deferred profile apply, a GUI-thread
// setFixedFrameCodewords, or (post-fix B) a commanded-rung change does in production,
// where the two resolves are separated by one processBuffer iteration.
BurstResult runTruncationTrial(bool knob_on) {
    setKnob(knob_on);

    // cw16 (the wide coherent rung) on the wire, cw4 latched at the receiver. The
    // gap has to be BIG for a reason that is itself informative: the full-anchor
    // search only declares sync once ~2.5 s of audio sits past the group start, so
    // `available` at the readiness check is already ~1.3 s. Only a frame LONGER than
    // that slack can be truncated — which is also why this defect shows up on the
    // widest rungs, exactly where a group crater costs the most airtime.
    constexpr int kGroup = 3;
    constexpr int kCw = 16;

    StreamingEncoder enc;
    enc.setMode(protocol::WaveformMode::OFDM_CHIRP);
    enc.setOFDMConfig(ofdmGeometry(Modulation::QPSK, CodeRate::R3_4));
    enc.setDataMode(Modulation::QPSK, CodeRate::R3_4);
    enc.setFixedFrameCodewords(kCw);
    enc.setBurstInterleave(false);
    enc.setBurstInterleaveGroupSize(kGroup);
    enc.setBurstDescriptorEnabled(false);
    enc.forceNextBurstGroupStartFullPreamble();

    const auto frames = makeGroupFrames(kGroup, CodeRate::R3_4, kCw);
    const auto audio = withSilence(enc.encodeBurstLight(frames));

    StreamingDecoder dec;
    dec.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                             ofdmGeometry(Modulation::QPSK, CodeRate::R3_4),
                             Modulation::QPSK, CodeRate::R3_4);
    dec.setFixedFrameCodewords(4);   // STALE latch: readiness gates on the short count
    dec.setBurstInterleave(false);
    dec.setBurstInterleaveGroupSize(kGroup);
    dec.applyPendingConfigForTesting();
    // NOTE: deliberately NOT expectFullOFDMAnchorOnce(). That arms the 2.5 s
    // CHIRP_MAX_SEARCH window, so sync is only declared once ~2.5 s of audio sits
    // past the group start — `available` is then far larger than any frame and NO
    // requirement growth can truncate. The connected light-LTS DATA-sync fallback is
    // both the tight-`available` path AND the path this bug actually travels (the
    // receiver missed the full dual-chirp anchor and fell back to DATA sync).

    BurstResult r;
    size_t fed = 0;
    dec.setBurstGroupCallback([&](uint16_t, const std::vector<Bytes>& got, bool all_ok,
                                  float, uint16_t mask, bool, uint8_t gsize) {
        if (r.fired) return;
        r.fired = true;
        r.frame_mask = mask;
        r.all_ok = all_ok;
        r.group_size = gsize;
        r.samples_at_callback = fed;
        for (const auto& want : frames) {
            for (const auto& g : got) {
                if (g == want) { ++r.frames_recovered; break; }
            }
        }
    });

    // Small chunks so `available` at the moment the readiness check passes is only
    // just above the STALE requirement — the tight window the guard must survive.
    constexpr size_t kChunk = 1200;
    bool bumped = false;
    for (size_t pos = 0; pos < audio.size() && !r.fired; pos += kChunk) {
        const size_t len = std::min(kChunk, audio.size() - pos);
        dec.feedAudio(audio.data() + pos, len);
        fed += len;
        dec.processBuffer();
        // THE STAGING: the readiness check has just passed at the cw4 length. Grow
        // the requirement before the decode runs. `available` is now ~cw4's frame
        // length while the decode will ask for cw8's (~0.8 s more audio), so
        // frame_len = min(frame_len, available) truncates unless the guard holds.
        if (!bumped && dec.stateForTesting() == DecoderState::DECODING) {
            dec.setFixedFrameCodewords(kCw);
            bumped = true;
        }
    }
    EXPECT(bumped);  // the staging must actually have happened
    r.truncated_holds = dec.truncatedBurstHoldsForTesting();
    r.commanded_arms = dec.commandedGeometryArmsForTesting();
    return r;
}

// ============================================================================
// TEST (B): a MISSED descriptor must be sliced with the COMMANDED rung.
// ============================================================================
// This mirrors the MEASURED rig failure: the receiver DEMOTED the sender (rig:
// 8PSK R2/3 -> QPSK R3/4), the sender obeyed immediately — a demote is never
// deferred, "a failing window never drains" (Connection::maybeObeyAuthorityCommand)
// — and the receiver then missed the BURST_HEADER anchor of the very first group at
// the new rung (setBurstDescriptorEnabled(false) is exactly "the descriptor was
// missed" from the decoder's point of view). It is still LATCHED at the previous,
// FASTER rung and slices the whole group with the wrong constellation.
//
// The rig's own 8PSK-R2/3-cw12 -> QPSK-R3/4-cw8 pair is airtime-IDENTICAL by design
// (cw is airtime-normalizing, connection_policy), which would hide the stride half of
// the defect. QPSK R3/4 cw8 (latched) -> QPSK R1/4 cw4 (commanded + on the wire) is
// the same DEMOTE direction with a genuinely different airtime — pilot spacing 8 vs 5
// AND cw 8 vs 4 — so the wrong-stride/air-end half is exercised too.
//
// PARAMETERIZED so the ADOPTION GATES get pinned by the same harness. "We commanded
// X" is NOT "the sender is at X" — Connection::maybeObeyAuthorityCommand declines a
// standing command on five distinct paths — and arming a rung the sender never took
// destroys a group that decodes perfectly today. Trials C and D below set the wire
// EQUAL to the latched rung (so a correct decoder recovers 5/5) and prove the
// resolver stays out of the way.
struct MissedDescriptorSpec {
    Modulation wire_mod;    CodeRate wire_rate;    int wire_cw;
    Modulation latch_mod;   CodeRate latch_rate;   int latch_cw;
    uint8_t    commanded_idx;                       // kRungIdxNone = no standing cmd
    Modulation learn_mod;   CodeRate learn_rate;   int learn_cw;
    // A descriptor decoded AFTER the command that announced a DIFFERENT rung — the
    // sender telling us on the wire that it has not taken the command.
    bool observed_declining_descriptor;
};

BurstResult runMissedDescriptorTrial(bool knob_on, const MissedDescriptorSpec& spec) {
    setKnob(knob_on);

    constexpr int kGroup = 5;

    StreamingEncoder enc;
    enc.setMode(protocol::WaveformMode::OFDM_CHIRP);
    enc.setOFDMConfig(ofdmGeometry(spec.wire_mod, spec.wire_rate));
    enc.setDataMode(spec.wire_mod, spec.wire_rate);
    enc.setFixedFrameCodewords(spec.wire_cw);
    enc.setBurstInterleave(false);
    enc.setBurstInterleaveGroupSize(kGroup);
    enc.setBurstDescriptorEnabled(false);   // <- THE MISSED DESCRIPTOR
    enc.forceNextBurstGroupStartFullPreamble();

    const auto frames = makeGroupFrames(kGroup, spec.wire_rate, spec.wire_cw);
    const auto audio = withSilence(enc.encodeBurstLight(frames));

    StreamingDecoder dec;
    dec.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                             ofdmGeometry(spec.latch_mod, spec.latch_rate),
                             spec.latch_mod, spec.latch_rate);
    dec.setFixedFrameCodewords(spec.latch_cw);
    dec.setBurstInterleave(false);
    dec.setBurstInterleaveGroupSize(kGroup);
    dec.applyPendingConfigForTesting();
    dec.expectFullOFDMAnchorOnce();
    // What the receiver already knows: it decoded a descriptor for the commanded rung
    // earlier in the session, so its (cw, z) is wire truth in the learned table.
    dec.seedRungGeometryForTesting(spec.learn_mod, spec.learn_rate, spec.learn_cw, 27);
    dec.setCommandedRungIndex(spec.commanded_idx);
    if (spec.observed_declining_descriptor) {
        dec.noteDescriptorRungForTesting(spec.latch_mod, spec.latch_rate);
    }

    BurstResult r;
    size_t fed = 0;
    dec.setBurstGroupCallback([&](uint16_t, const std::vector<Bytes>& got, bool all_ok,
                                  float, uint16_t mask, bool, uint8_t gsize) {
        if (r.fired) return;
        r.fired = true;
        r.frame_mask = mask;
        r.all_ok = all_ok;
        r.group_size = gsize;
        r.samples_at_callback = fed;
        for (const auto& want : frames) {
            for (const auto& g : got) {
                if (g == want) { ++r.frames_recovered; break; }
            }
        }
    });

    constexpr size_t kChunk = 4800;
    for (size_t pos = 0; pos < audio.size() && !r.fired; pos += kChunk) {
        const size_t len = std::min(kChunk, audio.size() - pos);
        dec.feedAudio(audio.data() + pos, len);
        fed += len;
        dec.processBuffer();
    }
    r.truncated_holds = dec.truncatedBurstHoldsForTesting();
    r.commanded_arms = dec.commandedGeometryArmsForTesting();
    return r;
}

void report(const char* name, const BurstResult& r) {
    std::fprintf(stderr,
                 "  %-28s fired=%d mask=0x%02X all_ok=%d group=%u recovered=%d "
                 "samples_at_cb=%zu trunc_holds=%u cmd_arms=%u\n",
                 name, r.fired ? 1 : 0, r.frame_mask, r.all_ok ? 1 : 0,
                 static_cast<unsigned>(r.group_size), r.frames_recovered,
                 r.samples_at_callback, r.truncated_holds, r.commanded_arms);
}

}  // namespace

int main() {
    LogLevel level = LogLevel::ERROR;
    if (const char* env = std::getenv("ULTRA_LOG_LEVEL")) {
        parseLogLevel(env, level);
    }
    setLogLevel(level);

    // Both arms run IN ONE PROCESS. That is the whole point of reading the knob
    // fresh instead of latching it in a function-local static: the knob=0 arm is
    // the live fail-before evidence, re-proved on every CI run, and it cannot rot
    // the way a commented-out "this used to fail" claim does.
    std::fprintf(stderr, "== (A) truncated group-start frame ==\n");
    const BurstResult a_broken = runTruncationTrial(/*knob_on=*/false);
    report("A knob=0 (pre-fix)", a_broken);
    const BurstResult a_fixed = runTruncationTrial(/*knob_on=*/true);
    report("A knob=1 (fixed)", a_fixed);

    // (B) The measured failure: a DEMOTE the sender took immediately, whose
    // descriptor we then missed. Wire = commanded = QPSK R1/4 cw4; latched = the
    // previous, faster QPSK R3/4 cw8.
    const MissedDescriptorSpec kAdopted{
        Modulation::QPSK, CodeRate::R1_4, 4,
        Modulation::QPSK, CodeRate::R3_4, 8,
        protocol::kRungIdxQpskR14,
        Modulation::QPSK, CodeRate::R1_4, 4,
        /*observed_declining_descriptor=*/false};
    std::fprintf(stderr, "== (B) missed descriptor / commanded geometry ==\n");
    const BurstResult b_broken = runMissedDescriptorTrial(/*knob_on=*/false, kAdopted);
    report("B knob=0 (pre-fix)", b_broken);
    const BurstResult b_fixed = runMissedDescriptorTrial(/*knob_on=*/true, kAdopted);
    report("B knob=1 (fixed)", b_fixed);

    // (C) ADOPTION GATE — DECLINED. Same demote command, but a descriptor decoded
    // since then announced the OLD rung: direct wire evidence the sender has not
    // taken it (legacy MODE_CHANGE round trip in flight, ladder snap, ULTRA_LOCK_RATE
    // on the far end...). The wire IS the latched rung, so the group decodes 5/5 —
    // unless the resolver arms the command anyway, which is the "destroys a healthy
    // group" regression this gate exists to prevent.
    const MissedDescriptorSpec kDeclined{
        Modulation::QPSK, CodeRate::R3_4, 8,   // wire == latched: a healthy group
        Modulation::QPSK, CodeRate::R3_4, 8,
        protocol::kRungIdxQpskR14,             // ...but a demote is standing
        Modulation::QPSK, CodeRate::R1_4, 4,
        /*observed_declining_descriptor=*/true};
    std::fprintf(stderr, "== (C) command DECLINED by the sender (must not arm) ==\n");
    const BurstResult c_fixed = runMissedDescriptorTrial(/*knob_on=*/true, kDeclined);
    report("C knob=1 declined", c_fixed);

    // (D) ADOPTION GATE — CLIMB. An UP command is DEFERRED by the sender while its TX
    // window drains, and the receiver re-stamps it on every ACK, so it can stand
    // un-adopted for many groups. Same shape as (C): wire == latched, healthy group.
    const MissedDescriptorSpec kDeferredClimb{
        Modulation::QPSK, CodeRate::R2_3, 8,
        Modulation::QPSK, CodeRate::R2_3, 8,
        protocol::kRungIdxQpskR34,             // a climb the sender is holding
        Modulation::QPSK, CodeRate::R3_4, 8,
        /*observed_declining_descriptor=*/false};
    std::fprintf(stderr, "== (D) climb command still deferred (must not arm) ==\n");
    const BurstResult d_fixed = runMissedDescriptorTrial(/*knob_on=*/true, kDeferredClimb);
    report("D knob=1 deferred climb", d_fixed);

    // ---- (A) ---------------------------------------------------------------
    // Pre-fix: the group is armed from a truncated frame, so every continuation
    // frame is sliced off-boundary. NO frame may survive.
    EXPECT(a_broken.frames_recovered == 0);
    // Post-fix: the guard defers the arm until the full frame is in the ring, so the
    // group decodes exactly as if nothing had happened — all 5 frames.
    EXPECT(a_fixed.fired);
    EXPECT(a_fixed.truncated_holds > 0);   // the guard actually fired (mechanism, not luck)
    EXPECT(a_broken.truncated_holds == 0); // and it is genuinely off in the broken arm
    EXPECT(a_fixed.frames_recovered == 3);
    EXPECT(a_fixed.frame_mask == 0x07);
    EXPECT(a_fixed.all_ok);
    // And the fix must not shorten the group's airtime accounting: the fixed arm
    // consumes at least as much audio before finalizing as the broken one.
    if (a_broken.fired) {
        EXPECT(a_fixed.samples_at_callback >= a_broken.samples_at_callback);
    }

    // ---- (B) ---------------------------------------------------------------
    // Pre-fix: sliced with the previous rung -> group destroyed.
    EXPECT(b_broken.frames_recovered == 0);
    // Post-fix: frames 2..5 decode. Frame 1 is a DELIBERATE erasure — it was
    // demodulated with the old constellation before the deferred profile change
    // could land, so its soft bits are pushed as zero-LLR (the SACK bitmap asks for
    // it in the next round trip, instead of a full 17.5 s RTO for the whole group).
    EXPECT(b_fixed.fired);
    EXPECT(b_fixed.commanded_arms > 0);    // armed from the COMMANDED rung, not a fluke
    EXPECT(b_broken.commanded_arms == 0);
    EXPECT((b_fixed.frame_mask & 0x1E) == 0x1E);
    EXPECT(b_fixed.frames_recovered >= 4);
    // The mis-strided-finalize half: the stale (wider) geometry made the pre-fix arm
    // consume a different amount of air before publishing its ack than the true
    // group occupies. The commanded stride must change that accounting.
    if (b_broken.fired) {
        EXPECT(b_fixed.samples_at_callback != b_broken.samples_at_callback);
    }

    // ---- (C) and (D): the adoption gates -----------------------------------
    // BOTH must be COMPLETELY inert: the wire is the latched rung, so the group is
    // healthy and must decode 5/5 exactly as it does today. An arm here is the
    // regression that would make this feature a net loss on the air.
    EXPECT(c_fixed.commanded_arms == 0);
    EXPECT(c_fixed.fired);
    EXPECT(c_fixed.frames_recovered == 5);
    EXPECT(c_fixed.all_ok);
    EXPECT(d_fixed.commanded_arms == 0);
    EXPECT(d_fixed.fired);
    EXPECT(d_fixed.frames_recovered == 5);
    EXPECT(d_fixed.all_ok);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_burst_stale_geometry: PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_burst_stale_geometry: %d FAILURE(S)\n", g_failures);
    return 1;
}
