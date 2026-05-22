# Coherent 8PSK Round 6 Findings (2026-05-22)

## Summary

Implemented a coherent 8PSK OFDM rung (`--expert --mod 8psk`) with absolute-phase
soft demapping, coherent mapping, QAM8/QAM16 decision-directed channel tracking,
and the same burst-interleaver policy treatment as coherent QAM16.

Result: the coherent 8PSK hypothesis is not proven for Good/SNR20. 8PSK R1/2 is
slightly more consistent than QAM16 on 5KB transfers, but it does not close the
20KB Good-fading gap. 8PSK R3/4, the raw-rate candidate, failed the 20KB seed43
run after max ARQ retries. For this channel and current ARQ/control path, QAM16
R1/2 remains the better 20KB rung.

Proof script: `/tmp/round6_proof_2026_05_22.sh`

## Implementation

- `src/ofdm/soft_demap.hpp`: added coherent `demap8PSK(Complex, float)` using
  the D8PSK Gray constellation, absolute phase, max-log-MAP LLRs, and the same
  LLR sign convention as the other coherent demappers.
- `src/ofdm/modulator.cpp`: added coherent 8PSK symbol mapping using the inverse
  D8PSK Gray phase table.
- `src/ofdm/ofdm_symbol_demap.cpp`: added QAM8/8PSK soft-demapping path.
- `src/ofdm/channel_equalizer_equalize.cpp` and
  `src/ofdm/channel_equalizer_pilot.cpp`: allowed the existing coherent DD
  channel tracker to consume QAM8 hard decisions as well as QAM16 decisions.
- `tools/sim/cli_enums.hpp`: added expert `--mod 8psk` / `qam8` / `8qam`.
- `src/replay/event_timeline.cpp`: keeps `D8PSK` differential and maps
  `8PSK`/`QAM8`/`8QAM` to coherent QAM8.
- `src/waveform/ofdm_chirp_waveform.cpp` and
  `src/waveform/ofdm_cox_waveform.cpp`: advertise and size QAM8 as a 3-bit
  coherent OFDM modulation.
- Tests cover 8PSK LLR signs, QAM8 OFDM symbol sizing, CLI parsing, and wide
  OFDM timing.

## Forced Good/SNR20 Results

All numbers are on-air goodput: DATA+ACK samples / 48 kHz.

### 5KB, seeds 42/43/44

| Mod/rate | seed42 | seed43 | seed44 | mean | spread | TX retx/timeouts |
|---|---:|---:|---:|---:|---:|---|
| QAM16 R1/2 reference | 2063 | 1542 | 1020 | 1542 | 1043 | 1/0, 10/10, 27/26 |
| 8PSK R1/2 | 1601 | 1688 | 1401 | 1563 | 287 | 2/0, 1/0, 6/6 |
| 8PSK R2/3 | 1651 | 963 | 1122 | 1245 | 688 | 7/5, 26/25, 18/16 |
| 8PSK R3/4 | 1233 | 1233 | 1353 | 1273 | 120 | 18/15, 16/12, 15/14 |

5KB conclusion: 8PSK R1/2 is more consistent than QAM16 and has a slightly
higher three-seed mean, but it is still far below the 2469 bps target. The
higher 8PSK rates lose their margin in fades.

### 20KB, seeds 42/43/44

| Mod/rate | seed42 | seed43 | seed44 | mean | spread | TX retx/timeouts |
|---|---:|---:|---:|---:|---:|---|
| QAM16 R1/2 reference | 2330 | 2008 | 1609 | 1982 | 721 | 4/1, 18/15, 41/36 |
| 8PSK R1/2 | 1472 | 1610 | 1466 | 1516 | 144 | 26/20, 18/12, 29/26 |
| 8PSK R2/3 | 1590 | 1988 | 1324 | 1634 | 664 | 39/33, 20/18, 61/54 |
| 8PSK R3/4 | 1762 | FAIL | 1638 | 1700 pass-only | 124 pass-only | 41/34, max-retry fail, 49/42 |

20KB conclusion: QAM16 R1/2 wins the three passing QAM16 seeds and remains the
best long-transfer rung. 8PSK R2/3 is the best 8PSK rate that passes all three
20KB seeds, but its mean is 348 bps below QAM16. 8PSK R3/4 is not acceptable:
seed43 hit max retries on seq 25-32 and was manually stopped after reproducing
the failure.

## Mechanism

8PSK is not failing because the new demapper is unwired: forced runs negotiate
`mod=8PSK`, the OFDM demapper emits 3 LLRs/carrier, and the QAM8 DD channel
observation path is active.

The failure is the same HF systems problem seen in Round 5: retx/timeouts and
fade-correlated frame failures dominate realized throughput. R1/2 8PSK has
lower raw payload rate and still pays substantial ARQ/control overhead on 20KB.
R3/4 has enough raw rate, but its receiver frame success falls to 48-68% in
the measured Good runs and one 20KB seed failed outright.

## Guards

- DQPSK guard: `./build/cli_simulator --channel good --snr 12 --file 1024 --seed 42`
  passed, negotiated DQPSK R1/4, 391 bps, 20 frames, 4 retx, 4 timeouts.
- QAM16 no-regression: seed42 remained at 2063 bps for 5KB and 2330 bps for
  20KB.
- 8PSK AWGN sanity: `--expert --mod 8psk --rate r1_2 --channel awgn --snr 16
  --file 20480 --seed 42` passed at 1624 bps with 15 retx and 10 timeouts.
- CTest: `ctest --test-dir build -j4 -E "OTASim.*Sweep|.*LongRun"
  --output-on-failure` passed 92/92 tests. `ctest -N` also reports 92 total
  configured tests in this build, so this was the full local test set.

## Recommendation

Keep coherent 8PSK as a forced lab rung, but do not promote it as the
Good/SNR20 production rung yet. For long Good-fading transfers, coherent QAM16
R1/2 still beats coherent 8PSK on this codebase. The next high-value work is
not another 8PSK threshold tweak; it is the ARQ/control-path reliability and
LLR/HARQ calibration work already identified in Round 5, because both QAM16
and 8PSK are still spending too much airtime on retries and timeouts.
