# Carrier-Sense Adaptive Threshold Design - 2026-05-20

## Diagnosis

The original contamination diagnosis is real, but it is not the whole
load-bearing explanation for the `0.26` threshold seen at AWGN SNR 14.

Code evidence:

- `ChannelBusyDetector::observeRms()` appended every non-local-blackout RMS
  sample to `noise_floor_window_`.
- `quietThresholdLocked()` then used the configured lower percentile over that
  mixed stream as the noise estimate.

That is structurally wrong because a long active burst can teach the estimator
that signal energy is the quiet floor.

The first verification run also exposed a measurement-domain problem: carrier
sense was taking RMS over raw broadband samples. The OTA AWGN model deliberately
raises broadband white-noise sigma so the receiver FIR leaves the configured
50-2950 Hz in-band noise power. At SNR 14, the expected in-band noise RMS is
about `0.06`, but the expected raw broadband RMS is about `0.18`. Therefore an
adaptive threshold near `0.26` is also consistent with raw broadband AWGN
multiplied by `1.5`, not only with signal contamination.

The detector must estimate and threshold the same receive-channel quantity the
modem actually cares about: in-band RMS.

## Chosen Fix

Use in-band RMS plus threshold-gated noise sampling with an explicit bootstrap
ceiling.

The detector keeps two separate jobs:

1. Carrier-state detection consumes every non-blackout audio block after a
   50-2950 Hz receive-band FIR, then feeds that in-band RMS through the short
   `rms_window_`.
2. Adaptive noise-floor estimation only records samples that are plausible idle
   noise, not samples that are already at carrier-signal level.

The noise-floor candidate rules are:

- Local TX/RX blackout remains excluded.
- Before enough accepted noise samples exist, accept only samples at or below a
  conservative bootstrap RMS ceiling. The default ceiling is `0.11`, high enough
  for the calibrated in-band AWGN idle floor around SNR 12-14, but below the
  observed OFDM signal RMS range of about `0.18` to `0.30`.
- Once a floor exists, accept only samples at or below
  `noise_floor * quiet_noise_multiplier`. With the default multiplier `1.5`,
  a SNR 14 AWGN floor near `0.06` admits idle-noise variation up to about
  `0.09` and rejects the observed active-carrier range.
- If an active burst lasts longer than the noise history, the accepted noise
  history is allowed to expire. The detector then falls back to the fixed floor
  rather than relearning the burst as "noise". This can be over-sensitive for a
  short time after a long busy interval, but it is half-duplex safe.

## Four-Lens Check

- PHY theorist: the threshold is based on accepted in-band idle-noise samples,
  not raw broadband noise and not the lower percentile of active carrier energy.
- Real-time DSP systems engineer: the update is O(n) only when computing the
  existing percentile, keeps bounded five-second history, uses a small stateful
  FIR per detector, and adds no decoder coupling or blocking path.
- Veteran HF operator: the adaptive squelch tracks the S-meter's quiet-channel
  passband floor. Active transmissions may make the channel busy, but they do
  not retune the quiet threshold upward.
- First principles: ordering all recent RMS values is valid only when the lower
  tail is actually idle noise. The new estimator makes that assumption explicit
  by measuring in the receive band and rejecting samples that are above the
  current idle-noise gate.

## Expected Verification

For AWGN SNR 14, DQPSK R1/4 OFDM_CHIRP:

- Accepted idle floor should settle near `0.06`.
- Quiet threshold should stay near `0.09`, not `0.26+`.
- Signal windows around `0.18` to `0.30` should remain busy.
- The end-to-end run should still pass, with retransmissions reduced from the
  observed `7` toward `<=3`.

For AWGN SNR 30:

- The cleaner idle floor should stay lower.
- The fix must not introduce retransmissions in the clean-channel baseline.
