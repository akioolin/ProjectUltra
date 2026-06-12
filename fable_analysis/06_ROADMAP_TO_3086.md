# 06 — Roadmap to ≥3086 bps delivered on Good@20

Ordered by (expected bps × confidence) / cost. Every step names its verification gate.
Run every gate multi-seed {42, 43, 44, 7, 2}, sequential, per 07's protocol. Apply the
four-tier perspective stack and the half-duplex/duty/Shannon guards from CLAUDE.md to
every change. Dead ends NOT to re-propose are in 04 §3, 05, and the docs-history
register (the survey's dead-end table) — read those before inventing alternatives.

## Phase 0 — Re-anchor reality (days, mostly measurement)

1. **Complete the 16QAM/8PSK re-anchor sweep** started in 07: 16QAM {R1/2, R2/3, R3/4}
   × 5 seeds, 8PSK R3/4 × 5 seeds, forced, Good@20. Bar: 5/5 CRC + damage ≤~10% for a
   "real rung". (First slices: 16QAM R1/2 PASS 1860/0 dmg; 8PSK R3/4 PASS 1740/31%
   dmg, seed 42.)
2. **Bisect what fixed 16QAM** (checkout around `be0bbce` / `c384b6a` / `e0ceee4` /
   `9189b70`, re-run the 07 cell) and rewrite the stale verdict in
   `docs/16QAM_DECODABILITY_DIAGNOSIS_2026_05_29.md` + KNOWN_BUGS. Knowing the real
   mechanism tells you whether it generalizes to Moderate and to real-CFO channels.
3. **Land the trivial fixes** from 08: R5/6 pilot fall-through (one line + GUI proof at
   sp8); budget phantom re-anchor removal (→ group 6 fits, +~105 bps); doc/stale-claim
   sweep. Gate: `ctest -R <touched>` + one adaptive Good@20 run, no regression.
4. Fix or quarantine the EMP_FLOOR knob crash (08 §0) — ASAN run of the 07 run-1 cell.

## Phase 1 — Give the ladder modulation rungs (1-2 weeks) → ~2700-3000 bps

The single highest-leverage structural change. Verified facts (07, v-no-mod-promotion):
the wire format already carries modulation; the RX MODE_CHANGE handler applies it
unrestricted; what's missing is sender policy + a trustworthy signal.

> **✅ Increment 1 LANDED (2026-06-12, env-gated, uncommitted):** the auto path can now
> SELECT 16QAM. `ULTRA_ENABLE_QAM16_LADDER` (default OFF) → `kCoherentLadderQAM16Exp`
> enables the {QAM16,R1/2} Good rung at the Phase-0a floor (18 dB); a code-derived
> per-mod cap `maxValidatedCoherentRate(QAM16)=R1/2` stops the modulation-blind
> RateController climbing into damage-bound 16QAM R2/3+; harness gained `--expect-mod
> any`. GUI-verified (07): gate-off → QPSK unbroken (1970), gate-on → 16QAM R1/2
> auto-selected + delivered, +adapt → pinned at R1/2 (1830/1740, vs pre-cap climb to
> R2/3 @1450). 3-lens adversarial review: default byte-identical, integration clean.
> Lateral vs QPSK R3/4 today by design — the throughput payoff needs Phase 2b. See
> CHANGELOG 2026-06-12 + MODEM_INFRASTRUCTURE_MAP §6.

Remaining for Phase 1:
1. **Default-on path:** before flipping the gate to default, (a) raise the 16QAM R1/2
   Good anchor 18→~20 dB for +2 margin parity, (b) derive `kCoherentLadderQAM16Exp` from
   `kCoherentLadder` (or static_assert the shared QPSK tail) to kill the duplication
   footgun, (c) prove the gated path ≥ QPSK default on a multi-seed Good@{18,20} sweep.
2. **Bump the QAM16 cap as Phase 2b validates higher rungs:** `maxValidatedCoherentRate`
   QAM16 → R2/3 → R3/4 as each is measured clean (this is the (mod,rate) ladder, grown
   one validated rung at a time).
3. **Mid-connection modulation promotion** (start QPSK low-SNR, promote to 16QAM as SNR
   rises): generalize `RateController` to (mod,rate) rungs, keeping the EMA/ssthresh/
   clean-boundary architecture. Only needed once a session must CHANGE modulation
   mid-stream — the connect-time selection above already covers the high-SNR case.
4. **Signal:** keep LDPC-iteration headroom as the closed-loop trim; wire the CIR
   delay-spread metric for class selection (05 §3); add a per-carrier post-EQ EVM
   histogram for the mod-promotion gate (the genie bit-load logic's form: "N% of
   carriers X-capable").
4. **Harness first:** add an "any coherent mod" `--expect-mod` option so the watchdog
   doesn't kill legitimate promotions (05 §5). Also fix the sticky-ceiling reset
   before enabling the WIP escape-drop alongside (05 §4).

Gate: adaptive Good@20 × 5 seeds converges to the best Phase-0 rung with ≤2 mode
changes, no oscillation, goodput ≥ the forced-rung number −10%.

## Phase 2a — Airtime levers (parallel track; necessary but NOT sufficient — see 2b)

From 04 (with the adversarial corrections — neither top lever is a knob flip):

1. **Short-anchor descriptor** (+~250-360 bps at QPSK, proportionally more at 16QAM):
   build TX short-chirp emission + RX detection; keep header+group_seq every burst
   (the "once per file" variant is a PROVEN dead end); keep full chirp on resends.
2. **Fast tone-ACK rung** (+~85 bps): 13.33 ms symbols (integer-cycle with the 75 Hz
   spacing — 12 ms is design-flawed), plumb symbol_ms through the callback + BOTH call
   sites, add to the production monitor scan list, FER-validate on AWGN+Watterson
   below 18 dB first.
3. **frame_mask widen + QAM16 window policy** (binding at 16QAM): reserved bits are
   outside the CRC-12 — coordinated wire change; also lift the high-throughput-window
   exclusion of QAM16 (`connection_policy.hpp:304-318`).
4. **Benchmark accounting:** 100 KB files + mid-transfer slope for leader-comparable
   numbers (01 §1; the 21 KB gate understates by ~15-20%).

## Phase 2b — Dense-rung margins work (parallel; MEASUREMENT PROMOTED IT TO MANDATORY)

The completed sweeps (07 runs 5-10) show every rung with raw headroom is damage-bound
(8PSK R3/4 ~31%; 16QAM R2/3 ~55-70%; 16QAM R3/4 ~50%+ with 1/3 link-death), while
every clean rung is protocol-capped. So the 3086 target is unreachable by levers
alone — this phase is co-equal with Phase 2a, not a follow-up:

1. **Unified per-carrier reliability model** (02 §4-5): pilot-anchored ε²_H term in
   `carrier_noise_var` (NOT the single-symbol floor — measured net-negative, 07 run 4),
   relative-null CSI deflation extended to 16QAM as part of one calibrated model (not
   gate-stacking), MMSE-bias slicer correction for amplitude bits, LLR-scale/MAX_LLR
   rederivation. Mechanism goal: convert confident-wrong null-region bits into
   properly-weighted near-erasures so 25-33% parity absorbs the ~23% null erasure.
2. **Isolate the R2/3 < R3/4 anomaly first** (07 runs 8-10): `ULTRA_R23_PILOT_SPACING=8`
   A/B — it may be a config divergence worth +hundreds of bps on its own.
3. **Estimator upgrade** per 03 §3 ranking (iterative data-aided re-estimation is the
   bet) if (1) is insufficient; **data-aided genie fix** (03 §4) as the oracle either way.
4. Per-carrier bit-loading toward the ~3 764 genie ceiling — last, after one uniform
   dense rung is clean.

Gate for this phase: forced 16QAM R2/3 (or R3/4) Good@20 × 5 seeds, 5/5 CRC, damage
≤~10%, goodput ≥ ~2 500 at current overheads.

## Arithmetic sanity (zero-retx ceilings; measured anchors in 07)

| Configuration | Ceiling | Measured |
|---|---:|---|
| 16QAM R1/2, current overheads | ~2 030 | 1860 (clean) |
| 16QAM R2/3, current overheads | ~2 800 | 740-1200 (damage-bound) |
| 16QAM R3/4, current overheads | ~3 270 | 0-1330 (damage-bound) |
| 16QAM R1/2 + all Phase-2a levers | ~2 570-2 780 | — short of target |
| 16QAM R2/3 + levers, IF made clean (Phase 2b) | ~3 540 | target +15% margin |
| 16QAM R3/4 + levers, IF made clean | ~4 170 | target +35% margin |
| 8PSK R3/4 + levers, IF made clean (fallback) | ~3 430 | target +11% margin |

**Measured conclusion: 3086 requires Phase 2a AND Phase 2b together.** Neither alone
reaches it. Phase 3 (below) then widens the envelope (Moderate, real CFO/ppm, lower SNR).

## Standing cautions

- Re-anchor any >1-week-old measured wall before building on it (the May-29 16QAM
  verdict silently expired within days — 02's lesson box).
- OTASim has zero CFO and zero clock-ppm (01 §1): before claiming leader parity,
  spot-check the winning configuration with a real-CFO/ppm impairment (or note the
  caveat) — 16QAM's phase margins are the most exposed to what the sim doesn't model,
  and the phantom-CFO fix's confidence gate is exactly the kind of logic real chirp
  CFO will exercise differently.
- Every change rides the proof gate: build + targeted ctest + multi-seed GUI; losers
  get reverted; document in CHANGELOG/KNOWN_BUGS/the infrastructure map in the SAME
  change.
