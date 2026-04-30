#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

QUEUE_DIR=${AGENT_QUEUE_DIR:-agents/queue}
ARCHIVE_DIR=${AGENT_ARCHIVE_DIR:-agents/archive}
REPORT_ROOT=${AGENT_REPORT_ROOT:-agents/reports}
TMP_DIR=${AGENT_TMP_DIR:-agents/tmp}
LOCK_DIR=${AGENT_LOCK_DIR:-/tmp/projectultra_agent.lock}
AGENT_NAME=${AGENT_NAME:-agent}
AGENT_CMD=${AGENT_CMD:-}
AGENT_PROMPT_MODE=${AGENT_PROMPT_MODE:-stdin}
BASE_BRANCH=${AGENT_BASE:-main}
REMOTE_NAME=${AGENT_REMOTE:-origin}
UPDATE_BASE=${AGENT_UPDATE_BASE:-1}
ALLOW_BASE_AHEAD=${AGENT_ALLOW_BASE_AHEAD:-0}
ALLOW_DIRTY=${AGENT_ALLOW_DIRTY:-0}
DRY_RUN=${AGENT_DRY_RUN:-0}
RUN_LOCAL_GATE=${AGENT_RUN_LOCAL_GATE:-1}
LOCAL_GATE=${AGENT_LOCAL_GATE:-./agents/run_local_gate.sh}
RUN_HARDWARE=${AGENT_RUN_HARDWARE:-0}
HARDWARE_CMD=${AGENT_HARDWARE_CMD:-./agents/run_hardware_smoke.sh}
AUTO_COMMIT=${AGENT_AUTO_COMMIT:-0}
PUSH_BRANCH=${AGENT_PUSH:-0}
ARCHIVE_TASK=${AGENT_ARCHIVE_TASK:-0}
CREATE_PR=${AGENT_CREATE_PR:-0}
PR_DRAFT=${AGENT_PR_DRAFT:-1}
PR_BASE=${AGENT_PR_BASE:-$BASE_BRANCH}
RETURN_TO_BASE=${AGENT_RETURN_TO_BASE:-1}
ALLOW_AGENT_COMMITS=${AGENT_ALLOW_AGENT_COMMITS:-0}
TIMEOUT_SECONDS=${AGENT_TIMEOUT_SECONDS:-0}

mkdir -p "$QUEUE_DIR" "$ARCHIVE_DIR" "$REPORT_ROOT" "$TMP_DIR"

if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  echo "Agent lock is held: $LOCK_DIR" >&2
  exit 75
fi
trap 'rmdir "$LOCK_DIR" 2>/dev/null || true' EXIT

task_file=$(find "$QUEUE_DIR" -maxdepth 1 -type f -name '*.md' ! -name 'README.md' | sort | head -n 1)
if [[ -z "$task_file" ]]; then
  echo "No queued task in $QUEUE_DIR"
  exit 0
fi

if [[ -z "$AGENT_CMD" && "$DRY_RUN" != "1" ]]; then
  echo "AGENT_CMD is required unless AGENT_DRY_RUN=1" >&2
  exit 2
fi

if [[ "$TIMEOUT_SECONDS" != "0" && "$DRY_RUN" != "1" ]] && ! command -v timeout >/dev/null 2>&1; then
  echo "AGENT_TIMEOUT_SECONDS requires timeout(1), which is not available on this host." >&2
  exit 2
fi

if [[ "$ALLOW_DIRTY" != "1" && "$DRY_RUN" != "1" ]]; then
  status=$(git status --porcelain --untracked-files=all)
  non_queue_status=$(printf '%s\n' "$status" | grep -vE '^[ MARC?D]{2} agents/(queue|reports|tmp)/' || true)
  if [[ -n "$non_queue_status" ]]; then
    echo "Worktree has non-queue changes. Commit/stash them or set AGENT_ALLOW_DIRTY=1." >&2
    printf '%s\n' "$non_queue_status" >&2
    exit 2
  fi
fi

current_branch=$(git branch --show-current)
if [[ -z "$current_branch" ]]; then
  current_branch="detached"
fi

if [[ "$DRY_RUN" != "1" && "$current_branch" != "$BASE_BRANCH" ]]; then
  if [[ "$current_branch" == "detached" ]]; then
    echo "Refusing to start from detached HEAD; switch to $BASE_BRANCH first." >&2
    exit 2
  fi
  git switch "$BASE_BRANCH"
fi

