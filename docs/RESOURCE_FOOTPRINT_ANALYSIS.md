# Resource Footprint Analysis

Date: 2026-05-08
Branch: experimental/cleanup-overnight-2026-05-07
Scope: read-only footprint review for running ProjectUltra below a Pi 5 class target.

This is not a PHY tuning proposal. The modem core remains read-only unless a later
round has a specific hardware-validated reason.

## Evidence Commands

- `size -m build/cli_simulator build/ultra_gui build/ultra_tnc build/session_decode build/decode_bench`
- `/usr/bin/time -l ./build/cli_simulator --snr 15 --channel awgn --rate r1_2 --file 1024 --save-signals --save-prefix /tmp/round7_awgn`
- `ps -o pid,rss,etime,command -p <cli_simulator_pid>`
- `vmmap -summary <cli_simulator_pid>`
- `./build/profile_acquisition --trials 3 --snr-ofdm 25 --snr-dpsk 10 --snr-ping 10`
- `pmap` is not available on this macOS host; `vmmap -summary` is the local equivalent used here.

No `/tmp/*.profile` files were present during this pass.

## 1. Per-Component Static And Heap Memory Budget

| Component | Evidence | Budget today | Runtime count | Footprint read |
| --- | --- | ---: | ---: | --- |
| `StreamingDecoder` audio ring | `src/gui/modem/streaming_decoder.cpp:138-149`, `src/gui/modem/streaming_decoder.hpp:338-342`, `src/gui/modem/streaming_decoder.hpp:454-461` | `480000` floats = 1.92 MB per instance | 1 per `ModemEngine`; 2 in `cli_simulator`; GUI can have real + virtual modem | This is the largest intentional per-instance fixed heap block. It is sized for 10 s of audio, not for the 10 ms callback. |
| `StreamingDecoder` waveform/FEC state | `src/gui/modem/streaming_decoder.hpp:366-393`, `src/gui/modem/streaming_decoder.hpp:431-445` | small fixed objects plus burst vectors; burst buffers scale with group size and soft-bit count | same as decoder count | Fine on Linux SBCs; not microcontroller-compatible without redesign. |
| `AudioEngine` TX queue | `src/gui/audio_engine.hpp:144-150`, `src/gui/audio_engine.cpp:198-221`, `src/gui/audio_engine.cpp:331-357` | unbounded `std::queue<float>` samples | GUI/TNC/hardware CLI audio path | Wasteful: per-sample node/deque traffic instead of block ring writes. Runtime risk is latency/jitter, not raw MB at steady state. |
| `AudioEngine` RX queue | `src/gui/audio_engine.hpp:148-176`, `src/gui/audio_engine.cpp:387-438` | capped at `96000` floats = 384 KB; callback allocates `std::vector<float>` per input block | GUI/TNC/hardware CLI audio path | The cap is sane; per-callback allocation and `erase(begin, ...)` are avoidable. |
| LDPC decoder matrices | `src/fec/ldpc_decoder.cpp:22-36`, `src/fec/ldpc_decoder.cpp:41-63`, `src/fec/ldpc_decoder.cpp:105-183` | sparse rows/cols plus message buffers; per decoder instance per current rate | one codec in each decoder plus encoder-side codec state | Small on SBCs. The expensive transient is encoder matrix derivation. |
| LDPC 802.11n expanded matrices | `src/fec/ldpc_802_11n.hpp:21-24`, `src/fec/ldpc_802_11n.hpp:111-123`, `src/fec/ldpc_802_11n.hpp:156-195` | N=648. Expanded edges: R1/4 1971, R1/2/R2/3/R3/4/R5/6 2376. Encoder Gaussian-elim temp is documented as about 420 KB max | built when codec/matrix is constructed | Acceptable on Pi-class RAM; not acceptable for RP2040/ESP32 without static compact tables. |
| FFT state | `src/dsp/fft.cpp:13-36`, `src/dsp/fft.cpp:42-89`, `src/dsp/fft.cpp:94-128`, `src/dsp/fft.cpp:142-169` | FFTW: plans + 1024 complex + 1024 real buffers. Fallback: 512 twiddles + 1024 work complex = about 12 KB for 1024 FFT | modulator/demodulator/waveforms | Memory is modest; fallback CPU is the concern. Vector overloads resize outputs in hot paths. |
| OFDM modulator state | `include/ultra/types.hpp:226-241`, `src/ofdm/modulator.cpp:110-127`, `src/ofdm/modulator.cpp:143-207`, `src/ofdm/modulator.cpp:231-302` | carrier/pilot/sync vectors plus FFT; allocates `freq_domain`, `time_domain`, CP vector, and real vector per symbol/path | one per active waveform/encoder | The repeated vectors are a CPU/cache allocation hotspot. |
| OFDM demod/equalizer state | `src/ofdm/demodulator.cpp:64-81`, `src/ofdm/demodulator.cpp:116-181`, `src/ofdm/channel_equalizer.cpp:1282-1420` | several `fft_size` vectors: channel estimate, LMS weights, last decisions, RLS P, LTS templates | one per active waveform/decoder | Memory is fine on SBCs. Equalizer work and sync search are the CPU budget items. |
| Protocol/ARQ buffers | `src/protocol/selective_repeat_arq.hpp:141-172`, `src/protocol/selective_repeat_arq.hpp:182-214`, `src/protocol/protocol_engine.hpp:197-202`, `src/protocol/file_transfer.hpp:155-179`, `src/protocol/connection.hpp:292-307`, `src/fec/soft_combine.hpp:96-101` | ARQ window cap 16 TX/RX slots; file transfer stores whole TX/RX file; HARQ soft-combine disabled by default, max 32 entries | one `Connection` per station | Protocol memory is bounded except file-transfer payload size. This is fine for SBCs, not for small MCUs. |
| Two-station simulator heap | `tools/sim/simulated_station.hpp:447-485`, `tools/sim/simulated_station.hpp:632-679`, `tools/sim/simulated_station.hpp:1366-1438` | two decoders = 3.84 MB rings, two TX sample queues, two protocol stacks, channel queues | 2 stations | Measured RSS 40.2 MB at 8 s, 42.1 MB at 14 s; `/usr/bin/time -l` peak RSS 108.5 MB with signal capture enabled. |

