# Good/Moderate Channel Discriminator — Design & Proof (2026-06-15)

Status: **DESIGN + faithful-sounder PROOF complete (UNCOMMITTED prototype).** Production
wiring into the equalizer/rate-ladder and the GUI/hardware confirmation are the reviewed
next step (see §8). Nothing in this doc has been committed or merged.

Written under the mandatory four-tier stack (PHY theorist primary; real-time DSP systems;
veteran HF operator; first-principles physics arbiter).

---

## 1. The defect this fixes

The OFDM rate ladder, the warm short-anchor gate, and the Wiener channel model all branch on
`classifyChannel(fading_index)` (`connection_policy.hpp:172`, thresholds AWGN<0.15 / Good<0.65 /
Moderate<1.10). `fading_index = freq_cv + temporal_cv` (`channel_equalizer_pilot.cpp` /
`channel_equalizer_lts.cpp:745`) measures the **depth** of the |H| fade — the across-carrier /
across-time coefficient of variation of channel magnitude.

For the equal-gain 2-path CCIR presets the modem must distinguish, **fade depth is the same in
both channels** (both paths 0.707, `models.cpp:848-868`). Measured (16-seed GUI sweeps,
`project_fading_classifier_cannot_discriminate_2026_06_08`): Good `fading_index` mean 0.55 vs
Moderate 0.58 — statistically identical, identical rate split, a two-way misclassification
(Good→Moderate→R1/2 **and** the more dangerous Moderate→Good→R3/4 on the harder channel). The
whole ladder rides a blind input. This is a tracked ADAPTIVITY_AUDIT-class defect.

What actually differs between the presets is not the depth but the **rates**:

| Preset (CCIR / IONOS)        | Doppler spread (nominal) | RMS σ\* | Delay spread | Our `ChannelType` |
|------------------------------|--------------------------|---------|--------------|-------------------|
| MPG — Multipath **Good**     | 0.1 Hz                   | 0.05 Hz | 0.5 ms       | `GOOD`            |
| MPM — Multipath **Moderate** | 0.5 Hz                   | 0.25 Hz | 1.0 ms       | `MODERATE`        |
| MPP — Multipath **Poor**     | 1.0 Hz                   | 0.50 Hz | 2.0 ms       | `POOR`            |

\* The sim's RMS Doppler is **half** the nominal: `fading_sigma_hz_ = 0.5 * doppler_spread_hz`
(`models.cpp:470`). The IONOS hardware presets (`MPG/MPM/MPP`, manual §6, driven via
`tools/ionos_ctl.py`) are the **same CCIR profiles**, so anything validated on OTASim transfers
1:1 to the radio bench.

The Doppler axis separates the classes **5×** (0.05 vs 0.25 Hz); the delay axis only **2×**
(0.5 vs 1.0 ms). **Coherence time (Doppler) is the strong discriminant; delay spread is weak.**

---

## 2. Physics — why a ~1 second lag is the whole game

The sim fading taps are a sum of 128 sinusoids whose frequencies are **Gaussian**-distributed
with RMS σ (`initializeGaussianDopplerTap`, `models.cpp:561`; *not* Jakes/J₀). The temporal
autocorrelation of such a process is the characteristic function of a Gaussian:

```
    R(τ)      = exp(-2 π² σ² τ²)          (complex / field autocorrelation)
    A(τ) = |R(τ)|² = exp(-4 π² σ² τ²)     (squared-envelope |H|² autocovariance)
```

Decorrelation-to-0.5 time `τ½ = sqrt(ln2 / (2π²σ²))`: **Good ≈ 3.75 s, Moderate ≈ 0.75 s** (5×).

The separation is invisible at short lags and enormous at second-scale lags:

| lag τ | Good R(τ) (σ=0.05) | Moderate R(τ) (σ=0.25) | ratio |
|-------|--------------------|------------------------|-------|
| 24 ms (1 symbol) | 0.99999 | 0.9997 | ~1 (useless) |
| 0.5 s | 0.988 | 0.735 | 1.3× |
| **1.0 s** | **0.952** | **0.291** | **3.3×** |
| 1.5 s | 0.895 | 0.062 | 14× |
| 2.0 s | 0.821 | 0.007 | 114× |

