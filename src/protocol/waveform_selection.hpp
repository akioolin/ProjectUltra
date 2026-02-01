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
// Thresholds (2026-02-01):
//   AWGN (< 0.10):         R3/4 @ SNR >= 15, R2/3 @ SNR >= 10
//   Good (0.10-0.25):      R2/3 @ SNR >= 14, R1/2 @ SNR >= 10
//   Light-mod (0.25-0.30): R1/2 @ SNR >= 12
//   Moderate (0.30-0.50):  R1/4 @ SNR >= 10 (pilots not enough, need max LDPC)
//   Heavy (>= 0.50):       R1/4 only
inline CodeRate selectOFDMCodeRate(float snr_db, float fading_index) {
    // AWGN (no fading): Highest rates
    if (fading_index < 0.10f) {
        if (snr_db >= 15.0f) return CodeRate::R3_4;
        if (snr_db >= 10.0f) return CodeRate::R2_3;
        return CodeRate::R1_4;  // Fallback
    }
    // Good fading (0.10-0.25): R2/3 or R1/2
    if (fading_index < 0.25f) {
        if (snr_db >= 14.0f) return CodeRate::R2_3;
        if (snr_db >= 10.0f) return CodeRate::R1_2;
        return CodeRate::R1_4;  // Fallback
    }
    // Light-moderate fading (0.25-0.30): R1/2
    if (fading_index < 0.30f) {
        if (snr_db >= 12.0f) return CodeRate::R1_2;
        return CodeRate::R1_4;  // Fallback
    }
    // Moderate fading (0.30-0.50): R1/4 only (pilots not reliable enough)
    if (fading_index < 0.50f) {
        return CodeRate::R1_4;
    }
    // Heavy fading (>= 0.50): R1/4 only
    return CodeRate::R1_4;
}

// Recommend waveform and rate based on SNR and fading index
//
// Key findings from testing (2026-01-30) + adaptive pilots update (2026-02-01):
// - SNR < 10 dB: MC-DPSK is most robust (~938 bps)
// - SNR >= 10 dB + AWGN: OFDM_CHIRP R2/3 or OFDM_COX (high SNR)
// - With adaptive pilots, OFDM_CHIRP can now use higher rates on fading:
//   * Good fading (0.10-0.25): R2/3 @ SNR >= 14 dB (~3000 bps)
//   * Moderate fading (0.25-0.45): R1/2 @ SNR >= 10 dB (~2300 bps)
//   * Heavy fading (>= 0.45): R1/4 (~1150 bps) or MC-DPSK
// - Poor channel or below thresholds: MC-DPSK
//
// Fading thresholds (updated for adaptive pilots):
//   < 0.10: AWGN, < 0.25: Good, < 0.45: Moderate, >= 0.45: Heavy
inline WaveformRecommendation recommendWaveformAndRate(float snr_db, float fading_index) {
    WaveformRecommendation rec;

    if (snr_db < 10.0f) {
        // Low SNR: MC-DPSK 8 carriers is most robust
        rec.waveform = WaveformMode::MC_DPSK;
        rec.rate = CodeRate::R1_4;
        rec.estimated_throughput_bps = 938.0f;
    }
    else if (fading_index < 0.10f) {
        // AWGN (no fading): Use OFDM for higher throughput
        if (snr_db >= 25.0f) {
            // Very high SNR + AWGN: OFDM_COX for maximum throughput
            // (Raised threshold - OFDM_COX needs more testing)
            rec.waveform = WaveformMode::OFDM_COX;
            rec.rate = CodeRate::R2_3;
            rec.estimated_throughput_bps = 5300.0f;
        } else if (snr_db >= 15.0f) {
            // Mid-high SNR + AWGN: OFDM_CHIRP R3/4
            rec.waveform = WaveformMode::OFDM_CHIRP;
            rec.rate = CodeRate::R3_4;
            rec.estimated_throughput_bps = 3500.0f;
        } else {
            // Mid SNR + AWGN: OFDM_CHIRP R2/3
            rec.waveform = WaveformMode::OFDM_CHIRP;
            rec.rate = CodeRate::R2_3;
            rec.estimated_throughput_bps = 3000.0f;
        }
    }
    else if (fading_index < 0.25f && snr_db >= 12.0f) {
        // Good channel (fading 0.10-0.25): OFDM_CHIRP with pilots
        if (snr_db >= 14.0f) {
            rec.waveform = WaveformMode::OFDM_CHIRP;
            rec.rate = CodeRate::R2_3;  // 6 pilots for tracking
            rec.estimated_throughput_bps = 3000.0f;
        } else {
            rec.waveform = WaveformMode::OFDM_CHIRP;
            rec.rate = CodeRate::R1_2;  // 6 pilots for tracking
            rec.estimated_throughput_bps = 2300.0f;
        }
    }
    else if (fading_index < 0.30f && snr_db >= 12.0f) {
        // Light-moderate fading (0.25-0.30): OFDM_CHIRP R1/2 with pilots
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = CodeRate::R1_2;  // 6 pilots for tracking
        rec.estimated_throughput_bps = 2300.0f;
    }
    else if (fading_index < 0.50f && snr_db >= 10.0f) {
        // Moderate fading (0.30-0.50): OFDM_CHIRP R1/4 (more robust)
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = CodeRate::R1_4;
        rec.estimated_throughput_bps = 1150.0f;
    }
    else if (fading_index < 0.60f && snr_db >= 12.0f) {
        // Heavy fading (0.50-0.60): OFDM_CHIRP R1/4 (no pilots, max LDPC)
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = CodeRate::R1_4;
        rec.estimated_throughput_bps = 1150.0f;
    }
    else {
        // Very heavy fading or low SNR: MC-DPSK
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
// Special case: D8PSK R1/2 at SNR >= 20 + AWGN (experimental, +50% throughput)
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

    // Special case: D8PSK at very high SNR + AWGN (experimental)
    if (fading_index < 0.10f && snr_db >= 20.0f) {
        mod = Modulation::D8PSK;  // 3 bits/carrier (+50% vs DQPSK)
        rate = CodeRate::R1_2;    // ~5.3 kbps with D8PSK
        return;
    }

    // Use shared helper for rate selection (SINGLE SOURCE OF TRUTH)
    rate = selectOFDMCodeRate(snr_db, fading_index);
}

} // namespace protocol
} // namespace ultra
