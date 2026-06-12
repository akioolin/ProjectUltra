# 00 — Executive summary: the road from ~1900 to 3086 bps on Good@20

**Audit:** Claude Fable 5, 2026-06-12. Question asked: *"we have a ~2000 bps ceiling at
QPSK + LDPC R3/4 in 3 kHz; the market leader does ~3086 bps on Good fading at 20 dB —
what was missed?"* Method: 11-agent parallel code survey → 8-agent adversarial
verification of every load-bearing claim → live GUI-gate experiments. Everything is
file:line-cited in docs 01-08; nothing below rests on project docs alone (several were
stale).

## The headline (changes the whole campaign)

**The "16QAM is structurally undecodable on Good@20" verdict — the documented single
gate to 3086 — is STALE. Forced 16QAM R1/2 Good@20 seed 42 now PASSES on the current
stack: 21.5 KB CRC-clean, 1860 bps, ZERO codeword failures** (on 2026-05-29 the same
cell was FAIL / 256 CW fails / 0 delivery). 8PSK R3/4 likewise improved (710 → 1740
bps). The wall fell silently in the ~10 days of fixes after the diagnosis — prime
suspect: the 06-08 phantom-CFO confidence gate (`be0bbce`), whose mechanism
(fade-manufactured chirp CFO → per-symbol phase smear) matches the May-29 verdict
exactly — **and nobody re-measured 16QAM afterward.** The gate to 3000+ has been open
for days. (07 has the runs; 06 Phase 0 says bisect it for the record.)

## Why the ceiling is exactly where it is (01, 04)

- Your measured best (1910 bps) is **94-96% of the QPSK R3/4 protocol ceiling**
  (~2030 bps zero-retx from code constants). Decode quality is NOT the limiter; the
  cycle structure is: 1.41 s full-chirp BURST_HEADER every burst (15.7%), 875 ms
  tone-ACK key-down (9.8%), LTS/lead-ins/latencies (~8%), 38% total overhead.
- **QPSK can never reach 3086** in 2.8 kHz: best rung raw is 3279-3357 bps → would
  need >92% airtime efficiency — physically excluded by half-duplex turnaround.
  Denser modulation is mandatory, and now demonstrably available.
- Our benchmark comparison is valid and slightly conservative: the OTASim Good channel
  is exact ITU-R F.1487, and our SNR convention runs **0.61 dB harder** than the
  leader's 3 kHz convention; the 21 KB gate also understates steady state ~15-20%.

## What was actually missed (ranked)

