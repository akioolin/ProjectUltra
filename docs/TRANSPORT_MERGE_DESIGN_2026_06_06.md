# Transport Merge — Design & State (2026-06-06)

Goal: collapse the **three overlapping SR-ARQ-over-OFDM data transports** into ONE
adaptive path with a single 16-bit sequence space, one tone-burst ACK, one retransmit
window — where "burst (group) framing" is just a TX framing *choice*, not a separate
transport. This note captures the map, what's done, the keystone insight, the precise
remaining plan, and the gotchas, so the next pass starts clean.

---

## DONE & COMMITTED (this work is solid; verified on GUI)

- **`ce7766a` fix(B2F): encoder z-revert + honest VARA BUFFER.** Root of BUG-TNC-B2F-002:
  a burst lifts the *encoder* to z=81 and nothing reverted it → the next non-burst frame
  was encoded z=81 while the RX decoded z=27 → saturated/random LLRs → stall. Fix:
  `modem_engine.cpp` transmit() sets `setLDPCLiftingZ(27)` before encoding any non-burst
  frame. Plus TNC reports HONEST VARA BUFFER (true ACK-aware queue depth) — see CHANGELOG
  2026-06-05. 20 KB JPEG delivers byte-identical with clean teardown on the plain path.
- **`e11b854` feat(arq): interactive tone-burst ACK (env `ULTRA_TONE_ACK_INTERACTIVE`).**
  The interactive SR-ARQ path acks via the SAME tone-burst the burst path uses (not a SACK
  frame). `selective_repeat_arq.cpp` sendSack/onToneBurstAck; `connection.cpp` wiring + arm
  + route. GUI-verified: single msg, multi-frame **selective repeat** (drop seq=1 → only
  seq=1 resent), coalesced to ONE tone-burst per turn, window capped to 6 (6-bit mask).

**Net: the ACK layer of the merge is ALREADY unified** — all 3 paths emit the identical
`ToneBurstAckPayload{group_seq:6, frame_mask:6, type, rate_hint:3}`.

## WIP — UNCOMMITTED, env-gated OFF by default (`ULTRA_UNIFIED_SEQ` unset → default build untouched)

In `connection.cpp` + `selective_repeat_arq.cpp` (the **wrong-direction** first attempt;
keep as scaffold or revert):
- `kUnifiedSeqEnabled()` env helper.
- `startFileTransferNow` skip-gate `&& !kUnifiedSeqEnabled()` (routed the file AROUND
  `burst_transport_` → produced a malformed mega-burst; this direction is WRONG).
- `onBurstGroupReceived` early branch feeding all frames through `processArqFrame` (the RX
  half — correct *idea*, but needs the descriptor-flag routing, below).
- `selective_repeat_arq` coalescing: added `window_worth` (ack per full window, not just at
  message tail) — needed so a MULTI-window transfer doesn't stall waiting for a tail.

---

## THE THREE PATHS (file:line anchors, src/protocol/connection.cpp unless noted)

| | A: interactive SR-ARQ | B: SR-on-burst (interleave-off) | C: burst file (interleaved) |
|---|---|---|---|
| TX entry | sendPayload→startPayloadNow:1345 | startBurstFileTransfer→formAndSendBurstGroupSR:2690 | →formAndSendBurstGroup:2199 |
| seq | arq_ `tx_next_seq_` (selective_repeat_arq.cpp:154) | **`burst_chunk_seq_`** (separate!) | `burst_transport_` group_seq |
| RX | processArqFrame:3090→arq_.onFrameReceived | onBurstGroupReceivedSR:2559→**handleDataPayload (offset)** | burst_transport_.onGroupReceived |
| ack | tone-burst (sendSack) | tone-burst group_seq+mask:2624 | tone-burst GROUP_ACK:451 |
| z | 27 | 27/81 | 81 |

**TWO seq spaces** is the core problem: A uses arq_'s 16-bit seq; B/C use a separate
`burst_chunk_seq_` counter on TX and **bypass arq_ entirely on RX** (deliver by file
OFFSET embedded in the DATA-frame payload). Routing decision A-vs-B/C: payload size vs
`kOFDMFileBlockPayloadLimit=2300` (connection.cpp:16) + `use_burst_transport_` + channel.

**Key fact:** file chunks ride ordinary `DATA` frames with a **file-offset header in the
payload** (NOT a distinct FILE_DATA *frame type*) — so you CANNOT route file-vs-regular on
`hdr.type`. The signal must live in the **BURST_HEADER descriptor**.

