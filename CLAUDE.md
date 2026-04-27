# ProjectUltra - HF Sound Modem

## FRESH SESSION? START HERE

**If this is a new/fresh session, do this FIRST before any work:**

1. **Read current state:** `cat docs/REFACTOR_PROGRESS.md` - Shows what's done, what's in progress, blocking issues
2. **Check known bugs:** `cat docs/KNOWN_BUGS.md` - Active bugs you must not re-discover
3. **Check recent changes:** `git log --oneline -10` - See recent commits

**Before modifying ANY code, read:**
- `docs/INVARIANTS.md` - Critical rules that MUST NOT be violated (causes subtle bugs if ignored)

**This project has 16 documentation files.** They exist because context was lost repeatedly, causing rework. USE THEM.

---

## CRITICAL RULES (Never Violate)

**Test binaries:**
- ALWAYS run from `build/` directory: `./build/cli_simulator`, NOT `./cli_simulator`
- `cli_simulator` - **PRIMARY** test tool for full protocol with light preamble in connected mode
  - Tests: PING/PONG → CONNECT → MODE_CHANGE → DATA (4CW frame interleaved) → DISCONNECT
  - Command: `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test 2>&1 | tee /tmp/test_output.log`
- `test_waveform_simple` - Quick single-frame sanity checks (NOT for connected mode testing)
- `test_iwaveform` - REMOVED (was renamed to `test_iwaveform_DO_NOT_USE.cpp`, no longer builds)
- `test_hf_modem` is LEGACY - do not use

**MC-DPSK invariants:**
- ALWAYS call `mc_dpsk_demodulator_->reset()` at start of `rxDecodeDPSK()`
- ALWAYS call `setCFO(frame.cfo_hz)` to reset CFO accumulation between frames
- CFO from chirp detection is TRUSTED over training-based CFO estimation

**OFDM CFO invariants (see `docs/CFO_CORRECTION_FLOW.md` for full details):**
- Chirp CFO is coarse — on fading channels it can be wrong by ±2 Hz
- LTS residual CFO estimation REFINES chirp CFO per frame (threshold 0.3 Hz)
- CFO feedback loop: after demodulation, corrected CFO propagates back to cached `last_cfo_`
- NEVER remove the feedback in `ofdm_chirp_waveform.cpp:process()` or `streaming_decoder.cpp`
- Without feedback, wrong chirp CFO re-injects on every frame → progressive phase drift → CW failures

**Testing invariants:**
- Use SINGLE ModemEngine instance for entire audio stream (continuous RX)
- Buffer limit: MAX_PENDING_SAMPLES = 960000 (20 seconds at 48kHz)
- **PRIMARY regression test:** `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test 2>&1 | tee /tmp/test_output.log`
- `cli_simulator` is the ONLY tool that tests the full protocol with light preamble, two-station interaction, and proper connected-mode configuration. `test_waveform_simple` is for quick single-frame sanity checks only.
- `regression_matrix.sh` is OUTDATED — it uses `test_iwaveform` which no longer exists. Do NOT run it.
- **ALWAYS use `| tee /tmp/test_output.log`** when running tests - tests take minutes and we need full output for debugging

---

## TWO OPERATING MODES - CRITICAL ARCHITECTURE

**The modem has TWO completely different operating modes based on SNR:**

### Mode 1: MC-DPSK (SNR ≤ 10 dB)
- **When:** SNR below 10, or heavy fading conditions
- **Waveform:** Multi-Carrier DPSK with chirp sync
- **ARQ:** Stop-and-wait (window=1) - send ONE frame, wait for ACK
- **Frame format:** Variable codewords, simple sequential encoding
- **Control frames:** 20 bytes (ACK, NACK, etc.) - NO patching, encode as-is
- **Data frames:** Variable CWs based on payload size
- **Interleaving:** NONE (no frame interleaving, no channel interleaving)
- **Preamble:** ALWAYS full chirp preamble (no light sync)
- **Key files:** `decodeMCDPSKFrame()` in streaming_decoder.cpp

### Mode 2: OFDM (SNR ≥ 10 dB)
- **When:** SNR 10 and above with acceptable fading
- **Waveform:** OFDM with chirp or Schmidl-Cox sync
- **ARQ:** Selective Repeat (window=8) - send up to 8 frames before waiting
- **Frame format:** Fixed 4-codeword frames for data, 1-codeword for control
- **Control frames:** 20 bytes, 1 CW, no frame interleaving (fast ACK)
- **Data frames:** 4 CWs with frame interleaving
- **Interleaving:** Frame-level interleaving (spreads CWs), optional channel interleaving
- **Preamble:** Light preamble (LTS only) for data after handshake
- **Key files:** `decodeFixedFrame()` in frame_v2.cpp

