# Comprehensive Calibration Audit

Branch: `fix/honest-snr-in-band-and-rate-recalibration`

## Layer 1: Calibration constants

Status: measured with a code fix prepared in the worktree. Local git commit is
blocked in this sandbox because `.git/index.lock` cannot be created
(`Operation not permitted`).

Scope: Layer 1 covered `src/sim/channel_calibration.hpp`, the mirrored channel
core constants in `src/ota_channel_core/ota_channel_core/models.hpp`, SNR
reference consumers in idle-noise metering, OTASim/real-HF-loop scaling,
Watterson/AWGN channel models, and OFDM LTS/pilot SNR conversion. Sync
thresholds, LLR gates, rate selector thresholds, and waveform parameters are
reserved for Layers 4-8.

### Audit

Re-derived constants from first principles and verified them with the actual
modem code path:

| Quantity | Measured value | Layer-1 verdict |
|----------|----------------|-----------------|
| `StreamingEncoder::encodePing()` samples | `62208` | correct source signal |
| PING broadband RMS | `0.318072406640` | correct as broadband TX amplitude |
| PING broadband power | `0.101170055866` | correct as broadband TX power |
| RX FIR energy, `sum(h^2)` | `0.108587175481` | matches `kModemInBandNoisePowerFraction` |
| Broadband-to-in-band offset | `9.642214453172 dB` | matches `kModemBroadbandToInBandSnrOffsetDb` |
| PING in-band RMS after RX FIR | `0.304826641347` | should be the calibrated SNR reference |
| PING in-band power fraction | `0.918446475886` | signal loses `0.369461479098 dB` in the RX bandpass |

Finding: the FIR noise fraction and broadband-to-in-band offset were correct,
but `kModemReferenceRms`/`kModemReferencePower` still used the broadband PING
power while the operator-facing SNR path compares against receiver in-band
noise. That made the calibrated SNR reference `0.369461479098 dB` high.

Multi-perspective check:

- PHY: the SNR numerator and denominator must share the same 50-2950 Hz
  in-band convention; using broadband signal power with in-band noise is a
  unit mismatch.
- DSP: the result was measured through the real `StreamingEncoder::encodePing`
  path and the exact 101-tap Blackman FIR used by the channel calibration.
- HF operator: the change is a small but real label/gain correction, matching
  the expected size for filtering a narrow modem waveform into the receiver
  passband.
- Physics: Parseval/FIR-energy accounting explains the noise bandwidth
  constant; no fitted dB offset is needed.

### Fix

Prepared worktree changes:

- Split the reference into `kModemReferenceBroadbandRms` and
  `kModemReferenceInBandRms`.
- Set `kModemReferenceRms` and `kModemReferencePower` to the in-band PING
  reference.
- Mirrored the same constants in `ota_channel_core`.
- Updated AWGN regression expectations and calibration tests so future drift
  must pass through the actual encoder/FIR derivation.

Regression status:

- `cmake --build build -j4`: passed.
- Targeted calibration tests:
  `ChannelCoreModels`, `ChannelSNRCalibration`,
  `ChannelModemSNRMeterCalibration`, and
  `ChannelIdleNoiseSNRCalibration`: passed `4/4`.
- Non-socket ctest gate: passed `81/81`.
- Full `86/86` ctest cannot complete in this sandbox because five socket
  smoke tests fail before modem code runs with local TCP/UDP bind errors:
  `GrpcServiceSmoke`, `CLISyntheticNotch`, `OtasimServeSmoke`,
  `UltraGuiOtaClient`, and `UltraTncSimAudio`.
  `OTASimulatorFileTransferSNR15` failed once under the full parallel run but
  passed on rerun and in the non-socket gate.

### Measure

Required Layer 1 MC-DPSK QSO floor sweep through OTASim:

| SNR dB | Result | Notes |
|--------|--------|-------|
| 18 | pass | 5 assertions passed, 11 RX frames |
| 16 | fail | Bob detected Alice PING twice; Alice did not connect |
| 14 | fail | no decoded frames before connect assertions failed |
| 12 | fail | no decoded frames before connect assertions failed |
| 10 | fail | no decoded frames before connect assertions failed |

Because the numerical fix is smaller than the required 2 dB grid, a 17 dB
refinement was run. It passed with the full PING/PONG/CONNECT/CONNECT_ACK/DATA/
ACK/DISCONNECT path.

Layer 1 MC-DPSK floor delta: improved from `18 dB` to `17 dB`.

Since MC-DPSK improved, the required cascade OFDM forced R1/4 floor sweep was
run:

