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

## Near-noiseless sweep — verdict: STRUCTURAL, noise-independent, 16QAM-specific

16QAM R1/2 Good (n=100, seed7): SNR 36→51, 44→50, 52→47, **60→45** /100.
QPSK R3/4 @60 → **92/100**; 16QAM R2/3 @60 → 38/100.

- QPSK reaches 92% at high SNR ⇒ the burst path can hit high completion; the 16QAM
  ~50% cap is **16QAM-specific, not a harness ceiling**.
- 16QAM **plateaus ~50% even noiselessly** ⇒ NOT noise/margin.
- 16QAM **peaks at ~36 dB then declines** (51→45 toward 60 dB) ⇒ the classic
  **overconfident-LLR** signature: at high SNR the noise estimate → 0, LLRs → huge,
  and any channel-*estimate* error (wrong phase near the null) becomes a
  confident-WRONG bit with enormous weight that poisons the LDPC.

**Refined root cause:** channel-estimate interpolation error near the null —
specifically PHASE error — converted into confident-wrong 16QAM LLRs, amplified at
high SNR. Explains the flat erasure/anti-poison tests: the **magnitude-based**
erasure can't catch these carriers (fine |H|, wrong *interpolated phase*); QPSK's
45° margin rides through, 16QAM cannot. ⇒ an **estimation (interpolation)** problem
expressed at the **LLR** stage. Likely contributor: the equalizer interpolating
data-symbol H from sparse pilots instead of using the full-band LTS estimate already
measured from the preamble (on frozen Good the LTS H is valid for the whole burst).

## TRUE-GENIE split (LTS full-band H freeze) — verdict: POST-EQUALIZATION, not estimation

Built a low-risk genie (`ULTRA_GENIE_LTS_FREEZE`): freeze the data-symbol
`channel_estimate` to the full-band LTS H (the demod's own per-carrier
`Y_LTS/X_LTS`, refreshed per frame). On frozen/near-noiseless Good the LTS H is the
exact true frequency-domain H — so this isolates frequency-estimation from
post-equalization. Result (Good, n=100, seed7):

| Cell | genie | chunks/100 |
|---|---|---|
| QPSK R3/4 @60 | on | 90 (sanity — genie does not break QPSK) |
| 16QAM R1/2 @60 | off | 45 |
| 16QAM R1/2 @60 | **on** | **44** (perfect full-band H — NO improvement) |
| 16QAM R2/3 @60 | on | 33 |
| 16QAM R1/2 @20 | on | 18 |

**A perfect full-band frequency-domain channel estimate does NOT unlock 16QAM.**
So the wall is **post-equalization, not the channel estimate / interpolation.**
Sharper: at noiseless SNR with exact frequency H, equalization should yield
near-perfect symbols ⇒ 16QAM ~100%; it's 44%. The one thing a frozen *frequency*
H cannot capture is a **per-symbol phase rotation** — residual **CFO / common-phase
error** accumulating across the data symbols after the LTS. 16QAM (tight phase) can't
absorb it; QPSK's 45° margin can (→ 90%). That also explains the overconfident-LLR
high-SNR decline (a rotated symbol with a huge LLR = confident-wrong).

