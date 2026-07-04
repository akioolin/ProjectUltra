# Connect-Entry Calibration — rig ledger extraction (2026-07-03)

**Source:** every connect-entry line
`MODE_CHANGE: ... (peer_snr=X dB (local_measured), ..., local_fading=Y ...)` in
`/tmp/campaign_3000/rig_W*_mac_gui.log` + `rig_run*_mac_gui.log` (the 3000-bps
campaign ledger). **Every run was at dial MPG@20** (Watterson Good, dial/AWGN-equiv
SNR 20 dB), so the true channel class for every row is **Good** and the true dial
SNR is **20**. `local_measured` lines are the responder's own connect-time reading —
the exact input to the entry pick (`handleConnect`); `wire_peer` lines (mid-session
rate climbs) are excluded. **N = 48** entries. Extraction:
`grep -h "local_measured" /tmp/campaign_3000/rig_W*_mac_gui.log /tmp/campaign_3000/rig_run*_mac_gui.log`
then regex `MODE_CHANGE: (\S+ \S+ \S+) \(peer_snr=([-\d.]+) dB \(local_measured\), ..., local_fading=([\d.]+)`.

Motivating bug (screenshot, dial-20 Good): a SINGLE CONNECT frame read
`fading=0.66` → classified Moderate (`kFadingGoodMax = 0.65`) → entry at QPSK R1/4
on a channel that carries QPSK R2/3. The SNR reading was pooled (#58 increment 3,
`ConnectSnrPool`); the fading never was — increment 4 (this change) pools it.

## 1. Distributions (N = 48, all true dial 20 / true Good)

| statistic | SNR reading (dB) | fading reading |
|---|---|---|
| mean | **12.38** | **0.521** |
| sigma | **3.14** | **0.129** |
| min / max | 6.2 / 19.4 | 0.24 / 0.74 |
| median | 12.05 | 0.55 |

| entry rung chosen | count | share | SNR range (mean) | fading mean |
|---|---|---|---|---|
| QPSK R2/3 | 33 | 69% | 10.0–19.3 (13.6) | 0.49 |
| QPSK R1/4 | 8 | 17% | 7.0–10.4 (9.2) | **0.69** |
| QPSK R1/2 | 6 | 12% | 6.2–9.7 (8.8) | 0.49 |
| QPSK R3/4 | 1 | 2% | 19.4 | 0.38 |

All 8 R1/4 entries were fading-misclassified Moderate (see §3) — the fading
mis-class, not the SNR, is what buys the deep-conservative rung.

## 2. Measured reading → dial-20 offset (the fade-basis evidence)

Offset := reading − 20 (dial), per entry:

| statistic | offset (dB) |
|---|---|
| mean | **−7.62** |
| sigma | 3.14 |
| min / max | −13.8 / −0.6 |
| median | −7.95 |

- The current selection basis `connectSnrFadeBasisDb() = +5.0` recovers only ~⅔ of
  the mean gap at dial-20 Good: the data says the mean offset is **−7.6 dB**, i.e.
  the +5 basis leaves the *average* entry ~2.6 dB pessimistic vs its dial.
- **The offset is SNR-dependent — an additive constant is the wrong model at the
  tails:** below-median readings sit at mean offset **−10.2 dB**, above-median at
  **−5.1 dB**. Deep-fade connect windows compress the reading much harder (fade
  trough + differential-EVM saturation, cf. the saturation bound in
  `connectSelectionSnrDb`), so a single constant either under-corrects troughs or
  over-corrects crests. Pooling attacks exactly this: the pooled mean converges on
  the *population* mean (−7.6), which IS the regime a constant basis can serve.
- corr(SNR reading, fading reading) = **−0.60**: the same fade trough that
  depresses the SNR reading inflates the fading reading — the two entry inputs
  fail *together*, which is why the mis-picks compound (low SNR ∧ false-Moderate
  → R1/4).
