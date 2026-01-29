# ModemEngine Refactor Plan

**Date:** 2026-01-28
**Goal:** Clean up ModemEngine from 13+ modulator/demodulator pointers to a clean IWaveform-based architecture

---

## Current State (The Problem)

### Architecture Issues

1. **13+ Pointers** in ModemEngine:
   - 8 legacy modulator/demodulator pairs (OFDM, OTFS, DPSK, MC-DPSK)
   - 4 new IWaveform pointers (partially integrated)
   - 1 ChirpSync pointer
   - 1 RxPipeline pointer (buggy)

2. **4 Duplicate processRxBuffer_* Methods** (~670 lines):
   - `processRxBuffer_OFDM()` - 150 lines
   - `processRxBuffer_OTFS()` - 210 lines
   - `processRxBuffer_DPSK()` - 160 lines
   - `processRxBuffer_OFDM_CHIRP()` - 150 lines

3. **Dual Code Paths** - Old and new paths run simultaneously, fighting for buffers

4. **BUG-002: RxPipeline Broken** - Growing buffer + periodic search doesn't work
   - test_iwaveform uses sliding window (works)
   - RxPipeline uses growing buffer (fails)

---

## Key Protocol Insight (Simplifies Design)

**Frame type is determined by connection state, NOT by detecting preamble type:**

| State | RX Waveform | Why |
|-------|-------------|-----|
| Disconnected | MC-DPSK R1/4 | PING/CONNECT always use this |
| Connected | Negotiated mode | From CONNECT_ACK bitfield |

**This means:**
- No need to "detect" if a chirp is MC-DPSK vs OFDM_CHIRP
- Acquisition thread (disconnected) → always decode as MC-DPSK
- Connected mode → use `waveform_mode_` from negotiation
- BUG-003 is NOT a real bug - just use connection state

---

## Target Architecture

**2-Thread Model (down from 3):**

```
┌─────────────────────────────────────────────────────────────┐
│ AUDIO THREAD                                                │
│   feedAudio() → streaming_decoder_->feedAudio()             │
│   (No more rx_sample_buffer_, no acquisition thread)        │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│ RX DECODE THREAD                                            │
│   while (streaming_decoder_->hasFrame()) {                  │
│       auto frame = streaming_decoder_->getFrame();          │
│       deliverFrame(frame);  // PING callback or data queue  │
│   }                                                         │
└─────────────────────────────────────────────────────────────┘
```

**ModemEngine after refactor:**

```cpp
class ModemEngine {
private:
    // Waveform creation
    WaveformFactory waveform_factory_;

    // RX: Streaming decoder (handles BOTH disconnected + connected)
    std::unique_ptr<StreamingDecoder> rx_decoder_;

    // TX waveform (created on mode change)
    std::unique_ptr<IWaveform> tx_waveform_;

    // State
    WaveformMode current_mode_;
    bool connected_;
};
```

**Benefits:**
- ~4 pointers instead of 13+
- 2 threads instead of 3
- Single RX path for all states (disconnected + connected)
- No more rx_sample_buffer_ management in ModemEngine
- Waveform determined by state, not detection
- Add new waveforms without touching ModemEngine

**What moves INTO StreamingDecoder:**
- ChirpSync (for preamble detection)
- Active RX waveform (IWaveform*)
- Sliding window buffer management
- Frame queue (replaces detected_frame_queue_)

---

## Phased Approach

### Phase 1: Create StreamingDecoder (Fix BUG-002 + Remove Acquisition Thread)

**Problem:**
- RxPipeline uses growing buffer + periodic search (fails)
- Acquisition thread is separate complexity (3 threads)
- Buffer management split between ModemEngine and RxPipeline

**Solution:** Create StreamingDecoder that handles EVERYTHING:
- Chirp detection (replaces acquisition thread)
- Frame decoding (replaces rxDecodeDPSK + processRxBuffer_*)
- Buffer management (replaces rx_sample_buffer_)