`BurstHeaderInfo` (frame_v2.hpp:558): group_size, cw_per_frame, modulation, code_rate,
burst_interleave, carrier_ldpc, **lifting_z** (payload[5]; 0→legacy z=27). No "regular vs
file" flag yet — ADD one (or reuse z=27-vs-81 as the convention).

---

## CHOSEN DIRECTION (the "better" one) + KEYSTONE INSIGHT

DON'T reroute the file through the older `sendNextFileChunk` path (it lacks the file→6-frame
group chunking that `burst_transport_` does → it mega-bursts the whole file → `0/6` decode).
INSTEAD, keep `burst_transport_`'s chunking/descriptor/encoder and unify the **bookkeeping**.

**Keystone (operator's idea):** put a flag in the BURST_HEADER that says *"regular SR-ARQ
frames, z=27 — NOT a file"*. Then:
- TX marks a group `regular` when it carries arq_ data;
- RX routes `regular` groups → `processArqFrame` (arq_ window: dedup/reorder by seq,
  in-order delivery, tone-burst ack), and un-flagged (file) groups → the offset assembler;
- the file, to ride arq_, stops being offset-addressed and becomes **in-order DATA** (arq_
  guarantees order → write sequentially, offset header disappears). This is the real
  offset→seq conversion.

For **regular data (messages) this already works** (tone-burst tests proved a multi-frame
message rides arq_ end-to-end). It's specifically the **FILE** that needs the 3 steps.

---

## REMAINING PLAN (each step gated on byte-identical file CRC)

1. **BURST_HEADER `regular` flag.** Add a bit to `BurstHeaderInfo` (a reserved payload bit);
   TX sets it for arq_-sourced groups, clears for file-offset groups.
2. **RX route on the flag.** `onBurstGroupReceived` (the descriptor's flag must be threaded
   to it — currently it gets group_seq/frames/all_ok/quality/frame_mask/interleaved, NO z/
   flag, so add it): flagged → `processArqFrame`; else → existing offset path. Seq alignment:
   sender's burst frames must draw seq from arq_ so the receiver's `rx_base_seq_` matches.
3. **TX 6-frame-group chunking for the arq_ path.** THE concrete bug: `sendNextFileChunk`
   (and `flushBurstBuffer`) flush back-to-back / the whole file as one ~40-frame mega-burst
   despite window=6 (`active=1196639` samples ≈ 25 s in one TX; descriptor said `group=6` →
   structure mismatch → `0/6`). `isReadyToSend()=tx_in_flight_<6` SHOULD gate it
   (selective_repeat_arq.cpp:188/265 increment tx_in_flight_) — investigate why it didn't
   (deferred_file_refill_ re-submitting without ack? burst_mode_active_ buffering across
   refills?). Must emit ONE 6-frame group, wait for the tone-burst ack, then next.

Clean seam for later: `arq_.transmitDataBatch` (selective_repeat_arq.cpp:1765) ALREADY sends
a multi-frame batch as a burst via `on_transmit_batch_ → transmitFrameBatch → on_transmit_burst_`
(used today for timeout-repairs). Make arq_ batch the INITIAL window-worth too → "burst" = arq_
batched framing, and `burst_transport_`'s stop-and-wait controller can eventually retire.

---

## GOTCHAS / non-obvious
- Coalescing for tone-burst acks must fire **per window** (frames_since_ack ≥ window) AND at
  tail AND on hole-fill — NOT tail-only (multi-window files stall). NO immediate
  per-out-of-order acks (half-duplex: one keyup per turn). Window capped to 6 = the 6-bit mask.
- PAT/VARA caps at **889 B** (B2F `MaxMsgLength=125` × VARA `magicNumber=7`); honest BUFFER
  can't widen it — so bursting PAT/B2F is impossible without lying. (n8jja/pat-vara conn.go.)
- `use_burst_transport_` is the gate at top of `onBurstGroupReceived` — check it's true.

## Test harnesses (Mac-only GUI sim; reuse/adapt)
- `/tmp/tone_ack_msg.sh` — 1 interactive msg + tone-burst ack.
- `/tmp/tone_ack_drop.sh` — multi-frame msg, drop seq=1, prove selective repeat (run with
  `ULTRA_TONE_ACK_INTERACTIVE=1`, BRAVO `ULTRA_DROP_RX_SEQ=1`).
- `/tmp/unified_file.sh awgn 28` — file transfer; checks `[FILE] Received … CRC ok` + path used.
- `tools/gui_qso_scenario.sh` — faithful gate (needs matching `--expect-rate/--expect-mod`,
  else fails `unexpected_data_mode`; defaults R1/4 16QAM don't match clean channels).
- Drop hook for proving selective repeat: `processArqFrame` env `ULTRA_DROP_RX_SEQ=N`.
