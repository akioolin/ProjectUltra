#!/usr/bin/env bash
# b2f_image_local_e2e.sh — single-machine Winlink B2F image transfer over the modem.
#
# Two `pat` (Winlink) clients ↔ two `ultra_tnc` over one `ota_simulator` (loopback, AWGN 30 dB):
#   PAT ALPHA  --compose+connect-->  ultra_tnc ALPHA  --OTASim-->  ultra_tnc BRAVO  --> PAT BRAVO
# ALPHA composes a JPEG attachment and forwards it P2P to BRAVO. We then decode BOTH mailboxes
# with `pat extract` and compare ALPHA-sent vs BRAVO-received — the true MODEM-integrity check
# (PAT may re-encode/compress the image on compose, so comparing to the on-disk original is wrong).
#
# Self-contained: generates its own pat configs / tokens / mailboxes in a temp dir; touches
# nothing in your real ~/.config/pat. No credentials — placeholder callsigns, empty passwords.
#
# Requires: `pat` (https://github.com/la5nta/pat). Override its path with PAT=/path/to/pat.
# Skips cleanly (exit 0) if `pat` is not installed, so it's safe in CI.
#
# Usage:  ./tools/b2f_image_local_e2e.sh
set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PAT="${PAT:-$HOME/go/bin/pat}"
IMG="${IMG:-$REPO_ROOT/tests/fixtures/image_util/ft891_setup_20kb.jpg}"
WORK="${WORK:-/tmp/b2f_image_e2e}"

OTA_GRPC=127.0.0.1:52001
OTA_UDP=127.0.0.1:52002
A_TNC_PORT=8300 ; B_TNC_PORT=8302
A_HTTP=localhost:8080 ; B_HTTP=localhost:8081

if [ ! -x "$PAT" ]; then
  echo "[SKIP] pat not found at '$PAT' (set PAT=/path/to/pat). https://github.com/la5nta/pat"
  exit 0
fi
[ -f "$IMG" ] || { echo "[FATAL] image fixture missing: $IMG"; exit 2; }
[ -x "$REPO_ROOT/build/ultra_tnc" ] && [ -x "$REPO_ROOT/build/ota_simulator" ] || {
  echo "[FATAL] build ultra_tnc + ota_simulator first (cmake --build build)"; exit 2; }

md5q() { if command -v md5 >/dev/null 2>&1; then md5 -q "$1"; else md5sum "$1" | cut -d' ' -f1; fi; }

cleanup() {
  pkill -f "$PAT" 2>/dev/null
  pkill -f "ultra_tnc --sim-audio" 2>/dev/null
  pkill -f "ota_simulator serve" 2>/dev/null
}
trap cleanup EXIT
cleanup; sleep 1
rm -rf "$WORK"; mkdir -p "$WORK/alpha_mbox" "$WORK/bravo_mbox" "$WORK/captures"

# ---- generated config (no secrets: placeholder calls, empty passwords) ----
printf 'alpha_tok:ALPHA:alpha\nbravo_tok:BRAVO:bravo\n' > "$WORK/tokens"
pat_cfg() { # $1=call $2=http $3=tnc_port $4=mbox $5=listen?
  cat > "$WORK/${1}_config.json" <<EOF
{ "mycall": "$1", "secure_login_password": "", "auxiliary_addresses": [], "locator": "",
  "service_codes": ["PUBLIC"], "http_addr": "$2", "motd": [], "connect_aliases": {},
  "listen": [$5], "hamlib_rigs": {},
  "varahf": { "addr": "localhost:$3", "bandwidth": 2300, "rig": "", "ptt_ctrl": false },
  "mailbox_path": "$4" }
EOF
}
pat_cfg ALPHA "$A_HTTP" "$A_TNC_PORT" "$WORK/alpha_mbox" ''
pat_cfg BRAVO "$B_HTTP" "$B_TNC_PORT" "$WORK/bravo_mbox" '"varahf"'

echo "[rig] OTASim serve ($OTA_GRPC, AWGN 30 dB)"
"$REPO_ROOT/build/ota_simulator" serve --bind "$OTA_GRPC" --udp-bind "$OTA_UDP" \
  --tokens "$WORK/tokens" --captures-root "$WORK/captures" \
  --lobby-channel awgn --lobby-snr-db 30 --lobby-seed 42 \
  --lobby-station-cap 16 --shutdown-deadline-sec 3600 > "$WORK/serve.log" 2>&1 &
for _ in $(seq 1 40); do grep -q OTASIM_SERVE_READY "$WORK/serve.log" 2>/dev/null && break; sleep 0.25; done
grep -q OTASIM_SERVE_READY "$WORK/serve.log" && echo "[rig]   serve READY" || { echo "[rig] serve FAILED"; exit 1; }