```cpp
class StreamingDecoder {
public:
    // Called from AUDIO THREAD - must be fast (<1ms)!
    void feedAudio(const float* samples, size_t count);

    // Called from DECODE THREAD - does the heavy work
    void processBuffer();       // Chirp detection + decode (blocking)
    bool hasFrame() const;
    DecodeResult getFrame();

    // Called on mode change
    void setMode(WaveformMode mode, bool connected);

private:
    // Buffer (fixed size, circular) - written by audio, read by decode
    std::vector<float> buffer_;
    size_t write_pos_ = 0;
    size_t read_pos_ = 0;
    static constexpr size_t MAX_BUFFER = 192000;  // 4 seconds
    std::mutex buffer_mutex_;           // Protects buffer access
    std::condition_variable data_cv_;   // Signals new data available

    // Sync detection (used by decode thread only)
    std::unique_ptr<ChirpSync> chirp_sync_;

    // Active waveform (used by decode thread only)
    WaveformFactory waveform_factory_;
    std::unique_ptr<IWaveform> waveform_;
    WaveformMode mode_ = WaveformMode::DPSK;
    bool connected_ = false;

    // Decoded frame queue
    std::queue<DecodeResult> frame_queue_;
    std::mutex queue_mutex_;
};
```

**Thread safety design:**
- `feedAudio()` (audio thread): Lock buffer_mutex_, copy samples, signal data_cv_
- `processBuffer()` (decode thread): Wait on data_cv_, lock buffer_mutex_, copy out samples, unlock, do chirp+decode
- Heavy work (chirp correlation, LDPC) happens **outside** any lock

**Continuous operation safeguards:**
```cpp
void StreamingDecoder::feedAudio(const float* samples, size_t count) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    // Overflow protection: if buffer full, drop oldest samples
    size_t available = MAX_BUFFER - samplesInBuffer();
    if (count > available) {
        size_t drop = count - available;
        read_pos_ = (read_pos_ + drop) % MAX_BUFFER;  // Drop oldest
        LOG_WARN("Buffer overflow, dropped %zu samples", drop);
    }

    // Copy new samples to circular buffer
    // ... (wrap-around handling)

    data_cv_.notify_one();  // Wake decode thread
}

void StreamingDecoder::processBuffer() {
    std::unique_lock<std::mutex> lock(buffer_mutex_);

    // Wait for minimum samples (or timeout to check for shutdown)
    data_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
        return samplesInBuffer() >= MIN_SAMPLES_FOR_SEARCH || shutdown_;
    });

    if (shutdown_) return;
    if (samplesInBuffer() < MIN_SAMPLES_FOR_SEARCH) return;  // Timeout, no data

    // Copy samples out for processing (release lock during heavy work)
    std::vector<float> work_buffer = copyOutSamples(SEARCH_WINDOW);
    lock.unlock();

    // Heavy work outside lock: chirp detection + decode
    detectAndDecode(work_buffer);
}
```

**CPU safeguards:**
- Rate-limit chirp correlation: Only search every ~500ms of audio (SLIDE_STEP)
- On false positive: LDPC fails fast, move on
- Timeout on wait: Thread stays responsive for shutdown

**PING detection (chirp-only frames):**
```cpp
DecodeResult StreamingDecoder::detectAndDecode(SampleSpan samples) {
    auto sync = chirp_sync_->detectDualChirp(samples, 0.15f);
    if (!sync.success) return {};

    // Estimate SNR from chirp correlation strength
    float snr_db = estimateSNRFromChirp(sync.correlation, noise_floor_);
    last_snr_ = snr_db;

    // Check if data follows chirp or just PING (chirp-only)
    size_t chirp_end = sync.down_chirp_start + CHIRP_SAMPLES;
    float post_energy = measureEnergy(samples, chirp_end, chirp_end + 5000);
    float chirp_energy = measureEnergy(samples, sync.up_chirp_start, chirp_end);
    float ratio = (chirp_energy > 0.001f) ? (post_energy / chirp_energy) : 0.0f;

    if (ratio < 0.3f) {
        // PING - just chirp, no data after
        return DecodeResult{FrameType::PING, snr_db, {}, sync.cfo_hz};
    }

    // Data frame - continue to full decode
    waveform_->reset();
    waveform_->setFrequencyOffset(sync.cfo_hz);
    // ... decode logic ...
}
```

