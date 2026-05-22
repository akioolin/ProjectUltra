# Soft CSI LLR Tuning - 2026-05-22

## Verdict

The coherent OFDM demapper already had per-carrier CSI LLR weighting through
`carrier_noise_var`. This round did not add a duplicate demapper weighting path.

The accepted change is a smooth gray-zone reliability inflation in the existing
`carrier_noise_var` path for coherent QAM16/QAM32/QAM64. It leaves clean carriers
nearly unchanged and reduces trust in carriers near the deep-fade floor without
enabling the old hard erasure cliff.

Result on the required 30-seed gate, QAM16 R1/2 Good/SNR20, 5 KB, seeds 42..71:

| Metric | Dense-comb baseline | Soft gray-zone CSI | Delta |
|---|---:|---:|---:|
| Median BRAVO frame_success | 79.5% | 83.3% | +3.8 pp |
| Median end-to-end goodput | 558 bps | 610 bps | +52 bps |
| Median ALPHA retransmissions | 10 | 9 | -1 |
| Clean seeds | 3 inferred from 27/30 fail | 4 | +1 |
| Failed or timeout seeds | 27/30 | 26/30 | -1 |

This is a real but modest LLR-calibration win. It does not solve the remaining
fade-limited cases; cross-retransmission diversity or other diversity mechanisms
remain the first-order follow-up.

## Implementation

File changed: `src/ofdm/channel_equalizer_equalize.cpp`.

The existing coherent QAM path still computes the MMSE post-equalizer carrier
noise variance:

```text
carrier_noise_var = noise_variance / (|H|^2 + noise_variance)
```

The accepted tuning multiplies that existing variance by:

```text
inflation = clamp((gamma + 0.5) / gamma, 1.0, 12.0)
gamma = |H|^2 / noise_variance
```

This is continuous:

| Carrier gamma | Inflation | Effect |
|---:|---:|---|
| 20 dB | 1.005x | Clean carrier unchanged |
| 10 dB | 1.05x | Clean carrier nearly unchanged |
| 0 dB | 1.5x | Gray-zone carrier softened |
| -3 dB | 2.0x | Fade-edge carrier softened |
| Deep null | up to 12x | Bounded soft down-weight |

Hard erasure remains default-off. Differential DQPSK does not use this path.

## Rejected Configurations

| Config | Result | Decision |
|---|---|---|
| Duplicate relative-CSI multiplier in `ofdm_symbol_demap.cpp` | No smoke win; risked double-weighting an existing path | Reverted |
| Measured guard/LTS residual sigma2 | Regressed SNR20 seed43 to 81.8% vs 82.8% baseline and SNR17 seed43 stalled | Reverted |
| QAM ZF/unbiased LLR variant | Smoke was neutral: 100.0 / 82.8 / 95.5 / 83.3 | Reverted |
| Fading-index hard gate for gray-zone curve | Lost the useful seed43 improvement and regressed SNR17 seed43 to 71.4% | Rejected |
| Ungated smooth gray-zone CSI | Passed smoke and 30-seed gate; AWGN spot was not a regression vs clean baseline | Accepted |

## Smoke Probe

Required smoke probe: QAM16 R1/2 Good, 5 KB, seeds 42 and 43 at SNR 20 and
17 dB, judged on BRAVO data `frame_success`.

| SNR | Seed | Dense-comb baseline reference | Soft gray-zone CSI | BRAVO decoded/failed | ALPHA retx | E2E bps |
|---:|---:|---:|---:|---:|---:|---:|
| 20 | 42 | 100.0% | 100.0% | 21 / 0 | 1 | 1457 |
| 20 | 43 | 82.8% | 86.2% | 25 / 4 | 12 | 695 |
| 17 | 42 | 82.8% user gate reference | 84.6% | 22 / 4 | 8 | 695 |
| 17 | 43 | 83.3% observed reference | 83.3% | 25 / 5 | 22 | 404 |

The smoke win is concentrated on the SNR20 seed43 fade case. The SNR17 cases are
not fixed; they remain dominated by fade/SNR margin.

## 30-Seed Gate

Required gate: QAM16 R1/2 Good/SNR20, 5 KB, seeds 42..71.

Seed45 timed out again and is counted conservatively as a failed/timeout seed
with zero frame success, zero bps, and 999 retransmissions.

