#!/usr/bin/env bash
# Phase 0 chain-validation gate: clean-channel bit-exact identity sweep on the
# TRUSTED path (cli_simulator real connected protocol — NOT test_waveform_simple,
# which lies on OFDM 4-CW frames; see project_test_waveform_simple_cw0_artifact).
#
# At high-SNR AWGN with zero CFO, every supported {mod}x{rate} must deliver the
# file with failed=0 and retx ~0. Anything that fails here is a STRUCTURAL bug,
# localized to that mod/rate — not a noise/margin issue.
#
# Usage: tools/chain_validation_gate.sh [SNR] [FILE_BYTES] [SEED]
set -u
cd "$(dirname "$0")/.."
SNR="${1:-38}"
FILE="${2:-4096}"
SEED="${3:-42}"
BIN=./build/cli_simulator
RETX_TOL=2   # allow a small handshake-edge retx; structural bug = many more

[ -x "$BIN" ] || { echo "ERROR: $BIN not built"; exit 2; }

MODS=(dqpsk qpsk d8psk qam16)
RATES=(r1_4 r1_2 r2_3 r3_4)

printf "%-8s %-6s %-8s %-6s %-6s %-9s %s\n" MOD RATE RESULT RETX FAILED GOODPUT VERDICT
echo "-------------------------------------------------------------------------"
pass=0; fail=0; unsup=0
for mod in "${MODS[@]}"; do
  for rate in "${RATES[@]}"; do
    out=$("$BIN" --expert --mod "$mod" --rate "$rate" --channel awgn --snr "$SNR" \
                 --seed "$SEED" --file "$FILE" --test 2>&1)
    rc=$?
    res=$(echo "$out"   | grep -oE 'TEST (PASSED|FAILED)' | head -1 | awk '{print $2}')
    # UNSUPPORTED only if the run never produced a TEST verdict (parser/setup rejected
    # the mod/rate). A benign handshake "Unsupported modulation, using DQPSK" WARN does
    # NOT count — the OFDM data phase still ran. Key off the TEST verdict, not log text.
    if [ -z "$res" ]; then
      reason="ERROR"
      echo "$out" | grep -qiE 'invalid|unknown (mod|rate)|not (a valid|supported|allowed) (mod|rate)|usage:' && reason="UNSUPPORTED"
      printf "%-8s %-6s %-8s %-6s %-6s %-9s %s\n" "$mod" "$rate" "(rc=$rc)" "-" "-" "-" "$reason"
      [ "$reason" = "UNSUPPORTED" ] && unsup=$((unsup+1)) || fail=$((fail+1))
      continue
    fi
    retx=$(echo "$out"  | grep -oE 'retransmissions=[0-9]+' | head -1 | grep -oE '[0-9]+')
    failed=$(echo "$out"| grep -oE 'failed=[0-9]+' | head -1 | grep -oE '[0-9]+')
    gp=$(echo "$out"    | grep -oE 'End-to-end goodput:.*= [0-9]+ bps' | grep -oE '[0-9]+ bps' | head -1)
    retx="${retx:-?}"; failed="${failed:-?}"; res="${res:-NONE}"; gp="${gp:-?}"
    verdict="FAIL"
    if [ "$res" = "PASSED" ] && [ "$failed" = "0" ] && [ "$retx" != "?" ] && [ "$retx" -le "$RETX_TOL" ]; then
      verdict="OK"; pass=$((pass+1))
    else
      fail=$((fail+1))
    fi
    printf "%-8s %-6s %-8s %-6s %-6s %-9s %s\n" "$mod" "$rate" "$res" "$retx" "$failed" "$gp" "$verdict"
  done
done
echo "-------------------------------------------------------------------------"
echo "SUMMARY: OK=$pass  FAIL=$fail  UNSUPPORTED=$unsup   (SNR=$SNR AWGN, file=${FILE}B, seed=$SEED, retx_tol=$RETX_TOL)"
[ "$fail" -eq 0 ] && echo "GATE: PASS" || echo "GATE: FAIL (structural bug localized above)"
