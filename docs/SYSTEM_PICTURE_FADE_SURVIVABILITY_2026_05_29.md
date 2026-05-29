# System Picture: Fade Survivability → Throughput (2026-05-29)

The global view the operator asked for: every receiver subsystem, what job it
*should* do for the mission (reliable HF data → 3000 bps on Good@20), and
today's measured verdict on whether it does. Read this with
`docs/ADAPTIVITY_AUDIT_2026_05_29.md` (subsystem-by-subsystem register) and
`docs/PHY_ADAPTATION_DESIGN_2026_05_26.md` (the design plan). The theme: each
subsystem must be **channel-adaptive by design**, and several were baked for one
channel and misapplied — the same disease in DD, the Wiener estimator, and
(likely) the interleaver axis.

## 1. The physics that frames everything

"Good HF" here = **0.5 ms delay spread, 0.1 Hz Doppler**. Two clocks follow:

- **Coherence time ≈ 4 s** (from Doppler). A whole 6-frame burst (~0.9 s) is
  *frozen* — the channel does not move during the transfer.
- **Coherence bandwidth ≈ 600 Hz** (from delay spread), vs ~94 Hz subcarrier
  spacing → a **frequency-selective null sits in the band** and stays put.

**Consequence that drives the whole design:** on Good HF the impairment is
*frequency-selective, not time-selective*. The receiver does **not** need to
*track* a moving channel; it needs to **survive a static notch**. This single
fact decides which subsystems matter (frequency diversity, erasure-aware FEC)
and which are misapplied (time-tracking DD, time-axis interleaving).

We are nowhere near Shannon (≈18 kbps at SNR 20 in 2.8 kHz). The link is
**overhead- and rung-limited, not capacity-limited.**

## 2. The receive chain — each subsystem's job and today's verdict

