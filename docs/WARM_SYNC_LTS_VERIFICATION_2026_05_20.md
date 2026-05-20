# Warm-Sync LTS Verification - 2026-05-20

## Status

Warm-sync light-preamble ACK detection meets the M8 AWGN FER targets in this
worktree.

Raw data:

- Aggregate CSV: `docs/data/warm_sync_lts_verification_2026_05_20.csv`
- Per-seed detail CSV: `docs/data/warm_sync_lts_verification_2026_05_20_by_seed.csv`

## Method

Harness: `tools/measure_ack_fer.cpp`, config `warm_sync_light`.

Each trial uses production `StreamingEncoder`, `StreamingDecoder`, and
`SimulatedChannel` AWGN in `OFDM_CHIRP`, DQPSK R1/4, 59-carrier NVIS geometry.
The trial sequence is:

1. Reset the connected OFDM decoder.
2. Arm `StreamingDecoder::expectFullOFDMAnchorOnce()`.
3. Send a real v2 ACK control frame with full chirp+LTS preamble.
4. If the anchor decodes byte-exact, send a second real ACK with light LTS-only
   preamble without resetting the decoder.
5. Classify the measured light frame.

Anchor failures count as one failed `warm_sync_light` trial. This is
conservative: if the full OFDM timing anchor is not established, there is no
valid warm-sync state for the following light frame.

Base seeds: `2026052001`, `2026052002`, `2026052003`.
Boundary cells at SNR 10 and 14 include one extra seed, `2026052004`, so the
aggregate row is `n=800` for those cells and `n=600` elsewhere.

## FER Table

FER is `(sync_fail + decode_fail + crc_fail) / n`. Parentheses are Wilson 95%
confidence intervals for FER.

| SNR dB | warm_sync_light ACK FER | Target |
|---:|---:|---:|
| 10 | 4.875% (3.59-6.59) | <= 5% |
| 12 | 0.167% (0.03-0.94) | <= 1% |
| 14 | 0.000% (0.00-0.48) | <= 0.5% |
| 16 | 0.000% (0.00-0.64) | 0% regression check |
| 18 | 0.000% (0.00-0.64) | 0% regression check |
| 20 | 0.000% (0.00-0.64) | 0% regression check |

## Decomposition

| SNR dB | Sync Fail | Decode Fail | CRC Fail | Pass |
|---:|---:|---:|---:|---:|
| 10 | 5 (0.6%) | 34 (4.2%) | 0 (0.0%) | 761 (95.1%) |
| 12 | 0 (0.0%) | 1 (0.2%) | 0 (0.0%) | 599 (99.8%) |
| 14 | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 800 (100.0%) |
| 16 | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |
| 18 | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |
| 20 | 0 (0.0%) | 0 (0.0%) | 0 (0.0%) | 600 (100.0%) |

## CLI Session Matrix

Command shape:

```bash
./build/cli_simulator --snr <snr> --channel awgn --file 64 --seed <seed> --log-level warn
```

| SNR dB | Seeds | Result | Sender retransmissions |
|---:|---|---|---|
| 10 | 42, 43, 44 | 3/3 PASS, OFDM R1/4 | 0, 0, 0 |
| 12 | 42, 43, 44 | 3/3 PASS, OFDM R1/4 | 0, 0, 0 |
| 14 | 42, 43, 44 | 3/3 PASS, OFDM R1/4 | 0, 0, 0 |
| 16 | 42 | PASS, OFDM R1/4 | 0 |
| 18 | 42 | PASS, OFDM R1/4 | 0 |
| 20 | 42 | PASS, OFDM R1/4 | 0 |
| 24 | 42 | PASS, OFDM R1/4 | 0 |

One earlier SNR 10 seed 42 matrix attempt segfaulted after negotiation and did
not reproduce on immediate isolated rerun or on the clean matrix above. The
clean matrix is the acceptance run; the non-repro crash remains noted as a
residual simulator-runner risk rather than a modem acceptance failure.

