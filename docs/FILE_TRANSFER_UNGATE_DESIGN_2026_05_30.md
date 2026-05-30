# File-Transfer "Ungate" Design (2026-05-30)

> **SUPERSEDED 2026-05-30 — the real root cause was found and fixed.** FILE_START never had
> a slot/sync/metadata-design problem. It is a ~90%-zero-padding frame, and the modem has
> **no data whitener**, so the all-zero pad LDPC-encoded to a wall of identical 16QAM symbols;
> any systematic channel-estimate error then hit them the same way -> correlated errors LDPC
> can't fix. The DATA frames (random payload) in the SAME group decoded fine. Fix:
> **whiten the pad with a PRBS instead of zeros** in `encodeFixedFrame` (`frame_v2.cpp`, ~5
> lines, RX-transparent — the receiver reads `payload_len` and ignores the pad, so no compat
> bit). Proven seed 7: group 0 went 5/6-forever -> **6/6 first attempt, 0 staging, no churn**;
> the 106 s stall vanished. So the manifest / ungate / move-FILE_START-off-anchor designs
> below are NOT needed. General gap for later: add a full frame scrambler (protects
> low-entropy DATA too, not just padding).
>
> **What we KEEP (already committed) as the SAFETY NET:** the pre-FILE_START **staging** +
> **idempotent FILE_START** + the **"incoming burst" indicator**. The PRBS fix makes
> FILE_START decode in group 0 in the COMMON case — but if a deep fade genuinely wipes
> group 0, the staging still buffers the data that DOES decode (later groups), ARQ keeps
> resending FILE_START until it lands, and it drains then. So even in the worst case the
> metadata arrives EVENTUALLY with NO data loss and no permanent stall. PRBS = fast common
> case; staging = safe rare case. They compose.

**Original status (now historical):** validated design, not built. Distilled from a long
live-debug session on 16QAM R1/2 Good@20 seed 7.

## The one flaw (what we're fixing)

The whole file transfer is held hostage by **one fragile frame**: `FILE_START` (the file
metadata — size, CRC, filename) is sent as **frame 0 of group 0**, with the same light,
in-burst preamble as the data. Frame 0 is the **group-start anchor slot**, which is
systematically the most failure-prone position. Consequences observed:
- **106 s stall**: nothing finalizes / no progress / (pre-staging) data dropped until
  FILE_START decodes.
- **Churn**: FILE_START never acks, so ARQ resends it for ~100 s; the sender isn't "done"
  until it lands.
- All of it while the *actual file data* (frames 1–5) decoded fine (logs were consistently
  **5/6 with only frame 0 missing**).

The transport itself (chunk + SR-ARQ + offset-keyed assembly) is correct and standard
(ZMODEM/TCP lineage). The resend logic is correct (timeout => resend whole group; partial
ACK => resend only un-acked — verified). The single real defect is **putting the most
critical, must-decode info in the least-reliable frame, and then *waiting* for it.**

## The design: ungate + move off the anchor

**Nothing is moved to a trailer. FILE_START stays an EARLY frame.** Two cheap changes:

