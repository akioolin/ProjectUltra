# Follow-Up Title

## Goal

State one measurable production outcome.

## Context

- PR/commit/report/log path:
- Current metric:
- Target metric:

## Blockers

State `None` or name the blocker, for example: `Do not approve until PR #21 is merged into main.`

## Allowed Files

- `src/...`
- `tests/...`

## Out Of Scope

- Do not weaken thresholds.
- Do not rewrite unrelated subsystems.

## Required Local Gate

```bash
./agents/run_local_gate.sh
```

## Required Hardware Or Benchmark Gate

State `Not required` or provide the exact command.

```bash
SSH_KEY="$HOME/.ssh/id_pi5" ./agents/run_hardware_sentinel.sh
```

## Reject Conditions

- No root-cause classification.
- No before/after metrics.
- Fix only changes thresholds or hides failures.

## Expected Output

- Root-cause classification with exact evidence.
- Patch or precise next task.
- Gate results and residual risks.
