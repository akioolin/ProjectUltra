# Agentic Development

ProjectUltra can use Claude Code, Codex, or another CLI agent for long-running
improvement work, but the process must stay bounded. The goal is not to keep a
chat window alive forever. The goal is to keep a queue of reproducible tasks
flowing through build, regression, coverage, and hardware gates.

## Operating Model

1. Write one task in `agents/queue/`.
2. Start one worker with `agents/run_next_task.sh` or `agents/watchdog.sh`.
3. The worker creates an `agent/...` branch and feeds the task to the selected CLI agent.
4. The worker runs local gates and optional hardware gates.
5. A human reviews the branch before merge.

This prevents the usual failure mode of 24/7 agents: broad goals, stale context,
unreviewed edits, and benchmark regressions that are discovered days later.

For the recommended MacBook M4 Pro isolation setup, read
`docs/AGENT_DEDICATED_ENV_MACOS.md`. For compacted-session handoff state, read
`docs/AGENT_CURRENT_STATE.md`.

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
./agents/watchdog.sh
```

Keep PRs as drafts overnight. Promote them to ready-for-review only after a
human checks the diff, evidence, and CI result.

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

Merge only when the benchmark evidence matches the task goal.
