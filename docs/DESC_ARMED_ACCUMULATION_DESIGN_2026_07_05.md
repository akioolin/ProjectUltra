# Descriptor-Armed Accumulation (late-join) — BUG-BURST-HEADNULL-DROP recovery
2026-07-05 · knob `ULTRA_DESC_ARMED_ACCUM` (default OFF = byte-identical) · RX-only, no wire change

## 0. The four-tier stack (mandatory)
PHY theorist · real-time DSP systems engineer · veteran HF operator · physics escape hatch.
Reject heuristic patches lacking principled justification under all three mandatory lenses.

## 1. Problem (measured)
When the burst-group HEAD dies (the group-start frame carrying the negated-LTS marker — and
on mode-switch boundaries, the full anchor + BURST_HEADER descriptor), accumulation never
arms. Every clean mid-group frame then syncs (corr 0.9+), fails the 1-CW control peek, and
is consumed by the §14.24-guarded re-search with **no decode attempt and no ACK credit**
([HEADNULL] counter, 2026-07-01; backstop empty-NACK, 2026-07-04). Measured cost:
- steady-state head-null: one ~18-28 s RTO per incident (backstop NACK trims the tail case);
- mode-switch boundary (F94, 2026-07-05): anchor faded twice → geometry-blind 40 s;
- overnight batch F78-F87: forward-path group losses = THE dominant loss pool (~200 s worst run).

## 2. Physics & information position
The mid-group frames are *received energy carrying decodable information*. Each physical
frame is self-contained at the PHY layer (own light-LTS → per-frame channel estimate); the
group's cross-frame interleaving spreads every logical CW across all N physical frames, and
the deinterleaver already accepts per-frame erasures (a nulled frame = erasure block; R2/3
LDPC tolerates ~1/3 erasure). Discarding M clean frames because frame 0 died throws away
M/N of the group's mutual information that the code was *designed* to exploit. Half-duplex
cost asymmetry: an immediate (even empty) group verdict costs one ACK slot; silence costs a
full RTO.

## 3. Design
**Trigger** (the current [HEADNULL] drop site, streaming_ofdm_decode.cpp ~1116): connected,
burst regime, `sync_controller_.have_burst_descriptor_` (the latch PERSISTS across the
transfer by design), group NOT armed, sync-accepted frame failed the control peek.

**Action (knob ON): late-join arm instead of drop.**
1. Configure the DATA profile from the latched descriptor (mod/rate/cw-per-frame/z/
   interleave flags — the same fields a live BURST_HEADER applies).
2. Re-demodulate THIS frame from the ring at the data profile (per-frame light-LTS channel
   estimate — the §14.24 poisoning guard is honored: no control-profile probe, no shared-
   estimate reuse across the join; the join SEEDS a fresh group estimate exactly as a
   marker-armed group would from its first frame).
3. Enter the EXISTING armed-accumulation machinery with `late_join = true`:
   `expected_next_frame_abs = this_abs + frame_stride`; subsequent frames follow the normal
   armed path (including its existing missed-slot erasure handling — relative positions are
   exact from arrival timing).
4. **Finalize (tail-anchor):** when the group completes by count or by the existing group
   timeout, the caught run of M frames (with exact relative offsets) is anchored to the
   TAIL of the declared N-frame group: leading N−M slots erasure-filled, then deinterleave
   + LDPC as normal. Rationale: the timeout fires ~one frame period after the last catch,
   which is consistent with the tail having survived; a wrong anchor cannot corrupt
   (per-CW LDPC + frame CRC gate everything) — it degrades to today's outcome plus a
   prompt NACK.
5. **ACK:** the normal finalize → group callback path emits the group verdict
   (partial SACK from decoded logical seqs, or empty NACK). `group_seq` for the tone-ack =
   latched `descriptor.group_seq + 1 (mod 64)` (best inference; see failure table F5).

## 4. Failure-mode table
| # | Scenario | Outcome under late-join | vs today |
|---|----------|-------------------------|----------|
| F1 | Head nulled, tail survives (dominant class) | Tail-anchor correct → deinterleave with N−M erasures → partial/full delivery + SACK | today: total loss + RTO |
| F2 | Head AND tail nulled (mid-run caught) | Tail-anchor wrong → CRC fails → empty NACK ~1 group period | today: total loss + RTO (strictly ≤ cost) |
| F3 | Geometry changed this group (mode-switch, descriptor lost — F94) | Demod at stale geometry → garbage → CRC fails → prompt empty NACK; sender's resend (full anchor + descriptor) re-anchors | today: 40 s blind saga (biggest win in latency, no decode win) |
| F4 | False sync (noise) enters late-join | Data-profile demod garbage → all-CW CRC fail → empty NACK; LLR shape gate (8230fea) already filters noise-shaped populations upstream | new false-NACK risk: bounded — one NACK per declared group max (the arm consumes the group) |
| F5 | group_seq inference wrong (resent group = same seq) | Sender-side ack matching is seq-mask-driven; group_seq6 affects dedup/crater logic only — a stale value can suppress one crater command (rx_rate_cmd dedup) for one event | acceptable; logged for the A/B |
| F6 | Estimate poisoning (§14.24 regression) | Not possible by construction: no control-profile fall-through; per-frame LTS estimates; fresh group seed | guard preserved |
| F7 | Late-join arms on the LAST frame only (M=1) | Deinterleave at N−1 erasures fails (over budget) → empty NACK promptly | = backstop-NACK behavior, arrives sooner |

## 5. What this does NOT do (scope fences)
- No wire change: descriptor re-announce every burst already exists; frames unchanged.
- No TX change; sender behavior (RTO/resend/§16.4) untouched — late-join only makes the
  receiver's verdict arrive earlier and with recovered data.
- The marker-armed path is untouched when the head IS decoded (knob-independent).
- OFDM_CHIRP burst regime only (the gate condition already scopes it).

## 6. Validation plan
1. Unit: tail-anchor placement math + erasure-fill (new test alongside burst-interleave tests).
2. ctest full (UltraTncSimAudio pre-existing red excluded).
3. Sim A/B good@20 (seeds 42/43): no regression; [HEADNULL] drops → late-join arms.
4. Rig A/B vs the F90-F94 baseline: metric = RTO count + time-in-saga after head-null
   incidents ([HEADNULL] drop lines with no delivery vs late-join line + group verdict).
