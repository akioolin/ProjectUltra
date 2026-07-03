# Retransmit Pacing + Collapse-Conditioned Escape — Design (2026-07-03)

**Status: DESIGN ONLY — no code changed.** This is the campaign's next lever after the wide
coherent window (CHANGELOG 2026-07-03: "the principled fix for collapse eras is retx PACING …
+ collapse-conditioned escape … — filed as the campaign's next lever", docs/CHANGELOG.md:64-69).

**Perspective stack (mandatory, per CLAUDE.md):**
- **PHY theorist (primary):** a retransmission into a channel still correlated with the state
  that just killed the frame has ≈ the same failure probability — the Clarke/Jakes
  autocorrelation A(τ)=exp(−4π²σ²τ²) (the model our own estimator uses,
  `src/ofdm/doppler_coherence_estimator.hpp:14`) is the *quantitative* definition of
  "repeat-blind redundancy". Pacing must be derived from coherence time Tc = 0.423/f_D
  (`connection_policy.hpp:864-871`), never a magic ms constant.
- **Real-time DSP systems (mandatory):** every resend trigger lives in exactly three places
  (ARQ slot RTO in `tick()`, the tone-burst turn refill, the receiver's fast-NACK); a pacing
  hold that gates one but not the others just moves the blind re-blast to another timer.
  State must be per-connection, reset at boundaries, and never block the tick thread.
- **Veteran HF operator (mandatory):** when the band drops out you *stop sending and wait a
  breath* — you don't key the same traffic into the same dead band five times. A few seconds
  of silence after a failed burst is normal QSB rhythm; a 65-second same-window nack-storm is
  what an operator would call "machine-gunning a closed band". Half-duplex + PA duty are hard
  constraints: the pause must never strand the other station and strictly *reduces* key-down duty.
- **Physics arbiter:** fading loss is irreducible; ARQ is mandatory. The only free variables
  are *when* and *what* we re-key. Nothing here may assume TX+RX at once or >rung-ceiling
  throughput.

---

## 0. The measured problem and the current machinery (file:line inventory)

### 0.1 Measured collapse signature (Phase-0 forensics, 2026-07-02/03 campaign)

- **Sim, Good@20 seed 43 (g43):** during 16QAM "collapse eras" the ACK base froze for
  **42.7 s and 41.8 s**; seqs **59-74** (one full wide window) were retransmitted **4-5× each**;
  **69 retx to advance 11 frames = 6.3 retx/delivered**; **~35-45 s of the 65.2 s nack-storm
  airtime is repeat-blind redundancy** (same frames re-sent into the same trough). The in-code
  record: "~84 s of frozen-base blind re-blast per 16QAM collapse (sim g43 and live rig MPG@20
  both) — each retx round at 672 ms/frame x8 + RTO is a whole group-time spent re-sending into
  the same trough" (`src/protocol/connection.cpp:1713-1717`).
- **Live rig (IONOS MPG@20, run 1):** same signature — "16QAM@153 … → COLLAPSE (base frozen,
  blind retx) → ESCAPE-drop@250 (5 stuck retx)" (`/tmp/campaign_3000/RESULTS.md` run 1; ~107 s
  collapse era end-to-end, operator-witnessed).
- **The rejected hair-trigger:** `ULTRA_STUCK_ESCAPE_RETX=3` A/B was **seed-dependent** — g43
  1210→1530 (+26%) but g42 1940→1390 (**−28%**, "fled QPSK R3/4 early and never re-climbed to
  16QAM") (docs/CHANGELOG.md:64-69). Conclusion already filed: fix = **pacing** (when/what to
  resend) + **collapse-conditioned** escape (zero-delivered evidence), NOT a lower retry count.

### 0.2 Every path that can re-key a data frame today (the choke-point inventory)

