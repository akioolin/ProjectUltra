# 03 — Channel estimation/tracking audit & the Good@20 phase-error budget

**Scope:** the coherent wideband OFDM data path. This explains *why* the May-29
"per-carrier H accuracy" verdict is correct, quantifies it, and ranks the estimator
upgrades. The companion demap-side finding is in `02_LLR_CALIBRATION_THE_MISSED_FIX.md`
— read that first; the two compound.

## 1. The pipeline as implemented (file:line verified)

1. **LTS anchor** — every data frame (full or warm preamble) carries 2 LTS symbols
   (`ofdm_chirp_waveform.cpp:828-831`), but the initial H uses **only the LAST LTS
   symbol** by default (`channel_equalizer_lts.cpp:510-515`). The CFO-clean 2-symbol
   averaging (−3 dB estimator noise, free) is built but env-gated **default OFF**
   (`ULTRA_LTS_CFO_AVG`, `demodulator_impl.hpp:66`); same for the Gaussian frequency
   denoise (`ULTRA_LTS_DFT_DENOISE`, `demodulator_impl.hpp:59`).
2. **Per-symbol pilots** — scattered (rotate 1 carrier/symbol, `pilot_pattern.hpp:88-97`);
   each carrier gets a *direct* pilot only every `spacing` symbols = **187 ms at sp8
   (R3/4)** / 117 ms at sp5. LS at pilots → CPE correction (|H|-weighted circular mean
   over the symbol's 8-12 pilots, applied full-gain to all carriers, zero-lag,
   `channel_equalizer_pilot.cpp:585-622`) → pilot-bin EMA α=0.9
   (`pilot.cpp:546-551,771`) → data-carrier H **rebuilt every symbol** by a separable
   2×1D Wiener (time over ≤4 past obs, freq over ≤16 carriers, `pilot.cpp:217-321`,
   `wiener_interpolator.hpp:79-196`).
3. **Wiener priors are MODERATE-fixed, not measured:** `ULTRA_WIENER_DELAY_SPREAD_S`
   default **1.0 ms** and `ULTRA_WIENER_DOPPLER_HZ` default **0.5 Hz**
   (`pilot.cpp:34-53`) — Good is 0.5 ms / 0.1 Hz. The in-code comment (`pilot.cpp:23-27`)
   admits the 0.5 Hz prior discards pilot history "5× too aggressively" on Good.
   Meanwhile the receiver **already measures** delay spread per full preamble
   (`last_delay_spread_ms`, `channel_equalizer_lts.cpp:816-882`, the 3be0975 CIR
   metric) — and never feeds it to the Wiener.
4. **Per-symbol frequency tracking:** pilot CFO tracking is compiled out
   (`constexpr enable_pilot_cfo_tracking=false`, `pilot.cpp:787`); CPE is the only
   intra-frame mechanism. LTS residual-CFO refinement is gated such that on fading with
   a near-zero seed it is usually REJECTED (`lts.cpp:382-396`) — up to ±0.3 Hz rides in
   per frame and is absorbed symbol-by-symbol by CPE. Measured ±3° CPE, no ramp — fine.
5. **DD tracker:** structurally OFF on Good (gate `last_fading_index < 0.15`, Good reads
   ~0.34; `pilot.cpp:890-903`). The QPSK per-carrier DD blend requires
   `!scatteredPilotsActive` → dead in production (`pilot.cpp:1081-1093`).
6. **Timing/clock:** NO sample-clock (ppm) estimator or resampler exists in RX. Burst
   member frames are sliced at a FIXED stride from the group anchor with per-frame
   timing retry deliberately disabled (`streaming_burst_interleave.cpp:359-368`).
   At 20 ppm × 8.6 s key-down = **8.3 samples** slip — inside CP, and each frame's own
   2-LTS re-anchors. Residual *within-frame* slip ≈ 0.63 samples → ±6.4° phase ramp at
   band-edge carriers by frame end (QPSK-length frame). Unmodeled in the noise var, but
   secondary. ⚠ OTASim imposes ZERO ppm offset (see 01 §1) — this term is untested
   against real clocks; do not delete the per-frame LTS re-anchor.

Stale-doc flag: CLAUDE.md's "per-symbol pilot tracking … residual CFO + timing
recovery; pilots ~every 10 carriers" does not match the code (CFO tracking compiled
out; no timing recovery; spacing 5/8).

## 2. Phase-error budget, Good@20 (1σ, per carrier)

| Term | Clean mid-band carrier | Null-adjacent carrier |
|---|---|---|
| LTS single-shot anchor (no averaging) | 4.0° | 13°+ |
| Pilot EMA α=0.9 estimator noise | 3.7° | 12°+ |
| Wiener interp noise | 2-3° | 6-15° |
| Wiener MODEL error (Moderate priors on Good; 187 ms revisit vs null-transit phase slew; global noise weights) | 2-4° **SNR-independent** | **10-40°** |
| CPE residual | 1.4° | 2-3° |
| Residual CFO post-CPE | <1° | <1° |
| Timing-slip ramp (20 ppm) | 0-6.4° band-edge | same |
| **RSS** | **≈5.5-8°** | **≥15-40°** |