Rigor caveat: this genie freezes frequency H *and* overwrites the per-symbol CPE
correction, so it brackets rather than perfectly isolates. The logic holds (perfect
frequency-H, even minus CPE, didn't help). Clean confirmation = a CPE-preserving
genie + a CFO-disabled run.

**Revised root-cause: per-symbol phase tracking (residual CFO / CPE) inadequate for
16QAM on fading — NOT the frequency-domain estimate, pilots, Wiener, DD, erasure, or
SNR.** Fix lives in the phase/CFO tracking or the 16QAM demap, not the estimator.

## Static-multipath resolver — INCONCLUSIVE (different confound) + correction

To remove the LTS-freeze temporal-staleness confound, ran a static-multipath test
(`ULTRA_CHANNEL_DOPPLER_HZ=0`: Good 2-path, no fading drift, so a frozen H is
exactly perfect). Result (Good, n=100, seed7): QPSK@60 static **41** (vs **92**
fading!), 16QAM@60 static **0** (genie on AND off), 16QAM@20 static 13, 16QAM@60
fading 45.

The static channel is HARDER than fading: with Doppler=0 the seed-7 equal-2-path
(0.707/0.707) null is a FIXED near-perfect cancellation → |H|≈0 on a fixed band →
those carriers carry no energy, unrecoverable even with perfect H, even noiselessly
(genie on=off=0). Fading's MOVING null is diversity (no carrier stays dead) — which
is why QPSK does 92 on fading but 41 static. So the static test is confounded by a
fixed dead band and does NOT cleanly resolve estimation-vs-post-eq.

**Correction:** both cheap genie proxies are confounded — LTS-freeze (temporally
stale on the fading channel; note there is NO CFO in the offline good channel, so
the per-symbol effect is fading-induced H drift, not CFO) and static (fixed dead
band). So the earlier "post-equalization" verdict is NOT settled. The clean split
requires a **true per-symbol genie** (inject the exact per-symbol per-carrier H):
either data-aided `H[k] = Y[k]/X_true[k]` with the known TX symbols threaded to the
decoder (no passband math, but encoder→decoder plumbing), or tap-based from the
Watterson fading taps (`fadingTap1/2ForDiagnostics`, but passband-phase care +
validate vs channel_probe). That is the next real work item.

**Solid regardless:** the 16QAM wall is structural, 16QAM-specific (QPSK rides the
same channel at 92%), overconfident-LLR-flavored, and NOT pilots / Wiener / DD /
erasure / SNR-margin / frozen-frequency-H. Fading's moving null is diversity, not
the enemy; 16QAM's per-symbol sensitivity is.

## Noiseless 2-tap per-symbol genie — confounded too; CONVERGENT verdict

Ran the existing 2-tap genie (`ULTRA_QAM16_GENIE_CHANNEL_TWOPATH_LS`, fits
tap0+tap1·e^{-j2πkD/N}, D=24, per symbol) at noiseless (snr 60). Result (Good,
n=100, seed7): QPSK@60 **92**; 16QAM@60 genie-off **45**, genie-on **5**;
16QAM R2/3 genie-on 5. The 2-tap genie made 16QAM far WORSE (45→5) — so the 2-tap
model is NOT exact even noiselessly (real channel = 2 paths + receive FIR +
analytic-signal shaping → more than 2 taps; production pilot-interp is a *better*
estimate than the 2-tap fit). Confounded.

**Convergent verdict across all three estimates fed to the demod (Good@60):**

| Channel estimate | 16QAM | QPSK |
|---|---|---|
| 2-tap LS (worst) | 5 | 92 |
| pilot-interp (production) | 45 | 92 |
| LTS-freeze (full-band) | 44 | 92 |

16QAM tracks H-estimate quality and is **exquisitely sensitive to per-carrier H
error**; QPSK is immune (92 throughout). No pilot-based estimate exceeds ~45 for
16QAM, and denser pilots were flat. **The 16QAM wall is per-carrier channel-estimate
ACCURACY** — a precision pilot-based estimation can't deliver on a frequency-
selective *fading* channel; QPSK's 45° margin tolerates the achievable accuracy.

HONEST CAVEAT: not cleanly proven that *truly-perfect* H → 16QAM works. All three
genie proxies were confounded (temporal staleness / fixed dead null / 2-tap model
error); the one unconfounded test — data-aided `H[k]=Y[k]/X_true[k]` with the known
TX symbols threaded encoder→decoder — was not built. The convergent evidence
(hypersensitivity, every estimator insufficient, pilots flat) points firmly at
estimation accuracy, not the demap, but the data-aided genie is the remaining way
to nail it definitively.

## FINAL diagnosis verdict (2026-05-29)

- **16QAM is the single gate to the leader's 3086 bps @ Multipath-Good SNR20.**
- It fails on Good fading because it demands **per-carrier channel-estimate accuracy
  beyond what pilot-based estimation achieves on a frequency-selective fading
  channel.** GUI-confirmed real (256 CW fails). 16QAM-specific (QPSK 92% same
  channel). Overconfident-LLR-flavored.
- **All cheap levers exhausted:** pilots/density, Wiener-tuning, DD, erasure, SNR,
  frozen-frequency-H — none fix it.
- **Channel headroom exists** (genie ceiling 3764 > 3086); fading's moving null is
  diversity, not the enemy — 16QAM's per-symbol H-error sensitivity is.
- **Path to 3086 is a genuine receiver-DSP frontier:** a materially better channel
  estimator for high-order modulation (iterative/data-aided done right, longer
  training, per-carrier refinement) and/or a more robust 16QAM demap. QPSK alone
  caps ~1820–2250, short of 3086. This is the industry leader's edge, now scoped.
- **Next, if pursued:** build the data-aided per-symbol genie for the definitive
  estimation-vs-demap split, then attack the estimator (the likely lever).

## ⛔ CORRECTION 2026-07-29 — the data-aided genie is NOT the unconfounded test this doc hoped for

This document repeatedly nominates `H[k] = Y[k]/X_true[k]` as "the one unconfounded test" that
would finally split estimation from post-equalization. **It is confounded, and worse than the
proxies it was meant to replace.** The alignment was fixed on 2026-07-29 and the arm then ran —
but `channel_equalizer_equalize.cpp:526` sets `Ĥ = Y/X` and the MMSE equalizer at `:644`
computes `conj(Ĥ)·Y/(|Ĥ|²+nv)`, which with `Ĥ = Y/X` collapses to

```
X · |Y|² / (|Y|² + nv·|X|²)
```

— the transmitted symbol EXACTLY in direction with the additive noise divided out. It is
circular (use X to get Ĥ, use Ĥ to get X back): a **noiseless symbol oracle**, not perfect CSI.
Proof by capacity: at −6 dB in-band it decoded QPSK R3/4 (1.5 info bits/carrier) where Shannon
allows 0.32 — ~4.7x capacity. Per CLAUDE.md's category-error guard that is a bug, not a result.

**So this doc's central open question remains open**, and its own noise-preserving evidence is
still the best available: LTS-freeze (true full-band frequency CSI) measured 44 vs pilot-interp
45 here, and 2/30 vs a 14/30 baseline when re-run on 2026-07-29. **Perfect frequency-domain
channel knowledge does not unlock 16QAM.** The convergent "16QAM tracks H-estimate quality"
verdict below stands on the three noise-preserving estimates; the data-aided arm must be
excluded from it. See docs/CHANGELOG.md 2026-07-29 RETRACTION.

## Data-aided per-symbol genie (H=Y/X) — BUILT, alignment UNSOLVED (WIP)

Built the unconfounded genie: the OFDM modulator captures the exact transmitted
freq-domain symbol X[k] (`genie_tx_capture.hpp`, pushed in `modulator.cpp`); the
equalizer overrides channel_estimate with H[k]=Y[k]/X[k]
(`channel_equalizer_equalize.cpp`); harness enables/resets per iteration
(`measure_ack_fer.cpp`, env `ULTRA_GENIE_DATA_AIDED`). Env-gated, default OFF,
not in any production path.

**Blocker — symbol alignment.** The QPSK@60 sanity (which must be ~92 if H=Y/X is
exact) comes out 0–76, and a direct check shows the genie H is essentially RANDOM
vs the production H for symbol 0 (relDiff ≈ 1.3). Root cause: the modulator
modulates the **LTS via the same `freq_domain[data_carrier_indices]` path** as data
(`lts_data_scratch`), so the capture pushes **LTS + data** symbols, while the
decoder handles the LTS separately (`estimateChannelFromLTS`) and runs `equalize()`
on **data only** → a content offset (push[0]=LTS ≠ decoder's first data symbol).
The offset isn't a clean constant (per-frame push counts vary, 30 vs 28). Two
keying attempts (FIFO read-cursor; `current_data_symbol_index_`) did not resolve it.
**Fix attempt 1 (done):** gated the capture to DATA symbols only — `createOFDMSymbol`
gained a `genie_capture` flag, set true only at the data caller (modulator.cpp:515),
false for the LTS (582/637) and probe (665) callers. This removed the LTS symbols
from the FIFO (push count 30→26) and improved QPSK@60 genie from 0 → **56** — real
progress, but still < 91, so a RESIDUAL misalignment remains.

**Residual (still open):** QPSK@60 genie = 56 (< 91) ⇒ the per-symbol X still doesn't
fully correspond to the decoded Y. Likely a decoder probe / control-first peek that
calls `equalize()` out of band, consuming the FIFO cursor for a symbol the encoder
didn't push as data. (Note: the relDiff genie-vs-production metric is a CONFOUNDED
alignment check — true-H and interpolated-production-H differ even when aligned — so
QPSK pass rate is the validator.) **Fix (focused, future):** trace the decoder's
exact data-symbol equalize sequence vs the encoder's push sequence (account for any
probe/peek), or key the capture by a shared frame+symbol identity. QPSK@60 genie must
read ~92 before trusting any 16QAM genie result.

