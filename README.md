# ProjectUltra

**High-performance HF modem for amateur radio**

*Last updated: 2026-02-12*

> **EXPERIMENTAL SOFTWARE - WORK IN PROGRESS**
>
> This project is under active development and is **not ready for production use**.
> Features may be incomplete, untested, or broken. APIs and protocols may change
> without notice. Performance numbers are from simulation only - real-world HF
> testing is ongoing. **Use at your own risk for experimentation and development only.**

ProjectUltra is a software modem that achieves reliable, high-speed data transfer over HF radio. It uses adaptive waveform selection to maintain communication across varying ionospheric conditions - from quiet bands to disturbed polar paths.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Status: Experimental](https://img.shields.io/badge/Status-Experimental-orange.svg)]()
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)]()

---

## Alpha Reality Snapshot (v0.2.4-alpha)

- **Stable baseline**: MC-DPSK plus OFDM DQPSK (`R1/4` to `R2/3`) are the most reliable paths in current testing.
- **Control path hardening landed**: DISCONNECT now uses a hardened 1-codeword control profile (`R1/4`) with `seq=0xFFFF`; recent 10-seed simulator check showed 10/10 pass and 0 disconnect timeouts.
- **File transfer gate passed**: recent 2048-byte OFDM test (`SNR=20`, good fading, `R1/2`) passed 5/5 seeds with 0 retransmissions and 0 timeouts.
- **Still experimental**: D8PSK and coherent high-order modes can show higher run-to-run variability on fading channels; keep these as opportunistic/expert modes for now.
- **OTA status**: on-air testing is active, but this is still alpha software and not production-ready.

---

## Features

- **Adaptive Waveforms**: Automatically selects optimal waveform based on channel conditions
- **Wide SNR Range**: Operates from -11 dB to 30+ dB (DPSK tested at -11 dB)
- **Strong FEC**: LDPC codes with rates from R1/4 to R5/6
- **ARQ Protocol**: Reliable delivery with automatic retransmission
- **GUI Application**: Real-time waterfall, constellation display, and message log
- **CLI Tools**: Frame-level transmit/receive for testing and debugging

---

## Performance

**Raw waveform throughput** (1024 FFT, 59 carriers, CP=96, 42.9 sym/s):
| SNR | Mode | Data carriers | Throughput | Notes |
|-----|------|---------------|------------|-------|
| 5+ dB | MC-DPSK (8 carriers) | 8 | 938 bps | 100% verified, ±50 Hz CFO |
| 10+ dB | OFDM DQPSK R1/4 | 59 (no pilots) | 1264 bps | 100% verified, fading OK |
| 15+ dB | OFDM DQPSK R1/2 | 53 (6 pilots) | 2271 bps | 100% verified, good + moderate fading |
| 20+ dB | OFDM DQPSK R2/3 | 53 (6 pilots) | 3028 bps | 100% verified, good fading |
| 20+ dB | OFDM DQPSK R3/4 | 55 (4 pilots) | 3536 bps | 100% verified, AWGN only |
| 20+ dB | OFDM D8PSK R1/2 | 53 (6 pilots) | 3407 bps | Works in strong channels; fading reliability still being hardened |

**Coherent modes (stable channels only):**
| SNR | Mode | Data carriers | Throughput | Notes |
|-----|------|---------------|------------|-------|
| 20+ dB | OFDM QPSK R1/2 | 47 (12 pilots) | 2014 bps | Coherent path, sensitive to phase/fading |
| 25+ dB | OFDM 16QAM R3/4 | 44 (15 pilots) | 5657 bps | Stable paths (NVIS, ground wave) |
| 30+ dB | OFDM 32QAM R3/4 | 44 (15 pilots) | **7071 bps** | Maximum throughput |

**When to use coherent modes (16QAM/32QAM):** Stable propagation paths like
ground wave or direct cable connection. These paths have stable phase, allowing
coherent demodulation with pilot-assisted channel estimation.

### Waveform Strategy

ProjectUltra uses adaptive waveform selection based on channel conditions:

```
SNR Range           Waveform              Why
─────────────────────────────────────────────────────────────
5-10 dB             MC-DPSK (8 carriers)  Robust sync, differential encoding
10-17 dB            OFDM-CHIRP 1024-FFT   Dual chirp sync, 59 carriers
17+ dB              OFDM-COX 1024-FFT     Schmidl-Cox sync, max throughput
25+ dB (NVIS)       OFDM 16QAM            Coherent mode, ~5.7 kbps
```

