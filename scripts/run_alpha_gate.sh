#!/usr/bin/env bash
set -euo pipefail

# Alpha release gate harness for cli_simulator.
# Runs deterministic seed matrices and writes:
# - per-run logs
# - machine-readable CSV metrics
# - markdown summary with gate pass/fail

usage() {
    cat <<'EOF'
Usage: scripts/run_alpha_gate.sh [options]

Options:
  --binary PATH         cli_simulator binary (default: ./build/cli_simulator)
  --seed-start N        First seed (default: 42)
  --seed-count N        Number of seeds (default: 30)
  --file-bytes N        File test size in bytes (default: 2048)
  --timeout-sec N       Per-seed timeout in seconds (default: 180)
  --out-dir DIR         Output directory (default: /tmp/alpha_gate_<timestamp>)
  --quick               Shortcut for --seed-count 5
  --help                Show this message

Example:
  scripts/run_alpha_gate.sh --seed-start 42 --seed-count 30
EOF
}

BINARY="./build/cli_simulator"
SEED_START=42
SEED_COUNT=30
FILE_BYTES=2048
TIMEOUT_SEC=180
OUT_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary)
            BINARY="$2"
            shift 2
            ;;
        --seed-start)
            SEED_START="$2"
            shift 2
            ;;
        --seed-count)
            SEED_COUNT="$2"
            shift 2
            ;;
        --file-bytes)
            FILE_BYTES="$2"
            shift 2
            ;;
        --timeout-sec)
            TIMEOUT_SEC="$2"
            shift 2
            ;;
        --out-dir)
            OUT_DIR="$2"
            shift 2
            ;;
        --quick)
            SEED_COUNT=5
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ ! -x "$BINARY" ]]; then
    echo "Binary not found or not executable: $BINARY" >&2
    exit 1
fi

if ! command -v timeout >/dev/null 2>&1; then
    echo "'timeout' command is required" >&2
    exit 1
fi

if [[ -z "$OUT_DIR" ]]; then
    OUT_DIR="/tmp/alpha_gate_$(date +%Y%m%d_%H%M%S)"
fi

mkdir -p "$OUT_DIR/logs"
SUMMARY_MD="$OUT_DIR/summary.md"
RESULTS_CSV="$OUT_DIR/results.csv"

echo "gate,seed,rc,delivery_ok,disconnect_ok,frames_sent,retransmissions,timeouts,first_attempt_pct,retrans_timeout,retrans_fast_hole,retrans_hole_probe,retrans_nack,hole_events,dup_acks_ignored,repeat_coalesced,repeat_dropped,log_path" > "$RESULTS_CSV"

extract_alpha_key() {
    local log="$1"
    local key="$2"
    awk -v key="$key" '
        /--- ALPHA \(TX\) ---/ { in_alpha=1; next }
        /--- BRAVO \(RX\) ---/ { in_alpha=0 }
        in_alpha {
            for (i = 1; i <= NF; i++) {
                if (index($i, key "=") == 1) {
                    split($i, a, "=")
                    gsub(/[^0-9.]/, "", a[2])
                    print a[2]
                    exit
                }
            }
        }
    ' "$log"
}

