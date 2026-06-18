// waveform_selection.hpp - Waveform and rate selection algorithm
//
// Centralized algorithm for selecting waveform mode and code rate
// based on SNR and fading index. Used by both protocol negotiation
// and ModemEngine.
//
// Based on testing with CFO=20Hz across AWGN/good/moderate channels (2026-01-29)

#pragma once

#include "protocol/frame_v2.hpp"  // WaveformMode
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/types.hpp"        // CodeRate

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace ultra {
namespace protocol {

// OFDM "is the wideband waveform viable?" ENTRY floors (in-band SNR, dB), keyed by
// channel class. SINGLE source of truth: both recommendWaveformAndRate() below
// (fading-index-keyed) and connection_policy::selectLadderRung() (enum-keyed)
// reference these, so the OFDM-vs-MC-DPSK entry decision cannot drift between the
// two call sites. Classification thresholds: <0.15 AWGN, <0.65 Good, <1.10 Moderate.
inline constexpr float kOFDMEntryFloorAwgnDb = 8.0f;    // 2026-06-02: lowered 10->8 (R1/4 clean @ AWGN 8, 0% dmg; floor likely lower)
inline constexpr float kOFDMEntryFloorGoodDb = 10.0f;   // 2026-06-02: lowered 12->10 (R1/2 reliable @ Good 10; R1/4 @ Good 8 marginal/cliff)
inline constexpr float kOFDMEntryFloorModerateDb = 14.0f;
// Poor HF (fading >= 1.10: fast Doppler / heavy multipath) routes to MC-DPSK, NEVER
// OFDM (thread A, 2026-05-31). Coherent OFDM phase tracking breaks on fast fading and
// the OFDM band is now coherent-only; the decision doc + the long-standing "Poor HF:
// OFDM fails — use MC-DPSK" limitation both say Poor is MC-DPSK's. An unreachable floor
// expresses "no OFDM at Poor" in the single source both selection paths consult, so the
// retired Poor-OFDM corner can't reappear. (Was 18.0f.)
inline constexpr float kOFDMEntryFloorPoorDb = 1.0e9f;

// Fading-class boundaries (combined freq_cv + temporal_cv). Poor (>= kFadingModerateMax)
// routes to MC-DPSK upstream (kOFDMEntryFloorPoorDb), so the coherent OFDM ladder below
// only ever classifies AWGN / GOOD / MODERATE.
inline constexpr float kFadingAwgnMax = 0.15f;
inline constexpr float kFadingGoodMax = 0.65f;
inline constexpr float kFadingModerateMax = 1.10f;

enum class FadingClass { AWGN = 0, GOOD = 1, MODERATE = 2 };
inline FadingClass classifyFading(float fading_index) {
    if (fading_index < kFadingAwgnMax) return FadingClass::AWGN;
    if (fading_index < kFadingGoodMax) return FadingClass::GOOD;
    return FadingClass::MODERATE;
}

// A rung whose per-class auto-select SNR anchor is this value is "never auto-selected
// on that fading class" — still reachable via ULTRA_FORCE_DATA_MOD/RATE for measurement.
inline constexpr float kRungDisabledDb = 1.0e9f;

// Waveform + rate recommendation
struct WaveformRecommendation {
    WaveformMode waveform;
    CodeRate rate;
    float estimated_throughput_bps;
};

inline constexpr float kAnyFadingIndex = 1000.0f;
inline constexpr float kNoRateSnrDb = 1000.0f;
inline constexpr uint32_t kOFDMWideSampleRate = 48000;
inline constexpr uint32_t kOFDMWideSymbolSamples = 1024 + 128;
inline constexpr uint32_t kOFDMWideCarriers = 59;

// Per-rate METADATA only — selection is the coherent ladder below, not per-rate gates.
// Kept for frame sizing (info/coded bits, cw count) and the adaptive climb/drop
// (next/previousOFDMRateDescriptor walk these by code_rate).
struct OFDMCodeRateDescriptor {
    CodeRate rate = CodeRate::R1_4;
    float code_rate = 0.25f;
    uint32_t info_bits_per_codeword = 162;
    uint32_t coded_bits_per_codeword = v2::LDPC_CODEWORD_BITS;
    int wide_cw_count = v2::kDefaultFixedFrameCodewords;
};

inline constexpr std::array<OFDMCodeRateDescriptor, 5> kOFDMCodeRateDescriptors{{
    // rate           code_rate    K    coded                    cw
    {CodeRate::R1_4, 0.25f,       162, v2::LDPC_CODEWORD_BITS, v2::kDefaultFixedFrameCodewords},
    {CodeRate::R1_2, 0.50f,       324, v2::LDPC_CODEWORD_BITS, 8},
    {CodeRate::R2_3, 2.0f / 3.0f, 432, v2::LDPC_CODEWORD_BITS, 8},
    {CodeRate::R3_4, 0.75f,       486, v2::LDPC_CODEWORD_BITS, 8},
    {CodeRate::R5_6, 5.0f / 6.0f, 540, v2::LDPC_CODEWORD_BITS, 8},
}};

inline const OFDMCodeRateDescriptor* ofdmCodeRateDescriptor(CodeRate rate) {
    for (const auto& descriptor : kOFDMCodeRateDescriptors) {
        if (descriptor.rate == rate) {
            return &descriptor;
        }
    }
    return nullptr;
}

inline const OFDMCodeRateDescriptor& fallbackOFDMCodeRateDescriptor() {
    return kOFDMCodeRateDescriptors.front();
}

// ============================================================================
// COHERENT OFDM RATE LADDER — the single source of truth for auto (mod, rate)
// selection (replaces the old 4-gate-array × 3-pass machinery).
//
// The OFDM band is coherent-only (differential retired to MC-DPSK). One ordered
// ladder of (modulation, code-rate) rungs, each with a per-fading-class minimum
// in-band SNR "anchor" (dB). recommendDataMode() walks it HIGH throughput → LOW
// and picks the first rung the (snr, fading_class) clears; the floor rung
// (QPSK R1/4) anchors at the OFDM entry floors, so once OFDM is selected at all it
// always qualifies. Anchors come from MEASUREMENT, not hand-tuning — see
// docs/RATE_LADDER_ANCHORS.md. kRungDisabledDb = not auto-selected yet (forceable).
// ============================================================================
struct CoherentRung {
    Modulation mod;
    CodeRate rate;
    float min_snr_db[3];  // indexed by FadingClass: [AWGN, GOOD, MODERATE]
};

inline constexpr CoherentRung kCoherentLadder[] = {
    // mod              rate            AWGN             GOOD             MODERATE
    {Modulation::QAM16, CodeRate::R3_4, {kRungDisabledDb, kRungDisabledDb, kRungDisabledDb}},
    {Modulation::QAM16, CodeRate::R2_3, {kRungDisabledDb, kRungDisabledDb, kRungDisabledDb}},
    {Modulation::QAM16, CodeRate::R1_2, {kRungDisabledDb, kRungDisabledDb, kRungDisabledDb}},
    {Modulation::QPSK,  CodeRate::R3_4, {15.0f,           20.0f,           kRungDisabledDb}},  // AWGN@15 measure_ack_fer 2026-06-06 (data4_full floor ~12 dB + ~3 margin); Good@20 measured 2026-06-02
    {Modulation::QPSK,  CodeRate::R2_3, {12.0f,           15.0f,           20.0f}},            // AWGN@12 measure_ack_fer 2026-06-06 (floor ~8 dB + ~4 margin); Good@15 measured 2026-06-02; MOD@20 measured 2026-06-09 (genuine moderate R2/3 9/9 PASS @20-24 dB, qso_sweep) — softens the Good/Moderate cliff: a misclassification now costs 1 rung (R3/4->R2/3) not 2 (R3/4->R1/2)
    {Modulation::QPSK,  CodeRate::R1_2, {10.0f,           10.0f,           18.0f}},            // AWGN/Good 10: Good@10 measured + AWGN≤Good monotonicity (was 12/14); MOD 18 unmeasured-lower
    {Modulation::QPSK,  CodeRate::R1_4, {kOFDMEntryFloorAwgnDb,            // FLOOR = OFDM entry floors
                                         kOFDMEntryFloorGoodDb,
                                         kOFDMEntryFloorModerateDb}},
};

// ── Phase 1 (2026-06-12): env-gated experimental QAM16 ladder ──────────────────
// ULTRA_ENABLE_QAM16_LADDER=1 lets the AUTO path SELECT 16QAM (vs ULTRA_FORCE_DATA_MOD,
// which bypasses the ladder entirely). Default OFF → the default kCoherentLadder above
// is used unchanged (byte-identical behavior). The experimental ladder enables ONLY the
// 16QAM R1/2 Good rung at its measured-clean floor (Good@18: Phase 0a 5/5 PASS, 0
// CW-fails — fable_analysis/07 + data_phase0a_sweep_2026-06-12.tsv). 16QAM AWGN/Moderate
// and the higher-rate 16QAM rungs stay DISABLED: R2/3+ are damage-bound on Good until the
// dense-rung margins work lands (fable_analysis Phase 2b). The CodeRate RateController WILL
// still probe up into 16QAM R2/3 within the fixed modulation and ssthresh-pin back — that
// is the behavior this gate exists to observe, not yet to ship. Lateral vs QPSK R3/4 today
// (16QAM R1/2 ≈ QPSK R3/4, Phase 0a); the value is exercising the real CONNECT-negotiation
// + RX-accept + adaptive path, which the forced knob cannot.
inline bool qam16LadderEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("ULTRA_ENABLE_QAM16_LADDER");
        return e != nullptr && std::atoi(e) != 0;
    }();
    return on;
}

