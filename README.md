# ProjectUltra

**High-performance HF modem for amateur radio**

*Last updated: 2026-08-03 — current release `v0.5.2.1-pre-alpha`*

> **EXPERIMENTAL SOFTWARE — WORK IN PROGRESS**
>
> Active development. Not production-ready. APIs and protocols may change.
> Use at your own risk for experimentation and amateur-radio research.

ProjectUltra is a software modem for reliable HF data transfer. The normal
operator path is:

- **Headless TCP TNC** — `ultra_tnc` exposes the modem via the same TCP
  command/data API used by existing HF data clients. This is the primary
  on-air integration path on Linux, macOS, and Windows.
- **GUI application** — `ultra_gui` provides a local operator UI with
  waterfall, constellation, message log, and ARQ health view.

**In practice:** connect two stations over HF SSB and transfer a file byte-exact, at roughly
**1.5–2.5 kbps on a good 20 dB path**. Modulation and code rate adapt automatically as
conditions change, stepping down to a low-SNR waveform that still works around 5 dB, plus a
500 Hz narrowband mode for crowded bands.

The same repo also contains the modem core and diagnostic / lab tools
(`ota_simulator`, `measure_ack_fer`, `tools/gui_qso_scenario.sh`, raw frame CLI).
Those are for validation, profiling, and development; they are not the first-run
operator path.

[![Build Matrix](https://github.com/secup/ProjectUltra/actions/workflows/build-matrix.yml/badge.svg?branch=main)](https://github.com/secup/ProjectUltra/actions/workflows/build-matrix.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Status: Experimental](https://img.shields.io/badge/Status-Experimental-orange.svg)]()
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)]()

---

## Current state (2026-08)

- **The OFDM band is coherent.** Differential is gone from the OFDM ladder, which now runs
  QPSK R1/4 → R1/2 → R2/3 → R3/4 → 8PSK R2/3. Differential (DBPSK/DQPSK/D8PSK) lives on
  MC-DPSK, its low-SNR home. 8PSK R3/4 and the 16QAM rungs exist but ship **disabled** —
  each was measured on fading and did not hold (details in the ladder table below).
- **Rate control is outcome-fitted, not threshold-tuned.** A latent-state controller fits a
  link quality `x` from actual frame outcomes and picks the rung maximising expected
  goodput, so a common-mode calibration error cancels instead of biasing every decision.
  Default-on since `v0.5.1-pre-alpha` (+14.5% over the SNR-anchor ladder, 8 paired runs,
  p = 0.022 — its gain is *stability*: 2.2 vs 5.0 mode changes per transfer).
- **Where the throughput actually goes.** On a live ITU-Good path at 20 dB the sender is
  keyed down **~83% of wall-clock**, so turnaround is largely wrung out. Roughly 44% of the
  theoretical maximum is lost to FEC failure and repair — that, not airtime, is the
  remaining gap.

**Honest limits.** Numbers below are measured on a hardware HF channel simulator, not on
the ionosphere. No claim is made about relative performance versus other HF data modems:
that needs paired on-air testing this project has not done.

---

## Performance

ProjectUltra exposes throughput at two layers. Both matter; they answer
different questions.

### Raw PHY (theoretical maximum)

Strict raw-PHY rate: `data_carriers × bits_per_symbol × symbol_rate ×
code_rate`. No subtraction for preamble, frame header, ARQ, or ACK
turnaround — that's the ceiling the modulator could feed downstream
on a steady-state channel.

Production geometry used below:

- MC-DPSK: 8 carriers, 512 samples/symbol → 93.75 sym/s, DQPSK,
  data-mode pinned to R1/4 by `recommendDataMode()`. Adaptive ladder
  extends down to -5 dB via DBPSK + slower symbol rates
  (`docs/CHANGELOG.md` 2026-05-10).
- OFDM-CHIRP wideband: 1024-FFT, LONG CP=128 → 1152 samples/symbol,
  41.667 sym/s. 59 occupied carriers; pilot count comes from
  `recommendedPilotSpacing(mod, rate)`.
