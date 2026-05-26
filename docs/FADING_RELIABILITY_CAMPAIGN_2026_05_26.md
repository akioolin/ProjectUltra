# Fading-Reliability + Throughput Campaign (2026-05-26)

Goal (user): minimal-retx on fading, match the market leader's Good-fading goodput
(~3086 bps), **no cheating** — GUI auto-path, multi-seed, faithful clock, whole-matrix,
real proof per change. Honest target: clean on AWGN (have it); on fading, drive
CW-failures to the irreducible deep-fade floor + kill non-physical failures + stop
throughput-killing cascades. Zero-retx on fading is NOT a goal (physically impossible;
the leader retransmits on fades too).

## Honest baseline (clean single GUI run, Good@20 QPSK R2/3, seed 2, faithful clock)
- **86/91 data frames decode 4/4 (94.5%).** 5 fail: one 2/4, one 3/4, three 0/4.
- The 5 failures are **genuine frequency-selective deep fades** (e.g. t=83: pilot |H|
  swings 1.8→15.6; LTS |H|=9.35 collapses to data |H|=1.77 — ~14 dB null, channel
  changing within the ~720 ms frame). Largely irreducible physics.
- **Throughput killer = the cascade, not the fades:** 5 fades → retx → 74-frame backlog
  → `Forced downgrade after 6000ms queue age: QPSK R2/3 → R1/2` (despite SNR=20.4
  recommending R2/3). Goodput 590 bps with downgrade vs ~1190 forced-R2/3-no-downgrade.
- Disproven hypotheses (measured, not assumed): warm-sync timing drift (those frames
  decoded fine); comb-pilot linear interpolation (Wiener made it WORSE, 4→20 CWfail).

## Today's scorecard (branch feat/good-fading-qam16-ladder-2026-05-24, origin 65acbde, nothing pushed)
- ✓ **hole-probe redundant-retx fix** — committed `71406ec` + regression test (proven).
- ✗ **σ² LLR calibration** — GUI 4→12 CWfail (net-negative), reverted/stashed.
- ✗ **Wiener-for-comb interpolation** — GUI 4→20 CWfail, goodput 600→380, reverted.

## Discipline (anti-cheat, mandatory)
GUI auto-path only (never --expert-force the gate); multi-seed (single-seed variance is
huge: same cell ran 4/12/20 CWfail — never trust 1 seed); faithful real-time GUI clock
(cli ARQ timer is wall-clock-broken, see project_arq_timer_wallclock_vs_sampleclock);
whole-matrix (AWGN must not regress when helping fading); honest metrics (end-to-end
goodput + real CW-fail, not inflated on-air); revert any change that loses its gate.

## Plan (ranked, each proven before next)
1. **GENIE FIRST (stop guessing):** instrument the coherent equalizer with a true-H tap
   (simulator channel H) → per-symbol/per-carrier estimated-H vs true-H MSE, logged on
   failing frames. The Phase-1 doc flagged this tap is MISSING. Without it every PHY swing
   is a coin flip (mine were). Sample-clock faithful + fast. This localizes each failure:
   tracking-lag vs selective-fade vs estimate-error.
2. **Trustworthy multi-seed baseline** of current state (Good@20, ≥5 seeds): mean FER,
   retx, goodput + variance. Every change judged against this, not a single run.
