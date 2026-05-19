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