echo "[rig] TNC ALPHA :$A_TNC_PORT + BRAVO :$B_TNC_PORT"
"$REPO_ROOT/build/ultra_tnc" --sim-audio --ota-host "$OTA_GRPC" --token alpha_tok --station-id alpha \
  --session-id lobby --callsign ALPHA --port "$A_TNC_PORT" --log-level info \
  --log-category operator,tnc,modem --log-file "$WORK/alpha_tnc.log" > "$WORK/alpha_tnc.out" 2>&1 &
"$REPO_ROOT/build/ultra_tnc" --sim-audio --ota-host "$OTA_GRPC" --token bravo_tok --station-id bravo \
  --session-id lobby --callsign BRAVO --port "$B_TNC_PORT" --log-level info \
  --log-category operator,tnc,modem --log-file "$WORK/bravo_tnc.log" > "$WORK/bravo_tnc.out" 2>&1 &
sleep 4
grep -q "Connected to $OTA_GRPC as alpha" "$WORK/alpha_tnc.out" \
  && grep -q "Connected to $OTA_GRPC as bravo" "$WORK/bravo_tnc.out" \
  && echo "[rig]   both TNCs on OTASim" || { echo "[rig] TNC link FAILED"; exit 1; }

echo "[rig] PAT BRAVO (listening)"
"$PAT" --config "$WORK/BRAVO_config.json" --mbox "$WORK/bravo_mbox" http > "$WORK/bravo_pat.log" 2>&1 &
sleep 3

echo "[rig] compose ALPHA -> BRAVO with JPEG attachment ($(wc -c < "$IMG") B)"
printf "FT-891 setup screenshot attached (20 KB JPEG) over Winlink B2F.\n" \
  | "$PAT" --config "$WORK/ALPHA_config.json" --mbox "$WORK/alpha_mbox" \
      compose -s "Image test $(date +%H%M%S)" --attachment "$IMG" --p2p-only BRAVO > "$WORK/compose.log" 2>&1
echo "[rig]   $(grep -c 'Message posted' "$WORK/compose.log") message posted"

echo "[rig] CONNECT ALPHA -> BRAVO (B2F image transfer — may take a few minutes)"
"$PAT" --config "$WORK/ALPHA_config.json" --mbox "$WORK/alpha_mbox" --send-only \
  connect "varahf:///BRAVO" > "$WORK/alpha_connect.log" 2>&1
echo "[rig] connect exit=$?"
echo "===== B2F dialogue tail ====="; grep -vE "[0-9]+%$" "$WORK/alpha_connect.log" | tail -10

echo "===== ATTACHMENT VERIFY (pat extract: ALPHA-sent vs BRAVO-received = true modem check) ====="
RX=$(ls -1 "$WORK"/bravo_mbox/BRAVO/in/*.b2f 2>/dev/null | head -1)
TX=$(ls -1 "$WORK"/alpha_mbox/ALPHA/sent/*.b2f 2>/dev/null | head -1)
RC=1
if [ -n "$RX" ] && [ -n "$TX" ]; then
  rm -rf "$WORK/x_rx" "$WORK/x_tx"; mkdir -p "$WORK/x_rx" "$WORK/x_tx"
  ( cd "$WORK/x_rx" && "$PAT" extract "$RX" >/dev/null 2>&1 )
  ( cd "$WORK/x_tx" && "$PAT" extract "$TX" >/dev/null 2>&1 )
  RXJ=$(find "$WORK/x_rx" -type f ! -name '*.b2f' | head -1)
  TXJ=$(find "$WORK/x_tx" -type f ! -name '*.b2f' | head -1)
  echo "original on disk : $(wc -c < "$IMG") B  md5 $(md5q "$IMG" | cut -c1-12)"
  [ -f "$TXJ" ] && echo "ALPHA sent (PAT) : $(wc -c < "$TXJ") B  md5 $(md5q "$TXJ" | cut -c1-12)"
  [ -f "$RXJ" ] && echo "BRAVO received   : $(wc -c < "$RXJ") B  md5 $(md5q "$RXJ" | cut -c1-12)"
  if [ -n "$RXJ" ] && [ -n "$TXJ" ] && cmp -s "$TXJ" "$RXJ"; then
    echo "MODEM RESULT: sent == received  ->  BYTE-IDENTICAL (clean burst transfer)"; RC=0
    cmp -s "$TXJ" "$IMG" || echo "NOTE: PAT re-encoded the JPEG on compose ($(wc -c <"$IMG") B -> $(wc -c <"$TXJ") B) — that is PAT, not the modem."
  else
    echo "MODEM RESULT: sent != received  ->  TRANSFER CORRUPTION"
  fi
else
  echo "NOT delivered (RX='$RX' TX='$TX')"
fi
echo "[rig] logs under $WORK"
exit $RC