3. **Reduce within-frame FER** guided by the genie (the real lever): candidates =
   per-carrier soft-CSI LLR weighting (anti-poison, task #124) so faded carriers don't
   poison LDPC; faster/short re-estimation if tracking-lag; cross-frame interleaving so
   one fade doesn't kill a whole codeword. Pick based on what the genie shows.
4. **Downgrade-policy robustness:** don't abandon R2/3 on a transient fade burst when SNR
   still recommends it (but verify R1/2 isn't actually the correct adaptation).
5. **Throughput toward leader (after reliability holds):** reclaim dead air (~50%, protocol);
   8PSK rung (raw 3917 > 3086). Per project_onair_vs_endtoend + competitive_benchmark notes.

## MULTI-SEED BASELINE + DATA-DRIVEN RE-RANK (2026-05-26)
Good@20 QPSK R2/3, clean HEAD + hole-probe fix, faithful GUI, 5 seeds:
| seed | CWFAIL | retx | goodput | downgrade |
|------|--------|------|---------|-----------|
| 1 | 0 | 0 | 980 | 0 |
| 3 | 0 | 0 | 980 | 0 |
| 4 | 0 | 0 | 970 | 0 |
| 5 | 2 | 2 | 630 | 1 |
| 2 | ~4 | 3 | 600 | 1 |
**3/5 seeds already clean (~975 bps, 0 retx, 0 downgrade). 2/5 hit deep fades → downgrade → ~615 bps.**
LOAD-BEARING INSIGHT: a perfectly clean seed still only hits **980 bps = 37% of the
QPSK R2/3 raw ceiling (2611)**. So the dominant gap to 3000 is NOT reliability (even
flawless decode = 980); it is **protocol efficiency (dead air) + the rate ceiling**,
which hit all seeds equally.

**RE-RANKED PLAN (data-driven):**
1. **Dead-air / protocol efficiency (#1, universal):** clean seeds are at 37% efficiency;
   recovering dead air ~doubles goodput on EVERY seed. Root cause below. Biggest single lever.
2. **8PSK rung (#2):** required — QPSK R2/3 caps at 2611 < 3086; 8PSK raw 3917 clears it.
   Needs reliability support (8PSK ~3-4 dB more fade-fragile).
3. **Fade reliability (#3):** only 2/5 seeds; downgrade-driven. Genie-guided when reached.

## Dead-air lever — code-grounded scoping (2026-05-26)
The throughput-efficiency lever (Plan step 5a) is concrete: `Connection::sendNextFragment`
(connection.cpp:1598) fills the ARQ window then idles — `while (arq_.isReadyToSend() &&
hasMoreChunks())` sends up to window=8 frames, flushes the burst, then cannot send again
until ACKs free the window. Measured ARQ config: `window=8, sack_delay=7690ms`. The
receiver HOLDS its cumulative ACK up to **7.69 s** before returning it → sender window
stays full → sender idle ~7–10 s per burst = the measured ~50% dead air. Classic
bandwidth-delay-product starvation. Lever (global, all rates/waveforms): shorten ACK
return latency / pipeline window refill as slots free, sized to the BDP — WITHOUT
reintroducing the half-duplex ACK-collision problem that made sack_delay long
(see feedback_arq_window_history, project_throughput_bottleneck_is_arq_idle, #126).
Must be validated on faithful GUI clock (cli timer is wall-clock-broken). Big win:
even at the QPSK R2/3 ceiling, recovering dead air ~doubles delivered goodput.

## NEW LEVER (user-spotted 2026-05-26): adaptive ladder THRASH — high priority
Live BRAVO GUI log on a single 10 KB Good@20 transfer shows the adaptive mode controller
flipping **QPSK R2/3 ↔ DQPSK R1/2 ~8 times in ~2 min** (`[ADPT] ... hysteresis allows
switch` every ~10-15 s). It is chasing ESTIMATOR NOISE, not real channel change: fading
index wiggles 0.28→0.39→0.52→0.28→0.44→0.47 around the coherent/differential crossover and
SNR wiggles 19.6↔20.2 dB (~0.6 dB). Each flip = a MODE_CHANGE (control exchange + re-anchor
+ data disruption) and half drop to the SLOWER DQPSK R1/2 → this is likely a big reason the
2/5 fade-hit seeds sag to ~615 bps (controller panic, not the fades). The hysteresis is far
too weak — it debounces almost nothing.
FIX (distinct lever, do AFTER turnaround lands for clean attribution; arguably as big for the
fade seeds): smooth the F.I./SNR estimate feeding the controller (it's noisy per-frame),
widen the dead-band around the QPSK↔DQPSK crossover, and require a SUSTAINED crossing (longer
dwell / N consecutive windows) before switching — ride through estimator wiggle, switch only
on real persistent shifts. Derive dwell from coherence time + estimator variance (no magic).
Codex's current dead-air round is NOT scoped to this and will not address it.

## MEASURED RESULT — turnaround reclaim is a NULL lever (do NOT re-run) [2026-05-26]
Codex 5-seed Good@20 GUI sweep with the turnaround_ms reclaim (arq_interface.hpp): goodput
**UNCHANGED** vs baseline — seeds 980/590/980/980/630, CWFAIL 0/12/0/0/0, **TX duty ≈ 45%**.
Turnaround (T/R switch time) is NOT the dead-air bottleneck: with big bursts there are only
~4 turnarounds/transfer, so shaving 480 ms each saves ~2 s of ~98 s (~2%, noise). KEEP the
500→realistic turnaround only as a hardware-FIDELITY fix; it is NOT a throughput lever — do
not chase it again.
NEW GROUND TRUTH: **TX duty cycle ≈ 45%** (the ~55% non-TX is dominated by the receiver's
ACK-hold `sack_delay` (~7.7 s, far more than decode needs) + half-duplex RX windows + the
ladder thrash — NOT the T/R turnaround). So the REAL dead-air levers are: (a) trim the
`sack_delay` ACK-hold to ~what's needed to decode the burst (within the half-duplex turn),
(b) damp the ladder thrash. (Codex self-measured single sweep; re-verify, but the
turnaround-null + duty-45% signal is clear.) Codex is now patching the thrash; sack_delay next.

## CRITICAL CORRECTION — the "ladder thrash" is a GUI COSMETIC ARTIFACT, not real [2026-05-26, in-house]
The `[ADPT] ... hysteresis allows switch QPSK R2/3 <-> DQPSK R1/2` lines flipping every few
seconds are emitted by a GUI-side VIRTUAL adaptation display in src/gui/app.cpp (~line 2077),
using `adapt_virtual_mod_/rate_` — variables that are read/written ONLY inside that display
function and are read by NOTHING in src/protocol or src/gui/modem (verified). It's a "what-if"
track with its own weak hysteresis, separate from the REAL mode controller
(connection.cpp::updateAdaptiveModeController, gated by ADAPTIVE_MODE_CHANGE_COOLDOWN_MS=30s).
The actual modulation on seed2 switched only ~1-2x (the real `[MODE]` / MODE_CHANGE lines),
NOT the ~6-8 the advisory log implied. The waveform was NOT thrashing — operator instinct
("waveform looks identical") was correct.
CONSEQUENCE: the thrash-damp is a NON-PROBLEM at the protocol layer (Codex's 339-line protocol
dwell was fixing the wrong layer — STASHED/discarded). Do NOT re-chase protocol thrash-damp.
TWO real follow-ups instead: (a) operator-clarity: the GUI virtual-adaptation log is MISLEADING
(spams "allows switch" while the real link holds) — make it reflect the real controller's
cooldown-gated state, or relabel it as advisory; low-risk display fix. (b) seed2/seed5 real
limiter = the genuine downgrade to R1/2 (correct response to fades) + fade-collapse frames =
the RELIABILITY/channel-est lever (#3), NOT thrash.
NET re-rank of REAL throughput levers: sack_delay ACK-hold trim (dead-air, real, untested) +
fade reliability (channel-est, the fade seeds) + 8PSK (ceiling). Thrash is OFF the list.

## 8PSK RUNG SWAP — measured, REVERTED, and an honest correction [2026-05-26, in-house]
Put QAM8 (coherent 8PSK) on the Good workhorse rung in `recommendDataMode` and ran the
faithful GUI (Good@20, seed1, 10 KB). Result: **8PSK delivered the file CLEAN** (CRC ok,
0 CW-fail, 8-point ring) but the auto-path selected **R1/2, not R2/3** — because measured
SNR 19.6 < the R2/3 floor (20.0). 8PSK R1/2 = 1.5 info-bits/sym ≈ QPSK R2/3's 1.33, so this
was NOT a valid "faster modulation" test. Goodput **900 bps (vs 980 QPSK R2/3)** — a slight
loss. `RESULT=FAIL` was a HARNESS artifact (`process_exit_before_pass`: EXIT_AFTER=271s killed
the proc before the PASS sentinel; the file delivered at t≈160s). The reported duty 24.3% is
DILUTED by ~110s trailing idle after the transfer — do NOT use it as a duty comparison.
CORRECTION (I overstated): this run did NOT "prove overhead-bound" or "halve the duty." It only
showed 8PSK works + that at Good@20 the channel sits right at the R2/3 floor and often picks
R1/2. REVERTED the swap (R1/2 8PSK is a non-win); baseline binary restored; origin still 65acbde.

## STRATEGIC GROUND TRUTH — 3000 needs BOTH levers; efficiency is the dominant one
The overhead-bound conclusion stands on the EXISTING baseline, not the 8PSK run: a perfectly
clean QPSK R2/3 seed = 980 bps = **37% of its 2611 raw ceiling**. Math:
- 8PSK R2/3 (raw 3917) at today's 37% efficiency → ~**1450 bps**. Helps, FAR short of 3086.
- Leader's 3086 at Good@20 implies **~80% efficiency**.
- **=> 3000 requires a faster rung (8PSK+) AND ~2× efficiency. Neither alone suffices, and
  efficiency (~2.1×) is the bigger multiplier AND applies to every rung incl. today's QPSK.**
So the dominant lever is the dead-air. MECHANISM = half-duplex burst amortization: each cycle
pays fixed overhead (T/R turnaround + ACK airtime + sack padding), amortized over the burst;
bigger clean-channel burst → higher duty. CODE-VERIFIED: window_size=4 (NOT 8 as prior doc
text said), ack_timeout_ms=8000, sack_delay default 2000 but runtime-sized to burst-complete
(~7.6s) via connection_policy::wideOFDMSlidingSackDelayMs (connection.cpp:3031). CONSTRAINT:
feedback_arq_window_history — naive window=8 / hold-all-ACKs ALREADY FAILED (fading); must
understand that failure mode before re-touching the window.

## NEXT DISCIPLINED STEP (do NOT bump constants blind): measure the airtime breakdown
Before any ARQ change, instrument/measure on a CLEAN seed where the ~55% non-TX time actually
goes: TX-on vs receiving-the-ACK vs T/R turnaround vs pure idle (sack-hold padding beyond
decode). The harness already has TX_SECONDS/duty; add the RX-on + idle decomposition. Verify
(don't trust) the "sack_delay ACK-hold" attribution. THEN: if pure idle/sack-padding dominates
=> trim it within the half-duplex turn (low risk). If it's the per-cycle fixed overhead =>
channel-adaptive burst sizing (bigger clean-channel burst) AFTER root-causing the window=8
fading failure. All gates unchanged (faithful GUI multi-seed, AWGN no-regress, branch-only).

## WIN: cw=8 restored on Good via honest channel-Doppler CW cap [2026-05-26, +38%]
ROOT CAUSE (user-spotted): overnight commit a1c9c34 "bound coherent frames to Good-channel
coherence time" capped QPSK R2/3 cw=8→cw=4 on fading, justified by kGoodHFDesignDopplerHz=0.5
Hz → 846 ms Clarke coherence. But 0.5 Hz is the ITU-R F.1487 *Moderate* Doppler; the real Good
channel (models.cpp itu_r_f1487::good) is 0.1 Hz → Tc≈4230 ms, inside which a cw=8 frame
(~1392 ms) fits easily. The 72-CWFAIL a1c9c34 cited at cw=8 was a confounded pre-fix baseline,
NOT real — it did not reproduce on current HEAD.
FIX (principled, not the global-constant hack): recommendCWCountForChannel now derives its
design Doppler from the measured fading_index via designDopplerForFadingIndex() — Good (<0.65)
→ 0.1 Hz → cw=8; Moderate (<1.10) → 0.5 Hz → cw=4 (protective cap kept); Poor → 1.0 Hz. The
doppler_hz param is now a -1 sentinel meaning "derive from channel". test_connection_policy
corrected (846→4230 ms Good, added Moderate cw=4 cap assertion) → ConnectionPolicy 197/197 PASS.
PROOF (faithful GUI, Good@20 QPSK R2/3, cw=8, 6 seeds across two sweeps): 6/6 PASS, all cw=8,
goodput ~1350-1360 bps each (+38% vs the cw=4 980 baseline). One seed saw 8 CWFAIL from a single
deep fade, ARQ-recovered (1 retx) — expected cw=8 fade exposure, net positive (all delivered).
NEW Good@20 baseline: ~1355 bps = ~52% of the QPSK R2/3 raw ceiling (2611), up from 37%.
NO-REGRESS STATUS: Moderate provably unchanged (fading_index>0.65 → 0.5 Hz → cw=4 = old global
behavior; CONNECT is MC-DPSK, untouched). AWGN<25 dB now correctly gets cw=8 (was cw=4 — the old
isNearAwgnOFDM snr≥25 gate let it fall to the 846 ms cap); AWGN no-regress + full ctest PENDING.
REMAINING Good gap to 3000: QPSK R2/3 ceiling is 2611 < 3000, so Good@20→3000 needs BOTH (a)
dead-air efficiency (52%→higher) AND (b) a faster rung (8PSK, now cw=8-enabled, raw 3917).

## CONCLUSIVE: rung axis is EXHAUSTED at Good@20 — channel-est is the keystone [2026-05-26]
Tested every rung above QPSK R2/3 at Good@20 (~19.6 dB measured), faithful GUI, multi-seed:
| rung | raw bps | clean-seed goodput | fade-hit seed |
|------|---------|--------------------|---------------|
| QPSK R2/3 (cw=8, committed a72c572) | 2611 | 1355, 0 CWFAIL | sags ~600 (2/5), always DELIVERS |
| 8PSK R2/3 (cw=8) | 3917 | 1730 | ~900-1000, 16 CWFAIL (3-seed 1000/900/1730) |
| QPSK R3/4 (cw=8, spacing 8, ~51 data) | ~3187 | **1790** (best clean) | **145 CWFAIL, 0 bytes delivered, run TIMED OUT** |
FINDING (proven 3 ways): at 19.6 dB, every rung above QPSK R2/3 has great clean-seed upside
(1730-1790) but COLLAPSES on the deep-fade draws — R3/4's thin FEC actually FAILS the transfer
(worse than R2/3, which always grinds through). The limiter is NOT the rung/pilots/rate — it is
**fade reliability = channel-estimation quality**. The rung-tuning axis is exhausted.
CEILING MATH confirms a rung alone can't reach 3000 anyway: even a stable QPSK R3/4 (3187 raw) ×
realistic 80% efficiency = ~2550 < 3000. 3000 at Good@20 needs the STACK: (1) channel-est quality
[keystone — stabilizes R3/4 AND enables leaner pilots], (2) more data carriers via scattered/
fewer pilots [needs (1)], (3) efficiency 52%→~83% [dead-air]. Pilots are ALREADY per-rate
(recommendedPilotSpacing: coherent R3/4=spacing 8, R2/3=spacing 5).
ACTION: experimental QAM8 gate + QPSK R3/4 gate STASHED (would regress goal cell). Kept: cw=8
(committed), GUI auto-close fix (app.cpp/hpp: quit ~8s post scripted-disconnect), tools/good_mod_sweep.sh.
NEXT (pending user fork): channel-estimation work (#123: scattered pilots + 2-D Wiener + CD3) is
the empirically-proven keystone — OR bank cw=8 and reassess whether 3000 targets a slightly
higher Good SNR than 19.6 dB.

## ROOT CAUSE FOUND: QPSK deep-null carriers POISON the LDPC (anti-poison gated to QAM16+ only) [2026-05-26]
Data-grounded via the QPSK R3/4 seed2 failing-frame logs (NOT a guess):
- per-carrier |H| spread min 0.93 → median 21.6 → max 51.8 = deep ~27 dB frequency-selective
  nulls present, while avg in_band_snr is fine (~20 dB) and fading_index 0.38-0.62.
- failing CWs show |llr|=mean ~14 (CONFIDENT), unsat 25-51 (many checks fail), llr_avg biased
  ~12 → the decoder is fed confident-WRONG bits = POISONING, NOT clean erasure (which would be
  low |llr|). So it is NOT purely fundamental fade — it is fixable.
MECHANISM (channel_equalizer_equalize.cpp): per-carrier reliability = MMSE noise var
σ²/(|H|²+σ²) + erasure (h_power < GAMMA·noise_var) + softGrayZoneNoiseInflation — but ALL
reference the GLOBAL noise_variance, and `useSoftGrayZoneCsi(mod)` returns true ONLY for
QAM16/32/64. QPSK and QAM8 get NO gray-zone down-weighting. At avg SNR 20 dB a deep-null
carrier (|H|=0.93, |H|²=0.86 ≫ global σ²≈0.01, gamma≈86) reads as "clean" → full-confidence
wrong LLR → poison. R3/4 thin FEC dies on it (145 CWFAIL); R2/3 sags (2/5 seeds → ~600).
PRINCIPLED FIX (#124): per-carrier reliability must reference the RELATIVE fade depth (|H|² vs
the frame's mean |H|²), not the global noise floor — a carrier X dB below the frame average has
a ~X dB less trustworthy estimate, so inflate its LLR noise var ~proportionally. Enable for
QPSK + QAM8 (QAM16 keeps its version). Expected: stops deep nulls poisoning → helps R2/3 fade
seeds AND makes R3/4 survivable. GATE: multi-seed Good@20 (R2/3 no-regress on clean seeds +
improve the 2/5 fade seeds) + AWGN no-regress; revert if it regresses (CSI tweaks have regressed
before — rounds 10-21 — so MEASURE, don't assume). This is the empirically-located keystone.

## Workflow
Iterate: hypothesis (from genie data) → fix → build → GUI multi-seed → measure → keep/revert,
commit wins branch-only. Hard PHY rounds → Codex collaboration (mandated counter-check).
Loop is driven by background-run completion notifications. Update this doc as the durable
state so context loss doesn't reset the campaign.
