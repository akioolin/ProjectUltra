# Why 16QAM doesn't decode on Good@20 — diagnosis (2026-05-29)

**Question:** the leader hits ~3086 bps effective at Multipath-Good SNR 20
(23142 B/min). That needs high-order modulation. But uniform 16QAM decodes only
~18–21% of frames on our Watterson Good@20 — at *every* code rate. Why, and is it
fixable in the receiver (vs needing bit-loading or a milder channel)?

**Method:** same experimental-elimination discipline as the 8PSK/DD diagnosis.
Offline `measure_ack_fer --config burst_chunk --mod qam16 --channel good`,
seed 7, plus a genie channel-transfer probe (`tools/channel_probe`).

## Finding 0 (the reframe): the CHANNEL is not the limiter

`channel_probe good` (51-tone, noiseless, the raw Watterson Good transfer
function) + `tools/bitload_ceiling.py`, at avg in-band SNR 20 dB:

```
per-carrier SNR: median 20.0 dB, 5%ile 10.2, 95%ile 22.8, only 1.5% truly nulled
carrier capability:  <12dB 6.7% | 8PSK(12-16) 7.8% | 16QAM(16-22) 71.6% | 64QAM(>=22) 13.8%
=> 85% of carriers are 16QAM-capable. Genie bit-load ceiling 4.02 bits/carrier ≈ 3764 bps (> 3086).
```

So the channel **has the headroom** — 85% of carriers can physically carry 16QAM,
and the ideal ceiling (3764) exceeds the 3086 target. Yet uniform 16QAM decodes
~21%. **The wall is in the receiver, not the channel.** Cross-check: 16QAM works
on *flat* AWGN@20; it fails on Good@20 where 85% of carriers are at
AWGN@20-equivalent SNR — so the new variable is the **frequency-selective
variation**, i.e. how the RX handles the varying channel for 16QAM.

## Elimination matrix (16QAM R1/2 Good@20, n=150, seed7; baseline 31/150)

| # | Hypothesis | Knob | chunks/150 | Verdict |
|---|---|---|---|---|
| A | baseline | — | 31/150 | — |
| C | null/weak-carrier poison | `ULTRA_REL_FADE_ONSET=0.5 _MAX=100` | 31/150 | ❌ erasure not the wall |
| D | Wiener mis-tuned (Moderate-baked) | `ULTRA_WIENER_DELAY_SPREAD_S=0.0005 _DOPPLER_HZ=0.1` | 32/150 | ❌ Wiener tuning not the wall (flat, like 8PSK) |
| E | DD poison | `ULTRA_COHERENT_DD_OFF=1` | 31/150 | ❌ DD not involved (already off on Good via the adaptive gate) |
| B | channel estimation | `ULTRA_QAM16_GENIE_CHANNEL_TWOPATH_LS=1` | **7/150** | ⚠️ **INVALID TEST** — see caveat |
| F | est + erasure | B+C | 7/150 | ⚠️ invalid (dragged by B) |
| G | est, throughput rung | B at R2/3 | 6/150 | ⚠️ invalid |

**Ruled out: erasure, Wiener tuning, DD.** None move 16QAM.

### Caveat: the "genie" hook is NOT a genie (it makes things worse)

`applyDiagnosticTwoPathChannelOracle` (`channel_equalizer_pilot.cpp:130`) does
**not** inject true/perfect channel knowledge. It fits a **2-tap least-squares
model to the noisy per-symbol pilots** (rigid 2-path structure, no smoothing, no
time history) and extrapolates to all carriers. That is a *worse* estimator than
the production Wiener (which smooths across frequency + tracks across time), so
it degrades 16QAM (31→7) rather than improving it. **Cells B/F/G therefore tell
us nothing about whether estimation is the wall.** They only show this particular
constrained estimator is worse than Wiener — which incidentally confirms the
production Wiener is doing real work. NOTE: the 2026-05-29 8PSK diagnosis used the
same hook and called it "genie perfect-H"; that line is unreliable (the 8PSK DD
conclusion stands on its own via the DD-off PASS, so it is unaffected). The hook
should be renamed/clarified or replaced with a true (channel-model-injected) genie.

## Open question being measured now: margin vs structural

With erasure/Wiener/DD ruled out and the genie hook unusable, the clean separator
is an **SNR sweep on Good**: does 16QAM turn on at higher SNR?
- **Turns on at higher SNR** → margin / estimation-noise wall (more SNR → cleaner
  pilots → better estimate → 16QAM decodes). The implementation loss = (turn-on
  SNR) − (genie-ideal ~16 dB) tells us how much the RX is leaving on the table.
- **Never turns on** → structural (phase/CFO, or a 16QAM-chain bug on
  frequency-selective channels).

### SNR turn-on sweep — verdict: STRUCTURAL, not margin

16QAM R1/2 Good (n=100, seed7): SNR 20→**22**, 24→23, 28→33, 32→**44** /100.
QPSK R3/4 @20 = 72/100 reference.

