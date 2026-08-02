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
// (A) is an unconditional correctness repair: both environment arms must hold and
// recover the truncated group start. Only (B), the commanded-geometry experiment,
// is controlled by ULTRA_COMMANDED_GEOMETRY (DEFAULT-OFF). Its two arms run in one
// process, which requires streaming_decode_policy::commandedGeometryEnabled() to
// read the environment fresh rather than latch it in a function-local static.
//
// Harness: real StreamingEncoder -> in-memory samples -> real StreamingDecoder, fed
// in 4800-sample chunks with one processBuffer() per chunk (single-threaded, no audio
// thread), copied from test_ofdm_snr_calibration.cpp. NO channel impairment: these
// are GEOMETRY defects, and a clean channel makes the fail/pass split structural
// rather than statistical.

#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "protocol/connection_policy.hpp"
#include "protocol/frame_v2.hpp"
#include "protocol/waveform_selection.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/timing_profiler.hpp"
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
    uint64_t air_remaining_at_callback = 0;
    int frames_recovered = 0;
    uint32_t truncated_holds = 0;     // guard (A) fired this many times
    uint32_t commanded_arms = 0;      // guard (B) armed this many times
    bool truncation_hold_active = false;
};

struct MarkerProvenanceResult {
    int callbacks = 0;
    int unknown_outcomes = 0;
    uint16_t frame_mask = 0;
    bool all_ok = false;
    bool geometry_proven = false;
    uint8_t group_size = 0;
    int frames_recovered = 0;
};

struct PhysicalTailGroupResult {
    int callbacks = 0;
    uint16_t frame_mask = 0;
    bool all_ok = false;
    bool geometry_proven = false;
    uint8_t group_size = 0;
    int frames_recovered = 0;
    size_t samples_at_callback = 0;
    size_t physical_end_sample = 0;
    uint64_t fixed_decode_calls = 0;
};

struct StandaloneGateResult {
    int callbacks = 0;
    int backstop_callbacks = 0;
    bool backstop_payload_seen = false;
    bool physical_complete = false;
    uint64_t air_remaining_at_callback = 0;
    uint64_t one_sample_before_deadline = 0;
    uint64_t at_deadline = 0;
};

struct FailedAnchorGateResult {
    int callbacks = 0;
    int backstop_callbacks = 0;
    uint64_t before_reset = 0;
    uint64_t after_reset = 0;
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

bool sameLogicalDataFrame(const Bytes& a, const Bytes& b) {
    auto normalize = [](const Bytes& bytes) {
        auto frame = v2::DataFrame::deserialize(bytes);
        if (!frame) return bytes;
        frame->flags = static_cast<uint8_t>(
            frame->flags & ~v2::Flags::PHYSICAL_BURST_END);
        return frame->serialize();
    };
    return normalize(a) == normalize(b);
}

std::vector<float> withSilence(const std::vector<float>& burst) {
    std::vector<float> audio(48000, 0.0f);   // 1 s lead-in for chirp search
    audio.insert(audio.end(), burst.begin(), burst.end());
    audio.resize(audio.size() + 192000, 0.0f);  // 4 s trailing room for the tail frame
    return audio;
}

void feedSilenceSamples(StreamingDecoder& dec, uint64_t samples) {
    std::vector<float> zeros(4800, 0.0f);
    while (samples > 0) {
        const size_t n = static_cast<size_t>(
            std::min<uint64_t>(samples, static_cast<uint64_t>(zeros.size())));
        dec.feedAudio(zeros.data(), n);
        samples -= n;
    }
}

void feedAndProcessSilenceSamples(StreamingDecoder& dec, uint64_t samples) {
    std::vector<float> zeros(4800, 0.0f);
    while (samples > 0) {
        const size_t n = static_cast<size_t>(
            std::min<uint64_t>(samples, static_cast<uint64_t>(zeros.size())));
        dec.feedAudio(zeros.data(), n);
        dec.processBuffer();
        samples -= n;
    }
}

constexpr uint64_t kAnchoredBackstopWindowSamples =
    48ull * (12000ull + 1410ull + 1200ull);

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

