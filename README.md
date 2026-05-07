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

[![Build Matrix](https://github.com/secup/ProjectUltra/actions/workflows/build-matrix.yml/badge.svg?branch=main)](https://github.com/secup/ProjectUltra/actions/workflows/build-matrix.yml)
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

| Test                          | Channel                | Wall  | Throughput | Notes |
|-------------------------------|------------------------|------:|-----------:|-------|
| 50 KB Mac↔Pi5 cable           | Clean USB cable        | 174 s |  2354 bps  | Auto DQPSK R3/4 @ SNR=28 |
| 20 KB Mac↔Pi5 injected        | AWGN, SNR=15           |  72 s |  2266 bps  | Auto DQPSK R2/3, 0 retx, byte-exact |
| 20 KB Mac↔Pi5 injected        | Watterson Good, SNR=15 | 100 s |  1631 bps  | Auto DQPSK R1/2, 0 retx, byte-exact |
| 5 KB Mac↔Pi5 injected ×5      | Watterson Good, SNR=15 |  28 s |  1440 bps  | Median of 5 seeds, R1/2; 5/5 PASS post-BUG-RATE-001 fix (worst-case 684 bps; was 444 bps pre-fix with R1/2→R1/4 panic) |
| 500 KB Mac↔Pi5 injected       | Watterson Good, SNR=15 | 3742 s |  1094 bps | Long-haul, 1346 retx, 0 failed, byte-exact |

End-to-end results match or exceed real-world numbers reported for
existing commercial HF data modems in equivalent conditions. The 500 KB
Good15 result is the realistic-HF baseline; 50 KB cable is the upper
bound given a clean channel.

### Features

**Waveforms.** MC-DPSK (chirp sync, low-SNR robust), OFDM-CHIRP
(wideband 2.8 kHz, 59 carriers), OFDM-NARROW (500 Hz crowded-band
mode), OFDM-COX (Schmidl-Cox sync), SC-DPSK (very low SNR).

**Modulation + FEC.** DBPSK / DQPSK / D8PSK / BPSK / QPSK with
802.11n LDPC at four code rates (R1/4, R1/2, R2/3, R3/4).
Min-sum belief-propagation decoder.

**Synchronization.** Dual-chirp detection with FFTW-accelerated
correlation, Schmidl-Cox training, light-preamble (LTS-only) for
in-session frames, LTS-residual CFO refinement, per-symbol pilot
tracking with common-phase-error correction.

**ARQ.** Selective-repeat with cumulative + selective ACKs,
window 1 (DPSK / narrowband) or 8–16 (wideband OFDM), variable
1–8 codeword frames, wire-negotiated CW count, frame +
optional channel + burst interleavers.

**Adaptive rate.** SNR + fading-index ladder (R3/4 / R2/3 /
R1/2 / R1/4) with bootstrap cap, per-burst clean-window upgrade,
two-window hysteresis on downshift to prevent panic-downshift on
short fading transfers.

**Per-carrier RX erasure.** Each OFDM-CHIRP frame computes
`γ_k = |H_k|² / σ²_k` from its own LTS + pilots; carriers below
-6 dB emit `LLR = 0` to LDPC after a persistence gate (3
consecutive symbols or 2 consecutive multi-CW frames). Bits are
spread across LDPC base columns by the CarrierLDPC v1
interleaver `a = (307·i) mod (648·Ncw)`. Silent on AWGN /
light fading; converts deep stationary notches and in-band QRM
from a TEST FAILED into a clean decode (validated A/B on a
fixed -25 dB notched carrier: 2,271 bps vs 15-frame loss
baseline).

**HARQ Chase soft-combining.** LLR-accumulating buffer keyed by
full PHY digest (rate, modulation, interleaver, carrier mask,
erasure-policy epoch). Currently active only when the frame's
header CW decodes; broader CW0-fail integration is in
experimental branches awaiting further hardware validation.

**Channel testing.** Built-in Watterson HF channel injector
(ITU-R F.1487) with AWGN, Good (0.1 Hz / 0.5 ms), Moderate
(0.5 Hz / 1 ms), Poor (1 Hz / 2 ms) presets. Two-machine hardware
harness (Mac ↔ Pi5 over USB sound cards) with byte-exact
end-to-end validation.

**Protocol v2.** PING / PONG, CONNECT / CONNECT_ACK,
MODE_CHANGE, DATA, ACK / SACK, DISCONNECT. Wire-level CRC-16
on every frame, capability flags, measured-SNR + fading-index
exchange.

**TNC integration.** `ultra_tnc` daemon exposes the modem over
the same TCP command/data API used by Pat, Winlink Express,
BPQ32, and similar clients (cmd port 8300 / data port 8301);
verified end-to-end with real Pat sessions across all major
B2F message types.

**GUI application.** `ultra_gui` with real-time waterfall,
constellation, message log, and ARQ health view (ImGui +
SDL2). Virtual-station / simulator mode for development.

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
- ctest: **37/37** (TNC parser, TCP integration, bridge tests, plus
  modem regressions including the new `CarrierLDPC v1` math gate
  and per-carrier mask plumbing).
- End-to-end byte-exact transfers (hardware harness, Mac ↔ Pi5
  over USB sound cards): see throughput table at the top of this
  README. 50 KB cable run hits 2,354 bps; injected Watterson
  Good at SNR=15 holds 1,631 bps clean; forced R3/4 at SNR=15
  AWGN delivers 2,676 bps clean (2026-05-07 calibration).
- **Real Pat client validated end-to-end** with Pat 1.0.0 (Mac)
  ↔ Pat 0.15.1 (Pi5) over real audio cable: full B2F session
  matrix passes byte-exact (empty connect/disconnect, text up
  to 12.5 KB, binary attachments, bidirectional, both
  directions). Five real bugs found + fixed during integration;
  full audit at `docs/PAT_VARA_AUDIT.md`.
- Known TNC limitation: back-to-back sessions within ~30 s of
  teardown don't always recover cleanly (~1/3 retry success).
  Root cause traced upstream to pat-vara
  (`vara.go:344-349` drops inbound connection if `Accept()`
  hasn't re-armed). Single-session flows are reliable.
- Winlink Express on Windows: spec-compatible, not yet manually
  tested.

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

> **macOS Gatekeeper note (downloaded prebuilt binaries only).**
> macOS quarantines anything downloaded from the internet. If you
> grabbed a release bundle and macOS shows *"cannot be verified"*
> when you try to launch it, run this once per binary to remove
> the quarantine flag:
>
> ```bash
> xattr -d com.apple.quarantine /path/to/ultra_gui
> ```
>
> Or right-click the binary in Finder → **Open** → confirm
> *Open* in the warning dialog (one-time exception). Binaries
> built locally from source are not affected. Proper code-sign
> + notarization is on the post-alpha release roadmap.

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

### Solid (hardware-validated)

- MC-DPSK baseline (5+ dB SNR, ±50 Hz CFO tolerance).
- OFDM-CHIRP DQPSK R1/4 → R3/4 with adaptive ladder
  (R3/4 hardware-calibrated at SNR ≥ 15 + fading < 0.10).
- OFDM-NARROW (500 Hz) for crowded bands or low-SNR conditions.
- Per-carrier RX erasure with CarrierLDPC v1 interleaver
  (deep notch / QRM survival on OFDM-CHIRP).
- Adaptive MODE_CHANGE with hysteresis (BUG-RATE-001 fixed:
  no panic-downshift on short fading transfers).
- Selective-repeat ARQ with cumulative + selective ACKs,
  hardened DISCONNECT, no timeout storms.
- TNC subsystem: cross-platform Linux / macOS / Windows,
  byte-exact end-to-end, validated with real Pat sessions.
- Hardware-in-the-loop test rig (Mac ↔ Pi5 with Watterson
  injection) with byte-exact file-transfer validation.

### Experimental (in tree, not on by default)

- HARQ Chase soft-combining: math validated, infrastructure
  in place; broader CW0-fail integration is on
  `experimental/harq-audit-2026-05-06` pending further
  hardware validation.
- D8PSK on fading channels: variable run-to-run, gated to
  high SNR + AWGN-class fading only.
- Coherent QPSK / 16QAM / 32QAM: stable-path only (NVIS,
  ground wave, clean cable).

### Active work

- Long-running stability soak (multi-hour `ultra_tnc` uptime).
- Bootstrap-rate cap relaxation: short transfers can't yet
  reach R3/4 even on clean AWGN because the initial-rate
  cap requires SNR ≥ 24; hardware validation needed before
  lowering.
- Real over-the-air validation infrastructure: currently
  limited to the synthetic Watterson hardware harness;
  KiwiSDR-based remote-receiver TX validation is the
  near-term plan for solo operators.

### Deliberately deferred

- Higher-order constellations (16/32/64-QAM) as production
  ladder rungs: enum reserved, no production code. Real
  capacity headroom (~2× peak throughput) but needs proper
  EVM gating, coherent phase tracking, IQ-imbalance
  handling, and a new auto-rate policy layer — multi-week
  scope, not autonomous-friendly.
- Iterative LDPC ↔ equalizer (turbo) loops.
- OTFS / MFSK: enum reserved for wire compatibility, no
  production implementation.
- Code-sign + notarization for prebuilt macOS binaries
  (Apple Developer ID needed; on the post-alpha roadmap).

Active engineering goal lives in `docs/PROJECT_GOALS.md`.
Recent design + audit notes are in `docs/CHANGELOG.md`,
`docs/PHASE2_CARRIER_MASK_DESIGN.md`, and the
`docs/SESSION_*.md` series. Speculative / archived research
lives under `docs/archive/` — historical only, not part of
the production build.

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
