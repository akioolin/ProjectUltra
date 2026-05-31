# Sync Acquisition Fix Plan (2026-05-31)

Implements the root cause traced in `SYNC_SUBSYSTEM_AUDIT_2026_05_29.md §9.7`, extended with
2026-05-31 low-SNR floor evidence and code-verified path ownership. Goal: make OFDM connected-data
sync **rock-solid at every SNR the modem supports**, so the burst/OFDM path inherits the low-SNR
reach the rugged MC-DPSK path already has.

---

## 1. The problem (plain English)

Before reading a chunk of data, the receiver must find **where it starts** ("sync"). Two methods:
- **Full chirp** ("foghorn") — a loud frequency sweep with ~30 dB of matched-filter processing
  gain. Findable at deeply negative SNR. Reliable but costs airtime.
- **Light LTS** ("quiet nudge") — a quiet marker; the receiver *predicts* where the next chunk is
  from timing. Fast and cheap, but quiet → easy to lose in noise.

To go fast, OFDM file transfer uses the quiet nudge mid-burst. The receiver then applies a
**correlation gate**: *"accept the chunk only if the LTS marker correlates above a loudness cutoff."*
In noise the marker dips under the cutoff, so the receiver **rejects a chunk whose actual data is
perfectly decodable** → NACK 0x00 → resend forever → `max retries`. Measured: DQPSK R1/4 at AWGN@10
the warm gate locks **1 of 11**, file fails; the *same data* on the legacy per-frame full-chirp path
decodes **776 codewords** and delivers. The data was always fine — the doorman checked the wrong thing.

## 2. Root cause (audit §9.7, confirmed)

- The **chirp detector is healthy** — not the bug. ("Down chirp NOT found" is a benign
  window-alignment transient; it locks once the window spans up+gap+down.)
- The blocker is the **acceptance gate**: WARM contiguous-data is gated by an **LTS-correlation
  threshold** (`lightSyncThresholds` → 0.90 coherent / 0.52 / 0.50 differential) that was designed
  for **COLD** acquisition. A fresh, already-positioned frame whose LTS correlation dips on a fade or
  at low SNR is thrown away even though its timing is known and its data would decode.
- **Principled fix (audit lock-in):** in WARM mode the LTS provides *channel estimation (H)*, not an
  *acquisition decision*. Acceptance confidence = (fresh anchor) + (position match) + (downstream
  **LDPC**), NOT LTS correlation. The correlation gate applies only to COLD / RE_ACQUIRE.

- **CRITICAL CONSTRAINT (measured 2026-05-31, AWGN@10 DQPSK R1/4):** the warm light-LTS correlation
  there is **0.15–0.16 — the noise floor** (`DATA sync rejected (corr=0.16 < 0.52)`), not a dipped
  0.55. So the fix **cannot** be "lower the threshold" (0.15 *is* noise → accept-everywhere). The LTS
  is still usable for H once positioned (legacy decoded 776 CW on the same signal), but its
  correlation peak is **unfindable by search**. The frame must be located **entirely by the cadence
  prediction**, then demod+LDPC. This makes **Phase A (accurate cadence) load-bearing** — WARM
  processes at `next_expected` with NO correlation search at all, not a relaxed one.

## 3. Path ownership — VERIFIED in code (2026-05-31)

| Waveform | Connected-data sync path | Affected? |
|----------|--------------------------|-----------|
| **MC-DPSK** | `supportsDataPreamble()=false` → `use_light_search=false` (`streaming_sync_acquisition.cpp:213`); base `detectDataSync` falls back to full `detectSync` (`waveform_interface.hpp:138`); explicit `MC_DPSK` gates (`:135/:421`) | **No — cannot regress** |
| **OFDM_CHIRP** (wideband) | `detectDataSync` + `evaluateLightSyncCandidate` (`:639/:644`) | **Yes — primary** |
| **OFDM_NARROW** | same gate; `is_narrowband` (`:605`) just selects narrowband thresholds | **Yes — fixed for free** |

→ One fix in the shared OFDM light path lifts wideband **and** narrowband; MC-DPSK is untouched.
This is the project's "fix the family by construction, not per-mode" standard.