- OFDM-NARROW: 21 occupied carriers, 2240 samples/symbol → 21.429 sym/s,
  pilot spacing 10 → 18 data carriers.

**59 occupied carriers is not 59 data carriers.** Pilots are subtracted first:
`recommendedPilotSpacing()` returns 8 for R1/2–R3/4 (→ 8 pilots, **51 data**) and 5 for the
R1/4 survival band (→ 12 pilots, **47 data**). The table below is generated from the
production functions, not hand-derived.

| Rung (OFDM-CHIRP wideband) | Pilot spacing | Data carriers | Raw PHY | Ladder status |
|---|---:|---:|---:|---|
| QPSK R1/4  | 5 | 47 |  **979 bps** | entry rung |
| QPSK R1/2  | 8 | 51 | **2125 bps** | enabled |
| QPSK R2/3  | 8 | 51 | **2833 bps** | enabled |
| QPSK R3/4  | 8 | 51 | **3188 bps** | enabled (AWGN 15 / Good 20) |
| 8PSK R2/3  | 8 | 51 | **4250 bps** | enabled — top rung on Good (17 dB) |
| 8PSK R3/4  | 8 | 51 |   4781 bps  | **disabled** — 38.9% FER on Good even at 24 dB |
| 16QAM R1/2 | 8 | 51 |   4250 bps  | **disabled** — no gain over 8PSK R2/3 at equal η |
| 16QAM R2/3 | 8 | 51 |   5667 bps  | **disabled** — 51.4% FER on Good at 20 dB |
| 16QAM R3/4 | 8 | 51 |   6375 bps  | **disabled** |

Other modes: MC-DPSK adaptive ladder (8 carriers) 47–375 bps; OFDM-NARROW (18 data
carriers, 500 Hz) 193 bps at R1/4, 386 at R1/2.

> **Correction (2026-08-02).** An earlier "~2450 bps ceiling on ITU Good" circulated in this
> project's docs. It was wrong twice: it counted all 59 *occupied* carriers as payload when 8
> are pilots, and applied a stale fixed scheduling factor. True 8PSK R2/3 raw is **4250 bps**,
> so the headroom above today's delivered rate is a protocol-efficiency problem, not the
> channel limit it was described as. The measured FER data behind that document was never in
> question; only the derived throughput column was.

### End-to-end measured (real hardware)

What a user actually waits for: handshake, ACK round-trips, retransmissions and rate
changes all included.

Measured over a **hardware HF channel simulator** — real soundcards, real audio, real
half-duplex turnaround — between two machines (Raspberry Pi 5 sender → channel sim →
Mac receiver). Wall-clock from first key-down to CRC-verified completion.

**Latest: 16 consecutive 50 KB transfers, ITU Good (0.1 Hz / 0.5 ms) at 20 dB, 2026-08-02.**
Rung forced to 8PSK R2/3 to isolate the PHY from rate-selection variance:

| | |
|---|---|
| Completed byte-exact | **16 / 16** (no timeouts, no void runs) |
| Mean | **1.98 kbps** |
| Median | 2.01 kbps |
| Best / worst | **2.47** / 1.37 kbps |
| Sender duty cycle | 82.8% mean |
| End-to-end efficiency | **58.1%** of the 4250 bps rung ceiling on the best run; 46.5% at the mean |

Under the **automatic** ladder (rate control live, which is what an operator actually gets)
the same path and file measured **1.50 – 2.32 kbps** across a separate campaign. Automatic
is lower and more variable than forced because it must *find and hold* the right rung; the
forced number is the ceiling that rate control is chasing, not a user-facing figure.

**Run-to-run spread is large and irreducible** on a fading channel: two runs on *identical
binaries* over this path have differed by 0.70 kbps. Quote the distribution, not a best run.

Low-SNR MC-DPSK figures (13–81 bps at −5 to +5 dB) come from a Mac↔Pi5 USB-cable rig retired
2026-05-30 and have **not** been re-measured on the current stack. They are kept only as an
order-of-magnitude indication of the sub-OFDM floor.