Static binary size on macOS ARM64 with FFTW/SDL:

- `cli_simulator`: `__TEXT` 704512 bytes, `__DATA` 16384 bytes.
- `ultra_tnc`: `__TEXT` 671744 bytes, `__DATA` 16384 bytes.
- `session_decode`: `__TEXT` 622592 bytes, `__DATA` 16384 bytes.
- `decode_bench`: `__TEXT` 442368 bytes, `__DATA` 16384 bytes.
- `ultra_gui`: `__TEXT` 1474560 bytes, `__DATA` 32768 bytes.

`vmmap -summary` on `cli_simulator --file 1024` at 10 s: physical footprint
33.0 MB, writable resident 32.2 MB, default malloc allocated 14.3 MB with 55%
fragmentation, 5 stack regions. This aligns with `ps` RSS samples around
40-42 MB without capture.

## 2. Thread Map

| Runtime | Threads launched by code | Evidence | Role | Foldability |
| --- | ---: | --- | --- | --- |
| One `ModemEngine` | 0 until audio arrives, then 1 RX decode thread | `src/gui/modem/modem_engine.cpp:138-140`, `src/gui/modem/modem_rx.cpp:55-60`, `src/gui/modem/modem_rx.cpp:88-137`, `src/gui/modem/modem_rx.cpp:143-149` | Blocks in `StreamingDecoder::processBuffer()`, keeps decode off audio callback | Foldable only in synchronous/offline tools; risky for live audio. |
| `cli_simulator` two-station mode | 4 station threads: audio + decode for ALPHA, audio + decode for BRAVO | `tools/sim/simulated_station.hpp:476-485`, `tools/sim/simulated_station.hpp:649-651`, `tools/sim/simulated_station.hpp:1366-1449` | Audio loop runs 480 samples every 10 ms; decode loop blocks independently | Audio+decode folding is possible for offline speed, not for hardware-realistic pacing. |
| GUI | main UI thread + real `AudioEngine` callbacks + one `ModemEngine` decode thread; simulator adds one `sim_thread_` and a virtual modem decode thread | `src/gui/app.hpp:71-73`, `src/gui/app.hpp:176-200`, `src/gui/app.cpp:1233-1241`, `src/gui/app.cpp:1271-1284` | UI/render, audio callback, decode, optional virtual simulator loop | GUI should stay out of small-headless targets. |
| TNC/hardware CLI | SDL audio callbacks + one modem decode thread + socket/server reactor threads from TNC stack | `src/gui/audio_engine.cpp:331-357`, `src/gui/audio_engine.cpp:359-383`, `src/gui/modem/modem_rx.cpp:55-60` | real-time audio plus host-side API | Keep audio/decode split. Fold only socket/control paths if profiling proves thread overhead. |

