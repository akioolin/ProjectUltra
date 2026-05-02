#!/usr/bin/env bash
#
# tnc_loopback_test.sh - two-station ultra_tnc hardware loopback driver.
#
# This starts real audio-backed ultra_tnc instances on this Mac and on a Pi.
# Do not run it while another hardware/audio sweep is using the soundcard.

set -euo pipefail

# Defaults, overridable by environment or CLI.
PI=${PI:-math@pi5tester}
PI_REPO=${PI_REPO:-/home/math/ProjectUltra}
PI_AUDIO_OUT=${PI_AUDIO_OUT:-USB Audio Device, USB Audio}
PI_AUDIO_IN=${PI_AUDIO_IN:-USB Audio Device, USB Audio}
MAC_AUDIO_OUT=${MAC_AUDIO_OUT:-Sound Blaster Play! 3}
MAC_AUDIO_IN=${MAC_AUDIO_IN:-Sound Blaster Play! 3}
MAC_CALLSIGN=${MAC_CALLSIGN:-ALPHA}
PI_CALLSIGN=${PI_CALLSIGN:-BRAVO}
TNC_PORT=${TNC_PORT:-18300}
PAYLOAD_SIZE=${PAYLOAD_SIZE:-5120}

BIND_TIMEOUT=${BIND_TIMEOUT:-20}
CMD_TIMEOUT=${CMD_TIMEOUT:-5}
CONNECT_TIMEOUT=${CONNECT_TIMEOUT:-60}
RX_TIMEOUT=${RX_TIMEOUT:-180}
DISCONNECT_TIMEOUT=${DISCONNECT_TIMEOUT:-45}
NC_IO_TIMEOUT=${NC_IO_TIMEOUT:-5}
START_SETTLE_SECONDS=${START_SETTLE_SECONDS:-5}
DATA_CAPTURE_SETTLE_SECONDS=${DATA_CAPTURE_SETTLE_SECONDS:-1}

usage() {
  sed -n '2,12p' "$0" | sed 's/^# \?//'
  cat <<EOF

Usage:
  $0 [options]

Options:
  --pi HOST                 SSH target for the Pi (default: $PI)
  --pi-repo PATH            ProjectUltra checkout on the Pi (default: $PI_REPO)
  --port N                  TNC command port; data uses N+1 (default: $TNC_PORT)
  --payload-size BYTES      Binary payload size (default: $PAYLOAD_SIZE)
  --connect-timeout SEC     Wait for CONNECTED (default: $CONNECT_TIMEOUT)
  --rx-timeout SEC          Wait for Pi data-port bytes (default: $RX_TIMEOUT)
  -h, --help                Show this help

Environment:
  PI_AUDIO_OUT, PI_AUDIO_IN, MAC_AUDIO_OUT, MAC_AUDIO_IN
  MAC_CALLSIGN, PI_CALLSIGN, SSH_KEY, SSH_EXTRA_OPTS
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pi) PI="$2"; shift 2;;
    --pi-repo) PI_REPO="$2"; shift 2;;
    --port) TNC_PORT="$2"; shift 2;;
    --payload-size) PAYLOAD_SIZE="$2"; shift 2;;
    --connect-timeout) CONNECT_TIMEOUT="$2"; shift 2;;
    --rx-timeout) RX_TIMEOUT="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done

DATA_PORT=$((TNC_PORT + 1))
RUN_ID=$(date +%Y%m%d_%H%M%S)
LOG_DIR=/tmp/ultra_tnc_loopback_$RUN_ID
REMOTE_TNC_LOG=/tmp/ultra_tnc_B.log
REMOTE_RX=/tmp/ultra_tnc_loopback_${RUN_ID}_rx.bin
REMOTE_RX_LOG=/tmp/ultra_tnc_loopback_${RUN_ID}_rx.log
REMOTE_RX_READY=/tmp/ultra_tnc_loopback_${RUN_ID}_rx.ready

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
MAC_BIN=$REPO_ROOT/build/ultra_tnc

MAC_TNC_PID=""
PI_TNC_PID=""
MAC_CMD_PID=""
PI_CMD_PID=""
PI_RX_PID=""
MAC_CMD_FIFO=""
PI_CMD_FIFO=""
MAC_CMD_OUT=""
PI_CMD_OUT=""
PAYLOAD=""
RX_LOCAL=""
CLEANED_UP=0

SSH_ARGS=()
if [[ -n "${SSH_KEY:-}" ]]; then
  SSH_ARGS+=(-i "$SSH_KEY")