| SNR dB | Result | Notes |
|--------|--------|-------|
| 20 | pass | 5 assertions passed, 9 RX frames |
| 18 | pass | 5 assertions passed, 9 RX frames |
| 16 | pass | 5 assertions passed, 9 RX frames |
| 14 | pass | 5 assertions passed, 9 RX frames |
| 12 | fail | 3 assertions passed, 2 failed, 5 RX frames |

Cascade OFDM R1/4 floor delta: improved from `18 dB` to `14 dB`.

Layer 1 verdict: one numerical calibration error was fixed. The remaining
MC-DPSK failure below 17 dB is handshake/sync limited rather than explained by
Layer 1 calibration constants.

## Layer 2: Channel models

Status: audited, fixed, measured, and ready to commit.

Scope: Layer 2 covered `src/ota_channel_core/models.cpp`, including AWGN noise
generation, AWGN spectrum, Watterson preset delays/Doppler/noise, real-HF-loop
normalization/scaling, and Watterson CFO injection.

### Audit

AWGN noise generation and spectrum:

| Quantity | Expected | Measured | Layer-2 verdict |
|----------|----------|----------|-----------------|
| AWGN mean at 18 dB | `0` | `0.0000767593` | correct |
| AWGN broadband variance at 18 dB | `0.0135621` | `0.0134855` | correct, `-0.57%` |
| AWGN spectrum flatness | white across sampled 0-24 kHz bins | `0.75261 dB` max/min band spread | correct |

Derivation: the simulator's SNR knob is in-band SNR. White broadband sigma is
therefore `kModemReferenceRms * 10^(-(snr_db -
kModemBroadbandToInBandSnrOffsetDb)/20)`, so the 50-2950 Hz receiver bandpass
sees `kModemReferencePower * 10^(-snr_db/10)` noise power. With zero input,
`AWGNChannelModel` should produce zero-mean independent samples with that
broadband variance and a flat periodogram.

Multi-perspective check:

- PHY: the variance uses the Layer 1 in-band reference and the broadband
  sigma expansion required for white noise before receiver filtering.
- DSP: the actual `AWGNChannelModel` path was run for 65,536 samples; sampled
  periodogram bands stayed within `0.75261 dB`.
- HF operator: AWGN remains a neutral lab baseline; no colored-noise or
  in-band label bias was found in this model.
- Physics: Parseval/noise-bandwidth accounting matches the variance and
  spectrum measurements.

Watterson taps, Doppler, and noise:

| Quantity | Expected | Measured | Layer-2 verdict |
|----------|----------|----------|-----------------|
| Good delay | `0.5 ms = 24 samples` | impulse path at sample `24` | correct |
| Moderate delay | `1.0 ms = 48 samples` | impulse path at sample `48` | correct |
| Poor delay | `2.0 ms = 96 samples` | impulse path at sample `96` | correct |
| Watterson noise variance at 18 dB | `0.0135621` | `0.0134592` | correct, `-0.76%` |
| Flutter fading mean-square gain | near unity | `1.13437` over 65,536 samples | correct within stochastic tolerance |

Derivation: delays are `delay_spread_ms * sample_rate / 1000`, so the three
shipping presets resolve to 24/48/96 samples at 48 kHz. The tap amplitudes are
`0.707` and `0.707`, giving approximately unit two-path power before fading.
The Doppler parameter maps to the one-pole fading update
`alpha = 1 - exp(-2*pi*doppler_hz/sample_rate)`. This is an envelope-fading
HF simulator, not a full complex-baseband ionospheric channel, but the audited
tap timing, stochastic gain normalization, and injected noise power match the
implementation's stated SNR contract.

Multi-perspective check:

- PHY: two equal taps give unity nominal multipath power; the one-pole Doppler
  envelope is normalized so mean-square path gain stays near one.
- DSP: impulse tests verified each preset delay, zero-input Watterson noise
  matched AWGN variance, and a constant-input flutter run measured
  mean-square gain `1.13437`.
- HF operator: Good/Moderate/Poor/Flutter order still maps to increasing delay
  and Doppler severity; no hidden gain offset was found in the presets.
- Physics: energy accounting is consistent for the two-tap model; the
  stochastic variation observed is expected for finite Rayleigh-like fading
  samples.

real-HF-loop normalization and scaling:

| Quantity | Expected | Measured | Layer-2 verdict |
|----------|----------|----------|-----------------|
| WAV load normalization | broadband RMS normalized to `1.0` | existing loader divides by measured RMS | correct |
| 12 dB target in-band noise power | `0.00586281` | `0.00586310` | correct, `0.000210984 dB` high |
| Phase/loop wrap | seeded start and wrap deterministic | covered by `RealHfLoopChannel` | correct |

