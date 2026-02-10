# Burst-Level Long Interleaver for OFDM

## Context

Coherent QPSK R1/2 achieves ~80% frame success on good fading (0.1 Hz Doppler, SNR=20). DQPSK R1/2 is similar (~84%). The bottleneck: deep spectral nulls from multipath fading zero out groups of adjacent carriers, and frame interleaving only spreads bits within ONE frame (~0.7s). All 4 CWs in that frame see the same null → all fail together.

Commercial HF modems solve this with **long interleavers** (2-4 seconds), spreading coded bits across multiple frames so different frames see different channel realizations. A carrier nulled in frame 1 may be fine in frame 3. By spreading each CW's bits across 4 frames, a total frame loss means each CW only loses 25% of its bits — well within R1/2 LDPC correction capacity (~50% erasure tolerance).

The existing burst infrastructure (`encodeBurstLight()`, burst continuation in decoder, `Connection::flushBurstBuffer()`) already groups multiple frames into bursts. We add a **burst interleaver** that shuffles coded bytes across frames within a burst.

## Design Overview

### Interleaving Strategy: Byte-Level Row-Column Block Interleaver

For N frames, each with B=324 bytes (2592 bits after frame interleaving):

**TX (byte level):**
```
For logical frame f, byte b:
  flat_pos = N * b + f
  physical_frame = flat_pos / B
  physical_byte  = flat_pos % B
  physical[pf][pb] = logical[f][b]
```

Each physical frame gets bytes from ALL logical frames in round-robin: physical frame 0's bytes are `{L0[0], L1[0], L2[0], L3[0], L0[1], L1[1], ...}`. Each group of N consecutive bytes comes from N different logical frames — maximum time diversity.

**RX (soft-bit level, same permutation on byte groups of 8 floats):**
```
For logical frame f, byte b:
  flat_pos = N * b + f
  pf = flat_pos / B,  pb = flat_pos % B
  logical_soft[f][8*b .. 8*b+7] = physical_soft[pf][8*pb .. 8*pb+7]
```

Works for any N (2, 3, 4, ..., 8). No requirement for N to divide B.

### When Burst Interleaving Applies

- **ON:** Connected OFDM mode, data frame bursts with exactly N=4 frames
- **OFF:** Standalone control frames (ACK/NACK), bursts with ≠4 frames, MC-DPSK mode

Rule: **both TX and RX agree that exactly-4-frame bursts are burst-interleaved.** No protocol signaling needed — it's a hardcoded convention for V1.

- Window size set to 4 in connected OFDM mode (ensures bursts are at most 4 data frames)
- If message has <4 remaining fragments, burst has <4 frames → no interleaving
- Retransmissions are typically 1 frame → no interleaving

### Decoder Flow

Current: `demodulate frame 0 → decode → if success, burst continuation → decode each independently`

New:
```
demodulate frame 0 → try normal decode →
  if success (standalone control frame): done
  if fail AND burst_interleave enabled:
    demodulate 3 more continuation frames
    if got exactly 4 total: burst deinterleave → decode 4 logical frames
    if got <4: decode individually (non-interleaved burst)
```

The normal decode attempt handles standalone control frames (1 CW, not interleaved). For burst-interleaved data frames, CW0 probe will fail (scrambled bits), triggering the burst path.

### Lost Frame Handling

If one frame in a 4-frame burst is lost (energy drops), the decoder gets 3 frames and falls back to individual decode — all 3 produce garbage (TX used N=4 interleaving), so the entire burst fails. ARQ retransmits all 4 frames. This is acceptable because:
- Burst frames are back-to-back, all see similar energy levels
- Frame loss within a burst is very rare (energy detection is reliable)
- Simplicity outweighs the small efficiency loss in edge cases

## Files to Modify

### 1. NEW: `src/fec/burst_interleaver.hpp`

```cpp
#pragma once
#include <vector>
#include <cstdint>

namespace ultra { namespace fec {

class BurstInterleaver {
public:
    static constexpr int BYTES_PER_FRAME = 324;  // 4 CWs × 81 bytes
    static constexpr int BITS_PER_FRAME = 2592;  // 4 CWs × 648 bits

    // TX: interleave coded bytes across N frames
    static std::vector<std::vector<uint8_t>> interleave(
        const std::vector<std::vector<uint8_t>>& logical_frames);

    // RX: deinterleave soft bits back to logical frames
    static std::vector<std::vector<float>> deinterleave(
        const std::vector<std::vector<float>>& physical_soft);
};

}}
```

### 2. NEW: `src/fec/burst_interleaver.cpp`

Implement using the row-column permutation described above. Both methods determine N from input vector size.

### 3. `src/gui/modem/streaming_encoder.hpp`

- Add `#include "fec/burst_interleaver.hpp"`
- Add member: `bool use_burst_interleave_ = false;`
- Add setter: `void setBurstInterleave(bool enable)`