inline constexpr CoherentRung kCoherentLadderQAM16Exp[] = {
    // mod              rate            AWGN             GOOD             MODERATE
    {Modulation::QAM16, CodeRate::R3_4, {kRungDisabledDb, kRungDisabledDb, kRungDisabledDb}},
    {Modulation::QAM16, CodeRate::R2_3, {kRungDisabledDb, 20.0f,           kRungDisabledDb}},  // 2026-06-14: Good@20 + cross-frame TIME interleave (auto-on for QAM16) = ~1790 bps, 6/6 PASS — beats the R1/2 rung (~1550). EXPERIMENTAL zero-margin anchor (measured FLOOR, not floor+2); raise to ~22 for margin parity before default, beware Moderate-misclassified-as-Good. Below 20 -> R1/2@18.
    {Modulation::QAM16, CodeRate::R1_2, {kRungDisabledDb, 18.0f,           kRungDisabledDb}},  // EXPERIMENTAL zero-margin anchor: Good@18 measured 5/5 PASS, 0 CW-fail (Phase 0a) — this is the measured FLOOR, NOT floor+2dB like the QPSK rungs. Acceptable while env-gated/observed; raise to ~20 (margin parity) before this graduates to default, and beware Moderate-misclassified-as-Good landing here.
    {Modulation::QPSK,  CodeRate::R3_4, {15.0f,           20.0f,           kRungDisabledDb}},
    {Modulation::QPSK,  CodeRate::R2_3, {12.0f,           15.0f,           20.0f}},
    {Modulation::QPSK,  CodeRate::R1_2, {10.0f,           10.0f,           18.0f}},
    {Modulation::QPSK,  CodeRate::R1_4, {kOFDMEntryFloorAwgnDb,
                                         kOFDMEntryFloorGoodDb,
                                         kOFDMEntryFloorModerateDb}},
};

