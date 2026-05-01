#!/usr/bin/env bash
#
# run_hw_test.sh — Two-machine hardware-audio test driver.
#
# Runs station A locally (this Mac), station B on a remote host (Pi 5)
# via SSH. They exchange audio over a real cable / soundcard chain.
# An optional --inject-channel flag applies a synthetic HF channel
# (AWGN + Watterson fading) on each side's TX before the soundcard,
# so a clean physical cable still carries a realistic HF signal.
#
# ────────────────────────────── Setup once ──────────────────────────────
#
# On the Pi 5 (Ubuntu Server, headless):
#   sudo apt update
#   sudo apt install -y build-essential cmake git pkg-config \
#                       libsdl2-dev libfftw3-dev
#   git clone <repo-url> ~/ProjectUltra && cd ~/ProjectUltra
#   mkdir build && cd build
#   cmake -DULTRA_BUILD_GUI=OFF ..
#   make cli_simulator -j4
#
# On the Mac (this host): build is the same, with or without GUI:
#   mkdir build && cd build
#   cmake .. && make cli_simulator -j4
#
# Set up SSH key auth so this script can run unattended:
#   ssh-copy-id ubuntu@pi5.local
#
# ──────────────────────────────── Usage ────────────────────────────────
#
#   ./tools/run_hw_test.sh                          # message test, AWGN, R1/4
#   ./tools/run_hw_test.sh --file 5120              # 5 KB file transfer
#   ./tools/run_hw_test.sh --snr 15 --channel good  # synthetic Good fading
#   PI=ubuntu@my-pi.local ./tools/run_hw_test.sh    # custom host
#
# Override audio device names with env vars (see defaults below).
# Use --list-devices to discover what SDL2 sees on each side.

set -euo pipefail

# ─── Defaults (override via env or CLI) ─────────────────────────────────
PI=${PI:-math@pi5tester}
# Absolute path on the Pi — keeping ~ here would let the Mac shell expand it
# to /Users/<me>/ProjectUltra before the SSH command was even sent.
PI_REPO=${PI_REPO:-/home/math/ProjectUltra}
# Optional: SSH_KEY=~/.ssh/id_pi5 to use a specific key.
# SSH_EXTRA_OPTS can carry options such as -o HostKeyAlias=pi5tester.
SSH_OPTS="${SSH_KEY:+-i "$SSH_KEY"} ${SSH_EXTRA_OPTS:-}"
PI_AUDIO_OUT=${PI_AUDIO_OUT:-USB Audio Device, USB Audio}
PI_AUDIO_IN=${PI_AUDIO_IN:-USB Audio Device, USB Audio}
MAC_AUDIO_OUT=${MAC_AUDIO_OUT:-Sound Blaster Play! 3}
MAC_AUDIO_IN=${MAC_AUDIO_IN:-Sound Blaster Play! 3}
SNR=${SNR:-20}
CHANNEL=${CHANNEL:-awgn}              # awgn|good|moderate|poor|flutter
RATE=${RATE:-r1_4}                    # auto|r1_4|r1_2|r2_3|r3_4
FILE_SIZE=${FILE_SIZE:-}              # empty = message test
INJECT_CHANNEL=${INJECT_CHANNEL:-0}   # 1 = synthetic HF channel on each TX
INJECT_GAIN=${INJECT_GAIN:-}          # optional post-injection gain/headroom
B_IDLE_SECONDS=${B_IDLE_SECONDS:-0}   # how long B waits before giving up (0 = until peer disconnects)
EXTRA_CLI_ARGS=${EXTRA_CLI_ARGS:-}     # optional raw cli_simulator args for both A and B

# ─── CLI parsing ────────────────────────────────────────────────────────
LIST_DEVICES=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --pi)             PI="$2"; shift 2;;
    --snr)            SNR="$2"; shift 2;;
    --channel)        CHANNEL="$2"; shift 2;;
    --rate)           RATE="$2"; shift 2;;
    --file)           FILE_SIZE="$2"; shift 2;;
    --inject)         INJECT_CHANNEL=1; shift;;
    --no-inject)      INJECT_CHANNEL=0; shift;;
    --inject-gain)    INJECT_GAIN="$2"; shift 2;;
    --extra-args)     EXTRA_CLI_ARGS="$2"; shift 2;;
    --list-devices)   LIST_DEVICES=1; shift;;
    --idle-seconds)   B_IDLE_SECONDS="$2"; shift 2;;
    -h|--help)
      sed -n '2,40p' "$0" | sed 's/^# \?//'
      exit 0;;
    *) echo "Unknown option: $1" >&2; exit 2;;
  esac
done

# ─── Resolve binary ─────────────────────────────────────────────────────
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
MAC_BIN="$SCRIPT_DIR/../build/cli_simulator"
if [[ ! -x "$MAC_BIN" ]]; then
  echo "Mac binary not found at $MAC_BIN — build first:" >&2
  echo "  cd $SCRIPT_DIR/.. && cmake --build build --target cli_simulator -j4" >&2
  exit 2