The faithful in-repo gate is `tools/gui_qso_scenario.sh` (two real `ultra_gui -sim` stations
over an `ota_simulator serve` channel). Anything claiming fade, throughput, or
full-protocol behaviour goes through it or the hardware path — never a unit test.

### Features

**Waveforms.** MC-DPSK (chirp sync, low-SNR / heavy-multipath
workhorse — also the home for differential DQPSK/D8PSK), OFDM-CHIRP
(wideband 2.8 kHz, 59 carriers, coherent QPSK → 16QAM), OFDM-NARROW
(500 Hz crowded-band mode), SC-DPSK (very low SNR). (OFDM-COX was
removed — only its Schmidl-Cox sync primitive survives, reused for
warm-LTS in-session sync.)

**Modulation + FEC.** Coherent QPSK / 16QAM on the OFDM band;
differential DBPSK / DQPSK / D8PSK on MC-DPSK. 802.11n LDPC at four
code rates (R1/4, R1/2, R2/3, R3/4) with a min-sum belief-propagation
decoder.

**Synchronization.** Dual-chirp detection with PocketFFT-accelerated
correlation, Schmidl-Cox training, light-preamble (LTS-only) for
in-session frames, LTS-residual CFO refinement, per-symbol pilot
tracking with common-phase-error correction.

**ARQ.** Selective-repeat with cumulative + selective ACKs. Window is
per-mode: MC-DPSK uses a timing-derived 1–5, OFDM-NARROW 3, wideband
OFDM 8 (up to 16 on near-AWGN). Variable 1–8 codeword frames with
wire-negotiated CW count; burst transport is the wideband file path.
Frame interleaving is **default-off** — it spreads each codeword across
every frame in a burst, so one time-localized fade craters the whole
group instead of a single frame (measured: 5x fewer full craters off).

**Adaptive rate.** A **latent-state controller** (default-on since `v0.5.1-pre-alpha`) fits
a single link-quality value `x` from observed frame outcomes and evaluates
`P_success(rung) = f(x − θ_rung)` for *every* rung, then picks the stateless argmax on
expected goodput. Because all rungs are scored against the same `x`, a common-mode
calibration error cancels rather than biasing every decision — the failure mode of the
SNR-threshold ladder it replaced. Per-rung `θ` comes from a measured FER waterfall, and
correlated fading is handled by tempering each 5-frame group to ~2.2 effective independent
observations. `ULTRA_LATENT_RATE=0` restores the legacy SNR-anchor ladder in
`selectOFDMCodeRate()`.

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

**OTASim — network HF channel server.** Long-running daemon
(`ota_simulator serve`) that hosts the HF medium in software:
multiple clients join a session, audio flows over gRPC (control) +
UDP (samples), and the channel model is applied server-side.
`ultra_gui` and `ultra_tnc` can both run as OTASim
clients instead of opening a soundcard, so protocol/feature work
needs no cable rig and no audio hardware. The server holds a
session reference clock that bridges per-host audio-clock drift,
so cross-machine runs (e.g. Mac ↔ Pi5 ↔ Windows over LAN or VPN)
behave as a single timeline.

**Protocol v2.** PING / PONG, CONNECT / CONNECT_ACK,
MODE_CHANGE, DATA, ACK / SACK, DISCONNECT. Wire-level CRC-16
on every frame, capability flags, measured-SNR + fading-index
exchange.

**TNC integration.** `ultra_tnc` daemon exposes the modem over
the same TCP command/data API used by existing HF data clients
(cmd port 8300 / data port 8301); verified end-to-end with
real client sessions across all major B2F message types.

**GUI application.** `ultra_gui` with real-time waterfall,
constellation, message log, and ARQ health view (ImGui +
SDL2). Virtual-station / simulator mode for development.

### Waveform selection (automatic)

```
SNR         Waveform              Reason
─────────────────────────────────────────────────────────────────────
-5 to +7 dB MC-DPSK (auto-rung)   DBPSK→DQPSK + variable SPS, adaptive
5–10 dB     OFDM-NARROW (500 Hz)  Crowded bands, low SNR
8+ dB       OFDM-CHIRP (1024)     Production auto ladder, coherent
              ├─ QPSK R1/4 → R3/4   the working range
              └─ 8PSK R2/3          top enabled rung (Good ≥17 dB)
forced only 8PSK R3/4, 16QAM      measured on fading, did not hold — disabled
```

