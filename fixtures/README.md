# Decoder bench fixtures

Deterministic, listenable WAV files used by `decode_bench` so AI agents
and humans can A/B decoder changes against bit-identical input.

Replay-gate fixtures are either 32-bit float mono at 48 kHz or 16-bit PCM
mono at 24 kHz. `decode_bench` resamples WAV input to 48 kHz before decode.
Open them in Audacity / VLC / `sox`.

## Generating

The historical R1/4 fixtures are reproducible from seed=1:

```bash
./build/decode_bench --mode gen \
  --wav fixtures/ofdm_chirp_r14_dqpsk_clean.wav \
  --rate r1_4 --mod dqpsk --waveform ofdm_chirp \
  --snr 100 --frames 4 --payload 60 --seed 1
```

Small replay-gate fixtures use one fixed 4-CW DATA frame, deterministic
channel seeds, and PCM16/24 kHz output to keep each file under 200 KB:

```bash
./build/decode_bench --mode gen \
  --wav fixtures/ofdm_chirp_r12_dqpsk_snr15_awgn.wav \
  --rate r1_2 --mod dqpsk --waveform ofdm_chirp \
  --channel awgn --snr 15 --frames 1 --payload 60 --seed 12 \
  --wav-format pcm16 --sample-rate 24000
```

## Benching

```bash
./build/decode_bench --mode bench \
  --connected \
  --wav fixtures/ofdm_chirp_r14_dqpsk_clean.wav \
  --rate r1_4 --mod dqpsk --waveform ofdm_chirp --cw-count 4
```

Output prints `frames_decoded` / `frames_failed` and the full
`DecoderProfile` breakdown (LTS sync, OFDM process, data symbol loop,
fixed-frame decode, LDPC inner, single-CW paths, HARQ key build, etc.).

Current status (2026-05-08): these bench fixtures decode cleanly when run
with `--connected`, which is the intended mode for post-CONNECT fixed DATA
frames. Running without `--connected` starts the decoder in disconnected
control-search mode and produces `0` decoded frames; that is an invocation
mismatch, not a stale fixture.

The maintained CTest gate is `DecodeBenchReplay`; it asserts
`frames_failed=0`, byte-exact DATA decode, and the expected frame count for
every replay fixture below.

## Available fixtures

| File | Rate | Channel | SNR | Frames | Expected result |
|------|------|---------|-----|-------:|-----------------|
| `ofdm_chirp_r14_dqpsk_clean.wav` | R1/4 | clean | ∞ | 4 | 4 byte-exact DATA frames |
| `ofdm_chirp_r14_dqpsk_snr15_awgn.wav` | R1/4 | AWGN | 15 dB | 4 | 4 byte-exact DATA frames |
| `ofdm_chirp_r12_dqpsk_snr15_awgn.wav` | R1/2 | AWGN | 15 dB | 1 | 1 byte-exact DATA frame |
| `ofdm_chirp_r34_dqpsk_snr15_awgn.wav` | R3/4 | AWGN | 15 dB | 1 | 1 byte-exact DATA frame |
| `ofdm_chirp_r14_dqpsk_snr15_good.wav` | R1/4 | Good fading | 15 dB | 1 | 1 byte-exact DATA frame |
| `ofdm_chirp_r12_dqpsk_snr18_good.wav` | R1/2 | Good fading | 15 dB | 1 | 1 byte-exact DATA frame |
| `ota_test_r14_15s.wav` | R1/4 | clean | ∞ | 21 | OTA/manual fixture with readable payload `"PROJECTULTRA OTA TEST 2026 R1/4 DQPSK 73 ..."` |

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
   ./build/decode_bench --mode bench --connected --wav <recording>.wav --rate r1_4
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
- **Post-handshake decode.** `cli_simulator` runs the full PING /
  CONNECT / DATA / DISCONNECT path. The bench skips all of that and
  starts in connected DATA mode, exercising the data-frame decode hot path —
  the surface most worth optimizing for throughput.