fi
if [[ -n "${SSH_EXTRA_OPTS:-}" ]]; then
  # Same spirit as tools/run_hw_test.sh: allow simple space-separated ssh args.
  # Complex shell-quoted values should be wrapped in a local ssh config entry.
  # shellcheck disable=SC2206
  EXTRA_SSH_ARGS=($SSH_EXTRA_OPTS)
  SSH_ARGS+=("${EXTRA_SSH_ARGS[@]}")
fi

die() {
  echo "ERROR: $*" >&2
  exit 1
}

info() {
  printf '%s\n' "$*"
}

require_tool() {
  command -v "$1" >/dev/null 2>&1 || die "required tool not found: $1"
}

shell_quote() {
  printf "'"
  printf "%s" "$1" | sed "s/'/'\\\\''/g"
  printf "'"
}

ssh_pi() {
  ssh "${SSH_ARGS[@]}" "$PI" "$@"
}

file_size() {
  wc -c < "$1" | tr -d '[:space:]'
}

now_s() {
  date +%s
}

cleanup() {
  local rc=$?
  if [[ "$CLEANED_UP" == "1" ]]; then
    return "$rc"
  fi
  CLEANED_UP=1
  set +e

  exec 3>&- 2>/dev/null
  exec 4>&- 2>/dev/null

  [[ -n "$MAC_CMD_PID" ]] && kill "$MAC_CMD_PID" 2>/dev/null
  [[ -n "$PI_CMD_PID" ]] && kill "$PI_CMD_PID" 2>/dev/null
  [[ -n "$MAC_TNC_PID" ]] && kill "$MAC_TNC_PID" 2>/dev/null

  if [[ -n "$PI_TNC_PID" || -n "$PI_RX_PID" ]]; then
    local remote_cleanup=""
    [[ -n "$PI_RX_PID" ]] && remote_cleanup="$remote_cleanup kill $PI_RX_PID 2>/dev/null || true;"
    [[ -n "$PI_TNC_PID" ]] && remote_cleanup="$remote_cleanup kill $PI_TNC_PID 2>/dev/null || true;"
    remote_cleanup="$remote_cleanup pkill -9 ultra_tnc 2>/dev/null || true;"
    remote_cleanup="$remote_cleanup rm -f $(shell_quote "$REMOTE_RX_READY") 2>/dev/null || true;"
    ssh_pi "$remote_cleanup" >/dev/null 2>&1
  fi

  return "$rc"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

wait_for_local_tcp() {
  local port="$1" timeout_s="$2" pid="$3"
  local deadline=$(( $(now_s) + timeout_s ))
  while (( $(now_s) <= deadline )); do
    if nc -z -w 1 127.0.0.1 "$port" >/dev/null 2>&1; then
      return 0
    fi
    if [[ -n "$pid" ]] && ! kill -0 "$pid" 2>/dev/null; then
      return 1
    fi
    sleep 0.25
  done
  return 1
}

wait_for_pi_tcp() {
  local port="$1" timeout_s="$2"
  local deadline=$(( $(now_s) + timeout_s ))
  while (( $(now_s) <= deadline )); do
    if ssh_pi "nc -z -w 1 127.0.0.1 $port >/dev/null 2>&1"; then
      return 0
    fi
    sleep 0.5
  done
  return 1
}

wait_for_cmd_pattern() {
  local label="$1" file="$2" pattern="$3" timeout_s="$4" offset="$5" pid="$6"
  local start_byte=$((offset + 1))
  local deadline=$(( $(now_s) + timeout_s ))
  local line=""

  while (( $(now_s) <= deadline )); do
    if line=$(tail -c +"$start_byte" "$file" 2>/dev/null | tr '\r' '\n' | grep -E -m1 "$pattern"); then
      printf '%s\n' "$line"
      return 0
    fi
    if [[ -n "$pid" ]] && ! kill -0 "$pid" 2>/dev/null; then
      echo "Command socket closed while waiting for $label: $pattern" >&2
      tail -c 2000 "$file" | tr '\r' '\n' >&2 || true
      return 1
    fi
    sleep 0.2
  done

  echo "Timed out after ${timeout_s}s waiting for $label: $pattern" >&2
  echo "Recent command output from $file:" >&2
  tail -c 2000 "$file" | tr '\r' '\n' >&2 || true
  return 1
}

send_cmd_expect() {
  local label="$1" fd="$2" out_file="$3" pid="$4" line="$5" pattern="${6:-^OK$}"
  local offset
  offset=$(file_size "$out_file")
  printf '%s\r' "$line" >&$fd
  wait_for_cmd_pattern "$label response to '$line'" "$out_file" "$pattern" "$CMD_TIMEOUT" "$offset" "$pid" >/dev/null
}

start_mac_cmd_client() {
  MAC_CMD_FIFO=$LOG_DIR/mac_cmd.in
  MAC_CMD_OUT=$LOG_DIR/mac_cmd.out
  mkfifo "$MAC_CMD_FIFO"
  : > "$MAC_CMD_OUT"
  nc 127.0.0.1 "$TNC_PORT" < "$MAC_CMD_FIFO" > "$MAC_CMD_OUT" &
  MAC_CMD_PID=$!
  exec 3>"$MAC_CMD_FIFO"
}

start_pi_cmd_client() {
  PI_CMD_FIFO=$LOG_DIR/pi_cmd.in
  PI_CMD_OUT=$LOG_DIR/pi_cmd.out
  mkfifo "$PI_CMD_FIFO"
  : > "$PI_CMD_OUT"
  ssh_pi "nc 127.0.0.1 $TNC_PORT" < "$PI_CMD_FIFO" > "$PI_CMD_OUT" &
  PI_CMD_PID=$!
  exec 4>"$PI_CMD_FIFO"
}

start_pi_data_capture() {
  local remote_rx_q remote_log_q remote_ready_q
  remote_rx_q=$(shell_quote "$REMOTE_RX")
  remote_log_q=$(shell_quote "$REMOTE_RX_LOG")
  remote_ready_q=$(shell_quote "$REMOTE_RX_READY")

  ssh_pi "rm -f $remote_rx_q $remote_log_q $remote_ready_q"
  ssh_pi "nohup sh -c 'touch $remote_ready_q; nc 127.0.0.1 $DATA_PORT | dd of=$remote_rx_q bs=1 count=$PAYLOAD_SIZE 2>>$remote_log_q' > $remote_log_q 2>&1 & echo \$!" > "$LOG_DIR/pi_rx_pid.txt"
  PI_RX_PID=$(tr -d '[:space:]' < "$LOG_DIR/pi_rx_pid.txt")
}

wait_for_pi_file_size() {
  local remote_file="$1" wanted="$2" timeout_s="$3"
  local remote_file_q
  remote_file_q=$(shell_quote "$remote_file")
  local deadline=$(( $(now_s) + timeout_s ))
  local size="0"

  while (( $(now_s) <= deadline )); do
    size=$(ssh_pi "if [ -f $remote_file_q ]; then wc -c < $remote_file_q; else echo 0; fi" | tr -d '[:space:]')
    if [[ "$size" == "$wanted" ]]; then
      return 0
    fi
    sleep 1
  done

  echo "Timed out after ${timeout_s}s waiting for $wanted bytes on Pi data port (last size: ${size:-unknown})" >&2
  return 1
}

send_payload_to_mac_data_port() {
  local out_log=$LOG_DIR/mac_data_send.out
  local err_log=$LOG_DIR/mac_data_send.err
  nc -w "$NC_IO_TIMEOUT" 127.0.0.1 "$DATA_PORT" < "$PAYLOAD" > "$out_log" 2> "$err_log"
}

crc_and_size() {
  cksum "$1" | awk '{print $1 " " $2}'
}

collect_pi_log() {
  ssh_pi "cat $(shell_quote "$REMOTE_TNC_LOG") 2>/dev/null || true" > "$LOG_DIR/pi_tnc.log" || true
  ssh_pi "cat $(shell_quote "$REMOTE_RX_LOG") 2>/dev/null || true" > "$LOG_DIR/pi_rx.log" || true
}

for tool in awk cksum cmp dd grep mkfifo nc sed ssh tail tr wc; do
  require_tool "$tool"
done

[[ -x "$MAC_BIN" ]] || die "Mac ultra_tnc not found at $MAC_BIN; build target ultra_tnc first"

mkdir -p "$LOG_DIR"
PAYLOAD=$LOG_DIR/payload.bin
RX_LOCAL=$LOG_DIR/rx_from_pi.bin

# PAYLOAD_FILL controls payload content. RANDOM (default) tests raw
# byte-exactness. TEXT exercises the compression path with highly
# repetitive ASCII (well-formed prose-shaped pseudo-text).
case "${PAYLOAD_FILL:-RANDOM}" in
  RANDOM|random)
    dd if=/dev/urandom of="$PAYLOAD" bs="$PAYLOAD_SIZE" count=1 >/dev/null 2>&1
    ;;
  TEXT|text)
    # Repeat a fixed phrase up to PAYLOAD_SIZE. deflate ratio ~50x on
    # this kind of input. awk avoids SIGPIPE issues that bite a
    # `yes | tr | head -c` pipeline under `set -o pipefail`.
    awk -v n="$PAYLOAD_SIZE" 'BEGIN {
      s = "The quick brown fox jumps over the lazy dog. Pack my box now!";
      l = length(s);
      out = "";
      while (length(out) < n) out = out s;
      printf "%s", substr(out, 1, n);
    }' > "$PAYLOAD"
    ;;
  *)
    die "PAYLOAD_FILL must be RANDOM or TEXT, got: $PAYLOAD_FILL"
    ;;
