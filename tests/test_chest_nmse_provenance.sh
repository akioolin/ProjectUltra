#!/usr/bin/env bash
# Regression for the estimate-vs-truth diagnostic's CAPTURE PROVENANCE (2026-07-29).
#
# WHY THIS EXISTS. The fix in dbaf028 shipped with no test, so nothing prevented a
# regression of a bug that had already misled two rounds of analysis. The receiver runs
# a CONTROL-FIRST decode hypothesis (coherent QPSK R1/4, pilot spacing 5) before the
# data profile (spacing 8) -- streaming_ofdm_decode.cpp "control always rides coherent
# QPSK R1/4". A "first capture per frame" rule stores that speculative pass, capturing
#
#     H_captured = H_channel * X_spacing8 / X_spacing5
#
# a deterministic quotient of two near-unit-modulus training sequences. |H| looks clean
# while adjacent-carrier phases jump 50-140 deg, and the identity-channel NMSE reads
# +6.44 dB on a channel where decode is 20/20 PERFECT.
#
# WHAT IS ASSERTED
#   1. The identity control passes: on a channel collapsed to identity (truth H = 1 at
#      every carrier) the NMSE must be near the noise floor, NOT order 1.
#   2. The control-first pass is actually REJECTED, once per frame. Without this the
#      first assertion could pass for the wrong reason (e.g. the gate rejecting
#      everything, or the geometry never mismatching in the first place).
#   3. Decode is perfect, which is what makes assertion 1 sound: a receiver cannot
#      decode 16QAM R2/3 flawlessly with a bad channel estimate.
#   4. The fail-fast guard refuses a fading channel without frozen taps -- TEMPORAL
#      provenance, the second bug of the same family (truth taps are read after the
#      burst while the estimate is captured at the LTS).

set -uo pipefail

EXE="${1:?usage: $0 <path-to-measure_ack_fer>}"
N=10
FAIL=0

note() { printf '%s %s\n' "$1" "$2"; }
ok()   { note "[ ok ]" "$1"; }
bad()  { note "[FAIL]" "$1"; FAIL=1; }

echo "=== chest-nmse capture provenance ==="

# ---- 1-3: identity channel. Taps collapse to 1, multipath off => truth H == 1.
OUT="$(ULTRA_CHANNEL_DELAY_MS=0 ULTRA_CHANNEL_DOPPLER_HZ=0 "$EXE" \
        --snr 20 --config data4_full --channel good --mod qam16 --rate r2_3 \
        --seed 7 --n "$N" --chest-nmse 1 2>&1)" || true

NMSE=$(printf '%s\n' "$OUT" | grep -o 'mean_nmse=[0-9.e+-]*' | head -1 | cut -d= -f2)
REJ=$(printf '%s\n'  "$OUT" | grep -o 'rejected_profile_captures=[0-9]*' | head -1 | cut -d= -f2)
FRAMES=$(printf '%s\n' "$OUT" | grep -o ' frames=[0-9]*' | head -1 | cut -d= -f2)
PASSES=$(printf '%s\n' "$OUT" | tail -1 | awk -F, '{print $12}')

echo "   identity: nmse=${NMSE:-?} rejected=${REJ:-?} frames=${FRAMES:-?} decode=${PASSES:-?}/$N"

if [ -z "${NMSE:-}" ]; then
    bad "diagnostic produced no NMSE at all"
else
    # Measured 0.0202 after the fix; 4.401 before it. 0.20 is an order of magnitude
    # above the real value and two below the bug -- it cannot be straddled by noise.
    if awk "BEGIN{exit !($NMSE < 0.20)}"; then
        ok "identity-channel NMSE is near the noise floor (< 0.20)"
    else
        bad "identity-channel NMSE = $NMSE -- capture provenance regressed"
    fi
fi

if [ "${REJ:-0}" = "${FRAMES:-x}" ] && [ "${REJ:-0}" != "0" ]; then
    ok "control-first pass rejected exactly once per frame (rejected=$REJ frames=$FRAMES)"
else
    bad "expected one rejected control pass per frame, got rejected=${REJ:-?} frames=${FRAMES:-?}"
fi

if [ "${PASSES:-0}" = "$N" ]; then
    ok "decode is perfect on the identity channel (premise of the NMSE assertion)"
else
    bad "identity-channel decode is ${PASSES:-?}/$N -- the NMSE premise no longer holds"
fi

# ---- 4: temporal provenance guard.
if ULTRA_CHANNEL_DELAY_MS= "$EXE" --snr 20 --config data4_full --channel good \
        --mod qam16 --rate r2_3 --seed 7 --n 2 --chest-nmse 1 >/dev/null 2>&1; then
    bad "fading channel WITHOUT frozen taps was accepted -- temporal guard missing"
else
    ok "fading channel without ULTRA_CHANNEL_DOPPLER_HZ=0 is refused"
fi

if [ "$FAIL" = "0" ]; then
    echo "ALL PASS"
else
    echo "FAILURES"
fi
exit "$FAIL"