### 4. `src/gui/modem/streaming_encoder.cpp`

Modify `encodeBurstLight()` (line 178):

```cpp
std::vector<float> StreamingEncoder::encodeBurstLight(
    const std::vector<Bytes>& frame_data_list) {

    if (frame_data_list.size() == 1) {
        return encodeFrameLight(frame_data_list[0]);
    }

    // Phase 1: LDPC encode all frames
    std::vector<Bytes> encoded_frames;
    for (const auto& fd : frame_data_list)
        encoded_frames.push_back(encodeFrameBytes(fd));

    // Phase 2: Burst interleave (only for exactly 4 frames)
    if (use_burst_interleave_ && encoded_frames.size() == 4)
        encoded_frames = fec::BurstInterleaver::interleave(encoded_frames);

    // Phase 3: Modulate with preambles
    // First frame: light preamble
    Samples preamble = waveform_->supportsDataPreamble()
        ? waveform_->generateDataPreamble()
        : waveform_->generatePreamble();
    Samples modulated = waveform_->modulate(encoded_frames[0]);

    std::vector<float> result;
    result.reserve(preamble.size() + modulated.size() * encoded_frames.size());
    result.insert(result.end(), preamble.begin(), preamble.end());
    result.insert(result.end(), modulated.begin(), modulated.end());

    // Subsequent frames: training + data
    for (size_t i = 1; i < encoded_frames.size(); i++) {
        Samples training = waveform_->generateDataPreamble();
        Samples mod = waveform_->modulate(encoded_frames[i]);
        result.insert(result.end(), training.begin(), training.end());
        result.insert(result.end(), mod.begin(), mod.end());
    }
    return result;
}
```

### 5. `src/gui/modem/streaming_decoder.hpp`

- Add `#include "fec/burst_interleaver.hpp"`
- Add member: `bool use_burst_interleave_ = false;`
- Add setter: `void setBurstInterleave(bool enable)`
- Add private method: `void decodeBurstInterleaved(const std::vector<float>& first_soft, float snr, float cfo);`
- Add private helper: `std::vector<float> demodulateNextContinuation();` — extracts the burst continuation demodulation logic into a reusable method

### 6. `src/gui/modem/streaming_decoder.cpp`

**6a. Extract continuation demodulation helper** from burst loop (lines 814-873):

```cpp
std::vector<float> StreamingDecoder::demodulateNextContinuation() {
    // Same sample-reading, energy-checking, waveform::process logic
    // as current burst continuation loop body (lines 816-873)
    // Returns soft bits, or empty vector if no more frames
}
```

**6b. New burst-interleaved decode method:**

```cpp
void StreamingDecoder::decodeBurstInterleaved(
    const std::vector<float>& first_soft, float snr, float cfo) {

    constexpr int BURST_N = 4;
    std::vector<std::vector<float>> physical_soft;
    physical_soft.push_back(first_soft);

    // Demodulate N-1 continuation frames
    for (int i = 1; i < BURST_N; i++) {
        auto next_soft = demodulateNextContinuation();
        if (next_soft.empty()) break;
        physical_soft.push_back(next_soft);
    }

    if ((int)physical_soft.size() == BURST_N) {
        // Full burst received → deinterleave
        auto logical_soft = fec::BurstInterleaver::deinterleave(physical_soft);

        // Decode each logical frame
        for (int i = 0; i < BURST_N; i++) {
            DecodeResult result = decodeFrame(logical_soft[i], snr, cfo);
            if (result.success || result.codewords_ok > 0) {
                std::lock_guard<std::mutex> qlock(queue_mutex_);
                frame_queue_.push(result);
                if (result.success && frame_callback_) frame_callback_(result);
            }
            // Update stats
            std::lock_guard<std::mutex> slock(stats_mutex_);
            if (result.success) stats_.frames_decoded++;
            else stats_.frames_failed++;
        }
    } else {
        // Partial burst (< 4 frames) → decode individually (not interleaved)
        for (auto& soft : physical_soft) {
            DecodeResult result = decodeFrame(soft, snr, cfo);
            if (result.success || result.codewords_ok > 0) {
                std::lock_guard<std::mutex> qlock(queue_mutex_);
                frame_queue_.push(result);
                if (result.success && frame_callback_) frame_callback_(result);
            }
            std::lock_guard<std::mutex> slock(stats_mutex_);
            if (result.success) stats_.frames_decoded++;
            else stats_.frames_failed++;
        }
    }
}
```

**6c. Modify processFrame()** — insert burst-interleaved path after first frame decode fails (after line 782, before burst continuation):