### Mode 3: OFDM_NARROW (SNR 5-10 dB, 500 Hz bandwidth)
- **When:** Narrowband chirp detected (1250-1750 Hz), low SNR where wideband fails
- **Waveform:** OFDM with narrowband chirp sync, FFT=2048, 21 carriers, 492 Hz BW
- **ARQ:** Stop-and-wait (window=1) - same as MC-DPSK due to long frame times (~3s)
- **Frame format:** Same as wideband OFDM (4 CW data, 1 CW control)
- **Interleaving:** Frame-level + optional channel interleaving (same as OFDM)
- **Preamble:** Light preamble (LTS only) for data after handshake
- **Dual-listen:** RX listens for both wideband AND narrowband chirps when idle
- **Throughput:** ~103 bps (R1/4) to ~230 bps (R1/2) — 10× slower than wideband but works at 7.5 dB lower SNR

### Mode Selection Flow
1. **Connection starts:** MC-DPSK for PING/PONG/CONNECT (wideband or narrowband chirp)
2. **Dual-listen:** RX detects chirp type → sets BandwidthMode (WIDE or NARROW)
3. **After CONNECT_ACK:** SNR is measured, mode is negotiated
   - SNR < 10 + wideband: Stay in MC-DPSK
   - SNR ≥ 10 + wideband: Switch to OFDM_CHIRP
   - Narrowband detected: Switch to OFDM_NARROW
4. **enterConnected():** Sets ARQ window based on mode (1 for MC-DPSK/NARROW, 4 for wideband OFDM)
5. **StreamingEncoder/Decoder:** Check `mode_` to use correct path, `isOFDMMode()` for OFDM family

### NEVER MIX THESE:
- MC-DPSK frames through OFDM encoder (corrupts control frames)
- OFDM interleaving on MC-DPSK data
- Light preamble with MC-DPSK
- Window > 1 with MC-DPSK

---

**Performance Requirements (cli_simulator --test):**
| Mode | Channel | SNR | Required CW Success |
|------|---------|-----|---------------------|
| MC-DPSK | AWGN | 5+ | 100% |
| MC-DPSK | Moderate fading | 10 | 100% |
| OFDM_CHIRP | AWGN | 15+ | 100% |
| OFDM_CHIRP | Good fading | 15 | **100%** |
| OFDM_CHIRP | Moderate fading | 15 | ~90% |
| OFDM_NARROW | AWGN | 8+ | 100% |
| OFDM_NARROW | Good fading | 8 | 100% data, 90%+ ACK |

