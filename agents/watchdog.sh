#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

SLEEP_SECONDS=${AGENT_SLEEP_SECONDS:-300}
MAX_ITERATIONS=${AGENT_MAX_ITERATIONS:-0}
iteration=0

while true; do
  iteration=$((iteration + 1))
  echo "==> watchdog iteration $iteration"

  set +e
  ./agents/run_next_task.sh
  rc=$?
  set -e

  if [[ "$rc" -ne 0 && "$rc" -ne 75 ]]; then
    echo "run_next_task exited with rc=$rc" >&2
  fi

  if [[ "$MAX_ITERATIONS" != "0" && "$iteration" -ge "$MAX_ITERATIONS" ]]; then
    exit 0
  fi

  sleep "$SLEEP_SECONDS"
done
