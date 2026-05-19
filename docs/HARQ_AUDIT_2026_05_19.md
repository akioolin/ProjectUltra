# HARQ Soft-Combine Audit (2026-05-19)

## Executive Diagnosis

The R1/4 Good SNR=15 storm is not primarily a soft-combine coordinate bug. The A/B scanner showed the same upstream failure shape with HARQ off and on: uniform 4/4 CW failures, LTS residual CFO corrections far above the Good-channel Doppler budget, long TX queue backlog, and slow handshake completion.

This pass contained two modem-side P0 causes:

- LTS residual CFO was allowed to create a correction from a zero trusted CFO seed. On TX CFO=0 Good-fading runs, the chirp/cache seed was 0.00 Hz, but LTS residual estimates of about +/-1 Hz were being fed back as if they were oscillator CFO. The LTS path now only applies residual CFO when the residual is coherent and refines an already trusted non-zero seed. Ignored residuals are logged under `ULTRA_CFO_DEBUG_LOG=1` or `ULTRA_HARQ_DEBUG_LOG=1`. Code: `src/ofdm/channel_equalizer_lts.cpp:20`, `src/ofdm/channel_equalizer_lts.cpp:335`, `src/ofdm/channel_equalizer_lts.cpp:348`, `src/ofdm/channel_equalizer_lts.cpp:403`.
- Burst interleaving was enabled for low-rate R1/4 OFDM. In this cell that spread one erased physical block across every logical frame and made failures appear as uniform 4/4 CW failures. Burst interleaving is now gated to high-throughput OFDM modes, and control/non-file partial bursts are padded only for high-throughput modes. Code: `tools/sim/simulated_station.hpp:1169`, `src/protocol/connection.cpp:57`.

The remaining end-to-end storm is ACK/control-path loss, not data LDPC collapse. In the fixed OTA HARQ-on run, BRAVO decoded 8 DATA frames and sent 18 ACKs plus 18 SACKs, while ALPHA received 0 ACKs and 1 SACK; the run failed at 112 retx after every TX slot reached max retries. That cannot be solved by DATA soft combining.

Because the operator-visible HARQ-on OTA path still regresses/fails in this cell, `cli_simulator` now defaults HARQ off. HARQ remains opt-in with `--harq`. Code: `tools/cli_simulator.cpp:1136`, `tools/cli_simulator.cpp:2535`, `tools/cli_simulator.cpp:2651`.

## P0 Evidence

All `cli_simulator` logs below were scanned with `scripts/scan_cli_log.py`.

| Run | Verdict | Retx | Scanner anomalies |
| --- | --- | ---: | --- |
| Direct R1/4 Good SNR=15, HARQ off, after P0 fixes | PASS | 40 | Handshake duration 18.0s; TX_pending 271488 samples |
| Direct R1/4 Good SNR=15, HARQ on, after P0 fixes | PASS | 38 | Handshake duration 18.0s; TX_pending 272448 samples |
| OTA R1/4 Good SNR=15, HARQ off, after P0 fixes | PASS | 85 | Handshake duration 18.0s; TX_pending 270048 samples |
| OTA R1/4 Good SNR=15, HARQ on, after P0 fixes | FAIL | 112 | Handshake duration 18.0s; TX_pending 272928 samples |

What changed versus the original scanner addendum:

- No scanner LTS CFO budget anomaly remained after the LTS trusted-seed gate. There were many `LTS residual CFO ignored` diagnostics, but no `LTS residual CFO: ... corrected` events.
- No scanner uniform 4/4 CW anomaly remained after disabling low-rate burst interleaving. Some individual 4/4 failures still occur, but not the previous 100% uniform failure pattern.
- No `Connection: Handshake fail-safe triggered` events remained after extending the responder wait to cover the CONNECT_ACK rescue retry. Code: `src/protocol/connection_handlers.cpp:336`, `src/protocol/connection_handlers.cpp:344`, `src/protocol/connection_handlers.cpp:346`, `src/protocol/connection_handlers.cpp:354`, plus the manual accept path at `src/protocol/connection.cpp:357`.

Open P0/control issue:

- Handshake duration is still 18.0s and TX queue depth still reaches about 5.7s. The scanner correctly keeps flagging this. The current evidence points at MC-DPSK/OFDM control delivery and radio queue timing, especially ACK/SACK delivery on Good fading with ACK repeat count=1.
- ARQ timers start when a DATA frame is submitted to `transmitData()`, before any simulator/radio queue on-air callback exists. Fixed-frame send sets `timeout_ms` and `first_tx_ms`, then calls `transmitData()`. Retransmit does the same. The callback is just `on_transmit_(data)`. With 5.7s queued audio, timers can age while samples are still pending in the radio model. Code: `src/protocol/selective_repeat_arq.cpp:222`, `src/protocol/selective_repeat_arq.cpp:223`, `src/protocol/selective_repeat_arq.cpp:246`, `src/protocol/selective_repeat_arq.cpp:995`, `src/protocol/selective_repeat_arq.cpp:996`, `src/protocol/selective_repeat_arq.cpp:1376`.

## LLR Coordinate Audit

The canonical retained/combined HARQ vector is the LDPC input codeword order after frame deinterleaving and optional channel deinterleaving.

Decode path:

1. `StreamingDecoder::decodeFrame()` receives the OFDM soft-bit vector and calls `v2::decodeFixedFrame()`. The call happens after selecting the fixed CW count and before LDPC reassembly. Code: `src/gui/modem/streaming_ofdm_decode.cpp:2142`, `src/gui/modem/streaming_ofdm_decode.cpp:2237`.
2. `decodeFixedFrame()` first frame-deinterleaves the full frame into per-CW vectors. Code: `src/protocol/frame_v2.cpp:1801`.
3. If channel interleaving was enabled, each CW vector is channel-deinterleaved before HARQ combine and before LDPC decode. Code: `src/protocol/frame_v2.cpp:1858`.
4. `SoftCombineBuffer::combine()` receives the same post-frame-deinterleave, post-channel-deinterleave per-CW vector that LDPC will decode. Code: `src/protocol/frame_v2.cpp:1863`, `src/fec/soft_combine.cpp:154`.
5. Failed CWs are retained from `decoder_soft_bits`, which is populated from the same canonical `cw_bits` variable after combine/deinterleave. Code: `src/protocol/frame_v2.cpp:1878`, `src/protocol/frame_v2.cpp:1840`, `src/protocol/frame_v2.cpp:1850`.

Result: retained and incoming vectors are in the same coordinate space for the current fixed-frame OFDM path. The P0 uniform-failure symptom was caused upstream by burst interleaving and false LTS correction, not by retained/new HARQ vectors being in different coordinate spaces.

## Sign Convention Audit

`decodeFixedFrame()` passes the retained/new/summed LLR vector directly to the LDPC decoder after deinterleaving. The soft-combine implementation sums LLRs in place and clamps only the accumulated result. Code: `src/fec/soft_combine.cpp:181`, `src/fec/soft_combine.cpp:192`, `src/fec/soft_combine.cpp:195`.

The HARQ debug run produced two real combine hits that decoded successfully:

- `seq=6 cw=3/4`: attempts=2, sign_disagree=163/648, mean_abs retained=2.048, new=2.404, sum=4.065, decode-after-combine success=1.
- `seq=3 cw=1/4`: attempts=2, sign_disagree=166/648, mean_abs retained=2.453, new=1.962, sum=4.089, decode-after-combine success=1.

That is consistent with same-sign LLR convention and independent soft information: mean absolute magnitude increased after summing, and LDPC decoded after combine. A global sign inversion would tend to cancel magnitudes and fail the post-combine decode.

## Key Completeness Audit

The HARQ key includes sender hash, sequence, code rate, total CW count, CW index, modulation, channel-interleave flag, and a geometry hash over waveform mode plus OFDM carrier count. Code: `src/fec/soft_combine.hpp:13`, `src/fec/soft_combine.hpp:59`.

The key is built only when CW0 can be decoded well enough to parse a data-frame header, and the header `total_cw` must match the current decode CW count. Code: `src/gui/modem/streaming_ofdm_decode.cpp:2176`, `src/gui/modem/streaming_ofdm_decode.cpp:2187`, `src/gui/modem/streaming_ofdm_decode.cpp:2191`.

Known limitation: the key does not include any future dynamic pilot-layout or dynamic interleaver-seed fields. Today the fixed OFDM modes have stable carrier/interleaver shape for a given waveform/mode/rate/modulation tuple, and ARQ retransmits the same DATA frame sequence without changing the fixed-frame permutation.

## Channel Independence Audit

The Good channel uses slow fading. At 0.1 Hz Doppler, coherence time is on the order of 10 seconds, while ARQ retransmission timing is well inside that interval. Chase combining therefore should not be expected to deliver a clean 3 dB gain per doubling of retransmissions on this cell even when the implementation is correct. The direct run after P0 fixes shows only a small HARQ delta: 40 retx off versus 38 retx on.

This is a physics limitation, not a soft-buffer proof failure. HARQ remains useful only when retransmissions provide genuinely new noise/channel observations and the ACK/control path is not the bottleneck.

## False-Positive Interaction

CRC-guided LDPC false-positive recovery runs before HARQ finalization. If all CWs decode but the reassembled frame is invalid, the code marks affected CWs failed and only then calls `finalize_harq()`. Code: `src/protocol/frame_v2.cpp:1998`, `src/protocol/frame_v2.cpp:2018`, `src/protocol/frame_v2.cpp:2027`, `src/protocol/frame_v2.cpp:2033`, `src/protocol/frame_v2.cpp:2275`, `src/protocol/frame_v2.cpp:2284`.

This means HARQ does not retain a frame as successful merely because LDPC produced a wrong codeword with a valid syndrome. However, when a false-positive recovery fails, the current policy can retain the pre-recovery soft vectors for all CWs. That is safer than retaining decoded bytes, but it still deserves later scrutiny because a false-positive event marks that observation as suspicious.

## HARQ Disabled Path

The HARQ disabled path does not call `SoftCombineBuffer::combine()` or increment HARQ key miss counters because `buildHarqKey()` returns before building a key when the buffer is absent or disabled. Code: `src/gui/modem/streaming_ofdm_decode.cpp:2153`, `src/gui/modem/streaming_ofdm_decode.cpp:2219`, `src/gui/modem/streaming_ofdm_decode.cpp:2228`.

`SoftCombineBuffer` itself is disabled by default. Code: `src/fec/soft_combine.hpp:96`. The simulator default now matches that safer protocol default and requires `--harq` to engage soft combining.

## Next Work

The next root cause is the ACK/control path, not HARQ coordinate space:

- Is OFDM ACK/SACK decode too weak at R1/4 Good SNR=15 with ACK repeat count=1?
- Move ARQ timeout accounting to an on-air/queue-drained event, or make the simulator radio queue expose enough timing for ARQ to subtract pending audio before declaring a timeout.
- Is the 5.7s queued-audio backlog making the scanner handshake/TX_pending anomalies a simulator queue artifact?
- Should OFDM ACK robustness be restored with delayed time-spaced repeat, lower-rate ACK modulation, or piggy-backed cumulative ACK in return DATA?

Until those are answered, HARQ should stay opt-in for simulator runs and should not be used as the default operator path.
