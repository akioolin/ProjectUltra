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

inline constexpr float kQAM16AwgnFadingMax = 0.15f;
inline constexpr float kQAM16AwgnSnrFloorDb = 16.0f;
inline constexpr float kQAM16AwgnR12SnrFloorDb = 19.0f;
inline constexpr float kQAM16AwgnR23SnrFloorDb = 19.5f;
inline constexpr float kQAM16AwgnR34SnrFloorDb = 19.7f;
inline constexpr float kQAM16GoodFadingMax = 0.80f;
inline constexpr float kQAM16GoodSnrFloorDb = 17.0f;
inline constexpr float kQPSKMultipathFadingMin = kQAM16AwgnFadingMax;
inline constexpr float kQPSKGoodFadingMax = kQAM16GoodFadingMax;
inline constexpr float kProtocolSnrQuantumDb = 0.25f;
inline constexpr float kQPSKGoodR23MeasuredFloorDb = 20.0f;
inline constexpr float kQPSKGoodR23SnrFloorDb =
    kQPSKGoodR23MeasuredFloorDb - kProtocolSnrQuantumDb;
// R3/4 is PROMOTED on Good fading (selected, not forced) once the file-class
// composite — cross-frame burst interleave (design §14.14) — makes its thin 25%
// FEC survive Good@20 (offline harness: ~92% chunk recovery vs ~33% without).
// Reliability is delivered by the burst transport, not this gate; the gate only
// lets the ladder choose R3/4 when SNR/fading warrant it.
inline constexpr float kQPSKGoodR34MeasuredFloorDb = 20.0f;
inline constexpr float kQPSKGoodR34SnrFloorDb =
    kQPSKGoodR34MeasuredFloorDb - kProtocolSnrQuantumDb;

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

struct OFDMRateGate {
    float max_fading_index = 0.0f;
    float required_snr_db = kNoRateSnrDb;
};

struct OFDMCodeRateDescriptor {
    CodeRate rate = CodeRate::R1_4;
    float code_rate = 0.25f;
    uint32_t info_bits_per_codeword = 162;
    uint32_t coded_bits_per_codeword = v2::LDPC_CODEWORD_BITS;
    int wide_cw_count = v2::kDefaultFixedFrameCodewords;
    std::array<OFDMRateGate, 4> differential_gates{};
    size_t differential_gate_count = 0;
    std::array<OFDMRateGate, 4> bootstrap_gates{};
    size_t bootstrap_gate_count = 0;
    std::array<OFDMRateGate, 4> qam16_gates{};
    size_t qam16_gate_count = 0;
    std::array<OFDMRateGate, 4> qpsk_gates{};
    size_t qpsk_gate_count = 0;
};

