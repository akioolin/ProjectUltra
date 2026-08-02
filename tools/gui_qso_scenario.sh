#!/usr/bin/env bash
#
# gui_qso_scenario.sh — the FAITHFUL real-time GUI test harness.
# (Renamed 2026-05-29 from the misleading "qam16_ladder_scenario.sh"; it is not
#  QAM16- or ladder-specific.)
#
# Drives two real `ultra_gui -sim` instances (ALPHA, BRAVO) through a live
# `ota_simulator serve` channel for a full connected one-way file transfer:
# PING/PONG -> CONNECT -> MODE_CHANGE -> ALPHA->BRAVO file transfer -> DISCONNECT.
# (Chat messaging was removed 2026-05-29 — the GUI is a one-way FILE SENDER per
# design §14; this harness is file-transfer-only.) This is the real-time path
# (audio-clock paced), so it is the trustworthy reliability/throughput gate —
# unlike cli_simulator, which is CPU-paced and not faithful for fade reliability.
#
# Goodput reported (summary.env GOODPUT_BPS) is ALPHA's (sender) on-air goodput
# only — the honest full-transfer number. BRAVO's "Received OK kbps" is NOT used
# (it spans only first->last decode and over-reports; see the goodput block).
#
# The warm-handoff burst-transport config is BAKED IN below (overridable
# defaults) — a bare run is the warm test, no env exports needed:
#   tools/gui_qso_scenario.sh --channel good --snr-db 20 --seed N \
#       --expect-rate R3/4 --expect-mod QPSK --file-kb 21 --out /tmp/X
# Override any knob inline, e.g.:
#   ULTRA_LOCK_RATE=1 tools/gui_qso_scenario.sh ...          (pin the rate; adaptive
#                       fade-riding ladder is the default since 2026-07-02)
# Multi-seed: loop this script over seeds (it is the single test harness).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHANNEL="awgn"
SNR_DB="20"
SEED="42"
EXPECT_RATE="R1/4"
EXPECT_MOD="16QAM"
CONNECT_DELAY=5
DISCONNECT_AFTER=20
EXIT_AFTER=""
FILE_KB=10
OUT=""
PAYLOAD_SEED=""

usage() {
  printf 'Usage: %s [--channel awgn] [--snr-db 20] [--seed 42] [--expect-rate R1/4] [--expect-mod 16QAM] [--out DIR]\n' "$0"
  printf '       [--exit-after SEC] [--auto-disconnect-after SEC] [--file-kb KB]\n'
}

