#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT}/build/measure_ack_fer"
OUT="${ROOT}/docs/data/ack_fer_baseline_2026_05_20.csv"
DETAIL="${ROOT}/docs/data/ack_fer_baseline_2026_05_20_by_seed.csv"

SNRS=(8 10 12 14 16 18 20 24)
CONFIGS=(ack_light data4_light ack_full)
SEEDS=(2026052001 2026052002 2026052003)
N_PER_SEED="${N_PER_SEED:-200}"

if [[ ! -x "${BIN}" ]]; then
    echo "measure_ack_fer binary not found: ${BIN}" >&2
    echo "Run: cmake --build build -j4" >&2
    exit 1
fi

mkdir -p "${ROOT}/docs/data"

printf "snr,config,seed,n,sync_fail,decode_fail,crc_fail,pass\n" > "${DETAIL}"
printf "snr,config,seed,n,sync_fail,decode_fail,crc_fail,pass\n" > "${OUT}"

seed_list="$(IFS=';'; echo "${SEEDS[*]}")"

for snr in "${SNRS[@]}"; do
    for config in "${CONFIGS[@]}"; do
        total_n=0
        total_sync_fail=0
        total_decode_fail=0
        total_crc_fail=0
        total_pass=0

        for seed in "${SEEDS[@]}"; do
            echo "measuring snr=${snr} config=${config} seed=${seed} n=${N_PER_SEED}" >&2
            line="$("${BIN}" --snr "${snr}" --config "${config}" --seed "${seed}" --n "${N_PER_SEED}")"
            printf "%s\n" "${line}" >> "${DETAIL}"

            IFS=, read -r got_snr got_config got_seed got_n got_sync_fail got_decode_fail got_crc_fail got_pass <<< "${line}"
            if [[ "${got_snr}" != "${snr}" || "${got_config}" != "${config}" || "${got_seed}" != "${seed}" ]]; then
                echo "unexpected harness output: ${line}" >&2
                exit 1
            fi

            total_n=$((total_n + got_n))
            total_sync_fail=$((total_sync_fail + got_sync_fail))
            total_decode_fail=$((total_decode_fail + got_decode_fail))
            total_crc_fail=$((total_crc_fail + got_crc_fail))
            total_pass=$((total_pass + got_pass))
        done

        printf "%s,%s,\"%s\",%d,%d,%d,%d,%d\n" \
            "${snr}" "${config}" "${seed_list}" "${total_n}" \
            "${total_sync_fail}" "${total_decode_fail}" \
            "${total_crc_fail}" "${total_pass}" >> "${OUT}"
    done
done

awk -F, '
    NR > 1 && $1 == 12 && $2 == "data4_light" {
        pass_rate = $8 / $4
        if (pass_rate < 0.95) {
            printf "WARNING: SNR=12 data4_light sanity pass rate %.2f%% is below 95%%; measurement baseline is invalid per task acceptance.\n", pass_rate * 100 > "/dev/stderr"
        }
    }
' "${OUT}"

echo "wrote ${OUT}" >&2
echo "wrote ${DETAIL}" >&2