- **Per the one-change-at-a-time rule the basis constant is NOT touched in this
  pass.** The data would support revisiting +5 → ~+7.5 *only after* fading+SNR
  pooling is validated on the rig (pooling narrows the reading distribution the
  basis is applied to; recalibrating against unpooled data first would
  double-count).

## 3. Single-frame fading false-Moderate rate at true Good

- Boundary: `kFadingGoodMax = 0.65`.
- **False-Moderate rate: 9/48 = 18.8%** (readings 0.66–0.74 at true Good).
- 8 of those 9 entered at QPSK R1/4 (the ninth, W10 at SNR 13.2, still cleared the
  Moderate floor to R2/3). At the campaign's ~2 kbps R2/3 cruise vs R1/4's ~⅓
  rate, every false-Moderate entry costs the climb-out time of the rate ladder
  (~60–100 s of transfer at depressed rate — W13's ledger shows R1/4→R1/2→R2/3→R3/4
  taking ~100 s).
- Note the pileup at 0.66–0.67 (5 of 9): the single-frame estimate sits right ON
  the boundary — exactly the regime where variance reduction pays most.

## 4. Projected false-Moderate rate after pooling N decorrelated readings

Variance argument: the pooled fading is the mean of N decorrelated (Tc-clustered)
readings, so sigma_pool = sigma/√N with the same mean. With mu = 0.521,
sigma = 0.129 (normal approximation), P(pooled mean > 0.65):

| N (decorrelated clusters) | sigma_pool | projected false-Moderate | empirical (all ledger N-combos) |
|---|---|---|---|
| 1 (status quo) | 0.129 | 15.8% (empirical **18.8%**) | 18.8% |
| 2 | 0.091 | **7.8%** | 5.3% |
| 3 | 0.074 | **4.1%** | 2.3% |
| 4 | 0.064 | 2.3% | — |

- The empirical column pools actual ledger readings (all unordered cross-run
  pairs/triples — fully decorrelated, so a slightly optimistic bound for
  same-handshake readings a few Tc apart).
- N=2–3 is what a handshake realistically yields (CONNECT + retry/second control
  decode, > Tc apart): **~2–4× fewer false-Moderate entries**, from ~1-in-5 to
  ~1-in-20…1-in-40.
- The normal projection at N=1 (15.8%) slightly under-predicts the empirical 18.8%
  (right-tail pileup at the boundary); the projections above are therefore mildly
  conservative in relative improvement.

## 5. Per-run ledger (all dial MPG@20, true Good)