struct CoherentPick {
    Modulation mod;
    CodeRate rate;
};

// Highest ladder rung whose per-class anchor the (snr, fading) clears. Falls back
// to the QPSK R1/4 floor (the last entry always qualifies once above the entry floor).
inline CoherentPick selectCoherentOFDM(float snr_db, float fading_index) {
    // Poor (>= kFadingModerateMax) routes to MC-DPSK upstream and OFDM is never
    // selected there; hold the QPSK R1/4 floor defensively if ever reached.
    if (fading_index >= kFadingModerateMax) {
        return {Modulation::QPSK, CodeRate::R1_4};
    }
    const int cls = static_cast<int>(classifyFading(fading_index));
    // Default = kCoherentLadder (QPSK-only auto). ULTRA_ENABLE_QAM16_LADDER swaps in the
    // experimental ladder with the measured 16QAM R1/2 Good rung enabled — default OFF.
    const CoherentRung* rungs = kCoherentLadder;
    size_t n = sizeof(kCoherentLadder) / sizeof(kCoherentLadder[0]);
    if (qam16LadderEnabled()) {
        rungs = kCoherentLadderQAM16Exp;
        n = sizeof(kCoherentLadderQAM16Exp) / sizeof(kCoherentLadderQAM16Exp[0]);
    }
    for (size_t i = 0; i < n; ++i) {
        if (snr_db >= rungs[i].min_snr_db[cls]) {
            return {rungs[i].mod, rungs[i].rate};
        }
    }
    return {Modulation::QPSK, CodeRate::R1_4};
}

