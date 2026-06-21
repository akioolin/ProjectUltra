# Within-Frame (Sub-Frame-Lag) Coherence Discriminator — Design

**2026-06-17. STATUS: REJECTED on hardware (sim-only win). The per-symbol approach is
DEFEATED by real-path per-symbol noise on the IONOS rig — keep the per-frame disc + a
re-based threshold instead. See "RIG RESULT" below.**

## ADDENDUM 2026-06-20 — the per-symbol rejection was NOISE-limited, not signal-absent (PARTIAL OVERTURN)
Re-analysis of the SAME rig PSYM-DIAG captures (`/tmp/psym_mac_{good,mod}.log`, ~7700 sym each,
`/tmp/psym_window_test.py`) with **block-averaging** of the per-symbol |H| BEFORE the autocorrelation
(segmented at turnaround gaps; each segment demeaned; pooled autocov at integer block-lags; channel
time = 24 ms/symbol since the PSYM-DIAG timestamps are decode-CPU time, ~0, not channel time):

| channel-time lag | W=1 (raw, the rejected feed) | W=2 | W=4 | W=8 | **W=16 (~384 ms blocks)** |
|---|---|---|---|---|---|
| 384 ms | 0.05 | 0.07 | 0.09 | 0.09 | **0.14** |
| 600 ms | 0.08 | 0.09 | 0.09 | 0.12 | **0.17** |
| 1200 ms | 0.08 | 0.11 | 0.13 | 0.18 | **0.22** |

**Monotonic in W at every lag.** Raw per-symbol ≈ 0 (reproduces the rejection); de-noising to ~384 ms
blocks recovers **0.14–0.22** G−M separation — comparable to the per-frame disc (~0.20). So the
per-symbol feed DOES contain the channel Doppler; raw per-sample estimation noise (sparse ~pilot-only
LS + cheap-card/loop dynamics) swamped it. The doc's "the feed doesn't contain the Doppler / R(2)/R(1)
≈ 1" conclusion was wrong — R(2)/R(1) is an unstable ratio of two noisy small numbers; block-averaging
is the right de-bias.

