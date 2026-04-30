# Refactoring Progress Tracker

**Purpose:** Track progress on the architecture refactoring plan. This prevents re-doing work after context loss.

**Source Plan:** `~/.claude/plans/eager-percolating-naur.md`

**Last Updated:** 2026-02-06

---

## Overall Progress: ~95% Complete

```
Phase 1: Core Interfaces     [########--] 80%   (IWaveform complete, ISyncMethod/ICodec optional)
Phase 2: Waveform Impl       [##########] 100%  (OFDM_COX CFO fixed!)
Phase 3: ModemEngine Refactor[##########] 100%  (StreamingDecoder works, continuous audio fixed)
Phase 4: Sync Methods        [----------]  0%   (optional - waveforms own their sync internally)
Phase 5: Configuration       [----------]  0%   (optional - nice-to-have)
Phase 6: Bug Fixes           [##########] 100%  (BUG-005 fixed, all active bugs resolved)
Phase 7: Continuous Audio    [##########] 100%  (cli_simulator full protocol working)
Phase 8: Throughput          [##########] 100%  (1-CW ACK + R1/2 rate selection)
```

### Recent Updates (2026-02-06)
- ✅ **1-CW OFDM ACK frames**: Control frames encoded as 1 CW (0.216s) instead of 4 CW (0.648s)
- ✅ **R1/2 rate selection enabled**: Automatic selection based on SNR + fading index
- ✅ **Fixed detectDataSync false peaks**: Early exit at corr>0.95 prevents LDPC zero-padding peaks
- ✅ **Fixed 1-CW sample overconsumption**: Decoder advances by correct frame size for control frames
- ✅ **Fixed ARQ advanceRXWindow MORE_FRAG**: Per-slot flag delivery prevents message reassembly failure
- ✅ **Diagnostic cleanup**: Removed all temporary INFO-level debug logging

### Previous Updates (2026-01-30)
- ✅ **FIXED BUG-005**: StreamingDecoder batch-search → continuous audio fixed
- ✅ cli_simulator full protocol working (PING→CONNECT→MODE→DATA→DISCONNECT)
- ✅ Good fading 100% CW success at R1/4 SNR=15 (CFO feedback + LTS residual correction)
- ✅ MC-DPSK low SNR fix (relative PING detection + SNR-proportional soft bits)

### Previous Updates (2026-01-28)
- ✅ Fixed PING detection in cli_simulator (connection phase works)
- ✅ Added fading detection for mode negotiation
- ✅ Fixed control frame code rate (ACK/NACK use negotiated rate when connected)
- ✅ Fixed OFDM_COX min_samples for short frames (ACK detection now works)
- ✅ **cli_simulator DATA phase WORKING!** All 3 messages sent/received correctly
- ✅ **cli_simulator DISCONNECT phase WORKING!** Fixed total_cw mismatch for negotiated rate

### Blocking Issue
**BUG-005** blocks cli_simulator and gui_simulator from working with continuous audio.
- test_iwaveform still works (batch file injection)
- See `docs/archive/STREAMING_DECODER_REDESIGN.md` for fix plan

---

## Phase 1: Core Interfaces (Foundation)

### 1.1 IWaveform Interface
- [x] Create `src/waveform/waveform_interface.hpp`
- [x] Define SyncResult struct
- [x] Define WaveformCapabilities struct
- [x] Define IWaveform abstract class
- [x] Methods: detectSync, process, getSoftBits, reset
- [x] Methods: setFrequencyOffset, isSynced, hasData
- [x] Methods: generatePreamble, modulate (TX)
- [x] Methods: getStatusString, getCarrierCount (GUI support) - all waveforms implement these

**Status:** ✅ COMPLETE
**Files:** `src/waveform/waveform_interface.hpp`
**Commit:** `ed32e05`

### 1.2 ISyncMethod Interface
- [ ] Create `src/sync/sync_interface.hpp`
- [ ] Define ISyncMethod abstract class
- [ ] Methods: detect, generatePreamble, estimateCFO

**Status:** ❌ NOT STARTED
**Notes:** Low priority - waveforms currently own their sync methods internally

