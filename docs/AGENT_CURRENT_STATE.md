# Agent System Current State

Last updated: 2026-04-30.

This file is a compact handoff for future Claude/Codex sessions if context is
compacted or lost.

## Objective

Use Claude Code, Codex/GPT-5.5, and GitHub CI to continuously improve
ProjectUltra, with the main technical goal of maximizing reliable HF modem
throughput across AWGN, Good, Moderate, and Poor fading while maintaining
critical-software engineering discipline.

## Operating Decision

Use a bounded autonomous PR factory:

```text
task backlog -> agents/queue/*.md -> agent branch -> local gates -> commit
-> push agent/* branch -> draft PR -> CI -> human review -> merge
```

Do not run open-ended overnight prompts. Every agent run must have a task file,
acceptance criteria, reject conditions, and required gates.

## Implemented Locally

Agent infrastructure added:

- `agents/run_next_task.sh`
- `agents/watchdog.sh`
- `agents/run_local_gate.sh`
- `agents/run_hardware_smoke.sh`
- `agents/run_hardware_sentinel.sh`
- `agents/hardware_watchdog.sh`
- `agents/run_planner.sh`
- `agents/planner_watchdog.sh`
- `agents/publish_planner_proposals.sh`
- `agents/process_approved_proposals.sh`
- `agents/approval_watchdog.sh`
- `agents/task_template.md`
- `agents/queue/`
- `agents/archive/`
- `agents/reports/`
- `agents/tmp/`
- `agents/planner/`
- `agents/permissions/claude-settings.example.json`
- `agents/permissions/codex-policy.md`
- `agents/launchd/`
- `docs/AGENTIC_DEVELOPMENT.md`
- `docs/AGENT_TASK_BACKLOG.md`
- `docs/AGENT_DEDICATED_ENV_MACOS.md`

Security hardening added:

- `agents/queue/`, `agents/archive/`, `agents/reports/`, and `agents/tmp/`
  ignore local task/report/prompt contents by default.
- `.claude/settings.local.json` remains ignored by repo `.gitignore`.
- Claude/Codex permission examples allow build/test/branch/PR commands but deny
  destructive git, `sudo`, broad network tools, `gh auth token`, and broad
  GitHub API access.
- GitHub Actions workflow changed to `contents: read` globally and
  `contents: write` only for release publishing.
- `actions/checkout` uses `persist-credentials: false` in normal jobs.

CI portability fix added:

- `tests/test_protocol.cpp` no longer writes fixed `/tmp` test files.
- `tests/test_wav_loopback.cpp` no longer writes fixed `/tmp/test_loopback.wav`.
- Both now use `std::filesystem::temp_directory_path()` with unique directories.

## Current Validation

Passed locally after agent-infrastructure changes:

```bash
bash -n agents/run_local_gate.sh agents/run_hardware_smoke.sh agents/run_next_task.sh agents/watchdog.sh
bash -n agents/run_hardware_sentinel.sh agents/hardware_watchdog.sh agents/run_planner.sh agents/planner_watchdog.sh agents/publish_planner_proposals.sh agents/process_approved_proposals.sh agents/approval_watchdog.sh
python3 -m json.tool agents/permissions/claude-settings.example.json
python3 -m json.tool .claude/settings.local.json
ruby -e 'require "yaml"; YAML.load_file(".github/workflows/build-matrix.yml")'
AGENT_QUEUE_DIR=agents AGENT_DRY_RUN=1 AGENT_CMD='claude -p' ./agents/run_next_task.sh
git diff --check
ctest --test-dir build -R '^(Protocol|WavLoopback)$' --output-on-failure -j2
ctest --test-dir build --output-on-failure -j4
```

The full local CTest result was `29/29` passed.

Planner/hardware sentinel validation:

```bash
AGENT_HW_SENTINEL_DRY_RUN=1 AGENT_HW_SENTINEL_MODE=quick AGENT_HW_SENTINEL_AUDIO_CHECK=0 ./agents/run_hardware_sentinel.sh
AGENT_PLANNER_REPORT_DIR=/tmp/projectultra_planner_report AGENT_PLANNER_PROPOSAL_DIR=/tmp/projectultra_planner_proposals ./agents/run_planner.sh
AGENT_PUBLISH_DRY_RUN=1 AGENT_PLANNER_PROPOSAL_DIR=/tmp/projectultra_planner_proposals ./agents/publish_planner_proposals.sh
AGENT_APPROVAL_DRY_RUN=1 AGENT_APPROVERS=secup ./agents/process_approved_proposals.sh
./agents/run_local_gate.sh
```

