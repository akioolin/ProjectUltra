# 07 — Live verification runs (2026-06-12, this audit session)

All runs: `tools/gui_qso_scenario.sh --channel good --snr-db 20 --file-kb 21`
(the faithful gate, two real GUI stations over OTASim), SEQUENTIAL (never parallel —
wall-clock pacing artifact), forced rungs via `ULTRA_FORCE_DATA_MOD/RATE`, harness
defaults `ULTRA_LOCK_RATE=1 ULTRA_ADAPTIVE_RATE=1`. Build = working tree @ 65ce3de +
uncommitted WIP (escape-drop; inert here — gated on `rateAdaptationActive()`,
which is false under LOCK_RATE=1). Run dirs + full logs under `/tmp/fable_*`.
Reminder when reproducing: this gate is ~0.61 dB HARDER than a 3 kHz-convention 20 dB
(see 01 §1).

## Results

| # | Rung | Seed | Knobs | RESULT | Goodput | CW fails | Retx (frame dmg) | Notes |
|---|---|---|---|---|---:|---:|---|---|
| 1 | 16QAM R1/2 | 42 | `ULTRA_LLR_NOISE_EMP_FLOOR=1.0` | **CRASH** | — | — | — | ALPHA segfault @76 s mid control-peek decode, right after LTS estimate; see 08 §0 |
| 2 | 16QAM R1/2 | 42 | none | **PASS** | **1860** | 0 | 3 (~4%) | **vs 2026-05-29: FAIL, 256 CW fails, 0 delivery — same cell** |
| 3 | 8PSK R3/4 | 42 | none | **PASS** | **1740** | 0 | 22 (~31% dmg) | vs 05-29: 710 bps / marginal / seed-44 FAIL |
| 4 | 8PSK R3/4 | 42 | `ULTRA_LLR_NOISE_EMP_FLOOR=1.0` | **PASS** | 1490 | 0 | 30 | **floor k=1.0 is net-negative** (vs run 3); no crash |
| 5 | 16QAM R3/4 | 42 | none | **PASS** | 1330 | 8 | 46 | damage-bound |
| 6 | 16QAM R3/4 | 43 | none | **PASS** | 1110 | 0 | 75 | damage-bound |
| 7 | 16QAM R3/4 | 7 | none | **FAIL** | 0 | 0 | 81 | hard failure (link death by retx) |
| 8 | 16QAM R2/3 | 42 | none | **PASS** | 1200 | 0 | 58 | damage-bound |
| 9 | 16QAM R2/3 | 43 | none | **PASS** | 740 | 0 | 118 | damage-bound |
| 10 | 16QAM R2/3 | 7 | none | **PASS** | 820 | 0 | 102 | damage-bound |

**Runs 8-10 reading — the "R2/3 sweet spot" prediction is REFUTED:** R2/3 delivers
3/3 but at 740-1200 bps with 58-118 retx — *worse goodput and more retx than R3/4 on
the same seeds despite 33% vs 25% parity*. That violates naive FEC-margin ordering and
is an open anomaly: the salient config difference is pilot spacing (R2/3 → 5, 12
pilots/47 data; R3/4 → 8, 8 pilots/51 data, `ofdm_link_adaptation.hpp:46-64`) — but
16QAM R1/2 also runs sp5 and is clean, so it is spacing × thin-margin, or another
per-rate config divergence. Phase-0 item: isolate it (sp8-vs-sp5 A/B at R2/3 via
`ULTRA_R23_PILOT_SPACING=8`).

**Consolidated empirical picture (Good@20, current stack, 21 KB, forced):**

