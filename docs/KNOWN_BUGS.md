# Known Bugs

Last updated: 2026-07-05

## Purpose
Track only currently relevant issues that can affect reliability, throughput, or release quality.
Fixed/obsolete historical deep dives belong in `docs/CHANGELOG.md`.

## Active Issues

### BUG-PHYSICAL-SNR-RIG-REF: the new physical in-band SNR readout is wrong on the IONOS bench (read 4.4 dB below effective 6.7 — physically impossible) and stale after handshake
- Status: **OPEN (filed 2026-07-07; display-only — nothing consumes it).** The two-SNR model
  (CHANGELOG 2026-07-07) computes physical = (P_train − N)/N with N = the burst-time
  inter-chirp-gap RMS. On sim it reads 9.0 at a true 10.0 (known −1 dB training-window
  geometry bias). On the rig (WGN@10) it read 4.4 vs effective 6.7 — impossible (physical ≥
  effective always), so the reference is polluted: the IONOS gap noise measured ~4-5 dB hotter
  than the noise present during the training span (S:N-machine tracker dynamics inside the
  100 ms gap, and/or gap-window geometry on the corrected chirp positions). Also STALE: only
  computed on ping-check paths (handshake), then the atomic latches — data-frame logs reprint
  the connect-era value.
- Fix path: (1) move the measurement into `updateTrainingSNREstimate` where the training
  geometry is exact (per-symbol span, settled FIRs); (2) characterize the IONOS gap-noise
  level vs during-signal noise (one bench experiment: long chirp train, compare gap RMS vs
  post-burst decay curve); (3) recompute per decoded frame, not per ping-check.
