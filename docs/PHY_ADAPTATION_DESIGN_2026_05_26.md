# Channel- & Traffic-Class-Adaptive PHY Design (Pilots / LDPC / Interleaver / ARQ)
2026-05-26. Design document for reaching the industry-leader's Good-fading file-transfer
throughput (~3000 bps at in-band SNR 20) **reliably**, without cheating or hacking. Captures
the diagnosis from the 2026-05-26 campaign and the staged, coupled correction plan.

Status: DESIGN — not yet implemented. Companion measured evidence in
`docs/FADING_RELIABILITY_CAMPAIGN_2026_05_26.md`; competitive analysis in the private docs.

---

## 1. Goal & the wall we hit

Goal 1: ~3000 bps **delivered file-transfer** throughput at Good fading, in-band SNR 20,
matching the industry leader. Current proven state: QPSK R2/3 = ~1350 bps (5/5 seeds, rock
solid after the anti-poison fix). The gap is **not** a single tuning knob — every attempt to
go faster by rung (8PSK R2/3, QPSK R3/4) or by reclaiming carriers (pilot spacing 5→10)
**passed on clean/mild seeds and collapsed on the deep-fade seeds** (e.g. R3/4 seed: 145
CWFAIL/0 bytes; spacing-10 seed5: 40 CWFAIL/630 bps/2 downgrades). The common wall is
**fade survival of sustained high-rate traffic**, and it is an *architectural* gap, not a
parameter.

Leader reference (2750-Tactical ladder, ~2.75 kHz): levels 11/12/13 are **4PSK (QPSK), 59
carriers, 42 baud**, net 2423/3230/3877 = code rates ~1/2, 2/3, 3/4. We match baud (41.667).
The leader's edge is three stacked things: (a) ~59 usable data carriers (~2% pilot overhead
vs our 20%), (b) long interleaved (turbo-class) FEC giving fade-diversity so the high rungs
survive fading, (c) a lean, pipelined protocol (~high airtime efficiency).

---

## 2. Core principle

PHY parameters must adapt along **two axes simultaneously**:

1. **Channel** — SNR, delay spread (→ coherence bandwidth → pilot spacing, CP, CIR window),
   Doppler spread (→ coherence time → interleaver depth, pilot time-rate, burst validity).
2. **Traffic class** — control/ACK vs interactive chat vs bulk file. These have *opposite*
   latency/reliability/throughput priorities, so they want different LDPC length and
   interleaver depth even on the *same* channel.

Today we use ONE profile (N=648 LDPC, fixed comb-ish pilots spacing 5, within-frame
interleaving) for everything. That is the root simplification to correct.

---

## 3. Traffic classes and their requirements

| Class | Payload | Priority | Latency budget | Fade strategy |
|-------|---------|----------|----------------|---------------|
| **Control / ACK** | ~20 B, 1 CW | **reliability + low latency** (gates the whole ARQ loop) | must be ~instant | survive by robustness (low rate, dense pilots), NOT by interleaving |
| **Chat / interactive** | small, 1–few frames | low latency + first-try reliability | sub-second | survive by *brevity* + cheap retx |
| **Bulk file** | large, many frames | **throughput** | seconds OK | survive by *diversity* (long code + deep interleave) |

Key consequence (user-identified, correct): **ACKs must NOT use big LDPC or a deep
interleaver.** A deep interleaver delays decode by its depth; putting that on an ACK adds
latency to *every* turnaround → cripples the ARQ loop. ACKs stay short, uninterleaved,
robust, instant. Chat stays low-latency (shallow interleave). Only **bulk file** pays the
latency for deep interleaving — and there it's free (a file doesn't care about ~1–2 s).

---

## 4. Pilot-spacing strategy (channel-adaptive, delay-spread-derived)

Pilot density is set by the **2-D sampling theorem**, not by trial:
- Frequency (carrier spacing): `Δf_pilot ≤ 1/τ_max` (delay spread). Subcarrier δf = 46.875 Hz.
- Time (pilot symbol rate): `Δt_pilot ≤ 1/f_D` (Doppler); scattered rotation already covers this.

Nyquist table (carriers), with margin:

| Channel | τ_max | Nyquist | 3× margin |
|---------|-------|---------|-----------|
| Good | 0.5 ms | 42 | ~14 |
| Moderate | 1.0 ms | 21 | ~7 |
| Poor | 2.0 ms | 10 | ~3–5 |