**Current state (2026-03-15):**
- MC-DPSK: WORKING - 100% at SNR=10 with moderate fading
- OFDM_CHIRP DQPSK R1/4 AWGN: WORKING - 100% at SNR=15 and SNR=20 (0 retries)
- OFDM_CHIRP DQPSK R1/4 Good fading SNR=15: WORKING - 100% (0 retries, 0 failures)
- OFDM_CHIRP DQPSK R1/4 Good fading SNR=10: WORKING - 30/30 seeds PASS (avg 1.5 retx, 100% delivery)
- OFDM_CHIRP DQPSK R1/4 Moderate fading SNR=15: WORKING - 5/5 seeds PASS (avg 1.4 retx, 100% delivery)
- OFDM_CHIRP DQPSK R1/2 AWGN: WORKING - 100% at SNR=15 and SNR=20 (0 retries)
- OFDM_CHIRP DQPSK R1/2 Good fading: WORKING - 100% at SNR=15 (5/5 seeds, 0 retries)
- OFDM_CHIRP DQPSK R1/2 Moderate fading SNR=15: WORKING - 5/5 seeds PASS (avg 2.4 retx, 100% delivery)
- OFDM_CHIRP DQPSK R2/3 AWGN: WORKING - 100% at SNR=20 (0 retries)
- OFDM_CHIRP DQPSK R2/3 Good fading SNR=20: WORKING - 30/30 seeds PASS, 0 retransmissions
- OFDM_CHIRP DQPSK R2/3 Good fading SNR=15: WORKING - 10/10 seeds PASS (avg 1.5 retx, 100% delivery)
- OFDM_CHIRP QPSK R1/2 AWGN: WORKING - 100% at SNR=20 (0 retries)
- OFDM_CHIRP QPSK R1/2 Good fading: WORKING - avg 95% frame success at SNR=20 (30-seed survey, all messages delivered via ARQ)
- OFDM_CHIRP QPSK R2/3 AWGN: WORKING - 100% at SNR=20 (0 retries)
- OFDM_CHIRP QPSK R2/3 Good fading: WORKING - 5/5 seeds PASS (2 seeds had retx, 3 clean)
- OFDM_CHIRP DQPSK R3/4 AWGN: WORKING - 100% at SNR=20 (10/10 seeds, 0 retries)
- OFDM_CHIRP DQPSK R3/4 Good fading: NOT RECOMMENDED (23 retx / 5 seeds — AWGN only)
- 1-CW ACK frames: WORKING - control frames use 1 CW (3x faster ACK)
- Variable-CW frames: WORKING - CONNECT/DISCONNECT use exact CW count (2 at R1/2, 3 at R1/4)
- OFDM_NARROW DQPSK R1/4 AWGN: WORKING - 100% at SNR=8 (0 retransmissions)
- OFDM_NARROW DQPSK R1/4 Good fading SNR=8: WORKING - 100% data decode, 93% ACK, all messages delivered via ARQ
- OFDM_COX: WORKING - DATA phase passes at SNR=20 dB
- OTFS: EXPERIMENTAL - See OTFS Status section below
- cli_simulator: FULLY WORKING - all phases pass on AWGN and fading

**Auto rate selection ladder (2026-03-15, updated with 802.11n LDPC):**
| Condition | Auto rate | Payload/frame | Throughput |
|-----------|-----------|---------------|------------|
| SNR >= 20, AWGN (fading < 0.15) | **R3/4** | 243 bytes | ~3900 bps |
| SNR >= 15, near-AWGN (fading < 0.15) | **R2/3** | 197 bytes | ~3200 bps |
| SNR >= 15, good/moderate fading (< 1.10) | **R1/2** | 141 bytes | ~2300 bps |
| Everything else | **R1/4** | 62 bytes | ~1150 bps |

**Temporal fading measurement (2026-02-03):**
- `getFadingIndex()` now combines freq_cv (multipath) + temporal_cv (Doppler spread)
- temporal_cv measures per-carrier magnitude variance over ~40+ symbols (~0.4s)
- Good channels (0.1Hz Doppler) show low temporal_cv (~0.03-0.30)
- Moderate channels (0.5Hz Doppler) show high temporal_cv (~0.40-0.55)
- **Trailing silence bug found and fixed**: `demodulateSoft()` was demodulating ~9 silence symbols
  at end of long frames (131 symbols, only 122 valid). This caused temporal_cv=0.27 on AWGN.
  Now detects and excludes trailing silence using energy-based threshold (20% of reference).
- Calibrated combined fading index values:
  - AWGN: ~0.04 (freq_cv ~0.003, temporal_cv ~0.032)
  - Good fading: ~0.62 (freq_cv ~0.32, temporal_cv ~0.30)
  - Moderate fading: ~0.90 (freq_cv ~0.42, temporal_cv ~0.49)
  - Poor fading: ~0.82 (freq_cv ~0.33, temporal_cv ~0.49)
- All `isFading()` thresholds updated from 0.4 to 0.75 across codebase
- Waveform selection thresholds recalibrated: AWGN < 0.15, Good < 0.75, Moderate < 1.10
- OFDM internal fading thresholds (LLR scaling, two-pass) NOT changed — they use
  pilot-variance-based `last_fading_index` on a separate scale

