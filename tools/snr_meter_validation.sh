#!/usr/bin/env bash
# Solo triple-verification of the modem's diagnostic SNR estimators.
#
# Runs the same channel scenarios three times across the channel-type axis
# (AWGN / Good / Moderate) and across the SNR axis (-5..+20 dB), with
# multi-seed averaging. Reports absolute bias and slope per channel for
# both the OFDM broadband and OFDM internal estimators, plus a PASS/FAIL
# verdict against ±1.5 dB bias and 0.8-1.2 slope tolerances.
#
# Does NOT touch operator-facing snr_db; this is a calibration probe
# against the documented, locked SimulatedChannel reference.
#
# Usage: tools/snr_meter_validation.sh [seeds]
#   seeds: number of seeds per cell (default 5)

set -euo pipefail

PROBE="${PROBE:-./build/ofdm_snr_probe}"
SEEDS="${1:-5}"
OUTDIR="${OUTDIR:-/tmp/snr_validation_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$OUTDIR"
CSV="${OUTDIR}/sweep.csv"

if [[ ! -x "$PROBE" ]]; then
    echo "ERROR: $PROBE not built. Run cmake --build build -j4 first."
    exit 2
fi

echo "=== SNR meter validation ==="
echo "Output: $OUTDIR"
echo "Seeds per cell: $SEEDS"
echo ""

# CSV header
echo "channel,configured_snr,seed,success,cw_ok,cw_failed,sync_quality_db,ofdm_broadband_snr_db,ofdm_internal_snr_db,fading_index" > "$CSV"

CHANNELS=(awgn good moderate)
SNRS=(-5 0 5 10 15 20)

for ch in "${CHANNELS[@]}"; do
    for snr in "${SNRS[@]}"; do
        for s in $(seq 1 "$SEEDS"); do
            # ofdm_snr_probe writes the row to stdout; we prepend channel & snr
            line=$("$PROBE" --no-header --snr "$snr" --channel "$ch" --rate r1_2 --seed "$s" 2>/dev/null || echo "ERR,,,,,,,,,")
            # Insert seed as the 3rd field (between configured_snr and success)
            # ofdm_snr_probe row format already has channel,snr,rate,success,...; we drop rate and add seed
            python3 - "$line" "$s" >> "$CSV" <<'PY'
import sys
row = sys.argv[1].strip().split(",")
seed = sys.argv[2]
if len(row) < 10 or row[0] == "ERR":
    print(",,,," + ",".join(["nan"] * 6))
else:
            # Original: channel,configured_snr,rate,success,cw_ok,cw_failed,sync_quality,ofdm_broadband,ofdm_internal,fading
            # Output:   channel,configured_snr,seed,success,cw_ok,cw_failed,sync_quality,ofdm_broadband,ofdm_internal,fading
    out = [row[0], row[1], seed, row[3], row[4], row[5], row[6], row[7], row[8], row[9]]
    print(",".join(out))
PY
        done
    done
done

echo "Sweep complete. Analyzing..."
echo ""

# Post-process per (channel, snr): compute mean and std of estimator deltas
python3 - "$CSV" "$OUTDIR/summary.txt" <<'PY'
import csv, sys, statistics

path = sys.argv[1]
summary_path = sys.argv[2]

rows = []
with open(path) as f:
    rdr = csv.DictReader(f)
    for r in rdr:
        try:
            r['configured_snr'] = float(r['configured_snr'])
            r['ofdm_broadband_snr_db'] = float(r['ofdm_broadband_snr_db'])
            r['ofdm_internal_snr_db'] = float(r['ofdm_internal_snr_db'])
            r['success'] = int(r['success'])
            rows.append(r)
        except (ValueError, KeyError):
            pass

# Group by (channel, configured_snr)
cells = {}
for r in rows:
    key = (r['channel'], r['configured_snr'])
    cells.setdefault(key, []).append(r)

