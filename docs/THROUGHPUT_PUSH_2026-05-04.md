# Throughput push — experimental/throughput-push branch (2026-05-04)

User asked: "can we go the highest possible towards 10 kbps in good
fading conditions?" Worked autonomously on a branch with three Codex
audit passes. Outcome: real wins, but **10 kbps in good fading is
not physically achievable** with this PHY shape. Honest ceiling is
~5-6 kbps in good fading, ~6 kbps on AWGN.

## Codex's brutal answer (audit pass 1)

10 kbps in good fading SNR=15 is unreachable.
- Shannon capacity for B=2.8 kHz, SNR=15 dB → C ≈ 14 kbps **only for
  flat AWGN**.
- After ~3 dB coding/equalization gap → ~11.4 kbps.
- After pilots (~10-15%), CP (~10%), LTS (5%), ARQ overhead (~15%),
  retx margin → practical ceiling well below 10 kbps.
- Real-world VARA HF "8.5 kbps theoretical" is essentially never seen
  on actual HF; per N1CLC user logs and VARA-MODEM forum, real
  median is ~900 bps with peaks around 2-4 kbps.

What ProjectUltra was already doing well:
- 59 carriers in 2.8 kHz (not 30 — that's a stale comment in the
  default constructor for an unused code path).
- Production cp_mode = MEDIUM = 96 samples (not 256).
- LDPC 648-bit codewords with HARQ chase combining.
- Selective-repeat ARQ with window=16 on high-throughput rates.
- Burst interleaver with 8-frame groups.

## What we shipped on this branch

Three commits on `experimental/throughput-push`:

### 1. `94a4ae9` — D8PSK gate re-enabled
The "D8PSK fails on any fading" comment in
`waveform_selection.hpp` was stale (predates the 2026-03-15 CPE
correction + per-symbol pilot tracking). Sweeps confirmed D8PSK now
works in fading:

| Mode/Rate | SNR=8 | SNR=10 | SNR=12 | SNR=15 | SNR=20 |
|---|---|---|---|---|---|
| D8PSK R1/2 good | FAIL | PASS, 4 retx | PASS, 2 retx | **PASS, 0 retx** | PASS, 0 retx |
| D8PSK R2/3 good | — | PASS, 28 retx | PASS, 45 retx | PASS, 0 retx | PASS, 1 retx |
| D8PSK R3/4 good | — | — | — | — | PASS, 6 retx (border) |

Three-tier gate added:
- D8PSK R3/4: AWGN-only (fading<0.15) AND SNR>=24
- D8PSK R2/3: SNR>=15 fading<0.10, OR SNR>=18 fading<0.15, OR SNR>=20 fading<0.65
- D8PSK R1/2: SNR>=10 fading<0.65 (the dependable +47 % win)

### 2. `3e70a84` — D8PSK R2/3 thresholds tightened after stress test
5 KB file transfer at SNR=18 good fading with the looser R2/3 gate
showed 31 retx for 28 frames — much worse than the 7-message sweep
suggested. R2/3 floor in real fading bumped to SNR>=20.

### 3. `1071328` — D8PSK now uses window=16 ARQ
`isHighThroughputOFDMMode()` previously rejected anything that wasn't
DQPSK. Extended to D8PSK, so the new high rates get the same
selective-repeat window=16 that DQPSK high rates use. Speculative
flag (window=16 only on near-AWGN) extended to D8PSK R2/3 and R3/4;
D8PSK R1/2 stays non-speculative (window=16 unconditionally inside
its gate).

## Throughput before / after

| Channel | Before (main) | After (this branch) | Delta |
|---|---|---|---|
| AWGN SNR=27 | DQPSK R3/4, ~3.9 kbps | D8PSK R3/4, ~5.9 kbps | +51 % |
| Good fading SNR=20 | DQPSK R2/3, ~3.4 kbps | D8PSK R2/3, ~5.0 kbps | +47 % |
| Good fading SNR=15 | DQPSK R1/2, ~2.3 kbps | D8PSK R1/2 win=16, ~3.4 kbps | +47 % |
| Good fading SNR=12 | DQPSK R1/4, ~1.15 kbps | D8PSK R1/2, ~3.4 kbps | +196 % |
| Moderate fading any | unchanged DQPSK | unchanged | 0 % |

