# 02 — The missed fix: estimation-error-aware LLR calibration

> ## ⚡ 2026-06-12 LIVE UPDATE (read first)
> While verifying this document, the audit re-ran **forced 16QAM R1/2 Good@20 seed 42
> on the current stack: PASS, 21 504 B CRC-clean, 1860 bps, 0 CW fails, 3 retx** —
> versus FAIL / 256 CW fails / 0 delivery on 2026-05-29 (same cell, GUI gate).
> **The 16QAM wall has largely FALLEN since May-29 and nobody re-measured it.** Prime
> suspect: the 06-08 phantom-CFO fix (`be0bbce` — fade-manufactured chirp CFO smearing
> per-symbol phase, exactly the May-29 "per-symbol phase" suspect); other candidates:
> PRBS-whitened padding (`c384b6a`, explicitly a 16QAM correlated-error fix), warm
> position-gating (`e0ceee4`), RX air-block miscount fix (`9189b70`). Higher-rung and
> multi-seed results in `07_VERIFICATION_RUNS.md`.
>
> The structural analysis below remains valid — the missing ε²_H term is a fact of the
> code and still matters for the *margins* (8PSK reliability, 16QAM R3/4 at the edge,
> Moderate-channel rungs) — but it is no longer the single gate it appeared to be.
> **And the cheap falsifier came back negative on the new stack:** 8PSK R3/4 seed 42
> with `ULTRA_LLR_NOISE_EMP_FLOOR=1.0` measured 1490 bps / 30 retx vs 1740 / 22
> baseline (07 run 4) — the single-symbol floor form mis-fires both ways, exactly as
> §3's counter-evidence predicted. Any production attempt MUST be the pilot-anchored
> smoothed ε²_H form (§5), gated on a multi-seed A/B, and is now priority *after* the
> ladder/airtime work, not before.
> Lesson for the project: **diagnoses decay; re-anchor measured walls after every
> intervening fix before building on them** (the May-29 "structural ~10-14 dB penalty"
> was true on the May-29 stack, and silently stopped being true within ten days).

**This is the centerpiece structural finding of the audit.** The May-29 campaign correctly
bounded the 16QAM wall ("per-carrier H-estimate accuracy, overconfident-LLR flavored")
but attacked the *estimator*. The cheaper, principled attack is on the **LLR noise
model**: the demapper asserts thermal-only confidence on a channel whose dominant
residual is estimation error. The corrective quantity is *already computed in-tree*
and discarded.

## 1. The mechanism (PHY-theorist form)

After MMSE equalization (`equalized = conj(H)·Y/(|H|²+σ²)`,
`channel_equalizer_equalize.cpp:506`), the per-carrier LLR noise variance is

```
carrier_noise_var[k] = σ² / (|H_k|² + σ²)        (equalize.cpp:507-509)
```

— a **thermal-only** model (× constant per-mod margins 1.0/1.1/1.2,
`demodulator_constants.hpp:117-119`, × an |eq|-EMA inflation, `ofdm_symbol_demap.cpp:335-346`).
Every term is ∝ σ². The true post-equalization residual on a fading channel is

```
σ²_eff(k) ≈ σ²/|H_k|²  +  ε²_H(k)·|x|²   (channel-estimate error term — MISSING)
           (+ phase-ramp/CFO residual terms)
```

As SNR→∞, σ²→0 but ε_H stays finite (it is set by pilot density, interpolation, and
fading dynamics — see `03`). So LLR = 2·d/nv → the ±20 clip (`MAX_LLR`,
`demodulator_constants.hpp:24`) for every bit, right or wrong: the decoder degenerates
to hard-decision decoding of an H-error-floored bit stream. This exactly reproduces the
measured signature — **16QAM decode peaks ~36 dB and DECLINES toward 60 dB**
(`docs/16QAM_DECODABILITY_DIAGNOSIS_2026_05_29.md:137-148`) — which no thermal-noise
mechanism can produce. QPSK survives because its 45° margin keeps the *sign* of the
LLR right; 16QAM's ring bits flip sign with ~10-17° of phase or a few % of gain error,
and those wrong bits arrive at the same ±20 magnitude as the right ones.

