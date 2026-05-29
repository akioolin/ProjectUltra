#!/usr/bin/env bash
# Good@20 fading-reliability gate, faithful real-time GUI auto-path. Records FER
# proxy (BRAVO CWFAIL), retx, goodput, downgrades per seed so every change is
# judged against the multi-run mean, not a single noisy run (same cell has
# varied 4/12/20 CWfail).
#
# Defaults are the SIMPLE QSO R3/4 probe: seed 3, --expect-rate R3/4, and
# --message-count 1 — one message each way (alpha->bravo, bravo->alpha) then an
# alpha->bravo file transfer. This is the minimal realistic exchange, kept small
# on purpose: the heavy default ladder sends 3 chat messages each way AND the
# PASS gate (gui_qso_scenario.sh PASS gate) requires every one delivered both
# directions, so on a marginal rung a single dropped chat message fails the run
# even when the file was clean, and the extra two-way chat adds half-duplex
# turn-taking contention unrelated to the file. One message each keeps a real
# handshake-into-data exchange without burying the file result in chat noise.
#
# Usage: tools/good20_baseline_sweep.sh "[seeds]" "[tag]" "[expect_rate]" "[msg_count]"
#   default (seed3, R3/4, 1 msg each + file):  tools/good20_baseline_sweep.sh
#   repeat seed3 5x:                           tools/good20_baseline_sweep.sh "3 3 3 3 3"
#   legacy heavy R2/3 (3 msgs each):           tools/good20_baseline_sweep.sh "1 3 4 5" baseline R2/3 3
set -u
cd "$(dirname "$0")/.."
SEEDS="${1:-3}"
TAG="${2:-seed3_r34_simpleqso}"
RATE="${3:-R3/4}"
MSGS="${4:-1}"
echo "Good@20 probe: rate=$RATE  message-count=$MSGS  seeds=[$SEEDS]"
printf "%-5s %-7s %-9s %-6s %-8s %-9s %-5s %-7s %s\n" SEED RESULT CWFAIL RETX GOODPUT DOWNGRADE MC DUTY OUT
echo "------------------------------------------------------------------------"
run_idx=0
for sd in $SEEDS; do
  run_idx=$((run_idx + 1))
  out="/tmp/good20_${TAG}_s${sd}_r${run_idx}"
  ./tools/gui_qso_scenario.sh --channel good --snr-db 20 --seed "$sd" \
    --expect-rate "$RATE" --expect-mod QPSK --message-count "$MSGS" --out "$out" \
    > "$out.run" 2>&1
  s="$out/summary.env"
  res=$(grep -oE 'RESULT=[A-Z]+' "$s" 2>/dev/null | cut -d= -f2)
  cw=$(grep -oE 'BRAVO_CWFAIL_COUNT=[0-9]+' "$s" 2>/dev/null | cut -d= -f2)
  rtx=$(grep -oE 'ALPHA_RETX_COUNT=[0-9]+' "$s" 2>/dev/null | cut -d= -f2)
  gp=$(grep -oE 'GOODPUT_BPS=[0-9]+' "$s" 2>/dev/null | cut -d= -f2)
  duty=$(grep -oE 'MAX_TX_DUTY_PCT=[0-9.]+' "$s" 2>/dev/null | cut -d= -f2)
  mc=$(grep -oE 'ADAPTIVE_MODE_CHANGE_COUNT=[0-9]+' "$s" 2>/dev/null | cut -d= -f2)
  dg=$(grep -cE 'Forced downgrade|Adaptive downgrade queued' "$out/alpha.log" 2>/dev/null)
  printf "%-5s %-7s %-9s %-6s %-8s %-9s %-5s %-7s %s\n" "$sd" "${res:-NONE}" "${cw:-?}" "${rtx:-?}" "${gp:-?}" "${dg:-?}" "${mc:-?}" "${duty:-?}" "$out"
done
echo "------------------------------------------------------------------------"
echo "BASELINE SWEEP DONE ($TAG): seeds=[$SEEDS]"
