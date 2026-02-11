# Alpha Release Gate

Last updated: 2026-02-11

## Purpose
This file is the source of truth for alpha readiness.
If chat context is compacted, use this file to resume without losing targets.

## Alpha Definition
ProjectUltra alpha means:
- Reproducible reliability on simulated HF channels (seeded, documented commands).
- No known critical data-loss bugs in connection/ARQ/control path.
- Default mode ladder only uses modes that pass reliability gates.
- A user can build and run test/transfer flows from docs without hidden steps.

## Current Status (Summary)
- Core protocol works end-to-end in simulator.
- ARQ observability is in place (retransmit-cause split, ACK filtering/coalescing counters).
- OFDM SR-ARQ window is set to 4 (aligned with burst interleaver group size), which materially reduced R2/3 file-transfer retransmission tails.
- D8PSK remains experimental for fading until its dedicated control-path metrics pass.

### Latest Full Gate Run
- Date: 2026-02-11
- Command:
  - `scripts/run_alpha_gate.sh --seed-start 42 --seed-count 30 --out-dir /tmp/alpha_gate_full_w4`
- Result: **PASS**
- Report:
  - `/tmp/alpha_gate_full_w4/summary.md`
  - `/tmp/alpha_gate_full_w4/results.csv`

## Required Release Gates
All gates below must pass before alpha tag:

1. Regression safety (must pass):
- `DQPSK R1/4`, SNR 10, good fading, 30 seeds: 30/30 delivery.
- `DQPSK R1/4`, SNR 10, moderate fading, 30 seeds: >= 29/30 delivery, no persistent disconnect deadlock.
- `DQPSK R1/2`, SNR 20, good fading, 30 seeds: 30/30 delivery.

2. Throughput baseline (must pass):
- `DQPSK R2/3`, SNR 20, good fading, 30 seeds, message test:
  - 30/30 delivery
  - average first-attempt success >= 90%
  - average retransmissions <= 2.5
- `DQPSK R2/3`, SNR 20, good fading, 30 seeds, file 2048B test:
  - 30/30 file integrity pass
  - average retransmissions <= 4
  - timeout count trend stable (no long-tail explosive seeds)

3. Control-path robustness (must pass):
- No stale ACK accepted as disconnect ACK.
- ACK/SACK decode path shows no frame-size mismatch fallback loops.
- Disconnect completes in >= 95% of 30-seed runs (data success remains primary; disconnect non-fatal but tracked).

4. Mode policy guardrails (must pass):
- Default auto-rate excludes modes that fail fading reliability gates.
- `D8PSK` and coherent high-order modes remain Expert/forced until validated.

5. Release hygiene (must pass):
- `README.md` and `docs/TESTING_METHODOLOGY.md` commands match current CLI behavior.
- `docs/KNOWN_BUGS.md` updated with open critical issues.
- Build instructions verified on a clean clone.

## Work Plan
1. Add deterministic gate harness script
- Add `scripts/run_alpha_gate.sh` to run seeded matrices and emit compact summaries.
- Store logs in `/tmp` and summary in one markdown report.

2. Finish ARQ control-path tuning using A/B toggles
- Keep new observability counters.
- Add runtime toggles for ACK dedup, ACK repeat coalescing, hole-probe.
- Run 30-seed A/B and keep only variants that improve median and p90 retrans/timeouts.

3. Freeze conservative default rate ladder
- Keep robust defaults for normal users.
- Keep aggressive modes behind Expert force until their own 30-seed gates pass.

4. OTA readiness checklist
- Define one low-risk OTA profile (modulation/rate/power/bandwidth assumptions).
- Run short scripted OTA smoke tests and capture failure classes.

5. Alpha cut
- Update changelog.
- Tag alpha only when all gates above pass.

## Tracking Template
Use this for each gate run:

```
Gate:
Command:
Seeds:
Pass/Fail:
Delivery:
Avg retx:
Avg timeouts:
Notes:
Log path:
```

## Harness
Run the deterministic gate harness:

```bash
scripts/run_alpha_gate.sh --seed-start 42 --seed-count 30
```

Quick smoke run:

```bash
scripts/run_alpha_gate.sh --quick
```

Outputs:
- Markdown summary: `/tmp/alpha_gate_<timestamp>/summary.md`
- CSV metrics: `/tmp/alpha_gate_<timestamp>/results.csv`
- Per-seed logs: `/tmp/alpha_gate_<timestamp>/logs/*`

## Alpha Tag Step
After a passing full gate run:

```bash
git tag -a v0.2.0-alpha.1 -m "ProjectUltra v0.2.0-alpha.1"
git push origin v0.2.0-alpha.1
```