| # | Trigger | Code | Timing today |
|---|---------|------|--------------|
| 1 | **Turn-boundary refill** after ANY tone-burst ack — *including one with zero new SACK bits* | `Connection::onToneBurstAck` sets `deferred_file_refill_ = true` "even when the cumulative base did NOT advance" → `runDeferredArqRefill()` → `sendNextFileChunk()` → `arq_.retransmitInFlightUnacked(burst_frame_cap)` re-blasts every unacked slot as the head of the next burst (`connection.cpp:1691-1700`, `connection.cpp:1590-1598`; `selective_repeat_arq.cpp:1598-1626`) | **Immediate** — this is the nack-storm inner loop |
| 2 | **Per-slot ARQ RTO** | `SelectiveRepeatARQ::tick()` per-slot `timeout_ms` expiry → `retransmitFrame(TIMEOUT, batch)` (`selective_repeat_arq.cpp:1117-1143`), batched to `transmitFrameBatch` which re-keys the whole expired set as ONE re-interleaved full-anchor burst + re-arms the ack monitor (`connection.cpp:3982-4022`, wiring `connection.cpp:3906-3911`) | RTO = `unifiedBurstAckTimeoutMs` ≈ **18-19 s** at 16QAM R2/3 cw8 ×9 (see §0.3) |
| 3 | **Receiver fast-NACK** on a timed-out group: delivers a FAILED group (frame_mask=0, quality 0) so the sender's ack path fires path #1 | `StreamingDecoder::accumulateBurstFrames()` group timeout = `max(8000, remaining_airtime×1.5 + 3000)` ms from first-frame decode (`streaming_burst_interleave.cpp:176-219`; `BURST_TIMEOUT_MS_BASE` `streaming_decoder.hpp:877`) | Zero-progress "turn" arrives ~**3-4 s after burst end** — the fastest blind-re-blast trigger |
| 4 | Frame-NACK / CW-repair | `handleNackFrame` → `sendDataRepair`/`retransmitFrame(NACK)` (`selective_repeat_arq.cpp:997-1018`) | prompt; already guarded by repair cooldowns (`computeRepairGuardMs`, `selective_repeat_arq.cpp:1181-1199`) |
| 5 | Hole fast-retx / hole-probe | **already DISABLED on the tone-burst burst path** (`!on_emit_tone_burst_sack_` gate, `selective_repeat_arq.cpp:917`, rationale 908-916) | n/a |

Paths **1** and **2** are the blind re-blast; path **3** is what makes path 1 fire fast.
Any deferral design must gate **both 1 and 2 from one state**, or the RTO leaks around the hold.

### 0.3 Timing arithmetic at the collapse rung (16QAM R2/3 cw8, z=27)

All derived from `wideOFDMFrameTiming`/`wideOFDMBurstAirtimeMs`
(`connection_policy.hpp:790-831`; symbol = 1152 samples = 24 ms, 59 carriers):

- **Frame airtime ≈ 672 ms** (28 symbols; the constant the escape comment cites,
  `connection.cpp:1713-1717`).
- **Burst cap** (`burstAirtimeBudgetFrames`, `kMaxBurstAirtimeMs = 8600`,
  `connection.cpp:3434-3486`): full anchor 1200 ms (`kWideOFDMFullAnchorExtraMs`,
  `connection_policy.hpp:23-29`) + 100 ms short-reanchor/frame on fading
  (`shouldUseWideOFDMShortReanchor`, `connection_policy.hpp:749-759`) →
  **9 frames/round on fading** (8048 ms), 11 on AWGN (8592 ms). The wide window
  (`kToneBurstAckWindowCapFrames = 16`, `connection_policy.hpp:70-84`) no longer binds here —
  the PA-duty budget does.
- **Sender RTO** (`connection_policy.hpp:1002-1053`, member wrapper `connection.cpp:3488-3520`,
  armed per burst via `prepareUnifiedBurstWindow` → `arq_.setAckTimeout`):
  burst 8048 + rx_response ((9−1)×336 + 3000 = 5688) + decode 1400 + ack ~300 + slack 1500 +
  reliability anchor 1200 ≈ **18.1 s**.
- **Good channel:** design Doppler 0.1 Hz (`connection_policy.hpp:50-55`) → **Tc ≈ 4.23 s**
  (`coherenceTimeMsForDoppler`), fade cycle 1/f_D ≈ 10 s (measured "~10-20 s Good cycle",
  docs/CHANGELOG.md:212).

So one blind round costs ~8 s of key-down + ~3-10 s of wait ⇒ the measured ~10 s/round,
6-7 rounds/collapse-era. **The round period (~10 s) is ≥ 2×Tc — yet rounds keep failing**,
because a 16QAM collapse era is not one Clarke null: at 16QAM's zero-margin operating point the
entire below-median half-cycle (~5 s) plus consecutive low cycles is undecodable (the rig eras
ran 42-107 s). That is why pacing alone cannot finish the job — it must be paired with the
collapse-conditioned escape (§2) that exits the rung when the *era*, not a frame, is the problem.

---

## 1. Trough-aware resend deferral

### 1.1 Definition of a "fully-failed round" (the trigger)

A **round** ends at any of:
- a tone-burst ack/NACK processed while data is in flight (`Connection::onToneBurstAck`,
  `connection.cpp:1649-1707`), or