### 1.3 ICodec Interface
- [ ] Create `src/fec/codec_interface.hpp`
- [ ] Define ICodec abstract class
- [ ] Methods: encode, decode, setRate, getRate

**Status:** ❌ NOT STARTED
**Notes:** Low priority - LDPC codec works fine as-is

---

## Phase 2: Waveform Implementations

### 2.1 Concrete Waveforms

#### MCDPSKWaveform
- [x] Create `src/waveform/mc_dpsk_waveform.hpp`
- [x] Create `src/waveform/mc_dpsk_waveform.cpp`
- [x] Implement detectSync with chirp detection
- [x] Implement process with demodulation
- [x] Implement getSoftBits
- [x] Implement setFrequencyOffset
- [x] CFO correction working
- [x] Tested with test_iwaveform

**Status:** ✅ COMPLETE
**Commit:** `ed32e05`, `48e6271`

#### OFDMChirpWaveform
- [x] Create `src/waveform/ofdm_chirp_waveform.hpp`
- [x] Create `src/waveform/ofdm_chirp_waveform.cpp`
- [x] Implement detectSync with chirp detection
- [x] Implement process with OFDM demodulation
- [x] Implement getSoftBits (fixed loop issue)
- [x] Implement setFrequencyOffset
- [x] CFO correction working
- [x] Tested with test_iwaveform

**Status:** ✅ COMPLETE
**Commit:** `ed32e05`, `84bb563`

#### OFDMCoxWaveform (was OFDM_NVIS)
- [x] Create `src/waveform/ofdm_cox_waveform.hpp`
- [x] Create `src/waveform/ofdm_cox_waveform.cpp`
- [x] Implement detectSync with Schmidl-Cox (uses searchForSync)
- [x] Implement process (uses processPresynced - same as OFDM_CHIRP!)
- [x] Implement getSoftBits
- [x] Implement setFrequencyOffset with accumulated phase
- [x] CFO correction verified (100% at ±50 Hz on AWGN 17+ dB)
- [x] Tested with test_iwaveform

**Status:** ✅ COMPLETE
**Commit:** `20a2643`, `7264753`
**Notes:** Fixed 2026-01-28 - now uses same processPresynced path as OFDM_CHIRP.
         Works on AWGN/stable channels. Use OFDM_CHIRP for fading channels.

#### OTFSWaveform
- [ ] Create `src/waveform/otfs_waveform.hpp`
- [ ] Create `src/waveform/otfs_waveform.cpp`
- [ ] Wrap existing OTFS modulator/demodulator

**Status:** ❌ NOT STARTED
**Notes:** Deferred until core refactor is complete

#### AFDMWaveform (IN PROGRESS)
- [x] Create `src/afdm/afdm_config.hpp` - configuration parameters
- [x] Create `src/afdm/daft.hpp/cpp` - DAFT/IDAFT transforms
- [x] Create `src/afdm/afdm.hpp/cpp` - modulator/demodulator
- [x] Create `tools/test_afdm.cpp` - test suite
- [x] DAFT roundtrip tests PASS (error ~5e-7)
- [x] Complex baseband chain PASSES (0% BER at SNR=10 dB)
- [x] Upmix/downmix with lowpass filter PASSES for narrowband signals
- [ ] Update modulator to use `num_carriers` instead of all N bins
- [ ] Update demodulator to extract only active carrier bins
- [ ] Integrate with IWaveform interface
- [ ] Test on fading channels

**Status:** 🔄 IN PROGRESS (~50%)
**Files:** `src/afdm/afdm_config.hpp`, `src/afdm/daft.hpp`, `src/afdm/daft.cpp`, `src/afdm/afdm.hpp`, `src/afdm/afdm.cpp`, `tools/test_afdm.cpp`

**Key Finding:** AFDM audio chain requires using only `num_carriers` (e.g., 30) out of the full N bins (e.g., 512) to create a narrowband signal that fits within the HF audio band (300-3000 Hz). This allows lowpass filtering to remove the 2×fc image without destroying the signal.