**SNR estimation (for mode negotiation):**
```cpp
float StreamingDecoder::estimateSNRFromChirp(float correlation, float noise_floor) {
    // Correlation strength maps to SNR
    // At SNR=0dB, correlation ~0.15-0.20
    // At SNR=10dB, correlation ~0.50-0.70
    // At SNR=20dB, correlation ~0.85-0.95
    float signal_power = correlation * correlation;
    float noise_power = noise_floor * noise_floor;
    if (noise_power < 1e-10f) noise_power = 1e-10f;
    return 10.0f * std::log10(signal_power / noise_power);
}

float StreamingDecoder::getLastSNR() const {
    return last_snr_;  // Used by ModemEngine for mode negotiation
}
```

**Graceful shutdown:**
```cpp
void StreamingDecoder::stop() {
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        shutdown_ = true;
    }
    data_cv_.notify_all();  // Wake decode thread so it can exit
}

void StreamingDecoder::processBuffer() {
    std::unique_lock<std::mutex> lock(buffer_mutex_);

    data_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
        return samplesInBuffer() >= MIN_SAMPLES || shutdown_;
    });

    if (shutdown_) return;  // Clean exit
    // ... continue processing ...
}
```

**Mode switch handling:**
```cpp
void StreamingDecoder::setMode(WaveformMode mode, bool connected) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    if (mode_ == mode && connected_ == connected) return;  // No change

    mode_ = mode;
    connected_ = connected;

    // Create new waveform for the mode
    waveform_ = waveform_factory_.create(mode);

    // Reset search position (don't lose buffered audio)
    // But clear any partial decode state
    search_pos_ = read_pos_;

    LOG_INFO("StreamingDecoder: Mode changed to %s (%s)",
             waveformModeToString(mode),
             connected ? "connected" : "disconnected");
}
```

**Noise floor tracking (for SNR estimation):**
```cpp
void StreamingDecoder::updateNoiseFloor(SampleSpan samples) {
    // Measure energy in regions without signal
    // Use exponential moving average for stability
    float energy = measureEnergy(samples, 0, samples.size());
    float alpha = 0.1f;  // Slow adaptation
    noise_floor_ = alpha * energy + (1.0f - alpha) * noise_floor_;
}
```

**Key design: Sliding window search**
```cpp
void StreamingDecoder::tryDecode() {
    while (samplesAvailable() >= MIN_FRAME_SAMPLES) {
        SampleSpan search_span = getSearchSpan(search_pos_);

        // 1. Detect chirp preamble
        auto sync = chirp_sync_->detectDualChirp(search_span, 0.15f);
        if (!sync.success) {
            search_pos_ += SLIDE_STEP;  // 24000 samples (500ms)
            trimOldSamples();
            continue;
        }

        // 2. Apply CFO and decode frame
        waveform_->reset();
        waveform_->setFrequencyOffset(sync.cfo_hz);

        SampleSpan frame_span = getFrameSpan(sync.start_sample);
        if (waveform_->process(frame_span)) {
            auto soft_bits = waveform_->getSoftBits();
            auto result = decodeFrame(soft_bits);

            std::lock_guard<std::mutex> lock(queue_mutex_);
            frame_queue_.push(result);
        }

        // 3. Advance past this frame
        search_pos_ += sync.start_sample + FRAME_SAMPLES;
        trimOldSamples();
    }
}
```