**This is the crux the prior work missed.** A 1-symbol or few-hundred-ms window sees both
channels as fully correlated (≈1) — it measures the *sampling noise of a slow process, not its
rate*. The discriminant lives at a **~1 s lag**, where Good is still strongly correlated and
Moderate has essentially decorrelated.

---

## 3. Autopsy — why the earlier Doppler/CIR attempts "weren't good enough"

(commits e4405f2, 3be0975, ecb70bb, a72c572; surviving code `channel_equalizer_lts.cpp:798-882`.)

1. **`fading_index` is fundamentally blind** — it measures magnitude CV (depth), invariant for
   equal-gain 2-path channels. No post-processing fixes a depth metric when the discriminant is a
   rate. (§1.)
2. **The delay-spread (coherence-bandwidth) metric is the *weak* axis and is noisy.** It exists
   (`last_delay_spread_ms`, the rho(L) 0.5-crossing at `channel_equalizer_lts.cpp:846`) but: warm
   data preambles read artificially flat (under-read), deep fades crash coh-BW to a false high delay,
   single-snapshot variance is large, it was never time-averaged, and **it was never wired into rate
   selection**. With only a 2× physical separation and that noise, it can't carry the decision alone.
   Our sounder reproduces this: delay axis Good 0.18 ms vs Moderate 0.27 ms — overlapping.
3. **The Doppler axis was never measured at the right lag over enough data.** The estimators looked
   at a connect-window snapshot (tens of symbols, sub-second) — far below the ~3.8 s of contiguous
   data the statistic needs to converge (§5). "Window too short vs coherence time" is exactly right.
