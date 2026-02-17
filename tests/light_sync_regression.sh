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
MAX_REJECT_STREAK="${MAX_REJECT_STREAK:-8}"
MAX_PEAK_BACKLOG_MS="${MAX_PEAK_BACKLOG_MS:-10000}"
MIN_RECOVERY_SUCCESS="${MIN_RECOVERY_SUCCESS:-0}"
MAX_ATTEMPTS_PER_SEED="${MAX_ATTEMPTS_PER_SEED:-2}"

if [[ ! -x "$BIN" ]]; then
  echo "error: binary not found or not executable: $BIN" >&2
  exit 2
fi

overall_rc=0
echo "light-sync regression: bin=$BIN snr=$SNR channel=$CHANNEL rate=$RATE max_failed=$MAX_FAILED max_reject_streak=$MAX_REJECT_STREAK max_peak_backlog_ms=$MAX_PEAK_BACKLOG_MS min_recovery_success=$MIN_RECOVERY_SUCCESS attempts_per_seed=$MAX_ATTEMPTS_PER_SEED"

for seed in "${SEEDS[@]}"; do
  echo
  echo "=== seed=$seed ==="
  tmp_log="$(mktemp)"

  passed=0
  attempt=1
  while (( attempt <= MAX_ATTEMPTS_PER_SEED )); do
    "$BIN" --snr "$SNR" --channel "$CHANNEL" --rate "$RATE" --seed "$seed" --test \
      >"$tmp_log" 2>&1 || true
    if rg -q "TEST PASSED" "$tmp_log"; then
      passed=1
      break
    fi
    echo "warn: seed=$seed attempt=$attempt failed, retrying..."
    ((attempt++))
  done

  if (( passed == 0 )); then
    echo "FAIL: simulator test did not pass after $MAX_ATTEMPTS_PER_SEED attempts (seed=$seed)"
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

  rejects="$( (rg -o "DATA sync rejected" "$tmp_log" || true) | wc -l | tr -d ' ')"
  weak_accepts="$( (rg -o "DATA sync weak-accepted" "$tmp_log" || true) | wc -l | tr -d ' ')"

  max_reject_streak=0
  while IFS= read -r s; do
    [[ -z "$s" ]] && continue
    (( s > max_reject_streak )) && max_reject_streak="$s"
  done < <((rg -o "streak=[0-9]+" "$tmp_log" || true) | cut -d= -f2)

  max_peak_backlog_ms="0"
  while IFS= read -r b; do
    [[ -z "$b" ]] && continue
    max_peak_backlog_ms="$(awk -v a="$max_peak_backlog_ms" -v b="$b" 'BEGIN{print (b>a)?b:a}')"
  done < <((rg -o "peak_backlog_ms=[0-9]+(\\.[0-9]+)?" "$tmp_log" || true) | cut -d= -f2)

  max_recovery_success=0
  while IFS= read -r r; do
    [[ -z "$r" ]] && continue
    (( r > max_recovery_success )) && max_recovery_success="$r"
  done < <((rg -o "SyncR: attempts=[0-9]+  success=[0-9]+" "$tmp_log" || true) | sed -E 's/.*success=([0-9]+)/\1/')

  echo "result: PASSED, max_frames_failed=$max_seen, rejected=$rejects, max_reject_streak=$max_reject_streak, weak_accepted=$weak_accepts, peak_backlog_ms=$max_peak_backlog_ms, max_recovery_success=$max_recovery_success"

  if (( max_seen > MAX_FAILED )); then
    echo "FAIL: max_frames_failed=$max_seen exceeds threshold=$MAX_FAILED (seed=$seed)"
    overall_rc=1
  fi

  if (( max_reject_streak > MAX_REJECT_STREAK )); then
    echo "FAIL: max_reject_streak=$max_reject_streak exceeds threshold=$MAX_REJECT_STREAK (seed=$seed)"
    overall_rc=1
  fi

  if ! awk -v v="$max_peak_backlog_ms" -v t="$MAX_PEAK_BACKLOG_MS" 'BEGIN{exit !(v<=t)}'; then
    echo "FAIL: peak_backlog_ms=$max_peak_backlog_ms exceeds threshold=$MAX_PEAK_BACKLOG_MS (seed=$seed)"
    overall_rc=1
  fi

  if (( max_recovery_success < MIN_RECOVERY_SUCCESS )); then
    echo "FAIL: max_recovery_success=$max_recovery_success below minimum=$MIN_RECOVERY_SUCCESS (seed=$seed)"
    overall_rc=1
  fi

  rm -f "$tmp_log"
done

exit "$overall_rc"
