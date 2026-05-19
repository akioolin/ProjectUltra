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

## Layer 4: Chirp/sync detector

Round 1 scope: diagnose the known MC-DPSK AWGN SNR 15 QSO failure and ship one
targeted fix only.

### Audit

Current chirp constants from `src/sync/chirp_sync.hpp`:

- Up chirp: 300 Hz to 2700 Hz over 0.5 s.
- Down chirp: 2700 Hz to 300 Hz over 0.5 s.
- Gap: 0.1 s, so expected up-to-down start gap is 28800 samples.
- Dual-chirp threshold: `0.15`; single-chirp `detect()` default threshold:
  `0.30`.
- Dual-chirp CFO sanity limit: `+/-100 Hz`, corresponding to `+/-1000`
  samples of chirp peak shift at `10 samples/Hz`.
- PING chirp-lock fallback in `streaming_frame_policy` requires correlation
  `>=0.30` and gap error `<=1000` samples.

Processing gain: the code uses `T=0.5 s` and `B=2400 Hz`, so one chirp has
`T*B=1200` or `30.8 dB` ideal time-bandwidth gain. The up+down pair has twice
the energy, `T*B=2400` or `33.8 dB` ideal gain before implementation losses.
That is close to the prior rough 36 dB expectation and still far below the
observed 17 dB CONNECT floor, so the failing gate needed to be found from logs.

Correlation false-alarm derivation: the detector uses normalized complex
correlation magnitude. For white noise and a 24000-sample chirp, a conservative
Rayleigh tail model gives:

`P(false peak > r) ~= exp(-N*r^2/2)`, with `N=24000`.

| Threshold | Per-position tail | Approx per 128K FFT search |
|-----------|-------------------|-----------------------------|
| 0.30 | `~1e-469` | `~1e-464` |
| 0.15 | `~5e-118` | `~7e-113` |
| 0.10 | `~8e-53` | `~1e-47` |
| 0.05 | `~9e-14` | `~1e-8` |

The dual-chirp validator adds a second chirp, gap consistency, and CFO sanity,
so the effective false alarm probability is lower than the single-peak table.
However, SNR 15 logs showed correlation around `0.907-0.909`, gap error `0.0`,
and CFO `0.00 Hz`. Therefore lowering the raw chirp threshold was not the
highest-payoff fix for this round.

Other Layer 4 gates:

- Gap tolerance is not the SNR 15 blocker: measured gap error was `0.0`
  samples against the `1000` sample PING fallback tolerance.
- CFO compensation is not the SNR 15 blocker: dual chirp estimated `0.00 Hz`
  and the correlation peak stayed above `0.90`.
- Search window is not the SNR 15 blocker: the FFT detector found both chirps
  repeatedly at the true PING/PONG positions.
- Coherent integration across retries is not implemented. It remains a
  possible later-layer improvement, but it was not required to fix this
  measured failure.
- Normalization is active: correlation divides by signal-window energy and
  template energy, so the observed `~0.908` statistic is not an input-RMS
  artifact.

### Finding

The SNR 15 failure was not a raw chirp miss. Bob detected Alice's PING with a
strong dual-chirp lock:

- `chirp_corr ~= 0.908`
- `gap_error = 0.0`
- `CFO = 0.00 Hz`

The first PING classifier path failed because the post-chirp region is no longer
quiet relative to the training chirp at low SNR:

- `ratio ~= 0.53-0.55`
- threshold is `0.50`

After that, the decoder's pre-LDPC LLR false-lock screen rejected the window
before the existing chirp-locked PING fallback could run:

- `False chirp lock rejected: |llr|_avg ~= 1.32 ... re-searching`

That kept Bob from emitting the PING event, so Bob never sent PONG, Alice never
sent CONNECT, and the scenario remained Alice `PROBING`, Bob `DISCONNECTED`.

Multi-perspective check:

- PHY: the detector had enough processing-gain evidence; the lost event was a
  classifier ordering error after successful synchronization.
- DSP: INFO logs from the failing QSO showed high correlation, zero gap error,
  zero CFO, and the pre-LDPC LLR gate as the rejecting branch.