**Files:**
- Create `src/gui/modem/streaming_decoder.hpp` (~150 lines)
- Create `src/gui/modem/streaming_decoder.cpp` (~400 lines)

**Verification:**
```bash
./build/test_iwaveform --snr 15 -w ofdm_chirp --frames 5
```

---

### Phase 2: Integrate StreamingDecoder into ModemEngine

**Remove 3-thread model, replace with 2-thread model:**

1. **Audio thread (feedAudio):**
   ```cpp
   void ModemEngine::feedAudio(const float* samples, size_t count) {
       // OLD: Write to rx_sample_buffer_, route to rx_pipeline_ if connected
       // NEW: Always feed to StreamingDecoder
       rx_decoder_->feedAudio(samples, count);
   }
   ```

2. **Decode thread (replaces BOTH acquisitionLoop AND rxDecodeLoop):**
   ```cpp
   void ModemEngine::rxDecodeLoop() {
       while (rx_running_) {
           // OLD: Complex state machine with detected_frame_queue_, processRxBuffer_*, etc.
           // NEW: StreamingDecoder does all the heavy lifting

           // This blocks until data available, then does chirp detection + LDPC
           rx_decoder_->processBuffer();

           // Deliver any decoded frames
           while (rx_decoder_->hasFrame()) {
               auto result = rx_decoder_->getFrame();

               if (result.type == FrameType::PING) {
                   ping_received_callback_(result.snr);
               } else {
                   deliverFrame(result.data);
               }
           }
       }
   }
   ```

3. **Remove acquisitionLoop entirely:**
   ```cpp
   // DELETE: void ModemEngine::acquisitionLoop() { ... }
   // DELETE: std::thread acquisition_thread_;
   // DELETE: std::atomic<bool> acquisition_running_;
   ```

4. **Mode switching notifies StreamingDecoder:**
   ```cpp
   void ModemEngine::onConnected(WaveformMode negotiated_mode) {
       current_mode_ = negotiated_mode;
       rx_decoder_->setMode(negotiated_mode, /*connected=*/true);
   }

   void ModemEngine::onDisconnected() {
       current_mode_ = WaveformMode::DPSK;
       rx_decoder_->setMode(WaveformMode::DPSK, /*connected=*/false);
   }
   ```

**Files:**
- Modify `src/gui/modem/modem_rx.cpp` - remove acquisitionLoop, simplify feedAudio
- Modify `src/gui/modem/modem_engine.cpp` - remove acquisition thread start/stop
- Modify `src/gui/modem/modem_engine.hpp` - remove acquisition thread members
- Modify `src/gui/modem/modem_mode.cpp` - notify StreamingDecoder on mode change

**Verification:**
```bash
./build/cli_simulator --snr 20 --test  # Full protocol should work
```

---

### Phase 3: Remove Legacy Code

Once Phase 1-2 verified with regression tests:

1. **Remove processRxBuffer_* methods** (-670 lines):
   ```cpp
   // DELETE these from modem_rx_decode.cpp:
   void ModemEngine::processRxBuffer_OFDM() { ... }
   void ModemEngine::processRxBuffer_OTFS() { ... }
   void ModemEngine::processRxBuffer_DPSK() { ... }
   void ModemEngine::processRxBuffer_OFDM_CHIRP() { ... }
   ```

2. **Remove rxDecodeDPSK** (moved into StreamingDecoder):
   ```cpp
   // DELETE from modem_rx_decode.cpp:
   void ModemEngine::rxDecodeDPSK(const DetectedFrame& frame) { ... }
   ```

