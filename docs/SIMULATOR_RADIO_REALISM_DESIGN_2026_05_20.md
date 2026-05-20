# Simulator Radio Realism Design - 2026-05-20

## Mandate

`cli_simulator` must model two real radios, not two idealized modem queues. A real
soundcard is the clock: every audio period it pulls the next samples from the
modem and pushes the next received samples into the modem. The simulator must
therefore make the station audio loop the master clock and must not expose a
multi-second software TX FIFO as "radio pending" time.

The goal is not to retune protocol constants around simulator behavior. The
goal is to make simulator timing match the radio/audio chain that those
constants already describe.

At 48 kHz, 100 ms is 4,800 samples. Any "TX_pending" assertion for a 100 ms
radio-audio buffer should use that order of magnitude, not a full 50,000-sample
frame.

## Current Failure Mode

`SimulatedStation` currently accepts protocol TX callbacks as complete audio
vectors. A single frame can be about 50,000 samples and an ARQ burst can enqueue
eight of them before the 10 ms audio loop has drained the first callback. That
models a modem-owned audio FIFO, not a soundcard-owned DAC stream.

That inversion creates these artifacts:

- PTT and carrier-sense state follow submission backlog rather than RF output.
- `TX_pending` can report seconds of future audio.
- Handshake fail-safe and ARQ/SACK timers race audio that has not gone on air.
- Test fixes become coupled to simulator queue depth instead of radio timing.

## Design Principles

Every architectural decision is checked from four perspectives:

- PHY theorist: timing-sensitive tests are valid only if "on air" timing, not
  CPU submission timing, drives RX deafness, carrier sense, and protocol timers.
- Real-time DSP systems engineer: audio callbacks must be pull-clocked, bounded,
  and non-blocking in the steady state.
- Veteran HF operator: PTT, LEDs, S-meter visibility, and busy state must follow
  actual RF/audio egress, not future frames accepted by software.
- First principles: the simulator may simplify modem internals, but it must not
  simplify away the physical audio-chain dependency being tested.

## Target Interfaces

Add an explicit radio/modem audio contract beside the existing `AudioPort`
contract:

```cpp
struct RadioTxPullResult {
    size_t emitted_samples = 0;
    size_t active_samples = 0;
    size_t audio_chain_pending_samples = 0;
    bool tx_active = false;
    bool tx_draining = false;
    std::string label;
};

class IRadioModem {
public:
    virtual ~IRadioModem() = default;
    virtual RadioTxPullResult pullTxSamples(float* out, size_t count) = 0;
    virtual void pushRxSamples(const float* in, size_t count) = 0;
};
```

`AudioPort` remains the medium/soundcard adapter:

- `pullRx(count)` supplies the next received samples to the station.
- `queueTx(samples)` receives exactly one callback-sized TX block from the
  station audio loop on paced backends.
- hardware-backed ports can still bypass the simulator pacer because SDL is
  already the device clock.

## Encoder State

Minimal implementation:

- Keep `StreamingEncoder::{encodeFrame,encodeFrameLight,encodeBurstLight}` as
  synchronous frame encoders.
- Move complete frame vectors out of the audio-chain queue and into a single
  active TX cursor owned by `SimulatedStation`.
- Protocol callbacks enqueue logical TX jobs, not complete waveform samples:
  `Frame`, `Burst`, `Ping`, `Pong`, and test-only `RawSamples`.
- The audio loop starts at most one active job when a pull callback needs TX
  samples. That job may internally hold one encoded waveform vector and an
  offset cursor.
- Subsequent callbacks copy only the next `count` samples from the active job.
- Additional ARQ submissions wait as logical jobs/deferred jobs; they do not
  increase audio-chain pending time.

This gives the simulator pull semantics at the radio boundary without
requiring immediate invasive changes to every waveform modulator. A later
optimization can split `StreamingEncoder` into symbol/chunk generators, but
that is not required to remove the false multi-second audio FIFO.

