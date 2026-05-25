# Channel-Estimation Proof Notes - 2026-05-25

Branch: `feat/good-fading-qam16-ladder-2026-05-24`

Status: intermediate floor improvement committed for the estimator inner loop.
This is not yet the final GUI ladder acceptance result.

## Implemented Named Methods

1. DVB-T / ETSI EN 300 744 style scattered pilots
   - TX/RX now share `src/ofdm/pilot_pattern.hpp`.
   - Coherent OFDM modes with `scattered_pilots=true` cycle pilot positions by
     OFDM data-symbol index while keeping per-symbol pilot overhead constant.
   - Differential modes keep the fixed comb so the DQPSK floor path is not
     rewritten.

2. Hoeher/Kaiser/Robertson 1997 2-D Wiener/MMSE interpolation
   - Implemented as separable 2x1-D filtering in `src/ofdm/wiener_interpolator.hpp`.
   - The demodulator runs time-Wiener per logical carrier, then frequency-Wiener
     across logical carriers.
   - The design is robust/mismatched in the Edfors sense: one conservative
     channel-statistics setting is used rather than per-cell tuning.

3. Mignone/Morello CD3-aligned decision-directed refinement
   - Existing coherent QAM DD observations now key state by physical FFT bin
     rather than the current data-carrier ordinal. That is required once pilot
     and data roles move between symbols.
   - DD updates remain reliability-gated and step-limited to avoid poisoning.

4. No-pilot differential guard preservation
   - The shared carrier-pattern refactor initially exposed an old assumption in
     `updateChannelEstimate()`: when a legacy no-pilot differential path calls
     the pilot updater, there are no pilot anchors and interpolation must not
     rewrite the channel estimate. The updater now returns early when no pilots
     are active, preserving the existing unity/LTS channel state.

## Labeled Deviations

- Scattered-pilot pattern is adapted to ProjectUltra's 59 active carriers. It is
  DVB-T style cyclic time/frequency sampling, not a literal DVB-T carrier index
  table.
  - PHY view: preserves the named method's time/frequency sampling benefit at
    the existing pilot overhead.
  - DSP view: keeps a constant data-carrier count per symbol, avoiding frame
    sizing churn.
  - HF operator view: buys time diversity through fades without adding more
    known-symbol airtime.

- Wiener interpolation is separable 2x1-D, not a full joint 2-D matrix solve.
  The findings brief explicitly allowed this form.
  - PHY view: still uses covariance-derived MMSE weights with a documented
    delay/Doppler reference.
  - DSP view: bounded small systems keep the hot path predictable.
  - HF operator view: robust mismatch is preferable to one-cell tuning because
    real QSB/multipath stats move during a contact.

- Robust mismatch currently uses delay spread `1.0e-3 s` and Doppler `0.5 Hz`.
  That is deliberately conservative for Good/Moderate instead of Good@20-only
  tuning.
  - PHY view: this is a documented covariance model, not a magic scalar.
  - DSP view: a single stable filter family avoids per-cell state-machine
    branching.
  - HF operator view: the estimator should survive a worse-than-Good moment
    rather than collapse when the path changes.

- CD3 is partial. It aligns the existing pre-FEC coherent QAM DD path to
  physical carriers and keeps reliability gating, but it does not yet feed
  post-FEC decoded symbols back as virtual pilots.
  - PHY view: this is a method-aligned DD correction, but not the full CD3
    receiver loop.
  - DSP view: it avoids a larger decoder/equalizer feedback restructuring in
    this commit.
  - HF operator view: bounded pre-FEC refinement is safer than aggressive
    feedback that can spread one bad fade across later symbols.

## Measurement Table

4539 reference for 16-QAM on Ricean/Good is 17.0 dB required SNR for the 6400 bps
class waveform. These ProjectUltra measurements are not claiming 4539 parity;
they are the current floor evidence for the estimator change.

