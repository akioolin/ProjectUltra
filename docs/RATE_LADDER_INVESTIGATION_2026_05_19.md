# Rate Ladder Ground-Up Investigation - 2026-05-19

Phase 1 status: diagnosis was completed first. Follow-up status on
2026-05-19: the external OTASim half-duplex RX-blackout fix has been
implemented in this working tree; no rate-ladder retuning has been
done in this round.

Branch observed locally: `fix/rate-ladder-groundup-2026-05-19`.

Base state note: the prompt says main/audit HEAD was `bcfa716`; this checkout's local `main` and this branch were already at `b8639c5` when Phase 1 started. I treated `b8639c5` as the observed local starting point and did not revert anything. Existing untracked `recordings/` directories were left untouched.

## Executive Summary

The current problem is not a single "bad threshold" problem. The code already has a rate/window/CW-aware ACK timeout formula, but the operational rate ladder is mixing three different notions of robustness:

1. Per-codeword PHY robustness: lower code rate is more redundant.
2. Transaction robustness: more ARQ fragments and more ACK turns multiply failure chances.
3. Simulator realism: the current external OTASim path does not model receiver blackout/half-duplex PTT for a station that is still transmitting.

The strongest Phase 1 finding is architectural: OTASim, as wired through `OtaAudioPort`, is not yet a real-HF half-duplex authority for rate-ladder claims. It continuously mixes other stations into the receiver and `OtaAudioPort` does not wire the `setRxBlackoutCallback()` hook that the in-process `SimulatedChannel` uses. That means ACKs can be generated and delivered while the peer is still transmitting. A real HF operator/radio cannot rely on that.

Fresh OTASim sweeps could not be run from this sandbox: connecting to the already running `127.0.0.1:50051` server failed, direct `nc` to localhost was denied with `Operation not permitted`, and the CLI self-spawn path failed to bind an ephemeral TCP port. So this document does not claim new OTASim floors. It records code-derived timing, fragmentation, decode timing, and the testing blocker.

## Phase 1 Evidence

### 1. ACK Timeout Is Rate-Aware, But R1/4 Has A Tight Margin

Relevant code:

- `Connection::configureArqForCurrentDataMode()` configures ARQ from negotiated waveform/mod/rate/CW count.
- Wide OFDM uses `ofdmWindowSizeForChannel()`, `wideOFDMFrameTiming()`, `ofdmSackDelays()`, `ofdmAckRepeatProfile()`, and `computeWideOFDMAckTimeoutMs()`.
- `SelectiveRepeatARQ::tick()` increments `timeouts` and retransmits with cause `TIMEOUT` when a TX slot countdown expires.
- The RTT estimator is Karn-safe, but its floor is at least the configured timeout, so it cannot shrink below the initial formula.

Computed for DQPSK OFDM_CHIRP using current policy at the boundary cells where `near_awgn_ofdm` is false unless SNR is at least 25:

| Rate | CW/frame | Payload cap | 7-msg ARQ frames | Window | Data frame | Full-window TX | SACK delay | ACK timeout |
|------|----------|-------------|------------------|--------|------------|----------------|------------|-------------|
| R1/4 | 4 | 61 B | 11 | 8 | 648 ms | 5184 ms | 120 ms | 8000 ms |
| R1/2 | 8 | 301 B | 7 | 16 | 1224 ms | 19584 ms | 9912 ms | 31328 ms |
| R2/3 | 8 | 413 B | 7 | 8 | 1224 ms | 9792 ms | 120 ms | 11744 ms |
| R3/4 | 8 | 461 B | 7 | 8 | 1200 ms | 9600 ms | 120 ms | 11504 ms |

For R1/4, the raw formula is about 7136 ms and then clamps up to the 8000 ms floor. That leaves only about 864 ms above the modeled full-window burst plus ACK path and decode/audio margin. OTASim's client RX buffer cap is 480 ms, the server RX outbox cap is 200 ms, UDP polling waits up to 100 ms, and both station and session clocks use 10 ms pacing. That is not proof of timeout failure, but it means R1/4 has the thinnest absolute ACK slack in the table.

