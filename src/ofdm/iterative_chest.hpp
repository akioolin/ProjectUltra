#pragma once

// ============================================================================
// ULTRA_ITERATIVE_CHEST — POST-FEC DATA-AIDED CHANNEL ESTIMATION (default OFF)
// ============================================================================
//
// WHY. The 2026-07-28 data-aided genie (H[k] = Y[k]/X_TRUE[k]) showed that EVERY
// rung at EVERY SNR decodes at 98-100% given the exact channel, while production
// reads 93.5 (QPSK R3/4 @20), 82.3 (8PSK R2/3 @20), 51.0 (16QAM R2/3 @20), 27.8
// (16QAM R3/4 @20). The wall is CHANNEL ESTIMATION, not the channel, the demap or
// the LDPC. The genie cheats by knowing X. This knob buys the achievable part of
// that: after LDPC parity AND the frame CRC accept a frame, the receiver knows
// EXACTLY what was transmitted, so re-encoding + re-modulating recovers X and
// H_dd = Y/X becomes an exact (noise-limited, model-free) channel observation.
//
// WHAT IT IS NOT. This is NOT the existing pre-FEC decision-directed tracker
// (`dd_qam16_*`, ULTRA_COHERENT_DD_OFF). That one hard-decides X from Y/H_hat, so
// its errors are CORRELATED with the estimate it feeds — a positive-feedback loop,
// measured FLAT on this exact problem (31/150 vs 31 baseline, 2026-05-29) and,
// per BUG-8PSK-001, actively harmful on 8PSK. Post-FEC X is verified, so H_dd's
// error is statistically independent of H_hat. Different estimator, not a better
// decision device. Nothing here touches that path.
//
// WHY IT IS CARRY-FORWARD AND NOT AN INTRA-FRAME SECOND PASS. FrameInterleaver's
// permutation is `interleaved_idx = bit*CW + (cw+bit) % CW` (frame_interleaver.cpp:46),
// so any CW consecutive air bits carry one bit from EACH codeword. A 16QAM carrier
// consumes 4 consecutive air bits at cw_count=4 => it draws from ALL FOUR codewords.
// Therefore a PARTIALLY decoded frame determines the constellation point of exactly
// ZERO carriers (QPSK 2/4 -> 0.50 of carriers, 8PSK 0.375, 16QAM 0.000), and a FULLY
// decoded frame needs no help. Re-decoding a frame with its own bits is a structural
// no-op at the target rung. The value is entirely in TRANSFERRING a decoded frame's
// exact channel to a frame that has not decoded yet — i.e. across frames of a burst
// group, on an ABSOLUTE time axis, through the Wiener that already models rho(dt).
// That also makes this cheap: no second equalize pass, no second LDPC pass.
//
// ADAPTIVITY (CLAUDE.md, non-negotiable). The only modulation-dependent quantity in
// the whole path is the observation noise weight, and it is DERIVED, not branched:
//   H_dd[k] = Y[k]/X[k] = H[k] + N[k]/X[k]  =>  noise scales by 1/|X[k]|^2.
// Every constellation in mapBits() is unit-AVERAGE-power, so
//   noise_norm_dd[k] = wiener_noise_norm * E[|X|^2]/|X[k]|^2 = wiener_noise_norm/|X[k]|^2
// which is exactly 1x for QPSK/8PSK (unit modulus), +7.0 dB for the 16QAM inner
// points (|X|^2 = 0.2) and -2.6 dB for its corners, and is correct for 32/64/256-QAM
// without being told they exist. No kQam16* constant, no `if (mod == X)`.
//
// KNOB DISCIPLINE. Deliberately NOT a function-local `static const`: that latches
// getenv on first call and makes the knob untestable in-process. This is read ONCE
// PER FRAME (never per symbol or per carrier), so getenv is not a hot-path hazard.

#include <cstdlib>

namespace ultra {
namespace ofdm {

// Post-FEC data-aided channel estimation. Default OFF (production decode path).
inline bool iterativeChestEnabled() {
    const char* e = std::getenv("ULTRA_ITERATIVE_CHEST");
    return e != nullptr && e[0] == '1';
}

}  // namespace ofdm
}  // namespace ultra
