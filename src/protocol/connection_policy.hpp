#pragma once

#include "frame_v2.hpp"
#include "waveform_selection.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cmath>

namespace ultra {
namespace protocol {
namespace connection_policy {

inline constexpr uint32_t kOFDMSampleRate = 48000;
inline constexpr uint32_t kWideOFDMFFTSamples = 1024;
inline constexpr uint32_t kWideOFDMLongCPSamples = 128;
inline constexpr uint32_t kWideOFDMSymbolSamples = kWideOFDMFFTSamples + kWideOFDMLongCPSamples;
inline constexpr uint32_t kWideOFDMCarriers = 59;
inline constexpr uint32_t kWideOFDMChirpDurationMs = 500;
inline constexpr uint32_t kWideOFDMChirpGapMs = 100;
inline constexpr uint32_t kWideOFDMFullAnchorExtraSamples =
    2 * ((kOFDMSampleRate * kWideOFDMChirpDurationMs) / 1000) +
    2 * ((kOFDMSampleRate * kWideOFDMChirpGapMs) / 1000);
inline constexpr uint32_t kWideOFDMFullAnchorExtraMs =
    (kWideOFDMFullAnchorExtraSamples * 1000 + kOFDMSampleRate - 1) / kOFDMSampleRate;
inline constexpr uint32_t kWideOFDMShortReanchorDefaultMs = 100;
inline constexpr uint32_t kWideOFDMShortReanchorMinMs = 100;
inline constexpr uint32_t kWideOFDMShortReanchorMaxMs = 300;
// ITU-R F.1487 design Doppler spreads, keyed to the channel class the link is
// actually operating on. a1c9c34 had set the "Good" constant to 0.5 Hz — but 0.5 Hz
// is the *Moderate* value (itu_r_f1487::moderate, models.cpp); the real Good channel
// (itu_r_f1487::good) is 0.1 Hz. That mislabel forced QPSK R2/3 cw=8→cw=4 on Good via
// a bogus 846ms coherence cap, when a cw=8 frame (1392ms) fits the TRUE Good coherence
// (Tc≈4230ms) easily. Verified 2026-05-26: cw=8 QPSK R2/3 Good@20 = 3/3 PASS, 0 CWFAIL,
// goodput ~1355 bps (+38% vs cw=4 980). The CW cap now derives its Doppler from the
// measured fading_index (designDopplerForFadingIndex) so Good gets cw=8 while Moderate
// keeps its protective cw=4 cap.
inline constexpr float kGoodHFDesignDopplerHz = 0.1f;
inline constexpr float kModerateHFDesignDopplerHz = 0.5f;
inline constexpr float kPoorHFDesignDopplerHz = 1.0f;

// Map the measured combined fading_index (same scale as waveform_selection.hpp:
// <0.15 AWGN, <0.65 Good, <1.10 Moderate, else Poor) to the channel class's design
// Doppler, so the coherence-time CW cap reflects the channel actually in use rather
// than a fixed worst-case constant.
inline float designDopplerForFadingIndex(float fading_index) {
    if (fading_index < 0.65f) return kGoodHFDesignDopplerHz;
    if (fading_index < 1.10f) return kModerateHFDesignDopplerHz;
    return kPoorHFDesignDopplerHz;
}
inline constexpr float kClarkeCoherenceNumerator = 0.423f;
inline constexpr uint32_t kNarrowOFDMSymbolSamples = 2240;
inline constexpr uint32_t kNarrowOFDMCarriers = 21;
inline constexpr uint32_t kNarrowOFDMPilotSpacing = 10;
inline constexpr uint32_t kLDPCBitsPerCodeword = 648;  // z=27 (n=648) short LDPC

// Coded bits per LDPC codeword for a given lifting size. The 802.11n base matrix has
// 24 columns, so N = z × 24: z=27 → 648 (short, default), z=81 → 1944 (long, ~3 dB more
// FEC margin for fading). Airtime scales with TOTAL coded bits (cw × N), so any airtime/
// timeout derived from codeword count MUST know z or it under-counts a z=81 frame ~3×.
inline constexpr uint32_t ldpcCodewordBits(int lifting_z) {
    return (lifting_z == 81) ? 1944u : kLDPCBitsPerCodeword;
}
inline constexpr uint32_t kFixedFrameCodewords = v2::kDefaultFixedFrameCodewords;
inline constexpr uint32_t kOFDMBurstAckBatchFrames = 4;
inline constexpr size_t kWideOFDMWindowFrames = 8;
inline constexpr size_t kHighThroughputOFDMWindowFrames = 16;
// Hard cap on the in-flight OFDM ARQ window when the interactive tone-burst ack is the
// ack mechanism: the tone-burst carries an 8-bit per-frame SACK `frame_mask` (0xFF,
// connection.cpp), so it can selectively acknowledge at most 8 in-flight frames.
// A window > 8 leaves frames 9+ outside the mask — un-ackable, hence falsely "lost" and
// resent forever. The wide/high-throughput windows above are therefore capped to this on
// the unified tone-burst path (an N-frame message streams as ≤8-frame windows). SINGLE
// source of truth for the 8 — code (configureArqForCurrentDataMode) and tests reference it.
// 2026-06-17: widened 6->8 (with the tone-burst frame_mask 6->8) so a THIN-frame burst
// (cw5: ~0.79s/frame) is no longer window-bound at 6 frames/~6.2s — it can carry 8 frames
// and fill the ~8.6s PA-duty airtime budget, reclaiming the wasted ~2.4s/burst of duty.
// (cw8 bursts stay airtime-bound at 5 frames, so R2/3 is unaffected.) WIRE-BREAKING:
// both stations must run the same build (no version field on the tone-burst payload).
inline constexpr size_t kToneBurstAckWindowCapFrames = 8;
// Burst group size for the INTERLEAVE-ON (Moderate/Poor) path's whole-group ACK and the
// partial-burst padding. The SACK frame_mask is now 8 bits (2026-06-17), so the ceiling
// is 8; this default stays 6 deliberately — the interleave-ON group size is a fade-
// diversity-vs-loss tradeoff that has NOT been re-swept at 8, and the measured Good-path
// win comes entirely from the WINDOW cap above (kToneBurstAckWindowCapFrames), not from
// this. (The Good interleave-OFF SR burst sizes itself by airtime/window — it does not use
// this constant.) 6 <= the 8-bit mask, so every group frame is still addressable. A group
// larger than the mask width leaves trailing frames un-ACKable; keep it <= 8.
// Overridable for the interleave-ON path via ULTRA_BURST_GROUP_FRAMES (clamped [2,32]).
inline constexpr size_t kBurstInterleaveGroupFrames = 6;

// Runtime burst group size. Read at the chunk/pad/config sites so TX file-chunking,
// padding, and the encoder's declared group_size all agree (the RX self-describes
// from the descriptor). Overridable via ULTRA_BURST_GROUP_FRAMES, clamped [2,32].
inline size_t burstInterleaveGroupFrames() {
    if (const char* env = std::getenv("ULTRA_BURST_GROUP_FRAMES")) {
        const int v = std::atoi(env);
        if (v >= 2) {
            return static_cast<size_t>(std::clamp(v, 2, 32));
        }
    }
    return kBurstInterleaveGroupFrames;
}

// 2026-05-30: SINGLE source of truth for the cross-frame burst-interleave profile.
// OFF by default. Rationale: SR-ARQ per-frame resend (interleave OFF) is the proven
// robustness lever on Good/AWGN, and deriving the TX encoder flag, the TX ARQ semantics,
// AND the on-wire BURST_HEADER descriptor bit all from THIS one function makes them
// impossible to disagree. They disagreed before (the encoder turned interleave OFF for
// QAM16 via a QPSK||QAM8 hardcode, but the ARQ stayed whole-group from the env default)
// -> ALPHA ignored BRAVO's per-frame masks and skipped partial-group holes (the QAM16
// offset-skip bug). Invariant: interleave OFF -> per-frame SR masks; ON -> whole-group
// ACK/NACK. Default OFF for ALL modulations/rates "for now"; re-enable for Moderate/Poor
// time-diversity later. Override: ULTRA_BURST_INTERLEAVE=1 (force ON), =0 (force OFF).
inline bool burstCrossFrameInterleaveOn(Modulation mod) {
    if (const char* env = std::getenv("ULTRA_BURST_INTERLEAVE")) {
        return env[0] == '1';  // explicit override forces ON/OFF for ALL modulations (testing)
    }
    // Default: cross-frame TIME interleave ON for DENSE coherent mods (>=16QAM, >=4 bits/symbol),
    // OFF (per-frame SR-ARQ) for QPSK/8PSK/BPSK. GUI-measured 2026-06-14: +47% on 16QAM R2/3
    // Good@20 (6/6 seeds, deint-fails ~halved). MECHANISM: a dense constellation's tight rings
    // cannot absorb a frequency-null codeword-wipe (frame << coherence time -> the null is frozen
    // within a frame); the burst spans ~1.7 coherence times, so spreading each codeword across the
    // burst's frames turns a static-null WIPE into a recoverable ~1/N NICK. Frequency interleaving
    // is already structurally maxed (each CW touches all 59 carriers) — TIME diversity is the only
    // remaining axis. Safe because 16QAM is only SELECTED on benign channels, so the whole-group-
    // ACK cost (lost per-frame SR masks) is ~0 there (no-regress measured on AWGN@30 + QPSK R3/4
    // Good@20). QPSK/8PSK stay per-frame SR-ARQ — their margin absorbs nulls and they serve the
    // lossier channels where fine-grained retransmit is the proven robustness lever.
    return ofdm_link_adaptation::isCoherentModulation(mod) && getBitsPerSymbol(mod) >= 4;
}
// ═════════════════════ Software-ALC (closed-loop TX-drive control, 2026-07-02) ═════════════════════
// BUG-QAM16-RIG-LEVEL-BUDGET: rig wire captures showed OFDM data arriving at only ~6-7 dB
// broadband SNR over the receiver's chain-noise floor with ~4-5 dB of unused TX level headroom
// — a chain-noise-dominated link the sender cannot see blindly. The RECEIVER measures, per
// decoded burst: (a) data-segment RMS over the group's kept data frames, (b) its idle
// chain-noise floor (IdleNoiseSNREstimator), (c) the burst crest factor CF = peak/RMS (clip
// signature). It derives a verdict — LOW (headroom available), CLIPPED (upstream clipping),
// OK — and feeds a 2-bit drive advisory back on the tone-burst ACK (bits [30..31]); the
// SENDER walks its per-burst peak target (tx_drive) within [configured baseline, 0.85].
//
// ULTRA_SOFTWARE_ALC=0 disables the LOOP on both ends (receiver always advises hold, sender
// ignores advisories); the receiver-side LEVEL ADVISORY log line stays active either way.
inline bool softwareAlcEnabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("ULTRA_SOFTWARE_ALC");
        return !(e && e[0] == '0');  // default ON
    }();
    return enabled;
}

// Per-burst RX level verdict (receiver-side measurement, decoder-computed).
enum class RxLevelVerdict : int {
    OK = 0,       // healthy level, no advisory
    LOW = 1,      // data rides < kAlcLowHeadroomDb over the chain-noise floor
    CLIPPED = 2,  // crest factor collapsed below kAlcClipCfDb (upstream clipping)
};

// LOW threshold: burst data RMS over the idle chain-noise floor, in dB (env
// ULTRA_ALC_LOW_DB). Default 12 dB, derived from three anchors:
//  1. MEASURED separation: the level-limited rig state reads ~6-7 dB (wire captures,
//     BUG-QAM16-RIG-LEVEL-BUDGET), a healthy sim-reference link at Good@20 reads ~20 dB
//     — 12 splits them with >=5 dB margin each side.
//  2. FIRST PRINCIPLES: with data >=12 dB above the noise floor, the floor degrades a
//     channel-noise-limited link by <= 10*log10(1+10^-1.2) ~= 0.27 dB — the chain has
//     stopped being the bottleneck, so no drive-up is warranted; below it, the arriving
//     level itself caps the dense-constellation rungs (16QAM R1/2 needs ~11-13 dB eff.).
//  3. LOOP GEOMETRY: the full ALC walk (0.5 -> 0.85 peak target) is +4.6 dB — from the
//     measured 6-7 dB it reaches ~11-12 dB, i.e. the loop converges AT this threshold
//     (or at the digital ceiling), never chasing an unreachable target.
inline float alcLowHeadroomDb() {
    static const float v = [] {
        if (const char* e = std::getenv("ULTRA_ALC_LOW_DB")) {
            const float f = static_cast<float>(std::atof(e));
            if (f > 0.0f && f < 40.0f) return f;
        }
        return 12.0f;
    }();
    return v;
}

// CLIPPED threshold: burst data crest factor (peak/RMS) in dB below which upstream
// clipping is assumed (env ULTRA_ALC_CLIP_CF_DB). Default 6.5 dB:
//  - healthy OFDM data arrives at CF ~9-14 dB (near-Gaussian sum of ~59 carriers:
//    max|x| over an N-sample burst ~ sqrt(2 ln N)*sigma ~= 12-13 dB at N~3e4; wire
//    captures measured 9-14);
//  - a noise-dominated segment (the LOW case) is itself Gaussian -> CF ~10-12 dB, so
//    a buried burst can NOT false-trigger CLIPPED;
//  - hard upstream clipping collapses CF toward 0-6 dB (the 2026-06-15 IONOS
//    square-wave disaster measured CF=1.01 ~= 0.1 dB).
//  6.5 dB sits >=2.5 dB below the healthy floor and >=3 dB above the collapse signature.
inline float alcClipCrestFactorDb() {
    static const float v = [] {
        if (const char* e = std::getenv("ULTRA_ALC_CLIP_CF_DB")) {
            const float f = static_cast<float>(std::atof(e));
            if (f > 0.0f && f < 12.0f) return f;
        }
        return 6.5f;
    }();
    return v;
}

// Advisory hysteresis: consecutive LOW bursts required before advising "up". A single
// LOW can be a deep-fade artifact (the burst-average is fade-averaged over ~1-2 Tc,
// but a whole-burst trough remains possible); two consecutive multi-second bursts
// decorrelate across Tc. CLIPPED advises "down" IMMEDIATELY (clipping destroys frames
// now; asymmetric fast-attack/slow-release is classic ALC).
inline constexpr int kAlcLowStreakForUp = 2;

// Sender step sizes: +0.5 dB per advised-up ACK (slow release), -2 dB per advised-down
// (fast attack). One adjustment per ACKed group (repeat-ACK detections are deduped).
inline constexpr float kAlcUpStepFactor = 1.0593f;    // 10^(0.5/20)
inline constexpr float kAlcDownStepFactor = 0.7943f;  // 10^(-2/20)

inline constexpr uint32_t kResponderHandshakeFailSafeMs = 2200;
inline constexpr uint32_t kMCDPSKDualChirpPreambleMs = 1200;
inline constexpr uint32_t kMCDPSKInterFrameGuardMs = 100;
inline constexpr uint32_t kMCDPSKRobustLowAckTimeoutFloorMs = 36000;
inline constexpr uint32_t kCarrierSenseSackCoalesceMs = 30;
inline constexpr int kCarrierSenseAckRepeatCount = 1;
inline constexpr uint32_t kWideOFDMAckTimeoutFloorMs = 8000;

struct OFDMFrameTiming {
    uint32_t data_symbols = 0;
    uint32_t ack_symbols = 0;
    uint32_t data_ms = 0;
    uint32_t ack_ms = 0;
};

struct MCDPSKFrameTiming {
    uint32_t overhead_symbols = 0;
    uint32_t data_only_symbols = 0;
    uint32_t data_symbols = 0;
    uint32_t ack_symbols = 0;
    uint32_t overhead_ms = 0;
    uint32_t data_only_ms = 0;
    uint32_t data_ms = 0;
    uint32_t ack_ms = 0;
};

inline const char* fadingLabel(float fading) {
    if (fading < 0.15f) return "AWGN";
    if (fading < 0.65f) return "Good";
    if (fading < 1.10f) return "Moderate";
    return "Poor";
}

enum class ChannelClassification {
    AWGN,
    GOOD,
    MODERATE,
    POOR,
};

inline ChannelClassification classifyChannel(float fading) {
    if (fading < 0.15f) return ChannelClassification::AWGN;
    if (fading < 0.65f) return ChannelClassification::GOOD;
    if (fading < 1.10f) return ChannelClassification::MODERATE;
    return ChannelClassification::POOR;
}

// Good/Moderate discriminator constants (docs/CHANNEL_DISCRIMINATOR_DESIGN_2026_06_15.md).
// fading_index (fade DEPTH) cannot tell Good from Moderate; the Doppler coherence score
// (per-frame pilot |H|^2 autocorrelation @~1.5 s inter-frame cadence) can. It refines ONLY
// the Good<->Moderate boundary when valid; AWGN and Poor stay driven by fade depth.
//
// TWO-THRESHOLD DEAD ZONE (multi-seed GUI calibration, 2026-06-16). coherenceScore is the
// CUMULATIVE MEAN of the per-frame lag-1 autocorrelation (a single 40-snapshot read has ~0.16
// SE — Moderate single-reads scatter to ~0.45 — too noisy; the transfer mean separates cleanly).
// 12-seed×2-channel GUI sweep (cumulative-mean): confident-Good (>= kCoherenceGoodThreshold)
// = 11/12 Good (min 0.50), 0/12 Moderate; confident-Moderate (<= kCoherenceModerateThreshold)
// = 11/12 Moderate (max 0.30), 0/12 Good. The in-between dead zone DEFERS to the raw
// fading_index (conservative status quo) and absorbed the 2 marginal seeds (Good 0.421,
// Moderate 0.359). ZERO dangerous misreads: no Moderate reached the Good threshold (max 0.359
// < 0.45, margin 0.09), so "Moderate read as Good -> over-high rate" cannot happen on this data.
inline constexpr float kCoherenceGoodThreshold = 0.45f;      // confident Good (legacy lag-1; SIM-scale)
inline constexpr float kCoherenceModerateThreshold = 0.30f;  // confident Moderate (legacy lag-1; SIM-scale)
inline constexpr float kRepresentativeGoodFadingIndex = 0.40f;      // mid-Good (< 0.65)
inline constexpr float kRepresentativeModerateFadingIndex = 0.85f;  // mid-Moderate (0.65-1.10)

// RADIO-AGNOSTIC coherence-AREA thresholds (2026-06-20, docs/SCALE_INVARIANT_COHERENCE_DISC_2026_06_20.md).
// The legacy lag-1 score above is SIM-calibrated and platform-broken (needs ~0.045 on the IONOS rig,
// ~0.30 on sim — a flat re-base would break sim). The coherence-AREA (cumulative-mean of the sliding-
// window Sum_{lag=1..5} normalized |H|^2 autocov, DopplerCoherenceEstimator::coherenceArea) separates
// Good from Moderate on a SINGLE dimensionless threshold across BOTH audio paths. Cross-platform
// validation (sim + IONOS rig, 7 transfers, faithful C++-algorithm replication): Good {rig +0.09,
// +0.11; sim +0.66} vs Moderate {rig −0.10, −0.10, −0.18; sim −0.12} -> worst-Good +0.091 vs best-Mod
// −0.100, gap 0.19, midpoint ~0. (The sliding-window LOCAL demean compresses Good toward 0 but rejects
// a Moderate transfer's transient Good-like patch — "Mod-b" reads −0.10 here vs +0.08 single-window.)
// Hysteresis (enter > exit): enter is above max-Mod by 0.15 (a Moderate misread needs a +0.15 jump =
// safe), and Good clearing enter by only ~0.04 fails SAFE (a benign Good->uncertain just keeps the
// conservative no-skip). The consumer ALSO has a REACTIVE override (escalations/resends/backlog/cold-
// start) so a momentarily-wrong label cannot strand a frame.
inline constexpr float kCoherenceAreaEnterGood = 0.05f;  // climb into confident-Good (clear of max Mod −0.10 by 0.15)
inline constexpr float kCoherenceAreaExitGood  = 0.00f;  // drop out of confident-Good (hysteresis floor; Mod sits <0)

// Returns a fading_index reflecting the coherence verdict on the Good<->Moderate axis when
// the coherence is valid AND confident; otherwise the raw fading_index (so CONNECT-time —
// before any OFDM data has pooled — and the uncertain dead zone are unchanged = status quo).
// AWGN/Poor are never overridden.
inline float coherenceAdjustedFadingIndex(float fading_index, float coherence_score,
                                          bool coherence_valid) {
    if (!coherence_valid) return fading_index;
    const ChannelClassification base = classifyChannel(fading_index);
    if (base != ChannelClassification::GOOD && base != ChannelClassification::MODERATE) {
        return fading_index;
    }
    if (coherence_score >= kCoherenceGoodThreshold) return kRepresentativeGoodFadingIndex;
    if (coherence_score <= kCoherenceModerateThreshold) return kRepresentativeModerateFadingIndex;
    return fading_index;  // uncertain dead zone -> defer to the blind metric (conservative)
}

struct LadderRung {
    LadderRungId id = LadderRungId::UNKNOWN;
    const char* name = "Unknown";
    WaveformMode waveform = WaveformMode::MC_DPSK;
    Modulation modulation = Modulation::DQPSK;
    CodeRate code_rate = CodeRate::R1_4;
    int num_carriers = 0;
    int samples_per_symbol = 0;
    int cw_count = v2::kDefaultFixedFrameCodewords;
};

inline LadderRung ladderRungForId(LadderRungId id) {
    switch (id) {
        case LadderRungId::ROBUST_LOW:
            // #72/#71 (2026-06-28): MC-DPSK standardized on ONE baud (sps=1024).
            // The CONTROL waveform is fixed at sps=1024 (the handshake profile); a
            // DATA rung at a DIFFERENT baud (the old 2048/512) re-cut the shared
            // waveform and shipped CONNECT_ACK at a baud the peer couldn't sync to
            // -> handshake strand. With every rung at 1024, control==data baud by
            // construction and the only thing that varies is the constellation
            // (DBPSK/DQPSK) + rate — handled by the control-profile path, exactly
            // like OFDM. The 512 "fast" gear's throughput is recovered at 1024 via
            // DQPSK (ROBUST) and code rate; its only real edge was fast-Doppler.
            return {id, "Robust-Low", WaveformMode::MC_DPSK,
                    Modulation::DBPSK, CodeRate::R1_4, 8, 1024, 3};
        case LadderRungId::ROBUST_MID:
            return {id, "Robust-Mid", WaveformMode::MC_DPSK,
                    Modulation::DBPSK, CodeRate::R1_4, 8, 1024, 3};
        case LadderRungId::ROBUST:
            return {id, "Robust", WaveformMode::MC_DPSK,
                    Modulation::DQPSK, CodeRate::R1_4, 8, 1024,
                    v2::kDefaultFixedFrameCodewords};
        case LadderRungId::STANDARD:
            // #72/#71: standardized to sps=1024 (see ROBUST_LOW). Same
            // constellation/rate as ROBUST now (DQPSK 1024 R1/4); the throughput
            // the old 512 baud gave is reachable here via DQPSK + a higher code
            // rate, without the variable-baud handshake hazard.
            return {id, "Standard", WaveformMode::MC_DPSK,
                    Modulation::DQPSK, CodeRate::R1_4, 8, 1024,
                    v2::kDefaultFixedFrameCodewords};
        case LadderRungId::OFDM_CHIRP:
            // Coherent-only wideband OFDM (thread A, 2026-05-31). Nominal mod is
            // QPSK; the real data mod still comes from recommendDataMode(). NARROW
            // below stays DQPSK — narrowband/low-SNR is a robust differential regime.
            return {id, "OFDM_CHIRP", WaveformMode::OFDM_CHIRP,
                    Modulation::QPSK, CodeRate::R1_4, 0, 0,
                    v2::kDefaultFixedFrameCodewords};
        case LadderRungId::OFDM_NARROW:
            return {id, "OFDM_NARROW", WaveformMode::OFDM_NARROW,
                    Modulation::DQPSK, CodeRate::R1_4, 0, 0,
                    v2::kDefaultFixedFrameCodewords};
        case LadderRungId::UNKNOWN:
        default:
            return {};
    }
}

inline LadderRung rungForMCDPSKConfig(Modulation modulation,
                                      int num_carriers,
                                      int samples_per_symbol,
                                      int cw_count) {
    if (num_carriers != 8) {
        return {};
    }
    if (modulation == Modulation::DBPSK && samples_per_symbol >= 2048 && cw_count == 3) {
        return ladderRungForId(LadderRungId::ROBUST_LOW);
    }
    if (modulation == Modulation::DBPSK && samples_per_symbol == 1024 && cw_count == 3) {
        return ladderRungForId(LadderRungId::ROBUST_MID);
    }
    if (modulation == Modulation::DQPSK && samples_per_symbol == 1024 &&
        cw_count == v2::kDefaultFixedFrameCodewords) {
        return ladderRungForId(LadderRungId::ROBUST);
    }
    if (modulation == Modulation::DQPSK && samples_per_symbol == 512 &&
        cw_count == v2::kDefaultFixedFrameCodewords) {
        return ladderRungForId(LadderRungId::STANDARD);
    }
    return {};
}

// #58 connect-time SNR basis correction (2026-07-01, fable_analysis/09 §4).
// The OFDM entry floors and coherent-ladder anchors are DIAL-calibrated (forced-rung
// sim sweeps at --snr-db = the AWGN-equivalent dial), but the connect-time reading
// compared against them (#74 ratiometric MC-DPSK training SNR) is FADE-EFFECTIVE and
// INSTANT: on a fading channel it averages ~2 dB below the dial (Jensen penalty over
// the ~170 ms << Tc training window) and swings with the fade phase run-to-run.
// Comparing effective-instant readings against dial thresholds double-penalizes — a
// single fade dip at CONNECT dropped a dial-20 rig channel (sync SNR 21.8) to a 12.4
// reading -> below the Moderate entry floor 14 -> MC-DPSK DBPSK (~94 bps nominal, 0
// bytes delivered) on a channel that carries ~1.5 kbps at QPSK R2/3. Align the bases:
// on any FADING channel the SELECTION comparison adds back the measured penalty. The
// reported/wire SNR stays raw (honest measurement); MC-DPSK-internal rung floors were
// themselves calibrated against this same ratiometric reading on the rig (#71
// floor-finding, already effective-basis) — the +2 there equals lowering the DQPSK
// floor by 2, which the #71 rig data supports (DQPSK 3/3 across effective 2.4-9 dB)
// and whose comment anticipated pending #58. Cost asymmetry backs the sign: wrongly
// entering OFDM at true-effective ~10 still delivers ~450 bps (Good@10 R1/2 5/5,
// RATE_LADDER_ANCHORS), while wrongly falling to MC-DPSK costs 15-40x.
// ULTRA_CONNECT_SNR_FADE_BASIS=0 disables; a value in (0,6] overrides the 2.0 default.
inline float connectSnrFadeBasisDb() {
    static const float v = [] {
        if (const char* e = std::getenv("ULTRA_CONNECT_SNR_FADE_BASIS")) {
            const float parsed = std::strtof(e, nullptr);
            if (parsed <= 0.0f) return 0.0f;
            if (parsed <= 6.0f) return parsed;
        }
        return 2.0f;
    }();
    return v;
}
inline float connectSelectionSnrDb(float measured_snr_db, float fading_index) {
    if (fading_index >= kFadingAwgnMax) {
        return measured_snr_db + connectSnrFadeBasisDb();
    }
    return measured_snr_db;  // AWGN: reading and thresholds share the basis already
}

inline LadderRung selectLadderRung(float snr_db, ChannelClassification channel) {
    // DIAGNOSTIC force (ULTRA_FORCE_MCDPSK_RUNG=LOW|MID|ROBUST|STANDARD): pin the
    // MC-DPSK rung, bypassing the SNR thresholds, to MEASURE each rung's real floor
    // on the faithful gate (#71). The DQPSK rungs ROBUST (1024) / STANDARD (512) are
    // otherwise UNREACHABLE (robust_floor > ofdm_floor; STANDARD never returned), so
    // their floors are unknown. No-op unless set; not a production path.
    if (const char* e = std::getenv("ULTRA_FORCE_MCDPSK_RUNG")) {
        const std::string s(e);
        if (s == "LOW")      return ladderRungForId(LadderRungId::ROBUST_LOW);
        if (s == "MID")      return ladderRungForId(LadderRungId::ROBUST_MID);
        if (s == "ROBUST")   return ladderRungForId(LadderRungId::ROBUST);
        if (s == "STANDARD") return ladderRungForId(LadderRungId::STANDARD);
    }
    float ofdm_floor = 10.0f;
    float robust_mid_floor = 5.0f;  // DBPSK R1/4 floor (the robust MC-DPSK fallback)

    // OFDM entry floors are the SINGLE source in waveform_selection.hpp
    // (kOFDMEntryFloor*Db) so this enum-keyed path and the fading-index-keyed
    // recommendWaveformAndRate() can't drift. robust_mid_floor is the MC-DPSK
    // DBPSK floor, local to this ladder.
    switch (channel) {
        case ChannelClassification::AWGN:
            ofdm_floor = kOFDMEntryFloorAwgnDb;
            robust_mid_floor = 5.0f;
            break;
        case ChannelClassification::GOOD:
            ofdm_floor = kOFDMEntryFloorGoodDb;
            robust_mid_floor = 6.0f;
            break;
        case ChannelClassification::MODERATE:
            ofdm_floor = kOFDMEntryFloorModerateDb;
            robust_mid_floor = 7.0f;
            break;
        case ChannelClassification::POOR:
            ofdm_floor = kOFDMEntryFloorPoorDb;
            robust_mid_floor = 9.0f;
            break;
    }

    // #71: the DQPSK rung (ROBUST = DQPSK/1024/R1/4, ~2x DBPSK throughput) is now
    // REACHABLE in the MC-DPSK sub-band. The old robust_floor (13-17) sat ABOVE
    // ofdm_floor (10-14), so the DQPSK interval [robust_floor, ofdm_floor) was EMPTY
    // and MC-DPSK was pinned to DBPSK R1/4. Partition the sub-band by per-rung
    // GEOMETRY: DQPSK R1/4 needs ~+2.5 dB over DBPSK (the differential BPSK->QPSK gap).
    // MEASURED on BENIGN channels: forced DQPSK decodes CRC-clean at good@8/10/12 (0
    // cw_fail), matching the +2.3 dB prediction; the +2.5 dB floor sits with hysteresis
    // above that, clamped just below ofdm_floor.
    //
    // FAST-FADING (Moderate/Poor): the DIFFERENTIAL demod compounds the Doppler phase
    // error symbol-to-symbol, so DQPSK costs MORE than +2.5 dB there and its floor is
    // UNMEASURED. Keep DQPSK unreachable on fading channels (DBPSK only) until a fading
    // floor-probe sets a principled threshold — DBPSK is the robust differential pick
    // for fast fading anyway. DQPSK is already wired end-to-end; control stays fixed
    // DBPSK/1024 (#72), so this is a pure selector change.
    float robust_dqpsk_floor;
    switch (channel) {
        case ChannelClassification::AWGN:
        case ChannelClassification::GOOD:
            // #71: with the DQPSK window spiral fixed (round-trip-safe window=3, above),
            // rig A/B @ MPG@9 showed DQPSK-window=3 matches DBPSK reliability (3/3 vs 3/3)
            // at ~2x goodput across effective connect-SNR ~2.4-9 dB. The old +2.5 dB
            // differential BPSK->QPSK geometry margin (floor 8.5) is an AWGN bound and
            // over-penalizes DQPSK on FADING, where ARQ means only the good-fade frames
            // must decode. Lower to a validated-conservative +1.0 dB over the DBPSK floor
            // (Good 7.0, AWGN 6.0), clamped below the OFDM floor, keeping a thin DBPSK-mid
            // buffer above the deep-fade fallback. NOTE: going lower (toward the DBPSK
            // floor) is supported by the data but needs MPG@8/7 floor-finding + a
            // fade-AVERAGED connect SNR (task #58) — the single-snapshot reading is noisy,
            // so dial-9 auto-selection is not yet deterministic.
            robust_dqpsk_floor = std::min(robust_mid_floor + 1.0f, ofdm_floor - 0.5f);
            break;
        case ChannelClassification::MODERATE:
            robust_dqpsk_floor = 15.0f;  // UNCHANGED (old robust_floor): > ofdm 14 ->
                                         // DBPSK only; fading DQPSK floor unmeasured.
            break;
        case ChannelClassification::POOR:
        default:
            robust_dqpsk_floor = 17.0f;  // UNCHANGED (old robust_floor).
            break;
    }

    if (snr_db >= ofdm_floor) {
        return ladderRungForId(LadderRungId::OFDM_CHIRP);
    }
    if (snr_db >= robust_dqpsk_floor) {
        return ladderRungForId(LadderRungId::ROBUST);  // DQPSK R1/4 — ~2x DBPSK
    }
    if (snr_db >= robust_mid_floor) {
        return ladderRungForId(LadderRungId::ROBUST_MID);  // DBPSK R1/4
    }
    return ladderRungForId(LadderRungId::ROBUST_LOW);  // DBPSK R1/4 (deepest fallback)
}

inline LadderRung selectLadderRung(float snr_db, float fading_index) {
    return selectLadderRung(snr_db, classifyChannel(fading_index));
}

inline uint8_t modeToCapabilityBit(WaveformMode mode) {
    switch (mode) {
        case WaveformMode::OFDM_CHIRP:  return ModeCapabilities::OFDM_CHIRP;
        case WaveformMode::OFDM_NARROW: return ModeCapabilities::OFDM_NARROW;
        case WaveformMode::OTFS_EQ:     return ModeCapabilities::OTFS_EQ;
        case WaveformMode::OTFS_RAW:    return ModeCapabilities::OTFS_RAW;
        case WaveformMode::MFSK:        return ModeCapabilities::MFSK;
        case WaveformMode::MC_DPSK:     return ModeCapabilities::MC_DPSK;
        default: return 0;
    }
}

inline bool isNearAwgnOFDM(float fading_index, float snr_db) {
    return fading_index < 0.15f && snr_db >= 25.0f;
}

inline bool isHighThroughputOFDM(float fading_index, float snr_db) {
    return fading_index < 0.65f && snr_db >= 25.0f;
}

inline bool isHighThroughputOFDMMode(Modulation mod, CodeRate rate) {
    // High-throughput predicate gates window=16 selective-repeat
    // (vs window=8 default). DQPSK at R1/2+ uses bigger window because
    // fading correlation across an 8-frame burst is tolerable; D8PSK
    // gets the same treatment because the 2026-05-04 D8PSK gate only
    // fires when the channel is good enough to support it (in-band SNR>=20
    // fading<0.65 minimum), which is the same precondition the larger
    // window assumes.
    if (mod == Modulation::DQPSK || mod == Modulation::D8PSK) {
        const auto* descriptor = ofdmCodeRateDescriptor(rate);
        const auto* floor = ofdmCodeRateDescriptor(CodeRate::R1_2);
        return descriptor != nullptr && floor != nullptr &&
               descriptor->code_rate >= floor->code_rate;
    }
    return false;
}

inline bool isSpeculativeHighRateOFDM(Modulation mod, CodeRate rate) {
    // R2/3 and R3/4 are speculative (window=16 only on near-AWGN);
    // R1/2 is non-speculative (window=16 always when fading channel
    // is good). Both DQPSK and D8PSK follow the same logic.
    const auto* descriptor = ofdmCodeRateDescriptor(rate);
    const auto* floor = ofdmCodeRateDescriptor(CodeRate::R1_2);
    const bool risky_rate = descriptor != nullptr && floor != nullptr &&
                            descriptor->code_rate > floor->code_rate;
    return risky_rate && (mod == Modulation::DQPSK || mod == Modulation::D8PSK);
}

inline bool isBurstInterleavedOFDMMode(Modulation mod, CodeRate rate) {
    // Coherent 8PSK/16-QAM need the per-frame chirp/LTS channel reference while
    // the 2026-05 burst-accumulation path is still being reworked. Round-5 Good
    // fading A/B showed burst groups 2/4/8 lose to independent coherent frames
    // because continuation timing failures dominate the diversity gain.
    return isSpeculativeHighRateOFDM(mod, rate);
}

inline size_t ofdmWindowSize(Modulation mod, CodeRate rate, bool near_awgn_ofdm) {
    if (!isHighThroughputOFDMMode(mod, rate)) {
        return kWideOFDMWindowFrames;
    }

    if (isSpeculativeHighRateOFDM(mod, rate) && !near_awgn_ofdm) {
        return kWideOFDMWindowFrames;
    }

    return kHighThroughputOFDMWindowFrames;
}

inline size_t ofdmWindowSize(Modulation mod, CodeRate rate) {
    return ofdmWindowSize(mod, rate, true);
}

inline size_t ofdmWindowSizeForChannel(Modulation mod,
                                       CodeRate rate,
                                       float fading_index,
                                       float snr_db) {
    return ofdmWindowSize(mod, rate, isNearAwgnOFDM(fading_index, snr_db));
}

inline bool shouldPadHighRateFadingBurst(Modulation mod,
                                         CodeRate rate,
                                         bool near_awgn_ofdm,
                                         size_t burst_frames) {
    if (!isSpeculativeHighRateOFDM(mod, rate) || near_awgn_ofdm) {
        return false;
    }
    if (burst_frames <= 1) {
        return false;
    }

    return (burst_frames % burstInterleaveGroupFrames()) != 0;
}

inline bool shouldPadBurstInterleaveGroup(size_t burst_frames) {
    if (burst_frames <= 1) {
        return false;
    }
    return (burst_frames % burstInterleaveGroupFrames()) != 0;
}

inline uint32_t ofdmAckBatchSize(bool near_awgn_ofdm) {
    (void)near_awgn_ofdm;
    return 0;
}

inline uint32_t bitsPerOFDMSymbol(uint32_t carriers,
                                  bool include_pilots,
                                  int pilot_spacing,
                                  Modulation mod) {
    return static_cast<uint32_t>(ofdm_link_adaptation::bitsPerOFDMSymbol(
        static_cast<int>(carriers), include_pilots, pilot_spacing, mod));
}

inline uint32_t wideOFDMSymbolsForCodewords(Modulation mod, CodeRate rate, int codewords,
                                            int lifting_z = 27) {
    const int pilot_spacing = ofdm_link_adaptation::recommendedPilotSpacing(mod, rate);
    const uint32_t bits_per_symbol = bitsPerOFDMSymbol(
        kWideOFDMCarriers, true, pilot_spacing, mod);
    // z-AWARE: coded bits per codeword is N = z×24 (648 at z=27, 1944 at z=81). A z=81
    // frame carries ~3× the coded bits per codeword, so it occupies ~3× the data symbols.
    const uint32_t frame_bits =
        static_cast<uint32_t>(codewords) * ldpcCodewordBits(lifting_z);
    const uint32_t data_symbols = (frame_bits + bits_per_symbol - 1) / bits_per_symbol;
    return 2 + data_symbols;
}

inline uint32_t wideOFDMShortReanchorChirpDurationMs() {
    static const uint32_t duration_ms = [] {
        const char* value = std::getenv("ULTRA_SHORT_REANCHOR_CHIRP_MS");
        if (!value || value[0] == '\0') {
            return kWideOFDMShortReanchorDefaultMs;
        }

        char* end = nullptr;
        const float parsed = std::strtof(value, &end);
        if (end == value || !std::isfinite(parsed)) {
            return kWideOFDMShortReanchorDefaultMs;
        }
        const uint32_t rounded = static_cast<uint32_t>(parsed + 0.5f);
        return std::clamp<uint32_t>(rounded,
                                    kWideOFDMShortReanchorMinMs,
                                    kWideOFDMShortReanchorMaxMs);
    }();
    return duration_ms;
}

inline bool shouldUseWideOFDMShortReanchor(WaveformMode waveform,
                                           Modulation modulation,
                                           float fading_index) {
    if (waveform != WaveformMode::OFDM_CHIRP ||
        !ofdm_link_adaptation::isCoherentModulation(modulation) ||
        !std::isfinite(fading_index)) {
        return false;
    }

    return fading_index >= kFadingAwgnMax;
}

// Channel gate for the Phase 2a warm SHORT-DUAL descriptor anchor
// (ULTRA_SHORT_ANCHOR_DESCRIPTOR_MS). The short anchor reclaims ~400-600 ms/burst of chirp
// airtime but trades ~1.5-3 dB of matched-filter margin vs the full 500 ms dual. GUI-measured
// (2026-06-13, gui_qso_scenario): on fading that margin loss produces a fat tail of
// descriptor-miss / fade-alignment storms whose worst-case seed RELOCATES with chirp duration
// but never disappears (250 ms cratered Moderate seed 2 @88 retx; 350 ms fixed it but cratered
// seed 43 @44 retx/96 CW-fail and dragged Good seed 2 to -18%). It is a clean win ONLY on benign
// Good/AWGN (250 ms = +7.2%, 3/3 seeds, 0 CW-fail, no crater).
//
// Gate on BENIGN-channel operating points (robust SENDER-SIDE proxies the ladder only sustains
// at high SNR + shallow fading, unlike the raw fading_index which suffers the Good/Moderate
// classifier blindness — the measured fading distributions overlap):
//   - QPSK R3/4: the ladder's top rung (entry floors + adaptive hysteresis), OR
//   - dense coherent mods (>=16QAM, 2026-06-14): the ladder only SELECTS these on benign Good
//     channels, so 16QAM at any rate IS a benign operating point. The reclaim is proportionally
//     BIGGER here — a denser payload packs the burst's data into fewer symbols, so the fixed
//     descriptor chirp is a larger fraction of the burst.
// When the channel degrades the ladder drops below R3/4 / off 16QAM and the short anchor
// auto-reverts to the full dual chirp, mid-session.
inline bool shouldUseWarmShortAnchorDescriptor(WaveformMode waveform,
                                               Modulation modulation,
                                               CodeRate rate) {
    if (waveform != WaveformMode::OFDM_CHIRP ||
        !ofdm_link_adaptation::isCoherentModulation(modulation)) {
        return false;
    }
    return rate == CodeRate::R3_4 || getBitsPerSymbol(modulation) >= 4;
}

inline OFDMFrameTiming wideOFDMFrameTiming(Modulation mod,
                                           CodeRate rate,
                                           int cw_count = v2::kDefaultFixedFrameCodewords,
                                           int data_lifting_z = 27) {
    cw_count = v2::sanitizeFixedFrameCodewords(cw_count);
    constexpr float symbol_ms =
        (1000.0f * static_cast<float>(kWideOFDMSymbolSamples)) /
        static_cast<float>(kOFDMSampleRate);

    OFDMFrameTiming timing;
    // DATA frames carry the negotiated lifting z (27 short / 81 long); the ACK/control
    // frame is ALWAYS a 1-CW short (z=27) frame, so its airtime is z-independent.
    timing.data_symbols = wideOFDMSymbolsForCodewords(mod, rate, cw_count, data_lifting_z);
    timing.ack_symbols = wideOFDMSymbolsForCodewords(mod, rate, 1, /*lifting_z=*/27);
    timing.data_ms = static_cast<uint32_t>(timing.data_symbols * symbol_ms + 0.5f);
    timing.ack_ms = static_cast<uint32_t>(timing.ack_symbols * symbol_ms + 0.5f);
    return timing;
}

inline uint32_t wideOFDMBurstAirtimeMs(Modulation mod,
                                       CodeRate rate,
                                       size_t frame_count,
                                       int cw_count = v2::kDefaultFixedFrameCodewords,
                                       uint32_t continuation_reanchor_ms = 0,
                                       int data_lifting_z = 27) {
    if (frame_count == 0) {
        return 0;
    }

    const OFDMFrameTiming timing =
        wideOFDMFrameTiming(mod, rate, cw_count, data_lifting_z);
    uint64_t burst_ms = static_cast<uint64_t>(frame_count) * timing.data_ms;
    if (frame_count > 1) {
        // StreamingEncoder::encodeBurstLight() emits a full chirp anchor on the
        // first OFDM burst frame, then either light LTS-only preambles or
        // adaptive short chirp+LTS reanchors for continuations.
        burst_ms += kWideOFDMFullAnchorExtraMs;
        burst_ms += static_cast<uint64_t>(frame_count - 1) *
                    static_cast<uint64_t>(continuation_reanchor_ms);
    }
    return static_cast<uint32_t>(std::min<uint64_t>(burst_ms, 0xFFFFFFFFull));
}

inline uint32_t wideOFDMSackDelayMs(Modulation mod,
                                    CodeRate rate,
                                    size_t window_size,
                                    int cw_count = v2::kDefaultFixedFrameCodewords,
                                    uint32_t continuation_reanchor_ms = 0) {
    const uint32_t burst_ms = wideOFDMBurstAirtimeMs(
        mod, rate, std::max<size_t>(1, window_size), cw_count,
        continuation_reanchor_ms);
    return burst_ms + kCarrierSenseSackCoalesceMs;
}

inline uint32_t wideOFDMSackTailDelayMs() {
    return kCarrierSenseSackCoalesceMs;
}

inline uint32_t wideOFDMSlidingSackDelayMs(
        Modulation mod,
        CodeRate rate,
        int cw_count = v2::kDefaultFixedFrameCodewords) {
    const OFDMFrameTiming timing = wideOFDMFrameTiming(mod, rate, cw_count);
    // This timer is re-armed on every decoded DATA frame, so it is a quiet
    // interval after the observed burst tail, not a full-window hold. It covers
    // one selected-rate DATA frame cadence plus one selected-rate ACK/control
    // frame and the carrier-sense coalescing guard.
    const uint64_t quiet_interval_ms =
        static_cast<uint64_t>(timing.data_ms) +
        static_cast<uint64_t>(timing.ack_ms) +
        static_cast<uint64_t>(kCarrierSenseSackCoalesceMs);
    return static_cast<uint32_t>(std::min<uint64_t>(quiet_interval_ms, 0xFFFFFFFFull));
}

inline uint32_t coherenceTimeMsForDoppler(float doppler_hz) {
    if (!std::isfinite(doppler_hz) || doppler_hz <= 0.0f) {
        return UINT32_MAX;
    }
    // Clarke/Jakes 50%-correlation coherence-time approximation: Tc ~= 0.423/fD.
    const float tc_ms = (kClarkeCoherenceNumerator * 1000.0f) / doppler_hz;
    return static_cast<uint32_t>(std::max(1.0f, tc_ms) + 0.5f);
}

// Recommend fixed-frame CW count for a given OFDM data rate + waveform.
// Inputs are deterministic and shared by both peers (rate is negotiated;
// waveform is negotiated too) so both peers compute the same CW count
// without risk of one reading a different SNR/fading and picking a
// different CW geometry.
//
// Wide OFDM (OFDM_CHIRP):
//   R1/2, R2/3, R3/4 → 8 (hardware A/B Mac↔Pi5 5KB DQPSK R1/2 SNR=15
//   good fading: CW=4 → 1077 bps 2 retx; CW=8 → 1615 bps 0 retx, +50%.
//   D8PSK R3/4 SNR=27 AWGN ceiling: CW=8 → 3127 bps.)
//   R1/4 stays at default 4 (low-SNR robustness, no measured win wider).
//
// Narrow OFDM (OFDM_NARROW): always 4. Narrow R1/2 frames are already
// ~6 s at CW=8 with the 21-carrier geometry; window=3 burst would be
// ~18 s — longer than typical narrow good-fading coherence (~10 s).
// 3-seed sim A/B at SNR=8 good fading R1/2 (2 KB): CW=8 1/3 FAIL with
// 240 s timeout, 2/3 PASS at 124-172 bps; CW=4 baseline 3/3 PASS at
// 116-149 bps. The bigger frame becomes a single fade event's victim.
inline int recommendCWCount(CodeRate rate, WaveformMode waveform) {
    if (waveform == WaveformMode::OFDM_NARROW) {
        return v2::kDefaultFixedFrameCodewords;  // 4 — fade-coherence cap
    }
    if (const auto* descriptor = ofdmCodeRateDescriptor(rate)) {
        return descriptor->wide_cw_count;
    }
    return v2::kDefaultFixedFrameCodewords;
}

// Modulation-aware data-frame CW policy for waveforms whose fade exposure is
// dominated by frame duration. OFDM keeps the fixed-frame policy above.
// Robust-Low MC-DPSK DBPSK uses variable LDPC frames; with R1/4, 3 CW carries
// a 37-byte ARQ payload, i.e. 32 file bytes after FILE_DATA overhead. 1 CW
// cannot carry file data and 2 CW is too slow to be operationally useful.
inline int recommendCWCount(Modulation mod, CodeRate rate, WaveformMode waveform) {
    if (waveform == WaveformMode::MC_DPSK && mod == Modulation::DBPSK) {
        (void)rate;
        return 3;
    }
    return recommendCWCount(rate, waveform);
}

inline int recommendCWCountForChannel(Modulation mod,
                                      CodeRate rate,
                                      WaveformMode waveform,
                                      float fading_index,
                                      float snr_db,
                                      float doppler_hz = -1.0f) {
    const int baseline = recommendCWCount(mod, rate, waveform);
    if (waveform != WaveformMode::OFDM_CHIRP ||
        !ofdm_link_adaptation::isCoherentModulation(mod) ||
        isNearAwgnOFDM(fading_index, snr_db)) {
        return baseline;
    }

    const auto* descriptor = ofdmCodeRateDescriptor(rate);
    const auto* coherence_floor = ofdmCodeRateDescriptor(CodeRate::R1_2);
    if (descriptor == nullptr || coherence_floor == nullptr ||
        descriptor->code_rate < coherence_floor->code_rate) {
        return baseline;
    }

    // Derive the design Doppler from the measured channel class (fading_index)
    // unless a caller overrides it explicitly (>0). Good→0.1 Hz keeps cw=8;
    // Moderate→0.5 Hz / Poor→1.0 Hz keep the protective shorter-frame cap.
    const float effective_doppler =
        (doppler_hz > 0.0f) ? doppler_hz : designDopplerForFadingIndex(fading_index);
    const uint32_t coherence_ms = coherenceTimeMsForDoppler(effective_doppler);
    int selected = v2::kDefaultFixedFrameCodewords;
    for (int candidate = baseline; candidate >= v2::kDefaultFixedFrameCodewords; --candidate) {
        const auto timing = wideOFDMFrameTiming(mod, rate, candidate);
        if (timing.data_ms <= coherence_ms) {
            selected = candidate;
            break;
        }
    }
    return v2::sanitizeFixedFrameCodewords(selected);
}

inline uint32_t computeWideOFDMAckTimeoutMs(Modulation mod,
                                            CodeRate rate,
                                            size_t window_size,
                                            uint32_t sack_delay_ms,
                                            int ack_repeat_count,
                                            int cw_count = v2::kDefaultFixedFrameCodewords,
                                            uint32_t continuation_reanchor_ms = 0) {
    const int sanitized_cw_count = v2::sanitizeFixedFrameCodewords(cw_count);
    const OFDMFrameTiming timing = wideOFDMFrameTiming(mod, rate, sanitized_cw_count);

    const uint32_t ack_copies = static_cast<uint32_t>(std::clamp(ack_repeat_count, 1, 3));
    const size_t window_frames = std::max<size_t>(1, window_size);
    const uint32_t tx_burst_ms = wideOFDMBurstAirtimeMs(
        mod, rate, window_frames, sanitized_cw_count,
        continuation_reanchor_ms);
    const uint32_t physical_sack_hold_ms = std::max<uint32_t>(
        sack_delay_ms,
        wideOFDMSackDelayMs(mod, rate, window_frames, sanitized_cw_count,
                            continuation_reanchor_ms));
    const uint32_t ack_path_ms = ack_copies * timing.ack_ms + physical_sack_hold_ms;

    constexpr uint32_t audio_chain_rtt_margin_ms = 700;
    const uint32_t decode_jitter_margin_ms = std::max<uint32_t>(700, timing.data_ms / 2)
                                             + audio_chain_rtt_margin_ms;

    // tx_burst_ms spans the actual multi-frame OFDM burst, including the full
    // chirp anchor on the first frame. The SACK path must also include that
    // receiver holdoff because a half-duplex peer cannot ACK until the burst
    // has physically cleared.
    const uint32_t timeout_ms = tx_burst_ms + ack_path_ms + decode_jitter_margin_ms;

    return std::max<uint32_t>(timeout_ms, kWideOFDMAckTimeoutFloorMs);
}

// Burst-transport (unified SR-ARQ) ACK timeout: the deadline the sender waits for the prompt
// tone-burst group-ACK before resending the WHOLE burst. A half-duplex peer cannot ACK until it
// has (1) physically received the multi-frame burst (real-time airtime), (2) held off for its
// deliberate SACK-coalesce delay, (3) decoded, then (4) keyed up to return the 1-CW tone-burst.
// EVERY term is mod/rate/cw/z-derived, so the deadline is correct for the whole modulation family
// by construction (QPSK..16QAM, R1/4..R3/4, any cw/frame-count, z=27/81).
//
// 2026-06-19 FIX: added physical_sack_hold_ms. The sibling computeWideOFDMAckTimeoutMs() budgets the
// receiver's SACK holdoff (line ~697) but THIS burst path did not — only a fixed 1500 ms round-trip
// slack, which is SMALLER than the receiver's mod/rate-scaled SACK-coalesce delay (~2000 ms+). So for
// a full cw8 burst the deadline fired ~1-2 s before the receiver's intentionally-delayed SACK could
// arrive, and the sender resent the entire group even though most frames had already decoded and the
// ACK was in flight (measured live: IONOS MPG@20, group seq63-67 — 4/5 frames OK at RX, SACK sent,
// yet a 5-frame timeout-resend cost ~8.8 s, the single largest retx in the run). Budgeting the same
// hold the receiver applies (wideOFDMSackDelayMs / setSackDelay) makes sender and receiver agree
// across every mod/rate. Conservative by design: an over-long deadline only delays a genuinely-lost
// ACK's resend slightly; an under-long one triggers spurious whole-burst resends (far worse).
inline uint32_t unifiedBurstAckTimeoutMs(Modulation data_mod,
                                         CodeRate data_rate,
                                         int cw_count,
                                         size_t burst_frames,
                                         int data_lifting_z,
                                         Modulation control_mod,
                                         uint32_t configured_sack_delay_ms,
                                         uint32_t reanchor_ms = 0) {
    const int sanitized_cw = v2::sanitizeFixedFrameCodewords(cw_count);
    const size_t frames = std::max<size_t>(1, burst_frames);
    const OFDMFrameTiming timing =
        wideOFDMFrameTiming(data_mod, data_rate, sanitized_cw, data_lifting_z);
    const OFDMFrameTiming control_timing =
        wideOFDMFrameTiming(control_mod, CodeRate::R1_4);

    // (1) actual on-air burst airtime (frames + first-frame anchor), mod/rate/cw/z-derived.
    const uint32_t burst_ms = wideOFDMBurstAirtimeMs(
        data_mod, data_rate, frames, sanitized_cw, reanchor_ms, data_lifting_z);
    // (2) receiver response envelope — RE-DERIVED 2026-07-02 (closes
    // BUG-ACK-TIMEOUT-DOUBLECOUNT). Rig calibration (124 groups across 4 MPG@20
    // transfers) measured the CLEAN-path group-end->SACK hold at 0-1 ms — the
    // old max(configured window-hold, burstAirtime+30) term modeled a hold the
    // receiver never applies on the burst path (it double-counted the burst
    // airtime, ~+8-12 s of deadline). The REAL worst-case delayed response is
    // the receiver's group-timeout fast-NACK, whose airtime-derived budget
    // (accumulateBurstFrames: remaining x1.5 + 3000 ms from first-frame decode)
    // lands at most 0.5 x remaining-airtime + 3000 ms after burst end. Using
    // the SAME formula family keeps sender deadline and receiver timer coherent
    // by construction across every mod/rate/cw/z. configured_sack_delay_ms is
    // deliberately NOT consumed here anymore (it carries the sender-side
    // window-hold arming, which the burst-path receiver does not apply); the
    // parameter stays for call-site stability.
    (void)configured_sack_delay_ms;
    const uint32_t rx_response_ms =
        (frames > 1 ? static_cast<uint32_t>(
                          (frames - 1) * static_cast<size_t>(timing.data_ms) / 2)
                    : 0u) +
        3000u;
    // (3) peer LDPC decode-jitter envelope — a floor plus half a data-frame, so it scales with size.
    const uint32_t decode_margin_ms =
        std::max<uint32_t>(700, timing.data_ms / 2) + 700;
    // (4) one prompt 1-CW group-ack returns (+ any re-anchor); diversity repeats don't gate.
    const uint32_t ack_return_ms = control_timing.ack_ms + reanchor_ms;
    constexpr uint32_t kRoundTripSlackMs = 1500;  // T/R turnaround + jitter cushion
    // §16.4 reserve: a warm-sync-cold escalation re-keys a RELIABILITY-mode burst with a SECOND full
    // anchor beyond what wideOFDMBurstAirtimeMs models; budget it unconditionally (free on clean
    // cycles, the ack-monitor auto-disarms the instant an ACK decodes).
    const uint32_t reliability_full_anchor_ms = kWideOFDMFullAnchorExtraMs;

    return burst_ms + rx_response_ms + decode_margin_ms + ack_return_ms +
           kRoundTripSlackMs + reliability_full_anchor_ms;
}

inline uint32_t bitsPerMCDPSKCarrier(Modulation mod) {
    switch (mod) {
        case Modulation::DBPSK: return 1;
        case Modulation::D8PSK: return 3;
        case Modulation::DQPSK:
        default: return 2;
    }
}

inline uint32_t mcDpskSymbolsToMs(uint32_t symbols, int samples_per_symbol) {
    const int sps = std::clamp(samples_per_symbol, 1, 8192);
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(symbols) * static_cast<uint64_t>(sps) * 1000ULL +
         kOFDMSampleRate / 2) / kOFDMSampleRate);
}

