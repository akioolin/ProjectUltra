# ProjectUltra

**High-performance HF modem for amateur radio**

*Last updated: 2026-05-02*

> **EXPERIMENTAL SOFTWARE — WORK IN PROGRESS**
>
> Active development. Not production-ready. APIs and protocols may change.
> Use at your own risk for experimentation and amateur-radio research.

ProjectUltra is a software modem for reliable HF data transfer. It ships
three things in one repo:

- **Modem core** — adaptive OFDM + MC-DPSK waveforms with LDPC FEC and
  Selective-Repeat ARQ.
- **GUI application** — real-time waterfall, constellation, and message
  log (ImGui + SDL2).
- **VARA-compatible TCP TNC** — `ultra_tnc` exposes the modem via the
  same TCP command/data API used by Pat, Winlink Express, BPQ32, and
  other clients. Drop-in alternative on Linux, macOS, and Windows.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Status: Experimental](https://img.shields.io/badge/Status-Experimental-orange.svg)]()
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)]()

---

## Performance

ProjectUltra exposes throughput at two layers. Both matter; they answer
different questions.

### Raw PHY (theoretical maximum)

Useful payload bits / second over the air, ignoring ARQ overhead and
auto-rate adaptation. This is what one bulk frame delivers in steady
state on a stable channel.

OFDM 1024-FFT, 59 carriers, CP=96, ~42.9 sym/s:

| SNR  | Mode             | Throughput | Notes |
|------|------------------|-----------:|-------|
| 5+   | MC-DPSK (8 car)  |    938 bps | ±50 Hz CFO, robust sync |
| 8+   | OFDM-NARROW R1/4 |    103 bps | 500 Hz BW for crowded bands |
| 8+   | OFDM-NARROW R1/2 |    230 bps | 500 Hz BW |
| 10+  | OFDM DQPSK R1/4  |   1264 bps | Fading-tolerant baseline |
| 15+  | OFDM DQPSK R1/2  |   2271 bps | Good + moderate fading |
| 20+  | OFDM DQPSK R2/3  |   3028 bps | Good fading only |
| 20+  | OFDM DQPSK R3/4  |   3536 bps | AWGN only |
| 25+  | OFDM 16QAM R3/4  |   5657 bps | Stable paths (NVIS, ground wave) |
| 30+  | OFDM 32QAM R3/4  |   7071 bps | Stable paths only |

### End-to-end measured (real hardware)

What a user actually sees over the cable / air, including handshake,
ACK roundtrips, retransmissions, and auto-rate downgrades. Auto-rate
ladder is on; Connection picks among R1/4 / R1/2 / R2/3 / R3/4 based
on measured SNR and fading.

| Test                       | Channel              | Wall  | Throughput | Notes |
|----------------------------|----------------------|------:|-----------:|-------|
| 50 KB Mac↔Pi5 cable        | Clean USB cable      | 174 s |  2354 bps  | Auto picked DQPSK R3/4 @ SNR=28 |
| 500 KB Mac↔Pi5 injected    | Watterson Good, SNR=15 | 3780 s | 1083 bps  | 930 retx, 0 failed, byte-exact |
| 5 KB Mac↔Pi5 injected      | Watterson Good, SNR=15 |       |            | 0 retx, byte-exact |

End-to-end results match or exceed real-world numbers reported for
existing commercial HF data modems in equivalent conditions. The 500 KB
Good15 result is the realistic-HF baseline; 50 KB cable is the upper
bound given a clean channel.

### Waveform selection (automatic)

```
SNR         Waveform              Reason
─────────────────────────────────────────────────────────────────────
5–10 dB     MC-DPSK (8 carriers)  Differential encoding, dual-chirp sync
5–10 dB     OFDM-NARROW (500 Hz)  Crowded bands, low SNR
10–17 dB    OFDM-CHIRP (1024)     Dual-chirp sync, 59 carriers
17+ dB      OFDM-COX (1024)       Schmidl-Cox sync, faster acquisition
25+ dB NVIS OFDM 16QAM            Coherent + pilot tracking
```

Selection happens during CONNECT (peer-advertised SNR + fading index)
and continues adapting during the session. See `docs/PROJECT_GOALS.md`
for the throughput/reliability targets driving this work.

---

## TNC integration (Pat, Winlink Express, BPQ32)

`ultra_tnc` is a daemon that exposes ProjectUltra's modem through the
VARA HF TCP TNC protocol. Existing clients can connect to it the same
way they connect to a commercial HF data modem — no protocol changes
on the client side.

