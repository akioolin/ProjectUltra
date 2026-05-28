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

## 11d. PLAN CORRECTION: the keystone is the ARQ/transport fix, NOT a new LDPC [2026-05-26]

Re-reading `burst_interleaver.hpp`: it already spreads *each codeword's bytes across N physical
frames* (N=`kBurstInterleaveGroupFrames`=8) → that IS per-codeword fade-diversity, already
implemented. And `8d37864` disabled it because of **retx/timeouts 32/30 → 4/1**, a TRANSPORT
problem, not a decode problem: the deinterleaver needs all N frames of a group before it can
decode any, so on half-duplex with fades one lost/late frame stalls the whole group → retx/
timeout storm that outweighed the PHY diversity on Good@20.

=> The cheapest, lowest-risk keystone (adapt-what's-there) is **Step E FIRST**: fix the ARQ/
transport coupling so a deep-interleaved group does not stall —
- decode each codeword as soon as ITS bits have arrived across the group (don't gate the whole
  group on the last frame),
- align the SR-ARQ retransmission unit to the interleaver group + HARQ soft-combine partial
  groups (we already soft-combine),
- size the group vs the half-duplex burst so a group = one TX burst (no mid-group turnaround),
- ACK at group granularity, RTO covering group decode latency.
Then RE-ENABLE the existing burst interleaver on the FILE class and measure: fade seeds should
survive (diversity) WITHOUT the retx/timeout storm (transport fixed).

This REORDERS the build: the long LDPC (n=1944) + codeword-spanning interleaver (Steps A/B/D)
become a *later coding-gain refinement*, NOT the gate. The gate is the ARQ/transport fix +
re-enabling the diversity infra that already exists. No new LDPC matrices needed to test the
keystone hypothesis. (And the matrices must be SOURCED correctly if/when we do the long code —
not fabricated/hand-rolled; the custom R1/4 hand-roll already cost ~3 dB via 4-cycles.)

## 12. Honest expectation

- Robust QPSK R2/3 floor (anti-poison + pilots-when-clean + efficiency): ~2400–2500 bps.
- Reaching a *reliable* ~3000 at Good@20 requires the **file-class long-code + deep-interleaver
  keystone** so the high rungs survive fading — that is the architectural step, multi-day to
  multi-week, and the real distance to the leader.
- "3000" is a file-transfer (latency-tolerant) target; the leader's table is PHY-net (~3230),
  their delivered file speed is lower (~2700–2900) — clarify which we're matching.

## 13. FOLLOW-UP 2026-05-26 (evening) — measured reality + leader architecture blueprint

### 13.1 What the faithful GUI measurements actually showed (this session)
- **R3/4 Good@20 is seed-dependent / marginal**, NOT reliable: seed1 PASS ~1780, seed3 PASS
  ~1780, **seed2 FAIL** (40 CWfail, 0 bytes — never delivered the first data frame).
- **Discriminator (same seed-2, only rate changed):** R2/3 (33% FEC) decodes seed-2's fade with
  **0 CWfail**; R3/4 (25% FEC) gets 40. ⇒ R3/4's failure is **FEC margin, NOT a broken
  equalizer/σ²/CSI** (estimator is fine; ruled out empirically). Do NOT chase equalizer tweaks.
- **R2/3 20-seed reliability:** file delivered **18/20**, full-pass **17/20**, **goodput mean
  ~963 bps** (clean ~1350, fade-stressed 570–870). The 3 fails = 1 real PHY fade (seed20) +
  2 PROTOCOL bugs independent of rate: seed14 disconnect-teardown (file actually delivered
  clean!), seed16 half-duplex turn/ACK stall. So even the reliable rung is ~⅓ of goal AND has
  rate-independent protocol reliability gaps.
- **cli_simulator is NOT faithful for fade reliability** (same seed passes cli / fails GUI;
  CPU-paced protocol timing samples a different fade phase). Use the GUI for ALL fade work.
- **HARQ soft-combine is architecturally blocked**: it keys the combine by decoding CW0's
  header (`streaming_ofdm_decode.cpp` buildHarqKey peek), which fails on exactly the deep-fade
  frames that need combining → fires once, never accumulates. A provisional-key shortcut was
  already tried and reverted (false combines). Needs a fade-robust frame ID to work.

### 13.2 Leader architecture blueprint (the proven shape of reliable 3000 @ Good@20)
Public description of the industry-leader OFDM modem: **data blocks of ~196 OFDM symbols
(~5.2 s)**, each followed by a **small FSK ACK** that commands **speed-up / slow-down / resend**.
Three structural elements we lack, and why each matters (maps to our failures):
1. **Multi-second data block (~5.2 s).** At Good 0.1 Hz Doppler, coherence time Tc≈4 s. A 5.2 s
   block **spans >1 full fade cycle**, so interleaving over the block gives REAL fade diversity
   (a deep null only nicks each codeword instead of killing a frame). **Our ~1.2–1.8 s frames
   are SHORTER than the fade** → interleaving them buys ~nothing; the whole short frame sits in
   one fade (this is exactly why seed2 dies). **CORRECTION to earlier plan: the lever is BLOCK
   LENGTH (multi-second, >Tc), not "burst-interleave depth-N of short frames."** Bonus: one
   preamble + one T/R turnaround per 5.2 s amortizes overhead ~4× → on-air approaches raw
   (most of our 1780-on-air-vs-3250-raw efficiency gap).
2. **Robust out-of-band FSK ACK.** FSK survives fades that kill OFDM, so the **reverse-link ACK
   doesn't die** — dissolves our seed-2 reverse-link asymmetry, the OFDM-control fragility, AND
   the HARQ "can't ID a faded frame" catch-22 (the robust ACK channel carries the frame ID +
   rate command). This is the right answer to every ACK/control reliability problem we hit.
3. **Per-block adaptive rate** (speed-up/slow-down via the ACK) — **never grind a fixed thin code
   into a fade**; back off when it deepens, climb when it clears. seed2 would trigger a
   slow-down, not a 40-CWfail wipeout. We have only a hint of this (seed7 auto-dropped to R1/2
   and delivered); it must become the core loop driven by the robust feedback channel.

### 13.3 Revised path to 3000 (structural, multi-day → multi-week; de-risked by the blueprint)
Reliable 3000 @ Good@20 = **long interleaved block + robust (FSK/heavily-coded) ACK channel +
per-block adaptive rate**, working together. NOT a code-rate tweak. Ordered follow-up steps:
- **Step 1 (most leverage, attacks seed-2 directly):** grow the data block to MULTI-SECOND with
  interleave spanning the whole block (long-LDPC n=1944+ AND/OR multi-frame block + deep
  cross-block interleaver sized to >Tc). Gate: does seed-2 go FAIL→deliver on the faithful GUI?
- **Step 2:** robust out-of-band ACK/feedback channel (FSK or heavily-coded narrowband) carrying
  ack + frame-ID + rate command — fixes ACK fragility and unblocks HARQ-combine.
- **Step 3:** per-block closed-loop rate adaptation driven by Step 2's feedback.
- **Step 4:** efficiency (preamble/turnaround amortized by the long block) to convert on-air→3000.
- **Parallel, cheap, rate-independent:** fix the two protocol reliability bugs found this session
  — seed14 disconnect/teardown (file delivered but ALPHA never saw disconnect) and seed16
  half-duplex message turn/ACK stall. These cost ~2/20 reliability at ANY rate.

### 13.4 Banked this session (branch feat/good-fading-qam16-ladder-2026-05-24, NOT pushed)
- Burst-marker keystone FIXED (commits fc4bbf2 connect-deadlock + 469b60b light-LTS marker);
  phyDiagLine per-line flush. Equalizer ruled OUT as the seed-2 cause. R3/4-marginality,
  R2/3 20-seed stats, HARQ catch-22, and this blueprint recorded in memory
  `project_burst_keystone_broken_on_real_path` and `project_cli_not_faithful_for_fade_reliability`.

---

## 14. ARCHITECTURE DECISION 2026-05-26/27 — two-channel, one-way, sender-driven session

This section supersedes the framing of §6/§13 where it conflicts. It is the agreed
architecture after working through the *whole exchange cycle* (not just the file PHY). The
modem is redesigned at the **session/ARQ/orchestration layer only** — every PHY block
(MC-DPSK modem, OFDM modem, LDPC codec incl. n=1944, burst interleaver, chirp/LTS sync, CFO,
equalizer) is **reused**, and the MC-DPSK↔OFDM switch already exists (the handshake does it).
It is NOT a PHY rewrite. The ~25% being rebuilt is the connected-data engine, which today is
"negotiate→OFDM→selective-repeat window=8→DATA frames."

### 14.1 The two channels (TDD, role-fixed for a session)
- **Forward = OFDM bulk traffic.** Long interleaved data blocks, sender→receiver only.
- **Reverse = coordination channel only.** Robust, narrow, lean. Carries block-ACK/NACK,
  resend requests, per-block rate commands (speed-up/slow-down), and CANCEL. **It NEVER carries
  bulk data.** Because of the one-way rule (14.2) the reverse link can stay the lean robust
  waveform for the entire session — it never has to switch to an OFDM bulk block.

The handshake is already MC-DPSK, and with chat dropped (14.3) there is nothing between CONNECT
and the file, so the link **stays MC-DPSK from PING until the file-mode MODE_CHANGE**, switches
to OFDM-bulk for the forward link only, and the reverse stays MC-DPSK/FSK throughout.

### 14.2 One-way, sender-driven session (user-specified, 2026-05-27) — the big simplifier
A session is **strictly unidirectional bulk transfer.** Alpha holds the floor and sends to
Bravo until one of three terminal states: **completion** (DATA_END acked), **cancellation**
(either side), or **HF link dead** (sender loses N consecutive block-ACKs → declares dead).
**There is no mid-session role swap and no bidirectional bulk transfer.** If Bravo wants to
send, it waits until **both stations are idle**, then initiates its *own* one-way session the
same way Alpha did (symmetric at the session level, one-way within a session).

Why this is the keystone simplification (designs out bug classes, not patches them):
- **Kills the bidirectional-data contention class entirely** — the seed16 half-duplex turn/ACK
  stall (#145) was a *bidirectional chat collision*. With one floor owner and the receiver only
  ever ACKing, that ambiguity cannot exist. The hardest part of the control-plane hardening is
  removed by construction.
- **Reverse channel is permanently coordination-only** — never needs to carry/serve an OFDM
  block, so it stays a fixed lean robust signaling channel for the whole session.
- **Floor ownership is explicit and trivial within a session** — strictly alternating
  sender-block / receiver-ACK, one originator.

Floor acquisition between sessions = carrier-sense idle → initiate (the same cold handshake).
Contention (both grab at once) resolves by a **deterministic tiebreaker** (initiator/callsign
priority or asymmetric backoff) so no symmetric livelock can persist. Both stations converge to
idle on a **timeout** when the link dies, so a dead link never leaves a station hung (this also
covers the seed14 teardown bug: bounded DISCONNECT retransmit + unilateral teardown after N
tries → return to idle).

### 14.3 Chat dropped for now (scope cut, deliberate)
Interactive chat is **not supported in the redesign initially.** Chat is what introduced the
bidirectional turn-taking complexity and the OFDM-control-path deadlocks. A pure
handshake→one-way-file→teardown link has none of it. Chat can return later as a degenerate
one-way session (a 1-block transfer) once the spine is proven.

### 14.4 The exchange cycle (session state machine)
```
IDLE ──carrier-sense clear + traffic to send──▶ ACQUIRE FLOOR (tiebreaker on collision)
ACQUIRE ──▶ COLD HANDSHAKE (MC-DPSK, full chirp): CONNECT carries proposed file-mode params
            (rate, block length, interleave depth); CONNECT_ACK confirms/counters.
            [MODE_CHANGE folded into CONNECT_ACK; PING/PONG sounding folded into CONNECT]
HANDSHAKE ──▶ TRANSFER LOOP (forward OFDM bulk / reverse MC-DPSK coordination):
   repeat:
     SENDER:   TX one long interleaved OFDM block  ──▶ T/R turnaround ──▶ RX
     RECEIVER: (buffer+decode block) TX coalesced block-ACK/NACK + rate command (warm MC-DPSK/FSK)
     SENDER:   apply rate command; resend NACKed blocks (HARQ soft-combine); advance
   until  DATA_END-acked (completion) | CANCEL (either side) | N missed ACKs (link dead)
TRANSFER ──▶ TEARDOWN: bounded DISCONNECT retransmit + unilateral teardown after N ──▶ IDLE
```

### 14.5 Coordination-channel design (cold vs warm — the right split)
The reverse/coordination channel has **two jobs with opposite needs**; do not force one
waveform to do both:
- **Cold acquire (handshake, once/session):** no prior timing/freq lock → must search time +
  estimate CFO from scratch. A **chirp is the correct primitive** (sharp time localization +
  CFO in one shot). **MC-DPSK full-chirp is right here — keep it.** Robustness > speed for a
  once-per-session cost.
- **Warm ACK (every block, frequent):** timing + CFO already known from the just-received block
  → a full chirp is pure waste. Want a **short warm-synced burst** found in a narrow predicted
  window, **coalesced** (one cumulative block-ACK per block → minimize turnarounds, the
  half-duplex efficiency lever).

**THE N=648 TRAP (do NOT "make the control frame send fewer bytes"):** a control frame is
already 1 codeword and **LDPC N=648 is fixed at every rate** — the codeword is the same airtime
whether filled with 20 bytes or 2. Shrinking payload buys **zero** airtime. The real levers are
**(a) fewer handshake round-trips** (fold MODE_CHANGE into CONNECT_ACK, fold PING sounding into
CONNECT → 3 round-trips → ~2, ~halves cold-handshake airtime) and **(b) warm + coalesced ACK**
(attacks preamble + turnaround count). Bytes are not a lever; preamble and turnaround count are.

**Warm-ACK waveform — decide with data, not up front:**
- *Ideal endgame:* a tiny **non-coherent FSK** burst — the most robust way to send a few bits
  with no channel estimate, tolerant of CFO/timing slop, Goertzel-detectable in a known window
  (~4 dB worse than coherent, worth it at the floor). This is the leader's small-FSK-ACK and the
  right answer to every ACK/control-fragility problem we hit. BUT MFSK is "reserved only" today →
  new work (this is task #144).
- *Pragmatic interim (CORRECTED 2026-05-27 after code audit):* **full-chirp MC-DPSK ACK,
  coalesced** (one cumulative block-ACK per block). This is the TRUE zero-new-work interim — it
  exists today. ⚠️ MC-DPSK has **NO** warm/short/light-preamble path (verified
  `mc_dpsk_waveform.cpp:98-102` — always full dual-chirp; only OFDM has a short data preamble).
  So "warm-MC-DPSK" is NOT free; warm-MC-DPSK AND FSK are BOTH new PHY work. Ship full-chirp
  coalesced first (~1 s/ACK, ~17% overhead at a ~5 s block), measure whether ACK overhead is
  actually limiting, and only then build warm-MC-DPSK or FSK (#144). Do not block on this choice.

### 14.6 CORRECTION — diversity axis is channel-specific (frequency for Good, time for Moderate/Poor)
A key error in §6/§13 to fix: "long time-interleaved block" is the diversity lever for
**Moderate/Poor**, NOT for **Good**. Coherence time Tc≈0.42/f_D:

| Channel | Doppler | Tc | Delay τ | Bc≈1/(2πτ) | indep. freq bins / 2.8 kHz |
|---------|---------|-----|---------|------------|-----------------------------|
| Good | 0.1 Hz | **~4 s** | 0.5 ms | ~318 Hz | **~9** |
| Moderate | 0.5 Hz | ~0.85 s | 1.0 ms | ~159 Hz | ~17 |
| Poor | 1.0 Hz | ~0.42 s | 2.0 ms | ~80 Hz | ~35 |

- **Time diversity** ≈ `block_duration / Tc`. At **Good** (slow fade) you'd need an
  **8–12 s block** (2–3× Tc) for real time diversity — which **violates PA duty cycle** and
  goes **stale-CSI** across the block. So time interleaving has **diminishing returns at Good**.
  *This is exactly why the measurement said "burst doesn't help at Good@20" — physics, not a bug.*
- **Frequency diversity** is available NOW at Good: ~9 independent bins across the band, every
  symbol, **no duty-cycle cost**. The right Good@20 diversity tool is **frequency interleaving
  across carriers** (a frequency-selective null nicks each codeword instead of killing a frame)
  + FEC margin + rate adaptation. **NOT the time/burst interleaver.**
- **Moderate/Poor** decorrelate fast (Tc ≤ ~0.85 s), so a multi-second block spans several Tc →
  **time/burst interleaving is the right tool there**, and the block is sized for the *slowest*
  fade you support (a Good-sized block over-serves Moderate diversity automatically).

⇒ The depth-sweep harness must sweep **both axes**: carrier-spread (frequency) at Good, and
block-length (time) at Moderate — size each diversity tool against the channel where it works.
For the **Good@20 3000 target specifically**, the multi-second block's payoff is mostly
**EFFICIENCY** (one preamble + one turnaround per ~5 s amortizes overhead ~4×, closing the
on-air↔raw gap) + **frequency interleave** for diversity + **rate adaptation** to avoid grinding
a thin code into a deep null — NOT time interleaving.

### 14.7 How long-LDPC + burst interleaver fit under this architecture
- **Long LDPC (n=1944+, Step B):** serves the bulk-file class. At Good its value is coding gain
  + efficiency (longer codeword amortizes overhead) more than time diversity; at Moderate/Poor a
  long codeword spread over the multi-second block IS the time-diversity vehicle. Source matrices
  from the IEEE standard, never hand-roll (the R1/4 hand-roll cost ~3 dB via 4-cycles).
- **Burst interleaver (existing):** it is a **time-diversity** tool → its home is
  **Moderate/Poor / fragile rungs**, not Good@20 (confirmed by measurement *and* by 14.6).
  Keystone fixes (connect-deadlock fc4bbf2, light-LTS marker 469b60b) are done; it is ready for
  the Moderate/Poor file class. Per §11c/§11d its ARQ/transport coupling (all-or-nothing group
  stall) must be fixed before re-enabling: decode per-codeword as bits arrive, align retransmit
  unit to the group, HARQ soft-combine partial groups, size group = one TX burst.
- **Frequency interleaver (carrier-domain):** NEW emphasis from 14.6 — this is the **Good@20**
  diversity tool and may be the cheaper win for the stated target than the time block.

### 14.8 Master build order (incremental, parallel-path, GUI-gated — never a dead modem)
Build the new engine **alongside** the working one; prove each step on the GUI faithful clock;
do not remove the SR-window-8 engine until the new path beats it.

0. **Tc-aware diversity depth-sweep harness** (offline, deterministic, cheap) — sweep
   carrier-spread@Good and block-length@Moderate to find the REAL "long" before building the
   engine around a guess. *Highest information per effort; do first.*
1. **One-way session state machine + floor acquisition** (14.2/14.4): roles, terminal states,
   carrier-sense + tiebreaker, timeout→idle convergence, bounded teardown. Arm the file-mode
   transition **lock-free** (cached snapshot — the fc4bbf2 deadlock lesson), never a re-entrant
   mutex call. Designs out seed16; fixes seed14. cli is fine for this (transport mechanics).
2. **Lean handshake**: fold MODE_CHANGE→CONNECT_ACK, PING sounding→CONNECT (round-trip cut).
3. **Forward OFDM bulk block** (single long interleaved block) + **reverse coalesced warm ACK**
   (warm-MC-DPSK interim). Verify **per-turnaround MC-DPSK↔OFDM re-sync** early — RX dual-listen
   exists but per-turnaround switching under load is untested.
4. **Multi-block transfer + block-ARQ + HARQ** on the GUI faithful clock. **Gate: seed-2
   FAIL→deliver** (the discriminator). Apply the right diversity axis per channel (14.6).
5. **Per-block adaptive rate** driven by the reverse rate command (never grind a thin code into a
   fade; seed-2 should slow-down, not wipe out).
6. **Efficiency** (amortized preamble/turnaround from the long block; adaptive ack cadence) to
   convert on-air→3000.
7. **(Later) FSK reverse channel** (#144) if warm-MC-DPSK ACK overhead proves limiting.
8. **(Later) long LDPC n=1944** (Steps A/B) as coding-gain/efficiency refinement; **(later)
   chat** as a degenerate 1-block session.

Each step: GUI multi-seed (≥5) + whole-matrix (Good/Moderate/AWGN, no regression) + faithful
clock + honest end-to-end goodput; Codex independent review of PHY/FEC diffs; revert losers.

### 14.9 Bug classes this architecture eliminates by construction (not by patch)
- Bidirectional data contention / turn-stall (seed16) — one floor owner, receiver only ACKs.
- Reverse channel switching to fragile OFDM control — reverse is coordination-only, always lean.
- File-regime activation re-entrant deadlock — single deliberate lock-free MODE_CHANGE boundary.
- Hung station on dead link — timeout→idle convergence + bounded unilateral teardown (seed14).

### 14.10 Honest ceiling (restated)
This architecture delivers **reliable one-way file transfer** at Good@20 (the diversity block +
rate adaptation make the high rung survivable — the thing that's been failing). That is the real
milestone. **3000** then depends on block-length + R3/4 + dead-air *adding up*; the architecture
*enables* it but does not guarantee it. Ship reliable-transfer first; 3000 falls out (or doesn't)
from the efficiency/rate math on top. Multi-day → multi-week, built incrementally, never holding
a dead modem.

### 14.11 Prerequisite audit (2026-05-27, verified against code — what exists vs what to build)
Before building, an Explore pass verified each capability §14 leans on. Result: the plan is
directionally complete but **not build-ready as written** — 2 foundational pieces are missing, 1
needs wiring, 1 needs verification. Two assumptions were wrong and are corrected here.

| # | Capability | Verdict | Where | Action |
|---|------------|---------|-------|--------|
| 1 | **Long OFDM block as a TX unit** | 🔴 **DOES NOT EXIST** | OFDM TX hardcoded 4-CW (`frame_v2.hpp:932`); burst path *groups* separate 4-CW frames, doesn't merge | **Foundational build.** Default approach: use the **burst-group AS the block** (reuse `encodeBurstLight` `streaming_encoder.cpp:426` + keystone-fixed marker path) and fix its all-or-nothing transport (§11d), rather than rewriting the 4-CW frame core. |
| 2 | **Warm/short MC-DPSK ACK** | 🔴 **DOES NOT EXIST** | `mc_dpsk_waveform.cpp:98-102` always full dual-chirp; only OFDM has a short data preamble | **Corrected:** interim ACK = **full-chirp coalesced** (exists today, ~1 s, ~17% at 5 s block). warm-MC-DPSK and FSK are BOTH new work → later (#144). |
| 3 | **Production carrier-sense** | 🟡 **NOT IN MODEM** (exists one layer up) | `ModemEngine` has no busy-check (`modem_carrier_sense.cpp` is turnaround *timing* only); real detector is `AudioPort::isChannelIdleFor` / `ChannelBusyDetector` | **Wire**, don't build: Step-1 floor acquisition calls the AudioPort idle check. |
| 4 | **n=1944 long LDPC** | 🟡 **PARTIAL / unverified** | No IEEE n=1944 tables — code *lifts* n=648 base with Z=81 (`ldpc_802_11n.hpp:160-164`); girth-≥6 argued, not BER-proven; R1/4 is the known hand-rolled weak matrix | **Verify** with a BER curve before trusting at R3/4 (Step 8, deferred). |
| 5 | **Fade-faithful offline FER harness** | ✅ **EXISTS** | `tools/measure_ack_fer.cpp` runs frames through Watterson, no protocol | Step-0 tool ready. *Being protocol-free, it sidesteps the cli fade-unfaithfulness cause (CPU-paced protocol timing).* Needs extending to emit a long block (loops to #1). |
| 6 | **Frequency (carrier) interleaver** | ✅ **EXISTS** (fixed) | `carrier_ldpc_interleaver.cpp` spreads one codeword's bits across carriers; 307-multiplier **fixed**, opt-in | Step-0 freq axis = **ON/OFF A/B**, not a depth sweep. The Good@20 diversity tool. |

**HARQ catch-22 RESOLVED by the one-way model (was §13.1 blocker):** HARQ no longer needs to
decode a faded frame's CW0 header to key the combine — in the one-way sender-driven model the
**sender assigns block sequence numbers** and the receiver requests "resend block N" over the
coordination channel, so block identity is **positional/signaled, not decoded from the faded
payload**. Retransmissions of block N soft-combine by signaled ID. This removes a blocker the
plan was carrying into Step 4. (Requires the reverse coordination channel to carry block IDs —
which it does by design.)

### 14.12 BUILD-READY step map (file entry points, no open decisions)
Each step: branch-only, full-ctest-gated, GUI multi-seed where it touches fade, Codex review of
PHY/FEC diffs, revert losers. Default decisions are baked in below so building can start now.

- **Step 0 — Tc-aware diversity sweep** (#146). Extend `tools/measure_ack_fer.cpp` to (a) emit a
  multi-frame burst-group block (the §14.11-#1 long-block path), (b) toggle
  `carrier_ldpc_interleaver` ON/OFF. Sweep QPSK R3/4 over Watterson: **Good** = freq-interleaver
  ON/OFF A/B (the ~9-bin diversity claim); **Moderate** = block-length sweep (1×→3× Tc). Output:
  where each diversity axis actually buys FER margin. *Pure offline, deterministic, cheap. START HERE.*
- **Step 1 — one-way session SM + floor acquisition** (#147). New session state machine
  (roles, terminal states completion/cancel/link-dead, timeout→idle, bounded teardown) in the
  protocol layer. Wire floor acquisition to `AudioPort::isChannelIdleFor` (§14.11-#3) + a
  deterministic tiebreaker. Arm the file-mode MODE_CHANGE **lock-free** via the cached-snapshot
  pattern already in `tools/sim/simulated_station.hpp` (`file_profile_active_cached_` — the
  fc4bbf2 deadlock fix). Designs out seed16; fixes seed14. cli OK (transport mechanics).
- **Step 2 — lean handshake.** Fold MODE_CHANGE into CONNECT_ACK and PING-sounding into CONNECT
  in `frame_v2.*` + protocol engine (round-trip cut; pre-deployment wire formats are free).
- **Step 3 — forward block + reverse ACK.** Forward = burst-group-as-block (`encodeBurstLight`,
  marker path fc4bbf2/469b60b). Reverse = **full-chirp MC-DPSK coalesced block-ACK** (exists).
  Verify the **per-turnaround MC-DPSK↔OFDM re-sync** early (RX dual-listen exists; per-turnaround
  switching under load untested).
- **Step 4 — multi-block + block-ARQ + HARQ.** Positional/signaled block ID keys HARQ (§14.11
  resolution); fix burst all-or-nothing transport (decode-per-CW-as-arrives, align retransmit to
  group, soft-combine partials, §11d); frequency interleaver ON for Good diversity. **GATE:
  seed-2 FAIL→deliver on the faithful GUI.**
- **Step 5 — per-block adaptive rate** driven by the reverse rate command (reuse adaptive-downgrade
  machinery; never grind a thin code into a fade — seed-2 slows down, not wipes out).
- **Step 6 — efficiency** (amortized preamble/turnaround from the long block; adaptive ack cadence).
- **Later — Step 7 FSK reverse channel** (#144, new waveform; MFSK is reserved-only today);
  **Step 8 long LDPC n=1944** (BER-verify the Z=81 lift first, §14.11-#4); **chat as 1-block session.**

**Critical-path build order:** Step 0 (harness, also delivers the long-block emitter prototype) →
Step 1 (session SM, the hardening win) → Step 3/4 (the block + the seed-2 gate). Steps 2/5/6 and
the "later" items stack after the gate is green.

### 14.13 STEP 0 RESULTS (2026-05-27, branch feat/oneway-arch-2026-05-27) — harness validated + first finding REDIRECTS §14.6

Extended `tools/measure_ack_fer.cpp` (branch-only): added `--channel good|moderate|poor`,
`--mod`, `--rate`, `--carrier-interleave`, and a `data4_full` config (4-CW DATA frame, full
chirp preamble admitted as an anchor on the CONNECTED decoder so sync succeeds at *any* fade
depth — isolating decode-diversity from light-sync acquisition). In-process `SimulatedChannel`,
no OTASim transport → not subject to the cli clock-drift fade-distortion.

**HARNESS VALIDATED.** With sync isolated (sync_fail=0), it reproduces the GUI-known direction
on Good@20 (in-band 20 dB), n=150, coherent QPSK:
- R2/3: 7/150 = **4.7% decode FER**
- R3/4: 23/150 = **15.3% decode FER** (~3.3× worse)
AWGN@25 is 0% for both rates (failures are fade-driven, not a harness artifact). → fast,
deterministic, faithful PHY fade-diversity ground. Cold light-preamble runs are sync-dominated
(~45% sync_fail) and selection-biased — do NOT use the light path for decode-diversity.

**FINDING 1 — frequency interleaving is NOT the Good@20 R3/4 lever (REDIRECTS §14.6).**
Carrier-LDPC interleaver OFF vs ON, R3/4 Good@20, 3 seeds: 23↔22, 27↔28, 34↔35 — **within noise,
zero benefit.** Verified the toggle is genuinely applied (not a no-op): `carrierLdpcPlumbingEligible`
true (OFDM_CHIRP, 59 carriers == CARRIER_LDPC_MASK_CARRIERS, fft 1024), 4 CW ∈ [2,8] supported,
`carrier_ldpc_interleaver_enabled_` set, decoder `connected_`=true → `applyCarrierLdpcForward/Inverse`
run on both ends. So §14.6's claim that "frequency interleaving is the Good@20 diversity tool" is
**not supported by measurement.**

Most likely why (hypothesis, untested): a 4-CW frame's bits are ALREADY spread across all 59
carriers by the natural bit→carrier mapping, so each codeword already samples the ~9 independent
freq bins — the 307-permutation just reshuffles an already-spread set. If so, frequency diversity
is *already exhausted* and R3/4 still fails ~15% → the residual failures are NOT
frequency-selective-null losses (flat/slow Doppler amplitude fades + thin 25% FEC margin), which
neither freq nor (practical) time interleaving fixes.

**Architectural consequence:** at Good@20, R3/4 is **FEC-margin-limited**, matching the GUI
discriminator (R2/3 clean / R3/4 fragile). The realistic 3000 levers are therefore **R2/3-class
reliability (proven) + per-block rate adaptation (opportunistic R3/4 only when momentarily clean)
+ EFFICIENCY** — NOT interleaving diversity. The long interleaved block stays valuable for
**Moderate/Poor** (time axis, untested here) and for **efficiency** (preamble/turnaround
amortization), but it is not the mechanism that makes R3/4 reliable on Good.

**Step 0 still-open (cheap, same harness):** (a) does LONGER LDPC (n=1944 coding gain) cut R3/4
Good@20 FER — the distinct §14.7 lever, not yet wired into the harness; (b) Moderate time-block
axis; (c) confirm the "codeword already spans carriers" hypothesis. Item (a) is the highest-value
next measurement: it tests whether ~0.5–1.5 dB coding gain meaningfully helps the margin-limited
rung before any engine is built.

### 14.14 STEP 0 FINDING 2 — the CROSS-FRAME (burst) interleaver DOES rescue R3/4 on Good (REVERSES Finding 1's pessimism)

Per user redirect: the keystone test is **chunk recoverability**, not single-frame FER — send a
chunk of N file frames as one burst (full chirp anchor + N-1 light, via `encodeBurstLight`), deep
**cross-frame** interleave ON vs OFF, through Good fade, measure how many frames recover. Added
`burst_chunk` config + `--group`/`--burst-interleave` to the harness (branch-only). Fresh
encoder+decoder per chunk (full anchor → reliable sync), persistent channel (fade advances).

**RESULT — R3/4 Good@20, coherent QPSK, deep cross-frame interleave OFF vs ON:**

| group (chunk) | ~dur | OFF recovery | ON recovery | note |
|---------------|------|--------------|-------------|------|
| 2 | ~1.5 s | 50% | **0% — BROKEN** (all 40 chunks `chunk_sync_fail`) | min-clamp interleave bug, NOT data |
| 4 | ~3 s | 38% | **89%** | clean (sync_fail 0) |
| 8 | ~6 s | 34% | **92%** (3 seeds: 92/91/92%, chunks-complete ~88%) | clean |

Reference: isolated full-preamble single frame (`data4_full`) = **85%** recovery (15% FER).

**Reading (honest, three numbers):** the interleaved chunk delivers **~92% frame recovery** on
R3/4 Good@20. Compared to a *non-interleaved chunk* (same framing) it's 33%→92% — huge; compared
to *isolated full-preamble frames* (85%) it's a modest +7 pts. So the interleave does two things:
(1) compensates for the light-preamble fragility of in-chunk frames, and (2) adds genuine
cross-frame fade diversity over isolated frames. Either way the **deliverable number is ~92%**,
which makes R3/4 a *usable* rung on Good with ARQ resending the ~8% — **reopening R3/4 as the
Good@20 path**, reversing §14.13's "FEC-margin-limited, R2/3-only" conclusion.

**Why this does NOT contradict Finding 1 (frequency interleave = no help):** within-frame
frequency spreading can't help (one frame = one fade epoch). **Cross-frame** spreading puts each
codeword across multiple frames' independent-ish fade states — that's the diversity that works.
Different axis, opposite result, both correct.

**Why this does NOT contradict the prior "burst doesn't help Good@20" memo:** that cli run had
`full_groups=0` — the interleaver never engaged (ARQ batched 7 then 4 < group 8). It was a
non-test. This harness *forces* a full group every chunk, so it actually engages.

**Caveats before this is load-bearing (NOT yet GUI-confirmed):**
- **group=2 + interleave ON is BROKEN** (0% recovery, 100% sync-fail) — a real min-clamp bug to
  investigate; it makes group=2 unusable as the "<Tc baseline," so the scaling evidence is only
  group 4 vs 8 (89%→92%, weakly positive). Diversity is mostly "combine a few frame-fades per
  codeword," not a clean Tc-span law (group=4 ≈ 3 s < Tc≈4 s already gives 89%).
- OFF baseline (33%) is degraded by light-frame fragility, so the 33%→92% headline overstates the
  *pure-diversity* gain (which is the 85%→92% figure). Both are real; cite the right one for the
  claim.
- Offline isolated-chunk harness; the GUI faithful-clock end-to-end (with ARQ, turnaround,
  warm-state continuity) is the final gate — still pending.

**Architectural consequence:** the long **cross-frame-interleaved chunk IS the Good@20 R3/4
lever** — the file-class composite's core premise is validated in the offline harness. This
restores §6/§14.7 (deep interleave keystone) for Good, with the refinement that the working
mechanism is cross-frame (not within-frame frequency, Finding 1) and that ~3–6 s chunks already
capture most of it. Next: multi-seed group=4, fix/understand the group=2 bug, the Moderate axis,
then the GUI cross-check.

### 14.15 TRANSPORT MODEL — burst stop-and-wait (user-specified 2026-05-27; SUPERSEDES SR-ARQ for the file path)

The one-way file transport is **group-level stop-and-wait on big interleaved bursts**, NOT
selective-repeat. This is the leader's model and the correct fit for half-duplex.

- **A burst = one ~6–8 s interleaved chunk** = the validated cross-frame group (~8 frames at
  QPSK R3/4 ≈ 6 s). The whole burst is the ARQ unit.
- **Sender:** TX burst *k* → T/R turnaround → RX, wait for one ACK. ACK → burst *k+1*; no ACK
  (timeout/NACK) → **resend the entire burst *k*** (no partial/selective retransmit).
- **Receiver:** decode the full burst (deinterleave + LDPC), send **one** burst-ACK.
- **Rate PINNED to R3/4** for the file burst — the composite is what makes R3/4 hold, so the
  adaptive ladder must NOT downgrade it (the downgrade-to-R2/3 is what invalidated the first GUI
  attempt). No `recommendDataMode`/`capInitialOFDMRate`/adaptive-downgrade on the file path.
- **ACK** is the robust MC-DPSK coordination frame (§14.5).

**REMOVED for the one-way file path (do NOT carry forward):** selective repeat, SACK bitmaps,
fast-hole repair, "fast blocks", ack-repeat jobs, per-frame windowing — the entire
`selective_repeat_arq` complexity. Replaced by dumb-robust whole-burst stop-and-wait.

**KEPT:** the cross-frame burst interleave (the chunk itself, validated §14.14), `transmitBurst`,
the PHY. The interleave enablement wired into `ModemEngine::setDataMode` is correct; the
**transport around it** is what gets rebuilt.

**Rebuild increments (file path, parallel to the working modem, GUI-gated):**
1. **Pin R3/4 + coherent QPSK** for the one-way file transfer — bypass the rate downgrade.
2. **Group stop-and-wait transport** — chunk the file into 8-frame interleaved bursts, send one,
   wait one burst-ACK, resend the whole burst on timeout. Bypass/replace SR-ARQ for the file path.
3. Prove on the automated GUI file-send (faithful clock): R3/4 Good@20 file delivers across seeds
   (incl. the previously-failing ones).
4. Then strip chat + shorten handshake + remove turn-taking → the clean one-way sender.

### 14.16 BURST TRANSPORT — implementable design (mapped 2026-05-27 against the real ARQ code)

Landscape confirmed: `Connection` uses `SelectiveRepeatARQ arq_` (connection.hpp:373);
`StopAndWaitARQ` exists (arq.hpp); SR-ARQ already supports **window=1 stop-and-wait** (used by
MC-DPSK). The handshake is ALREADY lean (mode folded into CONNECT_ACK; `enterConnected`→
`applyDataMode`→`sendFile` already reaches one-way file exchange). So no handshake rework needed.

**The model:** stop-and-wait where the ARQ unit is the **interleaved burst** (one group of
`kBurstInterleaveGroupFrames`=8 frames ≈ 6 s), not a single frame. Send burst → one group-ACK →
resend the WHOLE burst on timeout. This is mandatory (not just simpler): the RX must have all 8
frames to deinterleave, so per-frame selective retransmit (SR-ARQ) corrupts the group.

**Why a new controller, not SR-ARQ reuse:** SR-ARQ's core is per-frame selective SACK/retransmit
— structurally incompatible with whole-group resend. Window=1 gives *frame*-level stop-and-wait,
not *group*-level. So the file path gets a dedicated **BurstStopAndWaitController** that sits
above the waveform and bypasses `arq_`:
- **TX:** chunk file → groups of 8 frames → `ModemEngine::transmitBurst(group)` (interleave on,
  committed) → T/R turnaround → await group-ACK(seq). ACK → next group; timeout/NACK → resend the
  same group. One outstanding group at a time (stop-and-wait).
- **RX:** decode the burst (deinterleave emits the 8 frames — proven in the burst_chunk harness)
  → all 8 present? → send group-ACK(seq); else → no ACK / NACK(seq) → sender resends the group.
- **Group-ACK:** a control frame carrying the group sequence (reuse `ControlFrame` ACK with group
  seq semantics; robust MC-DPSK coordination frame per §14.5).
- Rate pinned to R3/4 via the selector promotion (committed d7a4ca7).

**File targets:** new `burst_transport` controller; `file_transfer.cpp` (chunk into 8-frame
groups, drive the controller instead of feeding `arq_`); `connection.cpp` (route file-path TX/RX
through the burst controller, group-ACK handling); a group-ACK frame in `frame_v2`. Build as a
PARALLEL file path (keep `arq_` for any non-file use until removed), PHY tests green throughout,
GUI-prove R3/4 Good@20 delivery across seeds, then remove SR-ARQ/turn-taking/`sendMessage`.

This is the focused next build; the design above is the spec.

### 14.17 CORRECTION — protocol + header + ISS removal are COUPLED to the transport (GUI-proven 2026-05-27)

A GUI run (Good@20 seed 2, R3/4, file-only) PROVED the coupling that earlier sections missed.
Verified from logs: burst interleave DID engage on the real path (`burst_interleave=1`,
`[BURST-INTERLEAVED]` sync, `Burst interleave marker detected`, `Burst group complete (8 frames)`),
BUT the very first clean group decoded **0/8 CWs** (not fade, not retransmit — a structural
decode mismatch). The offline harness got 92% on the same config. The difference is the
**protocol/decode coupling**: `finalizeBurstGroup` deinterleaves+decodes the group using the
receiver's *locally configured* `fixed_frame_codewords_` + `burst_group_size_`, and on the real
path those don't deterministically match what the sender built. There is no per-burst negotiation
to reconcile it.

**Consequence (user-identified, correct): the burst transport CANNOT be bolted onto the existing
protocol. The protocol must be reworked WITH it.** Three coupled pieces, one rework:

1. **Burst DATA header declares the group structure.** The header must carry group size, CW
   count, modulation/rate, and interleave geometry so the one-way receiver decodes the group
   DETERMINISTICALLY from the bitstream — no negotiation. ⚠️ This CORRECTS §14.16's "no frame_v2
   change needed" (that was about the *ACK* frame; the *DATA* side needs a header change). This
   is the direct root of the 0/8 failure.
2. **Rip out ISS/IRS turn-taking.** `TURNOVER` (0x22), `TURN_REQUEST` (0x23), and the
   `local_data_turn_`/`peer_data_turn_requested_`/turn-ownership state are obsolete — one-way:
   initiator transmits, responder only listens + ACKs. Removing it also simplifies the
   send/receive flow the burst path rides.
3. **Burst stop-and-wait transport** (BurstStopAndWaitController, already built+tested §14.16) —
   wired against the new header + the one-way flow, not the SR-ARQ/turn-taking machinery.

These three are not independently shippable — the decode (1) needs the header, the flow needs ISS
gone (2), and the transport (3) drives both. So the next build is a coherent protocol rework, done
together and GUI-proven, not the incremental bolt-on §14.16 implied. Build order within it: header
format first (so RX can decode a sender-declared group → fixes 0/8), then route the one-way file
path through the controller, then delete ISS/IRS + SR-ARQ for the file path.

**0/8 ROOT CAUSE CONFIRMED = cross-station config mismatch (2026-05-27).** Offline isolation
(measure_ack_fer burst_chunk, ONE process configuring both ends): 4-CW, 8-CW, carrier-ldpc ON,
carrier-ldpc OFF — ALL give full recovery (64/64, 80/80) at clean AWGN. The GUI (ALPHA encoder
and BRAVO decoder in SEPARATE processes, configured via negotiation) ALWAYS gives 0/8. So the
interleaver/geometry/carrier-ldpc are all FINE — the failure is purely that BRAVO deinterleaves
against params that don't match what ALPHA built. The self-describing burst header fixes this by
construction (RX reads the sender's declared params, no reliance on negotiation agreeing).

**HEADER BOOTSTRAP (how RX decodes the header without prior knowledge):** the descriptor is a
FIXED-FORMAT, non-interleaved control frame (the existing 1-CW robust control encoding — a
compile-time constant both ends always know), emitted BEFORE the interleaved group. RX always
decodes it (format never varies), reads the group params, THEN deinterleaves+decodes the bulk.
On-air: `[fixed descriptor frame | non-interleaved]` then `[interleaved data group]`. Reuses the
existing control-vs-data split (control = fixed 1-CW; RX already peeks CW0 as a control frame).

**GUI requirement (operator, 2026-05-27):** the decoded burst-header info (group size, CW/frame,
mod/rate, interleave flags) must be surfaced in the GUI left compact block so the operator can
SEE what burst type is arriving. Decoder exposes the received descriptor → GUI side-panel display.

### 14.18 OPTIMAL FILE FRAMING — two layers, minimize air bytes (design 2026-05-27)

Goal: pack [per-burst decode info] + [file metadata] + [file bytes] so the receiver has
everything, with minimum air bytes. The optimization that matters most is **fewest bytes on air**
(compression), not framing micro-overhead.

**Layer 1 — PHY descriptor (per burst, fixed-format, decoded FIRST):** `BURST_HEADER` carries
`seq, group_size, cw/frame, mod, rate, interleave_flags, START/END`. ~1 CW inside the keyup
(~1-2%), repeated per burst (near-free, enables per-burst rate adaptation). Already built (§14.17).

**Layer 2 — transfer header (ONCE, in the front of burst #0's decoded data):** after PHY decode
the byte stream is `[FileStreamHeader][payload bytes]`. Compact binary, ~15 B + name:
```
FileStreamHeader:
  version       1B
  flags         1B   bit0 = compressed; bits1-3 = codec id (none/deflate/zstd…)
  original_size 4B   uncompressed file size (progress + decompress buffer)
  payload_size  4B   bytes actually transmitted (= compressed size if compressed) → drives done
  crc32         4B   checksum of the ORIGINAL (decompressed) file
  (reserved)    4B   start_offset for future RESUME after a dead link (re-send tail only)
  name_len      1B
  name          name_len B
```

**Compression is OPTIONAL and per-file (user, 2026-05-27):** TX compresses the whole file once
(best ratio) ONLY when it shrinks it; for incompressible input (.jpg/.zip/already-compressed) it
sends `codec=none` raw — never wastes effort. `flags` bit0 + codec id tell the RX whether/how to
decompress. RX: accumulate `payload_size` bytes → if compressed, inflate to `original_size` →
verify `crc32` → write as `name`.

**Why optimal (first principles):**
- Compression (when beneficial) is the real throughput multiplier — fewer air bytes beats any
  framing tweak; and it's free when it doesn't help (raw fallback).
- **Size-driven completion** (`payload_size`) — no wasted END frame/burst; the END flag is a hint.
- Header sent ONCE (front of burst #0); made reliable by stop-and-wait resending burst #0 if
  lost — not repeated per burst (only the cheap PHY descriptor repeats).
- Two integrity levels, no overlap: per-burst LDPC+frame-CRC (drives resend) vs whole-file `crc32`
  (final correctness gate after assemble+decompress).
- `start_offset` reserved now → resume-after-dead-link later without a format change.

On-air: `burst0 = [preamble][DESC START][{FileStreamHeader}{payload…}]`; `burstK =
[preamble][DESC][{payload…}]`; RX stops when bytes == payload_size → decompress → crc32 → write.

### 14.19 DROP LIGHT SYNC — one full anchor per burst (decision 2026-05-27)

**Decision (user): the new burst model does NOT need light/warm sync. Each burst self-syncs with
ONE full chirp+LTS anchor.** Light sync existed to avoid re-sending the chirp on every short frame
in the old fast-frame protocol (reusing warm timing state frame-to-frame). In the new model frames
are batched into 6–8 s bursts and a full anchor is ~0.4 s → ~5–7% per burst, amortized and ROBUST.
Light sync's saving is tiny on a long burst while its cost is fragility (the documented 100%
cold-light FER, warm-state staleness, cross-burst timing dependence). Trade the fragile ~5% for
robustness + simplicity.

**Clean burst structure (one anchor, descriptor + group under it):**
```
[ full chirp+LTS anchor ][ BURST_HEADER (1-CW, non-interleaved) ][ interleaved data group ]
```
RX syncs on the anchor → control-first peek decodes the descriptor → applies its declared config →
decodes the group. One anchor kills the double-anchor waste AND the RX-skip bug (a *light*
descriptor before the burst got skipped because the RX waits for a full anchor — proven in the
GUI log: ALPHA emitted the descriptor 2×, BRAVO decoded it 0×, syncing on the burst's full anchor
instead). So the descriptor moves INSIDE the burst keyup, right after the anchor.

**SEPARATE WORKSTREAM (after the burst path works): faster sync + faster ACK.** Once robust
full-anchor bursts deliver, optimize: a leaner re-sync (cheaper than a full chirp) and a tiny
FSK/short ACK (#144). These are efficiency layers, deliberately deferred so we get the robust
burst working first.

### 14.20 CLEANUP MAP — what to THROW vs KEEP for the one-way burst file modem

**THROW (old tech, remove during cleanup — replaced by the new burst path):**
- **Light/warm sync** machinery: light-preamble data path, warm-timing prediction,
  `expectFullOFDMAnchorOnce` dance, `seedExpectedFrameArrivalAfterSamples`, continuation-reanchor.
  Replaced by full-anchor-per-burst (§14.19).
- **ISS/IRS turn-taking**: `TURNOVER` (0x22), `TURN_REQUEST` (0x23),
  `local_data_turn_`/`peer_data_turn_requested_`/turn-ownership/`noteTurnRequestOnAckIfNeeded`.
  One-way: ALPHA sends, BRAVO listens + ACKs.
- **Selective-repeat ARQ** for the file path: SACK bitmaps, fast-hole repair, ack-repeat jobs,
  windowing, `sack_delay`, the whole `selective_repeat_arq` complexity. Replaced by group
  stop-and-wait (`BurstStopAndWaitController`).
- **Chat / interactive**: GUI chat compose (done), `protocol_.sendMessage`, fragment-message
  machinery, auto-message scenario paths.
- **Adaptive mid-session rate churn** + the over-engineered rate picker (selectOFDMCodeRate →
  capInitialOFDMRate → adaptive 3-pass) — simplify to one table AFTER 3000 (separate workstream).

**KEEP (reused substrate — the genuinely valuable, hard-won parts):**
- **PHY**: OFDM mod/demod, LDPC codec (incl. n=1944 lift), full chirp+LTS sync + CFO flow,
  channel estimation / MMSE equalizer, the burst (cross-frame) interleaver, carrier-LDPC.
- **MC-DPSK handshake** (PING/PONG/CONNECT/CONNECT_ACK) — but leaned (fold MODE_CHANGE in; already
  lean). Robust low-SNR coordination.
- **The full chirp anchor** — now used per-burst.
- **New pieces (this session)**: `BurstStopAndWaitController`, `BURST_HEADER` descriptor,
  `FileStreamHeader`, R3/4 promotion gate, OTASim + the GUI faithful-clock test path.

**SEPARATE WORKSTREAMS (deferred, after the burst path delivers):** faster sync (lean re-anchor),
faster ACK (#144 tiny FSK), n=1944 long LDPC coding gain, adaptive pilots, rate-picker simplify.

### 14.21 BURST_HEADER descriptor — implemented + the residual truncation bug (2026-05-27)

**Status: descriptor DECODE + RECONFIGURE proven; one residual feed-cadence bug; gated OFF in shipping.**

The self-describing burst from §14.17 is built end-to-end and the core mechanism is
proven on the faithful in-process harness (`measure_ack_fer --burst-descriptor 1`,
which deliberately mis-configures the decoder's group size + interleave to simulate
the cross-station mismatch that causes the GUI 0/8):

- **TX** (`StreamingEncoder::encodeBurstLight`, gated by `emit_burst_descriptor_`):
  emits a full-anchor 1-CW `BURST_HEADER` control frame at the head of an interleaved
  OFDM group, declaring `{group_size, cw_per_frame, mod, rate, interleave_flags}` from
  the encoder's own config. `BURST_HEADER` is now a control frame (`isControlFrame`),
  so it rides the robust DQPSK control profile.
- **RX** (`streaming_ofdm_decode.cpp` control-first intercept): decodes the descriptor
  (no address filtering — magic + CRC only), applies `setBurstInterleaveGroupSize /
  setFixedFrameCodewords / setBurstInterleave / setCarrierLdpcInterleaver`, stashes it
  in `last_burst_descriptor_`, and consumes the frame. **Proven**: with the decoder
  deliberately mis-set (group=2, interleave OFF), the log shows
  `Burst descriptor RX: group=8 cw/frame=4 bi=1 cldpc=0` — the receiver reconfigures
  itself from the sender's declaration. This is the fix for the cross-station 0/8.

**Two decoder bugs found + fixed during validation:**
1. The intercept consumed the descriptor but did not advance `correlation_pos_`, so the
   LTS detector re-locked the same anchor and re-decoded the descriptor forever. Fixed
   by advancing past the descriptor frame (`correlation_pos_ = sync_position_+frame_len`,
   `setSearchFloorLocked`).
2. The descriptor's full anchor seeded the warm-timing window, biasing the group sync.
   Mitigated by `resetFrameArrivalTrackingLocked()` + `expect_full_ofdm_anchor_=true` in
   the intercept (mirrors the FILE_CANCEL control path).

**RESIDUAL BUG (deterministic, faithfully reproducible at clean AWGN40):** with the
descriptor ON, the group-start frame is demodulated **one symbol short (28 → 27)** and
the deinterleave yields 0/4 CWs on every logical frame. Root-caused precisely:
- The encoded burst samples are **byte-identical** with/without the descriptor (the
  descriptor is simply prepended). So this is NOT an encoder-layout or fade problem.
- In `streaming_ofdm_decode.cpp`, `frame_len = std::min(frame_len, available)` processes
  whatever is buffered rather than waiting. When the descriptor precedes the burst it
  consumes the buffered lead, so at the moment the group-start frame is processed
  `available` is one symbol short → truncated frame → wrong bit count → deinterleave
  garbage. In the no-descriptor path the feed cadence happens to have the full frame
  buffered, so it works.
- This is **feed-cadence-dependent**: the fast `measure_ack_fer` pump exposes it; the
  real-time GUI feeds differently and may not reproduce it. That uncertainty is exactly
  why it must be validated on the GUI before enabling in the shipping path.

**Therefore the descriptor is gated OFF in `transmitBurst`** behind
`ULTRA_BURST_DESCRIPTOR=1` (env), so the working file path is byte-unchanged. To
validate live: run both GUI stations with `ULTRA_BURST_DESCRIPTOR=1` and watch BRAVO's
log for `Burst descriptor RX:` followed by `Burst group complete` + recovered frames
(instead of 0/8). The proper fix for the truncation is to make the group-start frame
**defer** until `available >= full_frame_samples` (don't process a truncated burst
group-start), then re-enable by default. Quick local repro of the bug:
`./build/measure_ack_fer --snr 40 --config burst_chunk --channel awgn --mod qpsk \
 --rate r3_4 --group 8 --burst-interleave 1 --burst-descriptor 1 --seed 1 --n 5`
(currently 0 recovered; `--burst-descriptor 0` gives 40/40).

### 14.22 Defer-until-buffered attempt — the real mechanism + principled fix (2026-05-27)

Took a shot at the §14.21 truncation. Two hypotheses tested on the faithful harness
(`measure_ack_fer --burst-descriptor 1`), both reverted (no magic-number fix shipped):

1. **Reset warm-timing on descriptor consume** (`resetFrameArrivalTrackingLocked` +
   `expect_full_ofdm_anchor_`): did NOT change the symbol count. Not a warm-window-bias
   problem.
2. **Buffer headroom** (`requirement.samples += getDataPreambleSamples()` for the
   ConnectedOFDMBurst group-start, in both `decodeCurrentFrame` and
   `checkIfReadyToDecode`): bare buffer → 27 symbols; +2-LTS-symbol headroom → **29**
   symbols (target is 28). Reverted — overshoots.

**Real mechanism (now understood):** the group-start (marker) frame is demodulated via
the variable-buffer path — `OFDMChirpWaveform::process()` → `processPresynced(samples, 2)`
skips 2 training symbols then demods **all remaining samples** with NO frame-length cap.
So **output symbol count == buffer size** (1 buffer-symbol = 1 output-symbol, confirmed:
bare=27, +2sym=29). descriptor-OFF lands on exactly 28 only by feed-cadence luck of how
much was buffered when `available >= requirement.samples` first held; the descriptor
shifts that timing by one symbol. `checkIfReadyToDecode`'s `available >= requirement`
gate is therefore NOT a true "wait for the whole frame" — it waits for a *minimum*, and
the demod then eats whatever is in the copied buffer.

**Principled fix (do this on the GUI-co-validated session, NOT a magic +1-symbol
headroom — that violates the no-heuristic-patch rule):** slice the group-start frame at
an EXACT size, identical to how `tryDemodulateNextBurstFrame` already slices frames
2..N (it copies exactly `burst_min_block_ = getMinSamplesForCWCount(fixed_frame_codewords_)`
and demods that — proven correct, 77/80 Good@20). Cleanest form: have the BURST_HEADER
descriptor-consume intercept **enter BURST_ACCUMULATING directly** with
`burst_next_pos_` pointing at the first group frame's data, so ALL N frames (including
the first) go through the exact-slice accumulation path and the negated-LTS marker /
variable-buffer group-start is retired entirely. This is the "descriptor-triggered
accumulation" design and it also removes the last dependence on the warm light marker
(aligns with §14.19 drop-light-sync). Needs the encoder group-start data-start offset
(descriptor_end + group-start preamble length) computed precisely + GUI two-process
validation.

### 14.23 PROPER FIX — descriptor-consume profile restore (2026-05-27)

The §14.21/§14.22 group-start truncation is **fixed**, root cause found with a
targeted `req.mode`/`full_frame` diagnostic rather than more theorizing:

**Root cause:** decoding the BURST_HEADER descriptor switches the waveform to the
1-CW **control profile** (control modulation + pilot geometry). Every normal
control-decode path restores the data profile afterward
(`if (switched_profile) waveform_->configure(saved_mod, saved_rate)`), but the
BURST_HEADER intercept `return`ed *before* that restore. So the following
group-start DATA frame was sized/demodulated with the control-profile pilots →
`getMinSamplesForCWCount(4)` came back **31104 instead of 32256** (one OFDM symbol
short) → the group-start demodulated 27 symbols instead of 28 → deinterleave 0/4 on
every logical frame. (The earlier "feed-cadence / buffer headroom" reads in §14.22
were wrong; the buffer size differed because the *waveform config* differed.)

**Fix:** restore the data waveform profile in the BURST_HEADER intercept before
returning. One line, mirrors the existing control path, no magic numbers.

**Proof** (`measure_ack_fer --burst-descriptor 1`, decoder DELIBERATELY mis-set to
group=2 / interleave-off to simulate the cross-station mismatch that causes the GUI
0/8):
- AWGN40: **40/40 frames, 5/5 chunks, 3 seeds** == descriptor-off baseline.
- Good@20: 70/80, 80/80, 62/80 == descriptor-off baseline (77/74/75); per-seed
  spread is fade-phase variation (the descriptor adds ~1.4 s ahead of the group).
- descriptor-OFF path unchanged (AWGN 40/40, Good 77/74/75); non-burst data4_full
  20/20. No regression.

**Status:** descriptor **enabled by default** in `transmitBurst`
(`ULTRA_BURST_DESCRIPTOR=0` disables as an escape hatch). The cross-station config
sync (the real 0/8 cause) is solved end-to-end on the faithful harness. **Remaining:
cross-station GUI two-process validation** — run both stations (default on) and
confirm BRAVO logs `Burst descriptor RX:` → `Burst group complete` → recovered
frames, with a real file delivering CRC-clean at Good@20 R3/4.

### 14.24 First cross-station GUI validation (2026-05-27) — header RX confirmed + 2 follow-on bugs

Ran the file-only Good@20 path (`good20_baseline_sweep.sh "3" <tag> R3/4 0`, message-count
0 = no chat, straight to file). Two-process `ultra_gui -sim` via OTASim, descriptor ON by
default.

**GOAL 1 ACHIEVED — the descriptor is received and interpreted correctly cross-station:**
```
[221.959] BRAVO Burst descriptor RX: group=8 cw/frame=8 bi=1 cldpc=1   (== ALPHA's send)
[224.356] BRAVO Burst interleave marker detected, entering accumulation
[232.640] BRAVO Burst group complete (8 frames), deinterleaving...
```
The header decodes, the declared params match the sender, and it drives accumulation +
group assembly on the real two-process GUI. The §14.23 profile-restore fix holds here.

**Follow-on bug A — coherent-control-profile ENABLE SKEW (breaks early bursts):**
ALPHA logs `coherent OFDM control profile ENABLED` at 29.4s; BRAVO not until 61.2s. The
descriptor rides the control profile, so in that window ALPHA encodes it as coherent QPSK
control (`OFDM control profile TX: QPSK R1/4`) while BRAVO decodes control-first as DQPSK
R1/4 → coherent-vs-differential mismatch → the early descriptor (and its burst) fails to
decode. The first burst at 31.5s failed for exactly this reason; a later burst at ~221s
(after both sides coherent-enabled) decoded the header fine. Fix axis: synchronize the
coherent-control-profile enable across stations (tie it to a confirmed handshake state both
peers agree on), or make the control-first decoder try both profiles during the transition.

**Follow-on bug B — assembled group decodes 0/8 with near-erasure LLRs on the live path:**
Even the burst whose header decoded (221s) failed the group decode: all 8 CWs FAIL with
`|llr| mean ~2.0, p50 0.00, p90 ~8` — near-erasure, not the strong LLRs the offline harness
sees (where the same group decodes 70-80/80). So the DATA group demodulated to garbage on
the live coherent/warm-sync/real-time path despite correct header + accumulation. This is a
data-path timing/config issue distinct from the descriptor and from the offline harness;
needs its own diagnosis (candidate: group-frame slicing/CFO/timing under the live warm-sync
path, or the R2/3 downgrade interacting with the burst geometry).

Net: the self-describing-burst CONTROL plane is validated cross-station (header RX works);
the DATA plane on the live GUI path is not yet decoding (bugs A + B). Both are now the
gating items for an actual file delivery.

### 14.25 Burst file transfer WORKS on the live GUI — 0% → 95% (2026-05-27)

Full causal chain fixed (commit set: §14.23 profile-restore, §14.24 descriptor,
bug A, re-anchor stride, per-frame timing). On the file-only Good@20 R3/4 GUI path
(`good20_baseline_sweep.sh "3" <tag> R3/4 0`):
- Clean-fade-phase bursts decode **8/8 (64/64 CWs)** — was **0/8 on every burst** before.
- Fade-dependent burst success ~50–60%; stop-and-wait ARQ resends the rest.
- **~95% of the file delivered** (FILE_CRC_OK=0 only because the last ~5% didn't land
  before the run ended — not a decode failure).

The three coupled fixes (all GUI-proven, committed branch-only):
1. **Bug A — coherent-control-profile enable deadlock**: enable at setConnected()
   (negotiated mode known) instead of handshake_complete_'s "first frame" gate.
   Descriptor RXs ~30s earlier.
2. **Re-anchor stride**: all burst-group MEMBER frames use plain light LTS (no 100ms
   re-anchor chirp), matching the fixed burst_min_block_ accumulation stride.
3. **Per-frame timing retry disabled within a burst**: trust the fixed stride off the
   single group anchor; a faded frame becomes a clean erasure, not mis-sliced garbage.

**Gap to 100% — two levers, both above the (now-correct) PHY:**
a. **Rate margin**: R3/4 is the highest code rate / thinnest FEC; a burst with 3–4
   deeply-faded physical frames exceeds interleaver+R3/4 recovery. R2/3 or R1/2 (or a
   per-burst descriptor-declared rate) carries the deep-fade bursts.
b. **Protocol cruft (§14.20 cleanup, NOT yet done)**: the session layer still runs the
   interactive-era machinery — SR-ARQ/SACK config, adaptive MODE_CHANGE renegotiation
   mid-transfer (observed R3/4->R2/3 on backlog=18), chat-style turn-taking. This wastes
   airtime and silently changes the tested rate, so the run times out before the last
   bursts land. The clean replacement (BurstStopAndWaitController) is built but NOT wired
   into the file path. Wiring it + removing SR-ARQ/SACK + adaptive MODE_CHANGE so the wire
   is only [descriptor][burst] -> [whole-burst ACK] is the next workstream.

### 14.26 §14.20 protocol cleanup — staged migration plan to BurstStopAndWaitController (2026-05-27)

Fallback tag: `burst-1210-working` = commit a27df2b (locked-R3/4 Good@20 file
delivers, FILE_CRC_OK=2, GOODPUT_BPS=1210). Every stage below must keep a working
GUI file transfer; if a stage regresses, revert to the tag.

**Why staged, not one-pass:** `arq_` (SelectiveRepeatARQ) is the load-bearing data
path for BOTH file and messages — transmit cb, data-received cb, sendComplete ->
file_transfer_.onChunkAcked, sendFixedDataWithTypeAndFlags, every isBusy/hasPendingTx
check, ACK + retransmit. ~40+ call sites in connection.cpp (3500 lines). A blind delete
breaks the build AND the 1210 working state and is untestable until fully reconnected.

**Target wire (one-way file path only; messaging/handshake unchanged for now):**
`[descriptor][interleaved burst] -> [single whole-burst ACK]`; resend whole burst on
ACK timeout; no SACK, no per-frame window, no adaptive MODE_CHANGE (already disabled),
no chat turn-taking on the file path.

**BurstStopAndWaitController contract (from burst_transport.hpp):**
- TX: setTransmitGroup -> ModemEngine::transmitBurst(group); onGroupAck(seq) <- decoded
  group-ACK; tick(elapsed_ms) <- session clock; startTransfer(groups).
- RX: onGroupReceived(seq, frames) <- fully-decoded deinterleaved burst;
  setSendGroupAck -> emit ONE group-ACK control frame; setGroupDelivered -> reassemble.

**RIP list (§14.20 throw, file path):** SR-ARQ window + SACK (sendSACK / "SACK timer
expired"), per-frame selective retransmit, the file-chunk -> arq_ fragment path,
adaptive MODE_CHANGE controller (already no-op'd), chat TURNOVER/TURN_REQUEST on the
file path. KEEP: arq_ for MESSAGES + handshake control for now (don't boil the ocean);
the file path is the first consumer of the new controller.

**Staging order (each builds + GUI-tests green before the next):**
1. **RX whole-burst-ACK (contained, do first):** when a burst group fully decodes,
   emit ONE group-ACK control frame (new GroupAck control type, carries group_seq) and
   stop the SACK timer/SACK send for the file-data path. Sender still on arq_ — but it
   now sees a clean single ACK per group. Verify file still delivers; SACK gone from
   the wire (the "short OFDM + half chirp" BRAVO spam disappears).
2. **TX file -> controller:** file send pre-chunks into groups; startTransfer(groups);
   setTransmitGroup -> transmitBurst; onGroupAck <- decoded GroupAck; tick <- clock.
   Bypass arq_ for file DATA (messages still use arq_). Whole-burst resend on timeout.
3. **Delete dead file-path SR-ARQ/SACK** once 1+2 are green: remove the file branches
   from setSendCompleteCallback, the SACK send, transmitFrameBatch repair, etc.
4. **(later) GroupAck without re-anchor chirp** + ACK profile tidy.

Stage 1 is the surgical, immediately-testable cut that also kills the visible SACK
spam. Stages 2-3 are the real controller swap. Do NOT proceed to a stage until the
prior one shows a green GUI file delivery.

### 14.27 Controller swap — exact execution checklist (Stage A done 2026-05-27)

Stage A DONE + committed: FrameType::GROUP_ACK (0x26) + makeGroupAck/getGroupAckSeq,
isControlFrame=true (frame_v2). Additive, builds clean, frame_v2 test green. No behavior
change. Member `burst_transport_` (BurstStopAndWaitController) + `use_burst_transport_=false`
already exist in connection.hpp:450-451 (inert). Types: Frame=Bytes, Group=vector<Bytes>.

Approach: build the controller path ALONGSIDE arq_ behind `use_burst_transport_` (flag OFF
by default → build-safe, zero behavior change), activate the flag for the FILE path only,
GUI-test, THEN delete the dead arq_ file branches. Messages/handshake stay on arq_. Fallback
tag burst-1210-working (a27df2b). Each step builds + (where it changes behavior) GUI-tests
green before the next.

Exact ordered steps:
1. **Wire controller callbacks** in Connection ctor (near arq_ cb setup, ~line 374), inert:
   - setTransmitGroup(seq, frames) -> on_transmit_burst_(frames)  [reuse existing burst TX cb]
   - setSendGroupAck(seq) -> transmitFrame(ControlFrame::makeGroupAck(local_call_, remote_call_, seq).serialize())
   - setGroupDelivered(seq, frames) -> for each frame payload: file_transfer_.processPayload(...) (reassemble)
   - setTransferDone(success) -> file_transfer_ onSendComplete/onSendFailed
   - Config{ack_timeout_ms ~= burst airtime + T/R + ACK; max_retries ~= existing 15}.
2. **tick wiring**: call burst_transport_.tick(elapsed_ms) from the Connection update/tick path
   (same place arq_.update is driven) when use_burst_transport_.
3. **TX activation (file send)**: when starting a file send AND OFDM burst regime, set
   use_burst_transport_=true; pre-chunk the whole file into groups (loop getNextChunk() into
   N=group_size frames per Group) and burst_transport_.startTransfer(groups). Bypass the
   arq_ file-chunk path (sendNextFileChunk / setSendCompleteCallback file branch) when the
   flag is on.
4. **RX**: hook the decoded burst group -> burst_transport_.onGroupReceived(group_seq, frames).
   The decoder already delivers a finalized group (finalizeBurstGroup -> frame_callback_ per
   frame); collect the group's frames and feed onGroupReceived once. Needs a group_seq on the
   wire — carry it in the BURST_HEADER descriptor (add group_seq field; descriptor already
   precedes every burst) so RX knows which group it is for dup-ACK suppression.
5. **GUI-test** with flag ON (file-only Good@20 R3/4): expect [descriptor][burst]->[GROUP_ACK],
   no SACK, no standalone repairs; file delivers CRC-clean, goodput >= 1210.
6. **Delete dead file-path SR-ARQ/SACK** once 3-5 green: file branch of setSendCompleteCallback,
   the SACK send for file data, transmitFrameBatch repair. (Messages keep arq_.)
7. **(later)** drop the re-anchor chirp on GROUP_ACK; group_seq dedupe; N=1944 blocklength.

Resume at step 1. This is the §14 one-way-session rewrite (backlog #143/#147), executed
flag-gated so the 1210 working state is never at risk.

### 14.28 Controller swap — implementation status (2026-05-27, branch feat/16qam-promotion)

Steps 1-2 (callbacks + tick wiring), 3b (TX), 4a (sender GROUP_ACK), 4b (RX group-as-unit)
are IMPLEMENTED + committed, **env-gated `ULTRA_BURST_TRANSPORT=1` (default OFF)**. Build clean
on all targets. The flag-OFF (SR-ARQ 1210) path is byte-for-byte unchanged by construction
(every new branch is gated on `use_burst_transport_` / decoder `burst_transport_rx_`).

Commits: b4291db (descriptor group_seq) → 0ed1860 (callback ripple) → 5afce4c (TX) →
1ed6222 (GROUP_ACK) → a4402c4 (RX). Fallback tag burst-1210-working (a27df2b).

Hard-won implementation specifics (read before touching this path):
- **group_seq on the wire = the BURST_HEADER descriptor FRAME HEADER `seq`** (encoder stamps
  `burst_group_seq_` there via setBurstGroupSeq; NOT a payload field). RX reads it as `hdr.seq`
  at the descriptor intercept (streaming_ofdm_decode.cpp ~728) into `last_burst_group_seq_`.
- **The encoder only interleaves + emits a descriptor for FULL groups** (`encoded_frames.size()
  / BURST_GROUP_SIZE`); descriptor declares `group_size = BURST_GROUP_SIZE` (fixed, =8). So each
  controller Group MUST be exactly BURST_GROUP_SIZE serialized frames — `startBurstFileTransfer`
  pads the final partial group with ULPAD-addressed frames (RX drops them by address filter).
- **finalizeBurstGroup is SHARED with the SR-ARQ burst path.** Group-as-unit emission is gated
  on `burst_transport_rx_`: when set it accumulates the decoded frames + emits one
  BurstGroupCallback(group_seq, frames, all_ok) and SUPPRESSES the per-frame frame_queue_ push
  (else the file group double-processes through onRxData/SR-ARQ). all_ok = (logical_ok ==
  group_size) — a partial group is undecodable → dropped → sender whole-burst-resends (no SACK).
- **Payload extraction**: setGroupDelivered strips each serialized DATA frame header
  (DataFrame::deserialize) and feeds the payload to the SAME reassembly path as SR-ARQ
  (handleDataPayload, MORE_FRAG→more_data). Pads filtered in onBurstGroupReceived by dst addr.
- **Completion accounting**: TX drains ALL chunks up front (chunks_sent_=total, hasMoreChunks
  false); setTransferDone(true) loops onChunkAcked() while hasPendingChunks() to drive
  FileTransferController to COMPLETE (one sent-callback for the whole transfer).

### 14.29 Burst transport GUI-PROVEN end-to-end (2026-05-27)

**Core PROVEN, but MARGINAL on time (2/3 seeds).** The one-way burst file transport
delivers a 10 KB file CRC-clean on the faithful GUI via `[descriptor][burst]→
[GROUP_ACK]` (no SACK, whole-burst stop-and-wait): `ULTRA_BURST_TRANSPORT=1
tools/good20_baseline_sweep.sh "3" tag R3/4 0` (file-only, msg=0). Multi-seed
[1,3,5] R3/4 Good@20: **seed1 PASS 440bps, seed3 PASS 510bps, seed5 FAIL** (file
CRC-clean when it completes, every group decodes 8/8, RETX=0 — but seed5 ran out of
the scenario time budget before finishing). The limiter is NOT decode reliability;
it is the **group-ACK latency** below.

Two real bugs found + fixed via the GUI (both committed):
1. **Responder handshake-confirm bypass** — the burst group-as-unit RX path skipped
   the onFrameReceived handshake confirmation, so the responder's handshake_complete_
   stayed false → its GROUP_ACK went out as MC-DPSK (full preamble), undecodable by
   the OFDM-mode initiator → sender stuck on group 0. Fixed: confirm the responder
   handshake in onBurstGroupReceived before the GROUP_ACK is emitted.
2. **GROUP_ACK timeout collision** — controller's hardcoded 14 s ack_timeout < one
   QPSK R3/4 group burst (~11.3 s) + turnaround → listen window collapsed → sender
   resent before the ACK landed. Fixed: setAckTimeoutMs(arq_.getAckTimeout()) — the
   same burst-aware budget the SR-ARQ window=8 path computes.

**THE LIMITER — group-0 ACK latency (next task, makes it reliable + ~2x goodput):**
group 0's GROUP_ACK takes ~3-4 resend cycles to land (bravo re-decodes group 0 at
43/70/97/124s before the initiator catches the ACK), but groups 1 & 2 are clean (one
cycle, ~12 s each). So steady-state turnaround is healthy; only the FIRST group-ACK
round trip is slow → ~80 s wasted → marginal vs the time budget (seed5 ran out).
Diagnostic clue: during/right after its own ~11 s group-0 TX (31.6→~42.9 s), the
initiator's RX churns low-corr junk syncs ("Data sync detected corr=0.27 SNR=0.9 dB
[BURST-INTERLEAVED]" at 38.6 s — i.e. WHILE still transmitting), and doesn't cleanly
acquire bravo's GROUP_ACK until the link settles by group 1. Root cause is a
half-duplex RX-during/after-TX-tail acquisition issue (initiator RX confused by its
own TX tail / OTASim loopback timing / CONNECT_ACK-rescue transient), NOT the burst
PHY or the ACK timeout. This is subtle half-duplex timing — needs focused analysis,
not a blind tweak. Fixing it converts seed5→PASS and roughly doubles goodput
(440-510 → ~1000+). This is the clear next work item on the branch.

NEXT (this branch): GUI-test flag-ON `ULTRA_BURST_TRANSPORT=1 tools/good20_baseline_sweep.sh "3"`
(seed3 R3/4 Good@20, one msg each way + alpha→bravo file). Expect [descriptor][burst]→[GROUP_ACK],
no SACK/standalone-repair, FILE_CRC_OK, goodput ≥ 1210. WATCH FOR: (a) data-turn fairness
yielding mid-transfer (noteDataTurnPayloadStarted(total) called once — may trip a byte-budget
yield; if so, hold the turn for the whole one-way transfer); (b) GROUP_ACK turnaround timing vs
the 14 s ack_timeout_ms; (c) group_seq wrap/order on multi-group files. THEN step 6 (delete dead
SR-ARQ file branches) only after 3-seed green; step 7 (drop GROUP_ACK re-anchor chirp; N=1944).

### 14.31 SESSION STATE + NEXT STEPS (2026-05-27, branch feat/oneway-arch-2026-05-27)

**Where we are: the one-way burst file transport WORKS end-to-end and is GUI-proven.**
Env-gated `ULTRA_BURST_TRANSPORT=1` (default OFF — SR-ARQ shipping path untouched).
Clean Good@20 R3/4: ~3.1 kbps, file CRC-clean, clean two-sided teardown. Fading
transfers recover CRC-clean via whole-burst ARQ (goodput dips to ~1.3-2.2 kbps when a
group lands in a rough patch — see the limiter below).

Commits this session (all branch-only, NOT pushed): 85ba6ca → d3824f0
1. 85ba6ca disarm CONNECT_ACK rescue on first decoded burst group
2. 80b5b4a disarm CONNECT_ACK rescue on first accepted OFDM data sync (kill pre-decode collision)
3. b4a0cae GROUP_ACK carries full chirp+LTS anchor (robust ACK)
4. be7c168 sender runs full-chirp WIDE acquisition for GROUP_ACK (bypass warm-sync narrow window)
5. 0fbac38 DISCONNECT carries full chirp+LTS anchor (fix responder teardown hang — was orphaning bravo)
6. d3824f0 fast-NACK (GROUP_NACK=0x27): receiver NACKs a 0/8 group → sender resends now (not 27s timeout). 9/9 unit tests.
(Earlier in session: TX/RX group transport, GROUP_ACK, group_seq plumbing, handshake-confirm, burst-budget ack timeout — see §14.28/14.29.)

**THE THROUGHPUT LIMITER (root-caused this session, GUI + waterfall confirmed):**
On Good@20, ~one group per transfer lands in a **frequency-selective multipath null**
and decodes 0/8, then recovers when the null sweeps off. KEY EVIDENCE:
- Bravo waterfall (seed1, failing burst): a BROAD deep null parked over ~900-1500 Hz
  (≈20% of the data carriers dead/dark) while the rest of the band is strong; on the
  recovering burst the null has swept away (just the normal center/DC notch remains).
- During the failure window the LTS/pilot SNR reads **18-20 dB the whole time** (power
  is fine) yet decode is 0/8 — because the un-nulled carriers are strong (SNR meter
  happy) but the nulled ~20% carry near-zero data and R3/4 (~25% parity) can't fill them.
- Bravo fading index rose 0.3 (clean groups) → 0.48-0.59 (failure window); side-panel
  flipped Good→Moderate. Confirmed: **R3/4 is at its fading cusp** — when the channel
  realization dips toward Moderate, R3/4 has no margin. Matches the floor table
  ("R3/4 Good = marginal, seed-dependent"). Failure moves with the seed (seed1 group5,
  seed2 group0) → it's the seeded channel, not a code position.
- Fast-NACK (#6) is mechanically confirmed (halves the resend gap 27s→~13.5s burst time)
  but NEUTRAL on these LONG rough patches: recovery is patch-duration-limited (the null
  must sweep off), not ACK-timing-limited. It will help SHORT fades (< the 27s timeout).

**NEXT STEP (the real lever — FREQUENCY DIVERSITY for Good):** spread each LDPC codeword's
coded bits across the FULL ~1.4 kHz band so a contiguous null only PUNCTURES a fraction
of every codeword (FEC recovers it) instead of WIPING whole codewords/frames → turns a
0/8 group into a clean decode, no resend. Concrete first task: audit the burst carrier
mapping / carrier-LDPC interleaver (descriptor flag 0x02 BURST_FLAG_CARRIER_LDPC is ON)
— verify whether a codeword's bits are actually scattered across the whole band or
landing on contiguous carrier blocks a single notch can wipe. This is the design's
"FREQ interleave for Good" diversity axis ([[project_two_channel_oneway_architecture]]).
(Reproduce: `ULTRA_BURST_TRANSPORT=1 tools/qam16_ladder_scenario.sh --channel good
--snr-db 20 --seed 1 --expect-rate R3/4 --expect-mod QPSK --message-count 0 --file-kb 21
--out /tmp/X`. seed1 fades ~109-163s on group 5; seed2 fades group 0 ~43-97s.)

**Open caveats / honest unknowns:**
- A ~50 s persistently-elevated-selectivity stretch is longer than the ~4-5 s coherence
  time at 0.1 Hz Doppler predicts → possible Watterson "Good" sim-fidelity question about
  how it time-varies selectivity. Verify the channel-gain trace if frequency diversity
  doesn't fully fix it. (Don't assume the sim is right.)
- PRE-EXISTING branch bug (NOT this work): seed3 R3/4 Good@20 with a chat message
  (msg=1) fails identically flag-on/flag-off — the chat message doesn't LDPC-decode on
  bravo, blocking the queued file. Use msg=0 (file-only) to exercise the burst path.
- Multi-seed: only seeds 1 & 2 run at 20-21KB (both PASS, CRC-clean, recover). Broader
  multi-seed confirmation still TODO.
- Reusable: /tmp/overlap_check.py (A↔B TX overlap detector) — fold into tools/ if kept.
- Burst geometry (exact): descriptor 67680 smp=1.41s; data group (8 frames) 542560 smp
  =11.30s; total burst 12.71s; group-to-group cadence ~13.2s (burst + ~0.5s turnaround+ACK).

### 14.32 EXPERIMENT (FAILED, instructive): "one BURST_HEADER per file"

Hypothesis: since file chunks self-address (offset in each FILE_DATA) and the
receiver remembers the format (have_burst_descriptor_/fixed_frame_codewords_ are
sticky), send the BURST_HEADER only on group 0 and drop it on inner bursts to
reclaim ~1.4 s/burst. Implemented opt-in: ULTRA_BURST_HEADER_ONCE=1 (commit on
feat/oneway-arch; default OFF).

**RESULT: FAILED — and it's structural, not tunable.** GUI seed1/21KB: alpha sent
exactly 1 header (gate works), but the sender **stuck on group 0 forever** —
bravo decoded every inner group but reported it as group_seq=0 (43/55/82/109/136/
163 s), re-ACKed seq 0, alpha (waiting seq 1) ignored the stale ACKs, timed out,
resent → infinite loop.

**Root cause: the BURST_HEADER carries the GROUP SEQUENCE NUMBER** (in the
descriptor frame's header seq → decoder's last_burst_group_seq_). It is *different
every burst* and is what the GROUP_ACK matches on. With no header on inner bursts,
last_burst_group_seq_ stays at group 0's value, so every inner group is mislabeled
group 0. The header does THREE jobs, not two: (1) format [constant, once-able],
(2) chirp/sync [every burst — re-acquire after turnaround], (3) **group sequence
[every burst — needed for ACK]**. (3) is the killer.

Corollary: dropping only the *format* saves almost nothing — the header's airtime
is ~all chirp + LTS (the sync), not the 1-CW format payload. So "header diet" is
not a real lever.

**Implication for the roadmap — the real levers are unchanged:**
- **Shorter bursts** (group_size 8 → ~4): fewer groups ⇒ fewer headers *total*,
  burst fits one coherence interval (fresh CSI), cheaper retransmit, easier PA
  duty. Does NOT require touching the per-burst header. Highest-leverage, least
  entangled.
- **Frequency diversity** (scatter each codeword across the full band) so a
  contiguous null punctures instead of wipes (the §14.31 fade limiter).
- (Only if we ever want to drop the per-burst header: the group seq would have to
  ride the data frames' existing seq fields and the receiver derive the group from
  decoded frames — a real redesign, entangled with the fixed-stride accumulation
  / §14.25. Not worth it vs just shortening the burst.)

ULTRA_BURST_HEADER_ONCE left in as default-OFF + documented dead-end (or revert).

---

### §14.33 — Accurate measurement of a failing burst group (the wide-null limiter, quantified) — 2026-05-27

Mined the LLRs going INTO the LDPC decoder for one reproducible failing group
(seed 1, group 5, the 0/8 that ARQ later recovered at 163s) vs. a clean group,
to settle "is R3/4 truly maxed (→ N=1944) or is a diversity tool under-performing?"

| metric (avg over 64 CWs) | failing group (109s) | clean group (163s) |
|---|---|---|
| overall LTS SNR | 20.4 dB | 19.8 dB |
| pilot \|H\| spread across band | 6.2→38.3 (≈16 dB) | flat |
| \|llr\| mean into decoder (clamp=20) | **15.46** | 19.95 |
| \|llr\| p10 (weakest 10%) | **2.11** | 15.20 |
| implied erasure-grade fraction (1−mean/20) | **~23%** | ~0% |
| outcome | all 64 CWs FAIL (unsat 22–59) | all 64 OK (0–1 iters) |

**Conclusions (measured, not assumed):**
1. The **carrier-LDPC frequency interleaver IS active and working** — all 8 frames ×
   8 CWs fail *uniformly* (every CW ≈23% damaged), the signature of the interleaver
   spreading the band-null evenly. Without it you'd see some CWs clean + some wiped.
2. The **anti-poison soft-CSI IS working** — the weakest 10% of LLRs arrive at ≈2
   (honest erasures), NOT at 20-with-wrong-sign (confident-wrong poison). Nulled
   carriers are correctly down-weighted to "don't know."
3. **So the failure is information-theoretic, not a bug:** ~23% of the band is nulled
   and R3/4 has only ~25% redundancy. The interleaver spread the damage perfectly
   evenly → every codeword sits at the cliff edge at once → uniform total failure.
4. **N=1944 is NOT the answer** for this null: same ~25% erasure ceiling. A longer
   code sharpens the waterfall (≈1–2 dB, a few % closer to the limit) but does not
   raise the 25% wall. A 23% null is already at the wall.
5. **Pilots are not a lever in either direction.** Spacing=10 carriers is ≈Nyquist
   for a ~12-carrier null; FEWER pilots under-samples the null → soft-CSI mis-targets →
   poison returns. MORE pilots sharpens null resolution but soft-CSI already resolves
   it adequately (p10≈2). Throughput payoff of fewer pilots is ~3% and lost to worse
   decoding. The wall is the code rate vs the null width, not channel-estimate quality.

The remaining wide-null levers are **partial survival** (interleave-off + per-CW ARQ
to deliver the ~77% un-nulled carriers DURING the notch) or **surgical notch-escape
rate drop** on a stuck group — NOT more diversity (the cheap diversity lever is
already pulled and measured working).

### §14.34 — Burst group size 8→4 A/B: REJECTED (group size is not the lever) — 2026-05-27

Env-gated the burst group size (`ULTRA_BURST_GROUP_FRAMES`, default 8 = shipping;
accessor `connection_policy::burstInterleaveGroupFrames()` routes file-chunk +
padding + encoder-declared group_size; RX self-describes from the descriptor).
Tested group=4 vs group=8, Good@20 R3/4 QPSK 21KB, GUI auto-path, seeds 1–3:

| seed | group=4 | group=8 |
|---|---|---|
| 1 | PASS 1750 bps, 0 retx | PASS 1290 bps (group-5 stuck ~50s) |
| 2 | **FAIL — CRC=0, link-dead at 333s** | PASS 2170 bps |
| 3 | PASS 1740 bps | PASS **2170 bps** |

**Verdict: group=4 loses — keep group=8.** 2/3 pass vs 3/3; lower goodput even on
passing seeds (extra ACK turnarounds cost more on clean stretches than they save).
The seed-1 "+36%" was favorable fade-alignment (a 7s burst threaded between the
deepest null instants on that one seed) — the multi-seed rule caught it.

**Valuable byproduct — a real reliability cliff (latent at group=8 too):** under
whole-burst stop-and-wait, a frequency notch that *recurs* across the ~27s NACK
retry cadence can pin a group, burn the entire `max_retries=15` budget, and **kill
the whole file transfer** ("max retries exceeded", CRC=0). Seed-2 group=4 stuck on
group_seq=0 for 119→309s and died. `max_retries=15 × ~27s ≈ 7 min` hang before death.
This re-confirms §14.33: group size doesn't beat the wide null; it only changes which
seed gets unlucky. Real fixes = partial survival (§14.33 lever 1) or notch-escape
rate drop (lever 2). Two side-bugs to log: GROUP_NACK message hardcodes "0/8" even
when group=4 (cosmetic; grep for a hardcoded-8 smell), and the link-death-after-7min
is bad operator UX (a stuck transfer should degrade/recover, not silently hang).

Apparatus kept (default-8, shipping untouched): env accessor + two stale-test compile
fixes (test_connection_adaptive burst-callback lambdas gained the uint16_t group_seq
param). test_simulator_determinism remains pre-existing-broken (not touched).

---

### §14.35 — Competitive teardown: the leader's HF modem (web research) + why PAPR is a real-radio-only lever — 2026-05-27

Pulled the leader's published spec (OFDM HF modem, 2.4 kHz SSB). Recorded here so
we stop re-deriving it. **Refer to it as "the leader" outside this doc.**

**PHY geometry — nearly identical to ours (so the gap is NOT carrier layout):**
- 52 carriers in 2.4 kHz; 37.5 baud; cyclic prefix 5.33 ms; useful symbol ~21.3 ms
  → carrier spacing **~46.9 Hz**. Ours: 53 data carriers, 41.7 baud, ~46.9 Hz spacing.
  Their CP (5.33 ms) is ~2× ours (2.67 ms) → more delay-spread margin.

**FEC + framing — the real edge #1:**
- **Turbo code** over a DATA frame of **196 OFDM symbols = 5225 ms** (the "5.2 s burst").
  One turbo codeword spanning ~5.2 s × full band = deep time+freq interleave built
  INTO the FEC block, and turbo is ~0.5–1 dB closer to Shannon than our N=648 LDPC.
  Their 5.2 s is SHORTER than our 11 s group yet gets more diversity/sec.

**Adaptation — the real edge #2:**
- **11 net-data-rate levels**, levels 1–8 differential PSK, 9–11 QAM. **Net** range
  **35 → 6782 bps**, adapting continuously to power + link conditions (demo rode
  7k→3k→7k as QRM came and went). Per-frame ARQ (ISS/IRS), not whole-group.

This CONFIRMS our first-principles conclusion: same carriers/band/spacing as us; they
reach ~3 kbps on Multipath-Good via **(a) per-block rate/mod adaptation + (b) turbo over
a long block**, exactly the levers on our fork. Not the OFDM layout.

**PAPR (spec §2.4) — REAL-RADIO-ONLY lever, PARKED for sim work:**
- They pin **9 dB PAPR constant across ALL 11 levels** (DATA), **6 dB for ACK bursts**.
  Two payoffs, both at the PA, not the PHY: (1) low PAPR → more *average* power under a
  fixed *peak* ceiling → far-end SNR → ladder rungs; (2) *constant across levels* → a
  gateway sets ALC/drive ONCE and it's valid at every level (no power swing on adapt).
- **Why it can't help our current (sim) performance work:** OTASim does NOT model a
  peak-limited PA, so lowering PAPR yields ZERO sim throughput. Our floors were even
  measured PAPR-OFF (`waveform_selection.hpp`: PAPR-ON adds "IMD-vs-headroom asymmetry
  on simulator paths"). So PAPR is a genuine real-hardware edge the leader has that is
  **invisible/penalized in our numbers** — a documented simulator-fidelity gap, not a
  current lever. **Decision (user, 2026-05-27): park PAPR as a hardware-phase item.**
- **BUT** it becomes a hard *requirement* once we build the adaptive ladder AND run on
  real hardware: our clipper is fixed-threshold (≠ constant PAPR across QPSK/8PSK/QAM),
  so without constant-PAPR-across-rungs an adaptive ladder would pump a gateway's power
  on every level change. Track alongside the PA-peak-limit OTASim fidelity gap.

---

### §14.36 — Phase 5c: BER-driven per-block rate adaptation (design + build) — 2026-05-27

The #1 lever toward 3000-on-Good (§14.33-35): hold R3/4 when carriers are up, dip a
rung only while the ~20% deep-fade carriers exceed the parity budget, climb straight
back. NOT the old global downgrader — fast/fine/per-block, climbs back in seconds.

**Steering signal — pre-FEC BER, not SNR, not post-FEC BER.**
- SNR lies on frequency-selective channels (failing group was 20.4 dB, §14.33).
- Post-FEC BER (`ldpc_codec.cpp:120`, heuristic-from-iters) is ~binary (0 on success,
  0.5 on fail) — reactive only, no graduation, can't say "room to climb".
- **Pre-FEC BER** (raw hard-decision errors the LDPC had to fix) is graduated +
  predictive: it measures how hard the code worked → whether a higher rate survives.
  Cheap: count sign(received_LLR) != decoded_bit over the codeword after a successful
  decode. Its soft equivalent is the LLR/erasure margin (§14.33). Audit
  (MAJOR_ARCHITECTURE_GAPS, Phase 5c): "SNR is a proxy; BER is the truth."

**Loop (half-duplex forces it):** sender is deaf while transmitting; only the RECEIVER
sees channel quality. So: receiver decodes group → computes a normalized headroom
`quality∈[0,1]` (0=failed) from pre-FEC BER (+ iters) → runs the RateController locally
→ stamps the **recommended rate** on the GROUP_ACK → sender obeys it on the next burst.
We send the *recommended rate* (absolute, ~3 bits), not raw BER: receiver has the
per-CW stats, it's leaner, and an absolute rate self-corrects if one ACK is lost
(a lost "step-down" delta would not). One round-trip stale — fine at 0.1 Hz (fades
last seconds), the bet weakens at higher Doppler.

**RateController (pure state machine, `src/protocol/rate_controller.hpp`, unit-tested):**
- Ladder = ordered supported rates {R1/4, R1/2, R2/3, R3/4} (skips unsupported R1_3 etc).
- **Fast-down:** quality < drop_below → step down one rung immediately, reset climb streak.
- **Slow-up:** quality ≥ climb_above for `climb_streak` consecutive groups → step up one,
  reset. Mid-zone holds and resets the streak (must earn climb with fresh consecutive good).
- Asymmetric because costs are asymmetric: a failed burst loses the whole burst + a retx;
  running one rung slow costs a little rate. Hysteresis gap (drop_below ≪ climb_above)
  kills thrash (prior scar: a damp that still flipped at 3 s — fixed by streak + gap).

**Metric mapping (at the measurement site, v1):** failed group → quality 0.0; success →
quality = 1 − avg_iters/max_iters (iteration headroom, rate-normalized, available today),
later refined to 1 − prefec_ber/correctable_ber(rate). Both measure the same headroom.

**Build order:** (1) RateController + unit tests [THIS STEP]; (2) decoder emits quality
on the burst-group callback; (3) GROUP_ACK gains a rate-hint byte (frame_v2); (4) connection
maps quality→rate via the controller, stamps GROUP_ACK (RX) and applies the hint to the
next burst's encoder rate (TX) — the self-describing BURST_HEADER already re-declares
code_rate, so mid-transfer rate switches need no extra plumbing; (5) flag-gate
ULTRA_ADAPTIVE_RATE=1 (default OFF); (6) GUI multi-seed verify on Good@20 (does the
average sit near 3000 with brief dips, no thrash, CRC-clean).

**Phase 5c GUI verification #1 (2026-05-27) — loop CORRECT, transport BLOCKER found.**
Ran Good@20 R3/4 QPSK 21KB, seed 1, ULTRA_BURST_TRANSPORT=1 ULTRA_ADAPTIVE_RATE=1.
- Loop brain VERIFIED correct: quality measured cleanly (groups 0-4 q≈0.96-0.99 at
  R3/4; group 5 hit the fade -> q=0.00), and the controller stepped exactly as designed
  on successive NACKs: R3/4 -> R2/3 -> R1/2 -> R1/4.
- BUT the transfer FAILED (CRC=0, stuck on group 5 to timeout) — strictly WORSE than
  baseline (which recovered group 5 at 163s at fixed R3/4).
- ROOT CAUSE: groups are PRE-CHUNKED at the start rate. `startBurstFileTransfer` drains
  the whole file into R3/4-sized frames (8 CW × 486 = 486 B/frame) up front. The rate
  drop DID reach the wire (ALPHA encoded the resend "QPSK R1/4", verified in TX
  descriptor log), but an R1/4 frame holds only ~162 B for 8 CW — the 486 B R3/4 payload
  overflows → malformed resend → 0/8 at any SNR → never recovers.
- FIX REQUIRED (next increment): **chunk-at-rate.** The burst transport must hold the
  remaining file payload (raw chunk bytes) and form each group's frames at the CURRENT
  data_code_rate_ at send time, not pre-chunk all groups at the start rate. Then both a
  proactive drop (next group) and a NACK re-rate (re-form the stuck group at the new
  rate) produce correctly-sized frames. Until then Phase 5c is non-functional; flag stays
  default-OFF (shipping path unaffected — the loop is inert unless ULTRA_ADAPTIVE_RATE=1).
- The control loop (RateController + GROUP_ACK quality feedback + sender controller) is
  done and proven; only the transport's chunk-at-rate refactor remains to make it work.