| Rung | Raw bps | Measured goodput | Damage | Character |
|---|---:|---|---|---|
| QPSK R3/4 (anchor) | 3 279 | 1630 avg / 1910 max | 0-18% | clean, AT protocol ceiling |
| **16QAM R1/2** | 4 029 | **1860** | ~4% | **clean, AT protocol ceiling** |
| 8PSK R3/4 | 4 918 | 1740 | ~31% | damage-bound |
| 16QAM R2/3 | 5 371 | 740-1200 | ~55-70% | damage-bound |
| 16QAM R3/4 | 6 557 | 0-1330 | ~50%+, 1/3 link-death | damage-bound |

There is a **reliability cliff between 50% and 33% FEC redundancy** for dense
modulations on this channel. Every clean rung is protocol-ceiling-capped (~2030);
every rung with raw headroom is damage-bound. Conclusion: the 3086 target requires
BOTH (a) the airtime levers (raise the ceiling) AND (b) the margins work that makes a
≥R2/3 dense rung clean — converting confident-wrong null-region bits into properly
weighted erasures so the parity budget works as designed (02 §4-5, 03 §3). Neither
alone suffices: clean-rung + levers tops out ~2570-2780 (16QAM R1/2), and a clean
dense rung without levers tops out ~2800-3270 with no damage margin.

**Runs 5-7 reading:** 16QAM R3/4 on Good@20 is 2/3 PASS with heavy damage and 1/3
link-death-by-retransmission — the rung *can* decode (the old 0%-delivery structural
wall is gone) but loses whole frames at the deinterleave stage at a ~50-60% rate
(seed 43: 75 deinterleave-fails vs 49 OK on BRAVO, with the CWFAIL counter at 0 —
losses register as frame-level deinterleave failures, worth a counter-semantics check
when characterizing further). Its 25% FEC redundancy against Good's ~23% instantaneous
null erasure is the structural reason. R3/4 is a fair-weather rung pending Phase-3
margins work; the throughput rung race on Good@20 is between 16QAM R1/2 (clean,
protocol-capped ~2030) and R2/3 (runs 8-10).

**Run-4 reading:** the single-symbol EMP_FLOOR at k=1.0 *hurts* on the current stack
(1740→1490 bps, 22→30 retx, same seed): with the per-symbol-phase poison gone, the
floor's failure modes dominate — it over-inflates noise on legitimately-scattered good
carriers and under-fires on confidently-wrong ones (small |eq−dec|²). This empirically
confirms the v-deadend-sigma caution and demotes the LLR-floor from "the missed fix"
to "a margins refinement whose production form must be pilot-anchored/smoothed"
(02 §5). It also bounds the crash: 16QAM×floor-specific or a flake (run 4 ran the
same knob clean on 8PSK).

## Phase 0a — 5-seed anchor sweep (Opus, 2026-06-12 midday)

