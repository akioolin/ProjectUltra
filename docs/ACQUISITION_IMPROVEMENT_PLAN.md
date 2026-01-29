# Smart Acquisition System Plan

## Date: 2026-01-29

## Problem Statement

The current acquisition system uses a fixed `MIN_SAMPLES_FOR_SEARCH` threshold that cannot serve all use cases:
- GUI with real audio needs longer buffer (noise before signal)
- CLI simulator needs shorter buffer (fast ACK response)
- Different waveforms need different approaches

### Root Cause

When searching through noise before the real signal arrives:
1. Correlation is very low (0.02-0.03) - pure noise
2. We still advance `search_pos` by SLIDE_STEP (4800 samples)
3. After several iterations, `search_pos` moves past the real signal
4. When PING arrives, we've already advanced past it

Console evidence:
```
[CHIRP] FAIL: buf=72704, max_corr=0.027 at pos=39696 (threshold=0.15)
[CHIRP] FAIL: buf=81216, max_corr=0.028 at pos=48576 (threshold=0.15)
```
Correlation 0.027 = pure noise, but we kept advancing.

## Solution: Smart Acquisition Pipeline

```
┌────────────────────────────────────────────────────────────────┐
│                     ACQUISITION PIPELINE                        │
├────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐         │
│  │   ENERGY    │───>│   CHIRP     │───>│    FINE     │         │
│  │    GATE     │    │  PRE-CHECK  │    │    SYNC     │         │
│  └─────────────┘    └─────────────┘    └─────────────┘         │
│        │                  │                   │                 │
│        v                  v                   v                 │
│   rms < 2x noise?    No chirp energy?    correlation?          │
│   ───────────────    ────────────────    ─────────────         │
│   YES: skip search   YES: don't advance  < 0.05: noise         │
│   NO: continue       NO: continue        0.05-0.12: slow adv   │
│                                          0.12-0.15: min adv    │
│                                          >= 0.15: FOUND!       │
│                                                                 │
└────────────────────────────────────────────────────────────────┘
```

## Implementation Details

### 1. Energy Gate

Before expensive correlation, check if there's actual signal:

```cpp
// Compute RMS of search window
float computeSearchRMS(const std::vector<float>& buffer) {
    float sum_sq = 0.0f;
    for (float s : buffer) sum_sq += s * s;
    return std::sqrt(sum_sq / buffer.size());
}

// In detectAndDecode():
float rms = computeSearchRMS(work_buffer);
if (rms < noise_floor_ * 2.0f) {
    // Silence/noise - skip search entirely, don't advance
    return false;
}
```

### 2. Adaptive Advancement

Based on correlation quality, advance search_pos differently:

| Correlation | Meaning | Advancement |
|-------------|---------|-------------|
| < 0.05 | Pure noise | 0 (stay put) |
| 0.05 - 0.10 | Weak/uncertain | SLIDE_STEP/4 (1200 = 25ms) |
| 0.10 - 0.15 | Close to signal | SLIDE_STEP/8 (600 = 12ms) |
| >= 0.15 | Found! | N/A - detected |

```cpp
if (!sync_found) {
    float corr = sync_result.correlation;
    size_t advance = 0;

    if (corr < 0.05f) {
        advance = 0;              // Pure noise - stay put
    } else if (corr < 0.10f) {
        advance = SLIDE_STEP / 4; // 1200 samples = 25ms
    } else {
        advance = SLIDE_STEP / 8; // 600 samples = 12ms (we're close!)
    }

    if (advance > 0) {
        search_pos_ = (search_pos_ + advance) % MAX_BUFFER_SAMPLES;
    }
}
```

### 3. Mode-Aware Minimum Samples

```cpp
size_t getMinSamplesForMode() const {
    if (mode_ == WaveformMode::OFDM_COX || mode_ == WaveformMode::OFDM_CHIRP) {
        return 15000;   // Schmidl-Cox/LTS needs less
    } else if (connected_) {
        return 60000;   // Connected MC-DPSK: faster response for ACKs
    } else {
        return 72000;   // Disconnected MC-DPSK: reliable chirp detection
    }
}
```

### 4. Constants

```cpp
// Adaptive advancement thresholds
static constexpr float CORR_NOISE_THRESHOLD = 0.05f;     // Below = pure noise
static constexpr float CORR_WEAK_THRESHOLD = 0.10f;      // Below = weak/uncertain
static constexpr float CORR_DETECT_THRESHOLD = 0.15f;    // At/above = detected

// Energy gate
static constexpr float ENERGY_GATE_MULTIPLIER = 2.0f;    // rms > noise * this to search

// Minimum samples by mode
static constexpr size_t MIN_SAMPLES_DISCONNECTED = 72000; // 1.5 sec
static constexpr size_t MIN_SAMPLES_CONNECTED = 60000;    // 1.25 sec
static constexpr size_t MIN_SAMPLES_OFDM = 15000;         // 0.3 sec
```

## Expected Results

| Scenario | Before | After |
|----------|--------|-------|
| GUI noise before PING | Searches noise, advances past PING | Energy gate skips, waits for signal |
| CLI ACK reception | Timeout with 144000 | Works with 60000 (connected mode) |
| Weak signal (corr=0.13) | Advances 4800, might miss | Advances 600, keeps searching nearby |
| Pure noise (corr=0.02) | Advances 4800 | Doesn't advance |
| OFDM data transfer | Fast (15000) | Same |

## Files Modified

1. `src/gui/modem/streaming_decoder.hpp` - Add constants
2. `src/gui/modem/streaming_decoder.cpp` - Implement energy gate and adaptive advancement

## Testing

1. `./build/cli_simulator --snr 10 --test` - All phases should pass
2. `./build/cli_simulator --snr 20 --test` - All phases should pass
3. GUI simulator - Should connect on first PING attempt (no retries needed)
4. Check logs for "Energy gate" and adaptive advancement messages

## Implementation Status: COMPLETE (2026-01-29)

### Changes Made

1. **streaming_decoder.hpp** - New constants:
   - `MIN_SAMPLES_DISCONNECTED = 72000` (1.5 sec)
   - `MIN_SAMPLES_CONNECTED = 60000` (1.25 sec)
   - `MIN_SAMPLES_OFDM = 15000` (0.3 sec)
   - `CORR_NOISE_THRESHOLD = 0.05f`
   - `CORR_WEAK_THRESHOLD = 0.10f`
   - `ENERGY_GATE_MULTIPLIER = 2.0f`

2. **streaming_decoder.cpp** - processBuffer():
   - Mode-aware min samples selection

3. **streaming_decoder.cpp** - detectAndDecode():
   - Mode-aware min samples
   - Energy gate (disconnected mode only, checks whole buffer)
   - Adaptive advancement based on correlation

### Key Insight

The energy gate must:
- Only apply in disconnected mode (connected expects frames)
- Check energy across the WHOLE search buffer, not just the start
- Because the frame might start later in the buffer (after silence)

### Test Results

- `cli_simulator --snr 10 --test`: All 4 phases pass
- `cli_simulator --snr 20 --test`: All 4 phases pass
