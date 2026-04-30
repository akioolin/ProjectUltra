#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

PROPOSAL_DIR=${AGENT_PLANNER_PROPOSAL_DIR:-agents/planner/proposals}
DRY_RUN=${AGENT_PUBLISH_DRY_RUN:-0}
MAX_ISSUES=${AGENT_PUBLISH_MAX_ISSUES:-10}

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 2
  fi
}

need_cmd gh
need_cmd jq

ensure_label() {
  local name="$1"
  local color="$2"
  local description="$3"

  if [[ "$DRY_RUN" == "1" ]]; then
    echo "dry-run: ensure label $name"
    return 0
  fi

  gh label create "$name" --color "$color" --description "$description" >/dev/null 2>&1 || true
}

proposal_title() {
  local path="$1"
  local heading

  heading=$(sed -n 's/^# //p' "$path" | head -1)
  if [[ -z "$heading" ]]; then
    heading=$(basename "$path" .md)
  fi
  printf 'Planner proposal: %s' "$heading"
}

proposal_labels() {
  local path="$1"
  local title="$2"
  local labels=(planner-proposal needs-approval)
  local heading

  heading=$(sed -n 's/^# //p' "$path" | head -1)

  if grep -Eiq 'hardware|sentinel' <<<"$heading"; then
    labels+=(hardware)
  fi
  if grep -Eiq 'failed pr|triage failed pr' <<<"$heading"; then
    labels+=(failed-pr)
  fi
  if grep -Eiq 'test-only|tests-only|coverage|property tests' <<<"$heading"; then
    labels+=(test-only)
  fi

  local IFS=,
  printf '%s' "${labels[*]}"
}

existing_open_issue_number() {
  local title="$1"

  gh issue list \
    --state open \
    --label planner-proposal \
    --json number,title \
    --limit 200 \
    | jq -r --arg title "$title" '.[] | select(.title == $title) | .number' \
    | head -1
}

write_issue_body() {
  local path="$1"
  local body="$2"

  {
    echo "<!-- projectultra-planner-proposal: v1 -->"
    echo
    echo "Planner generated this proposal from local file:"
    echo
    echo "\`$path\`"
    echo
    echo "Approval commands:"
    echo
    echo "- \`/approve codex\`: queue for Codex"
    echo "- \`/approve claude\`: queue for Claude"
    echo "- \`/hold\`: keep open but stop approval polling"
    echo "- \`/reject\`: reject this proposal"
    echo
    echo "Only usernames listed in \`AGENT_APPROVERS\` on the agentic laptop are honored."
    echo "The approval watcher only writes a local ignored task file; it cannot merge PRs or push to \`main\`."
    echo
    echo "## Proposal"
    echo
    cat "$path"
  } > "$body"
}

ensure_label planner-proposal "7057ff" "Planner-generated task proposal"
ensure_label needs-approval "fbca04" "Waiting for explicit human approval"
ensure_label approved "0e8a16" "Approved by an allowlisted human"
ensure_label queued "1d76db" "Queued locally on the agent runner"
ensure_label rejected "b60205" "Rejected by an allowlisted human"
ensure_label hold "d4c5f9" "Held by an allowlisted human"
ensure_label agent-codex "0969da" "Approved for Codex"
ensure_label agent-claude "5319e7" "Approved for Claude"
ensure_label hardware "c2e0c6" "Hardware or audio-rig related"
ensure_label failed-pr "d93f0b" "Failed PR triage"
ensure_label test-only "bfd4f2" "Test-only or coverage-related"
ensure_label manual-followup "fef2c0" "Human-authored agent follow-up proposal"

published=0
skipped=0
seen_titles=$(mktemp)
trap 'rm -f "$seen_titles"' EXIT

shopt -s nullglob
for proposal in "$PROPOSAL_DIR"/*.md; do
  [[ -f "$proposal" ]] || continue
  title=$(proposal_title "$proposal")
  labels=$(proposal_labels "$proposal" "$title")

  if grep -Fx -q "$title" "$seen_titles"; then
    echo "skip duplicate proposal in this publish pass: $title"
    skipped=$((skipped + 1))
    continue
  fi
  printf '%s\n' "$title" >> "$seen_titles"

  existing=$(existing_open_issue_number "$title")

  if [[ -n "$existing" ]]; then
    echo "skip existing issue #$existing: $title"
    skipped=$((skipped + 1))
    continue
  fi

  if (( published >= MAX_ISSUES )); then
    echo "publish limit reached: $MAX_ISSUES"
    break
  fi

  body_file=$(mktemp)
  write_issue_body "$proposal" "$body_file"

  if [[ "$DRY_RUN" == "1" ]]; then
    echo "dry-run: would create issue: $title labels=$labels"
  else
    gh issue create \
      --title "$title" \
      --body-file "$body_file" \
      --label "$labels"
  fi

  rm -f "$body_file"
  published=$((published + 1))
done

echo "planner issue publish complete: published=$published skipped=$skipped"
