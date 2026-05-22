# ACK RTO Airtime Findings (2026-05-22)

**REQUIRES CODEX COUNTER-CHECK BEFORE THE FIX SHIPS.** This is a Claude-found,
Claude-measured finding on the most safety-critical part of the stack (ARQ
correctness). Per standing rule, Codex must independently reproduce the spurious
timeout and verify the fix before it is treated as done.

## The finding

The ACK retransmission timeout (RTO) in
`src/protocol/selective_repeat_arq_policy.hpp::updateRTO()` is an RFC6298-style
SRTT/RTTVAR estimator, but its floor/ceiling are hard-clamped to magic constants
**independent of how long the frame actually takes on air**:

```
floor_ms   = clamp(srtt*1.5, 600, 2500)  then  max(floor, config_.ack_timeout_ms=8000)
ceiling_ms = max(12000, config_.ack_timeout_ms=8000)        // == 12000
rto_ms     = clamp(srtt + 4*rttvar, floor_ms, ceiling_ms)    // i.e. clamp to [8000, 12000]
```

So the RTO can never exceed **12 s**, regardless of modulation, code rate,
codeword count, per-burst chirp+LTS anchor cost, or half-duplex turnaround. On a
long, deeply-interleaved transfer the real round-trip (frame airtime + BRAVO
decode + BRAVO mid-burst ACK/SACK + half-duplex turnaround + ACK airtime) can
exceed 12 s, so ALPHA times out and retransmits a frame **that BRAVO already
decoded and ACKed** — pure wasted airtime.

This is **mode-independent**: it is not gated to coherent. The summary's
"gated coherent timeout fix improving DQPSK to 411/20/3/2" does NOT exist in the
committed tree — `selective_repeat_arq.cpp`, `arq_interface.hpp`, and
`connection.hpp` are byte-identical to baseline `6c11197`. The real defect is
the magic `[8000, 12000]` clamp.

## The measurement (committed binary, HEAD of feat/16qam-promotion-2026-05-21)

`./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20
--file 20480 --seed 44` (the worst-timeout cell in the committed floor map):

| Endpoint | Stat |
|---|---|
| BRAVO (RX) | `frames_decoded=92  frames_failed=16  frame_success=85.2%` |
| ALPHA (TX) | `frames_sent=71  retransmissions=41  timeouts=36  failed=0` |
| On-air goodput | 1609 bps |

- BRAVO genuinely lost **16** frames to deep Good-fading fades (real PHY loss).
- ALPHA fired **36** timeouts. Only ~16 are explained by real BRAVO failures.
- **~20 timeouts are spurious**: the frame was decoded and ACKed, but ALPHA's
  12 s RTO ceiling fired before the half-duplex ACK turnaround completed.

Cross-check, 8PSK R1/2 Good/SNR20 5KB seed42 (short transfer): `timeouts=0`,
`retransmissions=2` (`fast_hole`/`hole_probe`, not timeout), 1601 bps, 100%
frame success — i.e. the spurious-timeout pathology only appears once the
transfer is long enough for the real RTT to approach/exceed the 12 s ceiling.

## Recommended principled fix (universal)

Replace the magic `[8000, 12000]` clamp with a **physically-derived per-frame
airtime budget**, computed from quantities the engine already exposes:

```
frame_airtime_ms  = cw_count * samples_per_cw / sample_rate * 1000
                    + per_burst_anchor_ms (chirp+LTS when this frame carries one)
ack_airtime_ms    = control_frame_samples / sample_rate * 1000   (getOFDMControlFrameSamples pattern)
turnaround_ms     = half-duplex T/R + carrier-sense settle budget (already modeled elsewhere)
rto_floor_ms      = frame_airtime_ms + turnaround_ms + ack_airtime_ms + margin
rto_ceiling_ms    = k * rto_floor_ms   (k ~ 2-3, bounded; NOT a flat 12000)
```

Then keep the RFC6298 SRTT/RTTVAR estimator on top, but bound it by the
*airtime-derived* floor/ceiling instead of magic constants. Plumb the per-frame
airtime estimate into `SelectiveRepeatARQ` whenever mode/rate/cw-count changes
(the connection/mode layer already knows modulation, code rate, cw_count,
symbol rate via the waveform's `getSamplesPerSymbol()` /
`getOFDMControlFrameSamples()` — reuse those, do NOT hardcode a new table).

Apply to **every** mode (DQPSK / 8PSK / QAM16 / MC-DPSK / OFDM_NARROW). The user
explicitly authorized generalizing the improvement to all modes, including
letting the DQPSK guard improve (391/20/4/4 -> better is acceptable, as long as
it does not regress).

## Guardrails

- DQPSK guard `good/snr12/1KB/seed42`: must stay 391/20/4/4 **or improve**
  (improving to e.g. 411/20/3/2 is acceptable and expected; regression is not).
- QAM16 + AWGN: no goodput regression on the committed cells.
- `ctest --test-dir build` must stay green (currently 92/92).
- No PHY/demap/equalizer change — this is a transport-timing fix only.
- Branch-only, never push.

## Reproducers

```bash
# spurious-timeout cell (before/after):
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel good --snr 20 --file 20480 --seed 44
./build/cli_simulator --expert --mod 8psk  --rate r1_2 --channel good --snr 20 --file 20480 --seed 42
# DQPSK guard:
./build/cli_simulator --channel good --snr 12 --file 1024 --seed 42
# AWGN sanity:
./build/cli_simulator --expert --mod qam16 --rate r1_2 --channel awgn --snr 16 --file 20480 --seed 42
```
