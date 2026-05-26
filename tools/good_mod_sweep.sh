#!/usr/bin/env bash
# Parametrized multi-seed Good-fading sweep for the throughput campaign.
# Unlike good20_baseline_sweep.sh (hardcoded QPSK R2/3), this takes the expected
# modulation/rate/SNR so it works for the 8PSK (QAM8) and QAM16 ladder probes
# without the harness flagging the negotiated mode as unexpected. Reports the
# ACTUAL negotiated mod/rate/cw from the log alongside CWFAIL/retx/goodput/duty,
# so a rate mismatch (e.g. ladder picks R1/2 at a marginal SNR) is visible rather
# than silently failing the run. Faithful real-time GUI auto-path.
#
# Usage: tools/good_mod_sweep.sh "<seeds>" <tag> <EXPECT_MOD> <EXPECT_RATE> [snr_db]
#   e.g. tools/good_mod_sweep.sh "1 2 3" psk8 QAM8 R1/2 20
set -u
cd "$(dirname "$0")/.."
SEEDS="${1:-1 2 3}"
TAG="${2:-modsweep}"
EXPECT_MOD="${3:-QPSK}"
EXPECT_RATE="${4:-R2/3}"
SNR="${5:-20}"
printf "%-5s %-7s %-9s %-6s %-8s %-9s %-7s %-22s %s\n" SEED RESULT CWFAIL RETX GOODPUT DOWNGRADE DUTY ACTUAL_MODE OUT
echo "---------------------------------------------------------------------------------------------"
for sd in $SEEDS; do
  out="/tmp/good_${TAG}_s${sd}"
  ./tools/qam16_ladder_scenario.sh --channel good --snr-db "$SNR" --seed "$sd" \
    --expect-rate "$EXPECT_RATE" --expect-mod "$EXPECT_MOD" --message-count 2 --out "$out" \
    > "$out.run" 2>&1
  s="$out/summary.env"
  res=$(grep -oE 'RESULT=[A-Z]+' "$s" 2>/dev/null | cut -d= -f2)
  cw=$(grep -oE 'BRAVO_CWFAIL_COUNT=[0-9]+' "$s" 2>/dev/null | cut -d= -f2)
  rtx=$(grep -oE 'ALPHA_RETX_COUNT=[0-9]+' "$s" 2>/dev/null | cut -d= -f2)
  gp=$(grep -oE 'GOODPUT_BPS=[0-9]+' "$s" 2>/dev/null | cut -d= -f2)
  duty=$(grep -oE 'MAX_TX_DUTY_PCT=[0-9.]+' "$s" 2>/dev/null | cut -d= -f2)
  dg=$(grep -cE 'Forced downgrade|Adaptive downgrade queued' "$out/alpha.log" 2>/dev/null)
  actual=$(grep -hoE 'cw=[0-9]+, continuation_reanchor=[0-9]+ms \(OFDM [A-Z0-9]+ R[0-9]/[0-9]' "$out/alpha.log" 2>/dev/null \
           | sed -E 's/.*\(OFDM /(/; s/cw=([0-9]+).*/cw\1/' | head -1)
  cwn=$(grep -hoE 'cw=[0-9]+, continuation' "$out/alpha.log" 2>/dev/null | grep -oE '[0-9]+' | head -1)
  mr=$(grep -hoE 'OFDM [A-Z0-9]+ R[0-9]/[0-9]' "$out/alpha.log" 2>/dev/null | head -1 | sed 's/OFDM //')
  printf "%-5s %-7s %-9s %-6s %-8s %-9s %-7s %-22s %s\n" "$sd" "${res:-NONE}" "${cw:-?}" "${rtx:-?}" "${gp:-?}" "${dg:-?}" "${duty:-?}" "${mr:-none} cw${cwn:-?}" "$out"
done
echo "---------------------------------------------------------------------------------------------"
echo "MOD SWEEP DONE ($TAG): seeds=[$SEEDS] expect=$EXPECT_MOD $EXPECT_RATE @ Good SNR=$SNR"
