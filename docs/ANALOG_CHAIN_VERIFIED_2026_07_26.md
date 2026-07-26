# Analog chain — VERIFIED CLEAN (2026-07-26)

**Purpose of this document.** To stop the hardware being blamed for software problems. Every
figure here is a direct measurement with a committed, re-runnable tool. Before attributing any
SNR shortfall to "hardware loss", read this and re-run the tool — do not infer hardware loss by
subtraction, which is exactly the mistake this document exists to prevent.

Rig under test: **Pi5 (Fe-Pi Audio / sgtl5000 codec HAT) → IONOS SIM rev 2.03 → Mac (Creative
USB)**. Tool: `tools/measure_analog_path.py`.

---

## 1. Verified clean — do not re-litigate without new measurements

| # | property | measured | verdict |
|---|---|---|---|
| 1 | TX digital scaling | peak-SCALED, 1-3 samples of >100k at ceiling | no clipping, nothing forced low |
| 2 | OFDM payload PAPR | **13.2–15.7 dB per frame, in-band** (`build/papr_tx_measure`) | high, but inherent to 59-carrier OFDM |
| 3 | band tilt, 486-2859 Hz | **0.7 dB** p-p | negligible |
| 4 | analog SNDR ceiling | **30.7 dB** | costs **0.36 dB** at dial 20 |
| 5 | Pi5 PipeWire sink volume | 1.00 | the documented "buried TX" mode is absent |
| 6 | bandwidth referencing | ~2.8 kHz vs 3 kHz reference | **<0.6 dB** per the IONOS manual |

**Reproduce (1):** generate waveforms with `./build/ultra ptx ... -o file.f32` and measure peak / RMS.
**Reproduce (2): use `build/papr_tx_measure`** — it reports PER-FRAME IN-BAND PAPR, which is the
figure a peak-limited stage responds to (13.23 dB anchor frame, 13.91/15.12/15.70/15.52 dB for
data frames 1-4). ⚠ **CORRECTION 2026-07-26:** an earlier entry in this document quoted "9.7 dB,
1.7 dB below textbook". That was measured on the COMPOSITE FILE over the FULL BAND, where the
chirp's low crest and high RMS inflate the denominator — it is NOT comparable to the per-frame
in-band figure and understated the real crest by ~4-6 dB. Use the tool, not an ad-hoc
peak/RMS of a whole file. **Reproduce (3)-(4):** `tools/measure_analog_path.py` — see its docstring.

### Validity of the (3)-(4) measurement
- Taken at the **operational level**: capture RMS **0.0924** vs **0.082** during real transfers.
- Operator panel confirmed the probe sat in the simulator's linear range: **Lvl ~650 mV p-p
  green, CF 2.10 / 6.5 dB** against a predicted ~790 mV / CF 2.2.
- Deliberately run at digital peak **0.38**, not the production 0.70, so clipping could not
  confound tilt and distortion. Clipping is measured separately in §3.
- Run in **WGN** (non-fading): fading modulates the tones and makes tilt indistinguishable from
  fades. The simulator's own 40 dB noise is de-embedded arithmetically.

---

## 2. The IONOS input-level contract (from the manual, rev 2.03)

- Target internal level **200-1200 mV p-p** (`Lvl=`), displays **green** in range.
  Setup procedure says adjust the modem output to **200-1100**.
- Full usable internal range 0-2000 mV p-p; a second monitor trips red above **1800 mV p-p** at
  the mixIQ1234 mixer. Max simulator output 2000 mV p-p.
- The 200-1200 window exists **"to accommodate fading and peaking in multipath modeling"** — so
  a level that is merely legal in WGN may not be legal on MPG/MPM.
- **Crest Factor** is displayed when Lvl > 50 mV, in RF power metrology convention where a pure
  sine = **1.00 (0 dB)**. **Measured convention (2026-07-26): the firmware displays a VOLTAGE
  ratio normalised so a sine reads 1.00, with dB = 20·log10(CF).** Verified: a sine read
  `CF 1, 0.01 dB`; our OFDM payload (voltage peak/RMS 3.06, i.e. 3.06/√2 = 2.16) read `CF ≈ 2.10,
  6.5 dB`. An earlier prediction of ~4.7 used the power convention and was wrong.
- **S:N is SYNTHESISED from the dial** ("Adjusting the S:N lower should increase the noise"). The
  simulator has **no fixed noise floor** our signal must climb above. Therefore on this bench
  **absolute level and PAPR do NOT set SNR.** PAPR is a real-radio lever (peak-limited PA), not a
  throughput lever here.

