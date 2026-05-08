#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 /path/to/cli_simulator" >&2
  exit 2
fi

CLI_BIN="$1"
LOG_FILE="$(mktemp "${TMPDIR:-/tmp}/ultra_cli_notch.XXXXXX.log")"
trap 'rm -f "$LOG_FILE"' EXIT

"$CLI_BIN" --snr 15 --fading good --rate r1_2 \
  --mask-clear-carrier 17 --test >"$LOG_FILE" 2>&1

retx="$(
  sed -n '/--- ALPHA (TX) ---/,/--- BRAVO (RX) ---/p' "$LOG_FILE" |
    sed -n 's/.*retransmissions=\([0-9][0-9]*\).*/\1/p' |
    head -1
)"

if [[ -z "$retx" ]]; then
  echo "failed to parse ALPHA retransmissions" >&2
  tail -80 "$LOG_FILE" >&2
  exit 1
fi

if (( retx > 4 )); then
  echo "synthetic-notch regression: retransmissions=$retx > 4" >&2
  tail -120 "$LOG_FILE" >&2
  exit 1
fi

echo "synthetic-notch regression passed: retransmissions=$retx"