mean_from_array() {
    if [[ $# -eq 0 ]]; then
        echo "0"
        return
    fi
    printf '%s\n' "$@" | awk '{s+=$1; n++} END {if (n==0) print "0"; else printf "%.2f", s/n}'
}

p90_from_array() {
    if [[ $# -eq 0 ]]; then
        echo "0"
        return
    fi
    printf '%s\n' "$@" | sort -n | awk '
        { vals[++n] = $1 }
        END {
            if (n == 0) { print "0"; exit }
            idx = int((9*n + 9) / 10)   # ceil(0.9*n)
            if (idx < 1) idx = 1
            if (idx > n) idx = n
            print vals[idx]
        }'
}

append_gate_summary() {
    local gate_id="$1"
    local title="$2"
    local mode="$3"
    local min_delivery="$4"
    local min_disc_pct="$5"
    local max_avg_retx="$6"
    local min_avg_first="$7"

    local gate_csv="$OUT_DIR/${gate_id}.csv"
    local seeds_total
    seeds_total=$(awk -F, 'NR>1{c++} END{print c+0}' "$gate_csv")
    local run_success
    run_success=$(awk -F, 'NR>1 && $3==0{c++} END{print c+0}' "$gate_csv")
    local delivery_ok
    delivery_ok=$(awk -F, 'NR>1 && $4==1{c++} END{print c+0}' "$gate_csv")
    local disconnect_ok
    disconnect_ok=$(awk -F, 'NR>1 && $5==1{c++} END{print c+0}' "$gate_csv")
    local disconnect_pct="0.00"
    if [[ "$seeds_total" -gt 0 ]]; then
        disconnect_pct=$(awk -v ok="$disconnect_ok" -v n="$seeds_total" 'BEGIN { printf "%.2f", 100.0*ok/n }')
    fi

    mapfile -t retx_vals < <(awk -F, 'NR>1 && $3==0 {print $7+0}' "$gate_csv")
    mapfile -t timeout_vals < <(awk -F, 'NR>1 && $3==0 {print $8+0}' "$gate_csv")
    mapfile -t first_vals < <(awk -F, 'NR>1 && $3==0 {print $9+0}' "$gate_csv")
    mapfile -t hole_vals < <(awk -F, 'NR>1 && $3==0 {print $14+0}' "$gate_csv")

    local avg_retx avg_timeout avg_first p90_retx p90_timeout max_retx max_timeout avg_holes
    avg_retx=$(mean_from_array "${retx_vals[@]}")
    avg_timeout=$(mean_from_array "${timeout_vals[@]}")
    avg_first=$(mean_from_array "${first_vals[@]}")
    p90_retx=$(p90_from_array "${retx_vals[@]}")
    p90_timeout=$(p90_from_array "${timeout_vals[@]}")
    max_retx=$(printf '%s\n' "${retx_vals[@]:-0}" | sort -n | tail -n1)
    max_timeout=$(printf '%s\n' "${timeout_vals[@]:-0}" | sort -n | tail -n1)
    avg_holes=$(mean_from_array "${hole_vals[@]}")

    local required_delivery="$min_delivery"
    if (( required_delivery > seeds_total )); then
        required_delivery="$seeds_total"
    fi

    local pass=true
    if (( delivery_ok < required_delivery )); then
        pass=false
    fi
    if awk -v v="$disconnect_pct" -v t="$min_disc_pct" 'BEGIN{exit !(v+0 < t+0)}'; then
        pass=false
    fi
    if [[ -n "$max_avg_retx" ]]; then
        if awk -v v="$avg_retx" -v t="$max_avg_retx" 'BEGIN{exit !(v+0 > t+0)}'; then
            pass=false
        fi
    fi
    if [[ -n "$min_avg_first" ]]; then
        if awk -v v="$avg_first" -v t="$min_avg_first" 'BEGIN{exit !(v+0 < t+0)}'; then
            pass=false
        fi
    fi

    {
        echo "### ${title}"
        echo ""
        echo "- Gate ID: \`${gate_id}\`"
        echo "- Mode: \`${mode}\`"
        echo "- Result: **$( [[ "$pass" == true ]] && echo PASS || echo FAIL )**"
        echo "- Runs: ${run_success}/${seeds_total} process-success, delivery ${delivery_ok}/${seeds_total} (required ${required_delivery}), disconnect ${disconnect_ok}/${seeds_total} (${disconnect_pct}%)"
        echo "- Retransmissions: avg ${avg_retx}, p90 ${p90_retx}, max ${max_retx}"
        echo "- Timeouts: avg ${avg_timeout}, p90 ${p90_timeout}, max ${max_timeout}"
        echo "- First-attempt success (derived): avg ${avg_first}%"
        echo "- Hole events (ALPHA): avg ${avg_holes}"
        echo "- Logs: \`$OUT_DIR/logs/${gate_id}\`"
        echo ""
    } >> "$SUMMARY_MD"
}

run_gate() {
    local gate_id="$1"
    local title="$2"
    local mode="$3"            # message|file
    local min_delivery="$4"
    local min_disc_pct="$5"
    local max_avg_retx="$6"    # optional empty
    local min_avg_first="$7"   # optional empty
    shift 7
    local -a base_cmd=("$@")

    local gate_log_dir="$OUT_DIR/logs/$gate_id"
    local gate_csv="$OUT_DIR/${gate_id}.csv"
    mkdir -p "$gate_log_dir"
    echo "gate,seed,rc,delivery_ok,disconnect_ok,frames_sent,retransmissions,timeouts,first_attempt_pct,retrans_timeout,retrans_fast_hole,retrans_hole_probe,retrans_nack,hole_events,dup_acks_ignored,repeat_coalesced,repeat_dropped,log_path" > "$gate_csv"

    local end_seed=$((SEED_START + SEED_COUNT - 1))
    echo "[gate:$gate_id] $title"
    echo "  seeds: $SEED_START..$end_seed"

    local seed
    for ((seed = SEED_START; seed <= end_seed; seed++)); do
        local log="$gate_log_dir/seed_${seed}.log"
        local -a cmd=("$BINARY" "${base_cmd[@]}" --seed "$seed")
        if [[ "$mode" == "file" ]]; then
            cmd+=(--file "$FILE_BYTES")
        fi

        local rc=0
        timeout "$TIMEOUT_SEC" "${cmd[@]}" >"$log" 2>&1 || rc=$?

        local delivery_ok=0
        if [[ "$mode" == "file" ]]; then
            if grep -q "File contents verified!" "$log"; then
                delivery_ok=1
            fi
        else
            if grep -q "All 7 messages transferred successfully!" "$log"; then
                delivery_ok=1
            fi
        fi

        local disconnect_ok=0
        if grep -q "Disconnect timeout (non-fatal)" "$log"; then
            disconnect_ok=0
        elif grep -q "Disconnected!" "$log"; then
            disconnect_ok=1
        fi

        local frames_sent retrans timeouts
        local retx_timeout retx_fast_hole retx_hole_probe retx_nack
        local hole_events dup_acks repeat_coalesced repeat_dropped
        frames_sent="$(extract_alpha_key "$log" "frames_sent")"
        retrans="$(extract_alpha_key "$log" "retransmissions")"
        timeouts="$(extract_alpha_key "$log" "timeouts")"
        retx_timeout="$(extract_alpha_key "$log" "timeout")"
        retx_fast_hole="$(extract_alpha_key "$log" "fast_hole")"
        retx_hole_probe="$(extract_alpha_key "$log" "hole_probe")"
        retx_nack="$(extract_alpha_key "$log" "nack")"
        hole_events="$(extract_alpha_key "$log" "hole_events")"
        dup_acks="$(extract_alpha_key "$log" "dup_ignored")"
        repeat_coalesced="$(extract_alpha_key "$log" "repeat_coalesced")"
        repeat_dropped="$(extract_alpha_key "$log" "repeat_dropped")"

        frames_sent="${frames_sent:-0}"
        retrans="${retrans:-0}"
        timeouts="${timeouts:-0}"
        retx_timeout="${retx_timeout:-0}"
        retx_fast_hole="${retx_fast_hole:-0}"
        retx_hole_probe="${retx_hole_probe:-0}"
        retx_nack="${retx_nack:-0}"
        hole_events="${hole_events:-0}"
        dup_acks="${dup_acks:-0}"
        repeat_coalesced="${repeat_coalesced:-0}"
        repeat_dropped="${repeat_dropped:-0}"

        local first_attempt_pct
        first_attempt_pct=$(awk -v s="$frames_sent" -v r="$retrans" 'BEGIN {
            total = s + r
            if (total <= 0) printf "0.00";
            else printf "%.2f", 100.0 * s / total;
        }')

        echo "$gate_id,$seed,$rc,$delivery_ok,$disconnect_ok,$frames_sent,$retrans,$timeouts,$first_attempt_pct,$retx_timeout,$retx_fast_hole,$retx_hole_probe,$retx_nack,$hole_events,$dup_acks,$repeat_coalesced,$repeat_dropped,$log" >> "$gate_csv"
        echo "$gate_id,$seed,$rc,$delivery_ok,$disconnect_ok,$frames_sent,$retrans,$timeouts,$first_attempt_pct,$retx_timeout,$retx_fast_hole,$retx_hole_probe,$retx_nack,$hole_events,$dup_acks,$repeat_coalesced,$repeat_dropped,$log" >> "$RESULTS_CSV"
    done

    append_gate_summary "$gate_id" "$title" "$mode" "$min_delivery" "$min_disc_pct" "$max_avg_retx" "$min_avg_first"
}

{
    echo "# Alpha Gate Report"
    echo ""
    echo "- Generated: $(date -u +"%Y-%m-%d %H:%M:%S UTC")"
    echo "- Binary: \`$BINARY\`"
    echo "- Seed range: \`$SEED_START..$((SEED_START + SEED_COUNT - 1))\` (${SEED_COUNT} seeds)"
    echo "- File size: \`${FILE_BYTES}\` bytes"
    echo "- Timeout: \`${TIMEOUT_SEC}\` s per seed"
    echo "- Output dir: \`$OUT_DIR\`"
    echo ""
} > "$SUMMARY_MD"

COMMON_WF=(-w ofdm_chirp)

run_gate "g1_r14_good" "G1 Regression: DQPSK R1/4, SNR 10, good fading (message)" \
    "message" 30 95 "" "" \
    --snr 10 --fading good --mod dqpsk --rate r1_4 "${COMMON_WF[@]}"

run_gate "g2_r14_moderate" "G2 Regression: DQPSK R1/4, SNR 10, moderate fading (message)" \
    "message" 29 95 "" "" \
    --snr 10 --fading moderate --mod dqpsk --rate r1_4 "${COMMON_WF[@]}"

run_gate "g3_r12_good" "G3 Regression: DQPSK R1/2, SNR 20, good fading (message)" \
    "message" 30 95 "" "" \
    --snr 20 --fading good --mod dqpsk --rate r1_2 "${COMMON_WF[@]}"

run_gate "g4_r23_good_msg" "G4 Throughput: DQPSK R2/3, SNR 20, good fading (message)" \
    "message" 30 95 2.5 90 \
    --snr 20 --fading good --mod dqpsk --rate r2_3 "${COMMON_WF[@]}"

run_gate "g5_r23_good_file" "G5 Throughput: DQPSK R2/3, SNR 20, good fading (file ${FILE_BYTES}B)" \
    "file" 30 95 4 "" \
    --snr 20 --fading good --mod dqpsk --rate r2_3 "${COMMON_WF[@]}"

echo "## Overall" >> "$SUMMARY_MD"
echo "" >> "$SUMMARY_MD"
overall_fail_count=$(grep -c "Result: \\*\\*FAIL\\*\\*" "$SUMMARY_MD" || true)
if [[ "$overall_fail_count" -eq 0 ]]; then
    echo "- Alpha gate status: **PASS**" >> "$SUMMARY_MD"
else
    echo "- Alpha gate status: **FAIL** (${overall_fail_count} failing gate(s))" >> "$SUMMARY_MD"
fi
echo "- Full CSV: \`$RESULTS_CSV\`" >> "$SUMMARY_MD"
echo "" >> "$SUMMARY_MD"

echo "Alpha gate run complete."
echo "Summary: $SUMMARY_MD"
echo "CSV:     $RESULTS_CSV"
