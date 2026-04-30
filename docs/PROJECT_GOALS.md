# Project Goals

Last updated: 2026-04-30

This document is the durable mission brief for human and agent work on
ProjectUltra. Agents must read this before selecting or executing tasks.

## North Star

Build ProjectUltra into a production-grade open HF modem that reliably transfers
files over real audio hardware and HF-like channels, with measured throughput
competitive with commercial HF modems, while keeping the codebase testable,
maintainable, and safe for agentic development.

## Product Priorities

1. Reliability first.
   - Supported profiles must deliver files without silent corruption.
   - Ideal simulator and AWGN baselines should trend toward zero retries.
   - Real hardware may have bounded retries, but every retry class must be
     diagnosable from logs.
   - CRC, frame validation, LDPC, ARQ, sync, and calibration invariants must stay
     strict.

2. Throughput second, but measured continuously.
   - Short term: make 50 KB AWGN and Good R1/2 transfers consistently clean and
     faster than current baselines.
   - Medium term: reach stable 1500-2000 bps data-phase throughput on Good
     channels without ARQ storms.
   - Long term: use adaptive profiles to push higher rates only when safe and
     fall back cleanly under Moderate and Poor channels.

3. Channel robustness.
   - AWGN must be boring and clean.
   - Good fading must work reliably.
   - Moderate fading must deliver without timeout storms or decode backlog
     collapse.
   - Poor channels should degrade gracefully instead of collapsing.
   - ACK/control frames must be more robust than data frames.

4. Hardware reality.
   - Maintain a known-good hardware test path for the Mac/Pi and dedicated-agent
     laptop/Pi rigs.
   - Store calibration levels and expected RMS/peak ranges.
   - Distinguish PHY loss, audio clipping, false sync, ACK loss, scheduling
     backlog, decode CPU pressure, and ARQ bugs.
   - Hardware tests must produce logs detailed enough to diagnose failures
     without guessing.

5. Critical software quality.
   - Core modem blocks need heavy tests: LDPC, interleaving, framing, sync,
     OFDM, ARQ, file transfer, channel injection, and hardware harness behavior.
   - Retire stale tests and dead code instead of preserving experimental paths by
     default.
   - Keep invariants documented and enforced.
   - CI must stay green before merge.
   - Avoid duplicate AI-generated systems and unused experimental code.

6. Agentic development discipline.
   - Agents work from queued, bounded tasks, not broad open-ended prompts.
   - Agents create branches and evidence, not direct pushes to `main`.
   - Every task must report tests, logs, benchmarks, before/after results, and
     residual risks.
   - Hardware tests must be lock-controlled because the audio path is a single
     physical resource.
   - Human review is required for production-impacting changes.

7. Community-ready project.
   - Build instructions must work for macOS, Linux, and Raspberry Pi.
   - Hardware wiring and calibration must be documented and reproducible.
   - Simulator sweeps must be reproducible by command.
   - Performance tables must include commit, profile, SNR, channel, hardware,
     command, and logs.
   - Users should not need private context to reproduce public results.

## Immediate Milestones

1. Establish clean baseline tables for AWGN, Good, Moderate, and Poor using both
   simulator and hardware-injected tests.
2. Fix ACK/control robustness so Good and Moderate profiles do not collapse.
3. Add diagnostics for burst-tail loss, ACK timing, false locks, decode backlog,
   and audio-level issues.
4. Improve throughput only after reliability for the relevant profile is stable.
5. Convert large goals into agent task lanes with acceptance criteria,
   reproducible commands, and rejection conditions.

## Agent Task Rule

If a task cannot be evaluated against one of these goals, it is probably too
broad or not ready for autonomous execution. Decompose it before putting it in
`agents/queue/`.