**16QAM does not turn on with SNR** — at +12 dB (32 vs 20) it's still 44%, below
QPSK at the *target* SNR. It climbs ~5–10 pts / 4 dB, implying it needs ~+16–20 dB
to match QPSK — a **~10–14 dB structural penalty** (theory says 16QAM should need
only ~+6 dB over QPSK). A penalty that scales with frequency-selectivity rather
than SNR ⇒ NOT a noise/margin wall.

### Pilot density — DISPROVEN (and a harness-fidelity bug found)

Hypothesis: sparse pilots under-sample the fast frequency variation near the null
⇒ interpolation error ⇒ 16QAM fails. **`measure_ack_fer` was hardcoding
`pilot_spacing = 10`** (`tools/measure_ack_fer.cpp:221`) while production uses
`ofdm_link_adaptation::recommendedPilotSpacing(mod,rate)` = **5 for R1/2 & R2/3,
8 for R3/4** — i.e. the harness under-piloted coherent high-order mods by ~2×.
Fixed the harness to use the production spacing. **Re-test at production density
(Good@20, n=120, seed7):** 16QAM R1/2 27/120 (22%), R2/3 23/120 (19%), R3/4
22/120 (18%) — **unchanged vs spacing-10.** So denser pilots do NOT unlock 16QAM;
pilot density is not the wall (but the harness bug was real and is fixed).

## ⚠️ Tool-fidelity caveat — the Good 16QAM verdict needs the GUI

`measure_ack_fer` has now shown **two** fidelity defects: (1) the AWGN burst_chunk
path returns 0/720 for all mods (broken AWGN reference), and (2) it under-piloted
(spacing 10 vs production 5/8, now fixed). Per the project rule *offline tools are
not faithful for fade — use the real-time GUI*, the offline "16QAM folds on Good"
result is **suspect** and must be confirmed on the GUI before any conclusion. The
remaining structural suspects (phase/CFO residual; 16QAM's intrinsic amplitude
sensitivity to residual estimation error) are exactly what a CPU-paced offline
harness can mis-model. **GUI confirmation (forced 16QAM R1/2 & R2/3, Good@20,
multi-seed) is running.**

## GUI confirmation (faithful gate) — 16QAM genuinely fails on Good@20

Forced 16QAM on the real-time GUI, Good@20 (the offline tool had 2 fidelity
defects, so this is the authoritative test):

| Rate | Seed | Result | CW fails (BRAVO) | Delivery |
|---|---|---|---|---|
| R1/2 | 42 | FAIL | **256** | 0 |
| R1/2 | 43 | FAIL | — | 0 |
| R2/3 | 42 | FAIL | — | 0 |

`ALPHA_UNEXPECTED_MODE_COUNT=0`, `ADAPTIVE_MODE_CHANGE_COUNT=0` → mode *was* 16QAM,
no downgrade — **not** a BUG-HARNESS-001 mode-mismatch abort. 256 codeword failures
is the PHY genuinely unable to decode 16QAM. **16QAM truly does not decode on
Good@20 — confirmed faithful, not a harness artifact.** Contrast: 16QAM *works* on
flat AWGN@20 (~801 bps, prior) → the wall is specifically frequency-selectivity
handling, not the 16QAM chain itself.

## Verdict

- **16QAM fails on Good@20 (GUI-confirmed real).** A ~10–14 dB structural
  decodability penalty on frequency-selective fading. NOT erasure / Wiener-tuning /
  DD / pilot-density / SNR-margin (all eliminated). Root cause **not yet isolated**;
  remaining suspects: **phase/CFO residual**, 16QAM's **intrinsic amplitude
  sensitivity to residual channel-estimate error**.
- **This is the single gate to 3086.** QPSK caps ~1820; the leader's 3086 requires
  high-order mod; high-order mod is blocked by this wall.
- **Bit-loading does NOT escape it** — it places 16QAM on the strong carriers, but if
  16QAM needs ~30+ dB (the structural penalty) even the ~26 dB peaks can't carry it.
  Fixing decodability is the prerequisite for both uniform-16QAM and bit-loading.
- **Channel has the headroom** (genie ceiling 3764 > 3086) — worth solving, but it is
  a genuine receiver-engineering problem, not a config tweak.

## Next diagnostic (to isolate the root cause)

The invalid "genie" hook must be replaced with a **true genie** — inject the channel
model's *actual* per-carrier H (the OTASim/Watterson state knows it), not a 2-tap
pilot LS. Then: true-genie-H decodes 16QAM on Good ⇒ the wall is **channel
estimation** (residual |H|/phase error the Wiener can't remove on a varying channel);
true-genie-H still fails ⇒ the wall is **post-equalization** (phase/CFO, or a 16QAM
demap/LLR issue). That single test splits the remaining suspects. Pair with a GUI
SNR sweep (Good 20/26/32) to confirm the structural-penalty magnitude on the faithful
gate (the offline sweep's magnitude is suspect per BUG-HARNESS-002).
