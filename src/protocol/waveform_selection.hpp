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
    {Modulation::QPSK,  CodeRate::R3_4, {kRungDisabledDb, 20.0f,           kRungDisabledDb}},  // Good@20 measured 2026-06-02 (5/5 clean, beats R5/6 + 16QAM)
    {Modulation::QPSK,  CodeRate::R2_3, {kRungDisabledDb, 15.0f,           kRungDisabledDb}},  // Good@15 measured 2026-06-02
    {Modulation::QPSK,  CodeRate::R1_2, {10.0f,           10.0f,           18.0f}},            // AWGN/Good 10: Good@10 measured + AWGN≤Good monotonicity (was 12/14); MOD 18 unmeasured-lower
    {Modulation::QPSK,  CodeRate::R1_4, {kOFDMEntryFloorAwgnDb,            // FLOOR = OFDM entry floors
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
    for (const auto& rung : kCoherentLadder) {
        if (snr_db >= rung.min_snr_db[cls]) {
            return {rung.mod, rung.rate};
        }
    }
    return {Modulation::QPSK, CodeRate::R1_4};
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

// Cap the initial OFDM rate during handshake bootstrap (before post-connect quality
// is known): never start above what the ladder supports for (snr, fading), and never
// above R1/2 — the adaptive loop climbs from there once burst-ACK quality arrives.
inline CodeRate capInitialOFDMRate(float snr_db, float fading_index, CodeRate candidate) {
    const CodeRate ladder_rate = selectOFDMCodeRate(snr_db, fading_index);
    CodeRate capped =
        (ofdmCodeRateValue(candidate) < ofdmCodeRateValue(ladder_rate)) ? candidate : ladder_rate;
    if (ofdmCodeRateValue(capped) > ofdmCodeRateValue(CodeRate::R1_2)) {
        capped = CodeRate::R1_2;
    }
    return capped;
}

inline CodeRate capInitialOFDMRate(float snr_db,
                                   float fading_index,
                                   CodeRate candidate,
                                   Modulation /*modulation*/) {
    // ULTRA_FORCE_DATA_RATE: the operator is probing a specific rung — no demotion.
    if (std::getenv("ULTRA_FORCE_DATA_RATE") != nullptr) {
        return candidate;
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
// - In-band SNR >= 30 dB + AWGN: OFDM_CHIRP R3/4 (~3438 bps raw)
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