**Remaining Work:**
1. Modify `insertPilots()` to populate only bins around DC (±15 bins for 30 carriers)
2. Modify `extractDataSymbols()` to only read from active bins
3. Update `estimateChannel()` to only use pilots in active bins
4. Create `AFDMWaveform` class implementing `IWaveform`
5. Add AFDM to `WaveformFactory`

### 2.2 WaveformFactory
- [x] Create `src/waveform/waveform_factory.hpp`
- [x] Create `src/waveform/waveform_factory.cpp`
- [x] Implement create() method
- [x] Support MC_DPSK, OFDM_CHIRP, OFDM_COX

**Status:** ✅ COMPLETE
**Commit:** `20a2643`

---

## Phase 3: Refactor ModemEngine

### 3.1 Simplify ModemEngine
- [ ] Replace 8 modulator/demodulator pointers with single IWaveform
- [ ] Remove waveform-specific if-else chains
- [ ] Use WaveformFactory for waveform creation

**Status:** ❌ NOT STARTED
**Blocked by:** RxPipeline bugs (BUG-002)

### 3.2 Create WaveformState Class
- [ ] Create `src/gui/modem/waveform_state.hpp`
- [ ] Encapsulate mode state machine
- [ ] Methods: getPhase, getActiveMode, getModeForTx

**Status:** ❌ NOT STARTED

### 3.3 Refactor TX Path
- [x] TX uses IWaveform for OFDM_CHIRP
- [x] TX uses IWaveform for MC_DPSK
- [x] TX uses IWaveform for OFDM_COX
- [ ] TX uses IWaveform for OTFS (no OTFSWaveform yet)
- [ ] Remove legacy TX modulators (cleanup)

**Status:** ✅ MOSTLY COMPLETE (OTFS pending)

### 3.4 Refactor RX Path with StreamingDecoder (Replaces RxPipeline)
- [x] Create `src/gui/modem/streaming_decoder.hpp` (fixes BUG-002)
- [x] Create `src/gui/modem/streaming_decoder.cpp`
- [x] Sliding window search (like test_iwaveform)
- [x] Correct IWaveform call sequence (reset, detectSync, setFrequencyOffset, process)
- [x] Circular buffer with bounded size
- [x] Thread-safe with condition variable
- [x] Compiles and passes regression tests
- [x] Integrate into ModemEngine
- [x] Replace processRxBuffer_* methods
- [x] Delete RxPipeline (DELETED 2026-01-28)

**Status:** ✅ COMPLETE (StreamingDecoder is primary, acquisition thread removed)
**Notes:** StreamingDecoder is now the ONLY decoder (2026-01-28).
- feedAudio() only feeds to StreamingDecoder
- rxDecodeLoop() only uses StreamingDecoder
- Acquisition thread removed (~1200 lines of legacy code deleted)
- All 11 regression tests pass
- PING detection fixed (2026-01-28): cli_simulator connection phase works

**Bug Fix:** RxPipeline called reset() AFTER setFrequencyOffset(), violating INV-WAVE-002.
         StreamingDecoder uses correct order: reset() → detectSync() → setFrequencyOffset() → process()

### 3.5 Fading Detection for Mode Negotiation
- [x] Add per-carrier magnitude tracking to MultiCarrierDPSKDemodulator
- [x] Implement getFadingIndex() (coefficient of variation)
- [x] Add to IWaveform interface
- [x] Add to MCDPSKWaveform
- [x] Add to StreamingDecoder (last_fading_index_)
- [x] Add to ModemEngine (getFadingIndex, isFading)
- [x] Update Connection class (setChannelQuality)
- [x] Update negotiateMode() in connection_handlers.cpp

**Status:** ✅ COMPLETE (2026-01-28)
**Notes:** Mode negotiation now considers both SNR and fading index

### 3.6 CLI Simulator Full Protocol Flow
- [x] Fix PING detection in StreamingDecoder
- [x] Add trailing silence to PING/PONG for buffer fill
- [x] PING → PONG → CONNECT → CONNECT_ACK flow works
- [x] DATA phase works (all 3 messages sent/received correctly)
- [x] DISCONNECT phase works (fixed total_cw mismatch for negotiated rate)

