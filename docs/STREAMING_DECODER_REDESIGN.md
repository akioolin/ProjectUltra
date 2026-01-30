# Streaming Decoder Redesign Plan

## Status: IN PROGRESS (2026-01-30)

## Problem Summary

The CLI simulator's continuous audio model is conceptually correct, but the **StreamingDecoder** uses a batch-search architecture that doesn't match how real receivers work. This causes:

1. **3-second latency** before first sync search (MIN_SAMPLES_FOR_SEARCH = 144000)
2. **Buffer position misalignment** - by the time search happens, positions have drifted
3. **Decode failures** - chirp is detected but data demodulation reads noise instead of signal

### Evidence of the Bug

From test logs:
```
[MC-DPSK] process: samples=25600, training=4096, ref=512
[MC-DPSK] RMS: training[0]=0.005609, ref[4096]=0.005656, data[4608]=0.005596
```

The RMS is **0.005** (noise level) when it should be **0.28** (signal level). The `process()` function receives noise instead of the actual training+ref+data samples because buffer positions are wrong.

---

## Current Architecture (WRONG)

```
Audio Thread                          Decode Thread
───────────                          ─────────────
Every 10ms:                          Loop:
1. Read 480 RX samples               1. Wait for 144000 samples (3 SEC!)
2. feedAudio() → circular buffer     2. Copy to work_buffer
3. Pop 480 TX samples                3. Search for chirp
4. Send to channel                   4. If found, try to decode
                                     5. Advance search_pos by 4800
                                     6. Repeat

PROBLEMS:
- 3 second wait before first search
- search_pos advances independently of audio arrival
- By time we search, signal may be at wrong position in buffer
- Complex state: search_pos, read_pos, write_pos, pending_frame_, etc.
```

### Key Files

1. `src/gui/modem/streaming_decoder.cpp` - Main decoder logic
2. `src/gui/modem/streaming_decoder.hpp` - Constants and state
3. `tools/cli_simulator.cpp` - Test harness (audio model is correct)
4. `src/waveform/mc_dpsk_waveform.cpp` - Waveform detectSync/process

### Critical Constants (streaming_decoder.hpp:279-285)

```cpp
static constexpr size_t MAX_BUFFER_SAMPLES = 960000;    // 20 seconds
static constexpr size_t MIN_SAMPLES_FOR_SEARCH = 144000; // 3 seconds - THE PROBLEM
static constexpr size_t SLIDE_STEP = 4800;              // 100ms between searches
static constexpr size_t CHIRP_SAMPLES = 53000;          // ~1.1 second
```

---

## Correct Architecture (Real Receiver)

```
Audio Thread (Single Callback)
──────────────────────────────
Every 10ms (480 samples):
1. Read RX samples from channel
2. Write to small ring buffer
3. Run correlation on NEW samples (continuous!)
4. If correlation > threshold:
   → SYNC DETECTED at exact position
   → Transition to DECODE state
5. If in DECODE state and have enough samples:
   → Decode frame immediately
6. Send TX samples to channel

KEY DIFFERENCES:
- Correlation runs on EVERY callback, not after 3 seconds
- Sync detected within 10ms of chirp arriving
- No complex buffer position tracking
- Simple state machine: SEARCHING → SYNCED → DECODING → SEARCHING
```

### Minimum Buffer Size Actually Needed

```
Chirp (dual):     ~57,600 samples (1.2 sec)
Training:          4,096 samples
Reference:           512 samples
Data (1 CW):      ~21,000 samples
────────────────────────────────────────────
Total:            ~83,208 samples (1.7 seconds)

Recommended:      ~120,000 samples (2.5 seconds) with margin
```

---

## Implementation Plan

### Phase 1: Simplify StreamingDecoder State Machine

**File: `src/gui/modem/streaming_decoder.hpp`**

Change from complex buffer management to simple state machine:

```cpp
enum class State {
    SEARCHING,      // Running correlation on incoming samples
    SYNC_FOUND,     // Chirp detected, collecting frame samples
    DECODING,       // Have enough samples, decoding in progress
};

// Remove these complex tracking variables:
// - search_pos_, read_pos_ (keep only write_pos_)
// - pending_frame_, pending_search_pos_, pending_data_start_, etc.
// - mode_switch_write_pos_

// Add simple state:
State state_ = State::SEARCHING;
size_t sync_position_ = 0;        // Where chirp was detected
size_t samples_since_sync_ = 0;   // How many samples collected since sync
float sync_cfo_ = 0.0f;           // CFO from sync detection
```

### Phase 2: Continuous Correlation in feedAudio()

**File: `src/gui/modem/streaming_decoder.cpp`**

Move correlation from `processBuffer()` into `feedAudio()`:

```cpp
void StreamingDecoder::feedAudio(const float* samples, size_t count) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    // Write samples to circular buffer
    for (size_t i = 0; i < count; i++) {
        buffer_[write_pos_] = samples[i];
        write_pos_ = (write_pos_ + 1) % MAX_BUFFER_SAMPLES;
    }

    switch (state_) {
        case State::SEARCHING:
            // Run correlation on new samples
            runCorrelationSearch(count);
            break;

        case State::SYNC_FOUND:
            // Accumulate samples for frame
            samples_since_sync_ += count;
            if (samples_since_sync_ >= min_frame_samples_) {
                state_ = State::DECODING;
                data_cv_.notify_one();  // Wake decode thread
            }
            break;

        case State::DECODING:
            // Decode thread is processing, just buffer samples
            break;
    }
}

void StreamingDecoder::runCorrelationSearch(size_t new_samples) {
    // Run matched filter on the new samples
    // Correlation slides naturally as we add samples

    size_t search_start = (write_pos_ - new_samples - chirp_len) % MAX_BUFFER_SAMPLES;

    for (size_t i = 0; i < new_samples; i += CORRELATION_STEP) {
        size_t pos = (search_start + i) % MAX_BUFFER_SAMPLES;

        SyncResult result;
        if (waveform_->detectSync(getSamplesAt(pos, chirp_len), result, 0.15f)) {
            // SYNC DETECTED!
            sync_position_ = pos;
            sync_cfo_ = result.cfo_hz;
            samples_since_sync_ = 0;
            state_ = State::SYNC_FOUND;

            LOG_MODEM(INFO, "Sync detected at position %zu, CFO=%.1f Hz", pos, sync_cfo_);
            return;
        }
    }
}
```

### Phase 3: Simplify processBuffer()

**File: `src/gui/modem/streaming_decoder.cpp`**

The decode thread only wakes up when we have a complete frame:

```cpp
void StreamingDecoder::processBuffer() {
    std::unique_lock<std::mutex> lock(buffer_mutex_);

    // Wait for DECODING state (means we have sync + enough samples)
    data_cv_.wait(lock, [this] {
        return state_ == State::DECODING || shutdown_.load();
    });

    if (shutdown_.load()) return;

    // Extract frame samples starting from sync_position_ + training_offset
    size_t frame_start = (sync_position_ + training_offset_) % MAX_BUFFER_SAMPLES;
    auto frame_samples = extractSamples(frame_start, min_frame_samples_);

    lock.unlock();  // Release lock for heavy processing

    // Decode the frame
    decodeFrame(frame_samples);

    // Return to searching
    lock.lock();
    state_ = State::SEARCHING;
}
```

### Phase 4: Update Constants

**File: `src/gui/modem/streaming_decoder.hpp`**

```cpp
// Reduced buffer - only need 2.5 seconds for worst case
static constexpr size_t MAX_BUFFER_SAMPLES = 120000;   // 2.5 seconds

// Remove MIN_SAMPLES_FOR_SEARCH - not needed with continuous correlation
// static constexpr size_t MIN_SAMPLES_FOR_SEARCH = 144000;  // DELETE

// Correlation step - check every 10ms worth of samples
static constexpr size_t CORRELATION_STEP = 480;  // 10ms at 48kHz
```

### Phase 5: Update cli_simulator.cpp

The cli_simulator audio model is already correct (single audio I/O thread per station). No changes needed there.

---

## Testing Plan

### Test 1: Basic PING Detection
```bash
./build/cli_simulator --snr 25 2>&1 | grep -E "Sync|PING|PONG"
```
Expected: PING detected within ~1.5 seconds (chirp duration), not 4+ seconds

### Test 2: Full Connection
```bash
./build/cli_simulator --snr 25
```
Expected: All 4 phases pass (CONNECT, MODE NEGOTIATION, DATA, DISCONNECT)

### Test 3: Verify Timing
Add timing logs to measure:
- Time from TX start to sync detection
- Time from sync to frame decode complete

Expected latency: ~1.5 seconds (chirp) + ~0.5 seconds (data) = ~2 seconds total

### Test 4: Regression
```bash
./tests/regression_matrix.sh
```
All existing tests must still pass.

---

## Files to Modify

1. `src/gui/modem/streaming_decoder.hpp`
   - Simplify state machine
   - Remove complex position tracking
   - Update constants

2. `src/gui/modem/streaming_decoder.cpp`
   - Move correlation to feedAudio()
   - Simplify processBuffer()
   - Remove pending frame handling

3. `tools/cli_simulator.cpp`
   - No changes needed (audio model is correct)

---

## Rollback Plan

If the redesign causes issues:
1. Git stash or branch the changes
2. Revert to current implementation
3. The current code "works" for test_iwaveform (batch file injection)
4. Issue is specific to continuous audio (cli_simulator, gui_simulator)

---

## Notes for Context Recovery

If you're reading this after context compaction:

1. **The CLI simulator audio model is CORRECT** - don't change it
2. **The bug is in StreamingDecoder** - batch search vs continuous correlation
3. **Key symptom**: `[MC-DPSK] RMS: training[0]=0.005` shows noise, not signal
4. **Root cause**: Buffer position drift due to 3-second batch search delay
5. **Solution**: Continuous correlation in feedAudio(), not batch search in processBuffer()

Read this file first, then:
- `src/gui/modem/streaming_decoder.cpp` - current implementation
- `tools/cli_simulator.cpp` - test harness (audio model is fine)