```
┌──────────────┐  TCP 8300 (cmd)   ┌──────────────┐
│  Pat /       │  TCP 8301 (data)  │  ultra_tnc   │  Audio   ┌─────────┐
│  Winlink /   │ ◄──────────────► │  (modem +    │ ◄──────► │  HF     │
│  BPQ32 / ... │                   │   TCP shell) │          │  Radio  │
└──────────────┘                   └──────────────┘          └─────────┘
```

### Quick start

```bash
# Build (see Getting Started below)
cmake -S . -B build && cmake --build build -j 4

# Listen on default ports 8300/8301
./build/ultra_tnc --audio-output "USB Audio Device" \
                  --audio-input  "USB Audio Device" \
                  --callsign     N0CALL

# Smoke test from another terminal
printf 'VERSION\r' | nc 127.0.0.1 8300
# → VERSION 4.9.0
```

Full command reference: [`docs/TNC_INTERFACE.md`](docs/TNC_INTERFACE.md).

### Supported commands

Standard VARA: `VERSION`, `MYCALL`, `LISTEN`, `CONNECT`, `DISCONNECT`,
`ABORT`, `BW500` / `BW2300` / `BW2750`, `BUFFER`, `SN`, `BITRATE`,
`COMPRESSION`, `CHAT`, `CWID`, plus Mercury / Pat-Vara compatibility
no-ops (`PUBLIC`, `P2P`, `WINLINK`, `IGNOREKISSDCD`, `RETRIES`,
`CALLINT`).

ProjectUltra extension:

- **`STATS`** — single-line ARQ + PHY snapshot for debugging stalled
  sessions: `frames_sent`, `frames_recv`, `retx`, `timeouts`, `failed`,
  `out_of_order`, current `rate` / `mod` / `mode`, `snr`, `bps`,
  `backlog`. Pat ignores unknown commands, so this is safe to leave on.

### Status

- Cross-platform: Linux + macOS + Windows. CI matrix all green.
- ctest: 34/34 (98 TNC parser tests, 19 TCP integration tests, 17
  bridge tests, plus modem regressions).
- End-to-end byte-exact transfers: 50 KB Mac↔Pi5 over real audio
  cable, 2354 bps random / 102400 bps with compression on prose-shaped
  text (deflate ratio 222×).
- **Real Pat client validated end-to-end**: Pat 1.0.0 (Mac) ↔ Pat
  0.15.1 (Pi5) drove a full battery of B2F sessions through two
  `ultra_tnc` instances over real audio cable:
  - Empty CONNECT/DISCONNECT cycle — 13 s, clean
  - 41 B text Mac→Pi — 21 s, byte-exact
  - 2.6 KB text Mac→Pi — 50 s, gzipped to 357 B, byte-exact
  - 1 KB binary attachment Mac→Pi — 56 s, byte-exact
  - 47 B text Pi→Mac (reverse direction) — 37 s, byte-exact
  - Bidirectional in single session — 57 s, both inboxes updated
  - 12.5 KB Lorem ipsum Mac→Pi — 50 s, gzipped to 448 B (28× ratio)
  - Known limitation: back-to-back sessions within ~30 s of teardown
    don't always recover cleanly (1/3 retry success). Root cause
    traced upstream: pat-vara silently drops inbound connections if
    `Accept()` hasn't re-armed (pat-vara `vara.go:344-349`, falls
    through to `select` default + sends `DISCONNECT`). Single-session
    flows are reliable.
- Five real bugs found + fixed during the integration: VERSION
  response format, CONNECTED line initiator/responder ordering,
  missing `BUFFER N` event emission, BUFFER staging accounting, and
  the audit document `docs/PAT_VARA_AUDIT.md` (Codex-generated, full
  pat-vara expectations spec) for future reference.
- Winlink Express on Windows: spec-compatible, not yet manually tested.

---

## Getting Started

### Requirements

- Linux, macOS, or Windows
- CMake 3.16+
- C++20 compiler (GCC 10+, Clang 12+, MSVC 2019+)
- SDL2 (GUI + audio I/O for `cli_simulator` / `ultra_tnc`)
- FFTW3 (required for fast chirp detection — Cooley-Tukey fallback is
  unusable for real-time)

### Building