esac

info "Logs: $LOG_DIR"
info "Mac: $MAC_CALLSIGN port=$TNC_PORT data=$DATA_PORT audio out='$MAC_AUDIO_OUT' in='$MAC_AUDIO_IN'"
info "Pi:  $PI_CALLSIGN target=$PI repo=$PI_REPO audio out='$PI_AUDIO_OUT' in='$PI_AUDIO_IN'"
info "Payload: $PAYLOAD_SIZE bytes"
info

info "[1/7] Checking Pi prerequisites..."
ssh_pi "test -x $(shell_quote "$PI_REPO")/build/ultra_tnc && command -v nc >/dev/null && command -v dd >/dev/null" \
  || die "Pi is missing build/ultra_tnc, nc, or dd"

info "[2/7] Starting ultra_tnc on Pi..."
PI_REPO_Q=$(shell_quote "$PI_REPO")
PI_AUDIO_OUT_Q=$(shell_quote "$PI_AUDIO_OUT")
PI_AUDIO_IN_Q=$(shell_quote "$PI_AUDIO_IN")
PI_CALLSIGN_Q=$(shell_quote "$PI_CALLSIGN")
REMOTE_TNC_LOG_Q=$(shell_quote "$REMOTE_TNC_LOG")
PI_CMD="cd $PI_REPO_Q || exit 1; \
  pkill -9 ultra_tnc 2>/dev/null || true; \
  rm -f $REMOTE_TNC_LOG_Q; \
  nohup ./build/ultra_tnc --port $TNC_PORT \
    --audio-output $PI_AUDIO_OUT_Q \
    --audio-input $PI_AUDIO_IN_Q \
    --callsign $PI_CALLSIGN_Q \
    > $REMOTE_TNC_LOG_Q 2>&1 & \
  echo \$!"