- HF operator: a clean dual-chirp PING in a noisy idle passband should still
  wake the peer even if the following data-like region cannot decode as a frame.
- Physics: the high normalized matched-filter output and exact up/down spacing
  are stronger evidence than a low-SNR RMS silence heuristic.

### Fix

`StreamingDecoder::decodeCurrentFrame()` now runs the chirp-lock PING fallback
before advancing past a pre-LDPC LLR false-lock rejection. It only emits a PING
when `evaluatePingFrame()` reports `ping_by_chirp_lock`, so a random weak frame
still falls through to the existing false-lock rejection path.

The fix does not lower the global chirp threshold. It preserves the existing
multi-hypothesis validation: strong chirp correlation, bounded dual-chirp gap
error, and no valid LDPC frame.

Validation support: scenario v2 `channel.seed` is now parsed and passed to
`SimulatedChannel`, defaulting to the old seed 42 when omitted. This made the
requested seeded AWGN QSO validation reproducible without changing existing
fixtures.

Regression added:

- `test_streaming_frame_policy`: measured SNR 15 style case with
  `chirp_corr=0.908`, `gap_error=0`, no valid LDPC frame, and non-silent
  post-chirp RMS must classify as `ping_by_chirp_lock`.

### Measure

Pre-fix failing case:

- `./build/ota_simulator run --scenario /tmp/layer4_mcdpsk_snr15.json`
- Result: failed with 3 scenario assertions.
- State at 35 s: Alice `PROBING`, Bob `DISCONNECTED`.
- Gate: pre-LDPC LLR false-lock rejection before PING fallback.

Post-fix validation:

| Scenario | Seed | Result | Evidence |
|----------|------|--------|----------|
| MC-DPSK AWGN SNR 15 QSO | 42 | pass | CONNECT, DATA, ACK, DISCONNECT completed |
| MC-DPSK AWGN SNR 15 QSO | 43 | pass | CONNECT, DATA, ACK, DISCONNECT completed |
| MC-DPSK AWGN SNR 15 QSO | 44 | pass | CONNECT, DATA, ACK, DISCONNECT completed |

Targeted build/test status:

- `cmake --build build --target ota_simulator test_streaming_frame_policy -j4`:
  passed.
- `./build/tests/test_streaming_frame_policy`: passed `38/38`.

Layer 4 round-1 floor delta: MC-DPSK AWGN QSO moved from the Layer 3
`17 dB` floor to at least `15 dB` across three AWGN seeds. Per the
stop-after-measurable-move rule, lower SNRs were not swept in this round.
The next Layer 4 round should start by probing `14 dB` before attempting a
second independent improvement such as retry integration or threshold tuning.

## Layer 4: Round 2 and OFDM cascade

Round 2 scope: restart below the round-1 `15 dB` MC-DPSK floor, identify the
next sync gate, apply one targeted fix only, then run the requested OFDM forced
R1/4 cascade.

### MC-DPSK lower-SNR probe

Command family:

- Generated temporary scenario v2 files from
  `tests/fixtures/ota_simulator/two_endpoint_mcdpsk_low_snr_qso.json`.
- Set `channel.type=awgn`, `channel.seed={42,43,44}`, and swept SNR.
- Full per-run logs are under `/tmp/l4r2_mcdpsk_snr*_seed*_full.log`.
- Filtered audit log: `/tmp/codex_audit_and_chain_part5.log`.

Results:

| SNR dB | Seeds | Result | Chirp/sync evidence |
|--------|-------|--------|---------------------|
| 14 | 42,43,44 | 3/3 pass | PING chirp corr `~0.887-0.889`, gap error `0.0` |
| 13 | 42,43,44 | 3/3 pass | PING chirp corr `~0.862-0.865`, gap error `0.0` |
| 12 | 42,43,44 | 3/3 pass | PING chirp corr `~0.832-0.839`, gap error `0.0` |
| 11 | 42,43,44 | 3/3 pass | PING chirp corr `~0.803-0.809`, gap error `0.0` |
| 10 | 42,43,44 | 3/3 pass | PING chirp corr `~0.768-0.777`, gap error `0.0` |
| 9 | 42,43,44 | 3/3 pass | PING chirp corr `~0.731-0.740`, gap error `0.0` |
| 8 | 42,43,44 | 3/3 pass | PING chirp corr `~0.690-0.699`, gap error `0.0` |
| 7 | 42,43,44 | 3/3 pass | PING chirp corr `~0.647-0.655`, gap error `0.0` |
| 6 | 42,43,44 | 3/3 pass | PING chirp corr `~0.603-0.618`, gap error `0.0` |
| 5 | 42,43,44 | 3/3 pass | PING chirp corr `~0.557-0.570`, gap error `0.0` |

