# Component Decomposition Plan — the "one component per night" roadmap

**Purpose.** The leaf DSP/PHY stages are already their own classes. What remains is decomposing the
three **orchestrator god-classes** into encapsulated components, each owning its state behind a defined
read/write seam, so we can *"pick one component a night and rewrite it properly."* The §7 `SyncController`
carve-out is the working template for how to do this safely.

| God-class | Size | Role |
|-----------|------|------|
| `StreamingDecoder` | **65 methods / 5 .cpp files** | RX conductor (the big one) |
| `ModemEngine` | 62 methods | top-level TX+RX+audio engine |
| `StreamingEncoder` | 26 methods | TX conductor |

> Why they resist a clean split: a real-time modem RX is **not a feedforward pipeline** — it has
> feedback loops (CFO, warm-sync prediction, ARQ, burst-z, mode-change). Stages **share state** across
> those loops. So you cannot box the god-class by cutting lines; you must **define the seam** (the
> interface each component exposes). Defining the seam *is* the work.

---

## The method (proven by §7 — reuse it every night)

1. **Pick the concern's STATE cluster** in the god-class (the table below groups them).
2. **Move the state into a class.** Absorb external **WRITES** into named methods (`noteX()`/`reportX()`/
   `setX()`); expose **READS** via getters. The writes are always a *small set of logical operations* even
   when scattered across many sites (e.g. C4's 8 `sync_reject_streak_=0` writes → one `clearRejectStreak()`).
3. **perl-rename the call sites** + **gate** each step: ctest red-set byte-identical, then GUI floor
   (R1/4 AWGN@10) + Good@12 + no-regress (R3/4 AWGN@20). Pure code-motion ⇒ byte-identical.
4. **Privatize** the fields once their writers live in the new class. Commit; never push unreviewed.

Golden rule: **a stage that the test matrix can't exercise stays put** (e.g. the dual-listen narrowband
`waveform_` swap is gate-untested → it stayed decoder-side in C3). Don't move what you can't prove.

---

## The RX god-class state — grouped into candidate components

Every member below lives on `StreamingDecoder` today (unless marked → already moved). Grouping them by
concern *is* the decomposition.

| # | Candidate component | State it owns (the cluster) | Status |
|---|---------------------|-----------------------------|--------|
| 0 | **`SyncRingBuffer`** | `buffer_`, `write_pos_`, `correlation_pos_`, `total_fed_`, `buffer_mutex_`+`data_cv_`, `search_floor_*`, `noise_floor_` + ring helpers | ✅ DONE (§7 C3 P1-2; now owned by SyncController) |
| 1 | **`SyncController`** | warm-sync prediction (`sync_reject_streak_`, `frame_arrival_confidence_`, `consecutive_sync_misses_`, `warm_sync_active_`), cadence (`next_expected_*`, `expected_frame_gap_`), `last_cfo_` (CFO loop), burst-z descriptor (`have/last_burst_descriptor_`), `expect_full_ofdm_anchor_`, + the acquisition decisions | 🔶 IN PROGRESS (§7 C3/C4) |
| 2 | **`ModeChangeScheduler`** | `pending_descriptor_rate_change_`/`_mod_`/`_rate_`, `pending_connected_ofdm_change_`/`_mode_`/`_mod_`/`_rate_` | ⬜ candidate (isolated) |
| 3 | **`MCDPSKBurstContinuation`** | the `mc_burst_*` cluster (`next_pos/abs`, `snr`, `cfo`, `frames_decoded`, `pending_*`, `pending_soft_bits_`, `wait_start_time_`) | ⬜ candidate (self-contained) |
| 4 | **`OFDMBurstAssembler`** | `burst_soft_buffer_`, `burst_metric_templates_`, `burst_start_time_`, `last_burst_group_seq_`, `burst_physical_diag_`, `burst_diag_*`, `burst_group_size_` | ⬜ candidate |
| 5 | **`WaveformProfile`** (config) | `waveform_`, `narrow_waveform_`+`_initialized_`, `mode_`, `connected_`, `current_modulation_`, `code_rate_`, `detected_bandwidth_`, `mc_dpsk_config_`, `carrier_mask_`, `fixed_frame_codewords_`, `use_channel_interleave_`, `use_carrier_ldpc_interleaver_`, `use_burst_interleave_`, `burst_transport_rx_`, `interleaver_` | ⬜ candidate (read-mostly config) |
| 6 | **`FrameDemodulator`** | wraps `ChannelEqualizer` (already a class) + the demod-call orchestration (`applyCFOPreCorrection`, `decodeFixedFrame`, the LDPC call) | ⬜ candidate |
| 7 | **`DecodeStateMachine`** | `state_` (`DecoderState`), `sync_position_`, `sync_cfo_`, `sync_snr_`, `sync_correlation_`, `sync_from_*`, `pending_total_cw_`, `last_decoded_sync_pos_`, `reset_generation_` | ⬜ candidate (the conductor — carve LAST) |
| 8 | **`FrameOutput`** | `frame_queue_` + the callbacks (`frame_callback_`, `burst_group_callback_`, `ping_callback_`, `data_sync_accepted_callback_`, `harq_context_callback_`) | ⬜ candidate (thin) |
| — | already-classes used as members | `idle_noise_snr_estimator_` (`IdleNoiseSNREstimator`), `tone_burst_monitor_` (`ToneBurstAckMonitor`) | ✅ leaf classes |

---

## The coupling map — who writes / who reads (the seams)