| Subsystem | Job for the mission | Verdict (2026-05-29) |
|---|---|---|
| **Sync / LTS** (chirp + light LTS) | Acquire timing/CFO | Works; warm-handoff shortens per-group airtime (env-gated). |
| **Channel estimation — pilot Wiener MMSE** | Estimate H(f) across the band from pilots | Math correct (MMSE, Jakes, sinc). **Non-adaptive** (Moderate-baked 1 ms/0.5 Hz). **Re-tuning is FLAT** (measured) — *not the survivability lever*. Can't interpolate energy that isn't in a null. |
| **Decision-directed tracking (DD)** | Refine H between pilots / track time-variation | **Wrong tool on Good** (frozen channel; its wrong decisions in the null poison H → cascade). **FIXED today**: channel-adaptive gate (`last_fading_index < 0.15`) → off on faded frames, on for AWGN/flat. Removes the self-inflicted cascade. |
| **Equalization — MMSE one-tap** | `conj(H)·Y/(|H|²+σ²)` | Standard, correct, channel-independent. Fine. |
| **Soft demap + LLR** | Per-carrier bit LLRs | Correct; mod-aware. |
| **Anti-poison — relativeFadeNoiseInflation** | Turn null carriers into honest *erasures* (don't feed confident-wrong LLRs to LDPC) | Right idea, references frame-mean |H|²; the correct partner to FEC for nulls. Audit its adaptivity next. |
| **Diversity — interleavers** (frame / burst / carrier) | Spread each codeword so a localized loss is recoverable | **The actual survivability lever on Good is FREQUENCY diversity** (carrier spread). The burst interleaver's *time* spread buys little on Good (frozen channel) and earns its keep on Moderate/Poor. **Axis should be channel-adaptive** — open audit item. |
| **FEC — LDPC** | Recover erased/weak bits from strong carriers | The engine that turns diversity into delivery. |
| **Rung selection (mod × rate)** | Pick a constellation the channel can carry | **8PSK is too marginal on Good@20** (measured: 1 clean / 1 slow / 1 fail across 3 seeds; offline 52/150 chunks vs **QPSK 106/150**). QPSK is ~2× more survivable. **Rung choice is itself a survivability lever.** |
| **ARQ / burst transport** | Retransmit lost groups | Couples survivability ↔ airtime: every fade loss = a full resend + turnaround. |
| **Airtime efficiency (protocol)** | Minimize turnarounds & dead air | The *separate, probably-bigger* throughput lever: 8PSK hits only **49% of ceiling on a perfect AWGN channel** — that half is pure overhead, no channel involved. |

## 3. Where the survivability lever actually is (and isn't)

Measured today, against the "estimator lever" hypothesis:

- **NOT the channel estimator.** Re-tuning the Wiener model Moderate→Good was
  flat on both 8PSK (52→50 chunks) and QPSK (106→110) — noise-level. A carrier
  in a deep null has low SNR regardless of estimate quality; you cannot
  interpolate energy that isn't there.
- **IS diversity + FEC + the right rung.** What recovers the null-hit bits is
  spreading each codeword across *strong* carriers (frequency diversity) and
  letting LDPC fill the gap — and *not* asking a too-tight constellation (8PSK)
  to ride a notch QPSK sails through.

So fade survivability on Good = **frequency diversity (carrier interleave) +
erasure-aware FEC + a rung the channel can carry (QPSK over 8PSK)** — *not*
estimator polishing, *not* DD.

## 4. The gap to 3000, decomposed

8PSK R3/4 raw ceiling ≈ 4780 bps. Delivered on Good@20 ≈ 610–710 bps. The gap
splits into three, and naming them stops us optimizing the wrong one:

1. **Rung marginality** — 8PSK barely survives Good@20 → heavy resends → slow.
   QPSK (ceiling 3190) is far more reliable there. *Likely the cheapest win:
   pick the right rung per channel.*
2. **Airtime/efficiency** — even on a perfect channel a rung delivers ~49% of
   ceiling (preamble/CP/pilots/turnaround/ACK/gaps). The separate, large lever.
3. **Irreducible null loss** — recovered by diversity+FEC, bounded by physics.

Reliable 3000 on Good@20 most plausibly comes from **QPSK at high airtime
efficiency** (3190 ceiling × good efficiency), *not* 8PSK fighting the nulls —
unless/until frequency diversity makes 8PSK reliable enough that its higher
ceiling pays off. That is the open design question.

## 5. Priority & sequencing (decided)

**Survivability first, then airtime** — and they're coupled, so survivability
work pays an airtime dividend (fewer resends) and unlocks running a faster rung.
Order:

1. **Rung selection adaptive to channel** — stop forcing 8PSK on Good; let the
   channel pick QPSK vs 8PSK by measured fade/SNR. (Cheap, high-impact.)
2. **Frequency-diversity audit** — confirm the carrier interleaver actually
   scatters each codeword across the full band on Good; make the diversity
   *axis* channel-adaptive (frequency for Good, time/burst for Moderate/Poor).
3. **Erasure-aware LLR (anti-poison) audit** — confirm optimal null handling.
4. **THEN airtime efficiency** — with proper measurement tooling, attack the
   protocol overhead (the 49%-of-ceiling gap).

Every step under the standing rule: **channel-adaptive by design**, proven on
multi-seed faithful (GUI) gates, no single-cell claims.

## 6. Today's measured findings (consolidated)

- **DD is net-negative on Good HF; adaptive gate shipped.** DD-on cascades
  (8PSK Good@20: 83–125 CW fails, no delivery); DD-off/adaptive delivers. Gate:
  `use_coherent_dd` requires `last_fading_index < 0.15`. GUI: seed 42 PASS,
  seed 43 delivered CRC-clean (slow, hit harness time budget), seed 44 real fail
  (8PSK rung marginality, not DD). AWGN unchanged (DD stays on).
- **Per-symbol pilot-anchor innovation gate is ineffective** (flat across 4×
  tightness sweep) — a wrong-decision rotation and a legit between-pilot
  interpolation error are indistinguishable per-symbol. Removed.
- **Wiener estimator: correct but non-adaptive; re-tuning is flat** — not the
  survivability lever.
- **8PSK is the wrong rung for Good@20** (2× less survivable than QPSK,
  measured) — rung selection is a survivability lever.
- **Airtime is the separate, likely-bigger throughput lever** (49% of ceiling on
  a perfect channel) — attacked after survivability, with proper tooling.

## 7. Open questions / next

- Is the carrier/frequency interleaver actually full-band on Good? (audit)
- Channel-adaptive diversity axis & rung selection: design + implement.
- Quantify the airtime decomposition on the current post-warm-handoff build
  (payload vs overhead vs resend) to size lever #2 precisely.
- Harness: `exit-after` budget too tight for slow marginal transfers → false
  FAIL on delivered files (BUG-HARNESS-001 family); score on CRC-clean delivery.

## 8. Throughput north star + the bit-loading history (2026-05-29)

### The target

Reference industry-leader curve at **Multipath-Good** (operator-supplied):

| SNR (dB) | bytes/min | ≈ effective bps |
|---|---|---|
| 40 | 28120 | 3749 |
| 30 | 27385 | 3651 |
| **20** | **23142** | **3086** |
| 10 | 5973 | 796 |
| 0 | 1752 | 234 |

**North star: ~3086 bps effective at Multipath-Good SNR 20.** This is physical
(Shannon at SNR 20 in 2.8 kHz ≈ 18 kbps; 3086 is well within). The leader runs
near its own MPG ceiling (~3749) at SNR 20, i.e. high-order modulation at high
efficiency. Our current Good@20: QPSK ~1820 clean, 8PSK marginal 610–710.

### Why uniform QPSK cannot reach it

QPSK R3/4 **raw** ceiling ≈ 3190 bps; the leader's *effective* 3086 is 97% of
that. Overhead alone forbids reaching 3086 effective on a rung whose raw ceiling
is 3190. So the target **requires higher-order modulation** working on Good —
retreating to uniform QPSK caps us below the leader by construction.

### The prior bit-loading attempt and its hard finding (re-examined)

Branch `experimental/per-carrier-bit-loading` (2026-05-05/06) tried **closed-loop
dynamic per-carrier TX masks** (RX measures per-carrier γ_k, signals TX which
carriers to load). Killed (commit `35997a9`, Codex 2026-05-06 review + hardware
test): *"per-carrier γ_k swings on sub-second timescales while mask propagation
has 1–2 frame (1.5–3 s) latency → any TX-supplied mask is stale by the time it
reaches the RX; feature ENABLED on Good caused 117 retx / 120 timeouts."*
Redirected to RX-side per-carrier **erasure** (no TX adaptation — the
carrier-erasure / anti-poison now in the tree), which makes the existing rung
robust but adds **no** throughput.

**Re-examination with the current big picture (two corrections):**
1. **The wall is real but was inflated by a channel bug.** The 2026-05-05 test
   predates the 2026-05-26 discovery that "Good" was mislabeled with **Moderate's
   0.5 Hz Doppler** (`kGoodHFDesignDopplerHz`, now 0.1 Hz). At 0.5 Hz, coherence
   time ≈ 0.85 s ≪ the 1.5–3 s feedback latency → closed-loop is hopeless. At
   *true* Good (0.1 Hz, ~4 s coherence) it is merely marginal. So the finding was
   sound *for the channel they tested*, but harsher than true Good.
2. **The right conclusion is not "redirect to erasure" — it's "don't use
   feedback-driven per-carrier loading at all."** Closed-loop CSI is
   fundamentally stale on any fading HF channel (feedback latency vs coherence).
   Per-carrier bit-loading is the wrong tool for HF for this reason. The leader
   almost certainly does **not** use it.

### The no-feedback path to 3086 (the reframe)

Get high-order throughput by **absorbing** the notch with redundancy, not
**routing around** it with stale CSI:

- **Uniform higher-order modulation (16QAM) at a lower code rate (e.g. R1/2)**
  with **strong long-block FEC** + frequency interleave. 16QAM R1/2 = 2.0 info
  bits/sym (vs QPSK R3/4's 1.5) and 50% redundancy comfortably exceeds the ~23%
  notch — no fast CSI feedback needed. Raw ceiling ≈ 4250 > 3086.
- **Per-burst adaptive mod-cod (AMC)** — pick the uniform rung from a *slow*
  channel-quality estimate. Slow adaptation matches slow feedback (no staleness
  wall); this is the leader's "adaptation," coarse not per-carrier.
- **Stronger/longer FEC** — long-block (we have N=1944 via Z=81; the leader's
  turbo-long-block has more coding gain) to maximize what redundancy buys.
- **Airtime efficiency** — the separate ~2× lever (49% of ceiling on a perfect
  channel).

**Verdict on the lever — UPDATED 2026-05-29 (this section's first take was wrong;
measured):** the "uniform high-order + low rate" idea was tested and **failed** —
uniform 16QAM folds on Good@20 at every rate (rung sweep), and the deeper cause is
that **16QAM does not decode on frequency-selective Good@20 at all** (GUI-confirmed:
256 codeword failures, 0 delivery), despite 85% of carriers being 16QAM-capable
(channel headroom is fine; genie ceiling 3764 > 3086). The real gate is a **16QAM
receiver-decodability wall** (~10–14 dB structural penalty; not pilots/Wiener/DD/
erasure/SNR — root cause un-isolated, suspects phase/CFO + amplitude sensitivity).
This blocks BOTH uniform-16QAM AND per-carrier bit-loading (bit-loading still needs
16QAM to decode on the carriers it places it on). **So the path to 3086 is: first
fix 16QAM decodability on fading, then choose uniform-high-order vs bit-loading.**
Full diagnosis: `docs/16QAM_DECODABILITY_DIAGNOSIS_2026_05_29.md`.