Finding: the MC-DPSK QSO target floor was reached without a new MC-DPSK code
change. The round-1 chirp-lock PING ordering fix was the actual blocker below
15 dB; after that fix, MC-DPSK DBPSK R1/4 completes CONNECT, DATA, ACK, and
DISCONNECT through `5 dB` AWGN across three seeds.

Candidate gate status:

- Coherent integration across PING retries: not required for AWGN down to
  `5 dB`; first visible PING/PONG locks were still dual-chirp-valid.
- Pre-LDPC LLR threshold: no longer blocks MC-DPSK PING after the round-1
  ordering fix; data/control frame decodes also completed at `5 dB`.
- Dual-chirp threshold: not binding; measured `5 dB` PING correlations remain
  `~0.56`, well above the `0.30` PING fallback floor and `0.15` dual-chirp
  detector floor.
- First-PING classifier ratio: still rises above `0.50` at low SNR, but the
  chirp-lock path correctly carries PING classification.

Multi-perspective check:

- PHY: the matched-filter evidence remains strong; even at `5 dB`, the
  normalized dual-chirp correlations are far above the current acceptance
  floors with exact up/down spacing.
- DSP: this is not a component-only chirp test; the seeded two-endpoint QSO
  completed all protocol frames at each SNR.
- HF operator: noisy post-chirp passband energy is expected at low SNR and
  should not suppress a geometrically valid chirp lock.
- Physics: the correlation trend decreases monotonically with SNR, but the
  time-bandwidth gain still leaves substantial acquisition margin at `5 dB`.

Layer 4 round-2 MC-DPSK verdict: no new MC-DPSK sync fix was justified. Per the
one-change rule, no extra PING/chirp threshold was changed without a failing
MC-DPSK gate.

### OFDM forced R1/4 cascade

Cascade scenario:

- Same two-endpoint QSO fixture.
- `force_connected_waveform=OFDM_CHIRP`.
- `force_data_mode=true` on both endpoints.
- Initial mode `MC_DPSK`, `DQPSK`, `R1_4`; this preserves the cold-call
  handshake while pinning the connected data path to OFDM DQPSK R1/4.

Pre-fix cascade:

| SNR dB | Seeds | Result | Failing gate |
|--------|-------|--------|--------------|
| 14 | 42,43,44 | 3/3 pass | None; OFDM DATA decoded `4/4` CWs and OFDM controls completed |
| 12 | 42,43,44 | 0/3 pass | Connected OFDM light-sync threshold delayed DATA and lost ACK/DISCONNECT completion |
| 10 | 42,43,44 | 0/3 pass | Connected OFDM DATA sync rejected repeatedly after successful CONNECT/CONNECT_ACK |

The 12 dB/10 dB cascade failures were not cold-call chirp failures. PING,
CONNECT, and CONNECT_ACK succeeded with strong dual-chirp evidence. The new gate
was connected OFDM LTS-only data/control sync:

- 12 dB DATA/control candidates often appeared around `0.40-0.45` correlation.
- The existing connected wideband non-coherent policy started at `0.52` and
  relaxed only to `0.45`.
- At 12 dB, successful DATA decodes occurred when a candidate eventually
  reached `~0.45-0.48`, but ACK/DISCONNECT completion was still inconsistent.
- At 10 dB, most connected OFDM candidates stayed below `0.40`; the handshake
  still completed, but data/control delivery did not.

Multi-perspective check:

