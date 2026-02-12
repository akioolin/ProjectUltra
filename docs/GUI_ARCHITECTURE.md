# GUI Architecture

**Purpose:** Current GUI structure and runtime flow.

---

## Overview

ProjectUltra GUI stack:

| Layer | Technology |
|-------|------------|
| UI framework | Dear ImGui |
| Window/input/audio platform | SDL2 |
| Renderer | OpenGL 2.1 backend (`imgui_impl_opengl2`) |

Entry point: `src/gui/main_gui.cpp`

---

## Main Components

| Component | Location | Role |
|-----------|----------|------|
| `App` | `src/gui/app.hpp/cpp` | Top-level coordinator: UI, modem, protocol, audio, simulator |
| `ModemEngine` | `src/gui/modem/modem_engine.*` | TX generation + RX decode orchestration |
| `ProtocolEngine` | `src/protocol/protocol_engine.*` | Link/session/file-transfer state machine |
| `AudioEngine` | `src/gui/audio_engine.*` | SDL audio devices, callbacks, buffering |
| Widgets | `src/gui/widgets/*` | Rendering controls/status/plots/settings |

---

## High-Level Data Flow

```text
User action (Send / Connect / File TX)
  -> App callback
  -> ProtocolEngine creates frame(s)
  -> ModemEngine::transmit(...)
  -> AudioEngine::queueTxSamples(...)
  -> SDL output callback -> radio audio
```

```text
Incoming audio (mic / rig)
  -> SDL input callback
  -> App RX callback
  -> ModemEngine::feedAudio(...)
  -> StreamingDecoder decode
  -> ProtocolEngine::onRxData(...)
  -> App updates UI/log/state
```

---

## Callback Wiring

### Protocol -> App
- `setTxDataCallback`
- `setTransmitBurstCallback`
- `setConnectionChangedCallback`
- `setMessageReceivedCallback`
- `setModeNegotiatedCallback`
- `setDataModeChangedCallback`
- file transfer callbacks (`setFileProgressCallback`, `setFileReceivedCallback`, `setFileSentCallback`)

### Modem -> App
- `setRawDataCallback`
- `setStatusCallback`
- `setPingReceivedCallback`

### Audio -> App
- `setRxCallback` (feeds modem + waterfall)

This keeps the GUI layer mostly orchestration logic with minimal direct coupling between protocol/audio internals.

---

## Threading Model

### Normal GUI mode

| Thread | Work |
|--------|------|
| Main/UI thread | SDL events, ImGui render, protocol tick, widget callbacks |
| SDL output callback thread | Play TX queue |
| SDL input callback thread | Capture RX audio and fire RX callback |
| Modem RX decode thread | `StreamingDecoder::processBuffer()` loop |

### Simulator mode (`-sim`)

| Thread | Work |
|--------|------|
| Main/UI thread | Same UI responsibilities |
| Simulator thread | Bidirectional sample streaming + channel effects + protocol ticking |

In simulator mode, both local and virtual modems are switched to **synchronous decode mode** and driven directly by the simulator loop.

---

## Virtual Station Simulator

Enabled with `./ultra_gui -sim`.

Architecture:

```text
Local TX -> channel(A->B) -> virtual modem RX -> virtual protocol
Virtual TX -> channel(B->A) -> local modem RX -> local protocol
```

Key points:
- Two independent modem/protocol stacks (local + virtual).
- Channel effects include AWGN and optional Watterson fading profiles.
- Streaming cadence uses 480-sample chunks (~10 ms at 48 kHz), matching realtime flow.

---

## UI Structure

Primary UI elements:
- Constellation panel
- Channel status/controls
- Waterfall display
- Message/file controls + RX log
- Settings window (`Station`, `Radio`, `Audio`, `Expert`)
- Bottom status line with mode, SNR, frame counters, PHY rate, and last goodput sample

Recent behavior relevant to operators:
- `STOP TX` immediate control is available from UI.
- Goodput in status reflects completed file transfer timing, not only PHY rate.

---

## Connection State Handling

Protocol states:

```text
DISCONNECTED -> PROBING -> CONNECTING -> CONNECTED -> DISCONNECTING -> DISCONNECTED
```

On state changes, `App` updates:
- modem connected/disconnected mode
- operator log lines
- displayed link state
- fallback waveform state after disconnect

---

## Key Files

| File | Purpose |
|------|---------|
| `src/gui/main_gui.cpp` | SDL/OpenGL/ImGui bootstrap and main loop |
| `src/gui/app.cpp` | Core app orchestration and callback wiring |
| `src/gui/app.hpp` | App state model |
| `src/gui/audio_engine.cpp` | Audio device and callback implementation |
| `src/gui/modem/modem_engine.cpp` | Modem setup, TX path, configuration |
| `src/gui/modem/modem_rx.cpp` | RX thread/synchronous decode control |
| `src/gui/widgets/settings.cpp` | Persisted settings UI |