// Highest GUI-validated code rate for a given coherent modulation — a per-modulation
// ceiling for the adaptive RateController, which is otherwise modulation-BLIND (it walks
// {R1/4..R5/6} at whatever modulation was fixed at CONNECT). Without this, a clean stretch
// promotes the connect-time 16QAM R1/2 up into the measured DAMAGE-BOUND 16QAM R2/3/R3/4
// (Phase 0a: 55-70% frame loss, 1-of-3 link-death — fable_analysis/07) BEFORE the reactive
// ssthresh can cap it, taking a frame into a fade at the over-climbed rung. QAM16 is capped
// at R1/2 (the only Good-clean QAM16 rung measured to date); RAISE this per rung as the
// dense-rung margins work (fable_analysis Phase 2b) validates QAM16 R2/3+ on the GUI gate.
// Non-QAM16 modulations keep the full ladder (R5_6 = no cap = current behavior).
inline CodeRate maxValidatedCoherentRate(Modulation mod) {
    switch (mod) {
        // 2026-06-14: lifted R1/2 -> R2/3. Cross-frame TIME interleave (auto-on for QAM16 via
        // burstCrossFrameInterleaveOn) makes 16QAM R2/3 Good@20 viable at ~1790 bps GUI-measured
        // (6/6 seeds), now ABOVE the R1/2 clean rung (~1550). Capped at R2/3, NOT R3/4 — 16QAM
        // R3/4 stays damage-bound even with interleave; AWGN@30 ceiling is ~2850.
        case Modulation::QAM16: return CodeRate::R2_3;
        default:                return CodeRate::R5_6;
    }
}

inline float estimateWideOFDMRawBps(Modulation mod, CodeRate rate) {
    const auto* descriptor = ofdmCodeRateDescriptor(rate);
    if (descriptor == nullptr) {
        descriptor = &fallbackOFDMCodeRateDescriptor();
    }
    const int pilot_spacing = ofdm_link_adaptation::recommendedPilotSpacing(mod, rate);
    const auto bits_per_symbol = static_cast<float>(
        ofdm_link_adaptation::bitsPerOFDMSymbol(
            static_cast<int>(kOFDMWideCarriers), true, pilot_spacing, mod));
    const float symbol_rate =
        static_cast<float>(kOFDMWideSampleRate) / static_cast<float>(kOFDMWideSymbolSamples);
    return bits_per_symbol * symbol_rate * descriptor->code_rate;
}

inline const OFDMCodeRateDescriptor* previousOFDMRateDescriptor(CodeRate rate) {
    const auto* current = ofdmCodeRateDescriptor(rate);
    if (current == nullptr) {
        return nullptr;
    }
    const OFDMCodeRateDescriptor* previous = nullptr;
    for (const auto& descriptor : kOFDMCodeRateDescriptors) {
        if (descriptor.code_rate < current->code_rate &&
            (previous == nullptr || descriptor.code_rate > previous->code_rate)) {
            previous = &descriptor;
        }
    }
    return previous;
}

inline const OFDMCodeRateDescriptor* nextOFDMRateDescriptorToward(CodeRate current_rate,
                                                                  CodeRate target_rate) {
    const auto* current = ofdmCodeRateDescriptor(current_rate);
    const auto* target = ofdmCodeRateDescriptor(target_rate);
    if (current == nullptr || target == nullptr || target->code_rate <= current->code_rate) {
        return current;
    }
    const OFDMCodeRateDescriptor* next = target;
    for (const auto& descriptor : kOFDMCodeRateDescriptors) {
        if (descriptor.code_rate > current->code_rate &&
            descriptor.code_rate < next->code_rate) {
            next = &descriptor;
        }
    }
    return next;
}

inline float ofdmCodeRateValue(CodeRate rate) {
    if (const auto* descriptor = ofdmCodeRateDescriptor(rate)) {
        return descriptor->code_rate;
    }
    return fallbackOFDMCodeRateDescriptor().code_rate;
}

