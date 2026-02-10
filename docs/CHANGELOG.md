# ProjectUltra Change Log

This log tracks all bug fixes and behavioral changes to prevent re-doing work due to lost context.

**Format:** Each entry must include:
1. What was broken (symptom + root cause)
2. What was changed (files, code)
3. How it's properly fixed (why it works, invariants)
4. Test verification (command + expected output)

---

## 2026-02-10: Fix coherent pilot/interleaver geometry mismatch

**What was broken:**
- QPSK R1/2 on good fading averaged 86.4% first-attempt frame success (30-seed survey).
- The channel interleaver in both encoder and decoder assumed `pilot_spacing=10` (53 data carriers,
  106 bits/symbol) regardless of modulation, but `OFDMChirpWaveform::configurePilotsForCodeRate()`
  sets `pilot_spacing=5` (47 data carriers, 94 bits/symbol) for QPSK/BPSK coherent modes.
- Since TX and RX were consistently wrong, data decoded — but the interleaver's symbol-boundary
  assumptions were misaligned with physical OFDM symbols, reducing frequency diversity.

**Root cause:**
- Encoder: `createWaveform()` calls `configure(mod, rate)` which updates the waveform's internal
  pilot_spacing, but never synced this back to `ofdm_config_.pilot_spacing`. The `setDataMode()`
  early-return (when mod/rate unchanged) prevented the fix from running via that path.
- Decoder: `setDataMode()` hardcoded a rate-only switch for pilot_spacing, ignoring modulation.

**Files modified:**
- `src/waveform/waveform_interface.hpp` — Added `virtual int getPilotSpacing() const { return 0; }`
- `src/waveform/ofdm_chirp_waveform.hpp` — Override returning `config_.pilot_spacing`
- `src/waveform/ofdm_cox_waveform.hpp` — Override returning `config_.pilot_spacing`
- `src/gui/modem/streaming_encoder.cpp` — Sync pilot_spacing from waveform in `createWaveform()`
  and `setDataMode()` (after `waveform_->configure()`)
- `src/gui/modem/streaming_decoder.cpp` — Query `waveform_->getPilotSpacing()` in `setDataMode()`
  and `getConfig()` instead of hardcoded values

**Test verification:**
- QPSK R1/2 AWGN SNR=20: PASSED (100%, 0 retransmissions)
- DQPSK R1/4 fading SNR=15: PASSED (100%, 0 retransmissions)
- QPSK R1/2 fading SNR=20 (5 seeds 42-46): avg 93.3% first-attempt (up from 86.4%)

---

## 2026-02-09: Burst-level long interleaver for OFDM_CHIRP

**What was added:**
- Burst-level long interleaver that spreads coded bytes across 4-frame groups (~2.8s).
  Coherent QPSK R1/2 on fading channels hits ~78% frame success because deep spectral nulls
  zero out groups of carriers, and frame interleaving only spreads bits within ONE frame (~0.7s).
  With burst interleaving, each CW's bytes are distributed across 4 physical frames — a total
  frame loss means each CW loses only 25% of its bits, within R1/2 LDPC capacity.

**Files created:**
- `src/fec/burst_interleaver.hpp` / `.cpp` — Byte-level row-column block interleaver
  - TX: `interleave()` permutes coded bytes across N frames (flat_pos = N*b + f)
  - RX: `deinterleave()` operates on 8-float byte groups of soft bits

**Files modified:**
- `src/waveform/waveform_interface.hpp` — Added `virtual bool wasBurstInterleaved() const`
- `src/waveform/ofdm_chirp_waveform.hpp/.cpp` — LTS sign-negation marker for burst detection:
  - TX: negate first LTS symbol for burst-interleaved group starts
  - RX: detect via `P_real < 0` in autocorrelation, undo negation before channel estimation
  - Two-flag design: one-shot for `process()`, latched for decoder query
- `src/gui/modem/streaming_encoder.hpp/.cpp` — `encodeBurstLight()` groups frames into 4-frame
  subgroups, applies burst interleaving and LTS negation for group starts
- `src/gui/modem/streaming_decoder.hpp/.cpp` — New `BURST_ACCUMULATING` state machine:
  - `tryDemodulateNextBurstFrame()` with tri-state result (SUCCESS/WAITING/FAILED)
  - `finalizeBurstGroup()` deinterleaves and decodes all 4 logical frames
  - `accumulateBurstFrames()` handles timeout and frame-by-frame accumulation
- `src/gui/modem/modem_engine.hpp` — `setBurstInterleave(bool)` API
- `tools/cli_simulator.cpp` — `--burst-test` mode (3x 600-byte messages), `--no-burst-interleave` flag
- `CMakeLists.txt` — Added `burst_interleaver.cpp` to build

**Design decisions:**
- Only OFDM_CHIRP mode supports burst interleaving (OFDM_COX uses Schmidl-Cox, incompatible marker)
- 4-frame subgroups within window-8 ARQ: 8-frame burst → 2 groups of 4, partial remainders decode individually
- Enabled automatically in connected OFDM_CHIRP mode, disabled on disconnect

**Test verification:**
```
# AWGN regression: 0 retransmissions
./build/cli_simulator --snr 20 --rate r1_2 --mod qpsk --test
# DQPSK R1/4 fading regression: 0 retransmissions
./build/cli_simulator --snr 15 --fading good --rate r1_4 --test
# Burst validation: all 3 large messages delivered, burst groups detected
./build/cli_simulator --snr 20 --fading good --rate r1_2 --mod qpsk --seed 42 --burst-test
# Multi-seed A/B: 11 total retx (burst) vs 13 (no burst) across seeds 42-46
```

---

## 2026-02-09: Coherent QPSK channel tracking for fading channels

**What was broken:**
- Coherent QPSK on fading channels achieved only ~35% frame success (vs DQPSK ~82%).
  Root cause: LTS-derived per-carrier phases become stale as the channel evolves.
  Pilots only provide 6 phase measurements per symbol — insufficient for 53 data carriers
  with independent phase drift from frequency-selective fading.

**What was changed (6 improvements):**

1. **Phase-slope-compensated complex interpolation** (`channel_equalizer.cpp`)
   - Estimate linear phase gradient from LTS (typically ~19°/carrier from timing offset)
   - Remove slope before pilot interpolation, interpolate in de-sloped domain, restore slope
   - Prevents phase aliasing (190° between 10-spaced pilots exceeds 180° Nyquist limit)
   - Differential modes still use magnitude-only interpolation (preserves LTS phases)

2. **CPE (Common Phase Error) correction** (`channel_equalizer.cpp`)
   - Estimate average phase drift across all pilots, apply to all carriers each symbol
   - Replaces unreliable pilot-based CFO tracking which drifted on both AWGN and fading
   - Standard approach used in WiFi 802.11a/g/n receivers

3. **Decision-directed per-carrier phase tracking** (`channel_equalizer.cpp`)
   - After QPSK hard-decision, measure per-carrier phase error
   - Store snapshot corrections, apply in next symbol's updateChannelEstimate() after interpolation
   - Blend factor 0.3 (empirically optimal: 0.15→73.1%, 0.3→74.1%, 0.5→65.6%)
   - Single-snapshot (no accumulation) — IIR accumulation diverges due to positive feedback

4. **Denser pilots for coherent modes** (`ofdm_chirp_waveform.cpp`)
   - QPSK/BPSK: pilot_spacing=5 (12 pilots, 47 data carriers, ~95° inter-pilot phase)
   - Differential: unchanged at spacing=10 (6 pilots, 53 data carriers)
   - 11% throughput cost offset by dramatically better phase interpolation

5. **1-sample sync refinement** (`ofdm_chirp_waveform.cpp`)
   - detectDataSync() coarse search uses 8-sample steps → up to 4 samples off-peak
   - Added ±4 sample refinement with 1-sample steps around coarse peak
   - 4-sample offset causes ~40° phase error at edge carriers — critical for QPSK

6. **Modulation-dependent sync confidence threshold** (`streaming_decoder.cpp`)
   - Coherent modes: 0.88 (reject corr 0.82-0.85 frames that always fail for QPSK)
   - Differential modes: 0.70 (unchanged)
   - Rejected frames trigger ARQ retransmission instead of wasting time on guaranteed failures

