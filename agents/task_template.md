# Task Title

## Goal

State the production outcome in one paragraph. Prefer measurable behavior over
implementation preference.

## Context

List the files, logs, commits, or benchmark results that matter. Include exact
paths when possible.

## Allowed Files

- `src/...`
- `include/...`
- `tests/...`

If the task is exploratory only, say `No code changes`.

## Out Of Scope

- Do not rewrite unrelated subsystems.
- Do not change hardware calibration unless the task explicitly asks for it.
- Do not weaken production invariants in `docs/INVARIANTS.md`.

## Required Local Gate

Run:

```bash
./agents/run_local_gate.sh
```

If a narrower gate is acceptable, explain why.

## Required Hardware Or Benchmark Gate

State the exact command or state `Not required`.

Examples:

```bash
SSH_KEY="$HOME/.ssh/id_pi5" ./agents/run_hardware_smoke.sh
```

```bash
./build/cli_simulator --snr 15 --fading good --rate r1_2 --test
```

## Reject Conditions

- New unit/regression failures.
- Lower throughput without a documented reason.
- New retransmission storm or decode backlog growth.
- Unexplained changes to LDPC, ARQ, or hardware calibration.

## Expected Output

- Short explanation of the change.
- Files changed.
- Gate results.
- Remaining risks or follow-up tasks.