// Shared helper: Select code rate for OFDM modes based on SNR and fading
// This is the SINGLE SOURCE OF TRUTH for rate selection thresholds.
// Both recommendWaveformAndRate() and recommendDataMode() use this.
//
// Fading index now combines freq_cv + temporal_cv (Doppler measurement).
// Thresholds (2026-05-21 floor recalibration sweep, PAPR-OFF baseline,
// in-band SNR convention; forced-waveform caveat below):
//   AWGN only (< 0.10):             R3/4 @ in-band SNR >= 25 (legacy)
//   Near-AWGN (< 0.15):             R2/3 @ in-band SNR >= 25 (legacy)
//   AWGN     (< 0.15):              R1/2 @ in-band SNR >= 12 (was 25)
//   Good     (< 0.65):              R1/2 @ in-band SNR >= 14 (was 25)
//   Moderate (< 1.10):              R1/2 @ in-band SNR >= 18 (was 25)
//   Heavy+   (>= 1.10):             R1/4 only
//
// 2026-05-21 floor recalibration measurements (PAPR-OFF, --waveform ofdm_chirp
// forced — see FLOOR_RECALIBRATION_2026_05_21.md). Each "reliable floor" is
// the lowest SNR where 3/3 seeds PASS with stable retx behavior:
//   OFDM R1/2 AWGN     reliable floor = 10 dB → gate ≥ 12 dB (+2 dB margin)
//   OFDM R1/2 Good     reliable floor = 12 dB → gate ≥ 14 dB (+2 dB margin)
//   OFDM R1/2 Moderate reliable floor = 16 dB → gate ≥ 18 dB (+2 dB margin)
//
// The +2 dB margin protects against seed variance and the
// IMD-vs-headroom asymmetry that PAPR ON introduces on simulator paths.
// Production hardware should see net +1.4 to +1.6 dB additional margin
// from PAPR-driven on-wire RMS gain (see PAPR_HARDWARE_VERIFICATION
// CHANGELOG entry).
//
// Caveat: today's floor measurements used `--waveform ofdm_chirp` which
// skips the MC-DPSK→OFDM handshake upgrade. Auto-negotiation
// re-verification is a deferred follow-up workstream
// (feedback_test_with_auto_negotiation.md). The relative ordering of
// R1/2 vs R1/4 should be unaffected, but absolute thresholds may shift
// ±1 dB.
//
// R3/4 verified (2026-02-10) — not re-measured:
//   DQPSK R3/4 AWGN SNR=20: 10/10 seeds PASS, 0 retransmissions
//   DQPSK R3/4 Good fading: FAILS (23 retx / 5 seeds) — AWGN only!
// R2/3 verified (2026-03-15) — not re-measured:
//   10KB Good fading SNR=15: 1485 bps, 33% retx
//   Demoted from Good: R1/2 gives similar throughput with half the retx.
// Rate for the coherent OFDM ladder at (snr, fading). SINGLE SOURCE OF TRUTH for
// rate selection — both recommendDataMode() and recommendWaveformAndRate() use it.
inline CodeRate selectOFDMCodeRate(float snr_db, float fading_index) {
    return selectCoherentOFDM(snr_db, fading_index).rate;
}

// Initial OFDM rate at handshake bootstrap: start at the LADDER rate for the measured
// (snr, fading) — never above what the ladder supports, but no more conservative either.
// The per-class anchors (kCoherentLadder: AWGN measure_ack_fer 2026-06-06, GOOD measured
// 2026-06-02) ARE the measured connect-time reliability for that channel, so the
// connect-time SNR+fading already picks the right starting rung. (Removed the old "on
// fading, pin to R1/2 and climb" rule: with adaptive rate off it permanently froze
// fading channels at R1/2 even where the ladder has a measured R2/3+ — e.g. Good@15
// crawled at R1/2 instead of the measured R2/3.)
inline CodeRate capInitialOFDMRate(float snr_db, float fading_index, CodeRate candidate) {
    const CodeRate ladder_rate = selectOFDMCodeRate(snr_db, fading_index);
    return (ofdmCodeRateValue(candidate) < ofdmCodeRateValue(ladder_rate))
               ? candidate
               : ladder_rate;
}