4. **The Wiener model hard-codes Moderate** (`robustDopplerHz()` 0.5 Hz / `robustDelaySpreadS()`
   1.0 ms, `channel_equalizer_pilot.cpp:28-41`), 5× too pessimistic on Good — never derived from a
   measurement (ADAPTIVITY_AUDIT Case #2). A working Doppler estimate also closes this.

---

## 4. The estimator

Measure the **temporal autocorrelation of the per-symbol pilot channel magnitude**, evaluated at a
~1 s lag, accumulated over enough contiguous data symbols, and (optionally) regressed to a Doppler
σ̂ in Hz.

**Inputs (already computed):** `updateChannelEstimate` (`channel_equalizer_pilot.cpp:529`) forms the
raw per-pilot LS estimate `h_ls_all[i] = rx/tx` once per data OFDM symbol, before any smoothing.

**Statistic — use the magnitude, not the complex value.** The raw `h_ls` carries residual-CFO phase
drift across symbols (CPE correction is applied to `channel_estimate`, *not* to `h_ls_all`); over a
1 s lag even 0.1 Hz of residual CFO is 36° of rotation and would corrupt a complex autocorrelation.
The squared-envelope `|H|²` autocovariance is **immune to CFO and to the warm-LTS phase
re-anchoring between frames**, and follows `A(τ)=exp(-4π²σ²τ²)` — still a clean, monotonic,
hugely-separating function of σ.

Per symbol m, carrier-average `a[m] = mean_pilots |h_ls[m]|²`. Over a contiguous run of symbols,
for the lag `L` nearest 1 s (`L = round(1.0 / T_sym)`, `T_sym=(FFT+CP)/fs`):

```
    ρ_env(1s) = Σ_m (a[m] - μ)(a[m-L] - μ) / Σ_m (a[m] - μ)²       (pooled across frames)
```

**SNR-robustness (the key property).** White estimation noise is uncorrelated across symbols, so it
inflates only the τ=0 term — it scales ρ_env(τ>0) by a *constant* floor independent of lag.
Therefore a regression of `ln ρ̂(τ)` on `τ²` has slope `-4π²σ²` (pure Doppler) and an intercept that
**absorbs the SNR/noise-floor bias**: σ̂ is SNR-independent. The sounder confirms ρ@1s is flat from
12 to 20 dB (§5). For the binary decision, the single ρ_env(1s) feature suffices; the regression σ̂
is the by-product that also feeds the Wiener model (§7).

**Pooling.** A run must be *contiguous in time* (24 ms/symbol); frames are separated by multi-second
T/R gaps, so pairs straddling a gap are invalid. Accumulate the numerator/denominator sums
**within each frame** and **pool across frames** until ≥ the convergence floor of §5 is reached.

---

## 5. Validation — faithful channel sounder

Tool: `tools/channel_discriminator_probe.cpp` (built target `channel_discriminator_probe`). A
multitone comb is pushed through the **real `WattersonChannel`** (real multipath + Gaussian-Doppler
fading + calibrated noise, FFT=1024/CP=128/48 kHz); per-carrier `H_k[t] = rxFFT/txFFT` is exactly
what the equalizer's pilots see. This is faithful to the channel and waveform; it is not the full
protocol (that is the GUI gate, §8).

**Separation verdict — pooled 48 runs (8 seeds × SNR {20,15,12}), 7.68 s window:**

```
NEW  ρ@1s (complex):              Good [0.674..0.873]  vs  Moderate [-0.109..0.423]   margin +0.251  100% @0.5
NEW  ρ_env@1s (|H|², CFO-immune): Good [0.235..0.670]  vs  Moderate [-0.270..0.074]   margin +0.161  100% @~0.15
NEW  σ̂ (Doppler RMS Hz):          Good ~0.066 (truth .05) vs Moderate ~0.266 (truth .25)        well-calibrated
OLD  fading depth (|H| CV):       Good [0.215..0.572]  vs  Moderate [0.112..0.530]    OVERLAP        56% (chance)
delay-spread τ (coherence-BW):    Good 0.18 ms vs Moderate 0.27 ms                    overlap (weak axis)
```

The discriminant **fully separates** (no overlap) and is **flat across SNR**, where the production
`fading_index` is at chance. Both the complex and the CFO-immune envelope statistic separate
cleanly; the envelope is the production choice (§4).

**Convergence floor — how much contiguous data is required (8 seeds × 3 SNR each):**

| window | ρ@1s margin | ρ_env@1s margin | separated? |
|--------|-------------|------------------|-----------|
| 70 sym (1.68 s — one cw=8 frame) | −0.141 | −0.070 | **no (overlap)** |
| 110 sym (2.64 s) | −0.028 | −0.025 | marginal |
| **160 sym (3.84 s — ~2–3 frames)** | **+0.275** | **+0.099** | **yes (100%)** |
| 320 sym (7.68 s) | +0.251 | +0.161 | yes (100%) |

**A single frame is not enough** — this is precisely the prior "window too short" failure,
quantified. The estimator must **pool ≥ ~3.8 s (~160 data symbols, ~2–3 cw=8 frames)** of
contiguous-within-frame pairs before its output is trusted; until then, fall back to the current
classifier (conservative).

---

## 6. Decision boundary

Doppler-dominant, with the existing depth metric retained only for the no-fading (AWGN) gate:

```
  if fading_depth < ~0.05            -> AWGN     (no fading; existing fading_index<0.15 also covers this)
  else if ρ_env(1s) >= θ_GM          -> GOOD     (coherence time long)
  else if σ̂ < σ_MP boundary          -> MODERATE
  else                               -> POOR
```

- `θ_GM` is placed in the measured no-overlap gap; physics anchor = the log-midpoint of the two
  operating points. On a fully-converged window the gap is wide (margin +0.16); on the minimum
  ~3.8 s window pick θ at the gap centre (~0.15 for ρ_env normalized as in the sounder). **Calibrate
  θ on the real-modem pilot statistic (§8 offline harness), not on the sounder's absolute scale.**
- Moderate vs Poor: the Doppler axis still orders them (σ 0.25 vs 0.50), but with more variance;
  Poor is already SNR-gated into MC-DPSK territory, so the high-value decision is Good vs Moderate.
- Every constant here is a channel physical quantity or a log-midpoint of two physical operating
  points — no SNR-specific magic number (adaptivity rule satisfied). The delay-spread metric is kept
  as an **independent cross-check** axis (it should *agree* Good<Moderate), not as a primary input.

---

## 7. Integration plan (files / functions — reviewed next step, not yet wired)

1. **`src/ofdm/` — new `DopplerCoherenceEstimator`** (small, header + cpp or folded into the
   equalizer). Read-only. Fed one `addSymbol(mean_pilot_abs_h2)` per data symbol from
   `updateChannelEstimate` (`channel_equalizer_pilot.cpp:~648`, right after `h_ls_all` is formed);
   `onFrameBoundary()` at each data-frame start to bound contiguous runs; `reset()` per connection.
   Holds pooled Σ_xy[L]/Σ_xx accumulators for the ~1 s lag + the `ln ρ̂` vs `τ²` regression for σ̂.
   Exposes `valid()` (≥ floor reached), `rhoEnvAtRefLag()`, `dopplerSigmaHz()`.
2. **`OFDMDemodulator` accessor** — `getDopplerCoherence()` / `getMeasuredDopplerHz()` mirroring
   `getFadingIndex()` (`ofdm_stream_processor.cpp:~657`); also log it on the `ULTRA_PHY_DIAG_LOG`
   line next to `fading_index` / `delay spread`.
3. **Surface to the connection layer** like SNR (the `IDLE_IN_BAND`/`OFDM_BROADBAND` pattern), as a
   per-connection `measured_doppler_hz_` / `channel_coherence_`.
4. **Consume it** — replace the `fading_index` input to `classifyChannel` / `selectLadderRung`
   (`connection_policy.hpp:247`) and `selectOFDMCodeRate` (`waveform_selection.hpp:313`) with the
   coherence-based class for the Good/Moderate decision; gate `shouldUseWarmShortAnchorDescriptor`
   (`connection_policy.hpp:472`) on the real measurement instead of the R3/4 proxy
   (this removes the Moderate-crater risk that proxy was guarding against).
5. **Close ADAPTIVITY_AUDIT Case #2** — feed σ̂ into `robustDopplerHz()` / `robustDelaySpreadS()`
   (`channel_equalizer_pilot.cpp:28-41`) so the Wiener correlation model is derived, not hard-coded
   Moderate.

Behaviour change is gated on `valid()` (≥3.8 s pooled); before convergence, keep current behaviour.

---

## 8. Verification plan (what makes it "count")

1. **Offline harness (faithful PHY, deterministic, no GUI flakiness):** drive the real
   `StreamingEncoder → SimulatedChannel(GOOD/MODERATE) → StreamingDecoder` (pattern:
   `tests/test_streaming_mc_dpsk.cpp`), read `getDopplerCoherence()`, assert separation across seeds.
   Becomes a permanent regression test. **This calibrates θ_GM on the real pilot statistic.**
2. **Faithful GUI gate:** `tools/gui_qso_scenario.sh --channel good|moderate` × multi-seed with
   `ULTRA_PHY_DIAG_LOG`; show the metric separates on the production ModemEngine — the same
   methodology that proved `fading_index` blind. **Run lid-OPEN** (closed-lid throttle →
   OTASim stale-drop → false FAIL, `reference_lid_closed_throttles_gui_sim`). Per project rule a
   PHY/rate change only "counts" after this multi-seed GUI pass.
3. **Labeled hardware confirmation:** with the IONOS micro-USB on a data cable,
   `tools/ionos_ctl.py --preset good --snr 20` then `--preset moderate --snr 20` (MPG/MPM, 9600 8N1,
   manual §6) between runs; confirm the discriminator flips on the real radio bench.

---

## 9. Risks & caveats

- **Latency to first decision:** ≥ ~3.8 s of pooled data symbols before `valid()`. The link spends
  that on the conservative current classifier. Acceptable: the rate decision is re-evaluated through
  the transfer, and a Good link only *gains* once converged.
- **Envelope statistic is a 4th-order quantity** → noisier than the complex one (margin +0.16 vs
  +0.25 at 7.68 s) — but CFO/phase-jump immune, which the complex one is not on the real modem. The
  pooling floor (§5) is set from the *envelope* margins, so it is the conservative choice.
- **σ̂ absolute calibration** is good for Moderate/Poor but biases high for Good when the window
  barely exceeds Good's slow decay — which is *why the decision uses the fixed-lag ρ_env feature*,
  robust even when the Hz estimate is biased. Feed σ̂ to the Wiener model only when `valid()`.
- **Threshold portability:** θ_GM is calibrated on the OTASim pilot statistic; the IONOS run (§8.3)
  is the check that it holds on hardware. The physics (CCIR equivalence) says it should.

---

## 10. Artifacts (this session, uncommitted)

- `tools/channel_discriminator_probe.cpp` (+ CMake target) — the faithful sounder & proof.
- `tools/ionos_ctl.py` — IONOS USB-serial control (manual §6) for labeled hardware runs.
- This design doc.
- Memory: `project_doppler_coherence_discriminator_2026_06_15`.

No production source touched; no commits. Greenlight needed to wire §7 and run §8.

---

## 11. AS-BUILT (2026-06-16 — user greenlit the wiring; UNCOMMITTED)

The §7 plan was implemented and verified through ctest; the GUI gate (§8.2) is the final
check. All changes are read-only/gated so the default path is unchanged until the metric
is valid (which never happens at CONNECT — no OFDM data has pooled yet).

**Estimator** — `src/ofdm/doppler_coherence_estimator.hpp` (new, header-only). **PER-FRAME
snapshot model, HOSTED IN THE StreamingDecoder** (see the two redesign notes below). It is fed ONE
`|H|²` snapshot per decoded OFDM frame (the LTS channel power, `getLastLTSChannelMagnitude()²`);
the estimate is the normalized `|H|²` autocorrelation of the snapshot series at **snapshot-lag-1**
(the inter-frame cadence, ~1–2 s), detrended against the **global** window mean. `coherenceScore()`
is the decision feature (Good high, Moderate ≈ 0); calibrated **Good/Moderate threshold 0.5**.
`valid()` requires **≥ 24 frame snapshots** (sliding window 40) so the lag-1 sampling error is small
enough to separate. `dopplerHz()` inverts the lag-1 value assuming a ~1.6 s nominal cadence — a
Good-channel readout for the Wiener model (fast channels are decorrelated at lag-1, so σ is not
estimable there, which is fine).

**TWO REDESIGNS driven by the GUI gate (the faithful gate earned its keep here):**
1. *Within-frame → per-frame snapshot.* The first model pooled the *within-frame* per-symbol `|H|²`
   autocorrelation at a fixed ~1 s lag. The GUI gate proved it inert: OFDM burst data frames are far
   shorter than a 1 s lag, so nothing accumulated. Fix: consecutive burst frames are ~1.5 s apart
   (group airtime / frames-per-group) — right in the discrimination zone — so ONE snapshot per frame,
   correlated at lag-1, measures the right thing.
2. *Demodulator-resident → StreamingDecoder-resident.* Even per-frame, the GUI gate showed the pool
   resetting to 0 every burst group (`snaps` climbed 1→4 then dropped). Root cause: **burst transport
   reconstructs the OFDMDemodulator at every group's BURST_HEADER re-anchor** (the estimator's only
   reset path is its `configure()` in the demod constructor), so a demod-resident estimator can never
   reach the 24-snapshot floor. Fix: host the estimator in the StreamingDecoder (persists per
   connection) and feed it one per-frame `|H|²` snapshot from `populateDecodeMetrics` (which runs
   ~once per decoded frame). (The earlier global-mean detrending carries over.)

