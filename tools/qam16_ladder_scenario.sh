#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHANNEL="awgn"
SNR_DB="20"
SEED="42"
EXPECT_RATE="R1/4"
EXPECT_MOD="16QAM"
MSG_COUNT=1
MSG_INTERVAL=8
CONNECT_DELAY=5
DISCONNECT_AFTER=20
EXIT_AFTER=""
FILE_KB=10
OUT=""

usage() {
  printf 'Usage: %s [--channel awgn] [--snr-db 20] [--seed 42] [--expect-rate R1/4] [--out DIR]\n' "$0"
  printf '       [--exit-after SEC] [--auto-disconnect-after SEC] [--message-count N]\n'
}

modulation_bits() {
  awk -v mod="$1" '
    BEGIN {
      table = "16QAM 4\nQAM16 4\n8PSK 3\nQAM8 3\nQPSK 2\nDQPSK 2\nD8PSK 3\nDBPSK 1\nBPSK 1"
      n = split(table, rows, "\n")
      for (i = 1; i <= n; ++i) {
        split(rows[i], f, " ")
        if (f[1] == mod) {
          print f[2]
          exit
        }
      }
      print 1
    }'
}

rate_descriptor() {
  awk -v rate="$1" -v field="$2" '
    BEGIN {
      table = "R1/4 0.25 5\nR1/2 0.50 5\nR2/3 0.6666667 5\nR3/4 0.75 8"
      n = split(table, rows, "\n")
      for (i = 1; i <= n; ++i) {
        split(rows[i], f, " ")
        if (f[1] == rate) {
          if (field == "code_rate") print f[2]
          else if (field == "pilot_spacing") print f[3]
          exit
        }
      }
      if (field == "code_rate") print "0.25"
      else if (field == "pilot_spacing") print "5"
    }'
}

estimate_exit_after() {
  local code_rate pilot_spacing bits_per_carrier
  code_rate="$(rate_descriptor "$EXPECT_RATE" code_rate)"
  pilot_spacing="$(rate_descriptor "$EXPECT_RATE" pilot_spacing)"
  bits_per_carrier="$(modulation_bits "$EXPECT_MOD")"
  awk -v file_bytes="$FILE_BYTES" \
      -v msg_count="$MSG_COUNT" \
      -v msg_interval="$MSG_INTERVAL" \
      -v disconnect_after="$DISCONNECT_AFTER" \
      -v code_rate="$code_rate" \
      -v pilot_spacing="$pilot_spacing" \
      -v bits_per_carrier="$bits_per_carrier" '
    BEGIN {
      carriers = 59
      pilots = int((carriers + pilot_spacing - 1) / pilot_spacing)
      data_carriers = carriers - pilots
      symbol_rate = 48000.0 / 1152.0
      raw_info_bps = data_carriers * bits_per_carrier * symbol_rate * code_rate
      # The first-rung GUI path includes half-duplex turns, ACK diversity, and
      # retransmission slack. Use a conservative PHY-to-payload efficiency so
      # the failure ceiling scales by rate without becoming a fixed wall clock.
      expected_payload_bps = raw_info_bps * 0.25
      if (expected_payload_bps < 300.0) expected_payload_bps = 300.0
      handshake = 25.0
      scripted_messages = 1.0 * msg_count * msg_interval
      payload = (file_bytes * 8.0) / expected_payload_bps
      margin = 20.0
      expected = handshake + scripted_messages + payload + disconnect_after + margin
      ceiling = int(expected * 1.5 + 0.999)
      if (ceiling < 90) ceiling = 90
      print ceiling
    }'
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --channel) CHANNEL="${2:?}"; shift 2 ;;
    --snr-db) SNR_DB="${2:?}"; shift 2 ;;
    --seed) SEED="${2:?}"; shift 2 ;;
    --expect-rate) EXPECT_RATE="${2:?}"; shift 2 ;;
    --expect-mod) EXPECT_MOD="${2:?}"; shift 2 ;;
    --out) OUT="${2:?}"; shift 2 ;;
    --exit-after) EXIT_AFTER="${2:?}"; shift 2 ;;
    --auto-disconnect-after) DISCONNECT_AFTER="${2:?}"; shift 2 ;;
    --message-count) MSG_COUNT="${2:?}"; shift 2 ;;
    --message-interval) MSG_INTERVAL="${2:?}"; shift 2 ;;
    --file-kb) FILE_KB="${2:?}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$OUT" ]]; then
  stamp="$(date +%Y%m%d_%H%M%S)"
  OUT="/tmp/qam16_ladder_${CHANNEL}_snr${SNR_DB}_seed${SEED}_${stamp}"
