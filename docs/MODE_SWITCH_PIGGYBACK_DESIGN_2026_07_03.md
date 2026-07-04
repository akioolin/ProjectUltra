# Mode-Switch Piggyback Design — rate/mod moves ride the data plane, not a control exchange

**Date:** 2026-07-03 · **Status:** DESIGN (no code changed) · **Scope:** wideband OFDM_CHIRP
burst-transport rate/modulation moves (the fade-riding ladder). MC-DPSK / OFDM_NARROW /
connect-time negotiation are explicitly OUT of scope (they keep MODE_CHANGE).

**Perspective stack (mandatory, restated):** every claim below is checked as
(1) **PHY theorist** — information actually required to cross the channel, coding/detection
floors, coherence time; (2) **real-time DSP engineer** — state-machine and lifecycle
correctness across the encoder/decoder/ARQ, no mid-decode reconfig hazards;
(3) **veteran HF operator** — half-duplex turnarounds are the scarce resource, dead-air is
the failure the user feels; (4) **physics arbitration** — half-duplex means a dedicated
control exchange costs ≥2 T/R turnarounds; fading troughs make any OFDM-codeword exchange
unreliable exactly when the ladder must move; nothing requires two stations to hold a
synchronized copy of "current mode" if every burst is self-describing.

---

## 0. Executive summary (the decision)

**Commit to Option C — "the anchor IS the announcement" — as the switch mechanism, with
Option A's receiver command as a 2-bit steering upgrade in the same wire footprint
(staged, Phase 2).** Option B (DATA-flag announcement) is REJECTED.

The load-bearing discovery of this study: **the switch-announcement mechanism already
exists and is already trusted.** Every burst group is preceded by a 1-CW `BURST_HEADER`
descriptor that declares `mod / rate / cw_per_frame / group_size / interleave / lifting_z`
(`frame_v2.cpp:450-470`), transmitted at the FIXED control profile (coherent QPSK R1/4,
`streaming_control_profile.hpp:15-23`, applied at `streaming_encoder.cpp:355-362`), and the
receiver's demodulator ALREADY reconfigures itself from it — including a deferred
mod/rate swap (`streaming_ofdm_decode.cpp:729-798`, `streaming_decoder.cpp:931-974`).
The receiver needs **zero advance notice** to demodulate a switched burst. What the
MODE_CHANGE stop-and-wait exchange actually buys today is (a) ARQ seq-grid safety and
(b) receiver-side protocol/ARQ state follow-through. (a) is now structurally provided by
the move-epoch machinery (`selective_repeat_arq.cpp:226-235, 104-177, 1017-1037`;
KNOWN_BUGS BUG-ARQ-SEQ-COLLISION). (b) is a small, local addition (a decoder→Connection
notification). Therefore the entire MODE_CHANGE round-trip — measured at 2.4-4 s per clean
move and **18.5 s-per-retry, 2-9 receptions per switch, ~60-90 s dead-air per rough
transfer in troughs** (rig W4/W5 forensics, `connection.cpp:3378-3423` comments;
CHANGELOG 2026-07-02 §2) — can be deleted from the rate-move path with **zero new on-air
bits** for the mechanism, and 2 previously-padded tone-ACK bits for the steering upgrade.

---

## 1. The problem, quantified

### 1.1 What a rate move costs today

A mid-transfer move runs `requestModeChange()` (`connection_handlers.cpp:799-856`):
1-CW MODE_CHANGE control frame → peer `handleModeChange()` applies + ACKs + schedules
diversity ACK repeats (`connection_handlers.cpp:642-685`, `connection.cpp:3425-3470`) →
initiator matches ACK seq (`connection.cpp:2650-2651`) → `commitPendingModeChange()`
(`connection.cpp:4075-4094`) → `applyDataMode()` (`connection.cpp:3987-4073`). While
pending, `mode_change_pending_` blocks new submits and refills (gate semantics,
CHANGELOG 2026-06-10), so the link is **idle-by-design** for the whole round-trip.

Measured (rig, IONOS MPG@20, campaign_3000 workflows, recorded in the
`modeChangeRetryMs()` provenance comment `connection.cpp:3378-3423`):
- Retry spacing is the **full unified burst deadline** (`arq_.getAckTimeout()`), ~18.5 s
  wideband — the W5/W5b/W6 bisect proved shorter timers livelock (72 MODE_CHANGE
  receptions, moves never committed) because the peer legitimately cannot ACK until its
  decode backlog drains. So the deadline is *correct* — the exchange itself is the problem.
- W4: ~74 s of one 328 s transfer burned waiting on MODE_CHANGE alone (~22%).
- Trough switches (exactly when the ladder MUST move): 2-9 receptions per switch,
  ~60-90 s dead-air per rough transfer.
- Clean-path cost: control round-trip ≈ 2.4-3 s sim / ~4 s rig per move
  (CHANGELOG 2026-07-02 §2), at a fade-riding cadence of ~2 moves per fade cycle.

### 1.2 Why the exchange dies where the tone-ACK survives