Perspective check:

- PHY theorist: lower code rate increases per-codeword redundancy, but the operational event is not one codeword. It is a whole stop-and-ack transaction with multiple fragments. A lower rate can be more reliable per frame while still worse for delivered bits per second if it multiplies frame count and ACK exposure.
- DSP systems: the timeout formula is not rate-agnostic, but it is model-driven and R1/4 lands on the global 8 s floor. Any extra queueing, scheduling, callback jitter, or early/late ACK behavior eats most of the remaining R1/4 slack.
- HF operator: an R1/4 burst with more fragments means more time on air and more ACK/turnaround opportunities. QSB or QRM during the ACK window looks exactly like a timeout storm to the sender.
- Physics escape hatch: transaction success is approximately the product of all required frame and ACK events. More fragments increase the number of required successes, even when each individual coded block has stronger FEC.

### 2. The Frame-Count Difference Is Real And Mostly Expected

The 7-message CLI test sends five 20-byte short messages plus two long messages of 132 and 126 bytes. Current fixed-frame payload capacities explain the observed counts:

| Rate | Current CW policy | Payload cap | Fragment counts | Total |
|------|-------------------|-------------|-----------------|-------|
| R1/4 | 4 CW | 61 B | 1,1,1,1,1,3,3 | 11 |
| R1/2 | 8 CW | 301 B | 1,1,1,1,1,1,1 | 7 |
| R2/3 | 8 CW | 413 B | 1,1,1,1,1,1,1 | 7 |
| R3/4 | 8 CW | 461 B | 1,1,1,1,1,1,1 | 7 |

This is not, by itself, a `streaming_encoder` bug. It is the direct consequence of:

- `Connection::sendMessages()` prefragmenting each application message by current data payload capacity.
- `recommendCWCount()` keeping R1/4 at 4 CW while promoting R1/2 and above to 8 CW.
- R1/4 information bits per LDPC CW being 162 bits, or 20 payload bytes before frame overhead.

The user-side statement that "R1/4 frames are longer on air" is not true under current policy. R1/4 4-CW frames are about 648 ms. R1/2 8-CW frames are about 1224 ms. R1/4 is operationally slower because it produces more frames and more ARQ work, not because each frame is longer.

Perspective check:

- PHY theorist: comparing code rates while changing CW count changes the experiment. R1/2 at 8 CW is not just a higher-rate code; it is also a much larger packetization unit that amortizes headers, ACKs, and burst overhead.
- DSP systems: the encoder and ARQ window are behaving consistently with the configured capacity. The stale `docs/PROTOCOL_V2.md` 4-CW capacity table no longer describes the R1/2+ default wide-OFDM path.
- HF operator: operators care about delivered message time. If R1/2 can carry the whole operator message in one frame where R1/4 needs three, R1/2 may sound and behave "cleaner" even though R1/4 is the more redundant code.
- Physics escape hatch: goodput is payload bits divided by total transaction time. Packetization and protocol overhead are part of the channel use.

### 3. SACK Timing Uses Application `MORE_FRAG`, Not Physical Burst State

The receiver's SACK scheduler treats `MORE_FRAG` as the in-burst signal. If `MORE_FRAG` is false and `sack_delay_short_ms` is configured, it can choose the short timer. If `sack_delay_short_ms` is zero, wide OFDM still uses the 120 ms default SACK delay.

For the 7-message test, the five short messages are independent application messages, so their frames have `MORE_FRAG=0`. The last fragment of each long message also has `MORE_FRAG=0`. But `sendMessages()` pipelines OFDM frames into one physical transmit burst. Therefore "application message boundary" and "physical burst tail" are not the same thing.

This can produce ACK/SACK attempts while the sender is still transmitting. In a full-duplex simulator this may work. In real HF it should be treated as collision/blackout unless the peer has actually stopped transmitting and the radio has turned around.

Perspective check:

- PHY theorist: ACK reliability is part of the coded link. A decoded data frame is not delivered until its ACK survives; using a control channel during another station's transmit interval violates the half-duplex channel model.
- DSP systems: this is a state-machine boundary problem. `MORE_FRAG` is an application reassembly flag, not a TX burst descriptor. The SACK scheduler needs a physical-burst/half-duplex guard or explicit TX-tail knowledge.
- HF operator: a station cannot hear the other station's ACK while its own transmitter is still keyed, and ALC/VOX/PTT tail timing can add more dead time. Any benchmark that counts those early ACKs is too optimistic for real HF.
- Physics escape hatch: two simultaneous signals in the same audio passband sum. Without a full-duplex receiver and echo isolation, the ACK's information is not recoverable at the transmitting station.

### 4. External OTASim Does Not Currently Enforce Station RX Blackout

Evidence:

- `AudioPort::setRxBlackoutCallback()` is a no-op by default.
- `VirtualAudioPort` overrides it and wires the callback into `SimulatedChannel`.
- `OtaAudioPort` does not override it.
- `SimulatedChannel::transmitFromA/B()` drops peer-delivered samples when the receiver is in RX blackout.
- `SessionContext::advanceSessionClock()` continuously mixes every other station into a receiver's RX outbox.
- `SampleIndexedMixer::mixForReceiver()` skips only the receiver's own station ID; it does not check whether the receiver is transmitting.

This means the external OTASim path is closer to a full-duplex audio mixer than a half-duplex HF channel. It remains useful for channel-model, gRPC/UDP, live-lab, and decode stress work, but it is not yet sufficient to declare "real HF" rate-ladder behavior.

Perspective check:

- PHY theorist: a full-duplex mixer changes the protocol capacity region. It permits control information in intervals that should be unavailable in a half-duplex link.
- DSP systems: the missing callback bridge is a concrete integration bug/architecture gap, not a threshold issue. The OTASim path and in-process path do not share the same half-duplex semantics.
- HF operator: this misses the actual operator/radio behavior: when transmitting, the receiver is muted or desensed, and there is PTT/ALC settling before useful receive resumes.
- Physics escape hatch: unless the hardware has simultaneous TX/RX isolation, the ACK cannot be observed by the station currently transmitting.

#### 2026-05-19 Fix Addendum

The external OTASim path now carries explicit TX state on the UDP
audio plane and applies RX blackout in `SessionContext`:

- `OtaAudioPacketHeader::flags` uses `TX_STATE_VALID` and `TX_ACTIVE`
  bits without changing the packet size or version.
- New clients stamp every UDP audio packet with `TX_STATE_VALID`;
  `TX_ACTIVE` follows the audio-port PTT/RX-blackout callback.
- `OtaAudioPort` now overrides `setRxBlackoutCallback()` and bridges
  `SimulatedStation`'s explicit PTT state into `OtaAudioBackend`.
- `SessionContext::enqueueTransmit()` stores a per-sample RX-blackout
  queue beside the TX audio queue, so server mixing blacks out the
  receiver window aligned with the station's own samples-in-flight.
- Older clients that omit `TX_STATE_VALID` keep the previous
  full-duplex behavior for compatibility; the server emits a
  `tx_state_legacy_client` event the first time it sees such traffic.
- `rx_settling_ms` now controls only post-TX recovery tail. Active TX
  itself always blackouts local RX.

Validation in this sandbox:

- `cmake -S . -B build`: passed.
- Build targets: `ota_channel_core`, `ota_simulator_service`,
  `ultra_otasim_client`, `cli_simulator`, `ota_simulator`, and the
  touched tests: passed.
- `ctest --test-dir build --output-on-failure -R
  '^(AudioPacket|SessionContext|SessionHalfDuplexBlackout|AudioPlaneOrdering)$'`:
  4/4 passed.
- `ctest --test-dir build --output-on-failure -R '^OTASimulator'`:
  7/7 passed after updating `OTASimulatorTwoEndpointSimultaneousProbe`
  to expect a half-duplex collision/disconnect instead of the old
  full-duplex successful connect.
