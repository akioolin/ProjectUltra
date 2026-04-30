#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

APPROVERS=${AGENT_APPROVERS:-}
QUEUE_ROOT=${AGENT_QUEUE_ROOT:-agents/queue}
DRY_RUN=${AGENT_APPROVAL_DRY_RUN:-0}
MAX_ISSUES=${AGENT_APPROVAL_MAX_ISSUES:-50}

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 2
  fi
}

need_cmd gh
need_cmd jq

if [[ -z "$APPROVERS" ]]; then
  echo "AGENT_APPROVERS is required, for example: AGENT_APPROVERS=secup" >&2
  exit 2
fi

slugify() {
  tr '[:upper:]' '[:lower:]' \
    | tr -cs '[:alnum:]' '-' \
    | sed 's/^-//; s/-$//'
}

issue_has_label() {
  local issue_json="$1"
  local label="$2"

  jq -e --arg label "$label" '.labels[]? | select(.name == $label)' "$issue_json" >/dev/null
}

latest_allowed_command() {
  local issue="$1"
  local comments_json="$2"

  gh api "repos/{owner}/{repo}/issues/$issue/comments" --paginate > "$comments_json"
  jq -r --arg approvers "$APPROVERS" '
    def trim: gsub("^[[:space:]]+|[[:space:]]+$"; "");
    def allowed:
      ($approvers | split(",") | map(trim) | map(select(length > 0)));
    [
      .[] |
      select(.user.login as $u | allowed | index($u)) |
      . as $comment |
      ($comment.body | split("\n") | map(trim)) as $lines |
      ["/approve codex", "/approve claude", "/reject", "/hold"][] as $cmd |
      select($lines | index($cmd)) |
      {
        created_at: $comment.created_at,
        login: $comment.user.login,
        command: $cmd
      }
    ]
    | sort_by(.created_at)
    | last
    | if . == null then empty else [.created_at, .login, .command] | @tsv end
  ' "$comments_json"
}

edit_issue_labels() {
  local issue="$1"
  local add="$2"
  local remove="$3"

  if [[ "$DRY_RUN" == "1" ]]; then
    echo "dry-run: issue #$issue add-label=$add remove-label=$remove"
    return 0
  fi

  if [[ -n "$add" ]]; then
    gh issue edit "$issue" --add-label "$add" >/dev/null
  fi
  if [[ -n "$remove" ]]; then
    gh issue edit "$issue" --remove-label "$remove" >/dev/null 2>&1 || true
  fi
}

comment_issue() {
  local issue="$1"
  local body="$2"

  if [[ "$DRY_RUN" == "1" ]]; then
    echo "dry-run: would comment on issue #$issue: $body"
    return 0
  fi

  gh issue comment "$issue" --body "$body" >/dev/null
}

queue_issue() {
  local issue_json="$1"
  local agent="$2"
  local approver="$3"
  local command="$4"

  local number title url body queue_dir slug queue_file
  number=$(jq -r '.number' "$issue_json")
  title=$(jq -r '.title' "$issue_json")
  url=$(jq -r '.url' "$issue_json")
  body=$(jq -r '.body' "$issue_json")
  queue_dir="$QUEUE_ROOT/$agent"
  slug=$(printf '%s' "$title" | sed 's/^Planner proposal: //' | slugify)
  queue_file=$(printf '%s/%03d-github-issue-%s.md' "$queue_dir" "$number" "$slug")

  mkdir -p "$queue_dir"

  if [[ -e "$queue_file" ]]; then
    echo "issue #$number already queued at $queue_file"
  elif [[ "$DRY_RUN" == "1" ]]; then
    echo "dry-run: would queue issue #$number to $queue_file"
  else
    {
      echo "# Approved Planner Proposal"
      echo
      echo "Source issue: $url"
      echo "Approved by: $approver"
      echo "Approval command: $command"
      echo "Target agent: $agent"
      echo
      echo "## Issue Body"
      echo
      printf '%s\n' "$body"
    } > "$queue_file"
  fi

  edit_issue_labels "$number" "approved,queued,agent-$agent" "needs-approval"
  comment_issue "$number" "Queued for \`$agent\` as \`$queue_file\` after approval by \`$approver\`. The worker will create a draft PR if it makes changes."
}

issues_json=$(mktemp)
trap 'rm -f "$issues_json" "$comments_json" "$issue_json"' EXIT
comments_json=$(mktemp)
issue_json=$(mktemp)

gh issue list \
  --state open \
  --label planner-proposal \
  --label needs-approval \
  --json number,title,url,body,labels \
  --limit "$MAX_ISSUES" \
  > "$issues_json"

processed=0
queued=0
rejected=0
held=0

while IFS= read -r issue; do
  [[ -n "$issue" ]] || continue
  gh issue view "$issue" --json number,title,url,body,labels > "$issue_json"

  if ! issue_has_label "$issue_json" planner-proposal || ! issue_has_label "$issue_json" needs-approval; then
    continue
  fi

  command_row=$(latest_allowed_command "$issue" "$comments_json" || true)
  [[ -n "$command_row" ]] || continue

  IFS=$'\t' read -r _created_at approver command <<<"$command_row"
  processed=$((processed + 1))

  case "$command" in
    "/approve codex")
      queue_issue "$issue_json" codex "$approver" "$command"
      queued=$((queued + 1))
      ;;
    "/approve claude")
      queue_issue "$issue_json" claude "$approver" "$command"
      queued=$((queued + 1))
      ;;
    "/reject")
      edit_issue_labels "$issue" "rejected" "needs-approval"
      comment_issue "$issue" "Rejected by allowlisted approver \`$approver\`. No task was queued."
      rejected=$((rejected + 1))
      ;;
    "/hold")
      edit_issue_labels "$issue" "hold" "needs-approval"
      comment_issue "$issue" "Put on hold by allowlisted approver \`$approver\`. No task was queued."
      held=$((held + 1))
      ;;
  esac
done < <(jq -r '.[].number' "$issues_json")

echo "approval processing complete: processed=$processed queued=$queued rejected=$rejected held=$held"