This is the part that decides the carve order. "Feedback" = a later stage writes state an earlier stage reads.

| State cluster | Written by | Read by | Feedback loop? |
|---------------|-----------|---------|----------------|
| **ring** (`SyncRingBuffer`) | `feedAudio` (audio thread, producer) | `searchForSync` + decode (`decodeCurrentFrame`) | no (shared buffer, mutex-guarded) |
| **warm-sync prediction** | `noteGroupDelivered`, `noteFrameArrival*`, `clearRejectStreak` (now all in SyncController) | acquisition (`lightSyncThresholds`) + diagnostics/snapshot | ⮌ delivered-group → next acquisition window |
| **`last_cfo_`** | demod writes (`.store` after equalize) | acquisition reads (`.load`) | ⮌ **CFO feedback** (the classic one) |
| **burst-z descriptor** | OFDM burst decode (on BURST_HEADER decode) | acquisition + demod (per-frame z derivation) | ⮌ descriptor → rest-of-group decode |
| **`expect_full_ofdm_anchor_`** | burst decode (`BURST_HEADER`-consume keeper, group re-arm) | acquisition (`use_full_ofdm_anchor_search`) | ⮌ group boundary → next search |
| **pending mode-change** | descriptor decode | applied at next frame boundary (state machine) | ⮌ descriptor → next-frame profile |
| **OFDM burst assembly** | demod (appends soft bits per frame) | `finalizeBurstGroup` (deinterleave + LDPC the group) | no (forward accumulate) |
| **MC-DPSK continuation** | MC-DPSK decode (per frame in a multi-frame burst) | the next continuation tick | no (forward accumulate) |
| **decode state machine** | every stage transitions it | every stage reads it | n/a (it IS the conductor) |
| **waveform profile** | mode-change apply + dual-listen swap | every stage (config) | no (read-mostly) |

---

## Nightly backlog — ordered by isolation (least-coupled first)

Carve the isolated forward-accumulate clusters first (cheap wins, build confidence), save the feedback-
heavy conductor for last.

1. **Finish `SyncController` (§7 C3/C4)** — IN PROGRESS. Cadence fields (`next_expected_*`) are the last
   clean-privatize follow-on; `last_cfo_`/burst-z/`expect_full_ofdm_anchor_` stay public until their
   owning concerns (CFO, burst) are carved.
2. **`ModeChangeScheduler`** (cluster #2) — **easiest next.** The `pending_*` fields are written on
   descriptor decode and consumed at the next frame boundary — a clean one-writer/one-reader seam, no
   feedback into acquisition. Method seam: `scheduleModeChange(...)` + `applyPendingModeChange()`.
3. **`MCDPSKBurstContinuation`** (cluster #3) — self-contained `mc_burst_*` state, lives only on the
   MC-DPSK decode path (forward accumulate, no feedback). Carve into its own object; the decoder holds one.
4. **`OFDMBurstAssembler`** (cluster #4) — forward-accumulate group assembly; the `noteGroupDelivered`
   seam to SyncController already exists. Seam: `appendFrame(soft_bits)` / `finalizeGroup()` →
   `{group, mask, quality}`.
5. **`WaveformProfile`** (cluster #5) — read-mostly config object; widely read but rarely written
   (mode-change + dual-listen swap). Encapsulating it tames a lot of `if (mode==X)` sprawl. ⚠ the
   dual-listen swap mutates `waveform_` — handle like C3 (decoder owns the swap, profile carries the rest).
6. **`FrameDemodulator`** (cluster #6) — wraps `ChannelEqualizer`; the seam is the CFO feedback
   (`last_cfo_` in/out). Do AFTER a `CFOTracker` exists so the feedback has a home.
7. **`CFOTracker`** — extract the `last_cfo_` feedback loop (currently split SyncController↔equalizer)
   into one owner. Trickiest (it IS a feedback loop); needs careful gate on fading (Good@12).
8. **`DecodeStateMachine`** (cluster #7) — the conductor; carve LAST, once the stages it sequences are
   their own components. At that point `StreamingDecoder` becomes a thin wiring shell.
9. **`FrameOutput`** (cluster #8) — thin; bundle `frame_queue_` + callbacks. Low priority, low risk.

### After RX: TX + top-level
10. **`StreamingEncoder`** (26 methods) — mirror the RX carve: waveform profile, frame builder, burst
    grouper, preamble emitter. Smaller; reuse the same method.
11. **`ModemEngine`** (62 methods) — mostly TX/RX/audio **wiring**; decompose last, once both ends are
    componentized — it should shrink to a thin composition root.

---

## Anti-footguns (hard-won)
- **Gate-untested stages stay put** (dual-listen narrowband, any path the wideband matrix can't reach) —
  moving them is an unprovable-regression risk.
- **Pass the live waveform IN; don't trust a stored copy** — `waveform_` gets swapped (dual-listen);
  a stale member-pointer is a trap (C3 learned this).
- **Feedback fields privatize last** — `last_cfo_`, burst-z, `expect_full_ofdm_anchor_` are written by a
  *downstream* stage; they only encapsulate cleanly once that downstream stage is itself a component.
- **One concern per night, byte-identical, gated.** Never a big-bang rewrite — 6 months of subtle coupling
  cannot be safely cut in one go (the project's documented scar tissue).

---

*Authored 2026-06-01 during the §7 C4 gate-wait. Grounded in the live `StreamingDecoder` member inventory.
Update the Status column as components are carved.*