**Status:** ✅ COMPLETE (all 4 phases work)
**Notes:** Full protocol flow working at SNR 20 dB AWGN: PING→PONG→CONNECT→DATA×3→DISCONNECT

---

## Phase 4: Sync Method Implementations

### 4.1 Wrap Existing Sync Methods
- [ ] Create ChirpSyncMethod wrapper
- [ ] Create SchmidlCoxMethod wrapper
- [ ] Create BarkerSyncMethod wrapper
- [ ] Create ZadoffChuMethod wrapper

**Status:** ❌ NOT STARTED
**Notes:** Low priority - current internal ownership works

---

## Phase 5: Configuration System

### 5.1 Centralized Config
- [ ] Create `src/config/modem_config.hpp`
- [ ] Define ChirpConfig, OFDMConfig structs
- [ ] YAML file support (optional)

**Status:** ❌ NOT STARTED
**Notes:** Nice to have, not blocking

---

## Phase 6: Bug Fixes During Refactor

### 6.1 Fix CFO Handling
- [x] MC-DPSK CFO correction (100% at ±50 Hz)
- [x] OFDM_CHIRP CFO correction (100% at -45 to +50 Hz, verified 2026-01-27)
- [x] OFDM_COX CFO correction (100% at ±50 Hz on AWGN 17+ dB, verified 2026-01-28)

**Status:** ✅ COMPLETE (all three waveforms verified)
**Notes:** OFDM_COX now uses same processPresynced path as OFDM_CHIRP. Works on stable channels; use CHIRP for fading.

### 6.2 Acquisition Thread Routing
- [ ] Detect frame type from preamble
- [ ] Route to correct decoder

**Status:** ❌ NOT STARTED
**Tracked as:** BUG-003

### 6.3 Thread Safety Audit
- [ ] Audit shared variables
- [ ] Add proper synchronization

**Status:** ❌ NOT STARTED

### 6.4 Disconnect ACK Waveform
- [x] Fixed: Uses disconnect_waveform_

**Status:** ✅ COMPLETE
**Commit:** `cdfacbb`

---

## Test Tool Status

| Tool | Status | Future |
|------|--------|--------|
| **test_iwaveform** | ✅ WORKING | **PRIMARY** - Will become official test tool (rename planned) |
| **cli_simulator** | ✅ FULLY WORKING | **PRIMARY** - Full PING→CONNECT→DATA→DISCONNECT flow works! |
| test_hf_modem | ❌ DEPRECATED | Removed from build - references deleted rx_pipeline.hpp |
| profile_acquisition | ⚠️ UNKNOWN | TBD |

### Testing Strategy

**Official test tools (keep and improve):**
1. `test_iwaveform` → Rename to `test_modem` or `modem_test`
   - Tests IWaveform interface directly
   - CFO simulation uses FFT-based Hilbert (no group delay, fixed 2026-01-27)
   - Single-frame and multi-frame testing
   - All waveforms, channels, SNR, CFO combinations

2. `cli_simulator` → Keep for protocol testing
   - Full protocol flow (PING → CONNECT → DATA → DISCONNECT)
   - Two-station simulation
   - **DATA phase working (2026-01-28)** - All messages sent/received correctly
   - All 4 phases complete (DISCONNECT fixed 2026-01-28)

**Removed:**
- `test_hf_modem` - Removed from build, references deleted rx_pipeline.hpp

---

## Documentation Status

| Document | Status | Purpose |
|----------|--------|---------|
| `INVARIANTS.md` | ✅ COMPLETE | Critical rules |
| `KNOWN_BUGS.md` | ✅ COMPLETE | Bug tracking |
| `CHANGELOG.md` | ✅ COMPLETE | Change history |
| `REFACTOR_PROGRESS.md` | ✅ COMPLETE | This file |
| `MODEM_ENGINE_ARCHITECTURE.md` | ✅ COMPLETE | ModemEngine internals |
| `DUAL_CHIRP_CFO_ANALYSIS.md` | ✅ COMPLETE | CFO handling |
| `TESTING_METHODOLOGY.md` | ✅ COMPLETE | Test approach |
| `CFO_CORRECTION_FLOW.md` | ✅ COMPLETE | CFO code paths |
| `PROTOCOL_V2.md` | ✅ COMPLETE | Protocol spec |
| `GUI_ARCHITECTURE.md` | ✅ COMPLETE | GUI structure |
| `AUDIO_SYSTEM.md` | ✅ COMPLETE | Audio I/O |
| `CONFIGURATION_SYSTEM.md` | ✅ COMPLETE | Settings/config |
| `BUILD_SYSTEM.md` | ✅ COMPLETE | CMake/deps |
| `ADDING_NEW_WAVEFORM.md` | ✅ COMPLETE | How-to guide |
| `GIT_WORKFLOW.md` | ✅ COMPLETE | Commit/branch strategy |

