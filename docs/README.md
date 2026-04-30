# Documentation Map

Last updated: 2026-04-30

This folder separates active source-of-truth docs from historical reference docs.
Agents must treat `docs/archive/` as background only unless a task explicitly
cites an archived file.

## Start Here

- `docs/PROJECT_GOALS.md`: Mission, priorities, throughput/reliability targets, and agent task filter.
- `docs/AGENT_CURRENT_STATE.md`: Current agent-system handoff and dedicated-machine context.
- `docs/INVARIANTS.md`: Critical modem invariants that must not be weakened.
- `docs/KNOWN_BUGS.md`: Current open reliability/throughput issues only.

## Agent And Quality Workflow

- `docs/AGENTIC_DEVELOPMENT.md`: Bounded agent workflow, permissions, gates, and review process.
- `docs/AGENT_TASK_BACKLOG.md`: Approved bounded task candidates for agents.
- `docs/AGENT_DEDICATED_ENV_MACOS.md`: Dedicated-user isolation setup for macOS agent hosts.
- `docs/QUALITY_STRATEGY.md`: Critical-software test, coverage, CI, and refactor policy.
- `docs/QUALITY_AUDIT.md`: Current quality baseline, coverage gaps, and hardening backlog.
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

Do not base production changes on archived plans without first verifying current
code and adding a fresh task or design note.