**The codebase already measured this.** The FAILURE_ATTRIBUTION eq-diag found the
analytic model under-estimates the true post-eq residual by **4-14× on Good@20** —
recorded verbatim in the comment at `channel_equalizer_equalize.cpp:533-545`:
*"over-confident, confident-WRONG LLRs that poison the LDPC — fatal for tight 16QAM,
absorbed by QPSK's margins."*

## 2. What exists in-tree, today

| Piece | Where | Status |
|---|---|---|
| Per-carrier estimation-error variance (Wiener normalized MMSE residual `error_var = clamp(1−Σwᵢrᵢ,0,1)`) | `wiener_interpolator.hpp:195-196`, surfaced `channel_equalizer_pilot.cpp:971-979` | **computed, then discarded** on the LLR path |
| The exact σ²+ε²_H form, proving the pattern fits the architecture | DD-Kalman only: `pilot_measurement_var = σ² + (√err_var·|H_ref|)²`, `pilot.cpp:1009-1014` | wired to a tracker that is OFF on fading |
| Empirical proof knob: floor nv at k·\|eq − hardDecision\|² (only RAISES nv; auto-erasure on deep nulls) | `ULTRA_LLR_NOISE_EMP_FLOOR`, `equalize.cpp:546-561` | built 05-29, **default off (k=0), never run to a recorded conclusion** |
| Per-symbol phase-residual split (CPE vs per-bin slope) to apportion ε²_H vs phase terms | `recordFailureAttributionSymbol`, `equalize.cpp:345-373` | diagnostic only |

## 3. Why this is NOT the 05-26 dead end — and the honest counter-evidence

The dead-end register has "σ² LLR calibration — GUI 4→12 CWfail, net-negative,
reverted" (`docs/FADING_RELIABILITY_CAMPAIGN_2026_05_26.md:23`). **Adversarially
verified distinct** (recovered from `git stash@{3}`): that change was a *frame-global
multiplicative* calibration (one scalar applied to all carriers). The proposal here is
**per-carrier** — a mechanistically different design, and per-carrier only-raise
inflation is independently proven shippable (`relativeFadeNoiseInflation`, commit
12f2a0c, 5/5 seeds Good@20). The register entry was also a single GUI run that the
same doc flags as within single-seed variance. Do not let it kill the proposal.

**Counter-evidence to carry honestly** (from the adversarial pass):
- The 2026-05-25 attribution (`docs/QAM16_FAILURE_ATTRIBUTION_2026_05_25.md`, retired
  cli-era harness, pre-coherent-only/pre-warm-sync-fixes) tested the
  empirical-residual-σ² *family* and recovered only **4/23** 16QAM CW failures; the
  measured wall then was **timing/sync-window rejection (18/23)**. If the timing share
  has since been fixed by the warm-sync work, σ²'s residual share is larger today —
  but expecting calibration ALONE to unlock 16QAM contradicts that data point until
  re-measured on the current stack.
- The single-symbol floor `k·|eq − hardDecision|²` shares a failure mode with the old
  DD-EVM evidence: a *confidently-wrong* decision yields a SMALL residual, so the
  floor fails to trigger exactly on poison carriers. The in-code comment
  (`equalize.cpp:541-545`) itself says the production form must be **smoothed /
  pilot-anchored**, not single-symbol.
- "Only-raises" is not a safety argument — the 05-26 attempt also only raised and
  still regressed.

The **structural finding stands regardless** (the missing ε²_H term is a fact of the
code); what is uncertain is how much of the 16QAM wall it carries *alone* vs combined
with the §4 defects and the 03 estimator items. The fixed data-aided genie (03 §4) is
the instrument that settles the split.

## 4. Compounding 16QAM-specific defects (fix together, smallest first)

1. **Relative-fade null gate excludes 16QAM** (`equalize.cpp:450-456`): the only
   mechanism that deflates LLRs in a deep *relative* null at high mean SNR is scoped to
   QPSK/QAM8. The May-29 erasure tests (REL_FADE knobs) therefore never touched 16QAM —
   "erasure ruled out" is true only for the magnitude-gate family, not for CSI
   deflation on 16QAM.
