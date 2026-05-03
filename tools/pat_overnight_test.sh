#!/usr/bin/env bash
#
# pat_overnight_test.sh — autonomous Pat ↔ ultra_tnc test battery.
#
# Runs a sequence of scenarios end-to-end across Mac (ALPHA) and Pi (BRAVO).
# Each scenario has a hard timeout. State resets between tests so a hang in
# one test doesn't cascade.
#
# Outputs:
#   /tmp/pat_overnight/<test-id>/ — per-test log dir
#   /tmp/pat_overnight/SUMMARY.md — final summary
#
# Run with:
#   SSH_KEY=$HOME/.ssh/id_pi5 ./tools/pat_overnight_test.sh

set -uo pipefail

# Config
SSH_KEY=${SSH_KEY:?SSH_KEY env var required}
PI=${PI:-math@pi5tester}
PI_REPO=${PI_REPO:-/home/math/ProjectUltra}
PI_AUDIO_OUT=${PI_AUDIO_OUT:-USB Audio Device, USB Audio}
PI_AUDIO_IN=${PI_AUDIO_IN:-USB Audio Device, USB Audio}
MAC_AUDIO_OUT=${MAC_AUDIO_OUT:-Sound Blaster Play! 3}
MAC_AUDIO_IN=${MAC_AUDIO_IN:-Sound Blaster Play! 3}
MAC_PAT=${MAC_PAT:-$HOME/go/bin/pat}
PI_PAT=${PI_PAT:-/usr/bin/pat-winlink}
MAC_TNC=${MAC_TNC:-/Users/mathieuvachon/Projects/ProjectUltra/build/ultra_tnc}
PI_TNC=${PI_TNC:-$PI_REPO/build/ultra_tnc}
TEST_TIMEOUT_S=${TEST_TIMEOUT_S:-120}

LOG_ROOT=/tmp/pat_overnight
mkdir -p "$LOG_ROOT"
SUMMARY=$LOG_ROOT/SUMMARY.md
: > "$SUMMARY"

ssh_pi() { ssh -i "$SSH_KEY" -o ConnectTimeout=8 "$PI" "$@"; }
log() { echo "[$(date '+%H:%M:%S')] $*"; }

reset_state() {
  log "  cleaning up processes..."
  pkill -9 -f "go/bin/pat" 2>/dev/null || true
  pkill -9 -f "ultra_tnc" 2>/dev/null || true
  ssh_pi "pkill -9 -f ultra_tnc 2>/dev/null; pkill -9 -f pat-winlink 2>/dev/null" || true
  sleep 2
}

write_pat_config_mac() {
  local path="$HOME/Library/Application Support/pat/config.json"
  mkdir -p "$(dirname "$path")"
  cat > "$path" <<EOF
{
  "mycall": "ALPHA",
  "secure_login_password": "",
  "auxiliary_addresses": [],
  "locator": "",
  "service_codes": ["PUBLIC"],
  "http_addr": "localhost:8080",
  "motd": [],
  "connect_aliases": {},
  "listen": [],
  "hamlib_rigs": {},
  "varahf": {"addr": "localhost:8300", "bandwidth": 2300, "rig": "", "ptt_ctrl": false}
}
EOF
}

write_pat_config_pi() {
  ssh_pi "mkdir -p ~/.config/pat && cat > ~/.config/pat/config.json <<'EOF'
{
  \"mycall\": \"BRAVO\",
  \"secure_login_password\": \"\",
  \"auxiliary_addresses\": [],
  \"locator\": \"\",
  \"service_codes\": [\"PUBLIC\"],
  \"http_addr\": \"localhost:8080\",
  \"motd\": [],
  \"connect_aliases\": {},
  \"listen\": [\"varahf\"],
  \"hamlib_rigs\": {},
  \"varahf\": {\"addr\": \"localhost:8300\", \"bandwidth\": 2300, \"rig\": \"\", \"ptt_ctrl\": false}
}
EOF"
}

