#!/usr/bin/env bash
#
# check_hw_audio_path.sh - Raw bidirectional audio sanity test for Mac <-> Pi.
#
# This intentionally bypasses the modem, SDL, ARQ, FEC, and channel injection.
# It records each physical direction while the other side plays a sine tone,
# then reports RMS/frequency/headroom. Use this before hardware modem tests
# when the link looks silent, clipped, or asymmetric.

set -euo pipefail

PI=${PI:-math@pi5tester}
PI_PLAYBACK_DEV=${PI_PLAYBACK_DEV:-plughw:0,0}
PI_CAPTURE_DEV=${PI_CAPTURE_DEV:-plughw:0,0}
MAC_AV_AUDIO_INDEX=${MAC_AV_AUDIO_INDEX:-0}   # ffmpeg avfoundation audio index
MAC_OUTPUT_DEVICE=${MAC_OUTPUT_DEVICE:-Sound Blaster Play! 3}
DURATION=${DURATION:-8}
FREQ=${FREQ:-1000}
MAC_TONE_VOLUME=${MAC_TONE_VOLUME:-0.5}
LOG_DIR=${LOG_DIR:-/tmp/ultra_audio_path_$(date +%Y%m%d_%H%M%S)}

SSH_ARGS=()
if [[ -n "${SSH_KEY:-}" ]]; then
  SSH_ARGS=(-i "$SSH_KEY")
fi

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 2
  fi
}

capture_stat() {
  local capture="$1"
  local label="$2"
  shift 2
  local out rms freq peak
  out=$(sox "$capture" -n "$@" stat 2>&1 >/dev/null || true)
  rms=$(awk '/RMS[[:space:]]+amplitude/ {print $3}' <<<"$out")
  peak=$(awk '/Maximum[[:space:]]+amplitude/ {print $3}' <<<"$out")
  freq=$(awk '/Rough[[:space:]]+frequency/ {print $3}' <<<"$out")
  printf "  %-9s RMS=%s  peak=%s  rough_freq=%s Hz\n" \
    "$label" "${rms:-n/a}" "${peak:-n/a}" "${freq:-n/a}"
}

need_cmd ffmpeg
need_cmd sox
need_cmd ssh
need_cmd scp

mkdir -p "$LOG_DIR"
MAC_CAPTURE="$LOG_DIR/pi_to_mac_capture.wav"
PI_CAPTURE="$LOG_DIR/mac_to_pi_capture.wav"
MAC_TONE="$LOG_DIR/mac_tone.wav"
FFMPEG_LOG="$LOG_DIR/ffmpeg.log"
PI_LOG="$LOG_DIR/pi_speaker_test.log"
PI_ARECORD_LOG="$LOG_DIR/pi_arecord.log"

echo "Logs: $LOG_DIR"
echo "Mac capture: avfoundation audio index $MAC_AV_AUDIO_INDEX"
echo "Mac playback: default output should be '$MAC_OUTPUT_DEVICE'  tone_volume=$MAC_TONE_VOLUME"
echo "Pi playback: $PI  device=$PI_PLAYBACK_DEV  freq=${FREQ}Hz"
echo "Pi capture:  $PI  device=$PI_CAPTURE_DEV"
echo
echo "=== Mac AVFoundation devices ==="
ffmpeg -hide_banner -f avfoundation -list_devices true -i "" 2>&1 \
  | sed -n '/AVFoundation audio devices:/,$p' || true
echo
echo "=== Pi ALSA playback devices ==="
ssh "${SSH_ARGS[@]}" "$PI" "aplay -l; arecord -l" || true
echo

if command -v SwitchAudioSource >/dev/null 2>&1; then
  SwitchAudioSource -s "$MAC_OUTPUT_DEVICE" -t output >/dev/null 2>&1 || true
fi

echo "[1/6] Recording Mac input while Pi plays..."
ffmpeg -y -hide_banner -f avfoundation -i ":$MAC_AV_AUDIO_INDEX" \
  -t "$DURATION" -ar 48000 "$MAC_CAPTURE" >"$FFMPEG_LOG" 2>&1 &
FFMPEG_PID=$!

sleep 1

echo "[2/6] Playing stereo tone from Pi..."
ssh "${SSH_ARGS[@]}" "$PI" \
  "speaker-test -D '$PI_PLAYBACK_DEV' -t sine -f '$FREQ' -c 2 -l 1" \
  >"$PI_LOG" 2>&1 || true

wait "$FFMPEG_PID" || {
  echo "ffmpeg capture failed; see $FFMPEG_LOG" >&2
  exit 1
}

echo "[3/6] Pi -> Mac capture stats..."
soxi "$MAC_CAPTURE"
echo
capture_stat "$MAC_CAPTURE" "combined"
capture_stat "$MAC_CAPTURE" "left" remix 1
capture_stat "$MAC_CAPTURE" "right" remix 2
echo

echo "[4/6] Recording Pi input..."
sox -n -r 48000 -c 2 "$MAC_TONE" synth "$((DURATION - 2))" sine "$FREQ" vol "$MAC_TONE_VOLUME"
ssh "${SSH_ARGS[@]}" "$PI" \
  "arecord -D '$PI_CAPTURE_DEV' -f S16_LE -r 48000 -c 2 -d '$DURATION' /tmp/ultra_mac_to_pi.wav" \
  >"$PI_ARECORD_LOG" 2>&1 &
ARECORD_PID=$!

sleep 1

echo "[5/6] Playing stereo tone from Mac..."
afplay "$MAC_TONE"

wait "$ARECORD_PID" || {
  echo "Pi arecord failed; see $PI_ARECORD_LOG" >&2
  exit 1
}

scp "${SSH_ARGS[@]}" "$PI:/tmp/ultra_mac_to_pi.wav" "$PI_CAPTURE" >/dev/null

echo "[6/6] Mac -> Pi capture stats..."
soxi "$PI_CAPTURE"
echo
capture_stat "$PI_CAPTURE" "combined"
capture_stat "$PI_CAPTURE" "left" remix 1
capture_stat "$PI_CAPTURE" "right" remix 2
echo

echo "Interpretation:"
echo "  Calibrated path target: RMS 0.05-0.25, peak 0.15-0.80, rough_freq near ${FREQ}Hz."
echo "  Too hot/clipping risk: peak > 0.90. Silent/bad path: RMS near 0.0003 or lower."
echo
echo "Captures: $MAC_CAPTURE  $PI_CAPTURE"
