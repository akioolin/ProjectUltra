# ACK Loss in Client Deaf Window Diagnosis

Date: 2026-05-19
Branch: `feat/harq-soft-combine-2026-05-19`

## Summary

The ACK loss was real, but the local radio blackout predicate was not the faulty
predicate. `RadioPttStateMachine::isInRxBlackout()` is already restricted to
`TX` and `TX_TR_SWITCH` (`tools/sim/simulated_station.hpp:281`). `TX_COOLDOWN`
is RX-open in both the state machine and the audio-port unit tests.

The loss path was:

1. ALPHA queued an 8-frame wide-OFDM ARQ window as one continuous physical burst.
2. BRAVO decoded early DATA frames while ALPHA was still physically transmitting
   later frames from the same burst.
3. Wide-OFDM SACK timing treated the default 8-frame window as if a 120 ms SACK
   delay was enough (`src/protocol/connection_policy.hpp:321` before this fix).
4. The OTA CLI also forced `ack_tx_delay_ms = 0`
   (`tools/cli_simulator.cpp:964` before this fix), removing the last
   post-DATA guard.
5. BRAVO therefore transmitted ACK/SACK audio while ALPHA was still in true
   `TX`. The samples reached ALPHA's audio port, but
   `AudioPort::shapeRxForLocalRadio()` zeroed them at
   `tools/sim/simulated_station.hpp:1757`, correctly modelling a keyed local
   receiver.

This is a timing bug above the radio gate, not a cooldown predicate bug. The
physics escape hatch is: the medium delivered the reply samples, but the local
receiver was still keyed, so the samples were legitimately consumed and muted
before the decoder/ARQ state machine could see them.

## Exact Bug Lines

- `src/protocol/connection.cpp:1748` previously enabled burst-tail SACK
  deferral only when `window_size > kWideOFDMWindowFrames`. The default
  wide-OFDM window is exactly 8 frames, so R1/4 used the short 120 ms SACK path.
- `src/protocol/connection_policy.hpp:321` previously computed an 8-frame
  deferral as zero deferred frames, again producing a 120 ms SACK delay for the
  default burst.
- `tools/cli_simulator.cpp:964` previously forced `ack_tx_delay_ms = 0` for OTA
  runs, so immediate ACK/SACK frames had no post-DATA TX-tail guard.

## Fix

- Wide OFDM now defers SACKs for every multi-frame window (`window_size > 1`).
- `ofdmSackDelays()` now uses `(window_size - 1) * data_frame_ms + 120 ms`,
  capped at 12 s, so the receiver does not transmit the delayed SACK until the
  sender's physical window burst has reached its tail.
- The short SACK override is disabled for OFDM because the existing
  `MORE_FRAG` flag is an application/file-fragment signal, not a reliable
  physical-burst-tail signal for batched messages.
- OTA CLI runs now use a 200 ms ACK guard instead of zero. This is shorter than
  the real-radio default 500 ms but long enough to avoid replies landing in the
  sender's TX/TR-switch edge.

## Verification

Pre-fix AWGN SNR=30 repro:

- `TEST FAILED`
- ALPHA: `acks_rcvd=1`, `sacks_rcvd=2`, `retransmissions=34`, `timeouts=36`
- BRAVO: `acks_sent=21`, `sacks_sent=21`, `frames_rcvd=8`
- Scanner: handshake and TX queue warnings, plus failed verdict.

Post-fix AWGN SNR=30 repro:

- Command: `./build/cli_simulator --ota-host 127.0.0.1:50051 --ota-alpha-token admin_tok --ota-bravo-token bravo_tok --snr 30 --channel awgn --waveform ofdm_chirp --mod dqpsk --rate r1_4 --test`
- Log: `/tmp/ack_loss_repro_after.log`
- Scanner was run: `python3 scripts/scan_cli_log.py /tmp/ack_loss_repro_after.log`
- Result: `TEST PASSED`, `retransmissions=0`, `timeouts=0`
- ALPHA: `acks_rcvd=8`, `sacks_rcvd=1`
- BRAVO: `acks_sent=2`, `sacks_sent=2`, `frames_rcvd=11`

Post-fix R1/4 Good SNR=15 re-measure:

- Command: `./build/cli_simulator --ota-host 127.0.0.1:50051 --ota-alpha-token admin_tok --ota-bravo-token bravo_tok --snr 15 --channel good --waveform ofdm_chirp --mod dqpsk --rate r1_4 --test`
- Log: `/tmp/r14_good_snr15_after.log`
- Scanner was run: `python3 scripts/scan_cli_log.py /tmp/r14_good_snr15_after.log`
- Result: `TEST PASSED`, `retransmissions=61`, `timeouts=59`
- ALPHA: `acks_rcvd=8`, `sacks_rcvd=3`
- BRAVO: `acks_sent=20`, `sacks_sent=20`, `frames_rcvd=11`

Post-fix R1/4 Good SNR=15 with HARQ re-measure:

- Command: `./build/cli_simulator --ota-host 127.0.0.1:50051 --ota-alpha-token admin_tok --ota-bravo-token bravo_tok --snr 15 --channel good --waveform ofdm_chirp --mod dqpsk --rate r1_4 --harq --test`
- Log: `/tmp/r14_good_snr15_harq_after.log`
- Scanner was run: `python3 scripts/scan_cli_log.py /tmp/r14_good_snr15_harq_after.log`
- Result: `TEST PASSED`, `retransmissions=30`, `timeouts=30`
- ALPHA: `acks_rcvd=10`, `sacks_rcvd=2`
- BRAVO: `acks_sent=11`, `sacks_sent=11`, `frames_rcvd=11`
- HARQ key build: `success=19`, `failed=1` (5% miss)

The scanner still flags the known 18 s handshake and large OFDM TX queue in both
post-fix runs. Those are not hidden; the ACK-loss criterion is fixed because
legitimate post-burst ACK/SACK frames now reach ALPHA's ARQ path at perfect SNR.