Turned Fable's single-seed numbers into 5/5 statistics. Seeds {42,43,44,7,2}, forced,
sequential, 21 KB, Good. **Raw data: `data_phase0a_sweep_2026-06-12.tsv`.**
⚠ **Load caveat:** ran under midday load avg ~2.4–2.7 (Fable's were ~08:0x, quieter).
Absolute goodput is therefore *load-depressed* and noisy — the trustworthy outputs are
**pass-rate, damage pattern, and relative rung ordering**. Corroboration that it's
load, not regression: the one clean-load run (16QAM R1/2 @20 seed 7, 124 s) hit 1890,
matching Fable's morning 1860/127 s; the slowest runs (153–239 s) gave the lowest bps.
Treat absolutes as a conservative floor; re-anchor on a quiet box for the leader compare.

| Cell | Result | Goodput (median / range) | Retx range | CW-fails |
|---|---|---|---|---|
| **16QAM R1/2 Good@20** | **5/5 PASS** | **1550 / 1400–1890** | 4–18 | 8 on 1 seed, 0 on 4 |
| 8PSK R3/4 Good@20 | 5/5 PASS | 1440 / 840–1620 | **24–72** | 0 (8 on seed 44) |
| 16QAM R1/2 Good@18 | 5/5 PASS | 1250 / 990–1550 | 18–64 | 0 (all 5) |

Per-run detail (goodput / retx / ~frame-loss):
- 16QAM R1/2 @20: 1400/16/22% · 1540/16/17% · 1550/18/20% · 1890/4/5% · 1760/10/12%
- 8PSK R3/4 @20: 1440/34/35% · 1390/40/42% · 840/72/60% · 1480/24/33% · 1620/28/36%
- 16QAM R1/2 @18: 990/64/45% · 1550/18/20% · 1270/34/31% · 1250/38/34% · 1220/31/29%

**Three findings, all multi-seed solid:**

1. **16QAM R1/2 Good@20 is a genuinely clean, robust rung — 5/5 PASS, 0 CW-fails on
   4/5** (the lone 8-fail was the load-stressed 156 s run). Confirms + hardens Fable's
   single seed-42 point. This is the trustworthy baseline Phase 1 needs.

2. **16QAM R1/2 ≥ 8PSK R3/4 on BOTH goodput and reliability** — despite 8PSK's 22%
   higher raw rate (4918 vs 4029). 8PSK ran 24–72 retx (vs 4–18) and a worse median
   (1440 vs 1550); its thin 25% FEC margin is shredded by Good's ~23% null erasure and
   ARQ claws it all back — net loss. **8PSK is confirmed a throughput dead end on
   Good@20**, not a shortcut around the 16QAM margins work. (It IS reliable — 5/5 — just
   slow.) The dense-rung win must come from 16QAM at higher code rate made clean.

3. **16QAM R1/2 holds at Good@18 — 5/5 PASS, 0 CW-fails, median ~1250.** The decode is
   robust 2 dB below the anchor; the only degradation is fade-erasure (whole-frame
   losses ARQ recovers), confirming the lever is FEC-margin/diversity, not decode.
   Seed-to-seed spread (990 vs 1550 at the same 18 dB) dwarfs the 2 dB SNR step —
   the fade realization dominates, consistent with the classifier-can't-discriminate
   finding. → cheap goodput is available by bracketing the Good R1/2 floor below 20 dB.

**Methodology note for the next session:** same-seed run-to-run variance is real on
this real-time gate (seed 42 @20 gave 1860 for Fable, 1400 for this run — load + fade
re-alignment). Single-run absolute goodput is ±25%. For absolute anchors, run on a
quiet box AND/OR average ≥3 runs per seed; for the leader compare use 100 KB +
mid-transfer slope (far less wall-clock-sensitive). Pass-rate and relative ordering are
robust to this; absolute bps is not.

## Phase 1 — env-gated 16QAM auto-ladder (Opus, 2026-06-12)

Verifying the increment that lets the AUTO path select 16QAM (vs `ULTRA_FORCE_DATA_MOD`
which bypasses the ladder). Good@20, 21 KB, seed 42 unless noted. Code: `--expect-mod any`
harness option, `ULTRA_ENABLE_QAM16_LADDER` ladder gate, `maxValidatedCoherentRate` per-mod cap.

| Run | Config | First/trajectory mode | Result | Goodput |
|---|---|---|---|---|
| A baseline | gate OFF | QPSK R3/4 (×1) | PASS | 1970 | 
| B gated | `ENABLE_QAM16_LADDER=1`, rate locked | **16QAM R1/2 (×1)** auto-selected | PASS | 1450 |
| C gated+adapt (pre-cap) | `+RATE_ADAPT=1 LOCK_RATE=0` | 16QAM R1/2 → **R2/3** (climbed!) | PASS | 1450 |
| cap s42 | post-cap, gated+adapt | **16QAM R1/2 only** | PASS | 1830 |
| cap s7 | post-cap, gated+adapt | **16QAM R1/2 only** | PASS | 1740 |

**Reading:**
- **A:** gate-off default is unbroken — Good@20 picks QPSK R3/4, exactly as before. The
  env-gated change is byte-identical with the flag unset (confirmed by review + ctest 5/5).
- **B:** the auto path now selects **16QAM R1/2** end-to-end (real CONNECT negotiation +
  RX-accept, not a forced knob) and delivers CRC-clean. The highest-leverage structural
  capability from the roadmap, now real. 1450 vs A's 1970 is the lateral-move reality
  (16QAM R1/2 ≈ QPSK R3/4) plus run-to-run load noise — payoff waits for Phase 2b.
- **C → cap:** the adversarial review's HIGH finding, reproduced then fixed. Pre-cap the
  modulation-blind controller climbed R1/2 → **R2/3** (damage-bound) on a clean stretch;
  ssthresh is reactive so the first climb is unguarded (→ seed-7 R3/4 link-death risk).
  The per-mod cap pins it at R1/2: 2/2 seeds stay R1/2 AND goodput *rises* (1450 → 1830/1740)
  because no airtime is wasted probing the losing rung. ctest policy/rate suite 5/5 green.

## Reading of the results so far

1. **The May-29 "16QAM is structurally undecodable on Good@20" verdict is STALE.**
   Same seed, same channel, same gate: 0 CW fails today. The wall fell somewhere in
   the ~10 days of fixes after the diagnosis was written. Prime suspect by mechanism
   match: **phantom-CFO confidence gate** (`be0bbce`, 06-08) — a fade-manufactured
   chirp CFO smears per-symbol phase across the burst, which QPSK's 45° margin rides
   and 16QAM cannot; the May-29 convergent verdict ("per-symbol phase / per-carrier H
   accuracy, overconfident-LLR flavored") is *exactly* this signature. Other
   candidates landed in the window: PRBS-whitened frame padding (`c384b6a` — explicitly
   fixed correlated 16QAM symbol errors), warm position-gating (`e0ceee4`), carrier-LDPC
   RX air-block miscount (`9189b70`). Isolating which (by checking out commits around
   be0bbce and re-running this cell) is cheap and worth doing for the record.
2. **Damage ordering matches FEC-margin physics, not modulation order:** 16QAM R1/2
   (50% redundancy) ~4% frame damage; 8PSK R3/4 (25% redundancy) ~31% damage. Good's
   moving null instantaneously erases ~23% of the band; the code-rate margin against
   that erasure — not the constellation — is the first-order survival variable now
   that the per-symbol-phase poison is gone. Expect 16QAM R2/3 (33%) to be the
   throughput/reliability sweet spot, 16QAM R3/4 to lean on ARQ.
3. **Both passing runs sit at 91-96% of their *protocol* cycle ceilings** (R1/2
   ceiling ≈ 2030 bps, measured 1860; computed in 01 §4 / 04) — decode quality is no
   longer the limiter at these rungs; the airtime structure is (04's ledger).
4. **The EMP_FLOOR knob crash (run 1)** blocks the LLR-floor A/B; with the baseline
   now passing at R1/2, the calibration question moves to the margins (8PSK damage,
   16QAM R3/4 edge, Moderate) — see 02's live-update box.

## Protocol for whoever re-runs / extends these

```bash
ULTRA_FORCE_DATA_MOD=QAM16 ULTRA_FORCE_DATA_RATE=R2_3 \
  tools/gui_qso_scenario.sh --channel good --snr-db 20 --seed <S> \
  --expect-mod 16QAM --expect-rate R2/3 --file-kb 21 --out /tmp/<name> \
  2>&1 | tee /tmp/<name>.log
```
- Seeds: {42, 43, 44, 7, 2} minimum for any claim (seed 2 is the standing CFO-phantom
  guard). 5/5 CRC-clean + damage ≤~10% is the "rung is real" bar
  (`docs/RATE_LADDER_ANCHORS.md` method).
- For leader-comparable numbers use `--file-kb 100` and the mid-transfer slope
  (25%→75% TX milestones in alpha.log), per 01 §1.
- Never run two scenarios concurrently; never benchmark on a loaded machine.