MODE_CHANGE and its ACK are 1-CW OFDM control frames at QPSK R1/4
(`streaming_encoder.cpp:355-362`, `streaming_control_profile.hpp:22`): they need a
chirp/LTS anchor acquisition plus an LDPC codeword decode — floor ≈ the OFDM control
floor (~10 dB in-band AWGN class; worse in a moving trough because the anchor and the
codeword must BOTH survive the same null). The tone-burst ACK is a 34-symbol 4-FSK
matched-filter burst with Costas sync and an SNR staircase down to 200 ms symbols —
detection floor ~−2 dB in-band at baseline and lower with longer symbols
(`tone_burst_constants.hpp:35-41, 86-121`). That is an **order 15-20 dB** detection-floor
gap on a fading channel. The control plane for rate moves is therefore built on the
*most fragile* waveform in the system, while a *fade-proof* control channel (the tone
ACK) already flows every burst. One-way loss of the OFDM ACK is also what armed
BUG-ARQ-SEQ-COLLISION (KNOWN_BUGS, 2026-07-03 entry) — now epoch-cured, but the cure's
design purpose was exactly to make regrid transitions safe *without* lockstep.

---

## 2. Inventory of the machinery this design builds on (file:line-verified)

### 2.1 The MODE_CHANGE flow (what would remain / be replaced)

| Piece | Where | Notes |
|---|---|---|
| `requestModeChange(mod, rate, snr, reason)` | `connection_handlers.cpp:799-856` | stores `pending_*`, picks CW via `recommendCWCountForChannel`, sends 1-CW frame; local mode NOT applied until ACK |
| `handleModeChange` (peer) | `connection_handlers.cpp:642-685` | `applyDataMode(info.modulation, info.code_rate, info.data_frame_cw_count, info.ladder_rung_id)` then ACK + `scheduleModeChangeAckRepeats` |
| ACK match → commit | `connection.cpp:2650-2651` → `commitPendingModeChange` `connection.cpp:4075-4094` | |
| Retry tick | `connection.cpp:3058-3090`, budget `MODE_CHANGE_MAX_RETRIES=4` `connection.hpp:683`, timer `modeChangeRetryMs()` `connection.cpp:3378-3423` | keep-current-mode on exhaustion |
| Wire payload | `ModeChangeInfo` `frame_v2.hpp:550-574` | mod, rate, SNR byte, fading byte, reason, CW-count/rung nibble |
| `applyDataMode` | `connection.cpp:3987-4073` | requeue/re-encode pending chunks on mod/rate/CW change, HARQ flush, `configureArqForCurrentDataMode` (`connection.cpp:3508+`), rung telemetry |
| Clean-boundary gate | `connection.cpp:2313-2351` (QPSK ladder), `:2184-2242` (QAM16 demote/climb) | issue only when `!hasPendingChunks() && getTxInFlightBytes()==0`; hold + re-assert otherwise |
| Escape paths (bypass gate BY DESIGN) | `executeEscapeDrop` `connection.cpp:1894-1930`, `maybeEscapeStuckFrame` `:1932-1949`, `maybeCollapseEscape` `:1958-1985` | fire with frames in flight; still route through `requestModeChange` today |
| Decision plumbing | receiver measures group quality `onBurstGroupReceived` `connection.cpp:2380-2402` → 3-bit `rate_hint` in tone-ACK (`connection.cpp:262-267`) → sender de-quantizes and feeds `RateController` (`connection.cpp:1709-1717`, `applyAdaptiveRateFeedback` `:2247+`) | sender-side EMA + ssthresh + QAM16 climb/demote; default-ON for OFDM_CHIRP (`rateAdaptationActive` `connection.cpp:1880-1888`) |

**Stale-doc flag found during this study:** `tone_burst_payload.hpp:39` documents
`rate_hint` as a rate ENCODING ("0=R1/4 … 7=hold") but the implementation transmits a
quantized decode-headroom quality (`connection.cpp:264-267`) and consumes it as quality
(`connection.cpp:1713-1717`). The comment is a claim, not the code. Fix the comment
whenever the field is next touched (Phase 2 below re-specifies it precisely).

### 2.2 Move-epoch (the seq-safety substrate — its design purpose was THIS)

- TX epoch bump exactly on `setCodeRate`'s abort/rewind (`selective_repeat_arq.cpp:226-235`;
  the rewind itself `:218-224`); `EPOCH_REBASE` stamped on frames created at the window
  base (`:62-73`; flag = repurposed 0x08, `frame_v2.hpp:277-296`).
- Receiver adoption + rebase-anchored re-anchor + ACK-silent unanchored interregnum with
  FILE salvage (`selective_repeat_arq.cpp:104-177`).
- Sender ignores stale-era ACKs (`:1017-1037`); tone-ACK echoes the epoch in bits 40-41
  (`tone_burst_constants.hpp:165-178`).
- Status: knob-gated `ULTRA_ARQ_MOVE_EPOCH`, default-OFF, **rig-validation pending**
  (KNOWN_BUGS). This design DEPENDS on it for mid-window (escape) switches — see §7.

### 2.3 Tone-burst ACK payload budget