1. **Ungate** — the transfer never waits for FILE_START:
   - "It's a file" recognition comes from the **FILE_DATA type byte** (already on the wire,
     decoded constantly) and/or a spare **`BURST_FLAG_FILE`** bit in the descriptor
     (`payload[4]` has 6 free bits) — both reliable and early.
   - Data **commits by absolute offset** as it arrives (the pre-FILE_START staging already
     built does this).
   - **Completion = last-frame (MORE_FRAG=0) + no gaps.** The last frame *defines* the size
     (`last_offset + last_len`), so size-for-completion is free; ARQ fills the gaps. No
     dependency on FILE_START.
   - When FILE_START *does* arrive (via normal ARQ — it's guaranteed to, eventually), it
     upgrades the display (size => % bar, filename) and provides the whole-file CRC for the
     final verify. Until then: bytes-only.

2. **Move FILE_START off frame 0 -> frame 1+** so it decodes as reliably as the data
   (frames 1–5 were 5/5) and lands in the **first group** instead of churning ~100 s.
   Frame 0 (anchor) then carries ordinary data the ARQ resends cheaply.

### Why it's correct (the challenge survived)
- **ARQ already guarantees delivery** of every frame, FILE_START included — no special
  "re-broadcast" needed. The churn we saw *is* ARQ doing its job; the bug is that we *wait*.
- **Completion needs no upfront size** — the last frame supplies it.
- **No re-gating of the sender**: FILE_START is an early frame that arrives fast (off the
  anchor) and acks fast, so the sender completes normally without a separate gating frame.
- **Graceful degradation**: lose FILE_START entirely (rare) => you still got the file
  (auto-name, "received, unverified"). The per-frame CRCs guard integrity meanwhile.

### GUI states
1. file detected, no metadata yet -> flashing *"FILE INCOMING — metadata pending — 12 KB
   received…"* (the burst indicator already built, extended with "FILE").
2. FILE_START arrives -> solid bar *"photo.jpg — 38% (12/31 KB)"*.
3. last frame + gaps filled + CRC ok -> *"photo.jpg received ✓"* (or "received, unverified").

### Why frame 0 fails — INVESTIGATED 2026-05-30 (seed 7, /tmp/r12_burstui)
**Root cause: frame 0 carries the burst's WORST channel estimate — a phase/estimate error,
not noise.** Per-CW decode of group 0: frame 0 `CW FAIL iters=50 unsat=252/332
llr_avg=4.16/6.13 |llr|mean≈15 p50=20`; frames 1–5 `OK iters≤26 llr_avg≈0`. The high-
magnitude, BIASED, high-unsat LLRs = a confidently-WRONG constellation read (rotated/mis-
scaled estimate), categorically NOT low SNR.

Why frame 0 specifically:
1. It's the FIRST frame after group-start sync re-acquisition (timing/CFO least settled).
2. Its estimate rides the **negated-LTS group-start preamble**, un-negated before estimation
   (`ofdm_chirp_waveform.cpp:934`) — extra handling on the most marginal estimate.
3. It gets **no per-symbol pilot tracking history** — tracking converges over frames 1–5
   (that's why their `llr_avg→0`); frame 0 eats the raw, un-converged estimate + σ².

NOT the cause this run: the negated-LTS marker MIS-detection. Group 0 synced at corr=0.94,
CFO=0.0, marker correctly detected ([BURST-INTERLEAVED]) — yet frame 0 still failed. So the
fragile sign-check (`marker_metric.real()<0`, `ofdm_chirp_waveform.cpp:788`) is a real LATENT
second failure mode on *marginal* sync (corr≈0.25 cases), but it did not cause this failure.

Implications:
- **Validates the move**: frames 1–5 use clean light-LTS + converged tracking. Moving
  FILE_START off frame 0 puts the metadata on an estimate that actually works.
- **Same problem as the estimator work**: the failure is over-confident BIASED LLRs from a
  bad group-start estimate + mis-scaled σ² — exactly what ① (CFO-clean averaging) + the
  de-biased σ² target. Worth a targeted test: does ① cut the frame-0 `llr_avg` bias?
- Deeper future fix: stop overloading the estimation LTS with the CFO-fragile sign-marker.

## Rejected alternatives
- **Manifest in the existing BURST_HEADER**: `ControlFrame::PAYLOAD_SIZE = 6`, fully used by
  the descriptor (group/cw/mod/rate/flags/Z). No room for size(4)+CRC(4).
- **2-CW BURST_HEADER**: doubles descriptor airtime per group.
- **Separate FILE_MANIFEST control frame**: to be robust it must repeat per group => two
  control frames per group, two decode dependencies, no upside over the above.
- **Metadata only in a trailer (last frame)**: clean for completion but no *live* % bar
  (size only known at the end). The ungate design keeps size early (in FILE_START) for the
  live bar, CRC verifies at the end either way.

## Build order
1. Investigate + confirm the frame-0 fragility cause (below).
2. Move FILE_START off frame 0 (TX burst formation).
3. Ungate: recognition from FILE_DATA type / `BURST_FLAG_FILE`; commit + complete without
   FILE_START; non-gating finalize/verify.
4. GUI: extend the burst indicator to "FILE INCOMING …" -> % bar on metadata.
5. Capability bit for old<->new interop.
6. Validate on the faithful GUI (does metadata land in group 0? stall gone? churn gone?).