inline MCDPSKFrameTiming mcDpskFrameTiming(Modulation mod,
                                           int num_carriers,
                                           int samples_per_symbol,
                                           int data_cw_count = v2::kDefaultFixedFrameCodewords) {
    const int carriers = std::clamp(num_carriers, 1, 64);
    const int sps = std::clamp(samples_per_symbol, 1, 8192);
    const int cw_count = v2::sanitizeFixedFrameCodewords(data_cw_count);
    const uint32_t bits_per_symbol = static_cast<uint32_t>(carriers) * bitsPerMCDPSKCarrier(mod);
    const uint32_t data_symbols_per_cw =
        (kLDPCBitsPerCodeword + bits_per_symbol - 1) / bits_per_symbol;

    constexpr uint32_t kMCDPSKTrainingSymbols = 8;
    constexpr uint32_t kMCDPSKReferenceSymbols = 1;
    const uint32_t overhead_symbols = kMCDPSKTrainingSymbols + kMCDPSKReferenceSymbols;

    MCDPSKFrameTiming timing;
    timing.overhead_symbols = overhead_symbols;
    timing.data_only_symbols = static_cast<uint32_t>(cw_count) * data_symbols_per_cw;
    timing.data_symbols = overhead_symbols + timing.data_only_symbols;
    timing.ack_symbols = overhead_symbols + data_symbols_per_cw;
    timing.overhead_ms = mcDpskSymbolsToMs(timing.overhead_symbols, sps);
    timing.data_only_ms = mcDpskSymbolsToMs(timing.data_only_symbols, sps);
    timing.data_ms = mcDpskSymbolsToMs(timing.data_symbols, sps);
    timing.ack_ms = mcDpskSymbolsToMs(timing.ack_symbols, sps);
    return timing;
}

