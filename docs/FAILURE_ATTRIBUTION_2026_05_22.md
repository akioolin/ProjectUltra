# OFDM Failure Attribution - 2026-05-22

## Verdict

The baseline dense-comb OFDM receiver failures on QAM16 R1/2 Good are fade/SNR-limited, not channel-estimation-limited.

The failed frames are dominated by frequency-selective carrier nulls and occasional whole-frame SNR wipes. The equalizer output is usually inside the measured noise model on the affected frames, while per-carrier `|H|` collapses far below the frame mean. A better estimator cannot recover information carried through a spectral null; the next lever is frequency/time diversity in the coded-bit mapping, plus interleaving/HARQ/rate/power policy.

The scattered-pilot and 2-D Wiener experiment was reverted from the shipping path. The retained changes are diagnostic instrumentation only and are gated by `ULTRA_FAILURE_ATTRIBUTION=1`.

## Reproducer Commands

Baseline dense-comb smoke cells:

```bash
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 5120 --seed 42
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 5120 --seed 43
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 17 --file 5120 --seed 42
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 17 --file 5120 --seed 43
```

Failure-attribution runs:

```bash
ULTRA_FAILURE_ATTRIBUTION=1 ./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 5120 --seed 43 > /tmp/failure_attr_qam16r12_good_snr20_seed43_passwarn.log 2>&1
ULTRA_FAILURE_ATTRIBUTION=1 ./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 17 --file 5120 --seed 43 > /tmp/failure_attr_qam16r12_good_snr17_seed43_passwarn.log 2>&1
ULTRA_FAILURE_ATTRIBUTION=1 ./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 17 --file 5120 --seed 42 > /tmp/failure_attr_qam16r12_good_snr17_seed42.log 2>&1
ULTRA_FAILURE_ATTRIBUTION=1 ./build/cli_simulator --expert --mod qam16 --rate r2_3 --channel good --snr 20 --file 5120 --seed 43 > /tmp/failure_attr_qam16r23_good_snr20_seed43.log 2>&1
```

## Baseline Frame Success

Judged on BRAVO data `frame_success`, not end-to-end goodput.

| Cell | Seed | BRAVO frame_success | Failed frames | Notes |
|---|---:|---:|---:|---|
| QAM16 R1/2 Good SNR20 | 42 | 100.0% | 0 | 5 KB dense-comb baseline |
| QAM16 R1/2 Good SNR20 | 43 | 82.8% | 5 | Failure-attribution target |
| QAM16 R1/2 Good SNR17 | 42 | 95.5% | 1 | Single localized CW failure |
| QAM16 R1/2 Good SNR17 | 43 | 64.3% to 83.3% | 5 to 15 | Variable, same failure mechanism |

## Classification

| Cell | Evidence | Classification |
|---|---|---|
| QAM16 R1/2 Good SNR20 seed43 | Failed frames: EVM mean 0.282, normalized EVM mean 1.27, p95 4.54, inside-noise fraction 0.88. Failed CWs had mean unsatisfied checks 99.9 and failed-CW LLR p10 0.22. Deep-null example had `min_absH=0.090` with inside-noise fraction 0.952. | Fade/SNR-limited |
| QAM16 R1/2 Good SNR17 seed42 | One failed frame at `sync_abs=1130852`: `cw_ok=7`, `cw_fail=1`, failed CW6 unsatisfied checks 33 on the current verification run. Other CWs had strong LLR means around 10.7 to 11.5. EQ had `min_absH=0.231`, normalized EVM mean 3.116, inside-noise fraction 0.649. | Localized frequency-null failure |
| QAM16 R1/2 Good SNR17 seed43 | Failed frames: EVM mean 0.267, normalized EVM mean 2.05, p95 6.47, inside-noise fraction 0.79, average `min_absH=2.86`. Local example at `sync_abs=1035834`: `cw_ok=7`, `cw_fail=1`, failed CW3 unsatisfied checks 88, `min_absH=1.387`. Whole-frame wipeout at `sync_abs=3634975`: `cw_ok=0`, all CWs failed with low p10 LLRs. | Mixed localized nulls and whole-frame SNR fades |
| QAM16 R2/3 Good SNR20 seed43 | BRAVO frame_success 64.0%. Failed frames: normalized EVM mean 5.08, p95 15.99, inside-noise fraction 0.63, average `min_absH=2.10`. Higher-rate CWs fail with less parity margin even when many LLR means remain high. | Same fade mechanism, less LDPC margin |

