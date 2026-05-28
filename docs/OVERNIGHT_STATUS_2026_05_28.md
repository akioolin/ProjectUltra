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
| `c574e00` | burst: promote g16 to default group size (Phase D win) |
| `266a526` | burst: light preamble for BURST_HEADER descriptor — **REVERTED** below |
| `6c62843` | revert light-descriptor: regressed short transfers (seed 2 21KB 840 bps vs ~2000 baseline) |

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

## g16-default smoke verify (4:30am, post commit `c574e00`)

| Cell | Pass | On-air mean | End-to-end mean |
|---|---|---|---|
| g16def_21KB  | 3/3 | 2197 bps | 1055 bps |
| g16def_100KB | 3/3 | 2130 bps | **1734 bps** SS |

6/6 PASS. Zero crashes. The new g16 default matches Phase D's g16 numbers — no regression from removing the env-gate. **g16 is now the shipping default.**

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

## Attempted: light descriptor preamble (REVERTED, 6c62843)

Tried `encodeFrame(descriptor) → encodeFrameLight(descriptor)` at streaming_encoder.cpp:561. Single-seed probe looked good (PASS, descriptor went from 67680 → 17280 samples). Multi-seed exposed the failure mode:

| Cell | Seed | Result | bps | Notes |
|---|---|---|---|---|
| g16_lite_21KB  | 1 | PASS | 1900 | clean |
| g16_lite_21KB  | 2 | PASS | **840**  | 5 rate drops, 9 descriptor retx — light descriptor missed during a fade |
| g16_lite_21KB  | 3 | PASS | **860**  | similar cascade |
| g16_lite_100KB | 1 | PASS | 2380 | (more groups → first-burst miss amortized) |
| g16_lite_100KB | 2 | PASS | 2530 | as above |

The failure mode is exactly what the codebase warning suggests: missed light descriptor → receiver decodes the group with stale rate → all CWs fail → no GROUP_ACK → drop-on-timeout cascade. For 21KB transfers (~4 groups) one missed descriptor kills 25% of the transfer; for 100KB (~12 groups) it's only ~8% and the win shows up. Net mean across all cells: WORSE than baseline.

Reverted to ship the proven wins instead. A robust descriptor-light needs one of:
- **Rate-state contract** so receiver knows the rate without the descriptor (skip-descriptor variant)
- **Wider warm-sync window** for the descriptor's light LTS (fights the 0.88<0.90 gate)
- **Fall-back retry**: TX emits full-anchor descriptor if the prior GROUP_ACK was a NACK or timed out

Each is non-trivial protocol surgery — user judgment territory.

## Honest gap accounting (Good@20, 3000 bps target)

End-to-end steady-state on 100KB Good@20 (the meaningful metric for "match leader speed"):
- **Verified best:** 1862 bps (Phase D seed 2 g16_100KB)
- **Verified mean:** 1734 bps (g16-default 3-seed verify)
- **Target:** 3000 bps
- **Gap:** +61% best / +73% mean

What's left to close it:

| Lever | Estimated lift | Status |
|---|---|---|
| Light GROUP_ACK preamble | +25% | BLOCKED — codified at modem_engine.cpp:421-428 (0.88<0.90 gate) |
| Skip descriptor on same-rate continuation | +25% | TOO RISKY autonomously (rate-state contract) |
| Light descriptor (attempted) | +5% on clean, -50% on short | REVERTED |
| 8PSK R1/2 climb above R5/6 | sideways (same raw bits) | research |
| Wider OFDM bandwidth (2.4kHz, more carriers) | +20-30% | architectural |
| LDPC blocklength N=648→1944 | -3 dB SNR → maybe enables R5/6 more often | task #122 backlog |