Layout (`tone_burst_constants.hpp:152-235`): 42 bits used of 44 Hamming info capacity
(4×(15,11) blocks → 60 coded bits → 30 payload symbols + 4 Costas = 34 symbols, airtime
unchanged since the 2026-07-02 mask widen; `:243-265, 280-283`):

| bits | field | consumer |
|---|---|---|
| 0-5 | group_seq | ARQ ack reconstruction |
| 6-21 | frame_mask (16) | SACK window (`kToneBurstAckWindowCapFrames=16`, `connection_policy.hpp:84`) |
| 22-24 | rate_hint (3) | sender RateController quality feed |
| 25 | type ACK/NACK | |
| 26-37 | crc12 (covers 0-25 + 38-39) | |
| 38-39 | drive_advisory (2) | software-ALC, deduped by group_seq (`connection.cpp:1718-1731`) |
| 40-41 | move_epoch (2) | ARQ era gate; NOT CRC-covered (byte-identity rationale `:170-178`) |
| **42-43** | **ZERO-PAD — the 2 spare bits** | transmitted anyway (block 4 pad, `:243-249`) |

The sender's monitor scans ALL symbol durations {12, 25, 50, 100} ms per pass
(`tone_burst_ack_monitor.hpp:54-59`), so no pre-agreement on the staircase is needed —
a property this design inherits for free.

### 2.4 The burst descriptor + RX self-configuration (Option C's existing 90%)

- TX: every burst group is prefixed by `makeBurstHeader(seq=group_seq, group_size,
  cw_per_frame, mod, rate, interleave_flags, lifting_z)` (`streaming_encoder.cpp:594-716`,
  `frame_v2.cpp:450-470`), full/short/light anchor per warm-streak policy
  (`:606-695`); unified path sizes group = burst (`modem_engine.cpp:506-532`).
- RX: descriptor decoded at the fixed control profile, then **configures the receiver
  from the SENDER's declaration**: group size, CW/frame, interleave flags, lifting-Z,
  next-anchor type, and — critically — a **deferred mod/rate reconfigure**
  (`streaming_ofdm_decode.cpp:735-798`; `pending_descriptor_*` applied at the top of the
  next `processBuffer` via `applyPendingDescriptorDataMode()`,
  `streaming_decoder.cpp:931-974`, crash-fix rationale `:912-929`). §14.36 Phase 5c
  comment (`streaming_ofdm_decode.cpp:746-752`): *"the descriptor's declared rate is
  authoritative."*
- The descriptor IS `isControlFrame` (`frame_v2.hpp:313-324`) → always control-profile
  QPSK R1/4, 1 CW — decodable **independent of the data mode on either side.**

### 2.5 DATA-frame flags budget (Option B's raw material)

`Flags` byte (`frame_v2.hpp:267-296`): 0x01 VERSION_V2, 0x02 URGENT/TURN_REQUEST,
0x04 COMPRESSED, 0x08 EPOCH_REBASE (ex-ENCRYPTED, DATA-only, knob-ON), 0x10 MORE_FRAG,
0x20 FINAL, 0xC0 move-epoch (bits 6-7, ex rate-in-flags reserve). **Inventory: the flags
byte is FULL.** The DATA header (`frame_v2.cpp:812-864`) has no reserved bytes; adding a
rate-index field means header surgery = wire-breaking for every frame, forever.

### 2.6 The 2026-06-09 lesson (why naive unilateral switching failed once already)

CHANGELOG 2026-06-09/06-10/06-11: the first unilateral sender flip (announced via this
same BURST_HEADER descriptor) failed on THREE independent arms:
1. **Policy churn** — single-NACK rate drops ratcheted to R1/4. FIXED since: EMA
   controller + ssthresh ceiling + climb streaks (CHANGELOG 2026-06-09/06-11), all
   GUI-proven and default-ON since 2026-07-02.
2. **ARQ seq renumber** — mid-stream regrid deadlocked the in-order receiver; the
   gate-less "abort-coordinated requeue" was built and REJECTED (CHANGELOG 2026-06-11).
   FIXED since (structurally): move-epoch (§2.2) — bump/stamp/adopt/rebase makes a regrid
   a recognized new era instead of a silent collision.
3. **Pilot/carrier geometry desync** — QPSK R3/4 = 51 data/8 pilots vs R2/3 = 47/12; the
   receiver's stale warm-sync produced |H| garbage → 0/8 CWs forever. This is a REAL PHY
   hazard for any descriptor-driven switch and gets a mandatory mitigation in §6.4:
   **a mode-hop group always carries a full chirp+LTS anchor (sender) and demotes
   warm-handoff (receiver)** — the fresh LTS re-derives |H| under the new geometry.
   Cost already priced in: today's ladder pays `kWideOFDMFullAnchorExtraMs` ≈ 1200 ms
   per move anyway (`connection_policy.hpp:28-29`, CHANGELOG 2026-07-02 §2).

So the historical failure decomposes exactly into parts that have each been fixed or
are priced: the *conclusion* "unilateral = broken, handshake = mandatory" is stale.

---