- PHY: connected OFDM frames use LTS-only light preambles, so chirp fallback is
  impossible after CONNECT; the relevant detector is `detectDataSync()`.
- DSP: full logs showed repeated `DATA sync rejected` messages before missing
  message or disconnect assertions, and successful frame decodes immediately
  after late weak accepts at 12 dB.
- HF operator: the link sounded/behaved connected, but control tail frames
  needed retries because light-sync acquisition, not the ARQ state machine, was
  rejecting candidates.
- Physics: lowering a sync gate in the autocorrelation-noise region is risky,
  so the change must remain late, connected-only, and backed by downstream
  LLR/LDPC validation rather than unconditional acceptance.

### Fix

Code change:

- `streaming_signal_policy::lightSyncThresholds()` now has named constants for
  connected OFDM light-sync relaxation.
- Non-coherent connected OFDM still starts at `0.52`.
- After repeated rejects, the minimum confidence can now relax to `0.40`
  instead of `0.45`.
- After a longer reject streak, wideband non-coherent connected OFDM can weak
  accept down to `0.35`.
- Coherent modes, disconnected acquisition, and the initial cold-call chirp path
  are unchanged.

This is intentionally a late rescue path, not a global threshold lowering.
Candidates admitted by this path still have to pass the existing LLR and LDPC
frame decode gates before any protocol frame is accepted.

Regression added:

- `test_streaming_signal_policy` now checks the `0.40` connected OFDM
  relaxation floor and the late `0.35` rescue floor.
- The test also verifies a `0.36` connected OFDM rescue candidate is accepted
  as a weak sync after the configured reject streak.

### Validation

Targeted build/test:

- `cmake --build build --target test_streaming_signal_policy ota_simulator -j4`:
  passed.
- `./build/tests/test_streaming_signal_policy`: passed `65/65`.

Post-fix OFDM cascade:

| SNR dB | Seeds | Result | Evidence |
|--------|-------|--------|----------|
| 12 | 42,43,44 | 3/3 pass | Weak-accepted DATA/control syncs followed by valid DATA, ACK, DISCONNECT, and final ACK decodes |
| 10 | 42,43,44 | 0/3 pass | Handshake completes; DATA sometimes decodes late, but ACK/DISCONNECT control completion still fails |

Layer 4 round-2 floor delta:

- MC-DPSK AWGN QSO: `15 dB` after round 1 to `5 dB` after round-2 probe
  validation, with no additional MC-DPSK change required.
- OFDM forced DQPSK R1/4 AWGN QSO: `14 dB` to `12 dB` after one connected
  OFDM light-sync policy change.
- Remaining OFDM target gap: `10 dB` still fails 3/3. The next gate is
  connected OFDM LTS/control sync margin below `0.40`, not cold-call chirp
  acquisition.

Layer 5 status: not entered in this round. The cascade left an unresolved Layer
4 connected-OFDM sync gate at `10 dB`, so moving into LLR scaling before another
sync round would mix layers and violate the one-change protocol.

### Round-stop status

Commit not made in this sandbox. Targeted builds, policy regression tests, and
the direct multi-seed simulator sweeps above passed, but the required full ctest
baseline could not be preserved here:

- `ctest --test-dir build --output-on-failure`: `75/86` passed.
- Immediate environment-sensitive failures included gRPC service startup,
  ephemeral TCP bind, and UDP bind tests.
- The ctest log also reported `/bin/ps: Operation not permitted`, and multiple
  long OTA ctests timed out after the restricted process/socket failures.
- `OTASimulatorTwoEndpointMCDPSKLowSNR` also timed out when run by ctest, even
  though direct `ota_simulator run --scenario` low-SNR QSO validation reached
  `5 dB` at `3/3` seeds in this round.

Because the protocol requires the `86/86` ctest baseline to remain preserved,
this round stops with uncommitted working-tree changes for review or rerun in an
unrestricted environment. No push was performed.

## Layer 5: LLR scaling

Round 1 scope: diagnose the current forced OFDM DQPSK R1/4 AWGN 10 dB QSO
failure before changing any LLR/LDPC constants. The suspects were LDPC
iterations, LLR magnitude scaling, pre-LDPC LLR gates, LLR clipping, and
soft-combine HARQ.

