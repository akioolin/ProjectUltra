# Honest OFDM SNR Estimation

## 2026-05-14 Phase 1 Feasibility

Scope: estimator-only. No wire format, channel calibration, or mode-ladder
thresholds changed.

Patch summary:
- `src/ofdm/channel_equalizer_lts.cpp`: adds `last_snr_db_estimate` updates from
  LTS residual noise, converted to the calibrated broadband noise reference.
- `src/ofdm/channel_equalizer_pilot.cpp`: keeps the SNR estimate anchored to the
  LTS residual because transfer-sized Phase-1 runs showed temporal pilot-channel
  residuals include channel-estimate motion during longer OFDM frames. Pilots
  still update fading and equalization.
- `include/ultra/ofdm.hpp`, `src/ofdm/demodulator_impl.hpp`, and
  `src/ofdm/ofdm_stream_processor.cpp`: expose/reset `hasLastSNREstimate()` and
  `getLastSNREstimate()`.
- `src/waveform/*ofdm*` and `src/waveform/waveform_interface.hpp`: expose the
  OFDM estimate through waveform wrappers while non-OFDM keeps no pilot estimate.
- `src/gui/modem/streaming_*`: logs pilot/LTS SNR alongside chirp-derived SNR and
  fading index without changing `frame.rx.snr_db` in Phase 1.
- `tools/ofdm_snr_probe.cpp`: adds a direct one-frame OFDM probe through
  `SimulatedChannel`, OFDM demodulation, and fixed-frame LDPC decode.

Sweep command:

```sh
mkdir -p /tmp/honest_snr_phase1
{
  echo "channel,configured_snr,rate,success,cw_ok,cw_failed,sync_snr_db,pilot_snr_db,lts_snr_db,fading_index"
  for ch in awgn good moderate; do
    for snr in 20 15 10 5 0 -3 -5; do
      ./build/ofdm_snr_probe --no-header --snr "$snr" --channel "$ch" --rate r1_2 || true
    done
  done
} | tee /tmp/honest_snr_phase1/sweep_r1_2.csv
```

Correlation summary:

| Channel | Points | Pearson r | Bias dB | MAE dB | RMSE dB | Decoded-frame bias dB | Decoded-frame MAE dB |
|---------|--------|-----------|---------|--------|---------|------------------------|----------------------|
| AWGN | 7 | 1.000 | +3.51 | 3.51 | 3.51 | +3.51 | 3.51 |
| Good | 7 | 0.987 | +1.66 | 2.82 | 3.06 | -0.69 | 2.02 |
| Moderate | 7 | 0.980 | +1.09 | 3.13 | 3.40 | -3.56 | 3.56 |

Raw sweep:

| Channel | Configured SNR | Success | CW ok/fail | Chirp SNR | Pilot/LTS SNR | LTS internal SNR | Fading |
|---------|----------------|---------|------------|-----------|---------------|------------------|--------|
| AWGN | +20 | yes | 4/0 | 27.49 | 23.50 | 29.32 | 0.02 |
| AWGN | +15 | yes | 4/0 | 0.00 | 18.50 | 24.29 | 0.04 |
| AWGN | +10 | yes | 4/0 | 0.00 | 13.50 | 19.29 | 0.07 |
| AWGN | +5 | yes | 4/0 | 0.00 | 8.55 | 14.46 | 0.11 |
| AWGN | 0 | no | 1/3 | 0.00 | 3.53 | 9.92 | 0.17 |
| AWGN | -3 | no | 0/4 | 0.00 | 0.50 | 7.61 | 0.18 |
| AWGN | -5 | no | 0/4 | 0.00 | -1.50 | 6.38 | 0.17 |
| Good | +20 | yes | 4/0 | 23.71 | 16.19 | 15.26 | 0.23 |
| Good | +15 | yes | 4/0 | 0.00 | 14.75 | 13.83 | 0.22 |
| Good | +10 | yes | 4/0 | 0.00 | 11.99 | 11.24 | 0.19 |
| Good | +5 | no | 0/4 | 0.00 | 8.04 | 8.01 | 0.14 |
| Good | 0 | no | 0/4 | 0.00 | 3.46 | 5.41 | 0.11 |
| Good | -3 | no | 0/4 | 0.00 | 0.58 | 5.00 | 0.13 |
| Good | -5 | no | 0/4 | 0.00 | -1.38 | 5.00 | 0.14 |
| Moderate | +20 | yes | 4/0 | 24.01 | 14.46 | 15.25 | 0.28 |
| Moderate | +15 | yes | 4/0 | 0.00 | 13.41 | 14.31 | 0.29 |
| Moderate | +10 | no | 2/2 | 0.00 | 11.26 | 12.44 | 0.28 |
| Moderate | +5 | no | 0/4 | 0.00 | 7.79 | 9.70 | 0.24 |
| Moderate | 0 | no | 0/4 | 0.00 | 3.41 | 6.99 | 0.21 |
| Moderate | -3 | no | 0/4 | 0.00 | 0.61 | 5.81 | 0.21 |
| Moderate | -5 | no | 0/4 | 0.00 | -1.32 | 5.24 | 0.19 |

