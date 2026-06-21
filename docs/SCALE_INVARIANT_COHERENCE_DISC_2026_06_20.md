# Radio-Agnostic Coherence Disc — Continuous Scale-Invariant Estimate + Reactive Consumer

**2026-06-20. STATUS: DESIGN (measure-first). Supersedes the threshold-re-base and the per-symbol
within-frame redesign (both rejected — see below). Task #57.**

> Four-tier stack (mandatory): PHY theorist (primary) · real-time DSP systems engineer · veteran HF
> operator · first-principles physics escape hatch. Every claim below is checked against the hard
> physical constraints (half-duplex, stale CSI, fading irreducible, no shared timebase). Reject any
> heuristic patch lacking a principled justification under all three mandatory lenses.

## The problem, restated from measurement (not assumption)
The shipped `DopplerCoherenceEstimator` (per-frame |H|² lag-1 autocorrelation, cumulative mean) is
used to tell **Good** (slow fading, can go aggressive: skip chirps, climb rate) from **Moderate**
(fast fading, must stay conservative). Three measured facts, all from real IONOS rig + the captured
COH-DIAG logs (`/tmp/cohdiag_mac_mp{g,m}.log`, `cohval_mac_mp*.log`, n=80–99 frames each):

1. **It DOES separate on hardware** — single-window lag-1: Good ≈ **+0.15**, Moderate ≈ **−0.25**
   (cumulative-mean: +0.05 / −0.15). The classes genuinely differ; the metric sees it.
2. **The shipped threshold is wrong for hardware** — `kCoherenceGoodThreshold=0.45`,
   `kCoherenceModerateThreshold=0.30` are SIM-calibrated. On HW a Good channel reads +0.05 →
   `[MODERATE/POOR]`. Live mislabel. **A flat re-base to ~0 fixes HW but BREAKS SIM** (sim Moderate
   reads +0.1..0.25 → would read "Good"). No single fixed threshold serves both → **not radio-agnostic.**
3. **De-bias does NOT close the platform gap.** The LTS estimation-noise floor is only ~7% of the
   snapshot variance (`floor/lag0 = 0.07`), so lag-0 is REAL channel fluctuation, not noise. The
   sim↔rig scale gap is (a) a real channel-autocorrelation difference and (b) a **cadence/timing**
   difference — ρ(τ)=exp(−4π²σ²τ²) depends on the inter-frame TIME τ, and rig turnaround (~1.5 s)
   ≠ sim (~0.6–0.8 s after the warm-turnaround fix). Same Doppler → different per-frame-index ρ.

