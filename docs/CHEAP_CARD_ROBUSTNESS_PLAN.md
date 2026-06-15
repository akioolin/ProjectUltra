# Cheap-Card Robustness Plan — MC-DPSK handshake (VARA-parity)

**Status:** PLAN (2026-06-15). Born from the live Mac↔IONOS↔Pi5 bringup
(see `KNOWN_BUGS.md::BUG-IONOS-PI5-CHEAP-DAC`). Operate under the four-tier stack
(PHY theorist / DSP systems / veteran HF op / first-principles).

## The problem, precisely

A cheap USB dongle's TRANSMIT path has measured impairments (A/B tone test, same
IONOS, only the TX card swapped):

| impairment | cheap card TX | SoundBlaster TX |
|---|---|---|
| band tilt 0.5–2.6 kHz | **14.8 dB** | 5.7 dB |
| 2nd-harmonic distortion | **−17.8 dB** | −36.7 dB |
| carrier freq jitter | **±7 Hz** | ±0.5 Hz |

The MC-DPSK 4-CW DQPSK R1/4 CONNECT/CONNECT_ACK decodes to garbage at the *receiver*
even at a clean, matched, non-clipping level (Mac RX 0.16–0.20, = the Pi5's working
0.17). The wideband chirp survives (it's a matched-filter sweep); the differential
payload doesn't. **The user runs this exact dongle successfully with a commercial HF
modem → the card is usable by a robust-enough handshake; ours isn't (yet).** This is
OUR robustness gap, not (only) a hardware limit.

Why each impairment hurts the differential payload:
- **±7 Hz jitter** → ±53.8°/symbol differential rotation > DQPSK's ±45° decision
  margin → systematic symbol errors. THE dominant killer.
- **14.8 dB tilt** → edge carriers (≈2.5 kHz) starved → their LLRs are weak but, with
  one global LLR scale, still poison the LDPC.
- **−17.8 dB distortion** → in-band intermodulation products land on other carriers
  (structured interference). Hardest to fix at the RX; mitigated by reducing TX drive
  and by robust modulation, not by inversion.

## Levers (prioritized; principled, not band-aids)

1. **DBPSK control frames** (PRIMARY). Send PING/CONNECT/CONNECT_ACK/MODE_CHANGE as
   **DBPSK** (1 bit/symbol) instead of DQPSK (2). Doubles the phase margin (±45° →
   ±90°) → tolerates the ±53.8°/symbol jitter outright. Cost: 2× control airtime
   (irrelevant for a handshake). This is the single biggest jitter win and the most
   likely thing that gets us to VARA parity on this card. Protocol: both ends must
   agree control = DBPSK (it's a fixed convention, not negotiated — control is always
   the most-hardened profile). KEEP the DQPSK enum for MC-DPSK *data*.
   - Risk: a hard-coded control-modulation constant on a shared path violates the
     CLAUDE.md adaptivity rule unless it's genuinely geometry-derived. DBPSK-control is
     a documented robustness convention (like R1/4-control), acceptable; verify it
     doesn't regress the existing AWGN/Good handshake.

2. **Per-carrier SNR/reliability LLR weighting** (handles tilt + selective fade).
   `multi_carrier_dpsk.hpp::demodulateSoft` currently scales all carriers' soft bits by
   ONE global `scale` from aggregate phase-noise variance. Replace with a **per-carrier
   scale** = 2·√(1/σ²_c) (σ²_c from each carrier's decision-residual variance, capped),
   optionally also weighted by carrier magnitude (per-carrier SNR with a documented
   reference). A starved/notched carrier then contributes a low-confidence LLR instead
   of an overconfident wrong one. Generic — helps fading broadly, not just cheap cards.
   - No-regression guard: on flat AWGN all σ²_c ≈ equal → per-carrier ≈ global; verify
     StreamingMCDPSK + the floor unchanged.

3. **Carrier-jitter tracker reach** (already landed for SLOW jitter; commit e66143e).
   The decision-free M-th-power common-phase tracker recovers slow jitter and safely
   no-ops on fast. If the real DAC jitter is faster than it tracks, the DBPSK margin
   (lever 1) is the backstop. Don't over-tune the tracker into chasing fading.

4. **TX-side drive discipline** (cheap-card-independent good practice). A real PA / a
   cheap DAC must not be overdriven; the per-burst hardware peak normalization
   (`normalizeTxBurstForHardware`, tx_drive) already targets a safe peak — but the cheap
   card's analog output saturates regardless of digital level (observed: tx_drive
   0.10/0.20/0.70 barely moved the IONOS Lvl on the Pi5, while it worked on the
   SoundBlaster). That saturation is a hardware trait; the lever here is the soundcard
   analog output gain (ALSA `Speaker`), not tx_drive. Document the gain-staging.

NOT a lever: blind RX nonlinear *inversion* of the harmonic distortion. In-band IMD
inversion is research-grade, noise-amplifying, fails on hard clipping, and can't be
made generic per-card. Tolerate (robust mod + per-carrier weighting) instead of invert.

## Proof gate (sim-first, before any HW claim)

Extend `tests/test_mcdpsk_clock_offset.cpp` (it already injects clock/tilt/jitter through
the real encode→AWGN→decode path) into a cheap-card fixture: inject the *measured* cheap
profile (14.8 dB tilt + −17.8 dB 2nd-harmonic + ±7 Hz jitter at a realistic rate) on a
CONNECT/CONNECT_ACK-geometry frame and assert it DECODES with the robustness levers on,
FAILS (or is marginal) with them off, and that nothing regresses on clean AWGN/Good.
Then re-test on the IONOS with the SGTL5000 HAT for the clean baseline and (optionally)
the cheap dongle for the parity check.

## Sequencing

1. Lever 2 (per-carrier LLR weighting) — smallest, self-contained, broadly useful;
   land + prove no-regression first.
2. Lever 1 (DBPSK control) — the big jitter win; needs a protocol convention + both-ends
   build; prove on the cheap-card fixture.
3. Re-measure the cheap dongle driven from an UNLOADED host (separate DAC-intrinsic
   defects from Pi-side USB starvation) to set the real jitter target for lever 3.

## Pragmatic now

For an immediate working hardware QSO: swap the Pi5 to the SGTL5000 I2S HAT (clean,
Pi-clocked codec) — the modem is proven correct in sim, so a decent card just works.
This plan is the path to *also* working on the cheap dongle, matching the commercial
modem the user already runs on it.
