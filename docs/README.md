# Documentation Map

Last updated: 2026-05-07

This folder separates active source-of-truth docs from historical reference docs.
Agents must treat `docs/archive/` as background only unless a task explicitly
cites an archived file.

## Start Here

- `docs/PROJECT_GOALS.md`: Mission, priorities, throughput/reliability targets, and agent task filter.
- `docs/AGENT_CURRENT_STATE.md`: Current agent-system handoff and dedicated-machine context.
- `docs/INVARIANTS.md`: Critical modem invariants that must not be weakened.
- `docs/KNOWN_BUGS.md`: Current open reliability/throughput issues only.

## Operator Path

- `docs/RUNNING.md`: One-page runbook for the normal `ultra_tnc` operator flow.
- `docs/TNC_INTERFACE.md`: TCP command/data interface exposed by `ultra_tnc`.
- `README.md`: Top-level operator overview, build instructions, and status.

## Diagnostic / Lab Tools

- `tools/cli_simulator.cpp`: Full-protocol simulator and hardware-audio test harness.
- `tools/decode_bench.cpp`: Deterministic fixture generation/replay for decoder regressions.
- `tools/session_decode.cpp`: Offline decode of recorded full sessions.
- `tools/test_waveform_simple.cpp` and `ultra ptx/prx`: raw waveform/frame diagnostics.

## Agent And Quality Workflow

- `docs/AGENTIC_DEVELOPMENT.md`: Bounded agent workflow, permissions, gates, and review process.
- `docs/AGENT_TASK_BACKLOG.md`: Approved bounded task candidates for agents.
- `docs/AGENT_DEDICATED_ENV_MACOS.md`: Dedicated-user isolation setup for macOS agent hosts.
- `docs/QUALITY_STRATEGY.md`: Critical-software test, coverage, CI, and refactor policy.
- `docs/QUALITY_AUDIT.md`: Current quality baseline, coverage gaps, and hardening backlog.
- `docs/COVERAGE_MAP.md`: Module-specific critical coverage expectations and test priorities.
- `docs/GIT_WORKFLOW.md`: Commit/push/release workflow used in this repo.
- `docs/ALPHA_RELEASE_GATE.md`: Alpha readiness criteria and gate commands.

## Current Implementation References

- `docs/PROTOCOL_V2.md`: Current protocol behavior/spec aligned to implementation.
- `docs/BUILD_SYSTEM.md`: Build instructions, targets, dependencies, and coverage entry points.
- `docs/AUDIO_SYSTEM.md`: Audio I/O architecture.
- `docs/CONFIGURATION_SYSTEM.md`: Configuration model and runtime flow.
- `docs/GUI_ARCHITECTURE.md`: GUI structure and components.
- `docs/CFO_CORRECTION_FLOW.md`: Current CFO handling pipeline.
- `docs/ADDING_NEW_WAVEFORM.md`: Guide for adding waveform implementations.
- `docs/CHANGELOG.md`: Historical fixes and behavior changes.

## Archived Docs

Archived docs are retained for traceability and postmortem context but are not
source-of-truth for current behavior.

- `docs/archive/`
- Dated session reports, one-off audit reports, validation postmortems, and
  superseded research plans live here unless a task explicitly promotes them
  back to active source-of-truth status.

Do not base production changes on archived plans without first verifying current
code and adding a fresh task or design note.
