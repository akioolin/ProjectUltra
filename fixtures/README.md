# OTA test fixture

`ota_test_r14_15s.wav` is a self-contained, pre-encoded OFDM_CHIRP R1/4 DQPSK
data burst with a readable payload (`"PROJECTULTRA OTA TEST 2026 R1/4 DQPSK 73 ..."`).
It is a listenable, bit-identical reference for manual / over-the-air round-trip
checks — open it in Audacity / VLC / `sox`.

> The `decode_bench` headless replay tool and its `ofdm_chirp_*dqpsk*.wav`
> replay fixtures were retired 2026-05-30 — all decoder testing is now done on the
> live GUI path (`tools/gui_qso_scenario.sh`, two real `ultra_gui -sim` stations
> over `ota_simulator serve`). This OTA listening fixture survives.

## OTA test workflow (no peer, no ProjectUltra install needed)

A friend with an HF radio can play this WAV through their soundcard into the
radio's audio input — no peer, no ARQ.

1. Friend plays the WAV (`aplay`, `afplay`, Audacity, anything).
2. The radio transmits the audio; signal goes over the air.
3. Receive side records the on-air signal (own radio, WebSDR, another friend's
   RX, etc.) into a WAV / `.f32`.
4. Decode the recording locally — run `ultra_gui` and play the recording into
   its audio input, or use the raw CLI: `./ultra prx <recording>.f32`.
5. If the decoded payload reads `"PROJECTULTRA OTA TEST 2026 R1/4 DQPSK 73 ..."`
   the round trip worked.

The decoder cares only about audio samples, not how they got there — real RF
chain quirks (TCXO drift, AGC pumping, bandpass shape, impulsive noise) all get
exercised "for free" through this playback flow.
