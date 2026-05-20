# ACK Frame FER Baseline - 2026-05-20

## Status

This is a measurement-only result from branch `meas/ack-fer-baseline-2026-05-20`.
The requested acceptance sanity check did not pass: the 4-CW data light-preamble
baseline at SNR=12 decoded 0/600 frames, not >=95%. The raw data is preserved,
but this run is not an accepted baseline under the task criteria.

Raw data:

- Aggregate 24-row CSV: `docs/data/ack_fer_baseline_2026_05_20.csv`
- Per-seed detail CSV: `docs/data/ack_fer_baseline_2026_05_20_by_seed.csv`

## Method

Harness: `tools/measure_ack_fer.cpp`, driven by
`tools/measure_ack_fer.sh`.

Each cell uses `StreamingEncoder` and `StreamingDecoder` in `OFDM_CHIRP`,
DQPSK R1/4, 59-carrier NVIS geometry, and `SimulatedChannel` AWGN. The AWGN
SNR is the project in-band convention from `SimulatedChannel`: reference
in-band RMS `0.30482664` over the 50-2950 Hz receive band.

Frame content:

- `ack_light`: actual v2 ACK `ControlFrame` from `ControlFrame::makeAck`,
  serialized to the production 20-byte control frame and encoded with
  `StreamingEncoder::encodeFrameLight`.
- `ack_full`: the same actual ACK bytes, encoded with
  `StreamingEncoder::encodeFrame`.
- `data4_light`: v2 fixed 4-CW `DataFrame` from `makeFixedDataFrame`, carrying
  a random 20-byte payload, encoded with `StreamingEncoder::encodeFrameLight`.

Seeds: `2026052001`, `2026052002`, `2026052003`.
Each aggregate row is `n=600` from 3 seeds x 200 frames.

Failure classification:

- `sync_fail`: no accepted `StreamingDecoder` sync for the isolated frame.
- `decode_fail`: sync was acquired, but no full matching frame was delivered.
- `crc_fail`: decoder delivered a frame that did not match the input bytes, or
  reported codeword success without a full matching frame.
- `pass`: delivered bytes exactly matched the transmitted serialized frame.

## FER Table

FER is `(sync_fail + decode_fail + crc_fail) / n`. Parentheses are Wilson 95%
confidence intervals for FER.

| SNR dB | ACK light | 4-CW data light | ACK full |
|---:|---:|---:|---:|
| 8 | 100.0% (99.4-100.0) | 100.0% (99.4-100.0) | 67.5% (63.7-71.1) |
| 10 | 100.0% (99.4-100.0) | 100.0% (99.4-100.0) | 3.8% (2.6-5.7) |
| 12 | 100.0% (99.4-100.0) | 100.0% (99.4-100.0) | 0.3% (0.1-1.2) |
| 14 | 28.8% (25.4-32.6) | 31.0% (27.4-34.8) | 0.0% (0.0-0.6) |
| 16 | 0.0% (0.0-0.6) | 0.7% (0.3-1.7) | 0.0% (0.0-0.6) |
| 18 | 0.0% (0.0-0.6) | 0.0% (0.0-0.6) | 0.0% (0.0-0.6) |
| 20 | 0.0% (0.0-0.6) | 0.0% (0.0-0.6) | 0.0% (0.0-0.6) |
| 24 | 0.0% (0.0-0.6) | 0.0% (0.0-0.6) | 0.0% (0.0-0.6) |

## Decomposition

| SNR dB | Config | Sync Fail | Decode Fail | CRC Fail | Pass |
|---:|---|---:|---:|---:|---:|
| 8 | ack_light | 600 (100.0%) | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) |
| 8 | data4_light | 600 (100.0%) | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) |
| 8 | ack_full | 0 (0.0%) | 391 (65.2%) | 14 (2.3%) | 195 (32.5%) |
| 10 | ack_light | 600 (100.0%) | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) |
| 10 | data4_light | 600 (100.0%) | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) |
| 10 | ack_full | 0 (0.0%) | 22 (3.7%) | 1 (0.2%) | 577 (96.2%) |
| 12 | ack_light | 600 (100.0%) | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) |
| 12 | data4_light | 600 (100.0%) | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) |
| 12 | ack_full | 0 (0.0%) | 2 (0.3%) | 0 (0.0%) | 598 (99.7%) |
| 14 | ack_light | 168 (28.0%) | 5 (0.8%) | 0 (0.0%) | 427 (71.2%) |
| 14 | data4_light | 167 (27.8%) | 19 (3.2%) | 0 (0.0%) | 414 (69.0%) |
| 14 | ack_full | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |
| 16 | ack_light | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |
| 16 | data4_light | 0 (0.0%) | 4 (0.7%) | 0 (0.0%) | 596 (99.3%) |
| 16 | ack_full | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |
| 18 | ack_light | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |
| 18 | data4_light | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |
| 18 | ack_full | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |
| 20 | ack_light | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |
| 20 | data4_light | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |
| 20 | ack_full | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |
| 24 | ack_light | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |
| 24 | data4_light | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |
| 24 | ack_full | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |

## Interpretation

PHY theorist: the measured cliff is dominated by the light-preamble sync regime,
not by the 1-CW LDPC payload itself. At 8, 10, and 12 dB, both light-preamble
ACK and light-preamble 4-CW data are 100% sync failures. The full-preamble
ACK reference is already 96.2% pass at 10 dB and 99.7% pass at 12 dB, so the
data does not isolate a clean 1-CW versus 4-CW coding disadvantage below 14 dB
in this run.

Real-time DSP systems engineer: the measurement uses production
`StreamingEncoder` and `StreamingDecoder` paths and therefore exposes the
production light-sync acceptance behavior. The requested SNR=12 4-CW data
sanity check failed, and an independent canary command,
`./build/test_waveform_simple --snr 12 --channel awgn -w ofdm_chirp --rate r1_4 --mod dqpsk --frames 3 --session --seed 2026052001 -q`,
also decoded 0/3 and logged LTS correlations around 0.20-0.34 below the
production 0.52 connected light-sync threshold. That makes this a real
production-path observation, but not an accepted calibration baseline.

Veteran HF operator: the low-SNR light-preamble failures look like silence,
not payload CRC errors. For `ack_light` and `data4_light`, all failures at
8, 10, and 12 dB are classified as `sync_fail`; at 14 dB most remaining
failures are still sync failures. Full-preamble ACK failures at 8 and 10 dB
mostly move into `decode_fail`, which is a different operator-visible failure
mode.

First-principles escape hatch: the result is materially sensitive to the
production LTS acceptance threshold, not to a new harness threshold. The harness
does not override or tune that threshold. No alternate-threshold sweep was run
because this task is measurement-only and forbids modem changes; the reported
tables are the production-threshold baseline from this checkout.

## Reproduce

This worktree did not contain a usable default GUI build dependency set, so the
build was configured with GUI disabled:

```bash
cmake -S . -B build -DULTRA_BUILD_GUI=OFF
cmake --build build -j4
tools/measure_ack_fer.sh
```

The driver writes the aggregate and by-seed CSV files listed above. The default
driver seeds are `2026052001 2026052002 2026052003`, and the default sample
count is `N_PER_SEED=200`.
