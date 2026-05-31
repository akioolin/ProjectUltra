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

### R3. Differential SELECTION on the OFDM_CHIRP path — `DONE` (selection); code removal `BLOCKED on OFDM_NARROW`
- **What:** retire differential modulation from the **wideband OFDM_CHIRP** band. SNR ≥ 10
  (AWGN/Good/Moderate) → coherent QPSK; Poor → MC-DPSK.
- **Why:** mode selection routes SNR<10 / Poor → MC-DPSK (differential, its real home) and SNR≥10 →
  OFDM. Coherent beats differential across the OFDM band — GUI clean-rate **81% Good@10 / 89-90%
  Moderate@14 (2 seeds) vs differential 32%**, `max_iters` flat 1–6 as fading sped up. Full rationale:
  `docs/OFDM_COHERENT_ONLY_DECISION_2026_05_31.md`.
- **DONE (commit `4c72a51`, thread A2):** the *selection* of differential on OFDM_CHIRP is removed —
  `recommendDataMode` OFDM default DQPSK→QPSK, D8PSK rungs deleted, Poor routes to MC-DPSK
  (`kOFDMEntryFloorPoorDb` unreachable), OFDM_CHIRP ladder rung mod→QPSK, policy tests updated. The
  bug-causing coherent-vs-differential *ambiguity* on OFDM_CHIRP is GONE (no path forks on it).
- **⚠ BLOCKED — the differential CODE cannot be deleted yet (verified 2026-05-31):** **OFDM_NARROW
  uses the SAME `OFDMChirpWaveform` + `ofdm_stream_processor` demod and is still DQPSK**
  (`streaming_decoder.cpp:788/847`, `waveform_selection.hpp:575`). So the `is_differential` demod
  branches, the magnitude-only `|H|` path, `differential_prev_erased_`, AND the
  `profileForDataMode(DQPSK)→DQPSK` control switch are **LIVE for OFDM_NARROW** — deleting them
  breaks narrowband. The **carrier-LDPC is LIVE for coherent** OFDM_CHIRP (`cldpc=1` in the coherent
  runs; the air-block fix `9189b70` serves it) — also not deletable.
- **PLAN for code deletion (user-chosen 2026-05-31): DISABLE OFDM_NARROW now → remove the differential
  code → REVAMP OFDM_NARROW as coherent later.**
  1. **Disable** OFDM_NARROW by dropping it from `ModeCapabilities::ALL` (`frame_v2.hpp:51`) — a single
     clean lever: it stops being advertised/negotiated/constructed (the narrow handlers in
     `connection.cpp` become harmless dead branches). The narrowband mode is secondary to the wideband
     burst focus, so losing it temporarily is acceptable.
  2. **Verify** no OFDM path ever holds a DQPSK/D8PSK config (default/transient/fallback) — only then
     are the `is_differential` demod branches + `profileForDataMode(DQPSK)→DQPSK` control switch truly
     dead. (OFDM_CHIRP is coherent QPSK incl. its control profile; MC-DPSK uses a SEPARATE demod.)
  3. **Remove** the now-dead differential demod/control branches (NOT carrier-LDPC — still live for
     coherent). KEEP the coherent `dd_qam16` tracker + MC-DPSK + the `Modulation` enum.
  4. **Later — revamp OFDM_NARROW as COHERENT** (reuses the coherent OFDM machinery with a narrowband
     config; no differential code needed). **⚠ This is a real PHY re-validation, NOT a config flip:**
     narrowband ~17 dB / 500 Hz is where differential's no-phase-reference robustness is the point, so
     coherent narrow may land at a HIGHER SNR floor (fewer carriers for the equalizer to track phase
     across). Don't assume it reaches the differential floor.
- **⚠ KEEP regardless:** MC-DPSK differential (`multi_carrier_dpsk.hpp`); the `Modulation` enum
  `DQPSK/DBPSK/D8PSK`; the **COHERENT** DD tracker `dd_qam16_*` (`channel_equalizer_equalize.cpp:636+`).

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
