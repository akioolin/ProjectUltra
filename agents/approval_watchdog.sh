#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

SLEEP_SECONDS=${AGENT_APPROVAL_SLEEP_SECONDS:-300}
MAX_ITERATIONS=${AGENT_APPROVAL_MAX_ITERATIONS:-0}
iteration=0

while true; do
  iteration=$((iteration + 1))
  echo "==> approval iteration $iteration"

  set +e
  ./agents/process_approved_proposals.sh
  rc=$?
  set -e

  if [[ "$rc" -ne 0 ]]; then
    echo "process_approved_proposals exited with rc=$rc" >&2
  fi

  if [[ "$MAX_ITERATIONS" != "0" && "$iteration" -ge "$MAX_ITERATIONS" ]]; then
    exit 0
  fi

  sleep "$SLEEP_SECONDS"
done
