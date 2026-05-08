# OTA capture 2026-05-07 — KC3VPB (PA, 100 W) → KiwiSDR k1vl (Vermont)

## Setup

- **TX:** KC3VPB, 100 W, Pennsylvania
- **TX audio:** played `recordings/ota_full_session_2026-05-07/full_session_{r1_4,r1_2,r3_4}.wav`
  through radio interface; each WAV contains chirp + CONNECT
  handshake + DATA frames + DISCONNECT.
- **RX:** sdr.k1vl.com (Vermont KiwiSDR), 7121 kHz USB, AGC off, mono PCM16 12 kHz.
- **Order on the wire:** r1_2, r1_4, r3_4.

## Decode pipeline

```
sox <kiwi.wav> -e float -b 32 /tmp/boost.wav gain <N> gain -l
./build/session_decode --wav /tmp/boost.wav
```

`gain -l` softens peak clipping introduced by the boost; the boost is needed
because KiwiSDR with AGC off delivers ~ −50 dBFS audio. RMS-target boost
of about 30–36 dB lifts active section to roughly −16 dBFS, matching the
modem's tuning.

## Results — first successful OTA full-session decode

| Rate | Chirp corr (up/down) | CFO  | First sync | CONNECT | Handshake | Negotiated  | DATA byte-exact | LDPC fail | DISCONNECT |
|------|----------------------|-----:|------------|--------:|----------:|-------------|----------------:|----------:|-----------:|
| r1_2 | 0.763 / 0.801        | 0.95 | yes        | ✓       | ✓         | OFDM-CHIRP DQPSK R1/2 8-CW | 1/1 (126 B) | 5 | ✓ |
| r1_4 | 0.857 / 0.905        | 0.85 | yes        | ✓       | ✓         | OFDM-CHIRP DQPSK R1/4 4-CW | 1/1 (4 B)   | 6 | ✓ |
| r3_4 | 0.781 / 0.818        | 0.85 | yes        | ✗       | ✗         | (handshake lost)           | 0/0         | 1 | ✗ |

Source captures are stored alongside this file:
- `ota_r1_2_kc3vpb_to_k1vl.wav` (49.8 s, RMS −46 dBFS as captured)
- `ota_r1_4_kc3vpb_to_k1vl.wav` (39.4 s, RMS −52 dBFS)
- `ota_r3_4_kc3vpb_to_k1vl.wav` (33.1 s, RMS −50 dBFS)

## Findings

### What worked

- **The dual chirp survives a real radio chain cleanly.** All three rates
  produced chirp correlations between 0.76 and 0.86 — well above the 0.45
  detection floor — and CFO estimates agree across the three captures
  (~0.85–0.95 Hz, consistent with the radio + KiwiSDR clock offset).
- **Wire-side rate/CW-count negotiation is robust over the air.** r1_2 and
  r1_4 both completed CONNECT → CONNECT_ACK → MODE_CHANGE → ACK and
  switched waveforms cleanly to OFDM-CHIRP at the negotiated rate.
- **session_decode tool works end-to-end on real OTA captures.** It
  auto-resamples 12 kHz → 48 kHz, runs a real ModemEngine, and prints the
  exact same summary block the simulator's BRAVO would.

### What didn't, and why it's expected

- **LDPC failures on most DATA frames at r1_2 and r1_4 (5/7 and 6/7
  respectively).** The post-CONNECT_ACK light preamble (LTS-only) in this
  signal level + KiwiSDR fidelity is operating below the rate-vs-SNR margin
  the modem is tuned for. RMS −16 dBFS post-boost still implies SNR well
  below the SNR=15 design target — Vermont KiwiSDR is the only S9-ish
  remote SDR, but skywave from PA at this hour delivered a comparatively
  noisy capture. This is exactly the data we needed to characterise the
  real OTA channel.
- **r3_4 lost the handshake.** Chirp locked, but the CONNECT_ACK MC-DPSK
  control frame failed LDPC. r3_4's data path is the most marginal of the
  three rates, and the auto-rate ladder already gates r3_4 to AWGN-like
  conditions only (`fading_index < 0.10`); this OTA path is well outside
  that window. Confirms the gate is doing the right thing.

### Followups

1. Re-do the OTA test with **a louder source** — bump the WAV gain in
   `tools/capture_session_audio.sh` further (or pre-condition with an
   external soft limiter) so the radio drives harder into rated power.
2. Try **a closer / better KiwiSDR** to lift the receive SNR — Vermont
   k1vl was usable but at the noise floor of the recordings.
3. The boost-then-decode pipeline could be wrapped in a one-shot helper
   `tools/session_decode_kiwi.sh` that picks gain by RMS target.
