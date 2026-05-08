# Carrier-Mask Regression Forensics (2026-05-08)

## Finding

`PHY_MASK_V1` negotiation was not the failing layer. Both baseline and
synthetic-notch simulator runs negotiated the capability and enabled
CarrierLDPC on both stations before the DATA burst.

The failing path was a burst-group completeness bug in the simulator message
transfer. The default connected OFDM burst-interleaver group is eight frames,
but `cli_simulator --test` queues seven DATA frames. `StreamingEncoder` only
interleaves full groups:

```
full_groups = encoded_frames.size() / BURST_GROUP_SIZE
```

With seven frames and group size eight, `full_groups == 0`; no LTS marker is
emitted, so the receiver does not enter the burst deinterleaver accumulator.
The log still printed `burst_interleave=yes`, which hid the bypass.

## Evidence

Reproduced on current `main` with:

```
./build/cli_simulator --snr 15 --fading good --rate r1_2 \
  --log-level info --log-categories all --test

./build/cli_simulator --snr 15 --fading good --rate r1_2 \
  --mask-clear-carrier 17 --log-level info --log-categories all --test
```

Observed baseline: `TEST PASSED`, but ALPHA reported
`retransmissions=14 timeouts=13`. Synthetic notch passed in this checkout but
still degraded to `retransmissions=8 timeouts=5`.

Both logs contained `Connection: PHY_MASK_V1 negotiated` and
`CarrierLDPC interleaver: ENABLED` on both peers. Neither contained
`Burst interleaved group` or `Burst group complete` for the DATA run.

Falsification run:

```
./build/cli_simulator --snr 15 --fading good --rate r1_2 \
  --burst-group-size 7 --test

./build/cli_simulator --snr 15 --fading good --rate r1_2 \
  --mask-clear-carrier 17 --burst-group-size 7 --test
```

Both completed with ALPHA `retransmissions=0 timeouts=0`, proving the
seven-frame partial group was the material failure.

Raw logs:

- `/tmp/notch_baseline_diag.log`
- `/tmp/notch_17_diag.log`
- `/tmp/notch_baseline_group7_diag.log`
- `/tmp/notch_17_group7_diag.log`
- `/tmp/codex_carrier_mask_round1.log`

## Fix Direction

The fix is protocol-level group completion, not demodulator threshold tuning.
For non-file OFDM message bursts, incomplete multi-frame burst-interleaver
groups are padded with valid dummy DATA frames addressed to `ULPAD`; receiver
routing ignores those frames after PHY decode. Existing file-transfer padding
policy is preserved to avoid changing the measured 20 KB throughput path
without hardware evidence.