The hard real-time budget remains 480 samples per 10 ms callback. Decode can
run longer than 10 ms if buffering absorbs it, but callback allocations or locks
must stay boring.

## 3. CPU Hot-Path Budget

`tools/profile_acquisition.cpp` defines profile result fields and 480-sample
chunk feeding at `tools/profile_acquisition.cpp:25-31`,
`tools/profile_acquisition.cpp:99-130`, and CLI options at
`tools/profile_acquisition.cpp:414-424`. Current run result: all 3 OFDM,
3 DPSK, and 3 PING trials failed, so this tool is stale as a decode benchmark.

Maintained evidence comes from `cli_simulator --snr 15 --channel awgn --rate r1_2 --file 1024`.
It passed byte-exact file transfer. `/usr/bin/time -l` with signal capture:
22.36 s real, 0.70 s user, 0.19 s sys, 1.75e9 cycles elapsed, 7.11e9
instructions retired, 108.5 MB max RSS.

No Pi 5 hardware cycle profile was available in this checkout. The table below
uses measured wall times from the maintained simulator and converts to a Pi 5
2.4 GHz equivalent for order-of-magnitude budgeting. One 10 ms callback is
about 24 million cycles on one Pi 5 core.

| Hot path | Measured bucket | Pi 5 equivalent | 10 ms callback budget | Read |
| --- | ---: | ---: | ---: | --- |
| Chirp/data sync search | `detect_data_sync` mean 2.9 ms, max 6.5 ms | mean 7.0M cycles, max 15.5M | 29-65% | Top spike on slower cores. |
| OFDM frame processing | `ofdm_process_total` mean 1.6 ms, max 4.2 ms | mean 3.8M, max 10.0M | 16-42% | Healthy on Pi 5; watch A53/A8. |
| Data symbol loop/equalizer | `data_symbol_loop` mean 64 us, max 197 us per symbol-loop call | mean 154K, max 473K | <2% | Not the first bottleneck today. |
| Fixed-frame decode | `decode_fixed_frame_total` mean 2.4 ms, max 10.5 ms | mean 5.7M, max 25.1M | 24-105% | Worst case can exceed one callback; async thread tolerates it. |
| 1-CW control decode | `single_cw_decode_total` mean 6.0 ms, max 20.0 ms | mean 14.5M, max 47.9M | 60-200% | Largest latency spike. The profile labels control-first retries as the culprit. |
| LDPC codeword core | `ldpc_cw_total` mean 20 us, max 39 us | mean 48K, max 94K | <1% | LDPC core is not the current CPU villain. Wrapper/probe/retry path is. |

The three paths most likely to spike on cheaper CPUs are sync correlation,
control-frame 1-CW retry/probe decode, and per-symbol OFDM allocation/cache churn.

## 4. Cheaper Hardware Feasibility

