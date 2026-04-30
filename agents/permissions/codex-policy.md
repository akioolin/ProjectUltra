# Codex Permission Policy

Codex permissions are environment/session controlled, so keep the durable policy
in this file and mirror it into the active Codex approval settings as needed.

Approved categories:

- Git inspection: `git status`, `git diff`, `git log`, `git show`, `git branch`.
- Build and local test: `cmake -S . -B build`, `cmake --build build`, `ctest --test-dir build`.
- Maintained gates: `./tests/regression_matrix.sh`, `./scripts/coverage_report.sh`, `./agents/run_local_gate.sh`.
- Hardware gates: `./tools/check_hw_audio_path.sh`, `./tools/run_hw_test.sh`, `./agents/run_hardware_smoke.sh`.
- Safe branch workflow: `git switch -c`, `git add`, `git commit`.
- PR workflow: `git push -u origin agent/*`, `gh pr create`, `gh pr view`, `gh pr status`.
- Agent control plane, when running trusted watchdogs only, not worker agents:
  `./agents/publish_planner_proposals.sh`,
  `./agents/process_approved_proposals.sh`, `./agents/approval_watchdog.sh`.

Require human approval:

- pushing non-agent branches
- dependency installation
- network access
- edits outside this repository
- hardware tests longer than the default smoke
- direct `gh issue` commands outside the maintained approval scripts

Never pre-approve:

- `git reset --hard`
- `git checkout --`
- `rm -rf`
- `sudo`
- broad shell file readers such as `cat`, `sed`, `rg`, and `find`
- arbitrary shell wrappers that hide multiple operations

Use Codex's native file-read/search tools for repo inspection instead of broad
Bash read permissions.
