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
// Thresholds (2026-02-10) - Full rate ladder:
//   AWGN only (< 0.15):             R3/4 @ SNR >= 20 (10/10 seeds, 0 retx)
//   Good fading or better (< 0.65): R2/3 @ SNR >= 20 (30/30 seeds, 0 retx)
//   Good fading or better (< 0.65): R1/2 @ SNR >= 15 (5/5 seeds, 0 retx)
//   Moderate+ (>= 0.65):            R1/4 only
//
// R3/4 verified (2026-02-10):
//   DQPSK R3/4 AWGN SNR=20: 10/10 seeds PASS, 0 retransmissions
//   DQPSK R3/4 Good fading: FAILS (23 retx / 5 seeds) — AWGN only!
//   Payload: 243 bytes/frame — 23% gain over R2/3
// R2/3 verified (2026-02-10):
//   DQPSK R2/3 Good fading SNR=20: 30/30 seeds PASS, 0 retransmissions
//   Payload: 197 bytes/frame — 40% gain over R1/2
// R1/2 verified (2026-02-10):
//   DQPSK R1/2 Good fading SNR=15: 5/5 seeds PASS, 0 retransmissions
inline CodeRate selectOFDMCodeRate(float snr_db, float fading_index) {
    // AWGN only: R3/4 at SNR >= 20 (too many retx on fading)
    if (fading_index < 0.15f && snr_db >= 20.0f) return CodeRate::R3_4;

    // Good fading or better: R2/3 at SNR >= 20
    if (fading_index < 0.65f && snr_db >= 20.0f) return CodeRate::R2_3;

    // Good fading or better: R1/2 at SNR >= 15
    if (fading_index < 0.65f && snr_db >= 15.0f) return CodeRate::R1_2;

    // All other conditions: R1/4 (most robust)
    return CodeRate::R1_4;
}

// Recommend waveform and rate based on SNR and fading index
//
// Fading index now combines freq_cv + temporal_cv (Doppler measurement).
// Key findings from testing (2026-02-10):
// - SNR < 10 dB: MC-DPSK is most robust (~938 bps)
// - SNR >= 20 dB + AWGN: OFDM_CHIRP R3/4 (~3900 bps)
// - SNR >= 20 dB + good fading: OFDM_CHIRP R2/3 (~3200 bps)
// - SNR >= 15 dB + good fading: OFDM_CHIRP R1/2 (~2300 bps)
// - Moderate+ fading (>= 0.65): R1/4 only (~1150 bps)
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
    else if (fading_index < 1.10f && snr_db >= 12.0f) {
        // Good-to-moderate fading: OFDM_CHIRP
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = selectOFDMCodeRate(snr_db, fading_index);
        rec.estimated_throughput_bps = (rec.rate == CodeRate::R3_4) ? 3900.0f :
                                       (rec.rate == CodeRate::R2_3) ? 3200.0f :
                                       (rec.rate == CodeRate::R1_2) ? 2300.0f : 1150.0f;
    }
    else if (snr_db >= 11.0f) {
        // Heavy fading or borderline SNR: OFDM_CHIRP R1/4 still viable
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = selectOFDMCodeRate(snr_db, fading_index);
        rec.estimated_throughput_bps = (rec.rate == CodeRate::R3_4) ? 3900.0f :
                                       (rec.rate == CodeRate::R2_3) ? 3200.0f :
                                       (rec.rate == CodeRate::R1_2) ? 2300.0f : 1150.0f;
    }
    else {
        // Very heavy fading or low SNR: MC-DPSK
        // SNR < 11 with fading needs MC-DPSK robustness
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