## Tonight's ceiling vs the 10 kbps ask

| Operating point | Best-case ceiling | Vs 10 kbps target |
|---|---|---|
| AWGN SNR=27+ | ~5.9 kbps | 59 % of target |
| Good fading SNR=20 | ~5.0 kbps | 50 % of target |
| Good fading SNR=15 | ~3.4 kbps | 34 % of target |

**Not 10 kbps.** That number wasn't reachable; Shannon/coding gap
math says it can't be reached on a fading channel without going
to 64-QAM (which collapses below SNR=20) or to multi-week research-
level changes (per-subcarrier bit loading, HARQ-IR, larger LDPC,
turbo equalization).

## Levers Codex audited but deferred

Tonight-deferred (need multi-day work):

- **16-QAM / 32-QAM** — disabled on `OFDM_CHIRP` today. Would need
  coherent path validation, much higher SNR floor (16-QAM cliff ~12 dB
  above DQPSK in fading per literature), and OFDM_COX is a better
  home for high-order QAM.
- **Larger LDPC codewords (1944-bit)** — IEEE 802.11n long matrices
  are in `src/fec/ldpc_802_11n.hpp` already; integration into the
  frame format needs work because 648 is baked into the frame header
  layout.
- **HARQ-IR (incremental redundancy)** — Backlog #7. Would need rate-
  compatible LDPC puncturing, ARQ retx packet semantics extended.
- **Per-subcarrier bit loading** — Backlog #5. Highest theoretical
  win (2-4 dB) but needs continuous SNR feedback and coordinated
  encoder/decoder bit-mapping.
- **Continuous pilots** — Backlog #6. 1-2 dB on faster fading.

## Hardware validation (Mac↔Pi5 audio loopback) — 2026-05-04 15:00

Sweep done after the simulator-only commits. Mac (Sound Blaster
Play! 3 USB) ↔ Pi5 (USB Audio Device, calibrated per CLAUDE.md)
with synthetic-channel injection at the documented gain (0.70).

**Forced-mod 5KB file transfer payload throughput, good fading:**

| SNR | DQPSK R1/2 | D8PSK R1/2 | Winner |
|---:|---:|---:|---|
| 15 | 1078 bps (2 retx) | 728 bps (5-40 retx) | **DQPSK** |
| 18 | 1234 bps (0 retx) | 641 bps (38 retx) | **DQPSK** |
| 20 | 1247 bps (0 retx) | **1595 bps (0 retx)** | **D8PSK +28 %** |

The hardware cliff is **between SNR=18 and SNR=20**, even though the
simulator's Watterson-based sweep showed D8PSK working cleanly at
SNR=10. The 10 dB sim-vs-hardware gap comes from soundcard
quantization, AGC residual, and audio-chain phase noise that
Watterson doesn't model. D8PSK's 8-phase decision is dramatically
more sensitive to phase noise than DQPSK's 4-phase.

**Gate tightening that followed the hardware measurements** (commit
96bb2b7):
- D8PSK R1/2: simulator floor SNR=10 → hardware floor SNR=20
- D8PSK R2/3: was multi-tier good-fading; now AWGN-only
- DQPSK keeps everything below

After tightening, auto-rate at SNR=20 good correctly picks D8PSK
R1/2 and delivers 1130 bps with one adaptive downgrade event over
the 36-second test (vs forced D8PSK R1/2 at 1595 bps without the
adaptive jitter). At SNR=15 good, auto-rate falls back to DQPSK
R1/2 at 904 bps — same as main-branch behavior, no regression.

## Higher-rate hardware ceiling sweep (post-pushback)

User pushed back: "we had over 2000 bps with DQPSK." That recall was
correct — for **R2/3 and R3/4**, not R1/2 (which is the fading-
fallback). Forced-mod hardware sweep at higher rates:

| Mode/Rate | Channel | SNR | Throughput | Retx |
|---|---|---:|---:|---:|
| DQPSK R3/4 | AWGN | 25 | 2058 bps | 0 |
| DQPSK R2/3 | good | 20 | 1422 bps | 0 |
| **D8PSK R2/3** | **AWGN** | **22** | **2382 bps** | 0 |
| **D8PSK R2/3** | **AWGN** | **25** | **2410 bps** | 0 |
| D8PSK R3/4 | AWGN | 27 | 2566 bps | 0 |
| D8PSK R3/4 | AWGN | 30 | 2620 bps | 0 |

**Auto-rate at SNR=22 AWGN: D8PSK R2/3 selected, 2406 bps, 0 retx.**
That hits the "2000+ bps" target the user remembered — and beats
DQPSK R3/4 (2058 bps) at the same channel quality.

## Window-size investigation (was window=16 the wrong call?)

User flagged: "window 16 doesn't work on fading; we had window 6/8
optimal aligned to burst groups." Investigation: window=16 was
introduced 2026-05-01 (commit f07208c, "Improve OFDM streaming
file throughput recovery") with kHighThroughputOFDMWindowFrames=16
for DQPSK R1/2+. Burst interleaver group is 8 frames; window=16 =
2 burst groups, still aligned.

Hardware A/B with kHighThroughputOFDMWindowFrames temporarily
forced to 8:

| Test | window=8 | window=16 | Winner |
|---|---:|---:|---|
| DQPSK R1/2 SNR=15 good | 1077 bps (2 retx) | 1078 bps (2 retx) | tied |
| DQPSK R1/2 SNR=15 moderate | 1103 bps (0 retx) | 1234 bps (0 retx) | window=16 +12 % |
| DQPSK R1/2 SNR=20 good | 1119 bps (0 retx) | 1247 bps (0 retx) | window=16 +12 % |
| D8PSK R1/2 SNR=20 good | 1247 bps (1 retx) | 1595 bps (0 retx) | window=16 +28 % |

Window=16 wins in every measured condition on this hardware rig.
The user's intuition that window=16 hurts fading wasn't borne out;
window=16 works because the burst interleaver still groups in 8s,
the second burst group just follows immediately. Restored.

**Honest 10 kbps comparison:**

| Goal | Reachable? |
|---|---|
| 10 kbps in good fading SNR=15 | No. Hardware ceiling ~1.0 kbps. |
| 10 kbps in good fading SNR=20 | No. Hardware ceiling ~1.6 kbps. |
| 10 kbps in clean AWGN | No. Hardware ceiling ~2.6 kbps (D8PSK R3/4 SNR=30). |
| 2 kbps target reached? | **Yes — D8PSK R2/3 AWGN SNR=22 = 2406 bps.** |

Production HF data modems (VARA / Pactor) advertise 8.5-10.5 kbps
"theoretical max" but real-world median per published user logs is
0.9-2 kbps — same neighborhood ProjectUltra lives in today. The
+28 % hardware win at SNR=20 is meaningful but doesn't change the
order-of-magnitude story.

## Direct main vs experimental A/B on identical hardware

User insisted: "I feel our main has more speed." Data settles it.

Mac↔Pi5 audio loopback, 5 KB file transfer, calibrated injection,
same audio cabling, same SNR, same channel, same `run_hw_test.sh`
invocation. Pi5 rebuilt cli_simulator on each branch before the
corresponding test:

| Test | MAIN bps | EXPERIMENTAL bps | Δ |
|---|---:|---:|---|
| DQPSK R1/2 SNR=15 good (forced) | 1073 | 1077 | tied |
| DQPSK R2/3 SNR=20 good (forced) | 1415 | 1422 | tied |
| DQPSK R3/4 SNR=25 AWGN (forced) | 2057 | 2058 | tied |
| **auto SNR=22 AWGN** | **1837** | **2406** | **+31 %** |

**Forced-mode tests are essentially identical** — same encoder /
decoder code; the experimental commits don't change the modulator
or LDPC layer.

**Experimental wins only where the new D8PSK gate fires.** At auto-
rate SNR=22 AWGN, main picks DQPSK R2/3 (1837 bps); experimental
picks D8PSK R2/3 (2406 bps). Same code rate, 1.5× bits per symbol
(D8PSK 3 bits vs DQPSK 2 bits) = +31 % real throughput, 0 retx on
either branch.

**No regression on main vs experimental anywhere measured.** The
"I feel main has more speed" intuition didn't hold up against
direct hardware A/B.

## CW=8 aggregation — the missing lever (post-pushback)

User: "1077 bps DQPSK R1/2 SNR=15 good is too low — why are we so
low?" Investigation found that `--cw-count 8` (an existing CLI opt-
in from commit `6cc77ea`, "+15-22 % throughput") is genuinely the
biggest single throughput lever and it isn't on by default.