fi

mkdir -p "$OUT"
TOKENS="$OUT/tokens.conf"
PAYLOAD="$OUT/qam16_${FILE_KB}KB.bin"
FILE_BYTES=$((FILE_KB * 1024))
if [[ -z "$EXIT_AFTER" ]]; then
  EXIT_AFTER="$(estimate_exit_after)"
fi
SERVER_LOG="$OUT/serve.log"
ALPHA_LOG="$OUT/alpha.log"
BRAVO_LOG="$OUT/bravo.log"
E2E_SERVER_LOG="$OUT/e2e_server.log"
E2E_ALPHA_LOG="$OUT/e2e_alpha.log"
E2E_BRAVO_LOG="$OUT/e2e_bravo.log"
SUMMARY="$OUT/summary.env"

count_pattern() {
  local pattern="$1"
  local file="$2"
  grep -Ec "$pattern" "$file" 2>/dev/null || true
}

sum_cw_fail() {
  local file="$1"
  local sum
  sum="$(
    grep -Eo 'cw_fail=[0-9]+' "$file" 2>/dev/null |
      cut -d= -f2 |
      awk '{s += $1} END {print s + 0}'
  )"
  printf '%s\n' "${sum:-0}"
}

unexpected_data_mode_pattern() {
  case "$EXPECT_MOD" in
    16QAM|QAM16)
      printf '%s\n' 'Adaptive downgrade queued: .* -> (D8PSK|DQPSK|QPSK|8PSK|QAM8)|MODE_CHANGE: OFDM (D8PSK|DQPSK|QPSK|8PSK|QAM8) |Data mode set to: (D8PSK|DQPSK|QPSK|8PSK|QAM8)|TX: Using (D8PSK|DQPSK|QPSK|8PSK|QAM8)'
      ;;
    QPSK)
      printf '%s\n' 'Adaptive downgrade queued: .* -> (8PSK|QAM8|QAM16|16QAM)|MODE_CHANGE: OFDM (8PSK|QAM8|QAM16|16QAM) |Data mode set to: (8PSK|QAM8|QAM16|16QAM)|TX: Using (8PSK|QAM8|QAM16|16QAM)'
      ;;
    8PSK|QAM8)
      printf '%s\n' 'Adaptive downgrade queued: .* -> (D8PSK|DQPSK|QPSK|QAM16|16QAM)|MODE_CHANGE: OFDM (D8PSK|DQPSK|QPSK|QAM16|16QAM) |Data mode set to: (D8PSK|DQPSK|QPSK|QAM16|16QAM)|TX: Using (D8PSK|DQPSK|QPSK|QAM16|16QAM)'
      ;;
    *)
      printf '%s\n' 'Adaptive downgrade queued:|MODE_CHANGE: OFDM |Data mode set to:|TX: Using '
      ;;
  esac
}

