# RX-AUTHORITY PREDICTIVE — per-carrier virtual rung evaluation (design brief)

**Date:** 2026-07-07 · **Status:** implementing (this session) · **Owner:** receiver side only — zero wire changes, zero sender changes.

## 1. Problem

The receiver authority's verdict quality is information-starved in two ways, and
every stabilizer added on 2026-07-06 (one-rung climbs, post-crater dwell,
penalty escalation) is a workaround for exactly this starvation:

1. **Survivor bias.** Observations (scalar SNR + fading) enter the ring only
   from DELIVERED groups. Failed groups contribute nothing — so the ring rides
   fade crests (26–32 dB reads on a 20 dB channel, F149/F160) and every fixed
   haircut eventually re-clears after a demote.
2. **Scalar blindness.** The verdict maps ONE number (fade-averaged mean SNR)
   through a scalar anchor table. At MPG@20 the mean says 24 dB while a parked
   notch has 20 % of carriers unusable — the scalar map cannot see the
   difference between "24 dB flat" (16QAM R2/3 flies) and "24 dB with a notch"
   (16QAM R2/3 craters, measured 43–51 % FER). Hence climbs had to be one rung,
   each proven by risking a group on it: QPSK R2/3 → 16QAM R2/3 costs ~4
   switches and 40–80 s of laddering (F163 P5: 78 s).

## 2. Key physics fact

**The channel measurement is constellation-independent.** Every ANCHORED group
— delivered or cratered — yields the full per-carrier picture: the equalizer
computes |H_k|² per data carrier and the frame noise variance σ² regardless of
what the data symbols carry. γ_k = |H_k|²/σ² for all ~51 carriers is available
on every group we sync to, today, and is discarded.

With {γ_k} in hand the receiver can evaluate ANY rung's decodability on the
channel **as measured**, without transmitting at it — the principled form of
the "constant monitoring" commercial HF modems are believed to do.

## 3. Predictor (EESM, self-calibrated from our own anchor table)

For rung r = (modulation m with b bits/symbol, code rate c), define the
per-carrier symbol-capacity fraction with the standard EESM exponential:

```
f_r(γ) = 1 − exp(−γ / α_r)
```

and declare r sustainable on a snapshot {γ_k} iff

```
mean_k f_r(γ_k) ≥ c            (capacity fraction meets the code rate)
```

**Self-calibration (no new tuned constants):** on a FLAT channel at r's AWGN
anchor A_r (linear), the rung by definition JUST decodes — so α_r is fixed by
the identity `1 − exp(−A_r/α_r) = c`:

```
α_r = −A_r / ln(1 − c)
```

Properties:
- Flat channel ⇒ reproduces the anchor table EXACTLY (by construction). Zero
  behavior change on AWGN.
- Selective channel ⇒ strong carriers' surplus compensates weak ones up to the
  code budget — which is literally what LDPC + the channel interleaver do —
  and a parked notch that exceeds the budget FAILS the prediction even when
  the mean SNR looks generous. The 2026-07-06 negative LLR result ("16QAM R2/3
  physics-limited at MPG@20") becomes *visible to the controller* instead of
  being rediscovered by cratering.
- Uses the **AWGN column only**: the Good/Moderate columns are scalar-fading
  margins, which the per-carrier measurement replaces on this path. (The
  fallback path keeps using the fading columns unchanged.)

**Scale anchoring:** the demod's γ_k lives on the FFT-bin scale; the anchor
table on the receiver in-band (3 kHz) scale. Each group vector is normalized
so `10·log10(mean_k γ_k)` equals the same group's measured OFDM broadband SNR
(the estimator already routed to rate selection) — flat-channel identity holds
on the anchor table's own scale, per group, with no calibration constant.

**Margins & recidivism:** an UP-candidate r is evaluated with every γ_k shifted
down by `kClimbMarginDb (2.5) + rx_auth_rung_penalty_db_[r]` — the existing
hysteresis and crater-pricing carry over unchanged (posterior beats prior).

**Time rule:** keep the last `kRxAuthGammaRing = 4` group snapshots (max age
180 s, same policy as the scalar ring). An up-jump target must pass on ALL
retained snapshots — "would r have survived every recent channel state",
worst-case over ~40 s of history. This is what makes a DIRECT multi-rung jump
safe: it is not a bet, it is a measurement.

## 4. What changes where

- **Demod (`src/ofdm/`):** accessor exposing the last equalized symbol's
  per-data-carrier γ_k (|channel_estimate|²/σ²). Constellation-independent.
- **StreamingDecoder:** per-group accumulator (mean per carrier across the
  group's frames — CWs span the group via the burst interleaver, so the group
  mean is the first-order decode predictor), captured alongside the existing
  per-frame metric templates, normalized to broadband-SNR scale at group end.
  Hosted in the decoder because the demodulator is recreated per group.
- **Binding → Connection:** `setBurstCarrierGammas(vector<float>)` mirrors
  `setBurstChannelObservation` (called before `onBurstGroupReceived`, for
  delivered AND cratered groups — the survivor-bias kill is that failed groups
  now contribute their measured channel).
- **`waveform_selection.hpp`:** pure predictor helpers —
  `rungPredictedSustainable(gammas, mod, rate, margin_db)` and
  `highestPredictedRungIdx(...)` walking the enabled ladder top-down.
- **`updateRxAuthorityCommand`:** for CLIMBS only — when ≥2 fresh snapshots
  exist, the climb target = highest enabled rung passing prediction on all
  snapshots (direct multi-rung jump; the one-rung walk and the scalar haircut
  become the FALLBACK when prediction is unavailable). The post-crater dwell,
  penalties, two-crater rule, down-limits, enabled-ladder snap, and every
  2026-07-06 safety stay exactly as they are.

Knob: `ULTRA_RX_PREDICTIVE_CLIMB` (default ON after gate validation; `=0`
opt-out reverts to the one-rung ladder).

## 5. What this does NOT do

- No wire change (rung_cmd already carries an absolute index).
- No sender change (obey path untouched).
- No demapper/LLR change (the 2026-07-06 experiment showed that cell is
  physics-limited; this feature ROUTES AROUND unsustainable rungs instead).
- Down-moves keep the existing crater/map machinery (already evidence-driven).

## 6. Validation

1. Unit (`test_rx_authority` + new `test_rung_prediction`): flat-channel
   identity vs the anchor table (± margin); notched vectors (20 % of carriers
   −30 dB) reject dense rungs while the scalar mean says climb — the exact
   F149 crest trap; direct-jump verdict (calm flat history ⇒ idx 3 → 8 in one
   verdict); fallback intact with no snapshots.
2. Full ctest; faithful gate multi-seed vs baseline (switch count, goodput).
3. Rig: switch count and time-to-ceiling vs the F169–F173 ledger; expected
   shape — one direct climb to the sustainable rung after ~2 groups, near-zero
   16QAM R2/3 visits at MPG@20, F170-style hunting gone.
