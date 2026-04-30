#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

timestamp=$(date +%Y%m%d_%H%M%S)
REPORT_DIR=${AGENT_PLANNER_REPORT_DIR:-agents/planner/reports/$timestamp}
PROPOSAL_DIR=${AGENT_PLANNER_PROPOSAL_DIR:-agents/planner/proposals}
CREATE_PROPOSALS=${AGENT_PLANNER_CREATE_PROPOSALS:-1}
MAX_PROPOSALS=${AGENT_PLANNER_MAX_PROPOSALS:-5}

mkdir -p "$REPORT_DIR" "$PROPOSAL_DIR"

proposal_count=0
report="$REPORT_DIR/report.md"

slugify() {
  tr '[:upper:]' '[:lower:]' \
    | tr -cs '[:alnum:]' '-' \
    | sed 's/^-//; s/-$//'
}

can_create_proposal() {
  [[ "$CREATE_PROPOSALS" == "1" && "$proposal_count" -lt "$MAX_PROPOSALS" ]]
}

write_failed_pr_proposal() {
  local number="$1"
  local title="$2"
  local url="$3"
  local head="$4"

  can_create_proposal || return 0

  local slug path
  slug=$(printf 'triage-failed-pr-%s' "$number" | slugify)
  proposal_count=$((proposal_count + 1))
  path=$(printf '%s/%03d-%s-%s.md' "$PROPOSAL_DIR" "$proposal_count" "$timestamp" "$slug")

  cat > "$path" <<EOF
# Triage Failed PR #$number

## Goal

Determine whether PR #$number should be fixed, rerun, or closed. This is a
review/triage task, not an authorization to merge.

Project goal: critical software quality and agentic development discipline from
\`docs/PROJECT_GOALS.md\`.

## Context

- PR: $url
- Title: $title
- Branch: \`$head\`
- Planner report: \`$REPORT_DIR/report.md\`

## Allowed Files

- No code changes for the first pass.
- If the failure is clearly a small tooling/test issue, produce a separate
  repair plan before editing.

## Out Of Scope

- Do not merge the PR.
- Do not bypass CI.
- Do not change modem behavior while doing initial triage.

## Required Local Gate

Not required for initial triage. If a repair is made, run:

\`\`\`bash
./agents/run_local_gate.sh
\`\`\`

## Required Hardware Or Benchmark Gate

Not required unless the PR changes PHY/ARQ/audio/hardware behavior.

## Reject Conditions

- Treating a red CI job as acceptable without a root-cause explanation.
- Closing useful work without preserving the reason in the PR.
- Making production changes before the failure mode is classified.

## Expected Output

- Failure class: real bug, CI flake, stale branch, external dependency, or low-value PR.
- Recommended action: fix branch, rerun CI, close PR, or split into smaller tasks.
- Exact logs or commands used.
EOF

  echo "$path"
}

write_hardware_due_proposal() {
  can_create_proposal || return 0

  local path
  proposal_count=$((proposal_count + 1))
  path=$(printf '%s/%03d-%s-run-hardware-sentinel.md' "$PROPOSAL_DIR" "$proposal_count" "$timestamp")

  cat > "$path" <<EOF
# Run Hardware Sentinel

## Goal

Refresh the hardware baseline so the planner has current Mac/Pi or agentic/Pi
evidence before proposing PHY/ARQ throughput work.

Project goal: hardware reality and channel robustness from
\`docs/PROJECT_GOALS.md\`.

## Context

- No hardware sentinel report was found in \`agents/reports/hardware_sentinel_*\`.
- Planner report: \`$REPORT_DIR/report.md\`

## Allowed Files

- No code changes.

## Out Of Scope

- Do not tune modem constants based on one run.
- Do not commit hardware logs.

## Required Local Gate

Not required.

## Required Hardware Or Benchmark Gate

Run one of:

\`\`\`bash
SSH_KEY="\$HOME/.ssh/id_pi5" ./agents/run_hardware_sentinel.sh
\`\`\`

\`\`\`bash
AGENT_HW_SENTINEL_MODE=nightly SSH_KEY="\$HOME/.ssh/id_pi5" ./agents/run_hardware_sentinel.sh
\`\`\`

## Reject Conditions

- Missing report directory.
- Missing \`summary.txt\` or \`metrics.tsv\`.

## Expected Output

- Report directory path.
- Overall pass/warn/fail.
- Any failed or warning cases.
EOF

  echo "$path"
}

write_hardware_failure_proposal() {
  local summary="$1"
  local state="$2"

  can_create_proposal || return 0

  local path
  proposal_count=$((proposal_count + 1))
  path=$(printf '%s/%03d-%s-investigate-hardware-sentinel.md' "$PROPOSAL_DIR" "$proposal_count" "$timestamp")

  cat > "$path" <<EOF
# Investigate Hardware Sentinel $state

## Goal

Classify the latest hardware sentinel $state and decide whether it is a real
modem regression, hardware/audio setup problem, or transient run noise.

Project goal: hardware reality and channel robustness from
\`docs/PROJECT_GOALS.md\`.

## Context

- Latest sentinel summary: \`$summary\`
- Planner report: \`$REPORT_DIR/report.md\`

## Allowed Files

- No code changes for initial classification.
- If a code defect is proven, write a separate bounded repair task.

## Out Of Scope

- Do not change calibration constants from a single failed run.
- Do not weaken ARQ/LDPC/sync invariants to make the symptom disappear.

## Required Local Gate

Not required for initial classification.

## Required Hardware Or Benchmark Gate

Rerun once to confirm:

\`\`\`bash
SSH_KEY="\$HOME/.ssh/id_pi5" ./agents/run_hardware_sentinel.sh
\`\`\`

## Reject Conditions

- No comparison between the latest run and at least one prior run.
- No classification of PHY loss, ACK loss, audio clipping/silence, decode backlog, or ARQ timing.

## Expected Output

- Confirmed/rejected failure.
- Suspected layer.
- Exact log paths and metrics.
- Next bounded task if code work is justified.
EOF

  echo "$path"
}

write_empty_queue_proposal() {
  can_create_proposal || return 0

  local path
  proposal_count=$((proposal_count + 1))
  path=$(printf '%s/%03d-%s-select-next-bounded-task.md' "$PROPOSAL_DIR" "$proposal_count" "$timestamp")

  cat > "$path" <<EOF
# Select Next Bounded Task

## Goal

Choose the next single bounded task from \`docs/AGENT_TASK_BACKLOG.md\` after
open PRs are reviewed and hardware status is known.

Project goal: agentic development discipline from \`docs/PROJECT_GOALS.md\`.

## Context

- Agent queues are empty.
- Planner report: \`$REPORT_DIR/report.md\`

## Allowed Files

- A new task file under \`agents/queue/claude/\` or \`agents/queue/codex/\`.

## Out Of Scope

- Do not edit production code.
- Do not queue broad goals like "improve throughput"; decompose to one measurable task.

## Required Local Gate

Not required.

## Required Hardware Or Benchmark Gate

Not required.

## Reject Conditions

- Task lacks allowed files, acceptance criteria, commands, or reject conditions.
- Task asks an agent to merge PRs or push to \`main\`.

## Expected Output

- One concrete queued task file or a reason not to queue work.
EOF

  echo "$path"
}

open_prs_json="$REPORT_DIR/open_prs.json"
open_prs_table="$REPORT_DIR/open_prs.tsv"
queue_file="$REPORT_DIR/queues.txt"
proposals_file="$REPORT_DIR/proposals.txt"

git status --short --branch > "$REPORT_DIR/git_status.txt"
git log --oneline -10 > "$REPORT_DIR/recent_commits.txt"
find agents/queue -mindepth 1 -maxdepth 2 -type f -name '*.md' ! -name 'README.md' | sort > "$queue_file"

if command -v gh >/dev/null 2>&1; then
  if gh pr list --state open --json number,title,isDraft,headRefName,url,statusCheckRollup --limit 50 > "$open_prs_json" 2>"$REPORT_DIR/gh_pr_list.err"; then
    :
  else
    echo "[]" > "$open_prs_json"
  fi
else
  echo "[]" > "$open_prs_json"
  echo "gh not found" > "$REPORT_DIR/gh_pr_list.err"
fi

if command -v jq >/dev/null 2>&1; then
  jq -r '
    .[] |
    [
      .number,
      .title,
      .headRefName,
      (if .isDraft then "draft" else "ready" end),
      ([.statusCheckRollup[]? | select(.conclusion == "FAILURE")] | length),
      ([.statusCheckRollup[]? | select(.status != "COMPLETED")] | length),
      .url
    ] | @tsv
  ' "$open_prs_json" > "$open_prs_table"
else
  : > "$open_prs_table"
fi

: > "$proposals_file"

if command -v jq >/dev/null 2>&1; then
  while IFS=$'\t' read -r number title url head; do
    [[ -n "$number" ]] || continue
    write_failed_pr_proposal "$number" "$title" "$url" "$head" >> "$proposals_file"
  done < <(
    jq -r '
      .[] |
      select(any(.statusCheckRollup[]?; .conclusion == "FAILURE")) |
      [.number, .title, .url, .headRefName] | @tsv
    ' "$open_prs_json"
  )
fi

latest_hw_summary=$(find agents/reports -maxdepth 2 -type f -path '*/summary.txt' 2>/dev/null \
  | grep '/hardware_sentinel_' \
  | sort \
  | tail -1 || true)

latest_hw_state=""
if [[ -z "$latest_hw_summary" ]]; then
  write_hardware_due_proposal >> "$proposals_file"
else
  latest_hw_state=$(awk -F= '/^hardware_sentinel=/ {print $2}' "$latest_hw_summary" | tail -1)
  if [[ "$latest_hw_state" != "pass" ]]; then
    write_hardware_failure_proposal "$latest_hw_summary" "${latest_hw_state:-unknown}" >> "$proposals_file"
  fi
fi

queue_count=$(wc -l < "$queue_file" | tr -d ' ')
if [[ "$queue_count" == "0" ]]; then
  write_empty_queue_proposal >> "$proposals_file"
fi

{
  echo "# Planner Report"
  echo
  echo "Generated: $timestamp"
  echo "Head: $(git rev-parse --short HEAD)"
  echo
  echo "## Queues"
  echo
  if [[ -s "$queue_file" ]]; then
    sed 's/^/- `/' "$queue_file" | sed 's/$/`/'
  else
    echo "- empty"
  fi
  echo
  echo "## Open PRs"
  echo
  if [[ -s "$open_prs_table" ]]; then
    awk -F'\t' '{printf "- #%s `%s` [%s] failures=%s pending=%s %s\n", $1, $3, $4, $5, $6, $7}' "$open_prs_table"
  else
    echo "- none or unavailable"
  fi
  echo
  echo "## Hardware Sentinel"
  echo
  if [[ -n "$latest_hw_summary" ]]; then
    echo "- latest summary: \`$latest_hw_summary\`"
    echo "- state: ${latest_hw_state:-unknown}"
  else
    echo "- no hardware sentinel report found"
  fi
  echo
  echo "## Proposals"
  echo
  if [[ -s "$proposals_file" ]]; then
    sed 's/^/- `/' "$proposals_file" | sed 's/$/`/'
  else
    echo "- none"
  fi
} > "$report"

cat > "$REPORT_DIR/summary.txt" <<EOF
planner=pass
timestamp=$timestamp
head=$(git rev-parse --short HEAD)
queue_count=$queue_count
proposal_count=$proposal_count
latest_hw_summary=$latest_hw_summary
latest_hw_state=$latest_hw_state
report=$report
EOF

echo "Planner report: $report"
if [[ -s "$proposals_file" ]]; then
  echo "Planner proposals:"
  sed 's/^/  /' "$proposals_file"
else
  echo "Planner proposals: none"
fi