This does NOT change the verdict below — it would only confirm/refine
estimation-vs-demap. The convergent evidence already points at estimation accuracy.

## Data-aided genie — ALIGNMENT SOLVED (2026-07-28), and the split it produces

**Corrections to the WIP section above (both mechanisms it names are wrong):**

1. *"A 4-CW frame decodes as re-synced chunks whose per-chunk carrier-pattern reset
   diverges from the push order, so read_index drifts +1 per codeword; use
   `--frame-cw 1`."* — **False.** OFDM_CHIRP only ever uses `processPresynced`, which
   has no `active_ldpc_block_size` break; a 4-CW frame is ONE continuous 26-symbol
   pass, and within a pass `current_data_symbol_index_` equals the encoder's
   `symbol_index` 1:1. `--frame-cw 1` was never the right config and is a bad gate
   (12.5% baseline vs 87.5% at `--frame-cw 4 --burst-interleave 0`).
2. The real cursor over-run: **the decoder equalizes each frame 2–6 times.** A clean
   4-CW frame = control-first peek (7 symbols, at the CONTROL geometry 47 data/12
   pilots) + CW0 header peek (7) + authoritative pass (26) = **40 equalize() calls for
   26 pushed symbols, +14 spurious consumptions per frame**; failure paths add whole
   passes (`smallframe-1cw`, `sync-recovery` ±8, `cw-discovery`, `marker-retry`).
   The read set is also channel-dependent (frames the RX never syncs to are never
   read). **No consuming cursor can work** — the fix is addressing, not counting.