## Dedicated Agent Laptop

The Linux Mint `agentic` laptop is the current dedicated runtime. SSH access is:

```bash
ssh -i "$HOME/.ssh/id_pi5" ultra-agent@agentic
```

Active clones:

- `~/Projects/ProjectUltra-claude`
- `~/Projects/ProjectUltra-codex`

The tmux watchdog sessions are:

- `ultra-claude-watch`
- `ultra-codex-watch`

Optional additional sessions:

- `ultra-hardware-watch`: runs `agents/hardware_watchdog.sh` for periodic
  report-only hardware sentinel checks.
- `ultra-planner-watch`: runs `agents/planner_watchdog.sh` for periodic
  report/proposal generation and, when enabled, GitHub Issue publication.
- `ultra-approval-watch`: runs `agents/approval_watchdog.sh` to turn
  allowlisted GitHub approvals into local queue files.

Both agents are configured to create draft PRs from `agent/*` branches, not push
directly to `main`. The runner owns `git add`, `git commit`, `git push`, and
`gh pr create`; coding agents should only edit files and report evidence.

Observed supervised PRs:

- PR #3 `001-coverage-map`: CI passed.
- PR #4 `001-bench-matrix`: useful draft PR; one Windows build failed because
  vcpkg hit an external GitHub 502 while downloading SDL, not because of code.
- PR #5 `001-leak-gate`: local gate passed; CI was still running when this note
  was updated.

GitHub CLI auth:

- `gh auth status` inside the restricted sandbox reported a misleading invalid
  token.
- With network/keyring access, `gh auth status -h github.com` succeeds for
  account `secup`.
- Do not allow agents to run `gh auth token`.

## Known GitHub CI Failure Addressed

Failed run:

- workflow: `Build Matrix`
- commit: `d2bed4ee3d15f26d00fca91ced7e37d1d9378875`
- failure: Windows `Run CTest`
- failing tests: `Protocol`, `WavLoopback`
- cause: hardcoded `/tmp` paths do not exist on the Windows runner.

Local fix:

- changed tests to use portable temp directories.

## Recommended Dedicated Runtime

Run overnight agents from a dedicated macOS standard user named `ultra-agent`,
not from the owner's normal account.

See `docs/AGENT_DEDICATED_ENV_MACOS.md`.

Minimum runtime:

```bash
cd ~/Projects/ProjectUltra
export AGENT_CMD='claude -p'
export AGENT_AUTO_COMMIT=1
export AGENT_PUSH=1
export AGENT_CREATE_PR=1
export AGENT_PR_DRAFT=1
export AGENT_RUN_HARDWARE=0
./agents/watchdog.sh
```

Hardware lane:

```bash
export AGENT_RUN_HARDWARE=1
export AGENT_HW_LONG=1
./agents/watchdog.sh
```

Only one hardware lane may run at a time.

Hardware sentinel lane:

```bash
AGENT_HW_SENTINEL_SLEEP_SECONDS=14400 SSH_KEY="$HOME/.ssh/id_pi5" ./agents/hardware_watchdog.sh
```

Planner lane:

```bash
AGENT_PLANNER_SLEEP_SECONDS=1800 ./agents/planner_watchdog.sh
```

Remote approval lane:

```bash
AGENT_APPROVERS=secup AGENT_APPROVAL_SLEEP_SECONDS=300 ./agents/approval_watchdog.sh
```

## GitHub Requirements Before Sleep-Safe Mode

Protect `main`:

- require pull requests before merging,
- require at least one approval,
- require status checks to pass,
- require branches to be up to date,
- block force pushes,
- block branch deletion,
- do not allow agents to bypass protection.

Agents create draft PRs only. Humans merge.

## Main Agent Backlog

Use `docs/AGENT_TASK_BACKLOG.md`.

Highest-value lanes:

- Lane A: critical tests and quality infrastructure.
- Lane B: reproducible channel benchmark matrix.
- Lane C: throughput improvements with before/after evidence.
- Lane D: fading robustness.
- Lane E: security and agent governance.

## Sleep-Safe Caveat

The worker watchdogs can run continuously, but they do not invent work from
broad goals. They process bounded task files in `agents/queue/`. If the queues
are empty, they stay alive and idle.

The planner loop is allowed to run continuously because it is proposal-only by
default. Its output is local and ignored under `agents/planner/proposals/`.
Humans must promote proposals into `agents/queue/` before coding agents execute
them.

Remote approvals are GitHub Issue comments. Only exact usernames listed in
`AGENT_APPROVERS` are honored. The approval watcher queues tasks locally only;
it does not merge PRs or push to `main`.