ssh_pi "$PI_CMD" > "$LOG_DIR/pi_tnc_pid.txt"
PI_TNC_PID=$(tr -d '[:space:]' < "$LOG_DIR/pi_tnc_pid.txt")
info "    Pi ultra_tnc PID=$PI_TNC_PID"

info "[3/7] Starting ultra_tnc locally..."
MAC_ARGS=(--port "$TNC_PORT" --callsign "$MAC_CALLSIGN")
[[ -n "$MAC_AUDIO_OUT" ]] && MAC_ARGS+=(--audio-output "$MAC_AUDIO_OUT")
[[ -n "$MAC_AUDIO_IN" ]] && MAC_ARGS+=(--audio-input "$MAC_AUDIO_IN")
"$MAC_BIN" "${MAC_ARGS[@]}" > "$LOG_DIR/mac_tnc.log" 2>&1 &
MAC_TNC_PID=$!
info "    Mac ultra_tnc PID=$MAC_TNC_PID"

sleep "$START_SETTLE_SECONDS"

info "[4/7] Waiting for TCP ports..."
wait_for_pi_tcp "$TNC_PORT" "$BIND_TIMEOUT" || die "Pi command port did not bind"
wait_for_pi_tcp "$DATA_PORT" "$BIND_TIMEOUT" || die "Pi data port did not bind"
wait_for_local_tcp "$TNC_PORT" "$BIND_TIMEOUT" "$MAC_TNC_PID" || die "Mac command port did not bind"
wait_for_local_tcp "$DATA_PORT" "$BIND_TIMEOUT" "$MAC_TNC_PID" || die "Mac data port did not bind"

info "[5/7] Opening command sockets and arming Pi data capture..."
start_pi_cmd_client
start_mac_cmd_client

send_cmd_expect "Pi" 4 "$PI_CMD_OUT" "$PI_CMD_PID" "MYCALL $PI_CALLSIGN"
send_cmd_expect "Pi" 4 "$PI_CMD_OUT" "$PI_CMD_PID" "BW2300"
send_cmd_expect "Pi" 4 "$PI_CMD_OUT" "$PI_CMD_PID" "COMPRESSION TEXT"
send_cmd_expect "Pi" 4 "$PI_CMD_OUT" "$PI_CMD_PID" "LISTEN ON"