3. **Remove legacy pointers** from modem_engine.hpp:
   ```cpp
   // DELETE (RX demodulators - now in StreamingDecoder):
   std::unique_ptr<OFDMDemodulator> ofdm_demodulator_;
   std::unique_ptr<OTFSDemodulator> otfs_demodulator_;
   std::unique_ptr<DPSKDemodulator> dpsk_demodulator_;
   std::unique_ptr<MultiCarrierDPSKDemodulator> mc_dpsk_demodulator_;

   // DELETE (modulators replaced by IWaveform):
   std::unique_ptr<OFDMModulator> ofdm_modulator_;
   std::unique_ptr<OTFSModulator> otfs_modulator_;
   std::unique_ptr<DPSKModulator> dpsk_modulator_;
   // Keep mc_dpsk_modulator_ temporarily for TX (Phase 4 removes it)

   // DELETE (replaced by StreamingDecoder):
   std::unique_ptr<RxPipeline> rx_pipeline_;
   std::unique_ptr<ChirpSync> chirp_sync_;  // Moved into StreamingDecoder

   // DELETE (buffer management now in StreamingDecoder):
   std::vector<float> rx_sample_buffer_;
   std::mutex rx_buffer_mutex_;
   ThreadSafeQueue<DetectedFrame> detected_frame_queue_;
   ```

4. **Delete RxPipeline files:**
   - `src/gui/modem/rx_pipeline.hpp`
   - `src/gui/modem/rx_pipeline.cpp`

**Files:**
- Modify `src/gui/modem/modem_engine.hpp` - remove ~10 member variables
- Modify `src/gui/modem/modem_engine.cpp` - simplify constructor
- Modify `src/gui/modem/modem_rx_decode.cpp` - delete ~800 lines
- Delete `src/gui/modem/rx_pipeline.hpp` and `rx_pipeline.cpp`

---

### Phase 4: TX Path Unification

Use IWaveform for TX instead of direct modulator calls:

```cpp
// OLD (scattered if-else):
if (mode == MC_DPSK) {
    chirp_sync_->generate(chirp_samples);
    mc_dpsk_modulator_->generateTraining(...);
    mc_dpsk_modulator_->modulate(data);
} else if (mode == OFDM_CHIRP) {
    // Different complex logic
}

// NEW (unified):
tx_waveform_ = waveform_factory_.create(current_mode_);
auto samples = tx_waveform_->generatePreamble();
samples += tx_waveform_->modulate(encoded_data);
```

**Files:**
- Modify `src/gui/modem/modem_engine.cpp` - transmit() function
- Ensure all waveforms have working `generatePreamble()` and `modulate()`

---

### Phase 5: Verify OFDM_COX CFO

Currently untested through IWaveform interface:

1. Add OFDM_COX to test_iwaveform.cpp
2. Test with CFO = ±30, ±50 Hz at SNR = 20 dB
3. Fix any issues found

**Files:**
- Modify `tools/test_iwaveform.cpp` - add OFDM_COX support

---

### Phase 6: Add OTFSWaveform (Optional)

Wrap existing OTFS modulator/demodulator in IWaveform:

1. Create `src/waveform/otfs_waveform.hpp/cpp`
2. Implement IWaveform interface
3. Add to WaveformFactory
4. Test with test_iwaveform

---

## File Changes Summary

### New Files
| File | Lines | Purpose |
|------|-------|---------|
| `src/gui/modem/streaming_decoder.hpp` | ~150 | Header for unified RX decoder |
| `src/gui/modem/streaming_decoder.cpp` | ~400 | Chirp detection + decode + buffer mgmt |
| `src/waveform/otfs_waveform.hpp` | ~80 | OTFS wrapper (Phase 6) |
| `src/waveform/otfs_waveform.cpp` | ~200 | OTFS wrapper (Phase 6) |

### Modified Files
| File | Change |
|------|--------|
| `modem_engine.hpp` | Remove ~10 members (demodulators, buffers, threads) |
| `modem_engine.cpp` | Remove acquisition thread, simplify constructor |
| `modem_rx.cpp` | Remove acquisitionLoop(), simplify feedAudio(), simplify rxDecodeLoop() |
| `modem_rx_decode.cpp` | Delete processRxBuffer_* and rxDecodeDPSK (~800 lines removed) |
| `modem_mode.cpp` | Notify StreamingDecoder on mode change |
| `test_iwaveform.cpp` | Add OFDM_COX support |