inline uint32_t mcDpskBurstAirtimeMs(const MCDPSKFrameTiming& timing,
                                     size_t window_size) {
    if (window_size == 0) return 0;
    const uint64_t burst_ms =
        static_cast<uint64_t>(kMCDPSKDualChirpPreambleMs) +
        static_cast<uint64_t>(timing.overhead_ms) +
        static_cast<uint64_t>(window_size) * timing.data_only_ms;
    return static_cast<uint32_t>(
        std::min<uint64_t>(burst_ms, static_cast<uint64_t>(UINT32_MAX)));
}

inline size_t mcDpskWindowSizeForTiming(const MCDPSKFrameTiming& timing) {
    if (timing.data_ms == 0 || timing.data_only_ms == 0) return 1;

    // #71: the window must be sized so the FULL half-duplex ACK round-trip fits under the
    // ACK RTO with margin — TX burst + the receiver's SERIAL decode of that burst (task
    // #56, and heavier/more variable for DQPSK's 2-bit soft demod) + SACK hold + ACK
    // airtime. The old sizing targeted a 19 s TX burst ONLY and was blind to the
    // decode+ACK half, so DQPSK's shorter frames let the window grow to 5; that burst's
    // round-trip intermittently exceeded the RTO -> the sender blind-resends the whole
    // window -> spiral. Rig-measured @ MPG@9 (paired, live fades): DQPSK window=5 delivered
    // 1/3 (2/3 spiraled, all cause=timeout, 0 cw_fail — decode was clean, the ACK just
    // didn't get back in time); DQPSK window=3 delivered 3/3 CRC-clean at ~2x DBPSK
    // goodput; DBPSK window=3 delivered 3/3. DBPSK was never affected — its longer frames
    // already cap it below 5 (test_connection_policy + the rig log confirm ROBUST_MID/sps=1024
    // -> 3, ROBUST_LOW/sps=2048 -> 1). So cap at the round-trip-safe, rig-validated 3.
    constexpr uint32_t kTargetContinuousBurstMs = 19000;
    constexpr size_t kMaxRoundTripSafeMCDPSKWindow = 3;
    size_t selected = 1;
    for (size_t candidate = 2; candidate <= kMaxRoundTripSafeMCDPSKWindow; ++candidate) {
        if (mcDpskBurstAirtimeMs(timing, candidate) > kTargetContinuousBurstMs) {
            break;
        }
        selected = candidate;
    }
    // Diagnostic override (ULTRA_MCDPSK_WINDOW_CAP=N): further cap the window for A/B
    // round-trip measurement per rung; unset = no-op.
    if (const char* cap = std::getenv("ULTRA_MCDPSK_WINDOW_CAP")) {
        const long v = std::strtol(cap, nullptr, 10);
        if (v >= 1) selected = std::min<size_t>(selected, static_cast<size_t>(v));
    }
    return selected;
}