Our fixed `spacing = 5` is **Poor-sized**, applied to all channels → ~8× over-dense on Good
(20% pilot overhead vs the leader's ~2%). BUT: a 2026-05-26 multi-seed test proved that
**spacing alone is gated by channel-estimation quality, not just Nyquist** — spacing-10 on
Good gained +9% on clean seeds but *broke the deep-fade seeds* (40 CWFAIL). So:

**Pilot spacing is a coupled set of THREE parameters that must move together, all derived
from the same delay-spread estimate:**
1. `pilot_spacing` (frequency sampling).
2. Wiener interpolator delay model (`kRobustDelaySpreadS`, currently a fixed 1 ms worst-case —
   must become channel-class-derived, RX-local; no TX/RX negotiation needed since only the
   pilot *positions* must match, which follow from spacing).
3. CIR window length `L` in `interpolateChannel()` (currently hard-coded 5 taps; must = τ_max·BW).

Design: make pilot spacing **adaptive per channel class** (negotiated like rate/mod, both ends
agree), with the Wiener delay model + CIR window co-derived. Good → sparser (recover carriers)
ONLY once fade-diversity (Section 6) makes the deep-fade seeds survivable; otherwise the sparse
pilots break exactly when we need them.

---

## 5. LDPC strategy (blocklength by traffic class)

Current: single 802.11n-style **N=648** for everything (K varies by rate). At QPSK R2/3 a
codeword spans ~7 OFDM symbols ≈ **170 ms** — far shorter than the Good fade coherence time
(~4 s), so a codeword lives entirely inside one fade state and a deep null wipes it whole.

| Class | LDPC blocklength | Why |
|-------|------------------|-----|
| Control/ACK | short (N=648, 1 CW — current) | tiny payload; coding gain less critical than latency |
| Chat | N=648 (current) | low latency; brevity gives fade robustness |
| **Bulk file** | **long: N=1944 → DVB-S2-class (4k–16k)** | coding gain (~0.5–1.5 dB) AND a long codeword spans more diversity samples when interleaved |

The long code is necessary but **not sufficient** alone — a long codeword on contiguous,
co-fading resource gains nothing. It must be paired with deep interleaving (Section 6). Ties
to backlog #122. Consider whether to keep LDPC + deep interleave vs a turbo code (the leader's
likely choice); LDPC + long block + deep interleave should achieve comparable fade-diversity
and keeps our decoder.

---

## 6. Interleaver strategy (depth by traffic class) — THE KEYSTONE

The fade-diversity mechanism. Good fading is **slow (0.1 Hz, Tc≈4 s) and frequency-selective
(0.5 ms delay)**. A deep null sits on some carriers for the whole frame; within-frame
interleaving (what we have) only spreads a codeword across carriers, so a null corrupts a
fixed fraction (~17–21%) of *every* codeword persistently — R2/3 (50% parity) survives,
R3/4 (25%) does not.

To make high rungs survive, the codeword must span **time** long enough that the slow fade
*varies* underneath it → the persistent loss becomes an averaged, recoverable loss = **time
diversity**.

| Class | Interleaver | Depth | Latency cost |
|-------|-------------|-------|--------------|
| Control/ACK | **none** | 0 | none (instant decode — required) |
| Chat | within-frame (current) | ~1 frame | sub-second |
| **Bulk file** | **deep cross-frame/time** | **~1–2 s (toward Tc)** | seconds (acceptable for bulk) |

Deep interleaving is what lets the file class run R3/4 / 8PSK reliably on fading — the leader's
real edge. The depth is bounded by: (a) fade coherence time (want to span it for full
diversity) vs (b) acceptable file latency + buffer memory + (c) half-duplex burst length (the
interleaver block should fit within a TX burst). Tunable; start ~1 s and measure.

---

## 7. Synthesized per-class PHY profiles (the target)

| Param | Control/ACK | Chat | Bulk file (Good) |
|-------|-------------|------|------------------|
| Modulation | DQPSK/QPSK robust | QPSK | QPSK→8PSK (channel-adaptive) |
| Code rate | low (R1/4–R1/2) | R1/2–R2/3 | R2/3–R3/4 (diversity-protected) |
| LDPC N | 648 (1 CW) | 648 | long (1944+) |
| Interleaver | none | within-frame | deep cross-frame (~1–2 s) |
| Pilot spacing | dense (5) | dense (5–8) | adaptive (toward ~10–14 when clean, once diversity holds) |
| Latency | instant | sub-second | seconds (OK) |

---