inline constexpr std::array<OFDMCodeRateDescriptor, 4> kOFDMCodeRateDescriptors{{
    {
        CodeRate::R1_4,
        0.25f,
        162,
        v2::LDPC_CODEWORD_BITS,
        v2::kDefaultFixedFrameCodewords,
        {{{kAnyFadingIndex, -1000.0f}}},
        1,
        {{{kAnyFadingIndex, -1000.0f}}},
        1,
        {{{kQAM16AwgnFadingMax, kQAM16AwgnSnrFloorDb}}},
        1,
        {},
        0,
    },
    {
        CodeRate::R1_2,
        0.50f,
        324,
        v2::LDPC_CODEWORD_BITS,
        8,
        {{{0.15f, 12.0f}, {0.65f, 14.0f}, {1.10f, 18.0f}}},
        3,
        {{{kAnyFadingIndex, -1000.0f}}},
        1,
        {{{kQAM16AwgnFadingMax, kQAM16AwgnR12SnrFloorDb}}},
        1,
        {{{kQPSKGoodFadingMax, kQPSKGoodR23SnrFloorDb}}},
        1,
    },
    {
        CodeRate::R2_3,
        2.0f / 3.0f,
        432,
        v2::LDPC_CODEWORD_BITS,
        8,
        {{{0.15f, 25.0f}}},
        1,
        {{{0.10f, kQAM16AwgnR23SnrFloorDb}}},
        1,
        {{{kQAM16AwgnFadingMax, kQAM16AwgnR23SnrFloorDb}}},
        1,
        {{{kQPSKGoodFadingMax, kQPSKGoodR23SnrFloorDb}}},
        1,
    },
    {
        CodeRate::R3_4,
        0.75f,
        486,
        v2::LDPC_CODEWORD_BITS,
        8,
        {{{0.10f, 25.0f}}},
        1,
        {{{0.05f, kQAM16AwgnR34SnrFloorDb}}},
        1,
        {{{kQAM16AwgnFadingMax, kQAM16AwgnR34SnrFloorDb}}},
        1,
        // QPSK R3/4 Good gate (PROMOTION): the 2026-05-26 "R3/4 hard-fails Good@20"
        // result held for the NON-interleaved path. With the file-class cross-frame
        // burst interleave (design §14.14) the thin 25% FEC survives Good@20 fades
        // (offline ~92% chunk recovery). The ladder may now promote to R3/4 here;
        // the burst stop-and-wait transport is what makes it deliver.
        {{{kQPSKGoodFadingMax, kQPSKGoodR34SnrFloorDb}}},
        1,
    },
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

inline bool rateGateAllows(const OFDMRateGate& gate, float snr_db, float fading_index) {
    return fading_index < gate.max_fading_index && snr_db >= gate.required_snr_db;
}

inline bool descriptorAllowsDifferentialOFDM(const OFDMCodeRateDescriptor& descriptor,
                                             float snr_db,
                                             float fading_index) {
    for (size_t i = 0; i < descriptor.differential_gate_count; ++i) {
        if (rateGateAllows(descriptor.differential_gates[i], snr_db, fading_index)) {
            return true;
        }
    }
    return false;
}

inline bool descriptorAllowsQAM16OFDM(const OFDMCodeRateDescriptor& descriptor,
                                      float snr_db,
                                      float fading_index) {
    for (size_t i = 0; i < descriptor.qam16_gate_count; ++i) {
        if (rateGateAllows(descriptor.qam16_gates[i], snr_db, fading_index)) {
            return true;
        }
    }
    return false;
}

inline bool descriptorAllowsQPSKOFDM(const OFDMCodeRateDescriptor& descriptor,
                                     float snr_db,
                                     float fading_index) {
    if (fading_index < kQPSKMultipathFadingMin) {
        return false;
    }
    for (size_t i = 0; i < descriptor.qpsk_gate_count; ++i) {
        if (rateGateAllows(descriptor.qpsk_gates[i], snr_db, fading_index)) {
            return true;
        }
    }
    return false;
}

inline bool descriptorAllowsBootstrapOFDM(const OFDMCodeRateDescriptor& descriptor,
                                          float snr_db,
                                          float fading_index) {
    for (size_t i = 0; i < descriptor.bootstrap_gate_count; ++i) {
        if (rateGateAllows(descriptor.bootstrap_gates[i], snr_db, fading_index)) {
            return true;
        }
    }
    return false;
}

template <typename Predicate>
inline const OFDMCodeRateDescriptor* selectBestOFDMRateDescriptor(Predicate allows) {
    const OFDMCodeRateDescriptor* best = nullptr;
    for (const auto& descriptor : kOFDMCodeRateDescriptors) {
        if (!allows(descriptor)) {
            continue;
        }
        if (best == nullptr || descriptor.code_rate > best->code_rate) {
            best = &descriptor;
        }
    }
    return best;
}

inline const OFDMCodeRateDescriptor* selectDifferentialOFDMRateDescriptor(float snr_db,
                                                                          float fading_index) {
    return selectBestOFDMRateDescriptor([&](const OFDMCodeRateDescriptor& descriptor) {
        return descriptorAllowsDifferentialOFDM(descriptor, snr_db, fading_index);
    });
}

inline const OFDMCodeRateDescriptor* selectQAM16OFDMRateDescriptor(float snr_db,
                                                                   float fading_index) {
    return selectBestOFDMRateDescriptor([&](const OFDMCodeRateDescriptor& descriptor) {
        return descriptorAllowsQAM16OFDM(descriptor, snr_db, fading_index);
    });
}

inline const OFDMCodeRateDescriptor* selectQPSKOFDMRateDescriptor(float snr_db,
                                                                  float fading_index) {
    return selectBestOFDMRateDescriptor([&](const OFDMCodeRateDescriptor& descriptor) {
        return descriptorAllowsQPSKOFDM(descriptor, snr_db, fading_index);
    });
}

inline const OFDMCodeRateDescriptor* selectBootstrapOFDMRateDescriptor(float snr_db,
                                                                       float fading_index,
                                                                       CodeRate candidate) {
    const auto* candidate_descriptor = ofdmCodeRateDescriptor(candidate);
    if (candidate_descriptor == nullptr) {
        return nullptr;
    }
    return selectBestOFDMRateDescriptor([&](const OFDMCodeRateDescriptor& descriptor) {
        return descriptor.code_rate <= candidate_descriptor->code_rate &&
               descriptorAllowsBootstrapOFDM(descriptor, snr_db, fading_index);
    });
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
inline CodeRate selectOFDMCodeRate(float snr_db, float fading_index) {
    const auto* descriptor = selectDifferentialOFDMRateDescriptor(snr_db, fading_index);
    return descriptor ? descriptor->rate : fallbackOFDMCodeRateDescriptor().rate;
}

inline CodeRate selectQAM16CodeRate(float snr_db, float fading_index) {
    const auto* descriptor = selectQAM16OFDMRateDescriptor(snr_db, fading_index);
    return descriptor ? descriptor->rate : CodeRate::AUTO;
}

inline CodeRate selectQPSKCodeRate(float snr_db, float fading_index) {
    const auto* descriptor = selectQPSKOFDMRateDescriptor(snr_db, fading_index);
    return descriptor ? descriptor->rate : CodeRate::AUTO;
}

inline bool shouldSelectQAM16(float snr_db, float fading_index) {
    return selectQAM16OFDMRateDescriptor(snr_db, fading_index) != nullptr;
}

inline bool shouldSelectQPSK(float snr_db, float fading_index) {
    return selectQPSKOFDMRateDescriptor(snr_db, fading_index) != nullptr;
}

// Cap initial OFDM rate during handshake bootstrap using only chirp-era metrics.
// This avoids optimistic R2/3 starts when first post-connect OFDM quality is unknown.
inline CodeRate capInitialOFDMRate(float snr_db, float fading_index, CodeRate candidate) {
    if (const auto* descriptor =
            selectBootstrapOFDMRateDescriptor(snr_db, fading_index, candidate)) {
        return descriptor->rate;
    }
    return candidate;
}

inline CodeRate capInitialOFDMRate(float snr_db,
                                   float fading_index,
                                   CodeRate candidate,
                                   Modulation modulation) {
    // 2026-05-28 test-only: if ULTRA_FORCE_DATA_RATE is set, the operator is
    // explicitly probing a specific rung — do NOT apply the bootstrap-safety
    // demotion. Mirrors the recommendDataMode override.
    if (std::getenv("ULTRA_FORCE_DATA_RATE") != nullptr) {
        return candidate;
    }

    const auto* candidate_descriptor = ofdmCodeRateDescriptor(candidate);
    if (candidate_descriptor == nullptr) {
        return capInitialOFDMRate(snr_db, fading_index, candidate);
    }

    const auto* capped = selectBestOFDMRateDescriptor([&](const OFDMCodeRateDescriptor& descriptor) {
        if (descriptor.code_rate > candidate_descriptor->code_rate) {
            return false;
        }
        if (modulation == Modulation::QPSK &&
            descriptorAllowsQPSKOFDM(descriptor, snr_db, fading_index)) {
            return true;
        }
        return descriptorAllowsBootstrapOFDM(descriptor, snr_db, fading_index);
    });
    return capped ? capped->rate : capInitialOFDMRate(snr_db, fading_index, candidate);
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
        (fading_index < 0.15f) ? 10.0f :
        (fading_index < 0.65f) ? 12.0f :
        (fading_index < 1.10f) ? 14.0f : 18.0f;

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
    // Coherent QPSK Good-fading workhorse. Stage-1 forced Good@20 R2/3 seed1
    // (2026-05-25) cleared the QAM16 erasure seed with 0 BRAVO frame failures.
    // Keep this out of near-AWGN so the clean QAM16 ladder remains unchanged,
    // and keep Moderate+ on DQPSK until a measured QPSK floor exists there.
    if (shouldSelectQPSK(snr_db, fading_index)) {
        mod = Modulation::QPSK;
        rate = selectQPSKCodeRate(snr_db, fading_index);
        return;
    }

    // Coherent QAM16 rate ladder. Descriptor gates now keep QAM16 on the
    // clean-channel path; Good fading uses QPSK or DQPSK.
    if (shouldSelectQAM16(snr_db, fading_index)) {
        mod = Modulation::QAM16;
        rate = selectQAM16CodeRate(snr_db, fading_index);
        return;
    }

    // D8PSK R3/4 — only on near-AWGN with very high SNR. Sweep showed
    // 6 retx at SNR=20 good fading (borderline) so reserve for AWGN.
    if (fading_index < 0.15f && snr_db >= 34.0f) {
        mod = Modulation::D8PSK;
        rate = CodeRate::R3_4;
        return;
    }

    // D8PSK R2/3 — gated to AWGN-only after Mac↔Pi5 hardware A/B
    // showed the simulator's "good fading" promotion path destabilizes
    // on real audio. SNR=20 good fading auto-rate: adaptive promoted
    // to D8PSK R2/3, hit 15 retx, dropped throughput from 1595 bps
    // (forced R1/2) down to 486 bps (auto with R2/3 promotion attempt).
    // Restricting R2/3 to fading<0.15 keeps the adaptive ladder from
    // chasing R2/3 on the rougher channels where it reliably fails.
    const bool d8psk_r23_clean = (fading_index < 0.10f && snr_db >= 28.0f);
    const bool d8psk_r23_awgn  = (fading_index < 0.15f && snr_db >= 32.0f);
    if (d8psk_r23_clean || d8psk_r23_awgn) {
        mod = Modulation::D8PSK;
        rate = CodeRate::R2_3;
        return;
    }

    // D8PSK R1/2 — gated on the hardware-measured cliff. Mac↔Pi5 audio
    // loopback 10-seed sweep at SNR=20/22/24 good fading injected
    // (2026-05-04, post-CW=8 wire negotiation) showed:
    //   SNR=20 good: D8PSK retx-hit 38 % (3/8 storms incl. 270 bps)
    //                mean 1448 bps ≈ DQPSK alt 1444 bps — wash with
    //                catastrophic tail.
    //   SNR=22 good: D8PSK retx-hit 17 % (1/6 single retx, no storms)
    //                mean 1783 bps vs DQPSK 1450 bps — +23 % real win.
    //   SNR=24 good: D8PSK retx-hit 43 % (3/7 incl. 2 FAILs at 320-374 bps,
    //                17-78 retx). Counterintuitively WORSE than 22:
    //                higher SNR doesn't fix the soundcard/Doppler-induced
    //                phase glitches that cliff D8PSK; it just promotes
    //                D8PSK in more conditions where those glitches hit.
    // The single-seed CLAUDE.md datapoint (SNR=20 D8PSK 1595 bps clean)
    // was unrepresentative — variance hidden in single-seed measurements.
    // Conclusion: SNR=22 is the floor where D8PSK is net-positive.
    // Storms aren't predictable from bulk fading_index, so tightening
    // fading further doesn't help.
    if (fading_index < 0.65f && snr_db >= 32.0f) {
        mod = Modulation::D8PSK;
        rate = CodeRate::R1_2;
        return;
    }

    // Default: DQPSK with the existing wide ladder.
    mod = Modulation::DQPSK;  // Always differential for HF phase stability
    rate = selectOFDMCodeRate(snr_db, fading_index);
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