Wideband entry floors are AWGN 8 / Good 8 / Moderate 14 dB (Poor disabled), lowered from
10/12/14/18 on measured sweeps. In-session the latent-state controller takes over from the
entry pick and re-decides per burst group from observed frame outcomes.

Selection happens during CONNECT (peer-advertised SNR + fading index)
and continues adapting during the session. See `docs/PROJECT_GOALS.md`
for the throughput/reliability targets driving this work.

---

## TNC Integration

`ultra_tnc` is a daemon that exposes ProjectUltra's modem through a
legacy-compatible HF TCP TNC command/data interface. Existing clients
can connect to it the same way they connect to a commercial HF data
modem, with no protocol changes on the client side.

```
┌──────────────┐  TCP 8300 (cmd)   ┌──────────────┐
│  HF data     │  TCP 8301 (data)  │  ultra_tnc   │  Audio   ┌─────────┐
│  client /    │ ◄──────────────► │  (modem +    │ ◄──────► │  HF     │
│  app         │                   │   TCP shell) │          │  Radio  │
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
# -> VERSION 0.5.2
```

Full command reference: [`docs/TNC_INTERFACE.md`](docs/TNC_INTERFACE.md).

### Supported commands

Standard TNC shell: `VERSION`, `MYCALL`, `LISTEN`, `CONNECT`, `DISCONNECT`,
`ABORT`, `BW500` / `BW2300` / `BW2750`, `BUFFER`, `SN`, `BITRATE`,
`COMPRESSION`, `CHAT`, `CWID`, plus legacy-client compatibility
no-ops (`PUBLIC`, `P2P`, client-mode probes, `IGNOREKISSDCD`,
`RETRIES`, `CALLINT`).

ProjectUltra extension:

- **`STATS`** — single-line ARQ + PHY snapshot for debugging stalled
  sessions: `frames_sent`, `frames_recv`, `retx`, `timeouts`, `failed`,
  `out_of_order`, current `rate` / `mod` / `mode`, `snr`, `bps`,
  `backlog`. Existing clients ignore unknown commands, so this is safe
  to leave on.

### Status

- Cross-platform: Linux + macOS + Windows. CI matrix all green.
- ctest: **101/101**, green on Linux, macOS and Windows, plus a
  sanitizer and a coverage gate (`v0.5.2.1-pre-alpha`, CI run 30778355854).
  Covers the TNC parser, TCP integration, bridge tests, and the modem
  regressions including the `CarrierLDPC v1` math gate and per-carrier
  mask plumbing.
- End-to-end byte-exact transfers over the hardware channel simulator:
  **16/16** at 50 KB on ITU Good @20 dB, mean 1.98 kbps — see the
  throughput table above.
- **Real HF data client validated end-to-end** across Mac and Pi5
  over real audio cable: full B2F session
  matrix passes byte-exact (empty connect/disconnect, text up
  to 12.5 KB, binary attachments, bidirectional, both
  directions). Five real bugs found + fixed during integration;
  full audit at `docs/TNC_CLIENT_AUDIT.md`.
- Known TNC limitation: back-to-back sessions within ~30 s of
  teardown don't always recover cleanly (~1/3 retry success).
  Root cause traced to a reference-client listener race where
  inbound connections can arrive before `Accept()` has re-armed.
  Single-session flows are reliable.
- Mainstream Windows HF mail client: spec-compatible, not yet
  manually tested.

---

## Getting Started

### Alpha operator bundle

For a release build, download `projectultra-<platform>.zip` from the
GitHub release assets. That is the operator bundle: `ultra_tnc`,
`ultra_gui`, `ultra`, `tools/ultra_tnc.conf.example`, and operator
docs. Do not use the source-code archive as the operator download.
Simulator and bench binaries are published separately as
`dev-tools-<platform>.zip`.