## Per-CW Evidence

The decisive pattern is a good-frame/good-CW contrast within the same OFDM burst:

- QAM16 R1/2 Good SNR17 seed42 had one failed data frame with `cw_ok=7` and `cw_fail=1`. The failed CW had 33 unsatisfied checks on the current verification run while the other seven CWs converged. The good CWs carried strong pre-FEC LLR means near 11, so this was not a global estimator collapse.
- QAM16 R1/2 Good SNR20 seed43 had localized failures such as `cw_ok=7`, `cw_fail=1`, with failed CW unsatisfied checks in the 20s to 80s. Whole-frame wipeouts also appeared, with `cw_ok=0` and all CWs showing high unsatisfied checks.
- QAM16 R2/3 Good SNR20 seed43 fails more often because each CW has less LDPC redundancy. The observed mechanism remains carrier/fade concentration, not a coherent channel-estimator failure.

## Per-Carrier And EVM Evidence

Failed frames show the receiver is often equalizing consistently with the noise model while the channel response has deep spectral holes:

- Seed42 SNR17 localized failure: `min_absH=0.231` while the surviving CWs had strong LLRs.
- Seed43 SNR20 deep-null failure: `min_absH=0.090`, normalized EVM mean 0.914, inside-noise fraction 0.952.
- Seed43 SNR20 aggregate failed frames: inside-noise fraction 0.88, so most post-equalizer samples are not wild estimator outliers.
- Seed43 SNR17 aggregate failed frames: inside-noise fraction 0.79, with average `min_absH=2.86`.

This points to coded bits being over-exposed to frequency-local fades. The next investigation should trace whether a codeword's coded bits land on adjacent or narrow carrier groups after the carrier/frame/burst interleavers.

## Time And Seed Evidence

Failures did not repeat at the same frame or symbol positions across seeds. Seed42 SNR17 had a single localized failure at `sync_abs=1130852`; seed43 failures occurred at different offsets and included both localized CW loss and whole-frame fades. That is consistent with channel-realization-dependent fades, not a deterministic burst-edge estimator defect.

## PAPR Caveat

QAM16 runs also show a separate transmit-side stressor: `peak_after_gain` around 2.19 to 2.24 and roughly 900 clipped samples in some runs. That may contribute to margin loss, but the first-order failure evidence above is still frequency-selective fade concentration. Treat PAPR/clipping as a parallel follow-up unless it becomes trivial to remove while preserving DQPSK behavior.

## Instrumentation Kept

The retained diagnostic path records:

- Per-frame LDPC codeword outcome, iterations, final unsatisfied checks, and pre-FEC LLR summaries.
- Failed-frame post-equalizer EVM, normalized EVM, per-symbol edge metrics, per-carrier `|H|`, and per-carrier SNR.
- Waveform forwarding of the compact equalizer diagnostic string to the streaming decoder log.

The instrumentation is read-only with respect to decoding. It does not change demapping, LDPC, ARQ, pilot placement, or equalizer adaptation.

## Next Lever

Channel estimation is not the right primary lever for these baseline Good failures. Pivot to coded-bit diversity:

1. Trace coded-bit placement from LDPC CW to OFDM carriers and symbols.
2. Confirm whether each CW spans the full carrier set and multiple symbols after the existing interleavers.
3. If any CW is frequency-localized, re-permute/deepen the interleaver so every CW sees the whole band.
4. Re-test on the 5 KB smoke probe before any full floor sweep.