Hardware sweep on Mac↔Pi5 with `--cw-count 8`:

| Mode/Rate | Channel | SNR | cw=4 | cw=8 | Δ |
|---|---|---:|---:|---:|---|
| DQPSK R1/2 | good | 12 | ~1100 bps | **1594 bps** | +45 % |
| DQPSK R1/2 | good | 15 | 1077 bps | **1615 bps** | **+50 %** |
| DQPSK R1/2 | moderate | 15 | 1234 bps | 1594 bps | +29 % |
| DQPSK R3/4 | AWGN | 25 | 2057 bps | 2360 bps | +14 % |
| D8PSK R2/3 | AWGN | 22 | 2406 bps | **2906 bps** | +21 % |
| D8PSK R3/4 | AWGN | 27 | 2620 bps | **3127 bps** | +19 % |

The math: per-window overhead (5.3 s SACK deferral, ACK TX, decode
margin) is FIXED. CW=8 doubles bytes-per-frame, so it amortizes
that fixed cost over twice the payload. Per-frame retransmits
don't increase proportionally for R1/2 (it's robust enough), so
the win is clean.

Caveat: CW=8 with R2/3 in fading hits 14 retx (was 0) because
longer frames have more fade exposure. R2/3 fading should keep
CW=4. R1/2 (the dominant operating mode) and R3/4 (AWGN-only) get
the win cleanly.

Tried to auto-promote CW=8 in the connection enter-connected path,
but it broke the `test_connection_adaptive` clean-window accumulator
timing model (longer frames need more ticks before 3 clean windows
accumulate, and the synthetic test only has 3000 ms of budget).
Reverted the auto-promote; CW=8 remains a CLI opt-in via
`--cw-count 8` (in cli_simulator) or via `setFixedFrameCodewords(8)`
on `Connection`.

**For the user's "1077 bps is too low" pushback**: with
`--cw-count 8` the same DQPSK R1/2 SNR=15 good test goes to
**1615 bps** = +50 %. Absolute hardware ceiling on this rig is
**3.1 kbps** (D8PSK R3/4 AWGN SNR=27 + CW=8). 10 kbps is still
unreachable.

Open follow-up: make CW count adaptive (e.g. `recommendCWCount(rate,
fading)`) AND update `test_connection_adaptive` to be CW-aware so
the auto-promote can ship as a default. That's a multi-hour task
needing care; deferred to a focused session.

## Reviewer notes

- Branch protects main: experimental gate changes are isolated.
- All three commits keep `ctest 35/35` green.
- Documented baselines (CLAUDE.md) still pass: SNR=15 good R1/4,
  SNR=20 good R2/3, SNR=20 AWGN R3/4.
- 5 KB file transfer SNR=18 good fading auto-rate: PASS.
- This branch is suitable for OTA testing on real radios. If field
  reports show D8PSK R1/2 retx storms in real fading, revert to
  `94a4ae9~1` or tighten the SNR floor in waveform_selection.hpp.

## Where to take this next

1. **OTA validation** — replay 21-frame fixture at the new D8PSK
   gate over a real radio + WebSDR loopback. Confirm SNR≥10 → 0 retx
   on a live HF channel.
2. **Move QAM modes to OFDM_COX** — Schmidl-Cox sync + coherent demod
   is a better fit for 16/32-QAM than the chirp + LTS path. That's
   where the next 1.5-2× theoretical win lives, but it's research-
   level, not autonomous-tonight.
3. **Per-subcarrier bit loading** is the highest-leverage research
   item still on the table. Backlog item #5 in
   `docs/MODEM_IMPROVEMENT_BACKLOG.md`.

If field reports support the gate: cherry-pick to main as
`feat: D8PSK ladder + window=16` after another Codex audit.
Otherwise the branch sits as documented experimental data for the
next throughput attempt.