send_cmd_expect "Mac" 3 "$MAC_CMD_OUT" "$MAC_CMD_PID" "MYCALL $MAC_CALLSIGN"
send_cmd_expect "Mac" 3 "$MAC_CMD_OUT" "$MAC_CMD_PID" "BW2300"
send_cmd_expect "Mac" 3 "$MAC_CMD_OUT" "$MAC_CMD_PID" "COMPRESSION TEXT"

start_pi_data_capture
sleep "$DATA_CAPTURE_SETTLE_SECONDS"

info "[6/7] Connecting and sending payload..."
MAC_CONNECT_OFFSET=$(file_size "$MAC_CMD_OUT")
PI_CONNECT_OFFSET=$(file_size "$PI_CMD_OUT")
printf '%s\r' "CONNECT $MAC_CALLSIGN $PI_CALLSIGN" >&3
wait_for_cmd_pattern "Mac CONNECT command ACK" "$MAC_CMD_OUT" "^OK$" "$CMD_TIMEOUT" "$MAC_CONNECT_OFFSET" "$MAC_CMD_PID" >/dev/null
wait_for_cmd_pattern "Mac CONNECTED event" "$MAC_CMD_OUT" "^CONNECTED[[:space:]]+$MAC_CALLSIGN[[:space:]]+$PI_CALLSIGN[[:space:]]+2300$" "$CONNECT_TIMEOUT" "$MAC_CONNECT_OFFSET" "$MAC_CMD_PID" >/dev/null
wait_for_cmd_pattern "Pi CONNECTED event" "$PI_CMD_OUT" "^CONNECTED[[:space:]]+$PI_CALLSIGN[[:space:]]+$MAC_CALLSIGN[[:space:]]+2300$" "$CONNECT_TIMEOUT" "$PI_CONNECT_OFFSET" "$PI_CMD_PID" >/dev/null

TX_START=$(now_s)
send_payload_to_mac_data_port
wait_for_pi_file_size "$REMOTE_RX" "$PAYLOAD_SIZE" "$RX_TIMEOUT"
TX_FINISH=$(now_s)

info "[7/7] Disconnecting and verifying CRC..."
MAC_DISCONNECT_OFFSET=$(file_size "$MAC_CMD_OUT")
PI_DISCONNECT_OFFSET=$(file_size "$PI_CMD_OUT")
printf '%s\r' "DISCONNECT" >&3
wait_for_cmd_pattern "Mac DISCONNECT command ACK" "$MAC_CMD_OUT" "^OK$" "$CMD_TIMEOUT" "$MAC_DISCONNECT_OFFSET" "$MAC_CMD_PID" >/dev/null
wait_for_cmd_pattern "Mac DISCONNECTED event" "$MAC_CMD_OUT" "^DISCONNECTED$" "$DISCONNECT_TIMEOUT" "$MAC_DISCONNECT_OFFSET" "$MAC_CMD_PID" >/dev/null
wait_for_cmd_pattern "Pi DISCONNECTED event" "$PI_CMD_OUT" "^DISCONNECTED$" "$DISCONNECT_TIMEOUT" "$PI_DISCONNECT_OFFSET" "$PI_CMD_PID" >/dev/null

ssh_pi "cat $(shell_quote "$REMOTE_RX")" > "$RX_LOCAL"
collect_pi_log

SRC_CRC_SIZE=$(crc_and_size "$PAYLOAD")
RX_CRC_SIZE=$(crc_and_size "$RX_LOCAL")
if [[ "$SRC_CRC_SIZE" != "$RX_CRC_SIZE" ]] || ! cmp -s "$PAYLOAD" "$RX_LOCAL"; then
  echo "Source cksum:   $SRC_CRC_SIZE" >&2
  echo "Received cksum: $RX_CRC_SIZE" >&2
  die "payload mismatch"
fi

DURATION=$((TX_FINISH - TX_START))
if (( DURATION > 0 )); then
  THROUGHPUT=$(awk -v bytes="$PAYLOAD_SIZE" -v seconds="$DURATION" 'BEGIN { printf "%.1f", (bytes * 8.0) / seconds }')
else
  THROUGHPUT="n/a"
fi

info "PASS: $PAYLOAD_SIZE bytes delivered byte-exact"
info "CRC32/size: $SRC_CRC_SIZE"
info "Payload delivery window: ${DURATION}s (${THROUGHPUT} bps)"
info "Logs: $LOG_DIR"