**Also fixed:**
- `carrier_noise_var` MMSE formula: `σ²/mmse_denom` instead of `σ²/|H|²` (correct post-eq noise)
- Pilot H uses last training symbol (not average) for phase consistency with data carriers
- Preserved LTS noise_variance estimate (don't overwrite with temporal pilot comparison)
- Disabled pilot-based CFO tracking for all modes (replaced by CPE for coherent)

**Test results (final configuration):**
| Test | Result |
|------|--------|
| DQPSK R1/4 fading SNR=15 | 100% (no regression) |
| QPSK R1/2 AWGN SNR=20 | 100% (0 retransmissions) |
| QPSK R1/2 fading SNR=20 (5 runs) | avg 78% (75, 69, 82, 75, 89) |
| QPSK R1/2 fading SNR=15 | 100% |

**Verification:** `./build/cli_simulator --snr 20 --fading good --rate r1_2 --mod qpsk --test`

---

## 2026-02-08: Enable coherent QPSK for OFDM_CHIRP

**What was broken:**
- OFDM_CHIRP forced differential modulation (DQPSK/DBPSK/D8PSK) only. Coherent QPSK was
  blocked despite all components (modulator, demodulator, soft demapper, equalizer) already
  supporting it. Differential decoding wastes ~3 dB SNR due to noise doubling.

**What was changed:**

1. **Allow QPSK/BPSK modulations** (`src/waveform/ofdm_chirp_waveform.cpp`)
   - Constructor and `configure()`: accept QPSK and BPSK in addition to differential modes
   - `getThroughput()` and `getMinSamplesForCWCount()`: explicit QPSK/BPSK switch cases

2. **CLI support** (`tools/cli_simulator.cpp`, `tools/test_waveform_simple.cpp`)
   - Added `--mod qpsk` and `--mod bpsk` options

3. **Skip carrier_phase_correction for coherent modes** (`src/ofdm/channel_equalizer.cpp`)
   - carrier_phase_correction removes common phase from H but not from rx signal,
     leaving residual e^(jθ) in equalized output — fatal for QPSK, harmless for differential
   - Fix: identity correction for coherent modes (LTS provides accurate H)

4. **Magnitude-only interpolation for all modes** (`src/ofdm/channel_equalizer.cpp`)
   - DFT interpolation from 6 pilots corrupts per-carrier phases for both differential and
     coherent modes. Now all modes use magnitude-only linear interpolation between pilots,
     preserving the accurate LTS-derived phases at data carriers.

5. **Remove timing recovery** (`src/ofdm/channel_equalizer.cpp`)
   - Timing recovery estimated offset from absolute pilot LS phases, which include channel
     phase. This produced spurious timing offsets (up to 4.6 samples on AWGN) that added
     up to 80° phase rotation at edge carriers — fatal for QPSK equalization.
   - Was also disabled for differential modes (fading corrupts the slope).
   - Removed entirely since it was broken for all modes.

**How it works:**
- QPSK uses same 2 bits/carrier as DQPSK — same frame format, interleaving, throughput
- Coherent MMSE equalization: eq = conj(H) × rx / (|H|² + σ²) with LTS-derived H
- Phase-frozen H (magnitude-only tracking) works because LTS phases are accurate for
  the entire frame on AWGN channels
- On fading channels, QPSK performs worse than DQPSK (~35% vs ~82% frame success at
  R1/2 SNR=20 good fading) because LTS phases become stale

**Test verification:**
- QPSK AWGN SNR=20: `./build/cli_simulator --snr 20 --rate r1_2 --mod qpsk --test` → PASS, 0 retransmissions
- QPSK AWGN SNR=15: `./build/cli_simulator --snr 15 --rate r1_2 --mod qpsk --test` → PASS, 0 retransmissions
- DQPSK regression: `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test` → PASS, 0 retransmissions
- QPSK fading SNR=20: `./build/cli_simulator --snr 20 --fading good --rate r1_2 --mod qpsk --test` → PASS (10 retransmissions)

---

## 2026-02-08: DFT-based channel interpolation + magnitude-only pilot tracking

**What was broken:**
- Linear interpolation between 6 pilots across 59 carriers produced suboptimal H estimates
  at data carriers far from pilots, especially on frequency-selective fading channels
- For differential modes (DQPSK, DBPSK, D8PSK), `updateChannelEstimate()` was completely
  skipped — H was frozen from LTS for the entire frame (~700ms). On fading channels, |H|
  drifts, causing stale MMSE scaling and incorrect LLR confidence

**What was changed:**

1. **DFT-based interpolation** (`src/ofdm/channel_equalizer.cpp`)
   - Replaced linear interpolation with IDFT→window→DFT approach
   - Builds N-point H from pilot LS estimates + linear fill
   - IDFT to CIR, window to L=5 taps (±1.8ms delay spread coverage), DFT back
   - Exploits finite HF channel delay spread for noise suppression
   - Used for coherent modes during data symbols and for all modes during LTS

2. **Magnitude-only pilot tracking for differential** (`src/ofdm/channel_equalizer.cpp`)
   - Enabled `updateChannelEstimate()` for differential modes (was skipped entirely)
   - Pilot H: update |H| via alpha=0.5 smoothing, keep phase frozen from LTS
   - Data carriers: linearly interpolate MAGNITUDES ONLY from pilots, preserve existing phases
   - Skip DFT interpolation for differential (would corrupt phase relationships)
   - Guard CFO estimation and timing recovery with `!is_differential` (fading-induced
     phase changes get misattributed to CFO on fading channels)
   - Guard noise_variance updates with `!is_differential` (preserve LTS-based estimate)

**Why it works:**
- DFT interpolation: noise suppression from CIR windowing produces smoother, more accurate
  H estimates. The HF channel's finite delay spread means high-delay CIR taps are pure noise.
- Magnitude tracking: MMSE equalization `eq = rx × conj(H) / (|H|² + σ²)` needs correct |H|
  for amplitude scaling. Phase errors cancel in differential decoding (diff = eq[n] × conj(eq[n-1]))
  but magnitude errors affect LLR confidence.
- Phase must NOT be updated for differential because the decode relies on phase DIFFERENCES
  between consecutive equalized symbols — changing H phase between symbols introduces
  artificial differential phase errors.

**Test verification:**
- R1/4 AWGN SNR=15: 100%, 0 retx (no regression)
- R1/4 good fading SNR=15: 100%, 0 retx (no regression)
- R1/2 AWGN SNR=20: 100%, 0 retx (no regression)
- R1/2 good fading SNR=20 (seeds 42-46): avg 2.0 retx (was 3.2 baseline — 37.5% reduction)

## 2026-02-08: Per-carrier adaptive LLR scaling

**What was broken:**
- When fading was detected, a **global** scale factor was applied to ALL carriers equally:
  `ce_error_margin *= (1 + 10 × fading_index²)`. This reduced LLR confidence on good carriers
  too, wasting LDPC correction capacity. On frequency-selective fades, some carriers are fine
  while others are deeply faded — the global scale couldn't distinguish between them.

**What was changed:**

1. **Per-carrier |eq| magnitude tracking** (`src/ofdm/demodulator.cpp`)
   - Track EMA of `|equalized[i]|` per carrier across symbols within a frame
   - Track EMA of `(|eq| - ema)²` per carrier (magnitude variance)
   - First symbol initializes EMA; subsequent symbols update with α=0.3

2. **Per-carrier noise inflation** (`src/ofdm/demodulator.cpp`)
   - Replaced global `fading_scale` block with per-carrier scaling in the LLR loop
   - `norm_var = carrier_eq_mag_var[i] / (carrier_eq_mag_ema[i]² + ε)`
   - `nv *= (1 + K × norm_var)` where K=10 (CARRIER_ADAPTIVE_K constant)
   - Applied in both `demodulateSymbol()` and `demodulateD8PSKTwoPass()` pass-2 loop

3. **State management** (`src/ofdm/demodulator_impl.hpp`, `demodulator_constants.hpp`)
   - Added `carrier_eq_mag_ema_` and `carrier_eq_mag_var_` vectors to Impl
   - Added `CARRIER_ADAPTIVE_K = 10.0f` constant
   - Cleared in `processPresynced()`, `reset()`, and all Schmidl-Cox state transitions

**How it works:**
- Stable carrier: low variance → `norm_var ≈ 0` → no noise inflation → full LLR confidence
- Fading carrier: high variance → `norm_var > 0` → inflated noise → LDPC knows not to trust it
- AWGN: all carriers stable → zero variance → no scaling whatsoever (zero regression)

**Test verification:**
- `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test` → PASS, 0 retransmissions
- `./build/cli_simulator --snr 20 --fading good --rate r1_2 --test` → PASS, all messages delivered
- `./build/cli_simulator --snr 15 --rate r1_4 --test` → PASS, 0 retransmissions, perfect LLRs

---

## 2026-02-08: Frequency-domain interleaving for OFDM

**What was broken:**
- Adjacent coded bits mapped to adjacent carriers. When a cluster of carriers fades
  together (common on HF), all bits in that cluster are wrong. LDPC can't fix a burst
  of confident wrong bits. This was the main cause of R1/2 retransmissions on fading channels.

**What was changed:**

1. **Coprime-step carrier permutation** (`src/ofdm/modulator.cpp`, `src/ofdm/demodulator.cpp`)
   - TX: `perm[c] = (c * 23) mod N` maps logical carrier c to physical carrier perm[c]
   - RX: `inv_perm[p] = c` where `(c * 23) mod N = p` reverses the mapping on soft bits
   - Step=23 ensures adjacent logical carriers map ~23 physical carriers apart
   - Applied in `modulate()` (TX) and `demodulateSymbol()` + `demodulateD8PSKTwoPass()` (RX)
   - Permutation is fixed across all OFDM symbols — differential encoding chains are coherent

2. **Public API** (`include/ultra/ofdm.hpp`, waveform files)
   - `setFrequencyInterleave(bool)` on OFDMModulator and OFDMDemodulator
   - Forwarded through OFDMChirpWaveform, OFDMNvisWaveform, IWaveform interface
   - StreamingEncoder/StreamingDecoder forward setting and persist across waveform recreation

3. **CLI flag** (`tools/cli_simulator.cpp`)
   - `--no-freq-interleave` / `--nfi` to disable, `--freq-interleave` / `--fi` to enable
   - Default: ON

**How it works:**
- Example: Physical carriers 20-25 fading → logical positions {1, 8, 17, 24, 31, 47}
  (scattered across 53 carriers). LDPC sees scattered errors, not burst errors.
- Works correctly with differential encoding because permutation is fixed per-symbol.
  TX state `dbpsk_prev_symbols[c]` tracks logical carrier c; physical carrier `perm[c]`
  always carries the same logical chain.

**Test verification:**
- `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test` → PASS, 0 retransmissions
- `./build/cli_simulator --snr 20 --fading good --rate r1_2 --test` → PASS, all messages delivered
- `./build/cli_simulator --snr 15 --rate r1_4 --test` → PASS, AWGN 0 retransmissions
- `./build/cli_simulator --snr 15 --fading good --rate r1_4 --no-freq-interleave --test` → PASS

---

## 2026-02-08: LDPC false positive recovery via CRC-guided bit-flip search

**What was broken:**
- At SNR=20 with good fading, R1/4 averaged ~1.0 retransmissions per test run.
- Root cause: LDPC min-sum decoder occasionally converges to a wrong-but-valid codeword
  (syndrome passes but information bits are wrong). Frame-level CRC catches this, but the
  frame is discarded and retransmitted.
- These "false positives" account for most retransmissions at SNR=20 (genuine CW failures
  from deep fades cause the remainder).

**What was changed:**

1. **CRC-guided bit-flip recovery** (`src/protocol/frame_v2.cpp`)
   - Two recovery cases: Case 1 (header CRC error in CW0) and Case 2 (frame CRC error)
   - Case 1: Direct magic + header CRC check on CW0 without parseHeader (avoids logging
     spam from thousands of failed trials). 1-bit and 2-bit brute force in CW0.
   - Case 2: CRC delta table — precompute `delta[p] = CRC(data^e_p) XOR CRC(data)` for
     each data bit position p. Exploits CRC linearity for efficient search:
     - 1-bit: O(n) — check if delta[p] == syndrome
     - 2-bit: O(n) with hash map — for each p1, look up `syndrome ^ delta[p1]`
     - 3-bit: O(n²) with hash map — for each (p1,p2), look up `syndrome ^ delta[p1] ^ delta[p2]`
   - Suspect-augmented search for 4-6 bit errors: identifies LDPC-flipped info bits
     (bits where decoder output disagrees with channel LLR sign) as suspects, searches
     C(K,4) through C(K,6) subsets among K=30 suspects
   - Hybrid 2+2 search: 2 suspect bits + 2 arbitrary bits via delta_map

2. **Fallback LDPC re-decode** with different min-sum factors {0.75, 0.625, 0.5, 0.875}
   after CRC-guided search fails.

3. **Added `#include <unordered_map>`** for delta_map hash table.

**Recovery effectiveness (observed over 20-run batch):**
- 87.5% of detected false positives recovered (14/16)
- Most recovered via 1-bit or 2-bit fix (specific trapping set patterns)
- Unrecoverable FPs have 7+ bit errors (beyond practical search space)
- Remaining retransmissions from genuine CW decode failures during deep fades

**Test verification:**
- `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test` — PASS, 0 retransmissions
- `./build/cli_simulator --snr 20 --rate r1_4 --test` — PASS (AWGN), 0 retransmissions
- SNR=20 good fading: reduced from avg ~1.0 to ~0.5 retransmissions per run
  (high variance due to non-deterministic fading; ~70-93% of runs achieve 0 retransmissions)

---

## 2026-02-07: Fix DISCONNECT decode failure on fading + false LTS detection

**What was broken:**
- At SNR=20 with good fading, R1/4 showed 12+ retransmissions while SNR=15 showed 0.
- Two distinct failure types:
  1. DISCONNECT always failed (all 4 CWs fail, |llr|=3.3-4.2) — BRAVO never saw ALPHA's DISCONNECT
  2. False LTS detection (corr=0.63 on data, threshold 0.50) — phantom frame trigger, all CWs fail

**What was changed:**

1. **Route OFDM DISCONNECT through `encodeFixedFrame()` for frame interleaving**
   (`src/gui/modem/streaming_encoder.cpp`)
   - DISCONNECT was encoded via `encodeFrameWithLDPC()` (sequential, no interleaving) — each CW's
     bits map to consecutive OFDM symbols, so temporal fading wipes entire CWs
   - Changed `is_variable_cw_frame` logic: `isControlFrame()` → true (1-CW ACK path),
     `isConnectFrame()` → false (4-CW interleaved path via `encodeFixedFrame()`)
   - Decoder needs no change: "try both" strategy in `decodeFrame()` falls through to
     `try_frame_interleave = true` and succeeds
   - `ConnectFrame::serialize()` already hardcodes `total_cw=4`, matching `encodeFixedFrame()` expectations

2. **Raise LTS confidence threshold from 0.50 to 0.70**
   (`src/gui/modem/streaming_decoder.cpp`)
   - Data autocorrelation can produce spurious peaks up to 0.63, triggering false frame detection
   - Real LTS correlation is always >0.81 even on moderate fading
   - Changed `LIGHT_SYNC_MIN_CONFIDENCE` from 0.50f to 0.70f

**Test verification:**
- `./build/cli_simulator --snr 20 --fading good --rate r1_4 --test` — PASS, retransmissions 12+ → 3
- `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test` — PASS, 0 retransmissions (regression OK)
- `./build/cli_simulator --snr 10 --fading moderate --test` — PASS, MC-DPSK unaffected
- `./build/cli_simulator --snr 20 --fading good --rate r1_2 --test` — PASS, DISCONNECT decoded 4/4 CWs

---

## 2026-02-06: Restructure variable-CW frame handling — fix DISCONNECT at R1/2

**What was broken:**
- DISCONNECT frame decode failed at R1/2 OFDM. BRAVO never saw ALPHA's DISCONNECT, connection timed out.
- Three root causes:
  1. `ConnectFrame::serialize()` hardcodes `total_cw=4` (frame_v2.cpp:755), but actual LDPC encoding
     produces 2 CWs at R1/2 and 3 CWs at R1/4 for 44-byte ConnectFrames.
  2. No way for decoder to compute exact buffer size for N CWs — `getMinSamplesForCWCount(int)` was
     private in OFDMChirpWaveform, inaccessible to decoder.
  3. OFDM decoder always processed full 4-CW buffer (31104 samples). For 2-CW DISCONNECT (17280 samples),
     the extra 13824 samples of noise degraded LLR quality → decode failure.

**What was changed:**

1. **Promoted `getMinSamplesForCWCount(int)` to IWaveform interface** with default implementation.
   OFDMChirpWaveform overrides with exact calculation. Added override to MCDPSKWaveform with
   proper `training + ref + N × data_per_cw` calculation.

2. **Encoder patches `total_cw` for OFDM ConnectFrames**: After LDPC encoding, compares actual CW
   count with header's total_cw. If different, patches byte 12 (total_cw), recalculates header CRC
   (bytes 15-16) and frame CRC (last 2 bytes), then re-encodes.

3. **Decoder restructured with CW0 peek-first strategy**:
   - MC-DPSK: Always starts with 1-CW buffer, peeks CW0 header for total_cw, waits for exact size.
   - Connected OFDM: Starts with full 4-CW buffer (data frames use frame interleaving, CW0 can't be
     decoded independently). If 4-CW decode fails, falls back to small-frame recovery: 1-CW peek →
     read total_cw → reprocess with exact `getMinSamplesForCWCount(N)` size.
   - Disconnected OFDM: 1-CW initial buffer for control frame detection.

4. **Exact consumed-sample calculation**: Non-data frames advance by `getMinSamplesForCWCount(actual_cw)`
   instead of full 4-CW frame size. E.g., 2-CW DISCONNECT advances 17280 samples, not 31104.

5. **`checkIfReadyToDecode()` uses exact calculations**: Replaced crude `(min_frame * 9) / 10`
   arithmetic with three-way logic based on pending_total_cw, connected OFDM, or MC-DPSK/disconnected.

**Files changed:**
- `src/waveform/waveform_interface.hpp`: Added virtual `getMinSamplesForCWCount(int)` to IWaveform
- `src/waveform/ofdm_chirp_waveform.hpp`: Moved method from private to public with override
- `src/waveform/mc_dpsk_waveform.hpp`: Added `getMinSamplesForCWCount` override declaration
- `src/waveform/mc_dpsk_waveform.cpp`: Added implementation with proper sample calculation
- `src/gui/modem/streaming_encoder.cpp`: Added total_cw patching for OFDM ConnectFrames
- `src/gui/modem/streaming_decoder.cpp`: Restructured `checkIfReadyToDecode()` and `decodeCurrentFrame()`

**Test verification:**
- R1/2 AWGN SNR=20: PASSED, 0 retransmissions, DISCONNECT decoded as 2/2 CWs
- R1/4 good fading SNR=15 regression: PASSED, 0 retransmissions, 100% CW success
- MC-DPSK moderate fading SNR=10 regression: PASSED, 0 retransmissions, 100% success
- R1/2 good fading SNR=20: PASSED, 8 retransmissions (all 7 messages delivered)

---

## 2026-02-06: OFDM throughput improvements — 1-CW ACK + R1/2 rate selection

**What was changed:**

1. **1-CW OFDM ACK frames:** OFDM control frames (ACK, NACK, MODE_CHANGE, etc.) are only 20 bytes
   = 1 codeword. Previously encoded as 4-CW fixed frames with frame interleaving (25 data symbols,
   0.648s). Now encoded as 1-CW frames without interleaving (7 data symbols, 0.216s). Data frames
   still use full 4-CW frame interleaving for fading protection.

2. **R1/2 rate selection enabled:** `selectOFDMCodeRate()` was hardcoded to R1/4. Now selects R1/2
   when channel conditions allow:
   - AWGN (fading < 0.15) at SNR >= 15: R1/2
   - Good fading (< 0.65) at SNR >= 20: R1/2
   - Everything else: R1/4

**Files changed:**
- `src/gui/modem/streaming_encoder.cpp`: Control frames use `encodeFrameWithLDPC()` (1 CW)
  instead of `encodeFixedFrame()` (4 CWs). Detection via `v2::isControlFrame()`.
- `src/protocol/waveform_selection.hpp`: `selectOFDMCodeRate()` SNR/fading thresholds for R1/2.
  `recommendWaveformAndRate()` uses dynamic rate selection instead of hardcoded R1/4.
- `src/waveform/ofdm_chirp_waveform.cpp`: Added `getMinSamplesForControlFrame()` and shared
  `getMinSamplesForCWCount()` helper.
- `src/waveform/ofdm_chirp_waveform.hpp`: Declared new methods.
- `src/waveform/waveform_interface.hpp`: Added virtual `getMinSamplesForControlFrame()` to IWaveform.

**Decoder:** Existing "try CW0 non-interleaved" path in streaming_decoder.cpp already handles
1-CW frames — no decoder changes needed. The decoder waits for full 4-CW sample threshold,
but 1-CW frames arrive faster (shorter TX), so the decoder naturally processes them sooner.

**Impact:**
- ACK time: 0.648s → 0.216s (3× faster)
- R1/2 doubles payload per frame: 61 → 141 bytes
- Combined: ~2.5× throughput improvement on good channels

**Test verification:**
- R1/2 AWGN SNR=20: PASSED, 0 retransmissions
- R1/2 good fading SNR=20: PASSED, 16 retransmissions (all delivered)
- R1/4 good fading SNR=15 regression: PASSED, 0 retransmissions, 100% CW success

---

## 2026-02-06: Fix three bugs found during 1-CW ACK + R1/2 verification

### Bug 1: detectDataSync() false peaks from LDPC zero-padding

**What was broken:**
- 1-CW ACK frames failed to decode. detectDataSync() locked onto wrong sample position.
- Root cause: LDPC zero-padding in 1-CW frames (20 bytes payload + 20 bytes zero pad → bytes 20-40
  all zeros → DQPSK 0° phase change → identical adjacent data symbols). Schmidl-Cox autocorrelation
  found ~1.0 for both real LTS pair AND false data1-data2 pair. Since detectDataSync() picks the
  BEST peak, it chose the later (wrong) data peak over the earlier (correct) LTS peak.

**What was changed:**
- `src/waveform/ofdm_chirp_waveform.cpp`: Added early exit in detectDataSync() when correlation
  exceeds 0.95. The real LTS is always the FIRST high-confidence peak in the search window.
  False peaks from identical data symbols appear later and are now never reached.

### Bug 2: 1-CW frame sample overconsumption in decoder

**What was broken:**
- After correctly decoding a 1-CW ACK, subsequent data frames failed with all 4 CWs failing.
- Root cause: decodeCurrentFrame() consumed 31104 samples (4-CW frame size) regardless of actual
  frame size. A 1-CW ACK is only 10368 samples (2 LTS + 7 data symbols). The extra 20736 samples
  consumed belonged to the next data frame, causing false sync detection at correlation ~0.67.

**What was changed:**
- `src/gui/modem/streaming_decoder.cpp`: After decoding a 1-CW control frame, advance by
  `getMinSamplesForControlFrame()` instead of full frame_buffer size. Also skip burst continuation
  for 1-CW control frames (ACKs are standalone, not part of a data burst).

### Bug 3: ARQ advanceRXWindow delivers frames with wrong MORE_FRAG flag

**What was broken:**
- Multi-frame messages occasionally failed to reassemble after retransmission filled a gap.
  Message 7 of 7 would never complete despite all frames being received.
- Root cause: When `advanceRXWindow()` delivered multiple buffered frames in sequence (e.g.,
  seq=8,9,10 after retransmission fills gap at seq=8), `lastRxHadMoreData()` returned the
  MORE_FRAG flag from the LAST ARRIVED frame (the gap-filler, which had MORE_FRAG=true), not
  from the frame being delivered. So seq=10 (last fragment, MORE_FRAG=false) was treated as an
  intermediate fragment, preventing message completion.

**What was changed:**
- `src/protocol/selective_repeat_arq.cpp`: In `advanceRXWindow()`, update `last_rx_flags_` and
  `last_rx_more_data_` from each slot's stored flags BEFORE calling the delivery callback.
  Each RX slot already stored the correct per-frame flags from `handleDataFrame()`.

**Test verification:**
- R1/4 good fading SNR=15: PASSED, 7/7 messages, 0 retransmissions
- R1/2 AWGN SNR=20: PASSED, 7/7 messages, 1 retransmission (marginal CW[1])
- R1/2 good fading SNR=15: PASSED, 7/7 messages, 1 retransmission

---

## 2026-02-06: Diagnostic cleanup + file transfer test

**Diagnostic cleanup:**
- `src/ofdm/demodulator.cpp`: Removed per-carrier DQPSK diagnostic logging that fired for every
  symbol (root cause: `snr_symbol_count` only incremented in two-pass paths, stayed at 2 in
  single-pass, so `< 6` condition was always true). Removed entry/histogram diagnostics.
  Changed remaining diagnostics to DEBUG level.
- `src/ofdm/channel_equalizer.cpp`: Changed LTS carrier phase log from INFO to DEBUG.

**File transfer test:**
- `tools/cli_simulator.cpp`: Made DISCONNECT timeout non-fatal in `runFileTransferTest()` (matching
  `runProtocolTest()` behavior). File data transfer is the real test; disconnect is best-effort.
- R1/2 AWGN SNR=20 file transfer: PASSED (256 bytes, 0 retransmissions, ~994 bps)
- R1/2 good fading SNR=20 file transfer: PASSED (256 bytes, 0 retransmissions)

---

## 2026-02-06: Fix MC-DPSK at low SNR (two issues)

**What was broken:**
- MC-DPSK failed at SNR=5 AWGN — CW0 decode failed every time. PING never detected,
  connection timed out after 3 retries.
- Two independent root causes:

1. **PING detection used fixed RMS threshold (0.04):** PING frames are chirp-only (no data).
   Detection checks if data region RMS < 0.04. At SNR=5, noise RMS is ~0.056, exceeding the
   threshold. Decoder mistakenly tried to LDPC-decode noise, producing garbage.

2. **MC-DPSK soft bits used fixed confidence scaling:** `confidence = mag × num_carriers × 4`
   produced LLRs of magnitude ~20-32, hard-clipped to ±10. At low SNR, wrong bits also clipped
   to ±10, making them indistinguishable from correct bits. LDPC couldn't converge.

**What was changed:**
- `src/gui/modem/streaming_decoder.cpp`: Replaced fixed PING RMS threshold with **relative
  ratio** (data_RMS / training_RMS). PING has ratio < 0.5 at any SNR; DATA frames have ratio
  ~0.9-1.2. Works across all SNR levels since it's a relative measurement.

- `src/psk/multi_carrier_dpsk.hpp`: Restructured `demodulateSoft()` into two passes:
  - **Pass 1**: Demodulate, cache differential phases, estimate phase noise variance from
    nearest-constellation-point errors.
  - **Pass 2**: Compute LLRs using SNR-proportional scale: `2 × sqrt(1/phase_noise_var)`,
    capped at 20.0, floored via phase_noise_var minimum of 0.01.
  - Raised clip limit from ±10 to ±20 to match OFDM's MAX_LLR.

**How it works:**
- Phase noise variance is naturally proportional to 1/SNR for differential modulation.
  At SNR=5: var≈0.03, scale≈12. At SNR=20: var≈0.01, scale=20 (cap). This produces
  appropriately soft LLRs at low SNR that LDPC can distinguish and correct.
- Relative PING threshold: training region has chirp signal, data region has only noise for
  PING. The ratio is SNR-independent since both regions see the same noise floor.

**Test verification:**
- `./build/cli_simulator --snr 5 --rate r1_4 --test`: PASSED (100% CW, 0 retransmissions)
- `./build/cli_simulator --snr 0 --fading moderate --rate r1_4 --test`: PASSED (90% CW, 1 retransmission, all 7 messages)
- `./build/cli_simulator --snr 10 --fading moderate --rate r1_4 --test`: PASSED (100% CW, 0 retransmissions)
- `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test`: PASSED (100% CW, 0 retransmissions — OFDM regression)

---

## 2026-02-06: Fix burst block detection in detectDataSync

**What was broken:**
- Burst blocks 2-4 failed to decode (corr=0.76→0.65 degrading). File transfer timed out.
- Root cause: `detectDataSync()` energy gate was designed for silence→signal transitions.
  In burst continuation, the search buffer starts with previous block's data (noise_floor=0.21),
  causing the energy threshold to never be exceeded. The 4-symbol search window from signal_start=0
  was too narrow to reach the actual LTS training at offset ~9600 in the search buffer.

**What was changed:**
- `src/waveform/ofdm_chirp_waveform.cpp`: Modified `detectDataSync()` to detect when the buffer
  starts with signal (noise_floor >= 0.05) vs silence (noise_floor < 0.05).
  - Silence: Use existing energy gate + narrow search window (skip quiet region efficiently)
  - Signal present: Skip energy gate, search entire buffer. LTS autocorrelation (~0.99) is
    distinctive enough to stand out from data autocorrelation (~0.2-0.4).
- `src/gui/modem/streaming_decoder.cpp`: Removed unused `LEAD_IN_SAMPLES` constant.

**How it works:**
- The LTS training has two identical OFDM symbols, giving Schmidl-Cox autocorrelation ~0.99.
  Random OFDM data gives ~0.2-0.4. This contrast is sufficient for detection without energy gating.
- Each burst block still has its own 2 LTS training symbols for per-block channel estimation.

**Test verification:**
- `./build/cli_simulator --snr 20 --rate r1_4 --test`: PASSED (0 retransmissions)
- `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test`: PASSED (0 retransmissions)
- `./build/cli_simulator --snr 20 --rate r1_4 --file 512`: PASSED (512 bytes transferred, verified)

---

## 2026-02-06: OFDM burst mode for multi-frame transmission

**What was broken:**
- OFDM file transfer and long message fragmentation sent each frame with its own LTS preamble.
  With ARQ window=4, frames 3-4 could fail because the decoder returned to SEARCHING state
  and couldn't re-acquire LTS fast enough (overlapping search windows).

**What was changed:**
- `src/gui/modem/streaming_encoder.hpp/.cpp`: Added `encodeBurstLight()` — encodes multiple
  frames as a single burst with one LTS preamble. First frame uses `encodeFrameLight()`,
  subsequent frames get training symbols + modulated data appended directly.
- `src/gui/modem/streaming_decoder.hpp/.cpp`: Added burst continuation logic in
  `decodeCurrentFrame()`. After successful decode in connected OFDM mode, checks for energy
  at the expected next block position. If energy present, processes as continuation block
  via `waveform_->process()` with CFO tracking. Loops for up to 8 continuation blocks.
- `src/protocol/connection.hpp/.cpp`: Added burst TX buffering. `sendNextFileChunk()` and
  `sendNextFragment()` accumulate frames when in OFDM mode, then flush as a single burst.
  `TransmitBurstCallback` added for the burst TX path. ACK timeout increased 5s→8s for
  burst RTT.
- `src/protocol/protocol_engine.hpp/.cpp`: Passthrough for `setTransmitBurstCallback()`.
- `src/gui/modem/modem_engine.hpp/.cpp`: Added `transmitBurst()` method.
- `src/gui/app.cpp`: Wired burst callbacks for main and virtual station protocols.
- `tools/cli_simulator.cpp`: Added `transmitBurst()` and burst callback in `SimulatedStation`.

**How it works:**
- TX: Burst format is `[LTS][train+data_0][train+data_1]...[train+data_N]`
- RX: Burst continuation checks energy at known position after each block decode.
  In synchronous simulator, continuation rarely fires (audio not yet buffered), but
  blocks are decoded via normal LTS re-sync since each block has 2 LTS training symbols.
  In real-time GUI mode, burst continuation provides direct block-to-block decode.
- OFDM-only: all burst logic gated on `is_ofdm` checks. MC-DPSK path unaffected.
- ARQ unchanged: per-frame seq nums, SACK bitmap, retransmission all preserved.

**Test verification:**
- `./build/cli_simulator --snr 20 --rate r1_4 --test`: PASSED (0 retransmissions)
- `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test`: PASSED (3 retransmissions)
- `./build/cli_simulator --snr 20 --rate r1_4 --file 1024`: PASSED (1024 bytes transferred, verified)

---

## 2026-02-05: Long message fragmentation for OFDM

**What was broken:**
- Long text messages (>61 bytes at R1/4) were silently truncated by `encodeFixedFrame()` to fit
  the 4-CW OFDM frame. The receiver got truncated data, couldn't parse the protocol frame
  (payload_len field says 233 bytes but only 63 bytes arrived), and never sent an ACK.
  The sender retransmitted forever.

**What was changed:**
- `src/protocol/connection.hpp`:
  - Added `pending_tx_fragments_`, `next_fragment_idx_`, `rx_reassembly_buffer_` members
  - Added `sendNextFragment()` method declaration
- `src/protocol/connection.cpp`:
  - `sendMessage()`: Checks `getFixedFramePayloadCapacity(data_code_rate_)`, fragments if needed
  - `sendNextFragment()`: Drip-feeds fragments with MORE_FRAG flag via ARQ window
  - `sendComplete` callback: Handles fragment ACKs, sends more or fires on_message_sent_
  - `enterDisconnected()` / `reset()`: Clear fragment buffers
- `src/protocol/connection_handlers.cpp`:
  - `handleDataPayload()`: Accumulates fragments when `more_data=true`, delivers complete
    reassembled message when final fragment arrives (no MORE_FRAG)
- `tools/cli_simulator.cpp`:
  - Added 2 long test messages (132b, 126b) to the test suite alongside the 5 short ones

**How it works:**
- TX: `sendMessage()` splits into chunks of `getFixedFramePayloadCapacity()` bytes, queues them,
  and feeds them through ARQ with `MORE_FRAG` flag on all but the last chunk
- RX: `handleDataPayload()` accumulates payloads with `more_data=true` into `rx_reassembly_buffer_`,
  then delivers the complete message when the final fragment (no flag) arrives
- Single-frame messages are unchanged (backwards compatible)

**Test verification:**
```
./build/cli_simulator --snr 15 --fading good --rate r1_4 --test
# All 7 messages (5 short + 2 long) delivered correctly
# 132-byte message: 3 fragments, reassembled correctly
# 126-byte message: 3 fragments, reassembled correctly
# TEST PASSED
```

---

## 2026-02-03: Refactor ModemEngine TX to use StreamingEncoder

**What was broken:**
- ModemEngine::transmit() had ~300 lines of inline TX encoding (LDPC, frame interleaving,
  CW patching, waveform creation) that duplicated StreamingEncoder
- Config mismatch bugs between GUI and cli_simulator (pilot settings, CRC, CFO)
- Two divergent TX paths to maintain
- Control frames (ACK/NACK) encoded as 1-CW in GUI but 4-CW in cli_simulator

**What was changed:**
- `src/gui/modem/modem_engine.hpp`:
  - Added `StreamingEncoder` member, removed `encoder_` (fec::CodecPtr),
    `active_tx_waveform_`, `channel_interleaver_`, `ack_4cw_enabled_`,
    `interleaving_enabled_`, `interleaver_bits_per_symbol_`, `frame_interleaving_enabled_`
  - Removed `ensureTxWaveform()`, `updateChannelInterleaver()`, `setInterleavingEnabled()`
  - Added `postProcessTx()` helper
- `src/gui/modem/modem_engine.cpp`:
  - Constructor creates StreamingEncoder instead of encoder_/channel_interleaver_
  - `transmit()` reduced from ~280 lines to ~60 lines: waveform decision + StreamingEncoder delegation
  - `transmitPing()/transmitPong()` delegate to `streaming_encoder_->encodePing()`
  - `transmitTestPattern()/transmitRawOFDM()` use StreamingEncoder
  - Extracted `postProcessTx()` for lead-in, filter, scale, stats
  - Deleted `ensureTxWaveform()` and `updateChannelInterleaver()`
- `src/gui/modem/modem_mode.cpp`:
  - `setWaveformMode()`, `setConnected()`, `setDataMode()` now mirror config to StreamingEncoder
  - `setCodecType()` no longer recreates encoder_ (StreamingEncoder manages its own)
- `CMakeLists.txt`: Added streaming_encoder.cpp to ultra_gui, threaded_simulator, profile_acquisition

**Key behavioral change:**
- OFDM control frames (ACK/NACK) now get 4-CW frame interleaving via StreamingEncoder,
  matching cli_simulator behavior. Should reduce ACK loss on fading channels.

**Test verification:**
```
./build/cli_simulator --snr 20 --test              # AWGN: PASS, 0 retransmissions
./build/cli_simulator --snr 15 --fading good --rate r1_4 --test   # Good fading: PASS, 0 retransmissions
./build/cli_simulator --snr 15 --fading moderate --rate r1_4 --test  # Moderate: PASS, 2 retransmissions (expected)
```

---

## 2026-02-02: Fix Light Sync Timing Errors on Fading Channels (68%→93%)

**What was broken:**
- OFDM R1/4 on moderate fading had ~68% CW success rate instead of expected ~100%
- Frames with low light sync correlation (0.5-0.8) failed completely with random LLR
- All 4 CWs would fail with |llr|_avg ~2.5 (random) instead of ~5-7 (valid)

**Root cause:**
- Light sync (Schmidl-Cox on LTS) uses 0.5 correlation threshold
- On fading channels, multipath can cause timing errors in sync detection
- Low correlation (0.6-0.75) indicates sync found at wrong position
- Wrong timing → wrong channel estimate → complete frame corruption

**Files modified:**
- `src/gui/modem/streaming_decoder.cpp`:
  - Raised LIGHT_SYNC_CONFIDENCE from 0.5 to 0.8
  - Marginal syncs now fall back to full chirp with accurate timing
  - Added CFO drift limit (±1 Hz) when connected to reject multipath-induced false CFO

**How it works:**
- Light sync with corr < 0.8 triggers fallback to chirp sync
- Chirp sync has sub-sample timing accuracy from dual chirp gap measurement
- Full chirp takes ~1.2s longer but gives reliable timing on fading channels

**Test verification:**
```bash
./build/cli_simulator --snr 25 --fading moderate --test
# Before: 68% CW success (48/71)
# After: 93% CW success (130/140 over 3 tests, including 1 test at 100%)
```

---

## 2026-02-02: Fix Two-Pass DQPSK Not Triggering on Fading Channels

**What was broken:**
- Two-pass DQPSK decoding (phase error correction) never triggered on fading channels
- Log showed no "DQPSK two-pass" messages during moderate fading tests
- Moderate fading CW success was ~63% when it should be ~68% with two-pass

**Root cause:**
- `demodulateSymbol()` called `computeFadingIndex()` to decide if two-pass should trigger
- `computeFadingIndex()` computes coefficient of variation from `channel_estimate[]` array
- After sync, `channel_estimate` is reset to unity (all 1.0) at line 814 in demodulator.cpp
- Unity channel estimate has zero variance → `computeFadingIndex()` returns 0
- Two-pass threshold (0.12) was never exceeded because fading index was always 0

**Files modified:**
- `src/ofdm/demodulator.cpp`:
  - Changed from `float fading_index = computeFadingIndex();`
  - To: `float fading_index = last_fading_index;`
  - `last_fading_index` is measured from pilot variance (correct source)
  - Also changed LOG_DEMOD(DEBUG) to LOG_DEMOD(INFO) to see triggering in logs

**How it works:**
- `last_fading_index` is updated during pilot tracking from actual pilot magnitude variance
- This correctly reflects channel fading state (0.12-0.50 on fading channels)
- Two-pass now triggers when fading > 0.12, applying per-carrier phase correction

**Test verification:**
```bash
./build/cli_simulator --snr 25 --fading moderate --test 2>&1 | grep "DQPSK two-pass"
# Expected: Many lines showing "DQPSK two-pass: fading=0.xxx > 0.120, applying correction"
# ✓ TEST PASSED - two-pass triggers, moderate fading CW success improved to ~68%
```

---

## 2026-02-02: Fix CW[0] LDPC Decode Failures in OFDM

**What was broken:**
- OFDM_CHIRP at SNR 20 dB with R1/4 intermittently failed to decode CW[0]
- CW[0] hit 50 iterations (max) and failed while CW[1-3] decoded with 3-5 iterations
- LLR statistics showed low |llr|_avg (~1.0-1.2) instead of expected 3-4 for SNR 20

**Root cause:**
- In `updateChannelEstimate()`, the first symbol fallback path sets `noise_count=1`
- But the noise variance update condition was `if (noise_count > 1)`, which FAILED
- Result: `noise_variance` stayed at hardcoded 0.1f instead of estimated ~0.01
- This compressed LLRs by ~3x, causing borderline decodes that sometimes failed
- CW[0] was more affected because its data has mixed bit polarity (llr_avg≈0)

**Files modified:**
- `src/ofdm/channel_equalizer.cpp`:
  - Changed condition from `noise_count > 1` to `noise_count > 0`
  - Handle single-sample fallback case (noise_count==1) separately
- `src/protocol/frame_v2.cpp`:
  - Added CW decode logging with LLR statistics for debugging

**How it works:**
- First symbol: noise_count=1 (fallback), now updates noise_variance from estimated 15dB SNR
- Subsequent symbols: noise_count=6 (from 6 pilots), updates from temporal comparison
- Correct noise_variance → correct LLR scaling → reliable LDPC decode

**Test verification:**
```bash
./build/cli_simulator --snr 20 -w ofdm_chirp --rate r1_4 --test
# Expected: All frames decode with 4/4 CWs
# ✓ TEST PASSED - all 5 messages transferred, all CW[0] decode OK
```

---

## 2026-02-02: Fix BUG-006 - Re-enable Channel Interleaving

**What was broken:**
- Channel interleaving was completely non-functional - the `--channel-interleave` flag did nothing
- When enabled, CW1 specifically failed to decode while CW0, CW2, CW3 succeeded
- The bug report said interleaving "caused" failures, but actually it wasn't being applied at all

**Root cause:**
- In `encodeFixedFrame()` and `decodeFixedFrame()`, the `use_channel_interleave` parameter was cast to void:
  ```cpp
  (void)use_channel_interleave;  // Disabled due to BUG-006
  ```
- This completely disabled channel interleaving at the protocol level
- The StreamingEncoder/Decoder were properly configured but frame_v2.cpp ignored the setting

**Files modified:**
- `src/protocol/frame_v2.cpp`:
  - `encodeFixedFrame()`: Added ChannelInterleaver creation and interleave call after LDPC encode
  - `decodeFixedFrame()`: Added ChannelInterleaver creation and deinterleave call before LDPC decode
  - Both use consistent `BITS_PER_SYMBOL = 106` (53 data carriers × 2 bits for DQPSK)

**How it works:**
- Channel interleaving spreads consecutive bits across OFDM symbols for fading resistance
- Interleaver is created with (bits_per_symbol=106, total_bits=648) matching LDPC codeword size
- TX: After LDPC encode, interleave coded bits before frame interleaving
- RX: After frame deinterleaving, channel-deinterleave before LDPC decode
- The order is: LDPC encode → channel interleave → frame interleave (TX); reverse for RX

**Test verification:**
```bash
# Clean AWGN with channel interleaving
./build/cli_simulator --snr 20 -w ofdm_chirp --rate r1_4 --channel-interleave --test
# Expected: Shows "Channel interleaving: ENABLED" and all frames decode
# ✓ TEST PASSED - all 5 messages transferred
```

---

## 2026-01-31: Fix MC-DPSK AUTO Rate Bug

**What was broken:**
- When forcing `--waveform mc_dpsk` without `--rate`, the system selected R1/2 instead of R1/4
- The algorithm in `waveform_selection.hpp` specifies MC-DPSK should ALWAYS use R1/4
- Log showed: `Connection: Initial data mode DQPSK R1/2 (SNR=10.0 dB, forced_mod=255, forced_rate=255)`

**Root cause:**
- `recommendDataModeWithFading()` auto-selected a waveform based on SNR/fading, ignoring the negotiated waveform
- At SNR=10/AWGN, it auto-selected OFDM_CHIRP, then passed that to `recommendDataMode()`
- Since OFDM (not MC-DPSK) was passed, the OFDM rate logic ran → R1/2 at SNR=10

**Files modified:**
- `src/protocol/connection_handlers.cpp`:
  - Renamed `recommendDataModeWithFading()` to `recommendDataModeForWaveform()`
  - Changed function to take waveform as INPUT instead of auto-selecting it
  - Call site now passes `negotiated_mode_` (the forced/negotiated waveform) instead of ignoring it

**How it works:**
- Waveform negotiation happens FIRST via `negotiateMode()` (respects forced waveform)
- If AUTO, select waveform based on SNR/fading
- Then call `recommendDataModeForWaveform()` with the negotiated waveform
- MC-DPSK now correctly triggers the R1/4 path in `recommendDataMode()`

**Test verification:**
```bash
./build/cli_simulator --snr 10 --test --waveform mc_dpsk 2>&1 | grep "Initial data mode"
# Expected: DQPSK R1/4
# ✓ Connection: Initial data mode DQPSK R1/4 (SNR=10.0 dB, forced_mod=255, forced_rate=255)

./build/cli_simulator --snr 8 --test 2>&1 | grep "Initial data mode"
# Expected: AUTO selects MC-DPSK R1/4 at low SNR
# ✓ Connection: Initial data mode DQPSK R1/4 (SNR=8.0 dB)
```

---

## 2026-01-28: Fix Disconnect ACK Code Rate (GUI Simulator)

**What was broken:**
- GUI simulator: After receiving DISCONNECT, the ACK was sent with R1/4 instead of R2/3
- Initiator couldn't decode ACK → timeout after 30 seconds
- Sequence: ACK queued → setConnected(false) called → ACK transmitted with wrong rate

**Root cause:**
- V2 Frame Path at modem_engine.cpp:283 checked `connected_ && handshake_complete_`
- When `setConnected(false)` was called, `connected_` became false
- The queued ACK was then transmitted with R1/4 instead of negotiated rate

**Files modified:**
- `src/gui/modem/modem_engine.cpp`: Added `use_connected_waveform_once_` to code rate check
  ```cpp
  // Before:
  tx_code_rate = (connected_ && handshake_complete_) ? data_code_rate_ : CodeRate::R1_4;
  // After:
  tx_code_rate = ((connected_ && handshake_complete_) || use_connected_waveform_once_) ? data_code_rate_ : CodeRate::R1_4;
  ```

**How it works:**
- `use_connected_waveform_once_` is set true when `setConnected(false)` is called
- This flag preserves the negotiated code rate for the disconnect ACK
- Flag is cleared after the ACK is transmitted

**Test verification:**
```bash
./build/cli_simulator --snr 20 --test
# Expected: DISCONNECT phase completes without timeout
# ✓ Disconnected!
```

---

## 2026-01-28: Fix total_cw Mismatch for Negotiated Code Rate Frames

**What was broken:**
- DISCONNECT frame (type=0x15) showed "PARTIAL (1/3 codewords)" on receiver
- Header had `total_cw=3` (calculated assuming R1/4) but encoded with R2/3 (1 codeword)
- `ConnectFrame::serialize()` calculates total_cw using R1/4 (default), but TX uses negotiated rate

**Root cause:**
- Frame serialization happens before code rate is determined
- `total_cw` in header is calculated at serialize time, not encode time
- Disconnect frame: 44 bytes payload → 3 codewords at R1/4, but 1 codeword at R2/3

**Files modified:**
- `src/gui/modem/modem_engine.cpp`: Added total_cw patching before LDPC encoding
  - Only patches data/connect frames (types 0x10-0x19 and 0x30-0x3F)
  - Control frames (ACK 0x20, NACK 0x21, etc.) are fixed 20 bytes = 1 codeword, no patching
  - Recalculates header CRC after patching

**How it works:**
1. Check if frame is data or connect type (needs total_cw field)
2. Read payload_len from header bytes 13-14
3. Calculate correct total_cw for actual TX code rate
4. Patch byte 12 if different
5. Recalculate header CRC (bytes 15-16)
6. Encode patched frame with LDPC

**Test verification:**
```bash
./build/cli_simulator --snr 20 --test
# Expected: DISCONNECT phase completes
# ✓ Disconnected!
# DISCONNECT frame shows total_cw=1 (not 3)
```

---

## 2026-01-28: Fix OFDM_COX Minimum Samples for Short Frames

**What was broken:**
- After receiving DATA, StreamingDecoder couldn't find ACK or subsequent frames
- OFDM_COX min_samples was set to 48000 but short frames (ACK = ~18000 samples) are smaller
- Available samples (19452) < min_samples (48000) caused decoder to skip

**Files modified:**
- `src/gui/modem/streaming_decoder.cpp`:
  - Changed OFDM_COX min_samples from `max(48000, getMinSamplesForFrame() * 2)` (was wrong)
  - To `max(15000, getMinSamplesForFrame() * 2)` (~14000 samples sufficient)

**Test verification:**
```bash
./build/cli_simulator --snr 20 --test
# Expected: All 3 messages received correctly
# ✓ Message 1 received correctly!
# ✓ Message 2 received correctly!
# ✓ Message 3 received correctly!
```

---

## 2026-01-28: Fix Control Frame Code Rate When Connected

**What was broken:**
- After connection, control frames (ACK, NACK, DISCONNECT) were sent with R1/4
- But receiver expected negotiated rate (e.g., R2/3)
- Caused ACK decode failures after DATA received correctly

**Root cause:**
- `modem_engine.cpp` line 283: `tx_code_rate = (is_data_frame && connected_) ? data_code_rate_ : CodeRate::R1_4;`
- This only used negotiated rate for DATA frames, not control frames

**Files modified:**
- `src/gui/modem/modem_engine.cpp`:
  - Changed rate selection: `tx_code_rate = (connected_ && handshake_complete_) ? data_code_rate_ : CodeRate::R1_4;`
  - Now ALL frames (data AND control) use negotiated rate after handshake

**How it works:**
1. Pre-connection (PING/PONG/CONNECT): Use R1/4 for robustness
2. During handshake (CONNECT_ACK): Still use R1/4 (remote not confirmed yet)
3. Post-handshake: ALL frames use negotiated rate for proper decoding

**Test verification:**
```bash
./build/cli_simulator --snr 20 --test
# Expected: All 3 messages received + ACKs decoded correctly
```

---

## 2026-01-28: Fix PING Detection in cli_simulator (Connection Phase)

**What was broken:**
- PING frames (chirp-only) were not being detected by StreamingDecoder
- Two root causes:
  1. Receiver needed MIN_SAMPLES_FOR_SEARCH (144000) but PING/PONG was only 57600 samples
  2. PING detection logic checked `codewords_ok == 0` but LDPC "succeeded" on garbage (codewords_ok=1)

**Files modified:**
- `src/gui/modem/modem_engine.cpp`: Added 100000 samples trailing silence to PING/PONG so receiver buffer fills
- `src/gui/modem/streaming_decoder.cpp`: Fixed PING detection logic
  - Changed check from `!result.success && result.codewords_ok == 0 && result.frame_data.empty()`
  - To `!result.success && result.frame_data.empty()` (catches LDPC "success" on garbage)
  - Added handlePingDetection() lambda for cleaner PING handling

**How it works:**
1. PING = chirp only (no training/data after)
2. After chirp detection, try to decode data
3. If no valid "UL" magic header found → it's a PING (regardless of LDPC success on noise)
4. Trailing silence ensures receiver has enough samples for chirp detection

**Test verification:**
```bash
./build/cli_simulator --snr 20
# Expected: Phase 1 CONNECTION shows "✓ Connected!"
# PING→PONG→CONNECT→CONNECT_ACK flow works
```

**Known limitation:** DATA phase still failing (separate issue with OFDM codeword handling)

---

## 2026-01-28: Add Fading Detection for Mode Negotiation

**What was changed:**
- Added per-carrier magnitude variance tracking to detect frequency-selective fading
- Mode negotiation now considers both SNR and fading index

**Files modified:**
- `src/psk/multi_carrier_dpsk.hpp`: Added `carrier_magnitudes_`, `getFadingIndex()`, `isFading()`
- `src/waveform/waveform_interface.hpp`: Added virtual `getFadingIndex()`, `isFading()`
- `src/waveform/mc_dpsk_waveform.hpp/cpp`: Override fading methods
- `src/gui/modem/streaming_decoder.hpp/cpp`: Added `last_fading_index_`, `getLastFadingIndex()`
- `src/gui/modem/modem_engine.hpp/cpp`: Added `getFadingIndex()`, `isFading()`
- `src/protocol/connection.hpp/cpp`: Added `fading_index_`, `setChannelQuality()`
- `src/protocol/connection_handlers.cpp`: Updated `negotiateMode()` with fading-aware logic
- `tools/cli_simulator.cpp`: Pass channel quality (SNR + fading) to protocol

**Mode selection logic:**
- SNR < 0 dB: MFSK (not implemented yet)
- SNR 0-10 dB: MC_DPSK
- SNR 10-17 dB: OFDM_CHIRP if fading, MC_DPSK if stable
- SNR > 17 dB: OFDM_COX if stable, OFDM_CHIRP if fading

**Fading index calculation:**
Coefficient of variation (std_dev / mean) of per-carrier magnitudes. Values > 0.4 indicate significant fading.

---

## 2026-01-28: Delete RxPipeline (Cleanup)

**What was changed:**
Removed the deprecated RxPipeline class. StreamingDecoder now handles all RX processing.

**Files removed:**
- `src/gui/modem/rx_pipeline.hpp` - DELETED
- `src/gui/modem/rx_pipeline.cpp` - DELETED

**Files modified:**
- `modem_engine.hpp`: Removed `rx_pipeline_` member and include
- `modem_engine.cpp`: Removed `rx_pipeline_` reset block
- `modem_mode.cpp`: Removed `rx_pipeline_` mode handling
- `fec/codec_interface.hpp`: Removed outdated comment
- `CMakeLists.txt`: Removed rx_pipeline.cpp from all 9 build targets

**Benefits:**
- Removed ~400 lines of deprecated code
- Cleaner codebase with single RX path (StreamingDecoder)
- Reduced binary size

**Test verification:**
```bash
./tests/regression_matrix.sh
# Expected: ALL TESTS PASSED! (11/11)
```

---

## 2026-01-28: TX Path Unification (Phase 4)

**What was changed:**
The TX path in `transmit()` now uses IWaveform abstraction instead of direct modulator calls.

**Before:** 4 separate if-else branches with direct modulator calls:
- MC-DPSK: `mc_dpsk_modulator_->modulate()` + `chirp_sync_->generate()`
- OFDM_CHIRP: `OFDMModulator chirp_modulator` + `chirp_sync_->generate()`
- OFDM_COX: `ofdm_modulator_->generatePreamble()` + `ofdm_modulator_->modulate()`
- OTFS: `otfs_modulator_->generatePreamble()` + `otfs_modulator_->modulate()`

**After:** Single IWaveform path for MC_DPSK, OFDM_CHIRP, OFDM_COX:
```cpp
ensureTxWaveform(active_waveform, tx_modulation, tx_code_rate);
preamble = active_tx_waveform_->generatePreamble();
modulated = active_tx_waveform_->modulate(to_modulate);
```

**OTFS:** Kept legacy path (no OTFSWaveform yet)

**Benefits:**
- Adding new waveform only requires implementing IWaveform (no TX code changes)
- Reduced code duplication (~50 lines removed)
- Consistent TX interface across all waveforms

**Test verification:**
```bash
./tests/regression_matrix.sh
# Expected: ALL TESTS PASSED! (11/11)
```

---

## 2026-01-28: Remove Legacy Acquisition Thread

**What was changed:**
The acquisition thread was running but its output (`detected_frame_queue_`) was never consumed.
StreamingDecoder now handles all RX processing, making the acquisition thread dead code.

**Files removed/modified:**
- `modem_engine.hpp`: Removed acquisition thread members, legacy RX buffer, processRxBuffer_* declarations
- `modem_rx.cpp`: Removed acquisitionLoop(), startAcquisitionThread(), stopAcquisitionThread(), buffer helpers
- `modem_rx_decode.cpp`: Removed ~1200 lines of legacy decode code (rxDecodeDPSK, processRxBuffer_*, etc.)
- `modem_engine.cpp`: Removed acquisition thread start/stop calls
- `modem_mode.cpp`: Replaced legacy buffer clears with `streaming_decoder_->reset()`

**Removed components:**
- `acquisition_thread_`, `acquisition_running_`, `acquisition_cv_`, `acquisition_mutex_`
- `rx_sample_buffer_`, `samples_consumed_`, `rx_buffer_mutex_`
- `detected_frame_queue_`, `rx_frame_state_`
- `rxDecodeDPSK()`, `processRxBuffer_OFDM/OTFS/DPSK/OFDM_CHIRP()`
- `waitForSamples()`, `deinterleaveCodewords()`, `detectPing()`
- Legacy accumulation state (ofdm_accumulated_soft_bits_, dpsk_accumulated_soft_bits_, etc.)

**Architecture after cleanup:**
- RX decode thread runs `rxDecodeLoop()` which drives `streaming_decoder_->processBuffer()`
- `feedAudio()` only feeds to StreamingDecoder
- Frame delivery via callbacks set in ModemEngine constructor
- Mode switches call `streaming_decoder_->reset()` instead of clearing legacy buffers

**Test verification:**
```bash
./tests/regression_matrix.sh
# Expected: ALL TESTS PASSED! (11/11)
```

---

## 2026-01-28: StreamingDecoder Becomes Primary Decoder

**What was broken:**
- StreamingDecoder frame decoding worked (3/3 codewords) but ConnectFrame::deserialize() failed
- CW0 decoded to 21 bytes instead of expected 20 bytes
- Frame reassembly used 21 bytes from CW0, causing 1-byte shift and CRC failure

**Root cause:**
LDPC R1/4 has 162 info bits = 20.25 bytes. Decoder returns `ceil(162/8) = 21` bytes,
but protocol `getBytesPerCodeword(R1_4)` returns `162/8 = 20` bytes (integer division).
The extra byte at position 20 is padding from fractional bits.

**What was changed:**
- `streaming_decoder.cpp`: Added CW0 resize to `bytes_per_cw` (20 bytes) after LDPC decode
- `modem_engine.hpp`: Fixed `setMCDPSKCarriers()` to recreate TX modulator and update StreamingDecoder
- `streaming_decoder.hpp/cpp`: Added `setMCDPSKCarriers()` method for carrier count sync

**How it's properly fixed:**
After LDPC decode, resize CW0 to exactly 20 bytes (discard padding):
```cpp
if (cw0_data.size() > bytes_per_cw) {
    cw0_data.resize(bytes_per_cw);  // Truncate to 20 bytes
}
```

**CFO handling verified:**
- Python analysis confirmed carrier frequencies shift by exactly the expected CFO amount
- CFO=30Hz: All 8 carriers shifted by 29.3-30.8 Hz (mean=30.0 Hz)
- CFO=0Hz: No shift (all 0.0 Hz)

**Test verification:**
```bash
./test_iwaveform --snr 10 -w mc_dpsk --frames 3 --cfo 30
# Expected: Decoded: 3/3 (100%)

./tests/regression_matrix.sh
# Expected: ALL TESTS PASSED! (11/11)
```

---

## 2026-01-28: StreamingDecoder Created (Fixes BUG-002: RxPipeline Broken)

**What was broken:**
- RxPipeline failed to detect chirps when integrated into ModemEngine
- test_iwaveform worked 100% using IWaveform directly
- RxPipeline integration in ModemEngine failed

**Root cause analysis:**
RxPipeline had incorrect IWaveform call sequence:
1. Line 147: `waveform_->setFrequencyOffset(sync_result.cfo_hz);` - CFO applied
2. Line 172: `waveform_->reset();` - CFO CLEARED (violates INV-WAVE-002!)
3. Line 173: `waveform_->process(process_span);` - Process with wrong CFO

Per INV-WAVE-002: "reset() MUST clear cfo_hz_ to prevent stale values"
This means calling reset() AFTER setFrequencyOffset() erases the CFO.

**What was changed:**
- Created `src/gui/modem/streaming_decoder.hpp` (~230 lines)
- Created `src/gui/modem/streaming_decoder.cpp` (~460 lines)
- Correct call sequence: reset() → detectSync() → setFrequencyOffset() → process()
- Circular buffer with bounded size (4 seconds max)
- Sliding window search (like test_iwaveform)
- Thread-safe with condition variable for blocking wait
- PING detection via energy ratio
- SNR estimation from chirp correlation
- Added to CMakeLists.txt for all executables

**How it's properly fixed:**
StreamingDecoder uses the correct IWaveform call sequence per INV-WAVE-001:
```cpp
waveform->reset();                           // Clear state
waveform->detectSync(samples, sync_result);  // Find preamble
waveform->setFrequencyOffset(sync_result.cfo_hz);  // Store CFO
waveform->process(samples_from_start);       // Demodulate
auto bits = waveform->getSoftBits();         // Get output
```

**Test verification:**
```bash
# Build with StreamingDecoder
make -j4 test_iwaveform  # Should compile without errors

# Regression tests pass
./tests/regression_matrix.sh
# Expected: ALL TESTS PASSED!
```

**Next steps:**
1. ~~Integrate StreamingDecoder into ModemEngine~~ DONE 2026-01-28
2. Make StreamingDecoder the primary decoder (currently parallel)
3. Remove acquisition thread
4. Replace processRxBuffer_* methods
5. Delete RxPipeline after integration verified

---

## 2026-01-28: StreamingDecoder Integration (Phase 2)

**What was changed:**
- `src/gui/modem/modem_engine.hpp`: Added `streaming_decoder_` member
- `src/gui/modem/modem_engine.cpp`: Initialize StreamingDecoder in constructor, set callbacks
- `src/gui/modem/modem_rx.cpp`:
  - feedAudio(): Feeds to StreamingDecoder in parallel with legacy path
  - rxDecodeLoop(): Checks StreamingDecoder for decoded frames

**Integration approach:**
Running in parallel mode for safety:
- Audio is fed to BOTH StreamingDecoder AND legacy path
- Legacy path (acquisition thread) still does primary decoding
- StreamingDecoder is receiving audio and processing but not yet primary

**Test verification:**
```bash
# All regression tests pass
./tests/regression_matrix.sh
# Expected: 11/11 PASS
```

**Status:** Parallel mode working. Next: Make StreamingDecoder primary.

---

## 2026-01-28: PING vs DPSK Frame Detection Fix (cli_simulator)

**What was broken:**
- cli_simulator connection phase failed - PING frames misdetected as "Chirp+DPSK frames"
- Energy threshold (0.05f) was absolute, failed at high SNR where noise exceeded threshold
- Overlapping chirps in buffer caused detection confusion

**Root cause analysis:**
1. Energy threshold was absolute (0.05f), not relative to signal level
2. At 20dB SNR, noise RMS (~0.057) exceeded the threshold
3. Multiple PINGs could pile up in buffer before processing
4. Energy ratio between chirp and post-chirp didn't account for fading or overlapping chirps

**What was changed:**
- `src/gui/modem/modem_rx.cpp`:
  - Changed PING/DPSK detection from absolute threshold to energy ratio (post_rms/chirp_rms)
  - Ratio < 0.3 = PING (post-chirp is noise)
  - Ratio 0.3-1.4 = DPSK data (similar energy levels)
  - Ratio > 1.4 = Another chirp starting (different transmission)
  - Added chirp detection in suspicious range (1.1-1.4): search for chirp in post-chirp region
  - Added 200ms guard period after consuming PING samples
- `src/gui/modem/modem_rx_constants.hpp`:
  - Reduced MIN_SAMPLES_FOR_ACQUISITION from 90000 to 65000 (PING is only 57600 samples)

**How it's properly fixed:**
- Energy ratio is SNR-independent (compares signal to signal, not signal to absolute)
- Fading channels can have ratio up to 1.3 due to energy variation - 1.4 threshold accommodates this
- When ratio is suspicious (1.1-1.4), quick chirp search in post region distinguishes overlapping chirps
- Guard period prevents partial chirp detection from overlapping transmissions

**Test verification:**
```bash
# CLI simulator should connect (PING/PONG, CONNECT, CONNECT_ACK)
./build/cli_simulator --snr 20 --test
# Expected: Connection phase succeeds, "✓ Connected!" displayed

# Regression tests all pass
./tests/regression_matrix.sh --quick
# Expected: 11/11 PASS, including MC-DPSK on fading channels
```

---

## 2026-01-28: MC-DPSK CFO Per-Segment Initial Phase Fix

**What was broken:**
- MC-DPSK degraded massively with CFO on fading channels
- Poor fading + CFO=30: 20% success (should be ~80%)
- Moderate fading + CFO=30: 40% success (should be ~80%)
- CFO=0 worked fine (80-100%), proving the issue was CFO handling

**Root cause analysis:**
- Each segment (training, ref, data) starts at a different sample position
- Each segment needs its OWN initial phase for CFO correction
- Bug: We set initial phase once for training_start, then used it for ALL segments
- Result: ref and data segments got wrong CFO correction, causing phase errors

**What was changed:**
- `src/gui/modem/modem_rx_decode.cpp` (3 locations in rxDecodeDPSK):
  - Added `calcInitialPhase` lambda to compute wrapped phase for any absolute position
  - Calculate separate initial phases: training_start_abs, ref_start_abs, data_start_abs
  - Call `setCFOWithPhase()` before each `applyCFO()` with the correct phase for that segment
  - Set final phase for data segment after processing training/ref

**How it's properly fixed:**
- Training at position T gets phase: -2π × CFO × T / sr
- Ref at position T+training_len gets phase: -2π × CFO × (T+training_len) / sr
- Data at position T+training_len+ref_len gets its own phase
- Each segment's CFO correction now starts at the correct accumulated phase
- Signal and correction cancel exactly for each segment independently

**Test verification:**
```bash
# MC-DPSK on poor fading with CFO
./build/test_iwaveform --snr 15 -w mc_dpsk --channel poor --cfo 30 --frames 5
# Expected: 80% (was 20% before fix)

# MC-DPSK on moderate fading with CFO
./build/test_iwaveform --snr 15 -w mc_dpsk --channel moderate --cfo 30 --frames 5
# Expected: 80% (was 40% before fix)
```

**Results after fix:**
| Channel | CFO=0 | CFO=30 |
|---------|-------|--------|
| Poor | 80% | 80% |
| Moderate | 80% | 80% |

---

## 2026-01-28: OFDM_CHIRP CFO Initial Phase in modem_rx_decode.cpp

**What was broken:**
- OFDM_CHIRP in modem_rx_decode.cpp used `setFrequencyOffset()` which resets phase to 0
- The IWaveform path (`ofdm_chirp_waveform.cpp`) already used `setFrequencyOffsetWithPhase()`
- modem_rx_decode.cpp path wasn't updated, causing CFO failures

**What was changed:**
- `src/gui/modem/modem_rx_decode.cpp` in `processRxBuffer_OFDM_CHIRP()`:
  - Track `buffer_start_abs` when taking samples from buffer
  - Calculate `training_start_abs = buffer_start_abs + chirp_end_offset`
  - Compute initial phase: -2π × CFO × training_start_abs / sr
  - Call `setFrequencyOffsetWithPhase(cfo_hz, initial_phase)` instead of `setFrequencyOffset(cfo_hz)`

**Test verification:**
```bash
./build/test_iwaveform --snr 15 -w ofdm_chirp --channel awgn --cfo 30 --rate r1_4 --frames 5
# Expected: 100%
```

---

## 2026-01-28: R1/4 Code Rate Required for Fading Channels

**What was broken:**
- OFDM_CHIRP with R1/2 (default): 0% on moderate fading
- R1/2 doesn't have enough redundancy for fading channels
- This was misdiagnosed as CFO or channel equalization issues

**What was changed:**
- No code changes - this is a configuration/usage discovery
- Added `--rate` flag to test_iwaveform.cpp for testing different rates

**How it's properly fixed:**
- Use R1/4 for fading channels (4x redundancy)
- R1/2 is only suitable for AWGN or very good channels
- MC-DPSK already uses R1/4 by default (protocol-defined)

**Test verification:**
```bash
# R1/2 on moderate fading - FAILS
./build/test_iwaveform --snr 15 -w ofdm_chirp --channel moderate --rate r1_2 --frames 5
# Expected: 0-20%

# R1/4 on moderate fading - WORKS
./build/test_iwaveform --snr 15 -w ofdm_chirp --channel moderate --rate r1_4 --frames 5
# Expected: 100%
```

**Performance comparison at 15dB:**
| Waveform | AWGN | Moderate (R1/2) | Moderate (R1/4) |
|----------|------|-----------------|-----------------|
| OFDM_CHIRP | 100% | 0% | 100% |
| MC-DPSK | 100% | 80% | 80% |

---

## 2026-01-27: Improved Channel Interleaver Symbol Separation

**What was broken:**
- OFDM_CHIRP fading performance was lower than expected (~60% on good HF)
- Interleaver only separated consecutive bits by 1 symbol (step=61, separation=1)
- Burst errors from fading affected adjacent bits, making LDPC correction harder

**What was changed:**
- `src/fec/ldpc_decoder.cpp`: Modified `findCoprimeStep()` to target step = 3 × bits_per_symbol
- For 60 bits/symbol: step changed from 61 to 181, separation from 1 to 3

**How it's properly fixed:**
- Consecutive input bits now land in OFDM symbols 3 apart instead of adjacent
- When fading causes a burst error in one symbol, the affected bits are spread
  across the codeword after deinterleaving
- LDPC can correct scattered errors better than clustered errors

**Test verification:**
```bash
# Good HF channel at 20 dB
for seed in 1 2 3 4 5; do
  ./build/test_iwaveform --snr 20 -w ofdm_chirp --channel good --frames 5 --seed $seed
done
# Expected: 80-100% (was 60-100%)
```

---

## 2026-01-27: OFDM_CHIRP CFO Initial Phase Fix

**What was broken:**
- OFDM_CHIRP failed at any CFO > 0 Hz (CFO=30 Hz: 0% success)
- CFO=0 worked perfectly (100%)
- MC-DPSK at CFO=30 Hz worked (100%), proving chirp detection was correct
- Root cause: CFO correction started from phase 0 instead of accumulated phase

**Root cause analysis:**
1. Test harness applies CFO to entire audio from sample 0
2. By training start (sample ~136,800), CFO has accumulated ~307° of phase
3. `processPresynced()` reset `freq_correction_phase = 0`, losing this accumulated phase
4. First training symbol got wrong CFO correction, corrupting H estimate
5. DQPSK differential decoding failed due to phase mismatch

**What was changed:**
1. `include/ultra/ofdm.hpp` + `src/ofdm/demodulator.cpp`:
   - Added `setFrequencyOffsetWithPhase(float cfo_hz, float initial_phase_rad)`
   - Sets both CFO and initial correction phase

2. `src/waveform/ofdm_chirp_waveform.hpp` + `.cpp`:
   - Added `training_start_sample_` member variable
   - `detectSync()`: Stores training start position
   - `process()`: Calculates initial phase = -2π × CFO × training_start / sample_rate
   - Calls `setFrequencyOffsetWithPhase()` instead of `setFrequencyOffset()`

3. `src/ofdm/demodulator.cpp`:
   - `processPresynced()`: Removed reset of `freq_correction_phase` to preserve initial phase

4. `src/ofdm/channel_equalizer.cpp`:
   - Simplified `lts_carrier_phases` to use (1,0) reference
   - With correct initial phase, no phase compensation needed
   - Previous `conj(h_unit) * phase_advance` was wrong with correct initial phase

**How it's properly fixed:**
- Initial CFO phase = -2π × CFO × training_start_sample / sample_rate
- This matches the accumulated CFO phase in the signal at training start
- CFO correction is now continuous from sample 0 (effectively)
- Signal's +φ and correction's -φ cancel exactly: corrected = TX
- DQPSK reference = (1,0) because equalized = TX (no extra phase)

**Test verification:**
```bash
# Test full CFO range
for cfo in -50 -30 0 30 50; do
  ./build/test_iwaveform -w ofdm_chirp --snr 17 --cfo $cfo --frames 3
done
# Expected: 100% success for all CFO values
```

**Results:** OFDM_CHIRP now works with ±50 Hz CFO at 10-20 dB SNR.

---

## 2026-01-27: OFDM_CHIRP CFO Test Harness Fix

**What was broken:**
- OFDM_CHIRP decoding failed for most CFO values (only CFO=0 reliable)
- CFO=10 Hz: 0% success, CFO=30 Hz: 20% success
- Root cause: FIR Hilbert transform (127-tap) in test_iwaveform had 63-sample group delay
- This caused CFO-dependent timing shifts that broke OFDM symbol alignment

**What was changed:**
- `tools/test_iwaveform.cpp`: Replaced FIR Hilbert with FFT-based Hilbert (no group delay)
  - FFT signal -> zero negative frequencies, double positive -> IFFT
  - This creates perfect analytic signal without timing artifacts
- `src/sync/chirp_sync.hpp`: Removed HILBERT_GROUP_DELAY (63 sample) correction
  - Was compensating for old FIR delay which no longer exists

**How it's properly fixed:**
- FFT-based Hilbert has zero group delay (unlike FIR which has N/2 delay)
- CFO simulation now shifts frequency without shifting timing
- Chirp position correction only accounts for CFO-induced peak shift, not filter delay

**Test verification:**
```bash
# Test CFO range -45 to +50 Hz
for cfo in -45 -30 0 30 50; do
  ./test_iwaveform -w ofdm_chirp --snr 15 --cfo $cfo --frames 1
done
# Expected: 100% success for all CFO values
```

**Note:** This was a TEST HARNESS bug, not a demodulator bug. Real radios don't have this issue.

---

## 2026-01-27: CFO Accumulation Bug Fix

**What was broken:**
- MC-DPSK failed on subsequent frames when CFO ~0 Hz
- Frame 1 decoded, Frames 2+ failed LDPC
- Residual CFO from training accumulated via `cfo_hz_ += residual_cfo`

**What was changed:**
- `src/gui/modem/modem_rx_decode.cpp`: Always call `setCFO(frame.cfo_hz)` to reset accumulated CFO
- Previously only called when `abs(cfo_hz) > 0.1f`
- Fixed in 3 places: PING decode, CW0 decode, full frame decode

**How it's properly fixed:**
- `setCFO()` resets `cfo_hz_` to the chirp-detected value
- This prevents residual CFO from training from accumulating across frames
- Chirp CFO is then re-estimated for each frame independently

**Test verification:**
```bash
./test_iwaveform --snr 5 --cfo 0 --channel awgn -w mc_dpsk --frames 5
# Expected: 100% decode rate (was 20% before fix)
```

**Commit:** `a2e6bed Fix CFO accumulation bug and improve test_iwaveform continuous RX`

---

## 2026-01-27: Demodulator Reset Per Frame

**What was broken:**
- Continuous RX decode degraded on subsequent frames at marginal SNR
- Demodulator state from previous frame affected current decode

**What was changed:**
- `src/gui/modem/modem_rx_decode.cpp`: Added `mc_dpsk_demodulator_->reset()` at start of `rxDecodeDPSK()`

**How it's properly fixed:**
- Reset clears carrier phases, previous symbols, and other state
- CFO is then set from chirp detection via `setCFO()`
- Each frame gets clean demodulator state

**Test verification:**
```bash
./test_iwaveform --snr 5 --cfo 30 --channel awgn -w mc_dpsk --frames 5
# Expected: 100% decode rate
```

**Commit:** `e52705b Add demodulator reset at start of each DPSK frame decode`

---

## 2026-01-27: test_iwaveform Continuous RX Mode

**What was broken:**
- test_iwaveform created fresh RX ModemEngine per frame ("cheating")
- Didn't test realistic continuous audio streaming
- Buffer overflow when feeding too much audio at once

**What was changed:**
- `tools/test_iwaveform.cpp`: Use single RX ModemEngine for entire audio stream
- Add throttling pauses every 5 seconds to let acquisition process
- Reduce gap between frames (1.5s) to fit under MAX_PENDING_SAMPLES (960000)
- Track decoded frames by sequence number using std::set

**How it's properly fixed:**
- Realistic test: audio streamed continuously like from HF rig
- Throttling prevents buffer overflow (acquisition can't keep up with instant feed)
- Single RX instance tests state management between frames

**Test verification:**
```bash
./test_iwaveform --snr 5 --cfo 30 --channel awgn -w mc_dpsk --frames 5
# Expected: 100% decode rate
```

**Commit:** `a2e6bed Fix CFO accumulation bug and improve test_iwaveform continuous RX`

---

## 2026-01-27: IWaveform Interface Documentation

**What was done:**
- Created comprehensive documentation for refactoring reference

**Files created:**
- `docs/MODEM_ENGINE_ARCHITECTURE.md` - Complete ModemEngine analysis
- `docs/DUAL_CHIRP_CFO_ANALYSIS.md` - CFO detection and position handling
- `docs/TESTING_METHODOLOGY.md` - Test tools and requirements

**Why it matters:**
- ModemEngine has two parallel code paths (old direct modulators, new IWaveform)
- RxPipeline integration has bugs - old `processRxBuffer_*` methods still work
- CFO must be applied via Hilbert transform, not simple multiplication

---

## 2026-01-27: OFDM_CHIRP Support in test_iwaveform

**What was broken:**
- test_iwaveform.cpp could not decode OFDM_CHIRP frames
- ModemEngine's acquisition thread routes ALL chirp frames to MC-DPSK decoder
- OFDMChirpWaveform::process() only returned 648 soft bits instead of all

**What was changed:**
- `tools/test_iwaveform.cpp`: Added `decodeOFDMChirpFrame()` that uses IWaveform directly
- `tools/test_iwaveform.cpp`: Added `setConnectWaveform()` call for TX (connect_waveform_ is used for disconnected mode TX, not waveform_mode_)
- `src/waveform/ofdm_chirp_waveform.cpp`: Fixed `process()` to loop and retrieve ALL soft bits from demodulator

**How it's properly fixed:**
- OFDM_CHIRP decode bypasses ModemEngine and uses IWaveform directly
- TX uses `setConnectWaveform(mode)` in addition to `setWaveformMode(mode)`
- `process()` now calls `demodulator_->getSoftBits()` in a loop until `hasPendingData()` returns false

**Test verification:**
```bash
./test_iwaveform --snr 17 --cfo 30 --channel awgn -w ofdm_chirp --frames 10
# Expected: 100% decode rate
```

**Commit:** `84bb563 Add OFDM_CHIRP support to test_iwaveform with CFO correction`

---

## 2026-01-27: MC-DPSK CFO Correction for Training/Reference Samples

**What was broken:**
- MC-DPSK decode failed with CFO on fading channels
- Training and reference samples were receiving UNCORRECTED signal
- `processTraining()` was estimating wrong residual CFO

**What was changed:**
- `src/psk/multi_carrier_dpsk.hpp`: CFO correction applied to training/ref samples BEFORE `processTraining()`
- Added public `applyCFO()` wrapper method that preserves `cfo_hz_` after correction

**How it's properly fixed:**
- CFO correction must happen BEFORE `processTraining()`, not after
- The demodulator's `applyCFOCorrection()` resets `cfo_hz_` to 0, so we save/restore it
- Chirp CFO is trusted over training CFO (more accurate from 1+ second signal)

**Invariants:**
1. CFO from chirp detection is the most accurate - trust it
2. Apply CFO to ALL samples (training, ref, data) before demodulation
3. Don't let `processTraining()` overwrite chirp CFO estimate

**Test verification:**
```bash
./test_iwaveform --snr 10 --cfo 30 --channel moderate -w mc_dpsk --frames 10
# Expected: 100% decode rate
```

**Commit:** `48e6271 Fix MC-DPSK CFO correction for training/reference samples`

---

## 2026-01-26: Complex Correlation for CFO-Tolerant Chirp Detection

**What was broken:**
- Real-valued chirp correlation oscillated at CFO beat frequency
- Detection position varied with CFO (±24-48 samples error)
- CFO estimation was inaccurate (~11.7 Hz for 20 Hz actual)

**What was changed:**
- `src/sync/chirp_sync.hpp`: Added cosine templates alongside sine templates
- `src/sync/chirp_sync.hpp`: New `computeComplexTemplateCorrelation()` returns magnitude √(I² + Q²)

**How it's properly fixed:**
- Complex correlation: I = Σ signal × cos(phase), Q = Σ signal × sin(phase)
- Magnitude √(I² + Q²) is CFO-invariant (phase rotation doesn't change magnitude)
- Peak position is now consistent regardless of CFO

**Invariants:**
1. Always use complex correlation for chirp detection
2. Dual chirp gap timing gives CFO estimate (up shifts left, down shifts right)
3. Position correction: `true_pos = detected_pos + CFO × 10`

**Test verification:**
```bash
./test_iwaveform --snr 5 --cfo 50 --channel awgn -w mc_dpsk --frames 10
# Expected: 100% decode rate
```

---

## 2026-01-26: OFDM_CHIRP CFO - Trust Chirp Estimate

**What was broken:**
- OFDM_CHIRP decode failed with CFO
- Training symbol CFO estimation was overwriting correct chirp CFO
- Training was measuring carrier phase advance (wrong metric)

**What was changed:**
- `src/ofdm/demodulator_impl.hpp`: Added `chirp_cfo_estimated` flag
- Flag is set when `setFrequencyOffset()` is called
- `processPresynced()` trusts chirp CFO instead of re-estimating from training

**How it's properly fixed:**
- When chirp-based CFO is available, skip training-based re-estimation
- Training-based CFO is less accurate (100ms vs 1+ second signal)
- The `toBaseband()` function applies CFO correction before FFT

**Invariants:**
1. Chirp CFO > Training CFO in accuracy
2. Set `chirp_cfo_estimated = true` when CFO comes from chirp detection
3. Apply CFO in `toBaseband()` before FFT

**Test verification:**
```bash
./test_iwaveform --snr 17 --cfo 50 --channel awgn -w ofdm_chirp --frames 10
# Expected: 100% decode rate
```