fi

# ─── --list-devices: dump device names from each side and exit ──────────
if [[ "$LIST_DEVICES" == "1" ]]; then
  echo "=== Mac devices ==="
  "$MAC_BIN" --list-audio-devices --role A 2>/dev/null | sed -n '/Output/,/Input/p; /Input/,$p'
  echo
  echo "=== Pi devices ($PI) ==="
  ssh $SSH_OPTS "$PI" "cd $PI_REPO && ./build/cli_simulator --list-audio-devices --role B" \
    2>/dev/null | sed -n '/Output/,/Input/p; /Input/,$p'
  exit 0
fi

# ─── Build flag strings ─────────────────────────────────────────────────
INJECT_FLAG=""
[[ "$INJECT_CHANNEL" == "1" ]] && INJECT_FLAG="--inject-channel"

INJECT_GAIN_FLAG=""
[[ -n "$INJECT_GAIN" ]] && INJECT_GAIN_FLAG="--inject-gain $INJECT_GAIN"

FILE_FLAG=""
[[ -n "$FILE_SIZE" ]] && FILE_FLAG="--file $FILE_SIZE"

RATE_FLAG=""
[[ "$RATE" != "auto" && "$RATE" != "AUTO" ]] && RATE_FLAG="--rate $RATE"

CHANNEL_FLAG=""
[[ "$CHANNEL" != "awgn" ]] && CHANNEL_FLAG="--channel $CHANNEL"

dev_flags() {  # $1=out $2=in -> emits --audio-output X --audio-input Y
  local out="$1" in="$2"
  local r=""
  [[ -n "$out" ]] && r="$r --audio-output '$out'"
  [[ -n "$in"  ]] && r="$r --audio-input  '$in'"
  echo "$r"
}

PI_DEVS=$(dev_flags "$PI_AUDIO_OUT" "$PI_AUDIO_IN")
MAC_DEVS_ARR=()
[[ -n "$MAC_AUDIO_OUT" ]] && MAC_DEVS_ARR+=(--audio-output "$MAC_AUDIO_OUT")
[[ -n "$MAC_AUDIO_IN"  ]] && MAC_DEVS_ARR+=(--audio-input  "$MAC_AUDIO_IN")

# ─── Logging dir ────────────────────────────────────────────────────────
LOG_DIR=/tmp/ultra_hw_$(date +%Y%m%d_%H%M%S)
mkdir -p "$LOG_DIR"
echo "Logs: $LOG_DIR"
echo "Test: SNR=$SNR  channel=$CHANNEL  rate=$RATE  inject=$INJECT_CHANNEL  file=${FILE_SIZE:-(message)}"
[[ -n "$INJECT_GAIN" ]] && echo "Inject gain: $INJECT_GAIN"
echo "Audio: Mac out='$MAC_AUDIO_OUT' in='$MAC_AUDIO_IN'  Pi out='$PI_AUDIO_OUT' in='$PI_AUDIO_IN'"
[[ -n "$EXTRA_CLI_ARGS" ]] && echo "Extra cli_simulator args: $EXTRA_CLI_ARGS"
echo

# ─── 1. Start station B on Pi (background, via SSH) ─────────────────────
# We build a single-string command so ~ expands on the Pi (not on the Mac).
# All other vars (PI_DEVS, SNR, etc.) are intentionally pre-expanded here.
echo "[1/3] Starting station B on $PI..."
PI_CMD="cd $PI_REPO && \
  pkill -9 cli_simulator 2>/dev/null || true; \
  rm -f /tmp/ultra_B.log; \
  nohup ./build/cli_simulator --role B \
    $PI_DEVS \
    --snr $SNR $RATE_FLAG $CHANNEL_FLAG $INJECT_FLAG $INJECT_GAIN_FLAG \
    --idle-seconds $B_IDLE_SECONDS \
    $EXTRA_CLI_ARGS \
    > /tmp/ultra_B.log 2>&1 & \
  echo \$!"
ssh $SSH_OPTS "$PI" "$PI_CMD" > "$LOG_DIR/B_pid.txt"
B_PID=$(tr -d '\n' < "$LOG_DIR/B_pid.txt")
echo "    B running on $PI, PID=$B_PID"
sleep 3   # let B open its audio devices before A starts

# ─── 2. Run station A locally (foreground) ──────────────────────────────
echo "[2/3] Running station A locally..."
set +e
"$MAC_BIN" --role A \
  ${MAC_DEVS_ARR[@]+"${MAC_DEVS_ARR[@]}"} \
  --snr "$SNR" $RATE_FLAG $CHANNEL_FLAG $INJECT_FLAG $INJECT_GAIN_FLAG $FILE_FLAG \
  $EXTRA_CLI_ARGS \
  > "$LOG_DIR/A.log" 2>&1
