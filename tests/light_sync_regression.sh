#!/usr/bin/env bash
set -euo pipefail

# Light preamble sync regression sweep using cli_simulator.
# Focus: MC-DPSK handshake -> OFDM_CHIRP connected flow under fading.

BIN="${BIN:-./build-macos/cli_simulator}"
SNR="${SNR:-15}"
RATE="${RATE:-r1_2}"
CHANNEL="${CHANNEL:-moderate}"
SEEDS=(${SEEDS:-42 43 44 45})
MAX_FAILED="${MAX_FAILED:-10}"

if [[ ! -x "$BIN" ]]; then
  echo "error: binary not found or not executable: $BIN" >&2
  exit 2
fi

overall_rc=0
echo "light-sync regression: bin=$BIN snr=$SNR channel=$CHANNEL rate=$RATE max_failed=$MAX_FAILED"

for seed in "${SEEDS[@]}"; do
  echo
  echo "=== seed=$seed ==="
  tmp_log="$(mktemp)"

  "$BIN" --snr "$SNR" --channel "$CHANNEL" --rate "$RATE" --seed "$seed" --test \
    >"$tmp_log" 2>&1 || true

  if ! rg -q "TEST PASSED" "$tmp_log"; then
    echo "FAIL: simulator test did not pass (seed=$seed)"
    rg -n "TEST PASSED|TEST FAILED|Connection failed|Handshake|Disconnected" "$tmp_log" || true
    overall_rc=1
    rm -f "$tmp_log"
    continue
  fi

  # Collect per-station decode-failure counters from summary.
  max_seen=0
  while IFS= read -r v; do
    [[ -z "$v" ]] && continue
    (( v > max_seen )) && max_seen="$v"
  done < <(rg -o "frames_failed=[0-9]+" "$tmp_log" | cut -d= -f2)

  rejects="$(rg -c "DATA sync rejected" "$tmp_log" || true)"
  weak_accepts="$(rg -c "DATA sync weak-accepted" "$tmp_log" || true)"

  echo "result: PASSED, max_frames_failed=$max_seen, rejected=$rejects, weak_accepted=$weak_accepts"

  if (( max_seen > MAX_FAILED )); then
    echo "FAIL: max_frames_failed=$max_seen exceeds threshold=$MAX_FAILED (seed=$seed)"
    overall_rc=1
  fi

  rm -f "$tmp_log"
done

exit "$overall_rc"