- a per-slot RTO batch fired (`transmitFrameBatch` timeout path, `connection.cpp:3982-4022`).

A round is **zero-progress** iff the ARQ made *no forward progress*: TX base did not advance
AND no new SACK bit was set (`handleAckFrame` already computes both — base advance at
`selective_repeat_arq.cpp:881`/`advanceTXWindow`, new bits at `selective_repeat_arq.cpp:883-902`).
An RTO round is zero-progress by definition (a timeout *is* the absence of an ack).
Duplicate re-heard SACKs must not create phantom rounds — the ARQ's existing ack-signature
dedup fields (`last_ack_signature_valid_`/`last_ack_seq_`/`last_ack_bitmap_`,
`selective_repeat_arq.cpp:78-81`) are the dedup key.

**New ARQ accessor (design):** `int lastAckProgressFrames() const` — set inside
`handleAckFrame` to (frames retired by base advance + newly-set SACK bits), −1 for "no ack this
round". The Connection must NOT infer progress from `FileTransferController` counters — those
are identity-blind (BUG-FILE-ACK-IDENTITY, docs/KNOWN_BUGS.md:134-155); ARQ window state is the
identity-agnostic ground truth.

### 1.2 The deferral interval (channel-derived, never a magic ms)

After a zero-progress round, the next resend round is deferred so that the channel has
decorrelated from the state that failed it. By the estimator's own model
(`doppler_coherence_estimator.hpp:14`), A(τ) ≤ 0.5 at τ ≥ Tc. Target:

```
T_defer(n) = clamp( frac × Tc × 2^(n−1)  −  t_since_last_tx_end,
                    0,  T_cycle/2 )                       n = consecutive zero rounds ≥ 1
Tc      = coherenceTimeMsForDoppler(f_D)                  connection_policy.hpp:864-871
T_cycle = 1000/f_D  (ms)                                  Clarke fade-cycle period
frac    = 1.0 default (knob, §5)
```

- **f_D source, in priority order:**
  1. The Doppler-coherence estimate: `DopplerCoherenceEstimator::dopplerHz()`
     (`doppler_coherence_estimator.hpp:122-131`) when `valid()`. **Plumbing gap (to close):**
     the Connection today receives only the lag-1 *score* + valid
     (`modem_protocol_binding.hpp:62-64` → `ProtocolEngine::setChannelCoherence`
     `protocol_engine.cpp:737-739` → `Connection::setChannelCoherence`
     `connection.hpp:439-456`); `dopplerHz()` must ride the same feed (one extra float, same
     hold-last-valid semantics — BUG-DOPPLER-COHERENCE-MODECHANGE-WIPE fix applies unchanged).
     Caveat honored: `dopplerHz()` is documented SECONDARY/approximate (fixed nominal 1.6 s
     cadence, `doppler_coherence_estimator.hpp:117-131`) — acceptable because T_defer is
     order-of-magnitude machinery bounded by the clamp, not a decode decision.
  2. Fallback: `designDopplerForFadingIndex(coherenceAdjustedFadingIndex(fading_index_,
     coherence_score_, coherence_valid_))` (`connection_policy.hpp:50-55`, `:305-315`) —
     Good 0.1 Hz → Tc 4230 ms, Moderate 0.5 → 846 ms, Poor 1.0 → 423 ms. This is the
     "fading-index-derived default"; it is ITU-R F.1487-keyed, not a tuned constant.
- **`t_since_last_tx_end` subtraction:** by the time the sender *learns* a round was
  zero-progress, part of Tc has already elapsed listening. On the fast-NACK path that is
  ~3-4 s (< Tc at Good ⇒ deferral bites, ~1-2 s hold); on the 18 s RTO path it exceeds Tc ⇒
  **T_defer = 0 at Good** — correct: the (over-long, see §6.4) RTO already over-paces that path;
  we add nothing on top.
- **The ×2 escalation** encodes "the era is longer than one Tc" evidence; it takes at most one
  step before the §2 escape fires (N=2), so the ladder is effectively {Tc, 2Tc}∩[0, T_cycle/2].
- **The T_cycle/2 cap** is the trough-dwell bound: deferring past half a fade cycle overshoots
  the next crest (Good: cap 5 s; Moderate 1 s; Poor 0.5 s — fast channels barely defer, which
  is right: their troughs pass on their own). An absolute engineering clamp of [0, 8000] ms
  additionally guarantees no hold can exceed one burst-time regardless of estimator garbage.

### 1.3 Where the hold is enforced (one state, both triggers)

