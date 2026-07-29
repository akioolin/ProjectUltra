#pragma once

// ============================================================================
// ULTRA_ITERATIVE_CHEST — POST-FEC DATA-AIDED CHANNEL ESTIMATION (default OFF)
// ============================================================================
//
// ⛔ WHY — RETRACTED 2026-07-29. READ THIS BEFORE USING THE RATIONALE BELOW.
//
// The original justification was: "the 2026-07-28 data-aided genie showed EVERY rung
// decodes at 98-100% given the exact channel, so the wall is CHANNEL ESTIMATION."
// **That is not established.** `ULTRA_GENIE_DATA_AIDED` sets Ĥ = Y/X
// (channel_equalizer_equalize.cpp:526) and the MMSE equalizer at :644 then computes
// conj(Ĥ)·Y/(|Ĥ|²+nv), which with Ĥ = Y/X collapses to
//     X · |Y|² / (|Y|² + nv·|X|²)
// — the transmitted symbol EXACTLY in direction, with the additive noise divided out.
// It is circular (use X to get Ĥ, use Ĥ to get X back) and is a NOISELESS SYMBOL
// ORACLE, not perfect CSI. Proof by capacity: at −6 dB in-band it decoded QPSK R3/4
// (1.5 info bits/carrier) where Shannon allows 0.32 — ~4.7x capacity, which per
// CLAUDE.md's category-error guard is a bug, not a measurement. See docs/CHANGELOG.md
// 2026-07-29 RETRACTION.
//
// The counter-evidence that was cited here is ALSO invalid (established 2026-07-29,
// same CHANGELOG entry). ULTRA_GENIE_LTS_FREEZE was described as "genuinely perfect
// full-band frequency CSI, noise intact". It is not. Its own apply-site comment states
// the validity condition — "on a NOISELESS frozen channel the stored LTS H is the exact
// true H" — and it is run at finite SNR on a fading channel, where it holds ONE noisy
// LTS snapshot across the whole frame in place of per-symbol pilot tracking that gets
// fresh pilots every symbol and averages estimation noise down. Its harm is that lost
// averaging, not a statement about perfect CSI. Staleness is excluded quantitatively:
// at ITU Good's 0.1 Hz Doppler the channel correlation over a 0.35 s frame is
// J0(2*pi*0.1*0.35) ~= 0.988. Controls (Good@20, seeds 7/11/23/42, n=100): freeze scores
// 76.8 vs 93.5 baseline at QPSK R3/4 and 27.2 vs 51.0 at 16QAM R2/3; on AWGN@25, where
// the channel is CONSTANT so freezing is exactly correct, it is byte-identical to
// baseline (19/20 both) — which proves the freeze PLUMBING is sound and localizes the
// fading harm to estimation noise.
//
// NET: BOTH oracles in this tree are invalid, so "perfect CSI does not help" was never
// measured and the estimator question is open in BOTH directions. Do not cite either
// oracle as evidence. A valid perfect-CSI bound needs a NOISELESS LTS taken in the SAME
// decode pass (see src/ofdm/genie_true_h.hpp for the design and for why the obvious
// cross-pass construction fails: H is only valid inside the FFT window it was measured
// in, so a different sync offset stamps a exp(-j*2*pi*k*delta/N) ramp across carriers).
//
// WHAT STILL JUSTIFIES THIS KNOB. Not the retracted ceiling. It stands on its own
// mechanism: after LDPC parity AND the frame CRC accept a frame, the receiver knows
// EXACTLY what was transmitted, so re-encoding + re-modulating recovers X and
// H_dd = Y/X is an exact, model-free, NOISE-LIMITED channel observation — genuinely
// more information than a pilot LS sample, and unlike the genie it is achievable.
// It is also, as measured, a NULL: it recovers ~none of the (now unquantified) gap.
// Kept default-OFF for its two findings (the FrameInterleaver blocker and the
// inter-frame common phase), not as a pending win.
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