**BUT it does NOT clearly BEAT the per-frame disc on separation (~0.20 either way), and it's n=1
capture-pair.** The value of a block-averaged per-symbol feed (#58) is therefore **convergence SPEED**
(many ~384 ms blocks per 7 s burst → a verdict in ~1–2 bursts vs the per-frame disc's ~24 readings ×
~1.4 s ≈ 34 s), not better separation. **The live defect is the per-frame disc's THRESHOLD, not its
feed** (see below: shipping 0.30/0.45 calls rig Good +0.05 "Moderate"). PRIORITY: re-base the threshold
first (cheap, fixes a real mislabel, unblocks anchor-skip); #58 block-averaged feed is a follow-up
refinement for convergence, now de-risked offline. Replicate on a fresh rig Good/Mod pair before
trusting the 0.14–0.22.

## RIG RESULT (decisive, 2026-06-17)
Per-symbol PSYM-DIAG captures, IONOS Good (MPG) vs Moderate (MPM), ~7700 symbols each:
- **Per-SYMBOL autocorrelation: Good ≈ Moderate, separation ~0** at every lag (raw AND
  lag-1-de-attenuated). acf(1) only +0.70 (sim was +0.97) → heavy per-symbol estimation
  noise. The sim's +0.55 margin did NOT transfer.
- **Disambiguator (same captures, PER-FRAME disc): Good +0.124 vs Moderate −0.037 = +0.16
  separation.** So the channels genuinely differ and the per-FRAME disc sees it — the
  per-SYMBOL metric cannot. The per-symbol pilot |H| is dominated by equalizer/pilot-tracking
  + cheap-card dynamics, not the slow channel Doppler.
- **CONCLUSION: per-frame disc > per-symbol on hardware.** Within-frame is a sim-only
  mirage. Use the per-frame disc with a re-based threshold (~+0.05: Good clusters +0.12..0.18,
  Moderate ~−0.04..+0.01). Thin margin (weak detector) but real, and beats the current 0.45
  (100% wrong on HW). Do NOT pursue the per-symbol redesign further on this hardware.

## LITERATURE VERDICT — SIGNAL-limited, not statistic-limited → see TASK #58
Doppler-spread-estimation survey (Kay 1979; Tepedelenlioglu/Abdi 2001; Sampath-Holtzman 1993;
Yucek 2005; Rice/Jakes LCR) pinned the autocorrelation bug: `normAutocov(1)` normalizes by
lag-0, and noise contaminates ONLY lag-0 → multiplicative shrink `ρ̂(1)=ρ_true/(1+1/SNR_H)`
(the "absorbed by the threshold" comment IS the bug). Proven fixes — **lag-0-EXCLUDED**
multi-lag `ln R(τ)` vs `τ²` / run-averaged `R(2)/R(1)`, and **LCR** on low-passed |H| —
**tested on the existing rig data, do NOT rescue it**: per-symbol `R(2)/R(1)` Good 0.905 ≈
Mod 0.888, LCR M/G ratio ~1.0 (not ~5). The per-symbol feed doesn't *contain* the channel
Doppler (too noisy + equalizer-loop-contaminated); per-frame separates but lag-0-exclusion is
degenerate (Mod R(1)≈0) and the cadence aliases 0.5 Hz. **So the disc is limited by the FEED,
not the estimator. The real lever is a cleaner + faster channel-estimate feed → TASK #58**
(raw pre-loop per-symbol LS estimate, or a per-few-symbol full-band estimate at ~100-300 ms).

---
## (Original design — superseded by the rig result above)

**2026-06-17. Status: DESIGN + measure-first gate.**

## Why the per-frame disc can't detect Moderate (root cause, proven)
The shipped `DopplerCoherenceEstimator` samples one carrier-averaged `|H|²` snapshot **per
burst frame** (~1.4 s inter-frame cadence) and reads its lag-1 autocorrelation. Measured
behavior (IONOS rig, pooled n=197 + sim n=225):
- **Good**: lag-1 ~+0.18 (positive — slow fading persists). Detected fine.
- **Moderate**: lag-1 ~0 (sim +0.13, rig +0.006), with large run-to-run scatter.

This is **physics, not a bug**: at the ~1.4 s lag, Moderate (0.5 Hz, Tc≈0.85 s) is **fully
decorrelated**, so its lag-1 *must* be ~0 — indistinguishable from weak Good near the
boundary. (The earlier "−0.28 artifact" was a single-run noise excursion; pooled data is
~0. The |H| math is clean — a 5-agent audit ruled out every per-stage normalization; the
fixed-stride continuation timing is deliberate, §14.25.) The absolute lag-1 scale is also
platform-dependent (sim Good +0.70 vs rig +0.18), so no single cross-platform threshold.

**Conclusion: per-frame snapshots are a GOOD-detector only. Detecting Moderate needs a
SHORTER lag.**

## The approach: per-symbol |H| at the discriminating lag
Sample `|H|(t)` at the **OFDM symbol cadence (~24 ms)** instead of per frame, and measure
its autocorrelation at the **discriminating lag ~0.5–0.85 s** (around Moderate's Tc), where
the channel classes separate: Good ≈0.8, Moderate ≈0.3–0.5. (At the per-frame 1.4 s lag
Moderate is already 0; at the within-one-frame 0.2 s lag both are ~0.97 — too close. The
sweet spot is multi-symbol, multi-frame.)

- **Enabler (exists):** `OFDMDemodulator::Impl::updatePilotFadingStats` computes
  `symbol_mag_mean = mean_c |h_ls_all[c]|` **every data symbol**
  (`src/ofdm/channel_equalizer_pilot.cpp:488`). That is the per-symbol carrier-averaged |H|.
- **Time axis:** one data frame ≈ 9 symbols ≈ 0.216 s; the discriminating lag spans ~2–4
  frames. The channel magnitude is continuous across the inter-frame light-LTS gap (the
  re-anchor resets phase/timing, not the physical |H|), so a per-symbol |H| stream across
  the burst, placed on a real time axis (symbol period + inter-frame gap), reaches the
  ~0.5 s lag.

## CONFOUND that must be settled FIRST (measure-first gate)
`channel_equalizer_pilot.cpp:688-690` warns of a **symbol-dependent pilot-magnitude RIPPLE
that is not RF fading** (modulation/pilot-pattern deterministic). That ripple contaminates
sub-frame-lag autocorrelation. **GATE: before building anything, confirm a clean
discriminating lag separates Good from Moderate *despite* the ripple.**

Plan:
1. **Instrument** per-symbol |H| (PSYM-DIAG, env `ULTRA_PSYM_DIAG`, read-only): log
   `symbol_mag_mean` + a monotonic symbol counter at pilot.cpp:488.
2. **Capture** sim Good vs Moderate (`gui_qso_scenario --channel good|moderate --expect-mod
   any`, `ULTRA_PSYM_DIAG=1`). Sim is a VALID testbed for the *lag* (relative measure,
   immune to the platform-scale issue; its Watterson channel evolves the real Doppler).
3. **Analyze** autocorrelation vs lag for both. Look for a lag where Good ≫ Moderate AND the
   ripple (a fixed short period) is separable (e.g. notch the ripple period, or it sits at a
   lag away from the discriminating one).
4. **DECISION:** if a clean discriminating lag exists → design the metric (autocorr at that
   lag, with ripple removal + symbol-noise de-bias + magnitude-continuity handling across
   re-anchors) + a threshold; implement; validate sim + rig. If the ripple buries it →
   different proxy (e.g. notched/whitened |H|, or a model-based Doppler estimator).

## MEASURE-FIRST GATE: PASSED (sim, 2026-06-17)
Per-symbol |H| autocorrelation, sim Good vs Moderate (~6000 symbols each, `ULTRA_PSYM_DIAG=1`):

| lag (sym) | ~ms | Good | Moderate | G−M |
|-----------|-----|------|----------|-----|
| 8  | 192  | +0.950 | +0.851 | +0.099 |
| 20 | 480  | +0.898 | +0.510 | +0.387 |
| 25 | 600  | +0.871 | +0.388 | +0.483 |
| **30** | **720** | **+0.844** | **+0.288** | **+0.556** |
| 40 | 960  | +0.786 | +0.175 | +0.611 |
| 50 | 1200 | +0.719 | +0.112 | +0.607 |

Both curves smooth (NO ripple confound). Separation ~0.55–0.61 at lag 30–50 — **3× the per-frame
disc's ~0.18 margin**, all within one cw=8 frame (~53 symbols). **Recommended metric: per-symbol
|H| autocorrelation averaged over a lag band ~20–40 symbols (~0.5–1.0 s), threshold ~+0.5.**
Viability proven; the absolute scale still needs rig validation (PSYM-DIAG on IONOS Good/Mod).

## Risks / open
- Ripple separability (the gate above).
- Symbol-level estimation noise (more than the LTS full-band) attenuates the autocorrelation
  (same de-bias concern as the per-frame disc, but more samples to average).
- Magnitude discontinuity at per-frame LTS re-anchors (level jumps) — may need per-frame
  de-meaning before concatenation.
- Platform-scale (sim vs rig absolute) still applies to the THRESHOLD; the *lag* is robust.
- Cost: per-symbol accumulation in the hot demod path — keep it cheap (running sums).

Tracked: task #57. Per-frame disc stays as the GOOD-detector until this lands.