inline size_t mcDpskWindowSizeForTiming(uint32_t data_frame_ms) {
    if (data_frame_ms == 0) return 1;

    // Round-trip-safe cap of 3 (see the MCDPSKFrameTiming overload above for the #71
    // rig-validated rationale). This coarse overload is not on the production window path
    // (connection.cpp uses the timing overload); kept consistent to avoid a stale bound.
    constexpr uint32_t kTargetBurstMs = 19000;
    constexpr size_t kMaxRoundTripSafeMCDPSKWindow = 3;
    const size_t by_burst = std::max<size_t>(1, kTargetBurstMs / data_frame_ms);
    return std::clamp<size_t>(by_burst, 1, kMaxRoundTripSafeMCDPSKWindow);
}

// Sender ACK RTO for an MC-DPSK selective-repeat window burst. It MUST exceed the real
// half-duplex ACK round-trip, else the sender blind-resends the whole window before the
// legitimate ACK lands -> doubled airtime -> the FINAL file chunk is never reached in a
// bounded session -> the transfer never finalizes (BUG-MCDPSK-FILE-COMPLETION), and on a
// hole the resend collides with the receiver's SACK (BUG-MCDPSK-ACK-COLLISION).
//
// The physical round-trip on MC-DPSK's long (~5.4 s) frames has FOUR terms the old formula
// under-budgeted (it summed only tx_burst + ack + a flat 12 s, and was passed the 30 ms
// carrier-sense coalesce instead of the real receiver hold):
//   (1) tx_burst  = window * data_ms         sender transmits the whole window
//   (2) rx_decode ~= window * data_ms         receiver SERIALLY decodes it before it can
//                                             build the SACK (streaming decoder / RXQ
//                                             backlog, task #56 — each frame ~= one data_ms;
//                                             this is the dominant term the old flat 12 s
//                                             margin missed, measured ~16 s on the rig)
//   (3) receiver_sack_hold_ms                 the tone-burst partial-SACK coalesce hold the
//                                             receiver actually applies (must be the SAME
//                                             value passed to setToneBurstPartialSackDelayMs)
//   (4) ack_copies * ack_ms                   ACK airtime (x repeat copies)
// plus a small T/R turnaround margin. Measured rig RTT (DBPSK R1/4, w=3) ~= 37.9 s; this
// budgets ~43.5 s. The lower clamp is lifted to the physical RTT so it can never truncate
// the round-trip and re-introduce the self-collision (mirrors the narrow-OFDM physical
// floor). See docs/CHANGELOG.md.
inline uint32_t computeMCDPSKAckTimeoutMs(const MCDPSKFrameTiming& timing,
                                          size_t window_size,
                                          uint32_t receiver_sack_hold_ms,
                                          int ack_repeat_count) {
    const uint32_t ack_copies = static_cast<uint32_t>(std::clamp(ack_repeat_count, 1, 3));
    const uint32_t tx_burst_ms  = static_cast<uint32_t>(window_size) * timing.data_ms;
    const uint32_t rx_decode_ms = static_cast<uint32_t>(window_size) * timing.data_ms;
    const uint32_t ack_path_ms  = ack_copies * timing.ack_ms + receiver_sack_hold_ms;
    constexpr uint32_t kTurnaroundMarginMs = 3000;

    const uint32_t physical_rtt_ms = tx_burst_ms + rx_decode_ms + ack_path_ms;
    const uint32_t timeout_ms = physical_rtt_ms + kTurnaroundMarginMs;
    // Never clamp below the physical RTT (else the sender self-collides with blind resends).
    return std::clamp(timeout_ms,
                      std::max<uint32_t>(18000u, physical_rtt_ms),
                      std::max<uint32_t>(72000u, physical_rtt_ms));
}

