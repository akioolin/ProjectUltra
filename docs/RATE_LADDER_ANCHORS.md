# OFDM Rate-Ladder Anchors (measured) — source of truth for the picker rework

The measured anchor table for the coherent-only OFDM rate-ladder rework (replacing the
over-engineered 4-gate-array picker in `src/protocol/waveform_selection.hpp`). Each
**anchor** is the in-band SNR (dB) at which the AUTO ladder may select a `(modulation,
code-rate)` rung, per fading class. **Anchors come from MEASUREMENT, not hand-tuning.**

## How an anchor is set (methodology)

- **Force the rung** (`ULTRA_FORCE_WAVEFORM=OFDM_CHIRP` + `ULTRA_FORCE_DATA_MOD/_DATA_RATE`)
  and run the GUI scenario file transfer via `tools/qso_sweep.sh` (which calls
  `gui_qso_scenario.sh` + `analyze_qso_run.sh`).
- **Verdict = file delivered CRC-clean** (`FILE_CRC_OK`), **NOT** the scenario's wall-clock
  `RESULT`. The wall-clock / deadline / TX-duty are corrupted by host CPU load and a
  GUI-lifecycle hang (a sim window that won't close on its own) — they false-FAIL runs
  that actually delivered. Trust **sample-space** metrics only: goodput (the modem's own
  "X kbps" over modem-time), per-group **damage%**, and LDPC **it_max** (margin proxy;
  50 = cap = fail). `analyze_qso_run.sh` extracts these.
- **Multi-seed.** Fade timing is seed-dependent and varies run-to-run; one run is not
  enough. The anchor is the rung that delivers CRC-clean **across seeds** AND wins the
  goodput crossover with margin.
- **Viability = reliability + efficiency + a robustness buffer:**
  - reliability — always delivers, worst-case group retries ≪ `max_retries`;
  - efficiency — best *effective* (sample-based) goodput among reliable rungs;
  - robustness — for the *recommended* operating rung prefer low/single-digit damage%
    with LDPC margin (median it_max well under the cap). The matched-cliff rung is a
    "need-peak-rate + verified-stable-channel" option, not the default.

## Anchor table (in-band SNR, dB)

`—` = TBD (sweep it). `x` = disabled / never auto-selected (still forceable for probing).

| rung        | AWGN | GOOD | MODERATE | basis |
|-------------|------|------|----------|-------|
| QPSK R1/4   | 10   | 12   | 14       | = OFDM entry floors (the floor rung) |
| QPSK R1/2   | 12   | 14   | 18       | measured 2026-05-21 (+2 dB margin) |
| QPSK R2/3   | —    | **15** | —      | **Good@15 measured 2026-06-02 (below)** |
| QPSK R3/4   | —    | cliff | —       | Good = fade-matched cliff (below) |
| QAM16 …     | —    | —    | —        | clean-channel throughput; TBD |
| 8PSK / QAM8 | x    | x    | x        | retired from the OFDM band (thread C to restore) |

## Findings

### Good@15 → QPSK R2/3 — DECIDED 2026-06-02
Multi-seed (seeds 42/7/123, 20 KB, forced rungs), judged on CRC-delivery + sample-space:

| rung | CRC-delivered | goodput (sample-based) | damage% | note |
|------|---------------|------------------------|---------|------|
| R2/3 | **3/3** | 980 / 1340 / 1170 (avg **1163**) | 15–38% | margin to spare |
| R3/4 | **3/3** | 1020 / 1220 / 1410 (avg **1217**) | 25–50% | it_max → 50 cliff |

Both reliable. On the multi-seed average R3/4 is only **~5% faster** (a single-seed run
had flattered it to +14%; across seeds its heavier fade-damage eats the raw-rate edge),
at markedly higher fade damage and tighter LDPC margin. That ~5% isn't worth R3/4's
fragility on a drifting channel → **anchor Good@15 = QPSK R2/3.** R3/4 stays forceable
(peak rate, verified-stable channel only).

Raw runs: `/tmp/sweep_good15_ms/results.csv` (this session).