| run | entry rung | SNR (dB) | fading | single-frame class |
|-----|------|----------|--------|-------|
| rig_W10 | QPSK R2/3 | 13.2 | 0.66 | **Moderate (false)** |
| rig_W11 | QPSK R2/3 | 16.9 | 0.44 | Good |
| rig_W12 | QPSK R2/3 | 14.3 | 0.53 | Good |
| rig_W13 | QPSK R1/4 | 10.4 | 0.66 | **Moderate (false)** |
| rig_W14 | QPSK R1/4 | 9.8 | 0.74 | **Moderate (false)** |
| rig_W15_failed | QPSK R1/4 | 9.8 | 0.67 | **Moderate (false)** |
| rig_W16_failed | QPSK R3/4 | 19.4 | 0.38 | Good |
| rig_W17 | QPSK R1/4 | 9.2 | 0.67 | **Moderate (false)** |
| rig_W18_failed | QPSK R2/3 | 12.3 | 0.61 | Good |
| rig_W18b | QPSK R1/4 | 7.0 | 0.66 | **Moderate (false)** |
| rig_W19 | QPSK R2/3 | 13.7 | 0.62 | Good |
| rig_W20 | QPSK R2/3 | 11.9 | 0.53 | Good |
| rig_W21 | QPSK R2/3 | 17.8 | 0.29 | Good |
| rig_W22 | QPSK R2/3 | 11.8 | 0.56 | Good |
| rig_W23 | QPSK R2/3 | 15.7 | 0.53 | Good |
| rig_W24 | QPSK R1/4 | 9.9 | 0.72 | **Moderate (false)** |
| rig_W25 | QPSK R1/4 | 9.4 | 0.67 | **Moderate (false)** |
| rig_W26 | QPSK R2/3 | 12.9 | 0.60 | Good |
| rig_W27_failed | QPSK R1/2 | 9.3 | 0.63 | Good |
| rig_W28 | QPSK R1/4 | 8.3 | 0.72 | **Moderate (false)** |
| rig_W29 | QPSK R2/3 | 11.3 | 0.56 | Good |
| rig_W30A | QPSK R2/3 | 12.9 | 0.54 | Good |
| rig_W31B | QPSK R1/2 | 9.4 | 0.49 | Good |
| rig_W32A_fail | QPSK R2/3 | 10.8 | 0.52 | Good |
| rig_W32A | QPSK R2/3 | 14.6 | 0.35 | Good |
| rig_W33B | QPSK R2/3 | 16.2 | 0.40 | Good |
| rig_W34 | QPSK R2/3 | 10.9 | 0.37 | Good |
| rig_W35 | QPSK R2/3 | 10.0 | 0.61 | Good |
| rig_W36 | QPSK R1/2 | 8.7 | 0.57 | Good |
| rig_W37 | QPSK R2/3 | 16.5 | 0.36 | Good |
| rig_W38 | QPSK R2/3 | 11.0 | 0.61 | Good |
| rig_W39 | QPSK R2/3 | 14.6 | 0.57 | Good |
| rig_W40 | QPSK R2/3 | 10.8 | 0.55 | Good |
| rig_W41 | QPSK R2/3 | 16.9 | 0.39 | Good |
| rig_W42 | QPSK R2/3 | 13.5 | 0.57 | Good |
| rig_W43 | QPSK R2/3 | 11.4 | 0.55 | Good |
| rig_W44 | QPSK R2/3 | 12.2 | 0.51 | Good |
| rig_W45 | QPSK R2/3 | 13.2 | 0.55 | Good |
| rig_W46 | QPSK R2/3 | 16.3 | 0.44 | Good |
| rig_W47 | QPSK R2/3 | 10.3 | 0.63 | Good |
| rig_W48_fail | QPSK R2/3 | 12.7 | 0.41 | Good |
| rig_W48 | QPSK R1/2 | 9.2 | 0.24 | Good |
| rig_W5_failed | QPSK R2/3 | 19.3 | 0.28 | Good |
| rig_W5b | QPSK R2/3 | 17.2 | 0.25 | Good |
| rig_W6 | QPSK R1/2 | 6.2 | 0.47 | Good |
| rig_W7 | QPSK R2/3 | 12.9 | 0.40 | Good |
| rig_W8 | QPSK R1/2 | 9.7 | 0.52 | Good |
| rig_W9 | QPSK R2/3 | 12.3 | 0.42 | Good |

## 6. Caveats

- Single channel condition: everything here is dial MPG@20 Watterson Good on the
  IONOS rig. The sigma and the false-Moderate rate at other dials/classes are
  unmeasured; the σ/√N argument is dial-independent, the absolute rates are not.
- The fading estimator itself (freq_cv + temporal_cv, single frame) is untouched —
  this is variance reduction on its output, not a better estimator.
- Same-handshake pooled readings a few Tc apart are *approximately* decorrelated
  (Clarke/Jakes A(Tc)=0.5); the Tc-clustering merges anything closer, so N_eff is
  honest but the residual inter-cluster correlation makes real pooling slightly
  worse than the table's ideal (and the empirical cross-run column slightly
  better than reachable).

## 7. Calibrated AFFINE entry-SNR basis (2026-07-03, `ULTRA_CONNECT_AFFINE_BASIS`, default OFF)