| Seed | Frame success | Decoded | Failed | Retx | E2E bps | Pass |
|---:|---:|---:|---:|---:|---:|---|
| 42 | 100.0% | 21 | 0 | 1 | 1457 | yes |
| 43 | 86.2% | 25 | 4 | 12 | 695 | yes |
| 44 | 82.6% | 38 | 8 | 27 | 360 | yes |
| 45 | 0.0% | 0 | 999 | 999 | 0 | timeout |
| 46 | 100.0% | 21 | 0 | 1 | 1457 | yes |
| 47 | 84.0% | 21 | 4 | 5 | 655 | yes |
| 48 | 91.3% | 21 | 2 | 4 | 780 | yes |
| 49 | 75.9% | 22 | 7 | 13 | 578 | yes |
| 50 | 76.7% | 23 | 7 | 14 | 492 | yes |
| 51 | 77.8% | 21 | 6 | 9 | 748 | yes |
| 52 | 80.0% | 24 | 6 | 13 | 353 | yes |
| 53 | 65.7% | 23 | 12 | 17 | 437 | yes |
| 54 | 67.7% | 21 | 10 | 17 | 547 | yes |
| 55 | 80.6% | 25 | 6 | 16 | 630 | yes |
| 56 | 77.8% | 21 | 6 | 10 | 554 | yes |
| 57 | 63.6% | 21 | 12 | 13 | 413 | yes |
| 58 | 62.9% | 22 | 13 | 18 | 157 | yes |
| 59 | 95.5% | 21 | 1 | 1 | 1456 | yes |
| 60 | 95.5% | 21 | 1 | 1 | 1457 | yes |
| 61 | 95.5% | 21 | 1 | 1 | 1457 | yes |
| 62 | 91.7% | 22 | 2 | 5 | 982 | yes |
| 63 | 100.0% | 21 | 0 | 0 | 1808 | yes |
| 64 | 91.7% | 22 | 2 | 7 | 745 | yes |
| 65 | 91.3% | 21 | 2 | 2 | 883 | yes |
| 66 | 100.0% | 21 | 0 | 0 | 1843 | yes |
| 67 | 72.7% | 24 | 9 | 21 | 376 | yes |
| 68 | 78.6% | 22 | 6 | 9 | 366 | yes |
| 69 | 60.5% | 26 | 17 | 30 | 252 | yes |
| 70 | 84.0% | 21 | 4 | 7 | 546 | yes |
| 71 | 88.0% | 22 | 3 | 6 | 591 | yes |

Summary:

```text
n=30 median_success=83.3 median_bps=610 median_retx=9 clean=4 failed_or_timeout=26
```

## Mechanism Check

Reproducer:

```bash
ULTRA_FAILURE_ATTRIBUTION=1 ./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 5120 --seed 43 --log-level warn --log-category modem,demod --log-file /tmp/soft_csi_gray_attr_s20_seed43_final.log
```

BRAVO result:

```text
frames_decoded=25 frames_failed=4 frame_success=86.2%
```

Baseline for this cell had 24 decoded / 5 failed / 82.8%.

Representative failed frames with the tuned CSI path:

| Failure mode | Evidence | Interpretation |
|---|---|---|
| Localized CW loss | `cw_ok=7`, `cw_fail=1`, failed CW unsat=26, failed CW LLR mean=8.93, p10=0.80, `min_absH=4.855` | Gray-zone evidence is softer than the previous confident-wrong signature, but one CW still lacks enough clean evidence |
| Whole-frame fade | `cw_ok=0`, all 8 CWs failed, LLR means 1.76..1.99, p10 0.01..0.02, `min_absH=1.793`, inside-noise fraction 0.878 | True fade/SNR wipeout; LLRs are already low confidence |
| Broad low-margin fade | `cw_ok=1`, `cw_fail=7`, failed CW LLR means 5.40..7.22, p10 0.16..0.23, inside-noise fraction 0.936 | Residual failure is diversity/margin-limited, not missing CSI weighting |
| Deep spectral notch | `cw_ok=4`, `cw_fail=4`, `min_absH=0.090`, inside-noise fraction 0.957 | The notch is still visible; soft weighting avoids over-trust but cannot create lost information |

The mechanism matches the earlier failure attribution: better LLR calibration
recovers a small amount of margin, while remaining losses require diversity.

## Guards

| Guard | Command | Result |
|---|---|---|
| DQPSK floor | `./build/cli_simulator --channel good --snr 12 --file 1024 --seed 42 --log-level warn` | Negotiated DQPSK R1/4 cw=4; on-air 391 bps; ALPHA frames_sent=20, retransmissions=4, timeouts=4; pass |
| AWGN QAM16 spot | `./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel awgn --snr 20 --file 5120 --seed 42 --log-level warn` | Final path: 88.5%, 23 decoded / 3 failed, 5 retx, 1131 bps; clean baseline measured during this round was 84.8%, 28 decoded / 5 failed, 12 retx, 801 bps |
| Good smoke | See table above | Survived, with SNR20 seed43 improved |
| Test suite | `ctest --test-dir build -j4 --output-on-failure` | 92/92 passed |

## Reproducer Commands

Smoke:

```bash
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 5120 --seed 42 --log-level warn
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 5120 --seed 43 --log-level warn
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 17 --file 5120 --seed 42 --log-level warn
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 17 --file 5120 --seed 43 --log-level warn
```

30-seed gate:

```bash
for seed in $(seq 42 71); do
  ./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 5120 --seed "$seed" --log-level warn
done
```

Guards:

```bash
./build/cli_simulator --channel good --snr 12 --file 1024 --seed 42 --log-level warn
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel awgn --snr 20 --file 5120 --seed 42 --log-level warn
ctest --test-dir build -j4 --output-on-failure
```

## Next Step

Do not return to channel-estimation tuning for this failure mode. The remaining
failures are still fade/SNR-limited after CSI LLR calibration. The next useful
lever is retransmission diversity with clean soft combining, or another diversity
mechanism that gives a failed codeword evidence from different carriers/time.
