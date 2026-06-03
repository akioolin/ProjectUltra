# TNC ↔ Winlink-B2F half-duplex bidirectional exchange — findings (2026-06-03)

Live cross-machine test (Mac ALPHA ↔ Pi5 BRAVO, real PAT 1.0.0 clients over `ultra_tnc`
over `ota_simulator serve`). Goal: deliver a Winlink P2P message ALPHA→BRAVO through the
modem. Outcome: **connection + B2F handshake succeed; the message exchange stalls and does
not deliver.** Two distinct issues found — one fixed, one open.

## The chain (verified up)
`PAT(ALPHA) --varahf TCP--> ultra_tnc(ALPHA) --OTASim--> ultra_tnc(BRAVO) --varahf--> PAT(BRAVO)`
- ultra_tnc emulates the VARA-HF cmd(8300)/data(8301) interface; PAT's `varahf` transport
  connects to it. Both PAT configs: `mycall` ALPHA/BRAVO, `varahf.addr localhost:8300`.
- OTASim serve LAN-bound (`--bind 0.0.0.0:52001 --udp-bind 0.0.0.0:52002`), both TNCs
  `--sim-audio --ota-host <mac>:52001`. Cross-machine OTASim works.
- B2F handshake completes: CONNECT (~24 s), callsign banners, `[Pat-1.0.0-B2FHMG$]`, GZIP
  negotiated. ALPHA proposes: `>FD EM <mid> 425 333 0`.

## Issue 1 — FIXED: one-way burst bypass → half-duplex collision
**Root cause.** This branch is `feat/oneway-arch` — strictly one-way sender-driven file push.
`Connection::sendFile()` (connection.cpp:1636) bypasses the ISS/IRS turn gate when
`use_burst_transport_ && isOFDMMode` (design §14.27: "ALPHA sends, BRAVO listens+ACKs").
For B2F **both** stations alternately transmit, so both hit the bypass and key up
uncoordinated → collide on the half-duplex channel. Confirmed in logs: both ALPHA and BRAVO
log `sendFile: ISS-bypass taken` and transmit independently.

**Fix (commit `c27aa45`).** Added `Connection::half_duplex_interactive_` (forwarded via
`ProtocolEngine::setHalfDuplexInteractive`; `ultra_tnc` sets it true). When set, `sendFile()`
keeps the turn gate: a station starts its burst only while it holds `local_data_turn_`,
else it queues + `TURN_REQUEST`s and the peer yields `TURNOVER`. GUI one-way path unchanged
(flag default false).

**Verified.** The collision is gone — BRAVO correctly waits for the turn, ALPHA (holding the
turn with no data) yields, BRAVO sends its banner burst while ALPHA receives. Single-machine
one-way TNC test (`UltraTncSimAudio`, 8 KB file) still passes — CI-safe.

## Issue 2 — OPEN: receiver cannot decode the new sender's burst after a turn-flip
**Symptom.** After the turn flips and BRAVO transmits its banner burst, ALPHA (now the
receiver) logs a continuous stream of `Burst marker frame timing retry: ±100–313 samples`
and **never completes the decode → never sends a GROUP_ACK → BRAVO retransmits the same
burst every ~17 s forever**; BRAVO's "responder handshake still unconfirmed". The message
never delivers.

**NOT cross-machine timing.** Reproduced **single-machine** (both TNCs + both PATs on one Mac,
one OTASim, shared clock): identical `±97–280 sample` timing retries and identical stall. So
it is not the independent-clock drift first suspected.

**Asymmetry is the clue.** The one-way path (BRAVO *receives* ALPHA) decodes fine — the 8 KB
file delivers byte-exact. The failure is specifically ALPHA *receiving* BRAVO **after a
turn-flip**. The receiver that has not been the sender cannot lock the new sender's burst
frame timing.

