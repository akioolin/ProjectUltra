#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

timestamp=$(date +%Y%m%d_%H%M%S)
REPORT_DIR=${AGENT_REPORT_DIR:-agents/reports/hardware_$timestamp}
LOCK_DIR=${AGENT_HW_LOCK_DIR:-/tmp/projectultra_hw.lock}
SSH_KEY=${SSH_KEY:-$HOME/.ssh/id_pi5}
INJECT_GAIN=${AGENT_HW_INJECT_GAIN:-0.70}
RUN_AUDIO_CHECK=${AGENT_HW_AUDIO_CHECK:-1}
RUN_LONG=${AGENT_HW_LONG:-0}

mkdir -p "$REPORT_DIR"

if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  echo "Hardware lock is held: $LOCK_DIR" >&2
  exit 75
fi
trap 'rmdir "$LOCK_DIR" 2>/dev/null || true' EXIT

run_step() {
  local name="$1"
  shift
  local log="$REPORT_DIR/$name.log"

  echo "==> $name"
  echo "+ $*" > "$log"
  if "$@" >> "$log" 2>&1; then
    echo "PASS $name"
  else
    local rc=$?
    echo "FAIL $name (log: $log)" >&2
    tail -120 "$log" >&2 || true
    exit "$rc"
  fi
}

if [[ "$RUN_AUDIO_CHECK" == "1" ]]; then
  run_step audio_path env SSH_KEY="$SSH_KEY" ./tools/check_hw_audio_path.sh
fi

run_step hw_awgn_1k_r12_snr15 \
  env SSH_KEY="$SSH_KEY" ./tools/run_hw_test.sh \
    --file 1024 --rate r1_2 --snr 15 --channel awgn --inject --inject-gain "$INJECT_GAIN"

run_step hw_good_1k_r12_snr15 \
  env SSH_KEY="$SSH_KEY" ./tools/run_hw_test.sh \
    --file 1024 --rate r1_2 --snr 15 --channel good --inject --inject-gain "$INJECT_GAIN"

run_step hw_moderate_1k_r12_snr15 \
  env SSH_KEY="$SSH_KEY" ./tools/run_hw_test.sh \
    --file 1024 --rate r1_2 --snr 15 --channel moderate --inject --inject-gain "$INJECT_GAIN"

if [[ "$RUN_LONG" == "1" ]]; then
  run_step hw_good_5k_r12_snr15 \
    env SSH_KEY="$SSH_KEY" ./tools/run_hw_test.sh \
      --file 5120 --rate r1_2 --snr 15 --channel good --inject --inject-gain "$INJECT_GAIN"
fi

cat > "$REPORT_DIR/summary.txt" <<EOF
hardware_smoke=pass
inject_gain=$INJECT_GAIN
audio_check=$RUN_AUDIO_CHECK
long=$RUN_LONG
timestamp=$timestamp
EOF

echo "Hardware smoke passed. Reports: $REPORT_DIR"
