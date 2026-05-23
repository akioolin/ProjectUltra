# Coherent Listen Window Fix - 2026-05-22

## Scope

This change keeps the half-duplex listen-window and adaptive SACK behavior only
on coherent wide-OFDM data modes. Differential wide OFDM, MC-DPSK, and
OFDM_NARROW stay on the timing path they had at baseline.

The practical reason is first-principles half-duplex timing: the fast coherent
QAM16 path needs a guaranteed RX interval after DATA so the ACK can land while
ALPHA is listening. The slow DQPSK R1/4 path already had a longer, audited
feedback cadence; adding the 306 ms QAM listen slot to that path made ALPHA go
deaf before the full-anchor ACK arrived.

## Timing Policy

The coherent-only gate is:

```text
wide OFDM && waveform != OFDM_NARROW &&
isCoherentModulation(mod) && !isDifferentialModulation(mod)
```

That admits coherent OFDM data constellations such as QPSK/QAM8/QAM16/QAM32/QAM64
through the existing coherent modulation helper, and excludes DBPSK/DQPSK/D8PSK.

For coherent wide OFDM DATA, the post-DATA listen maximum is derived:

```text
W = receiver_T/R_switch + receiver_decode_budget +
    ACK_airtime * ack_copies + guard
```

Measured current QAM16 cell:

```text
receiver_T/R_switch = 20 ms
receiver_decode_budget = 4 callbacks * 10 ms = 40 ms
ACK_airtime = 216 ms for the 1-CW OFDM control frame
ack_copies = 1
guard = 30 ms
W = 20 + 40 + 216 + 30 = 306 ms = 14688 samples at 48 kHz
```

This is a maximum. `SelectiveRepeatARQ` notifies `Connection` when an ACK/SACK
advances the transmit base, and `SimulatedStation` releases the post-DATA listen
hold immediately. Clean cells therefore do not wait out the full 306 ms.

Adaptive coherent SACK timing uses one expected DATA frame airtime plus the
30 ms coalescing guard. A real full burst still ACKs at the batch threshold; a
short or drained burst no longer waits a whole static window.

Differential/MC-DPSK/narrow paths:

- no coherent post-DATA listen window
- no adaptive SACK timing model
- no coherent carrier-sense guard shrink
- wide differential OFDM keeps full-window SACK hold, 3 ACK copies, and the
  audited DQPSK R1/4 ACK timeout of 12446 ms
- MC-DPSK and OFDM_NARROW keep the existing HEAD mode-specific connection policy
- global `ARQConfig` defaults remain `turnaround_ms=500` and `sack_delay_ms=2000`

## Before/After Gate

`E2E` is the simulator's committed data-phase metric: handshake excluded, dead
air included. `Useful` is original DATA CW divided by total keyed DATA/ACK CW
from `--phy-diag-log`.

For AWGN before rows, only a local seed42 pre-structural log was retained. The
independent pre-fix control supplied with this task was 801 bps, 56 percent dead
air, 57 percent useful airtime, and 12 timeouts across AWGN seeds 42/43/44.

| Cell | Seed | Before E2E / dead air / timeouts / useful | After E2E / dead air / timeouts / useful | Result |
| --- | ---: | --- | --- | --- |
| DQPSK R1/4 Good/SNR12 1KB | 42 | 221 bps / 43% / 4 / 76.2% | 221 bps / 43% / 4 / 76.2% | PASS: exact 391 on-air, 20/4/4 restored |
| QAM16 R1/2 AWGN/SNR20 5KB | 42 | 1131 bps / 45% / 5 / 72.0% | 2065 bps / 7% / 0 / 98.1% | PASS |
| QAM16 R1/2 AWGN/SNR20 5KB | 43 | 801 bps / 56% / 12 / 57% ref | 2067 bps / 7% / 0 / 98.1% | PASS |
| QAM16 R1/2 AWGN/SNR20 5KB | 44 | 801 bps / 56% / 12 / 57% ref | 2065 bps / 7% / 0 / 98.1% | PASS |
| QAM16 R1/2 Good/SNR20 5KB | 42 | 1457 bps / 29% / 0 / 86.4% | 1669 bps / 20% / 0 / 92.1% | PASS |
| QAM16 R1/2 Good/SNR20 5KB | 43 | 695 bps / 53% / 12 / 55.3% | 1097 bps / 34% / 8 / 67.9% | PASS |
| QAM16 R1/2 Good/SNR20 5KB | 44 | 360 bps / 65% / 26 / 37.3% | 704 bps / 53% / 9 / 59.8% | PASS |
| QAM16 R1/2 Good/SNR20 20KB spot | 42 | 1411 bps / 39% / 1 / 82.6% | 576 bps / 60% / 57 / 51.0% | Delivered; supporting spot regressed, not in final 5-point gate |

## Diagnostic Evidence

DQPSK differential path:

- `/tmp/dqpsk_gated.diag` has only four `event=arq_timeout` entries.
- Those timeouts are seq 16, 17, 18, and 19, all with `ack_timeout_ms=12446`.
- There are zero first-window seq 0..7 timeouts.
- There are zero `event=station_post_data_listen_pending` entries.
- Every post-TX listen arm is `source=legacy samples=1440 ms=30`.

