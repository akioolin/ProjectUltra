#!/usr/bin/env bash
# Trustworthy multi-seed baseline for the fading-reliability campaign.
# Current binaries (clean HEAD + hole-probe fix; NO sigma2, NO Wiener), faithful
# real-time GUI auto-path. Records FER proxy (BRAVO CWFAIL), retx, goodput, R1/2
# downgrades per seed so every later change is judged against the multi-seed mean,
# not a single noisy run (same cell has varied 4/12/20 CWfail).
#
# Usage: tools/good20_baseline_sweep.sh "<seed_list>"
set -u
cd "$(dirname "$0")/.."
SEEDS="${1:-1 3 4 5}"
TAG="${2:-baseline}"
printf "%-5s %-7s %-9s %-6s %-8s %-9s %s\n" SEED RESULT CWFAIL RETX GOODPUT DOWNGRADE OUT
echo "------------------------------------------------------------------------"
for sd in $SEEDS; do
  out="/tmp/good20_${TAG}_s${sd}"
  ./tools/qam16_ladder_scenario.sh --channel good --snr-db 20 --seed "$sd" \
    --expect-rate R2/3 --expect-mod QPSK --message-count 2 --out "$out" \
    > "$out.run" 2>&1
  s="$out/summary.env"
  res=$(grep -oE 'RESULT=[A-Z]+' "$s" 2>/dev/null | cut -d= -f2)
  cw=$(grep -oE 'BRAVO_CWFAIL_COUNT=[0-9]+' "$s" 2>/dev/null | cut -d= -f2)
  rtx=$(grep -oE 'ALPHA_RETX_COUNT=[0-9]+' "$s" 2>/dev/null | cut -d= -f2)
  gp=$(grep -oE 'GOODPUT_BPS=[0-9]+' "$s" 2>/dev/null | cut -d= -f2)
  dg=$(grep -cE 'Forced downgrade|Adaptive downgrade queued' "$out/alpha.log" 2>/dev/null)
  printf "%-5s %-7s %-9s %-6s %-8s %-9s %s\n" "$sd" "${res:-NONE}" "${cw:-?}" "${rtx:-?}" "${gp:-?}" "${dg:-?}" "$out"
done
echo "------------------------------------------------------------------------"
echo "BASELINE SWEEP DONE ($TAG): seeds=[$SEEDS]"
