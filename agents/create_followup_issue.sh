#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

TITLE=""
BODY_FILE=""
HARDWARE=0
HOLD=0

usage() {
  cat <<'EOF'
Usage:
  agents/create_followup_issue.sh --title "Short title" --body-file path.md [--hardware] [--hold]

Creates a GitHub issue that feeds the existing agent approval pipeline.

Default labels:
  planner-proposal, needs-approval, manual-followup

Options:
  --hardware    Add the hardware label so planner hardware work stays serialized.
  --hold        Use hold instead of needs-approval. Remove hold and add
                needs-approval later when the task is ready to approve.

After creation, approve from GitHub with:
  /approve codex
  /approve claude
EOF
}

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 2
  fi
}

ensure_label() {
  local name="$1"
  local color="$2"
  local description="$3"

  gh label create "$name" --color "$color" --description "$description" >/dev/null 2>&1 || true
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --title)
      TITLE="${2:-}"
      shift 2
      ;;
    --body-file)
      BODY_FILE="${2:-}"
      shift 2
      ;;
    --hardware)
      HARDWARE=1
      shift
      ;;
    --hold)
      HOLD=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

need_cmd gh

if [[ -z "$TITLE" || -z "$BODY_FILE" ]]; then
  usage >&2
  exit 2
fi

if [[ ! -f "$BODY_FILE" ]]; then
  echo "Body file not found: $BODY_FILE" >&2
  exit 2
fi

issue_title="$TITLE"
if [[ "$issue_title" != Planner\ proposal:* ]]; then
  issue_title="Planner proposal: $issue_title"
fi

ensure_label planner-proposal "7057ff" "Planner-generated or human-authored task proposal"
ensure_label needs-approval "fbca04" "Waiting for explicit human approval"
ensure_label approved "0e8a16" "Approved by an allowlisted human"
ensure_label queued "1d76db" "Queued locally on the agent runner"
ensure_label hold "d4c5f9" "Held by an allowlisted human"
ensure_label agent-codex "0969da" "Approved for Codex"
ensure_label agent-claude "5319e7" "Approved for Claude"
ensure_label hardware "c2e0c6" "Hardware or audio-rig related"
ensure_label manual-followup "fef2c0" "Human-authored agent follow-up proposal"

labels=(planner-proposal manual-followup)
if [[ "$HOLD" == "1" ]]; then
  labels+=(hold)
else
  labels+=(needs-approval)
fi
if [[ "$HARDWARE" == "1" ]]; then
  labels+=(hardware)
fi

body=$(mktemp)
trap 'rm -f "$body"' EXIT

{
  echo "<!-- projectultra-manual-followup: v1 -->"
  echo
  echo "Human-authored follow-up proposal."
  echo
  if [[ "$HOLD" == "1" ]]; then
    echo "Status: held. Remove \`hold\`, add \`needs-approval\`, then comment an approval command when ready."
    echo
  fi
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
  cat "$BODY_FILE"
} > "$body"

label_csv=$(IFS=,; printf '%s' "${labels[*]}")
gh issue create --title "$issue_title" --body-file "$body" --label "$label_csv"