if [[ "$DRY_RUN" != "1" && "$UPDATE_BASE" == "1" ]]; then
  git fetch "$REMOTE_NAME" "$BASE_BRANCH"
  git merge --ff-only "$REMOTE_NAME/$BASE_BRANCH"
  read -r remote_ahead local_ahead < <(git rev-list --left-right --count "$REMOTE_NAME/$BASE_BRANCH...HEAD")
  if [[ "$remote_ahead" != "0" ]]; then
    echo "$BASE_BRANCH is behind $REMOTE_NAME/$BASE_BRANCH after fast-forward attempt." >&2
    exit 2
  fi
  if [[ "$local_ahead" != "0" && "$ALLOW_BASE_AHEAD" != "1" ]]; then
    echo "$BASE_BRANCH has $local_ahead local commit(s) not on $REMOTE_NAME/$BASE_BRANCH." >&2
    echo "Push/rebase them first, or set AGENT_ALLOW_BASE_AHEAD=1 for a deliberate local experiment." >&2
    exit 2
  fi
fi

task_base=$(basename "$task_file" .md)
slug=$(printf '%s' "$task_base" | tr '[:upper:]' '[:lower:]' | tr -cs '[:alnum:]' '-' | sed 's/^-//; s/-$//')
timestamp=$(date +%Y%m%d_%H%M%S)
branch="agent/${timestamp}-${slug}"
report_dir="$REPORT_ROOT/${timestamp}-${slug}"
prompt_file="$TMP_DIR/${timestamp}-${slug}.prompt.md"

mkdir -p "$report_dir"

cat > "$prompt_file" <<EOF
You are working in ProjectUltra, a critical HF modem codebase.

Hard rules:
- Read CLAUDE.md, docs/PROJECT_GOALS.md, docs/INVARIANTS.md, and the task below before editing.
- Treat docs/archive/ as historical context only unless this task explicitly cites an archived file.
- Make the smallest production-quality change that satisfies the task.
- Do not weaken LDPC, ARQ, synchronization, or hardware calibration invariants.
- Do not run destructive git commands.
- Do not run git add, git commit, git push, or gh pr create; the runner handles version control and PR creation after gates pass.
- Do not push directly to main.
- If hardware tests are needed, use the maintained scripts and respect the hardware lock.
- End with changed files, commands run, results, and residual risks.

Task file: $task_file
Base branch requested: $BASE_BRANCH
Worker branch: $branch

$(cat "$task_file")
EOF

echo "Task: $task_file"
echo "Branch: $branch"
echo "Report: $report_dir"
echo "Prompt: $prompt_file"

if [[ "$DRY_RUN" == "1" ]]; then
  echo "Dry run only. No branch, agent, or gates executed."
  exit 0
fi

git switch -c "$branch"

pre_agent_head=$(git rev-parse HEAD)

set +e
case "$AGENT_PROMPT_MODE" in
  stdin)
    if [[ "$TIMEOUT_SECONDS" != "0" ]]; then
      timeout "$TIMEOUT_SECONDS" bash -lc "$AGENT_CMD" < "$prompt_file" > "$report_dir/agent.log" 2>&1
    else
      bash -lc "$AGENT_CMD" < "$prompt_file" > "$report_dir/agent.log" 2>&1
    fi
    agent_rc=$?
    ;;
  file)
    if [[ "$TIMEOUT_SECONDS" != "0" ]]; then
      timeout "$TIMEOUT_SECONDS" bash -lc "$AGENT_CMD '$prompt_file'" > "$report_dir/agent.log" 2>&1
    else
      bash -lc "$AGENT_CMD '$prompt_file'" > "$report_dir/agent.log" 2>&1
    fi
    agent_rc=$?
    ;;
  *)
    echo "Unsupported AGENT_PROMPT_MODE: $AGENT_PROMPT_MODE" >&2
    agent_rc=2
    ;;
esac
set -e

if [[ "$agent_rc" -ne 0 ]]; then
  echo "Agent failed with rc=$agent_rc. Log: $report_dir/agent.log" >&2
  exit "$agent_rc"
fi

post_agent_head=$(git rev-parse HEAD)
if [[ "$ALLOW_AGENT_COMMITS" != "1" && "$post_agent_head" != "$pre_agent_head" ]]; then
  {
    echo "Agent created commits before the runner gate."
    echo "pre_agent_head=$pre_agent_head"
    echo "post_agent_head=$post_agent_head"
    echo
    git log --oneline "$pre_agent_head..$post_agent_head"
  } > "$report_dir/agent_commit_violation.log"
  echo "Agent created commits before gates/runner commit. Log: $report_dir/agent_commit_violation.log" >&2
  exit 2
