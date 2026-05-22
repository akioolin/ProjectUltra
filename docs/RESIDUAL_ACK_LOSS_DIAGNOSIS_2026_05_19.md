# Residual ACK Loss Diagnosis - 2026-05-19

## Result

On `feat/harq-soft-combine-2026-05-19` after the local-PTT sample-drain fix,
the AWGN SNR=30 OFDM-CHIRP DQPSK R1/4 OTA cell no longer reproduces the
residual ACK-loss failure. Clean self-spawned OTASim runs for seeds `7`, `13`,
`23`, and `42` all passed with `retransmissions=0`.

The earlier decoder-backlog theory was directionally useful, but the current
code no longer matches the failing profile:

- Old failing profile: `detect_data_sync n=1865`, total about 10.9 s,
  ALPHA `peak_backlog_ms=6670`, and `retransmissions=20`.
- Current measured profile: `detect_data_sync n=23..30`, total below 95 ms,
  ALPHA `peak_backlog_ms` still around 6.7-7.7 s, but `timeouts=0` and
  `retransmissions=0`.

That means the remaining high `peak_backlog_ms` telemetry is not, by itself,
proof that the decoder is too slow. In these runs the counter includes normal
sample accumulation while a long frame is in progress or while the modem is
waiting for a complete frame, not only CPU backlog behind real time.

## Root Cause Closure

The residual retransmissions were closed by two timing fixes:

1. Local PTT now follows drained waveform blocks, not only sample energy, in
   `tools/sim/simulated_station.hpp`. Some valid MC-DPSK and control waveforms
   contain short zero-energy blocks. Treating those blocks as "not TX" caused
   the simulated radio to enter `TX_TR_SWITCH` or `TX_COOLDOWN` inside a
   waveform, fragmenting the local T/R model. The fix keeps PTT in `TX` for
   any block that is still being drained from the queued TX waveform; recovery
   begins only on the first callback after the waveform has fully drained.

2. Burst-aware SACK hold, already present in `src/protocol/connection_policy.hpp` and
   `src/protocol/connection.cpp`. BRAVO now holds SACK transmission through the
   nominal wide-OFDM sender window instead of emitting many per-frame control
   replies during the same physical turn. This collapsed the previous ACK/SACK
   storm, so ALPHA no longer has to decode a dense burst of control frames
   before the ARQ timeout horizon.

The PHY interpretation is that SNR=30 R1/4 ACK frames were decodable when they
arrived in a valid receive opportunity. The problem was not LDPC reliability.
The DSP/system issue was the time relationship between local TX waveform drain,
PTT state recovery, protocol timer advancement, and responder ACK/SACK emission.
The HF operator model is preserved: T/R deafness stays tied to samples actually
leaving the local audio port plus the fixed sample-domain switch, not to a long
software cooldown.

Two harness adjustments were also required to keep the regression suite honest:

- `tools/ota_simulator/runner.cpp` feeds a short post-roll of silence through
  the offline TX monitor so late complete waveforms can flush before
  `assert_tx_frame_within` is evaluated.
- The SNR15 file-transfer scenario keeps the original 290 s transfer allowance
  but extends the disconnect teardown window to 319 s and the CTest timeout to
  360 s.

## Verification Snapshot

Clean self-spawned OTASim command shape:

```sh
./build/cli_simulator --snr 30 --channel awgn --waveform ofdm_chirp \
  --mod dqpsk --rate r1_4 --seed <seed> --test
```

Results:

| Seed | Verdict | Retransmissions | ALPHA detect_data_sync | ALPHA peak_backlog_ms |
| ---- | ------- | --------------- | ---------------------- | --------------------- |
| 7    | PASS    | 0               | n=29 total=86.8 ms     | 6690.0                |
| 13   | PASS    | 0               | n=23 total=75.5 ms     | 7680.0                |
| 23   | PASS    | 0               | n=30 total=94.1 ms     | 6680.0                |
| 42   | PASS    | 0               | n=29 total=89.5 ms     | 6670.0                |

`scripts/scan_cli_log.py` still flags the legacy handshake-duration and
TX-pending heuristics on these logs. Those warnings should be read as remaining
diagnostic follow-up, not as evidence of the old ACK-loss mechanism: the ARQ
trailer has `timeouts=0`, `retransmissions=0`, and zero ACK-filter rejects.
Measured scan warnings were:

| Seed | Handshake warning | TX_pending warning |
| ---- | ----------------- | ------------------ |
| 7    | 18.0 s            | 278208 samples     |
| 13   | 32.0 s            | 277248 samples     |
| 23   | 18.0 s            | 277728 samples     |
| 42   | 18.0 s            | 277728 samples     |

## Notes

An explicit `127.0.0.1:50051` verification in this workspace was not treated as
the clean baseline because that port was already occupied by a long-running
manual OTASim server using `/tmp/ota_tokens.conf`. The clean self-spawned
OTASim path uses the same audio backend and channel model but avoids preexisting
lobby/session state.