| Target | CPU/RAM/audio constraints | Required changes | Verdict |
| --- | --- | --- | --- |
| Pi Zero 2W | 1 GHz quad Cortex-A53, 512 MB, Linux audio scheduling weaker than Pi 5 | Headless build, no GUI, ring-buffer audio queues, profile gate on sync/control decode, prefer FFTW or NEON path | FEASIBLE WITH WORK |
| OrangePi Zero 3 | 1.5 GHz quad Cortex-A53, 1 GB, USB/I2S audio varies by board | Headless build and queue cleanup; validate audio device latency | FEASIBLE |
| BeagleBone Black/Green | 1 GHz single Cortex-A8, 512 MB, old CPU, practical audio I/O exists | Must cut sync/control spikes, remove per-callback allocations, keep FFTW/fixed-point/NEON expectations realistic | FEASIBLE WITH MAJOR WORK |
| RP2040 + Pico SDK | 133 MHz dual M0+, 264 KB RAM, no POSIX threads/STL/audio stack | Rewrite modem/runtime: no current OFDM/LDPC stack, no 1.92 MB decoder ring, static tables only | NOT FEASIBLE |
| ESP32-S3 | 240 MHz dual LX7, about 512 KB internal SRAM class, RTOS/audio I2S possible | Different modem/runtime. Current C++ heap/thread/LDPC/OFDM footprint does not fit | NOT FEASIBLE |
| x86 mini PC | Modern Atom/Celeron, 4 GB+, mature USB audio | No footprint blocker | FEASIBLE |

## 5. Top 5 Footprint Reductions Worth The Effort

1. Replace per-sample queues with bounded block ring buffers.
   Evidence: `src/gui/audio_engine.hpp:144-150`, `src/gui/audio_engine.cpp:198-221`,
   `src/gui/audio_engine.cpp:331-357`, `tools/sim/simulated_station.hpp:655-657`,
   `tools/sim/simulated_station.hpp:1399-1415`.
   Good: contiguous 480-sample block push/pop, fixed capacity, no per-sample queue churn.
   Effort: 1-2 days. Risk: medium; audio-path hardware smoke required.

2. Ship a headless modem target separate from GUI/TNC.
   Evidence: GUI `__TEXT` 1.47 MB vs `cli_simulator` 0.70 MB; GUI owns real
   `AudioEngine` + `ModemEngine` at `src/gui/app.hpp:71-73` and virtual modem
   simulator state at `src/gui/app.hpp:176-200`.
   Good: a minimal SBC daemon without ImGui/OpenGL/virtual simulator.
   Effort: hours to 1 day. Risk: low if target-only.

3. Make `StreamingDecoder` ring size target-configurable.
   Evidence: 10 s ring at `src/gui/modem/streaming_decoder.hpp:454-461`;
   allocation at `src/gui/modem/streaming_decoder.cpp:138-149`.
   Good: headless/SBC profile uses 3-5 s unless acquisition tests prove 10 s is needed.
   Effort: 1 day. Risk: medium; sync/acquisition/hardware smoke required.

4. Preallocate OFDM symbol and FFT vectors.
   Evidence: per-symbol vectors in `src/ofdm/modulator.cpp:231-302`; FFT vector
   overload resizes at `src/dsp/fft.cpp:142-169`.
   Good: no allocation in OFDM hot loops; reuse scratch buffers per modulator/demodulator.
   Effort: 1-2 days. Risk: medium; modem core, so only with focused justification.

5. Replace stale `profile_acquisition` with a maintained profile gate.
   Evidence: current profile run failed every mode; source still feeds 480-sample
   chunks in `tools/profile_acquisition.cpp:99-130` and reports result fields at
   `tools/profile_acquisition.cpp:25-31`.
   Good: non-CTest or nightly budget gate around the actual `cli_simulator` /
   `session_decode` profile buckets.
   Effort: hours to 1 day. Risk: low-medium; tool-only if it reuses existing profile output.

## Bottom Line

Sub-Pi 5 Linux SBCs are plausible. The current modem is not RAM-heavy for any
512 MB Linux board, but the simulator and GUI carry unnecessary heap churn and
thread count. The limiting factor below Pi 5 is CPU latency in sync/control
decode plus audio callback hygiene, not LDPC table memory. Microcontrollers are
out of scope for this codebase without a rewrite.