inline CodeRate capInitialOFDMRate(float snr_db,
                                   float fading_index,
                                   CodeRate candidate,
                                   Modulation modulation) {
    // ULTRA_FORCE_DATA_RATE: the operator is probing a specific rung — no demotion.
    if (std::getenv("ULTRA_FORCE_DATA_RATE") != nullptr) {
        return candidate;
    }
    // ULTRA_R23_BASIS (2026-06-17, default ON; set =0 to disable): decouple the ENTRY rate
    // from the Good/Moderate classifier, which is unreliable on real hardware (it coin-flips
    // a genuinely-Good IONOS channel between Good and Moderate, dropping the entry from R3/4
    // to R1/2). On any FADING channel at usable SNR (>=18 dB) start every coherent-QPSK
    // connection at the robust R2/3 basis regardless of the (unreliable) Good-vs-Moderate
    // class, and let the closed-loop adaptive ladder climb to R3/4 on MEASURED headroom.
    //   GATED ON FADING PRESENT (fading_index >= kFadingAwgnMax): on a genuine AWGN channel
    //   there is no fade margin to recover and R3/4 is the measured-correct rung — a blanket
    //   R2/3 pin there is a pure ~11% throughput loss (OTASim AWGN@20: R3/4 2150 vs R2/3 1920
    //   bps). The fading_index cannot split Good from Moderate but it cleanly separates AWGN
    //   (~0) from any fading channel (~0.5), which is exactly the distinction this gate needs.
    // Entry-only: called only from the two initial-mode sites (connection.cpp:405,
    // connection_handlers.cpp:269); the rate_controller climb path is not gated, so
    // R2/3->R3/4 still works when adaptation is on.
    {
        const char* b = std::getenv("ULTRA_R23_BASIS");
        const bool r23_basis_on = !(b && b[0] == '0');  // default ON
        if (r23_basis_on && modulation == Modulation::QPSK && snr_db >= 18.0f &&
            fading_index >= kFadingAwgnMax) {
            return CodeRate::R2_3;
        }
    }
    // Coherent-only band: modulation no longer changes the cap.
    return capInitialOFDMRate(snr_db, fading_index, candidate);
}

// Recommend waveform and rate based on SNR and fading index
//
// Fading index now combines freq_cv + temporal_cv (Doppler measurement).
// Raw-PHY estimates below use the strict definition
// `data_carriers × bits/sym × sym_rate × code_rate` against the
// production geometry (8-car MC-DPSK; 1024-FFT 59-car OFDM-CHIRP with
// CP=LONG, 1152 samples/symbol; pilots from
// `ofdm_link_adaptation::recommendedPilotSpacing()`).
//
// Calibrated reliability bands (2026-02-11):
// - In-band SNR < 10 dB: MC-DPSK is most robust (~375 bps raw at 8 car DQPSK R1/4)
// - In-band SNR >= 10 dB + AWGN: OFDM_CHIRP R1/4 (~1104 bps raw, warm LTS sync)
// - In-band SNR >= 12 dB + good fading: OFDM_CHIRP R1/4 with extra fading margin
// - In-band SNR >= 14 dB + moderate fading: OFDM_CHIRP R1/4 with extra fading margin
// - In-band SNR >= 18 dB + heavy fading: OFDM_CHIRP R1/4 with extra fading margin
// - In-band SNR >= 12 dB + AWGN: OFDM_CHIRP QPSK R2/3; >= 15 dB: R3/4 (measure_ack_fer
//   2026-06-06 data4_full floors R2/3 ~8 dB, R3/4 ~12 dB + margin; the OLD ">= 30 dB
//   for AWGN R3/4" was a stale, never-measured estimate — AWGN R3/4 closes far lower)
// - In-band SNR >= 30 dB + good fading: OFDM_CHIRP R2/3 (~2944 bps raw)
// - In-band SNR >= 25 dB + good/moderate fading: OFDM_CHIRP R1/2 (~2208 bps raw)
// - Heavy+ fading (>= 1.10): R1/4 only (~1104 bps raw)
//
// Calibrated fading thresholds:
//   < 0.15: True AWGN, < 0.65: Good, >= 0.65: Moderate+
inline WaveformRecommendation recommendWaveformAndRate(float snr_db, float fading_index) {
    WaveformRecommendation rec;
    const float ofdm_floor =
        (fading_index < 0.15f) ? kOFDMEntryFloorAwgnDb :
        (fading_index < 0.65f) ? kOFDMEntryFloorGoodDb :
        (fading_index < 1.10f) ? kOFDMEntryFloorModerateDb : kOFDMEntryFloorPoorDb;

    if (snr_db < ofdm_floor) {
        // Low SNR: MC-DPSK 8 carriers is most robust
        rec.waveform = WaveformMode::MC_DPSK;
        rec.rate = CodeRate::R1_4;
        rec.estimated_throughput_bps = 375.0f;
    }
    else if (fading_index < 0.15f) {
        // True AWGN (no fading)
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = selectOFDMCodeRate(snr_db, fading_index);
        rec.estimated_throughput_bps = estimateWideOFDMRawBps(Modulation::DQPSK, rec.rate);
    }
    else if (fading_index < 1.10f) {
        // Good-to-moderate fading: OFDM_CHIRP with extra floor margin vs AWGN.
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = selectOFDMCodeRate(snr_db, fading_index);
        rec.estimated_throughput_bps = estimateWideOFDMRawBps(Modulation::DQPSK, rec.rate);
    }
    else {
        // Heavy fading keeps extra margin but still uses the OFDM_CHIRP R1/4 floor.
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = selectOFDMCodeRate(snr_db, fading_index);
        rec.estimated_throughput_bps = estimateWideOFDMRawBps(Modulation::DQPSK, rec.rate);
    }

    return rec;
}

