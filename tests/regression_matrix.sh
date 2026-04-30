#!/usr/bin/env bash
set -euo pipefail

# Maintained regression entry point.
#
# Quick mode runs the default CTest suite. Full mode also runs the maintained
# light-sync/cli_simulator sweep if the simulator binary is present.
#
# Usage:
#   ./tests/regression_matrix.sh [--quick|--full]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build}"
MODE="${1:---quick}"
CTEST_JOBS="${CTEST_JOBS:-4}"

if [[ "$MODE" != "--quick" && "$MODE" != "--full" ]]; then
  echo "usage: $0 [--quick|--full]" >&2
  exit 2
fi

if [[ ! -d "$BUILD_DIR" ]]; then
  echo "error: build directory not found: $BUILD_DIR" >&2
  echo "run cmake --build first" >&2
  exit 2
fi

echo "ProjectUltra regression suite"
echo "  mode:      $MODE"
echo "  build dir: $BUILD_DIR"
echo

ctest --test-dir "$BUILD_DIR" --output-on-failure -j "$CTEST_JOBS"

if [[ "$MODE" == "--full" ]]; then
  CLI_BIN="${BIN:-$BUILD_DIR/cli_simulator}"
  if [[ -x "$CLI_BIN" ]]; then
    BIN="$CLI_BIN" "$SCRIPT_DIR/light_sync_regression.sh"
  else
    echo "skip: cli_simulator not found at $CLI_BIN"
  fi
fi
