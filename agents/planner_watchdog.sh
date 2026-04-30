#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

SLEEP_SECONDS=${AGENT_PLANNER_SLEEP_SECONDS:-1800}
MAX_ITERATIONS=${AGENT_PLANNER_MAX_ITERATIONS:-0}
LOCK_DIR=${AGENT_PLANNER_LOCK_DIR:-/tmp/projectultra_planner.lock}
iteration=0

cleanup_lock() {
  rm -f "$LOCK_DIR/pid" 2>/dev/null || true
  rmdir "$LOCK_DIR" 2>/dev/null || true
}

acquire_lock() {
  if mkdir "$LOCK_DIR" 2>/dev/null; then
    echo "$$" > "$LOCK_DIR/pid"
    trap cleanup_lock EXIT
    return 0
  fi

  local pid=""
  if [[ -f "$LOCK_DIR/pid" ]]; then
    pid=$(cat "$LOCK_DIR/pid" 2>/dev/null || true)
  fi

  if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
    echo "Planner watchdog lock is held by pid $pid: $LOCK_DIR" >&2
    exit 75
  fi

  echo "Planner watchdog lock is stale: $LOCK_DIR" >&2
  rm -f "$LOCK_DIR/pid" 2>/dev/null || true
  rmdir "$LOCK_DIR" 2>/dev/null || true
  if ! mkdir "$LOCK_DIR" 2>/dev/null; then
    echo "Planner watchdog lock is held: $LOCK_DIR" >&2
    exit 75
  fi
  echo "$$" > "$LOCK_DIR/pid"
  trap cleanup_lock EXIT
}

acquire_lock

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