### Requirements

- Linux, macOS, or Windows
- CMake 3.16+
- C++20 compiler (GCC 10+, Clang 12+, MSVC 2019+)
- SDL2 (GUI + audio I/O for `ultra_gui` / `ultra_tnc`)
- gRPC + Protobuf + Abseil + c-ares + OpenSSL + RE2 + zlib
  (network audio plane for `ota_simulator` and the OTASim client modes
  of `ultra_gui` / `ultra_tnc`)

### Building

```bash
# Ubuntu/Debian
sudo apt install \
  libsdl2-dev cmake build-essential pkg-config \
  libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
  libabsl-dev libc-ares-dev libssl-dev libre2-dev zlib1g-dev

# macOS
brew install sdl2 cmake pkg-config grpc protobuf abseil c-ares

# Windows (vcpkg)
vcpkg install sdl2:x64-windows grpc:x64-windows

git clone https://github.com/secup/ProjectUltra.git
cd ProjectUltra
cmake -S . -B build
cmake --build build -j 4
```

On Windows, point CMake at the vcpkg toolchain:

```powershell
cmake -S . -B build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build -j 4 --config Release
```

### Running

#### Operator path

**TNC** (headless TCP shell for existing HF data clients):

```bash
./build/ultra_tnc --audio-output "USB Audio" --audio-input "USB Audio"
```

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

#### Diagnostic / lab tools

**OTASim server** (network HF channel medium — multiple clients share a
session, audio flows over gRPC + UDP, channel model is server-side):

```bash
# Token allowlist file: one `<token>:<station_id>:<description>` per line.
cat >/tmp/ota_tokens.conf <<EOF
alice_token:ALPHA:Alpha station
bob_token:BRAVO:Bravo station
EOF

# Long-running daemon — bind to LAN or localhost.
./build/ota_simulator serve \
  --bind 0.0.0.0:50051 --udp-bind 0.0.0.0:50052 \
  --tokens /tmp/ota_tokens.conf

# Clients (any combination, same or different hosts):
./build/ultra_gui  -sim --ota-host <server>:50051 --station-id ALPHA --token alice_token
./build/ultra_tnc       --sim-audio --ota-host <server>:50051 --station-id BRAVO --token bob_token
```

Use OTASim instead of the cable rig for protocol/feature work: no
soundcard needed, fully portable, and the server-side reference clock
bridges per-host audio-clock drift across multiple machines.

**Two-station scenario gate** (full protocol, real stations, channel injection;
not the operator TNC):

```bash
tools/gui_qso_scenario.sh --channel good --snr-db 20 --seed 42 --file-kb 21 --out /tmp/X
```

**Replay and capture analysis** (deterministic decode fixtures and recorded
sessions):

```bash
./build/decode_bench --mode bench --connected \
  --wav fixtures/ofdm_chirp_r14_dqpsk_clean.wav --rate r1_4
./build/session_decode --wav \
  recordings/ota_full_session_2026-05-07/full_session_r1_2.wav
```

**Raw frame CLI** (single-frame transmit/decode, for offline analysis):

```bash
./build/ultra ptx "Hello" -s MYCALL -d THEIRCALL | aplay -f FLOAT_LE -r 48000
arecord -f FLOAT_LE -r 48000 | ./build/ultra prx
```

---

## How It Works

### Protocol stack

