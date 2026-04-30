# ProjectUltra Critical Software Quality Strategy

Last updated: 2026-04-30

## Purpose

ProjectUltra should be developed like critical communications software: explicit
invariants, deterministic tests, measured coverage, seeded regressions, and
reproducible hardware evidence.

This document defines what "professional grade" means for this repository.

## Core Principle

Do not chase fake global 100% coverage. Instead:

1. Classify code by safety and mission criticality.
2. Demand near-complete meaningful coverage for modem-critical code.
3. Remove stale code instead of testing it artificially.
4. Exclude GUI/platform/glue code only when the exclusion is explicit and justified.
5. Convert every serious bug into a deterministic regression test or replay fixture.

Global coverage is useful as a trend. It is not the release gate by itself.

## Criticality Tiers

### Tier 0: Modem-Critical Core

Failures here can corrupt payloads, lose frames, collapse throughput, or make
hardware tests misleading. These modules need the strongest tests and highest
coverage.

Scope:
- FEC: `src/fec/`, LDPC encode/decode wrappers, frame/burst interleavers.
- DSP and waveform demodulation: `src/dsp/`, `src/ofdm/`, `src/psk/`, `src/sync/`.
- Framing: `src/protocol/frame_v2.*`, CRC/header/rate/callsign behavior.
- ARQ and transfer reliability: `src/protocol/selective_repeat_arq.*`,
  `src/protocol/arq.*`, `src/protocol/file_transfer.*`.
- Streaming modem pipeline: `src/gui/modem/streaming_encoder.*`,
  `src/gui/modem/streaming_decoder.*`.
- Waveform wrappers/factory: `src/waveform/`.
- Channel simulator when used as a release gate: `src/sim/hf_channel.hpp`.

Coverage target:
- Short term: 90% line coverage and 80% branch coverage for Tier 0 after
  exclusions/refactors.
- Medium term: 95% line coverage and 90% branch coverage for deterministic
  Tier 0 modules.
- Deterministic pure modules such as framing, interleavers, codec factory, and
  rate policy should trend toward 100% meaningful branch coverage.

Required test types:
- Unit tests for edge cases and invariants.
- Golden-vector tests for encoders/decoders and frame serialization.
- Deterministic randomized tests with fixed seeds.
- Fault-injection tests for corrupt frames, dropped ACKs, stale ACKs, false sync,
  low LLR, CFO, timing offsets, and buffer pressure.
- Seeded simulator matrix for AWGN/good/moderate/poor profiles.
- Hardware log replay where real Mac/Pi captures expose bugs.

### Tier 1: Integration-Critical Runtime

Failures here can break real usage even if the core math is correct.

Scope:
- `src/protocol/connection.*`
- `src/protocol/protocol_engine.*`
- `src/modem/modem.cpp`
- CLI and hardware test path in `tools/cli_simulator.cpp`
- `src/gui/modem/modem_engine.*`, `modem_rx.*`, `modem_mode.*`
- `src/gui/audio_engine.*`

Coverage target:
- Strong integration tests and state-machine regression tests.
- Line coverage is useful but lower priority than seeded end-to-end gates.
- Large files should be refactored when coverage cannot be made meaningful.

Required test types:
- Two-station protocol simulations.
- State transition tests.
- File-transfer integrity tests.
- Replay tests from captured logs/audio.
- Hardware smoke tests with documented calibration.

### Tier 2: Operational/UI/Packaging

Failures here affect usability but should not be mixed with modem correctness.

Scope:
- GUI widgets, settings panels, waterfall/constellation rendering.
- Startup tracing and log display.
- Build/package scripts and release artifacts.

Coverage target:
- Build and smoke tests.
- Static checks.
- Manual/visual checks when needed.
- Not part of critical coverage percentage unless logic becomes safety-critical.

### Tier 3: Research, Reserved, or Dead Code

Scope:
- Future codec placeholders.
- Reserved waveform enum values.
- Experimental modes not selected by the default ladder.
- Historical or prototype code.

Policy:
- Do not keep unmaintained prototypes in production builds.
- Reserved protocol values may remain, but tests must verify they are not
  advertised as supported.
- If code is not intended to ship, remove it or archive the rationale in docs.

## Required Local Gates

Before committing Tier 0 or Tier 1 changes:

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
./tests/regression_matrix.sh --quick
./scripts/coverage_report.sh
git diff --check
```

For protocol/ARQ/rate-ladder changes, also run:

```bash
scripts/run_alpha_gate.sh --quick
```

For CFO, sync, or OFDM demod changes, also run:

```bash
./tests/verify_cfo_chain.sh --cfo 50 --channel awgn --snr 20 --seed 42
```

Hardware-facing changes must state whether they were verified with:
- simulator only,
- hardware audio path,
- captured log/audio replay,
- or real OTA.

## CI Policy

GitHub CI must enforce at least:
- Multi-platform build.
- Multi-platform CTest.
- Linux sanitizer test job: ASAN + UBSAN.
- Linux coverage job with a baseline threshold.
- Packaging only after tests pass.

Coverage thresholds should only move upward unless code is explicitly
reclassified or removed.

## Coverage Scope Rules

Allowed in critical coverage:
- `src/fec`
- `src/dsp`
- `src/ofdm`
- `src/psk`
- `src/sync`
- `src/protocol`
- `src/waveform`
- streaming encoder/decoder under `src/gui/modem`

Excluded from critical coverage unless explicitly tested:
- GUI rendering widgets.
- Platform-specific audio device setup.
- Third-party code.
- Test code.
- Release packaging code.

If an exclusion hides real modem logic, refactor the logic into a testable
non-GUI module instead of excluding it.

## Refactoring For Testability

The following files are too large or too coupled for critical-grade testing in
their current shape:
- `src/gui/modem/streaming_decoder.cpp`
- `src/gui/modem/streaming_encoder.cpp`
- `src/protocol/connection.cpp`
- `tools/cli_simulator.cpp`
- `src/gui/app.cpp`

Required direction:
- Split pure logic out of thread/UI/hardware wrappers.
- Give each state machine explicit transition tests.
- Make sync acquisition, decode candidate selection, LDPC retry policy, and ARQ
  timers independently testable.
- Preserve behavior with regression tests before large rewrites.

## Bug Handling

Every serious bug needs:
- a `BUG-*` entry in `docs/KNOWN_BUGS.md` while open,
- a deterministic reproducer command or fixture,
- a failing test before or with the fix when practical,
- a changelog entry when fixed,
- and a regression gate so it does not return.

If a bug is only reproducible on hardware, capture enough logs/audio to replay or
diagnose it offline.

## Definition Of Done

A Tier 0/Tier 1 change is not done until:
- The intended behavior is specified in docs or tests.
- Relevant unit/integration tests are added or updated.
- Required local gates pass.
- Coverage does not regress, or the regression is explained by explicit
  reclassification/removal.
- Known bug docs are updated if applicable.
- The commit message describes the behavior change, not just files touched.