fi

if [[ "$RUN_LOCAL_GATE" == "1" ]]; then
  AGENT_REPORT_DIR="$report_dir/local_gate" "$LOCAL_GATE" > "$report_dir/local_gate.log" 2>&1
fi

if [[ "$RUN_HARDWARE" == "1" ]]; then
  AGENT_REPORT_DIR="$report_dir/hardware_gate" "$HARDWARE_CMD" > "$report_dir/hardware_gate.log" 2>&1
fi

git status --short > "$report_dir/git_status.txt"
git diff --stat > "$report_dir/git_diff_stat.txt"

task_archived=0

if [[ "$AUTO_COMMIT" == "1" ]]; then
  if ! git diff --quiet || ! git diff --cached --quiet || [[ -n "$(git ls-files --others --exclude-standard)" ]]; then
    git add -A
    git commit -m "Agent task: $task_base"
  else
    echo "No changes to commit."
  fi

  if [[ "$ARCHIVE_TASK" == "1" ]]; then
    mv "$task_file" "$ARCHIVE_DIR/${timestamp}-${task_base}.md"
    task_archived=1
  fi

  if [[ "$PUSH_BRANCH" == "1" ]]; then
    git push -u origin "$branch"
  fi
fi

if [[ "$ARCHIVE_TASK" == "1" && "$task_archived" == "0" ]]; then
  mv "$task_file" "$ARCHIVE_DIR/${timestamp}-${task_base}.md"
  task_archived=1
fi

if [[ "$CREATE_PR" == "1" ]]; then
  if [[ "$PUSH_BRANCH" != "1" ]]; then
    echo "AGENT_CREATE_PR=1 requires AGENT_PUSH=1 so GitHub can see the branch." >&2
    exit 2
  fi
  if ! command -v gh >/dev/null 2>&1; then
    echo "AGENT_CREATE_PR=1 requires GitHub CLI (gh)." >&2
    exit 2
  fi

  pr_body="$report_dir/pr_body.md"
  cat > "$pr_body" <<EOF
Automated agent task.

Task: \`$task_file\`
Branch: \`$branch\`
Base: \`$PR_BASE\`

## Required Review

- Verify the task scope was respected.
- Verify local gates and hardware/benchmark evidence in \`$report_dir\`.
- Reject if throughput, retransmissions, LDPC decode health, or security posture regressed without explanation.

## Agent Report

See local report directory: \`$report_dir\`
EOF

  pr_args=(pr create --base "$PR_BASE" --head "$branch" --title "Agent task: $task_base" --body-file "$pr_body")
  if [[ "$PR_DRAFT" == "1" ]]; then
    pr_args+=(--draft)
  fi
  gh "${pr_args[@]}" > "$report_dir/pr_create.log" 2>&1
fi

returned_to_base=0
if [[ "$RETURN_TO_BASE" == "1" ]]; then
  post_status=$(git status --porcelain --untracked-files=all)
  non_queue_post_status=$(printf '%s\n' "$post_status" | grep -vE '^[ MARC?D]{2} agents/(queue|archive|reports|tmp)/' || true)
  if [[ -z "$non_queue_post_status" ]]; then
    git switch "$BASE_BRANCH" > "$report_dir/return_to_base.log" 2>&1
    returned_to_base=1
  else
    {
      echo "Not returning to $BASE_BRANCH because worktree has non-queue changes:"
      printf '%s\n' "$non_queue_post_status"
    } > "$report_dir/return_to_base.log"
  fi
fi

cat > "$report_dir/summary.txt" <<EOF
task=$task_file
agent=$AGENT_NAME
base_branch=$BASE_BRANCH
remote=$REMOTE_NAME
update_base=$UPDATE_BASE
allow_base_ahead=$ALLOW_BASE_AHEAD
start_branch=$current_branch
worker_branch=$branch
local_gate=$RUN_LOCAL_GATE
hardware_gate=$RUN_HARDWARE
auto_commit=$AUTO_COMMIT
push=$PUSH_BRANCH
create_pr=$CREATE_PR
archive_task=$task_archived
return_to_base=$returned_to_base
allow_agent_commits=$ALLOW_AGENT_COMMITS
timeout_seconds=$TIMEOUT_SECONDS
EOF

echo "Agent task completed. Report: $report_dir"