collect_metrics() {
  mode_pattern="configured for ${EXPECT_MOD} ${EXPECT_RATE}"
  unexpected_mode_pattern="$(unexpected_data_mode_pattern)"
  alpha_mode_count="$(count_pattern "$mode_pattern" "$ALPHA_LOG")"
  bravo_mode_count="$(count_pattern "$mode_pattern" "$BRAVO_LOG")"
  alpha_unexpected_modes="$(count_pattern "$unexpected_mode_pattern" "$ALPHA_LOG")"
  bravo_unexpected_modes="$(count_pattern "$unexpected_mode_pattern" "$BRAVO_LOG")"
  alpha_rx_msgs="$(count_pattern '\[RX[^]]*BRAVO[^]]*\].*BRAVO QAM16 ladder' "$ALPHA_LOG")"
  bravo_rx_msgs="$(count_pattern '\[RX[^]]*ALPHA[^]]*\].*ALPHA QAM16 ladder' "$BRAVO_LOG")"
  file_crc_ok="$(count_pattern "\\[FILE\\] Received .*\\(${FILE_BYTES} bytes, CRC ok|FileTransfer: Received OK \\(${FILE_BYTES} bytes|Received OK .*${FILE_BYTES} bytes.*CRC" "$BRAVO_LOG")"
  alpha_file_done="$(count_pattern '\[FILE\] Transfer complete|FileTransfer: Transfer complete' "$ALPHA_LOG")"
  alpha_disconnected="$(count_pattern '\[SYS\] Disconnected|Connection state changed: 0|Disconnected from' "$ALPHA_LOG")"
  bravo_disconnected="$(count_pattern '\[SYS\] Disconnected|Connection state changed: 0|Disconnected from' "$BRAVO_LOG")"
  alpha_retx="$(count_pattern 'SR-ARQ: Retransmitting' "$ALPHA_LOG")"
  bravo_retx="$(count_pattern 'SR-ARQ: Retransmitting' "$BRAVO_LOG")"
  alpha_cwfail="$(sum_cw_fail "$ALPHA_LOG")"
  bravo_cwfail="$(sum_cw_fail "$BRAVO_LOG")"

  goodput_kbps="$(
    grep -E '\[FILE\] (Transfer complete|Received).*[0-9.]+ kbps|FileTransfer: (Transfer complete|Received OK).*[0-9.]+ kbps' "$ALPHA_LOG" "$BRAVO_LOG" 2>/dev/null |
      tail -1 |
      sed -E 's/.* ([0-9]+([.][0-9]+)?) kbps.*/\1/' || true
  )"
  if [[ -n "$goodput_kbps" ]]; then
    goodput_bps="$(awk -v k="$goodput_kbps" 'BEGIN { printf "%.0f", k * 1000.0 }')"
  else
    goodput_bps="0"
  fi
}

scenario_passed() {
  [[ "$alpha_mode_count" -gt 0 ]] &&
  [[ "$bravo_mode_count" -gt 0 ]] &&
  [[ "$alpha_unexpected_modes" -eq 0 ]] &&
  [[ "$bravo_unexpected_modes" -eq 0 ]] &&
  [[ "$bravo_rx_msgs" -ge "$MSG_COUNT" ]] &&
  [[ "$file_crc_ok" -gt 0 ]] &&
  [[ "$alpha_file_done" -gt 0 ]] &&
  [[ "$alpha_disconnected" -gt 0 ]] &&
  [[ "$bravo_disconnected" -gt 0 ]]
}

hard_failure_reason() {
  local pattern='max retries exceeded|maximum retries exceeded|transfer failed|Transfer failed|FileTransfer: .*failed|FILE.*failed|SR-ARQ:.*retries exhausted|Connection: Connect failed|Connect failed after|giving up'
  if [[ "${alpha_unexpected_modes:-0}" -gt 0 || "${bravo_unexpected_modes:-0}" -gt 0 ]]; then
    echo "unexpected_data_mode"
    return
  fi
  if grep -Eiq "$pattern" "$ALPHA_LOG" "$BRAVO_LOG" 2>/dev/null; then
    echo "hard_failure_marker"
  fi
}

write_summary() {
  local result="$1"
  local reason="$2"
  local elapsed="$3"
  {
    echo "OUT=$OUT"
    echo "CHANNEL=$CHANNEL"
    echo "SNR_DB=$SNR_DB"
    echo "EXPECT_MOD=$EXPECT_MOD"
    echo "EXPECT_RATE=$EXPECT_RATE"
    echo "FILE_BYTES=$FILE_BYTES"
    echo "EXIT_AFTER=$EXIT_AFTER"
    echo "ELAPSED_SEC=$elapsed"
    echo "ALPHA_MODE_COUNT=$alpha_mode_count"
    echo "BRAVO_MODE_COUNT=$bravo_mode_count"
    echo "ALPHA_UNEXPECTED_MODE_COUNT=$alpha_unexpected_modes"
    echo "BRAVO_UNEXPECTED_MODE_COUNT=$bravo_unexpected_modes"
    echo "ALPHA_RX_MSGS=$alpha_rx_msgs"
    echo "BRAVO_RX_MSGS=$bravo_rx_msgs"
    echo "FILE_CRC_OK_COUNT=$file_crc_ok"
    echo "ALPHA_FILE_DONE_COUNT=$alpha_file_done"
    echo "ALPHA_DISCONNECTED_COUNT=$alpha_disconnected"
    echo "BRAVO_DISCONNECTED_COUNT=$bravo_disconnected"
    echo "GOODPUT_BPS=$goodput_bps"
    echo "ALPHA_RETX_COUNT=$alpha_retx"
    echo "BRAVO_RETX_COUNT=$bravo_retx"
    echo "ALPHA_CWFAIL_COUNT=$alpha_cwfail"
    echo "BRAVO_CWFAIL_COUNT=$bravo_cwfail"
    echo "ALPHA_LOG=$ALPHA_LOG"
    echo "BRAVO_LOG=$BRAVO_LOG"
    echo "E2E_SERVER_LOG=$E2E_SERVER_LOG"
    echo "E2E_ALPHA_LOG=$E2E_ALPHA_LOG"
    echo "E2E_BRAVO_LOG=$E2E_BRAVO_LOG"
    echo "RESULT=$result"
    echo "REASON=$reason"
  } | tee "$SUMMARY"
}

cleanup() {
  pkill -f "$ROOT/build/ultra_gui" 2>/dev/null || true
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

cd "$ROOT"

if command -v caffeinate >/dev/null 2>&1 && ! pgrep -x caffeinate >/dev/null 2>&1; then
  caffeinate -dimsu >"$OUT/caffeinate.log" 2>&1 &
  echo "CAFFEINATE_PID=$!" | tee -a "$SUMMARY"
fi

pkill -f "$ROOT/build/ultra_gui" 2>/dev/null || true
pkill -f "$ROOT/build/ota_simulator serve" 2>/dev/null || true
sleep 2

dd if=/dev/urandom of="$PAYLOAD" bs=1024 count="$FILE_KB" status=none
printf 'alpha_tok:ALPHA:alpha\nbravo_tok:BRAVO:bravo\n' > "$TOKENS"

ULTRA_E2E_DEBUG_LOG="$E2E_SERVER_LOG" "$ROOT/build/ota_simulator" serve \
  --bind 127.0.0.1:0 \
  --udp-bind 127.0.0.1:0 \
  --tokens "$TOKENS" \
  --captures-root "$OUT/caps" \
  --lobby-channel "$CHANNEL" \
  --lobby-snr-db "$SNR_DB" \
  --lobby-seed "$SEED" \
  --shutdown-deadline-sec "$((EXIT_AFTER + 120))" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 80); do
  grep -q OTASIM_SERVE_READY "$SERVER_LOG" 2>/dev/null && break
  sleep 0.25
done
GRPC="$(grep -o 'grpc=[^ ]*' "$SERVER_LOG" | head -1 | cut -d= -f2)"
if [[ -z "$GRPC" ]]; then
  echo "RESULT=FAIL reason=server_start log=$SERVER_LOG" | tee -a "$SUMMARY"
  exit 1
fi

ULTRA_E2E_DEBUG_LOG="$E2E_BRAVO_LOG" "$ROOT/build/ultra_gui" -sim --ota-host "$GRPC" --token bravo_tok --station-id BRAVO \
  --session-id lobby \
  --auto-accept \
  --exit-after "$EXIT_AFTER" \
  --log-level debug --log-category all --log-file "$BRAVO_LOG" >/dev/null 2>&1 &
BRAVO_PID=$!

ULTRA_E2E_DEBUG_LOG="$E2E_ALPHA_LOG" "$ROOT/build/ultra_gui" -sim --ota-host "$GRPC" --token alpha_tok --station-id ALPHA \
  --session-id lobby \
  --auto-connect BRAVO \
  --connect-delay "$CONNECT_DELAY" \
  --auto-send-message "ALPHA QAM16 ladder" \
  --auto-message-count "$MSG_COUNT" \
  --auto-message-interval "$MSG_INTERVAL" \
  --auto-send-file "$PAYLOAD" \
  --auto-disconnect-after "$DISCONNECT_AFTER" \
  --exit-after "$EXIT_AFTER" \
  --log-level debug --log-category all --log-file "$ALPHA_LOG" >/dev/null 2>&1 &
ALPHA_PID=$!

sleep 12
if ! grep -Eq 'searchForSync|CCA:|Connection state changed|Sending PING|auto-connecting|configured for' "$ALPHA_LOG" 2>/dev/null; then
  echo "RESULT=FAIL reason=freeze_guard alpha_log=$ALPHA_LOG bravo_log=$BRAVO_LOG" | tee -a "$SUMMARY"
  exit 1
fi

deadline=$((SECONDS + EXIT_AFTER))
while true; do
  collect_metrics
  elapsed=$((SECONDS))

  if scenario_passed; then
    write_summary "PASS" "success_poll" "$elapsed"
    exit 0
  fi

  failure_reason="$(hard_failure_reason)"
  if [[ -n "$failure_reason" ]]; then
    write_summary "FAIL" "$failure_reason" "$elapsed"
    exit 1
  fi

  alpha_alive=0
  bravo_alive=0
  kill -0 "$ALPHA_PID" 2>/dev/null && alpha_alive=1
  kill -0 "$BRAVO_PID" 2>/dev/null && bravo_alive=1
  if [[ "$alpha_alive" -eq 0 && "$bravo_alive" -eq 0 ]]; then
    write_summary "FAIL" "process_exit_before_pass" "$elapsed"
    exit 1
  fi

  if (( SECONDS >= deadline )); then
    write_summary "FAIL" "timeout" "$elapsed"
    exit 1
  fi

  sleep 2
done
