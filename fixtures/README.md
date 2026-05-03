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
| `ota_test_r14_15s.wav` | R1/4 | DQPSK | ∞ | 21-frame, 15.16 s OTA fixture with readable payload `"PROJECTULTRA OTA TEST 2026 R1/4 DQPSK 73 ..."` — play through a real radio, record on the receive side, decode locally |

Both decode 4/4 frames cleanly today. R1/2, R2/3 fixtures TBD — they
need a per-rate pilot config nuance the bench doesn't yet replicate
(captured as a follow-up; not a fixture file issue).

## OTA test workflow (no peer needed)

`ota_test_r14_15s.wav` is a self-contained, pre-encoded data burst.
A friend with an HF radio can play this WAV through their soundcard
into the radio's audio input — no ProjectUltra installed, no peer
needed, no ARQ.

1. Friend plays the WAV (`aplay`, `afplay`, Audacity, anything).
2. The radio transmits the audio; signal goes over the air.
3. Receive side records the on-air signal (own radio, WebSDR,
   another friend's RX, etc.) into a WAV.
4. Decode the recording locally:
   ```bash
   ./build/decode_bench --mode bench --wav <recording>.wav --rate r1_4
   ```
5. Output prints frames decoded / failed and the first decoded payload
   verbatim. If you see `"PROJECTULTRA OTA TEST 2026 R1/4 DQPSK 73 ..."`
   the round trip worked.

The decoder cares only about audio samples, not how they got there —
real RF chain quirks (TCXO drift, AGC pumping, bandpass filter shape,
real impulsive noise) all get exercised "for free" through this
playback flow.

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