```
┌────────────────────────────────────────────────────────┐
│  HF data client / mail app / your own client           │
├────────────────────────────────────────────────────────┤
│  ultra_tnc      (TCP cmd 8300, data 8301)              │
│  TNCSession     (TNC command parser + state machine)   │
│  TNCBridge      (ModemAdapter ↔ ProtocolEngine)        │
├────────────────────────────────────────────────────────┤
│  Connection     (PING/CONNECT/MODE_CHANGE/DATA/DISC)   │
│  ARQ            (Selective-Repeat, window=16, SACKs)   │
│  Frame v2       (4-CW fixed frames + 1-CW control)     │
├────────────────────────────────────────────────────────┤
│  Waveforms      (OFDM-CHIRP, OFDM-NARROW, OFDM-COX,    │
│                  MC-DPSK + adaptive selection)         │
│  LDPC           (IEEE 802.11n; data CLI R1/4 to R3/4)  │
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
| Symbol rate      | ~94 baud   | 41.667 baud |
| Cyclic prefix    | n/a        | 128 (2.667 ms) |
| Sync             | Dual chirp | Dual chirp / Schmidl-Cox |
| LDPC codeword    | 648 bits   | 648 bits   |

---

## Testing

### Measuring throughput (read before quoting a number)

A fading channel is a noisy measurement instrument, and this project has retracted claims
for ignoring that. The rules that survived contact with the hardware:

- **Paired and interleaved.** Run arm A and arm B back to back and compare the per-pair
  delta. The channel drifts; comparing arm means across epochs has read −10% purely from a
  rougher epoch.
- **n ≥ 8 pairs.** Paired standard deviation here is 14–36%, so n=8 resolves about 15% and
  **n=3 resolves nothing**. Single runs are observations, not evidence.
- **Report both tests.** A paired t-test and a sign test, and say so when they disagree.
- **Prove the knob engaged.** Grep the log for it. A knob that silently did not engage has
  produced confident null results more than once.
- **Report the mechanism, not just goodput** — craters, mode changes, retransmits. Goodput
  alone has called levers a "wash" that mechanism metrics then decided.

### Unit + regression gate

```bash
cmake --build build -j 4
ctest --test-dir build --output-on-failure -j 4
```

The suite covers modem primitives, protocol/ARQ, TNC parser, TNC TCP
reactor, and TNC bridge. CI runs the full matrix on Linux + macOS + Windows
with ASAN/UBSAN and coverage gates.

Hardware smoke remains opt-in. Configure with either
`ULTRA_HARDWARE_TESTS=1 cmake -S . -B build-hw` or
`cmake -S . -B build-hw -DULTRA_BUILD_HARDWARE_TESTS=ON`, then run:

```bash
ctest --test-dir build-hw -R HardwareSmoke --output-on-failure
```

### Full-protocol gate

Two real `ultra_gui -sim` stations over a live `ota_simulator serve` channel:
PING/PONG → CONNECT → MODE_CHANGE → file transfer → DISCONNECT, in real time.
This is the ONLY gate that backs a fade, throughput, or full-protocol claim.

```bash
# Full handshake + ARQ file transfer + disconnect; reads RESULT/GOODPUT_BPS from summary.env
tools/gui_qso_scenario.sh --channel good --snr-db 20 --seed 42 \
    --expect-rate R3/4 --file-kb 21 --out /tmp/X

# Force a specific PHY rung (measurement only — these bypass rate control)
ULTRA_FORCE_DATA_MOD=QAM8 ULTRA_FORCE_DATA_RATE=R2_3 ULTRA_LOCK_RATE=1 \
  tools/gui_qso_scenario.sh --channel good --snr-db 20 --out /tmp/X