## 4. The fixes (multiple — phased, audit §9.5 order)

**Phase A — Timing prediction reliability (prerequisite).** "Process at the predicted position"
only works if the position is right. The audit found `expected_frame_gap_samples_` is **never set**
(`setExpectedFrameGapSamples` has no caller), so `next_expected_frame_sample_` mispredicts (off by
seconds). Fix the cadence so `next_expected` lands on the real next-frame arrival.

**Phase B — The acceptance rule (the core change).** In WARM phase, at the predicted position:
**accept the candidate and let LDPC decide** — do NOT reject on LTS correlation. Generalize the
existing `s16_warm_handoff` override (`:663`, currently coherent-only, floor 0.55) into the
principled rule for **all** mod classes: position-gated + LDPC-validated, correlation floor removed
for WARM. If LDPC fails → count a miss / NACK that frame; the 0.90/0.52/0.50 gate stays for
COLD/RE_ACQUIRE only. Site: `streaming_sync_acquisition.cpp:639-690`, `signal_policy::
{lightSyncThresholds, evaluateLightSyncCandidate}`.

**Phase C — Adaptive escalation (RE_ACQUIRE).** When LDPC keeps failing at the predicted position
(genuinely lost, or signal too weak), **escalate to a full chirp** for the next group-start
(robust re-acquire), then drop back to the quiet nudge. This is the speed-vs-toughness dial: cheap
when strong, robust when weak. Maps to the audit's `RE_ACQUIRE` SyncMode + the existing §16.4
full-chirp escalation.

**Phase D — Consolidate (remove the tangle).** Reduce the 6 sync entry points to **2** (`detectSync`
foghorn + `detectDataSync` quiet-nudge) and one `SyncMode {COLD, WARM, WARM_RELAXED, RE_ACQUIRE}`
state machine with one threshold policy. Delete the legacy short re-anchor (path 3) on the burst
path; constrain the path-5 fallback to a defined role; collapse the 11 `expect_full_ofdm_anchor_`
flips + 3 miss counters into explicit transitions. (Audit §9.4.)

## 5. Test plan (faithful gate)

Forced-rung floor probes via `gui_qso_scenario.sh` (`ULTRA_FORCE_WAVEFORM=OFDM_CHIRP
ULTRA_FORCE_DATA_MOD=DQPSK ULTRA_FORCE_DATA_RATE=R1_4`):
- **Target:** DQPSK R1/4 **AWGN@10 must now DELIVER** (legacy proved the PHY decodes there: 776 CW).
  Then Good@10, then sweep SNR down to find the *true* end-to-end floor.
- **No-regress:** AWGN@20 QPSK R3/4 still PASS (baseline 1610 bps); coherent warm path unchanged at
  high SNR; OFDM_NARROW floor improves or holds; MC-DPSK 5 dB floor untouched; `ctest` green.

## 6. Guardrails

- **Keep COLD/RE_ACQUIRE strict** (the 0.90/0.15 gates) — first contact must not false-lock on noise.
- **Bound the "accept + LDPC" cost** — only at the predicted position (one LDPC attempt per predicted
  frame), never a free-running decode on noise.
- **Do not touch MC-DPSK** (verified isolated) or the narrow *chirp-detect* path 4.
- Land behind the existing `ULTRA_S16_WARM_HANDOFF` checkpoint until the floor probe + no-regress
  pass, then promote to default.

---

## 7. Central `SyncController` module — target architecture

Today the sync logic is smeared across **four layers** (detectors in `chirp_sync.hpp` + waveforms;
orchestration in `streaming_sync_acquisition.cpp`; **state as 12 loose members on `StreamingDecoder`**;
policy in `signal_policy`/`arrival_policy`), with **6 entry points, 11 flip-sites for one flag, and
3 miss counters**. The bug leaked in *because* no one can hold that in their head. The refactor
collapses it into one object.

### 7.1 Responsibility (single, sharp)

`SyncController` owns **all sync state and the acquisition decision** for a connection — one object,
one place, readable top-to-bottom.