### Deleted Files
| File | Reason |
|------|--------|
| `rx_pipeline.hpp` | Replaced by StreamingDecoder |
| `rx_pipeline.cpp` | Replaced by StreamingDecoder |

### Net Change
| Metric | Before | After | Delta |
|--------|--------|-------|-------|
| Threads | 3 | 2 | -1 |
| Member pointers | 13+ | ~4 | -9 |
| Lines in modem_rx_decode.cpp | ~1450 | ~650 | -800 |
| RX code paths | 2 (old + new) | 1 | -1 |

---

## Risk Mitigation

1. **Keep old code until verified:**
   - Add `#ifdef USE_STREAMING_DECODER` guards
   - Run both paths in parallel during testing
   - Only delete old code when 100% regression pass

2. **Test at each phase:**
   - Phase 1: `test_iwaveform -w ofdm_chirp`
   - Phase 2: `cli_simulator --test` (full protocol)
   - Phase 3: `regression_matrix.sh` (all waveforms)
   - Phase 4: TX round-trip tests
   - Phase 5-6: Extended regression

3. **Incremental commits:**
   - One commit per phase
   - Easy to revert if issues found

---

## Estimated Effort

| Phase | Effort | Risk |
|-------|--------|------|
| 1. StreamingDecoder | 2-3 days | High (core component) |
| 2. Integration | 1 day | Medium |
| 3. Remove Legacy | 0.5 day | Low (deletion) |
| 4. TX Unification | 0.5 day | Medium |
| 5. OFDM_COX CFO | 0.5 day | Low |
| 6. OTFSWaveform | 1 day | Low (optional) |
| Testing & Stress | 1 day | Low |

**Total: ~6-7 days** for complete refactor with testing (excl. Phase 6)

---

## StreamingDecoder Complete Feature List

Everything StreamingDecoder handles (moved from ModemEngine):

| Feature | Source | Implementation |
|---------|--------|----------------|
| Audio buffering | rx_sample_buffer_ | Circular buffer with overflow protection |
| Chirp detection | acquisitionLoop() | Sliding window search |
| CFO estimation | chirp_sync_ | From dual chirp gap |
| PING detection | modem_rx.cpp | Energy ratio after chirp |
| SNR estimation | acquisitionLoop() | From chirp correlation |
| Noise floor tracking | (new) | Exponential moving average |
| Frame decode | rxDecodeDPSK() | IWaveform + LDPC |
| Mode switching | modem_mode.cpp | setMode() with waveform factory |
| Graceful shutdown | (new) | shutdown_ flag + condition var |
| Thread safety | (improved) | Lock-free heavy work |

**What remains in ModemEngine:**
- TX path (transmit via IWaveform)
- Protocol state machine (connected_, handshake_complete_)
- Callbacks (ping_received, frame_received)
- Configuration (ModemConfig)
- Statistics aggregation

---

## GUI Integration

**Audio flow (unchanged):**
```
SDL2 callback → GUI audio buffer (waterfall/spectrum - unchanged)
              → ModemEngine::feedAudio() → StreamingDecoder::feedAudio()
```

**GUI accessors needed on StreamingDecoder:**
```cpp
// For SNR indicator (already added)
float getLastSNR() const { return last_snr_; }

// For CFO indicator (in DecodeResult)
float getLastCFO() const { return last_cfo_; }

// For buffer level indicator (NEW)
float getBufferFillPercent() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return static_cast<float>(samplesInBuffer()) / MAX_BUFFER * 100.0f;
}

// For decode statistics (NEW)
struct DecoderStats {
    uint64_t frames_decoded = 0;
    uint64_t frames_failed = 0;
    uint64_t pings_received = 0;
    uint64_t buffer_overflows = 0;
    float avg_decode_time_ms = 0.0f;
};
DecoderStats getStats() const { return stats_; }

// For waterfall chirp marker (NEW - nice to have)
struct SyncInfo {
    bool valid = false;
    size_t position = 0;      // Sample position in audio stream
    float correlation = 0.0f;  // Strength
    float cfo_hz = 0.0f;
};
SyncInfo getLastSync() const { return last_sync_; }
```