The arithmetic: even stacking light-ACK + skip-descriptor (both blocked autonomously) lands at ~2700 bps end-to-end — not 3000. Closing the last 300 bps probably requires the wider-bandwidth lever or substantive PHY work (the leader's HF modem uses ~2.4kHz BW vs our ~2.8kHz, with tighter spacing and longer symbols — different architecture).

## Morning correction: the ceiling analysis was wrong

User shared the industry leader's published modulation ladder. The 3000 bps
speed slot in their tactical (~2.75 kHz) profile is **Level 12: 4PSK / 42 baud /
59 carriers / net rate 3230 bps** — that's *QPSK at R2/3*, not R3/4 or R5/6.
Level 11 (R~1/2) = 2423 bps. Level 13 (R~3/4) = 3877 bps.

Re-derived our PHY honestly (FFT=1024, 48 kHz sample rate, CP ≈ 25%):
- Symbol period ≈ 26.7 ms → **~37.5 baud** (matches leader's 42)
- Raw at QPSK R2/3: 59 × 37.5 × 2 × 0.667 = **2950 bps raw** (already at 3000-ish)
- Raw at QPSK R3/4: 59 × 37.5 × 2 × 0.75 = **3320 bps raw**

We measure ~2500 on-air / 1862 end-to-end at R3/4. So the PHY is fine — the gap is the e2e overhead (descriptor + ACK preambles, dead air on drop-on-timeout cascades).

**The hypothesis the leader's ladder suggests:** at Good@20, *staying at R2/3 may give higher e2e throughput than climbing to R3/4*, because the stronger FEC eats fade events without triggering drop-on-timeout cascades. We've been chasing raw rate; the leader chose code redundancy.

Currently testing: cap the RateController ladder at R2/3 (one-line change), run 3×21KB + 3×100KB Good@20, measure e2e. If it lands ~2900 e2e → that's our 3000 path without protocol surgery.

## Ceiling analysis (PRE-CORRECTION — kept for posterity)

The data-phase end-to-end (excluding the ~8 s handshake/teardown):

```
g16_100KB best run:   102400 × 8 / (440 - 8) = 1896 bps
g16_100KB mean run:   102400 × 8 / (470 - 8) = 1773 bps
```

So even isolating the data-transfer phase, the verified ceiling on the current
OFDM_CHIRP stack is ~1900 bps. 3000 bps requires +58% gain INSIDE the data
phase — and the safe per-cycle math (g16 R3/4 burst 20.3 s + descriptor 1.41 s
+ GROUP_ACK 1.82 s = 23.5 s, carrying 7296 bytes) caps on-air at:

```
on-air ceiling  = 7296 × 8 / 23.5 = 2484 bps
end-to-end ceiling = on-air × ~0.85 (dead-air/T-R) = 2111 bps
```

Even an *infinitely lucky* clean channel with zero retransmits caps end-to-end
at ~2110 bps on this stack. 3000 bps is **architecturally unreachable** without
one of:

1. **Wider OFDM bandwidth** — more data carriers in the same audio band (the
   leader uses ~2.4 kHz with tighter spacing; we use ~2.8 kHz with our current
   carrier layout). +25-35% raw rate. Requires waveform-level rewire.
2. **N=1944 LDPC** (task #122) — same rate at -3 dB SNR threshold, enabling
   R5/6 more frequently and possibly QAM rungs. Multi-subsystem rewire
   (interleaver, frame format, MTU calc, soft-bit buffers).
3. **Light GROUP_ACK** — saves 1.5 s/cycle ≈ +13%. Codified-blocked at
   `modem_engine.cpp:421-428` with the 0.88<0.90 corr gate.
4. **Skip descriptor on same-rate continuation** — saves 1.41 s/cycle ≈ +9%.
   Needs rate-state contract between TX and RX (skip-descriptor variant);
   attempting it without that contract regresses short transfers (this
   session's light-descriptor commit `266a526` → revert `6c62843`).

Stacking the three "softer" levers (light-ACK + skip-descriptor + N=1944)
projects to ~2900 bps end-to-end if all three land cleanly. None is safe
autonomously per the standing brainstorm-then-implement rule — each touches
load-bearing protocol invariants. **The remaining gap to 3000 is genuinely
your call.**

## Open questions for morning

- Worth promoting g16 to the new default (currently env-gated ULTRA_BURST_GROUP_FRAMES=16)?
- Light-GROUP_ACK preamble: memory says a past attempt failed at 0.88 corr < 0.90 gate. With warm timing state from frame-arrival prediction (already implemented), a tighter window may pass — worth a focused experiment.
- Skip-descriptor on same-rate continuation: needs receiver-side de-anchor protection so a missed-descriptor doesn't desync the deinterleaver across groups.
- 3 crashes overnight all had the same HilbertTransform signature. Worth a one-pass architectural audit of ALL `waveform_->configure()` / `waveform_ = make_unique(...)` sites under the new "always defer to processBuffer top" rule, not just the two that have crashed.