```cpp
// NEW: If decode failed and burst interleaving is enabled, try burst decode
if (!result.success && use_burst_interleave_ && connected_ && is_ofdm) {
    LOG_MODEM(INFO, "[%s] First frame decode failed, trying burst-interleaved decode",
              log_prefix_.c_str());
    decodeBurstInterleaved(soft_bits, sync_snr_, sync_cfo_);
    burst_blocks_decoded_ = 0;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        correlation_pos_ = /* advance past all consumed samples */;
    }
    state_ = DecoderState::SEARCHING;
    return;
}
```

**6d. Modify existing burst continuation** (line 810): add `!use_burst_interleave_` guard so the old path doesn't run when burst interleaving is active:

```cpp
if (result.success && connected_ && is_ofdm && !is_non_data_frame && !use_burst_interleave_) {
    // Existing burst continuation (non-interleaved)
}
```

### 7. `src/protocol/connection.cpp`

In `enterConnected()`:
- When OFDM mode is negotiated: set window_size = 4, `use_burst_interleave = true`
- Propagate to encoder and decoder via existing setters:
  ```cpp
  encoder_->setBurstInterleave(true);
  decoder_->setBurstInterleave(true);
  arq_.setWindowSize(4);
  ```

### 8. `tools/cli_simulator.cpp`

In `setupCallbacks()` where connected mode is configured:
- Set `encoder_->setBurstInterleave(true)` and `decoder_->setBurstInterleave(true)`
- Set ARQ window to 4
- Add `--no-burst-interleave` flag for A/B comparison testing

### 9. `CMakeLists.txt`

Add `src/fec/burst_interleaver.cpp` to the build.

## Implementation Phases

### Phase 1: BurstInterleaver class
- Create `burst_interleaver.hpp/cpp`
- Unit-testable standalone: interleave then deinterleave should be identity
- Build and verify compilation

### Phase 2: Encoder integration
- Modify `encodeBurstLight()` to apply burst interleaving
- Add `use_burst_interleave_` flag
- Build and verify — TX now produces interleaved bursts (will break RX until Phase 3)

### Phase 3: Decoder integration
- Extract `demodulateNextContinuation()` helper
- Add `decodeBurstInterleaved()` method
- Modify `processFrame()` to enter burst path on failure when enabled
- Guard existing burst continuation with `!use_burst_interleave_`
- Build — should now decode burst-interleaved frames

### Phase 4: Connection/cli_simulator integration
- Enable burst interleaving in connected OFDM mode
- Set window_size = 4
- Add `--no-burst-interleave` CLI flag
- Build and test

### Phase 5: Testing and tuning
- Run regression tests
- Compare with and without burst interleaving
- Verify AWGN 100%, fading improvement, DQPSK no regression

## Verification

```bash
# Build
cd /home/mathieu/Projects/ProjectUltra/build && make -j4

# QPSK R1/2 fading — primary target (should improve from ~80% to ~90%+)
./build/cli_simulator --snr 20 --fading good --rate r1_2 --mod qpsk --seed 42 --test 2>&1 | tee /tmp/burst_interleave.log

# Multi-seed comparison
for seed in 42 43 44 45 46; do
  ./build/cli_simulator --snr 20 --fading good --rate r1_2 --mod qpsk --seed $seed --test 2>&1 | tail -8
done

# DQPSK R1/4 fading regression (must stay 100%)
./build/cli_simulator --snr 15 --fading good --rate r1_4 --test 2>&1 | tee /tmp/burst_regression.log

# AWGN regression (must stay 100%, no burst interleaving activity)
./build/cli_simulator --snr 20 --rate r1_2 --mod qpsk --test 2>&1 | tee /tmp/burst_awgn.log

# DQPSK R1/2 fading (should also benefit)
./build/cli_simulator --snr 20 --fading good --rate r1_2 --test 2>&1 | tee /tmp/burst_dqpsk.log
```

## Expected Results

- **QPSK R1/2 fading**: ~80% → ~90%+ frame success. Each CW's bits span 4 frames (~2.8s). A deep null in one frame affects 25% of bits per CW. R1/2 LDPC handles 25% erasure easily.
- **DQPSK R1/4 fading**: Should remain 100% (already works, burst interleaving is bonus protection)
- **AWGN**: Should remain 100% (no frame failures → burst interleaving is transparent)
- **Throughput**: Window reduced from 8 to 4 → ~50% reduction in frames-in-flight. Offset by fewer retransmissions. Net effect depends on RTT vs frame duration.

## Edge Cases

1. **Standalone control frames**: Not burst-interleaved. Decoder's CW0 probe succeeds → normal decode
2. **Partial bursts (<4 frames)**: No interleaving. Decoded individually (existing path)
3. **Lost trailing frame in burst**: Decoder gets 3 frames instead of 4 → falls back to individual decode (since ≠4). All 3 frames produce garbage (TX used N=4 interleaving). ARQ retransmits. Acceptable — frame loss within a burst is rare.
4. **Retransmissions**: Single frames, not burst-interleaved. Decoded normally.
5. **Mode transitions**: `setBurstInterleave(false)` during mode change.