# Per-channel slope + bias against configured SNR
chan_stats = {}
for ch in ['AWGN', 'GOOD', 'MODERATE']:
    pts = []
    for (c, snr), rs in sorted(cells.items()):
        if c != ch:
            continue
        ok = [r for r in rs if r['success'] == 1]
        if not ok:
            continue
        broadband = statistics.mean(r['ofdm_broadband_snr_db'] for r in ok)
        internal = statistics.mean(r['ofdm_internal_snr_db'] for r in ok)
        pts.append((snr, broadband, internal, len(ok)))
    chan_stats[ch] = pts

# Linear regression for slope + intercept (configured vs reported)
def slope_intercept(xs, ys):
    n = len(xs)
    if n < 2:
        return float('nan'), float('nan')
    mx = sum(xs) / n
    my = sum(ys) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    den = sum((x - mx) ** 2 for x in xs)
    if den == 0:
        return float('nan'), float('nan')
    slope = num / den
    intercept = my - slope * mx
    return slope, intercept

lines = []
lines.append("=== SNR Meter Validation Summary ===")
lines.append("")
lines.append(f"{'Channel':<10} {'Cfg SNR':>8} {'Pilot mean':>12} {'LTS mean':>12} {'n':>4}")
lines.append("-" * 50)
for ch, pts in chan_stats.items():
    for snr, p, l, n in pts:
        lines.append(f"{ch:<10} {snr:>8.0f} {p:>12.2f} {l:>12.2f} {n:>4d}")
    lines.append("")

lines.append("=== Slope (reported / configured) and absolute bias ===")
lines.append("")
lines.append(f"{'Channel':<10} {'Est':<6} {'Slope':>8} {'Bias@15':>9} {'Verdict':>20}")
lines.append("-" * 60)

verdicts = []
for ch, pts in chan_stats.items():
    if not pts:
        continue
    xs = [p[0] for p in pts]
    broadband_ys = [p[1] for p in pts]
    internal_ys = [p[2] for p in pts]
    sp, ip = slope_intercept(xs, broadband_ys)
    sl, il = slope_intercept(xs, internal_ys)
    # Bias at SNR=15 (typical floor reference)
    bias_broadband_15 = (sp * 15 + ip) - 15
    bias_internal_15 = (sl * 15 + il) - 15
    def verdict(slope, bias):
        slope_ok = 0.8 <= slope <= 1.2
        bias_ok = abs(bias) <= 1.5
        if slope_ok and bias_ok:
            return "PASS"
        bits = []
        if not slope_ok:
            bits.append("slope")
        if not bias_ok:
            bits.append("bias")
        return "FAIL(" + ",".join(bits) + ")"
    vp = verdict(sp, bias_broadband_15)
    vl = verdict(sl, bias_internal_15)
    verdicts.append((ch, 'broad', vp))
    verdicts.append((ch, 'intern', vl))
    lines.append(f"{ch:<10} {'broad':<6} {sp:>8.2f} {bias_broadband_15:>9.2f} {vp:>20}")
    lines.append(f"{ch:<10} {'intern':<6} {sl:>8.2f} {bias_internal_15:>9.2f} {vl:>20}")
    lines.append("")

lines.append("=== Final verdict ===")
broadband_pass = all(v[2] == "PASS" for v in verdicts if v[1] == 'broad')
internal_pass = all(v[2] == "PASS" for v in verdicts if v[1] == 'intern')
if broadband_pass:
    lines.append("PASS: OFDM broadband meter is honest across all channels (slope 0.8-1.2, |bias@15| < 1.5 dB).")
    if not internal_pass:
        lines.append("NOTE: OFDM internal remains a diagnostic sibling and is not calibrated enough for operator-facing substitution.")
else:
    lines.append("FAIL: at least one OFDM broadband-meter channel fails ±1.5 dB bias or 0.8-1.2 slope.")
    lines.append("The operator-facing meter must not be substituted until all broadband rows pass.")
    lines.append("A calibrated meter is the workstream described in docs/SNR_METER_DESIGN.md.")

summary_text = "\n".join(lines) + "\n"
print(summary_text)
with open(summary_path, "w") as f:
    f.write(summary_text)
PY

echo ""
echo "Raw CSV:  $CSV"
echo "Summary:  $OUTDIR/summary.txt"