- The EFFECTIVE (routed) SNR is unaffected and remains the rate-selection input (#74).
- Context: rig effective 6.5 vs wire-truth 9.3 at WGN@10 = real hardware implementation loss
  (~1.2 Hz clock/jitter wander over the 144 ms training window explains it exactly; sim reads
  10.1 dead-on). The operator display should eventually show BOTH numbers.

### BUG-ANCHOR-WAIT-NO-ACK-STALL: marginal-SNR bursts rejected in full-anchor-wait emit NO ACK → sender 44 s RTO stall (THE marginal-SNR throughput lever, quantified ~+30%)

- **Discovered/quantified 2026-07-14 (operator-caught "we didn't even ACK at
  SNR 25"; MPG@25 natural batch F279-F283).** The receiver, while in
  full-anchor-wait, runs a light DATA sync on each incoming burst. At marginal
  SNR the burst-head correlations arrive at **0.15-0.32**, below the
  weak_accept floor (~0.44-0.45, `signal_policy.hpp` evaluateLightSyncCandidate
  :262-278 requires corr ≥ max(weak_floor 0.45, min_confidence-0.08)). So the
  burst is REJECTED (`Full-anchor wait rejected DATA fallback`,
  sync_controller.cpp:817), in streaks of 2-5. A rejected burst never reaches
  group-decode → **NO ACK is emitted** → the sender waits its full RTO
  (**44.68 s** at window-16, burst airtime 21.5 s × 2) before resending as a
  full-anchor burst the receiver finally accepts.
- **Cost (measured):** each stall ≈ 44 s dead air. MPG@25 batch: goodput
  inversely tracks CW-fails/stalls — F283 clean (4 fails) = **2.81 kbps** vs
  rough F281 (1015 fails) = 1.38; mean 2.16. Eliminating the stall lifts the
  mean toward the ~2.8 clean-epoch ceiling ≈ **+30% at this SNR** — bigger than
  any PAPR/QAM lever. Forensics: ~/Documents/ultra_forensics/{F283_mac,nat25_pi5}.log.
- **KEY (the elegant fix): even a FAILED (0/N) decoded group still emits a
  tone-burst ACK with the cumulative base UNCHANGED** ("no progress, resend") —
  which breaks the 44 s silence and gets an immediate resend. So the receiver
  does NOT need to decode the weak burst; it only needs to NOT be silent.
- **Status (2026-07-14): v1 SHIPPED (keepalive ACK, ULTRA_KEEPALIVE_ACK default
  off, fc75b5b+). Mechanism confirmed on rig (fires 1-2×/transfer on the
  stall), safe (ctest clean, cannot cause fails), but the 25 s threshold
  catches the stall late and the A/B was epoch-noise-dominated (inconclusive,
  faint positive in the one clean pair). NEXT = v2: ULTRA_KEEPALIVE_ACK_MS
  ≈ 8-10 s (safe — routes through listen-before-ACK, mid-burst fire is
  deferred; catches the stall ~15 s earlier) + many-pair rig A/B; then flip
  default-on. See CHANGELOG 2026-07-14.**
- **Fix approaches (protocol/sync path, rig-validate; ACK
  contract has regressed before, build carefully):**
  1. ACK-LIVENESS ACCEPT: when a burst is rejected in full-anchor-wait but
     light_found with clear energy (corr above a low floor, e.g. 0.15 — a real
     burst, weak sync), still let it into the decode path so a group-ACK (even
     0/N) is emitted. Risk: false-lock on noise / misaligning the next real
     sync — bound with an energy floor + the correct-base 0/N ACK is harmless.
  2. IMMEDIATE ANCHOR-NACK: on reject-with-energy, send a bare "resend full
     anchor" tone NACK immediately (needs a group-less NACK tone type). Cleaner
     semantically, more protocol surface.
  3. RTO CAP for the anchor-reject case: the receiver knows instantly it can't
     sync; the sender shouldn't wait 44 s. (Blunt; approach 1 is better.)
- Related: [[project_turnaround_rxq_defeats_warmsync]] (warm-sync defeat /
  echo-clear), the ACK-contract family.

### BUG-FILE-CRC-MISMATCH: complete 51200/51200 file assembled with WRONG CONTENT (P0)
- Status: **FIXED 2026-07-05 late evening.** Root-caused by 3-agent forensics (write-map + code audit + adversarial verify) over the preserved run; rig gate LIFTED.
- Mechanism (confirmed, single event in all logs): the LDPC false-positive BIT-FLIP SALVAGE (frame_v2.cpp Case 2). Under status.allSuccess() every CW is a VALID codeword, so the true error is a codeword DIFFERENCE (>= d_min, tens of bits) — a 1-bit "repair" can never be genuine there. The salvage searched ~5k bit positions for a 16-bit CRC syndrome match (~7.8% collision odds vs garbage), hit one at t=38.4 ("FALSE POSITIVE RECOVERED (1-bit flip frame byte 584 bit 3)"), and delivered a corrupted 616-byte chunk (file[616:1232), seq=2) that was ACKed and never resent. Receiver assembly was proven flawless (78 chunks, perfect tiling, all era boundaries exact ACK-edge requeues).
- Fix: ALL bit-flip salvage removed (1-bit, CRC-bit, 2-bit-suspect, Case-1 header search) — completes the 2026-03-15 removal of the 3/4-bit searches that had already caused file corruption (the 1-bit search was wrongly exempted as "exact"). The min-sum re-decode fallback KEPT (it can converge to the TRUE codeword; full frame CRC gates it). Detected false positives now fail the frame; ARQ resends.
- BONUS FIND, also fixed: the vendored CRC32 table had TWO corrupted entries (111: 0xDD0D7A9B->0xDD0D7CC9, 245: 0xCDD706B3->0xCDD70693 — independently re-derived from the polynomial). The file "CRC32" was non-polynomial (linearity broken, burst-detection guarantees void) but self-consistent between stations, so transfers verified — and it still caught this corruption. Wire note: both ends rebuild together.
- Validation: gate run PASS CRC-clean, 0 false-positive events; full ctest green. Prior PASSes confirmed genuinely clean (the gate CRC-checks every run; this was the only false-positive event in any preserved log).
- Evidence preserved: `/tmp/campaign_3000/PRESERVED_crc_mismatch_run/` (gate run good@20 s42, RX-AUTHORITY + anchor-CFO-fix v1 build). `FileTransfer: CRC mismatch (got=CC1983F9 expected=BFD6400B, size=51200/51200)` at t=268.7 — full size, wrong bytes, every fabrication guard silent.
- Run context (suspect factors, unproven): 5 authority mode moves + 8 crater'd groups + requeue-rewinds; heterogeneous chunk sizes across rate changes writing overlapping offsets is the prime suspect class (BUG-FILE-REQUEUE-OFFSET's sibling: content, not resume-point). Frame-CRC-passing constellation corruption is considered implausible (would need many simultaneous CRC collisions). HARQ cross-era combining is second suspect (soft_combine_harq_.clear() coverage on descriptor-committed moves).
- Investigation entry point: reconstruct the file's wrong byte ranges (diff against qam16_50KB.bin), map to chunk offsets/eras, find which transmission wrote them.

### BUG-ANCHOR-CFO-KILL: connected full-chirp re-anchor CFO seeding killed 25% of full-anchor groups at 16QAM (0/N, all frames)
- Status: **FIX v2 IN TREE 2026-07-05 (uncommitted), gate validation in progress.** Root-caused by 3-agent forensics over 4 gate runs (61 FULL groups: 15 fail vs 19 LIGHT: 0 fail; causal A/B sticky-G13 vs climb-G13 same trough; the one LTS-refine firing flipped a failing group to 5/5 iters=1).
- Mechanism: the full-anchor path seeds the whole group's CFO from the chirp gap estimate (sigma 0.3-1.15 Hz phantoms under fading) instead of the warm pilot-tracked value (<0.1 Hz); the drift clamp has a sub-1 Hz blind spot; the LTS residual refine is structurally gated off on fading (cv>=0.20); the poisoned burst_cfo_ rotates every frame -> 16QAM (~10 deg margin) dies group-wide with confident-but-rotated LLRs.
- Fix v1 (warm-keep alone) FAILED its gate run: the noisy chirp was accidentally load-bearing as the tracker's re-center — burst-frame pilot ingest runs BEFORE any LDPC verdict, so crater stretches walk the tracker (measured -0.10 -> +0.29) and v1 removed the only correction. Fix v2 adds outcome-owned certification: a delivered group certifies the warm value (certifyWarm), a 0/N group revokes it (revokeWarm) -> next full anchor re-centers from the chirp. Cold/idle/PING/MC-DPSK/narrow unchanged.
- Residual (separate lever): ~13% of full-anchor groups die to genuine deep nulls under cross-frame interleave (class A) — no CFO fix touches those.

### BUG-TONEACK-FABRICATION: phantom tone-ACK detection fabricated cumulative delivery of 6 undelivered frames — silent 3.7KB file hole + 9.5-min zombie stall (F116)
- Status: **FIXED 2026-07-05 (4-layer, unconditional — data integrity, no knob).** Root-caused from F116 rig forensics + 5-agent adversarial verification (workflow wf_1e79fe9e).
- Failure shape: rig 50KB transfer died with receiver FileTransfer stuck at `expected=34944` while BOTH ARQ ends stayed "consistent" — sender fully ACKed → "payload drained" → 900s-grace auto-disconnect; receiver kept ACKing everything it saw. Bytes 34944..38688 (exactly the crater'd 8PSK group) were never retransmitted by anyone. No error surfaced anywhere.
- Trigger: ToneBurstAckMonitor false positive on STALE AUDIO. The post-detection `consume_until` omitted `tail_base` (window-relative offset used as buffer-relative) → the decoded burst stayed re-scannable; a later cadence tick re-decoded the peer's old 12ms ACK at the 50ms rung (duration aliasing) and fluked Costas+Hamming+CRC-12 (1/4096/candidate) into `group_seq=5/type=NACK/mask=0x7E02` — 60k samples BEHIND the previous detection. The {12,25,50,100}ms multi-rung scan (2026-06-15) is the false-positive surface.
- Amplification: `SelectiveRepeatARQ::onToneBurstAck` nearest-mapped the 6-bit value onto the seq space → base=69, a seq NEVER SENT (nothing consulted `tx_next_seq_`); freshness guard passed (13 ≤ window+1); the cumulative walk retired all 6 in-flight slots firing `on_send_complete(true)` each → FileTransfer ledger popped irreversibly → every later `requeuePendingChunks()` resumed at 38688, past the hole. `tx_base` also advanced PAST `tx_next` (split window) so every real ack afterwards read as stale.
- Fix (all unconditional):
  1. `tone_burst_ack_monitor.cpp` — consume coordinates fixed (`tail_base + offset + needed`) + stream-MONOTONICITY guard (a detection at/behind the previous one is a stale-audio re-decode → dropped, WARN).
  2. `selective_repeat_arq.cpp onToneBurstAck` — SUPPORT-CONSTRAINED decode: an ack can only reference `[tx_base-1, tx_next-1]`; span ≤ window+1 < 64 ⇒ the 6-bit decode is unique inside the support; outside = prior probability zero → DROP as ack-loss (RTO recovers). Never nearest-map.
  3. `connection.cpp onToneBurstAck` — Nack-TYPED detections consumed whole before the ARQ/rate-controller/drive-advisory (nothing on the unified path emits type=Nack; the WAITING-REBASE voice's group_seq is a different sequence space).
  4. `handleAckFrame` — never-sent guard on the shared ack path (covers corrupt control-frame SACKs too) + structural invariant `tx_base != tx_next` in the cumulative walk. New stat `fabricated_acks_dropped`.
- Regression: `tests/test_arq_toneburst_fabrication.cpp` (4/4: exact F116 repro, 64-value property sweep, control-path fabrication, legit-ack preservation).
- Residual: a corrupt control-frame SACK could still phantom-retire WITHIN the sent window (needs an LDPC+frame-CRC fluke — astronomically rarer than the tone path).

### BUG-FALSE-COMPLETION-FAMILY (STRUCTURALLY CLOSED 2026-07-07 — F218 completion gate): chunk-count completion drifted three separate ways; the ARQ is now the ground truth
- Status: **BELT LANDED (F218, third family member).** F218: sender declared
  "Transfer complete (1.43 kbps)" on an ack that retired only thru seq 89 with
  **10 frames in flight** (salvage OFF — a different count leak than F181),
  went idle, receiver stranded at 93 % rcvd / 88 % assembled. Evidence:
  ~/Documents/ultra_forensics/F218_{mac,pi5}.log.
- **Structural fix:** `FileTransferController::maybeCompleteSend()` +
  completion gate injected by the Connection (`arq_.getTxInFlightBytes()==0`).
  The chunk ledger can say done, but completion DEFERS while any frame is in
  flight — state stays SENDING, RTOs keep retransmitting the holes, and the
  gate re-checks on every retiring ack (handleArqTxBaseAdvanced). Every count
  drift in this family is now non-fatal by construction.
- **Open (low prio):** the specific F218 count leak (which path over-counted
  chunks_acked_ / under-counted sent across the 16QAM re-encodes) — forensic
  from the preserved logs; the gate contains it regardless.

### BUG-SACK-DURABILITY-RESIDUAL (DEFUSED 2026-07-07; root-cause narrowed): F181 reproduced the sender-complete/receiver-stranded wedge WITH the F168 deliver-before-discard fix active — a third loss path exists
- Status: **OPEN — observed 2026-07-07 F181 (final batch, MPG@20).** Sender
  "Transfer complete 1.37 kbps" (salvaged ranges [1824,3048) at t=82); receiver
  logged ZERO file-progress marks in 900+ s (contiguous prefix stuck < 25 %)
  and died on the scenario timeout. The two patched discard sites (epoch
  adoption + RX setCodeRate) log salvages — 13 epoch/salvage-class lines
  present — yet the low-offset bytes never reached the receiver's file prefix.
- **Suspects (unverified):** (a) a THIRD RX-slot discard path (full arq reset?
  partial-slot clear?) that drops buffered FILE payloads unlogged; (b) sender
  slots marked `acked` by a STALE-EPOCH SACK (epoch-echo gating hole) so the
  salvage skipped ranges the receiver NEVER confirmed in the new era; (c) the
  salvage delivered but FILE_START/offset bookkeeping rejected the write.
- **Evidence preserved:** ~/Documents/ultra_forensics/F181_{mac,pi5}.log.
- **DEFUSED (same night):** forensics narrowed it — the sender skipped
  [0,456) (the file's FIRST chunk) on a SACK mark for a frame the receiver
  PROVABLY never had (prefix pinned at 0; receiver spent that era ack-silent/
  UNANCHORED, so it could not have SACKed anything → the mark was
  era-corrupted/phantom). The sender-side skip now defaults OFF
  (`ULTRA_SACK_SALVAGE=1` re-enables for measurement); SACKed ranges are
  re-sent and the receiver dedups by offset — the stranded-file class is
  structurally closed. Receiver-side deliver-before-discard stays (pure
  benefit; the rate-change site now logs its salvages for forensic parity).
- **Remaining open question (low priority):** the exact phantom-mark chain
  (stale-epoch SACK bit surviving the era gate on slot 0).

### BUG-DECODE-BACKLOG-COLLISIONS: under deep-fade search thrash the decoder falls 10-20 s behind LIVE audio — every receiver response (ACK, backstop, adopt) leaves stale, colliding with the sender's already-airing recovery bursts
- Status: **OPEN — pinned 2026-07-07 (F176 rig, MPG@20).** Hard evidence: a burst
  AIRED at t≈218, its anchor was ACCEPTED by the decoder at t≈239 (**~20 s of
  processing lag**), the anchored-burst backstop (sample-clock, correct by its own
  basis) therefore fired at t=251 — exactly as the sender's next recovery burst
  keyed. Operator waterfall shows the ACK tones over the burst head.
- **Mechanism:** when fades deepen (F176 post-80%: CFO drift to −0.49, 2/5
  groups), the sync search THRASHES — full-anchor-wait reject streaks with
  decaying thresholds, weak-DATA fallbacks, repeated correlation over the same
  audio — and per-buffer processing cost exceeds real time. The ring keeps
  filling; the decoder's "now" detaches from the antenna's "now".
- **Why no receiver-side gate can fix collisions while this holds:** the
  geometric ACK gate (F176 fix), CCA, and decoder-evidence checks all consume
  DECODED state; a burst the decoder hasn't reached yet is invisible to all of
  them. With the decoder 20 s behind, the receiver is answering questions from
  20 s ago. This is the same #56/RXQ class that defeated warm-sync in June
  (rig turnaround forensics) — now shown to also manufacture TX collisions.
- **MITIGATED 2026-07-07 (same night):** search LOAD-SHED landed — real-time
  decoders (production engine opts in; batch tools/tests never shed) cap the
  search backlog at 2 s: the search floor jumps forward and eats the loss
  (WARN-logged with shed seconds). Receiver staleness is now bounded at ~2 s —
  the collision class and 20 s-stale responses are structurally gone. The
  original fix direction below remains for the underlying thrash cost:
- **Fix direction (deeper, next session):**
  bound the search cost per fed buffer (correlation work budget per real-time
  interval; skip-ahead instead of re-correlating overlapping windows on reject
  streaks), and/or a load-shedding rule: when unsearched backlog exceeds ~2 s,
  jump the search floor to (write_pos − backlog_cap) and eat the loss — a
  real radio that falls behind MUST drop audio, not time-travel. Measure
  backlog_ms (already in DecoderStats: current/peak_unsearched_samples) around
  fade episodes to size the budget.
- **Impact:** the top remaining source of ACK/burst collisions and late-response
  stalls; also inflates every ack round-trip during fade episodes (the F163
  budget's "RTO dead-air" rows are partly this).

### BUG-POSTTX-ACK-MISS: tone monitor tail-window sweep never scans audio deeper than ~520ms into a single feedAudio append — the first ACK after our own key-down (capture-resume backlog) is captured, fed, and never scanned (~19s RTO each, 2/run F76-F77)
- Status: **FIXED 2026-07-05 (ULTRA_ACK_MONITOR_GAPLESS, DEFAULT-ON since 6340f51+flip; =0 opts out).** Root cause: all cadence passes triggered inside one feedAudio(count) call scan the SAME end-anchored window (tail_base = buffer end - window); a chunk larger than a bin's tail window leaves a permanent blind hole (12ms bin window ≈ 25k samples). Fade and staircase-bin mismatch REFUTED by capture ledger (tone arrived rms 0.033-0.091, all ACKs symbol_ms=12).
- Fix: gapless armed sweep — per-bin window extends to cover everything since the scan high-water mark + one burst of context (gapless by induction; hw jumps to buffer end per pass; end-straddling tones re-covered next pass). Plus permanent forensics: arm log INFO, armed-window-EXPIRED-undetected INFO (fed_in_window/max_chunk/passes classifies any future miss), monitor-level detection INFO chained in front of the production callback.
- Validation: 10-run rig batch F78-F87 — ~280 ack exchanges, 0 misses, 0 expired-undetected (was 2/run); every RTO classified genuine fade. Sim parity (1960 PASS), tone-burst tests 5/5.

### BUG-ACKLISTEN-TONE-FALSELOCK: the sender's warm data-sync detector S&C-false-locks on the PEER'S TONE-BURST ACK during ACK-listen — garbage decode + blind re-search races the tone monitor for the same samples; the rig loses the race (missed ACK -> ~28s RTO stall / demote spirals)
- Status: **FIXED 2026-07-05 (knob ULTRA_ACKLISTEN_SUPPRESS_OFDM, default-OFF, in the standing campaign config; 7752d60 — sync_controller.cpp guards in detectConnectedLightSync + detectFullAnchorFallback).**
- **CORRECTS the BUG-SELF-ECHO-REANCHOR-STALL attribution below** (2026-07-05 forensics, F73/F74 + three-way proof): the corr 0.9x locks during ACK-listen are NOT self-echo — (a) the OTASim server EXCLUDES self-audio by construction (`ota_channel_core/mixer.cpp:40-42` skips blocks whose station_id == receiver); (b) a solo-station control (one GUI alone on OTASim, PING into the void) heard NOTHING; (c) on the rig, capture is STOPPED during our own TX (app.cpp:3360-3364 setRxMuted + stopCapture) — recording our own burst is physically impossible. The locks are the peer's 4-FSK tone ACK: its periodic carrier lead-in + repeated FSK symbols score ~0.9+ on the Schmidl-Cox metric while the LTS matched filter stays ~0.1 — the sc-high/mf-low signature on EVERY such lock, sim and rig; in sim the locks land exactly on the ack-repeat copy (+376ms). A threshold cannot gate this (0.94 > every decayed threshold, incl. the full-anchor-wait 0.52 gate it slipped in F74) — only unconditional suppress-while-armed works. The dual-chirp path stays live (a tone cannot fake the up+down pair at the 28800-sample gap), so full-anchor control frames still acquire during the window; reject streaks / §16.4 untouched.
- Validation: OTASim A/B seed42 good@20 tone-locks 50->0, 0 resends, PASS 1990 (baseline 1730). Rig F75: first ACK 9.9s (F74: 28.5s + wasted resend + demote-to-R1/2), ACK cadence 9.56s metronomic, 0 timeout-resends, 0 craters, **2.62 kbps all-time rig record** (single cell ±25-30%; the mechanism metrics are the proof). Multi-seed validation queued.
- The earlier ULTRA_ECHO_REANCHOR_GATE (bfe5676) suppressed one downstream *reaction* (the 0-CW re-anchor) to the same tone-locks — mechanism mislabeled, suppression directionally right (+17% F65-F68). Both guards stack; bfe5676's log line still says "SELF-ECHO" (rename queued).
- Related F73 lesson (ULTRA_ENTRY_QAM16_SNR, 20f6006, default-OFF): cold 16QAM connect entry decodes marginal (quality 0.35, no warm estimate) — its slow decode widened this race and collapsed the ladder to R1/4. Keep entry at QPSK; the climb warms the equalizer.

### BUG-SELF-ECHO-REANCHOR-STALL: sender decodes its own burst echo during ACK-listen, re-anchors, and MISSES the receiver's crater-demote tone-ACK -> 2x-RTO collapse (~36s) instead of a ~3.5s demote
- Status: **FIXED 2026-07-04 (knob ULTRA_ECHO_REANCHOR_GATE, default-ON; streaming_ofdm_decode.cpp self-echo guard).** Root-caused by forensic wrp84o66o (high confidence, clocks aligned Mac=Pi5+22.48s). **2026-07-05 MECHANISM CORRECTION: the "own burst echo" attribution was WRONG — see BUG-ACKLISTEN-TONE-FALSELOCK above (the signal was the peer's tone ACK; self-echo is physically excluded on both benches). The fix keyed on the right state (monitor armed) and remains valid.**
- **THE 2.50->1.44 GAP.** F3 (morning 2.50) and afternoon runs deliver the SAME clean groups/craters/signal-level; the only difference is per-group cadence (10.2 vs 15.8 s/group). Steady state is 9.4s (fine); the gap is occasional 30-36s STALLS. Mechanism: a 16QAM R2/3 group genuinely craters on a deep fade (thin-margin payload dies, QPSK R1/4 header survives corr 0.99). The receiver immediately sends the crater-demote command (kRungCmdDownHard) on its tone-ACK — the fast path that resolves craters in ~3.5s (F4/F6/F7/F10/F11). BUT the sender never hears it: its RX path is decoding its OWN transmitted burst echoing back (self-echo, corr 0.96-0.98 at sample positions inside its just-transmitted span), each 0-CW fail re-arms expect_full_ofdm_anchor_ -> a 120000-sample blind search that consumes the RX path -> the tone-ACK is missed -> 2x18s RTO + collapse-escape = 36s.
- **Fix:** while tone_burst_monitor_.isArmed() (= we are the data-sender in post-burst ACK-listen; the peer emits ONLY a tone-ACK then), suppress the destructive full-chirp re-anchor on a 0-CW OFDM "decode". A genuine receiver decoding real peer OFDM never arms the monitor, so its legitimate crater re-anchoring is untouched. Sender-local, no wire change. Expected: 36s -> ~4-10s; most afternoon runs jump from ~1.4 toward the 16QAM ceiling.
- Not to be confused with BUG-BURST-HEADNULL-DROP (a fade nulls the group HEAD -> clean frames 2..N dropped; separate, still open).


### BUG-RESPONDER-HANDSHAKE-NEVER-CONFIRMS: responder handshake_confirmed_ only flipped in the CLASSIC frame path — a descriptor-era burst-only session never confirms, so the modem TX-routes every classic control frame via the handshake last-RX-waveform mirror = 3.1 s MC-DPSK DBPSK (operator saw "MC-DPSK at the end of the run")
- Status: **FIXED 2026-07-04** — confirm extracted to maybeConfirmResponderHandshake, now also fired on the first delivered burst group (equally hard evidence the initiator heard our CONNECT_ACK). Legacy runs confirmed via the initiator first MODE_CHANGE side effect (OFF-arm log: confirm@77s); full-descriptor runs confirmed only at DISCONNECT (@294s). Exposed by Phase 1/2 eliminating classic control frames by design.


### BUG-STAIRCASE-SNAPSHOT-INPUT: the tone-ACK staircase reads a single last-frame SNR cache write — no validity gate (a total-erasure slot at noise-floor RMS with 100% zero soft bits still lands in the cache), no fade averaging, no rung clamp
- Status: **ACTIVE, deferred (2026-07-04 forensics link 2).** The detonator (phantom frame from the DESC-SWITCH group-size clobber) and the trap (monitor buffer < 100ms rung) are both FIXED 2026-07-04, which de-fangs this for the observed incident class; the input remains unprincipled. Fix direction: gate cache writes on frame validity (zero-soft-bit/noise-RMS slots are not measurements) + fade-averaged/median statistic over kept frames (aligns with task #58) + clamp emitted rungs to the sender-decodable set.


### BUG-UNANCHORED-SILENCE-ESCAPE: a lost EPOCH_REBASE head frame makes the receiver by-design ACK-SILENT while it keeps DELIVERING data → the sender's zero-progress collapse-escape reads the silence as a forward-link crater → MANUFACTURED demote of a working rate + forced re-delivery
- Status: **FIX IMPLEMENTED 2026-07-04 (WAITING-REBASE voice: rung_cmd=3 under ULTRA_RX_RATE_CMD — unanchored receiver voices per group; sender resets zero-progress evidence + standalone era-base resend; design \x{a7}5.3). Validation: build+ctest+sim, then rig batch.**
  forensics, HIGH confidence, no rerun needed). Fix in progress.**
- Mechanism (E1, IONOS MPG@20, clocks aligned Mac=Pi5+16.93s): after the demote-1 epoch
  rewind, the epoch-1 EPOCH_REBASE frame (seq 33, head-of-burst = the most fade-exposed
  slot: acquisition marker-timing retries logged) never decoded across 3 rounds. The
  receiver adopted epoch 1 UNANCHORED → deliberate ack-silence
  (`selective_repeat_arq.cpp` interregnum rules) while still SALVAGING frames 34-37 every
  round — receiver rx_base=38 (everything delivered) vs sender acked=33. Two ACK-less RTO
  rounds → collapse-escape demoted working R2/3 → R1/2 (+ another full stop-and-wait
  exchange + re-delivery). ~83 s of a 600 s window burned. Same signature in D1 (base 15
  vs rewind 10) and D3 (four cycles, two 4-retry exhaustions).
- Fix direction (ranked by the adjudicator): give the unanchored receiver a VOICE on the
  4-FSK tone plane (rung_cmd reserved value 3 = "waiting-rebase" under `ULTRA_RX_RATE_CMD`)
  → sender resends the era-base frame instead of counting zero rounds; escape policy
  becomes reverse-path-aware (silence-while-unanchored ≠ forward crater).

### BUG-MC-RETRY-SPURIOUS: MODE_CHANGE retry timer is anchored at REQUEST time, but the frame rides the TAIL of the sender's own bundled key-down (~10.6 s) → the 18.2 s deadline structurally loses to the real 21-30 s pipeline → EVERY trough exchange retries even though copy #1 was ACKed
- Status: **ACTIVE (proven on E1 demotes 1+2, D1, D3 — all observed cycles 21.07/21.27/30.37 s
  vs 18.2 s timer; the winning MC-ACK had fully ARRIVED 1.9-3.0 s before each retry fired —
  the sender's own RX decode pipeline surfaces control decodes only after key-up).
  ALL FOUR FIXES IMPLEMENTED 2026-07-04:** (1) TX-hold retry clock (setTxActiveProvider — deadline holds while keyed), (3) receiver same-(seq,mod,rate) dedup single-re-ACK, (4) stale CCA-deferred data-TX purge on mode commit, plus (2) the waiting-rebase voice above. Validation: build+ctest+sim, then rig batch.
  carried the frame (evidence-driven), not at request time. Related receiver-side wart:
  `handleModeChange` has NO same-seq dedup — every duplicate copy re-applies + re-notifies
  + emits a fresh fading-aware 3-copy ACK set (the operator-visible dup [MODE] lines).
- Also caught in the same forensics: a stale CCA-deferred TX burst rendered at the OLD
  rate/epoch pre-commit was flushed post-commit → 9.0 s of undecodable airtime (fix:
  drop/re-render deferred audio on mode/epoch commit).

### BUG-ARQ-SEQ-COLLISION: rate-change abort under ONE-WAY ACK loss re-chunks DIFFERENT file bytes under seq numbers the receiver already retired → receiver's seq-keyed dedup destroys them before the offset-idempotent file layer can see them → permanent byte hole + sender false-complete + receiver stranded
- Status: **STRUCTURAL FIX IMPLEMENTED 2026-07-03 (knob-gated `ULTRA_ARQ_MOVE_EPOCH`,
  default OFF = byte-identical; edits-only/UNVALIDATED — no build/ctest/gate run yet;
  rig-validation-pending with BOTH stations knob-ON — WIRE/SEMANTICS-BREAKING when ON,
  lockstep, no capability negotiation this increment).** Interim receiver-side salvage
  (`ULTRA_BELOW_WINDOW_FILE_SALVAGE`, now DEFAULT-ON, 9/9 rig field engagements) stays as
  belt-and-braces. Root-caused from rig W16 (IONOS MPG@20, Pi5 sender → Mac receiver,
  50 KB) by multi-agent forensics + adversarial verify, 2026-07-03.
- **Downstream dependency (2026-07-03):** the descriptor-committed mode switch
  (`ULTRA_DESCRIPTOR_MODE_SWITCH` Phase 1, docs/MODE_SWITCH_PIGGYBACK_DESIGN_2026_07_03.md)
  HARD-DEPENDS on this epoch machinery for its Phase-2 ESCAPE (mid-window) commit path —
  until `ULTRA_ARQ_MOVE_EPOCH` rig-validates, escape drops stay on the legacy MODE_CHANGE
  exchange even with the descriptor-switch knob ON (Phase 1 = clean-boundary commits only,
  which need no epoch: an empty window has nothing to abort).
- **Structural fix (move-epoch) — what landed (unit-test-edited, unvalidated):**
  - **Epoch state:** two independent per-direction 2-bit (mod-4) counters in
    `SelectiveRepeatARQ`. `tx_epoch_` bumps exactly when `setCodeRate`'s TX-abort branch
    rewinds `tx_next_seq_` (the collision precondition; `setFixedFrameCodewords` →
    `abortPendingTx` abandons seqs FORWARD — no re-use, no bump). `rx_epoch_` is adopted
    from the wire (a receiver with nothing in flight never bumps locally).
  - **Wire (all bits are 0 when OFF = byte-identical):** DATA flags bits 6-7 carry the
    epoch (formerly the never-implemented rate-in-flags bits) and `EPOCH_REBASE` = the
    formerly never-implemented ENCRYPTED bit 0x08, stamped on any DATA frame created
    while its seq == the sender's window base ("nothing un-retired below me in this
    era" — invariant holds for the frame's life; baked into the serialized bytes so RTO
    resends re-carry it). ACK echo: v2 SACK bitmap bits 16-17 (window bitmap occupies
    bits 0-15 with MAX_WINDOW=16; bits 24-31 must stay clear of the `decodeSackBitmap`
    legacy-8-bit shim — that's why NOT bits 30-31); tone-burst ACK payload bits 40-41
    (the former Hamming zero-pad — still transmitted, so airtime is unchanged at 34
    symbols; deliberately NOT CRC-covered, else the knob-OFF CRC would change — a
    Hamming-miscorrected epoch fails SAFE: the ACK is ignored = ACK-lost = RTO resend).
  - **Sender gate:** `handleAckFrame` extracts+strips the echo and IGNORES any ACK whose
    epoch != `tx_epoch_` (log `stale-epoch ACK ignored`, stat `stale_epoch_acks_ignored`,
    phy-diag `reason=stale_epoch`; returns before the dedup signature and §RETX-PACING
    progress sentinel). This kills W16 kill-arm 2 (the out-of-window SACK crediting
    phantom chunks) AND structurally cures the review-flagged below-base zombie-TX wart
    (a late stale ACK can no longer advance tx_base past the rewound tx_next).
  - **Receiver adoption (the risky part — NOT naive re-anchor-to-incoming-seq):** on a
    DATA frame with epoch != `rx_epoch_` (serial half-duplex channel ⇒ any change is a
    newer era), adopt + discard all rx slots/pending-ack state, then anchor ONLY on an
    `EPOCH_REBASE` frame (`rx_base_seq_` = its seq — cumulative claims below it name
    only sender-retired seqs, zero fabrication). A rebase-less adoption (era head lost)
    enters an ACK-SILENT unanchored interregnum: no window bookkeeping, ALL acks
    suppressed (any cumulative claim from the old rx_base would fabricate delivery of
    new-era seqs = the phantom-retire disease), FILE payloads salvage-delivered
    (offset-idempotent), TEXT dropped (resent after anchor — in order, no dupes); the
    sender's RTO resends base-first and the rebase frame anchors us. Naive
    re-anchor-to-first-heard-seq was rejected: under head loss it makes the next
    cumulative ACK claim the lost head frames → sender retires them → their bytes
    permanently unresendable — recreating the hole. This kills W16 kill-arm 1 for ALL
    payload types (the salvage only covered FILE).
  - **Files:** `selective_repeat_arq.{hpp,cpp}` (state machine — full spec in the hpp
    MOVE-EPOCH block comment), `frame_v2.hpp` (`Flags::EPOCH_MASK/EPOCH_SHIFT/
    EPOCH_REBASE`, `epochFrom/ToFlags`), `arq_interface.hpp` (stat),
    `tone_burst_constants.hpp`/`tone_burst_payload.{hpp,cpp}` (bits 40-41),
    `connection.cpp` (tone-ACK emit/consume plumb). Unit tests:
    `test_selective_repeat` (`test_move_epoch_*`: bump+stamp on abort; the W16
    below-window regrid delivered as TEXT — proving epoch, not salvage; stale-epoch ACK
    ignored + fresh retires; unanchored ack-silence → late-rebase anchor; knob-off
    byte-identical) + `test_tone_burst_ack_payload` (epoch roundtrip/clamp + CRC-field
    invariance proof).
  - **Residuals (documented, accepted):** mod-4 wrap — 4 TX aborts with ZERO frames
    decoded in between return to the same epoch (falls back to today's salvage
    behavior); adoption only from COMPLETE DATA frames (stale/foreign-era partials and
    unanchored partials are dropped; DATA_REPAIR-sourced partials are epoch-exempt —
    synthesized flags — and cross-era merges are frame-CRC-rejected); GROUP_ACK/
    GROUP_NACK/NACK frames carry no epoch (they trigger retransmits, never retirement —
    a stale NACK costs at worst one duplicate resend); a rebase frame that exhausts
    max_retries kills the transfer exactly as an undecodable frame does today (no new
    failure mode); MODE_CHANGE ACKs are intercepted by the Connection before the ARQ
    and carry no epoch.
- **Mechanism (the full chain):**
  1. During a 16QAM R2/3 epoch the receiver DELIVERED seqs 69-77 (384 B/chunk grid),
     advancing its `rx_base_seq_` to 78 — but the sender heard NONE of the ACKs (one-way
     ACK loss: data direction alive, ACK direction dead). Sender base stayed 69.
  2. The collapse-escape's rate-change abort rewound the sender to its OWN stale base:
     `SelectiveRepeatARQ::setCodeRate` does `tx_next_seq_ = tx_base_seq_`
     (`selective_repeat_arq.cpp:~76`), and `FileTransferController::requeuePendingChunks`
     resumed at the (now exact, post-BUG-FILE-REQUEUE-OFFSET) sender-ledger offset 30576 —
     re-chunking bytes 30576+ on the NEW QPSK R3/4 grid (456 B/chunk) under the SAME
     seqs 69-77.
  3. New-69..77 covered bytes [30576, 34680) but the receiver's ARQ dedups BY SEQ, not by
     byte: the frames died at the below-window drop (`selective_repeat_arq.cpp` `handleDataFrame`
     out-of-window branch, ~:645-675) BEFORE reaching the offset-idempotent file layer —
     whose straddle-merge (`file_transfer.cpp` `processFileData`, ~:612-621) was built
     exactly for regrid resends and never ran. Old-69..77 covered only [30576, 34032)
     (9×384), so bytes [34032, 34680) — exactly 9×(456−384) = **648 B** — existed ONLY in
     the destroyed frames.
  4. Each below-window frame triggered an out-of-window SACK carrying cumulative base 78;
     the sender (hearing ACKs again by then) interpreted it as delivery of new-69..77 →
     the phantom chunks retired forever (`onChunkAcked`), never resendable.
  5. Ghost transfer: everything from seq 78 (offset 34680+) was received AND ACKed but
     buffered out-of-order behind the 648 B hole (~16.5 KB); contiguous edge frozen at
     34032 (66.5%). Sender declared "Transfer complete" (identity-blind counter equality,
     `file_transfer.cpp` `onChunkAcked` — no receiver confirmation; sibling:
     BUG-FILE-ACK-IDENTITY), idled out the 600 s grace, disconnected; receiver stranded
     237 s then "Transfer cancelled".
- **Precondition:** `rx_base > tx_base` at a rate-change abort — i.e. the data direction
  working while the ACK direction is dead, exactly the asymmetric failure the rig exhibits
  (W15 shows the same one-way signature without a rate change → RTO grind instead of a hole).
- **W16 evidence** (`/tmp/campaign_3000/pi5_W16_gui.log` (P) /
  `rig_W16_failed_mac_gui.log` (M), Pi5 clock = Mac − ~6 s; full forensics in the
  2026-07-03 W16 workflow output): P304.097 "Re-queued 9 pending chunks after ARQ abort
  (acked=69, resume_offset=30576)" + "SR-ARQ: Code rate changed, aborted 9 unACKed
  in-flight TX slots (cleared 0 SACKed); rewound TX seq to 69"; M320.205 "SR-ARQ: DATA
  seq=69..73 outside window [78, 94)" and M332.767 "seq=74..77 outside window [78, 94)";
  P397.334 "[FILE] Transfer complete (362.8s, 1.13 kbps)" (false complete); M648.694
  "[FILE] Transfer cancelled". Chunk-grid arithmetic exact: 22×456 + 6×384 = 12336 and
  (30576−12336)/40 = 456.
- **Why the earlier W16 escape (t=132) was benign:** that 16QAM epoch had failed BOTH
  directions (Mac logged 0/8-CW decode failures at 116-162), so `tx_base == rx_base == 29`
  at the abort (P161.263 "acked=29, resume_offset=12336") — same seqs re-covered the same
  bytes, no collision. The bug arms ONLY on one-way ACK loss.
- **Relation to prior work:**
  - BUG-FILE-REQUEUE-OFFSET (FIXED 2026-07-02): fixed the SENDER-side resume arithmetic
    (offset ledger). W16 proves the requeue is still destructive when the RECEIVER's base
    is ahead — the ledger resume is exact w.r.t. the sender's knowledge, but the sender's
    knowledge is stale by construction under one-way ACK loss. Same gate-bypassing
    escape-drop path (the clean-boundary gate cannot help: the escape fires with frames in
    flight BY DESIGN).
  - The review-flagged **stale-ACK epoch hazard** (2026-07-02-late adversarial review of
    the requeue fix): ACKs are epoch-blind — an ACK formed against the OLD chunk grid (or
    an out-of-window SACK carrying an advanced base) is credited by the post-abort sender
    against NEW-grid chunks carrying different bytes. W16 is the live confirmation of that
    hazard. The verify also found the abort leaves `tx_next_seq_` below an ack-advanced
    `tx_base_seq_` (`handleAckFrame`'s cumulative advance, `selective_repeat_arq.cpp:~863-885`,
    never re-clamps it) → 4 below-base zombie transmissions observed on air (the M332
    reject group).
  - 2026-06-09 verdict vindicated: gate-less escape-drop (abort-coordinated requeue) is
    unsafe; this is the rig proof.
- **STRUCTURAL fix direction (IMPLEMENTED 2026-07-03 as `ULTRA_ARQ_MOVE_EPOCH` — see the
  Status block above; kept for the record):** a **move-epoch** counter
  carried on DATA frames and echoed in ACKs, bumped at every rate/mod-change abort, so a
  stale-epoch ACK can never retire new-content seqs (the sender ignores ACKs whose epoch
  predates its current grid; the receiver's out-of-window SACK for old-epoch frames is
  then harmless). Complementary candidates from the forensics (still OPEN): carry the receiver's
  `rx_base_seq_` + contiguous byte offset in the MODE_CHANGE_ACK (which demonstrably DOES
  arrive — it triggers the abort) and resync the sender to the RECEIVER's state; and the
  receiver-confirmed completion handshake (DATA_END, CLAUDE.md Known Limitation 4) so a
  false sender-complete is detected and hole-repaired by byte offset while airtime remains
  (W16 had 244 s of idle grace and needed ~120-150 s).
- **Interim salvage (landed 2026-07-03, default OFF, rig-validation-pending):**
  `ULTRA_BELOW_WINDOW_FILE_SALVAGE=1` — receiver-side only: in the ARQ's below-window drop
  path, a frame whose payload decodes to FILE_START/FILE_DATA (`PayloadType`,
  `file_transfer.hpp`) is handed up the SAME delivery callback in-order frames use
  (Connection `handleDataPayload` → `file_transfer_.processPayload`) BEFORE the (unchanged)
  out-of-window SACK; the file layer's offset dedup + straddle-merge make double delivery
  safe by construction. NEVER salvages other payload types (messages are seq-deduped only —
  re-delivery would duplicate them) and never far-future seqs (strict half-space
  below-window test). Converts W16's 648 B hole into a delivered straddle-merge chunk: the
  contiguous edge advances, the ghost 16.5 KB drains, the receiver finalizes. Log grep:
  `SALVAGE below-window FILE frame`. Unit-tested in `test_selective_repeat`
  (`test_below_window_file_salvage`: knob-on FILE_DATA/FILE_START salvaged + SACK
  unchanged, TEXT never, far-future never, knob-off byte-identical drop).

### BUG-BURST-HEADNULL-DROP: group-head fade null → clean mid-group frames silently dropped (no decode attempt, no counter, no ACK credit) → whole-group RTO saga
- Status: **OPEN — root-caused in code + reproduced in logs (2026-07-01 audit, adversarially verified).** Found via `fable_analysis/09_WHY_STUCK_AT_2000_2026_07_01.md` §3.2.
- **What:** burst accumulation is marker-gated — only the group-start frame (negated-LTS marker) can enter accumulation (`streaming_ofdm_decode.cpp:~1090` marker check, `:1271` entry). When a fade null covers the group head (LTS + frame 1), frames 2..N arrive clean (observed corr 0.95-0.96, LTS SNR 26.7-27.0 dB), are sync-accepted (`accepted connected DATA sync fallback`), fail the 1-CW control-first peek, and are consumed by the **log-less** mid-burst re-search (`streaming_ofdm_decode.cpp:1042-1052`): no 8-CW decode attempt, no counter, no partial-ACK credit. Each occurrence costs a full sender RTO (~24.5 s) + a whole-group resend (~8 s). Observed: Good@20 seed-42 stock run lost 83 s on one group (3 resend attempts; ~20 s of clean 27 dB frames received and discarded) = ~23% of the transfer.
- **Why the silent path exists (do NOT naively fall through):** the legacy control→data profile fall-through double-demod **poisoned the burst's shared coherent channel estimate** (§14.24/§14.25, comment at `streaming_ofdm_decode.cpp:1021-1029`); the gate was also narrowed for BUG-TNC-B2F-001. A fix must re-demodulate from the ring at the data profile (fresh estimate), not fall through.
- **Fix path:** (1) FREE: add a counter/log for "sync-accepted data-profile frame consumed without decode attempt" (today it is invisible). (2) The descriptor already gives the receiver the group geometry — enter accumulation from ANY group-member sync with the head erasure-marked, converting the saga into a 1-frame nack. (3) Interim mitigation is the RX group-timeout fast-NACK, which requires the BURST_HEADER *and* ≥1 decoded data frame — the head-null case defeats it.

### BUG-ACK-STAIRCASE-FADE-BIN: SNR-adaptive fast tone-ACK never engages on fading channels — **FIXED (b85c0e1, 2026-07-01) — this entry was STALE; occupancy verified 2026-07-03**
- Status: **FIXED + GATE-VERIFIED.** The fix landed in b85c0e1 ("revive the SNR-adaptive ACK
  staircase (feed + edge)"): per-group broadband SNR feed (the cache had been FROZEN at the
  handshake MC-DPSK reading for whole transfers) + the fading-conditioned fast edge
  (`kFastAckEdgeFadingDb=16.0` vs AWGN 18.0, `tone_burst_constants.hpp` `symbolMsForSNR`).
  The staleness was found by the 2026-07-03 campaign Phase-0 audit. Occupancy measured on the
  2026-07-02 5-cell gate: g42 = 28/30 fast (12 ms), g43 = 35/38, g7 = 24/33, AWGN = 17/17 —
  the fast rung engages on fading. Residual: the 100 ms rung fires in trough sagas at RTO
  cadence (g7: 5×2700 ms), which is detection-safety-correct behavior, not this bug.
- History (kept for the record): the §15.5 staircase picked 12 ms at ≥18 dB but was fed
  fade-effective SNR (~16-17 at Good@20) AND a frozen cache → 0% fast occupancy everywhere
  incl. 30/30 rig ACKs at 675 ms. Same disease family as #74/#58 (fade-aware measurement vs
  AWGN-calibrated threshold) plus a dead feed. NOTE 2026-07-03: the tone-burst payload widened
  32→40 bits (16-bit SACK mask) — ACK airtimes are now 408/850/1700/3400/6800 ms per rung.

### BUG-CONNECT-SNR-VARIANCE (#58 completion): the connect-time SNR is a single 170 ms snapshot on a ~4 s-fade channel → ~10 dB pick-to-pick spread; the +2 dB basis correction fixes the BIAS, not the VARIANCE
- Status: **FIX IMPLEMENTED (increment 2, 2026-07-02, default-ON) — pending rig
  connect-spread re-measure.** The designed data-aided estimator LANDED
  (`updateDataAidedSNREstimate`, `multi_carrier_dpsk.hpp`; routed in
  `populateDecodeMetrics` via `ULTRA_CONNECT_DATA_AIDED_SNR`, default-ON, `=0` opts out;
  decode-then-measure — routed only when the frame's LDPC decode succeeded, else the
  training snapshot). Differential-level (unit-phasor chord error vs the config-driven
  DBPSK/DQPSK constellation, ~0.2 s block averaging ≪ Tc), so it needs NO static channel
  reconstruction and is drift-immune across the multi-second frame. Calibration derived,
  not tuned: magnitude normalization cancels the differential +3.01 dB; geometry-computed
  non-orthogonal-carrier ICI (−29 dB/carrier @8×1024, matches measured excess) subtracted;
  (k−2)/k inverse-chi-square block correction; +0.5 dB measured residual. GATED in
  test_mcdpsk_snr_calibration: AWGN within 1 dB at 0/5/10/15/20/25 dB (measured
  +0.5/−0.1/−0.1/−0.0/+0.4/+0.1). Both values logged per frame
  (`MC-DPSK SNR: training=X data_aided=Y (routed=...)`) for the rig spread comparison.
- Rig MPG@20 evidence (one evening, 6 connects): snapshots
  18.2 / 17.0 / 14.6 / 14.5 / 12.4 / **8.4** while the chirp-quality proxy read 22-27
  throughout. The 8.4 connect survived to OFDM only by 0.4 dB (sel=10.4 vs Good floor 10) and
  picked **QPSK R1/2** — half throughput on a channel that carries R2/3. Deeper dips than the
  +2 average penalty WILL occasionally cross into the MC-DPSK branch (the residual coin-flip).
- **Root cause was architectural, not the estimator** (AWGN-accurate to 30 dB,
  test_mcdpsk_snr_calibration): `updateTrainingSNREstimate` measures ONLY the ~170 ms training
  preamble = one fade state (Tc ≈ 4.2 s at Good); the CONNECT frame's 4 CWs span ~7.1 s ≈
  1.7 Tc and are fully decoded before the pick → the whole-frame estimate is fade-AVERAGED
  by construction at zero handshake latency.
- **Increment 3 IMPLEMENTED (2026-07-03, UNVALIDATED — edits-only session, all three knobs
  default-OFF/byte-identical):** the VARIANCE fix proper. Rig campaign data (12 connects at
  dial MPG@20) showed the increment-2 estimator still yields per-connect readings 3.9-17.9
  (σ 3.15) — one fade sample per pick; W3's lone 3.9 trough reading bought a ~90 bps DBPSK
  session (~20× mis-pick). Landed: `ConnectSnrPool` (pure-header ring, cap 8, population =
  data-aided MCDPSK + tagged OFDM_BROADBAND; decorrelation-clustered dB-mean, Tc from the
  trough-pacing derivation chain) behind `ULTRA_CONNECT_SNR_POOL` (entry pick + CONNECT_ACK
  byte + window-16/file-block gates); `ULTRA_CONNECT_PICK_DEFER` (N_eff==1 fading sub-OFDM
  pick withholds CONNECT_ACK once → the initiator's CONNECT retry supplies a decorrelated
  second reading); `ULTRA_WIRE_SNR_FRESH` (MODE_CHANGE embeds gate at 3·Tc, else the −10
  sentinel = wire byte 0 = the receiver's existing "n/a" rendering — fixes the W2 stale-wire
  signature: 3.2 dB shipped 31 s stale, 16.5/22.0 frozen 40-300 s). Companion knob-free GUI
  fix: `MODE_CHANGE:` line labels `wire_peer` vs `local_measured` (responder connect line was
  a mislabeled LOCAL reading). Composition unchanged: +5 basis and the Moderate saturation
  bound apply ONCE, downstream of the aggregation (`test_connect_snr_pool_*` gates this).
- **Increment 4 IMPLEMENTED (2026-07-03, BUG-CONNECT-FADING-VARIANCE — the FADING side of
  the same disease; rides `ULTRA_CONNECT_SNR_POOL`, no new knob):** the entry pick classified
  the channel from a SINGLE CONNECT frame's `fading_index` while the SNR beside it was pooled.
  Screenshot bug (dial-20 Good): one 0.66 reading → Moderate (boundary 0.65) → QPSK R1/4 on an
  R2/3 channel. Rig ledger (48 dial-MPG@20 entries, docs/CONNECT_ENTRY_CALIBRATION_2026_07_03.md):
  single-frame fading 0.24-0.74 (σ 0.129), **false-Moderate 18.8%** (8/9 of those entered R1/4)
  → projected 7.8%/4.1% at N_eff=2/3. Landed: fading rides each pooled reading
  (`ConnectSnrReading.fading_index`, fed from `setChannelQuality`), `clusteredFadingIndex`
  (same Tc clusters, mean of cluster means — bounded statistic, mean not median),
  `rateSelectionFadingIndex()` feeds all entry-pick fading consumers at
  handleConnect/acceptCall/negotiateMode incl. the defer predicate and the CONNECT_ACK
  fading byte; non-entry `fading_index_` uses untouched. ctest green
  (`test_connect_fading_pool_aggregate`), build clean; same rig validation gate as inc 3.
- **Remaining (do NOT close yet):** (1) knob-off byte-identical gui_qso gate,
  then the knobs-ON 5-cell sim gate and low-SNR safety cells (good@8, MPM@8); (2) the rig
  MPG@20 ≥10-connect bench — pass criteria: 0 sub-OFDM entries, effective spread ≤ ~6 dB,
  per-connect σ ≤ 2.2 (√2 tightening), **0 false-Moderate entries at Good**, W2 staleness
  signature absent, ≤1 extra CONNECT cycle on deferred picks; (3) re-evaluate the +5
  `connectSelectionSnrDb` basis after the rig re-measure (the 48-entry ledger says mean
  offset −7.6 dB, SNR-dependent — deliberately untouched in increments 2, 3 AND 4).

### BUG-QAM16-RIG-LEVEL-BUDGET — CLOSED 2026-07-02 (final): there is NO hidden level deficit anywhere on the bench; the IONOS dial is the only SNR lever. 16QAM at MPG@20 is trough-limited physics; it opens at dial ~22+ (matches the sim crossover)
- **Every gain lever measured, all dead:** (1) TX drive walk +4.6 dB digital (software-ALC, live)
  arrived as −0.8 dB at the Mac — the IONOS normalizes its input; (2) Mac input volume 60→90:
  the noise FLOOR scaled +9.0 dB with the gain — the floor is the IONOS's own calibrated output
  noise, not ADC self-noise, so RX gain moves signal and noise together. The bench delivers
  exactly what the dial says. The earlier "raise CH-OUT / +4-5 dB level" advice is RETRACTED —
  measurably wrong. The ladder's QPSK R2/3 pick at MPG@20 is the bench's true optimum (~1.7 kbps).
- **To test 16QAM on this bench: turn the SNR dial** to MPG@22-24 (sim crossover data: Good@22 =
  2710/2150/2050 for 16QAM R2/3). The software-ALC remains correct and valuable for REAL radios
  (no box normalizes your level there); it is proven mechanically end-to-end on the rig.
- **FIX IN VALIDATION (2026-07-02, software-ALC — CHANGELOG entry of same date):** the "level
  lever" is now CLOSED-LOOP instead of operator-manual. Receiver measures per-burst data-RMS
  over chain noise + crest factor (`[ALC-RX]` / `LEVEL ADVISORY:` log lines, thresholds
  `ULTRA_ALC_LOW_DB`=12 / `ULTRA_ALC_CLIP_CF_DB`=6.5); a 2-bit drive advisory rides the
  tone-burst ACK (bits [30..31], **WIRE-BREAKING — lockstep builds only**); sender walks
  tx_drive +0.5 dB/−2 dB within [baseline, 0.85] on connected OFDM data bursts only
  (`ULTRA_SOFTWARE_ALC=0` disables the loop). Sim-proven no-op at reference levels (gate PASS,
  zero `ALC:` moves). **PENDING: rig A/B** — expect the loop to walk 0.5→~0.85 (+4.6 dB) at
  MPG@20, lifting arriving data SNR from ~6-7 dB toward the ~11-12 dB the 16QAM rungs need;
  then re-run the 16QAM ladder. Grep `ALC: tx_drive` (sender) + `\[ALC-RX\]` (receiver).
- Status: **DIAGNOSED via wire capture + rung falsification (overnight 07-02).** The Mac-input
  ffmpeg captures (paired QPSK vs 16QAM forced runs, MPG@20) show IDENTICAL level structure for
  both mods: data segments ~0.077-0.079 RMS, anchors ~0.16-0.18, noise floor ~0.037 → the data
  arrives at only **~6-7 dB broadband wire SNR**, while anchors ride 6-7 dB hotter (per-burst
  PEAK normalization: OFDM crest ~14.3 dB eats average power). No 16QAM-specific TX defect.
- **Rung falsification:** 16QAM R1/2 (4-5 dB more margin than R2/3, sim-clean 5/5 at Good@18)
  ALSO fails to complete on the rig — decode is **bimodal** (13 groups 8/8 flawless, 13 groups
  0/8 dead at median 22.7 dB effective): the fade TROUGHS at this wire level kill any dense
  constellation whole-group; the crests pass 16QAM perfectly. QPSK's phase-only margins bridge
  the troughs. 16QAM transfers PROGRESS (~26 KB of clean groups in 480 s) but can't finish at
  ~50% group loss.
- **Dead end tested + reverted same night:** coherent-OFDM PAPR soft-clip (recover average power
  under peak normalization). Sim A/B: EVM cost >> benefit for 16QAM at every depth (9 dB target:
  181 vs 30 deint-fails, hard FAIL; 12 dB: 78 fails, 2210->1320). `ULTRA_COHERENT_PAPR_DB`
  ships default-0 (off) as an experiment knob only.
- **The fix is a LEVEL lever, not code:** ~+4-5 dB of arriving data SNR moves the troughs above
  16QAM R1/2's floor (tx_drive 0.5->~0.8 and/or IONOS CH-gain re-staging — needs the operator at
  the IONOS CF/level panel per the 2026-06-15 calibration method; peaks must stay under the
  1800 mVpp input clip). Alternates if level can't move: cw16 (raises per-bit efficiency, same
  trough problem), fade-phase-aware scheduling (research). The prior ANCHOR-COLLAPSE observation
  (07-01 afternoon: corr 0.95->0.2) did NOT reproduce in any of 6 subsequent 16QAM runs — kept
  below as historical until seen again.

### BUG-QAM16-RIG-ANCHOR-COLLAPSE: 16QAM bursts stop being ACQUIRED on the real rig (sync corr 0.95 → 0.2) while their PHY decodes clean — root cause NOT isolated
- Status: **OPEN — observed 2026-07-01 on IONOS MPG@20 (Mac↔Pi5, HEAD a81725d), forced 16QAM R2/3.** The transfer decoded 9+ groups cleanly (**77/77 deinterleave SUCCESS, 0 FAILED — zero fade-damage tax**), then fell into a persistent no-delivery saga: 117 nack + 74 timeout retx, no completion in 480 s. Sender escalates full-anchor resends; receiver pinned in `Full-anchor wait rejected DATA fallback (corr=0.34 < 0.50)`.
- **The discriminating signature:** same channel, minutes apart — QPSK R3/4 run sync-corr modes 0.92-0.99 (delivered 1.62 kbps clean); 16QAM run corr modes **0.16-0.29**. The constellation doesn't fail; the *acquisition* of its bursts does. Burst-erasure gate hits: 0.
- **Candidate mechanisms (untested):** (a) TX-side PAPR — 16QAM+cross-frame-interleave raises burst crest factor → per-burst hardware normalization lowers average power → the in-burst anchor chirp sinks toward the RX noise floor (the 2026-06-16 IONOS "high-PAPR data below the anchor" mechanism; `ULTRA_SIM_PAPR_PENALTY` exists because OTASim cannot exhibit this); (b) RX-side — decode/search backlog during the saga (#56 class) + the full-anchor-wait rejection threshold locking the pair into mutual starvation.
- **Discriminating experiment:** paired QPSK-vs-16QAM rig runs logging the Pi5 per-burst normalization factor (AUDIO category) and the Mac chirp-segment RMS/corr per burst; if 16QAM's normalization factor is materially lower, it's (a) — mitigations: anchor power boost within the normalization budget, PAPR reduction (clip/tone-reserve) on the data symbols, or descriptor-profile robustness. If not, instrument the full-anchor-wait state residency.
- **Impact:** blocks the entire 16QAM-on-hardware path (the only route to 3000 bps) regardless of the sim-side damage work. Sim CANNOT reproduce (fidelity gap — document per SIMULATOR FIDELITY rules). See `fable_analysis/09_WHY_STUCK_AT_2000_2026_07_01.md` §4.

### BUG-ACK-TIMEOUT-DOUBLECOUNT: unified burst ACK deadline counted burst airtime ~twice (+ phantom reanchor term) → every timeout saga paid ~8-10 s extra dead air — **CLOSED IN CODE by the 2026-07-02 RTO re-derivation (register reconciled 2026-07-03)**
- Status: **CLOSED in code — register was stale.** The 2026-07-02 `unifiedBurstAckTimeoutMs`
  re-derivation (`connection_policy.hpp` ~1002-1053, comment: "RE-DERIVED 2026-07-02 (closes
  BUG-ACK-TIMEOUT-DOUBLECOUNT)") replaced the double-counting `physical_sack_hold_ms =
  max(configured window-hold, burstAirtime+30)` term with a receiver-response envelope derived
  from the SAME formula family as the receiver's group-timeout fast-NACK (rig-calibrated: 124
  groups across 4 MPG@20 transfers measured the clean-path group-end→SACK hold at 0-1 ms);
  `configured_sack_delay_ms` is deliberately no longer consumed on the burst path. This entry
  had stayed OPEN with pre-07-02 line numbers while the code claimed the fix — reconciled while
  landing the retx trough-pacing design (its §6.4), which deliberately adds ZERO deferral on
  the RTO path at Good so nothing in pacing depends on the RTO's exact length (a later RTO
  tightening makes pacing MORE valuable, not less).
- **Floor rule still binds (do not cut below):** the deadline back-stops the receiver's
  wall-clock group-timeout fast-NACK (`streaming_decoder.hpp:877` `BURST_TIMEOUT_MS_BASE` —
  its 8000 ms wall-clock floor is itself a tracked adaptivity item, NOT changed by pacing)
  and rig worst-case turnaround (~5.8 s post-burst latency; #56 RXQ backlog). The 2026-06-19
  premature-resend incident is the regression class to avoid. **Move both timers together.**

### BUG-MCDPSK-ACK-COLLISION: tone-burst partial-SACK fires < one MC-DPSK frame airtime → half-duplex collision livelock — **FIX IMPLEMENTED (2026-06-30), pending lossy-rig validation**
- Status: **ROOT-CAUSED + REPRODUCED on the live IONOS rig + FIX IMPLEMENTED + ctest-clean (OFDM byte-identical). Pending: lossy-channel rig A/B (the faithful gate runs clean → no holes → can't exercise it; also confounded by BUG-MCDPSK-FILE-COMPLETION which blocks *completion* regardless).** Surfaced once #74 let MC-DPSK connect + transfer at low SNR (rig MPG@8, DQPSK R1/4, window=5).
- **Fix (implemented):** the real mechanism is the tone-burst PARTIAL (hole-bearing) SACK sliding timer (`selective_repeat_arq.cpp` ~622), hardcoded to `kToneBurstPartialSackDelayMs = 1500 ms` — correct for OFDM (short frames) but < one MC-DPSK frame airtime (3691 ms), so it fired while the sender was still transmitting a trailing failed frame. Made it configurable (`setToneBurstPartialSackDelayMs`, default 1500 → OFDM byte-identical) and the Connection scales it to `max(1500, timing.data_ms + 1000)` ≈ 4.7 s for MC-DPSK so the SACK lands in the inter-burst gap and the sender does a FAST retransmit instead of a timeout whole-window resend. Carrier-sense (defer until the channel is heard idle) is the fully radio-correct generalization + would also cover the rare multi-trailing-hole case; this airtime-scaled guard is the targeted fix.
- **What:** on a hole (a frame in the window fails to decode), the transfer livelocks: the SENDER retransmits the whole window on its 31.6 s RTO (`cause=timeout`, NEVER on a NACK), the RECEIVER keeps re-sending the same SACK (`group_seq=35 frame_mask=0x02`), they collide forever → 10/10 retries → DISCONNECT. The clean path (no holes) works fine — groups advance every ~21 s. Only the REPAIR path collides.
- **Trace (rig, MPG@8):** Pi5 (sender) TX bursts at 408.3 / 439.8 / 471.4 s (each 896256 samples ≈ **18.7 s**, period ~31.5 s); Mac (receiver) ACKs at 420.6 / 452.0 / 483.6 s (period ~31.5 s). Every ACK lands ~12 s INSIDE a sender burst → sender mid-TX (deaf) → never hears the NACK → times out → resends → next ACK lands in that burst too. Both ends on the same ~31.5 s period, phase-offset to collide.
- **Root cause:** `selective_repeat_arq.cpp:1747` — `guard_half_duplex_repeat = (bitmap == 0) && !sack_has_final`. The half-duplex peer-burst guard is DELIBERATELY disabled for hole-bearing SACKs (`bitmap != 0`) so repair feedback is "prompt." Correct for OFDM (short bursts → sender stops & listens fast), WRONG for MC-DPSK: the window burst is ~18.7 s so the "prompt" NACK fires straight into the sender's transmission. The RTO was made rate-agnostic (`computeMCDPSKAckTimeoutMs` scales with the 3691 ms frame) but the ACK-repeat guard was NOT — `setAckRepeatPeerBurstGuardMs(arq_.getSackDelay())` = **30 ms**, vs an 18.7 s burst.
- **Fix (next session):** peer-burst-guard the hole-SACK repeat ALSO on the long-burst path — delay it past the sender's window-burst airtime (`connection_policy::mcDpskBurstAirtimeMs ≈ 18.7 s`, already computed) so the NACK lands in the inter-burst gap. Rate-agnostic = guard derived from the actual burst airtime, not the 30 ms sack_delay. The fully radio-correct version is **carrier-sense** (don't key up while the peer is heard); the burst-airtime-scaled guard is the minimal targeted fix. Needs rig A/B on a lossy channel (collision only appears on a hole). Likely interacts with BUG-MCDPSK-FILE-COMPLETION (a persistent hole that never repairs).

### BUG-MCDPSK-FILE-COMPLETION: MC-DPSK file never finalizes — sender ACK RTO under-budgets the RTT → FINAL chunk never reached — **FIXED + GATE-PROVEN + HW-PROVEN on IONOS (2026-06-30)**
- Status: **ROOT-CAUSED (workflow + adversarial verification) + FIXED + ctest-clean + FAITHFUL-GATE PROVEN + LIVE-IONOS PROVEN.** gui_qso good@7 1KB MC-DPSK (ULTRA_ROBUST_IDLE_PING=1 to clear the handshake floor, DBPSK R1/4): **FILE_CRC_OK_COUNT=2, ALPHA_FILE_DONE_COUNT=1, RESULT=PASS** (was FILE_CRC_OK=0 / resend-forever / FAIL). GOODPUT=10 bps (~11 min/1KB — the #71 SPEED lever, not this bug). Pre-existing (#73); surfaced once the handshake fixes let MC-DPSK connect + the ACK-collision fix (33ccade) widened the receiver hold.
- **HW proof (2026-06-30, IONOS MPG@8 Good, Pi5→Mac, commit 9579a1a both ends, ULTRA_ROBUST_IDLE_PING=1 + ULTRA_CONNECT_RATIOMETRIC_SNR=1):** ladder correctly dropped to **MC-DPSK DBPSK R1/4** (mcdpsk_in_band effective SNR 0.9–5.1 dB read < 10 → not OFDM), **ARQ window=3 timeout=43.6 s** (the fixed RTO; old ~30 s < 37.9 s RTT), both tone-burst ACKs `bitmap=0x0` (**0 retx, 0 holes**), sender `[FILE] Transfer complete (37.4 s)`, receiver `Received OK (568 bytes, CRC ok)` → **md5 65f0ced2…b0de byte-identical**. The old build blind-resent forever here; now it finalizes cleanly. Goodput 0.12–0.20 kbps = #71.
- **NOT an assembly/flag/routing bug** (the earlier hypothesis): the receiver path is byte-correct and identical to OFDM (every delivered seq → "Processed file transfer payload"; FINAL/MORE_FRAG plumbing sound; file chunks are FrameType::DATA → `file_transfer_.processPayload`, NOT the binary DATA_START/CONT/END path). The real cause is a **sender-side ACK-RTO defect**: `computeMCDPSKAckTimeoutMs` was passed `arq_.getSackDelay()=30 ms` instead of the **6376 ms receiver tone-burst SACK hold** (that 33ccade added), AND omitted the **~16 s receiver serial-decode latency** (#56). So the RTO (30.1 s) < the measured half-duplex RTT (**37.9 s**) → the sender blind-resends the whole window before the ACK lands (all 21 retx `cause=timeout`) → doubled airtime → the FINAL chunk (~seq 32 of 33) is never reached in a bounded session → `checkAndFinalizeReceive` never fires → `file-recv=0`. This bug and BUG-MCDPSK-ACK-COLLISION are the SAME bug (33ccade widened the hold without updating the sender deadline).
- **Fix (CHANGELOG 2026-06-30):** `computeMCDPSKAckTimeoutMs` now budgets the full physical RTT (`2·tx_burst + receiver_sack_hold + ack + turnaround` ≈ 43.5 s > 37.9 s), lower clamp lifted to the physical floor; Connection computes the hold ONCE and feeds both the receiver setter and the sender RTO. Completion now reliable in an unbounded session.
- **Remaining (separate lever, NOT this bug):** SPEED. RTT ~38 s × ~11 windows ≈ **~7 min for 1 KB** (the #56 decode latency + 32-byte chunks / window=3 = ~33 seqs). Practical MC-DPSK files need #71 (DQPSK rung / bigger chunk / window). Repro: `/tmp/v72_nat7`; completion proof: `/tmp/v73_complete`.

### BUG-MCDPSK-CONTROL-BAUD: CONNECT_ACK shipped at the data rung's mod/baud → handshake strand on sps≠1024 rungs — **FIXED (#72, 2026-06-29, CHANGELOG)**
- Standardized MC-DPSK on sps=1024 + routed handshake-negotiation frames through the DBPSK control profile. Forced rungs now CONNECT; no regression. See CHANGELOG.

### BUG-FILE-ACK-IDENTITY: ARQ send-complete dispatch is identity-blind — a non-file frame retiring while a file is SENDING pops a file-chunk ledger entry / inflates chunks_acked_
- **Found 2026-07-02** by adversarial review of the requeue-ledger fix; **pre-existing in kind**
  (the same blindness inflated the old count arithmetic). `Connection::setSendCompleteCallback`
  (connection.cpp:~285) routes EVERY successful DATA-frame retirement to
  `file_transfer_.onChunkAcked()` while FileTransferState==SENDING — frame identity is never
  checked. Mixed windows are reachable because the burst-transport `sendFile` bypass
  (connection.cpp:~1407) skips the fragment-in-flight guards its sibling paths enforce, so a
  message/binary fragment already in flight shares the window with file chunks; its retirement
  mis-pops the ledger → a later requeue resumes one chunk forward (silent skip) and
  chunks_acked_ over-count can declare COMPLETE with a chunk still in flight.
- **Not exercised by any shipped flow today:** GUI operator messaging is a stub, the sim
  scenario driver drains messages before the file phase, and ultra_tnc sets
  half_duplex_interactive_=true (takes the guarded path). Triggerable via the public API
  (sendMessage/sendBinary then sendFile within the ACK RTT) or scenario configs combining
  auto-reply-message with auto-send-file.
- **Fix (structural, deferred):** identity-aware retirement — tag each ARQ TX submission with
  its origin (FILE_CHUNK / MESSAGE_FRAGMENT / OTHER) in the slot and pass it through
  `on_send_complete_(success, origin)`; dispatch by origin, not by FileTransferState. This also
  fixes the mirror fragment-starvation bug. NOTE: the tempting "guard parity" patch (queue the
  file when fragments are in flight) is STRAND-PRONE on the burst path — the queued-file pump
  (`tryStartQueuedFileIfReady`) requires `local_data_turn_`, which burst transport deliberately
  bypasses; don't take that shortcut.

### BUG-HANDSHAKE-PING-FLOOR: low-SNR PING/CONNECT classifier starves → handshake never connects below ~15 dB Good (caps ALL operation) — **FIXED, DEFAULT-ON (2026-07-01) — RE-OPENED for the mid-SNR SIM window (see re-open note)**
- **RE-OPEN NOTE (2026-07-01 evening, found by the #58 Good@12 boundary probe):** at SIM
  reference levels good@12 the PING's noise-flooded gap reads data_rms **0.23** — ABOVE the
  a81725d high-SNR-churn gate `kPingChirpLockMaxDataRMS=0.16` — so the robust chirp-lock emit
  is SUPPRESSED (chirp corr 0.66-0.75 solid, ratio 0.78 "data-bearing", path1=0 path2=0, no
  PONG, 5/5 PING timeouts, 2/2 seeds). This is the documented "0.16 is absolute → level-fragile"
  caveat biting: the gate separates rig PING gaps (0.04-0.09) from good@20 false-syncs
  (0.28-0.33), but the good@10-15 SIM window falls in between. The a81725d "PROVEN good@10/12"
  claim predates the 0.16 gate and no longer holds. Rig low-SNR (MPM@8 etc.) unaffected. Fix as
  already noted: noise-floor-RELATIVE emit gate. Until then, sim work at good@10-15 cannot
  connect out-of-box.
- **ctest reproducer (2026-07-02):** `UltraTncSimAudio` (OTASim lobby awgn@15) now FAILS on
  this window deterministically — PING gap reads data_rms 0.1653 (> the 0.16 emit gate),
  ratio 0.547 (≥ 0.5 "data-bearing"), robust emit suppressed → no PONG → "timed out waiting
  for command line". Verified pre-existing on main (fails at main HEAD with a clean tree,
  2026-07-02); NOT a live-ladder or connect-policy regression. awgn@15 sits in the same
  in-between band as good@10-15. The red ctest is THIS bug; fix = the noise-floor-relative
  emit gate above. **FIXED 2026-07-07 by STAGE 1.5 (below): PASS ×3, handshake 16 s (was
  never-completes). NOTE: it can still fail under `ctest -j4` when a GUI sim runs
  CONCURRENTLY on the same machine (real-time CPU starvation, harness artifact).**
- **STAGE 1.5 (2026-07-07, LANDED — the mid-SNR window CLOSED):** the noise-floor-RELATIVE
  emit gate the entries above called for. `gap_is_noise` = IN-BAND gap RMS (same 101-tap FIR
  as `IdleNoiseSNREstimator`) ≤ the receiver's own measured idle floor × 1.5 — level-invariant
  (#74-safe) and high-SNR-safe by construction (a real payload rides sqrt(1+SNR_lin) above the
  floor; the good@20 false-sync class reads ~10×). TWO WRONG CUTS documented for posterity:
  (a) comparing the RAW-domain gap vs the in-band floor never fires (broadband ambient reads
  ~2.9× the in-band floor); (b) comparing raw-vs-raw fires but COMPRESSES the payload
  discriminant to sqrt(1+SNR·B_band/B_tot) ≈ 1.49× at good@10 → misclassified a real CONNECT
  as a PING (caught in sim). In-band-vs-in-band is the only correct domain. Also: the CW0-peek
  wait gate honors gap_is_noise (no more multi-second 4-CW waits per flooded probe), and
  sync-found feeds the estimator the pre-chirp ambient prefix (SyncResult.preamble_start_sample,
  boot-only) so a first-pass responder has a floor reference. This RETIRES the never-implemented
  STAGE2 `bare_chirp_expected_` plan as the gate for the robust emit (the noise-relative test
  subsumes it). PROVEN: sim good@10 out-of-box CONNECT+PASS (QPSK R1/4, CRC ×2), good@20 PASS
  2070 bps 0 spurious, UltraTncSimAudio PASS ×3.
- Status: **FIXED + DEFAULT-ON (rig path); mid-SNR sim window re-opened by the emit gate.** `ULTRA_ROBUST_IDLE_PING` promoted DEFAULT-ON (opt-out `=0`) after all three sub-issues were closed: (1) initiator #27 (bare_chirp_expected_=FALSE during CONNECTING), (2) responder starvation (STAGE2 expects-CONNECT window), (3) high-SNR churn (data_rms≤0.16 emit gate). VERIFIED: good@20 sim PASS with the emit active (2/2 PING/PONG, 1860 bps); rig **MPM@8 pure-default (zero env vars) connects + delivers CRC-clean** (2/2). With #74 (ratiometric) also default-on, the low-SNR path is now the shipped default on a real radio. See CHANGELOG 2026-07-01.
- **What:** a PING is a bare chirp with no data (`encodePing`→`generatePreamble`). The receiver tells a PING from a CONNECT with a LEVEL test (`data/training RMS ratio < 0.5`, abs floor 0.16). At low SNR broadband noise floods the PING's silent gap (ratio 0.68–0.88 > 0.5) → real PING reads as a faded CONNECT → waits for a 4-CW frame that never comes → no PONG → never connects. The chirp itself locks solid (corr 0.6–0.75). Floor map (faithful gate): never connects awgn@6/8 good@8/10/12; marginal good@15; reliable good@20. The published 5 dB AWGN *data* floor never caught this (`measure_ack_fer` skips the live handshake).
- **Fix (env-gated):** when the knob is ON, a solid chirp-lock with a low-LLR (false-lock-rejected) frame emits the PING on chirp signature alone; gated on `bare_chirp_expected_` (FALSE during CONNECTING) so a faded CONNECT_ACK isn't mis-PONGed (#27-safe on the initiator). PROVEN: good@10/12 never-connect→PASS, good@20 no-regress.
- **STAGE2 (2026-07-01, LANDED — responder hole CLOSED):** on PONG-TX the responder now sets `bare_chirp_expected_`=FALSE + arms a ~20 s expects-CONNECT window (app.cpp), mirroring the initiator's PROBING→CONNECTING disarm, then the tick re-arms. Sound for any window: while CONNECTING the initiator re-SENDS CONNECT (never re-PINGs, connection.cpp:2421-2442) and a stray PONG to a CONNECTING initiator is a no-op + half-duplex-inaudible, so worst case is occasional harmless waste, never permanent starvation.
- **Remaining before default-ON (RESOLVED 2026-07-07 by STAGE 1.5 — kept for history):** flipping `ULTRA_ROBUST_IDLE_PING` default-ON REGRESSES the faithful sim gate at good@20 — the near-silent HIGH-SNR PONG's residual pushes the data/train ratio just above 0.5 (data_bearing) with low LLR + a real chirp, so the robust emit fires SPURIOUSLY → PING/PONG churn (11/9) → ~30 s connect → gate overrun. Isolation-proven: same build with `=0` → good@20 PASS (1860 bps); STAGE2 alone is clean. FIX NEEDED: a high-SNR-safe emit gate (e.g. a higher data_bearing floor separating a noise-FLOODED low-SNR PING (ratio 0.68–0.88) from high-SNR PONG residual (~0.5–0.6)). Also: throughput below ~8 dB is a separate lever (#71), not this bug.

### BUG-IONOS-PI5-CHEAP-DAC: Pi5→Mac handshake one-way — cheap-card carrier JITTER (root cause REFINED 2026-06-15) — partial software mitigation landed
- Status: **OPEN (hardware), root cause REFINED + software mitigation landed 2026-06-15.** First
  Mac↔IONOS↔Pi5 bringup. After the CONNECT-decode code fix (CHANGELOG 2026-06-14), Mac→Pi5 completes
  (CONNECT decodes 4/4, Pi5 CONNECTED) but Pi5→Mac CONNECT_ACK never decodes → initiator never
  CONNECTED → no file transfer.
- **UPDATE 2026-06-16 — when the handshake DOES complete (Mac→Pi5, MPG), the FILE TRANSFER stalls,
  and the proximate mechanism is now isolated from the real receiver log (`/tmp/pi5_full.log`):**
  the chirp anchor decodes but burst data frames 2–6 are ERASED every group at broadband RMS
  0.0038–0.0145 (all under the absolute `BURST_ERASURE_RMS_THRESHOLD=0.015`) → all-zero-LLR group →
  `header invalid` → ARQ retransmit → stall. The 0.015 gate was implicitly "~25 dB below the SIM
  anchor (0.27)"; at the cheap card's ~6× lower RX operating level it became only ~5 dB below the
  anchor and erased recoverable frames. **FIX A landed (UNCOMMITTED, see CHANGELOG 2026-06-16):** the
  gate is now operating-level-RELATIVE (`max(0.055*anchor, 0.005)`), zero sim regression, keeps 16/20
  of the erased frames on IONOS. **Hardware note (user, 2026-06-16): the MPG logs are the CLEAN Fe-Pi
  HAT, not the old USB card** — so the deficit is codec-INDEPENDENT (high-PAPR data below the anchor +
  lower RX operating level + Good fade nulls), and on a clean codec the KEPT frames should DECODE, so
  FIX A is likely the actual fix (the cheap-card per-carrier-LLR / `CHEAP_CARD_ROBUSTNESS_PLAN.md` work
  applies only to the old USB card). Re-test Mac→Pi5 MPG with `ULTRA_BURST_RMS_DIAG=1` to confirm the
  anchor/level + that frames are KEPT and decode; if marginal, raise RX gain and/or use R2/3 (the
  morning run negotiated the fragile R3/4). Decisive proof is rig-only.
- **UPDATE 2026-06-15 — the "bad clock, swap the card" diagnosis was RE-EXAMINED and largely REFUTED;
  the real lever is carrier JITTER, and a software mitigation now recovers the slow-jitter regime.**
  In-sim impairment discrimination through the real encode→AWGN→decode path (`test_mcdpsk_clock_offset`,
  CHANGELOG 2026-06-15) showed: (1) sample-CLOCK offset is NOT the bottleneck — R1/4 LDPC decodes CRC-
  clean to ±1000 ppm with or without correction, and the prior "−1800..−3000 ppm wandering" figure was a
  measurement artifact (a crystal cannot wander ~1200 ppm run-to-run; the values match an integer ALSA
  buffer-period drop = USB starvation on the headless Pi); (2) band TILT is NOT the bottleneck (frequency
  diversity + LDPC ride through 20 dB); (3) the cheap DAC's ±7 Hz carrier JITTER IS the bottleneck (it
  exceeds the DQPSK ±45°/symbol decision margin → payload garbage, chirp survives). The MC-DPSK demod now
  has clock-offset + carrier-jitter tracking (default on) that recovers SLOW jitter and is a proven no-op
  / no-harm otherwise. So this is no longer purely an equipment fault — re-test with the current build
  before concluding a card swap is needed.
- **LIVE IONOS BRINGUP 2026-06-15 — DEFINITIVE ISOLATION (the re-test above, run on real hardware).**
  Worked the full Mac↔IONOS↔Pi5 path knob-by-knob and eliminated everything except the card:
  - **Wiring:** one direction (Pi5→Mac) was dead (Mac RX rms 0.0004 = digital silence) until a CABLE
    SWAP — then both directions carried signal. (So part of the original failure was a bad cable, not
    the modem at all.)
  - **IONOS input CLIPPING (the big one):** the Pi5's transmit was overdriving the IONOS input — the
    panel read `Lvl=2000 mvp-p` RED with `CF=1.01 [0.05 dB] (PEP/Pavg)` = the multi-carrier signal
    arriving as a HARD-CLIPPED SQUARE WAVE (a clean MC-DPSK signal is CF≈10 dB). That clipping, not the
    DAC, was garbling the CONNECT_ACK at high level. Gain staging: the IONOS CH-IN gain (factory default
    1; had been run at 5–20) and the **Pi5 ALSA `Speaker` volume are the real level knobs** — see tx_drive
    note below. Nominal IONOS input max is 1800 mvp-p; the modem's ~10–12 dB crest factor means target the
    *average* well under that (peaks clip first).
  - **CONNECT_ACK→PING misclassification (software, FIXED):** once clipping was removed, the Mac decoded
    the low-level CONNECT_ACK to garbage AND then mis-classified it as a PING via the
    `pre_ldpc_llr_reject` path (ldpc_attempted=false short-circuits `ping_by_chirp_lock`), firing BEFORE
    the 06-14 ratiometric 4-CW wait gate. FIXED (CHANGELOG 2026-06-15): a ratiometric guard lets a
    data-bearing (ratio≈1) MC-DPSK frame fall through to the full 4-CW decode instead of being pinged.
    HW-confirmed the Mac now attempts the 4-CW decode (`got 2592 soft bits`); PingDetector + 16/16
    regression still pass.
  - **Level matched, decode STILL fails → THE CARD.** With wiring, clipping, and classification all
    fixed, swept the Mac's RX: at 0.08 (low), **0.16 (matched to the Pi5's working 0.17), and 0.20**, the
    4-CW CONNECT_ACK decodes to GARBAGE every time — while the SoundBlaster→Pi5 CONNECT decodes fine at
    0.17. Same level, clean, green, no clipping; only the transmitting card differs. **Definitive: the
    cheap Pi5 USB dongle's TX is the limit** (its measured tilt+distortion+jitter), beyond what the
    clock/slow-jitter trackers recover.
  - **tx_drive is NOT a software bug (correction of an in-flight claim).** On the Pi5, changing tx_drive
    (0.10/0.20/0.70) barely moved the level — but the AUDIO-category log shows the per-burst hardware
    normalization is APPLIED, not bypassed (no `normalization bypassed fragment` warning), and tx_drive
    DID work on the Mac (0.7 clipped to 2000). So the software is correct; the Pi5 cheap card's analog
    output just doesn't respond linearly to the digital level (saturation/compression — same hardware
    trait as the decode distortion).
  - **VARA paradox / forward path:** the user runs this exact dongle with a commercial HF modem, so the
    card is usable by a *robust-enough* handshake. Our 4-CW DQPSK R1/4 CONNECT_ACK simply isn't as
    robust. Two paths: (a) swap to the SGTL5000 I2S HAT (on hand) for an immediate working QSO;
    (b) earn cheap-card tolerance in software — DBPSK control frames (double the phase margin vs jitter)
    + per-carrier SNR weighting (handle tilt) + more control-frame FEC. See docs/CHEAP_CARD_ROBUSTNESS_PLAN.
- **Root cause (tone-test measured, /tmp/measure_pi5_tx.sh + measure_mac_tx.sh + analyze_tone.py):**
  the Pi5's cheap "USB Audio Device" **DAC/playback** is the problem. Same IONOS, same two cards,
  TX/RX roles swapped — only the transmitting card differs:
  | metric | Pi5 cheap card TX (fails) | Mac SoundBlaster TX (clean) |
  | band flatness across 0.5-2.6 kHz | **14.8 dB tilt** | 5.7 dB |
  | 2nd-harmonic distortion | **-17.8 dB** | -36.7 dB |
  | freq accuracy (jitter) | **±7 Hz** | ±0.5 Hz |
  The cheap DAC's 15 dB band tilt starves the edge MC-DPSK carriers + the distortion/jitter smear the
  DPSK phase → the multi-carrier 4-CW payload fails to decode, while the robust band-sweeping chirp
  shrugs it off (CFO≈0, chirp decodes) — hence PING works, CONNECT payload doesn't. The cheap card's
  **ADC/record is fine** (reverse dir clean), so it's specifically the TX path.
- Of the three measured TX impairments, the discrimination (above) shows tilt is absorbed by diversity
  and a stable clock offset is absorbed by LDPC; the **jitter** is what crosses the DQPSK margin. The
  measurement was also taken on the Pi5 host, so part of the apparent jitter/tilt may be Pi-side USB
  starvation rather than the DAC itself (re-measure driving the cheap card from an unloaded host to
  separate DAC-intrinsic defects from host starvation — recommended experiment).
- **Fix order (revised 2026-06-15):** (1) re-test Pi5→Mac with the current build — the new MC-DPSK
  clock+jitter tracking may already close it if the real jitter is slow; (2) if it still fails, capture
  a Pi5-TX CONNECT recording and decode it offline to confirm jitter rate vs an ALSA-drop discontinuity;
  (3) swap the Pi5's cheap USB soundcard as a BACKSTOP only if the residual jitter is fast/large enough
  to be below the trackable regime. Earlier "balance the L/R levels" framing was wrong/incomplete. HW
  knobs the bringup harness sets: Pi5 `callsign=PI5`, `tx_drive=0.7`, ALSA Speaker −3 dBFS; Mac
  `tx_drive=0.7`, input vol 60. Run the gui_qso gate LID-OPEN on an unloaded machine (lid/throttle →
  ALPHA audio starvation `SKIP unsearched < min` → false FAIL).

### BUG-TNC-B2F-002: post-burst non-burst FF frame not delivered (blocks bulk-accum-to-burst B2F) — LAYERED
- Status: **FIXED 2026-06-05.** ROOT CAUSE (LAYER 2, finally isolated): the ENCODER did not revert
  its LDPC lifting-z after a burst. A burst lifts the encoder to z=81 (`setLDPCLiftingZ(81)` in
  transmitBurst); nothing reverted it, so the next non-burst frame on the `transmit()` path (the FF
  terminator, a chat line, an SR-ARQ repair) was still encoded at **z=81 (~106 880 samples)** while
  the receiver — which DOES revert at group-end — decoded it as **z=27 (~17 920 samples)**. BRAVO
  read the first ~17 % of a z=81 frame as a z=27 frame → saturated-magnitude/random-sign LLRs
  (`|llr|=20`, `llr_avg≈0`) → LDPC 0/CW → never ACKed → stall. Fix: revert the encoder to z=27 for
  every non-burst frame (`modem_engine.cpp` transmit() dispatch). Plus the TNC accumulation was made
  spec-correct (honest VARA `BUFFER` — see CHANGELOG 2026-06-05) so the body no longer striped across
  burst+interactive transports (the out-of-order corruption). 20 KB JPEG now delivers byte-identical
  with a CLEAN teardown on the plain (no-bulk) path, GUI-verified. Layers, peeled in order:
  - LAYER 0 (sync acquisition): FIXED 2026-06-04 (catch-up drain + full-anchor buffer, CHANGELOG).
  - LAYER 1 (§14.24 gate drop): **FIXED 2026-06-05** — `have_burst_descriptor_` now drops at
    group-end (`finalizeBurstGroup`, streaming_burst_interleave.cpp) instead of persisting the whole
    connection, so the trailing FF is no longer gated as mid-burst. The FF now REACHES the decoder
    (`Chirp detected … escalating to N CWs`). Verified no multi-group regression: gui_bidir AWGN@20
    7/7 each way, 0 CWfail.
  - LAYER 2 (post-burst mode-revert — **REMAINING**): the FF demodulates to garbage — saturated
    magnitude, random sign (`CW FAIL, llr_avg≈0.3, |llr|=20`). RULED OUT as root (traced 2026-06-05):
    NOT a buffer/coordinate bug — `process()` calls `processPresynced(samples, 2)` which skips 2
    training symbols from the buffer START, and `sync_position_ = search_start + start_sample` points
    AT the training (the `training_start=63839` in the log is only the CFO phase ref, and cfo=0 here);
    NOT the CW-count (2 vs sent 1) — that fallback is a SYMPTOM: the CW0 header peek can't read a
    valid header BECAUSE the demod is already garbage. ROOT: post-burst, BRAVO does not fully revert
    to NON-burst interactive mode. The FF is a small non-burst interactive frame that on the normal
    (no-preceding-burst) path delivers fine via LIGHT-LTS + the proven non-burst decode; but here it
    is sent with a FULL preamble (post-burst `forceNextFrameFullPreamble` / `expect_full_ofdm_anchor_`
    re-anchor) and decoded through the full-anchor burst-data path, which mis-estimates/mis-structures
    it → garbage soft bits. LAYER 1's latch-clear reverted ONE piece of burst state
    (`have_burst_descriptor_`); the remaining burst-data-mode state (the full-anchor expectation, the
    data-decode profile, `fixed_frame_codewords_`) is not reverted, so the trailing FF is treated as
    burst data. Fix = complete the post-burst revert so the trailing interactive frame decodes as a
    normal non-burst frame — a COORDINATED encoder+decoder change (both ends must agree the burst is
    over and drop back to the light-LTS interactive PHY). This is the production form of the
    declared-but-unwired `traffic_profile.hpp` TrafficClass system (File vs Chat/Control). Deliberate
    work, must not regress the burst path; do not land blind against hanging-run verification.
  - LAYER 2 SIDESTEPPED (2026-06-05): rather than fix the broken non-burst full-anchor demod, the
    TNC now ROUTES the trailing FF through the BURST path too (`bulk_burst_started_`,
    tnc_session.cpp): once a body has bursted, trailing flushes stay burst, reusing the PROVEN
    burst descriptor+group decode (same path that delivers the body 10/10). This made the image
    deliver byte-intact over burst.
  - LAYER 3 (clean teardown — **REMAINING**): the FF is a tiny (3-byte) 1-frame burst. It DELIVERS
    (BRAVO `Received OK (3 bytes)`), but its tone-burst GROUP-ACK does not get back to ALPHA
    (`group-ACK timeout`), so ALPHA's `file_transfer_` stays SENDING/busy. The next trailing flush
    then fails `sendFile` "already in progress" and the bulk-accum retries every quiet period (~65
    wasted re-stages of the same 3 bytes) → the sender's `connect` never exits cleanly (the message
    IS delivered; only the sender-side close hangs). Root: tiny / turn-flip-boundary 1-frame burst
    groups don't ARQ-complete on the sender side (related to the old "Issue 2 turn-flip" ACK
    reliability). Fix candidates: (a) make tiny/last-group tone-burst ACKs reliable at the turn
    boundary; (b) coalesce body+FF into ONE burst (under-report BUFFER 0 early so PAT writes the FF
    before the body transmits, then burst both — risk: premature Flush close); (c) the LAYER-2 PHY
    fix so the FF can go non-burst. An idle-gated flush (only flush when `getTxBackloggBytes()==0`)
    was added but does NOT cover this (a tiny burst reports ~0 remaining bytes while file_transfer_
    is still busy on the ARQ).
- Symptom: with `ULTRA_TNC_BULK_ACCUM=1`, a PAT B2F body now bursts (z=81) and decodes CRC-clean,
  but the trailing non-burst `FF` terminator never reaches PAT (`Receiving … offset 0`, mailbox
  empty). PAT retries the whole message (`100%→0%→100%`) → the run HANGS instead of disconnecting.
- Root cause: after acquiring the post-burst full-anchor chirp (now succeeds, `up_pos` small),
  BRAVO does a control-first peek (1 CW) then processes the frame with the **burst-data geometry**
  it's still holding — `samples=10080` (burst frame size), reconfigures to the QPSK R1/2 *data*
  profile — but ALPHA's `FF` is a NON-burst frame (29 B → 106880 samples, full preamble, ~2 CW at
  R1/4). Geometry/profile mismatch → the FF never completes → not routed to the data port. The
  plain non-burst path WORKS when NOT preceded by a burst (the pre-bulk-accum image delivered),
  so the burst-left decoder state (`have_burst_descriptor_` set per group at
  `streaming_ofdm_decode.cpp:762`, the burst frame geometry, warm-sync next-group expectation) is
  what mis-decodes the trailing non-burst frame. There is no end-of-burst signal, so the receiver
  stays in burst-decode mode.
- SURGICAL root cause (fully traced 2026-06-05, with debug logging since reverted):
  1. The BODY delivers fine. Confirmed via temporary logs in `onFileReceived`/`onModemDataReceived`:
     the burst body decodes CRC-clean and BRAVO-TNC delivers all 12215 bytes out its data port to
     PAT-BRAVO (`state=CONNECTED, marker=raw, data_out=1`). The POLLOUT flush is correct. In B2F the
     sender writes body → `Flush()` → THEN the `FF` terminator, so PAT-BRAVO HAS the body and is
     waiting only for the FF (its `offset 0` line is just stale; VARA `Read` has no deadline so it
     waits forever, no timeout).
  2. The FF (a 1-CW non-burst frame after the burst) is DROPPED at the §14.24 gate
     `streaming_ofdm_decode.cpp:1013-1022`: `if ((use_burst_interleave_ || burst_transport_rx_) &&
     connected_ && have_burst_descriptor_) { re-search; return; }`. The control-first peek decodes
     the FF's CW0 at R1/4-control profile → garbage (the FF is DATA, not control) → reaches this
     gate → re-searched away.
  3. THE LATCH: `have_burst_descriptor_` is set on the first BURST_HEADER
     (`streaming_ofdm_decode.cpp:762`) and PERSISTS for the WHOLE connection — by deliberate design
     it is cleared ONLY on disconnect (`streaming_decoder.cpp:537-551`; it is the RX source-of-truth
     for the descriptor-declared z=81, must survive across groups+ACKs within a transfer). So after
     the body burst it stays set forever and the gate drops EVERY post-burst non-burst frame.
- Fix direction (NOT done — deliberate cross-engine change, must not regress §14.24 / z-lifecycle):
  clear `have_burst_descriptor_` when the burst FILE TRANSFER COMPLETES (the clean "burst is over"
  signal — preserves in-burst z-sizing). The completion signal lives in the ProtocolEngine
  (`FileTransferController::on_received_` → `Connection::setFileReceivedCallback`), but the latch
  lives in the ModemEngine's `StreamingDecoder`; there is no existing ProtocolEngine→ModemEngine
  path, so this needs new wiring (e.g. a `StreamingDecoder::clearBurstDescriptor()` invoked from the
  file-received callback at the app/TNC layer that holds both engines). Do NOT shortcut with
  `&& waveform_->wasBurstInterleaved()` on the gate — mid-burst noise has a ~50/50 marker and would
  fall through to the data decode, re-introducing the §14.24 shared-estimate poisoning.
- BONUS bug found: bulk-accum (BRAVO side) also hoards PAT-BRAVO's small CONTROL sends (BUFFER 50 on
  its SID/FS+) — the cap should apply only to bulk bodies, not tiny control writes.
- Repro: `ULTRA_TNC_BULK_ACCUM=1 /tmp/b2f_pat/run_image.sh` (BOUND with a timeout — it HANGS, PAT
  retries 100%→0%→100% forever). Diagnostics: VARA_DEBUG=1 on PAT for its data RX;
  `ULTRA_S16_TRACE_WARM_WINDOW=1` + BRAVO `--log-category …,sync`; mailbox
  `/tmp/pat_bravo_mbox/BRAVO/in/*.b2f`.

### BUG-TNC-B2F-001: bidirectional B2F stalls — the NON-BURST short path (the actual message path) was dead at RX + ACK
- Status: **FIXED for the message path** (2026-06-03). Issues 1, 3, 4 fixed; Issue 2 (burst
  turn-flip) remains but is **off the B2F message path** (Winlink messages are small → non-burst).
- Symptom: PAT↔PAT Winlink message over `ultra_tnc`/OTASim connects + completes the B2F
  handshake, then never delivers. **Reproduces single-machine** (shared clock) → NOT a
  cross-machine timing problem. (Earlier framing chased the burst path because an A/B test
  forced `kInteractiveMaxBytes=0`; the DESIGN routes small messages to the non-burst path.)
- Root cause (Issue 1, fixed `c27aa45`): the one-way burst bypass skipped the ISS/IRS turn
  gate → both B2F stations keyed up uncoordinated and collided. Fixed via `half_duplex_interactive_`
  turn-gating. Serialization verified correct.
- **Root cause (Issue 3, fixed 2026-06-03): the receiver synced cleanly (corr=1.0, SNR 28 dB)
  yet DROPPED every non-burst DATA frame.** When `burst_transport_rx_` became the unconditional
  default (ULTRA_BURST_TRANSPORT removed 2026-06-02), the §14.24 control-peek-fail re-search at
  `streaming_ofdm_decode.cpp:~1004` started discarding ALL non-burst multi-CW DATA as
  "burst-regime noise": a non-burst `sendBinary` frame carries no BURST_HEADER descriptor →
  `pending_total_cw_=0` → it enters the 1-CW control peek, fails it, and was re-searched away.
  Fix: gate the re-search on `sync_controller_.have_burst_descriptor_` (only mid-burst, where a
  real burst's SHARED channel estimate is at risk); a standalone non-burst frame falls through
  to the legacy data decode → delivered via SR-ARQ.
- **Root cause (Issue 4, fixed 2026-06-03): the responder ACK'd in the wrong waveform.** The B2F
  responder pre-confirms `handshake_confirmed_` in `enterConnected` (so it can speak first),
  which skipped the only site that fires `on_handshake_confirmed_()` → the modem's
  `handshake_complete_` stayed false (reset by `setConnected()`, with `setWaveformMode`→OFDM
  landing afterwards), so BOB keyed its SR-ARQ ACK in **MC-DPSK** while ALICE listened in OFDM
  → ACK never landed → ALICE retransmitted to the retry cap. Fix: re-fire `on_handshake_confirmed_()`
  once on the responder's first decoded frame (guaranteed past `setConnected`+`setWaveformMode`),
  switching TX onto the negotiated OFDM data waveform.
- Verified: `tests/test_ultra_tnc_sim_audio` (two real `ultra_tnc --sim-audio` over OTASim).
  Default = bulk burst file (8192 B, CRC-clean — no regression). `ULTRA_TNC_TEST_NONBURST=1` =
  bidirectional 300 B short message, **CRC-clean BOTH directions** (was: forward dropped entirely).
- **End-to-end VERIFIED (2026-06-03):** real PAT↔PAT Winlink B2F, single machine
  (OTASim → 2×`ultra_tnc` → 2×PAT). `pat connect varahf:///BRAVO` ran the FULL exchange clean —
  banner → proposal → `FS +` → body 0%→100% → `FF`/`>FQ` → disconnect — and the message landed in
  BRAVO's inbox. The prior session stalled at the proposal; now it delivers.
- Root cause (Issue 2, still open): the BURST path needs full chirp+LTS **anchor re-acquisition
  on every turn-flip**; a burst transfer whose turn flips mid-stream re-acquires at corr~0.27.
  Not on the message path; relevant only to bidirectional *bulk* over one connection.
- Full diagnosis + repro: `docs/TNC_B2F_HALFDUPLEX_FINDINGS_2026_06_03.md`.

### BUG-FINACK-001: Final-group ACK loss → sender infinite-resends the last group; no clean transfer close
- Status: **FIX LANDED, UNVALIDATED** (2026-06-02). Decode-independent re-ACK implemented
  in `connection.cpp::onBurstGroupReceived` (the `!all_ok` else branch now routes a resent
  ALREADY-DELIVERED group into the controller's existing `onGroupReceived`→`seqLess`→re-ACK
  path instead of dropping it). Builds; burst/ARQ unit tests pass (StopWaitARQ,
  SelectiveRepeatARQ, ConnectionAdaptive, ToneBurstAckMonitor/Watterson). **NOT yet validated
  on a triggering run** — needs a fade-timed seed where the final GROUP_ACK is genuinely lost
  so the re-ACK actually fires and the transfer closes cleanly on-air (re-ACK fired 0× on the
  post-fix runs because their final ACK happened to land). PRODUCTION-VISIBLE: the GUI/sweep
  quick-kill (scenario_passed now PASSes on delivery + pkills) MASKS this in the harness, but
  real hardware will still show the wasted-airtime infinite last-group resend until validated.
  Was OPEN since 2026-05-31, folded into thread C (`docs/OFDM_COHERENT_ONLY_DECISION_2026_05_31.md §5`).
  Pre-existing; exposed by a fade landing on the completion ACK.
- **Still TODO for the robust fix:** a FILE_END / completion handshake (FILE_END → FILE_END_ACK
  → DISCONNECT) so the close doesn't depend on a single per-group ACK landing at all. The re-ACK
  breaks the infinite loop; the handshake is the belt-and-suspenders close.
- Area: burst completion handshake — `Connection` GROUP_ACK path + the "payload drained →
  auto-disconnect" trigger; receiver duplicate-group handling in `onGroupReceived` / burst assembler.
- Reported by operator (live GUI), coherent QPSK R1/4 Moderate@14 seed 777:
  "it received the file and alpha is still sending."
- Repro / evidence (`/tmp/zmode_mod14_qpsk_s777b`): Bravo logs `Received …10240 bytes, CRC ok`
  at 282 s (file COMPLETE). Alpha sits at `cursor=10240/10240, resent=1, resend_left=0` and
  re-emits `TX Burst: 6 frames` for `group_seq=18` every ~10 s (ack_timeout) indefinitely; the
  scenario only ends via `exit_after`. seed 42 escaped it solely because its final ACK happened
  to land and `auto-disconnect (payload drained)` fired.
- Root cause: the completion handshake has no robust close. The last group's **GROUP_ACK was lost
  in a fade**, so the sender never learns the receiver has it and resends forever; "payload drained
  → disconnect" never triggers because the sender still considers the final group unacked. When the
  sender resends an **already-delivered** group, the receiver does **not re-ACK** — it tries to
  re-decode the duplicate (which here also hits the fade-lost-descriptor z=27 path →
  `0/6 max_iters=0`, a red herring) and stays silent. The file delivered fine; only the close fails.
- Impact: on real hardware, wasted airtime (sender keeps keying the last group) and a transfer that
  never cleanly terminates without an external timeout. Half-duplex final-ACK fragility.
- Fix (thread C): group-level **duplicate detection + re-ACK** — a resend of an already-delivered
  group re-emits the GROUP_ACK **without** re-decoding. Plus a proper **FILE_END / completion
  handshake** so the transfer terminates on "receiver has the whole file" (FILE_END → FILE_END_ACK →
  DISCONNECT), not on a single per-group ACK happening to land. (Note: §7.6 z-consolidation would
  *partly* help IF the re-ACK were decode-gated — but the robust fix is decode-independent.)
- Verification: a fade-timed seed where the last GROUP_ACK is lost (seed 777 Moderate@14) must still
  reach a clean DISCONNECT (no infinite group-18 resend); `ALPHA_DISCONNECTED_COUNT>0`, RESULT=PASS.

### BUG-CTRL-001: Control path is still the bottleneck in aggressive fading profiles
- Status: IN_PROGRESS (handshake leg fixed 2026-04-26)
- Area: OFDM connected mode (ACK/SACK/control reliability)
- Symptoms:
  - Data codewords decode but ACK reception misses trigger avoidable retransmits/timeouts.
  - Most visible with aggressive profiles (for example D8PSK R1/2), but can still appear on weaker seeds in other OFDM rates.
- Impact:
  - Throughput tail collapse on bad seeds.
  - File transfer latency variance much larger than message transfer variance.
- Current mitigations already in tree:
  - R1/4 control profile for OFDM control frames.
  - ACK repeat/coalescing and improved ARQ observability.
  - `DISCONNECT_SEQ` protection against stale data ACK being mistaken as disconnect ACK.
  - **Proactive CONNECT_ACK retransmission (responder side)** — covers handshake-leg
    losses at OFDM data modes. Auto-mode baseline at SNR=15 moderate (DQPSK R1/2)
    went from 4/5 → 5/5 message tests and 2/3 → 3/3 file 2048 tests on 5/3-seed
    samples. See CHANGELOG 2026-04-26.
  - **Airtime-derived ACK/retransmit RTO (commit `d182751`, 2026-05-23, backlog #119).**
    Replaced the `[8000,12000]` magic timeout clamp with an RTO derived from burst
    airtime + carrier-sense/SACK coalesce + ACK airtime, so the sender no longer
    times out before a half-duplex-deferred ACK can physically return. Eliminated
    premature timeout-retransmission of already-delivered frames on a clean channel
    (AWGN SNR20 16QAM R1/2: 5/10 seeds → 0/10). See CHANGELOG 2026-05-23. NOTE: a
    longer honest RTO may add idle wait at window boundaries on clean channels —
    quantify under #121 (recoverable dead air) before further RTO tuning.
- Remaining work:
  - Connected-mode tail variance under aggressive forced profiles (D8PSK R1/2,
    DQPSK R3/4) when channel falls outside auto-selector envelope — these aren't
    auto-selected, so they bite only operators forcing modes manually.
  - BRAVO missing the initiator's CONNECT (opposite leg of the same race) is rare
    on the production envelope but lacks a comparable fast retry — initiator's
    `connect_timeout_ms = 60000` is far longer than the cli_simulator harness's
    30s PHASE 1 budget, so harness exposure of this case looks like seed noise.

### BUG-NACK-001: Burst GROUP_NACK sent over MC-DPSK handshake waveform, not the tone-burst
- Status: FIXED (2026-05-29, branch feat/oneway-arch-2026-05-27, commit pending). Verified
  seed 2 Good@20 warm-ON: tone-burst NACK emits=1, MC-DPSK NACK=0, MC-DPSK TX post-connect=0,
  file PASS 11/11 CRC-clean, run ~34 s faster (171 s vs 205 s).
- Area: burst transport failure path (`Connection::onGroupReceived`, the `!all_ok` branch)
- Reported by operator (waterfall observation), seed 2 Good@20 file transfer:
  - "Bravo issues a NACK using the old NACK system, not the tone-burst."
  - "After group 0's initial failure there is a weird MC-DPSK signal repassing on the waterfall."
- Root cause (BOTH symptoms are one bug): on a failed group the receiver sent
  `transmitFrame(makeGroupNack(...))`, a 20-byte control frame over the CURRENT
  waveform. For group 0 `handshake_complete_` is still false (it is set later, in
  the `all_ok` path the NACK branch `return`s before reaching), so the frame went
  out as the **MC-DPSK handshake waveform = 149760 samples ≈ 3.1 s** on the air —
  the "weird MC-DPSK signal" — instead of the §15 tone-burst (675 ms). The §15
  work routed the success GROUP_ACK to the tone-burst but left the failure
  GROUP_NACK on the legacy control-frame path.
- Impact:
  - ~4.6× slower NACK (3.1 s vs 675 ms) → slower deep-fade resend recovery (a chunk
    of seed 2's latency); off-waveform MC-DPSK energy mid-OFDM transfer.
- Fix: emit a NACK-type tone-burst (`AckType::Nack`, whole-group missing mask) from
  the `!all_ok` branch, mirroring `setSendGroupAck`. The sender's `onToneBurstAck`
  already maps a NACK-type tone-burst to `burst_transport_.onGroupNack` (resend
  now), so the entire receive path already existed — only the emit was wrong.
  Falls back to the OFDM `makeGroupNack` frame if the tone-burst callback is absent.
- Verification: seed 2 warm-ON GUI re-run — expect 0 MC-DPSK TX bursts after
  connect and tone-burst NACK emits > 0, file still CRC-clean.

### BUG-8PSK-001: Decision-directed tracking corrupts the 8PSK channel estimate on fading
- Status: FIXED (2026-05-29, channel-adaptive DD gate). DD cascade removed; see
  `docs/8PSK_GOOD_FADING_DIAGNOSIS_2026_05_29.md` and
  `docs/SYSTEM_PICTURE_FADE_SURVIVABILITY_2026_05_29.md`.
- Fix: `use_coherent_dd` now also requires `last_fading_index < 0.15`
  (`channel_equalizer_pilot.cpp`) — DD off on frequency-selective/faded frames
  (where its wrong decisions poison H), on for AWGN/flat frames (where it's
  safe). Threshold from measured data (AWGN ≤0.07, Good ~0.34 median) and equal
  to the existing LLR-scaling "faded" boundary. Env override `ULTRA_DD_FADING_MAX`
  (default 0.15); `ULTRA_COHERENT_DD_OFF=1` force-off. A per-symbol pilot-anchor
  innovation gate was tried first and removed — ineffective (a wrong-decision
  rotation and a legit between-pilot interpolation error are indistinguishable
  per-symbol; flat across a 4× tightness sweep).
- Verification: GUI 8PSK AWGN30 PASS 2330 bps 0 CW fail (DD stays on, no
  regression); Good@20 cascade gone (DD-on 83–125 CW fails/no delivery →
  adaptive: seed 42 PASS, seed 43 delivered CRC-clean). Offline (measure_ack_fer
  qam8 Good@20): adaptive == DD-off (46/120 chunks) vs DD-on 41/120.
- RESIDUAL (separate, NOT this bug): 8PSK is too marginal a rung for Good@20 —
  delivers slowly with heavy resends and fails on hard fades (seed 44: 0
  delivery). QPSK is ~2× more survivable there (offline 106/150 vs 52/150
  chunks). This is a rung-selection design item (adaptive mod choice per
  channel), tracked in `docs/SYSTEM_PICTURE_FADE_SURVIVABILITY_2026_05_29.md`,
  not a DD bug.
- Original diagnosis (root-cause history):
- Area: `src/ofdm/channel_equalizer_pilot.cpp` (`use_coherent_dd`, ON for QAM8/QAM16)
- Symptom: forced 8PSK (QAM8) R3/4 delivers perfectly on clean AWGN (2330 bps) but
  fails on Good@20 fading — heavy resends, frequently no delivery, confident-WRONG
  bits (strong |llr|≈15-20, LDPC parity fails 50-99 unsat). Bimodal per group (6/6
  or 0/6, never partial).
- Root cause (experimentally confirmed by five-way elimination — chain/fade/demap/
  static-H all ruled out): decision-directed channel tracking feeds 8PSK's occasional
  wrong hard-decisions (its 22.5° boundaries are tight on a fading channel) back into
  the H estimate, poisoning it → cascade of confident-wrong bits. `ULTRA_COHERENT_DD_OFF=1`
  flips 8PSK Good@20 from FAIL → PASS. The base LTS+pilot H is fine (DD-off uses it and
  delivers); a genie perfect-H with DD *on* still failed (DD corrupts even a perfect H).
  Matches the code's own comment warning DD "poisons H on bad decisions during fades."
- Impact: 8PSK is the promotion lever toward 3000 bps; this blocks it on fading.
- Fix direction: reliability-gate DD (only DD a symbol whose EVM is well inside the
  95% noise radius — the code comment's own proposal), or gate DD off for 8PSK on
  fading; pair with channel-adaptive promotion (8PSK on clean/mild, QPSK on deep fade).
- Residual after DD-off: still marginal (heavy resends) — irreducible
  frequency-selective phase near spectral nulls; ARQ/adaptive-rate handles the tail.

### BUG-HARNESS-001: gui_qso_scenario.sh hard-aborts on auto-negotiated mode != --expect-mod
- Status: KNOWN (2026-05-29; harness limitation, not a modem bug).
- Symptom: on a channel where the auto rate-ladder legitimately promotes (e.g. AWGN30
  → 16QAM), running with `--expect-mod QPSK` makes `hard_failure_reason` abort the run
  at the MODE_CHANGE (`REASON=unexpected_data_mode`), ~25 s in, 0 file-transfer
  attempts. Falsely looks like a QPSK/AWGN modem failure (it is not — forced QPSK on
  AWGN30 delivers fine).
- Fix direction: when rate is not locked/forced, treat `--expect-mod` as advisory
  (accept the auto-selected mode) instead of hard-aborting.

### BUG-HARNESS-002: measure_ack_fer fidelity defects (under-piloting + broken AWGN burst path)
- Status: PARTIALLY FIXED (2026-05-29). The pilot-spacing defect is fixed; the AWGN
  burst_chunk defect is documented, not yet fixed.
- Defect 1 (FIXED): `makeOFDMConfig` hardcoded `cfg.pilot_spacing = 10`, while
  production uses `ofdm_link_adaptation::recommendedPilotSpacing(mod,rate)` = 5 for
  R1/2 & R2/3, 8 for R3/4. This under-piloted coherent high-order mods by ~2× and
  made 16QAM look structurally worse than production would. Fixed to call
  `recommendedPilotSpacing` (`tools/measure_ack_fer.cpp`). NOTE: re-test showed the
  under-piloting was NOT the cause of 16QAM folding (folds at spacing 5 too) — but
  the harness should still match production.
- Defect 2 (KNOWN): `--config burst_chunk --channel awgn` returns 0 frames recovered
  for all mods (qpsk/qam8/qam16, all DD modes) — the offline AWGN burst path does not
  match the GUI/OTASim AWGN path (where these mods decode fine). So measure_ack_fer
  AWGN burst_chunk numbers are not usable; use the GUI for AWGN.
- Defect 3 (KNOWN, 2026-05-29): the **Z=81 / N=1944 (long-LDPC burst keystone) path
  decodes to 0% in measure_ack_fer — even noiseless QPSK** — on BOTH the frame path
  (`--config data4_full` + `ULTRA_LDPC_Z=81`) and the burst path
  (`--config burst_chunk` + `ULTRA_LDPC_Z=81`). Verified Good@60: qpsk/qam16 = 0/100
  at Z=81 vs 91/84 at Z=27; confirmed pre-existing (reproduces with all genie edits
  git-stashed). ROOT CAUSE: `ULTRA_LDPC_Z=81` changes the ENCODER codeword size and
  the decoder's *block size* (→1944, `streaming_decoder.cpp:641`), but
  `decodeFixedFrame`'s `ldpc_z` argument is sourced ONLY from the burst descriptor
  (`last_burst_descriptor_.lifting_z`, `streaming_ofdm_decode.cpp:2876-2889`), NOT
  from the env. With no BURST_HEADER on the wire (frame path never sends one; the
  burst_chunk harness only enables the descriptor in its `wrong_group` branch), the
  decoder accumulates 1944 soft bits but decodes them with a **Z=27 matrix** →
  guaranteed failure. Production (GUI burst transport) IS unaffected: it transmits the
  BURST_HEADER with `lifting_z=81`, so the decoder learns Z and decodes correctly
  (GUI-proven CRC-clean ~3.1 kbps Good@20). CONSEQUENCE: **measure_ack_fer cannot
  validly screen the production burst config (Z=81); all offline Z=81 numbers are
  garbage.** The valid offline data is Z=27 only.
- DEEPER ROOT CAUSE (2026-05-29, attempted fix found it is MULTI-LAYER — NOT a quick
  fix): even with the encoder emitting Z=81 (via `setLDPCLiftingZ`, added as
  `--ldpc-z 81`) AND the decoder env-forced to Z=81 (block-size getter +
  `decodeFixedFrame` ldpc_z both read `ULTRA_LDPC_Z`), Z=81 STILL decodes 0% — even a
  1-CW frame, noiseless. Two more layers:
  (1) the waveform's soft-bit-grab size (`active_ldpc_block_size`, used by
      `getSoftBits`) is bumped to 1944 ONLY by `setActiveLDPCLiftingZ()`, which fires
      ONLY on descriptor-consume (streaming_ofdm_decode.cpp:763). With no descriptor
      the decoder grabs 648 bits of a 1944-bit codeword ("Got 648 soft bits,
      proceeding to decode") → garbage.
  (2) the descriptor-consume itself does not fire in the harness: the BURST_HEADER (a
      DQPSK-R1/4 control frame) is routed to the DATA decode path (line 1124), not the
      CONTROL path (line 661, which recognizes `FrameType::BURST_HEADER`), so it is
      mis-decoded as a failed data frame and dropped — never consumed (verified:
      `--burst-descriptor 1` gives 0% at BOTH Z=27 and Z=81 on Good@60).
  KEY INSIGHT (user): the BURST_HEADER is the SINGLE self-describing conveyor of
  lifting_z AND cw_per_frame AND group_size AND interleave flags together — the
  decoder is designed to learn the whole burst geometry from the wire in one shot, so
  piecemeal patching (env for Z, setFixedFrameCodewords for cw) cannot reproduce
  production. The ONLY faithful fix is to make descriptor-consume fire in the harness
  (route the BURST_HEADER to the control decode path). That is real plumbing across
  the control-detection + block-size + descriptor-consume layers — deferred. The
  `--ldpc-z 81` scaffolding (encoder emits + announces Z=81; env wires the decoder
  decode matrix + block-size getter) is committed as a correct PARTIAL toward that fix.
- Impact (updated): characterize ANY 16QAM-on-production-burst (Z=81) claim on the
  real-time GUI; the offline harness cannot do it until descriptor-consume is fixed.
- Impact: measure_ack_fer is a fast *screen* for relative offline comparisons on the
  Good path at **Z=27 only**; its AWGN path and its entire Z=81 path are not faithful.
  Confirm any fade/throughput conclusion — and ANY 16QAM-on-production-burst claim —
  on the real-time GUI per the project's standing rule.

### BUG-CFO-001: OFDM two-stage CFO refinement remains incomplete
- Status: OPEN
- Area: `src/ofdm/demodulator.cpp`
- Evidence:
  - TODO at `src/ofdm/demodulator.cpp:1307` for proper two-stage CFO (CP/frequency-domain refinement).
- Impact:
  - CFO handling works for current tested profiles but remains less robust than desired for broader OTA variation.
- Next steps:
  - Implement and validate two-stage CFO refinement with seeded regression + OTA logs.

### BUG-TEARDOWN-001: RX decode thread UAF crash on disconnect/shutdown (SIGSEGV in HilbertTransform::process)
- Status: OPEN (surfaced 2026-05-31; PRE-EXISTING — identical faulting stack in crash
  reports from 2026-05-28, predating the SyncController work).
- Symptom: `ultra_gui` (receiver side) SIGSEGVs at end-of-transfer / disconnect / shutdown.
  EXC_BAD_ACCESS / KERN_INVALID_ADDRESS. Faulting thread = the RX decode thread:
  `HilbertTransform::process` ← `OFDMChirpWaveform::detectDataSync` ← `StreamingDecoder::
  searchForSync` ← `processBuffer` ← `ModemEngine::rxDecodeLoop`. The receiver's log ends
  abruptly right after `Disconnected ... (Remote disconnected)` + `SR-ARQ: Reset`, with NO
  `RX decode thread stopped` line (cf. the sender, which tears down cleanly).
- Does NOT corrupt results: the file delivers CRC-clean BEFORE teardown (e.g. QPSK R1/4
  AWGN@10 seed42 → RESULT=PASS, GOODPUT=380, 0 retx, then crash on teardown).
- Root cause (CONFIRMED from both racer stacks 2026-05-31): on disconnect, the MAIN thread
  synchronously rebuilds the waveform while the RX decode thread is mid-demod on the old one:
  - WRITER (main thread, frees/rebuilds): `Connection::enterDisconnected` → `ModemEngine::
    setConnected(false)` → `StreamingEncoder::setDataMode` → `MCDPSKWaveform::initComponents`
    → `new MultiCarrierDPSKModulator` → `new ChirpSync` → `ChirpSync::generateTemplate`.
  - READER (Thread 22, faults): `rxDecodeLoop` → `StreamingDecoder::searchForSync` →
    `OFDMChirpWaveform::detectDataSync` → `HilbertTransform::process` → UAF (`KERN_INVALID_
    ADDRESS`, small offset = deref into freed/rebuilt waveform memory).
  The §14.36 "crash fix v3" (`applyPendingConnectedOFDMMode`) deferred only CONNECTED-mode
  reconstruction to a safe boundary in the RX loop; the DISCONNECT reconfig
  (`setConnected(false)`→`setDataMode`→waveform rebuild) is NOT synchronized with the RX
  thread at all. `stopRxDecodeThread()` joins, but `enterDisconnected` runs the rebuild on the
  protocol/main thread WITHOUT first quiescing the still-running RX `detectDataSync`.
- Why it matters for the SyncController refactor: this is exactly the §7.7#4 lifecycle-
  discipline scope the refactor must own. It also POLLUTES the GUI gate's crash signal —
  every gate run crashes at teardown, so a refactor-introduced crash can't be cleanly
  distinguished from this one. Fixing it first gives the behavioral phase a clean crash
  baseline.
- Fix direction: synchronize disconnect/shutdown waveform reconfig with the RX thread —
  either stop+join (or quiesce under `reset_generation_` + `buffer_mutex_`) BEFORE the
  waveform/HilbertTransform is reconfigured on disconnect, or route the disconnect reconfig
  through the same deferred safe-boundary the connected path uses. Fold into the
  SyncController lifecycle ownership (`reset()` discipline).

## Release Blockers

An issue is release-blocking if it causes any of:
- reproducible data loss,
- deterministic disconnect deadlock,
- non-deterministic gate failure in default mode ladder.

Current blockers:
- None identified for `0.2.1-alpha` default ladder.

## Fixed Bugs

- 2026-07-02: BUG-FILE-REQUEUE-OFFSET fixed — the live-ladder Moderate@20 data-loss root cause.
  `FileTransferController::requeuePendingChunks()` reconstructed the resume offset as
  `(chunks_acked_-1) * chunk_size_` — wrong the moment chunk_size_ has EVER changed mid-file
  (every mid-stream rate/mod move re-derives it). On the Moderate@20 gate cell the stuck-frame
  ESCAPE-drop (gate-bypassing by design) aborted 8 in-flight 16QAM chunks; the formula computed
  108×408=44064 where the true acked bytes were 34048, jumping the send cursor FORWARD 10016
  bytes (8 aborted chunks + 6.7 KB never sent). Reused seqs kept the receiver's ARQ space
  contiguous, the receiver-blind completion (count parity + cursor-at-EOF) declared done, and
  the auto-disconnect cancelled the receive — sender "Transfer complete", receiver stuck at
  expected=34048 with 16 buffered chunks. Fix: send-order ledger `tx_pending_ledger_`
  ({offset, metadata} per chunk handed to the ARQ, popped on retirement — retirement is strictly
  TX-base-order); requeue resumes exactly at `front().offset` for ANY chunk-size history;
  forward jumps impossible by construction. Companion receiver hardening (adversarial-review
  findings): `processFileData` tail-merges chunks that straddle the contiguous edge (a requeue
  resend on a CHANGED grid can start below the edge — the old whole-chunk drop lost the unseen
  tail forever), and the buffered-chunk drain is overlap-aware (covered entries drop, straddlers
  tail-append; the old exact-offset drain stranded covered entries and permanently blocked
  compressed finalization). `startSend` clears the ledger (SENDING→RECEIVING trample). Deferred
  structural sibling: BUG-FILE-ACK-IDENTITY (active, above). Proof: new
  test_file_transfer_controller cases (requeue-across-size-change reproduces 44064-vs-40 exactly;
  straddle-merge + covered-drain byte-exact CRC) + full 5-cell sequential gate PASS incl. the
  first-ever Moderate@20 pass (CRC-clean ×2, 1150 bps, 4 moves). See CHANGELOG 2026-07-02.

- 2026-07-02: BUG-DOPPLER-COHERENCE-MODECHANGE-WIPE fixed — the `ULTRA_RATE_ADAPT` precondition
  (2026-06-16 four-tier review): a mid-stream MODE_CHANGE could revert the Good/Moderate coherence
  verdict to the blind `fading_index` during the ~30 s re-pooling window. As-verified mechanics
  (2026-07-02 code read — the original "pool wiped every rate move" claim was PARTLY STALE): the
  decoder-hosted estimator pool + its atomics already survive `applyPendingConnectedOFDMMode`
  (waveform rebuild) and the pre-TX echo-clears (post 06-17/#67 `reset_doppler_coherence=false`
  paths); the remaining hole was that ANY modem-layer reset that does wipe the pool
  (`ModemEngine::reset`, future rebuild paths) immediately overwrote the Connection's valid verdict
  with invalid via the per-frame binding refresh. Fix per the named option: CARRY at the Connection
  layer — `Connection::setChannelCoherence` now holds the last VALID verdict while CONNECTED (the
  estimator is a cumulative mean and never un-validates on its own, so an invalid feed while
  connected can only mean "pool reset"), made safe by NEW per-connection clearing in
  `enterConnected()`/`reset()` (also fixes a latent cross-connection verdict leak). The mid-stream
  `requestModeChange` CW-count pick now routes through `coherenceAdjustedFadingIndex` (the
  CONNECT-time sites already did; remaining CONNECT-time raw-`fading_index` sites are provably
  identical there since coherence is invalid at CONNECT). Shipped with the fade-riding ladder
  default-ON (CHANGELOG 2026-07-02).
- 2026-05-13: BUG-PING-DETECTOR-001 fixed - real-HF PINGs now classify via additive chirp-lock plus LDPC-invalid PATH 2 while preserving the clean-cable/AWGN RMS-silence PATH 1.
- 2026-05-13: BUG-TNC-SESSION-001 fixed — R1 added the full RX decoder/session reset on disconnect; R2 added audio-producer quiesce/drain plus a reset-generation guard for in-flight decode callbacks, so a persistent `ultra_tnc` starts the next back-to-back PAT CONNECT from a fresh modem session boundary without stale capture backlog or stale cursor commits.
- 2026-05-08: BUG-CARRIER-LDPC-001 fixed — CarrierLDPC v1 was a new OFDM coded-bit wire image, but SP4 enabled the runtime as a local modem default instead of a negotiated peer capability. A partially upgraded Mac↔Pi pair applied the TX permutation on one endpoint while the peer decoded legacy ordering, producing the AWGN R1/2 1KB 0-ACK/15-timeout failure. The repair uses the existing `PHY_MASK_V1` capability: modern-modern CONNECT/CONNECT_ACK enables CarrierLDPC on both TX/RX; modern-legacy leaves the legacy ordering active. Synchronized upgraded hardware now passes AWGN / Good / Moderate at SNR=15 R1/2 1KB with DATA `Ncw=8` active and ACK/control `Ncw=1` inactive.
- 2026-05-08: BUG-BENCH-001 resolved — committed `fixtures/*.wav` are valid
  post-CONNECT DATA fixtures. They decode cleanly with
  `./build/decode_bench --mode bench --connected ...`; the earlier `0`-frame
  result came from running the bench in disconnected control-search mode.
- 2026-05-05: BUG-RATE-001 fixed — adaptive MODE_CHANGE panic-downshift on short Watterson-Good transfers. Hysteresis (`ADAPTIVE_PRESSURE_WINDOWS_FOR_DOWNGRADE = 2`) + lockout reduction (`ADAPTIVE_POST_DOWNGRADE_LOCKOUT_MS` 15 s → 5 s). 5-seed reproducer now 5/5 PASS with worst-case throughput improved 444 → 684 bps (no panic downgrade). See CHANGELOG 2026-05-05.
- 2026-02-12: GUI immediate TX abort control (`STOP TX`) added.
- 2026-02-12: GUI telemetry split into PHY vs effective goodput, plus ARQ health view.
- 2026-02-11: OTA control-path hardening and bootstrap safety updates.
- 2026-02-06 to 2026-02-10: ACK/control-frame decoding and ARQ robustness fixes.

For full details and commit-level history, use:
- `docs/CHANGELOG.md`