Decision check:

- PHY: only emitted callback blocks change PTT/RX deafness, so protocol timers
  see RF time.
- DSP: the steady-state callback copies bounded slices; only job activation
  performs full-frame encoding. If profiling shows activation overruns, move
  job encoding to a one-job-ahead worker, still capped by the active cursor.
- HF operator: logical ARQ intent is no longer the same as keyed RF output.
- First principles: one active waveform cursor is a modem implementation detail;
  the simulated audio chain remains bounded.

## Audio Loop Behavior

The station audio loop becomes:

1. Advance PTT recovery by one callback period.
2. Try to flush one eligible deferred logical TX job.
3. Pull exactly `SAMPLES_PER_CALLBACK` TX samples from the radio modem.
4. Pull exactly `SAMPLES_PER_CALLBACK` RX samples from the audio port.
5. Push RX samples into the decoder.
6. Queue the TX callback block into the audio port.
7. Log `TX_pending` as audio-chain pending samples, not logical job backlog.

`pullTxSamples` must not wait for future protocol work. If no TX job is ready,
it returns a zero-filled block with `tx_active=false` and
`audio_chain_pending_samples=0`.

If job activation cannot produce samples immediately, the safe behavior is to
return zeros for that callback and try again on the next callback. The first
milestone can encode synchronously because current frame encoders are already
CPU-fast relative to frame airtime; the interface must not require blocking.

## PTT State

TX starts when samples leave the simulated radio, not when protocol submits a
frame. Under pull semantics:

- Logical enqueue does not call `noteTxQueued`.
- The first callback that emits active or draining TX samples calls
  `noteTxSampleBlock(tx_active, tx_draining)`.
- Silent gaps inside an active waveform still count as draining while the
  active TX cursor has remaining samples.
- When the active cursor is exhausted and a later callback emits no draining
  samples, `noteTxSampleBlock(false, false)` starts T/R switch and cooldown.

This preserves half-duplex blackout and recovery behavior while anchoring it to
actual egress.

## Submission And Deferral Model

Replace sample FIFO operations with logical submission operations:

```cpp
struct TxSubmission {
    enum class Kind { Frame, Burst, Ping, Pong, RawSamples };
    Kind kind;
    Bytes frame;
    std::vector<Bytes> burst;
    std::vector<float> raw_samples;
    std::string label;
    uint64_t min_rx_observation_epoch = 0;
};

struct ActiveTx {
    std::vector<float> samples;
    size_t offset = 0;
    std::string label;
};
```

Carrier-sense and PTT gates operate on logical submissions:

- `CONNECT` still requires a fresh RX observation before it may key.
- cooldown and busy-channel submissions stay in `deferred_tx_submissions_`.
- only one deferred logical submission flushes per radio key-up.
- `canAcceptTxSubmission()` checks active cursor and ready logical queue, not a
  sample FIFO depth.

`RawSamples` exists for low-level unit tests and scripted audio fixtures during
migration. Production protocol TX should use logical frame/burst/ping jobs.

## Migration Path

Milestone 1: design doc only.

Milestone 2: add `IRadioModem`, `RadioTxPullResult`, `TxSubmission`, and
test-hook helpers additively. Keep old `queueTx(samples)` wrappers compiling by
mapping them to `RawSamples`.

Milestone 3: migrate `SimulatedStation` virtual-channel pacing:

- protocol callbacks enqueue logical jobs.
- audio loop calls `pullTxSamples`.
- `VirtualAudioPort::queueTx` continues to receive one callback block.
- keep old direct hardware behavior unchanged initially.

Milestone 4: migrate OTA client/backend paths:

- `OtaAudioPort` receives callback-sized blocks from station pacing.
- server/client medium semantics remain push-at-port because the network
  transport carries already clocked audio blocks.
- any direct test/client ports that override `queueTx` keep the same public
  shape.