**MC-DPSK (Multi-Carrier DPSK)**: 8 carriers with differential encoding. Works reliably at 5+ dB SNR with ±50 Hz CFO tolerance. Uses dual chirp synchronization for robust timing recovery.

**D8PSK (8-Phase DPSK)**: +50% throughput over DQPSK (3 bits/symbol vs 2). This mode is currently treated as opportunistic. It performs well in AWGN and some strong fading runs, but remains more variable than DQPSK in sustained HF fading. A two-pass decoder uses the embedded DQPSK grid to estimate and correct phase drift before D8PSK decoding.

**Two OFDM sync modes**:
- **OFDM-CHIRP**: Dual chirp preamble for robust sync, works at 10+ dB
- **OFDM-COX**: Schmidl-Cox sync for faster acquisition, requires 17+ dB

**NVIS/Local optimization**: For stable paths like NVIS (Near Vertical Incidence Skywave), 16QAM with pilots provides better performance than differential modes. The pilots track slow phase drift and frequency-selective fading. Maximum throughput ~7.1 kbps with 32QAM R3/4.

---

## Getting Started

### Requirements

- Linux, macOS, or Windows
- CMake 3.16+
- SDL2 (for GUI)
- **FFTW3** (required for fast chirp detection)
- C++20 compiler (GCC 10+, Clang 12+, MSVC 2019+)

### Building

```bash
# Install dependencies
# Ubuntu/Debian: sudo apt install libsdl2-dev libfftw3-dev cmake build-essential
# macOS: brew install sdl2 fftw cmake
# Windows: vcpkg install sdl2 fftw3

git clone https://github.com/mfrigerio/ProjectUltra.git
cd ProjectUltra
mkdir build && cd build
cmake ..
make -j4
```

### Running

**GUI Application:**
```bash
./ultra_gui              # Normal mode
./ultra_gui -sim         # Simulator mode (no radio needed)
```

**CLI Tools** (individual frames, not full protocol):
```bash
# Transmit single frame
./ultra ptx "Hello World" -s MYCALL -d THEIRCALL | aplay -f FLOAT_LE -r 48000

# Decode frames from audio
arecord -f FLOAT_LE -r 48000 | ./ultra prx

# Loopback test
./ultra ptx "Test message" -s A -d B | ./ultra prx
```

> **Note**: The CLI generates/decodes individual frames. It does not support
> full protocol sessions (PING→CONNECT→DATA→DISCONNECT). For interactive
> operation, use the GUI application.

---

## How It Works

### Protocol

1. **PING/PONG** - Fast presence probe (~1 sec each) to check if remote station is listening
2. **CONNECT** - Full callsign exchange after successful probe (FCC Part 97.119 compliance)
3. **MODE_CHANGE** - Negotiates optimal modulation/coding based on measured SNR
4. **DATA** - Transfers payload with per-frame acknowledgment
5. **DISCONNECT** - Graceful termination with callsign ID (regulatory compliance)

The PING/PONG probe allows quick "anyone home?" detection before committing to the
full CONNECT sequence. If no response after 5 PINGs (15 seconds), connection fails fast.

### Signal Parameters

| Parameter | MC-DPSK | OFDM |
|-----------|---------|------|
| Sample Rate | 48 kHz | 48 kHz |
| Bandwidth | ~2.4 kHz | ~2.8 kHz |
| Center Frequency | 1500 Hz | 1500 Hz |
| Carriers | 8 | 59 |
| FFT Size | N/A | 1024 |
| Symbol Rate | ~94 baud | ~42.9 baud |
| Cyclic Prefix | N/A | 96 samples (2ms) |
| Sync Method | Dual Chirp | Dual Chirp or Schmidl-Cox |
| LDPC Codeword | 648 bits | 648 bits |

### LDPC Codes

