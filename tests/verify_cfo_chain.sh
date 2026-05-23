#!/usr/bin/env bash
#
# End-to-end CFO chain verification:
# 1) Run cli_simulator with injected TX CFO and internal pre/post CFO dumps enabled
# 2) Verify post*conj(pre) phase slope matches expected correction using Python helper
#
# Usage:
#   ./tests/verify_cfo_chain.sh
#   ./tests/verify_cfo_chain.sh --cfo 50 --channel awgn --snr 20 --seed 42 --calls 6
#
# Exit:
#   0 on success, non-zero on failure

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
SIM_BIN="$BUILD_DIR/cli_simulator"
VERIFY_PY="$PROJECT_DIR/tools/verify_cfo_chain_dump.py"

# Defaults tuned for deterministic proof
EXPECTED_CFO=50
SNR=20
SEED=42
CHANNEL="awgn"          # awgn|good|moderate|poor|flutter
WAVEFORM="ofdm_chirp"
MOD="dqpsk"
RATE="r1_2"
CALLS=6
TOL=0.2
PREFIX="/tmp/cfo_chain_dump"
KEEP=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cfo) EXPECTED_CFO="$2"; shift 2 ;;
        --snr) SNR="$2"; shift 2 ;;
        --seed) SEED="$2"; shift 2 ;;
        --channel) CHANNEL="$2"; shift 2 ;;
        --waveform) WAVEFORM="$2"; shift 2 ;;
        --mod) MOD="$2"; shift 2 ;;
        --rate) RATE="$2"; shift 2 ;;
        --calls) CALLS="$2"; shift 2 ;;
        --tolerance) TOL="$2"; shift 2 ;;
        --prefix) PREFIX="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help)
            cat <<EOF
Usage: ./tests/verify_cfo_chain.sh [options]

Options:
  --cfo <Hz>          Injected TX CFO (default: 50)
  --snr <dB>          Channel SNR (default: 20)
  --seed <N>          RNG seed (default: 42)
  --channel <name>    awgn|good|moderate|poor|flutter (default: awgn)
  --waveform <name>   mc_dpsk|ofdm_chirp|ofdm_narrow (default: ofdm_chirp)
  --mod <name>        dqpsk|d8psk|dbpsk|qpsk|bpsk (default: dqpsk)
  --rate <name>       r1_4|r1_2|r2_3|r3_4 (default: r1_2)
  --calls <N>         Number of internal dump calls (default: 6)
  --tolerance <Hz>    Allowed error in applied CFO estimate (default: 0.2)
  --prefix <path>     Dump file prefix (default: /tmp/cfo_chain_dump)
  --keep              Keep generated log/dump files
EOF
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 2
            ;;
    esac
done

if [[ ! -x "$SIM_BIN" ]]; then
    echo "ERROR: $SIM_BIN not found/executable. Build first (cmake --build build)." >&2
    exit 2
fi

if [[ ! -x "$VERIFY_PY" ]]; then
    echo "ERROR: $VERIFY_PY not found/executable." >&2
    exit 2
fi

LOG_PATH="${PREFIX}_run.log"

if [[ "$KEEP" -eq 0 ]]; then
    rm -f "${PREFIX}"_*.cf32 "${PREFIX}"_*.txt "${PREFIX}"_run.log || true
fi

echo "=== CFO Chain Verify ==="
echo "  simulator: $SIM_BIN"
echo "  cfo_hz:    $EXPECTED_CFO"
echo "  snr_db:    $SNR"
echo "  channel:   $CHANNEL"
echo "  waveform:  $WAVEFORM"
echo "  mod/rate:  $MOD $RATE"
echo "  seed:      $SEED"
echo "  dump_calls:$CALLS"
echo "  prefix:    $PREFIX"
echo

ULTRA_DUMP_CFO_PREFIX="$PREFIX" \
ULTRA_DUMP_CFO_CALLS="$CALLS" \
"$SIM_BIN" \
  --snr "$SNR" \
  --channel "$CHANNEL" \
  --waveform "$WAVEFORM" \
  --mod "$MOD" \
  --rate "$RATE" \
  --seed "$SEED" \
  --tx-cfo "$EXPECTED_CFO" \
  --test >"$LOG_PATH" 2>&1

echo "Simulator run complete: $LOG_PATH"

python3 "$VERIFY_PY" \
  --prefix "$PREFIX" \
  --expected-cfo "$EXPECTED_CFO" \
  --tolerance "$TOL"

echo
echo "CFO chain verification: PASS"

