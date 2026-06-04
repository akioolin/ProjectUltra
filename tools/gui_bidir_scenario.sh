#!/bin/bash
# gui_bidir_scenario.sh — BIDIRECTIONAL file transfer over the GUI sim path.
#
# Two real `ultra_gui -sim` stations over `ota_simulator serve`, both with
# --half-duplex: ALPHA connects + sends a file to BRAVO, then the turn flips
# (role-swap) and BRAVO sends a file BACK to ALPHA, all in ONE session. The
# over-the-air exchange is captured to per-station WAVs and (if the viewer is
# built) rendered to an annotated waterfall PNG.
#
# This is the GUI counterpart of the TNC bidirectional burst — same ModemEngine
# PHY, exercised through the production GUI app + the --half-duplex flag.
#
# Usage:
#   tools/gui_bidir_scenario.sh [--channel awgn] [--snr-db 30] [--seed 42]
#                               [--file-kb 8] [--out /tmp/gui_bidir] [--no-render]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

CHANNEL=awgn; SNR_DB=30; SEED=42; FILE_KB=8; OUT=/tmp/gui_bidir; RENDER=1
while [ $# -gt 0 ]; do case "$1" in
  --channel) CHANNEL="$2"; shift 2;;
  --snr-db) SNR_DB="$2"; shift 2;;
  --seed) SEED="$2"; shift 2;;
  --file-kb) FILE_KB="$2"; shift 2;;
  --out) OUT="$2"; shift 2;;
  --no-render) RENDER=0; shift;;
  *) echo "unknown arg $1"; exit 2;;
esac; done

GRPC=127.0.0.1:52051
GROUPS=$(( (FILE_KB * 1024 + 1337) / 1338 ))   # ~1338 payload B/group at z=81 QPSK R1/2
EXIT=$(( 80 + FILE_KB * 22 ))                   # generous hard deadline
mkdir -p "$OUT/caps"
find "$OUT" -maxdepth 1 -name '*.log' -delete 2>/dev/null

cat > "$OUT/tokens" <<EOF
alpha_tok:ALPHA:alpha
bravo_tok:BRAVO:bravo
cap_admin:CAPADMIN:cap:admin
EOF
python3 - "$OUT/fileA.bin" "$OUT/fileB.bin" "$((FILE_KB*1024))" <<'PY'
import sys
n=int(sys.argv[3])
open(sys.argv[1],'wb').write(bytes((i*37+11)&255 for i in range(n)))
open(sys.argv[2],'wb').write(bytes((i*53+7)&255 for i in range(n)))
PY

echo "[rig] cleanup"; pkill -f "ultra_gui -sim" 2>/dev/null; pkill -f "ota_simulator serve" 2>/dev/null; sleep 1

echo "[rig] OTASim serve ($CHANNEL @ ${SNR_DB}dB, seed $SEED)"
"$ROOT/build/ota_simulator" serve --bind "$GRPC" --udp-bind 127.0.0.1:52052 \
  --tokens "$OUT/tokens" --captures-root "$OUT/caps" \
  --lobby-channel "$CHANNEL" --lobby-snr-db "$SNR_DB" --lobby-seed "$SEED" \
  --shutdown-deadline-sec 900 > "$OUT/serve.log" 2>&1 &
sleep 3
grep -q OTASIM_SERVE_READY "$OUT/serve.log" || { echo "serve FAILED"; cat "$OUT/serve.log"; exit 1; }
echo "[rig]   serve READY"

export ULTRA_LDPC_Z=81 ULTRA_BURST_TRANSPORT=1 ULTRA_ADAPTIVE_RATE=1 ULTRA_LOCK_RATE=1

echo "[rig] launch BRAVO (auto-accept + send fileB) + ALPHA (connect + send fileA), both --half-duplex"
"$ROOT/build/ultra_gui" -sim --ota-host "$GRPC" --token bravo_tok --station-id BRAVO --session-id lobby \
  --auto-accept --auto-send-file "$OUT/fileB.bin" --half-duplex --exit-after "$EXIT" \
  --log-level info --log-category modem,operator,tnc --log-file "$OUT/bravo.log" >/dev/null 2>&1 &
"$ROOT/build/ultra_gui" -sim --ota-host "$GRPC" --token alpha_tok --station-id ALPHA --session-id lobby \
  --auto-connect BRAVO --connect-delay 3 --auto-send-file "$OUT/fileA.bin" --half-duplex --exit-after "$EXIT" \
  --log-level info --log-category modem,operator,tnc --log-file "$OUT/alpha.log" >/dev/null 2>&1 &
sleep 6

"$ROOT/build/otasim_ctl" --server "$GRPC" --token cap_admin --session lobby start-capture >/dev/null 2>&1
echo "[rig] capturing — waiting for BOTH directions ($GROUPS groups each)..."
for i in $(seq 1 $(( (EXIT/5) ))); do
  b=$(grep -hc "delivered as unit" "$OUT/bravo.log" 2>/dev/null); b=${b//[!0-9]/}; b=${b:-0}
  a=$(grep -hc "delivered as unit" "$OUT/alpha.log" 2>/dev/null); a=${a//[!0-9]/}; a=${a:-0}
  echo "  t=$((i*5))s  BRAVO(fileA)=$b/$GROUPS  ALPHA(fileB)=$a/$GROUPS"
  if [ "$b" -ge "$GROUPS" ] && [ "$a" -ge "$GROUPS" ]; then echo "[rig] BOTH directions delivered"; sleep 4; break; fi
  sleep 5
done

"$ROOT/build/otasim_ctl" --server "$GRPC" --token cap_admin --session lobby stop-capture >/dev/null 2>&1
sleep 4
pkill -INT -f "ultra_gui -sim" 2>/dev/null; sleep 1
pkill -INT -f "ota_simulator serve" 2>/dev/null; sleep 2
pkill -f "ultra_gui -sim" 2>/dev/null; pkill -f "ota_simulator serve" 2>/dev/null
echo "[rig] captures in $OUT/caps/lobby/"

if [ "$RENDER" = 1 ] && [ -x "$ROOT/build/ultra_waterfall_viewer" ]; then
  echo "[rig] rendering annotated waterfall -> $OUT/gui_exchange.png"
  "$ROOT/build/ultra_waterfall_viewer" \
    "$OUT/caps/lobby/ALPHA_rx_48k_f32.wav" "$OUT/caps/lobby/BRAVO_rx_48k_f32.wav" \
    --annotate "$OUT/gui_exchange.png" --fmax 3000 --freq-scale 6 --db-min -58 --db-max -14 --width 3600
fi