Derivation: `loadRealHfLoopNoiseBedWav` normalizes the input noise bed to unit
broadband RMS. `RealHfLoopChannelModel` then measures the loop's actual
50-2950 Hz in-band power with the same 101-tap FIR and scales by
`sqrt(target_in_band_power / loop_in_band_power)`, where
`target_in_band_power = kModemReferencePower * 10^(-snr_db/10)`.

Multi-perspective check:

- PHY: scaling is in received in-band power, not broadband WAV RMS, which is
  the right convention for colored real-HF noise beds.
- DSP: a unit-RMS 1 kHz loop at configured 12 dB measured `0.00586310`
  in-band power against the `0.00586281` target.
- HF operator: replayed noise beds can have arbitrary spectral shape without
  moving the operator-facing SNR label.
- Physics: the scale equation is direct power normalization; no empirical dB
  fudge factor is present.

CFO injection:

| Quantity | Expected | Old measured | Fixed measured | Layer-2 verdict |
|----------|----------|--------------|----------------|-----------------|
| 2783.20 Hz tone with +37 Hz CFO | output near 2820.20 Hz, RMS preserved | RMS ratio `0.201606` | estimated `2820.03 Hz`, RMS ratio `1.00002` | fixed |

Finding: `WattersonChannel::applyCFO` was not a pure frequency translator. It
downmixed around a hard-coded `1500 Hz`, smoothed I/Q with a 48-sample moving
average, then remodulated. That low-passed the offset-from-1500 Hz component,
so tones near the modem band edge were attenuated by about `13.9 dB` even
though a real carrier-frequency offset should shift all positive-frequency
audio components by the same offset and preserve power. The file already had
an analytic-signal frequency shifter; `applyCFO` now uses that path.

Multi-perspective check:

- PHY: CFO is multiplication by `exp(j*2*pi*df*t)` on the analytic signal; it
  should translate frequency, not impose an audio-band gain curve.
- DSP: the regression test fails before the fix with RMS ratio `0.201606` and
  passes after the fix with estimated frequency `2820.03 Hz` and RMS ratio
  `1.00002`.
- HF operator: oscillator offset should not make high audio carriers disappear
  in the channel model; the old simulator overstated CFO damage near band
  edges.
- Physics: ideal frequency translation is unitary, so signal energy is
  preserved apart from finite-window edge effects.

### Fix

Code change:

- Replaced the custom Watterson CFO downmix/moving-average/remix path with the
  existing analytic-signal shifter in `WattersonChannel::applyCFO`.
- Extended `ChannelCoreModels` regression coverage for AWGN statistics,
  spectrum flatness, Watterson noise/fading normalization, real-HF in-band
  scaling, and band-edge CFO frequency/RMS preservation.

Regression status:

- `cmake --build build --target test_channel_core_models -j4`: passed.
- `./build/tests/test_channel_core_models`: passed and printed the Layer 2
  empirical measurements above.

### Measure

Required Layer 2 MC-DPSK QSO floor sweep through OTASim:

| SNR dB | Result | Notes |
|--------|--------|-------|
| 18 | pass | `OTASimulatorTwoEndpointMCDPSKLowSNR`, 5 assertions passed |
| 17 | pass | refinement, 5 assertions passed |
| 16 | fail | 2 assertions passed, 3 failed; Alice still `PROBING`, Bob `DISCONNECTED` at 35 s |
| 14 | fail | 2 assertions passed, 3 failed; Alice still `PROBING`, Bob `DISCONNECTED` at 35 s |
| 12 | fail | 2 assertions passed, 3 failed; Alice still `PROBING`, Bob `DISCONNECTED` at 35 s |
| 10 | fail | 2 assertions passed, 3 failed; Alice still `PROBING`, Bob `DISCONNECTED` at 35 s |

Layer 2 MC-DPSK floor delta: no change; floor remains `17 dB`.

Cascade decision: no OFDM cascade sweep was required for Layer 2 because the
MC-DPSK floor did not improve.

Layer 2 verdict: one real channel-model bug was fixed in Watterson CFO
injection. AWGN, Watterson noise/taps, and real-HF-loop SNR scaling matched the
in-band calibration contract. The AWGN MC-DPSK floor remains handshake/sync
limited below 17 dB, so the next expected payoff is still Layer 4 chirp/sync
rather than another channel-model calibration constant.

## Layer 3: SNR estimators

Status: audited, one routing bug fixed, measured, and ready to commit.

Scope: Layer 3 covered `IdleNoiseSNREstimator`, the OFDM LTS/pilot residual
meter exposed through `getLastOFDMBroadbandSNREstimate()`, the demodulator
internal `getEstimatedSNR()` scale, and the chirp correlation confidence score
used by disconnected sync.