inline OFDMFrameTiming narrowOFDMFrameTiming(Modulation mod,
                                             int cw_count = v2::kDefaultFixedFrameCodewords) {
    cw_count = v2::sanitizeFixedFrameCodewords(cw_count);
    constexpr float symbol_ms =
        (1000.0f * static_cast<float>(kNarrowOFDMSymbolSamples)) /
        static_cast<float>(kOFDMSampleRate);

    const uint32_t bits_per_symbol = bitsPerOFDMSymbol(
        kNarrowOFDMCarriers, true, static_cast<int>(kNarrowOFDMPilotSpacing), mod);

    const uint32_t data_cw_symbols =
        (static_cast<uint32_t>(cw_count) * kLDPCBitsPerCodeword + bits_per_symbol - 1) / bits_per_symbol;
    const uint32_t ack_cw_symbols =
        (kLDPCBitsPerCodeword + bits_per_symbol - 1) / bits_per_symbol;

    OFDMFrameTiming timing;
    timing.data_symbols = 2 + data_cw_symbols;
    timing.ack_symbols = 2 + ack_cw_symbols;
    timing.data_ms = static_cast<uint32_t>(timing.data_symbols * symbol_ms + 0.5f);
    timing.ack_ms = static_cast<uint32_t>(timing.ack_symbols * symbol_ms + 0.5f);
    return timing;
}