**Leading hypothesis.** The bidirectional path needs **full chirp+LTS anchor re-acquisition
on every turn-flip**. The one-way design only ever anchors once (ALPHA's first burst), so the
receiver tracks one sender's timing. When the turn flips, the new sender must send a FULL
anchor and the new receiver must `expectFullOFDMAnchorOnce()` — otherwise it warm-syncs
(light preamble) against a timing reference it never established → the ±200-sample marker
retries that never converge. The `expectFullOFDMAnchorOnce` mechanism exists but is not
armed/sent correctly across turn-flips. (Secondary suspects to rule out: z=81 long-LDPC burst
timing sensitivity; whether the yielded station is allowed to TX its GROUP_ACK promptly.)

## Update (later 2026-06-03): traffic-class routing landed; blocker confirmed transport-independent
- **Routing fix (committed).** `tnc_session.cpp::flushDataTxBuffer()` now routes by size: a
  block ≤ `kInteractiveMaxBytes` (4096) goes on the NON-BURST short-LDPC (z=27) `sendBinary`
  path (SR-ARQ + ISS/IRS turn gate); larger → the burst (z=81) file path. This is the
  design-correct class split (§3/§7). Verified: a B2F connect now takes the non-burst path
  (BRAVO `sendFile() called` = 0). One-way TNC file test (`UltraTncSimAudio`) still passes.
- **But the message still does not deliver, and the failure is identical** — so Issue 2 is
  **NOT** about burst vs non-burst. With BRAVO on the non-burst path, ALPHA STILL logs
  `burst marker frame timing retry ±70–313 samples` and decodes **zero** frames. (The OFDM
  data-frame RX uses "burst marker" timing recovery for both transports.) So the root cause is
  confirmed: **the receiver cannot acquire the new sender's frame timing on a turn-flip** —
  BRAVO transmits a light/warm preamble, ALPHA never established BRAVO's timing reference, so
  it cannot lock. The fix is **full chirp+LTS anchor on every turn-flip**, independent of which
  transport carries the bytes.

## Update 2 (2026-06-03): the full layer stack — 3 fixed, responder-first turn is the frontier

Working the bidirectional B2F revealed a STACK of layers, each hiding the next. Fixed +
committed (all CI-safe — `UltraTncSimAudio` one-way file test stays green):
1. **Collision** (`c27aa45`): one-way burst bypass → both stations key up uncoordinated.
   Fixed with `half_duplex_interactive_` turn-gating.
2. **Traffic class** (`c177b74`): tiny B2F control was shipped as a z=81 burst file. Fixed by
   routing ≤4 KB to the non-burst z=27 SR-ARQ path (`sendBinary`).
3. **Anchor on turn-flip** (`d3ea1fe`): new sender must re-anchor (full chirp+LTS) so the
   receiver can acquire it after a flip. Added `on_data_turn_acquired_` →
   `forceNextFrameFullPreamble()`; the yielding peer already arms `expectFullOFDMAnchorOnce()`.
4. **Responder handshake** (`d3ea1fe`): Winlink B2F has the RESPONDER speak first, but the
   modem's one-way flow makes the responder wait for the initiator's first DATA frame before
   it may transmit → deadlock. Pre-confirm `handshake_confirmed_` for the interactive responder.

**REMAINING FRONTIER — responder-first turn acquisition.** Even with 1-4, the message still
does not deliver, and the turn state is the unresolved knot:
- The modem turn model is **initiator-first** (initiator = ISS at connect). Winlink B2F is
  **responder-first**. The IRS requests the DATA turn piggybacked on an ACK — but the B2F
  responder has nothing to ACK yet, so it can't cleanly request the turn.
- Tried: the interactive INITIATOR proactively yields the turn to the responder right after
  connect (TURNOVER → responder force-full → initiator expect-anchor). **The yield did not
  fire** and observed turn state is contradictory (BRAVO transmits without queuing AND without
  holding the turn per the logs). This needs **targeted per-tick turn-state logging**
  (`local_data_turn_`, `handshake_confirmed_`, `shouldQueuePayloadForLinkTurn`, guard ms) on
  both stations to see who actually holds the turn and why BRAVO sends, plus a check of the
  **VARA-HF turn convention** (does the connecting station send an empty first frame so the
  answering station gets the turn for its SID?). The proactive-yield change was reverted as
  unproven; the mechanism (initiator yields first for interactive) is still the likely answer.

This is a multi-session protocol integration (cf. the README note that the audio-cable B2F
bring-up found "five real bugs"). The committed layers are real progress and the next step is
well-scoped: resolve the turn-state with instrumentation, then make the responder reliably
take the first turn.

## Update 3 (2026-06-03): turn handoff SOLVED; blocker is now bidirectional OFDM decode

The turn-handoff layer is **done and verified** on the single-machine PAT↔PAT rig (instrumented,
B2F-DBG logs at DEBUG). Commits `d3ea1fe` + `90181f6`:
- The interactive INITIATOR (ISS, empty TX buffer) **proactively yields** the DATA turn to the
  responder ~1.5 s after connect (VARA-HF turnaround). Verified: "interactive ISS yielded first
  DATA turn to BRAVO".
- The RESPONDER **queues its SID banner** correctly (IRS, handshake pre-confirmed): `B2F-DBG
  sendPayload 53B queue=1`.
- The yield's TURNOVER and the post-acquire first frame **force a full chirp+LTS anchor**
  (`on_data_turn_acquired_` → `forceNextFrameFullPreamble`; verified it reaches non-burst frames
  via `encodeFrameLight`→`encodeFrame`).

**REMAINING — bidirectional OFDM decode (PHY frontier).** Despite all the above, neither station
decodes the other's OFDM frames after the MC-DPSK→OFDM switch: ALPHA never decodes BRAVO's frames
(0 decoded, continuous `burst marker timing retry ±70–300`), and BRAVO never receives ALPHA's
(full-anchored) TURNOVER. No collision (verified: BRAVO was idle/RX when the TURNOVER arrived).
- **Key asymmetry:** the one-way `UltraTncSimAudio` test only ever has the RESPONDER *receive*
  OFDM (and it works). The **INITIATOR-receives-OFDM** path (BRAVO→ALPHA) is untested and is a
  failing direction here.
- **Leading hypothesis:** `expectFullOFDMAnchorOnce` is a ONE-SHOT armed at connect for both
  sides (`modem_mode.cpp:57,174` — symmetric, no is_initiator gate). The responder's interim TX
  (turn-requests) and/or a noise false-trigger likely consumes or resets that one-shot before the
  peer's first OFDM frame arrives, so neither cold-acquires. Next: instrument the decoder's
  expect-anchor state across TX→RX transitions; make the anchor expectation robust (re-arm on
  every turn-flip / don't consume on a failed detect); verify the initiator-RX-OFDM path in
  isolation. A pragmatic A/B: temporarily route B2F through the robust burst path to confirm the
  decode gap is non-burst-specific vs general bidirectional-OFDM.

## Next steps
1. Confirm hypothesis: log whether BRAVO's post-turnover burst carries a full anchor and
   whether ALPHA has `expect_full_ofdm_anchor_` set when it starts receiving it.
2. Wire full-anchor send + expect on each TURNOVER (both directions) for the interactive path.
3. Re-test single-machine PAT↔PAT first (fast, deterministic), then cross-machine Mac↔Pi.
4. Only after delivery works: tune B2F turnaround latency (each turn ≈ 15–25 s on the
   real-time link; the full B2F exchange is ~5 turnarounds — watch PAT's B2F timeouts).

## Repro
- Single-machine: OTASim `awgn@30`; `ultra_tnc` ALPHA :8300 + BRAVO :8302 on the lobby;
  PAT ALPHA (default cfg) + PAT BRAVO (`--config` with `varahf.addr localhost:8302`, own
  `--mbox`) `http`; `pat connect varahf:///BRAVO`.
- Cross-machine: same but BRAVO TNC+PAT on the Pi, `--ota-host <mac-ip>:52001`.
