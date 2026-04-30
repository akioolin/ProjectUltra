#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

SLEEP_SECONDS=${AGENT_PLANNER_SLEEP_SECONDS:-1800}
MAX_ITERATIONS=${AGENT_PLANNER_MAX_ITERATIONS:-0}
iteration=0

while true; do
  iteration=$((iteration + 1))
  echo "==> planner iteration $iteration"

  set +e
  ./agents/run_planner.sh
  rc=$?
  set -e

  if [[ "$rc" -ne 0 ]]; then
    echo "run_planner exited with rc=$rc" >&2
  elif [[ "${AGENT_PLANNER_PUBLISH_ISSUES:-0}" == "1" ]]; then
    set +e
    ./agents/publish_planner_proposals.sh
    publish_rc=$?
    set -e
    if [[ "$publish_rc" -ne 0 ]]; then
      echo "publish_planner_proposals exited with rc=$publish_rc" >&2
    fi
  fi

  if [[ "$MAX_ITERATIONS" != "0" && "$iteration" -ge "$MAX_ITERATIONS" ]]; then
    exit 0
  fi

  sleep "$SLEEP_SECONDS"
done
