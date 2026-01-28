# ProjectUltra Change Log

This log tracks all bug fixes and behavioral changes to prevent re-doing work due to lost context.

**Format:** Each entry must include:
1. What was broken (symptom + root cause)
2. What was changed (files, code)
3. How it's properly fixed (why it works, invariants)
4. Test verification (command + expected output)

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
