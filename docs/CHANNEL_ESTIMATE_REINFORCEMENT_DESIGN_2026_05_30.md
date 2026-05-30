# Channel-Estimate Reinforcement — Design (2026-05-30)

**Goal:** close the *estimation half* of the 16QAM-on-Good@20 decodability gate
([[project_16qam_gate_is_two_parts]]) — get **full-chirp-quality H at light-LTS airtime** —
**and** do it SNR-adaptively so it scales DOWN to low SNR (the modem must work below 20 dB
eventually; 20 dB is the *cheap end*, not a special case). Benefits 16QAM most but helps
QPSK/8PSK too (a cleaner H is universal).

Written under the 4-tier stack (PHY theorist primary; DSP systems; HF operator; physics
escape hatch). Every parameter is DERIVED from SNR/channel in scope — no constant tuned at
SNR 20 (CLAUDE.md adaptivity rule).

## Diagnosis (measured, grounded in code)

The 16QAM folds are DECODE failures, not sync/SNR: bursts acquire at the same corr≈0.16
(clean & failed alike), in_band_snr 18–20 dB, yet failed frames hit LDPC `max_iters=50
quality=0` — the LLRs are wrong. Warm-off A/B proved one half is **H-estimate quality**:
strong anchor (full chirp+LTS) → group 0 `iters=1 q=0.99`; warm light-LTS → stuck. We are
UNDER-EXTRACTING the LTS:
- **`channel_equalizer_lts.cpp:459`** — we transmit **2** LTS symbols but use only the
  **last** for H, deliberately ("averaging causes phase mismatch with residual CFO"). We
  throw away ~3 dB of averaging.
- **No frequency-domain (DFT/MMSE) denoise** on the LTS H — raw per-carrier LS. The channel
  is smooth in frequency (Good ≈0.5 ms delay spread → few taps); the LS keeps full noise.
- **Wiener interpolator is "MODERATE-HF baked"** (`channel_equalizer_pilot.cpp:23`) — not
  derived from the negotiated channel.

The chirp was never the source of H strength — the **LTS processing** is. The chirp is for
*cold acquisition* (timing/coarse-CFO).

## The design: "reinforced LTS anchor", SNR-adaptive

### The anchor itself is SNR-adaptive (the key reframe)
Dropping the chirp is a HIGH-SNR luxury — the chirp is acquisition processing gain; at low
SNR the warm light-LTS can't give reliable timing/CFO. So:
> **anchor strength ∝ 1/SNR** — warm light-LTS + reinforced estimate at high SNR (cheap),
> full chirp+LTS at low SNR (robust). Derived from measured in-band SNR + channel class.

"Fast AND strong" → "fast when affordable, strong when needed." The estimate reinforcement
below rides on EITHER anchor.

### Levers (gain order)

**① CFO-clean 2-symbol averaging (+3 dB).** The 2 LTS symbols are identical → their phase
diff IS the residual CFO. De-rotate both to a common phase, THEN average → −3 dB H-noise,
no phase mismatch (removes the exact reason `:459` uses last-symbol-only).
- *Low-SNR:* the inter-LTS CFO estimate is noisy → fall back to the **chirp CFO** (trusted
  per CFO invariants) + LTS refine. The averaging gain matters MORE at low SNR.

**② DFT-domain denoise (several dB; the low-SNR workhorse).** IFFT H → zero taps beyond the
delay-spread window (+CP guard) → FFT back. Denoise ≈ 10·log₁₀(N_carriers/L), SNR-independent
→ removes proportionally MORE noise at low SNR. Standard DFT-based estimation.
- *Adaptive:* window = the **negotiated channel's** delay spread (a-priori from the channel
  class), NOT per-burst estimated (can't separate taps from noise at low SNR). When a chirp
  IS present, its matched-filter impulse response can refine the delay-spread window.

**③ Pilot Wiener, re-tuned per channel.** Existing per-symbol tracking seeds from the LTS
anchor and tracks drift over the burst (Good Tc≈4.2 s). MMSE weights =
signal_var/(signal_var+**measured noise_var**) → SNR-adaptive by construction. Delay-spread
param from the channel, not the Moderate constant. (Denser pilots at low SNR = future lever.)

**④ Decision-directed sharpening — gated.** On frequency-flat carriers only (existing
BUG-8PSK-001 fading gate), refine H with hard decisions. *Low-SNR:* gate OFF below an SNR
threshold (hard decisions wrong too often → poison H). Extend the fading gate to gate on SNR.

**⑤ Iterative / turbo estimation — ESCALATION (only if ①–④ fall short of genie).** Feed the
LDPC decoder's SOFT output back to re-estimate H (the whole codeword acts as pilots),
iterate. State-of-the-art for 16QAM-on-fading; squeezes the last dB the linear estimator
can't. BUT couples decoder+equalizer, adds latency — much bigger lift. Hold unless needed.

### SNR-adaptive principle (correct across the family)
Nothing is a constant tuned at 20 dB:
- anchor (chirp vs warm) ← measured SNR + channel class
- DFT window ← channel delay spread (a-priori; chirp impulse response when present)
- Wiener weights ← measured noise variance
- DD gate ← SNR AND per-carrier flatness

Same code: full-chirp-quality H at 20 dB for cheap, AND holds at 12 dB by leaning on the
chirp + DFT denoise. 16QAM is inherently high-SNR (~18 dB+); at 10–14 dB the family drops to
QPSK/8PSK and the reinforced H makes THOSE more robust too.

## Do we need to change the chirp?
**No** — for this work. The chirp does acquisition (timing/coarse-CFO); the LTS does
estimation; we reinforce the LTS PROCESSING. The chirp can *optionally assist* (its
matched-filter impulse response → delay-spread for the ② DFT window when present). Separate
future levers (not this design): SNR-adaptive LTS LENGTH (more symbols at low SNR for more
averaging) and chirp processing gain for very-low-SNR acquisition.

## What this does NOT fix
The **frequency-null half** of the gate — no estimator conjures signal on a nulled carrier.
That's bit-loading / carrier-diversity / interleaving (separate workstream). Recipe to 3000
= reinforced estimate (this) + null mitigation (that).

## Implementation + proof
- `channel_equalizer_lts.cpp::estimateChannelFromLTS`: ① CFO-clean averaging (chirp-CFO
  fallback), ② DFT denoise (window from negotiated delay spread).
- `channel_equalizer_pilot.cpp`: Wiener delay-spread/noise from channel+measurement, not
  the Moderate constant; ④ DD SNR-gate.
- Anchor-selection (warm vs chirp) keyed on measured SNR + channel class.
- **Proof:** 16QAM R2/3 Good@20 **warm** (light-LTS) with the reinforced estimate → FER
  falls to the warm-off (full-chirp) level WITHOUT the chirp tax; distance to genie-3764.
  Multi-seed faithful GUI. Then sweep DOWN in SNR (QPSK/8PSK at 12–16 dB) to confirm the
  adaptive params hold — NOT just the 20 dB point.

## Build order
1. **② DFT denoise** — biggest gain, most SNR-robust, adaptive window from day one.
2. **① CFO-clean averaging** — with chirp-CFO fallback baked in.
3. ③/④ adaptivity framework (Wiener-from-channel, DD SNR-gate).
4. ⑤ iterative — only if genie gap remains.
