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
| QPSK R3/4   | —    | **20** | —      | **Good@20 measured 2026-06-02 (5/5 clean; below)** |
| QAM16 …     | —    | —    | —        | clean-channel throughput; TBD (loses to QPSK R3/4 on Good) |
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

### Good@20 → QPSK R3/4 — DECIDED 2026-06-02 (closes the GOOD column)
Forced rungs at Good@20, 20 KB, CRC + sample-space. Single-seed scan then 5-seed confirm:
- **QPSK R3/4: 5/5 CRC-clean, 1240–1750 bps (avg 1630), damage 0–18% (avg 8%), it_max <=10.**
  The extra 5 dB over Good@15 moves R3/4 OFF the cliff (Good@15 had a 50%-damage / it_max-40
  seed; Good@20 stays clean across seeds). The Good column is now R1/2@10 → R2/3@15 → R3/4@20.
- QPSK R5/6 (single seed): 1480 bps, **33% damage**, it_max 17 — LOSES to R3/4. Thin FEC (17%
  redundancy) < Good's ~23% fade-erasure → below the cliff; raw-rate gain eaten by resends.
  (Contradicts the old "R5/6 ~+10% over R3/4 on Good@20" note, which ignored fade-recovery cost.)
  **→ R5/6 RETIRED from the auto ladder 2026-06-17** on this evidence (rate_controller.hpp ladder
  is now {R1_4,R1_2,R2_3,R3_4}; R5_6 stays a forcible `ULTRA_FORCE_DATA_RATE` probe only). Above
  QPSK R3/4 the next throughput rung is a MODULATION step (QAM16 R2/3, `ULTRA_QAM16_CLIMB`), not a
  thinner code. See docs/CHANGELOG.md 2026-06-17.
- 16QAM R1/2 (single seed): 1190 bps, 17% damage, it_max 29 — LOSES to R3/4. The dense
  constellation eats freq-selective nulls (decodability gate); slower despite 2.0 vs 1.5 eff
  bits/sym. 16QAM is a clean/AWGN rung, not a Good-fading one.

Principle confirmed (redundancy vs ~23% Good fade-erasure): R5/6=17% (broken) < R3/4=25%
(clears) < R2/3=33% (comfortable). R3/4 is the Good@20 sweet spot.
Refine TODO: bracket Good@18 to find where R3/4 crosses cliff→clean (anchor could drop 20→~18).
Raw runs: `/tmp/sweep_r34_good20_ms/results.csv`, `/tmp/sweep_good20_20k/`, `/tmp/sweep_good20_r56/`.