Status: diagnosed, no code fix applied. The Layer 5 path is not the current
10 dB QSO gate.

### Audit

Current Layer 5 constants and gates:

- `CARRIER_ADAPTIVE_K = 10.0`: per-carrier noise inflation uses
  `nv *= (1 + K * norm_var)`, where `norm_var` is the EMA magnitude variance
  divided by squared EMA magnitude.
- `MAX_LLR = 20.0` and `MIN_LLR_MAG = 0.01`: final soft-demapper clipping and
  near-erasure floor.
- `kMinPreSyncLLR = 1.5`, `kMinLLRForSingleCWDecode = 1.5`, and
  `kMinLLRForEscalation = 1.5`: coarse false-lock gates before expensive LDPC
  work.
- R1/4 LDPC recommended iterations remain `50`, with fixed-frame decoder
  diversity retry factors after an initial miss.
- Soft-combine HARQ is available for fixed data frames, but only contributes
  after a failed decode is retained and the same frame is retransmitted.

The old global `1 + 10 * fading_index^2` heuristic is not present in the
current demapper. It was replaced by per-carrier `1 + K * norm_var` adaptive
noise inflation. The remaining `K=10` value is empirical rather than derived
from a closed-form AWGN or Watterson model.

### Diagnosis

Existing post-Layer-4 OFDM QSO logs show the 10 dB failure sequence:

| Seed | 10 dB QSO outcome | Layer 5 evidence |
|------|-------------------|------------------|
| 42 | handshake passes, DATA never accepted before message assertion | no OFDM DATA LLR/LDPC path is reached |
| 43 | DATA decodes `4/4` CWs; disconnect tail fails | accepted DATA CWs decode with `|LLR|_avg ~= 1.58-1.64` and iterations `5-6` |
| 44 | DATA decodes `4/4`; DISCONNECT control decodes; final ACK/disconnect tail fails | accepted DATA CWs decode with `|LLR|_avg ~= 1.81-1.87` and iterations `4-25` |

When connected OFDM sync is bypassed and the exact known frame slice is decoded
with `ofdm_snr_probe`, DQPSK R1/4 at 10 dB passes all three requested seeds:

| SNR dB | Seeds | Result | Measured in-band SNR / fading index |
|--------|-------|--------|-------------------------------------|
| 10 | 42 | pass, `4/4` CWs | `10.46 dB`, fading `0.16` |
| 10 | 43 | pass, `4/4` CWs | `9.58 dB`, fading `0.13` |
| 10 | 44 | pass, `4/4` CWs | `10.03 dB`, fading `0.05` |

Below the target, known-position 8 dB is genuinely marginal:

| SNR dB | Seeds | Result |
|--------|-------|--------|
| 8 | 42 | fail, `0/4` CWs |
| 8 | 43 | fail, `3/4` CWs |
| 8 | 44 | pass, `4/4` CWs |
| 9 | 42 | pass, `4/4` CWs |

Targeted scaling experiment: setting `CARRIER_ADAPTIVE_K` to `0.0` and
rebuilding did not change the 8 dB known-position outcomes for seeds 42-44.
The experiment was reverted. That rules out the `K=10` adaptive inflation as
the dominant AWGN edge at this rate.

Pre-LDPC gates:

- `control_first`: not the DATA failure gate at 10 dB. Seed 44 proved the path
  can decode an OFDM DISCONNECT control frame once light sync is accepted.
- `cw0_peek`: accepted DATA frames passed the escalation gate with
  `|LLR|_avg ~= 1.9-2.0`, above the `1.5` threshold.
- `pre_ldpc_llr`: seed 42 never reaches this gate for DATA because connected
  OFDM light sync is rejected first.

LLR clipping: accepted 10 dB frames logged max absolute LLRs around
`5.25-5.91`, far below the `MAX_LLR=20.0` clip. Clipping is not binding.