# CFO chain verification
./tests/verify_cfo_chain.sh --cfo 50 --channel awgn --snr 20 --seed 42
```

(`cli_simulator` was retired 2026-05-30: it was CPU-paced and wrapped a PHY that
had diverged from production, so it produced misleading passes.)

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

`--mod`: operator tools expose `auto` / `dqpsk` by default. Lab-only
forced modes (`d8psk`, `dbpsk`, `qpsk`, `bpsk`, `qam16`, `qam32`,
`qam64`) require `--expert` or `ULTRA_EXPERT_PHY=1` and are not
production ladder rungs.

`--rate`: `auto` (default), `r1_4`, `r1_2`, `r2_3`, `r3_4`. The
operator parsers intentionally reject higher LDPC rates until they are
part of the maintained on-air ladder.

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

- MC-DPSK baseline (5+ dB SNR, ±50 Hz CFO tolerance) — also the
  home for differential DQPSK/D8PSK after the coherent-only move.
- OFDM-CHIRP wideband PHY pipeline: dual-chirp sync, LTS warm-sync,
  per-symbol pilot tracking, adaptive-rate ladder
  (`selectCoherentOFDM()` is the rung source). The band is now
  coherent (QPSK → 8PSK → 16QAM), with rungs anchored on the
  faithful gate — see *Current state*.
- OFDM-NARROW (500 Hz) for crowded bands or low-SNR conditions.
- Per-carrier RX erasure with CarrierLDPC v1 interleaver
  (deep notch / QRM survival on OFDM-CHIRP).
- Adaptive MODE_CHANGE with hysteresis (BUG-RATE-001 fixed:
  no panic-downshift on short fading transfers).
- Selective-repeat ARQ with cumulative + selective ACKs,
  hardened DISCONNECT, no timeout storms.
- TNC subsystem: cross-platform Linux / macOS / Windows,
  byte-exact end-to-end, validated with real HF data client sessions.
- Hardware-in-the-loop test rig (Mac ↔ Pi5 with Watterson
  injection) with byte-exact file-transfer validation.
- **First OTA full-session decode (2026-05-07):** full
  chirp + CONNECT + DATA + DISCONNECT sessions decoded from
  Pennsylvania TX audio captured by the Vermont KiwiSDR at
  `recordings/ota_capture_2026-05-07_k1vl/`. R1/4 and R1/2
  completed handshake and byte-exact DATA decode; R3/4 captured
  chirp but lost the handshake, matching the current auto-rate
  exclusion for fading channels.

### Experimental (in tree, not on by default)

- HARQ Chase soft-combining: math validated, infrastructure
  in place; broader CW0-fail integration is on
  `experimental/harq-audit-2026-05-06` pending further
  hardware validation.
- 16QAM as a production OFDM rung: the constellation works, but its
  clean-channel anchors (AWGN + stable paths) are still being
  measured before it joins the auto ladder. On Good fading it
  currently loses to QPSK R3/4 (frequency-selective-null
  decodability), so it stays a forceable rung for now.
- Lightweight tone-burst ACK: detector + encoder landed; wiring it
  as the authoritative low-latency ACK source is in progress (part
  of the airtime-efficiency push).

### Active work

- **Re-anchoring the coherent rate ladder by measurement.** The
  Good-fading column is done (QPSK R1/2 @ 10 dB → R2/3 @ 15 dB →
  R3/4 @ 20 dB, multi-seed CRC-verified); the AWGN clean-channel and
  Moderate columns are next, plus where 16QAM earns a rung.
- **Airtime efficiency.** Closing the gap between measured goodput
  and each rung's information-theoretic ceiling — fewer turnarounds,
  leaner tone-burst ACKs, lower per-frame overhead.
- Long-running stability soak (multi-hour `ultra_tnc` uptime).
- Real over-the-air validation expansion: the 2026-05-07 KiwiSDR
  replay path works for full sessions; the remaining work is broader
  live two-way coverage and better low-SNR OTA margins.

### Deliberately deferred

- Highest-order constellations (32/64-QAM) as production ladder
  rungs: enum reserved, no production code. Real capacity headroom
  but needs proper EVM gating, coherent phase tracking, and
  IQ-imbalance handling — multi-week scope. (16QAM is a step closer
  — see *Experimental*.)
- Iterative LDPC ↔ equalizer (turbo) loops.
- OTFS / MFSK: enum reserved for wire compatibility, no
  production implementation.
- Code-sign + notarization for prebuilt macOS binaries
  (Apple Developer ID needed; on the post-alpha roadmap).

Active engineering goal lives in `docs/PROJECT_GOALS.md`.
Recent design + audit notes are in `docs/CHANGELOG.md`,
`docs/PHASE2_CARRIER_MASK_DESIGN.md`, and the
historical reports under `docs/archive/`. Speculative /
archived research is historical only, not part of the
production build.

---

## Contributing

Contributions welcome. Easiest entry points:

- On-air testing reports (especially with `STATS` output included).
- HF data client interop reports - what worked, what did not.
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
- [PocketFFT](https://github.com/mreineck/pocketfft) — Fast Fourier Transform.
- [miniz](https://github.com/richgel999/miniz) — compression.
