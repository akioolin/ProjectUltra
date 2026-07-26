#!/bin/bash
# Measure per-rung FER on a FADING channel (ITU-R F.1487 Good = the IONOS MPG setting),
# which is what the rate-ladder anchor table is supposed to encode.
#
# WHY THIS EXISTS. Several anchors in kCoherentLadder* were set from AWGN or from a single
# zero-margin measured floor. On 2026-07-26 a sweep showed 16QAM R2/3's Good anchor of 20.0 dB
# sits at 51.4% FER on actual fading -- i.e. the ladder confidently selects a rung that fails
# half its frames, craters, demotes, and reaches again. That is the churn the crater machinery
# spends every transfer correcting. Anchors must be set from FADING FER, not AWGN.
#
# CONVENTION: floor = lowest SNR with FER <= 10%; the QPSK rungs use floor + 2 dB as the anchor.
# Do NOT use a zero-margin anchor (the experimental QAM16 entries did, and the table itself
# flags them as needing "margin parity before default").
#
# Usage: tools/sweep_fading_anchors.sh   (writes /tmp/fadesweep.csv; ~10 min)
# 8PSK R3/4 fading validation: the rung was anchored on TRUE AWGN only and auto-disabled
# 2026-07-06 because a calm stretch on a Watterson bench unlocks it and it craters when
# fading returns. Sweep it on ITU Good (= MPG) against its enabled neighbours.
cd /Users/mathieuvachon/Projects/ProjectUltra
OUT=/tmp/fadesweep.csv; : > $OUT
for mod_rate in "qam8 r3_4" "qam8 r2_3" "qam16 r2_3" "qpsk r3_4"; do
  set -- $mod_rate; M=$1; R=$2
  for snr in 16 18 20 22 24; do
    for seed in 11 23 37 53 71 89; do
      ./build/measure_ack_fer --snr $snr --config data4_full --seed $seed --n 24 \
        --channel good --mod $M --rate $R 2>/dev/null | tail -1 >> $OUT
    done
  done
done
echo SWEEP_DONE