LDPC iterations/convergence: accepted 10 dB DATA frames converged within the
existing R1/4 `50` iteration budget. The worst observed accepted CW used
`25` iterations; seed 43 needed one decoder-diversity retry on CW0 and then
completed. Raising the iteration ceiling is not supported by this evidence.

Soft-combine HARQ: no `HARQ: combining attempt` evidence appears in the 10 dB
DATA decode path. When sync accepts a DATA frame, it decodes on that receive;
when sync does not accept, no soft buffer exists to combine. HARQ is not the
current floor gate.

Multi-perspective check:

- PHY: R1/4 LDPC has enough soft information at the 10 dB target when the
  correct OFDM frame is presented to the decoder. The remaining miss is before
  the decoder sees a codeword, not a Shannon/LDPC margin issue at 10 dB.
- DSP: `ofdm_snr_probe` used the production OFDM waveform, channel model,
  demapper, frame interleaver, carrier deinterleaver, and fixed-frame LDPC
  decoder on the exact known frame slice; it passed `3/3` seeds at 10 dB.
- HF operator: the observed behavior matches an acquisition/tail-control
  problem: the link connects and can deliver payloads when it locks, but short
  ACK/DISCONNECT frames are easy to miss at low SNR.
- Physics: LLR clipping is far from saturation, LDPC iterations have headroom,
  and the adaptive scaling experiment did not move the 8 dB edge. There is no
  measured basis for an LLR-scale fudge factor.

### Verdict

Layer 5 round 1 found no justified code fix. Applying an LLR scaling, clipping,
iteration, pre-screen, or HARQ change would not move the current OFDM R1/4 QSO
floor from 12 dB to 10 dB because the failing 10 dB QSO candidates usually die
before the Layer 5 path runs. The actionable remaining gate is connected OFDM
light-sync/tail-control acquisition for short ACK/DISCONNECT frames, with Layer
7 ARQ timing also relevant once DATA delivery succeeds.

Commands run:

- `cmake --build build --target ofdm_snr_probe test_streaming_signal_policy test_soft_combine -j4`
- `./build/ofdm_snr_probe --snr 10 --channel awgn --rate r1_4 --mod dqpsk --cw-count 4 --seed 42`
- `./build/ofdm_snr_probe --snr 10 --channel awgn --rate r1_4 --mod dqpsk --cw-count 4 --seed 43 --no-header`
- `./build/ofdm_snr_probe --snr 10 --channel awgn --rate r1_4 --mod dqpsk --cw-count 4 --seed 44 --no-header`
- `./build/ofdm_snr_probe --snr 8 --channel awgn --rate r1_4 --mod dqpsk --cw-count 4 --seed 42`
- `./build/ofdm_snr_probe --snr 8 --channel awgn --rate r1_4 --mod dqpsk --cw-count 4 --seed 43 --no-header`
- `./build/ofdm_snr_probe --snr 8 --channel awgn --rate r1_4 --mod dqpsk --cw-count 4 --seed 44 --no-header`
- `./build/tests/test_streaming_signal_policy`
- `./build/tests/test_soft_combine`

No code change remains in the worktree from the temporary
`CARRIER_ADAPTIVE_K=0.0` experiment.

### Gate / Stop Status

No commit was made for Layer 5 round 1. The full ctest gate did not meet the
required `86/86` baseline with only the five sandbox-bind exemptions:

- `ctest --test-dir build --output-on-failure -j4`: `80/86` passed.
- Allowed sandbox/bind failures: `GrpcServiceSmoke`, `CLISyntheticNotch`,
  `OtasimServeSmoke`, `UltraGuiOtaClient`, `UltraTncSimAudio`.
- Additional non-sandbox failure: `DecodeBenchReplay`.
- Rerun with `ctest --test-dir build -R DecodeBenchReplay --output-on-failure`
  reproduced the failure.

`DecodeBenchReplay` failed on
`fixtures/ofdm_chirp_r14_dqpsk_snr15_good.wav`, decoding `0` frames where the
test expected one byte-exact DATA frame. The log shows connected OFDM light-sync
weak acceptance at correlation `0.443`, CW0 peek `|llr|_avg=1.9`, then the
full 4-CW attempt rejected as a false chirp lock with `|llr|_avg=1.50`.

