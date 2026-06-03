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

**IMPLEMENTED 2026-06-02** in `kCoherentLadder` (`src/protocol/waveform_selection.hpp`) — the
single ladder replacing the old 4-gate-array machinery. Current table:

| rung        | AWGN | GOOD | MODERATE | basis |
|-------------|------|------|----------|-------|
| QPSK R1/4   | 8    | 10   | 14       | = OFDM entry floors (the floor rung) |
| QPSK R1/2   | 10   | 10   | 18       | Good@10 measured + AWGN≤Good monotonicity (was 12/14) |
| QPSK R2/3   | —    | **15** | —      | **Good@15 measured 2026-06-02 (below)** |
| QPSK R3/4   | —    | cliff | —       | Good = fade-matched cliff (below); SNR-20 probe TODO |
| QAM16 …     | —    | —    | —        | clean-channel throughput; TBD |
| 8PSK / QAM8 | x    | x    | x        | retired from the OFDM band (thread C to restore) |

Floor lowering (2026-06-02): AWGN entry 10→8 (R1/4 clean @ AWGN 8: 0% dmg, it_max 4 — floor
likely lower, single seed); GOOD entry 12→10 (R1/2 reliable @ Good 10, 5/5 multi-seed). R1/2
AWGN/GOOD 12/14→10 (monotonicity: R1/2 @ Good 10 reliable ⇒ ≤10 on AWGN, the easier channel).
At AWGN/Good 8–9 the floor rung is R1/4; R1/2 from 10. MODERATE untouched (unmeasured-lower).
Sub-8 AWGN + sub-10-Good R1/4 (marginal @ Good 8, it_max 48) = further-probe TODO.

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

### Good@10 → OFDM is RELIABLE below the current floor — MEASURED 2026-06-02 (not yet a decided anchor)
The current Good OFDM entry floor is 12 dB; it came from a 2026-05-21 *forced-waveform,
PAPR-OFF* recalibration (flagged "auto re-verification deferred, ±1 dB, pre-burst-transport")
— stale-scar-tissue vintage, slated for revalidation. Forced QPSK R1/2, 20 KB, 5 seeds
(42/7/123/99/256), CRC-verdict + sample-space:
- **5/5 CRC-clean** — even the worst-fade seed (99: 54% damaged, 9 NACKs, it_max 40) delivered.
- goodput **400–490 bps (avg 456, ~20% spread)** — tight despite 12–54% damage, because at
  Good@10 you're fade-recovery-bound (SR resends only failed frames, so heavy damage ≈ modest
  throughput cost). R2/3 @ Good 10 also delivered (~450 bps, 1 seed) — same fade-recovery floor.
- So OFDM does NOT run out at 10 dB; the floor rung (R1/4) will go lower still. The 12 dB
  entry floor is **conservative** vs the PHY. WHETHER to lower it is a separate call (OFDM
  ~450 bps at Good@10 may or may not beat whatever MC-DPSK does there — MC-DPSK unmeasured/
  unworked, deliberately out of scope for now). Floor decision: USER, when ready.
Raw runs: `/tmp/sweep_r12_good10_var/results.csv`.