1. **Nobody re-ran 16QAM/8PSK after the June fixes** — the campaign's central blocker
   dissolved without anyone noticing. Standing lesson: re-anchor measured walls after
   every intervening fix (02's lesson box).
2. **The ladder cannot express modulation at all**: RateController is CodeRate-only,
   `kCoherentLadder` has QAM16 disabled and no 8PSK rung, every adaptive MODE_CHANGE
   passes modulation unchanged. The wire format already carries modulation and the RX
   handler applies it unrestricted — this is sender policy, ~1-2 weeks (05, 06 Ph. 1).
3. **Airtime structure** caps even a perfect 16QAM link: both big levers (short-anchor
   descriptor, fast tone-ACK) are builds, not knob-flips — adversarial verification
   found the "easy" versions are a documented dead end (header-once) and a silent
   ACK-loss trap + PHY design flaw (12 ms ACK), respectively (04).
4. **LLR noise model lacks the channel-estimation-error term** (ε²_H) — structurally
   confirmed, the corrective per-carrier quantity is computed and discarded in-tree —
   but the live A/B of the cheap single-symbol form measured net-NEGATIVE on the new
   stack, so this is demoted to a margins refinement in its pilot-anchored form (02).
5. **Paper-cut bugs**: R5/6 pilot fall-through (+8.5% on the top rung, one line);
   phantom re-anchor pinning groups at 5 instead of 6 (+~105 bps); WIP sticky-ceiling
   never reset; EMP_FLOOR knob segfault; stale numerology in three layers (08).

## State of the rungs on Good@20 (07 has full table; Phase 0a = 5-seed, 2026-06-12)

| Rung | Raw | Measured (seeds) | Character |
|---|---:|---|---|
| QPSK R3/4 (anchor) | 3 279 | 1630-1910 (Fable) | clean, AT its protocol ceiling |
| **16QAM R1/2** | 4 029 | **5/5 PASS, 1550 med / 1400-1890** | **clean — 0 CW-fails on 4/5** |
| 8PSK R3/4 | 4 918 | **5/5 PASS, 1440 med / 840-1620** | reliable but throughput-poor (24-72 retx) |
| 16QAM R2/3 (3 seeds) | 5 371 | 740-1200 | ~55-70% damage |
| 16QAM R3/4 (3 seeds) | 6 557 | 0-1330 | heavy damage, 1/3 link-death |
| *16QAM R1/2 @ Good@18* | 4 029 | *5/5 PASS, 1250 med / 990-1550* | *holds 2 dB lower, fade-erasure only* |

(Phase-0a absolutes are load-depressed ~15-25% — see 07's load caveat; pass-rate and
relative ordering are the trustworthy outputs.)

Pattern (measured, multi-seed): a **reliability cliff between 50% and 33% FEC
redundancy** for dense modulations against Good's ~23% instantaneous null erasure.
Every clean rung is protocol-capped (~2030); every rung with raw headroom is
damage-bound. **New from Phase 0a: 16QAM R1/2 ≥ 8PSK R3/4 on both goodput AND
reliability despite 8PSK's higher raw rate — 8PSK is a confirmed throughput dead end,
not a shortcut.** (Open anomaly: R2/3 measured *worse* than R3/4 same-seeds despite
more parity — salient difference is pilot spacing 5 vs 8; Phase-0b A/B scoped in 07.)

## The path (06 has the full plan with gates)

- **Phase 0 (days):** finish the multi-seed rung re-anchor; bisect the 16QAM fix;
  isolate the R2/3-worse-than-R3/4 anomaly (sp5/sp8 A/B); land the one-line fixes.
- **Phase 1 (1-2 wks):** (mod, rate) rungs in the ladder + promotion signal
  (CIR delay-spread + per-carrier EVM histogram); harness watchdog fix first.
- **Phase 2a (parallel):** airtime levers — short-anchor descriptor, 13.33 ms
  tone-ACK (both ends), frame_mask/window widen, 100 KB benchmark protocol.
- **Phase 2b (parallel, NOW LOAD-BEARING):** the dense-rung margins work — unified
  per-carrier reliability model (pilot-anchored ε²_H term, relative-null CSI for
  16QAM, MMSE-bias slicer fix, LLR-scale/clip rederivation), with iterative
  data-aided re-estimation as the follow-up if needed and the data-aided genie as the
  oracle. The live sweeps proved BOTH workstreams are required (07).

**Arithmetic (measurement-corrected):** clean rungs + levers alone top out ~2 570-2 780
(16QAM R1/2); a clean dense rung without levers tops out ~2 800-3 270 with no damage
margin. **Together** — one dense rung (16QAM R2/3 or R3/4, or 8PSK R3/4) brought to
≤10% damage, plus the levers — the ceiling is ~3 430-4 170, comfortably above 3 086
within the channel's 3 764 genie envelope. Feasible with existing machinery; the one
open engineering risk is the dense-rung damage reduction (Phase 2b), which is now a
precisely-characterized problem (frame-level losses at 25-33% parity under ~23% null
erasure + null-region demap poisoning), not a mystery wall.

## Caveats the next worker must carry

- All of this session's rung results are **seed-42 single-seed** until the 06 Phase-0
  sweep lands. The bar is 5/5 seeds.
- OTASim runs zero CFO / zero clock-ppm (bench convention) — spot-check the winning
  config under real impairments before declaring parity; 16QAM is the most exposed.
- The 8.6 s key-down cap is the only PA-duty guard in code (TX duty already ~66% in
  these runs); don't trade past it for throughput — that's a hardware-impossible cheat.