- **Owns (moves in):** the `SyncMode`, the timing prediction (next-frame position + cadence gap),
  arrival confidence, miss counters, `last_cfo`; the *which-detector / which-window / accept-or-not*
  orchestration; the acceptance rulebook.
- **Does NOT own (stays out):** the DSP detectors (`chirp_sync::detectDualChirp`,
  `IWaveform::detectSync`/`detectDataSync`) — the controller *calls* them through the waveform
  interface; and the demod + LDPC decode — the loop runs those and reports the result back.

### 7.2 State machine — 3 states, explicit transitions

| State | Meaning | Detector | Acceptance |
|-------|---------|----------|------------|
| `COLD` | no timing lock (handshake / first frame / fully lost) | full chirp+LTS, wide ~2.5 s window | strict correlation (0.90 coh / 0.15 chirp) |
| `WARM` | locked + predicting | group boundary → descriptor chirp re-anchor; contiguous → light LTS (channel est. only) | **position + LDPC** (no correlation gate) |
| `RE_ACQUIRE` | lost (LDPC failed N× at predicted spot) | forces a full chirp on the next anchor | strict (back to chirp lock) |

Transitions (named — these *replace* the 11 `expect_full_ofdm_anchor_` flips):
```
COLD        --chirp+LTS lock (strict)----------> WARM
WARM        --contiguous frame: LDPC ok--------> WARM        (advance prediction)
WARM        --group boundary: descriptor chirp-> WARM        (re-anchor)
WARM        --LDPC fail × N--------------------> RE_ACQUIRE
RE_ACQUIRE  --full chirp re-lock--------------> WARM
RE_ACQUIRE  --no lock × M / timeout-----------> COLD
(any)       --connection / mode reset---------> COLD
```

### 7.3 API (the whole clean surface)

```cpp
enum class SyncMode { COLD, WARM, RE_ACQUIRE };
struct SyncDecision { bool found; bool tentative; size_t pos; float cfo; SyncMode mode; };

class SyncController {
public:
  void reset(WaveformMode mode, IWaveform* wf);              // connect / mode-change → COLD

  // Per decode-loop tick: decide + run the right detector.
  //  COLD / group-boundary : found=true only on a real chirp lock.
  //  WARM contiguous       : tentative=true at the PREDICTED position (process it; LDPC judges).
  SyncDecision detect(SampleSpan buffer, size_t buffer_abs_start);

  // Loop calls this after it demods + LDPC-decodes the frame at SyncDecision.pos:
  void reportFrameOutcome(bool ldpc_ok, size_t frame_end_abs);   // advances prediction; drives transitions

  void noteGroupBoundary(const BurstDescriptor& d);          // a new group started (fresh anchor expected)
  SyncMode mode() const;  bool isWarm() const;
};
```

The `tentative` + `reportFrameOutcome` pair **is** the "position + LDPC, not correlation" rule —
expressed exactly once, in one place. MC-DPSK is handled by the *same* controller with no special
case: `supportsDataPreamble()==false` ⇒ WARM's "light LTS" call falls back to the full chirp, so it
stays robust by construction.

### 7.4 Migration map — what moves in, what gets DELETED

**Move into `SyncController` (delete from old site):**
- `StreamingDecoder` members → controller state: `warm_sync_phase_`, `warm_sync_active_`,
  `frame_arrival_confidence_`, `consecutive_sync_misses_`, `sync_reject_streak_`,
  `next_expected_frame_sample_(_valid_)`, `expect_full_ofdm_anchor_`, the cadence gap (audit §1.2).
- `streaming_sync_acquisition.cpp` orchestration → `detect()`: the `use_light_search` decision (`:213`),
  detector dispatch (`:639/:747/:777`), warm-window planning, the MC-DPSK/narrow gating.
- `signal_policy::{lightSyncThresholds, evaluateLightSyncCandidate}` + `arrival_policy::
  {planWarmSearchWindow, WarmSyncPhase}` → private helpers inside the controller.
- `streaming_burst_interleave.cpp` descriptor re-arm → `noteGroupBoundary()`.

