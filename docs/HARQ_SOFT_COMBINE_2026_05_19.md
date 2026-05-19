# HARQ Soft-Combining for OFDM_CHIRP (2026-05-19)

## Goal

OFDM_CHIRP R1/4 currently wastes retransmissions at the PHY edge: every retry is decoded as an isolated LDPC observation, so the receiver throws away useful soft information. This work adds receiver-side Chase combining for fixed-frame OFDM LDPC codewords. A failed codeword's LLRs are retained, the next retransmission of the same frame/codeword is summed with the retained LLRs, and LDPC is retried on the accumulated evidence.

Incremental Redundancy is out of scope for this pass. IR would require transmitter-side parity-version scheduling and LDPC matrix support for puncturing/extension. Chase combining works with the existing retransmission contract because the same coded bits are sent again.

## Multi-Perspective Check

PHY theorist:
Each retransmission carries another noisy observation of the same coded bit. For independent observations, the joint log-likelihood ratio is the arithmetic sum of the per-attempt LLRs. Two attempts double the expected LLR magnitude, which is about +3 dB effective SNR; four attempts give about +6 dB. Averaging would erase this gain.

Real-time DSP systems engineer:
Combining is done after frame deinterleaving and optional channel deinterleaving, so retained vectors are in LDPC codeword order, not wire/interleaver order. The HARQ key includes sender hash, frame sequence, total CW count, CW index, rate, modulation, channel-interleave state, and OFDM geometry hash. This prevents mixing LLRs across different PHY layouts. Retention is bounded by `ARQ window * fixed_frame_cw_count` and also protected by the existing TTL/LRU behavior.

Veteran HF operator:
Operators do not want a visible retry storm when a channel sits on the decoding edge. HARQ keeps ARQ behavior unchanged on the wire while making each retry useful. A marginal retry sequence should converge into a decoded frame instead of producing repeated isolated failures.

First-principles physics:
Near the channel capacity boundary, a single observation can be below the LDPC decoder's convergence threshold while the average of independent noise realizations is above it. Chase combining turns ARQ retries into repeated measurements of the same codeword and moves the decoder toward the Shannon-side of the boundary.

## Keying and Buffer Semantics

`fec::SoftCombineBuffer::Key` now includes:

- `sender_hash`
- `seq`
- `rate`
- `cw_count`
- `cw_index`
- `modulation`
- `channel_interleave`
- `carrier_count_hash`

The streaming OFDM fixed-frame path still builds a base HARQ key from decoded CW0 because the protected frame header is where `sender_hash`, `seq`, and `total_cw` live. `decodeFixedFrame()` derives per-codeword keys from that base key by setting `cw_index`.

Current limitation:
If CW0 cannot decode well enough to identify the frame, the receiver cannot safely key the soft bits by frame sequence. In that case HARQ is skipped and `harq_key_build_failed` instrumentation records the miss. Avoiding that would require a separate robust PHY header, an ARQ-side receive expectation contract, or another unambiguous frame-identity source before LDPC succeeds.

## Decode Flow

1. OFDM demodulation produces fixed-frame soft bits.
2. `StreamingDecoder::decodeFrame()` builds a base HARQ key when CW0 can be decoded and parsed.
3. `v2::decodeFixedFrame()` frame-deinterleaves the full soft-bit vector.
4. Each CW is channel-deinterleaved if the profile requires it.
5. For each CW, the decoder asks `SoftCombineBuffer` for the retained entry with the same frame/CW key.
6. If an entry exists, new LLRs are summed with retained LLRs and LDPC runs on the combined vector.
7. On final decode success for a CW, that CW's retained entry is dropped.
8. On final decode failure for a CW, that CW's combined LLR vector is retained for the next retransmission.

False-positive recovery uses the same deinterleaved and combined soft-bit vectors, so CRC-guided suspect selection and retry decoding are aligned with the actual LLRs that LDPC saw.

## Memory Bound and Cleanup

The connection configures the soft-combine buffer after ARQ profile selection:

```text
max_entries = arq_window_size * fixed_frame_cw_count
```

Typical OFDM_CHIRP R1/4 is 8 frames * 4 CW = 32 retained CW buffers. At 648 floats per CW, that is about 2.6 KB per entry before vector overhead, or about 83 KB for 32 entries.

Cleanup paths:

- Success: `decodeFixedFrame()` drops the retained entry for every successfully decoded CW.
- Failure: only failed CWs are retained.
- ARQ window slide: `SelectiveRepeatARQ::advanceRXWindow()` notifies `Connection`, which calls `SoftCombineBuffer::retainOnlySeqWindow(base_seq, window_size)` with wrap-safe sequence arithmetic.
- Mode/rate/CW geometry change: `Connection::applyDataMode()` clears HARQ state if the LDPC rate or fixed-frame CW count changes.
- TTL/LRU: existing `SoftCombineBuffer` expiration and overflow eviction remain in place.

## Chase vs IR Decision

This implementation is Chase combining:

- TX sends the same coded bits on retransmission.
- RX sums same-codeword LLRs.
- No wire-format change.
- No LDPC matrix change.

IR remains a later workstream:

- TX must select additional parity bits or a new puncturing pattern per retry.
- RX must track redundancy version identity.
- Decoder must support the resulting effective code-rate changes.

## Validation Targets

Focused tests added in `tests/test_soft_combine.cpp`:

- identical retransmission retention and reuse
- two individually failing noisy R1/4 LDPC observations that pass when summed
- CW-index key disambiguation
- sequence-window pruning with 16-bit wrap

End-to-end validation target:

```bash
./build/cli_simulator --ota-host 127.0.0.1:50051 \
  --ota-alpha-token admin_tok --ota-bravo-token bravo_tok \
  --snr 12 --channel awgn --mod dqpsk --rate r1_4 \
  --waveform ofdm_chirp --test
```

Expected result is `TEST PASSED` with retransmissions collapsing from the pre-HARQ storm level. `cli_simulator` now enables RX soft-combining by default; use `--no-harq` only for explicit A/B comparisons against the old isolated-decode behavior.