```bash
# Ubuntu/Debian
sudo apt install libsdl2-dev libfftw3-dev cmake build-essential pkg-config

# macOS
brew install sdl2 fftw cmake pkg-config

# Windows (vcpkg)
vcpkg install sdl2 fftw3

git clone https://github.com/secup/ProjectUltra.git
cd ProjectUltra
cmake -S . -B build
cmake --build build -j 4
```

### Running

**GUI** (operator UI with waterfall and constellation):

```bash
./build/ultra_gui          # Normal mode
./build/ultra_gui -sim     # Developer / simulator mode (no radio needed)
```

**TNC** (VARA-compatible TCP shell, see TNC section above):

```bash
./build/ultra_tnc --audio-output "USB Audio" --audio-input "USB Audio"
```

**CLI simulator** (full protocol, two-station, channel injection):

```bash
./build/cli_simulator --snr 15 --channel good --rate auto --test
```

**CLI tools** (single-frame transmit/decode, for offline analysis):

```bash
./build/ultra ptx "Hello" -s MYCALL -d THEIRCALL | aplay -f FLOAT_LE -r 48000
arecord -f FLOAT_LE -r 48000 | ./build/ultra prx
```

---

## How It Works

### Protocol stack

```
┌────────────────────────────────────────────────────────┐
│  Pat / Winlink Express / BPQ32 / your own client       │
├────────────────────────────────────────────────────────┤
│  ultra_tnc      (TCP cmd 8300, data 8301)              │
│  TNCSession     (VARA command parser + state machine)  │
│  TNCBridge      (ModemAdapter ↔ ProtocolEngine)        │
├────────────────────────────────────────────────────────┤
│  Connection     (PING/CONNECT/MODE_CHANGE/DATA/DISC)   │
│  ARQ            (Selective-Repeat, window=16, SACKs)   │
│  Frame v2       (4-CW fixed frames + 1-CW control)     │
├────────────────────────────────────────────────────────┤
│  Waveforms      (OFDM-CHIRP, OFDM-NARROW, OFDM-COX,    │
│                  MC-DPSK + adaptive selection)         │
│  LDPC           (IEEE 802.11n, R1/4 to R5/6)           │
│  Sync / CFO     (dual chirp + Schmidl-Cox + LTS)       │
├────────────────────────────────────────────────────────┤
│  Audio I/O      (SDL2 — Linux ALSA, macOS CoreAudio,   │
│                  Windows WASAPI)                       │
└────────────────────────────────────────────────────────┘
```

### Connection flow

1. **PING/PONG** — fast presence probe (~1 s each) before committing
   to a full CONNECT.
2. **CONNECT** — callsign exchange (FCC Part 97.119 compliant) over
   MC-DPSK.
3. **MODE_CHANGE** — picks data waveform/rate from peer-advertised SNR
   and fading.
4. **DATA** — Selective-Repeat ARQ with cumulative SACKs.
5. **DISCONNECT** — graceful with callsign ID.

If no PONG after 5 PINGs (~15 s), connection fails fast.

### Signal parameters

| Parameter        | MC-DPSK    | OFDM       |
|------------------|-----------:|-----------:|
| Sample rate      | 48 kHz     | 48 kHz     |
| Bandwidth        | ~2.4 kHz   | ~2.8 kHz   |
| Center freq.     | 1500 Hz    | 1500 Hz    |
| Carriers         | 8          | 59         |
| FFT size         | n/a        | 1024       |
| Symbol rate      | ~94 baud   | ~42.9 baud |
| Cyclic prefix    | n/a        | 96 (~2 ms) |
| Sync             | Dual chirp | Dual chirp / Schmidl-Cox |
| LDPC codeword    | 648 bits   | 648 bits   |

---

## Testing

### Unit + regression gate

```bash
cmake --build build -j 4
ctest --test-dir build --output-on-failure -j 4
```

34 tests covering modem primitives, protocol/ARQ, TNC parser, TNC TCP
reactor, and TNC bridge. CI runs the full matrix on Linux + macOS +
Windows with ASAN/UBSAN and coverage gates.

### Full-protocol simulator

```bash
# Default regression (full handshake + ARQ data + disconnect)
./build/cli_simulator --snr 15 --channel good --rate auto --test

# Force specific PHY config
./build/cli_simulator --snr 20 --channel awgn --mod dqpsk --rate r2_3 --test

# CFO chain verification
./tests/verify_cfo_chain.sh --cfo 50 --channel awgn --snr 20 --seed 42
```

### Hardware loopback (two stations)

