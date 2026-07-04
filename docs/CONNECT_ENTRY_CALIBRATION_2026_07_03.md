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