### Audit

Idle-noise estimator:

| Quantity | Expected | Measured | Layer-3 verdict |
|----------|----------|----------|-----------------|
| FIR convention | 50-2950 Hz receiver in-band power | `normalized_noise_rms` is FIR-output RMS | correct |
| FIR energy / ENBW | `sum(h^2)=0.10858718`, ENBW `2606.09 Hz` | `0.10858718`, `2606.09 Hz` | correct |
| AWGN SNR delta | reported equals independent in-band reference | max absolute delta `0.00 dB` over -6..24 dB | correct |
| real-HF-loop SNR delta | reported equals independent in-band reference | max absolute delta `0.00 dB` over -6..24 dB | correct |

Derivation: the idle estimator is an idle-noise meter, so the signal numerator
is the calibrated in-band PING reference power and the denominator is the
receiver passband noise power measured by the same 101-tap 50-2950 Hz FIR:

`SNR_in_band = 10*log10(kModemReferencePower / E{y_fir^2})`.

The old broadband extrapolation (`E{y_fir^2}/sum(h^2)`) would only be valid for
white input noise and is intentionally absent. The current formula has matching
in-band numerator and denominator units for both AWGN and colored real-HF noise.

Multi-perspective check:

- PHY: the estimator reports received in-band signal/noise, not audio
  broadband-equivalent SNR.
- DSP: `test_idle_noise_snr_calibration` generated the actual AWGN and
  real-HF-loop channel paths, independently filtered the received samples, and
  matched the estimator with zero measurable delta.
- HF operator: an idle meter should rise when the actual receiver passband gets
  quieter; it should not penalize colored HF noise for not being white outside
  the modem band.
- Physics: Parseval/FIR energy gives the white-noise ENBW, while colored-noise
  power must be measured directly in the receiver passband.

OFDM LTS/pilot residual meter:

| Channel | Configured in-band SNRs | Mean measured delta | Layer-3 verdict |
|---------|--------------------------|---------------------|-----------------|
| AWGN | -5, 0, 5, 10, 15, 20 dB | `+0.24` to `+0.25 dB` | correct |
| AWGN idle-vs-OFDM cross-check | same seeds | OFDM `+0.27` to `+0.28 dB` over idle | correct |
| Good Watterson | -5, 0, 5, 10, 15, 20 dB | `+0.07` to `+0.08 dB` | correct |
| Moderate Watterson | -5, 0, 5, 10, 15, 20 dB | `+0.01` to `+0.08 dB` | correct |

Derivation: after downconversion, real white audio noise with broadband
variance `sigma^2` contributes `N*sigma^2` power to an unnormalized N-point FFT
bin. The channel knob is in-band SNR, so `sigma^2 =
kModemReferencePower/(SNR_in_band * kModemInBandNoisePowerFraction)`. The OFDM
meter therefore first recovers broadband SNR from the FFT-bin noise reference,
then applies `broadbandToInBandSnrDb()`. The two-LTS time-difference residual
uses `E{|H1-H0|^2}/4`, which is half a single-symbol FFT-bin noise reference,
so the meter scales that residual by `2.0`. Guard-bin noise is already a
single-symbol FFT-bin reference and uses scale `1.0`.

The accessor and source token still say `ofdm_broadband` for wire/API
compatibility, but the value is now in-band SNR. This was verified by
`test_modem_snr_meter_calibration`, including a same-channel AWGN cross-check
against the idle estimator.

Multi-perspective check:

- PHY: the FFT-bin noise derivation and two-LTS factor follow directly from the
  OFDM receiver math; averaging bins reduces variance, not RF gain.
- DSP: the actual `OFDMChirpWaveform`/`OFDMDemodulator` path measured within
  `0.3 dB` of the configured AWGN in-band SNR and within `0.1 dB` on the
  Watterson presets used by the test.
- HF operator: OFDM residual SNR now reads on the same in-band scale as the idle
  meter, so the operator does not see a 9.6 dB source-dependent jump.
- Physics: FFT noise power and FIR noise bandwidth account for the full
  conversion; no fitted offset remains.

OFDM internal SNR:

`OFDMDemodulator::getEstimatedSNR()` returns
`10*log10(estimated_snr_linear)`. That value is the demodulator's internal
LLR/channel-quality scale, initialized from LTS signal/noise and then preserved
for differential modes so pilot temporal fading does not compress LLRs. It is
not a physical in-band SNR meter and must not feed mode or rate selection.

The audit verified the call routing:

- `populateDecodeMetrics()` exposes it only as `ofdm_internal_snr_db`.
- `result.snr_db` for OFDM is overwritten by the OFDM residual in-band meter
  when that meter is valid.
- Operator display uses idle in-band first, then OFDM residual in-band, and
  never OFDM internal.

Finding: protocol `Connection::setMeasuredSNR()` and `setChannelQuality()`
accepted any `SNRSource`. That meant a future or TNC-side caller could store
`OFDM_INTERNAL` as `measured_snr_db_`, even though the value is explicitly not a
rate-selection SNR.

Multi-perspective check:

- PHY: an LLR scale is receiver-internal confidence, not a calibrated
  signal/noise ratio.
- DSP: the new protocol regression injects `OFDM_INTERNAL=35 dB` before
  negotiation and verifies it cannot promote the link.
- HF operator: the radio should not change modes based on a private demodulator
  confidence number.
- Physics: no absolute received noise reference exists in this value, so it has
  no physical SNR units.

Chirp sync confidence:

`StreamingDecoder::chirpSyncQualityDb()` computes
`clamp((correlation - 0.15)/0.03, -5, 30)`. The `noise_floor` argument is unused.
This is a correlation confidence score, not an SNR estimator. The detector's
physical evidence is the dual-chirp correlation and gap/CFO consistency; the
reported "dB" is only a legacy quality scale.

Finding: the display path already rejected `SYNC_QUALITY` as an operator SNR,
but the protocol path did not. `ultra_tnc` can call `setMeasuredSNR(...,
SYNC_QUALITY)` on ping detection, and decoded non-OFDM frames can also carry
`SYNC_QUALITY` if no idle estimate is available. Before this fix, that value
could feed the CONNECT responder's adaptive ladder and make chirp correlation
look like calibrated SNR.

Multi-perspective check:

- PHY: normalized chirp correlation is a detection statistic; converting it by
  a linear threshold mapping does not create signal/noise units.
- DSP: the new regression injects `SYNC_QUALITY=30 dB` before CONNECT and
  verifies the responder stays on the conservative MC-DPSK fallback instead of
  promoting to OFDM.
- HF operator: a strong sync lock should not be displayed or negotiated as a
  30 dB channel.
- Physics: processing gain belongs in detector probability-of-detection math;
  this score has no calibrated noise bandwidth or signal-power reference.

### Fix

Code change:

- `Connection::setMeasuredSNR()` and `Connection::setChannelQuality()` now
  accept only rate-selection-safe sources: `NONE` for explicit simulator/harness
  truth, `IDLE_IN_BAND`, and `OFDM_BROADBAND` (historical token for OFDM
  residual in-band SNR).
- `SYNC_QUALITY`, `OFDM_INTERNAL`, and non-finite values are ignored by the
  protocol rate-selection state.
- `setChannelQuality()` updates the fading index only when the SNR source is
  rate-selection-safe and the fading value is finite.
- Added a protocol regression that preloads both `SYNC_QUALITY=30 dB` and
  `OFDM_INTERNAL=35 dB`; the responder must still negotiate MC-DPSK from the
  conservative default rather than promote to OFDM.

Regression status:

- `cmake --build build --target test_protocol test_snr_source_routing test_idle_noise_snr_calibration test_modem_snr_meter_calibration -j4`: passed.
- `./build/tests/test_protocol`: passed `24/24`.
- `./build/tests/test_snr_source_routing`: passed.
- `./build/tests/test_idle_noise_snr_calibration`: passed `312` checks.
- `./build/tests/test_modem_snr_meter_calibration`: passed `378` checks.

### Measure

Required Layer 3 MC-DPSK QSO floor sweep through OTASim:

| SNR dB | Result | Notes |
|--------|--------|-------|
| 18 | pass | full scenario passed |
| 17 | pass | refinement to compare against Layer 1/2 floor |
| 16 | fail | 3 failed assertions; Alice `PROBING`, Bob `DISCONNECTED` at 35 s, message absent |
| 14 | fail | same 3 failed assertions |
| 12 | fail | same 3 failed assertions |
| 10 | fail | same 3 failed assertions |

Layer 3 MC-DPSK floor delta: no change; refined floor remains `17 dB`.

Cascade decision: no OFDM cascade sweep was required for Layer 3 because the
MC-DPSK floor did not improve.

Layer 3 verdict: the four estimators now have documented units and empirical
checks. One silent source-routing bug was fixed so chirp confidence and OFDM
internal quality cannot drive protocol rate selection. The remaining MC-DPSK
floor below 17 dB is still sync/handshake-limited, so Layer 4 remains the
expected high-payoff layer.