New per-connection state: `retx_pace_hold_ms_` (decremented in the CONNECTED tick,
`connection.cpp:2823-2827`, *before* `runDeferredArqRefill()`).

- **Trigger #1 (turn refill):** `runDeferredArqRefill()` already re-latches and returns when
  the turn is blocked (`connection.cpp:2531-2560`); add `retx_pace_hold_ms_ > 0` to that guard
  — the deferred-refill flags stay latched, so the refill fires automatically when the hold
  expires. No resubmission logic changes; `sendNextFileChunk`'s [holes]+[new] coalescing
  (`connection.cpp:1590-1626`) is untouched.
- **Trigger #2 (slot RTO):** when arming a hold, push every active unacked slot's timer by the
  hold (`SelectiveRepeatARQ::deferPendingRetransmits(uint32_t ms)` — adds `ms` to `timeout_ms`
  of every `active && !acked` slot, one log line). Without this, `tick()`
  (`selective_repeat_arq.cpp:1117-1143`) blind-fires mid-hold and `transmitFrameBatch` re-keys
  anyway. Deliberately NOT a global freeze: control frames, MODE_CHANGE retries, ack repeats
  and the receiver role are unaffected.
- **Early release:** any ack processed during the hold that shows progress (late/duplicate SACK
  finally decoding) zeroes `retx_pace_hold_ms_` and the round counter — never wait out a hold
  when the channel just proved it delivers.
