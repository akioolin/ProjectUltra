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
    // AWGN only: R3/4 at SNR >= 20 (too many retx on fading)
    if (fading_index < 0.15f && snr_db >= 20.0f) return CodeRate::R3_4;

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
// Key findings from testing (2026-02-11):
// - SNR < 10 dB: MC-DPSK is most robust (~938 bps)
// - SNR >= 20 dB + AWGN: OFDM_CHIRP R3/4 (~3900 bps)
// - SNR >= 20 dB + good fading: OFDM_CHIRP R2/3 (~3200 bps)
// - SNR >= 15 dB + good/moderate fading: OFDM_CHIRP R1/2 (~2300 bps)
// - SNR >= 10 dB + good/moderate fading: OFDM_CHIRP R1/4 (~1150 bps, 30/30 seeds)
// - Heavy+ fading (>= 1.10): R1/4 only (~1150 bps)
//
// Calibrated fading thresholds:
//   < 0.15: True AWGN, < 0.65: Good, >= 0.65: Moderate+
inline WaveformRecommendation recommendWaveformAndRate(float snr_db, float fading_index) {
    WaveformRecommendation rec;

    if (snr_db < 10.0f) {
        // Low SNR: MC-DPSK 8 carriers is most robust
        rec.waveform = WaveformMode::MC_DPSK;
        rec.rate = CodeRate::R1_4;
        rec.estimated_throughput_bps = 938.0f;
    }
    else if (fading_index < 0.15f) {
        // True AWGN (no fading)
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = selectOFDMCodeRate(snr_db, fading_index);
        rec.estimated_throughput_bps = (rec.rate == CodeRate::R3_4) ? 3900.0f :
                                       (rec.rate == CodeRate::R2_3) ? 3200.0f :
                                       (rec.rate == CodeRate::R1_2) ? 2300.0f : 1150.0f;
    }
    else if (fading_index < 1.10f && snr_db >= 10.0f) {
        // Good-to-moderate fading: OFDM_CHIRP (30/30 seeds at SNR=10)
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = selectOFDMCodeRate(snr_db, fading_index);
        rec.estimated_throughput_bps = (rec.rate == CodeRate::R3_4) ? 3900.0f :
                                       (rec.rate == CodeRate::R2_3) ? 3200.0f :
                                       (rec.rate == CodeRate::R1_2) ? 2300.0f : 1150.0f;
    }
    else if (snr_db >= 10.0f) {
        // Heavy fading at SNR >= 10: OFDM_CHIRP R1/4 still viable
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = selectOFDMCodeRate(snr_db, fading_index);
        rec.estimated_throughput_bps = (rec.rate == CodeRate::R3_4) ? 3900.0f :
                                       (rec.rate == CodeRate::R2_3) ? 3200.0f :
                                       (rec.rate == CodeRate::R1_2) ? 2300.0f : 1150.0f;
    }
    else {
        // Very heavy fading or low SNR: MC-DPSK
        // SNR < 10 with fading needs MC-DPSK robustness
        rec.waveform = WaveformMode::MC_DPSK;
        rec.rate = CodeRate::R1_4;
        rec.estimated_throughput_bps = 938.0f;
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

    // OFDM modes: use shared rate selection helper
    mod = Modulation::DQPSK;  // Always differential for HF phase stability

    // TEMPORARY: D8PSK disabled until R1/2+ rates are verified
    // D8PSK only on TRUE AWGN (fading_index < 0.15) + very high SNR
    // Testing showed D8PSK fails on any fading - too sensitive to phase errors
    // if (fading_index < 0.15f && snr_db >= 25.0f) {
    //     mod = Modulation::D8PSK;
    //     rate = CodeRate::R1_2;
    //     return;
    // }

    // Use shared helper for rate selection (SINGLE SOURCE OF TRUTH)
    rate = selectOFDMCodeRate(snr_db, fading_index);
}

} // namespace protocol
} // namespace ultra