// Receiver tone-burst partial-SACK coalesce hold, applied UNCONDITIONALLY for every mode that
// returns a tone-burst ack (selective_repeat_arq.cpp:622, kToneBurstPartialSackDelayMs). The narrow
// peer withholds its SACK this long before keying up, so the sender's deadline must budget it — the
// same omission the wide-OFDM burst path had (IONOS MPG E5).
inline constexpr uint32_t kToneBurstReceiverSackHoldMs = 1500;

inline uint32_t computeNarrowOFDMAckTimeoutMs(
        Modulation mod,
        int cw_count = v2::kDefaultFixedFrameCodewords,
        size_t window_size = 1,
        uint32_t configured_sack_delay_ms = kToneBurstReceiverSackHoldMs) {
    const OFDMFrameTiming timing = narrowOFDMFrameTiming(mod, cw_count);
    // For window>1 selective-repeat, we hold the ARQ window full during
    // the burst, so the ACK timeout has to cover the full TX burst plus
    // ACK turnaround plus a generous decode margin. Without this scale
    // the timer fires while later frames are still on the wire.
    const uint32_t tx_burst_ms = timing.data_ms *
                                 static_cast<uint32_t>(std::max<size_t>(1, window_size));
    // 2026-06-19: budget the receiver's tone-burst SACK-coalesce hold (the term the formula
    // OMITTED — narrow QPSK/8PSK/QAM16 at window=3 went 0.3-1.5 s short of the 1.5 s hold, the
    // same premature-resend class as wide-OFDM E5). Match the larger of the configured delay and
    // the physical tone-burst hold so sender and receiver agree across every narrow mod/rate.
    const uint32_t physical_sack_hold_ms =
        std::max<uint32_t>(configured_sack_delay_ms, kToneBurstReceiverSackHoldMs);
    const uint32_t timeout_ms = tx_burst_ms + 2 * timing.ack_ms + 120 +
                                std::max<uint32_t>(700, timing.data_ms / 2) +
                                physical_sack_hold_ms;
    // The cap only bounds the NON-physical extras (decode-jitter slop). It must NEVER fall below
    // the physical minimum the peer needs to ACK — burst airtime + ack turnaround + receiver SACK
    // hold — else the clamp re-introduces the premature-timeout bug for large narrow frames.
    const uint32_t physical_floor_ms =
        tx_burst_ms + 2 * timing.ack_ms + physical_sack_hold_ms;
    const uint32_t upper = std::max<uint32_t>(
        physical_floor_ms,
        14000u + physical_sack_hold_ms +
            8000u * static_cast<uint32_t>(std::max<size_t>(1, window_size) - 1));
    return std::clamp(timeout_ms, 4500u, upper);
}

