# Agent Task Backlog

This backlog converts the broad project objective into bounded tasks that an
agent can execute overnight without guessing.

Main objective: maximize reliable HF modem throughput across AWGN, Good,
Moderate, and Poor fading while preserving production reliability, security,
and testability.

## Global Acceptance Rules

Every task must include:

- exact code paths changed,
- exact local gate command and result,
- exact benchmark or hardware command when PHY/ARQ behavior changes,
- before/after metrics when throughput or robustness is touched,
- residual risks.

Reject any task result that:

- lowers frame/file delivery reliability without a documented tradeoff,
- weakens LDPC, ARQ, sync, or hardware calibration invariants,
- removes tests without replacement coverage,
- adds hidden global state, sleeps, or timing assumptions,
- commits logs, prompts, credentials, or host-specific secrets.

## Lane A: Critical Test And Quality Infrastructure

### A1. Cross-Platform Temp/File Hygiene

Goal: remove hardcoded POSIX-only temp paths from maintained tests.

Scope:

- `tests/`
- maintained test helpers

Acceptance:

- Windows, Linux, and macOS CTest must pass.
- Use `std::filesystem::temp_directory_path()` or CTest-provided working dirs.
- No persistent files left in the repo or fixed `/tmp` names.

Gate:

```bash
ctest --test-dir build --output-on-failure -j4
```

### A2. Coverage Map For Critical Blocks

Goal: define coverage expectations by subsystem instead of chasing arbitrary
whole-repo 100%.

Scope:

- LDPC encode/decode
- OFDM sync/demod
- streaming decoder acquisition gates
- selective-repeat ARQ
- file transfer controller
- waveform policy

Acceptance:

- Produce a markdown coverage map with required tests per block.
- Identify untested critical behavior and stale tests.
- No code changes unless needed for testability.

Gate:

```bash
./scripts/coverage_report.sh
```

### A3. Property/Boundary Tests For ARQ

Goal: harden selective-repeat behavior under ACK loss, stale ACKs, holes, and
window wrap.

Scope:

- `src/protocol/selective_repeat_arq.cpp`
- existing ARQ tests

Acceptance:

- Add deterministic tests for stale ACK, cumulative ACK, SACK hole repair,
  timeout repair, duplicate data, and wraparound.
- No sleeps or wall-clock flakiness.

Gate:

```bash
ctest --test-dir build -R 'SelectiveRepeat|Protocol' --output-on-failure
```

## Lane B: Throughput Baseline And Reproducibility

### B1. Reproducible Channel Benchmark Matrix

Goal: make throughput claims comparable across agents and commits.

Scope:

- `tools/`
- `scripts/`
- docs

Acceptance:

- One script runs AWGN/Good/Moderate/Poor for selected SNRs and rates.
- Output includes file size, wall time, data-phase bps, retx, timeouts,
  frame success, AckR, and log directory.
- Script fails if delivery fails.

Gate:

```bash
./build/cli_simulator --snr 15 --fading good --rate r1_2 --test
```

### B2. Decode CPU Attribution Report

Goal: quantify where decode-thread time goes before optimizing.

Scope:

- instrumentation only, preferably compile/runtime gated
- `src/gui/modem/streaming_decoder.cpp`

Acceptance:

- Report timing for sync search, 1-CW control decode, 4-CW data decode,
  LDPC attempts, acquisition rejection, and backlog.
- No production overhead unless explicitly enabled.

Gate:

```bash
ctest --test-dir build --output-on-failure -j4
```

## Lane C: Throughput Improvements

### C1. ACK Rate Reduction Without Stale-Repair Storms

Goal: reduce control-frame load while avoiding the stale timer failure mode.

Scope:

- selective-repeat ARQ ACK scheduling
- ACK repeat policy
- tests

Acceptance:

- Demonstrate lower AckR on Good/Moderate without higher retx/timeouts.
- Must include rollback sentinel for out-of-window/stale repair storms.
- Must pass at least 1 KB and 5 KB injected Good/Moderate hardware smoke.

Gate:

```bash
SSH_KEY="$HOME/.ssh/id_pi5" AGENT_HW_LONG=1 ./agents/run_hardware_smoke.sh
```

### C2. Adaptive RTO By Measured Decode Backlog

Goal: stop spurious ARQ timeouts when decoder backlog temporarily grows.

Scope:

- ARQ timeout policy
- streaming decode telemetry

Acceptance:

- RTO remains bounded.
- Timeout decrease on moderate fading without masking real loss.
- Unit tests cover min/max clamp and backlog spike behavior.

Gate:

```bash
ctest --test-dir build -R 'SelectiveRepeat|Streaming' --output-on-failure
```

### C3. Control Decode Fast-Fail Policy

Goal: reduce wasted 1-CW LDPC attempts on false locks without rejecting real ACKs.

Scope:

- 1-CW control decode path
- LLR/RMS/correlation gates

Acceptance:

- False-lock decode attempts decrease in profiler.
- ACK frame success remains healthy on Good/Moderate SNR15 and SNR12 canary.
- No hardcoded thresholds without documented evidence.

Gate:

```bash
SSH_KEY="$HOME/.ssh/id_pi5" ./agents/run_hardware_smoke.sh
```

## Lane D: Fading Robustness

### D1. Poor-Channel Mode Policy

Goal: define when the modem should switch to lower rate or MC-DPSK on Poor fading.

Scope:

- link adaptation policy
- waveform policy tests

Acceptance:

- Poor-channel tests deliver files reliably, even at lower throughput.
- Rate decisions are documented by SNR/fading evidence.

Gate:

```bash
ctest --test-dir build -R 'Waveform|OFDMLinkAdaptation|Protocol' --output-on-failure
```

### D2. Burst Erasure Recovery Tests

Goal: ensure weak physical bursts become erasures/repairs, not file-transfer
deadlocks.

Scope:

- streaming decoder weak-block handling
- file transfer tests

Acceptance:

- Synthetic tests cover one weak block inside a 4-frame burst.
- ARQ repairs the missing data without stale ACK storms.

Gate:

```bash
ctest --test-dir build --output-on-failure -j4
```

## Lane E: Security And Agent Governance

### E1. Secret/Artifact Leak Gate

Goal: prevent agent prompts, reports, logs, keys, and host-specific artifacts from
being committed.

Scope:

- `.gitignore`
- agent scripts
- CI

Acceptance:

- Queue/archive/reports/tmp remain ignored by default.
- CI or local script detects common secret patterns in tracked files.
- GitHub Actions jobs use least-privilege token permissions.

Gate:

```bash
git status --ignored agents .claude
git diff --check
```

### E2. Agent PR Template And Review Checklist

Goal: make every agent PR auditable.

Scope:

- `agents/`
- `.github/`

Acceptance:

- PR body requires task ID, gates, benchmark evidence, risks, and rollback notes.
- Throughput PRs must include before/after metrics.
- Security-sensitive PRs must state whether permissions changed.

Gate:

```bash
ruby -e 'require "yaml"; YAML.load_file(".github/workflows/build-matrix.yml")'
```
