# QAM16 DD Tracker Round 4 Findings - 2026-05-22

## Summary

Round 4 built a coherent-QAM16-only decision-directed per-carrier channel
tracker. The tracker forms `H_dd[k] = Y[k] / X_hat[k]` from reliable 16-QAM
hard decisions, fuses it with pilot interpolation through a per-carrier Kalman
state, and bounds DD innovations so a single wrong decision cannot swing a
carrier estimate unboundedly.

The implementation improves the forced-QAM16 Good 20 dB / 20 KB cell, but it
does not reach the 80% on-air goodput target. This is not a completed QAM16
promotion; it is a measured tracker win and an honest miss against the long-fade
target.

## Measured Result

All numbers below are forced QAM16 (`--expert --mod qam16`) at seed 42.

| Cell | Pilot-only / 82754a6 | DD Kalman tracker | Delta |
| --- | ---: | ---: | ---: |
| good/snr20/file20480 | 1497 bps, 58 retx, 56 timeouts | 1814 bps, 32 retx, 30 timeouts | +317 bps |
| good/snr20/file5120 | 1684 bps, 8 retx, 8 timeouts | 1723 bps, 6 retx, 5 timeouts | +39 bps |

Temporary mechanism probes, removed before commit, showed the tracker applied
about 23.8 DD carrier updates per OFDM symbol on the 20 KB run. Grouped logical
codeword failures dropped from 154 in the pilot-only probe to 8 with DD enabled.

The global nearest-constellation EVM probe did not show a clean reduction over
the full 20 KB run, so the current finding is that DD helps frame survival and
ARQ behavior, but the residual/EVM metric is not yet a clean promotion proof.

## Tracker Model

- Prediction: per-carrier random-walk `H[k]`, with process variance set from
  the existing Good-channel 5%/symbol fading model.
- Pilot anchor: pilot interpolation is treated as a measurement with pilot LS
  noise plus a 0.5 ms / five-carrier interpolation uncertainty floor.
- DD measurement: `H_dd = Y / X_hat`, with measurement variance from FFT-bin
  noise divided by decided-symbol power and inflated by decision reliability.
- Error guard: DD updates require 95% noise-model EVM, half-cell QAM16 spacing,
  and at least 9:1 bit posterior odds; high whole-symbol residual freezes DD.
- Step bound: DD innovation is clipped to 30% of carrier channel magnitude.

## Round 5 Candidate Lever

The next lever should not be another re-anchor cadence. The evidence points to
QAM16 still being marginal during long Good-channel fades even after per-carrier
DD tracking. Round 5 should evaluate one of these promotion-safe paths:

1. Add a coherent-QPSK fallback below a measured QAM16 residual/FER floor.
2. Add coded-decision feedback for DD, where only codewords that pass LDPC/CRC
   reinforce the channel state across a burst.
3. Calibrate QAM16 LLRs from post-DD residual variance instead of relying only
   on LTS noise plus magnitude variance.

Any QAM16 ladder integration should wait until the long-fade floor is proven
across seeds, because the current tracker does not yet clear the target.
