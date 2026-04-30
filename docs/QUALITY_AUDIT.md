# ProjectUltra Quality Audit

Last updated: 2026-04-30

## Purpose

This is the current quality baseline and hardening backlog. `QUALITY_STRATEGY.md`
defines the policy; this file tracks where the codebase stands against it.

## Current Test Baseline

Registered CTest targets: 23

Current maintained local gates:

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
./tests/regression_matrix.sh --quick
./scripts/coverage_report.sh
git diff --check
```

Latest measured coverage after the current hardening pass:
- Line: 52.93%
- Function: 57.42%
- Branch: 42.04%

This is a baseline, not an acceptable final state for critical modem code.

## Tier 0 Coverage Snapshot

Measured from `build-coverage/coverage.txt`.

| Area | Line Coverage | Function Coverage | Branch Coverage | Assessment |
|------|---------------|-------------------|-----------------|------------|
| `src/fec` | 81.09% | 86.42% | 76.20% | Stronger after codec wrapper/factory tests; LDPC internals still need edge cases |
| `src/ofdm` | 57.74% | 56.57% | 49.72% | Improved by waveform loopback; sync/equalizer branches still weak |
| `src/protocol/frame_v2.cpp` | 53.77% | 68.92% | 32.85% | Edge cases improved; fixed-frame recovery paths still weak |
| `src/protocol/frame_v2.hpp` | 89.29% | 95.24% | 76.42% | Strong helper coverage; keep malformed-frame tests growing |
| `src/protocol/connection.cpp` | 50.30% | 65.22% | 34.14% | Needs state-machine extraction and direct transition tests |
| `src/waveform` | 54.06% | 46.21% | 35.30% | Loopback coverage added; wrapper edge cases remain |
| `src/psk` | 63.56% | 42.55% | 53.57% | Needs direct DPSK edge/vector tests |
| `src/sync` | 37.61% | 42.31% | 48.56% | Needs chirp false-lock/CFO/timing tests |
| `src/dsp` | 91.23% | 92.68% | 85.85% | Strong direct primitive coverage; FFT/resampler edge cases remain |
| `src/gui/modem/streaming_buffer_policy.hpp` | 95.92% | 100.00% | 83.33% | Extracted and tested from `feedAudio()` backlog/overflow policy |
| `src/gui/modem/streaming_decode_policy.hpp` | 93.55% | 100.00% | 90.00% | Extracted and tested from decode sample-sizing policy |
| `src/gui/modem/streaming_decoder.cpp` | 30.78% | 57.14% | 19.46% | Geometry/config and sample policies covered; decode state machine still needs extraction |
| `src/gui/modem/streaming_encoder.cpp` | 35.75% | 65.00% | 18.06% | Geometry/config covered; frame encoding branches still need extraction |

## Highest-Risk Files

Large and under-tested:
- `src/gui/modem/streaming_decoder.cpp`
- `src/gui/modem/streaming_encoder.cpp`
- `src/protocol/connection.cpp`
- `tools/cli_simulator.cpp`
- `src/ofdm/channel_equalizer.cpp`
- `src/ofdm/demodulator.cpp`
- `src/sync/chirp_sync.hpp`

These should not be "covered" by superficial tests. They need extraction into
smaller units with direct tests around their real invariants.

## Immediate Hardening Backlog

1. Codec/FEC direct tests:
- `CodecFactory` name/type behavior and unimplemented-codec failures.
- `LDPCCodec` wrapper rate changes, iteration policy, encode/decode success/fail paths.
- More LDPC invalid-input and edge-size tests.

2. DSP direct tests:
- FIR low/high/bandpass sanity and reset behavior.
- Biquad low/high/bandpass/notch finite output and reset behavior.
- AGC convergence and clamp behavior.
- NCO phase/frequency behavior.
- Hilbert finite analytic output.

3. Protocol/framing tests:
- More fixed-frame false-positive/recovery paths.
- Reserved waveform values not advertised.
- Callsign hash collision handling expectations.
- Connection state-machine transitions extracted from `Connection`.

4. Sync/OFDM tests:
- Chirp detection with timing offset, silence lead-in, false-noise rejection.
- CFO stress with known offsets and absolute phase handling.
- LTS/data-preamble timing tolerance.
- Equalizer pilot tracking and fading-index behavior.

5. Streaming TX/RX tests:
- Extract and test frame candidate selection.
- Extract and test 1-CW control decode retry policy.
- Extract and test multi-CW decode/deinterleave decisions.
- Buffer overflow/backlog accounting tests.

6. CI and tooling:
- Keep CTest, sanitizer, and coverage gates mandatory.
- Add static analysis only after current warnings are triaged.
- Add full seeded alpha gate as scheduled/nightly, not per-PR.

## Hardening Added In This Pass

- CI now runs multi-platform CTest, Linux sanitizer, and Linux coverage before packaging.
- Local coverage is reproducible with `scripts/coverage_report.sh`.
- New maintained tests cover codec factory/LDPC wrapper behavior, DSP primitives,
  stop-and-wait ARQ compatibility, waveform loopback, OFDM link geometry, and
  streaming TX/RX config parity.
- `test_frame_v2` now covers callsign sanitation, ping frames, channel report
  quantization, connect-frame CRCs, malformed headers, `CodewordStatus` edge
  cases, and fixed-frame helper policy.
- OFDM pilot/data-carrier geometry is centralized in `ofdm_link_adaptation` and
  reused by streaming TX/RX, waveform sample/throughput estimates, modem-rate
  estimates, and connection timeout calculations.
- Streaming RX ring-buffer overflow/backlog policy is extracted into
  `streaming_buffer_policy.hpp` with wraparound, pointer-drift, overflow, and
  telemetry tests.
- Streaming RX decode sample-sizing policy is extracted into
  `streaming_decode_policy.hpp` with robust OFDM control-frame sizing and
  pending-CW/burst/control-peek selection tests.

## Definition Of Progress

Progress is not "number of tests." Progress is:
- more Tier 0 branch coverage,
- fewer untested state transitions,
- fewer huge untestable files,
- more bugs represented by deterministic regressions,
- and CI catching regressions before hardware time is wasted.