**All 15 documentation files complete!** Ready for any future development.

---

## Blocking Issues

~~1. **BUG-002: RxPipeline chirp detection** - FIXED by StreamingDecoder (2026-01-28)~~

Remaining:
1. **BUG-003: Acquisition routing** - Not a real issue (use connection state, see plan)

---

## Next Steps (Priority Order)

1. [x] Create StreamingDecoder (replaces RxPipeline, fixes BUG-002) - DONE 2026-01-28
2. [x] Integrate StreamingDecoder into ModemEngine - DONE 2026-01-28
   - feedAudio() feeds to both StreamingDecoder AND legacy path (parallel)
   - rxDecodeLoop() checks StreamingDecoder for frames
   - All 11 regression tests pass
3. [x] **StreamingDecoder is now PRIMARY decoder** - DONE 2026-01-28
   - feedAudio() only feeds to StreamingDecoder
   - rxDecodeLoop() only uses StreamingDecoder
4. [x] Remove acquisition thread - DONE 2026-01-28
   - Removed ~1200 lines of legacy code (acquisitionLoop, processRxBuffer_*, etc.)
   - Mode switches now call streaming_decoder_->reset()
   - All 11 regression tests pass
5. [x] TX Path Unification (Phase 4) - DONE 2026-01-28
   - transmit() now uses IWaveform for MC_DPSK, OFDM_CHIRP, OFDM_COX
   - OTFS keeps legacy path (no OTFSWaveform yet)
   - All 11 regression tests pass
6. [x] Verify OFDM_COX CFO correction - DONE 2026-01-28
   - Uses searchForSync() for Schmidl-Cox detection
   - Uses processPresynced() for demodulation (same as OFDM_CHIRP!)
   - 100% decode at 17+ dB AWGN with CFO=±50Hz
7. [x] Delete RxPipeline (deprecated, no longer used for decoding) - DONE 2026-01-28
   - Removed rx_pipeline_ member from ModemEngine
   - Removed all references from modem_engine.cpp, modem_mode.cpp
   - Deleted rx_pipeline.hpp and rx_pipeline.cpp
   - Updated CMakeLists.txt to remove from all build targets
   - All 11 regression tests pass
8. [ ] Create WaveformState class (optional, nice-to-have)
9. [x] Remove unused modulators - DONE 2026-01-28
   - Removed mc_dpsk_modulator_ and mc_dpsk_demodulator_ (never used)
   - Removed dpsk_modulator_ (never used, kept demodulator for mode switching)
   - Config structs kept for GUI display (getMCDPSKCarriers, etc.)
   - All 11 regression tests pass
10. [ ] Implement channel condition detection for adaptive mode selection

---

## Commits Reference

| Commit | Description |
|--------|-------------|
| `ed32e05` | Add IWaveform interface and concrete waveform implementations |
| `20a2643` | Rename OFDM_NVIS to OFDM_COX and integrate waveform factory |
| `e5baa72` | Integrate RxPipeline into ModemEngine for connected mode RX |
| `ffc979c` | WIP: Update RxPipeline and test_hf_modem for IWaveform interface |
| `398cbd0` | WIP: Debug RxPipeline chirp detection issues |
| `48e6271` | Fix MC-DPSK CFO correction for training/reference samples |
| `84bb563` | Add OFDM_CHIRP support to test_iwaveform with CFO correction |
| `cdfacbb` | Fix disconnect ACK waveform and improve GUI status display |