    const auto frames = makeGroupFrames(kGroup, CodeRate::R3_4, kCw);
    const auto audio = withSilence(enc.encodeBurstLight(
        frames, BurstAnchorOptions{/*force_full_group_start=*/true,
                                   /*keep_skip_streak=*/false}));

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
                                  float, uint16_t mask, bool, uint8_t gsize, bool) {
        if (r.fired) return;
        r.fired = true;
        r.frame_mask = mask;
        r.all_ok = all_ok;
        r.group_size = gsize;
        r.samples_at_callback = fed;
        r.air_remaining_at_callback = dec.burstAirSamplesRemaining();
        for (const auto& want : frames) {
            for (const auto& g : got) {
                if (sameLogicalDataFrame(g, want)) { ++r.frames_recovered; break; }
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
    r.truncation_hold_active = dec.truncationHoldActiveForTesting();
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

    const auto frames = makeGroupFrames(kGroup, spec.wire_rate, spec.wire_cw);
    const auto audio = withSilence(enc.encodeBurstLight(
        frames, BurstAnchorOptions{/*force_full_group_start=*/true,
                                   /*keep_skip_streak=*/false}));

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
                                  float, uint16_t mask, bool, uint8_t gsize, bool) {
        if (r.fired) return;
        r.fired = true;
        r.frame_mask = mask;
        r.all_ok = all_ok;
        r.group_size = gsize;
        r.samples_at_callback = fed;
        r.air_remaining_at_callback = dec.burstAirSamplesRemaining();
        for (const auto& want : frames) {
            for (const auto& g : got) {
                if (sameLogicalDataFrame(g, want)) { ++r.frames_recovered; break; }
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
    r.truncation_hold_active = dec.truncationHoldActiveForTesting();
    return r;
}

// ============================================================================
// TEST (E): a sign-only marker is not enough to manufacture a burst group.
// ============================================================================
// A standalone repair can carry a full OFDM anchor. If noise rotates the LTS sign,
// it looks exactly like a group marker even though no descriptor preceded it and no
// continuation member follows it. The historical path filled the declared remainder
// from trailing silence and emitted a synthetic 0/N group callback. Keep the marked
// LTS but erase frame 1's data symbols to reproduce that exact 0/N shape. The paired
// arm sends the real second member and proves that two substantive physical members
// still promote the marker to a genuine group.
MarkerProvenanceResult runMarkerProvenanceTrial(bool include_second_member,
                                                bool corrupt_tail = false) {
    constexpr int kGroup = 2;
    constexpr int kCw = 4;

    StreamingEncoder enc;
    enc.setMode(protocol::WaveformMode::OFDM_CHIRP);
    enc.setOFDMConfig(ofdmGeometry(Modulation::QPSK, CodeRate::R1_4));
    enc.setDataMode(Modulation::QPSK, CodeRate::R1_4);
    enc.setFixedFrameCodewords(kCw);
    enc.setBurstInterleave(false);
    enc.setBurstInterleaveGroupSize(kGroup);
    enc.setBurstDescriptorEnabled(false);  // marker has no descriptor provenance

    const auto frames = makeGroupFrames(kGroup, CodeRate::R1_4, kCw);
    auto burst = enc.encodeBurstLight(
        frames, BurstAnchorOptions{/*force_full_group_start=*/true,
                                   /*keep_skip_streak=*/false});
    EXPECT(!burst.empty());

    // The second member is exactly one light [LTS + data] block. Therefore the
    // prefix before it is the complete marked first member, including its full
    // chirp anchor. This derives the cut from production's own waveform geometry.
    const size_t member_samples = static_cast<size_t>(
        enc.getWaveform()->getMinSamplesForCWCount(kCw));
    EXPECT(burst.size() > member_samples);
    const size_t first_member_samples = burst.size() - member_samples;

    if (!include_second_member) {
        burst.resize(first_member_samples);
        // Preserve the full preamble (and its negated first LTS marker), but erase
        // all data symbols. Before the provenance gate this finalized as 0/2 once
        // the following silence was sliced as the missing member.
        const size_t full_preamble_samples = enc.getWaveform()->generatePreamble().size();
        EXPECT(full_preamble_samples < burst.size());
        std::fill(burst.begin() + static_cast<std::ptrdiff_t>(full_preamble_samples),
                  burst.end(), 0.0f);
    } else if (corrupt_tail) {
        // Preserve the second member's LTS and signal energy, but replace every
        // payload sample with deterministic broadband noise. Demodulation remains
        // substantive while LDPC/CRC rejects the DATA, so its tail flag is
        // unavailable. Reaching configured N=2 must emit a callback with unproven
        // geometry.
        const size_t light_preamble_samples =
            enc.getWaveform()->generateDataPreamble().size();
        const size_t tail_data_start = first_member_samples + light_preamble_samples;
        EXPECT(tail_data_start < burst.size());
        uint32_t noise_state = 0xD15EA5Eu;
        for (size_t i = tail_data_start; i < burst.size(); ++i) {
            noise_state = noise_state * 1664525u + 1013904223u;
            burst[i] = (noise_state & 0x80000000u) ? 0.10f : -0.10f;
        }
    }

    const auto audio = withSilence(burst);

    StreamingDecoder dec;
    dec.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                             ofdmGeometry(Modulation::QPSK, CodeRate::R1_4),
                             Modulation::QPSK, CodeRate::R1_4);
    dec.setFixedFrameCodewords(kCw);
    dec.setBurstInterleave(false);
    dec.setBurstInterleaveGroupSize(kGroup);
    dec.applyPendingConfigForTesting();
    dec.expectFullOFDMAnchorOnce();

    MarkerProvenanceResult r;
    dec.setBurstOutcomeUnknownCallback([&]() { ++r.unknown_outcomes; });
    dec.setBurstGroupCallback([&](uint16_t, const std::vector<Bytes>& got, bool all_ok,
                                  float, uint16_t mask, bool, uint8_t gsize,
                                  bool geometry_proven) {
        ++r.callbacks;
        r.frame_mask = mask;
        r.all_ok = all_ok;
        r.geometry_proven = geometry_proven;
        r.group_size = gsize;
        for (const auto& want : frames) {
            for (const auto& actual : got) {
                if (sameLogicalDataFrame(actual, want)) {
                    ++r.frames_recovered;
                    break;
                }
            }
        }
    });

    constexpr size_t kChunk = 4800;
    for (size_t pos = 0; pos < audio.size(); pos += kChunk) {
        const size_t len = std::min(kChunk, audio.size() - pos);
        dec.feedAudio(audio.data() + pos, len);
        dec.processBuffer();
    }
    return r;
}

// A missed descriptor leaves the receiver on its configured fallback group size.
// The wire may legitimately be shorter (the sender only had four ARQ frames left),
// and the last independently-decodable DATA frame carries a CRC-protected physical
// tail marker. Production fixed_default_04 demonstrated the pre-fix failure exactly:
// all four real frames decoded, then the accumulator sliced two blocks of silence to
// satisfy stale fallback N=6 and reported a delayed 4/6 partial group. The tail is
// stronger evidence than guessed geometry, so it must finalize the collected 4/4
// immediately. The decode counter also pins the early-decode cache alignment: frames
// 2..4 are decoded as they arrive and must not be decoded a second time at finalize.
PhysicalTailGroupResult runMissedDescriptorPhysicalTailTrial(bool corrupt_tail = false) {
    constexpr int kWireGroup = 4;
    constexpr int kFallbackGroup = 6;
    constexpr int kCw = 4;

    StreamingEncoder enc;
    enc.setMode(protocol::WaveformMode::OFDM_CHIRP);
    enc.setOFDMConfig(ofdmGeometry(Modulation::QPSK, CodeRate::R1_4));
    enc.setDataMode(Modulation::QPSK, CodeRate::R1_4);
    enc.setFixedFrameCodewords(kCw);
    enc.setBurstInterleave(false);
    enc.setBurstInterleaveGroupSize(kWireGroup);
    enc.setBurstDescriptorEnabled(false);  // receiver must use stale fallback geometry

    const auto frames = makeGroupFrames(kWireGroup, CodeRate::R1_4, kCw);
    auto burst = enc.encodeBurstLight(
        frames, BurstAnchorOptions{/*force_full_group_start=*/true,
                                   /*keep_skip_streak=*/false});
    EXPECT(!burst.empty());
    if (corrupt_tail) {
        const size_t member_samples = static_cast<size_t>(
            enc.getWaveform()->getMinSamplesForCWCount(kCw));
        const size_t light_preamble_samples =
            enc.getWaveform()->generateDataPreamble().size();
        EXPECT(burst.size() > member_samples);
        const size_t tail_data_start =
            burst.size() - member_samples + light_preamble_samples;
        EXPECT(tail_data_start < burst.size());
        uint32_t noise_state = 0x51A1E123u;
        for (size_t i = tail_data_start; i < burst.size(); ++i) {
            noise_state = noise_state * 1664525u + 1013904223u;
            burst[i] = (noise_state & 0x80000000u) ? 0.10f : -0.10f;
        }
    }
    const auto audio = withSilence(burst);

    StreamingDecoder dec;
    dec.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                             ofdmGeometry(Modulation::QPSK, CodeRate::R1_4),
                             Modulation::QPSK, CodeRate::R1_4);
    dec.setFixedFrameCodewords(kCw);
    dec.setBurstInterleave(false);
    dec.setBurstInterleaveGroupSize(kFallbackGroup);
    dec.applyPendingConfigForTesting();
    dec.expectFullOFDMAnchorOnce();

    auto& profile = ultra::timing::globalDecoderProfile();
    profile.reset();

    PhysicalTailGroupResult r;
    r.physical_end_sample = 48000 + burst.size();  // withSilence() lead-in
    size_t fed = 0;
    dec.setBurstGroupCallback([&](uint16_t, const std::vector<Bytes>& got, bool all_ok,
                                  float, uint16_t mask, bool, uint8_t gsize,
                                  bool geometry_proven) {
        ++r.callbacks;
        r.frame_mask = mask;
        r.all_ok = all_ok;
        r.geometry_proven = geometry_proven;
        r.group_size = gsize;
        r.samples_at_callback = fed;
        for (const auto& want : frames) {
            for (const auto& actual : got) {
                if (sameLogicalDataFrame(actual, want)) {
                    ++r.frames_recovered;
                    break;
                }
            }
        }
    });

    constexpr size_t kChunk = 4800;
    for (size_t pos = 0; pos < audio.size() && r.callbacks == 0; pos += kChunk) {
        const size_t len = std::min(kChunk, audio.size() - pos);
        dec.feedAudio(audio.data() + pos, len);
        fed += len;
        dec.processBuffer();
    }
    r.fixed_decode_calls =
        profile.decode_fixed_frame_total.count.load(std::memory_order_relaxed);
    return r;
}

// A full anchor is also the normal wire shape for a singleton repair. It need
// not carry FINAL, and its ARQ timeout is shorter than the conservative unknown-
// burst ceiling. A successful classic decode at that same anchor must therefore
// clear the provisional gate before the protocol callback runs.
StandaloneGateResult runStandaloneNonFinalGateTrial() {
    constexpr int kCw = 4;

    auto frame = v2::makeFixedDataFrame(
        "ALPHA", "BRAVO", 41, Bytes(20, 0x5A), CodeRate::R1_4, kCw);
    EXPECT((frame.flags & v2::Flags::FINAL) == 0);
    const Bytes serialized = frame.serialize();

    StreamingEncoder enc;
    enc.setMode(protocol::WaveformMode::OFDM_CHIRP);
    enc.setOFDMConfig(ofdmGeometry(Modulation::QPSK, CodeRate::R1_4));
    enc.setDataMode(Modulation::QPSK, CodeRate::R1_4);
    enc.setFixedFrameCodewords(kCw);
    const auto audio = withSilence(enc.encodeFrame(serialized));

    StreamingDecoder dec;
    dec.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                             ofdmGeometry(Modulation::QPSK, CodeRate::R1_4),
                             Modulation::QPSK, CodeRate::R1_4);
    dec.setFixedFrameCodewords(kCw);
    dec.applyPendingConfigForTesting();
    dec.expectFullOFDMAnchorOnce();

    StandaloneGateResult r;
    dec.setAnchoredBurstNoGroupCallback([&](bool payload_seen) {
        ++r.backstop_callbacks;
        r.backstop_payload_seen = payload_seen;
    });
    dec.setFrameCallback([&](const DecodeResult& got) {
        if (!got.success || got.frame_data != serialized) return;
        ++r.callbacks;
        r.physical_complete = got.physical_turn_complete;
        r.air_remaining_at_callback = dec.burstAirSamplesRemaining();
    });

    constexpr size_t kChunk = 4800;
    for (size_t pos = 0; pos < audio.size() && r.callbacks == 0; pos += kChunk) {
        const size_t len = std::min(kChunk, audio.size() - pos);
        dec.feedAudio(audio.data() + pos, len);
        dec.processBuffer();
    }
    // A successful singleton is the completed physical turn.  Its accepted
    // anchor must not manufacture an ANCHORED-BURST backstop callback later.
    feedAndProcessSilenceSamples(
        dec, kAnchoredBackstopWindowSamples + 4800);
    return r;
}

// Reproduce the collision's decoder shape without relying on ARQ: accept one
// expected full anchor, make that frame's body unreadable, then append a valid
// light DATA member. The later callback must still see the original air gate.
StandaloneGateResult runLaterLightFallbackGateTrial(bool physical_tail) {
    constexpr int kCw = 4;
    auto head = v2::makeFixedDataFrame(
        "ALPHA", "BRAVO", 50, Bytes(20, 0x71), CodeRate::R1_4, kCw);
    auto later = v2::makeFixedDataFrame(
        "ALPHA", "BRAVO", 51, Bytes(20, 0x72), CodeRate::R1_4, kCw);
    if (physical_tail) {
        later.flags |= v2::Flags::PHYSICAL_BURST_END;
    }
    const Bytes later_serialized = later.serialize();

    StreamingEncoder enc;
    enc.setMode(protocol::WaveformMode::OFDM_CHIRP);
    enc.setOFDMConfig(ofdmGeometry(Modulation::QPSK, CodeRate::R1_4));
    enc.setDataMode(Modulation::QPSK, CodeRate::R1_4);
    enc.setFixedFrameCodewords(kCw);
    auto failed_head = enc.encodeFrame(head.serialize());
    const size_t preamble_samples = enc.getWaveform()->generatePreamble().size();
    EXPECT(preamble_samples < failed_head.size());
    std::fill(failed_head.begin() + static_cast<std::ptrdiff_t>(preamble_samples),
              failed_head.end(), 0.0f);
    const auto light_later = enc.encodeFrameLight(later_serialized);
    failed_head.insert(failed_head.end(), light_later.begin(), light_later.end());
    const auto audio = withSilence(failed_head);

    StreamingDecoder dec;
    dec.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                             ofdmGeometry(Modulation::QPSK, CodeRate::R1_4),
                             Modulation::QPSK, CodeRate::R1_4);
    dec.setFixedFrameCodewords(kCw);
    dec.applyPendingConfigForTesting();
    dec.expectFullOFDMAnchorOnce();

    StandaloneGateResult r;
    dec.setAnchoredBurstNoGroupCallback([&](bool payload_seen) {
        ++r.backstop_callbacks;
        r.backstop_payload_seen = payload_seen;
    });
    dec.setFrameCallback([&](const DecodeResult& got) {
        if (!got.success || got.frame_data != later_serialized) return;
        ++r.callbacks;
        r.physical_complete = got.physical_turn_complete;
        r.air_remaining_at_callback = dec.burstAirSamplesRemaining();
    });
    constexpr size_t kChunk = 4800;
    for (size_t pos = 0; pos < audio.size() && r.callbacks == 0; pos += kChunk) {
        const size_t len = std::min(kChunk, audio.size() - pos);
        dec.feedAudio(audio.data() + pos, len);
        dec.processBuffer();
    }
    if (r.air_remaining_at_callback > 0) {
        feedSilenceSamples(dec, r.air_remaining_at_callback - 1);
        r.one_sample_before_deadline = dec.burstAirSamplesRemaining();
        feedSilenceSamples(dec, 1);
        r.at_deadline = dec.burstAirSamplesRemaining();
    }
    // An unmarked later member retains the recovery backstop. A CRC-protected
    // physical tail clears it and authorizes the immediate cumulative ACK path.
    feedAndProcessSilenceSamples(
        dec, kAnchoredBackstopWindowSamples + 4800);
    return r;
}