| Rate | Info Bytes | Use Case |
|------|------------|----------|
| R1/4 | 20 | Low SNR, maximum reliability |
| R1/2 | 40 | Moderate SNR, good balance |
| R2/3 | 54 | Good SNR, higher throughput |
| R3/4 | 60 | Very good SNR |
| R5/6 | 67 | Excellent SNR, maximum throughput |

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│              GUI Application (ImGui + SDL2)             │
├─────────────────────────────────────────────────────────┤
│     Protocol Engine (Connection, ARQ, File Transfer)    │
├─────────────────────────────────────────────────────────┤
│        Modem Engine (TX/RX coordination, SNR est)       │
├──────────────────────────┬────────────────────────────┤
│       OFDM Mod/Dem       │       DPSK Mod/Dem        │
├──────────────────────────┴────────────────────────────┤
│  LDPC Encoder/Decoder  │  Interleaver  │  Sync/CFO     │
├─────────────────────────────────────────────────────────┤
│                    Audio I/O (SDL2)                     │
└─────────────────────────────────────────────────────────┘
```

---

## Testing

```bash
cd build

# Primary test - full protocol (PING/CONNECT/MODE_CHANGE/DATA/DISCONNECT)
./cli_simulator --snr 15 --fading good --rate r1_4 --test

# Test specific modulation/rate combinations
./cli_simulator --snr 20 --fading good --mod d8psk --rate r1_2 --test
./cli_simulator --snr 20 --mod dqpsk --rate r2_3 --test

# Quick single-frame sanity check (not full protocol)
./test_waveform_simple -w ofdm_chirp --snr 15

# Run unit tests
ctest
```

**CFO Verification (Internal Pre/Post Correction Chain):**

```bash
# From repository root (one-command gate)
./tests/verify_cfo_chain.sh --cfo 50 --channel awgn --snr 20 --seed 42

# Optional: capture raw TX/RX audio for offline analysis
./build/cli_simulator --snr 20 --channel awgn --waveform ofdm_chirp \
  --mod dqpsk --rate r1_2 --tx-cfo 50 \
  --save-signals 1 --save-prefix /tmp/cfo_probe --test