**Fading channel notes (2026-03-15):**
- Fading index now combines freq_cv + temporal_cv for better Good vs Moderate separation
- OFDM internal uses separate `last_fading_index` from pilot variance (~0.15-0.50)
- LLR scaling (1 + 10×fading²) applied when OFDM fading_index > 0.15 to prevent overconfident wrong bits
- Two-pass DQPSK decoding enabled for fading channels (threshold=0.12)
- Two-pass uses `last_fading_index` from pilot variance (NOT computeFadingIndex() which returns 0 after sync)
- Light sync confidence threshold=0.8 (raised from 0.5) to reject marginal timing syncs on fading channels
- CFO drift limited to ±1 Hz per frame when connected (multipath can cause false CFO readings)
- **CFO feedback loop** (2026-02-03): Pilot-corrected CFO propagates back to StreamingDecoder's cached value
- **LTS residual CFO** (2026-02-03): Detects and corrects chirp CFO errors >0.3 Hz from training symbols
- **CPE correction for differential modes** (2026-03-15): Per-symbol common phase error tracking
  now enabled for DQPSK/D8PSK (was coherent-only). Estimates phase drift from pilots each symbol,
  clamps to ±15° for differential (prevents overcorrection from noisy fading pilots).
  This keeps channel_estimate phase tracking the actual channel, improving MMSE equalization.
  Safe for DQPSK: CPE cancels in diff = eq[n] × conj(eq[n-1]) since both use same corrected H.
- Good fading: **100% CW success** at R2/3 SNR=15 (10/10 seeds, enabled by CPE correction)
- Moderate fading: R1/4 avg 1.4 retx (5/5 seeds), R1/2 avg 2.4 retx (5/5 seeds) — up from ~89% CW

**OTFS Status (2026-01-31) - EXPERIMENTAL:**
- **AWGN:** 100% at SNR=20 with QPSK R1/2 - WORKS
- **Good fading:** ~52% average (vs OFDM DQPSK ~56%) - NOT COMPETITIVE
- **Integration:** Fully integrated with chirp sync, IWaveform interface, test_iwaveform
- **Files:** `src/otfs/otfs.cpp`, `src/waveform/otfs_waveform.cpp`, `include/ultra/otfs.hpp`

**What we tried:**
1. TF-domain equalization (preamble-based) - insufficient for fading
2. DD-domain differential encoding - doesn't help (adjacent DD symbols don't see similar channels)
3. DD-domain pilot with matched-filter equalization - marginal improvement (~52% vs ~28% baseline)
4. Combined TF + DD equalization - still not competitive with OFDM DQPSK

**Why OTFS doesn't outperform OFDM in our implementation:**
- OTFS's theoretical advantage requires DD-domain MMSE equalization exploiting channel sparsity
- Our simplified approaches (TF eq, single-tap DD eq, matched-filter) are insufficient
- Proper DD-domain equalization is research-level complexity (sparse channel estimation + iterative detection)

**Recommendation:** Use OFDM_CHIRP with DQPSK. Rate selection is automatic via `selectOFDMCodeRate()`:
- R3/4 for AWGN only (SNR≥20, fading<0.15) — ~3.4× throughput vs R1/4
- R2/3 for good fading or better (SNR≥15) — ~2.8× throughput vs R1/4
- R1/2 for good/moderate fading (SNR≥15) — ~2× throughput vs R1/4
- R1/4 for heavy fading or lower SNR — robust but slower
OTFS is parked - would need significant research effort to implement proper DD-domain equalization.

**FFTW requirement:**
- FFTW3 is REQUIRED for fast chirp detection (apt install libfftw3-dev)
- Without FFTW: Cooley-Tukey fallback takes ~1 second per correlation (unusable)
- With FFTW: Detection takes ~0.5s after frame TX ends (correct)
- FFTW planner is NOT thread-safe - protected by global mutex in fft.cpp

**StreamingDecoder design:**
- Expects audio fed at real-time rate (or close to it)
- Has overflow protection if audio fed faster than processing
- Mode switches reset correlation_pos_ to start searching new data

**Pending Improvements (2026-02-03):**
- **CFO Pre-Correction for LTS Sync:** Light preamble LTS sync detection still happens BEFORE CFO
  correction in StreamingDecoder. The LTS residual fix (in demodulator) corrects CFO after sync,
  but pre-correcting audio samples in StreamingDecoder would improve LTS detection reliability
  on fading+CFO channels. Cost: O(N) complex multiply per sample - very cheap.
- **Per-Symbol Pilot Tracking:** IMPLEMENTED and active in `channel_equalizer.cpp:424-694`.
  Every data symbol updates H via LS pilot estimation, alpha-smoothed tracking (alpha=0.8
  for differential w/ pilots), residual CFO tracking, and timing recovery from pilot phase slope.
  Pilots spaced every 10 carriers (~6 pilots across 59 carriers). R1/2 still struggles on
  moderate fading due to insufficient LDPC redundancy, not missing pilot tracking.

---

## Important Rules