// A detected expected anchor with an unreadable body is the precondition for
// later light duplicates taking the classic callback path. Pin both the initial
// provisional arm and its reset lifecycle with a real chirp/LTS encode.
FailedAnchorGateResult runFailedAnchorResetGateTrial() {
    constexpr int kCw = 4;
    auto frame = v2::makeFixedDataFrame(
        "ALPHA", "BRAVO", 42, Bytes(20, 0x6B), CodeRate::R1_4, kCw);

    StreamingEncoder enc;
    enc.setMode(protocol::WaveformMode::OFDM_CHIRP);
    enc.setOFDMConfig(ofdmGeometry(Modulation::QPSK, CodeRate::R1_4));
    enc.setDataMode(Modulation::QPSK, CodeRate::R1_4);
    enc.setFixedFrameCodewords(kCw);
    auto signal = enc.encodeFrame(frame.serialize());
    const size_t preamble_samples = enc.getWaveform()->generatePreamble().size();
    EXPECT(preamble_samples < signal.size());
    std::fill(signal.begin() + static_cast<std::ptrdiff_t>(preamble_samples),
              signal.end(), 0.0f);
    const auto audio = withSilence(signal);

    StreamingDecoder dec;
    dec.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                             ofdmGeometry(Modulation::QPSK, CodeRate::R1_4),
                             Modulation::QPSK, CodeRate::R1_4);
    dec.setFixedFrameCodewords(kCw);
    dec.applyPendingConfigForTesting();
    dec.expectFullOFDMAnchorOnce();

    FailedAnchorGateResult r;
    dec.setAnchoredBurstNoGroupCallback([&](bool) { ++r.backstop_callbacks; });
    dec.setFrameCallback([&](const DecodeResult&) { ++r.callbacks; });
    constexpr size_t kChunk = 4800;
    for (size_t pos = 0; pos < audio.size(); pos += kChunk) {
        const size_t len = std::min(kChunk, audio.size() - pos);
        dec.feedAudio(audio.data() + pos, len);
        dec.processBuffer();
    }
    r.before_reset = dec.burstAirSamplesRemaining();
    feedAndProcessSilenceSamples(
        dec, kAnchoredBackstopWindowSamples + 4800);
    dec.reset(/*reset_doppler_coherence=*/false);
    r.after_reset = dec.burstAirSamplesRemaining();
    return r;
}