- `GrpcServiceSmoke` still cannot run in this Codex sandbox: it aborts
  at `service.start(&error)` while binding the UDP audio plane on
  `127.0.0.1:0`. That should be rerun from an unrestricted shell or the
  Linux bench.

Perspective check:

- PHY theorist: the TX-state bit is on the sampled audio timeline, so
  blackout is applied to the same sample windows that contain the
  on-air signal and cannot race a separate control RPC.
- DSP systems: the server stores blackout state with queued samples,
  not as a stale wall-clock boolean, so UDP jitter and queue depth do
  not misplace PTT edges relative to audio.
- HF operator: synchronized probes now collide and time out instead of
  connecting through impossible full-duplex leakage, matching what an
  operator would experience with both stations keyed.
- Physics escape hatch: peer signal energy is removed from a station's
  RX stream while that station's transmitter is active; channel noise
  can remain, but peer information cannot be decoded locally.

### 5. Decode CPU Time Is Not The Dominant Timeout Budget

I generated connected OFDM_CHIRP decode fixtures with `decode_bench` and decoded 8 frames per rate. This measures local decode CPU cost, not OTASim transport latency.

| Rate | CW | Frames decoded | OFDM process mean/max | Fixed-frame decode mean/max | LDPC CW mean/max |
|------|----|----------------|-----------------------|-----------------------------|------------------|
| R1/4 | 4 | 8/8 | 749 us / 1817 us | 1871 us / 13741 us | 21 us / 42 us |
| R1/2 | 8 | 8/8 | 1339 us / 4240 us | 1252 us / 7225 us | 23 us / 67 us |
| R2/3 | 8 | 8/8 | 1549 us / 4727 us | 828 us / 3286 us | 28 us / 83 us |
| R3/4 | 8 | 8/8 | 1505 us / 4715 us | 650 us / 1980 us | 24 us / 81 us |

Even the worst local fixed-frame decode sample is around 14 ms. That is negligible compared with 648 ms or 1224 ms frame airtime and multi-second ARQ timers. Decode CPU is not the first suspect for 8 s timeout storms.

Perspective check:

- PHY theorist: LDPC iteration cost is not limiting the channel at these scales; packetization and ACK timing dominate.
- DSP systems: the decode thread can have occasional millisecond spikes, but these are far below the hundreds-of-milliseconds transport and scheduling margins.
- HF operator: the operator-visible delay is airtime and turn-taking, not CPU decode.
- Physics escape hatch: milliseconds of compute cannot explain repeated 8 s ARQ timeout epochs unless the state machine fails to deliver the ACK.

### 6. Current Ladder Comments And Tests Are Now Behind The Operational Question

`waveform_selection.hpp` currently uses in-band SNR thresholds:

- below 20 dB: MC-DPSK R1/4
- 20-25 dB: OFDM_CHIRP R1/4
- at least 25 dB AWGN: R3/4 or R2/3 depending fading gate
- at least 25 dB good/moderate: R1/2

That policy does not directly answer the user's forced-rate observation at configured SNR 12-15. It also still encodes the old idea that R1/4 is the conservative OFDM bridge between MC-DPSK and R1/2. Given the current CW policy, that bridge can be operationally worse than R1/2 when R1/2 decodes cleanly.

Perspective check:

- PHY theorist: rate-ladder monotonicity by FEC strength is not enough. The selector must maximize expected delivered goodput subject to a reliability floor.
- DSP systems: the selector must be tied to the actual ARQ geometry: CW count, frame count, window size, SACK delay, and timeout behavior.
- HF operator: the practical ladder should be "what completes the contact cleanly", not "what has the lowest nominal code rate".
- Physics escape hatch: the best operating point is the maximum mutual information successfully converted into acknowledged payload bits over time. Protocol overhead and half-duplex constraints are part of that optimization.

## Measurement Blocker

Required OTASim measurements could not be run in this Codex sandbox:

- `./build/cli_simulator --ota-host 127.0.0.1:50051 ...` failed before modem startup with `gRPC channel did not connect for SetChannel`.
- `nc -vz 127.0.0.1 50051` failed with `Operation not permitted`.
- CLI self-spawned OTASim failed with `failed to bind ephemeral TCP port`.

This prevents Phase 2 and Phase 3 claims from this process. The commands must be run from an unrestricted shell on this machine or from the Linux bench.

Raw logs from this Phase 1 attempt are under:

- `/tmp/codex_rate_ladder_groundup.log`
- `/tmp/rate_ladder_groundup_runs/`

## Recommended Phase 2 Methodology

Before changing the production ladder:

1. Decide whether OTASim must be fixed to enforce half-duplex RX blackout before it is used as the "real HF" gate. My recommendation is yes.
2. Run 5 seeds per cell minimum, not 1 seed.
3. Score both:
   - wall-clock payload throughput: delivered payload bits divided by data-phase seconds
   - ARQ proxy: nominal raw bps times original data frames divided by original data frames plus retransmissions
4. Parse and store at least these fields:
   - pass/fail
   - negotiated waveform/mod/rate/CW
   - data frames sent and received
   - retransmissions split by cause
   - timeouts
   - failed frames
   - ACK/SACK sent/received
   - SACK trigger reason
   - decoder frame success and peak backlog
5. Use both the 7-message test and a sustained file transfer, because startup/final-ACK overhead dominates short messages.
6. Treat any floor claim as invalid unless retx/timeouts are bounded, not just because the payload eventually arrives.

Suggested cells once OTASim half-duplex semantics are settled:

| Channel | Rates | SNR points | Seeds |
|---------|-------|------------|-------|
| AWGN | R1/4, R1/2, R2/3, R3/4 | 10, 12, 14, 16, 18, 20, 22, 25 | 5 |
| Good | R1/4, R1/2, R2/3 | 12, 14, 15, 16, 18, 20, 22, 25 | 5 |
| Moderate | R1/4, R1/2 | 14, 15, 16, 18, 20, 22, 25 | 5 |

## Candidate Fixes To Consider After Phase 2

No candidate below should be landed until it is validated with multi-seed OTASim or hardware-bench evidence.

1. Add half-duplex/PTT RX blackout to external OTASim. Completed in
   this working tree.
   - TX state is carried by UDP audio packet flags.
   - `SessionContext` applies per-sample receiver blackout while the
     receiver station's own TX samples are active.
   - Optional turnaround tail remains controlled by `rx_settling_ms`;
     default tail is 0 ms.

2. Replace `MORE_FRAG`-based SACK deferral with physical-burst-aware deferral.
   - The receiver should not send SACK before the sender's on-air burst is complete.
   - Application fragmentation flags are not a valid proxy for that.

3. Retune R1/4 ACK timeout only if timeout storms remain after (1) and (2).
   - A principled formula is:
     `timeout >= tx_burst + rx_decode + sack_deferral + ack_airtime + tx/rx_turnaround + bounded_transport_jitter`.
   - Current R1/4 has only about 864 ms slack above the modeled wide-OFDM full window.

4. Make the ladder data-driven by expected acknowledged goodput.
   - If R1/2 has higher throughput and lower timeout probability than R1/4 in a cell, it should win that cell.
   - R1/4 should remain available as the fallback when R1/2 cannot decode or when the half-duplex/ACK timing data proves it more reliable.

5. Update stale protocol docs and policy tests after the data lands.
   - `docs/PROTOCOL_V2.md` still describes 4-CW OFDM payload capacities for all rates, while current wide OFDM promotes R1/2+ to 8 CW by policy.

## Stop Condition

The original architectural stop was the missing half-duplex OTASim
semantics. The core/client semantics are now fixed, but live-daemon
smoke testing is still blocked in this sandbox by UDP bind denial. The
next useful step is to rerun `GrpcServiceSmoke` and the external
`ota_simulator serve`/client path from an unrestricted shell, then run
the Phase 2 multi-seed sweep before changing the production rate ladder.
