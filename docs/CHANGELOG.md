# ProjectUltra Change Log

This log tracks all bug fixes and behavioral changes to prevent re-doing work due to lost context.

**Format:** Each entry must include:
1. What was broken (symptom + root cause)
2. What was changed (files, code)
3. How it's properly fixed (why it works, invariants)
4. Test verification (command + expected output)

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