| Cell | Before | After | Delta / Status |
|---|---:|---:|---|
| Forced Good20 QAM16 R1/4, 1 KB, seed 1 | PASS, BRAVO CWFAIL 2, ALPHA retx 4, E2E 175 bps | PASS, BRAVO CWFAIL 0, ALPHA retx 0, E2E 454 bps | Same SNR, CWFAIL 2 -> 0, retx 4 -> 0, E2E +279 bps |
| Forced Good20 QAM16 R1/4, 10 KB, seeds 1/2/3 | Not remeasured in this patch; findings doc records GUI Good20 10 KB baseline as 1/3 | PASS 3/3 forced. Seed1 CWFAIL 13 retx 21 E2E 323 bps; seed2 CWFAIL 11 retx 25 E2E 294 bps; seed3 CWFAIL 15 retx 25 E2E 304 bps | Inner-loop estimator proof only; CWFAIL is not yet zero |
| GUI Good20 QAM16 R1/4, 10 KB, seeds 1/2/3 | Findings doc baseline: 1/3 GUI pass; seed2/seed3 fail in file phase | Not passed yet. Sequential seed3 after this patch stuck in connect/handshake and was stopped before QAM data transfer | Headline gate still open |
| DQPSK Good12 floor guard, 1 KB, seed 42 | Detached `HEAD` baseline reproduced the same floor profile: PASS, BRAVO CWFAIL 4, ALPHA retx 19, E2E 52 bps on second run | PASS, BRAVO CWFAIL 4, ALPHA retx 19, E2E 52 bps | No regression versus this branch baseline; earlier 391 bps note was not reproduced |
| Forced AWGN20 QAM16 R3/4, 1 KB, seed 1 | Guard: no QAM16/AWGN regression | PASS, BRAVO CWFAIL 0, ALPHA retx 0, E2E 2139 bps | Smoke guard green |
| QAM32/QAM64 Good floor | Not measured | Not measured | Still pending |

## Reproducers

Baseline seed-1 forced cell was run before the patch:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 1 --file 1024
```

After-patch forced estimator cells:

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 1 --file 1024
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 1 --file 10240
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 2 --file 10240
./build/cli_simulator --expert --mod qam16 --rate r1_4 --channel good --snr 20 --seed 3 --file 10240
```

Guard cells:

```sh
./build/cli_simulator --channel good --snr 12 --file 1024 --seed 42
./build/cli_simulator --expert --mod qam16 --rate r3_4 --channel awgn --snr 20 --seed 1 --file 1024
```

Unit and integration checks:

```sh
cmake --build build -j4
ctest --test-dir build -R "OFDMPilotPattern|OFDMWienerInterpolator|WaveformLoopback|OFDMCarrierMaskPlumbing|WaveformPolicy|OFDMLinkAdaptation" --output-on-failure -j4
ctest --test-dir build -R "ComprehensiveModem|OFDMPilotPattern|OFDMWienerInterpolator|WaveformLoopback|OFDMCarrierMaskPlumbing|WaveformPolicy|OFDMLinkAdaptation" --output-on-failure -j4
```

Full-suite note:

```sh
ctest --test-dir build -j4 --output-on-failure
```

The full suite is not green on this branch. After the no-pilot fix,
`ComprehensiveModem` passes. The remaining observed failures are not introduced
by this patch:

- `DecodeBenchReplay`: documented pre-existing red in the findings brief.
- `Protocol`: reproduced in detached baseline worktree at `HEAD` (`c3615a1`).
- `TxBurstNormalization`: reproduced in detached baseline worktree at `HEAD`.
- `CLISyntheticNotch`: reproduced in detached baseline worktree at `HEAD`.

## Open Work

- Finish the GUI ladder path. The estimator inner loop improved, but the
  shipping `tools/qam16_ladder_scenario.sh` Good20 seed3 run did not reach QAM16
  file data after this patch.
- Drive the full requested FER floor map over >=200 frames per cell.
- Complete CD3 with post-FEC virtual pilots if the pre-FEC DD alignment
  plateaus.
- Re-run the final GUI Good20 QAM16 R1/4 10 KB gate for 3 seeds, then 20 seeds.