A_EXIT=$?
set -e

# ─── 3. Stop B, pull its log ────────────────────────────────────────────
echo "[3/3] Stopping B and collecting log..."
ssh $SSH_OPTS "$PI" "kill $B_PID 2>/dev/null; sleep 1; pkill -9 cli_simulator 2>/dev/null; true" >/dev/null
ssh $SSH_OPTS "$PI" "cat /tmp/ultra_B.log" > "$LOG_DIR/B.log"

# ─── Metric extraction ─────────────────────────────────────────────────
print_file_metrics() {
  [[ -n "$FILE_SIZE" ]] || return 0

  local start_ts frames_sent final_seq ack_ts duration_s throughput_bps retx_line
  start_ts=$(awk '
    function ts(line, value) {
      if (match(line, /\[[[:space:]]*[0-9]+\.[0-9]+\]/)) {
        value = substr(line, RSTART, RLENGTH)
        gsub(/\[/, "", value); gsub(/\]/, "", value); gsub(/[[:space:]]/, "", value)
        return value
      }
      return ""
    }
    /Connection: Starting file transfer/ {
      print ts($0); exit
    }' "$LOG_DIR/A.log")
  frames_sent=$(sed -n 's/.*ARQ:  frames_sent=\([0-9][0-9]*\).*/\1/p' "$LOG_DIR/A.log" | head -1)

  if [[ -n "$frames_sent" && "$frames_sent" -gt 0 ]]; then
    final_seq=$((frames_sent - 1))
    ack_ts=$(awk -v seq="$final_seq" '
      function ts(line, value) {
        if (match(line, /\[[[:space:]]*[0-9]+\.[0-9]+\]/)) {
          value = substr(line, RSTART, RLENGTH)
          gsub(/\[/, "", value); gsub(/\]/, "", value); gsub(/[[:space:]]/, "", value)
          return value
        }
        return ""
      }
      $0 ~ "RX << ACK seq=" seq " \\(20 bytes\\)" {
        print ts($0); exit
      }' "$LOG_DIR/A.log")
  fi

  if [[ -z "${ack_ts:-}" ]]; then
    ack_ts=$(awk '
      function line_ts(line, value) {
        if (match(line, /\[[[:space:]]*[0-9]+\.[0-9]+\]/)) {
          value = substr(line, RSTART, RLENGTH)
          gsub(/\[/, "", value); gsub(/\]/, "", value); gsub(/[[:space:]]/, "", value)
          return value
        }
        return ""
      }
      /RX << ACK seq=/ && $0 !~ /seq=65535/ {
        ts = line_ts($0)
      }
      END { if (ts != "") print ts }' "$LOG_DIR/A.log")
  fi

  if [[ -n "$start_ts" && -n "${ack_ts:-}" ]]; then
    duration_s=$(awk -v start="$start_ts" -v finish="$ack_ts" 'BEGIN {
        dt = finish - start
        if (dt > 0) printf "%.3f", dt
      }')
    throughput_bps=$(awk -v bytes="$FILE_SIZE" -v duration="$duration_s" 'BEGIN {
        if (duration > 0) printf "%.1f", (bytes * 8.0) / duration
      }')
  fi

  retx_line=$(grep -E "  ARQ:  frames_sent=" "$LOG_DIR/A.log" | head -1 || true)

  echo "── Data phase ──"
  if [[ -n "${throughput_bps:-}" ]]; then
    echo "Payload throughput: ${throughput_bps} bps (${FILE_SIZE} bytes / ${duration_s}s)"
    [[ -n "${frames_sent:-}" ]] && echo "Final ACK seq: ${final_seq:-?}  frames_sent=${frames_sent}"
  else
    echo "Payload throughput: unavailable (missing start or final ACK timestamp)"
  fi
  [[ -n "$retx_line" ]] && echo "${retx_line#  }"
  grep -E "  RETX:" "$LOG_DIR/A.log" | head -1 | sed 's/^  //' || true
  echo
}

# ─── Summary ────────────────────────────────────────────────────────────
echo
echo "================================================================"
echo "  Summary"
echo "================================================================"
echo
print_file_metrics
echo "── A (this Mac) ──"
grep -E "TEST (PASSED|FAILED)|frame_success|retransmissions=|✓|✗ " \
  "$LOG_DIR/A.log" 2>/dev/null | head -10 || true
echo
echo "── B ($PI) ──"
grep -E "TEST|frame_success|RX MSG|RX FILE|Peer (connected|disconnected)" \
  "$LOG_DIR/B.log" 2>/dev/null | head -10 || true
echo
echo "Full logs: $LOG_DIR/A.log  $LOG_DIR/B.log"
exit $A_EXIT