Margins to nearest decision boundary: QPSK **45°**, 8PSK **22.5°**, 16QAM **~10-17°**
effective. So on Good@20: QPSK sits at 6-8σ (rides through), 8PSK at ~3σ (marginal —
matches its measured 2× damage vs QPSK), 16QAM at ~2σ on CLEAN carriers and <1σ near
nulls (guaranteed error floor). **The dominant residual is per-carrier H phase error on
data carriers near frequency-selective nulls — SNR-independent**, which is exactly why
16QAM "peaks at 36 dB then declines at 60 dB" once the CE-blind LLR asserts it with
thermal-only confidence (see 02).

Cross-check from the code's own diagnostics: the FAILURE_ATTRIBUTION eq-diag measured
the analytic noise model **underestimating the true post-eq residual 4-14× on Good@20**
(comment at `channel_equalizer_equalize.cpp:533-545`).

## 3. Estimator upgrade ranking (after the 02 calibration fix, if more is needed)

1. **Iterative data-aided re-estimation — the bet.** First-pass decode → re-encode
   LDPC-*verified* CWs → exact X̂ → H₂ = smoothed Y·X̂*/|X̂|² on ALL carriers →
   re-equalize → re-decode failed CWs. ×6-7 observation density (51 data + pilots vs
   pilots only ⇒ ~−8 dB CE noise) AND kills interpolation residual. No DD-poisoning
   mode (X̂ is parity-verified — unlike the hard decisions that caused BUG-8PSK-001).
   RX-CPU only; no airtime, no wire change. The multi-pass reprocessing scaffolding
   already exists (`streaming_ofdm_decode.cpp:1414-1480`). Insertion: stash per-symbol
   freq_domain in `ofdm_stream_processor.cpp:926-951`, trigger after partial
   `decodeFrame` success.
2. **Doppler/delay-matched Wiener priors — do regardless (~day).** Wire measured
   `last_delay_spread_ms` + a Doppler estimate into `robustDelaySpreadS/robustDopplerHz`
   (`pilot.cpp:34-53`); raise history 6→12, time obs 4→6 once the prior stops
   discarding it. +2-3 dB CE noise. (Note: naive "Wiener re-tune" was measured FLAT on
   05-29 — but that was tuning constants *without* fixing the LLR calibration; re-test
   *after* 02. Cheap, so worth one A/B; do not expect it alone to unlock 16QAM.)
3. **Enable `ULTRA_LTS_CFO_AVG` (2-LTS averaging)** — free −3 dB on the anchor estimate;
   built, default-off; needs a multi-seed no-regress gate.
4. **Pilot power boost (+3 dB)** — net ~+2 dB CE; wire-format change (synchronized
   deploy); below (1)-(3) on gain/cost.
5. **Midamble refresh / per-carrier Kalman** — little value on Good (Tc≈4.2 s); Kalman
   collapses into (1) when fed data-aided observations.

## 4. The data-aided genie — alignment bug ROOT-CAUSED (unblocks the definitive split)

The May-29 campaign built `ULTRA_GENIE_DATA_AIDED` (true per-symbol H=Y/X) but parked
it: the QPSK@60 sanity read ~56 instead of ~92. **Cause found in this audit:** the
capture FIFO cursor is consumed inside EVERY `equalize()` call
(`channel_equalizer_equalize.cpp:427-428`), and the connected-OFDM decode is
**multi-pass over the same audio** — control-first hypothesis peek
(`streaming_ofdm_decode.cpp:626-647`), data-profile pass + CW0 peek (`:1029,
:1333-1394`), full re-pass, CW-discovery loops (`:1414-1480`) — each pass advances the
cursor, so the decisive pass pairs Y with a LATER symbol's X and the garbage H *actively
poisons* equalization (which is why genie-on reads BELOW genie-off).

**Fix:** replace the cursor with stateless identity-keyed lookup —
TX: `frames[frame_ordinal][symbol_ordinal]` (new frame slot at preamble generation,
push at `modulator.cpp:279-281`); RX: frame ordinal from the absolute training position
(already plumbed via `setAbsoluteTrainingPosition`, `ofdm_chirp_waveform.cpp:777-780`),
symbol ordinal = `current_data_symbol_index_` (`ofdm_stream_processor.cpp:929/:485`).
Idempotent under any number of peeks/retries. Sanity gate: QPSK@60 genie-on must read
≈92 before trusting any 16QAM genie number.

This genie is the **definitive estimation-vs-demap splitter** and the exact upper-bound
experiment for upgrade (1).
