#!/usr/bin/env bash
# Foundation test: raw PHY decode reliability of a forced mod/rate in FADING,
# measured from per-frame cw_ok/cw_fail in the phy-diag log (the decode outcome
# BEFORE ARQ rescues anything). Trusted path = cli_simulator (NOT test_waveform_simple).
#
# Reports, per channel x SNR (aggregated over seeds): codeword error rate (CWER),
# frame error rate (FER = frames with >=1 failed CW), total CW/frames, end-to-end retx.
#
# Usage: tools/phy_fading_reliability.sh <mod> <rate> <file_bytes> "<snr_list>" "<chan_list>" "<seed_list>"
set -u
cd "$(dirname "$0")/.."
MOD="${1:-qpsk}"; RATE="${2:-r2_3}"; FILE="${3:-5120}"
SNRS="${4:-20 15}"; CHANS="${5:-good moderate}"; SEEDS="${6:-1 2 3}"
BIN=./build/cli_simulator
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "PHY foundation reliability: $MOD $RATE, file=${FILE}B, seeds=[$SEEDS]"
echo "(CWER/FER from RX cw_ok/cw_fail on DATA frames — raw decode, pre-ARQ)"
printf "%-9s %-5s %8s %8s %8s %8s %-7s\n" CHAN SNR CW_tot CW_fail CWER FER retx_tot
echo "------------------------------------------------------------------"
for chan in $CHANS; do
  for snr in $SNRS; do
    cw_ok=0; cw_fail=0; fr=0; fr_bad=0; retx=0
    for sd in $SEEDS; do
      dg="$TMP/d_${chan}_${snr}_${sd}.diag"
      out=$("$BIN" --expert --mod "$MOD" --rate "$RATE" --channel "$chan" --snr "$snr" \
                   --seed "$sd" --file "$FILE" --phy-diag-log "$dg" --test 2>&1)
      r=$(echo "$out" | grep -oE 'retransmissions=[0-9]+' | head -1 | grep -oE '[0-9]+'); retx=$((retx + ${r:-0}))
      # aggregate per-DATA-frame decode outcomes
      while read -r o f; do
        cw_ok=$((cw_ok + o)); cw_fail=$((cw_fail + f)); fr=$((fr + 1))
        [ "$f" -gt 0 ] && fr_bad=$((fr_bad + 1))
      done < <(grep -E 'event=station_frame_quality' "$dg" 2>/dev/null | grep -E 'frame_type=DATA' \
               | grep -oE 'cw_ok=[0-9]+ cw_fail=[0-9]+' | sed -E 's/cw_ok=([0-9]+) cw_fail=([0-9]+)/\1 \2/')
    done
    cw_tot=$((cw_ok + cw_fail))
    cwer=$(awk -v a="$cw_fail" -v b="$cw_tot" 'BEGIN{printf (b>0)?"%.4f":"n/a", (b>0)?a/b:0}')
    fer=$(awk -v a="$fr_bad" -v b="$fr" 'BEGIN{printf (b>0)?"%.4f":"n/a", (b>0)?a/b:0}')
    printf "%-9s %-5s %8d %8d %8s %8s %-7d\n" "$chan" "$snr" "$cw_tot" "$cw_fail" "$cwer" "$fer" "$retx"
  done
done
echo "------------------------------------------------------------------"
echo "CWER=codeword err rate; FER=frame err rate (>=1 bad CW); retx_tot=end-to-end retx across seeds"
