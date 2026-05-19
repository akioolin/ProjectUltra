# Radio vs Medium Architecture

Date: 2026-05-19

This note supersedes the earlier server-side half-duplex blackout direction from
the OTASim audit work on this branch.

## Core Separation

The OTASim server represents the HF propagation medium. It mixes station audio,
applies channel/noise models, advances session sample clocks, and distributes
the resulting receive streams. It must not decide that a station is deaf because
the medium is not half-duplex. The air and ionosphere carry simultaneous
signals, and a third receiver on the same band can monitor all active stations.

The client-side simulated radio owns half-duplex behavior. Its PTT state decides
when local RX samples are usable and when the next TX may be keyed.

## Decisions

1. Server-side blackout removed.
   - PHY theorist: the channel model remains a full-duplex medium mixer; it
     still omits a receiver's own transmitted sample stream, which is a local
     monitor-path choice, not propagation half-duplex.
   - Real-time DSP systems engineer: the server no longer depends on per-client
     TX state metadata or a parallel blackout queue that can drift from audio
     sample queues.
   - Veteran HF operator: the repeater-like "the band muted me because I keyed"
     behavior was wrong; a separate receiver would still hear the other station.
   - Physics escape hatch: simultaneous RF energy remains present in the
     medium; only the local radio front end can be disconnected.

2. Audio packet `tx_active` removed.
   - PHY theorist: TX-active is not a medium input; samples themselves are the
     physical excitation of the channel.
   - Real-time DSP systems engineer: fewer wire bits means fewer client/server
     ordering races and no mixed legacy/explicit TX-state semantics.
   - Veteran HF operator: a radio's PTT relay state is local equipment state,
     not something the ionosphere needs to know.
   - Physics escape hatch: the medium cannot infer or enforce receiver deafness
     from station hardware state.

3. Client AudioPorts consume then silence RX during deaf phases.
   - PHY theorist: incoming samples are still delivered by the channel model.
     The receiver discards them locally while deaf, so they are not replayed
     after PTT release.
   - Real-time DSP systems engineer: consuming before zeroing preserves sample
     clock continuity and avoids stale buffered ACKs.
   - Veteran HF operator: during TX and the T/R switch transient, the receiver
     is physically muted or disconnected.
   - Physics escape hatch: the signal existed in the air; this receiver simply
     did not observe it.

4. PTT state split into `RX`, `TX`, `TX_TR_SWITCH`, and `TX_COOLDOWN`.
   - PHY theorist: only `TX` and `TX_TR_SWITCH` are RX-deaf states.
     `TX_COOLDOWN` is RX-open but TX-locked.
   - Real-time DSP systems engineer: state advances in samples from the audio
     callback, keeping radio recovery aligned with the audio clock.
   - Veteran HF operator: PTT release is not instant full recovery; relay/PIN
     switching is brief deafness, followed by AGC/preamp recovery while
     listening is already possible.
   - Physics escape hatch: the local ability to transmit or receive changes;
     the medium remains unchanged.

5. New TX submissions are gated at the simulated radio boundary.
   - PHY theorist: ARQ does not need a propagation-side blackout rule to avoid
     self-colliding with ACKs.
   - Real-time DSP systems engineer: the station defers TX audio while the
     radio is in `TX_TR_SWITCH` or `TX_COOLDOWN`; the protocol layer is not
     given another timing heuristic.
   - Veteran HF operator: an operator cannot re-key until the radio's T/R
     recovery policy allows it, but can listen during cooldown.
   - Physics escape hatch: queued local transmitter audio is not in the air
     until the local radio keys again.

6. In-progress TX audio chunks bypass the recovery gate.
   - PHY theorist: a coded MC-DPSK frame is one physical burst, so chunking at
     the software queue boundary must not split the waveform into separate
     transmissions.
   - Real-time DSP systems engineer: the simulated station keeps a short
     continuation grace after the paced TX queue drains; chunks that arrive in
     that grace re-enter `TX` and append immediately.
   - Veteran HF operator: the operator keys once for the frame, sends the
     whole burst, and releases once. A sound-card callback boundary is not a
     PTT release.
   - Physics escape hatch: splitting a coded waveform mid-frame makes the
     receiver observe unrelated bursts; preserving continuity keeps the frame
     physically decodable.

Deferred submissions are still flushed one logical TX per RX-ready key-up.
This prevents multiple queued frames from collapsing into a multi-second burst
when cooldown ends, while allowing true in-progress chunks to remain continuous.

## Code Map

- `src/ota_channel_core/session_context.cpp`: session clock mixer carries
  simultaneous station audio and only skips a receiver's own station stream.
- `src/ota_channel_core/channel.cpp`: in-process channel no longer drops peer
  audio based on receiver PTT state.
- `src/ota_simulator_service/audio_plane.cpp`: UDP audio packets no longer
  parse or forward TX-active state.
- `src/otasim_client/ota_audio_backend.cpp`: client audio upload sends samples
  only; no TX-active flag is serialized.
- `tools/sim/simulated_station.hpp`: `RadioPttStateMachine` owns radio PTT
  phases; `AudioPort` gates RX at the local radio; `SimulatedStation` defers TX
  submissions until the radio may key again.

## Verification Targets

- `SessionHalfDuplexBlackout` is now a medium-mixer test despite the retained
  historical target name.
- `AudioPacket` verifies the packet format preserves generic flags without
  half-duplex semantics.
- `SimulatedRadioPttState` verifies PTT phase progression and RX/TX readiness.
- `ClientAudioPortRadioGate` verifies client-side consume-then-silence behavior.
- `DeferredTxFragmentation` verifies chunked in-progress TX bypasses the gate
  and deferred logical submissions do not all flush into one key-up.
- `MCDPSKHalfDuplexTiming` verifies the MC-DPSK timing regression stays within
  a low-retry envelope.
