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
// Key findings from testing (2026-01-30):
// - SNR < 10 dB: MC-DPSK is most robust (~938 bps)
// - SNR >= 10 dB + AWGN: OFDM_CHIRP R2/3 or OFDM_COX (high SNR)
// - SNR >= 15 dB + Good (fading 0.1-0.35): OFDM_CHIRP R1/4 (100%)
// - SNR >= 16 dB + Moderate (fading 0.35-0.55): OFDM_CHIRP R1/4 (100%)
// - Poor channel or below thresholds: MC-DPSK
//
// Fading thresholds match fadingToQuality() in GUI:
//   < 0.10: AWGN, < 0.35: Good, < 0.55: Moderate, >= 0.55: Poor
inline WaveformRecommendation recommendWaveformAndRate(float snr_db, float fading_index) {
    WaveformRecommendation rec;

    if (snr_db < 10.0f) {
        // Low SNR: MC-DPSK 8 carriers is most robust
        rec.waveform = WaveformMode::MC_DPSK;
        rec.rate = CodeRate::R1_4;
        rec.estimated_throughput_bps = 938.0f;
    }
    else if (fading_index < 0.1f) {
        // AWGN (no fading): Use OFDM for higher throughput
        if (snr_db >= 17.0f) {
            // High SNR + AWGN: OFDM_COX for maximum throughput
            rec.waveform = WaveformMode::OFDM_COX;
            rec.rate = CodeRate::R2_3;
            rec.estimated_throughput_bps = 5300.0f;
        } else {
            // Mid SNR + AWGN: OFDM_CHIRP R2/3
            rec.waveform = WaveformMode::OFDM_CHIRP;
            rec.rate = CodeRate::R2_3;
            rec.estimated_throughput_bps = 2300.0f;
        }
    }
    else if (fading_index < 0.35f && snr_db >= 15.0f) {
        // Good channel (fading 0.1-0.35) + SNR >= 15: OFDM_CHIRP R1/4
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = CodeRate::R1_4;
        rec.estimated_throughput_bps = 1150.0f;
    }
    else if (fading_index < 0.55f && snr_db >= 16.0f) {
        // Moderate channel (fading 0.35-0.55) + SNR >= 16: OFDM_CHIRP R1/4
        rec.waveform = WaveformMode::OFDM_CHIRP;
        rec.rate = CodeRate::R1_4;
        rec.estimated_throughput_bps = 1150.0f;
    }
    else {
        // Poor channel or below SNR thresholds: MC-DPSK
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