**GUI display elements:**

| Element | Data | Update Rate |
|---------|------|-------------|
| SNR meter | `getLastSNR()` | On frame decode |
| CFO display | `getLastCFO()` | On frame decode |
| Buffer bar | `getBufferFillPercent()` | 10 Hz |
| Stats panel | `getStats()` | 1 Hz |
| Chirp marker | `getLastSync()` | On detection |

**Thread safety for GUI:**
- All getters return copies (not references)
- GUI calls from main thread, StreamingDecoder runs in decode thread
- Atomic or mutex-protected where needed

**No changes needed to:**
- Waterfall display (uses separate GUI audio buffer)
- Spectrum analyzer (uses separate GUI audio buffer)
- TX controls (go through ModemEngine::transmit)
- Connection state display (from ModemEngine callbacks)

---

## Success Criteria

- [x] All regression tests pass (11/11 pass)
- [x] test_iwaveform works for MC-DPSK, OFDM_CHIRP, OFDM_COX
- [x] cli_simulator full protocol (PING→CONNECT→DATA→DISCONNECT) works ✅ 2026-01-28
- [ ] ModemEngine has ~4 pointers (down from 13+)
- [ ] 2 threads instead of 3
- [x] processRxBuffer_* methods removed (-800 lines)
- [x] RxPipeline deleted, replaced by StreamingDecoder
- [x] Adding new waveform = only implement IWaveform class
- [x] PING detection works (chirp-only frames)
- [x] SNR estimation works (for mode negotiation)
- [ ] Graceful shutdown (no hangs, no crashes)
- [ ] Continuous operation stable (1+ hour stress test)

---

## Testing Strategy

### Unit Tests (Phase 1)
```bash
# Test StreamingDecoder in isolation
./build/test_streaming_decoder --frames 10 --snr 5
```

### Integration Tests (Phase 2)
```bash
# Full protocol through ModemEngine
./build/cli_simulator --snr 20 --test

# Regression matrix
./tests/regression_matrix.sh
```

### Stress Tests (After Phase 3)
```bash
# Long-running continuous operation (1 hour)
./build/test_iwaveform --snr 5 -w mc_dpsk --frames 1000 --continuous

# Memory leak detection
valgrind --leak-check=full ./build/test_iwaveform --frames 100

# CPU profiling
perf record ./build/test_iwaveform --frames 100
perf report
```

### Real HF Tests (Final Validation)
```bash
# Connect to real rig, listen for 1 hour
./ultra_gui -sim  # With real audio input

# Verify:
# - No buffer overflows (check logs)
# - CPU usage stable (<20% idle, <50% during decode)
# - Memory stable (no growth over time)
# - PING detection works
# - Mode negotiation works
```

---

## Key Files Reference

**Current architecture (to understand):**
- `src/gui/modem/modem_engine.hpp:50-150` - all the pointers
- `src/gui/modem/modem_rx.cpp:38-146` - acquisitionLoop
- `src/gui/modem/modem_rx_decode.cpp:454-1218` - processRxBuffer_* methods

**Working reference (to copy pattern from):**
- `tools/test_iwaveform.cpp:579-700` - sliding window decode that works

**IWaveform interface (already done):**
- `src/waveform/waveform_interface.hpp` - the interface
- `src/waveform/mc_dpsk_waveform.cpp` - working implementation
- `src/waveform/ofdm_chirp_waveform.cpp` - working implementation