inline WaveformMode selectNegotiatedMode(uint8_t local_caps,
                                         uint8_t remote_caps,
                                         WaveformMode remote_pref,
                                         WaveformMode narrowband_override,
                                         WaveformMode local_pref,
                                         float snr_db,
                                         float fading_index) {
    const uint8_t common = local_caps & remote_caps;
    if (common == 0) {
        // No overlap in advertised capabilities: fall back to MC-DPSK, the
        // universal robust floor every station implements.
        return WaveformMode::MC_DPSK;
    }

    if (remote_pref != WaveformMode::AUTO &&
        (common & modeToCapabilityBit(remote_pref))) {
        return remote_pref;
    }

    if (narrowband_override != WaveformMode::AUTO &&
        (common & modeToCapabilityBit(narrowband_override))) {
        return narrowband_override;
    }

    if (local_pref != WaveformMode::AUTO &&
        (common & modeToCapabilityBit(local_pref))) {
        return local_pref;
    }

    const auto rec = recommendWaveformAndRate(snr_db, fading_index);
    if (common & modeToCapabilityBit(rec.waveform)) {
        return rec.waveform;
    }

    if (common & ModeCapabilities::OFDM_CHIRP) return WaveformMode::OFDM_CHIRP;
    if (common & ModeCapabilities::OFDM_NARROW) return WaveformMode::OFDM_NARROW;
    if (common & ModeCapabilities::MC_DPSK) return WaveformMode::MC_DPSK;

    // Last resort: MC-DPSK, the universal robust floor.
    return WaveformMode::MC_DPSK;
}

}  // namespace connection_policy
}  // namespace protocol
}  // namespace ultra
