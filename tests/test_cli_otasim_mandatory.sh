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
FILE_OUT=$(mktemp -t cli_otasim_file_1kb.XXXXXX)
FILE_LOG=$(mktemp -t cli_otasim_file_1kb_modem.XXXXXX)
trap 'rm -f "$LOG" "$FILE_OUT" "$FILE_LOG"' EXIT

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

# Regression for the single-process OTASim file-transfer stalls at marginal
# Good/SNR12/R1_4. The covered failure modes are:
#   1. local carrier-sense treating calibrated OTASim idle noise as busy and
#      filling the radio-recovery queue before DATA can leave ALPHA.
#   2. file-transfer waiting bypassing the sample-clock pump after the burst is
#      logically queued, so the BURST never reaches the TX active cursor.
file_start=$SECONDS
set +e
"$CLI" --channel good --snr 12 --rate r1_4 --file 1024 --seed 42 \
    --log-file "$FILE_LOG" --log-level info --log-category modem,operator,audio \
    > "$FILE_OUT" 2>&1
file_status=$?
set -e
file_elapsed=$((SECONDS - file_start))

if (( file_status != 0 )); then
    echo "FAIL: cli_simulator 1KB OTASim file regression failed (exit=$file_status)"
    echo "----- modem recovery-queue lines -----"
    grep "radio recovery queue is full" "$FILE_LOG" || true
    echo "----- cli_simulator output tail -----"
    tail -120 "$FILE_OUT"
    echo "----- modem log tail -----"
    tail -120 "$FILE_LOG"
    exit "$file_status"
fi

if (( file_elapsed >= 120 )); then
    echo "FAIL: cli_simulator 1KB OTASim file regression took ${file_elapsed}s (expected <120s)"
    echo "----- cli_simulator output tail -----"
    tail -120 "$FILE_OUT"
    echo "----- modem log tail -----"
    tail -120 "$FILE_LOG"
    exit 1
fi

if grep -q "radio recovery queue is full" "$FILE_LOG"; then
    echo "FAIL: cli_simulator 1KB OTASim file regression hit radio recovery queue stall"
    echo "----- recovery-queue lines -----"
    grep "radio recovery queue is full" "$FILE_LOG"
    echo "----- cli_simulator output tail -----"
    tail -120 "$FILE_OUT"
    exit 1
fi

if ! grep -q "\\[ALPHA\\] TX active cursor: frame_type=BURST .*data_frames=8" "$FILE_LOG"; then
    echo "FAIL: cli_simulator 1KB OTASim file regression queued the DATA burst but never started TX"
    echo "----- ALPHA TX queue/cursor lines -----"
    grep "\\[ALPHA\\] TX \\(logical queue\\|active cursor\\)" "$FILE_LOG" || true
    echo "----- cli_simulator output tail -----"
    tail -120 "$FILE_OUT"
    echo "----- modem log tail -----"
    tail -120 "$FILE_LOG"
    exit 1
fi

if ! grep -q "TEST PASSED" "$FILE_OUT"; then
    echo "FAIL: cli_simulator 1KB OTASim file regression did not PASS"
    echo "----- cli_simulator output tail -----"
    tail -120 "$FILE_OUT"
    echo "----- modem log tail -----"
    tail -120 "$FILE_LOG"
    exit 1
fi

echo "OK: cli_simulator OTASim mandatory guard and 1KB file-transfer regression passed (${file_elapsed}s)"
exit 0