**Delete outright (dead / superseded):**
- `detectShortDataSync` + the short-re-anchor plumbing (legacy `66db2d8`, path 3).
- The path-5 "full-anchor fallback" special case → becomes the `RE_ACQUIRE` transition.
- The `s16_warm_handoff` 0.55 coherent-only override (`streaming_sync_acquisition.cpp:663`) →
  becomes the general WARM rule.
- `expect_full_ofdm_anchor_` and its 11 flip-sites → the `SyncMode` enum.

**Stays untouched:** `chirp_sync::detectDualChirp` (the radar primitive), `IWaveform::detectSync/
detectDataSync` (the detectors), the MC-DPSK path (handled by construction), the narrow chirp-detect
dual-listen (path 4).

### 7.5 Incremental, reversible migration (each step flag-gated + GUI-gated)

1. **Shell move (no behavior change):** introduce `src/sync/sync_controller.{hpp,cpp}`; move the 12
   state members in; wrap the *existing* logic unchanged. Prove **byte-identical** on the GUI gate.
2. **Phase B fix inside it:** WARM = position + LDPC acceptance. Gate: **DQPSK R1/4 AWGN@10 delivers.**
3. **Phase A:** fix the cadence prediction inside the controller.
4. **Phase C:** add `RE_ACQUIRE` escalation.
5. **Phase D:** delete the old scattered code + dead paths — now safe; the controller owns it.
6. **Promote** past `ULTRA_S16_WARM_HANDOFF` once floor-probe + no-regress + `ctest` all pass.

Net: a smeared 4-layer, 6-entry, 12-variable sprawl → **one `SyncController`, 3 states, 2 detectors,
1 rulebook** — with the low-SNR bug fixed by construction inside it.

---

## 8. Status (2026-05-31) — sync fix landed; the remaining blocker is a SEPARATE z-state bug

**Sync position-gating fix DONE** (`streaming_sync_acquisition.cpp`, behind `ULTRA_S16_WARM_HANDOFF`):
in WARM, at the predicted position, accept + process even when the light-LTS correlation is at the
noise floor (~0.15 at DQPSK R1/4 AWGN@10) — don't gate on correlation, let LDPC decide. Verified:
AWGN@10 now **syncs at the predicted spot → demods real data → reaches LDPC → assembles the 6-frame
group** (was: never even attempted the data, stuck in chirp search). The sync layer is no longer the
wall.

**REMAINING BLOCKER — a separate, deeper z-state desync bug (the burst group decodes 0/6,
`max_iters=0`):** the per-group `setActiveLDPCLiftingZ(27)` reset
(`streaming_burst_interleave.cpp:734`, added only to size the *next* 1-CW z=27 BURST_HEADER search)
toggles the demod's `active_ldpc_block_size` back to 648. So the demod populates `soft_bits_` at
**z=27 (1296 = 2×648)** while the data is **z=81 (3888 = 2×1944)** → wrong-length codewords → LDPC
bails instantly → `0/6`. Measured smoking gun: descriptor sets z=81, `processPresynced` reads
`need 1944`, yet `getSoftBits` yields `1296`. There are **≥3 z-state copies** (`ldpc_lifting_z_`,
demod `active_ldpc_block_size`, the interleaver size) set/reset from scattered sites.

**Why coherent QPSK R3/4 AWGN@20 works:** it delivers every group fast and never desyncs the toggle.

**The fix (user's model — proven correct):** z=81 is a **FILE-TRANSFER mode** that persists for the
whole transfer; the reset to z=27 belongs at **transfer-end**, not group-end. Decouple the two uses:
the descriptor *search* uses a **fixed-local z=27** (the next BURST_HEADER is always 1-CW z=27 — a
constant, not a toggled variable); the **data** uses the descriptor's declared z=81 for the whole
transfer. Must touch the demod's `active_ldpc_block_size` **lifecycle** (establish z=81 BEFORE the
data demod and hold it through the group) — a patch at the soft-bit *extraction* point was
insufficient because the buffer is already built at the wrong z during `process()`.

**Next task:** the file-transfer-z-mode decoupling above. Well-specified, single-purpose. The
SyncController scaffold (commit `4061f44`) is the eventual home for the consolidated z-state, but the
decoupling can land first as a targeted fix.
