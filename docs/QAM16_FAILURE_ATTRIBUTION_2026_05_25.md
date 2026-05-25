# QAM16 Good@20 Failure Attribution - 2026-05-25

This is a diagnosis-only snapshot for forced inner-loop cells. It does not
claim a GUI ladder pass and it does not apply a shipping-path fix.

## Mandatory Stack

1. PHY theorist (primary) — error-vector/EVM decomposition, LLR calibration under a documented noise model, per-carrier CSI, CFO/timing/phase error signatures, information-theoretic limits.
2. Real-time DSP systems engineer (mandatory) — instrumentation must be test/diag-gated, not in the shipping hot path; numerics; reproducibility.
3. Veteran HF operator (mandatory) — does the attributed cause match real ionospheric behavior.
4. First-principles escape hatch.

## Scope

Target cell:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed <seed> --file 10240
```

The diagnostics are gated by explicit CLI flags that set diagnostic-only
environment variables:

- `--diag-attribution`: log BRAVO per-frame attribution data.
- `--diag-genie-sigma`: use empirical post-equalization residual variance as the QAM16 demapper sigma^2.
- `--diag-genie-channel`: replace the equalizer channel estimate with a diagnostic two-path LS fit.
- `--diag-genie-timing`: force the BRAVO connected-data OFDM demodulator to the expected FFT-window arrival and zero residual CFO.
- `--diag-genie-no-clip`: disable/raise TX PAPR limiting so the forced cell runs with no clipping intervention.

The current branch is `feat/good-fading-qam16-ladder-2026-05-24`, and the
foundation commit is `47ca0c2`.

## Important Caveats

The `--diag-genie-channel` result below is not true simulator-plumbed perfect H.
OTASim's exact per-subcarrier channel truth is not currently delivered to the
BRAVO receiver path. The measured run uses a two-path LS approximation with a
fixed 24-sample delay basis. It regressed badly and is invalid as a perfect-H
answer. Exact channel truth remains UNTESTED and not exonerated.

The sigma genie is also a diagnostic approximation: it uses measured
post-equalization residual variance against the nearest QAM16 constellation
point, not raw simulator noise variance before the channel/equalizer. It is
still a direct test of the suspected LLR sigma^2 mis-scaling mechanism.

The timing genie is not a shipping fix. It uses the simulator/test harness'
known expected connected-data frame arrival to override the light-LTS timing
decision and zero the residual CFO path. It is valid as an attribution oracle
for the light-sync timing hypothesis.

The no-clip genie is valid for this CLI OTASim path: PAPR reduction is off by
default for coherent OFDM in this harness, and the no-clip flag explicitly keeps
it off. The unchanged result rules out clipping/PAPR limiting as the cause for
these forced cells.

## Reproducer Commands

Baseline diagnostics:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 2 --file 10240 --diag-attribution --log-level warn --log-file /tmp/qam16_seed2_baseline_diag.log
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 3 --file 10240 --diag-attribution --log-level warn --log-file /tmp/qam16_seed3_baseline_diag.log
```

Empirical sigma^2 genie:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 2 --file 10240 --diag-genie-sigma --log-level warn --log-file /tmp/qam16_seed2_genie_sigma.log
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 3 --file 10240 --diag-genie-sigma --log-level warn --log-file /tmp/qam16_seed3_genie_sigma.log
```

Two-path LS channel approximation:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 2 --file 10240 --diag-genie-channel --log-level warn --log-file /tmp/qam16_seed2_genie_channel.log
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 3 --file 10240 --diag-genie-channel --log-level warn --log-file /tmp/qam16_seed3_genie_channel.log
```

CFO/timing genie:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 2 --file 10240 --diag-genie-timing --log-level warn --log-file /tmp/qam16_seed2_genie_timing.log
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 3 --file 10240 --diag-genie-timing --log-level warn --log-file /tmp/qam16_seed3_genie_timing.log
```

No-clip genie:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 2 --file 10240 --diag-genie-no-clip --log-level warn --log-file /tmp/qam16_seed2_genie_no_clip.log
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 3 --file 10240 --diag-genie-no-clip --log-level warn --log-file /tmp/qam16_seed3_genie_no_clip.log
```

