// waveform_selection.hpp - Waveform and rate selection algorithm
//
// Centralized algorithm for selecting waveform mode and code rate
// based on SNR and fading index. Used by both protocol negotiation
// and ModemEngine.
//
// Based on testing with CFO=20Hz across AWGN/good/moderate channels (2026-01-29)

#pragma once

#include "protocol/frame_v2.hpp"  // WaveformMode
#include "ultra/types.hpp"        // CodeRate

namespace ultra {
namespace protocol {

// Waveform + rate recommendation
struct WaveformRecommendation {
    WaveformMode waveform;
    CodeRate rate;
    float estimated_throughput_bps;
};

// Shared helper: Select code rate for OFDM modes based on SNR and fading
// This is the SINGLE SOURCE OF TRUTH for rate selection thresholds.
// Both recommendWaveformAndRate() and recommendDataMode() use this.
//
// Fading index now combines freq_cv + temporal_cv (Doppler measurement).
// Thresholds (2026-03-15) - Full rate ladder:
//   AWGN only (< 0.15):             R3/4 @ SNR >= 20 (10/10 seeds, 0 retx)
//   Near-AWGN (< 0.15):             R2/3 @ SNR >= 15
//   Good fading (< 0.65):           R1/2 @ SNR >= 15 (5/5 seeds, 14% retx)
//   Moderate fading (< 1.10):       R1/2 @ SNR >= 15 (100% delivery, 52% retx)
//   Heavy+ (>= 1.10):              R1/4 only
//
// R3/4 verified (2026-02-10):
//   DQPSK R3/4 AWGN SNR=20: 10/10 seeds PASS, 0 retransmissions
//   DQPSK R3/4 Good fading: FAILS (23 retx / 5 seeds) — AWGN only!
// R2/3 verified (2026-03-15, 802.11n LDPC + CPE correction):
//   10KB file transfer good fading SNR=15: 1485 bps, 33% retx
//   10KB file transfer AWGN SNR=15: near-ideal (low retx)
//   Demoted from good fading: R1/2 gives similar throughput (1418 bps)
//   with half the retransmissions (14% vs 33%) — more reliable.
// R1/2 verified (2026-03-15, 802.11n LDPC):
//   10KB file transfer good fading SNR=15: 1418 bps, 14% retx, 100% frame success
//   10KB file transfer moderate fading SNR=15: 1055 bps, 52% retx, 99% frame success
//   10KB file transfer AWGN SNR=15: 1636 bps, 3% retx, 100% frame success
inline CodeRate selectOFDMCodeRate(float snr_db, float fading_index) {
    // AWGN only: R3/4 at SNR >= 15 (Item 3 hw calibration 2026-05-07:
    //   5/5 seeds 20KB AWGN SNR=15 forced R3/4: 2670-2691 bps, 0 retx,
    //   +18% vs auto-R2/3. Tighter fading gate (< 0.10) protects against
    //   borderline-fading misclassifications since R3/4 is documented to
    //   FAIL on Good fading.) Previous gate was SNR >= 20 + fading < 0.15.
    if (fading_index < 0.10f && snr_db >= 15.0f) return CodeRate::R3_4;

    // Near-AWGN: R2/3 at SNR >= 15 (too many retx on real fading channels)
    if (fading_index < 0.15f && snr_db >= 15.0f) return CodeRate::R2_3;

    // Good-to-moderate fading: R1/2 at SNR >= 15
    if (fading_index < 1.10f && snr_db >= 15.0f) return CodeRate::R1_2;

    // All other conditions: R1/4 (most robust)
    return CodeRate::R1_4;
}

// Cap initial OFDM rate during handshake bootstrap using only chirp-era metrics.
// This avoids optimistic R2/3 starts when first post-connect OFDM quality is unknown.
inline CodeRate capInitialOFDMRate(float snr_db, float fading_index, CodeRate candidate) {
    if (candidate == CodeRate::R3_4) {
        // Keep R3/4 for near-ideal channels only.
        if (fading_index >= 0.05f || snr_db < 24.0f) {
            return CodeRate::R2_3;
        }
        return candidate;
    }

    if (candidate == CodeRate::R2_3) {
        // R2/3 is now AWGN-only (fading < 0.15). At bootstrap, chirp-era fading
        // can read slightly high, so cap to R1/2 if any fading detected.
        if (fading_index >= 0.10f || snr_db < 15.0f) {
            return CodeRate::R1_2;
        }
    }

    return candidate;
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
// - SNR < 10 dB: MC-DPSK is most robust (~375 bps raw at 8 car DQPSK R1/4)
// - SNR >= 20 dB + AWGN: OFDM_CHIRP R3/4 (~3438 bps raw)
// - SNR >= 20 dB + good fading: OFDM_CHIRP R2/3 (~2944 bps raw)
// - SNR >= 15 dB + good/moderate fading: OFDM_CHIRP R1/2 (~2208 bps raw)
// - SNR >= 10 dB + good/moderate fading: OFDM_CHIRP R1/4 (~1104 bps raw, 30/30 seeds)
// - Heavy+ fading (>= 1.10): R1/4 only (~1104 bps raw)
//
// Calibrated fading thresholds:
//   < 0.15: True AWGN, < 0.65: Good, >= 0.65: Moderate+
inline WaveformRecommendation recommendWaveformAndRate(float snr_db, float fading_index) {
    WaveformRecommendation rec;

    if (snr_db < 10.0f) {
        // Low SNR: MC-DPSK 8 carriers is most robust
        rec.waveform = WaveformMode::MC_DPSK;
        rec.rate = CodeRate::R1_4;
        rec.estimated_throughput_bps = 375.0f;
    }
    else if (fading_index < 0.15f) {
        // True AWGN (no fading)
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = selectOFDMCodeRate(snr_db, fading_index);
        rec.estimated_throughput_bps = (rec.rate == CodeRate::R3_4) ? 3438.0f :
                                       (rec.rate == CodeRate::R2_3) ? 2944.0f :
                                       (rec.rate == CodeRate::R1_2) ? 2208.0f : 1104.0f;
    }
    else if (fading_index < 1.10f && snr_db >= 10.0f) {
        // Good-to-moderate fading: OFDM_CHIRP (30/30 seeds at SNR=10)
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = selectOFDMCodeRate(snr_db, fading_index);
        rec.estimated_throughput_bps = (rec.rate == CodeRate::R3_4) ? 3438.0f :
                                       (rec.rate == CodeRate::R2_3) ? 2944.0f :
                                       (rec.rate == CodeRate::R1_2) ? 2208.0f : 1104.0f;
    }
    else if (snr_db >= 10.0f) {
        // Heavy fading at SNR >= 10: OFDM_CHIRP R1/4 still viable
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = selectOFDMCodeRate(snr_db, fading_index);
        rec.estimated_throughput_bps = (rec.rate == CodeRate::R3_4) ? 3438.0f :
                                       (rec.rate == CodeRate::R2_3) ? 2944.0f :
                                       (rec.rate == CodeRate::R1_2) ? 2208.0f : 1104.0f;
    }
    else {
        // Very heavy fading or low SNR: MC-DPSK
        // SNR < 10 with fading needs MC-DPSK robustness
        rec.waveform = WaveformMode::MC_DPSK;
        rec.rate = CodeRate::R1_4;
        rec.estimated_throughput_bps = 375.0f;
    }

    return rec;
}

// Recommend modulation and code rate for data mode within an established connection
// This is used after waveform negotiation to set the data transmission parameters.
// Uses selectOFDMCodeRate() for rate selection to stay consistent with recommendWaveformAndRate().
//
// For OFDM modes: DQPSK with rate from selectOFDMCodeRate()
// D8PSK only on true AWGN (< 0.05) + SNR >= 25 - too sensitive for any fading
// For MC-DPSK: Always DQPSK R1/4
//
inline void recommendDataMode(float snr_db, WaveformMode waveform,
                               Modulation& mod, CodeRate& rate, float fading_index = 0.0f) {
    // MC-DPSK always uses DQPSK R1/4 for robustness
    if (waveform == WaveformMode::MC_DPSK) {
        mod = Modulation::DQPSK;
        rate = CodeRate::R1_4;
        return;
    }

    // OFDM_NARROW: DQPSK only, R1/4 default, R1/2 for clean-enough
    // conditions. Sweeps in cli_simulator (2026-05-03) verified R1/2
    // narrow + window=3 passes 7-message test at:
    //   SNR=8  AWGN          (PASS)
    //   SNR=10 good fading   (PASS)
    //   SNR=12 good fading   (PASS)
    //   SNR=15 good fading   (PASS)
    // and R1/4 narrow + window=3 passes:
    //   SNR=8  good fading   (PASS — documented baseline)
    //   SNR=8  moderate      (PASS, slow but recovers)
    // The hard floor (SNR=8 good fading R1/4) is preserved by the
    // SNR>=10 gate on good fading. AWGN keeps the SNR>=8 trigger
    // because near-AWGN is a much easier channel.
    if (waveform == WaveformMode::OFDM_NARROW) {
        mod = Modulation::DQPSK;
        const bool awgn_path = fading_index < 0.15f && snr_db >= 8.0f;
        const bool good_path = fading_index < 0.65f && snr_db >= 10.0f;
        if (awgn_path || good_path) {
            rate = CodeRate::R1_2;
        } else {
            rate = CodeRate::R1_4;
        }
        return;
    }

    // OFDM modes: D8PSK gated on conditions, otherwise DQPSK.
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
    // D8PSK R3/4 — only on near-AWGN with very high SNR. Sweep showed
    // 6 retx at SNR=20 good fading (borderline) so reserve for AWGN.
    if (fading_index < 0.15f && snr_db >= 24.0f) {
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
    const bool d8psk_r23_clean = (fading_index < 0.10f && snr_db >= 18.0f);
    const bool d8psk_r23_awgn  = (fading_index < 0.15f && snr_db >= 22.0f);
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
    if (fading_index < 0.65f && snr_db >= 22.0f) {
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
    case WaveformMode::OFDM_COX:
        return 4000;
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
