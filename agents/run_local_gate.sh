#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

timestamp=$(date +%Y%m%d_%H%M%S)
REPORT_DIR=${AGENT_REPORT_DIR:-agents/reports/local_$timestamp}
BUILD_DIR=${AGENT_BUILD_DIR:-build}
BUILD_JOBS=${AGENT_BUILD_JOBS:-4}
CTEST_JOBS=${AGENT_CTEST_JOBS:-4}
SKIP_COVERAGE=${AGENT_SKIP_COVERAGE:-0}
RUN_REGRESSION=${AGENT_RUN_REGRESSION:-1}

mkdir -p "$REPORT_DIR"

run_step() {
  local name="$1"
  shift
  local log="$REPORT_DIR/$name.log"

  echo "==> $name"
  echo "+ $*" > "$log"
  if "$@" >> "$log" 2>&1; then
    echo "PASS $name"
  else
    local rc=$?
    echo "FAIL $name (log: $log)" >&2
    tail -80 "$log" >&2 || true
    exit "$rc"
  fi
}

run_step artifact_check ./scripts/check_artifacts.sh
run_step cmake_configure cmake -S . -B "$BUILD_DIR"
run_step build cmake --build "$BUILD_DIR" -j "$BUILD_JOBS"
run_step ctest ctest --test-dir "$BUILD_DIR" --output-on-failure -j "$CTEST_JOBS"

if [[ "$RUN_REGRESSION" == "1" ]]; then
  run_step regression_matrix ./tests/regression_matrix.sh --quick
fi

if [[ "$SKIP_COVERAGE" != "1" ]]; then
  run_step coverage ./scripts/coverage_report.sh
fi

run_step diff_check git diff --check

cat > "$REPORT_DIR/summary.txt" <<EOF
local_gate=pass
artifact_check=pass
build_dir=$BUILD_DIR
run_regression=$RUN_REGRESSION
skip_coverage=$SKIP_COVERAGE
timestamp=$timestamp
EOF

echo "Local gate passed. Reports: $REPORT_DIR"