start_tncs() {
  local logdir="$1"
  log "  starting Pi ultra_tnc..."
  ssh_pi "nohup $PI_TNC \
    --audio-output '$PI_AUDIO_OUT' --audio-input '$PI_AUDIO_IN' \
    --callsign BRAVO --port 8300 \
    > /tmp/pi_ultra_tnc.log 2>&1 < /dev/null & echo \$!" > "$logdir/pi_tnc_pid.txt"
  log "  starting Mac ultra_tnc..."
  $MAC_TNC \
    --audio-output "$MAC_AUDIO_OUT" --audio-input "$MAC_AUDIO_IN" \
    --callsign ALPHA --port 8300 \
    > "$logdir/mac_ultra_tnc.log" 2>&1 < /dev/null &
  echo "$!" > "$logdir/mac_tnc_pid.txt"
  sleep 5  # give TNCs time to bind before Pat tries to connect
}

start_pi_pat() {
  local logdir="$1"
  log "  starting Pi pat http (auto-listens)..."
  ssh_pi "nohup $PI_PAT http > /tmp/pi_pat.log 2>&1 < /dev/null & echo \$!" > "$logdir/pi_pat_pid.txt"
  sleep 4
}

stop_all() {
  reset_state
}

collect_logs() {
  local logdir="$1"
  ssh_pi "cat /tmp/pi_ultra_tnc.log 2>/dev/null" > "$logdir/pi_ultra_tnc.log" || true
  ssh_pi "cat /tmp/pi_pat.log 2>/dev/null" > "$logdir/pi_pat.log" || true
}

verify_disconnected_clean() {
  local logdir="$1"
  # Mac pat exited normally?
  if ! grep -q "Connected to BRAVO" "$logdir/mac_pat_connect.log" 2>/dev/null; then
    return 1
  fi
  if ! grep -q "Got connect" "$logdir/pi_pat.log" 2>/dev/null; then
    return 1
  fi
  return 0
}

count_inbox_pi() {
  ssh_pi "ls /home/math/.local/share/pat/mailbox/BRAVO/in/ 2>/dev/null | wc -l" || echo 0
}

count_inbox_mac() {
  ls "$HOME/Library/Application Support/pat/mailbox/ALPHA/in/" 2>/dev/null | wc -l | tr -d ' '
}

clear_inbox_pi() { ssh_pi "rm -rf /home/math/.local/share/pat/mailbox/BRAVO/in/* 2>/dev/null" || true; }
clear_inbox_mac() { rm -rf "$HOME/Library/Application Support/pat/mailbox/ALPHA/in/"*  2>/dev/null || true; }

run_test() {
  local id="$1" desc="$2"
  shift 2
  local body="$@"
  local logdir=$LOG_ROOT/$id
  rm -rf "$logdir"; mkdir -p "$logdir"

  log "==== $id: $desc ===="
  reset_state
  write_pat_config_mac
  write_pat_config_pi
  clear_inbox_pi
  clear_inbox_mac

  start_tncs "$logdir"
  start_pi_pat "$logdir"

  local start_s=$(date +%s)
  local rc=0
  # body fn runs with logdir in scope, returns 0=PASS, 1=FAIL, 2=HUNG
  $body "$logdir" || rc=$?
  local end_s=$(date +%s)
  local elapsed=$((end_s - start_s))

  collect_logs "$logdir"
  stop_all

  local status="PASS"
  case $rc in
    0) status=PASS ;;
    1) status=FAIL ;;
    2) status=HUNG ;;
    *) status="ERR ($rc)" ;;
  esac

  log "==== $id: $status (${elapsed}s) ===="
  echo "" >> "$SUMMARY"
  echo "## $id: $status (${elapsed}s)" >> "$SUMMARY"
  echo "**Description**: $desc" >> "$SUMMARY"
  echo "" >> "$SUMMARY"
  echo "Logs: \`$logdir\`" >> "$SUMMARY"
}

