<!--
ProjectUltra PR template. Keep entries short and concrete.
Strike out sections that are clearly inapplicable rather than padding them.
Do not push directly to `main`. Agent PRs must be drafts and require human review.
-->

## Summary

<!-- One paragraph: what this PR changes and why. Name the user-visible behavior, not the diff. -->

## Source

- Task or backlog item: <!-- e.g. agents/queue/.../002-agent-pr-template.md, docs/AGENT_TASK_BACKLOG.md#E2, BUG-XXX, or "human-authored" -->
- Related issues / PRs: <!-- #123, links, or "none" -->

## Risk Category

Pick exactly one primary category. Add secondaries only if a reviewer must look at them too.

- [ ] docs
- [ ] tooling (scripts, CI helpers, agent runner, repo config — no production source change)
- [ ] test-only (new/changed tests, fixtures, harnesses)
- [ ] PHY (waveform, DSP, sync, OFDM/PSK demod, CFO)
- [ ] ARQ (selective repeat, stop-and-wait, ACK/NACK, retransmission, file transfer)
- [ ] LDPC / FEC (encoder, decoder, interleaver, rate policy)
- [ ] audio (audio engine, device I/O, calibration paths)
- [ ] hardware (Mac/Pi rig scripts, calibration constants, lock behavior)
- [ ] security (permissions, secrets handling, network surface, agent allowlists)

## Required Gates

Run from the repo root. Paste the result for each gate that applies.

- [ ] Local gate: `./agents/run_local_gate.sh` — result: <!-- pass/fail + report dir under agents/reports/local_* -->
- [ ] Targeted CTest subset (if narrower than full ctest): `ctest --test-dir build -R '<pattern>' --output-on-failure` — result:
- [ ] Coverage (Tier 0/Tier 1 source change): `./scripts/coverage_report.sh` — line / function / branch deltas vs baseline:
- [ ] Hardware smoke (PHY / ARQ / audio / hardware change): `SSH_KEY="$HOME/.ssh/id_pi5" ./agents/run_hardware_smoke.sh` — result + report dir, or `not required because <reason>`
- [ ] CFO/sync/OFDM demod regression (if touched): `./tests/verify_cfo_chain.sh --cfo 50 --channel awgn --snr 20 --seed 42` — result:

If a required gate is intentionally skipped, justify it on one line. "Not run" without a reason blocks merge.

## Throughput / Robustness Evidence

Required for any PHY / ARQ / LDPC / audio / waveform-policy change. Otherwise write `not applicable: <reason>`.

| Profile (channel, SNR, rate, payload) | Command | Before (commit / metric) | After (commit / metric) |
|---|---|---|---|
| | | | |

Report at minimum: frame/CW success, retransmission count, throughput (bps), and seed(s). Link to log paths under `/tmp/ultra_*` or `agents/reports/`.

## Invariants & Calibration

- [ ] No change to documented invariants in `docs/INVARIANTS.md`, or this PR updates that doc and `docs/CHANGELOG.md` with the rationale.
- [ ] No change to hardware calibration (mixer levels, `--inject-gain`, device names) unless the task explicitly asks for it.
- [ ] LDPC, ARQ, sync, and CFO feedback loops are not weakened.

## Security & Privacy

- [ ] No prompts, agent transcripts, or task-file copies committed.
- [ ] No `agents/reports/`, `agents/tmp/`, `/tmp/ultra_*`, or other run-time logs committed.
- [ ] No credentials, tokens, SSH keys, or `.env` material committed.
- [ ] No host-specific paths (e.g. `/Users/<name>`, `pi5tester` IPs, private hostnames) hardcoded in source or tests.
- [ ] If permissions / allowlists / CI tokens changed, called out explicitly below:

<!-- describe permission or token surface changes, or write "none" -->

## Rollback

- Revert plan: <!-- e.g. "git revert <sha>; rebuild; rerun local gate" or "feature is gated by <flag>, disable to roll back" -->
- Data/state migrations to undo: <!-- "none" if pure code -->

## Residual Risks & Follow-ups

- Risks accepted in this PR: <!-- e.g. "Moderate fading R1/2 still has 2.4 avg retx; not regressed but not fixed" -->
- Suggested follow-up tasks: <!-- file paths under agents/queue/ or backlog refs; "none" is OK -->

---

## Automated Agent PR Checklist

Fill this section only if the PR was produced by an agent runner (`agents/run_next_task.sh` / `watchdog.sh`). Humans authoring directly may delete it.

- [ ] Task file path: <!-- agents/queue/<lane>/<id>-<slug>.md or archived path -->
- [ ] Report directory: <!-- agents/reports/<run-id>/ -->
- [ ] PR opened as **draft** (`AGENT_PR_DRAFT=1`).
- [ ] Branch is `agent/...`, not `main` and not a protected branch.
- [ ] CI status on this PR: <!-- pending / green / red + link; red blocks merge -->
- [ ] Human reviewer required before marking ready-for-review and before merge.
- [ ] No commits authored outside the runner (`AGENT_ALLOW_AGENT_COMMITS` not set unless approved).
