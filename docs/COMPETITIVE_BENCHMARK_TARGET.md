# Competitive Benchmark — Long-Term Beat-or-Exceed Target

Captured 2026-05-19. This is the industry-leader HF modem at 3 kHz BW
that ProjectUltra aims to match and exceed on goodput.

These numbers anchor our throughput goals across all channel conditions
and SNRs. Beating these is the long-term mission per
`docs/PROJECT_GOALS.md`.

## Reference table (industry leader, 3 kHz BW)

Channel codes per ITU/CCIR convention:
- **WGN** — White Gaussian Noise (AWGN, no multipath)
- **MPG** — Mid-Path Good fading (modest delay spread / Doppler)
- **MPM** — Mid-Path Moderate fading
- **MPP** — Mid-Path Poor fading (heavy delay spread / Doppler)

Throughput is in **bytes/min** as published. Conversion to bps shown
in parentheses for direct comparison with our raw-PHY numbers.

### WGN (AWGN)

| S/N (dB) | Bytes/min | Approx. bps |
|---:|---:|---:|
| 40 | 46,809 | ~6,240 |
| 30 | 44,964 | ~5,995 |
| 20 | 38,425 | ~5,123 |
| 10 | 18,232 | ~2,431 |
| 0 | 3,008 | ~401 |
| -10 | 1,070 | ~143 |

### MPG (Mid-Path Good)

| S/N (dB) | Bytes/min | Approx. bps |
|---:|---:|---:|
| 40 | 28,120 | ~3,749 |
| 30 | 27,385 | ~3,651 |
| 20 | 23,142 | ~3,086 |
| 10 | 5,973 | ~796 |
| 0 | 1,752 | ~234 |
| -10 | 324 | ~43 |

### MPM (Mid-Path Moderate)

| S/N (dB) | Bytes/min | Approx. bps |
|---:|---:|---:|
| 40 | 27,208 | ~3,628 |
| 30 | 26,617 | ~3,549 |
| 20 | 23,711 | ~3,161 |
| 10 | 7,447 | ~993 |
| 0 | 1,700 | ~227 |
| -10 | 383 | ~51 |

### MPP (Mid-Path Poor)

| S/N (dB) | Bytes/min | Approx. bps |
|---:|---:|---:|
| 40 | 17,519 | ~2,336 |
| 30 | 16,603 | ~2,214 |
| 20 | 16,592 | ~2,212 |
| 10 | 7,703 | ~1,027 |
| 0 | 1,740 | ~232 |
| -10 | 439 | ~59 |

## Where we are (post-2026-05-19 audit + half-duplex fix)

Raw-PHY ceilings (no ARQ overhead, no retx):

| Mode | Raw bps |
|---|---|
| MC-DPSK R1/4 | 375 |
| OFDM_CHIRP R1/4 | 1,104 |
| OFDM_CHIRP R1/2 | 2,208 |
| OFDM_CHIRP R2/3 | 2,944 |
| OFDM_CHIRP R3/4 | 3,438 |
| OFDM_CHIRP D8PSK R3/4 | 5,157 (forced only, near-ideal AWGN) |

Verified floors on honest half-duplex OTASim (1-seed unless noted):

| Mode | Channel | Floor (in-band dB) | Effective bps at floor |
|---|---|---|---|
| MC-DPSK R1/4 | AWGN | 5 | ~375 (clean, 0 retx) |
| OFDM R1/4 | AWGN | 12 (PHY edge) | ~109 (90 retx storm) |
| OFDM R1/2 | AWGN | 14 (operational) | ~2,200 (0 retx) |
| OFDM R1/4 | Good | 15 (PHY edge) | ~273 (34 retx storm) |
| ... | ... | ... | ... |

## The gap

**At 10 dB AWGN, the industry leader does ~2,431 bps. Our OFDM R1/4 at
12 dB AWGN can barely sustain ~109 bps under load.** That's a 22×
gap on the operational throughput.

**At 0 dB AWGN, the industry leader does ~401 bps. We don't operate
below ~5 dB in-band yet.**

**At -10 dB AWGN, the industry leader does ~143 bps. We have no
sub-zero-SNR mode at all.**

## Long-term workstreams that close the gap

1. **Sub-zero-SNR mode** — currently no production mode operates below
   ~5 dB in-band. Sub-zero requires either deeper FEC (LDPC R1/8 or
   below), narrower bandwidth (OFDM_NARROW or sub-band channels), or
   spread-spectrum techniques.

2. **ARQ throughput optimization** — at the operational R1/4 floor,
   ARQ retx storms destroy effective throughput. The industry leader
   delivers 2.4 kbps at 10 dB; our raw R1/4 there is 1.1 kbps minus
   storms = ~0.1 kbps. There's 10× of throughput on the table just
   from ARQ tuning at the floor.

3. **Fading-aware coding** — the industry leader holds 1 kbps even at
   MPP 10 dB. Our R1/4 needs fading-specific receivers (better CPE
   tracking, decision-directed equalization, time interleaving).

4. **Higher rates / modulations** — R3/4 + D8PSK currently only viable
   in near-ideal AWGN. The industry leader sustains >3 kbps in
   MPG/MPM at 20 dB; we'd need stable D8PSK on fading.

5. **Air-time efficiency** — preamble overhead, frame fragmentation,
   ACK turnaround all eat real bytes/min. Optimizing these can buy
   30-50% throughput without PHY changes.

## Notes

- Industry leader operates with 3 kHz BW (same as us).
- These numbers assume well-tuned auto-rate selection; the leader's
  ladder is well-developed across the full SNR range.
- Bytes/min implicitly includes ARQ overhead and protocol header
  overhead. So matching these means matching delivered bytes, not
  raw PHY bits.