// Recommend modulation and code rate for data mode within an established connection
// This is used after waveform negotiation to set the data transmission parameters.
// Uses selectOFDMCodeRate() for rate selection to stay consistent with recommendWaveformAndRate().
//
// For OFDM modes: measured coherent rungs first, then DQPSK with rate from
// selectOFDMCodeRate().
// QPSK is the Good-fading workhorse rung at the measured Good@20 R2/3 floor.
// Admit one protocol SNR quantum below the nominal 20 dB floor: CONNECT and
// MODE_CHANGE carry SNR in 0.25 dB steps, and Phase-1 GUI seeds 4/6 landed
// at the 19.8 dB gray band where DQPSK R1/2 was selected and failed.
// D8PSK/QAM16 are clean-channel rungs; too sensitive for Good-fading nulls.
// For MC-DPSK: Always DQPSK R1/4
//
inline void recommendDataMode(float snr_db, WaveformMode waveform,
                               Modulation& mod, CodeRate& rate, float fading_index = 0.0f) {
    // 2026-05-28 test-only override: ULTRA_FORCE_DATA_MOD / ULTRA_FORCE_DATA_RATE
    // bypass the ladder ENTIRELY (including MC-DPSK / OFDM_NARROW / ladder rungs)
    // so we can probe decode reliability of a specific (mod, rate) rung on a live
    // channel. Placed at the top so every return path is short-circuited.
    {
        bool forced_any = false;
        if (const char* env = std::getenv("ULTRA_FORCE_DATA_MOD")) {
            const std::string s(env);
            Modulation forced = Modulation::AUTO;
            if      (s == "DBPSK")                       forced = Modulation::DBPSK;
            else if (s == "BPSK")                        forced = Modulation::BPSK;
            else if (s == "DQPSK")                       forced = Modulation::DQPSK;
            else if (s == "QPSK")                        forced = Modulation::QPSK;
            else if (s == "D8PSK")                       forced = Modulation::D8PSK;
            else if (s == "QAM8" || s == "8PSK")         forced = Modulation::QAM8;
            else if (s == "QAM16" || s == "16QAM")       forced = Modulation::QAM16;
            else if (s == "QAM32" || s == "32QAM")       forced = Modulation::QAM32;
            if (forced != Modulation::AUTO) { mod = forced; forced_any = true; }
        }
        if (const char* env = std::getenv("ULTRA_FORCE_DATA_RATE")) {
            const std::string s(env);
            CodeRate forced = CodeRate::AUTO;
            if      (s == "R1_4" || s == "r1_4") forced = CodeRate::R1_4;
            else if (s == "R1_3" || s == "r1_3") forced = CodeRate::R1_3;
            else if (s == "R1_2" || s == "r1_2") forced = CodeRate::R1_2;
            else if (s == "R2_3" || s == "r2_3") forced = CodeRate::R2_3;
            else if (s == "R3_4" || s == "r3_4") forced = CodeRate::R3_4;
            else if (s == "R5_6" || s == "r5_6") forced = CodeRate::R5_6;
            if (forced != CodeRate::AUTO) { rate = forced; forced_any = true; }
        }
        if (forced_any) return;
    }

    // MC-DPSK always uses DQPSK R1/4 for robustness
    if (waveform == WaveformMode::MC_DPSK) {
        mod = Modulation::DQPSK;
        rate = CodeRate::R1_4;
        return;
    }

    // OFDM_NARROW: DQPSK only, R1/4 default, R1/2 for clean-enough
    // conditions. Sweeps in cli_simulator (2026-05-03) verified R1/2
    // narrow + window=3 passes 7-message test at:
    //   legacy SNR=8  AWGN          (PASS)
    //   legacy SNR=10 good fading   (PASS)
    //   legacy SNR=12 good fading   (PASS)
    //   legacy SNR=15 good fading   (PASS)
    // and R1/4 narrow + window=3 passes:
    //   SNR=8  good fading   (PASS — documented baseline)
    //   SNR=8  moderate      (PASS, slow but recovers)
    // The hard floor (SNR=8 good fading R1/4) is preserved by the
    // in-band SNR>=20 gate on good fading. AWGN keeps the in-band SNR>=18 trigger
    // because near-AWGN is a much easier channel.
    if (waveform == WaveformMode::OFDM_NARROW) {
        mod = Modulation::DQPSK;
        const bool awgn_path = fading_index < 0.15f && snr_db >= 18.0f;
        const bool good_path = fading_index < 0.65f && snr_db >= 20.0f;
        if (awgn_path || good_path) {
            rate = CodeRate::R1_2;
        } else {
            rate = CodeRate::R1_4;
        }
        return;
    }

    // OFDM modes: measured coherent rungs gated on conditions, otherwise DQPSK.
    //
    // D8PSK ladder (re-enabled 2026-05-04 after wide cli_simulator
    // sweeps showed it works in fading with the post-2026-03-15 CPE
    // correction + per-symbol pilot tracking already in the demod):
    //   sweep results for D8PSK on good fading (cli_simulator 7-msg):
    //     R1/2 SNR=8:   FAIL (cliff)
    //     R1/2 SNR=10:  PASS, 4 retx
    //     R1/2 SNR=12:  PASS, 2 retx
    //     R1/2 SNR=15:  PASS, 0 retx
    //     R2/3 SNR=10:  PASS, 28 retx (high)
    //     R2/3 SNR=12:  PASS, 45 retx (very high)
    //     R2/3 SNR=15:  PASS, 0 retx
    //     R2/3 SNR=20:  PASS, 1 retx
    //     R3/4 SNR=20:  PASS, 6 retx (border, AWGN-only)
    //   Moderate fading: R1/2 SNR>=15 also stable (3-6 retx).
    //
    // The throughput case: D8PSK R2/3 at SNR=15 good fading carries
    // 1.5× the bits/symbol of DQPSK R2/3 at the same conditions, so
    // the throughput jumps from ~3.4 kbps to ~5 kbps with zero retx.
    //
    // OFDM band is COHERENT-ONLY (differential DQPSK/D8PSK retired to MC-DPSK; Poor
    // routes to MC-DPSK upstream via kOFDMEntryFloorPoorDb, so OFDM never reaches
    // this with Poor fading). The single coherent ladder picks BOTH modulation and
    // rate from the measured per-fading-class anchors (kCoherentLadder) — no more
    // shouldSelectQPSK/QAM16 branches. See docs/RATE_LADDER_ANCHORS.md.
    const CoherentPick pick = selectCoherentOFDM(snr_db, fading_index);
    mod = pick.mod;
    rate = pick.rate;
}

// Conservative raw-PHY bitrate estimate per waveform mode. Used by
// TNC STATS and GUI wire-time estimators that need a single bps
// number without knowing the active rate/modulation pair. The numbers
// are the documented "raw PHY (theoretical maximum)" from
// README.md for the production geometry — they are deliberately
// pessimistic vs the per-rate ladder, so use selectOFDMCodeRate()
// when the caller wants per-rate precision.
inline int estimatedBitrateBpsForMode(WaveformMode mode) {
    switch (mode) {
    case WaveformMode::OFDM_NARROW:
        return 386;   // DQPSK R1/2, 18 data carriers @ 21.429 sym/s
    case WaveformMode::MC_DPSK:
        return 375;   // 8 carriers DQPSK R1/4 @ 93.75 sym/s
    case WaveformMode::OFDM_CHIRP:
        return 2208;  // DQPSK R1/2, 53 data carriers @ 41.667 sym/s
    case WaveformMode::OTFS_EQ:
    case WaveformMode::OTFS_RAW:
    case WaveformMode::MFSK:
    case WaveformMode::AUTO:
        return 0;
    }
    return 0;
}

} // namespace protocol
} // namespace ultra
