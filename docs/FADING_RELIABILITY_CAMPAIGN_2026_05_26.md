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

## Workflow
Iterate: hypothesis (from genie data) → fix → build → GUI multi-seed → measure → keep/revert,
commit wins branch-only. Hard PHY rounds → Codex collaboration (mandated counter-check).
Loop is driven by background-run completion notifications. Update this doc as the durable
state so context loss doesn't reset the campaign.
