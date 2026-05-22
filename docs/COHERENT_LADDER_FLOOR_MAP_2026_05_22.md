# Coherent Ladder Floor Map (2026-05-22)

## Scope

Measurement-only round at commit `034c1c1` plus the sweep script added in this
commit. No PHY, decoder, ARQ, or waveform code was changed.

Primary channel is Good fading. Moderate was not run in this pass; the QPSK
Good-fading cells were slow enough to dominate runtime, and the requested Good
floor/crossover map was the priority.

All throughput numbers are on-air goodput: DATA+ACK samples / 48 kHz.

## Reproducers

Build and fast tests:

```sh
cmake --build build -j4
ctest --test-dir build -j4 -E "OTASim.*Sweep|.*LongRun" --output-on-failure
```

Main Good 5KB sweep. This intentionally stops at SNR14 because QPSK already
hard-fails at SNR17 and the remaining coherent rungs show sustained
data-frame failures by SNR14:

```sh
./tools/sweep_coherent_ladder.py \
  --out-dir /tmp/coherent_ladder_floor_map_2026_05_22/good_5kb \
  --channels good --file-size 5120 --seeds 42,43,44 \
  --snrs 20,17,14 \
  --cells qpsk:r1_2,8psk:r1_2,qam16:r1_2,8psk:r2_3,qam16:r2_3 \
  --jobs 3 --timeout-sec 360
```

20KB spot checks around the observed 16QAM/8PSK crossover:

```sh
./tools/sweep_coherent_ladder.py \
  --out-dir /tmp/coherent_ladder_floor_map_2026_05_22/good_20kb_spots \
  --channels good --file-size 20480 --seeds 42 \
  --snrs 17,14 \
  --cells 8psk:r1_2,8psk:r2_3,qam16:r1_2 \
  --jobs 3 --timeout-sec 720
```

QAM32 P3 sanity:

```sh
./tools/sweep_coherent_ladder.py \
  --out-dir /tmp/coherent_ladder_floor_map_2026_05_22/qam32_awgn_sanity \
  --channels awgn --file-size 5120 --seeds 42 --snrs 24 \
  --cells qam32:r1_2 --jobs 1 --timeout-sec 360
```

DQPSK production guard:

```sh
./build/cli_simulator --channel good --snr 12 --file 1024 --seed 42
```

## Good 5KB Aggregate

| Channel | Mod | Rate | SNR | Mean bps | Spread | Mean retx | Mean timeouts | BRAVO frame success | Failed seeds |
|---|---|---|---:|---:|---:|---:|---:|---|---:|
| good | qpsk | R1/2 | 20 | 460 | 15 | 39.3 | 37.7 | 100.0-100.0% | 0 |
| good | qpsk | R1/2 | 17 | fail | - | - | - | no transfer summary | 3 |
| good | qpsk | R1/2 | 14 | fail | - | - | - | no transfer summary | 3 |
| good | 8psk | R1/2 | 20 | 1563 | 287 | 3.0 | 2.0 | 91.7-100.0% | 0 |
| good | 8psk | R1/2 | 17 | 1403 | 250 | 5.0 | 2.7 | 75.0-95.5% | 0 |
| good | 8psk | R1/2 | 14 | 1162 | 505 | 12.0 | 10.0 | 73.3-91.7% | 0 |
| good | 8psk | R2/3 | 20 | 1245 | 688 | 17.0 | 15.3 | 63.3-74.3% | 0 |
| good | 8psk | R2/3 | 17 | 1305 | 690 | 13.7 | 12.0 | 47.1-88.9% | 0 |
| good | 8psk | R2/3 | 14 | 1084 | 299 | 19.7 | 17.7 | 54.8-73.1% | 0 |
| good | qam16 | R1/2 | 20 | 1516 | 1043 | 13.3 | 12.7 | 80.4-100.0% | 0 |
| good | qam16 | R1/2 | 17 | 1483 | 982 | 13.7 | 11.7 | 70.3-95.8% | 0 |
| good | qam16 | R1/2 | 14 | 1105 | 547 | 25.0 | 22.0 | 61.9-83.3% | 0 |
| good | qam16 | R2/3 | 20 | 878 pass-only | 37 | 39.0 | 35.0 | 37.3-38.1% | 1 |
| good | qam16 | R2/3 | 17 | 1134 | 552 | 29.3 | 25.3 | 29.8-55.2% | 0 |
| good | qam16 | R2/3 | 14 | 909 | 175 | 38.0 | 34.7 | 35.6-45.7% | 0 |

