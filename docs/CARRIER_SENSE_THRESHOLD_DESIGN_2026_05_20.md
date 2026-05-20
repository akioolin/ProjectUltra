# Carrier-Sense Adaptive Threshold Design - 2026-05-20

## Diagnosis

I agree with the contamination diagnosis.

`ChannelBusyDetector::observeRms()` appends every non-local-blackout RMS sample
to `noise_floor_window_` before computing the quiet threshold. Then
`quietThresholdLocked()` sorts that mixed window and uses the configured 10th
percentile as the "noise floor". During a long OFDM ARQ burst, the five-second
history can be dominated by active carrier RMS. The lower percentile is then
still signal energy, so the adaptive threshold can climb into the
`0.25`-range and classify signal-level `window_rms` as quiet.

That is not a real noise-floor estimator. It is a lower-tail estimator over the
whole receive stream.

## Chosen Fix

Use threshold-gated noise sampling with an explicit bootstrap ceiling.

The detector keeps two separate jobs:

1. Carrier-state detection still consumes every non-blackout RMS sample through
   the short `rms_window_`.
2. Adaptive noise-floor estimation only records samples that are plausible idle
   noise, not samples that are already at carrier-signal level.

The noise-floor candidate rules are:

- Local TX/RX blackout remains excluded.
- Before enough accepted noise samples exist, accept only samples at or below a
  conservative bootstrap RMS ceiling. The default ceiling is `0.11`, high enough
  for the calibrated AWGN idle floor around SNR 12-14, but below the observed
  OFDM signal RMS range of about `0.18` to `0.30`.
- Once a floor exists, accept only samples at or below
  `noise_floor * quiet_noise_multiplier`. With the default multiplier `1.5`,
  a SNR 14 AWGN floor near `0.06` admits idle-noise variation up to about
  `0.09` and rejects the observed active-carrier range.
- If an active burst lasts longer than the noise history, the accepted noise
  history is allowed to expire. The detector then falls back to the fixed floor
  rather than relearning the burst as "noise". This can be over-sensitive for a
  short time after a long busy interval, but it is half-duplex safe.

## Four-Lens Check

- PHY theorist: the threshold is based on accepted idle-noise samples, not the
  lower percentile of active carrier energy, so the busy detector remains below
  signal-level RMS.
- Real-time DSP systems engineer: the update is O(n) only when computing the
  existing percentile, keeps bounded five-second history, and adds no decoder
  coupling or blocking path.
- Veteran HF operator: the adaptive squelch tracks the S-meter's quiet-channel
  floor. Active transmissions may make the channel busy, but they do not retune
  the quiet threshold upward.
- First principles: ordering all recent RMS values is valid only when the lower
  tail is actually idle noise. The new estimator makes that assumption explicit
  by rejecting samples that are above the current idle-noise gate.

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