void checkProvisionalCeilingCoversWorstPhysicalVector() {
    constexpr Modulation kMod = Modulation::QPSK;
    constexpr CodeRate kRate = CodeRate::R2_3;
    constexpr int kCw = 8;
    size_t group = 1;
    while (group < protocol::connection_policy::kToneBurstAckWindowCapFrames &&
           protocol::connection_policy::wideOFDMBurstAirtimeMs(
               kMod, kRate, group + 1, kCw) <=
               protocol::connection_policy::kMaxBurstAirtimeCeilingMs) {
        ++group;
    }

    StreamingEncoder enc;
    enc.setMode(protocol::WaveformMode::OFDM_CHIRP);
    enc.setOFDMConfig(ofdmGeometry(kMod, kRate));
    enc.setDataMode(kMod, kRate);
    enc.setFixedFrameCodewords(kCw);
    enc.setBurstInterleave(false);
    enc.setBurstInterleaveGroupSize(static_cast<int>(group));
    enc.setBurstDescriptorEnabled(true);
    enc.setBurstDescriptorIdentity("ALPHA", "BRAVO");
    // Worst wire shape: a full BURST_HEADER descriptor followed by another full
    // reliability anchor at the marked DATA group start.
    const auto physical = enc.encodeBurstLight(
        makeGroupFrames(static_cast<int>(group), kRate, kCw),
        BurstAnchorOptions{/*force_full_group_start=*/true,
                           /*keep_skip_streak=*/false});

    const uint64_t training_offset =
        protocol::connection_policy::kWideOFDMFullAnchorExtraSamples;
    EXPECT(physical.size() > training_offset);
    const uint64_t remaining_from_descriptor_training =
        static_cast<uint64_t>(physical.size()) - training_offset;
    const uint64_t provisional_ceiling_samples =
        static_cast<uint64_t>(
            protocol::connection_policy::maxWideOFDMPhysicalTurnAfterAnchorTrainingMs()) *
        protocol::connection_policy::kOFDMSampleRate / 1000u;
    EXPECT(remaining_from_descriptor_training <= provisional_ceiling_samples);
}