**Calibration / proof:** CI lock `tests/test_doppler_coherence_estimator.cpp`
(`DopplerCoherenceEstimator`): Good/Moderate separated (good_min 0.65 > mod_max 0.42 over 8 seeds,
threshold 0.5), score ordering Good 0.54 > Moderate 0.17 > Poor 0.02, `dopplerHz(good)` 0.078,
abstain-until-pooled. PASS (24/24). The faithful-sounder proof (`tools/channel_discriminator_probe.cpp`,
real `WattersonChannel` + noise) established the underlying separation. GUI gate: see Verification.

**Wiring (Stages A–C):**
- A: estimator hosted in `StreamingDecoder` (member `doppler_coherence_`, reset per connection).
  Fed read-only in `populateDecodeMetrics` (`streaming_sync_acquisition.cpp`, runs ~once per decoded
  OFDM frame): `addSnapshot(getLastLTSChannelMagnitude()²)`. Diag-logged once per frame when valid.
- Accessors: results stored in `StreamingDecoder` atomics (`last_doppler_*_`) + getters →
  `ModemEngine::getDopplerCoherenceScore/Valid` → binding → `ProtocolEngine::setChannelCoherence`
  → `Connection` (`coherence_score_/coherence_valid_`). (The earlier demod/waveform `getDopplerCoherence*`
  accessors were removed when the estimator moved to the decoder.)
