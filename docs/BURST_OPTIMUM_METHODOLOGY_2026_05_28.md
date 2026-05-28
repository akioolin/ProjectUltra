# Burst-Optimum Testing Methodology — 2026-05-28

The methodology for finding the most-reliable burst recipe per (rate × channel)
combination. **Read this before re-running the sweep so we don't re-derive it.**

## Goal

Find the burst recipe that **minimizes mean retx count** at a given rate on a
given channel. "Reliable" = few rate drops, few timeout-triggered retx,
high first-attempt PASS rate. Throughput is the SECOND axis we attack
(after the reliable foundation is locked).

## Variables that matter

| Variable | Env knob | Range |
|---|---|---|
| LDPC blocklength | `ULTRA_LDPC_Z` | {27, 81} → N=648 / N=1944 |
| Pilot spacing | `ULTRA_R23_PILOT_SPACING` (per rate) | {5, 7, 8, 10, 12, 15} |
| CW per frame | `ULTRA_FRAME_CW` | {1..8} |
| Burst group size | `ULTRA_BURST_GROUP_FRAMES` | {2..32} |
| Wiener Doppler | `ULTRA_WIENER_DOPPLER_HZ` | float (0.05..1.0 for HF) |
| Wiener delay-spread | `ULTRA_WIENER_DELAY_SPREAD_S` | float (0.0005..0.002) |
| Anti-poison onset | `ULTRA_REL_FADE_ONSET` | float (0.10..0.70) |
| Anti-poison max | `ULTRA_REL_FADE_MAX` | float (30..500) |
| Max rate cap | `ULTRA_MAX_OFDM_RATE` | R1_2 / R2_3 / R3_4 |
| QPSK DD tracking | `ULTRA_QPSK_DD` | 0/1 (default 0 — regressed) |

## Reliability metric

**Primary:** mean retx count per run (lower = better). Captured as
`grep -cE "adaptive rate.*q=0\.00"` in alpha.log — counts the timeout-driven
rate drops, the strongest signal of unreliability.

**Secondary:**
- First-burst PASS rate (count groups that PASS on first attempt, not after retx)
- End-to-end goodput (file_bytes × 8 / elapsed_seconds) — captures both reliability
  AND throughput in one number

## Phase 1: Wide matrix, few seeds (current sweep, 2026-05-28)

Cell axes for R2/3-target sweep:
- LDPC ∈ {N=648, N=1944}
- pilots ∈ {sp5 dense, sp8 medium, sp10 sparse}
- cw ∈ {4, 8}
- 12 cells × 5 seeds × 21KB Good@20 = 60 runs ≈ 3 hours

Held constant for this phase:
- R2/3 (the throughput target)
- g8 burst group (short burst → fast retx if needed)
- Wiener tuned to Good HF (0.1 Hz / 0.5 ms)
- Anti-poison at defaults

Output: top 2-3 candidates by mean retx.

## Phase 2: Validate winners with more seeds (after Phase 1)

Top 3 candidates from Phase 1 × 15-20 seeds × 21KB or 100KB → tight CI on retx
rate. 21KB if compute-bound, 100KB for better burst-group statistics per run.

## Phase 3: Cross-rate + cross-channel (NOT YET DONE)

Repeat the top-3 validation for the other production rates and channel cells:
- Rates: R3/4 (clean throughput), R2/3 (target), R1/2 (rough), R1/4 (worst)
- Channels: Good@20, Good@15, Good@10, Moderate@20

Build the operating-envelope table: per (rate × channel), what's the optimum
burst recipe?

## Honest limitations of the current Phase 1 sweep

1. **Single channel.** Only Good@20. Optimum may differ on Moderate or low SNR.
2. **Single rate.** R2/3 only. Per-rate optimums need their own sweeps.
3. **Few seeds.** 5 seeds × 1-2 burst groups = 5-10 observations per cell. Only
   detects ~30% effect sizes reliably. Top winners need validation.
4. **Group size held.** g8 is reasonable but the optimum MIGHT be g16 or g4.
5. **Anti-poison thresholds held.** These may interact with pilot density.
6. **21KB file.** Each run only has 1-2 burst groups. 100KB would give 12-14
   groups per run — much better burst-level statistics with same run count.

## Phase 0 (cleanup): cli_simulator wall-clock artifact

`cli_simulator` is CPU-paced (~40× faster than 48 kHz real-time). Its
"timeout"-cause retx are simulator artifacts. **Don't draw reliability
conclusions from cli runs** — only the GUI scenario (`tools/qam16_ladder_scenario.sh`)
uses real-time clock and gives faithful timing.

## Reference numbers to compare against

Baseline (shipping default: sp5, z27, cw=8, g16):
- Seed 1 (clean) e2e: ~1220 bps
- Seed 2 (hard fade) e2e: ~886 bps

The hard seed-2 wall is the reliability test. Anything that meaningfully
exceeds 886 bps e2e on seed 2 is real progress.