# ----- Test bodies -----

run_pat_with_timeout() {
  local logdir="$1" timeout_s="$2"
  : > "$logdir/mac_pat_connect.log"
  # gtimeout is brew install coreutils, fall back to a manual perl impl
  local timeout_bin=
  if command -v gtimeout >/dev/null 2>&1; then
    timeout_bin=gtimeout
  elif command -v timeout >/dev/null 2>&1; then
    timeout_bin=timeout
  fi
  if [[ -n "$timeout_bin" ]]; then
    "$timeout_bin" -k 5 "$timeout_s" \
      "$MAC_PAT" connect 'varahf:///BRAVO' \
      > "$logdir/mac_pat_connect.log" 2>&1
    return $?
  fi
  # Fallback: perl -e exec with alarm
  perl -e '
    my ($t, @cmd) = @ARGV;
    my $pid = fork();
    if ($pid == 0) { exec @cmd; exit 127; }
    eval {
      local $SIG{ALRM} = sub { kill 9, $pid; die "timeout\n"; };
      alarm $t;
      waitpid($pid, 0);
      alarm 0;
    };
    exit ($@ ? 124 : ($? >> 8));
  ' -- "$timeout_s" "$MAC_PAT" connect 'varahf:///BRAVO' \
    > "$logdir/mac_pat_connect.log" 2>&1
  return $?
}

test_connect_disconnect() {
  local logdir="$1"
  log "  pat connect varahf:///BRAVO (no msgs)..."
  local rc=0
  run_pat_with_timeout "$logdir" "$TEST_TIMEOUT_S" || rc=$?
  if [[ $rc -eq 124 ]]; then
    log "  TIMEOUT after ${TEST_TIMEOUT_S}s"
    return 2
  fi
  if grep -q "Connected to BRAVO" "$logdir/mac_pat_connect.log"; then
    return 0
  fi
  return 1
}

write_outbox_mac() {
  local subject="$1" body_text="$2"
  local outdir="$HOME/Library/Application Support/pat/mailbox/ALPHA/out"
  mkdir -p "$outdir"
  local mid=$(printf "TEST%012d" $RANDOM$RANDOM)
  cat > "$outdir/$mid.b2f" <<EOF
MID: $mid
Date: $(date -u '+%Y/%m/%d %H:%M')
From: ALPHA
To: BRAVO
Type: Private
Subject: $subject
Mbo: ALPHA
Body-encoding: 7BIT
Body: ${#body_text}

$body_text
EOF
  echo "$mid"
}

test_send_small() {
  local logdir="$1"
  local mid=$(write_outbox_mac "ovrntest1" "Hello from ALPHA. This is a small test message. End.")
  log "  composed mid=$mid in outbox"
  log "  pat connect (with outbox msg)..."
  local rc=0
  run_pat_with_timeout "$logdir" "$TEST_TIMEOUT_S" || rc=$?
  if [[ $rc -eq 124 ]]; then
    log "  TIMEOUT after ${TEST_TIMEOUT_S}s"
    return 2
  fi
  local n=$(count_inbox_pi)
  if [[ "$n" -ge 1 ]]; then
    log "  delivered: Pi inbox count=$n"
    return 0
  fi
  log "  not delivered: Pi inbox count=$n"
  return 1
}

# ----- Main -----

log "Pat overnight test battery starting"
log "Logs: $LOG_ROOT"

run_test "T1_connect_only" "Empty CONNECT/DISCONNECT cycle" test_connect_disconnect
run_test "T2_send_small_mac_to_pi" "Send 50-byte text Mac→Pi" test_send_small
run_test "T3_connect_only_repeat" "Empty CONNECT/DISCONNECT (sanity, repeat T1)" test_connect_disconnect

log ""
log "==== Battery complete ===="
log "Summary: $SUMMARY"
cat "$SUMMARY"