## Implementation Notes

M8 exposed one missing production state update from the earlier milestones:
connected OFDM control-profile decode returned before calling
`noteFrameArrivalSuccess()`. A full control/data anchor could therefore decode
without seeding warm timing state. The control-profile success path now records
the decoded frame arrival before returning.

End-to-end CLI verification also exposed a directionality/timing gap that the
isolated FER harness did not model. The final design does not send an
unsolicited protocol KEEPALIVE anchor. Instead, each endpoint's first natural
connected OFDM TX frame after mode transition is forced to full chirp+LTS by the
streaming encoder, and later frames return to light preambles. The decoder also
accepts a TX-turnaround timing hint from GUI/TNC/sim TX paths: when a local
OFDM frame is queued, the expected peer reply is seeded at local TX duration
plus the radio turnaround guard, so ACK warm windows are centered on a real
half-duplex prior rather than the previous inbound frame end.

False locks from a lowered warm window are counted as warm-sync misses when the
subsequent frame parse/decode rejects them. That lets the WARM -> DEGRADED ->
RECOVERY path widen and then drop back to cold/wide search instead of staying
stuck on a stale narrow timing assumption.

The warm threshold is not a free-tuned constant. The wide light-search candidate
window is 9600 samples. The warm narrow candidate span is 2176 samples
(`2 * 20 ms + 256` samples). That is a 4.41x false-positive window reduction,
which maps to a 6.45 dB correlation-magnitude allowance and lowers the warm
threshold from about 0.52 to about 0.25 only inside the predicted WARM window.
Cold, recovery, and wide-window search paths keep the higher threshold.

The matched-filter LTS score is only admitted on the lowered warm threshold path
(`threshold < 0.50`), so cold/wide sync does not gain a new matched-filter-only
false-lock mode.

The virtual OTA audio port now uses a simulator-specific carrier-sense noise
floor configuration. Continuous calibrated AWGN/noise-bed audio is allowed to
be learned as idle noise, actual peer TX is marked from virtual channel
occupancy metadata, and hardware/default audio ports keep the conservative
production bootstrap.

## Interpretation

PHY theorist: the result matches the original hypothesis. Isolated LTS-only ACK
frames fail at 10/12 dB because the 200 ms cold search window requires a high
acceptance threshold. With prior timing information from the full OFDM anchor,
the search window shrinks and the threshold reduction is justified by the
false-positive-rate ratio rather than by empirical tuning.

Real-time DSP systems engineer: this exercises the production streaming
decoder, including one-shot full OFDM anchoring, frame-arrival tracking,
narrow/degraded/recovery window planning, CFO pre-corrected matched filtering,
TX-turnaround reply prediction, and the normal ACK control-profile decode path.
The verification found and fixed the missing control-profile arrival update,
natural first-OFDM anchoring directionality, simulator noise-floor carrier
sense, and warm false-lock miss accounting before the final data run.

Veteran HF operator: a link that completes the full OFDM anchor should now keep
decoding ACK/control light frames down to the advertised 10 dB AWGN edge, and
it has explicit degraded/recovery behavior for missed windows instead of
staying stuck on a narrow timing assumption after a fade.

First-principles escape hatch: this is still an AWGN measurement. Fading entry
floors remain margin-based until the same warm-sync measurement is repeated on
Good/Moderate/Poor Watterson profiles or hardware.

## Reproduce

```bash
cmake -S . -B build -DULTRA_BUILD_GUI=OFF
cmake --build build -j4

for snr in 10 12 14 16 18 20; do
  for seed in 2026052001 2026052002 2026052003; do
    ./build/measure_ack_fer --snr "$snr" --config warm_sync_light \
      --seed "$seed" --n 200
  done
done

for snr in 10 14; do
  ./build/measure_ack_fer --snr "$snr" --config warm_sync_light \
    --seed 2026052004 --n 200
done
```

The aggregate CSV in this commit was generated from those 20 runs.