Three-perspective check:
- PHY theorist: PASS for feasibility. The estimator no longer saturates like
  chirp correlation, has strong monotonic correlation in all three channels, and
  separates noise SNR from fading by anchoring the frame estimate to the LTS
  residual instead of letting temporal pilot residuals masquerade as noise.
- Real-time DSP systems engineer: PASS for Phase 1. Runtime cost is already-paid
  pilot/LTS residual arithmetic plus scalar accumulation; no extra buffering or
  wire-format coupling was introduced.
- Veteran HF operator: PASS with caveat. AWGN and Good fading decoded frames
  report plausible band-facing SNR. Moderate fading at +20 dB reads lower
  because the frame is heavily selective; that is flagged for ladder review, not
  threshold tuning.

Decision: proceed to Phase 2. The LTS/pilot-residual estimate tracks configured
SNR instead of saturating. The direct one-frame probe has a +3.5 dB AWGN bias,
but transfer-sized streaming logs show the LTS residual centered closer to the
configured SNR; this is a verification focus for Phase 3 rather than a
fundamental saturation failure.

Follow-ups flagged, not fixed:
- The one-frame probe's chirp SNR column is only populated when the light-preamble
  detector accepts the frame; it is diagnostic context, not the new estimator.
- Moderate fading high-SNR cells can read several dB below configured SNR because
  the frame has real selective-fade penalty. Do not retune ladder thresholds in
  this phase.

## 2026-05-14 Phase 2 Wiring

Scope: OFDM decode metrics only. MC-DPSK remains chirp-derived.

Patch summary:
- `src/gui/modem/streaming_sync_acquisition.cpp`: `populateDecodeMetrics()` now
  replaces `DecodeResult::snr_db` with `getLastSNREstimate()` for OFDM frames
  once residual SNR is available. The chirp-derived value is still logged as
  `chirp_snr` and remains the fallback.
- `src/gui/modem/streaming_ofdm_decode.cpp`: the frame-decoded log now prints
  `result.snr_db`, so OFDM DATA logs show the value that enters frame metadata.
- `src/gui/modem/streaming_decoder.hpp`: `last_snr_` is mutable so the const
  metrics-population path can publish the residual-derived OFDM SNR.

Short software smoke:

```sh
for ch in awgn good moderate; do
  ./build/cli_simulator --snr 15 --channel "$ch" --rate r1_2 --file 256 \
    --log-level debug --log-category modem,demod,sync \
    --log-file "/tmp/honest_snr_phase2_${ch}15.log"
done
```

Representative OFDM DATA frames at configured SNR 15:

| Channel | Chirp SNR range | New frame SNR range | Result |
|---------|-----------------|---------------------|--------|
| AWGN | 25.2-25.8 dB | 11.5-15.0 dB | pass, file verified |
| Good | 17.2-26.3 dB | 12.7-17.7 dB | pass, file verified |
| Moderate | 20.2-25.6 dB | 8.6-13.4 dB | pass, file verified |

Three-perspective check:
- PHY theorist: PASS with caveat. The saturated chirp value no longer drives
  OFDM frame SNR, but per-frame residual estimates still have several dB of
  LTS-noise variance and must be judged by Phase-3 sweep averages.
- Real-time DSP systems engineer: PASS. The change is localized to metrics
  population and logging; no frame parser, encoder, ARQ, or wire-format path was
  touched.
- Veteran HF operator: PASS with caveat. The displayed DATA SNR now falls into
  plausible HF values instead of always reading mid/high 20s, but Moderate
  fading correctly reads worse than AWGN at the same configured noise setting.

Follow-ups flagged, not fixed:
- No mode-ladder thresholds were changed.
- The CONNECT/PING/control logs can still show chirp-derived SNR before OFDM
  residuals exist; this is intentional fallback behavior.