QPSK stop condition: at SNR17, seeds 42/43/44 logged 8/7/8 frames failing
after 15 retries. At SNR14, seeds 42/43/44 logged 8/8/8 frames failing after
15 retries. SNR11 and SNR8 were not run for QPSK because the lower-SNR ladder
had already collapsed.

SNR11 and SNR8 were also skipped for 8PSK/QAM16 after SNR14 showed sustained
data-frame failures on every measured coherent rung.

## Good 5KB Per-Seed Table

| Mod | Rate | SNR | Seed42 bps/retx/to/fail | Seed43 bps/retx/to/fail | Seed44 bps/retx/to/fail |
|---|---|---:|---|---|---|
| qpsk | R1/2 | 20 | 463 / 39 / 38 / 0 | 466 / 38 / 35 / 0 | 451 / 41 / 40 / 0 |
| qpsk | R1/2 | 17 | max-retry fail | max-retry fail | max-retry fail |
| qpsk | R1/2 | 14 | max-retry fail | max-retry fail | max-retry fail |
| 8psk | R1/2 | 20 | 1601 / 2 / 0 / 0 | 1688 / 1 / 0 / 0 | 1401 / 6 / 6 / 2 |
| 8psk | R1/2 | 17 | 1425 / 5 / 3 / 3 | 1267 / 8 / 5 / 7 | 1517 / 2 / 0 / 1 |
| 8psk | R1/2 | 14 | 1446 / 6 / 5 / 2 | 941 / 16 / 13 / 5 | 1099 / 14 / 12 / 8 |
| 8psk | R2/3 | 20 | 1651 / 7 / 5 / 7 | 963 / 26 / 25 / 9 | 1122 / 18 / 16 / 11 |
| 8psk | R2/3 | 17 | 1247 / 14 / 12 / 11 | 1679 / 5 / 4 / 2 | 989 / 22 / 20 / 18 |
| 8psk | R2/3 | 14 | 1241 / 14 / 12 / 7 | 942 / 25 / 23 / 14 | 1069 / 20 / 18 / 11 |
| qam16 | R1/2 | 20 | 2063 / 1 / 0 / 0 | 1464 / 12 / 12 / 5 | 1020 / 27 / 26 / 9 |
| qam16 | R1/2 | 17 | 1890 / 3 / 1 / 1 | 908 / 32 / 30 / 11 | 1650 / 6 / 4 / 1 |
| qam16 | R1/2 | 14 | 1080 / 22 / 19 / 14 | 844 / 36 / 32 / 16 | 1391 / 17 / 15 / 5 |
| qam16 | R2/3 | 20 | max-retry fail | 897 / 36 / 30 / 26 | 860 / 42 / 40 / 32 |
| qam16 | R2/3 | 17 | 1151 / 26 / 23 / 22 | 1401 / 17 / 14 / 13 | 849 / 45 / 39 / 40 |
| qam16 | R2/3 | 14 | 904 / 37 / 32 / 29 | 999 / 32 / 31 / 19 | 824 / 45 / 41 / 28 |

The per-seed tuple is `on-air bps / ALPHA retransmissions / ALPHA timeouts /
BRAVO frames_failed`.

## Good 20KB Spot Checks

| Channel | Mod | Rate | SNR | Seed | On-air bps | Retx | Timeouts | Frames failed | Frame success |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| good | 8psk | R1/2 | 17 | 42 | 1531 | 24 | 20 | 13 | 85.6% |
| good | 8psk | R1/2 | 14 | 42 | 1273 | 40 | 35 | 13 | 86.5% |
| good | 8psk | R2/3 | 17 | 42 | 1140 | 80 | 72 | 48 | 57.1% |
| good | 8psk | R2/3 | 14 | 42 | 1450 | 49 | 45 | 27 | 69.7% |
| good | qam16 | R1/2 | 17 | 42 | 1789 | 31 | 24 | 16 | 82.8% |
| good | qam16 | R1/2 | 14 | 42 | 1216 | 86 | 76 | 34 | 74.6% |

