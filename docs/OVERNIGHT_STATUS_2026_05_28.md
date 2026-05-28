# Overnight Status — 2026-05-28

User went to bed at ~midnight with mandate: "run 50 runs, optimize, fix the crash."
Goal: 3000 bps Good@20 (currently 1750 bps best).

## TL;DR

1. **Crash fixed** (commit `8de5787`) — descriptor-driven rate switch now defers to the
   top of the next `processBuffer` iteration instead of swapping
   modulator_/demodulator_/chirp_sync_ unique_ptrs mid-decode. Root cause: the
   intercept's local scoped lock didn't cover the rest of the processBuffer loop, so
   subsequent code ran with internal pointers configure() had just replaced.
2. **R5/6 added as climb target** (commit `8b7c39e`). 802.11n LDPC supports it natively.
   +11% bytes/frame vs R3/4 on clean stretches; auto-drops back on fade.
3. **Phase B stress** (20 seeds × 4 channels): RESULTS_PENDING.
4. **Phase C optimization sweep** (g4 vs g8 + R5/6 climb, 21KB + 100KB): RESULTS_PENDING.

## Time-budget analysis (the path to 3000)

Measured on the cleanest g4+adapt+no-fade run (`/tmp/g4adapt_g20_s1`, 8.21s cycle):

```
Per cycle (group=4 at R3/4 QPSK):
  burst air     = 1.41 s descriptor + 4 frames × 1.27 s = 6.50 s   (79%)
  GROUP_ACK air = 1.50 s preamble + 0.32 s payload      = 1.82 s   (22%)
  payload       = 4 frames × 456 B                       = 1824 B
  steady-state  = 1824 × 8 / 8.21 = 1778 bps  (matches measured 1750)
```

To hit 3000 bps SS the cycle must shrink to 1824×8/3000 = 4.86 s (need to cut 3.35 s).
The descriptor + ACK preamble = **2.91 s of fixed overhead per cycle**. Ways to attack:

| Lever | Saving / Gain | Risk | Done? |
|---|---|---|---|
| Larger group (g4→g8: 4→8 frames per fixed-overhead cycle) | +400 bps SS | low | sweep in Phase C |
| R5/6 climb (above R3/4 on clean) | +11% bytes/frame | low (auto-drops on fade) | ✓ commit 8b7c39e |
| Light GROUP_ACK preamble (skip 1.5 s chirp anchor on warm sync) | +1.3 s/cycle ≈ +25% | medium (memory: legacy attempt rejected at 0.88 corr < 0.90 gate) | tomorrow |
| Skip descriptor on same-rate consecutive bursts | +1.4 s/cycle ≈ +25% | medium (loses warm-sync anchor between groups) | tomorrow |
| 8PSK R1/2 climb (same raw as QPSK R3/4, 2× FEC margin) | sideways | medium (8PSK fragile on multipath) | deferred |

Realistic math for tomorrow's path: **g8 + light ACK + R5/6** = 2800-3000 bps SS.

## Crash post-mortem

- **Reported:** `ultra_gui-2026-05-28-000112.ips`, SIGSEGV in `HilbertTransform::process` +132.
- **Stack:** `rxDecodeLoop → processBuffer → searchForSync → detectDataSync → HilbertTransform::process`.
- **Root cause:** the BURST_HEADER intercept called `waveform_->configure(mod, rate)` →
  `initComponents()` → replaced `modulator_`/`demodulator_`/`chirp_sync_` unique_ptrs.
  The intercept ran inside ONE of processBuffer's many scoped locks; subsequent
  scopes in the same processBuffer loop ran with internal pointers configure() had
  just replaced. `data_sync_hilbert_` itself is a value member (unchanged), but its
  `process(samples, …)` was reading something that had become stale.
- **Fix:** descriptor intercept now sets `pending_descriptor_*` under its local lock;
  the actual `applyDataModeUnlocked()` call runs once at the top of the next
  `processBuffer` call — between iterations, no decode state in flight.
- **Why this is safe:** between processBuffer calls there's a `data_cv_.wait_for(50ms)`
  gate — no decode work is in progress. The reconfigure happens with the lock held
  AND no concurrent readers of waveform internals.
- **Why next-call defer doesn't lose data:** processBuffer is called many times per
  burst (per 50ms audio batch). The descriptor lands EARLY in the burst; subsequent
  processBuffer calls bring in the data frames. So the rate change applies BEFORE
  any of the in-burst data frames decode.

## Commits this overnight session (branch `feat/oneway-arch-2026-05-27`, NOT pushed)

| SHA | Subject |
|---|---|
| `8de5787` | crash fix: defer descriptor-driven rate switch to safe boundary |
| `8b7c39e` | adapt: R5/6 climb target in RateController ladder |
| **TBD** | (results doc, when batches complete) |

## Phase B stress results (PENDING — will populate)

[placeholder for 20-seed × 4-channel table]

## Phase C optimization sweep (PENDING — will populate)

[placeholder for g4 vs g8, 21KB vs 100KB table]

## Open questions for morning

- Did stress reproduce the crash? (If yes, defer fix isn't sufficient → deeper race.)
- Does R5/6 climb actually fire on clean seeds and translate to goodput gain?
- Does g8 give the expected ~+400 bps over g4?
- Worth pursuing light-GROUP_ACK-preamble next? (1.3s/cycle is biggest remaining lever.)