- **Scope gate:** pacing is armed only when `rateAdaptationActive()`-class conditions hold —
  CONNECTED, `isOFDMMode(negotiated_mode_)`, unified burst path, file SENDING with in-flight
  bytes (mirror of the escape's guards, `connection.cpp:1793-1800`). MC-DPSK and OFDM_NARROW
  are explicitly out of scope (§6): their timers were just re-derived
  (BUG-MCDPSK-ACK-COLLISION / BUG-MCDPSK-FILE-COMPLETION, docs/KNOWN_BUGS.md:116-129) and their
  RTT already dwarfs any Tc.

### 1.4 Why the RTO/deadline structure stays sound

- The RTO is a **lower bound** on when a resend is permitted, not a deadline the sender owes
  the peer; resending *later* than RTO is always protocol-legal. The dangerous direction —
  resending while the ACK is still in flight — was the 2026-06-19 premature-resend incident
  (`connection_policy.hpp:992-1001`), and deferral moves strictly away from it.
- `unifiedBurstAckTimeoutMs` and the tone-burst ack-listen window stay coupled exactly as
  today: the monitor floors to the ARQ timeout and is re-armed per (re)send
  (`armToneBurstAckListenWindow`, `connection.cpp:3522-3536`; timeout-resend re-arm at
  `connection.cpp:4016-4021`). A hold can let the monitor window lapse — harmless, because the
  deferred key-down re-arms it on send, same as any resend.
- **The peer is never stranded mid-group:** the hold is only ever armed *between* key-downs
  (round boundaries), and the receiver's group timeout runs only intra-group — the timer starts
  at the first decoded data frame and the RX returns to `SEARCHING` after fast-NACKing a
  timed-out group (`streaming_burst_interleave.cpp:199-233`). While the sender holds, the peer
  is idle-listening. There is no connection-level keepalive/inactivity timer to trip (verified:
  no such timer in `connection.{cpp,hpp}`); the only wall-clock watchdogs are harness-level,
  and the hold is capped at seconds.
- **Half-duplex turn rules:** the sender keeps `local_data_turn_`; if the peer requests the
  turn during a hold, `maybeYieldDataTurn` (`connection.cpp:1028-1076`) may yield — acceptable
  and even desirable (the channel is useless to us mid-trough; let control/reverse traffic use it).

---

## 2. Collapse-conditioned escape (zero-delivered evidence, not retry depth)

### 2.1 Trigger

Escape when the **window** is collapsing: `consecutive_zero_progress_rounds_ ≥ N` (default
**N=2**) while ≥⌈burst_cap/2⌉ frames are in flight at the current rung. Rounds and progress are
the §1.1 definitions; the counter resets on ANY progress. RTO-only rounds count — this is
essential because a cratered 16QAM "may emit NO tone-burst ack at all", the exact regime where
the ack-driven demote never runs (documented at `connection.cpp:1802-1805` and `:1895-1900`).

### 2.2 Action (reuse, don't fork)

Identical to today's escape body — refactor `maybeEscapeStuckFrame`'s action into a shared
`executeEscapeDrop(reason)`:
- QAM16 (either rate) → **straight to QPSK R3/4** + `noteQam16Demoted(2)` (double-weight
  cooldown) via `requestModeChange(..., CHANNEL_DEGRADED)` (`connection.cpp:1802-1816`);
- otherwise one-rung drop + `rate_controller_.noteRungFailed(...)` (`connection.cpp:1828-1835`).
All existing guards keep first refusal: `mode_change_pending_`, floor check, in-flight check
(`connection.cpp:1794-1800`).

### 2.3 The g42-protective property (why this cannot reproduce the −28% flee)

The `ULTRA_STUCK_ESCAPE_RETX=3` failure mode was fleeing a rung that was *delivering* — a
single unlucky frame reached 3 retx while its window-mates ACKed round after round
(docs/CHANGELOG.md:64-66). Under the round condition that history **cannot trip the escape**:
every round in which any frame is delivered or newly SACKed resets the zero-round counter, so a
lone straggler retrying amid deliveries never accumulates rounds — only a *whole-window* zero
streak (the g43/rig frozen-base signature, §0.1) does. Conversely, the g43 collapse trips it in
2 rounds (~20 s) instead of the ~42-107 s measured eras.

**Backstop kept:** the existing per-frame 5-retx trigger (`stuckRetransmitEscape()`,
`connection.cpp:1709-1727`, env `ULTRA_STUCK_ESCAPE_RETX` [2..10]) stays untouched as the
pathological-single-frame safety net (the Moderate@18 248-retx death it was built for,
`connection.cpp:1819-1827`). The round escape typically preempts it during collapses (2 rounds
≈ retry_count 2-3 < 5); on healthy windows only the backstop can fire — exactly today's behavior.

### 2.4 Interaction with pacing

Pacing makes each collapse round cheaper (no immediate re-blast); the escape caps how many
rounds a collapse era may consume. Order of events in a g43-style era with both ON:
round 1 fails → hold ~Tc (≈2-3 s net) → round 2 fails → **escape to QPSK R3/4** ≈ 20-25 s and
~2×9 re-blast frames total, vs measured 42.7-107 s and 69 retx. The requeue that escape's
mid-flight MODE_CHANGE performs is the ledger-exact path proven 2026-07-02-late
(`requeuePendingChunks`, `file_transfer.cpp:208-231`; docs/CHANGELOG.md:125-182) — no new
requeue semantics are introduced (§6).

---

## 3. What to resend on a partial-SACK round

**Decision: partial rounds resend immediately (status quo); only zero-progress rounds defer.**

EV analysis at 16QAM R2/3 (672 ms/frame) vs Good Tc ≈ 4.2 s:
- A **partial** SACK means some frames of the *just-finished* round decoded ⇒ the channel had
  usable crests within the last burst-time ⇒ the conditional near-term failure probability is
  the rung's ordinary operating loss, not the trough-conditioned ~1. The holes also do not pay
  their own turnaround: they ride as the head of the next budget burst together with new data
  (`sendNextFileChunk`, `connection.cpp:1590-1598` — "[in-flight holes]+[new chunks] … as ONE
  group"), so the marginal cost of resending k holes now is k×672 ms of airtime already inside
  a key-down we were going to make anyway. Deferring a partial round would idle a *proven-open*
  channel for ~Tc — strictly negative EV.
- A **zero-progress** round means the whole ~8 s burst window failed ⇒ trough-conditioned; the
  expected value of an immediate identical re-blast is ~8 s airtime × P(still-in-trough | failed
  ≤4 s ago) ≈ high — this is the measured 6.3-retx/delivered waste. Deferring ~Tc trades ≤5 s of
  silence against ~10 s of near-certainly-wasted round: positive EV whenever
  P(fail|A(τ)≈1) − P(fail|A(τ)≤0.5) ≳ 0.3, which the g43 era (4-5 consecutive same-frame
  failures) demonstrates by construction.

**Cross-frame interleave makes the split clean at exactly the rung that collapses:** ≥16QAM is
cross-frame interleaved by default (`burstCrossFrameInterleaveOn`,
`connection_policy.hpp:119-135`) ⇒ group loss is deliberately all-or-nothing (whole-group
ACK/NACK invariant, `connection_policy.hpp:109-118`) ⇒ 16QAM rounds are naturally bimodal:
mostly-delivered or zero — the zero-progress condition IS the 16QAM collapse detector, and
partial-SACK ambiguity barely exists there. QPSK/8PSK (interleave OFF, per-frame SR masks) see
genuinely partial rounds routinely — and there pacing correctly almost never engages, matching
the evidence that QPSK rungs don't exhibit the collapse signature. No per-modulation `if` is
needed: the same zero-progress rule specializes correctly through the interleave policy.

---

## 4. Half-duplex / PA / operator constraints

- **Receiver's group timeout:** cannot fire during a hold — it only runs between BURST_HEADER
  decode and group completion (`streaming_burst_interleave.cpp:176-219`); holds are armed only
  at round boundaries (after the group either completed, fast-NACKed, or the RTO expired), when
  the peer is in `SEARCHING`. The design adds **no mid-burst pauses** — a key-down, once
  started, is never split (PA relay + peer timer correctness).
- **Duty cycle:** a hold inserts silence between key-downs ⇒ duty strictly decreases during
  precisely the eras where today the PA re-keys ~8 s bursts back-to-back at the top of its
  thermal budget. No change to `kMaxBurstAirtimeMs` or the budget model
  (`connection.cpp:3434-3486`).
- **Carrier sense / turn structure:** unchanged; the hold is sender-local silence. Turn yields
  remain possible mid-hold (§1.4). The tone-burst partial-SACK timing on the receiver
  (`tone_burst_partial_sack_delay_ms_`, `selective_repeat_arq.cpp:616-626`) is untouched.
- **2 AM waterfall/audio:** today a collapse era *sounds* like the same 8-second roar repeating
  every ~10 s for a minute+ while nothing moves — the classic "your ARQ is beating a dead
  channel" tell. With pacing it sounds like: burst … quiet 2-5 s … burst … then an audible
  rate-drop (MODE_CHANGE chirp) and traffic resumes — i.e., what a competent operator does by
  hand. The GUI's existing "Adapt:" text (`last_adaptive_action_`, `connection.cpp:1850-1871`)
  should show `pace-hold <ms> (zero-progress round <n>)` so the pause is *labeled*, not
  mistaken for a hang (operator trust; also the A/B grep hook).
- **No shared timebase assumed:** all pacing state is sender-local wall/tick time; nothing on
  the wire changes, no peer clock is referenced. **Zero wire change** also means no lockstep
  rebuild — deployable unilaterally, unlike the window-16 lever (wire-break fatigue is real:
  CHANGELOG:80-84).

---

## 5. Knobs + A/B plan

### 5.1 Env knobs (all read-once static, default-OFF ⇒ byte-identical unset)

| Knob | Default | Range | Meaning |
|------|---------|-------|---------|
| `ULTRA_RETX_TROUGH_PACING` | `0` (off) | 0/1 | master switch for §1 deferral |
| `ULTRA_TROUGH_DEFER_TC_FRAC` | `1.0` | [0.25, 4.0] | `frac` in T_defer (§1.2) |
| `ULTRA_COLLAPSE_ESCAPE_ROUNDS` | `0` (off) | 0 (off), 2..8 | N zero-progress rounds → escape (§2) |
| `ULTRA_STUCK_ESCAPE_RETX` | `5` | [2..10] | existing backstop, unchanged (`connection.cpp:1718-1727`) |

Both new knobs get 🟡 EXPERIMENTAL rows in `docs/MODEM_INFRASTRUCTURE_MAP.md` in the same
change (mandatory map rule), alongside the existing `ULTRA_STUCK_ESCAPE_RETX` row (map line ~227).

### 5.2 Gate cells (faithful gate only — `tools/gui_qso_scenario.sh`, 50 KB, sequential, paired)

Baselines are the window-16 A/B records (docs/CHANGELOG.md:57-63): g42 2280, g43 1900, g7 1650,
AWGN 3520, Moderate@20 1150.

| Cell | Config | PASS criteria |
|------|--------|---------------|
| Good@20 **seed 43** (the churn seed) | pacing=1, escape_rounds=2 vs baseline | CRC-clean; goodput ≥ baseline (target +10-25%); collapse-era retx/delivered < 3 (was 6.3); max ACK-base-frozen span < 25 s (was 42.7) |
| Good@20 **seed 42** (the flee-victim seed) | same | CRC-clean; goodput within gate noise (±25% band, no worse than −10% over 2 paired runs); 16QAM time-in-rung (`[LADDER]` line) not materially reduced; **zero** `ESCAPE-drop … zero-progress` events |
| Good@20 seed 7 | same | CRC-clean, no regression |
| AWGN@20 seed 42 | same | **zero `[PACE]` holds and zero round-escapes must fire** (no zero-progress rounds exist on AWGN); goodput within noise of 3520 |
| Moderate@20 | same | PASS, byte-exact; holds if any are ≤1 s (Tc 846 ms cap) — verifies the fading-index fallback path |
| Knob-isolation | pacing=1 alone, escape=2 alone | attribute the win; both must independently not regress g42 |
| Rig MPG@20 (Pi5→Mac, ≥5 runs, clean level vol100) | both on | median goodput ≥ the 1.74 kbps clean-level baseline mean; any collapse era < 40 s (run-1 signature was ~107 s); md5 byte-exact |

### 5.3 Failure modes to grep for

- `"Resending ARQ timeout-repair"` (`connection.cpp:4012`) **during an armed hold** — RTO leak
  around the pacing state (trigger-#2 gating broken).
- `"Burst group timeout"` on the receiver (`streaming_burst_interleave.cpp:203`) correlated
  with sender holds — would mean a key-down was split mid-group (must be impossible; §4).
- `ESCAPE-drop` on g42-class runs (`connection.cpp:1812/1830` log lines) — protective-property
  violation.
- phy-diag `event=arq_timeout` (`selective_repeat_arq.cpp:1124-1135`) spacing < RTO — premature
  resend (the 2026-06-19 regression class).
- Repeated identical-signature acks incrementing rounds (phantom rounds; check
  `event=arq_ack_tx_toneburst` on the receiver vs sender round logs).
- `summary.env` `RESULT=FAIL` / watchdog overrun with a hold armed at end-of-log — over-deferral
  or a hold that never expired (tick decrement bug).
- Any `[PACE]` line while `negotiated_mode_ != OFDM_CHIRP` — scope-gate violation (MC-DPSK
  timers must never see this machinery).

---

## 6. Non-goals + interactions

1. **No wire change.** Sender-side only; no tone-burst payload bits consumed, no lockstep builds.
2. **MC-DPSK / OFDM_NARROW untouched.** Their RTT/collision fixes
   (BUG-MCDPSK-ACK-COLLISION, BUG-MCDPSK-FILE-COMPLETION, docs/KNOWN_BUGS.md:116-129) are
   freshly calibrated; their frame airtimes (~3.7 s) already exceed their channels' Tc — pacing
   is an OFDM-burst-path concern. Scope gate in §1.3.
3. **Not a fade-phase predictor.** No per-null scheduling, no channel forecasting — pacing acts
   only on *evidence* (a failed round). Predictive scheduling stays research
   (docs/KNOWN_BUGS.md:100-101 lists it as such).
4. **BUG-ACK-TIMEOUT-DOUBLECOUNT — not fixed here, but touched by reference.** The code path
   claims the 07-02 re-derivation closed it (`connection_policy.hpp:1020-1039` "closes
   BUG-ACK-TIMEOUT-DOUBLECOUNT") while the register still lists it OPEN with pre-07-02 line
   numbers (docs/KNOWN_BUGS.md:112-114) — **reconcile the register when landing this design**
   (the "move both timers together" rule there still binds: this design deliberately adds
   *zero* deferral on the RTO path at Good (§1.2) so nothing here depends on the RTO's exact
   length; a later RTO tightening makes pacing *more* valuable, not less).
5. **Requeue ledger.** Unaffected by pacing (holds don't reorder submissions;
   `retransmitInFlightUnacked` resends existing slots and pushes nothing —
   ledger pushes happen only in `getNextChunk`/`getSingleBlockPayload`,
   `file_transfer.cpp:195,427`). The round-escape reuses the exact MODE_CHANGE-with-frames-
   in-flight path the 07-02-late offset-ledger fix proved (`file_transfer.cpp:208-231`).
6. **BUG-FILE-ACK-IDENTITY.** Round-progress accounting is ARQ-level (base advance + SACK
   bits), never FileTransfer chunk counts — so the identity-blind retirement dispatch
   (docs/KNOWN_BUGS.md:134-155) cannot pollute pacing/escape decisions, and this design neither
   fixes nor worsens that bug.
7. **Wide window = bigger blast radius, quantified.** With `kToneBurstAckWindowCapFrames=16`
   (`connection_policy.hpp:84`) the per-round re-blast at 16QAM R2/3 cw8 is airtime-capped at
   **9-11 frames ≈ 6.0-7.4 s of repeated payload** (was 8 frames/5.4 s pre-widen), and a frozen
   base now pins up to 16 seqs (g43: seqs 59-74) — a full window of stuck frames blocks ALL new
   submission (`isReadyToSend`, `selective_repeat_arq.cpp:370-372`). A 5-round blind era
   therefore wastes ~30-37 s of pure repeats — matching the measured 35-45 s. **The window
   lever that bought +31% mean goodput is exactly what raised the stakes of blind resends** —
   pacing is its complement, not an independent nicety.
8. **`ULTRA_QAM16_R34` crest rung** (docs/CHANGELOG.md:13-51): the round-escape drops straight
   to QPSK R3/4 from either QAM16 rate, same as the backstop (`connection.cpp:1802-1816`); the
   R3/4→R2/3 soft-demote path is ack-driven and unaffected. The flagged same-rate
   `setCodeRate` no-op caveat (`selective_repeat_arq.cpp:38-41`) applies to the escape
   *transition*, not to pacing, and is already tracked in that entry.
9. **No receiver-side changes.** The group timeout re-derivation (its 8000 ms wall-clock floor,
   `streaming_decoder.hpp:877`) stays a separate tracked item.

---

## 7. Implementation checklist (one session)

1. **ARQ progress accessor** (`selective_repeat_arq.{hpp,cpp}`): `last_ack_progress_frames_`
   computed in `handleAckFrame` (base-advance count + newly-set SACK bits; reset to −1 at
   round consumption); dedup via the existing ack-signature fields. ~25 lines.
2. **ARQ hold primitive:** `deferPendingRetransmits(uint32_t ms)` — bump `timeout_ms` of every
   `active && !acked` slot (clamp against overflow), single WARN log. ~15 lines + unit test
   (slot timers extended; acked/inactive untouched; tick doesn't fire inside the hold).
3. **Doppler plumb:** `DopplerCoherenceEstimator::dopplerHz()` →
   `StreamingDecoder` atomic (beside the existing score/valid pair) → `ModemEngine` getter →
   `modem_protocol_binding.hpp:62-64` → `ProtocolEngine::setChannelCoherence(score, hz, valid)`
   → `Connection` member (hold-last-valid identical to `connection.hpp:439-456`; cleared in
   `enterConnected()`/`reset()` beside `coherence_score_`, `connection.cpp:3779-3783,4177-4179`).
4. **Policy helper** (`connection_policy.hpp`, next to `coherenceTimeMsForDoppler`):
   `retxTroughDeferMs(float doppler_hz, float fading_index, float coherence_score,
   bool coherence_valid, int zero_rounds, uint32_t elapsed_since_tx_end_ms)` implementing §1.2
   (pure, unit-testable across the whole channel family).
5. **Connection round accounting:** in `onToneBurstAck` (outermost bracket,
   `connection.cpp:1689-1701`) and `transmitFrameBatch` timeout path (`connection.cpp:3996`):
   update `zero_progress_rounds_`/`last_round_tx_end`; on zero-progress + knob ON → arm
   `retx_pace_hold_ms_` + call `deferPendingRetransmits`; any progress → reset counter + hold.
6. **Hold enforcement:** decrement `retx_pace_hold_ms_` in the CONNECTED tick before
   `runDeferredArqRefill()` (`connection.cpp:2823-2827`); add the guard to
   `runDeferredArqRefill` (`connection.cpp:2531-2560`, re-latch semantics as for
   `data_turn_tx_guard_ms_`). `[PACE]` log + `last_adaptive_action_` text.
7. **Collapse escape:** refactor `maybeEscapeStuckFrame` action into `executeEscapeDrop()`;
   add the round condition (§2.1) behind `ULTRA_COLLAPSE_ESCAPE_ROUNDS`; keep the 5-retx
   backstop verbatim. Reset all pacing/round state in `enterConnected()`/`reset()` and on
   `applyDataMode` (a mode change starts a new era).
8. **Knobs** (§5.1) read-once statics; rows added to `docs/MODEM_INFRASTRUCTURE_MAP.md` §env-knobs
   + §7 register in the same change.
9. **Unit tests:** `test_connection_policy` — `retxTroughDeferMs` (Good/Moderate/Poor Tc values,
   elapsed subtraction, ×2 escalation, T_cycle/2 cap, invalid-coherence fallback);
   `test_selective_repeat_arq`-class test for the progress accessor + hold primitive; a
   round-counter test proving the g42-protective property (partial ack resets).
10. **Gate:** `cmake --build build -j4 && ctest --test-dir build --output-on-failure -j4`
    (expect only the pre-existing `UltraTncSimAudio` red, docs/KNOWN_BUGS.md:168-174), then the
    §5.2 A/B matrix knob-off first (byte-identical expectation), then knob-on. CHANGELOG +
    KNOWN_BUGS (DOUBLECOUNT register reconcile, §6.4) in the same change.