- **No guessing.** If you don't know an answer, say "I don't know" or "I need to verify X" — don't fabricate. Read the code, run the test, or ask the user. Be direct. Short answers > long ones with hedging. Length is not a proxy for quality.

- **Never mention specific competing products by name** (e.g., no "VARA", "ARDOP", "Winlink" etc.). Always refer to "industry leaders", "commercial HF modems", or "existing systems" instead.

- **MANDATORY: Document ALL fixes and changes** in `docs/CHANGELOG.md` with what was broken, what changed, how it's fixed, and test verification.

- **Track bugs in KNOWN_BUGS.md.** Add with unique ID (BUG-XXX). When fixed, move to "Fixed Bugs" section.

- **Read INVARIANTS.md before changing critical code.** Violating these causes subtle bugs.

- **Run regression test before committing:** `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test 2>&1 | tee /tmp/test_output.log`

---

## Essential Documentation (16 files)

### Priority 1: Always Read First
| Document | Purpose |
|----------|---------|
| `docs/REFACTOR_PROGRESS.md` | Current state, what's done, what's blocked |
| `docs/KNOWN_BUGS.md` | Active bugs - DON'T rediscover these |
| `docs/INVARIANTS.md` | 25 critical rules that MUST NOT be violated |
| `docs/CHANGELOG.md` | History of all fixes - DON'T redo this work |

### Priority 2: Read When Working on Subsystem
| Document | Purpose |
|----------|---------|
| `docs/STREAMING_DECODER_REDESIGN.md` | **ACTIVE** - Fix for BUG-005, continuous audio |
| `docs/MODEM_ENGINE_ARCHITECTURE.md` | Thread model, TX/RX paths, state machine |
| `docs/DUAL_CHIRP_CFO_ANALYSIS.md` | CFO detection, position handling, IWaveform |
| `docs/CFO_CORRECTION_FLOW.md` | **CRITICAL** - 4-stage CFO flow, fading fix, feedback loop |
| `docs/TESTING_METHODOLOGY.md` | Test tools, CFO simulation, streaming RX |
| `docs/PROTOCOL_V2.md` | Frame formats, protocol flow |
| `docs/GUI_ARCHITECTURE.md` | ImGui widgets, threading, virtual station |
| `docs/AUDIO_SYSTEM.md` | SDL2 audio I/O, buffers, latency |
| `docs/CONFIGURATION_SYSTEM.md` | AppSettings, ModemConfig, presets |

### Priority 3: Reference
| Document | Purpose |
|----------|---------|
| `docs/BUILD_SYSTEM.md` | CMake, dependencies, adding components |
| `docs/ADDING_NEW_WAVEFORM.md` | Step-by-step guide for OTFS, AFDM, etc. |
| `docs/GIT_WORKFLOW.md` | Commit strategy, branching, push policy |
| `docs/RESEARCH_DIRECTIONS.md` | Long-term research goals, novel techniques |

---

## Quick Reference

### Build
```bash
mkdir build && cd build
cmake ..
make -j4
```

### GUI Application
```bash
./ultra_gui              # Normal mode
./ultra_gui -sim         # Developer mode with virtual station
./ultra_gui -sim -rec    # With audio recording
```

### Test Tools
| Tool | Purpose | Example |
|------|---------|---------|
| `cli_simulator` | **PRIMARY** - Full protocol with two-station interaction | `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test` |
| `test_waveform_simple` | Quick single-frame sanity check | `./build/test_waveform_simple -w ofdm_chirp --snr 15` |

**Testing priority:** Use `cli_simulator` for all real testing (handshake, light preamble, data transfer, ARQ). Use `test_waveform_simple` only for quick single-frame sanity checks. `regression_matrix.sh` is outdated and will not run.

### CLI Commands
```bash
# Send PING probe
./ultra ptx ping -s MYCALL | aplay -f FLOAT_LE -r 48000

# Send message
./ultra ptx "Hello World" -s MYCALL -d THEIRCALL -o msg.f32

# Decode recording
./ultra prx recording.f32           # OFDM
./ultra prx -w dpsk recording.f32   # DPSK/PING
```

---

## Waveform Summary