Implementation of the §2 finding — the flat `+5` basis under-corrects troughs
(dial-20 connects reading 9–11 entered QPSK R1/2 on a channel that carries R2/3)
and over-corrects nothing it should. Landed knob-gated in
`connection_policy.hpp` (`connectAffineCorrectionDb` / `dialEquivalentSnrDb` /
the 4-arg `connectSelectionSnrDb`), default OFF ⇒ byte-identical flat path.

### The fit (least squares on the 48 ledger points, dial target 20.0 for all)

`dial_equiv(reading) = a·reading + b`:

| quantity | value | note |
|---|---|---|
| slope `a` | **0.000** (exact) | one-dial design: cov(dial, reading) ≡ 0 |
| intercept `b` | **20.000** (exact) | = the calibration dial |
| in-sample residual sigma (dial space) | **0.00** | tautology — constant target |
| reading scatter (the REAL uncertainty) | mean 12.38, **sigma 3.14** (sample, N=48) | fade-phase noise around one dial |

**The fit is exactly degenerate, and that is the finding, not a failure:** offset
:= 20 − reading is an exact affine function of the reading, so the offset-on-
reading regression has slope −1 / intercept 20 with zero residual. Within the
calibrated population the reading carries **no** dial information — and §2's
"SNR-dependent offset" (−10.2 below the median vs −5.1 above) is, at a single
dial, pure regression to the mean, NOT evidence that the offset varies with the
dial. A one-dial design cannot identify the dial-vs-reading slope; that needs a
multi-dial sweep (open follow-up below).

### Deployed constants (pinned + unit-tested, `test_connect_affine_basis`)

- Deployed intercept `kConnectAffineDialEquivDb` = `b − sigma/√N` =
  20 − 3.14/√48 = 19.547 → **19.55 dB** — one standard error of the calibrated
  mean, the same one-sided-cost shrink as `entryClassificationFadingIndex`
  (commit 2996e37). Without it every mid reading lands **exactly on** the
  zero-margin Good QPSK R3/4 anchor (20.0, `kCoherentLadder`) — a boundary a
  point estimate should not sit on.
- Correction = `clamp(19.55 − reading, +2, +11)` dB
  (`kConnectAffineCorrMinDb`/`MaxDb`). The clamp **is** the extrapolation guard:
  outside roughly [8.5, 17.5] the map degrades to a slope-1 flat basis at the
  nearer clamp edge. Both ledger extremes clamp: reading 6.2 → +11 (sel 17.2),
  reading 19.4 → +2 (sel 21.4).
- **Data-aided-only:** the population is `local_measured` (data-aided) lines, so
  the selection applies the affine map only when `snr_is_data_aided`; a
  training-snapshot reading (fade-crest OVER-reads, 7.8 measured at true
  Moderate@8) keeps the flat basis — +11 on an over-read crest would put a
  true-8 dB channel deep into OFDM territory.
- **Composition:** the Moderate saturation bound in `connectSelectionSnrDb` is
  unchanged and stays keyed to the **raw** reading (≥ 6.5 zone test); only the
  `sel` it maxes against changes. One `max()`, applied once.
- **One source of truth for displays:** both GUI dial-equivalent sites (status
  bar + sidebar "dB eff", `app.cpp`) call the same `dialEquivalentSnrDb` helper
  as the selection.

### Reading → entry mapping at Good-class fading (knob ON, default entry knobs)

`sel = reading + clamp(19.55 − reading, 2, 11)`; Good anchors R1/4=10 (entry
floor), R1/2=10, R2/3=15, R3/4=20; `ULTRA_R23_BASIS` (default ON) caps any
fading entry at R2/3 for sel ≥ 18:

| reading (dB) | sel (dB) | ladder rate | entry after R2/3 cap | flat-+5 counterfactual |
|---|---|---|---|---|
| < −1.05 | r+11 < 10 | below Good OFDM floor → MC-DPSK | MC-DPSK | MC-DPSK (r < 5) |
| −1.05 – 4.0 | r+11 ∈ [10, 15) | QPSK R1/2 | R1/2 | MC-DPSK / R1/2 |
| 4.0 – 8.55 | r+11 ∈ [15, 19.55) | QPSK R2/3 | R2/3 | MC-DPSK → R1/2 |
| 8.55 – 17.55 | **19.55** (plateau) | QPSK R2/3 | **R2/3** | R1/2 (r<10) / R2/3 |
| 17.55 – 18.0 | r+2 ∈ [19.55, 20) | QPSK R2/3 | R2/3 | R3/4→capped R2/3 |
| ≥ 18.0 | r+2 ≥ 20 | QPSK R3/4 | R2/3 (cap) | R3/4→capped R2/3 |

- The user-critical case: **reading 9.5 → sel 19.55 → QPSK R2/3** (flat +5 gave
  14.5 → R1/2). All 48 ledger entries re-map to R2/3 except 19.3/19.4 → sel
  21.3/21.4 (ladder R3/4, entry-capped R2/3).
- `ULTRA_ENTRY_CAP_R34` interplay: R3/4 entry needs sel ≥ 20 + 3.15 = 23.15 ⇒
  reading ≥ **21.15** under the affine (+2 crest correction) vs ≥ 18.15 under
  flat +5 — the affine slightly de-rates crest entries, consistent with the
  calibration (a 19.4 reading at dial 20 means true dial ≈ 20, where Good R3/4
  is zero-margin).
- Moderate-class fading (affine applies, calibration extrapolated across class):
  plateau 19.55 → QPSK R1/2 (Moderate anchors R1/2=18, R2/3=20); the raw-keyed
  saturation bound still gates sub-6.5 readings.

### Risks / follow-ups (why the knob is default OFF)

1. **Extrapolation below the calibrated range is the big one:** a genuinely weak
   Good channel (e.g. true dial 10, readings ~2–6) gets +11 → sel 13–17 → R1/2
   or R2/3 entry where the flat basis kept MC-DPSK/R1/2. The clamp bounds, not
   eliminates, this. Rig A/B must include a low-dial leg (e.g. MPG@10/12) before
   default-ON.
2. Single channel condition (Watterson Good, one rig, one dial) — Moderate and
   other dials are extrapolated.
3. The slope is UNIDENTIFIED, not measured-zero. A 2–3-dial calibration sweep
   (e.g. MPG@12/16/20, ~15 entries each) would identify the true
   dial-vs-reading slope and replace the shrunk-intercept plateau with a real
   affine law. That is the principled successor to this pass.

## 8. Cross-environment checks (2026-07-02 late evening)

- **OTASim good@20 (faithful gate, seed 42, affine ON):** connect reading 13.7
  (data-aided, mid-calibrated-range) → sel 19.55 → **QPSK R2/3 Good entry** —
  identical behavior to the rig batch (readings 7.3-13.6 → R2/3, 4/4). The affine
  basis is NOT IONOS-specific at dial-20-equivalent conditions: the estimator's
  Doppler-EVM compression is a property of the waveform+channel class, and the
  sim's Watterson Good at 20 dB lands in the same reading band.
- **OTASim good@14 (affine ON): UNMEASURABLE — blocked by the pre-existing sim
  handshake floor** (BUG-HANDSHAKE-PING-FLOOR + 4-CW CONNECT decode: the gate
  never connects at good@8-12, marginal 15). The run died in CONNECT retries
  (repeated 4-CW CW-FAILs t=14.6/34.6/54.6, occupancy 0%) before any entry
  selection could run. No affine data. The low-dial portability leg therefore
  falls to the IONOS multi-dial sweep (§Risks item 1/3), which was already the
  gating requirement for default-ON.
- **Real-radio caveat (user question, answered 07-02):** on an arbitrary real
  channel the dial-20 intercept is uncalibrated — that is WHY the knob is
  default-OFF and per-environment. The portable successor is the measured
  multi-dial affine law + Phase-2 provisional-entry refinement (re-select from
  the first OFDM pilot SNR ~1-2 groups in), which removes most of the entry
  stakes entirely.