Because this is a non-sandbox gate failure, the audit documentation remains
uncommitted and the work stops here.

## Layer 4: Round 3 DecodeBenchReplay regression

Round 3 scope: repair the `DecodeBenchReplay` regression introduced after the
connected OFDM light-sync relaxation while preserving the Layer 4 floor gains.
The failing fixture was
`fixtures/ofdm_chirp_r14_dqpsk_snr15_good.wav`, which had regressed from one
byte-exact DATA frame to zero decoded frames. The companion
`fixtures/ofdm_chirp_r12_dqpsk_snr18_good.wav` continued to pass.

Status: the DecodeBenchReplay regression is fixed by one OFDM data-sync search
repair, but no commit was made because full ctest still has one non-exempt
failure outside the allowed bind failures.

### Diagnostics

Finding 1: the failing fixture was not an LDPC/LLR scaling failure. The decoder
accepted an early weak OFDM DATA sync at correlation `0.443`, then rejected the
full 4-CW decode as a false chirp lock at `|llr|_avg=1.50`. A direct LTS scan
of the same audio showed a stronger later peak at absolute sample `57722`
with correlation `0.681338`. The original capped search admitted the weak
candidate at absolute sample `57276` and did not get to the stronger LTS.

Multi-perspective check:

- PHY theorist: the later peak has materially higher Schmidl-Cox/LTS
  autocorrelation and produces stronger soft information. The weak early peak
  is a false timing hypothesis, not a code-rate or LDPC margin limit.
- DSP systems engineer: the failure is caused by the search boundary. The weak
  peak sits at the end of the capped search window, while the stronger peak is
  still inside the same streaming buffer.
- Veteran HF operator: this is the practical "locked too early" failure mode.
  The modem heard the burst, chose a marginal timing point, and then missed the
  actual usable part of the signal.
- First-principles physics: identical OFDM training symbols can only produce
  the intended high autocorrelation when the integration windows line up with
  the repeated LTS. The later peak aligns more energy coherently, so it is the
  physically better timing candidate.

Finding 2: raising the pre-sync LLR gate would not fix this replay. The failing
4-CW attempt is exactly at the existing `kMinPreSyncLLR` edge, but by the time
that gate rejects, the streaming decoder has already consumed the false lock and
advanced past the true LTS.

Multi-perspective check:

- PHY theorist: a stricter LLR screen would reject the bad hypothesis slightly
  earlier, but it would not present the correct hypothesis to the decoder.
- DSP systems engineer: the missed opportunity is in acquisition candidate
  ordering. Once `advancePastFalseOFDMLock()` runs from the weak lock, the later
  stronger peak is skipped.
- Veteran HF operator: this would sound like telling the receiver to distrust a
  bad lock after it already stopped listening at the right instant.
- First-principles physics: thresholding a derived soft-bit magnitude cannot
  recover timing energy that was never evaluated as the selected lock point.

Finding 3: the least invasive surviving fix is a bounded "prefer the better
peak" rescue for weak edge candidates, not a global floor rollback. Tightening
the relaxed floor from `0.40` toward `0.45` would likely repair the fixture by
rejecting the early `0.443` candidate, but it directly trades away the round-2
12 dB floor gain. SNR-conditioning the floor adds a new coupling to a noisy
measurement and would still tend to admit this fixture's weak candidate.

Multi-perspective check:

- PHY theorist: preferring a later peak only when it is stronger by a measured
  margin keeps the relaxed low-SNR hypothesis space while selecting the better
  timing maximum.
- DSP systems engineer: the rescue is bounded to sub-`0.45` candidates at the
  search edge and only scans the remainder of the current buffer. It does not
  change ordinary high-confidence locks or the initial threshold policy.
- Veteran HF operator: this preserves weak-signal patience, but if a stronger
  copy is immediately behind the marginal one, the receiver takes the better
  copy instead of committing to the first fluttery hint.
- First-principles physics: a stronger later autocorrelation peak in the same
  observation window is more likely to be the coherent repeated-symbol match.
  The `0.02` improvement margin avoids chasing numerical ties.

