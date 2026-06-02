#!/usr/bin/env bash
#
# analyze_qso_run.sh — extract the REAL decode / fade-recovery metrics from a
# gui_qso_scenario.sh --out directory.
#
# WHY THIS EXISTS: summary.env's RETX_COUNT / CWFAIL_COUNT track only specific
# SR-ARQ / adaptive-downgrade log lines. They do NOT count the burst-transport
# whole-group GROUP_NACK resends that actually recover fade-nulled groups, so on
# a fading channel they read 0 even while ARQ is busy. The truth is in the
# receiver's per-group log:
#   "Burst group_seq=N delivered as unit: K/6 logical OK (all_ok=X) max_iters=M"
# A damaged group (all_ok=0) shows max_iters=50 (the cap) and is whole-group
# resent; a clean group (all_ok=1) shows the real LDPC iteration count, which is
# a direct margin proxy (0 = comfortable, climbing toward 50 = near the cliff).
#
# Usage:
#   tools/analyze_qso_run.sh <out_dir>          # human-readable block
#   tools/analyze_qso_run.sh --csv <out_dir>    # one CSV row (for sweeps)
#   tools/analyze_qso_run.sh --csv-header        # print the CSV header
#
set -euo pipefail

CSV_HEADER='channel,snr_db,mod,rate,file_kb,result,crc_ok,goodput_bps,elapsed_s,uniq_groups,attempts,damaged,damage_pct,nacks,it_min,it_med,it_max,tx_duty_pct'

MODE=human
case "${1:-}" in
  --csv)        MODE=csv; shift ;;
  --csv-header) printf '%s\n' "$CSV_HEADER"; exit 0 ;;
  -h|--help)    sed -n '2,30p' "$0"; exit 0 ;;
esac

OUT="${1:?usage: analyze_qso_run.sh [--csv|--csv-header] <out_dir>}"
SENV="$OUT/summary.env"
[[ -f "$SENV" ]] || { echo "analyze_qso_run: no summary.env in $OUT" >&2; exit 1; }

# Topline from summary.env (RESULT/REASON/GOODPUT/etc.)
# shellcheck disable=SC1090
source "$SENV"
RX_LOG="${BRAVO_LOG:-$OUT/bravo.log}"   # BRAVO is the file receiver
[[ -f "$RX_LOG" ]] || RX_LOG="$OUT/bravo.log"

# Per-group truth from the receiver log. perl (portable regex captures; macOS awk
# lacks match(str,re,arr)). Emits: uniq attempts damaged damage_pct nacks itmin itmed itmax
read -r UNIQ ATTEMPTS DAMAGED DAMAGE_PCT NACKS IT_MIN IT_MED IT_MAX < <(
  perl -ne '
    if (/group_seq=(\d+) delivered as unit:\s*(\d+)\/6 logical OK \(all_ok=(\d)\)(?:\s+max_iters=(\d+))?/) {
      my ($seq,$ok,$it) = ($1,$3,$4);
      $att++; $seen{$seq}=1;
      if ($ok==1) { push @it,$it if defined $it; } else { $dmg++; }
    }
    $nack++ if /tone-burst NACK|GROUP_NACK/;
    END {
      my $uniq = scalar keys %seen;
      my @s = sort {$a<=>$b} @it;
      my $mn = @s ? $s[0]        : 0;
      my $mx = @s ? $s[-1]       : 0;
      my $md = @s ? $s[int(@s/2)] : 0;
      my $dp = $att ? 100*($dmg//0)/$att : 0;
      printf "%d %d %d %.0f %d %d %d %d\n", $uniq, ($att//0), ($dmg//0), $dp, ($nack//0), $mn, $md, $mx;
    }
  ' "$RX_LOG" 2>/dev/null || echo "0 0 0 0 0 0 0 0"
)

FILE_KB=$(( ${FILE_BYTES:-0} / 1024 ))
CRC_OK=$([[ "${FILE_CRC_OK_COUNT:-0}" -ge 1 ]] && echo 1 || echo 0)
GOODPUT="${GOODPUT_BPS:-0}"
ELAPSED="${ELAPSED_SEC:-0}"
DUTY="${MAX_TX_DUTY_PCT:-0}"
MOD="${EXPECT_MOD:-?}"; RATE="${EXPECT_RATE:-?}"; CH="${CHANNEL:-?}"; SNR="${SNR_DB:-?}"

if [[ "$MODE" == csv ]]; then
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$CH" "$SNR" "$MOD" "$RATE" "$FILE_KB" "${RESULT:-?}" "$CRC_OK" "$GOODPUT" "$ELAPSED" \
    "$UNIQ" "$ATTEMPTS" "$DAMAGED" "$DAMAGE_PCT" "$NACKS" "$IT_MIN" "$IT_MED" "$IT_MAX" "$DUTY"
  exit 0
fi

# Human block
verdict="$RESULT"
[[ "$CRC_OK" == 1 ]] || verdict="$RESULT (file NOT delivered)"
printf '=== QSO run: %s @ %s dB — %s %s — %s KB ===\n' "$CH" "$SNR" "$MOD" "$RATE" "$FILE_KB"
printf '  result   : %s  (%s)\n' "$verdict" "${REASON:-?}"
printf '  file CRC : %s\n' "$([[ "$CRC_OK" == 1 ]] && echo OK || echo MISSING)"
printf '  goodput  : %s bps   elapsed %ss   TX duty %s%%\n' "$GOODPUT" "$ELAPSED" "$DUTY"
printf '  groups   : %s delivered, %s reception attempts, %s fade-damaged (%s%%)\n' \
  "$UNIQ" "$ATTEMPTS" "$DAMAGED" "$DAMAGE_PCT"
printf '  ARQ      : %s whole-group NACK/resend requests\n' "$NACKS"
printf '  LDPC iter: min %s / median %s / max %s   (over clean decodes; 50=cap=fail)\n' \
  "$IT_MIN" "$IT_MED" "$IT_MAX"