| Mode | Sync Method | SNR Range | Max Throughput | CFO Tolerance | Fading |
|------|-------------|-----------|----------------|---------------|--------|
| **MC-DPSK** | Dual Chirp | -3 to 10 dB | 938 bps | ±50 Hz | Good |
| **OFDM_NARROW** | NB Chirp + LTS | 5-10 dB | ~230 bps | ±50 Hz | Good (R1/4) |
| **OFDM_CHIRP** | Dual Chirp + LTS | 10-17 dB | 3.4 kbps | ±50 Hz | Good (R1/4) |
| **OFDM_COX** | Schmidl-Cox | 17+ dB | 7.9 kbps | Needs testing | Poor |
| **OTFS** | Dual Chirp | 15+ dB | ~2 kbps | ±50 Hz | **Poor** (experimental) |
| **SC-DPSK** | Barker-13 | -8 to -3 dB | 125 bps | N/A | Good |

**Waveform Selection:**
- Poor HF channels (2ms delay): Use MC-DPSK
- Low SNR (5-10 dB) with good/moderate fading: Use OFDM_NARROW (~103 bps R1/4, ~230 bps R1/2)
- Moderate/Good HF: Use OFDM_CHIRP (10-17 dB) or OFDM_COX (17+ dB)
- Very low SNR: Use SC-DPSK or MC-DPSK
- **DO NOT use OTFS on fading channels** - it's experimental and underperforms OFDM_CHIRP

---

## Architecture Overview

```
src/
├── gui/modem/          # ModemEngine - TX/RX audio processing
├── ofdm/               # OFDM modulator/demodulator
├── psk/                # Single/Multi-carrier DPSK
├── fec/                # LDPC encoder/decoder (648-bit codewords)
├── protocol/           # Protocol v2 (PING/CONNECT/DATA/DISCONNECT)
├── sync/               # ChirpSync, Schmidl-Cox sync
└── waveform/           # IWaveform interface and implementations

tools/
├── test_iwaveform.cpp  # PRIMARY test tool
├── cli_simulator.cpp   # Full protocol test
└── test_hf_modem.cpp   # LEGACY - reference only
```

**Key Files:**
- `src/sync/chirp_sync.hpp` - Dual chirp detection + CFO estimation
- `src/gui/modem/modem_rx_decode.cpp` - RX decode logic
- `src/psk/multi_carrier_dpsk.hpp` - MC-DPSK modulator/demodulator
- `src/otfs/otfs.cpp` - OTFS modulator/demodulator (experimental)
- `src/waveform/otfs_waveform.cpp` - OTFS IWaveform wrapper with chirp sync

---

## Key Specifications

| Parameter | Standard Mode | NVIS Mode |
|-----------|---------------|-----------|
| Sample Rate | 48,000 Hz | 48,000 Hz |
| Center Frequency | 1,500 Hz | 1,500 Hz |
| Bandwidth | ~2.8 kHz | ~2.8 kHz |
| FFT Size | 512 | 1024 |
| Carriers | 30 | 59 |
| Max Throughput | 3.4 kbps | 7.2 kbps |

---

## Known Limitations

1. **OFDM_COX CFO:** Needs verification at 17+ dB with Schmidl-Cox
2. **Poor HF channels (2ms delay):** OFDM fails - use MC-DPSK instead
3. **MC-DPSK floor:** -5 dB is hard floor (20-40% success)
4. **File transfer:** DATA_START/DATA_END not fully implemented

---

## Protocol v2 Flow

```
Station A                          Station B
---------                          ---------
1. PING (1s) -------------------->
   <------------------------ PONG (1s)

2. CONNECT (DPSK) --------------->
   <------------------ CONNECT_ACK (DPSK)

3. MODE_CHANGE ------------------>  (SNR-based negotiation)
   <--------------------------- ACK

4. DATA ------------------------->  (negotiated waveform)
   <--------------------------- ACK

5. DISCONNECT ------------------->
   <--------------------------- ACK
```

---

## Development Workflow

### Before Making Changes
1. Read `docs/INVARIANTS.md` for the subsystem you're touching
2. Check `docs/KNOWN_BUGS.md` for related issues
3. Check `docs/REFACTOR_PROGRESS.md` for current status

### After Making Changes
1. Run `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test 2>&1 | tee /tmp/test_output.log`
2. If you fixed a bug: Add entry to `docs/CHANGELOG.md`
3. If you discovered a bug: Add entry to `docs/KNOWN_BUGS.md`
4. If you completed a refactor task: Update `docs/REFACTOR_PROGRESS.md`

### Commit Message Format
```
Short description (imperative mood)

- What was changed
- Why it was changed

Fixes: BUG-XXX (if applicable)
```