The 20KB spot checks agree with the 5KB trend that QAM16 remains a high-SNR,
high-ceiling rung, while 8PSK becomes the safer lower-SNR coherent rung. At
SNR17 seed42, QAM16 R1/2 still wins throughput. At SNR14 seed42, 8PSK wins;
R2/3 has the highest bps in that single spot but with worse frame success and
more control stress than 8PSK R1/2.

## QAM32 P3 Sanity

| Channel | Mod | Rate | SNR | File | Seed | On-air bps | Retx | Timeouts | Frames failed | Result |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| awgn | qam32 | R1/2 | 24 | 5120 | 42 | 1230 | 3 | 2 | 0 | decodes |

QAM32 is not a dead decoder path, but this single AWGN sanity run is not a
promotion argument. It only proves forced QAM32 can decode one 5KB AWGN cell.

## Crossover Read

### 16QAM to 8PSK

There is no clean monotonic 5KB crossover because QAM16 has a high ceiling and
large seed variance. The useful engineering boundary is:

- At Good/SNR20, 5KB mean favors 8PSK R1/2 by 47 bps (1563 vs 1516), but QAM16
  has the best individual seed (2063 bps) and the existing Round 6 20KB SNR20
  results still favor QAM16 R1/2 for longer transfers.
- At Good/SNR17, QAM16 R1/2 has the higher 5KB mean (1483 vs 1403), but the
  spread is 982 bps versus 250 bps for 8PSK R1/2. The 20KB seed42 spot also
  favors QAM16 R1/2 (1789 vs 1531).
- At Good/SNR14, 8PSK R1/2 beats QAM16 R1/2 in 5KB mean (1162 vs 1105), and
  the 20KB seed42 spot also favors 8PSK over QAM16 (8PSK R1/2 1273, 8PSK R2/3
  1450, QAM16 R1/2 1216).

So the measured Good-fading 16QAM-to-8PSK crossover is between 14 and 17 dB for
longer transfers. For a conservative adaptive selector, the practical switch
point should be around 17 dB unless recent frame/ACK evidence is clean enough
to keep QAM16.

### 8PSK to QPSK

No useful coherent 8PSK-to-QPSK crossover was observed. QPSK R1/2 had perfect
BRAVO data-frame success at SNR20, but only 460 bps mean because it burned
about 38 ACK/timeout retransmission turns per 5KB transfer. At SNR17 and SNR14
it failed all three seeds by max ARQ retries.

Under the current codebase, coherent QPSK is not yet a selectable lower rung
for Good fading. The robustness floor below coherent 8PSK should remain the
existing differential DQPSK production path until coherent QPSK's ARQ/control
behavior is fixed and remeasured.

## Recommended Good-Fading Selector Floor

Use this as an empirical starting point, with hysteresis and recent frame/ACK
statistics rather than an instantaneous SNR threshold:

| Good-fading condition | Recommended rung | Reason |
|---|---|---|
| SNR >= about 18 dB and recent retx/timeouts are low | coherent QAM16 R1/2 | Highest long-transfer ceiling; Round 6 20KB SNR20 still favors QAM16. |
| SNR 15-17 dB, or high seed variance / rising timeout count at higher SNR | coherent 8PSK R1/2 | Lower variance and fewer control failures than QAM16, even when QAM16's mean is slightly higher at SNR17. |
| SNR around 14 dB with coherent transfer still required | coherent 8PSK R1/2 | Best robust coherent default in the measured 5KB table; QAM16 frame failures and timeouts are too high. |
| Below about 14 dB, or sustained frame failures/timeouts on 8PSK | differential DQPSK floor | Coherent QPSK did not provide a viable lower rung in this code path. |

8PSK R2/3 is not the default Good-fading rung from this floor map. It can win a
single 20KB SNR14 seed, but its frame-success and timeout behavior are worse
than R1/2, and its three-seed 5KB mean is below 8PSK R1/2 at SNR20 and SNR14.

QAM16 R2/3 is not viable in this channel at these SNRs. It produced one
max-retry failure at SNR20 and low frame-success ranges across the measured
cells.

## Guards

- DQPSK guard: `./build/cli_simulator --channel good --snr 12 --file 1024
  --seed 42` passed, negotiated DQPSK R1/4, 391 bps, 20 frames, 4 retx,
  4 timeouts.
- Fast ctest tier: `ctest --test-dir build -j4 -E "OTASim.*Sweep|.*LongRun"
  --output-on-failure` passed 92/92 tests.
