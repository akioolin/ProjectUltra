# Removal Backlog (the demolition list)

A single tracked list of code / features / experiments **slated for deletion** —
superseded models, failed experiments, dead paths. This is the *action* list for
removals; it complements (does not duplicate) the broader cleanup register in
`docs/MODEM_INFRASTRUCTURE_MAP.md §7`, which also covers *consolidate / rename /
codify* work that is **not** deletion.

**How to use:** add an entry the moment something is decided-dead (don't let it rot
silently). Each entry states **what**, **why dead**, **scope** (exactly what gets
deleted), **KEEP** (what must NOT be touched — the anti-footgun), **blocker**, and
**status**. Verify file:line against the infra map before cutting — a wrong "dead"
here causes a wrong deletion later. Move finished items to *Completed* with the commit.

**Status:** `QUEUED` (decided, not started) · `BLOCKED` (needs X first) · `IN-PROGRESS` · `DONE`.

---

## Decided removals (architecture direction — confirmed)

### R1. Legacy OFDM-wideband **file routing** (NOT "SR-ARQ") — `BLOCKED`
- **What:** the legacy path that sends a wideband OFDM **file** through the continuous
  windowed `SelectiveRepeatARQ arq_` instead of the burst transport.
- **Why dead:** burst transport (`BurstStopAndWaitController burst_transport_`) is THE
  OFDM-wideband file method going forward (default ON since 2026-05-30, commit `c40a0b5`).
  We are not going back to the windowed-file model.
- **Scope (delete):** the `!use_burst_transport_` branches in `connection.cpp`
  (file-routing sites ~`1632`/`1707`/`1855`/`2327`/`2366`/`2394`/`2759`), the
  `use_burst_transport_` member + its `false` default, and the `ULTRA_BURST_TRANSPORT`
  opt-out knob (`connection.cpp:353`, `app.cpp:593`, `modem_engine.hpp:432`). Make burst
  unconditional for OFDM-wideband file transfer.
- **⚠ KEEP (do not over-cut):**
  - **`SelectiveRepeatARQ arq_` stays** — it still serves **MC-DPSK data, OFDM_NARROW
    data, and ALL control ACKs**. Only the *wideband-file routing through it* is removed.
  - **Burst is itself selective-repeat** (GROUP_ACK 6-bit SACK `frame_mask`,
    resend-failed-only + refill). This is NOT a "remove SR-ARQ" task — SR semantics are
    shared by both controllers. Do not rip out SACK / selective-repeat machinery.
- **Blocker:** the auto rate ladder is mid-rework (floors not re-established), so the
  burst default path is not yet throughput-proven end-to-end on the GUI gate. Keep the
  `=0` fallback until burst is proven post-ladder-rework, THEN delete the legacy routing
  + knob. (Tracking: the burst-default flip itself already shipped regression-free.)

### R2. Operator chat-message (free-text) feature — `QUEUED`
- **What:** interactive operator chat / free-text message sending.
- **Why dead:** long LDPC (n=1944) + the burst interleaver make short interactive chat
  impractical; the modem is specializing for **file transfer**. Decision 2026-05-30.
- **Scope (delete):** `ProtocolEngine::sendMessage` path, message fragmentation /
  reassembly, message-level ARQ, and the chat-message test cases in `test_protocol.cpp`
  ("Long fragmented message…", "Post-cancel sender message…", short-message status).
- **⚠ KEEP:** control-frame text inside the protocol (callsign, status) is not chat —
  don't remove protocol control plumbing. Verify the file-transfer path shares nothing
  with the message fragmenter before cutting.
- **Blocker:** none hard; do after the burst/file path is settled so we don't churn the
  shared frame plumbing twice.

---

## Dead code — audit-confirmed (verify no test-tool dependency, then cut)

Sourced from `MODEM_INFRASTRUCTURE_MAP.md §7` (file:line authoritative there):

- **R3. Schmidl-Cox TX preamble generators** — `modulator.cpp` `generatePreamble():551`,
  `createSchmidlCoxSTS():332`, `g_logged_tx_pilots:122`, `generateProbe():662` (verify).
  Test-tool-only; OFDM_COX is gone. **KEEP the S-C *correlation primitives*** in
  `ofdm_sync.cpp` — reused by warm-LTS sync.
- **R4. Reserved enum stubs** — `CodecType::{LDPC_5G,CONVOLUTIONAL,TURBO,POLAR}`;
  waveform `OTFS_EQ/OTFS_RAW/MFSK`; FrameType `DATA_START/CONT/END` (or finish the
  file-segment impl). Unreachable in production. Mind wire-compat (reserved values).
- **R5. Dead constellation mappers** — QAM32/64/256 in `modulator.cpp:85` (`mapBits`):
  no auto rung reaches them. Confirm no forced-mod test path first.
- **R6. `carrier_ldpc_interleaver`** — default-off diversity layer, auto-on only on masked
  carriers; likely removable. Confirm the masked-carrier path is truly unused first.

## Deprecate (divergent, not yet removed)

- **R7. `ultra_tnc` in-process `--inject-channel` AWGN (`applyAwgn`)** — a divergent 3rd
  channel; OTASim is THE single channel (shared Watterson + real-HF noise beds). Route
  ultra_tnc's channel through OTASim, then delete the in-process injector. (See memory:
  *OTASim is the single channel*.)

## Doc-only stale (not a code deletion)

- **R8. SC-DPSK** is listed in the CLAUDE.md waveform table but is not an `IWaveform` and
  not in the factory — fix the doc, no code to remove.

---

## Completed removals (record / momentum)

| Done | What | Commit |
|------|------|--------|
| 2026-05-30 | `decode_bench` tool + `DecodeBenchReplay` CTest + 6 replay fixtures | `bdee556` |
| 2026-05-30 | `agents/` autonomous system + Mac↔Pi5 hardware-cable rig + CI adaptation | `833725d` |
| 2026-05-30 | `cli_simulator` + `test_waveform_simple` + `SimulatedStation` (~14k lines) | `207a0af` |
| (earlier) | OFDM_COX as a selectable mode (enum `0x00` now reserved; S-C primitive kept) | — |