2. **CARRIER_ADAPTIVE_K constant-modulus bias** (`ofdm_symbol_demap.cpp:303-346`): the
   |eq|-EMA instability detector reads 16QAM's 3 rings as channel instability —
   blanket ~2× nv inflation even on AWGN and no fading contrast for exactly this
   modulation. Make it ring-aware (reference the EMA to the nearest-ring radius) or
   replace it with the principled ε²_H term from §5.
3. **MMSE bias on amplitude bits** (`equalize.cpp:506` + fixed 2/√10 grid,
   `demodulator_constants.hpp:104`): output mean is β·x, β=|H|²/(|H|²+σ²) — scale the
   slicer grid by β (or divide eq output by β). Deterministic ring errors on low-γ
   carriers; vanishes at high SNR (so it explains fading-carrier fragility, not the
   60 dB decline).
4. **LLR scale conventions** (`soft_demap.hpp:56,79,91-99`): 8PSK 4×, QAM16 ~3.16×
   overconfident vs the QPSK-exact convention — benign to min-sum *except* it advances
   the ±20 clip, erasing CSI dynamic range. Re-derive scales + revisit MAX_LLR.

## 5. The production fix (insertion points)

Minimal, modulation-agnostic by construction (the ADAPTIVITY rule):

1. Persist the Wiener `error_var` (and the non-Wiener 0.20·|H_ref| fallback,
   `pilot.cpp:1011`) per data carrier in `updateChannelEstimate`. **Dimensional note
   (verified):** `error_var` is a *normalized* MMSE residual in [0,1]
   (`wiener_interpolator.hpp:195-196`, obs noise normalized by pilot power,
   `pilot.cpp:649-650`) — it needs the `×|H_ref|²` absolute conversion the DD-Kalman
   already demonstrates (`pilot.cpp:1009-1011`), then mapping through the MMSE post-eq
   scaling. Not a drop-in wire-through.
2. In `equalize()` (`equalize.cpp:506-509`):
   `carrier_noise_var[k] = (σ² + err_var_norm(k)·|H_k|²) / (|H_k|² + σ²)`
   (+ optionally the measured per-symbol phase-residual term from the
   failure-attribution diag).
3. Validate `error_var` calibration once against a genie (`err_var·|H|²` vs
   `|H_est−H_true|²`). The fixed data-aided genie (03 §4) provides exactly this oracle.
4. Re-test the relative-fade gate ON for QAM16 *after* (2). Caution: the QPSK/QAM8-only
   scoping was a **deliberate, A/B-documented anti-double-count decision** (stacking
   with softGrayZone regressed AWGN@30, comment `equalize.cpp:450-454`) — the right
   fix is *unifying* the reliability model (one calibrated σ²_eff replaces the gate
   patchwork), not naively un-gating.
5. A second in-tree proof knob exists besides EMP_FLOOR:
   `ULTRA_QAM16_GENIE_SIGMA_EMPIRICAL` (`ofdm_stream_processor.cpp:56-57`,
   `ofdm_symbol_demap.cpp:351-360`) — REPLACES σ² with measured residual, QAM16-only.

## 6. Falsification experiments (this session — results in 07)

- **E1:** forced 16QAM R1/2 Good@20 seed42, `ULTRA_LLR_NOISE_EMP_FLOOR=1.0` — the
  knob's own comment: "If this rescues 16QAM on Good@20 the over-confident-LLR
  diagnosis is proven."
- **E2:** same, floor off — A/B baseline on the current stack (May-29 GUI evidence:
  FAIL, 256 CW fails).
- **E3/E4:** forced 8PSK R3/4 Good@20 seed42, floor off/on — 8PSK is unmeasured on the
  current stack (post transport-merge + phantom-CFO fix); the same mechanism predicts
  the floor narrows 8PSK's 2× survivability gap vs QPSK.

Interpretation guide: E1 ≫ E2 ⇒ mechanism proven, build §5. E1 ≈ E2 (both fail) ⇒
single-symbol hard-decision floor is too blunt for 16QAM (wrong-but-near decisions
under-floor); the §5 pilot-anchored form and the genie split (03 §4) are the next
moves — the structural finding (missing ε²_H term) stands either way, it is a fact of
the code, not of one experiment. Expect E1 to also *erase* fewer good carriers than
REL_FADE since it keys on the actual residual, not |H|.
