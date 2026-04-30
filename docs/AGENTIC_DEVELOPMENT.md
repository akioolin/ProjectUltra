# Agentic Development

ProjectUltra can use Claude Code, Codex, or another CLI agent for long-running
improvement work, but the process must stay bounded. The goal is not to keep a
chat window alive forever. The goal is to keep a queue of reproducible tasks
flowing through build, regression, coverage, and hardware gates.

All agent work must align with `docs/PROJECT_GOALS.md`. If a proposed task does
not improve reliability, throughput, channel robustness, hardware diagnostics,
software quality, community reproducibility, or the agentic workflow itself, it
should not enter the queue.

## Operating Model

1. Write one task in `agents/queue/`.
2. Start one worker with `agents/run_next_task.sh` or `agents/watchdog.sh`.
3. The worker switches to `main`, fetches `origin/main`, fast-forwards only,
   creates an `agent/...` branch, and feeds the task to the selected CLI agent.
4. The worker runs local gates and optional hardware gates.
5. A human reviews the branch before merge.

This prevents the usual failure mode of 24/7 agents: broad goals, stale context,
unreviewed edits, and benchmark regressions that are discovered days later.
The runner owns commits, pushes, and PR creation. The coding agent must edit
files only; if it creates commits itself, the runner rejects the task unless
`AGENT_ALLOW_AGENT_COMMITS=1` is explicitly set.
If `main` cannot be fast-forwarded cleanly, the runner stops instead of letting
an agent work from stale code. Set `AGENT_UPDATE_BASE=0` only for deliberate
offline experiments. If local `main` has unpublished commits, the runner also
stops by default; set `AGENT_ALLOW_BASE_AHEAD=1` only when intentionally testing
from local-only base commits.

For the recommended MacBook M4 Pro isolation setup, read
`docs/AGENT_DEDICATED_ENV_MACOS.md`. For compacted-session handoff state, read
`docs/AGENT_CURRENT_STATE.md`.
For the project mission, priorities, and current milestone filter, read
`docs/PROJECT_GOALS.md`.

## Planner Lane

The planner lane is a chief-engineer triage loop, not a coding agent. It reads
open PRs, queue state, recent commits, and hardware sentinel reports, then writes
local proposals under `agents/planner/proposals/`.

Run once:

```bash
./agents/run_planner.sh
```

Run continuously:

```bash
./agents/planner_watchdog.sh
```

Planner proposals are ignored by git. A human must review and promote a proposal
into `agents/queue/claude/` or `agents/queue/codex/` before worker agents run it.
Keep auto-queueing disabled until several planner reports have shown good
judgment.

## Permissions

Use relaxed permissions for maintained commands only. Do not grant blanket
shell access to a coding agent.

Recommended Bash allow categories:

- Git inspection: `git status`, `git diff`, `git log`, `git show`, `git branch`.
- Local gates: `cmake -S . -B build`, `cmake --build build`, `ctest --test-dir build`, `tests/regression_matrix.sh`, `scripts/coverage_report.sh`.
- Hardware gates: `tools/check_hw_audio_path.sh`, `tools/run_hw_test.sh`, `agents/run_hardware_smoke.sh`.
- Git workflow: `git switch -c`, `git add`, `git commit`.
- PR workflow: `git push -u origin agent/*`, `gh pr create`, `gh pr view`, `gh pr status`.

Keep blocked:

- `sudo`
- `rm -rf`
- `git reset --hard`
- `git checkout --`
- broad Bash file readers such as `cat:*`, `sed:*`, `rg:*`, and `find:*`
- arbitrary `curl` or `wget`
- direct push to `main`

Use the agent's native file-read/search tools for repo inspection. Avoid
granting Bash commands that can read `~/.ssh`, shell history, or other files
outside the repository.

For Claude Code, use `agents/permissions/claude-settings.example.json` as the
repo-local starting point. The local `.claude/settings.local.json` is ignored by
git, so each machine can keep its own allowlist.

GitHub auth should come from the local `gh` CLI credential store. Do not put a
GitHub token in a task file, prompt, environment dump, or committed config.

## Running One Task

Create a task:

```bash
cp agents/task_template.md agents/queue/001-throughput-investigation.md
```

Task files in `agents/queue/` and `agents/archive/` are local-only by default.
Do not put secrets in them anyway; they are still fed to an agent process.

Dry-run the runner:

```bash
AGENT_DRY_RUN=1 AGENT_CMD='claude -p' ./agents/run_next_task.sh
```

Run the task:

```bash
AGENT_CMD='claude -p' ./agents/run_next_task.sh
```

For a prompt-file based CLI:

```bash
AGENT_PROMPT_MODE=file AGENT_CMD='codex exec --prompt-file' ./agents/run_next_task.sh
```

Adjust the command to match the exact CLI installed on the machine.

## Running Continuously

Use `tmux` first. It is easier to observe and stop than `launchd`.

```bash
tmux new -s ultra-agent
AGENT_CMD='claude -p' ./agents/watchdog.sh
```

Recommended unattended mode:

```bash
export AGENT_CMD='claude -p'
export AGENT_AUTO_COMMIT=0
export AGENT_RUN_HARDWARE=0
export AGENT_SLEEP_SECONDS=300
./agents/watchdog.sh
```

Use `AGENT_AUTO_COMMIT=1` only after the runner and gates are behaving well.
Use `AGENT_PUSH=1` only for feature branches, never for `main`.

PR-producing mode:

```bash
export AGENT_CMD='claude -p'
export AGENT_AUTO_COMMIT=1
export AGENT_PUSH=1
export AGENT_CREATE_PR=1
export AGENT_PR_DRAFT=1
export AGENT_TIMEOUT_SECONDS=7200
./agents/watchdog.sh
```

Keep PRs as drafts overnight. Promote them to ready-for-review only after a
human checks the diff, evidence, and CI result.
`AGENT_TIMEOUT_SECONDS` requires `timeout(1)` on the agent host. Leave it unset
on hosts without that command, or install GNU coreutils.

GitHub auto-loads `.github/PULL_REQUEST_TEMPLATE.md` into the PR body. Fill the
risk category, gates, evidence, security, and automated-agent sections from the
local-gate report and the task file before requesting review.

After that, install the macOS LaunchAgent example from `agents/launchd/`.
Keep it in no-auto-commit mode until you have reviewed several successful
worker branches.

## Hardware Gates

Hardware tests are serialized with `/tmp/projectultra_hw.lock`.

Default smoke:

```bash
SSH_KEY="$HOME/.ssh/id_pi5" ./agents/run_hardware_smoke.sh
```

Longer smoke:

```bash
AGENT_HW_LONG=1 SSH_KEY="$HOME/.ssh/id_pi5" ./agents/run_hardware_smoke.sh
```

The smoke script uses the current known-good calibration from `CLAUDE.md`:

- Mac audio device: `Sound Blaster Play! 3`
- Pi audio device: `USB Audio Device, USB Audio`
- injected channel gain: `0.70`

Do not run multiple hardware agents at once. The audio path is a single shared
physical resource.

## Hardware Sentinel

Hardware sentinel runs are periodic report-only health checks for the planner.
They are separate from per-PR hardware gates. A sentinel failure should produce
logs and a planner proposal; it should not trigger blind autonomous tuning.

Quick sentinel:

```bash
SSH_KEY="$HOME/.ssh/id_pi5" ./agents/run_hardware_sentinel.sh
```

Nightly 20 KB sentinel:

```bash
AGENT_HW_SENTINEL_MODE=nightly SSH_KEY="$HOME/.ssh/id_pi5" ./agents/run_hardware_sentinel.sh
```

Continuous sentinel loop:

```bash
AGENT_HW_SENTINEL_SLEEP_SECONDS=14400 SSH_KEY="$HOME/.ssh/id_pi5" ./agents/hardware_watchdog.sh
```

The sentinel writes `summary.txt` and `metrics.tsv` under
`agents/reports/hardware_sentinel_*`. The planner reads those reports and may
propose follow-up triage tasks for failures or warnings.

## Task Quality Bar

Good task files state:

- exact production behavior desired,
- exact files or subsystems in scope,
- exact benchmark or hardware command,
- reject conditions,
- expected output.

Use `docs/AGENT_TASK_BACKLOG.md` as the source of candidate tasks. Copy one
task into `agents/queue/` and keep the queue ordered by priority.

Bad task files say only "improve throughput" or "clean code". Those goals must
be decomposed into measurable experiments, for example:

- reduce ACK decode CPU by 20% without changing frame success,
- improve 5 KB Good SNR15 R1/2 median throughput over three seeds,
- add regression coverage for a specific ARQ timeout edge case,
- remove a dead subsystem and prove no maintained target references it.

## Review Gate

Before merging an agent branch:

```bash
git diff --stat main...
git diff main...
./agents/run_local_gate.sh
```

For PHY/ARQ changes, also run:

```bash
SSH_KEY="$HOME/.ssh/id_pi5" ./agents/run_hardware_smoke.sh
```

Merge only when the benchmark evidence matches the task goal and the PR body
(filled from `.github/PULL_REQUEST_TEMPLATE.md`) shows the required gates,
risk category, evidence, and rollback notes.