**The deeper truth (the real design driver):** even the disc's own cumulative-mean has only
**0.018 separation** between worst-Good (+0.057) and best-Moderate (+0.039) across 5 rig runs — and
that "Mod-b" run (measured Doppler 0.179 Hz vs the other Moderates' ~0.0) **genuinely WAS Good-like
for its whole transfer.** The disc read it correctly; the *preset label* is the coarse thing. Real HF
Doppler wanders minute-to-minute, so **the Good/Moderate boundary is intrinsically fuzzy** and a single
~3-minute transfer is one noisy draw from a moving channel. **No scale-invariant statistic makes a
crisp per-transfer label — because the thing being labeled does not hold still.**

## Rejected alternatives (do not re-attempt)
- **Per-symbol within-frame disc (#57 original):** sim-only mirage; raw per-symbol pilot LS too noisy
  on HW. Block-averaging to ~384 ms recovers 0.14–0.22 (de-risked, `WITHIN_FRAME_COHERENCE_DESIGN`
  addendum) but only MATCHES the per-frame disc — its value is convergence speed, deferred to #58.
- **Flat threshold re-base:** breaks sim (fact 2). Not radio-agnostic.
- **Lag-0 noise de-bias:** noise is only 7% (fact 3); de-bias is a no-op here.

## The design (user-chosen 2026-06-20): continuous scale-invariant estimate + reactive consumer
Three parts. The estimate makes the *number* mean the same on any radio; the consumer survives the
fuzzy boundary.

### Part 1 — scale-invariant CONTINUOUS estimate (dimensionless, level/gain/cadence robust)
Output a continuous coherence number, NOT a hard label. Candidate statistics (all normalized by the
snapshot variance → level/gain invariant by construction):
- **coherence-area** `A = Σ_{τ=1..L} ρ(τ)` — multi-lag sum dilutes the single-lag mean-reverting
  artifact (the −0.28 selectivity spike); captures "how much positive correlation persists."
  Rig: Good {0.56, 0.30}, Mod {−0.19, +0.08, −0.22}.
- **slow-fraction** `slow_var/total_var` (variance of an N-wide moving average / total) — the fraction
  of |H|² variation that is SLOW. Rig: Good {0.18, 0.15}, Mod {0.07, +0.13, 0.06}.
- **CADENCE FIX (preferred if the sim cross-check shows a cadence gap):** feed the REAL per-frame
  channel-time Δt into the estimator and report physical **Doppler σ (Hz)** = the only truly
  platform-invariant quantity (threshold on Hz: Good < ~0.1, Moderate > ~0.2). Requires plumbing the
  frame's channel-time (group airtime / frames + turnaround) into `addSnapshot(h2, dt_s)`. The disc
  already has the σ-from-ρ math (`dopplerHz()`), it just uses a FIXED `kNominalCadenceS=1.6` today.

**Metric choice is DATA-GATED on the sim cross-platform capture** (in flight): if sim and rig land on
the same dimensionless scale for the same class, coherence-area/slow-fraction work with ONE threshold;
if a cadence gap shows, go to the Doppler-Hz form. Validate on rig (5 captures) + sim (good/mod).

**DECIDED 2026-06-20 (cross-platform capture landed): coherence-AREA wins.** Faithful C++-algorithm
replication (cumulative-mean of the sliding-40 window `Σ_{lag=1..5}` normalized autocov) on 7 transfers
(sim + IONOS rig): Good {rig +0.09, +0.11; sim +0.66} vs Moderate {rig −0.10, −0.10, −0.18; sim −0.12}
→ **worst-Good +0.091 vs best-Moderate −0.100, gap 0.19, ONE threshold ~0.** Beats the candidates:
lag-1 cumulative-mean gap 0.018 (platform-broken); slow-fraction gap 0.02 (thin). The Doppler-Hz form
is NOT needed — coherence-area is cadence-robust because Moderate sits <0 on every audio path. NOTE the
*implemented* (cumulative-mean, sliding-window LOCAL demean) scale differs from the single-global-window
area5 first measured (Good +0.30): the sliding window compresses Good toward 0 but REJECTS a Moderate
transfer's transient Good-like patch (the "Mod-b" run reads −0.10 here vs +0.08 single-window) — a net
win. **Thresholds: enter 0.05 / exit 0.00 hysteresis** (`connection_policy::kCoherenceArea*`).

### ⚠ PIVOT 2026-06-20 (rig-driven): the PREDICTIVE consumer below (Parts 2–3, ACK-bit) is ABANDONED
Validating the Stage-A coherence-area metric live on the IONOS rig **disproved it as a gate.** A
confirmed-**Moderate** transfer read clean-**Good** (area +0.20, raw acf lags 1–5 all +0.2–0.3); three
confirmed-Moderate transfers spanned +0.20 to −0.41. Physical, not a bug: ~60 s = ~10–20 fade cycles
(too few to pin Doppler) + non-stationarity → no predictive per-transfer label is safe (a false-Good →
skip on Moderate → stalls). **So we did NOT build the receiver `ChannelCoherenceGate` + ACK
skip-permitted bit. The skip is gated by a SENDER-SIDE REACTIVE clean-streak instead** (delivery-driven,
radio-agnostic, no channel model): `encodeBurstLight` counts consecutive clean (warm) groups, any
resend/cold/escalation resets it, skip engages past `ULTRA_ANCHOR_SKIP_CLEAN_STREAK` (default 4) and
reverts on crater. The coherence-area metric is **kept as read-only telemetry only.** See the
2026-06-20 reactive-gate CHANGELOG entry + `project_disc_radio_agnostic_fuzzy_boundary`. Parts 2–3
below are retained as the superseded design record.

### Part 2 — consume with HYSTERESIS + conservative-near-boundary [SUPERSEDED — see PIVOT above]
A `ChannelCoherenceGate` with two thresholds (enter > exit) so the verdict is sticky, and the
in-between band DEFAULTS TO THE SAFE (Moderate) action. Never coin-flip near the boundary. This is the
existing two-threshold dead-zone philosophy, but re-centered on the scale-invariant metric and made
stateful (hysteresis), and the dead zone defaults conservative rather than deferring to the blind
fading_index.

### Part 3 — REACTIVE fallback (the part that makes the fuzzy boundary safe)
The gate is overridden to the SAFE action by OBSERVED performance, not just the predicted label:
- recent §16.4 light-anchor escalations > 0 (the K-gated escalation already committed 8b08575) → force
  conservative immediately,
- cold-start (disc not `valid()`), RX backlog, or recent resends/timeouts → force conservative.
So even when the estimate is momentarily wrong (the Mod-b case: a Moderate channel reads Good), the
reactive override catches the consequence and backs off. **Predict-then-commit is brittle; predict-then-
verify-and-react is robust** — and matches what a veteran op does by hand.

### HALF-DUPLEX feedback (the key architectural decision)
The RECEIVER measures forward-channel coherence; the SENDER decides the forward-path anchor. The verdict
must cross the ACK. Options (decide in Stage C):
- **(a) 1-bit "anchor-skip permitted" flag in the ACK** (receiver computes the gate, tells the sender).
  Clean, explicit, one wire bit. Preferred.
- (b) Sender-side reactive only (resend/timeout rate) — no coherence head-start, loses Part 1's value.
- (c) Sender estimates the REVERSE channel from ACK tone-bursts — wrong channel (reciprocity not
  guaranteed on HF); reject.
The forward channel is what the skip rides, so the receiver's read must reach the sender → (a).

## Staged build (each stage independently gated; default-OFF until proven)
- **Stage A — read-only metric. DONE 2026-06-20 (uncommitted).** `coherenceArea()` added + thresholds
  calibrated from the cross-platform data + logged alongside the legacy score + ordering unit test.
  ctest 80/81, default byte-identical. Cross-platform separation proven (gap 0.19, ONE threshold).
- **Stage B — receiver gate (read-only).** `ChannelCoherenceGate` (hysteresis + reactive overrides);
  log when it WOULD permit anchor-skip. Still no skip.
- **Stage C — close the loop.** ACK "skip-permitted" bit (mechanism a); sender obeys → `ULTRA_ANCHOR_SKIP_K`
  becomes gate-driven, not a static env constant. Rig-validate anchor-skip default-on-when-Good
  (the original gate-dependency for K=2 default-on). ctest byte-identical when the gate is off.

## Gates (every stage)
`ctest` 80/81 (only pre-existing `UltraTncSimAudio`); knob-off byte-identical; cross-platform metric
validation (rig + sim, ONE threshold); rig paired A/B for any behavior change; Codex independent review
under the four-tier stack. Trust multi-seed PAIRED only (channel non-stationary, ±25% gate noise).