```bash
# 50 KB Mac↔Pi over real audio cable
SSH_KEY=$HOME/.ssh/id_pi5 PAYLOAD_SIZE=51200 \
  ./tools/tnc_loopback_test.sh
```

`tnc_loopback_test.sh` orchestrates two `ultra_tnc` instances (one
local, one over SSH), pushes a binary payload through the TCP data
port, and CRC-checks delivery.

### Manual modulation / rate selection

`--mod`: `dqpsk` (default, 2 bits/sym), `d8psk` (3 bits/sym), `dbpsk`
(1 bit/sym, most robust), `qam16`/`qam32`/`qam64` (coherent, stable
paths only).

`--rate`: `r1_4`, `r1_2`, `r2_3`, `r3_4`, `r5_6`, `auto` (default;
adaptive ladder).

---

## Radio Setup

### Requirements

- SSB transceiver with 2.8+ kHz filter bandwidth (or 500 Hz for
  OFDM-NARROW)
- Audio interface (SignaLink, RigBlaster, USB soundcard, or direct
  cable)
- PTT control (VOX, CAT, or hardware)

### Audio levels

- TX: clean signal without ALC compression
- RX: comfortable listening level, avoid clipping (peak < 0.9)

### Recommended frequencies (2.8 kHz BW, USB)

| Band | Frequency  | Notes |
|------|-----------:|-------|
| 80m  | 3.590 MHz  | Above narrow digital, below voice |
| 40m  | 7.102 MHz  | Common for wideband digital |
| 30m  | 10.145 MHz | Check for WSPR at 10.140 |
| 20m  | 14.108 MHz | Above FT8 crowd |
| 15m  | 21.110 MHz | Above narrow digital segment |
| 10m  | 28.120 MHz | Plenty of room |

**Avoid:** 14.070–14.095 MHz (FT8/PSK31), any .074 MHz (FT8), 14.100
MHz (NCDXF beacons). Listen 10–15 s before TX. Use minimum power
necessary. Be ready to QSY.

---

## Status & Roadmap

### Solid today

- MC-DPSK baseline (5+ dB, ±50 Hz CFO).
- OFDM-CHIRP DQPSK R1/4 → R3/4 with adaptive ladder.
- OFDM-NARROW (500 Hz) for crowded bands or low-SNR conditions.
- TNC subsystem: cross-platform, byte-exact end-to-end.
- ARQ control path: hardened DISCONNECT, cumulative SACKs, no
  timeout storms.

### Experimental / opportunistic

- D8PSK on fading channels (variable run-to-run).
- Coherent QPSK / 16QAM / 32QAM (stable-path only — NVIS, ground
  wave, cable).
- Higher rates (R2/3+) under deep fades.

### Active work

- Pat / Winlink Express integration: client-side validation pending.
- Long-running stability soak (multi-hour `ultra_tnc` uptime).
- Continued OTA validation; expanding hardware-injection seed coverage.

The active engineering goal lives in `docs/PROJECT_GOALS.md`. Speculative
research (removed waveforms, archived ideas) lives under
`docs/archive/` — historical only, not part of the production build.

---

## Contributing

Contributions welcome. Easiest entry points:

- On-air testing reports (especially with `STATS` output included).
- Pat / Winlink Express interop reports — what worked, what didn't.
- Bug fixes and DSP optimizations (profile first; see
  `docs/QUALITY_STRATEGY.md`).
- Documentation.

Please open an issue before submitting large PRs.

### Help wanted: real HF recordings

Real ionospheric propagation has characteristics simulation can't
capture. Even "failed" recordings are valuable.

To record your own signal: tune a WebSDR (websdr.org or kiwisdr.com)
to your TX frequency, start recording, transmit using ProjectUltra,
stop and save the file. Submit via GitHub issue with the "Recording"
label, including: callsign, location, band/freq/UTC, WebSDR used, path
distance, and S-meter readings if known.

---

## License

MIT License. See [LICENSE](LICENSE).

---

## Acknowledgments

- Community OTA testers, especially **KC3VPB**, for sharing real-station
  logs that helped diagnose post-handshake sync rejects and buffer-
  overflow edge cases.
- [Dear ImGui](https://github.com/ocornut/imgui) — GUI framework.
- [SDL2](https://libsdl.org/) — audio and windowing.
- [FFTW3](https://www.fftw.org/) — Fast Fourier Transform.
- [miniz](https://github.com/richgel999/miniz) — compression.
