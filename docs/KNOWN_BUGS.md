# Known Bugs and Issues

**Purpose:** Track all known bugs, limitations, and issues that need fixing. This prevents context loss from causing us to forget about problems or re-discover them.

**Format:** Each bug must have: ID, Status, Description, Impact, Workaround (if any), and Fix Plan.

---

## Active Bugs

### BUG-001: cli_simulator CFO Simulation is Incomplete

**Status:** FIXED - Full protocol works (CFO only affects preamble, but that's enough)
**Discovered:** 2026-01-27
**Updated:** 2026-01-28
**Location:** `tools/cli_simulator.cpp`

**Description:**
The `tx_cfo_hz` parameter only shifts the chirp preamble frequency, NOT the OFDM/DPSK data. However, for protocol testing this is sufficient as the preamble CFO is estimated and used for correction.

**Resolution (2026-01-28):**
- Fixed PING vs DPSK detection in `modem_rx.cpp`
- Fixed control frame code rate (uses negotiated rate when connected)
- Fixed OFDM_COX min_samples for short frames
- Fixed total_cw patching for negotiated code rate
- Full protocol flow now works: PING→PONG→CONNECT→DATA×3→DISCONNECT

**Test Verification:**
```bash
./build/cli_simulator --snr 20 --test
# All 4 phases pass: CONNECTION, MODE NEGOTIATION, DATA TRANSFER, DISCONNECT
```

**Note:** For precise CFO testing, use `test_iwaveform` which applies CFO via FFT-based Hilbert transform (no group delay).

---

### BUG-002: RxPipeline Chirp Detection Fails

**Status:** FIXED and DELETED - 2026-01-28
**Discovered:** 2026-01-26

**Description:**
RxPipeline had incorrect IWaveform call sequence. Fixed by StreamingDecoder, then RxPipeline deleted entirely.

**Resolution:**
- Created `StreamingDecoder` with correct call sequence (reset → detectSync → setFrequencyOffset → process)
- Deleted RxPipeline completely (2026-01-28)
- StreamingDecoder is now the sole RX decoder

---

### BUG-005: StreamingDecoder Batch-Search Causes Buffer Position Drift

**Status:** FIXED - 2026-02-01 (StreamingDecoder redesigned with continuous correlation)
**Discovered:** 2026-01-30
**Location:** `src/gui/modem/streaming_decoder.cpp`

**Description:**
The StreamingDecoder uses a batch-search architecture that doesn't match real receiver behavior:
1. Waits for 144000 samples (3 seconds) before first search
2. Copies buffer to work_buffer, searches for chirp
3. By the time search happens, buffer positions have drifted
4. Result: chirp is found, but `process()` reads noise instead of signal

**Symptom:**
```
[MC-DPSK] RMS: training[0]=0.005609, ref[4096]=0.005656, data[4608]=0.005596
```
RMS is 0.005 (noise) when it should be 0.28 (signal).

**Impact:**
- cli_simulator connection fails at CONNECT_ACK phase
- gui_simulator with virtual station has same issue
- test_iwaveform works because it injects entire signal at once (no timing issues)

**Root Cause:**
Real receivers use **continuous correlation** - run matched filter on every incoming sample. Current code uses **batch search** - wait 3 seconds, then search. This creates timing drift between audio thread and decode thread.

**Workaround:**
Use test_iwaveform for testing (batch file injection works fine).

**Fix Plan:**
See `docs/STREAMING_DECODER_REDESIGN.md` for detailed plan:
1. Move correlation from processBuffer() to feedAudio()
2. Simple state machine: SEARCHING → SYNC_FOUND → DECODING
3. Remove MIN_SAMPLES_FOR_SEARCH requirement
4. Reduce buffer size from 960k to 120k samples

---

### BUG-003: ModemEngine Routes ALL Chirp Frames to MC-DPSK

**Status:** OBSOLETE - Acquisition thread removed
**Discovered:** 2026-01-27

**Description:**
The acquisition thread was removed (2026-01-28). StreamingDecoder now handles all RX processing and uses the waveform mode set by the protocol/connection state.

**Current Behavior:**
- Disconnected mode: StreamingDecoder uses MC-DPSK for PING detection
- Connected mode: StreamingDecoder uses the negotiated waveform
- Waveform selection is determined by connection state, not frame detection

---

### BUG-004: OFDM_COX CFO Correction Not Verified

**Status:** FIXED - 2026-01-28
**Discovered:** 2026-01-27
**Commit:** `7264753`

**Resolution:**
OFDM_COX now uses the same path as OFDM_CHIRP:
- detectSync() uses searchForSync() for Schmidl-Cox sync detection
- process() calculates accumulated CFO phase and calls processPresynced()
- Both waveforms share processPresynced() for demodulation (no code duplication)

**Test Results:**
- 100% decode at 17+ dB AWGN with CFO = ±50 Hz
- Works on stable channels; use OFDM_CHIRP for fading channels

---

## Fixed Bugs (Reference)

### BUG-F001: MC-DPSK CFO Correction for Training Samples
**Status:** FIXED - 2026-01-27
**Commit:** `48e6271`
**Details:** See docs/CHANGELOG.md

### BUG-F002: Chirp Detection Position Shift with CFO
**Status:** FIXED - 2026-01-26
**Commit:** `b2592a0`
**Details:** See docs/CHANGELOG.md

### BUG-F003: OFDM_CHIRP Training Symbol CFO Override
**Status:** FIXED - 2026-01-26
**Commit:** `f1fce06`
**Details:** See docs/CHANGELOG.md

### BUG-F004: OFDM_COX CFO Correction
**Status:** FIXED - 2026-01-28
**Commit:** `7264753`
**Details:** OFDM_COX now uses processPresynced() path like OFDM_CHIRP. 100% at ±50 Hz on AWGN 17+ dB.

### BUG-F007: ARQ advanceRXWindow Delivers Frames With Wrong MORE_FRAG Flag
**Status:** FIXED - 2026-02-06
**Location:** `src/protocol/selective_repeat_arq.cpp`
**Details:** When `advanceRXWindow()` delivered multiple buffered frames in sequence after retransmission filled a gap, `lastRxHadMoreData()` returned the flag from the last-arrived frame (the gap-filler), not the frame being delivered. Fixed by updating `last_rx_flags_` and `last_rx_more_data_` from each slot's stored flags before calling the delivery callback.

### BUG-F008: detectDataSync False Peaks From 1-CW LDPC Zero-Padding
**Status:** FIXED - 2026-02-06
**Location:** `src/waveform/ofdm_chirp_waveform.cpp`
**Details:** 1-CW ACK frames have LDPC zero-padding creating identical adjacent OFDM symbols. Schmidl-Cox autocorrelation found false ~1.0 peaks from these data symbols. Fixed by early exit at first peak > 0.95 (the real LTS is always first).

### BUG-F009: 1-CW Control Frame Sample Overconsumption
**Status:** FIXED - 2026-02-06
**Location:** `src/gui/modem/streaming_decoder.cpp`
**Details:** After decoding a 1-CW ACK (10368 samples), decoder consumed 31104 samples (4-CW size), eating into the next data frame. Fixed by detecting 1-CW control frames and advancing by `getMinSamplesForControlFrame()`.

### BUG-F005: CW[0] Decode Failures Due to Noise Variance
**Status:** FIXED - 2026-02-02
**Commit:** `d4ff083`
**Details:** First symbol fallback set noise_count=1, but update condition required noise_count>1. Changed condition from `(noise_count > 1)` to `(noise_count > 0)` in channel_equalizer.cpp.

### BUG-F006: Channel Interleaving Was Completely Disabled
**Status:** FIXED - 2026-02-02
**Commit:** `71471d4`
**Location:** `src/protocol/frame_v2.cpp`
**Details:** Channel interleaving code in encodeFixedFrame and decodeFixedFrame was disabled via `(void)use_channel_interleave;`. Re-enabled properly using ChannelInterleaver with BITS_PER_SYMBOL=106 (53 data carriers × 2 bits for DQPSK). Verified working with `--channel-interleave` flag on clean AWGN at SNR 20 dB.

---

---

## Planned Improvements

### IMP-001: Rename test_iwaveform

**Status:** TODO
**Priority:** LOW

**Description:**
`test_iwaveform` will become the primary/official modem test tool. The name should reflect this importance.

**Suggested names:**
- `test_modem` - Simple, clear
- `modem_test` - Matches convention
- `ultra_test` - Project-branded

**Action items:**
1. Rename `tools/test_iwaveform.cpp` → `tools/test_modem.cpp`
2. Update CMakeLists.txt
3. Update regression_matrix.sh
4. Update all documentation references

---

### IMP-002: Remove Legacy test_hf_modem

**Status:** TODO - After refactor complete
**Priority:** LOW

**Description:**
`test_hf_modem` is legacy code kept for reference. Once `test_iwaveform` (renamed) is fully verified and the refactor is complete, remove it.

**Blocked by:** Refactor completion, full test coverage in test_iwaveform

---

## Limitations (Not Bugs)

### LIM-001: Poor HF Channel Performance
**Description:** OFDM modes fail on poor HF channels (2ms delay spread) because multipath exceeds cyclic prefix.
**Not a bug:** This is a fundamental limitation of OFDM with current CP length.
**Mitigation:** Use MC-DPSK for poor channels (90-100% success).

### LIM-002: Schmidl-Cox Requires 17+ dB
**Description:** Schmidl-Cox sync detection needs ~17 dB SNR minimum.
**Not a bug:** This is expected behavior for the algorithm.
**Mitigation:** Use chirp sync (MC-DPSK, OFDM_CHIRP) for lower SNR.

### LIM-003: Single Carrier DPSK Floor at -5 dB
**Description:** Even single-carrier DPSK fails below -5 dB (20-40% success).
**Not a bug:** Approaching theoretical limits.
**Mitigation:** -3 dB is the reliable threshold, use higher rate codes above 0 dB.

---

## Bug Tracking Process

When discovering a new bug:
1. Add entry here with unique ID (BUG-XXX)
2. Set status: OPEN, IN_PROGRESS, or FIXED
3. Document root cause if known
4. Document workaround if available
5. When fixed, move to "Fixed Bugs" section with commit hash

When fixing a bug:
1. Update status to FIXED
2. Add commit hash
3. Add entry to docs/CHANGELOG.md with full details
4. Move entry to "Fixed Bugs" section
