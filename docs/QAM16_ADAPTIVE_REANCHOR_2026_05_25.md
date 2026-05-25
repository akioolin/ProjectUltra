# QAM16 Good@20 Adaptive Short Re-Anchor

Date: 2026-05-25
Branch: `feat/good-fading-qam16-ladder-2026-05-24`
Foundation: `47ca0c2` plus diagnosis tooling commit `ebfae73`

## Cause Targeted

`docs/QAM16_FAILURE_ATTRIBUTION_2026_05_25.md` attributes the dominant
QAM16 Good@20 R1/4 BRAVO failures to connected-data FFT-window timing:
the timing genie collapsed seed2/seed3 failed frames from 11/12 to 4/1.

This fix does not change the OFDM equalizer, LDPC path, or LLR model. It
adds a short chirp re-anchor ahead of the existing two-LTS data preamble
only when the connected data mode is coherent OFDM on a fading/marginal
channel.

## Trigger

The production GUI/TNC trigger is the negotiated peer fading index already
used by the link adaptation layer:

`waveform == OFDM_CHIRP && coherent_modulation && peer_fading >= kQAM16AwgnFadingMax`

In `cli_simulator`, the same measured trigger is used when available. The
simulator also enables the path for coherent OFDM on non-AWGN channel classes,
because forced expert cells can bypass the normal measured channel-quality
history and the test channel class is ground truth inside OTASim.

Clean/AWGN sessions remain on the current light LTS-only data preamble.

## Duration Sweep

Command template:

```sh
ULTRA_SHORT_REANCHOR_CHIRP_MS=<ms> ./build/cli_simulator \
  --expert --mod qam16 --rate r1_4 --channel good --snr 20 \
  --seed <seed> --file 10240 --log-level warn \
  --log-file /tmp/qam16_good20_seed<seed>_short<ms>.log
```

Baseline from attribution doc: seed2 CWFAIL 11, seed3 CWFAIL 12.
Timing-genie floor from attribution doc: seed2 CWFAIL 4, seed3 CWFAIL 1.

| Chirp ms | Seed | BRAVO CWFAIL | ALPHA retx | On-air bps | E2E bps | CRC |
|---:|---:|---:|---:|---:|---:|---|
| 100 | 2 | 3 | 8 | 536 | 345 | OK |
| 100 | 3 | 1 | 6 | 545 | 372 | OK |
| 150 | 2 | 4 | 21 | 380 | 277 | OK |
| 150 | 3 | 1 | 15 | 413 | 322 | OK |
| 200 | 2 | 4 | 16 | 389 | 308 | OK |
| 200 | 3 | 7 | 20 | 379 | 295 | OK |
| 250 | 2 | 7 | 26 | 345 | 256 | OK |
| 250 | 3 | 4 | 15 | 389 | 298 | OK |
| 300 | 2 | 6 | 16 | 370 | 291 | OK |
| 300 | 3 | 5 | 10 | 394 | 319 | OK |

The selected default is 100 ms: it is the minimum tested duration in the
requested 100-300 ms sweep and the only duration that improves both seeds to
the timing-genie floor or better.

## Reproducer With Default

```sh
./build/cli_simulator --expert --mod qam16 --rate r1_4 \
  --channel good --snr 20 --seed 2 --file 10240 --log-level warn

./build/cli_simulator --expert --mod qam16 --rate r1_4 \
  --channel good --snr 20 --seed 3 --file 10240 --log-level warn
```

Default-path confirmation after the final state-gating patch:

| Seed | Baseline CWFAIL | Default CWFAIL | ALPHA retx | On-air bps | E2E bps | CRC |
|---:|---:|---:|---:|---:|---:|---|
| 2 | 11 | 3 | 8 | 536 | 345 | OK |
| 3 | 12 | 1 | 6 | 545 | 372 | OK |

## Guardrails

AWGN QAM16 ladder, genies off, 1 KB guard shape from the attribution doc:

| Rate | BRAVO CWFAIL | ALPHA retx | E2E bps | CRC |
|---|---:|---:|---:|---|
| R1/4 | 0 | 0 | 432 | OK |
| R1/2 | 0 | 0 | 1739 | OK |
| R2/3 | 0 | 0 | 2043 | OK |
| R3/4 | 0 | 0 | 2139 | OK |

DQPSK Good/SNR12 floor, genies off:

| Baseline | Result |
|---|---|
| attribution-doc seed42: BRAVO CWFAIL 2, ALPHA retx 12, E2E 108 bps | seed42: BRAVO CWFAIL 1, ALPHA retx 9, E2E 120 bps, CRC OK |

`ctest --test-dir build --output-on-failure -j4`: 91/94 passed. Remaining
failures were `Protocol`, `TxBurstNormalization`, and `DecodeBenchReplay`;
no new failure was introduced relative to the diagnosis guard, and
`CLISyntheticNotch` passed in this run.

## Stack Justification

PHY: the failure signature was a timing-induced phase ramp/ICI problem, so a
short wideband chirp gives processing gain and frequency diversity before the
same LTS/equalizer path.

DSP: the extra matched-filter work is only on connected coherent OFDM data
frames when the deterministic channel-state trigger enables it. The data hot
path after synchronization is unchanged.

HF operation: fading/multipath links get periodic timing margin; clean links
avoid the throughput cost and stay on LTS-only data frames.