modulation_bits() {
  awk -v mod="$1" '
    BEGIN {
      table = "16QAM 4\nQAM16 4\n8PSK 3\nQAM8 3\nQPSK 2\nDQPSK 2\nD8PSK 3\nDBPSK 1\nBPSK 1\nany 2\ncoherent 2"
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
      # The GUI path includes half-duplex turns, ACK diversity, retransmission
      # slack AND substantial dead-air on the slower rungs — a forced QPSK R1/2
      # 20 KB run measured ~22% TX duty (effective wall-clock rate well under
      # raw*0.25), which a 0.25 efficiency under-budgeted into a false timeout.
      # Budget conservatively: the deadline only has to NOT false-FAIL a run that
      # does deliver; a too-long ceiling costs nothing because a PASS ends early
      # on the success poll. (Dead-air on R1/2 itself is a separate pacing issue.)
      expected_payload_bps = raw_info_bps * 0.15
      if (expected_payload_bps < 250.0) expected_payload_bps = 250.0
      handshake = 25.0
      payload = (file_bytes * 8.0) / expected_payload_bps
      margin = 20.0
      expected = handshake + payload + disconnect_after + margin
      ceiling = int(expected * 1.8 + 0.999)
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
    --file-kb) FILE_KB="${2:?}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

# ---------------------------------------------------------------------------
# Warm-handoff burst-transport config — the "warm thing" this harness exists to
# test (§16 warm-handoff + one-way burst transport, 2026-05-29). Baked in here so
# Each is an OVERRIDABLE default (`:=`), so the caller can still flip any of them — e.g.
#   ULTRA_LOCK_RATE=1         (pin the negotiated rate — the pre-2026-07-02 fixed-rate cell)
#   ULTRA_FORCE_DATA_MOD=8PSK ULTRA_FORCE_DATA_RATE=R3_4  (force a rung)
# (warm-sync hand-off is now the PRODUCTION DEFAULT — the ULTRA_S16_WARM_HANDOFF flag was
#  removed 2026-05-31; there is no longer a full-chirp-every-group OFF baseline to select.)
# 2026-07-02: the fade-riding adaptive ladder is the PRODUCTION DEFAULT (ULTRA_RATE_ADAPT
# default-ON for wideband OFDM), so the harness no longer pins the rate — a bare run is
# the out-of-box adaptive run. Pin with ULTRA_LOCK_RATE=1 for fixed-rung baselines/probes.
: "${ULTRA_ADAPTIVE_RATE:=1}"        ; export ULTRA_ADAPTIVE_RATE
: "${ULTRA_LOCK_RATE:=0}"            ; export ULTRA_LOCK_RATE
# No longer pinned (now code defaults, reconciled 2026-05-30):
#   ULTRA_BURST_TRANSPORT  -> default ON (the production OFDM file path)
#   ULTRA_LDPC_Z           -> retired unsafe raw override (ignored by Connection)
#   ULTRA_BURST_GROUP_FRAMES -> default 6 (mask-width-matched)
# All three remain overridable via env (=0 / value); shown in the echo only when set.
echo "config: ADAPTIVE_RATE=$ULTRA_ADAPTIVE_RATE LOCK_RATE=$ULTRA_LOCK_RATE${ULTRA_BURST_TRANSPORT:+ BURST_TRANSPORT=$ULTRA_BURST_TRANSPORT}${ULTRA_LDPC_Z:+ LDPC_Z=$ULTRA_LDPC_Z}${ULTRA_BURST_GROUP_FRAMES:+ GROUP_FRAMES=$ULTRA_BURST_GROUP_FRAMES}${ULTRA_FORCE_DATA_MOD:+ FORCE_MOD=$ULTRA_FORCE_DATA_MOD}${ULTRA_FORCE_DATA_RATE:+ FORCE_RATE=$ULTRA_FORCE_DATA_RATE}"

if [[ -z "$OUT" ]]; then
  stamp="$(date +%Y%m%d_%H%M%S)"
  OUT="/tmp/qam16_ladder_${CHANNEL}_snr${SNR_DB}_seed${SEED}_${stamp}"
fi

mkdir -p "$OUT"
TOKENS="$OUT/tokens.conf"
PAYLOAD="$OUT/qam16_${FILE_KB}KB.bin"
FILE_BYTES=$((FILE_KB * 1024))
# A/B runs at the same channel seed must also transmit the same bits. The old
# /dev/urandom fixture changed both the code and payload between arms, weakening
# a paired FEC comparison. Override separately when desired, otherwise bind the
# deterministic payload to the OTASim seed.
PAYLOAD_SEED="${ULTRA_SCENARIO_PAYLOAD_SEED:-$SEED}"
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

# Burst-path (OFDM FILE) decode failures. The file path does NOT emit `cw_fail=`:
# it reconstructs each frame-interleaved group through the deinterleaver and logs
# "Frame deinterleave decode FAILED (n/m CWs)" per failed frame (SUCCESS on the good
# ones). So on a file transfer `cw_fail` is structurally ~always 0 and is BLIND to the
# real decode trouble — this counter is the burst-path equivalent (the per-frame
# erasure the channel inflicts under fading, all recovered by ARQ). Count both logs
# (the receiver emits these; the sender stays 0) so the metric is side-symmetric.
count_deinterleave_fail() { count_pattern 'Frame deinterleave decode FAILED' "$1"; }
count_deinterleave_ok()   { count_pattern 'Frame deinterleave decode SUCCESS' "$1"; }

count_unexpected_rate() {
  local file="$1"
  awk -v expected="$EXPECT_RATE" '
    /Data mode set to:/ {
      if (index($0, " " expected " ") == 0) ++n
    }
    END { print n + 0 }
  ' "$file" 2>/dev/null
}

sum_tx_samples() {
  local file="$1"
  { grep -E 'TX(:| Burst:).* -> [0-9]+ samples' "$file" 2>/dev/null || true; } |
    sed -E 's/.* -> ([0-9]+) samples.*/\1/' |
    awk '{s += $1} END {printf "%.0f", s + 0}'
}

tx_seconds_from_samples() {
  awk -v samples="$1" 'BEGIN { printf "%.3f", samples / 48000.0 }'
}

tx_duty_pct() {
  awk -v samples="$1" -v elapsed="$2" '
    BEGIN {
      if (elapsed <= 0) {
        printf "0.0"
      } else {
        printf "%.1f", (samples / 48000.0) * 100.0 / elapsed
      }
    }'
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
  # The mode the run ACTUALLY negotiated (last "Data mode set to: <MOD> <RATE>"), recorded
  # for diagnostics — on a fading channel the rate is seed-dependent (Good vs Moderate
  # classification), so a sweep must read this rather than assume EXPECT_RATE.
  actual_data_mode="$(
    grep -Eo 'Data mode set to: [A-Z0-9]+ R[0-9]+/[0-9]+' "$ALPHA_LOG" 2>/dev/null |
      tail -1 | sed -E 's/Data mode set to: //' || true
  )"
  [[ -z "$actual_data_mode" ]] && actual_data_mode="unknown"
  alpha_unexpected_modes="$(count_pattern "$unexpected_mode_pattern" "$ALPHA_LOG")"
  bravo_unexpected_modes="$(count_pattern "$unexpected_mode_pattern" "$BRAVO_LOG")"
  alpha_unexpected_rates="$(count_unexpected_rate "$ALPHA_LOG")"
  bravo_unexpected_rates="$(count_unexpected_rate "$BRAVO_LOG")"
  # Force knobs select the entry profile; ULTRA_LOCK_RATE is what pins it after
  # handshake. Keep the unexpected-mode watchdog active for locked probes so a
  # force/authority configuration error cannot be reported as a fixed-rung PASS.
  #
  # --expect-mod any|coherent (2026-06-12, Phase 1): the adaptive ladder is ALLOWED to
  # vary/promote the modulation (e.g. QPSK -> 16QAM under ULTRA_ENABLE_QAM16_LADDER), so a
  # modulation change is EXPECTED, not a failure. Same disable so the watchdog doesn't
  # false-kill a legitimate promotion. PASS still requires CRC-clean delivery + Transfer
  # complete; only the modulation-pinning check is relaxed.
  #
  # 2026-07-02: with the fade-riding ladder default-ON (ULTRA_LOCK_RATE=0), mid-transfer
  # rate/modulation moves are the MECHANISM, not a failure — the pinning watchdog only
  # applies when the operator pinned the rate (ULTRA_LOCK_RATE=1).
  if [[ "$EXPECT_MOD" == "any" || "$EXPECT_MOD" == "coherent" \
        || "${ULTRA_LOCK_RATE:-0}" == "0" ]]; then
    alpha_unexpected_modes=0
    bravo_unexpected_modes=0
    alpha_unexpected_rates=0
    bravo_unexpected_rates=0
  fi
  file_crc_ok="$(count_pattern "\\[FILE\\] Received .*\\(${FILE_BYTES} bytes, CRC ok|FileTransfer: Received OK \\(${FILE_BYTES} bytes|Received OK .*${FILE_BYTES} bytes.*CRC" "$BRAVO_LOG")"
  alpha_file_done="$(count_pattern '\[FILE\] Transfer complete|FileTransfer: Transfer complete' "$ALPHA_LOG")"
  alpha_disconnected="$(count_pattern '\[SYS\] Disconnected|Connection state changed: 0|Disconnected from' "$ALPHA_LOG")"
  bravo_disconnected="$(count_pattern '\[SYS\] Disconnected|Connection state changed: 0|Disconnected from' "$BRAVO_LOG")"
  alpha_retx="$(count_pattern 'SR-ARQ: Retransmitting' "$ALPHA_LOG")"
  bravo_retx="$(count_pattern 'SR-ARQ: Retransmitting' "$BRAVO_LOG")"
  alpha_cwfail="$(sum_cw_fail "$ALPHA_LOG")"
  bravo_cwfail="$(sum_cw_fail "$BRAVO_LOG")"
  alpha_deint_fail="$(count_deinterleave_fail "$ALPHA_LOG")"
  bravo_deint_fail="$(count_deinterleave_fail "$BRAVO_LOG")"
  alpha_deint_ok="$(count_deinterleave_ok "$ALPHA_LOG")"
  bravo_deint_ok="$(count_deinterleave_ok "$BRAVO_LOG")"
  # Honest TOTAL decode failures = partial-CW failures (cw_fail=, control/non-burst path)
  # + whole-frame burst deinterleave failures (the file path). CWFAIL alone reads ~0 on a
  # file transfer even when fading erased a large fraction of frames (all ARQ-recovered).
  alpha_decode_fail=$((alpha_cwfail + alpha_deint_fail))
  bravo_decode_fail=$((bravo_cwfail + bravo_deint_fail))
  alpha_adaptive_mode_changes="$(count_pattern 'Connection: Adaptive MODE_CHANGE at TX boundary' "$ALPHA_LOG")"
  bravo_adaptive_mode_changes="$(count_pattern 'Connection: Adaptive MODE_CHANGE at TX boundary' "$BRAVO_LOG")"
  alpha_advisory_switches="$(count_pattern '\[ADPT\].*hysteresis allows switch' "$ALPHA_LOG")"
  bravo_advisory_switches="$(count_pattern '\[ADPT\].*hysteresis allows switch' "$BRAVO_LOG")"
  alpha_tx_samples="$(sum_tx_samples "$ALPHA_LOG")"
  bravo_tx_samples="$(sum_tx_samples "$BRAVO_LOG")"

  # ALPHA (sender) goodput ONLY — this is the honest on-air throughput. ALPHA's
  # "Transfer complete" timer spans the entire transfer (TX start -> done),
  # including every resend, escalation, and turnaround. BRAVO's "Received OK"
  # timer only spans its first-decode -> last-decode window, which is roughly
  # constant (~86 s for 21 KB) regardless of how many resends ALPHA paid, so it
  # OVER-reports and hides deep-fade cost (e.g. seed 2: BRAVO 2.0 kbps vs ALPHA
  # 1.04 kbps before the NACK fix). Never grep BRAVO_LOG for goodput.
  goodput_kbps="$(
    grep -E '\[FILE\] Transfer complete.*[0-9.]+ kbps|FileTransfer: Transfer complete.*[0-9.]+ kbps' "$ALPHA_LOG" 2>/dev/null |
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
  # PASS = the file delivered CRC-clean (BRAVO verified the file + ALPHA finalized the
  # transfer). DELIVERY is the verdict.
  #
  # Adaptive runs do not have to hit the exact expected (mod, rate). The old gate
  # `alpha_mode_count>0 && bravo_mode_count>0` counted the literal string
  # "configured for <EXPECT_MOD> <EXPECT_RATE>" and so FALSE-FAILED any delivered transfer
  # whose negotiated rate differed from --expect-rate. That is common and correct on a
  # fading channel: at `good --snr-db 16` the MEASURED fading varies seed-to-seed and many
  # seeds land in the Moderate class (>=0.65), where the ladder rightly picks QPSK R1/4 —
  # not the R2/3 a caller guessed. Those runs delivered CRC-clean but were stamped FAIL
  # (REASON=process_exit_before_pass), corrupting reliability sweeps. The actual negotiated
  # mode is recorded as ACTUAL_DATA_MODE in summary.env for diagnostics. Locked probes
  # are different: every reported data-mode rate must match EXPECT_RATE, and the
  # modulation watchdog below enforces EXPECT_MOD.
  #
  # We reject drift to an unexpected modulation on locked probes (e.g. QAM16 when
  # probing QPSK); rate changes are rejected by the unexpected-rate counters.
  #
  # We intentionally do NOT gate on disconnect bookkeeping: the disconnect INITIATOR (ALPHA,
  # on the payload-drained auto-disconnect) quits during teardown and never logs a
  # "Disconnected" string, so requiring alpha_disconnected>0 false-negatived clean runs.
  # (A delivered-but-no-clean-close run, e.g. BUG-FINACK-001, still PASSes — delivery is the
  # verdict; close cleanliness is tracked separately.)
  [[ "$alpha_unexpected_modes" -eq 0 ]] &&
  [[ "$bravo_unexpected_modes" -eq 0 ]] &&
  [[ "$alpha_unexpected_rates" -eq 0 ]] &&
  [[ "$bravo_unexpected_rates" -eq 0 ]] &&
  [[ "$file_crc_ok" -gt 0 ]] &&
  [[ "$alpha_file_done" -gt 0 ]]
}

hard_failure_reason() {
  local pattern='max retries exceeded|maximum retries exceeded|transfer failed|Transfer failed|FileTransfer: .*failed|FILE.*failed|SR-ARQ:.*retries exhausted|Connection: Connect failed|Connect failed after|giving up'
  if [[ "${alpha_unexpected_modes:-0}" -gt 0 || "${bravo_unexpected_modes:-0}" -gt 0 \
        || "${alpha_unexpected_rates:-0}" -gt 0 || "${bravo_unexpected_rates:-0}" -gt 0 ]]; then
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
  local alpha_tx_seconds bravo_tx_seconds alpha_tx_duty_pct bravo_tx_duty_pct max_tx_duty_pct channel_occupancy_pct
  alpha_tx_seconds="$(tx_seconds_from_samples "${alpha_tx_samples:-0}")"
  bravo_tx_seconds="$(tx_seconds_from_samples "${bravo_tx_samples:-0}")"
  alpha_tx_duty_pct="$(tx_duty_pct "${alpha_tx_samples:-0}" "$elapsed")"
  bravo_tx_duty_pct="$(tx_duty_pct "${bravo_tx_samples:-0}" "$elapsed")"
  max_tx_duty_pct="$(
    awk -v a="$alpha_tx_duty_pct" -v b="$bravo_tx_duty_pct" '
      BEGIN { printf "%.1f", (a > b ? a : b) }'
  )"
  channel_occupancy_pct="$(
    awk -v a="${alpha_tx_samples:-0}" -v b="${bravo_tx_samples:-0}" -v elapsed="$elapsed" '
      BEGIN {
        if (elapsed <= 0) printf "0.0"
        else printf "%.1f", ((a + b) / 48000.0) * 100.0 / elapsed
      }'
  )"
  {
    echo "OUT=$OUT"
    echo "CHANNEL=$CHANNEL"
    echo "SNR_DB=$SNR_DB"
    echo "SEED=$SEED"
    echo "EXPECT_MOD=$EXPECT_MOD"
    echo "EXPECT_RATE=$EXPECT_RATE"
    echo "FILE_BYTES=$FILE_BYTES"
    echo "PAYLOAD_SEED=$PAYLOAD_SEED"
    echo "PAYLOAD_SHA256=$PAYLOAD_SHA256"
    echo "EXIT_AFTER=$EXIT_AFTER"
    echo "ELAPSED_SEC=$elapsed"
    echo "ACTUAL_DATA_MODE=\"$actual_data_mode\""   # quoted: multi-word (e.g. "QPSK R2/3") must stay source-safe
    echo "ALPHA_MODE_COUNT=$alpha_mode_count"
    echo "BRAVO_MODE_COUNT=$bravo_mode_count"
    echo "ALPHA_UNEXPECTED_MODE_COUNT=$alpha_unexpected_modes"
    echo "BRAVO_UNEXPECTED_MODE_COUNT=$bravo_unexpected_modes"
    echo "ALPHA_UNEXPECTED_RATE_COUNT=$alpha_unexpected_rates"
    echo "BRAVO_UNEXPECTED_RATE_COUNT=$bravo_unexpected_rates"
    echo "FILE_CRC_OK_COUNT=$file_crc_ok"
    echo "ALPHA_FILE_DONE_COUNT=$alpha_file_done"
    echo "ALPHA_DISCONNECTED_COUNT=$alpha_disconnected"
    echo "BRAVO_DISCONNECTED_COUNT=$bravo_disconnected"
    echo "GOODPUT_BPS=$goodput_bps"
    echo "ALPHA_RETX_COUNT=$alpha_retx"
    echo "BRAVO_RETX_COUNT=$bravo_retx"
    echo "ALPHA_CWFAIL_COUNT=$alpha_cwfail"
    echo "BRAVO_CWFAIL_COUNT=$bravo_cwfail"
    echo "ALPHA_DEINTERLEAVE_FAIL_COUNT=$alpha_deint_fail"
    echo "BRAVO_DEINTERLEAVE_FAIL_COUNT=$bravo_deint_fail"
    echo "ALPHA_DEINTERLEAVE_OK_COUNT=$alpha_deint_ok"
    echo "BRAVO_DEINTERLEAVE_OK_COUNT=$bravo_deint_ok"
    echo "ALPHA_DECODE_FAIL_COUNT=$alpha_decode_fail"
    echo "BRAVO_DECODE_FAIL_COUNT=$bravo_decode_fail"
    echo "ALPHA_ADAPTIVE_MODE_CHANGE_COUNT=$alpha_adaptive_mode_changes"
    echo "BRAVO_ADAPTIVE_MODE_CHANGE_COUNT=$bravo_adaptive_mode_changes"
    echo "ADAPTIVE_MODE_CHANGE_COUNT=$((alpha_adaptive_mode_changes + bravo_adaptive_mode_changes))"
    echo "ALPHA_ADVISORY_SWITCH_COUNT=$alpha_advisory_switches"
    echo "BRAVO_ADVISORY_SWITCH_COUNT=$bravo_advisory_switches"
    echo "ADVISORY_SWITCH_COUNT=$((alpha_advisory_switches + bravo_advisory_switches))"
    echo "ALPHA_TX_SAMPLES=$alpha_tx_samples"
    echo "BRAVO_TX_SAMPLES=$bravo_tx_samples"
    echo "ALPHA_TX_SECONDS=$alpha_tx_seconds"
    echo "BRAVO_TX_SECONDS=$bravo_tx_seconds"
    echo "ALPHA_TX_DUTY_PCT=$alpha_tx_duty_pct"
    echo "BRAVO_TX_DUTY_PCT=$bravo_tx_duty_pct"
    echo "MAX_TX_DUTY_PCT=$max_tx_duty_pct"
    echo "CHANNEL_OCCUPANCY_PCT=$channel_occupancy_pct"
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

python3 - "$PAYLOAD" "$FILE_BYTES" "$PAYLOAD_SEED" <<'PY'
import random
import sys

path, count, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3], 0)
rng = random.Random(seed)
with open(path, "wb") as out:
    remaining = count
    while remaining:
        block = min(remaining, 65536)
        out.write(bytes(rng.randrange(256) for _ in range(block)))
        remaining -= block
PY
PAYLOAD_SHA256="$(shasum -a 256 "$PAYLOAD" | awk '{print $1}')"
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

# One-way FILE SENDER (design §14): BRAVO auto-accepts and receives; ALPHA
# connects and sends the file. No chat messaging (removed 2026-05-29).
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
