#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 /path/to/cli_simulator" >&2
  exit 2
fi

CLI="$1"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

run_ok() {
  local name="$1"
  shift
  "$CLI" "$@" >"$TMP_DIR/$name.out" 2>"$TMP_DIR/$name.err"
}

run_fail() {
  local name="$1"
  shift
  if "$CLI" "$@" >"$TMP_DIR/$name.out" 2>"$TMP_DIR/$name.err"; then
    echo "FAIL: $name unexpectedly passed" >&2
    cat "$TMP_DIR/$name.out" "$TMP_DIR/$name.err" >&2
    exit 1
  fi
}

run_ok nominal --tx-drive 0.5 --help
grep -q -- "--tx-drive <0.05..0.70>" "$TMP_DIR/nominal.out"

run_ok high_clamp --tx-drive 1.5 --help
grep -q "clamped to 0.700" "$TMP_DIR/high_clamp.err"

run_ok low_clamp --tx-drive 0.01 --help
grep -q "clamped to 0.050" "$TMP_DIR/low_clamp.err"

run_fail invalid --tx-drive abc
grep -q "Invalid --tx-drive: abc" "$TMP_DIR/invalid.err"

run_fail missing --tx-drive
grep -q "Missing value for --tx-drive" "$TMP_DIR/missing.err"

"$CLI" --tx-drive 0.5 --log-category modem --hardware-tx-normalization-self-test \
  >"$TMP_DIR/selftest.log" 2>&1
grep -q "HardwareAudioPort: TX queue" "$TMP_DIR/selftest.log"
grep -q "target=0.500" "$TMP_DIR/selftest.log"
grep -q "post_norm_peak=0.5000" "$TMP_DIR/selftest.log"
grep -q "hardware_tx_normalization_self_test PASS" "$TMP_DIR/selftest.log"

echo "OK: cli_simulator tx-drive parse, clamp, and HardwareAudioPort peak-normalization self-test"