## 3. Physics framing: what MUST cross the channel for a mode switch?

1. **The demodulator's per-burst configuration.** Already crosses, per burst, at the
   control profile (descriptor, §2.4). Nothing additional required.
2. **ARQ era safety when the seq grid changes.** Crosses in DATA flags bits 6-7 +
   EPOCH_REBASE + tone-ACK echo (§2.2). Nothing additional required.
3. **The channel measurement that drives the decision.** Crosses receiver→sender in
   rate_hint / NACK / drive_advisory (§2.1, §2.3). Sufficient today; upgradable (§5).
4. **Receiver protocol-level follow-through** (its ARQ window/capacity bookkeeping and
   GUI state). Does NOT need to cross the channel at all — it is derivable at the
   receiver from the descriptor it already decoded. It only fails to happen today
   because nobody wired the notification (§6.2).

Conclusion: a synchronized two-copy mode state machine — the thing MODE_CHANGE's
stop-and-wait exists to maintain — is **not physically required**. The only state the two
stations must ever agree on per burst is carried by the burst itself. This is the
information-theoretic core of the design: *replace agreement-in-advance with
self-description-per-burst.*

---

## 4. The three shapes, analyzed

### 4.1 Option A — receiver-commanded switch via tone-ACK bits

The receiver (which measures the channel: group quality `connection.cpp:2380-2402`, LTS
coherence disc, per-carrier SNR) commands the next rung; the sender applies it at the
next burst boundary, epoch-stamping if a regrid occurs.

- **Bits:** absolute rung index needs ≥5 bits (mod×rate family + reserve): 2 spare +
  re-purposed rate_hint's 3. Relative step command needs 2 bits (see §5.2): fits the
  spare pad alone, rate_hint stays intact.
- **Airtime delta:** 0 (bits 42-43 are transmitted today as zero-pad; 4 Hamming blocks,
  34 symbols unchanged — same trick as move_epoch, `tone_burst_constants.hpp:243-249`).
- **Arbitration when sender disagrees:** the sender remains final authority over what it
  transmits — it clamps any command through its validated caps
  (`maxValidatedCoherentRate` `connection.cpp:2266-2269`, ssthresh, `ULTRA_MAX_OFDM_RATE`
  `:2250-2259`) and its escape-drops (`:1894-1985`) still fire on zero-ACK evidence the
  receiver cannot see (when the receiver is deaf it sends nothing, so there IS no
  competing command — the disagreement case is structurally narrow). Rule: **most-robust
  verdict wins within one boundary; climbs require receiver consent, drops don't.**
  This matches physics: the receiver is authoritative about what it can decode; the
  sender is authoritative about what it will send.
- **Race with in-flight bursts:** commands arrive only inside a group ACK = a turn
  boundary; dedup by `group_seq` exactly like `drive_advisory` (`connection.cpp:1718-1731`)
  makes re-emitted ACK copies idempotent.
- **The QAM16 mod-hop:** a bit-field alone is NOT enough to key `applyDataMode` on the
  receiver — but it doesn't have to be: under Option C's mechanism the receiver's
  waveform reconfig is keyed by the next descriptor, which carries the full
  mod/rate/CW/Z tuple. The command bits only steer the SENDER's decision.
- **Backward compat:** zero-pad bits are 0 on old builds = "no command" — same lockstep
  semantics class as move_epoch (no version field on the tone payload,
  `tone_burst_constants.hpp:180-190`).
- **What remains of MODE_CHANGE:** unchanged connect-time/initial negotiation (already
  mostly folded into CONNECT_ACK, `connection_handlers.cpp:498`), USER_REQUEST manual
  moves, MC-DPSK/OFDM_NARROW rungs (rung-id + carriers/sps ride `ModeChangeInfo`,
  `connection.cpp:3989-4002`), and the deaf-peer escalation fallback (§6.5).
- **Weakness if used ALONE** (i.e., with the old commit machinery): the command still
  needs an acknowledged application — you re-invent the round-trip unless the switch
  mechanism itself is announcement-free. Option A is a *steering* channel, not a
  *commit* mechanism. It is complementary to C, not an alternative.
- **Protection analysis for command bits:** outside the CRC (can't extend coverage
  without breaking knob-OFF byte-identity — the move_epoch precedent `:170-178`),
  Hamming-corrected only. A 2-bit-error miscorrect could forge a step command. Fails
  soft under Option C: a forged step changes the SENDER's rung by one; the next burst
  self-describes; the receiver's next real ACK re-steers. Bounded cost ≈ one group at a
  suboptimal rung (~8.4 s airtime), no deadlock, no desync. Acceptable; also mitigable
  by requiring the same command in 2 consecutive ACKs for CLIMBS (drops apply
  immediately — safety-asymmetric, like the QAM16 demote).

### 4.2 Option B — optimistic sender switch announced in DATA-frame flag bits

- **Bits:** the flags byte is FULL (§2.5). A rate index (≥4-5 bits incl. modulation)
  requires a new header byte → wire-breaking for every DATA frame forever, or an
  in-payload prefix → capacity/geometry churn. Strictly worse than the descriptor,
  which already exists and already carries the full tuple at 0 marginal bits.
