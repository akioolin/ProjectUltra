# Dedicated Agent Environment On macOS

This guide describes the recommended way to run long-lived Claude Code or Codex
workers on the MacBook M4 Pro without giving them broad access to the owner's
normal shell, home directory, SSH keys, browser sessions, or personal files.

## Recommendation

Use a dedicated macOS standard user named `ultra-agent`.

This is stronger than a separate `tmux` session because it gives the agents a
separate home directory, separate shell history, separate keychain scope, and a
separate repo checkout. It is weaker than a full VM, but it is practical on a
MacBook and good enough for overnight draft-PR generation when combined with
branch protection and command-scoped permissions.

## Security Boundary

The dedicated user may have:

- one checkout of this repository,
- Homebrew tools needed to build and test,
- `gh` auth limited to this repository,
- optional SSH access to the Pi test host only if hardware tests are assigned,
- Claude/Codex local settings for this repo.

The dedicated user must not have:

- broad personal SSH keys,
- personal cloud credentials,
- personal browser sessions,
- arbitrary GitHub admin tokens,
- access to unrelated project directories,
- permission to push `main`.

## Create The User

Preferred: use macOS System Settings.

1. Open `System Settings`.
2. Go to `Users & Groups`.
3. Add a new standard user named `ultra-agent`.
4. Log into `ultra-agent` once to create the home directory and keychain.

Avoid making this user an administrator. If admin access is needed for package
installation, do it from the owner account, not from the agent account.

## Tooling Setup

Homebrew installed under `/opt/homebrew` is normally usable by all local users.
In the `ultra-agent` account, ensure the PATH includes Homebrew:

```bash
echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> ~/.zprofile
eval "$(/opt/homebrew/bin/brew shellenv)"
```

Install shared tools from the owner account if missing:

```bash
brew install cmake gh tmux llvm sdl2
```

Then in the `ultra-agent` account:

```bash
mkdir -p ~/Projects
cd ~/Projects
git clone https://github.com/secup/ProjectUltra.git
cd ProjectUltra
cmake -S . -B build -DULTRA_BUILD_TESTS=ON -DULTRA_BUILD_GUI=OFF -DULTRA_USE_FFTW=OFF
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
```

## GitHub Auth

Use `gh` auth from the `ultra-agent` account. Do not put tokens in task files,
shell exports, prompts, or committed configs.

Preferred token shape:

- fine-grained token,
- repository limited to `secup/ProjectUltra`,
- `Contents: Read and write`,
- `Pull requests: Read and write`,
- no administration/secrets/workflow permissions unless explicitly needed.

Authenticate interactively:

```bash
gh auth login -h github.com
gh auth status -h github.com
```

The agent runner only needs:

```text
git push -u origin agent/*
gh pr create
gh pr view
gh pr status
```

Do not allow:

```text
gh auth token
gh api
gh auth login
git push origin main
git push --force
```

## Claude Code Setup

Use repo-local settings. In the `ultra-agent` checkout:

```bash
mkdir -p .claude
cp agents/permissions/claude-settings.example.json .claude/settings.local.json
```

Recommended Claude mode:

```json
{
  "permissions": {
    "defaultMode": "acceptEdits"
  }
}
```

Do not use `bypassPermissions` for this workflow. Claude's own documentation
states bypass mode should be limited to isolated environments such as containers
or VMs. A separate macOS user is helpful, but it is not a complete VM boundary.

## Codex Setup

Use Codex Auto Edit or Full Auto only in the dedicated checkout.

Recommended pattern:

- Auto Edit for code changes where shell execution should still be gated.
- Full Auto only when the command sandbox is scoped to the repository and
  network access is disabled or command-scoped.
- Never expose `gh auth token`, personal SSH keys, or unrelated directories.

Mirror `agents/permissions/codex-policy.md` into the active Codex approval
configuration.

## Running The Overnight Worker

Start in `tmux` first:

```bash
tmux new -s ultra-agent
cd ~/Projects/ProjectUltra
export AGENT_CMD='claude -p'
export AGENT_AUTO_COMMIT=1
export AGENT_PUSH=1
export AGENT_CREATE_PR=1
export AGENT_PR_DRAFT=1
export AGENT_RUN_HARDWARE=0
./agents/watchdog.sh
```

Use hardware gates only from one worker:

```bash
export AGENT_RUN_HARDWARE=1
export AGENT_HW_LONG=1
./agents/watchdog.sh
```

The hardware lane must be exclusive because the Mac/Pi audio path is a single
physical resource.

## Queue Discipline

Tasks must come from `docs/AGENT_TASK_BACKLOG.md` or a similarly specific
task file. Do not run overnight agents from broad prompts like "improve
throughput".

Good overnight tasks include:

- add deterministic ARQ edge-case tests,
- build a benchmark matrix script,
- instrument decode CPU with gated telemetry,
- harden cross-platform tests,
- reduce ACK rate with explicit rollback sentinels.

Bad overnight tasks include:

- rewrite the modem,
- optimize throughput without a benchmark plan,
- change LDPC thresholds without evidence,
- remove tests without replacement coverage.

## GitHub Protection Required

Before enabling PR-producing overnight mode, protect `main` on GitHub:

- require pull requests before merging,
- require at least one approval,
- require status checks to pass,
- require branches to be up to date before merge,
- block force pushes,
- block branch deletion,
- do not allow bypass except for explicitly trusted maintainers.

Agents may create draft PRs. Humans merge.

## Failure Handling

If an agent branch fails local gates:

- leave the task in `agents/queue/`,
- inspect `agents/reports/`,
- do not auto-push unless the failure itself is the requested artifact.

If an agent branch fails GitHub CI:

- let a new task target the exact CI failure,
- do not merge by exception for throughput or PHY work.

If the hardware rig fails:

- run `SSH_KEY="$HOME/.ssh/id_pi5" ./tools/check_hw_audio_path.sh`,
- verify the Mac device is `Sound Blaster Play! 3`,
- verify the Pi device is `USB Audio Device, USB Audio`,
- verify injected tests use `--inject-gain 0.70`.

## Practical Isolation Levels

Level 0: same user, separate `tmux`.
Fast, but not sleep-safe.

Level 1: dedicated macOS user.
Recommended default for this project.

Level 2: dedicated macOS user plus no hardware key and no PR token.
Good for pure local code/test cleanup agents.

Level 3: VM or container with mounted repo and no personal home directory.
Best for untrusted tasks, but more setup friction for audio/hardware tests.

Use Level 1 for normal overnight draft PR generation. Use Level 3 for risky
security experiments or broad refactors.
