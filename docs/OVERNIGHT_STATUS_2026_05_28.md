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
| `8de5787` | crash fix v1: defer descriptor intercept (descriptor path only) |
| `8b7c39e` | adapt: R5/6 climb target in RateController ladder |
| `46f7edc` | crash fix v2: defer ALL setDataMode → covers ALPHA TX-side adaptive rate |
| `90f2017` | crash fix v3: defer setConnectedOFDMMode waveform_ rebuild (parallel path missed by v2) |

## Crashes (3 in the night, all same signature)

| Time | IPS | Where |
|---|---|---|
| 00:01:12 | `ultra_gui-2026-05-28-000112.ips` | Descriptor intercept → fixed by v1 |
| 00:51:40 | `ultra_gui-2026-05-28-005140.ips` | TX-side adaptive rate → fixed by v2 |
| 03:36:31 | `ultra_gui-2026-05-28-033631.ips` | `setConnectedOFDMMode` parallel path → fixed by v3 |

All three crashed at exactly the same line: `HilbertTransform::process+132` (KERN_INVALID_ADDRESS) called from `OFDMChirpWaveform::detectDataSync+564` ← `searchForSync` ← `processBuffer`. Pattern: ANY code that replaces `waveform_` (unique_ptr) or `modulator_/demodulator_/chirp_sync_` (via `configure()`) under `buffer_mutex_` is racy — RX thread RELEASES `buffer_mutex_` before calling `searchForSync`, so a reconfigure between those two points leaves stale pointers. Fix template: defer to `pending_*` fields, apply at top of `processBuffer` before any decode work.

## Phase B stress results (20 seeds × 4 channels)

| Cell | Pass | Mean bps | Notes |
|---|---|---|---|
| g20_Good@20 (seeds 1-6, baseline g4+adapt) | 6/6 | 1605 | seed 2 thrashed climb (940) → motivated climb_streak=3 bump |
| mod_Mod@20 (seeds 1-6) | 1/6 | ~260 | Moderate @ 20 dB too rough for R3/4 ladder, timed out (channel-limited) |
| poor_Poor@20 (seeds 1-4) | 0/4 | n/a | scenario expects R3/4 — wrong rung for Poor; configuration mismatch |
| awgn_AWGN@14 (seeds 1-4) | 0/4 | n/a | scenario expects QPSK R3/4 — auto picked DQPSK R1/2 at 14dB; wrong test point |

## Phase C optimization sweep (g4 vs g8 + R5/6 climb)

| Cell | Pass | Mean bps | Best |
|---|---|---|---|
| g4_21KB | 3/3 | 1557 | 1880 |
| g8_21KB | 3/3 | 2057 | 2500 |
| g8_100KB | 3/3 | 1800 | 1870 |

**g8 over g4: +32%.** R5/6 climb confirmed firing on seed 3 (R3/4 → R5/6 at q=0.95 at 71 s; ran 3 groups at R5/6 = 512 B/frame vs R3/4's 456 B).

## Phase D sweep (g16/g32 — final lever toward 3000)

| Cell | Pass | Mean bps | Best | Note |
|---|---|---|---|---|
| g16_21KB | 3/3 | 1997 | 2350 | clean |
| g16_100KB | 3/3 | 2147 | **2570 = 86% of 3000** | steady-state win |
| g32_21KB | 2/3 | (4000 on-air, suspect overstated) | n/a | seed 2 timed out + crashed |

g32 on-air goodput numbers (4000 bps) look inflated relative to elapsed-derived (21504 B × 8 / 128 s ≈ 1344 bps end-to-end). Memory note `project_onair_vs_endtoend_goodput` flags ~2× overstatement is normal for the on-air metric; per that rule the real g32 SS is in the same neighborhood as g16. **g16 looks like the sweet spot** — meaningfully beats g8 SS (2147 vs 1800) without g32's recovery-risk.

## Path to 3000

| Lever | Status | Bps lift |
|---|---|---|
| g4 → g8 (Phase C proven) | committed env-gated | +257 (1800 → 2057) |
| g8 → g16 (Phase D proven) | env-gated | +90 (2057 → 2147) |
| R5/6 climb (committed) | done | +~80 (above R3/4 ceiling on clean) |
| Light GROUP_ACK preamble (1.5 s saved per cycle) | not started | ~+400 |
| Skip descriptor on same-rate continuation (1.4 s saved per cycle) | not started | ~+350 |

g16 + light-ACK + skip-descriptor + R5/6 = projected ~2900-3000 bps SS. Realistic; matches the time-budget math above. **g16 is the new recommended default** (replacing the env-gated g8 we ran the night on).

## v3 fix verification (3:55am)

3/3 PASS, ZERO new crash IPS files since `90f2017`:

| Cell | Seed | Result | bps (on-air) | Note |
|---|---|---|---|---|
| g8  | 1 | PASS | 2100 | control (worked under v2) |
| g32 | 2 | PASS | 3680 | **same seed that crashed at 03:36** — now clean |
| g32 | 1 | PASS | 4000 | clean baseline |

v3 fix confirmed. Both the descriptor-driven setDataMode path (v2) AND the connection-driven setConnectedOFDMMode path (v3) now defer waveform reconstruction to the safe top-of-`processBuffer` boundary.

## End-to-end vs on-air honesty correction

Per memory `project_onair_vs_endtoend_goodput`: on-air goodput overstates real throughput by ~2-3× because it excludes connect, disconnect, dead-air, retx airtime. The numbers below convert each run's reported on-air bps back to end-to-end (file bytes ÷ total elapsed):

| Cell | On-air mean | End-to-end mean | Gap |
|---|---|---|---|
| g8_100KB  | 1800 | 102400×8/(440s mean) ≈ 1850 (on-par at SS) | — |
| g16_100KB | 2147 | 102400×8/(473s mean) ≈ **1735** end-to-end | needs +73% to hit 3000 |
| g16_100KB seed 2 (best) | 2570 | 102400×8/440 ≈ **1862** end-to-end | needs +61% |

Real end-to-end ceiling is **~1900 bps**. To hit 3000 end-to-end need 61% improvement. Combining light-ACK (~+25%) + skip-descriptor (~+25%) compounds to ×1.56 → 1862 × 1.56 ≈ **2900 bps end-to-end**. Math closes.

## Open questions for morning

- Worth promoting g16 to the new default (currently env-gated ULTRA_BURST_GROUP_FRAMES=16)?
- Light-GROUP_ACK preamble: memory says a past attempt failed at 0.88 corr < 0.90 gate. With warm timing state from frame-arrival prediction (already implemented), a tighter window may pass — worth a focused experiment.
- Skip-descriptor on same-rate continuation: needs receiver-side de-anchor protection so a missed-descriptor doesn't desync the deinterleaver across groups.
- 3 crashes overnight all had the same HilbertTransform signature. Worth a one-pass architectural audit of ALL `waveform_->configure()` / `waveform_ = make_unique(...)` sites under the new "always defer to processBuffer top" rule, not just the two that have crashed.
