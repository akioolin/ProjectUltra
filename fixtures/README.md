# Decoder bench fixtures

Deterministic, listenable WAV files used by `decode_bench` so AI agents
and humans can A/B decoder changes against bit-identical input.

Format: 32-bit float mono, 48 kHz. Open in Audacity / VLC / `sox`.

## Generating

Each fixture is reproducible from its name + seed=1:

```bash
./build/decode_bench --mode gen \
  --wav fixtures/ofdm_chirp_r14_dqpsk_clean.wav \
  --rate r1_4 --mod dqpsk --waveform ofdm_chirp \
  --snr 100 --frames 4 --payload 60 --seed 1
```

## Benching

```bash
./build/decode_bench --mode bench \
  --wav fixtures/ofdm_chirp_r14_dqpsk_clean.wav \
  --rate r1_4 --mod dqpsk --waveform ofdm_chirp
```

Output prints `frames_decoded` / `frames_failed` and the full
`DecoderProfile` breakdown (LTS sync, OFDM process, data symbol loop,
fixed-frame decode, LDPC inner, single-CW paths, HARQ key build, etc.).

## Available fixtures

| File | Rate | Mod | SNR | Description |
|------|------|-----|-----|-------------|
| `ofdm_chirp_r14_dqpsk_clean.wav` | R1/4 | DQPSK | ∞ | Noiseless baseline; LDPC should converge in 0 iters |
| `ofdm_chirp_r14_dqpsk_snr15_awgn.wav` | R1/4 | DQPSK | 15 dB | AWGN at typical operating SNR |

Both decode 4/4 frames cleanly today. R1/2, R2/3 fixtures TBD — they
need a per-rate pilot config nuance the bench doesn't yet replicate
(captured as a follow-up; not a fixture file issue).

## Why fixtures?

- **Bit-identical comparison.** Without committed audio, encoder
  changes shift the bench input from run to run, masking decoder
  deltas.
- **Listenable.** 32-bit float WAV opens in any audio editor, so a
  human can sanity-check the test material against expected
  spectral content.
- **No-handshake decode.** `cli_simulator` runs the full PING /
  CONNECT / DATA / DISCONNECT path. The bench skips all of that and
  exercises only the data-frame decode hot path — the surface most
  worth optimizing for throughput.