**The residual bug, precisely.** After switching to absolute-sample addressing the
genie still destroyed QPSK on Good (0/24 vs 120/160 baseline) while being a perfect
no-op on AWGN. Cause: the RX→TX stream origin — the receiver's sync convention (FFT
window placed inside the CP) plus channel group delay — is **+796 samples on ITU Good**
and −148 on AWGN. Half a symbol is 576. So a nearest-SYMBOL match aliased by exactly
one symbol: small residual (0), correct carrier geometry, and completely wrong data.
Measured content agreement 0.25 = chance. **Fix: anchor the join at FRAME granularity**
(frames are ≥9 symbols apart, so a sub-symbol sync convention cannot alias), then index
the symbol within the frame. Guards: presynced-only, ≥4 agreeing origin votes,
unambiguous nearest frame, carrier-geometry match (this is what declines the control
peek), and a miss leaves the production estimate untouched, counted, and printed.

**Validation.** An independent CONTENT check (`ULTRA_GENIE_VERIFY=1`, quadrant
agreement between the production-equalized symbol and the looked-up X) reads **1.000**
on QPSK and on 16QAM. Hard gate, `--frame-cw 4 --burst-interleave 0`, Good@60,
frames_recovered/160: genie OFF 139/138/139/140 vs genie ON 139/140/140/140 on seeds
7/11/23/42 — the genie no longer costs anything. With the knob unset the harness is
bit-identical to clean HEAD across 9 cells; `ctest` 91/91.

**What the (now valid) genie says.** Good channel, seed 7, n=20:

| Cell | genie OFF | genie ON |
|---|---|---|
| QPSK R3/4 @60 | 139/160 | 139/160 |
| QPSK R3/4 @20 | 135/160 | 138/160 |
| QPSK R3/4 @14 | 106/160 | **133/160** |
| QPSK R3/4 @10 | 58/160 | **118/160** |
| 16QAM R1/2 @60 | 60/160 | 60/160 |
| 16QAM R1/2 @20 | 56/160 | 60/160 |
| 16QAM R2/3 @20 | 34/160 | **60/160** |
| 16QAM R2/3 @24 | 43/160 | **60/160** |

The genie is demonstrably live (QPSK@10 nearly doubles). **Every 16QAM cell pins at
exactly 60/160 = 3 frames per chunk, independent of SNR (20/24/60).** The genie
accounting explains it: 8 frames pushed, and only **3 are ever equalized at all**
(hits 60 = 3 × [7-symbol header peek + 13-symbol authoritative pass]). So with an
exact per-symbol per-carrier H, 16QAM decodes **every frame the receiver attempts** —
and the other 5/8 are lost in burst framing/sync before any demodulation happens.

**Consequence for the estimation-vs-demap split:** the demap is NOT the wall (exact H
→ 3/3), which supports the doc's convergent verdict; but the *magnitude* of the
16QAM opportunity cannot be read off this harness until the 5/8-never-attempted burst
framing limit for the 16QAM geometry is fixed. That is the next blocker, not the demap.

## Earlier "next diagnostic" notes (superseded by the above)

The invalid "genie" hook must be replaced with a **true genie** — inject the channel
model's *actual* per-carrier H (the OTASim/Watterson state knows it), not a 2-tap
pilot LS. Then: true-genie-H decodes 16QAM on Good ⇒ the wall is **channel
estimation** (residual |H|/phase error the Wiener can't remove on a varying channel);
true-genie-H still fails ⇒ the wall is **post-equalization** (phase/CFO, or a 16QAM
demap/LLR issue). That single test splits the remaining suspects. Pair with a GUI
SNR sweep (Good 20/26/32) to confirm the structural-penalty magnitude on the faithful
gate (the offline sweep's magnitude is suspect per BUG-HARNESS-002).
