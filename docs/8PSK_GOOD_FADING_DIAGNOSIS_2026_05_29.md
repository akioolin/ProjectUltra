# Why 8PSK (QAM8) R3/4 fails on Good@20 fading — diagnosis (2026-05-29)

**Question:** forced 8PSK R3/4 delivers perfectly on clean AWGN but fails (heavy
resends, file often not delivered) on Good@20 Watterson fading. Why? 8PSK is the
promotion lever toward 3000 bps (2330 bps measured on clean AWGN, +28% over QPSK),
so this is load-bearing for the throughput goal.

**Method:** systematic elimination against ground truth (the project's "validate the
chain stage-by-stage, clean channel first, don't trust 6 months of AI iteration"
rule). Each hypothesis killed by an *experiment*, not by reading code. Multiple
wrong guesses along the way (fade, residual-CFO, timing-ramp, channel estimate) —
each corrected by the next measurement. The mid-run snapshot also misled once
(DD-off looked like it was failing at 3/11 mid-run, but the run finished PASS) —
reconfirming "don't diagnose from a snapshot; wait for summary.env."

## Eliminations (all experimental)

| # | Hypothesis | Test | Result | Verdict |
|---|---|---|---|---|
| 1 | TX→EQ→demap→LDPC chain bug | force 8PSK R3/4 on **clean AWGN30** | PASS 11/11, 0 resends, **2330 bps** | ❌ chain is correct |
| 2 | Amplitude fade / weak signal | inspect LLRs on failing Good frames | **strong** \|llr\|≈15–20, parity fails (50–99 unsat), SNR 20 | ❌ not amplitude fade — confident-WRONG bits |
| 3 | 8PSK soft-demapper wrong | `test_ofdm` unit test (ideal symbols) + TX/RX mapping inverse check | LLR signs OK; Gray map `{0,1,3,2,6,7,5,4}` + MSB-first bit order are exact inverses | ❌ demapper correct |
| 4 | Channel **estimate** (LTS+pilot H) per-carrier phase error | `ULTRA_QAM16_GENIE_CHANNEL_TWOPATH_LS` (perfect 2-path H, extended to QAM8) on Good@20 | still FAIL (6/11, 60 CW fails) | ❌ not the static estimate |
| 5 | Per-symbol common-phase / residual-CFO ramp | per-symbol `cpe` trace (added at channel_equalizer_equalize.cpp) | `cpe` ±3°, **no ramp**, per-carrier slope ~0 | ❌ common phase + linear ramp are clean |
| 6 | **Decision-directed (DD) channel tracking** poisons H on fading | `ULTRA_COHERENT_DD_OFF=1` on Good@20 | **DD-off → PASS, file delivered** (DD-on → FAIL) | ✅ **DD is the primary culprit** |

## Root cause

**Decision-directed channel tracking (`use_coherent_dd`, ON for QAM8/QAM16 in
`channel_equalizer_pilot.cpp`) corrupts the channel estimate on fading.** On Good@20,
8PSK's tight 22.5° decision boundaries make occasional wrong hard-decisions; DD feeds
those wrong decisions back into the H estimate, which poisons it and cascades into
*confident-wrong* bits (strong LLRs, wrong sign) → LDPC fails. The code's own comment
(pilot.cpp ~884) already warned this: *"bad hard decisions during fades poison the
channel estimate and cascade. DD is only safe on QAM8/QAM16 today"* — the "safe on
QAM8" assumption is what this disproves for 8PSK on fading.

Confirming details:
- DD-off uses the **normal** (LTS+pilot) H and delivers — so the base estimate was
  fine; DD was poisoning it (which is also why the genie perfect-H with DD-*on* still
  failed — DD corrupts even a perfect initial H).
- Failures are **bimodal per group** (6/6 OK or 0/6 fail, never partial) — a group's
  window either escapes the DD-cascade or gets swallowed by it.
- Works on AWGN because a static channel gives DD no drift to make wrong decisions
  against — the cascade needs a moving channel to start.

## Residual (not fully solved by DD-off)

DD-off makes 8PSK *deliver* on Good@20 but still marginal (heavy resends, ~84 CW
fails/run on seed 42) — the irreducible frequency-selective-phase difficulty near
spectral nulls that 8PSK's 22.5° boundaries feel and QPSK's 45° rides through. ARQ /
adaptive rate handles that tail; DD-off removes the *self-inflicted* cascade on top.

## Fix direction

1. **Gate DD off (or reliability-gate it) for 8PSK on fading.** The code comment
   already proposes the principled form: *"only DD a symbol when its EVM is well
   inside the 95% Gaussian noise radius"* — i.e., don't feed a marginal/likely-wrong
   decision back into H. That's the real fix vs a blanket off-switch.
2. **Channel-adaptive modulation:** 8PSK shines on clean/mild channels (AWGN 2330
   bps); promote to it when the channel is clean enough, stay QPSK on deep fade. Ties
   into the runtime-config-derivation / adaptive-rate workstream.

## Reproduce / test instruments added (env-gated, test-only)

- `ULTRA_FORCE_DATA_MOD=8PSK ULTRA_FORCE_DATA_RATE=R3_4` — force the rung.
- `ULTRA_COHERENT_DD_OFF=1` — disable decision-directed tracking (added 2026-05-29,
  `channel_equalizer_pilot.cpp`).
- `ULTRA_QAM16_GENIE_CHANNEL_TWOPATH_LS=1` + `..._DELAY_SAMPLES=24` — genie perfect
  2-path H (extended to QAM8, `channel_equalizer_pilot.cpp`).
- `ULTRA_FAILURE_ATTRIBUTION=1` — per-symbol `cpe`/slope `PHASE-TRACE` log
  (`channel_equalizer_equalize.cpp`).
- Harness: `tools/gui_qso_scenario.sh --channel {awgn,good} --snr-db N --seed S
  --expect-mod 8PSK --expect-rate R3/4 --file-kb 21` (warm config baked in).

## Status

- Five hypotheses eliminated experimentally; DD identified as primary root cause.
- Multi-seed DD on/off A/B (seeds 42/43/44) running to confirm — results in
  `/tmp/dd_ab_8psk.txt` (update this section when complete).
- Test knobs are uncommitted diagnostic additions; commit once the A/B confirms.
