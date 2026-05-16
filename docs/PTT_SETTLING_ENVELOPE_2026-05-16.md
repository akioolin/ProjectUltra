# PTT Settling Envelope - 2026-05-16

## Headline

ProjectUltra at v0.3.5+ supports radios with PTT-off settling up to **0 ms**
for the scripted full QSO measured here.

The first nonzero settling point, `rx_settling_ms=100`, reaches CONNECTED but
fails before BRAVO decodes ALPHA's first DATA frame. PROBE/PONG survives through
`1500 ms` in this harness; at `2000 ms`, the session fails first in PROBE.

## Measurement Scope

Branch: `test/session-ptt-sweep`, forked from `origin/main` at `cd58b41`.

Harness: `tests/test_session_ptt_sweep.cpp`, one CTest case per sweep point:
`SessionPttSweep_0ms`, `SessionPttSweep_100ms`, `SessionPttSweep_200ms`,
`SessionPttSweep_300ms`, `SessionPttSweep_500ms`, `SessionPttSweep_700ms`,
`SessionPttSweep_1000ms`, `SessionPttSweep_1500ms`, and
`SessionPttSweep_2000ms`.

Important current-branch detail: this commit no longer sends a standalone
initial `MODE_CHANGE` after `CONNECT_ACK`. The initial data mode is embedded in
`CONNECT_ACK` and applied in `Connection::handleConnectAck()`
(`src/protocol/connection_handlers.cpp:383-408`). The table's `MODE_CHG`
column therefore means the post-CONNECT data-mode handoff succeeded. The sweep
also records whether a standalone `MODE_CHANGE` frame was seen; it was `NO` for
all points.

## Sweep Table

| rx_settling_ms | PROBE | CONNECT | MODE_CHG | DATA | ACK | DISC | DONE | first failed |
|---:|---|---|---|---|---|---|---|---|
| 0 | OK | OK | OK | OK | OK | OK | YES | NONE |
| 100 | OK | OK | OK | FAIL | FAIL | FAIL | NO | DATA |
| 200 | OK | OK | OK | FAIL | FAIL | FAIL | NO | DATA |
| 300 | OK | OK | OK | FAIL | FAIL | FAIL | NO | DATA |
| 500 | OK | OK | OK | FAIL | FAIL | FAIL | NO | DATA |
| 700 | OK | OK | OK | FAIL | FAIL | FAIL | NO | DATA |
| 1000 | OK | OK | OK | FAIL | FAIL | FAIL | NO | DATA |
| 1500 | OK | OK | OK | FAIL | FAIL | FAIL | NO | DATA |
| 2000 | FAIL | FAIL | FAIL | FAIL | FAIL | FAIL | NO | PROBE |

Maximum survivable `rx_settling_ms`: **0 ms** for the full scripted QSO.

## Weakest Link

The weakest link is the responder `CONNECT_ACK` -> initiator first post-CONNECT
DATA edge. In the current branch, that first post-CONNECT frame is DATA, not a
standalone initial `MODE_CHANGE`.

Representative `100 ms` event-log timing:

- BRAVO finishes CONNECT_ACK TX and enters RX settling at `t=10.910s`.
- ALPHA decodes `CONNECT_ACK`, reaches CONNECTED, and queues `sendMessage
  hello` at `t=10.940s`.
- ALPHA starts DATA TX at `t=10.940s`.
- BRAVO does not return to RX until `t=11.010s`.
- BRAVO never decodes the DATA frame.

The simulator enforces this through `SimulatedStation::setRxSettlingMs()` and
`isInRxBlackout()` (`tools/sim/simulated_station.hpp:757-774`). When one station
transmits while the peer is blacked out, `SimulatedChannel::transmitFromA()`
or `transmitFromB()` drops those samples (`tools/sim/simulated_station.hpp:248-268`).
Losing the beginning of the light OFDM DATA waveform loses the sync/training
front of the frame, so no ACK is generated.

The PING/PONG edge is not the first failure here. The existing
`pong_tx_delay_ms=500` default (`src/protocol/connection.hpp:31`) plus decoder
latency leaves enough natural delay for PROBE through `1500 ms`; `2000 ms`
finally fails in PROBE.

## Recommended Next Mitigations

CONNECT_ACK -> first post-CONNECT DATA:

- Add an initiator-side post-CONNECT TX guard, for example
  `post_connect_tx_delay_ms`, before the first DATA or future explicit
  `MODE_CHANGE` frame.
- Make the guard rig-profile configurable and default it from the same
  PTT-settling model as PONG.

DATA -> ACK and DISCONNECT -> final ACK:

- Re-measure after the post-CONNECT guard exists; these edges are not reached at
  nonzero settling in the current full-QSO script.
- If they become the next cliff, add a symmetric `data_to_ack_delay_ms` or raise
  the short SACK/ACK delay floor for half-duplex rigs.

PROBE/PONG at very slow settling:

- Expose or increase `pong_tx_delay_ms` for rigs with settling near or above
  `2000 ms`.
- Keep the default conservative for normal rigs; this is a rig-profile knob,
  not a PHY or wire-format issue.

## Three-Perspective Check

PHY theorist: the failure is expected once any light OFDM frame starts inside
the peer's RX-deaf interval. The first lost samples include the sync/training
front, so the decoder cannot recover the frame later.

Real-time DSP systems engineer: the result is deterministic in the registered
CTest sweep. `ctest --test-dir build -R 'SessionPttSweep' --output-on-failure`
passed `9/9` in `285.18 s`, with each case matching the measured envelope.

Veteran HF operator: ProjectUltra currently supports a zero-delay post-PTT
radio for this fully automatic QSO. A rig needing even `100 ms` PTT-off settling
connects, then loses the first data transmission immediately after connect.

First-principles physics escape hatch: no disagreement. A half-duplex receiver
that is still physically settling cannot observe the beginning of the next
waveform, and the beginning is exactly where synchronization information lives.