Coherent QAM16 path:

- `/tmp/qam_awgn_42_gated.diag` logs
  `event=station_post_data_listen_pending ... ms=306 ack_airtime_ms=216
  ack_copies=1 rx_tr_switch_ms=20 decode_budget_ms=40 guard_ms=30`.
- Advancing ACKs release the hold early, for example
  `event=station_post_tx_listen_release ... old_base=8 new_base=16`.
- Clean AWGN seeds 42/43/44 all complete at 2065-2067 bps E2E with 7 percent
  dead air and zero timeouts.
- Good seed44 still has fades, but the livelock storm is reduced from 26
  timeouts to 9 and E2E goodput rises from 360 to 704 bps.
- No `collision` or `overlap` diagnostic events appeared in the gated QAM16 or
  DQPSK logs checked.

Post-fix carrier-sense defer share from the diag logs:

| Cell | Carrier-sense defers / all defers |
| --- | ---: |
| AWGN seed42 | 2 / 4 = 50% |
| AWGN seed43 | 2 / 4 = 50% |
| AWGN seed44 | 2 / 4 = 50% |
| Good seed42 | 1 / 3 = 33% |
| Good seed43 | 4 / 16 = 25% |
| Good seed44 | 6 / 20 = 30% |
| Good 20KB seed42 | 18 / 94 = 19% |

## Commands Run

```bash
cmake --build build --target cli_simulator test_connection_policy test_connection_adaptive test_deferred_tx_fragmentation test_selective_repeat test_selective_repeat_policy -j4
./build/tests/test_connection_policy
./build/tests/test_connection_adaptive
./build/tests/test_deferred_tx_fragmentation
./build/tests/test_selective_repeat
./build/tests/test_selective_repeat_policy
./build/cli_simulator --channel good --snr 12 --file 1024 --seed 42 --log-level error --phy-diag-log /tmp/dqpsk_gated.diag
./build/cli_simulator --expert --channel awgn --snr 20 --file 5120 --seed 42 --mod qam16 --rate 1/2 --log-level error --phy-diag-log /tmp/qam_awgn_42_gated.diag
./build/cli_simulator --expert --channel awgn --snr 20 --file 5120 --seed 43 --mod qam16 --rate 1/2 --log-level error --phy-diag-log /tmp/qam_awgn_43_gated.diag
./build/cli_simulator --expert --channel awgn --snr 20 --file 5120 --seed 44 --mod qam16 --rate 1/2 --log-level error --phy-diag-log /tmp/qam_awgn_44_gated.diag
./build/cli_simulator --expert --channel good --snr 20 --file 5120 --seed 42 --mod qam16 --rate 1/2 --log-level error --phy-diag-log /tmp/qam_good_42_gated.diag
./build/cli_simulator --expert --channel good --snr 20 --file 5120 --seed 43 --mod qam16 --rate 1/2 --log-level error --phy-diag-log /tmp/qam_good_43_gated.diag
./build/cli_simulator --expert --channel good --snr 20 --file 5120 --seed 44 --mod qam16 --rate 1/2 --log-level error --phy-diag-log /tmp/qam_good_44_gated.diag
./build/cli_simulator --channel good --snr 12 --file 1024 --seed 42 --log-level error --phy-diag-log /tmp/dqpsk_gated_repeat.diag
./build/cli_simulator --expert --channel good --snr 20 --file 5120 --seed 44 --mod qam16 --rate 1/2 --log-level error --phy-diag-log /tmp/qam_good_44_gated_repeat.diag
ctest --test-dir build -j4
./build/cli_simulator --expert --channel good --snr 20 --file 20480 --seed 42 --mod qam16 --rate 1/2 --log-level error --phy-diag-log /tmp/qam_good_20kb_42_gated.diag
```

Determinism:

- DQPSK repeat seed42 matched exactly: 391 on-air, 221 E2E, 43 percent dead air,
  20 frames sent, 4 retransmissions, 4 timeouts.
- QAM16 Good seed44 repeat matched exactly: 1491 on-air, 704 E2E, 53 percent
  dead air, 19 frames sent, 11 retransmissions, 9 timeouts.

Full suite:

```text
100% tests passed, 0 tests failed out of 92
```

## Propagation Notes

The shared protocol changes apply through `ProtocolEngine` and `Connection`:
coherent ARQ/SACK policy, ACK-window-advance callback plumbing, and the
differential timing gate are not simulator-only.

The transport-level listen enforcement is currently implemented in
`tools/sim/simulated_station.hpp`, which is the path exercised by
`cli_simulator` and OTASim. The same transport behavior must be mirrored before
claiming hardware parity in:

- `src/gui/modem/modem_engine.*`: tag coherent DATA TX, arm the derived W after
  the keyed DATA burst drains, forbid new DATA while W is active, and release W
  on `ProtocolEngine::setTransmitWindowAdvancedCallback`.
- `tools/ultra_tnc.cpp` / the TNC audio/PTT path: apply the same DATA-tagged
  listen hold around real PTT/audio queue completion.

Do not apply that transport listen hold to DBPSK/DQPSK/D8PSK, MC-DPSK, or
OFDM_NARROW unless those paths get their own audited slow-mode timing gate.