void report(const char* name, const BurstResult& r) {
    std::fprintf(stderr,
                 "  %-28s fired=%d mask=0x%02X all_ok=%d group=%u recovered=%d "
                 "samples_at_cb=%zu air_rem=%llu trunc_holds=%u cmd_arms=%u hold_active=%d\n",
                 name, r.fired ? 1 : 0, r.frame_mask, r.all_ok ? 1 : 0,
                 static_cast<unsigned>(r.group_size), r.frames_recovered,
                 r.samples_at_callback,
                 static_cast<unsigned long long>(r.air_remaining_at_callback),
                 r.truncated_holds, r.commanded_arms,
                 r.truncation_hold_active ? 1 : 0);
}

}  // namespace

int main() {
    LogLevel level = LogLevel::ERROR;
    if (const char* env = std::getenv("ULTRA_LOG_LEVEL")) {
        parseLogLevel(env, level);
    }
    setLogLevel(level);

    // Both environment arms run IN ONE PROCESS. For (A), both must exercise the
    // unconditional alignment guard. For (B), the off/on pair remains live
    // fail-before evidence for the separately gated commanded-geometry experiment.
    std::fprintf(stderr, "== (A) truncated group-start frame ==\n");
    const BurstResult a_knob_off = runTruncationTrial(/*knob_on=*/false);
    report("A knob=0 (guard on)", a_knob_off);
    const BurstResult a_knob_on = runTruncationTrial(/*knob_on=*/true);
    report("A knob=1 (guard on)", a_knob_on);

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

    std::fprintf(stderr, "== (E) sign-only marker provenance ==\n");
    const MarkerProvenanceResult e_orphan =
        runMarkerProvenanceTrial(/*include_second_member=*/false);
    const MarkerProvenanceResult e_confirmed =
        runMarkerProvenanceTrial(/*include_second_member=*/true);
    const MarkerProvenanceResult e_stale_fallback =
        runMarkerProvenanceTrial(/*include_second_member=*/true,
                                 /*corrupt_tail=*/true);
    const StandaloneGateResult f_singleton = runStandaloneNonFinalGateTrial();
    const StandaloneGateResult f_later_light =
        runLaterLightFallbackGateTrial(/*physical_tail=*/false);
    const StandaloneGateResult f_later_tail =
        runLaterLightFallbackGateTrial(/*physical_tail=*/true);
    const FailedAnchorGateResult f_failed_anchor = runFailedAnchorResetGateTrial();
    std::fprintf(stderr, "== (G) missed descriptor / CRC physical tail ==\n");
    const PhysicalTailGroupResult g_physical_tail =
        runMissedDescriptorPhysicalTailTrial();
    const PhysicalTailGroupResult g_stale_fallback =
        runMissedDescriptorPhysicalTailTrial(/*corrupt_tail=*/true);
    std::fprintf(stderr,
                 "  G callbacks=%d group=%u mask=0x%02X recovered=%d all_ok=%d "
                 "callback_sample=%zu physical_end=%zu fixed_decodes=%llu\n",
                 g_physical_tail.callbacks,
                 static_cast<unsigned>(g_physical_tail.group_size),
                 g_physical_tail.frame_mask, g_physical_tail.frames_recovered,
                 g_physical_tail.all_ok ? 1 : 0,
                 g_physical_tail.samples_at_callback,
                 g_physical_tail.physical_end_sample,
                 static_cast<unsigned long long>(g_physical_tail.fixed_decode_calls));
    std::fprintf(stderr,
                 "  G stale callbacks=%d group=%u mask=0x%02X recovered=%d all_ok=%d "
                 "geometry=%d callback_sample=%zu physical_end=%zu fixed_decodes=%llu\n",
                 g_stale_fallback.callbacks,
                 static_cast<unsigned>(g_stale_fallback.group_size),
                 g_stale_fallback.frame_mask, g_stale_fallback.frames_recovered,
                 g_stale_fallback.all_ok ? 1 : 0,
                 g_stale_fallback.geometry_proven ? 1 : 0,
                 g_stale_fallback.samples_at_callback,
                 g_stale_fallback.physical_end_sample,
                 static_cast<unsigned long long>(g_stale_fallback.fixed_decode_calls));
    checkProvisionalCeilingCoversWorstPhysicalVector();
    std::fprintf(stderr,
                 "  E orphan callbacks=%d unknown=%d mask=0x%02X recovered=%d; "
                 "confirmed callbacks=%d mask=0x%02X recovered=%d geometry=%d; "
                 "stale-tail callbacks=%d mask=0x%02X recovered=%d geometry=%d\n",
                 e_orphan.callbacks, e_orphan.unknown_outcomes,
                 e_orphan.frame_mask, e_orphan.frames_recovered,
                 e_confirmed.callbacks, e_confirmed.frame_mask,
                 e_confirmed.frames_recovered, e_confirmed.geometry_proven ? 1 : 0,
                 e_stale_fallback.callbacks, e_stale_fallback.frame_mask,
                 e_stale_fallback.frames_recovered,
                 e_stale_fallback.geometry_proven ? 1 : 0);

    // ---- (A) ---------------------------------------------------------------
    // The alignment guard is correctness-critical and independent of the default-off
    // commanded-geometry experiment. Both arms must defer the short slice and recover
    // the same complete three-member group.
    EXPECT(a_knob_off.fired);
    EXPECT(a_knob_off.truncated_holds > 0);
    EXPECT(a_knob_off.frames_recovered == 3);
    EXPECT(a_knob_off.frame_mask == 0x07);
    EXPECT(a_knob_off.all_ok);
    EXPECT(a_knob_on.fired);
    EXPECT(a_knob_on.truncated_holds > 0);
    EXPECT(a_knob_on.frames_recovered == 3);
    EXPECT(a_knob_on.frame_mask == 0x07);
    EXPECT(a_knob_on.all_ok);

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

    // ---- (E): marker provenance --------------------------------------------
    // One sign decision plus trailing silence is not a group and must not emit a
    // synthetic 0/N callback/ACK. A second process-valid physical member confirms
    // the cadence, after which the normal complete-group path remains unchanged.
    EXPECT(e_orphan.callbacks == 0);
    EXPECT(e_orphan.unknown_outcomes == 1);
    EXPECT(e_confirmed.unknown_outcomes == 0);
    EXPECT(e_stale_fallback.unknown_outcomes == 0);
    EXPECT(e_confirmed.callbacks == 1);
    EXPECT(e_confirmed.group_size == 2);
    EXPECT(e_confirmed.frame_mask == 0x03);
    EXPECT(e_confirmed.frames_recovered == 2);
    EXPECT(e_confirmed.all_ok);
    EXPECT(e_confirmed.geometry_proven);
    EXPECT(e_stale_fallback.callbacks == 1);
    EXPECT(e_stale_fallback.group_size == 2);
    EXPECT(e_stale_fallback.frame_mask == 0x01);
    EXPECT(e_stale_fallback.frames_recovered == 1);
    EXPECT(!e_stale_fallback.all_ok);
    EXPECT(!e_stale_fallback.geometry_proven);

    // ---- (F): provisional full-anchor ACK gate -----------------------------
    EXPECT(f_singleton.callbacks == 1);
    EXPECT(f_singleton.physical_complete);
    EXPECT(f_singleton.air_remaining_at_callback == 0);
    EXPECT(f_singleton.backstop_callbacks == 0);
    EXPECT(f_later_light.callbacks == 1);
    EXPECT(!f_later_light.physical_complete);
    EXPECT(f_later_light.air_remaining_at_callback > 0);
    EXPECT(f_later_light.one_sample_before_deadline == 1);
    EXPECT(f_later_light.at_deadline == 0);
    EXPECT(f_later_light.backstop_callbacks == 1);
    EXPECT(f_later_light.backstop_payload_seen);
    EXPECT(f_later_tail.callbacks == 1);
    EXPECT(f_later_tail.physical_complete);
    EXPECT(f_later_tail.air_remaining_at_callback == 0);
    EXPECT(f_later_tail.backstop_callbacks == 0);
    EXPECT(f_failed_anchor.callbacks == 0);
    EXPECT(f_failed_anchor.before_reset > 0);
    EXPECT(f_failed_anchor.backstop_callbacks == 1);
    EXPECT(f_failed_anchor.after_reset == 0);
    EXPECT(!a_knob_off.truncation_hold_active);
    EXPECT(!a_knob_on.truncation_hold_active);
    EXPECT(a_knob_off.air_remaining_at_callback == 0);
    EXPECT(a_knob_on.air_remaining_at_callback == 0);
    EXPECT(b_fixed.air_remaining_at_callback == 0);
    EXPECT(c_fixed.air_remaining_at_callback == 0);
    EXPECT(d_fixed.air_remaining_at_callback == 0);

    // ---- (G): exact physical-tail finalization -----------------------------
    EXPECT(g_physical_tail.callbacks == 1);
    EXPECT(g_physical_tail.group_size == 4);
    EXPECT(g_physical_tail.frame_mask == 0x0F);
    EXPECT(g_physical_tail.frames_recovered == 4);
    EXPECT(g_physical_tail.all_ok);
    EXPECT(g_physical_tail.geometry_proven);
    // Callback may trail the exact sample boundary by at most one feed chunk. It
    // must not consume even one guessed fallback frame of trailing silence.
    EXPECT(g_physical_tail.samples_at_callback >= g_physical_tail.physical_end_sample);
    EXPECT(g_physical_tail.samples_at_callback <=
           g_physical_tail.physical_end_sample + 4800);
    EXPECT(g_physical_tail.fixed_decode_calls == 4);
    // Same physical N=4 and stale configured N=6, but destroy the CRC tail while
    // retaining a substantive final OFDM member. The legacy fallback still reaches
    // 6 by slicing two silence blocks; that callback may drive ARQ accounting but
    // must be explicitly untrusted by the selector.
    EXPECT(g_stale_fallback.callbacks == 1);
    EXPECT(g_stale_fallback.group_size == 6);
    EXPECT(g_stale_fallback.frame_mask == 0x07);
    EXPECT(g_stale_fallback.frames_recovered == 3);
    EXPECT(!g_stale_fallback.all_ok);
    EXPECT(!g_stale_fallback.geometry_proven);
    EXPECT(g_stale_fallback.samples_at_callback >
           g_stale_fallback.physical_end_sample + 4800);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_burst_stale_geometry: PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_burst_stale_geometry: %d FAILURE(S)\n", g_failures);
    return 1;
}