## 8. Protocol / ARQ interaction (must be co-designed)

- **ACK robustness & latency**: ACKs gate selective-repeat. Keep them short + uninterleaved +
  robust. Do NOT deep-interleave ACKs (would add the interleaver latency to every turnaround).
- **Half-duplex**: deep file-interleaver block must fit inside the sender's TX burst (its turn);
  then T/R turnaround → ACK. Interleaver depth couples to burst length and the ACK cadence.
- **Retransmission unit vs interleaver block**: the SR-ARQ retransmission granularity must align
  with interleaver block boundaries; a partial block can't be decoded. Consider HARQ soft-
  combining (we already soft-combine) so retransmitted diversity adds to the buffered block.
- **ACK RTO / sack-delay**: must account for the *decode latency* of the deep interleaver (the
  receiver can't ACK until it has buffered+decoded the block) — extends the half-duplex hold.
- **Efficiency lever (separate but stacks)**: ~48% airtime is dead air (ack_repeat=3,
  ~16.5 s file-start setup, inter-burst gaps). Recovering it is a ×1.5 on whatever rung is
  reliable — independent of the FEC keystone, do it in parallel.

---

## 9. Multi-step correction sequence (each step proven before the next)

0. **Bank anti-poison** (proven: relative-depth per-carrier CSI for QPSK/QAM8). Reliability
   foundation that makes sparser pilots / higher rungs survivable. Codex-review → commit.
1. **Traffic-class PHY profile framework** — dispatch (control/chat/file) selecting the param
   set; no behavior change yet (chat/file both = current profile). Plumbing + tests.
2. **Long LDPC blocklength for the file class** (N=648 → 1944+). Decoder + frame format.
3. **Deep cross-frame interleaver for the file class** (the keystone). Buffer/latency + ARQ
   alignment + HARQ.
4. **ARQ/ACK co-design** — ACK stays short/robust; RTO/sack-delay account for interleaver
   decode latency; retransmission↔block alignment.
5. **Adaptive pilot spacing** (channel-class-derived spacing + co-matched Wiener delay model +
   CIR window) — now safe because step 3 gives the fade-diversity that sparse pilots need.
6. **Efficiency / dead-air** (ack_repeat adaptive, file-start setup, inter-burst pipelining) —
   stacks ×1.5; can run in parallel from the start.

Each step: GUI multi-seed (≥5) + whole-matrix (Good/Moderate/AWGN, no regression) + faithful
clock + honest end-to-end goodput; Codex independent review of PHY diffs; revert losers.

---

## 10. Validation methodology

- **Channel-est quality (the genie)**: instrument estimated-H vs OTASim true channel-H,
  per-carrier MSE on failing frames. This is the principled oracle for pilot/interpolator
  changes (estimate quality), not just FER. Build before step 5; useful for step 3 too.
- **Per-class gates**: ACK = first-try reliability + turnaround latency; chat = end-to-end
  message latency; file = delivered goodput + completion under fading (multi-seed).
- **Whole-matrix**: never fix Good by breaking Moderate/Poor/AWGN (the recurring failure).

---

## 11. Risks & guardrails (lessons from rounds 7–21)

- **Coupled params move together** — pilot spacing ↔ Wiener delay model ↔ CIR window; LDPC
  length ↔ interleaver depth. Changing one alone is the classic regression.
- **Measure, don't assume** — every change carries a multi-seed before/after; single-seed and
  clean-only results lie (proven repeatedly today).
- **Latency is a real cost** — deep interleaving is for bulk only; never on ACK/chat.
- **Determinism** — pilot *positions* and LDPC/interleaver geometry must be identical TX/RX
  (negotiated/deterministic from signaled mode); RX-internal estimator params (Wiener delay)
  may adapt freely.
- **No 100%-duty / no-pipeline-across-ACK cheats** (half-duplex + PA duty constraints stand).

---

## 11b. Implementation plan — file-by-file, ctest-gated (the careful build order)

Scoped 2026-05-26. N=648 is wired into the FEC core; every step is **behavior-preserving and
full-ctest-gated** before the next. Pre-deployment → wire formats are free to change (no
back-compat), but the *codebase* must stay green throughout.

Where N=648 lives today (all must become length-aware):
- `src/fec/ldpc_codec.hpp` — `CODEWORD_BITS=648`, `CODEWORD_BYTES=81` (static constexpr).
- `src/fec/ldpc_802_11n.hpp` — 802.11n base matrices for **n=648, Z=27** only.
- `src/fec/burst_interleaver.hpp` — `CODEWORD_BITS=648`, `BITS_PER_FRAME`.
- `src/fec/carrier_ldpc_interleaver.hpp` — `kLdpcCodewordBits=648`, perm `(307·i) mod 648·Ncw`.
- `src/fec/frame_interleaver.hpp` — `BITS_PER_CODEWORD=648`.
- `src/fec/codec_factory.cpp` — 648 in the registry entry.
- `src/protocol/frame_v2.*` — payload-capacity math assumes 648.

**Step A — parameterize codeword length (behavior-preserving).** Make codeword bits a *codec
instance property* (default 648), not a global constexpr; thread it through the interleavers
(perm becomes `(P·i) mod (Nbits·Ncw)` with P chosen coprime per length) and the capacity math.
NO new length yet. Gate: full ctest green + a Good@20 multi-seed shows byte-identical behavior
to HEAD (648 unchanged).

**Step B — add the long code (additive).** Add the **802.11n n=1944, Z=81** base matrices
(reproduced exactly from the IEEE 802.11n standard tables — NOT from memory; cross-check) as a
second matrix set + a codec variant. Existing 648 path untouched. Gate: encode/decode unit
test for n=1944 at each rate (round-trip + AWGN BER curve sanity), full ctest green.

**Step C — traffic-class profile + frame signaling.** Add the profile selector
(control/chat/file) and a frame-header field for codeword-length/profile so RX matches TX.
Default everything to the *current* profile (648) — no behavior change until selected. Gate:
ctest + handshake/negotiation tests.

**Step D — deep cross-frame interleaver for the FILE profile (the keystone).** New interleaver
spanning multiple frames (~1–2 s depth, sized vs fade coherence time + half-duplex burst +
buffer). TX buffers/spreads, RX de-buffers before decode. Couple to ARQ block boundaries +
HARQ soft-combine. Gate: Good@20 multi-seed FER + delivered goodput vs the 1350 baseline +
whole-matrix no-regress; genie channel-est MSE if needed.

**Step E — ARQ/ACK co-design.** ACK stays short/uninterleaved; RTO/sack-delay account for the
interleaver decode latency; retransmission unit aligned to interleaver blocks. Gate: ARQ unit
tests + multi-seed file completion under fading.

**Step F — adaptive pilots + efficiency** (Sections 4 & 8), now safe atop the diversity.

Each step is independently committable branch-only; Codex independent review of every PHY/FEC
diff before merge; revert any step that fails its gate.

## 11c. KEY FINDING: the existing burst interleaver is the WRONG granularity (don't re-enable it)

Scoped 2026-05-26. `src/fec/burst_interleaver.hpp` + the streaming burst-interleave path
already implement cross-frame interleaving (depth `kBurstInterleaveGroupFrames=8`), but it is
**disabled by default** — and commit `8d37864` disabled it on Good@20 *because disabling it
improved throughput*: **20KB Good@20 seed42 went 1814 → 2330 bps, retx/timeouts 32/30 → 4/1.**

Why it hurt (and why there is NO "just re-enable it" shortcut): the existing interleaver
spreads a **group of independent short (648-bit) codewords** across 8 frames → an
**all-or-nothing group**: RX must receive all 8 frames before it can deinterleave/decode any,
and one lost/late frame stalls the whole group → more retx/timeouts on a mostly-good channel.
The PHY diversity is outweighed by the ARQ/latency coupling.

CONSEQUENCE — this confirms the Step B+D+E coupling: the keystone is **one LONG codeword spread
across the time span** (diversity *inside a single codeword's FEC*), NOT a group of independent
short codewords. Long LDPC (B) and the codeword-spanning interleaver (D) are inseparable, and
both require HARQ/ARQ co-design (E) so partial loss soft-combines instead of stalling. The
existing `BurstInterleaver` is the wrong structure for this — keep it disabled; build the
codeword-spanning version against the long code.

## 12. Honest expectation

- Robust QPSK R2/3 floor (anti-poison + pilots-when-clean + efficiency): ~2400–2500 bps.
- Reaching a *reliable* ~3000 at Good@20 requires the **file-class long-code + deep-interleaver
  keystone** so the high rungs survive fading — that is the architectural step, multi-day to
  multi-week, and the real distance to the leader.
- "3000" is a file-transfer (latency-tolerant) target; the leader's table is PHY-net (~3230),
  their delivered file speed is lower (~2700–2900) — clarify which we're matching.