- C: the two primary rate-decision sites in `connection_handlers.cpp` — `selectLadderRung`
  (:209) and `recommendWaveformAndRate` (:918) — consume
  `connection_policy::coherenceAdjustedFadingIndex(fading_index, score, valid)` (equals the raw
  `fading_index` until valid, then maps the Good/Moderate verdict to a representative fading
  index; AWGN/Poor never overridden). The CW-count / negotiate / acceptCall sites still use the
  raw `fading_index` (they run at CONNECT where coherence is provably invalid, so identical) —
  see review finding #2.

**Adversarial four-tier review (workflow, 5 agents):** verdict — **safe to keep as-is on the
default ship path** (read-only + gated; coherence invalid at CONNECT; consumers reached only via
default-off `ULTRA_RATE_ADAPT`). 10 alarming findings were dropped after reading the code (addSymbol
IS wired, `pool()` is per-frame not per-symbol, `onFrameBoundary` fires once per frame on the
`processPresynced` data path, finite-gate prevents NaN). Confirmed:
- **(major, gated) MODE_CHANGE recreates the OFDMDemodulator → wipes the pooled snapshots.**
  `applyPendingConnectedOFDMMode` rebuilds the waveform/demodulator on every rate change, so the
  pool resets and `valid()` reverts to the blind metric exactly when a rate moves. Harmless while
  `ULTRA_RATE_ADAPT` is off; **a correctness precondition before enabling it** (follow-up #2).
- **(minor) consistency:** the CW-count/negotiate/acceptCall sites use raw `fading_index` — inert
  today (CONNECT-time), to route through `coherenceAdjustedFadingIndex` when enabling rate-adapt.
- **(minor, done) defensive `!std::isfinite`/negated-positive guards** in the estimator.

**Verification:** ctest **25/25** (ConnectionPolicy, ConnectionAdaptive, Protocol, OFDM, Streaming*,
WaveformLoopback, FrameV2, DopplerCoherenceEstimator, WattersonProof, ToneBurstAckWatterson) — the
demod→decoder refactor introduced no regression. **GUI gate good vs moderate (disconnect 220 s,
21 KB, real ModemEngine over OTASim): both PASS, CRC-clean, no regression** (good→QPSK R3/4 2050 bps,
moderate→QPSK R2/3 1370 bps). **The discriminator now functions on the real burst path:**
- good@20: coherence **0.70 [GOOD], 27/27 verdicts GOOD** (`fading_index` meanwhile 0.21–0.39).
- moderate@20: coherence **−0.11 [MODERATE/POOR], 78/78 verdicts MODERATE** (`fading_index` 0.28–0.46).
- Separation margin **0.81** on that single run — but see §12: multi-seed showed that was the
  *mean* of noisy per-frame reads, and a single read is far noisier.

---

## 12. Multi-seed calibration + cumulative mean + dead zone (2026-06-16)

The single good/moderate run (§11) used the *mean* of the per-frame readings. A 5-seed sweep then a
12-seed sweep revealed two things the single run hid, and drove two refinements:

1. **A single 40-snapshot lag-1 autocorrelation has ~0.16 SE.** Moderate single-reads scatter to
   ~0.45 (and the real GUI's per-frame Moderate reads reached 0.31–0.44) — too noisy for a tight
   threshold. **Fix:** `coherenceScore()` now returns the **cumulative mean** of the per-frame
   readings over the transfer (the stable statistic). Per-run min/max then tightened to ~0.01–0.05.
2. **The Good distribution is wider/lower than one run implied** (multi-seed means 0.42–0.82, with a
   harder realization at ~0.42) and the real margin to Moderate is ~0.13, not 0.81. A single fixed
   0.5 threshold mis-classified the marginal Good. **Fix:** a **two-threshold dead zone** —
   confident-Good ≥ 0.45, confident-Moderate ≤ 0.30, in-between defers to the raw `fading_index`
   (conservative status quo). `connection_policy::coherenceAdjustedFadingIndex` + the decoder log +
   the Wiener push all use it; `kCoherenceGoodThreshold=0.45`, `kCoherenceModerateThreshold=0.30`.

**12-seed × 2-channel GUI validation (cumulative mean, dead zone), all 24 transfers CRC-clean:**
- Good: **11/12 confident-Good** (min 0.50), 1 dead-zone (seed 2 = 0.421), 0 read Moderate.
- Moderate: **11/12 confident-Moderate** (max 0.30), 1 dead-zone (seed 10 = 0.359), 0 read Good.
- **Every confident verdict correct (22/22); ZERO dangerous misreads** (Moderate max 0.359 < 0.45,
  margin 0.09); the dead zone absorbed the 2 marginal seeds → safe deferral.
- The Wiener push (fired on the 11 confident-Good runs) is **no-regression at Good@20** (CRC-clean);
  its *benefit* (lower Good SNR) is unvalidated — a focused low-SNR FER test is the follow-up.

**Verdict:** the metric is reliable for feature-gating **via the dead zone** — act on a confident
verdict, defer when marginal. Thresholds 0.45/0.30 locked on 12 seeds. CI test asserts the safety
property (no Moderate reaches confident-Good) + separation.

**Still NOT done (follow-ups, where the throughput WIN lives):**
1. **Wiener model (ADAPTIVITY_AUDIT Case #2):** feed `dopplerHz()` into
   `robustDopplerHz()/robustDelaySpreadS()` — a default-ON, mid-transfer decode-quality
   improvement on Good (the current hardcoded 0.5 Hz is 5× too pessimistic). Deferred: it
   changes the estimation hot path and needs its own multi-channel GUI validation.
2. **Adaptive rate ladder / mid-stream:** the coherence verdict only changes the *initial*
   pick if valid at CONNECT (it isn't) — so the default-path rate benefit needs the
   mid-stream/adaptive path (gated by `ULTRA_RATE_ADAPT`, off) to consume it. The coherence
   metric is exactly what makes that path safe to enable (it fixes the blind input that
   caused the rate churn). **PRECONDITION before enabling (review finding):** persist the
   estimator/coherence across the MODE_CHANGE-driven demodulator recreation (e.g. carry it at
   the Connection layer, or move the estimator out of the per-demodulator object), else a rate
   move wipes the pool and reverts to the blind metric during re-convergence; AND route the
   CW-count/negotiate sites through `coherenceAdjustedFadingIndex`.
3. **Short-dual-chirp gate:** replace the R3/4 proxy in `shouldUseWarmShortAnchorDescriptor`
   with the coherence verdict so short-anchor can engage on genuinely-Good channels.
4. **IONOS labeled HW (§8.3)** once a data USB cable is connected.