Milestone 5: remove or quarantine old sample FIFO API:

- production paths no longer call sample-based `queueTx`.
- test-only raw-sample hooks remain named as test hooks.
- logs distinguish `TX_pending` audio-chain samples from logical backlog.

Milestone 6: remeasure previously flaky cells and update the private audit only
after scanner output shows the handshake fail-safe item is actually closed.

## Effects On Existing Components

### `SimulatedStation`

`SimulatedStation` becomes the `IRadioModem` implementation for CLI tests. It
owns logical TX jobs, the active TX cursor, decoder feeding, and PTT state.

### `StreamingEncoder`

No wire-format change is required. The first refactor keeps synchronous encode
methods and uses an active cursor. A later `StreamingEncoder::beginFrame()` /
`readSamples()` API can replace the internal vector without changing the
station/audio-port contract.

### `VirtualAudioPort`

No conceptual change: it still models the in-process medium and receives one
callback block at a time from the station. It should never see an 8-frame ARQ
burst as one submitted vector.

### `OtaAudioPort`

No network contract change is required for this refactor. It should receive
callback-sized TX blocks from `SimulatedStation` and send those to the OTASim
audio backend. Remote jitter remains a transport concern, not a modem audio
FIFO concern.

### `HardwareAudioPort`

Hardware audio already has SDL as the output clock, so it can remain in the
direct path until the CLI station is fully migrated. The final cleanup should
make the public station TX submission model logical for both virtual and
hardware roles, but not double-pace hardware output.

## Tests

Existing tests that must keep passing:

- `test_audio_*`
- `test_session_*`
- `test_simulated_radio_*`
- `test_deferred_tx_*`
- `test_otasim_*`
- `CLISyntheticNotch`
- `OtasimServeSmoke`
- `cli_simulator --test` for current channel/SNR cells

New coverage:

- `test_radio_realism_tx_pending`
  - enqueue an ARQ-like set of logical frame/raw submissions rapidly.
  - run station pull callbacks at 480 samples.
  - assert audio-chain pending never exceeds 4,800 samples.
  - assert logical backlog can be larger without inflating `TX_pending`.
  - assert PTT enters TX only after the first pulled block, not on enqueue.

E2E verification:

- Run each `cli_simulator --test` log through `scripts/scan_cli_log.py`.
- Accept the refactor only when scanner output no longer reports handshake
  fail-safe or multi-second `TX_pending` anomalies for the standard scenarios.
- Re-run SNR=12 AWGN R1/4 OFDM_CHIRP four-seed floor and compare the old
  seed-23/42 112-retransmission failures against the new radio-realistic
  timing.

## Logging And Metrics

`TX_pending` must mean audio-chain samples that can play without another modem
pull. In the pull model this should normally be 0 to 480 samples, and it must
stay below 4,800 samples for the realism test.

Add separate debug fields for non-audio state:

- `tx_logical_pending`: count of logical submissions waiting for a future key-up.
- `tx_active_remaining`: remaining samples in the active modem waveform cursor.
- `tx_audio_pending`: callback-buffered samples visible to the audio chain.

The scanner should continue to read `TX_pending`, but that value should no
longer include active-frame or ARQ-window backlog.

## Open Risks

- Encoding on the first pull of a job can extend one simulated callback. If that
  appears in timing profiles, add a one-job-ahead encoder worker. The worker may
  prepare only the next eligible logical submission and must not recreate an
  8-frame audio FIFO.
- Some tests currently assert sample FIFO depth. They must be rewritten to
  assert logical backlog, active cursor state, and callback pull behavior.
- Hardware direct TX and virtual paced TX have different clocks today. The
  migration must avoid double-pacing hardware while still using the same
  logical TX submission semantics.
- `TX_pending < 100 ms` uses 4,800 samples at 48 kHz. Older notes that treat
  50,000 samples as 100 ms are sample-rate math errors and should not define
  the test threshold.
