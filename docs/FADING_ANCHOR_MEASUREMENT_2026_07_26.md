# Per-rung FER on FADING — measured 2026-07-26

**What this settles.** The fading FER ordering, whether 8PSK R3/4 should be re-enabled,
and which anchor-table entries are wrong.  Its historical delivered-bps model is not a
current throughput ceiling (see §2). Reproduce with
`tools/sweep_fading_anchors.sh` then `tools/analyze_fading_sweep.py`.

Channel: **ITU-R F.1487 Good (0.1 Hz Doppler / 0.5 ms delay) = the IONOS MPG setting.**
Harness: `build/measure_ack_fer --config data4_full --channel good` (the real
StreamingEncoder/StreamingDecoder). 6 seeds × 24 frames per point, 120 points.

---

## 1. Measured frame error rate

| rung | FER @16 | @18 | @20 | @22 | @24 | floor (FER ≤ 10%) |
|---|---|---|---|---|---|---|
| QPSK R3/4 | 22.9% | 12.5% | **5.6%** | 4.2% | 3.5% | **20 dB** |
| 8PSK R2/3 | 48.6% | 27.8% | 16.0% | **8.3%** | 5.6% | **22 dB** |
| 8PSK R3/4 | 76.4% | 60.4% | 50.7% | 42.4% | **38.9%** | **> 24 dB** |
| 16QAM R2/3 | 85.4% | 71.5% | 51.4% | 41.7% | **30.6%** | **> 24 dB** |

Historical modelled delivered throughput (`raw × pass-rate × 0.593` scheduling, where
0.593 = payload 6.19 s / (payload + 1.41 s sync + 1.79 s turnaround) × 0.90 retx).
The original 2026-07-26 calculation incorrectly treated all 59 occupied carriers as data;
production spacing-8 pilots leave 51 data carriers.  The corrected values below are 51/59
of the old table:

| rung | @20 dB | @24 dB |
|---|---|---|
| QPSK R3/4 | 1785 | 1824 |
| **8PSK R2/3** | **2117** | **2380** ← best value in this historical model |
| 8PSK R3/4 | 1398 | 1733 |
| 16QAM R2/3 | 1633 | 2332 |

---

## 2. What this sweep does—and does not—say about 3000 bps

Under the historical 0.593 scheduling model, the best point is ~2380 bps (8PSK R2/3 at
24 dB); at the bench's dial of 20 it is ~2117 bps.  Nothing reaches 3000 *under those
assumptions*.

That is not a hard current channel ceiling.  Production raw rates with 51 data carriers are
3188 bps for QPSK R3/4, 4250 for 8PSK R2/3, 4781 for 8PSK R3/4, and 5667 for 16QAM R2/3.
Consequently, 8PSK R2/3 has enough raw capacity to exceed 3000 if combined FER and protocol
efficiency exceed 70.6%; higher-order rungs are not mathematically mandatory.  The sweep still
shows why 8PSK R3/4 and 16QAM R2/3 are poor automatic choices: both remain above 30% FER even
at 24 dB.

> Treat the FER ordering as the result.  Do not treat these modelled bps as a measured or
> current ceiling: the model uses an old fixed scheduling factor, assumes independent
> per-frame errors, and predates the current burst/turnaround stack.

---

## 3. 8PSK R3/4: the auto-disable was CORRECT — do not re-enable

It was disabled 2026-07-06 with "the A18 anchor was validated on TRUE AWGN only … re-enable
only after a FADING validation". **This is that validation, and it fails.** 38.9% FER at 24 dB,
never approaching the 10% floor. The rung was attractive on paper — constant-envelope (immune
to the cheap-card compression that craters 16QAM above drive 0.70) and a lower nominal
requirement than 16QAM R2/3 — but it does not survive fading. Keep it disabled.

---

## 4. The anchor table is over-optimistic, and that IS the churn

| rung | Good anchor claims | measured FER at that SNR | verdict |
|---|---|---|---|
| QPSK R3/4 | 20.0 | 5.6% | ✅ correct |
| 8PSK R2/3 | 19.0 | ~20% (interp.) | ⚠ marginal |
| **16QAM R2/3** | **20.0** | **51.4%** | ❌ **badly wrong** |

The 16QAM R2/3 Good anchor is the "EXPERIMENTAL zero-margin anchor" the table itself warns
about — set from one measured floor with no margin. So at `snr_avg ≈ 23` the ladder confidently
selects a rung that fails **half its frames**, craters, demotes, and reaches again.

**That single wrong anchor is the mechanism behind the crater/churn/over-commit behaviour** that
the 2026-07-25/26 campaign kept treating downstream (goodput-graded crater predicate, EVM
demote, anchor-offset bracketing).

**The ladder is already compensating correctly at runtime.** Measured on the rig: the raw pick
averages **6.2** while the commanded rung lands at **5.3** — i.e. the clamps pull it down to
8PSK R2/3, which this sweep independently confirms is the best available rung. The controller
reaches the right answer *despite* the table, by cratering into it every transfer.

**Proposed correction** (applying the floor + 2 dB convention the QPSK rungs use, NOT
zero-margin): 16QAM R2/3 Good → disabled or ≥ 26; 8PSK R2/3 Good → 22-24; QPSK R3/4 Good →
22 (floor 20 + 2). **NOT YET APPLIED — needs an interleaved rig A/B**, because raising anchors
makes the ladder more conservative and the honest expectation is *less churn at similar
goodput*, which must be measured rather than assumed.

---

## 5. Caveats

- Modelled bps uses a historical fixed scheduling factor.  Current keyed-to-wall efficiency is
  trace-dependent, and correlated burst outages are not represented by multiplying raw rate by
  an independent per-frame pass rate.
- `data4_full` is one frame configuration; burst/group behaviour on the rig adds ARQ effects
  this per-frame FER does not capture.
- 6 seeds per point; FER differences under ~5 points are not resolved.
- ITU Good only. Moderate/Poor were not swept and their anchors remain unvalidated on fading.