- **Layer inversion (the disqualifying flaw):** the announcement rides the very frames
  that are modulated in the NEW mode — the receiver can only read the flag AFTER it has
  already successfully demodulated the frame, i.e. after the information was needed.
  To be readable in advance it must ride the *previous* burst's frames — which die in
  precisely the trough that triggers the switch (the frames' floor is the DATA floor,
  the worst of all available channels). Physics says: announcements must ride a channel
  MORE robust than the thing announced. DATA flags are the least robust channel we have;
  the descriptor (control profile) and the tone-ACK are both categorically better.
- **Fallback complexity:** "old control exchange when the receiver NACKs comprehension"
  re-introduces the full MODE_CHANGE machinery as a live path, doubling the state space
  (two commit mechanisms racing) for negative gain.
- **Verdict: REJECTED.** Epoch bits landed in flags because 2 bits sufficed and ACK-echo
  semantics fit; a mode announcement does not fit and does not belong at this layer.

### 4.3 Option C — the anchor IS the announcement (descriptor-committed switch)

The sender switches unilaterally at a burst boundary; the next group's
descriptor+anchor tells the demodulator; the ARQ epoch handles seq safety; no advance
notice, no handshake, no announcement bits at all.

- **Bits:** **0.** The descriptor already encodes group_size(8) cw_per_frame(8) mod(8)
  rate(8) flags(8) lifting_z(8) (`frame_v2.cpp:461-468`) — a superset of
  `ModeChangeInfo`'s mode-relevant content (SNR/fading/reason bytes are telemetry, not
  mechanism; rung-id is MC-DPSK-only).
- **Airtime delta per move:** +0 new. The mode-hop full anchor (~1200 ms,
  `connection_policy.hpp:28-29`) is already paid by today's ladder per move. REMOVED:
  the MODE_CHANGE+ACK exchange (2.4-4 s clean; 18.5 s × 2-9 receptions in troughs).
  Net: **−1.2 to −2.8 s per clean move; −20 to −90 s per rough transfer;** and the
  `mode_change_pending_` TX freeze disappears (the sender keeps sending at the OLD rung
  until the boundary, then switches — no idle round-trip at all).
- **"Receiver missing the switch"** — the central failure question — **is structurally
  impossible to strand on:** there is no per-connection mode agreement to miss. The only
  loss mode is losing the descriptor itself, which equals losing the whole group (the
  group's frames are undecodable without it *at any rate* — `pending_total_cw_` sizing,
  `streaming_ofdm_decode.cpp:773-785, 1052-1072`) — an EXISTING, handled event: no ACK →
  sender RTO → group resend with (timeout-batch) full anchor
  (`force_burst_group_start_full_preamble_`, `streaming_encoder.cpp:722-728`;
  RTO-resend re-anchor behavior per CHANGELOG dcfbe00). Recovery cost = one ACK-timeout
  round, identical to any lost group today, at ANY rung — vs MODE_CHANGE's 18.5 s
  per-retry × up to 4 retries + keep-current-mode ambiguity (`connection.cpp:3062-3072`,
  which leaves the two ends having to re-converge via yet another exchange).