### Fix

Code change:

- `OFDMChirpWaveform::detectDataSync()` now performs one bounded extension
  pass when the best DATA-sync candidate is below `0.45`, above the current
  relaxed threshold, and within `16` samples of the capped search end.
- The extension scans from the old search end to the end of the current buffer
  and replaces the candidate only if a later peak improves correlation by at
  least `0.02`.
- The normal search threshold, initial high-confidence exit, and non-edge
  candidates are unchanged.

This intentionally preserves the connected OFDM relaxed floor. It only handles
the measured regression shape: a weak relaxed-floor candidate at the search cap
with a clearly stronger LTS still present later in the same buffer.

### Validation

Targeted build/test:

- `cmake --build build --target decode_bench test_decode_bench_replay test_streaming_signal_policy cli_simulator ofdm_snr_probe -j4`: passed.
- `cmake --build build --target ota_simulator -j4`: passed.
- `./build/tests/test_streaming_signal_policy`: passed `65/65`.
- `ctest --test-dir build -R DecodeBenchReplay --output-on-failure`: passed.

Direct replay evidence:

- Before the fix, the failing fixture accepted `corr=0.443` at sample `57276`,
  reported CW0 peek `|llr|_avg=1.9`, rejected the 4-CW decode at
  `|llr|_avg=1.50`, and decoded `0` frames.
- After the fix, the decoder logged replacement of the weak peak at offset
  `4476` with a later peak at offset `4922` (`corr=0.68`), synced at sample
  `57722`, reported CW0 peek `|llr|_avg=3.2`, and decoded one byte-exact DATA
  frame.

Required QSO preservation sweeps:

| Scenario | Seeds | Result | Evidence |
|----------|-------|--------|----------|
| OFDM forced DQPSK R1/4 AWGN 12 dB | 42,43,44 | 3/3 pass | DATA decoded `4/4` CWs; DATA sync correlations `0.40`, `0.42`, `0.42`; ACK/DISCONNECT completed |
| OFDM forced DQPSK R1/4 Good 15 dB | 42,43,44 | 3/3 pass | DATA decoded `4/4` CWs; DATA sync correlations `0.53`, `0.55`, `0.61`; disconnect completed |
| MC-DPSK AWGN 5 dB | 42,43,44 | 3/3 pass | DATA decoded `2/2` CWs and ACK/DISCONNECT completed at measured SNR about `4.8-5.1 dB` |
| OFDM forced DQPSK R1/4 AWGN 10 dB | 42,43,44 | 0/3 pass | CONNECT/CONNECT_ACK completed, but the DATA message assertion still failed |

The OFDM 10 dB target did not move in this round. The change repairs the
measured false early timing commit without claiming a new floor.

Full ctest gate:

- `ctest --test-dir build --output-on-failure -j4`: `80/86` passed.
- `DecodeBenchReplay`: passed inside the full ctest run.
- Allowed sandbox/bind failures: `GrpcServiceSmoke`, `CLISyntheticNotch`,
  `OtasimServeSmoke`, `UltraGuiOtaClient`, `UltraTncSimAudio`.
- Additional non-exempt failure: `OTASimulatorTwoEndpointMCDPSKLowSNR`.

`OTASimulatorTwoEndpointMCDPSKLowSNR` is not a bind failure. Its session log
showed Bob received CONNECT, Alice remained `CONNECTING` at the 35 s assertion,
the QSO payload was not received, and Bob remained `CONNECTED` at the final
disconnect assertion. The direct socket-free MC-DPSK 5 dB sweep above still
passed `3/3`, so this ctest failure is recorded as a separate harness/scenario
gate rather than evidence that the OFDM replay repair regressed MC-DPSK.

### Round-stop status

Commit not made. The DecodeBenchReplay regression is fixed and the requested
direct sweeps passed, but the hard full-ctest rule allows only five
sandbox-bind exemptions. The sixth failure,
`OTASimulatorTwoEndpointMCDPSKLowSNR`, is non-exempt. No push was performed.

Evidence log for this round: `/tmp/codex_audit_and_chain_part7.log`.
