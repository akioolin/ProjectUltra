# Coherent Ladder Adaptive Selector — Design (P5, DESIGN ONLY — NOT wired live)

**Status: design + test spec only. No shipping-path code changed. Live wiring
into `recommendDataMode()` / `selectOFDMCodeRate()` is deferred to a
user-supervised session (it is a production-path change).**

Branch: `feat/16qam-promotion-2026-05-21`. Inputs: the committed coherent 8PSK
rung (`034c1c1`) and the empirical floor map
(`783c3fd`, `docs/COHERENT_LADDER_FLOOR_MAP_2026_05_22.md`).

## Purpose

Turn the measured Good-fading floor map into a concrete, testable selector that
chooses an OFDM coherent rung (or the differential floor) from channel state,
keeping differential DQPSK as the robustness floor below the coherent rungs.

## Inputs the selector consumes (already available in the engine)

- In-band SNR estimate (receiver, 3 kHz noise BW) — same source the rate
  selector already uses (`IDLE_IN_BAND` / `OFDM_BROADBAND`).
- Fading index (`getFadingIndex()`, combines freq_cv + temporal_cv) — to gate
  "Good" vs Moderate/Poor.
- Recent ARQ health: retransmission rate and timeout rate over a sliding window
  of recent frames (the floor map shows these — not SNR alone — separate the
  rungs; e.g. QAM16 SNR17 mean is high but its timeout/variance is what makes
  8PSK the safer pick).

## Recommended Good-fading rung policy (from the floor map, with hysteresis)

This is a *policy table*, applied with hysteresis and recent-frame statistics —
NOT an instantaneous SNR threshold (instantaneous SNR is too noisy on fading).

| Condition (Good fading) | Rung |
|---|---|
| SNR ≥ ~18 dB AND recent retx/timeout low | coherent QAM16 R1/2 (highest long-transfer ceiling) |
| SNR ~15-17 dB, OR rising timeout/variance at higher SNR | coherent 8PSK R1/2 (lower variance, fewer control failures) |
| SNR ~14 dB, coherent still required | coherent 8PSK R1/2 (best robust coherent default) |
| < ~14 dB, or sustained 8PSK frame/timeout failures | differential DQPSK floor (coherent QPSK R1/2 is NOT viable — cascade-crippled, 460 bps on Good even at SNR20) |

Notes baked in from the data:
- R2/3 is not a default rung (worse frame-success/timeout than R1/2; QAM16 R2/3
  not viable on Good at these SNRs).
- QPSK R1/2 must NOT be selected on Good fading — it delivers ~460 bps due to
  ARQ-cascade airtime even at SNR20. The floor below 8PSK is differential DQPSK.
- The "rising timeout/variance ⇒ prefer 8PSK over QAM16 even when QAM16 mean is
  higher" rule is the key robustness lever: QAM16 is spiky (5KB Good multiseed
  spread ~1000 bps) while 8PSK is tight (~250-500 bps).

## Hysteresis / dwell (proposed, to be tuned)

- Require N consecutive frames (proposal: 3-5) of the trigger condition before
  switching rung, to avoid thrashing on per-frame SNR noise.
- Downshift (to a more robust rung) faster than upshift: 1-2 bad frames trigger
  down; several good frames trigger up. Standard adaptive-modulation asymmetry.
- A mode change costs a renegotiation; account for that turnaround in the
  switch cost (don't switch for a transient).

## Integration points (where it WOULD wire — NOT wired here)

- `src/protocol/waveform_selection.hpp::selectOFDMCodeRate()` — single source of
  truth for OFDM code-rate. The coherent-mod choice (QAM16/8PSK) would extend
  this or sit beside it.
- `recommendDataMode()` (per the program notes; confirm exact location at wiring
  time) — the mode/rung negotiation entry point.
- Wideband entry floors in `src/protocol/connection_policy.hpp` (AWGN 10 / Good
  12 / Moderate 14 / Poor 18) — the coherent rungs sit above the differential
  floor and must not lower those entry floors.

## ctest fixture spec (to add WITH the live wiring, not before)

Lock the policy decisions as pure-function unit tests on the selector (no
cli_simulator needed for the decision logic):

- `selectCoherentRung(snr=20, fading=Good, retxRate=low) == QAM16_R1_2`
- `selectCoherentRung(snr=16, fading=Good, *) == 8PSK_R1_2`
- `selectCoherentRung(snr=14, fading=Good, *) == 8PSK_R1_2`
- `selectCoherentRung(snr=18, fading=Good, retxRate=high) == 8PSK_R1_2`  (variance/timeout override)
- `selectCoherentRung(snr=12, fading=Good, *) == DQPSK_FLOOR`  (never coherent QPSK on Good)
- Hysteresis: a single bad frame at SNR20 does NOT immediately downshift; N
  consecutive do.
- Guard (unchanged): the existing DQPSK floor path
  `good/snr12/1KB/seed42 == DQPSK R1/4, 391/20/4/4` must remain byte-identical
  (the selector must not alter the sub-coherent-floor path).

## Open questions for user review (before live wiring)

1. Does the airtime-scaled ACK-timeout fix (P1, currently coherent-path-gated)
   want to also apply to the differential DQPSK path? It *improved* DQPSK to
   411/20/3/2 in testing but was held back to keep the guard byte-identical
   unsupervised. (See journal "DQPSK-airtime-timeout decision item".)
2. Floor map is Good-fading + 5KB only (plus 20KB spot checks). Moderate/Poor
   fading and 20KB-primary maps are needed before the selector ships for those
   channels.
3. Several low-SNR Good cells (and all QPSK low-SNR cells) were timeout-capped
   in the sweep; the < ~14 dB region of the map is approximate. A higher
   per-cell timeout (or smaller files) would firm it up.
4. P2 (unify per-mod coherent-fading gating to `isCoherentHighOrder()`) is a
   prerequisite cleanliness refactor so QAM8/QAM16/QAM32 share one gated
   coherent-fading path — deferred as risky (shared-decode regression potential).

## What is NOT done here

- No `recommendDataMode` / `selectOFDMCodeRate` edits (production path).
- No selector implementation. This is the spec the implementation should follow.
- No QAM32 ladder rung (only AWGN decode-sanity confirmed; no fading floor).
