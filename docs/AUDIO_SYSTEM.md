# Audio I/O System

**Purpose:** Current audio path and buffering behavior for debugging and maintenance.

---

## Overview

| Parameter | Value |
|-----------|-------|
| Sample Rate | 48,000 Hz |
| Channels | 1 (mono) |
| Format | 32-bit float (`AUDIO_F32SYS`) |
| SDL Callback Buffer | 1024 samples (~21 ms) |
| Backend | SDL2 |

---

## Core Components

| Component | Location | Responsibility |
|-----------|----------|----------------|
| `AudioEngine` | `src/gui/audio_engine.hpp/cpp` | Device I/O, callbacks, TX/RX sample queues |
| `ModemEngine` | `src/gui/modem/modem_engine.*` | TX encode/modulate and RX routing |
| `StreamingDecoder` | `src/gui/modem/streaming_decoder.*` | Primary RX decode pipeline (sync + demod + FEC) |

---

## TX Path

```text
ProtocolEngine
  -> TxDataCallback / TransmitBurstCallback
  -> ModemEngine::transmit(...)
  -> AudioEngine::queueTxSamples(...)
  -> AudioEngine::outputCallback() [SDL output thread]
  -> speaker / rig audio output
```

### TX queue behavior

- Backing type: `std::queue<float>`
- Access: mutex protected (`tx_mutex_`)
- Empty queue outputs silence
- No hard cap on TX queue length

---

## RX Path

```text
mic / rig audio input
  -> AudioEngine::inputCallback() [SDL input thread]
     - DC blocker: y[n] = x[n] - x[n-1] + 0.995*y[n-1]
     - input gain
     - RMS meter update
     - append to AudioEngine rx_buffer_
     - invoke rx_callback_ (unless muted)
  -> App::startRadioRx callback
  -> ModemEngine::feedAudio(...)
  -> StreamingDecoder::feedAudio(...)
  -> decode thread (or synchronous simulator loop) calls processBuffer()
  -> decoded frame callback into ProtocolEngine
```

---

## Buffering And Overflow

| Buffer | Limit | Overflow policy |
|--------|-------|-----------------|
| AudioEngine RX buffer (`rx_buffer_`) | 96,000 samples (~2 s) | Drop oldest samples |
| StreamingDecoder circular buffer | 480,000 samples (~10 s) | Advance read/search position and count overflow event |

Notes:
- `ModemEngine` keeps `MAX_PENDING_SAMPLES` as a guard constant, but RX decode is handled by `StreamingDecoder`.
- Loopback TX->RX also writes through `AudioEngine::rx_buffer_` with the same 96,000-sample cap.

---

## Loopback Simulation

Used for local testing without radio hardware.

```cpp
audio_.setLoopbackEnabled(true);
audio_.setLoopbackSNR(15.0f);
```

Behavior:
1. TX samples are queued normally.
2. A Box-Muller AWGN stage is applied in `AudioEngine::addChannelNoise()`.
3. Noisy samples are appended to `rx_buffer_`.
4. Main loop / callback path consumes those samples on the next RX cycle.

---

## Threading Model

| Thread | Work |
|--------|------|
| Main/UI thread | Device open/close, settings, protocol ticking |
| SDL output callback thread | Drain TX queue to audio device |
| SDL input callback thread | Capture audio, filtering, callback dispatch |
| Modem RX decode thread | `StreamingDecoder::processBuffer()` in async mode |
| Simulator thread (`-sim`) | Optional; drives both modems in synchronous decode mode |

Synchronization:
- TX/RX sample containers use mutexes.
- Runtime flags and meters use atomics.

---

## Latency Notes

| Stage | Typical latency |
|-------|-----------------|
| SDL input/output callback cadence | ~21 ms |
| DC filter + gain work | <1 ms per callback |
| Sync + frame accumulation | frame dependent (hundreds of ms to a few seconds) |
| LDPC decode | tens to hundreds of ms |

Overall RX latency is dominated by waveform/frame duration and channel conditions, not SDL callback overhead.

---

## Key Files

| File | Purpose |
|------|---------|
| `src/gui/audio_engine.hpp` | AudioEngine API |
| `src/gui/audio_engine.cpp` | SDL callbacks and sample queue logic |
| `src/gui/modem/modem_rx.cpp` | RX thread and `feedAudio` routing |
| `src/gui/modem/streaming_decoder.hpp` | Streaming decode interfaces and limits |
| `src/gui/modem/streaming_decoder.cpp` | Sync search and decode state machine |