```

Useful `cli_simulator` flags for CFO diagnostics:
- `--tx-cfo` (alias `--cfo`) injects TX CFO in Hz across the full transmitted signal.
- `--save-signals [N]` writes raw TX/RX captures for up to `N` messages.
- `--save-prefix <path>` and `--save-max-samples <N>` control capture naming and size.

**Adaptive Advisory Smoke Test (log-only, no live MODE_CHANGE yet):**

```bash
./build/cli_simulator --adpt-test --snr 20 --channel good --waveform ofdm_chirp --seed 42
```

This test runs two message phases across two channel conditions and prints `[ADPT]` lines:
- Peer-reported advisory from CONNECT/CONNECT_ACK metrics.
- Local rolling advisory with hysteresis behavior:
  - fast downgrade path
  - delayed upgrade path (hold timer before promotion)

Useful flags:
- `--hop-snr <dB>` sets phase-B SNR (default: `12`)
- `--hop-channel <awgn|good|moderate|poor|flutter>` sets phase-B channel

**Manual Modulation Selection:**

The `--mod` flag allows testing specific modulations:
- `dqpsk` - 4-phase differential (default, 2 bits/symbol)
- `d8psk` - 8-phase differential (+50% throughput, 3 bits/symbol)
- `dbpsk` - 2-phase differential (most robust, 1 bit/symbol)

The `--rate` flag selects LDPC code rate:
- `r1_4` - Most robust (required for fading channels)
- `r1_2` - Balanced (default for AWGN)
- `r2_3`, `r3_4`, `r5_6` - Higher throughput, needs better SNR

**GUI Expert Settings:**

To force specific modulation in the GUI application:
1. Open **Settings** (gear icon)
2. Go to **Expert** tab
3. Set **Forced Modulation** to desired mode (D8PSK, DQPSK, etc.)
4. Optionally set **Forced Code Rate** (R1/4 recommended for fading)

When D8PSK is forced on a fading channel, the two-pass decoder automatically
activates to improve reliability (uses embedded DQPSK grid for phase correction).

**Test Tools:**
| Tool | Purpose |
|------|---------|
| `cli_simulator` | Primary - full protocol with two-station interaction |
| `test_waveform_simple` | Quick single-frame sanity checks |

---

## Radio Setup

### Requirements
- SSB transceiver with 2.8+ kHz filter bandwidth
- Audio interface (SignaLink, RigBlaster, or direct soundcard connection)
- PTT control (VOX, CAT, or hardware)

### Audio Levels
- TX: Adjust for clean signal without ALC compression
- RX: Set for comfortable listening level (avoid clipping)

### Recommended Operating Frequencies

ProjectUltra uses **~2.8 kHz bandwidth**. Operate in wideband digital segments,
not narrow-band FT8/PSK31 areas.

| Band | Frequency (USB) | Notes |
|------|-----------------|-------|
| 80m | 3.590 MHz | Above narrow digital, below voice |
| 40m | 7.102 MHz | Common for wideband digital |
| 30m | 10.145 MHz | Check for WSPR at 10.140 |
| 20m | 14.108 MHz | Above FT8 crowd, wideband digital |
| 15m | 21.110 MHz | Above narrow digital segment |
| 10m | 28.120 MHz | Plenty of room |

**Avoid:**
- 14.070-14.095 MHz (FT8/PSK31)
- Any .074 MHz frequencies (FT8)
- 14.100 MHz (NCDXF beacons)

**Best practice:** Listen for 10-15 seconds before transmitting. Use minimum
power necessary. Be ready to QSY if causing interference.

---

## Current Status

> **Note**: Core waveforms have been verified in loopback and acoustic testing.
> On-air HF testing is in progress.

### What Is Solid Today

| Waveform | SNR Range | CFO Tolerance | Status |
|----------|-----------|---------------|--------|
| **MC-DPSK** (8 carriers) | 5+ dB | ±50 Hz | Stable baseline |
| **OFDM-CHIRP** (59 carriers) | 10+ dB | ±50 Hz | Stable baseline |
| **OFDM-COX** (Schmidl-Cox sync) | 17+ dB | TBD | Working, OTA characterization ongoing |

Recent simulator release checks (v0.2.4-alpha):
- OFDM disconnect control path (`R1/2` data mode, `SNR=20`, good fading): 10/10 seeds pass, 0 disconnect timeouts.
- OFDM file transfer (2048 bytes, `R1/2`, `SNR=20`, good fading): 5/5 seeds pass, 0 retransmissions, 0 timeouts.

### Use With Caution (Experimental)

- D8PSK in fading: data decode can be strong, but end-to-end reliability remains more variable than DQPSK baseline.
- Coherent modes (QPSK/16QAM/32QAM): suitable for stable high-SNR paths, not the default for difficult HF fading.
- High-rate operation (`R2/3+`) still depends on channel quality and control-path stability.

### Active Work

- Control-path hardening under deep fades (ACK/SACK diversity and timeout tuning).
- Advisory hysteresis is implemented and testable in simulator logs; next step is wiring controlled live `MODE_CHANGE` policy.
- Continued OTA multi-station validation and log-driven fixes.

---

## Research Roadmap

**Goal: Exceed industry-leading HF modem speeds (currently ~8.5 kbps in 2.8 kHz bandwidth)**

ProjectUltra is a research platform for investigating advanced modulation and coding techniques for HF channels. Our target is **10+ kbps** throughput while maintaining robustness.

### Techniques Under Investigation

| Technique | Expected Gain | Status | References |
|-----------|---------------|--------|------------|
| **OTFS** | 20% SE over OFDM, full diversity | Implemented, needs HF testing | [arxiv.org/pdf/2302.14224](https://arxiv.org/pdf/2302.14224) |
| **AFDM** | >1 dB over OTFS, lower pilots | Not yet implemented | [arxiv.org/html/2507.21704v3](https://arxiv.org/html/2507.21704v3) |
| **Turbo DPSK** | 4.65 dB on Rayleigh fading | Planned | [Turbo DPSK paper](https://www.academia.edu/81703561/Turbo_DPSK_iterative_differential_PSK_demodulation_and_channel_decoding) |
| **Faster-than-Nyquist** | 25% more bits (Mazo limit) | Planned | [IEEE ComSoc overview](https://www.comsoc.org/publications/ctn/running-faster-nyquist-idea-whose-time-may-have-come) |
| **SC-LDPC** | 0.07 dB from capacity | Planned | [SC-LDPC tutorial](https://www.itsoc.org/sites/default/files/2021-03/laurent.schmalen@kit.edu%20-%20ESIT_20_Schmalen.pdf) |
| **OFDM-IM** | 2x SE via index modulation | Planned | [arxiv.org/html/2501.15437v1](https://arxiv.org/html/2501.15437v1) |

### Why These Techniques?

**OTFS/AFDM**: Traditional OFDM suffers from inter-carrier interference (ICI) when Doppler spread is high. OTFS and AFDM work in the delay-Doppler domain, achieving full diversity over doubly-selective channels. Literature shows AFDM outperforms OTFS by >1 dB with simpler channel estimation.

**Turbo DPSK**: Our single-carrier DPSK already works to -11 dB SNR. Adding iterative (turbo) processing between the DPSK demodulator and LDPC decoder could push this to -15 dB or improve throughput at higher SNR.

**Faster-than-Nyquist**: By accepting controlled intersymbol interference and using iterative detection, FTN can transmit 25% more symbols in the same bandwidth (Mazo limit: τ=0.8).

**Spatially-Coupled LDPC**: Our current LDPC codes are from IEEE 802.11n. SC-LDPC codes achieve threshold saturation, approaching capacity within 0.07 dB on fading channels.

### Current Benchmarks

| Mode | ProjectUltra | Industry Leader | Gap |
|------|--------------|-----------------|-----|
| Max throughput (verified, differential) | 3.5 kbps | 8.5 kbps | -59% |
| Max throughput (coherent, theoretical) | 7.1 kbps | 8.5 kbps | -16% |
| Low-SNR floor (verified) | 5 dB | ~0 dB | Needs work |
| CFO tolerance | ±50 Hz | ±100 Hz | TBD |

### How to Contribute

We're looking for collaborators interested in:
- Implementing AFDM modulation
- Adding turbo processing to DPSK
- Testing OTFS on real HF recordings
- SC-LDPC code design and optimization
- FTN with iterative ISI cancellation

If you have expertise in these areas, please open an issue or reach out!

---

## Contributing

Contributions welcome! Areas of interest:

- On-air testing and performance reports
- Documentation and tutorials
- Bug fixes and optimizations

Please open an issue before submitting large PRs.

### Help Wanted: Real HF Recordings

**We need real-world HF recordings to validate the modem beyond simulation.**

If you're a licensed amateur radio operator, you can help by recording your own
transmissions via WebSDR. This is legitimate amateur experimentation under
FCC Part 97 (advancement of the radio art).

**How to record your own signal:**

1. Pick a frequency from the recommended list above (e.g., 14.108 MHz USB)
2. Open a WebSDR receiver (websdr.org or kiwisdr.com) and tune to that frequency
3. Start recording on the WebSDR (or use Audacity to capture system audio)
4. Transmit using ProjectUltra (see options below)
5. Stop recording and save the file

**What to transmit:**

| Frame Type | Duration | Best For |
|------------|----------|----------|
| **PING probe** | ~1 second | Quick propagation test |
| **Full CONNECT** | ~8 seconds | Complete handshake test |

- **PING probe**: Short DPSK burst with "ULTR" marker. Fast way to check if your
  signal is reaching the WebSDR. In GUI: click Connect, recording starts immediately
  with the PING. Cancel after 2-3 seconds if you just want the probe.

- **Full CONNECT**: Complete connection attempt with callsign exchange (3 codewords).
  More comprehensive test of sync, LDPC decode, and protocol parsing.

**What to submit:**
- Recording file (48kHz mono WAV or F32 preferred)
- Your callsign and transmit location
- Band/frequency and time (UTC)
- WebSDR location used (e.g., "Twente WebSDR, Netherlands")
- Approximate path distance
- Band conditions if known (S-meter readings)
- Frame type transmitted (PING or CONNECT)

**Why this helps:**
Real ionospheric propagation has characteristics that simulation can't capture.
Even "failed" recordings where the signal didn't decode are valuable - they
help us improve sync detection and weak-signal performance.

**How to submit:** Open a GitHub issue with the "Recording" label.

---

## License

MIT License. See [LICENSE](LICENSE) for details.

---

## Acknowledgments

- [Dear ImGui](https://github.com/ocornut/imgui) - GUI framework
- [SDL2](https://libsdl.org/) - Audio and windowing
- [FFTW3](https://www.fftw.org/) - Fast Fourier Transform (required for chirp detection)
- [miniz](https://github.com/richgel999/miniz) - Compression