Secondary combination check:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 2 --file 10240 --diag-genie-timing --diag-genie-sigma --log-level warn --log-file /tmp/qam16_seed2_genie_timing_sigma.log
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 3 --file 10240 --diag-genie-timing --diag-genie-sigma --log-level warn --log-file /tmp/qam16_seed3_genie_timing_sigma.log
```

## Attribution Table

| Seed | Run | Status | BRAVO CWFAIL | ALPHA retx | E2E goodput | On-air goodput | Recovery vs baseline |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 2 | baseline | PASS | 11 | 25 | 294 bps | 437 bps | 0/11 |
| 2 | empirical sigma^2 genie | PASS | 11 | 24 | 295 bps | 468 bps | 0/11 |
| 2 | two-path LS channel approximation | aborted/regressed | >=140 failed frames before abort | n/a | n/a | n/a | negative, invalid perfect-H result |
| 2 | CFO/timing genie | PASS | 4 | 8 | 380 bps | 587 bps | 7/11 |
| 2 | no-clip genie | PASS | 11 | 25 | 294 bps | 437 bps | 0/11 |
| 2 | timing + sigma genies | PASS | 10 | 13 | 366 bps | 572 bps | 1/11 |
| 3 | baseline | PASS | 12 | 21 | 308 bps | 489 bps | 0/12 |
| 3 | empirical sigma^2 genie | PASS | 8 | 21 | 326 bps | 506 bps | 4/12 |
| 3 | two-path LS channel approximation | aborted/regressed | >=100 failed frames before abort | n/a | n/a | n/a | negative, invalid perfect-H result |
| 3 | CFO/timing genie | PASS | 1 | 3 | 427 bps | 647 bps | 11/12 |
| 3 | no-clip genie | PASS | 12 | 21 | 308 bps | 489 bps | 0/12 |
| 3 | timing + sigma genies | PASS | 5 | 11 | 355 bps | 557 bps | 7/12 |

The proven dominant measured cause of QAM16-on-Good CW failure is
light-sync timing/CFO-windowing, because the CFO/timing genie recovers 18 of 23
baseline BRAVO failed codewords across seeds 2 and 3.

It does not recover every failure: seed2 still has 4 BRAVO failed frames and
seed3 still has 1. The remaining failures are therefore residual structured
impairment, residual channel-estimation error, or another secondary mechanism.
Exact true-H remains UNTESTED.

The empirical sigma^2 genie recovers only 4 of 23 baseline failures by itself
and worsens the timing-only result when combined with the timing genie. The
sigma mismatch is real, but it is not the dominant root cause and the empirical
nearest-constellation sigma oracle is not a valid shipping fix.

The no-clip genie is unchanged from baseline on both seeds. Clipping/PAPR
limiting is ruled out for these forced CLI OTASim cells.

## Error Signature

| Seed | Run | Failed-frame diag n | EVM mean | empirical sigma^2 | LLR sigma^2 | LLR/empirical sigma^2 | mean abs CPE | edge phase ramp | outer/inner EVM^2 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | baseline | 11 | 0.266 | 0.093 | 1.397 | 17.601 | 1.658 deg | 6.987 deg | 1.716 |
| 2 | empirical sigma^2 genie | 11 | 0.262 | 0.089 | 0.089 | 1.000 | 1.919 deg | 6.514 deg | 1.634 |
| 2 | two-path LS channel approximation | 140 | 1.136 | 3.342 | 10.045 | 2.002 | 1.973 deg | 6.844 deg | 54.677 |
| 3 | baseline | 12 | 0.274 | 0.099 | 0.808 | 8.445 | 2.372 deg | 6.595 deg | 1.744 |
| 3 | empirical sigma^2 genie | 8 | 0.273 | 0.100 | 0.100 | 1.000 | 1.883 deg | 5.907 deg | 1.812 |
| 3 | two-path LS channel approximation | 100 | 0.992 | 2.489 | 8.171 | 1.953 | 1.971 deg | 6.463 deg | 40.923 |

The baseline LLR sigma^2 is much larger than the empirical post-equalization
residual variance on failed frames: 17.6x for seed2 and 8.4x for seed3. The
sigma genie forces the ratio to 1.0 but does not clear the failures, so the
suspected sigma^2 bug is a real scaling mismatch but not the dominant root cause
for these two seeds.

The failed-frame phase terms are small after a frame is accepted for demod, but
the timing genie log shows many light-sync decisions displaced by tens of
samples from the expected connected-data arrival. That explains the apparent
contradiction: local residual phase on failed accepted frames was small, while
the dominant failure mode was choosing the wrong FFT-window arrival before the
OFDM demodulator saw the data symbols.

The outer/inner QAM16 EVM^2 ratio is about 1.7x on baseline failed frames. The
direct no-clip genie does not move CWFAIL, so this is a symptom of the timing
problem or fading structure, not proof of TX clipping.

## True-H Feasibility

Exact simulator channel truth was assessed and not implemented in this bounded
diagnostic pass. The current OTASim gRPC path does not expose per-subcarrier
channel truth to the BRAVO demodulator. `SessionContext` owns receiver-side
channel instances and applies `WattersonChannelModel::process(...)` to mixed
audio. The wrapped Watterson channel evolves passband Hilbert delay, fading tap
oscillators, analytic delay line, noise, sample epoch, and optional CFO. There
is no existing API that maps an OFDM frame's absolute FFT-window and subcarrier
set to exact H(k,t), and no proto field to deliver that truth through the
receiver path.

A valid true-H genie would require new simulator/channel APIs plus gRPC plumbing
and exact sample-index alignment between the channel core and BRAVO OFDM
symbols. That is a larger diagnostic project. Channel estimation is therefore
UNTESTED, not cleared; it is simply no longer the dominant suspect for the
measured Good@20 seed2/seed3 failures because timing recovers 18/23 failures.

## Current Attribution

The proven dominant measured cause is light-sync timing/CFO-windowing:

- Seed2: BRAVO CWFAIL improves from 11 to 4, ALPHA retransmissions from 25 to 8,
  and E2E goodput from 294 bps to 380 bps.
- Seed3: BRAVO CWFAIL improves from 12 to 1, ALPHA retransmissions from 21 to 3,
  and E2E goodput from 308 bps to 427 bps.
- Total: the timing genie recovers 18 of 23 failed BRAVO codewords.

No-clip recovers 0 of 23. Sigma-only recovers 4 of 23. The invalid two-path LS
channel approximation is excluded from root-cause ranking.

## Guardrail Status

The diagnostic code is gated behind explicit CLI flags/env vars and is not
active in the default shipping path.

Build:

```sh
cmake --build build --target cli_simulator ota_simulator -j4
```

Result: passed.

No-genie Step 0 modem guards before committing this diagnosis artifact:

| Guard | Command shape | Result |
| --- | --- | --- |
| AWGN QAM16 R1/4 | `--expert --mod qam16 --rate r1_4 --channel awgn --snr 20 --seed 1 --file 1024` | PASS, BRAVO CWFAIL 0, ALPHA retx 0, E2E 432 bps |
| AWGN QAM16 R1/2 | `--expert --mod qam16 --rate r1_2 --channel awgn --snr 20 --seed 1 --file 1024` | PASS, BRAVO CWFAIL 0, ALPHA retx 0, E2E 1739 bps |
| AWGN QAM16 R2/3 | `--expert --mod qam16 --rate r2_3 --channel awgn --snr 20 --seed 1 --file 1024` | PASS, BRAVO CWFAIL 0, ALPHA retx 0, E2E 2043 bps |
| AWGN QAM16 R3/4 | `--expert --mod qam16 --rate r3_4 --channel awgn --snr 20 --seed 1 --file 1024` | PASS, BRAVO CWFAIL 0, ALPHA retx 0, E2E 2139 bps |
| DQPSK Good/SNR12 floor | `--channel good --snr 12 --file 1024 --seed 42` | PASS, negotiated DQPSK R1/4, BRAVO CWFAIL 2, ALPHA retx 12, E2E 108 bps |

Full ctest with genies off is not green in this workspace. Current run: 90/94
passed; failed:

- `Protocol`
- `TxBurstNormalization`
- `DecodeBenchReplay`
- `CLISyntheticNotch`

No push was made.
