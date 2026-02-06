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
// Thresholds (2026-02-03) - Calibrated with temporal fading measurement:
//   AWGN (< 0.15):         R3/4 @ SNR >= 18, R2/3 @ SNR >= 14, R1/2 @ SNR >= 10
//   Near-AWGN (0.15-0.30): R2/3 @ SNR >= 20, R1/2 @ SNR >= 14
//   Good fading (0.30-0.65): R1/2 @ SNR >= 25 (tested: 75% success)
//   Moderate+ (>= 0.65):   R1/4 only (R1/2 fails even at SNR 25)
//
// Key findings from testing:
//   - R2/3 and R3/4 are NOT reliable on fading channels (only AWGN)
//   - R1/2 requires good fading (< 0.65) AND high SNR (>= 25)
//   - R1/4 is the only reliable rate for moderate fading
//
// R1/2 verified (2026-02-06):
//   AWGN SNR=20: 100% CW success, 0 retransmissions
//   Good fading SNR=15: Works but 10 retransmissions (~58% CW success) - needs SNR>=20
//   Good fading SNR=20: Testing in progress
// R3/4 is still broken (LDPC issue). R2/3 not yet re-tested.
inline CodeRate selectOFDMCodeRate(float snr_db, float fading_index) {
    // AWGN: R1/2 at SNR >= 15
    if (fading_index < 0.15f && snr_db >= 15.0f) return CodeRate::R1_2;

    // Good fading: R1/2 at SNR >= 20 (needs margin for fading dips)
    if (fading_index < 0.65f && snr_db >= 20.0f) return CodeRate::R1_2;

    // All other conditions: R1/4 (most robust)
    return CodeRate::R1_4;
}

// Recommend waveform and rate based on SNR and fading index
//
// Fading index now combines freq_cv + temporal_cv (Doppler measurement).
// Key findings from fading channel testing (2026-02-03):
// - SNR < 10 dB: MC-DPSK is most robust (~938 bps)
// - SNR >= 10 dB + true AWGN (< 0.15): OFDM_CHIRP R2/3 or R3/4
// - Fading channels are MUCH more constrained than previously thought:
//   * R2/3 and R3/4: AWGN only (fails on any fading)
//   * R1/2: Needs good fading (< 0.65) AND SNR >= 25 (only ~75% success)
//   * R1/4: Works up to moderate fading (~75% at fading_index 0.65-1.0)
// - OFDM_COX not recommended for fading (needs further testing)
//
// Calibrated fading thresholds (2026-02-03):
//   < 0.15: True AWGN, < 0.30: Near-AWGN, < 0.65: Good, >= 0.65: Moderate+
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
        rec.estimated_throughput_bps = (rec.rate == CodeRate::R1_2) ? 2300.0f : 1150.0f;
    }
    else if (fading_index < 1.10f && snr_db >= 12.0f) {
        // Good-to-moderate fading: OFDM_CHIRP
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = selectOFDMCodeRate(snr_db, fading_index);
        rec.estimated_throughput_bps = (rec.rate == CodeRate::R1_2) ? 2300.0f : 1150.0f;
    }
    else if (snr_db >= 11.0f) {
        // Heavy fading or borderline SNR: OFDM_CHIRP R1/4 still viable
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = selectOFDMCodeRate(snr_db, fading_index);
        rec.estimated_throughput_bps = (rec.rate == CodeRate::R1_2) ? 2300.0f : 1150.0f;
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