### ⚠ The meter trap
`Lvl=` is a **smoothed/averaged** peak. With the SAME digital peak (0.70), a sine read **1458 mV
(RED)** while the OFDM payload read **~1100 mV (green)**. Same digital peak and same analog gain
means the true analog peak is **identical (~1458)** — physics does not allow otherwise. So the
green indication is **misleading for a high-crest signal**.

**Set levels with a SINE at the production digital peak**, not with the data waveform, and not
with a PING (a PING's sustained crest is 3.0 dB vs the payload's **13.2-15.7 dB per-frame in-band**
(`build/papr_tx_measure`) — a >10 dB difference, so a PING-based setup puts the payload somewhere
else entirely).

---

## 3. The one real analog defect found

We **over-drive the simulator input by 1.7 dB**: true peak ~**1458 mV** vs the 1200 mV ceiling.

- Impact is **minor**: hard-limiting at 1200 mV clips **0.58%** of payload samples, implying a
  **37.3 dB SNDR ceiling** — far above the operating SNR.
- Worth fixing anyway because it is **free** and because multipath peaking has no headroom at
  1458.
- **Fix on the simulator, not the modem:** reduce **CH-IN ~3 dB** so a sine at digital peak 0.70
  reads ~1000 mV green. On an S:N machine that costs **no SNR**. Cutting `tx_drive` 0.70 → 0.48
  would instead throw away 3.3 dB of real average power.

---

## 4. What this document does NOT cover

- **Carrier/sample-clock jitter** was NOT re-measured. `BUG-IONOS-PI5-CHEAP-DAC` recorded ±7 Hz
  jitter on the *previous* cheap USB dongle; the Fe-Pi has not been characterised for jitter.
  Only tilt, distortion and SNDR are verified here.
- The **~5.4 dB sim-vs-rig SNR discrepancy is now DECOMPOSED (2026-07-26) and is mostly NOT
  analog loss.** Baseline 20.1 − 14.68 = 5.42 dB. Signs: **+** explains the gap, **−** enlarges it.

  | mechanism | dB | tag |
  |---|---|---|
  | ring averages **dB** while the dial/anchors are **mean-power** (Jensen) | **+1.74** | proven |
  | **rung mismatch** — the sim gate is QPSK R1/4 ONLY; the rig ran QPSK R1/2·R2/3·R3/4, 8PSK R2/3, 16QAM R1/2·R2/3 (different pilot spacing) | +0.5…+1.1 | likely |
  | **analog SNDR ceiling 30.7 dB** | **+0.36** | proven |
  | 1200 mV clipping (structurally cancels: the two LTS symbols are identical waveforms, so a memoryless nonlinearity lands in the SIGNAL term) | +0.02…+0.10 | proven |
  | tilt / quantization / AGC / RX gain — meter is a pure measured ratio, **level-invariant by construction**, verified bit-identical over 0.0625×–4× RX level | **+0.00** | proven |
  | residual inter-LTS CFO inflating the **noise reference** — OUR METER, not the radio | +0.5…+2.0 | likely |
  | guard-bin `max()` branch mix (wins ~46% in sim vs **1.1%** on the rig) | −0.5 | proven |
  | white-noise band conversion, optimistic on band-limited noise | −0.47 | proven |
  | **unexplained residual** | **≈2 (1.1–3.3)** | — |

  **Pure statistics/reference mismatch = 2.2–2.8 dB (41–52%). Measured analog loss = 0.38–0.46 dB
  (~8%).** So the honest open question is *"why is there still ~2 dB"*, not *"where did 5.3 dB go"*.
  The only large remaining candidate is **crest-referenced S:N synthesis in the bench** (if its S
  detector is quasi-peak rather than average-power, a 14 dB-crest signal is penalised) — UNMEASURED.
  Weak supporting datum: the structurally independent MC-DPSK estimator (8 carriers, different noise
  reference) reads **1.5–2.2 dB higher** than the 59-carrier OFDM meter on the same runs, in the
  direction crest-referencing predicts, but far short of the crest difference and confounded by
  MC-DPSK's own ~2 dB fade-averaging penalty.
- **Per-run scatter:** rig power-means span **14.96–19.13 dB** across 8 nominally identical MPG@20
  transfers. **Every single-number claim on this bench needs ±1.5 dB.**
- **Decisive next experiment:** three signals at the SAME in-band RMS (0.082) but different crest —
  sine (3 dB), multitone (~10 dB), real OFDM burst (14.05 dB) — through **WGN at dial 20**. If
  delivered S/N tracks the crest difference, the bench is peak-referencing and owns the residual; if
  it is flat within ±0.5 dB, that hypothesis is dead and the residual is ours.
- **Superseded:** `BUG-IONOS-PI5-CHEAP-DAC`'s 14.8 dB tilt / −17.8 dB distortion figures describe
  a card no longer in the path. See the banner on that entry.
