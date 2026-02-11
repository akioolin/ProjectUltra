# D8PSK Control-Path Hardening Plan

## Goal
Make D8PSK data modes usable in fading by fixing control-path fragility (ACK/SACK/DISCONNECT ACK behavior), without rewriting the protocol stack.

## Success Criteria
1. D8PSK R1/2, OFDM, SNR 20, good fading, 30 seeds, message test:
- 30/30 pass
- Average retransmissions per seed <= 2
- Timeouts in <= 10% of seeds
2. D8PSK R1/2, OFDM, SNR 20, good fading, 30 seeds, file 2048B test:
- 30/30 pass
- No `File transfer reported failure`
- Average retransmissions per seed <= 3
3. Regression:
- DQPSK R2/3 good fading remains 30/30 pass
- QPSK R1/2 good fading remains at or above current reliability

## Root Cause Summary
1. Data path is robust due to 4-CW frame interleaving.
2. Control path is fragile because ACK/SACK are effectively single-CW control events under fading.
3. ACK repeat copies are too tightly clustered in time, so deep fades can erase all repeats.
4. Timeout behavior is model-based and static, not measured RTT adaptive.

## Workstream A: Split Control PHY From Data PHY

### A1. Introduce explicit OFDM control profile
Files:
- `src/gui/modem/streaming_encoder.cpp`
- `src/gui/modem/streaming_decoder.cpp`
- `src/gui/modem/streaming_encoder.hpp` (if new helpers are needed)
- `src/gui/modem/streaming_decoder.hpp` (if state fields are needed)

Implementation:
1. Define a control profile for connected OFDM control frames:
- modulation: `DQPSK`
- code rate: `R1_4`
- pilot spacing: dense (same robust setting used by current hardened control)
2. Keep data profile unchanged for data frames.
3. In encoder, for control frame types (`ACK`, `NACK`, `MODE_CHANGE`, `PROBE_ACK`, `KEEPALIVE`, disconnect ACK path), encode using control profile regardless of current data modulation.
4. Keep existing frame format unchanged (no protocol break).

Acceptance:
1. Logs clearly show control profile applied for control TX.
2. Data TX logs still show requested high-rate profile.

### A2. Decoder control-first hypothesis
Files:
- `src/gui/modem/streaming_decoder.cpp`

Implementation:
1. In connected OFDM RX path, try control-profile decode first on short frame buffers.
2. If valid 1-CW control header is found, accept immediately.
3. If not valid control, fall back to existing data decode path (including 4-CW deinterleave).
4. Avoid escalating likely control frames into full 4-CW decode unless header evidence requires it.

Acceptance:
1. ACK decode success increases on problematic D8PSK seeds.
2. No regression in data frame decode success.

## Workstream B: ACK Time-Diversity Scheduling

### B1. Spread ACK repeats over decorrelated time
Files:
- `src/protocol/selective_repeat_arq.cpp`
- `src/protocol/selective_repeat_arq.hpp`
- `src/protocol/connection.cpp` (mode-based policy wiring)

Implementation:
1. Replace tight ACK repeat spacing (for example 80-110 ms) with spaced schedule:
- copy 1: immediate
- copy 2: +250 ms
- copy 3 (if enabled): +700 ms
2. Add deterministic jitter per ACK sequence (for example +/- 30 ms) to avoid pathological periodic collisions.
3. Keep repeat count mode-aware:
- D8PSK R1/2: 3 copies
- others: 2 copies (unless testing shows need for 3)

Acceptance:
1. ACK receive ratio improves in D8PSK fading logs.
2. Control traffic overhead remains acceptable (no runaway queue).

## Workstream C: RTT-Adaptive RTO (Timeout)

### C1. Add sender RTT estimator in SR-ARQ
Files:
- `src/protocol/selective_repeat_arq.hpp`
- `src/protocol/selective_repeat_arq.cpp`

Implementation:
1. Add TX timestamp per slot at first send.
2. On ACK-derived completion, compute RTT sample only if frame was never retransmitted (Karn-safe sample).
3. Maintain:
- `srtt_ms`
- `rttvar_ms`
- `rto_ms = srtt + 4 * rttvar`
4. Clamp `rto_ms` to sane bounds:
- floor around 1200 ms
- ceiling around 12000 ms
5. Use `rto_ms` when arming per-slot timeout instead of only static modeled timeout.

Acceptance:
1. Timeout count drops, especially on long file transfers.
2. No oscillation causing aggressive spurious retransmits.

## Workstream D: Instrumentation and Diagnostics

### D1. Add explicit counters and log tags
Files:
- `src/protocol/selective_repeat_arq.cpp`
- `src/gui/modem/streaming_decoder.cpp`
- `src/protocol/connection.cpp`

Metrics to add:
1. ACK sent, ACK decoded, ACK repeats scheduled, ACK repeats delivered.
2. Control-first decode hits, fallback hits, salvage hits.
3. RTT samples count, current SRTT, current RTO.

Acceptance:
1. Post-run logs can explain whether bottleneck is decode loss, fade loss, or timeout policy.

## Test Plan

## T1. Core D8PSK validation
Commands (example):
1. `./build/cli_simulator --snr 20 --channel good --rate r1_2 --mod d8psk --waveform ofdm_chirp --seed <seed> --test`
2. `./build/cli_simulator --snr 20 --channel good --rate r1_2 --mod d8psk --waveform ofdm_chirp --seed <seed> --file 2048 --test`

Run:
1. Seeds: 30 deterministic seeds.
2. Report:
- pass/fail
- retransmissions
- timeouts
- ACK sent vs ACK received

## T2. Regression matrix
1. DQPSK R2/3 good fading, 30 seeds.
2. QPSK R1/2 good fading, 30 seeds.
3. DQPSK R1/4 moderate and good fading sanity checks.

## T3. Edge checks
1. Disconnect flow in fading (ensure no stale ACK misqualification regressions).
2. Verify no CRC or reassembly regressions on file transfer.

## Rollout Plan
1. Implement A first (largest reliability gain).
2. Implement B second (time-diversity gain).
3. Implement C third (latency and stall reduction).
4. Keep each workstream in separate commit for fast rollback.
5. If needed, guard B/C with runtime toggles to A/B in simulator before defaulting on.

## No-Go Conditions
1. Any regression in DQPSK R2/3 baseline reliability.
2. Increased false positives in control decode (bad header acceptance).
3. RTO adaptation causing flapping retransmission behavior.

## Deliverables
1. Code changes for A/B/C/D.
2. Seed-run summary table for T1/T2/T3.
3. Final recommendation for default ACK policy by modulation/rate.