- **What C does NOT provide by itself:** receiver protocol-level follow-through (§6.2)
  and the steering upgrade (that's A). Both are additive.

### 4.4 Comparison table

| | A (rx-commanded, alone) | B (DATA-flag announce) | C (descriptor-committed) |
|---|---|---|---|
| New on-air bits | 2-5 (tone-ACK pad) | ≥4-5, no room — header surgery | **0** |
| Airtime per move | still needs a commit mechanism | 0 nominal, but see flaw | −2.4 to −4 s clean, −20..90 s trough |
| Announcement channel robustness | 4-FSK tone (best) | DATA frames (worst — layer inversion) | control-profile QPSK R1/4 1-CW (good) + tone-ACK epoch echo |
| Receiver misses switch | n/a (steering only) | demod mismatch on whole burst, needs NACK fallback = 2 mechanisms | nothing to miss; descriptor loss = ordinary group loss |
| Seq safety | epoch (needed) | epoch (needed) | epoch (needed) |
| QAM16 mod-hop | can't key RX reconfig alone | same problem | descriptor keys full `waveform_->configure(mod,rate)` (`streaming_decoder.cpp:951-974`) |
| Implementation delta | controller migration if pure | header surgery + dual commit paths | small: delete-a-path + one notification + anchor rule |
| Verdict | **adopt as Phase-2 steering** | **reject** | **adopt as the mechanism** |

---

## 5. The chosen design: descriptor-committed switch, receiver-steered

### 5.1 Phase 1 — the mechanism (knob `ULTRA_DESCRIPTOR_MODE_SWITCH`, default OFF)

Decision-making is UNTOUCHED (sender-side EMA RateController + ssthresh + QAM16
climb/demote + clean-boundary gate + escape drops — all GUI/rig-proven, default-ON).
Only the COMMIT changes: wherever the ladder today calls
`requestModeChange(mod, rate, …)` for a **wideband-OFDM rate/mod move**
(`connection.cpp:2193, 2203, 2229, 2345` and `executeEscapeDrop` `:1909, 1928`), the
knob-ON path instead calls a new `commitLocalModeSwitch(mod, rate, reason)`:

1. `applyDataMode(mod, rate, /*cw=*/pick, rung)` immediately (same CW pick as today,
   `connection_handlers.cpp:838-844`). No `mode_change_pending_`, no retry timer, no TX
   freeze. At a clean boundary `requeuePendingChunks()` is a no-op (nothing in flight);
   on the escape path the ARQ abort fires → **move-epoch bumps** (`selective_repeat_arq.cpp:231-235`)
   → regrid is era-safe.
2. Arm the one-shot **full-anchor rule**: the next burst group MUST carry full chirp+LTS
   (set `force_burst_group_start_full_preamble_` via the existing hook,
   `streaming_encoder.hpp:103-110`) and reset the anchor-skip clean streak
   (`streaming_encoder.cpp:640`). This is the §2.6-arm-3 mitigation.
3. Transmit the next burst normally: its descriptor (already stamped with the new
   mod/rate/cw/z by `transmitBurst` → `setDataMode`, `modem_engine.cpp:506-532`) IS the
   announcement.

Receiver (knob-ON):
4. The existing descriptor intercept reconfigures the demod (already live,
   `streaming_ofdm_decode.cpp:735-798`). NEW: when the descriptor's mod/rate differs
   from the current one, (a) treat warm-handoff as ineligible for the data group that
   follows (force the reset path at `streaming_ofdm_decode.cpp:931-940` — expect the
   full anchor the sender is now guaranteed to send; kills the stale-|H| arm), and
   (b) fire a NEW decoder→engine→Connection callback `onDescriptorModeChange(mod, rate,
   cw_per_frame)` so the receiver's protocol layer follows: run the RX-relevant subset
   of `applyDataMode` (set `data_modulation_/data_code_rate_/data_frame_cw_count_`,
   `configureArqForCurrentDataMode()` — window/timers/chunk capacity — and
   `notifyDataModeChanged` for the GUI, `connection.cpp:3921-3928`, `app.cpp:955-968`).
   Window-size agreement matters because `ofdmWindowSize` is mod/rate-dependent
   (`connection_policy.hpp:926-951`); without the callback a QPSK(16)-window receiver
   would below-window-drop the tail of a 16-frame burst after a hop to a rung it thinks
   has window 8. At a clean-boundary switch the RX ARQ slots are provably empty (the
   sender's drained window ⟹ receiver delivered in-order), so `arq_.setCodeRate`'s RX
   discard (`selective_repeat_arq.cpp:243-268`) is a no-op; on an escape switch the
   epoch adoption already performs the discard (`:99-101`).
5. The tone-ACK for the switched group closes the loop: its epoch echo confirms era
   (mid-window case), its frame_mask/quality steer the next decision. **Confirmation is
   implicit and free.**

What remains of MODE_CHANGE (all knob-states): connect-time INITIAL_SETUP rescue,
USER_REQUEST, MC-DPSK rung moves (carriers/sps need `ladder_rung_id`,
`connection.cpp:3989-4002`), OFDM_NARROW, and the §6.5 deaf-peer fallback. The frame
type, handler, and retry machinery stay in the tree; only the wideband-OFDM ladder stops
using them. (Log a REMOVAL_BACKLOG candidate only after the fallback proves unneeded.)

### 5.2 Phase 2 — receiver steering (knob `ULTRA_RX_RATE_CMD`, default OFF; needs Phase 1)

Re-purpose tone-ACK pad bits 42-43 as a 2-bit **relative rung command**, deduped by
`group_seq` (the proven drive_advisory pattern):

| value | meaning | sender action (clamped through caps/ssthresh/cooldowns) |
|---|---|---|
| 0 | no command (back-compat zero) | EMA feedback via rate_hint as today |
| 1 | STEP-UP | one rung up the sender's own ladder table (incl. QPSK R3/4 → QAM16 R2/3 hop) at next clean boundary |
| 2 | STEP-DOWN | one rung down, immediately at next boundary (drop-safety asymmetric) |
| 3 | DROP-TO-FLOOR | trough panic: straight to the rung floor (QPSK R1/4 class) |

`rate_hint` keeps its (actual) quality semantics — the sender EMA remains the
belt-and-braces decision-maker and the sole authority when commands are absent (deaf
receiver ⇒ no ACK ⇒ escape-drop path, unchanged). Arbitration rule (§4.1): most-robust
verdict wins within a boundary; climbs additionally require 2 consecutive identical
STEP-UP commands (miscorrect guard, §4.1). Wire: NOT CRC-covered (byte-identity
precedent, `tone_burst_constants.hpp:170-178`), Hamming-protected, fails-soft under the
Phase-1 mechanism. Airtime: unchanged (44-bit block capacity, 34 symbols).

Rationale for staging: Phase 1 already removes the measured dead-air (the exchange);
Phase 2 improves decision LATENCY/fidelity (receiver sees the trough one group earlier
than the sender's quantized echo, and its coherence disc lives RX-side). Don't move two
variables in one validation campaign; the gate fade-noise is ±25% (MEMORY: 16QAM audit).

### 5.3 State machine (sender, knob-ON)

```
            quality/cmd feedback (every tone-ACK)
                     │
   ┌────────► STEADY(rung R) ── controller decides R→R' ──► SWITCH-ARMED(R')
   │                │                                            │ window busy: hold,
   │           escape/collapse trigger                           │ re-assert on later ack
   │                │                                            ▼ window clean
   │                ▼                                    COMMIT: applyDataMode(R'),
   │        ESCAPE-COMMIT: ARQ abort → epoch++            arm full-anchor one-shot
   │        applyDataMode(R'), full-anchor one-shot              │
   │                │                                            │
   │                └────────────► TX next burst: descriptor(R') + full chirp+LTS
   │                                        │
   │        ACK(epoch match) ───────────────┘
   └────────────── (implicit confirmation; no distinct WAIT state, TX never idles)
```
Receiver has NO mode-switch states at all: per-burst `descriptor → reconfigure demod +
notify Connection`, plus the existing epoch adopt/interregnum for the escape case.

---

## 6. Failure-mode table

| # | Failure | Behavior under the chosen design | Cost / recovery |
|---|---|---|---|
| 1 | Descriptor (=switch announcement) lost in trough | Whole group undecodable (existing semantics); no ACK → RTO → group resent WITH full anchor + descriptor | one RTO round; identical to any lost group today; no mode ambiguity is possible |
| 2 | Descriptor decoded, all data frames lost | RX already reconfigured + Connection notified; NACK/hole ACK returns; sender resends at same rung (or drops via controller) | normal ARQ; reconfig is idempotent (`applyPendingDescriptorDataMode` no-ops on match, `streaming_decoder.cpp:941-943`) |
| 3 | Tone-ACK after switched group lost | Sender RTO-resends group (old behavior); receiver dedups by seq/epoch | unchanged from today |
| 4 | Escape switch mid-window, one-way ACK loss (the W16 killer) | epoch bump + EPOCH_REBASE + stale-epoch ACK gate + interregnum salvage | the move-epoch design case; no phantom retire, no hole (KNOWN_BUGS entry) |
| 5 | Epoch mod-4 wrap (4 aborts, zero decodes between) | documented move-epoch residual; falls back to below-window salvage | accepted residual, unchanged |
| 6 | QAM16 craters so hard NO ack is emitted | escape-drop (`connection.cpp:1894-1911`) commits QPSK R3/4 locally; next descriptor rides control-profile QPSK R1/4 → decodable in the trough where a 16QAM-era MODE_CHANGE wait used to grind 18.5 s retries | this is the design's best case: recovery = 1 group |
| 7 | Channel below even control-profile floor | No OFDM scheme works (descriptor ≈ the old MODE_CHANGE's floor); after `kMissedAcksBeforeEscalation=3` lost ACK windows (`tone_burst_constants.hpp:292`) fall back to legacy MODE_CHANGE toward MC-DPSK / re-negotiation | same terminal behavior as today; not a regression |
| 8 | Phase-2 command bits Hamming-miscorrected | forged ±1 step, clamped by caps; self-heals next ACK; climbs need 2 consecutive commands | ≤1 group at wrong rung (~8.4 s airtime) |
| 9 | Stale warm-sync |H| across geometry change (the 06-09 arm 3) | prevented by construction: mode-hop group forces full anchor (sender) + warm-handoff demotion (RX) | +~1.2 s per move — already paid today |
| 10 | Mixed-version / knob-mismatch peers | knob-OFF is byte-identical (no new bits set; ladder uses MODE_CHANGE); knob-ON is SEMANTICS-BREAKING lockstep, same policy as move-epoch/tone-payload (no version field; `tone_burst_constants.hpp:180-190`) | operational rule, documented; capability negotiation deferred (same increment policy as move-epoch) |
| 11 | RX Connection notification races the group decode | notification fires at descriptor consume, config applies at `processBuffer` top (existing deferred pattern, crash-fix provenance `streaming_decoder.cpp:912-929`); ARQ reconfig runs on the protocol thread as `handleModeChange` does today | no new concurrency class |
| 12 | Receiver-ISS asymmetry (receiver later takes DATA turn) | its own TX rung is its own controller's business (per-direction rungs are already independent — MODE_CHANGE was per-direction too); descriptor self-describes each direction | none |

---

## 7. Dependencies, knobs, A/B plan

**Hard dependency:** `ULTRA_ARQ_MOVE_EPOCH` must be rig-validated and ON before
`ULTRA_DESCRIPTOR_MODE_SWITCH` can enable the ESCAPE (mid-window) commit path. A staged
intermediate is possible (clean-boundary-only descriptor switching needs no epoch), but
the escape path is where the trough dead-air lives, so validate epoch first — it is
already implemented and pending validation (KNOWN_BUGS). `ULTRA_BELOW_WINDOW_FILE_SALVAGE`
stays ON (belt-and-braces).

**Knobs:** `ULTRA_DESCRIPTOR_MODE_SWITCH` (0=off, byte-identical; 1=Phase 1),
`ULTRA_RX_RATE_CMD` (Phase 2, requires Phase 1 both ends). Both read-once, lockstep-ON.

**A/B plan (paired seeds, structural metrics only — ±25% gate noise):**
1. **Unit:** commit-path tests (gate held vs escape epoch-bump ordering); descriptor→
   Connection notification (window/capacity follow); warm-handoff demotion on mode-hop
   descriptor; Phase-2 command dedup/clamp/2-consecutive-climb.
2. **Faithful gate** (`tools/gui_qso_scenario.sh`, ULTRA_LOCK_RATE=0): Good@20 seeds
   {42,43,7,2,99} × 50 KB, Moderate@18 × 3 seeds, paired knob-OFF/ON. PASS criteria:
   (a) MODE_CHANGE frames on rate moves = 0 (knob-ON); (b) per-move dead-air (last data
   before decision → first data at new rung) median < 2 s vs baseline 4-20 s;
   (c) CRC-clean 100%, goodput ≥ baseline; (d) 0 `stale-epoch` permanent stalls;
   (e) ladder telemetry `[LADDER] moves` count comparable (no churn regression).
3. **Rig campaign** (IONOS MPG@20, both ends knob-ON, W-series protocol): reproduce the
   W4 measurement — MODE_CHANGE receptions per switch (target: 0), transfer-time share
   of switch overhead (baseline ~22% worst case), plus the W16 one-way-loss soak for the
   escape path.
4. **Trough targeting:** seeds/dial known to force 16QAM craters (g43 class) — verify
   failure-mode #6 recovery ≤ 1 group vs baseline's multi-retry grind.

---

## 8. One-session implementation checklist (Phase 1)

1. `connection.cpp`: add `commitLocalModeSwitch(mod, rate, reason)` (≈ `commitPendingModeChange`
   body minus pending bookkeeping: `applyDataMode` + `notifyDataModeChanged` + one-shot
   full-anchor request via the existing `on_data_turn_acquired_`-style encoder hook);
   route the 6 wideband-ladder `requestModeChange` call sites (`:2193, :2203, :2229,
   :2345, :1909, :1928`) through the knob switch. Keep `requestModeChange` for the
   residual uses (§5.1).
2. Plumb a `forceNextBurstGroupFullPreamble()` path Connection→ModemEngine→
   StreamingEncoder (`streaming_encoder.hpp:103-110` hook exists; today only the resend
   path sets it) + reset `anchor_skip_clean_streak_`.
3. `streaming_ofdm_decode.cpp` descriptor intercept (`:753-772`): when mod/rate differs,
   set a `descriptor_mode_hop` flag → (a) exclude warm-handoff for the following group
   (`:915-918` condition), (b) invoke new `on_descriptor_mode_change_(mod, rate,
   cw_per_frame)` callback (register through StreamingDecoder → ModemEngine → app →
   `protocol_.onDescriptorModeChange(...)`, mirroring `setDataModeChangedCallback`
   wiring at `app.cpp:955-968`).
4. `connection.cpp`: `onDescriptorModeChange` = RX-side `applyDataMode(mod, rate, cw)`
   guarded to knob-ON + CONNECTED + wideband-OFDM + not-ISS-frame-in-flight; verify the
   `arq_.setCodeRate` RX-discard no-op invariant (assert/log if slots non-empty without
   an epoch adoption).
5. Telemetry: extend `[LADDER]` per-move logging with commit kind
   (boundary/escape/legacy-MODE_CHANGE) + per-move dead-air ms; add
   `stats_.descriptor_mode_switches`.
6. Tests per §7.1; docs: CHANGELOG entry, KNOWN_BUGS cross-ref (BUG-ARQ-SEQ-COLLISION
   dependency), MODEM_INFRASTRUCTURE_MAP §6 knob rows (both knobs) + §7 register,
   fix the stale `tone_burst_payload.hpp:39` rate_hint comment while touching the file.
7. Gate: `cmake --build build -j4 && ctest …` + §7.2 paired faithful-gate sweep before
   any default flip; knob stays OFF until the rig campaign (§7.3) passes.

Phase 2 (separate session): `tone_burst_constants.hpp` bits 42-43 field + pack/unpack +
clamp tests (payload 42→44 bits, still 4 blocks/34 symbols — assert airtime invariance),
receiver command emission from its controller verdict, sender consume+dedup+clamp.

---

## 9. Bottom line

MODE_CHANGE solved a coordination problem the system no longer has: the burst descriptor
already tells the demodulator everything per burst on a channel that out-survives the
data, the move-epoch already makes seq regrids safe without agreement, and the tone-ACK
already carries the measurement (and has 2 free bits to carry a command). The remaining
work is deleting a round-trip, wiring one receiver-side notification, and enforcing the
full-anchor rule the ladder already pays for — turning every rate move from a
stop-and-wait exchange (2.4 s clean, 60-90 s in troughs) into **zero extra key-downs**.
