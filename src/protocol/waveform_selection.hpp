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

// Recommend waveform and rate based on SNR and fading index
//
// Key findings from testing (2026-01-29, CFO=20Hz):
// - SNR < 10 dB: MC-DPSK is most robust (~938 bps)
// - SNR >= 10 dB: Fading index determines optimal mode
//   - fading < 0.1 (AWGN): OFDM_CHIRP R2/3 works (~2.3 kbps)
//   - fading >= 0.1: MC-DPSK is more reliable than OFDM on fading channels
// - SNR >= 17 dB + no fading: OFDM_COX for highest throughput
//
// Note: Original testing suggested OFDM_CHIRP R1/2 for fading 0.1-0.35, but
// verification showed MC-DPSK (938 bps) outperforms OFDM on "good" channel
// (fading ~0.32). OFDM only reliable on AWGN-like channels (fading < 0.1).
inline WaveformRecommendation recommendWaveformAndRate(float snr_db, float fading_index) {
    WaveformRecommendation rec;

    if (snr_db < 10.0f) {
        // Low SNR: MC-DPSK 8 carriers is most robust
        rec.waveform = WaveformMode::MC_DPSK;
        rec.rate = CodeRate::R1_4;
        rec.estimated_throughput_bps = 938.0f;
    }
    else if (fading_index < 0.1f) {
        // No fading (AWGN-like): Use OFDM for higher throughput
        if (snr_db >= 17.0f) {
            // High SNR + no fading: OFDM_COX for maximum throughput
            rec.waveform = WaveformMode::OFDM_COX;
            rec.rate = CodeRate::R2_3;
            rec.estimated_throughput_bps = 5300.0f;
        } else {
            // Mid SNR + no fading: OFDM_CHIRP R2/3
            rec.waveform = WaveformMode::OFDM_CHIRP;
            rec.rate = CodeRate::R2_3;
            rec.estimated_throughput_bps = 2300.0f;
        }
    }
    else {
        // Any fading (>= 0.1): MC-DPSK is more reliable than OFDM
        // Testing showed OFDM_CHIRP fails on "good" channel (fading ~0.32)
        rec.waveform = WaveformMode::MC_DPSK;
        rec.rate = CodeRate::R1_4;
        rec.estimated_throughput_bps = 938.0f;
    }

    return rec;
}

// Recommend modulation and code rate for data mode within an established connection
// This is used after waveform negotiation to set the data transmission parameters.
// Should be consistent with recommendWaveformAndRate() thresholds.
//
// For OFDM modes (OFDM_CHIRP, OFDM_COX):
//   - Always use DQPSK (differential for phase stability)
//   - Code rate based on SNR
//
// For MC-DPSK:
//   - Always DQPSK R1/4 (fixed for robustness)
//
inline void recommendDataMode(float snr_db, WaveformMode waveform,
                               Modulation& mod, CodeRate& rate) {
    // MC-DPSK always uses DQPSK R1/4 for robustness
    if (waveform == WaveformMode::MC_DPSK) {
        mod = Modulation::DQPSK;
        rate = CodeRate::R1_4;
        return;
    }

    // OFDM modes: DQPSK with SNR-based code rate
    // Thresholds aligned with recommendWaveformAndRate()
    mod = Modulation::DQPSK;

    if (snr_db >= 25.0f) {
        rate = CodeRate::R3_4;   // High throughput
    } else if (snr_db >= 17.0f) {
        rate = CodeRate::R2_3;   // Good balance (matches OFDM_COX threshold)
    } else if (snr_db >= 10.0f) {
        rate = CodeRate::R1_2;   // More robust (matches OFDM_CHIRP threshold)
    } else {
        rate = CodeRate::R1_4;   // Maximum robustness
    }
}

} // namespace protocol
} // namespace ultra
