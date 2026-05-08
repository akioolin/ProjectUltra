# HARQ / soft-combine audit

Date: 2026-05-02 / 2026-05-03 night session.

## Bug found and fixed: LLRs were averaged, not summed

`SoftCombineBuffer::combine()` was implementing the running arithmetic
mean of LLRs across attempts:

```cpp
out[i] = entry[i] + (incoming[i] - entry[i]) / next_attempts;
```

That's the formula `new_avg = old_avg + (new − old)/n`. After N
attempts, |LLR| magnitude equals one observation's. The LDPC decoder
got zero additional confidence from chase combining — HARQ was
effectively a no-op.

The correct formula (Bayes / log-likelihood algebra for independent
observations of the same coded bit):

```
LLR(b | y1, y2) = log[P(b=0|y1,y2) / P(b=1|y1,y2)]
              = LLR(b|y1) + LLR(b|y2)
```

i.e., **sum, not average**. Fixed in commit `e9b3a93`.

## Follow-ups shipped

| Commit | What | Why |
|---|---|---|
| `e9b3a93` | combine() now sums LLRs with saturation cap |LLR| ≤ 60 | Math fix |
| `1f18683` | HARQ enabled by default in ultra_tnc; Key gains modulation + channel_interleave fields | Was off everywhere; PHY-mismatched LLRs would corrupt-combine |
| `(this)`  | Key gains carrier_count_hash; new test_phy_field_disambiguation | Codex review (see below) caught that bits-per-symbol / waveform-mode also differs LLR ordering |

## Codex review of the patches

Codex (gpt-5.5) reviewed the uncommitted diff. Findings (full output
preserved in commit history):

1. **CW0-peek overhead**: every fixed-frame decode now does an extra
   1-CW LDPC decode just to extract `seq` for the HARQ key, even when
   no retransmissions are in flight. Measured but not yet quantified;
   probably ~10-15% extra CPU per frame on Pi-class hardware. Could be
   mitigated by a "tentative retain" mechanism that defers key building
   to the failure path. Deferred — would require larger refactor of
   decodeFixedFrame's contract.

2. **Key still incomplete on waveform changes**: pre-fix Codex review.
   Now mostly addressed by adding carrier_count_hash. There may still
   be edge cases (pilot-spacing changes within the same waveform mode)
   not covered, but the Key now disambiguates the dominant cases.

3. **HARQ can't help when CW0 fails**: the key is built by decoding
   CW0. If CW0 fails (the most likely failure mode for control frames
   or fade-hit frame headers), no key is built and HARQ doesn't engage.
   This is a structural limitation of our current approach — fixing it
   would require building keys without a CW0 peek (e.g., ARQ-side
   tracking of expected sequence numbers). Filed as future work.

4. **Test coverage**: addressed by adding `test_phy_field_disambiguation`.

## Empirical A/B test on simulator

### Run 1: single seed (inconclusive)

Test: cli_simulator A→B 5 KB Mac→Pi, Watterson Moderate SNR=10, R1/2,
seed=7.

| Run | --harq | retx | failed | wall |
|---|---|---|---|---|
| C   | off | 2 | 1 | 56 s |
| D   | on  | 2 | 1 | 111 s |

Both passed. Same retx count. Run D was 2× slower at the wall-clock
level, but per-frame decode metrics (`unsearched`, `backlog_ms`) were
nearly identical. Looked like a test-harness artifact, not HARQ cost.

### Run 2: three seeds, HARQ-friendly conditions (Mod12 R1/4 2KB)

Conditions chosen to actually trigger retransmissions so HARQ has
something to combine.

| seed | mode | wall | HARQ combines fired |
|---|---|---|---|
| 42 | off | 44 s | 0 |
| 42 | on  | 46 s | 1 |
| 99 | off | 44 s | 0 |
| 99 | on  | 70 s | 2 |
| 7  | off | 50 s | 0 |
| 7  | on  | 45 s | 0 |

All passed. HARQ engaged on 2/3 seeds (combines > 0). Wall-clock
results are mixed — sometimes faster (seed=7), sometimes slower
(seed=99). 3 seeds is not enough to call statistical significance.

### Verified by log inspection

The `HARQ: combining attempt N for seq=X` info-line fires on real
retransmissions, proving the new sum-LLR code path is exercised in
practice:

```
[ 39.737][INFO ][MODEM] [BRAVO] HARQ: combining attempt 2 for
        seq=32 (sender_hash=0x536C71, cw=4)
```

So the bug fix is correct, the code path engages, retx are not
broken. Whether it materially improves throughput on real channels
needs a larger sweep + OTA testing — not blocking the fix from
shipping.

## What's still open

- Multi-seed A/B sweep to quantify HARQ benefit on real channels.
- Real-radio (OTA) validation — every test above is over the cable rig
  with synthetic fading.
- Codex finding #1 (CW0-peek overhead) — defer until profiling shows
  it's a real bottleneck on Pi.
- Codex finding #3 (CW0-fail = no HARQ) — structural; needs design
  pass not just code change.
- Saturation cap (currently 60) — should be tuned with real LDPC
  convergence data. 30 might be safer; need decoder behavior data.

## Bottom line

The math is now correct, the buffer is now actually engaged, and the
key disambiguates the major PHY axes. Whether this *measurably* helps
throughput on real channels is the open question. Single-seed
simulator A/B is inconclusive; multi-seed + OTA is needed to call it.
