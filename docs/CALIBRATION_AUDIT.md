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
