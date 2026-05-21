#!/usr/bin/env bash
# Enforces that cli_simulator's default --role both path MUST traverse
# OTASim. If anyone reintroduces an in-process audio bypass, this test
# fails CI loudly.
#
# Sentinel: the cli_simulator must print "OTASim BACKEND LOCKED" exactly
# once on a successful run (default role, default audio path).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CLI="$REPO_ROOT/build/cli_simulator"

if [ ! -x "$CLI" ]; then
    echo "FAIL: cli_simulator not built at $CLI"
    echo "      Run: cmake --build $REPO_ROOT/build --target cli_simulator"
    exit 1
fi

if [ ! -x "$REPO_ROOT/build/ota_simulator" ]; then
    echo "FAIL: ota_simulator not built at $REPO_ROOT/build/ota_simulator"
    echo "      Run: cmake --build $REPO_ROOT/build --target ota_simulator"
    exit 1
fi

LOG=$(mktemp -t cli_otasim_mandatory.XXXXXX)
trap 'rm -f "$LOG"' EXIT

# Quick AWGN run — cheap, never retransmits, just exercises the bring-up.
"$CLI" --snr 20 --channel awgn --rate r1_4 --waveform ofdm_chirp --test --seed 1 \
    > "$LOG" 2>&1 || true

if ! grep -q "OTASim BACKEND IS MANDATORY" "$LOG"; then
    echo "FAIL: 'OTASim BACKEND IS MANDATORY' banner missing from cli_simulator output"
    echo "      An in-process bypass may have been reintroduced."
    echo "----- cli_simulator output -----"
    cat "$LOG"
    exit 1
fi

if ! grep -q "OTASim BACKEND LOCKED" "$LOG"; then
    echo "FAIL: 'OTASim BACKEND LOCKED' line missing from cli_simulator output"
    echo "      cli_simulator did not reach the OTASim spawn/connect step."
    echo "----- cli_simulator output -----"
    cat "$LOG"
    exit 1
fi

# Belt-and-suspenders: ensure TEST PASSED is in the output (so we don't
# pass the test just because the binary failed before reaching anything).
if ! grep -q "TEST PASSED" "$LOG"; then
    echo "FAIL: cli_simulator reached the banner but did not PASS"
    echo "----- cli_simulator output -----"
    cat "$LOG"
    exit 1
fi

echo "OK: cli_simulator OTASim-mandatory guard intact (banner + LOCKED line + TEST PASSED)"
exit 0
