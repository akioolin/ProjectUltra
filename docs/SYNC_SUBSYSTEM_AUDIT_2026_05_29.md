# Sync Subsystem Audit (2026-05-29)

**Status:** Diagnostic only. No code changes proposed in this commit. The §15 tone-burst ACK path (1.60 kbps Good@20) is unchanged and is the current production lever. Safe revert tag `safe-revert-pre-s16-2026-05-28` (commit `3ac74f1`) remains the rollback target.

**Trigger:** §16.8 step 2 v1/v2/v3 each fixed one symptom and exposed the next, finishing at 8/11 groups delivered on Good@20 with no recovery from sync stalls. The user's observation: *"maybe the whole sync search has some bugs ... we had a lot of AI iterations in that area."* Confirmed — 18+ rounds of Codex iteration on sync (Tasks #2-#22 in the project log alone). This document maps everything before any further changes.

**Method:** Read all sync code paths end-to-end, list every state variable and threshold with the file:line where it's written/read, classify behaviours as intentional vs accumulated band-aid, and reverse-engineer the 3-5 architectural intents underneath.

**Multi-perspective stack applied:** PHY theorist (correlation-detector math, false-positive analysis under N-doubling), Real-time DSP systems engineer (state-variable lifecycle, threading), Veteran HF operator (half-duplex turnaround timing, what "warm sync" means on a real radio at 2 AM), First-principles physics (the inescapable Doppler-bounded coherence time vs delay-spread-bounded coherence bandwidth).

---

## 1. Inventory

### 1.1 Code paths (entry points for sync detection)

| # | Path | When invoked | Window | Threshold | Output |
|---|---|---|---|---|---|
| 1 | `waveform_->detectSync()` (full chirp+LTS) | Disconnected handshake; `expect_full_ofdm_anchor_=true` | 120k samples (~2.5 s) | `CORR_DETECT_THRESHOLD=0.15` | sync_pos, CFO from chirp gap |
| 2 | `waveform_->detectDataSync()` (light LTS) | Connected, `expect_full_ofdm_anchor_=false`, no short re-anchor | 9600 samples (~0.2 s) | `lightSyncThresholds()` returns 0.90 / 0.52 / 0.50 by mod class | sync_pos, refined CFO from LTS |
| 3 | `waveform_->detectShortDataSync()` (short chirp + LTS) | Connected, adaptive short re-anchor enabled (fading class flag) | 9600 + short_chirp_lead | 0.15 (short-chirp internal) | sync_pos, CFO from short chirp |
| 4 | `narrow_waveform_->detectSync()` (narrowband chirp fallback) | Dual-listen at handshake when wideband fails | 120k samples | 0.15 | sync_pos for OFDM_NARROW |
| 5 | Full-anchor fallback inside connected (line 729 in `streaming_sync_acquisition.cpp`) | Connected, expected full anchor, didn't find chirp → re-try as light LTS | LIGHT_SEARCH_SIZE | "unknown frame" thresholds | sync_pos for control safety |
| 6 | Control-first peek (`streaming_ofdm_decode.cpp` lines 618-689) | After sync accepted, before data demod, when `pending_total_cw_==0` | n/a — uses the accepted sync position | LLR gate `kMinLLRForSingleCWDecode=1.5` | Decoded control frame OR fall through to data |

**Observation 1.1.a:** Paths 1 and 3 both use 0.15 chirp threshold but with very different chirp durations (500 ms full vs ~150 ms short). CFO precision differs by ~3×; documented in §16.2 of the PHY design doc as the cause of the 500-ms-short-reanchor failure mode.

**Observation 1.1.b:** Paths 2 and 5 both invoke `detectDataSync` but with different threshold profiles (light vs "unknown frame"). The "unknown frame" profile is set up at line 662 with `unknown_frame_uses_control_sync_threshold = false`, but the variable name suggests historical alternative behaviour. Comment vs code disagree — needs verification.

**Observation 1.1.c:** Path 6 (control-first peek) is not a sync path per se but a *demod path* triggered by sync acceptance. It side-effects the coherent channel estimate (§14.24 bug B) when the BURST_HEADER intercept doesn't pre-size `pending_total_cw_`. The §14.27 BURST_HEADER descriptor fix sets `pending_total_cw_` so the next frame skips the peek; this only works AFTER one BURST_HEADER has been seen. The very first frame after CONNECT still goes through the peek.

---

### 1.2 State variables — write/read map

The sync state lives in `StreamingDecoder`. Twelve variables interact:

| Variable | Type | Reset | Set on Success | Decayed on Miss | Read for decisions | Notes |
|---|---|---|---|---|---|---|
| `warm_sync_phase_` | enum {COLD, WARM, DEGRADED, RECOVERY} | COLD on mode-change reset | WARM on `noteFrameArrivalSuccess` | DEGRADED→RECOVERY on N misses | sync acquisition (3 places), s16-warm-handoff (1 place) | Decoupled from `warm_sync_active_` |
| `warm_sync_active_` | bool | false on reset / RECOVERY | true on `noteFrameArrivalSuccess` | false on RECOVERY only | `planWarmSearchWindow` input | Why distinct from `phase_` is unclear — possibly historical |
| `frame_arrival_confidence_` | float [0,1] | 0.0 on reset | `update.confidence` (~0.35 from frame-arrival policy) | × 0.65 per miss | window-activation gate (kMinWarmWindowConfidence=0.25) | Decays geometrically; refresh only via `noteFrameArrivalSuccess` |
| `consecutive_sync_misses_` | int | 0 on reset | 0 on `noteFrameArrivalSuccess` | +1 per miss | phase transition + relax-streak | Wrap-protected |
| `sync_reject_streak_` | uint64_t | 0 on reset | 0 on weak-accept or accept | +1 per rejected candidate | `lightSyncThresholds()` relax (5+) and rescue (8+) | **Different from `consecutive_sync_misses_`** — fires inside the LTS detector loop, not the warm-arrival path |
| `next_expected_frame_sample_` | size_t | 0 on reset | frame_end + `expected_frame_gap_samples_` | advanced by cadence on miss | `planWarmSearchWindow` center | Predicts the NEXT frame's arrival in absolute sample space |
| `next_expected_frame_sample_valid_` | bool | false on reset | true on success | false on RECOVERY | gate for warm-window activation | Mirrors `warm_sync_active_` in practice |
| `expect_full_ofdm_anchor_` | bool | true on connect | false after light decode | true on multi-frame miss / group end | branches between paths 1 and 2 above | **9 flip locations** — see §1.4 |
| `last_cfo_` | atomic<float> | 0.0 on reset | (4 paths — see §1.3) | not decayed | every sync attempt's `known_cfo` seed | Only state survives `resetFrameArrivalTrackingLocked` |
| `correlation_pos_` | size_t (ring index) | write_pos on reset | advanced by frame_len on decode | step by CORRELATION_STEP=4800 on silence | search-window start position | Mixed advance semantics — see §1.5 |
| `search_floor_abs_` | size_t (absolute) | 0 on reset / various | frame_sync_abs + frame_len after decode | unchanged | gates warm-window if `next_expected < search_floor_abs` | Anti-replay guard |
| `last_frame_start/end_sample_` | size_t | 0 on reset | absolute frame boundaries on success | unchanged on miss (start +=cadence) | warm-window cadence math | Used to compute `cadence = last_duration + expected_frame_gap_samples_` |

**Observation 1.2.a:** Three separate "miss" counters — `consecutive_sync_misses_` (in arrival policy), `sync_reject_streak_` (in LTS detector loop), and the implicit "have we seen WARM in a while" via `warm_sync_phase_`. They count *different* events but their relaxation logic is conceptually similar. Should be one.

**Observation 1.2.b:** `warm_sync_active_` and `next_expected_frame_sample_valid_` always toggle together in current code. Either flag is redundant or there was a historical case where they diverged.

**Observation 1.2.c:** `last_cfo_` is the ONLY state that survives `resetFrameArrivalTrackingLocked`. This is intentional (CFO valid across hand-offs) but undocumented — the §16.11 instrumentation revealed it. The reset function name lies by omission.

---

### 1.3 The four `last_cfo_` writers

> ⚠ **Line numbers below are wrong — corrected in §8.2.3.** Actual pilot-CFO store sites:
> ofdm_decode 1191/1238/1642/1931 + burst_interleave 436. Count of 5 is correct.

| # | Path | Location | Input source |
|---|---|---|---|
| 1 | Chirp detection (handshake / cold) | `ofdm_chirp_waveform.cpp:452` and `mc_dpsk_waveform.cpp:129` | `chirp_result.cfo_hz` (from up/down gap error) |
| 2 | Light LTS detection (connected) | `ofdm_chirp_waveform.cpp:780` | `known_cfo` arg → in: `last_cfo_.load()`, refined by LTS pilots |
| 3 | Short re-anchor (fading) | `ofdm_chirp_waveform.cpp:845` | Same as #2 |
| 4 | Pilot-tracking post-decode | `streaming_ofdm_decode.cpp:1108, 1155, 1559, 1848` + `streaming_burst_interleave.cpp:435` | `cfo_update.accepted_cfo` from `signal_policy::combinePilotCFO` with ±2 Hz drift clamp |

**Observation 1.3.a:** Five physical write sites for Path 4 (pilot tracking). Each calls `last_cfo_.store(...)` directly without going through a helper. If any code path forgets to clamp or accept-gate, drift can propagate. No central writer.

**Observation 1.3.b:** No write path validates the new CFO against the previous. The atomic store is unconditional. The ±2 Hz drift clamp is inside `combinePilotCFO` but bypassed for chirp re-detection.

---

### 1.4 The nine `expect_full_ofdm_anchor_` flip locations

> ⚠ **There are 11 sites, not 9 — corrected in §8.2.2.** Line 655 is double-listed (single
> `=false`; connect-`true` is at 700); decoder:861 (`=preserve_full_anchor` keeper) and
> decoder:1180 (hard reset) are missing.

| File:line | From → To | Trigger |
|---|---|---|
| `streaming_decoder.cpp:655` | true | Mode-change to connected OFDM |
| `streaming_decoder.cpp:655` | false | Mode-change to disconnected |
| `streaming_decoder.cpp:696` | false | Explicit `clearPendingOFDMFullAnchor()` |
| `streaming_decoder.cpp:710` | false | After consuming full anchor in `consumeFullOFDMAnchorsUntilFound()` |
| `streaming_ofdm_decode.cpp:866` | false | §16 warm-handoff keeper (env-gated) |
| `streaming_ofdm_decode.cpp:875` | true | Post-BURST_HEADER, warm-handoff NOT eligible |
| `streaming_ofdm_decode.cpp:954` | true | After 1-CW decode if multi-CW expected next |
| `streaming_ofdm_decode.cpp:1819` | false | After normal control/data decode (ready for light next) |
| `streaming_ofdm_decode.cpp:1829` | true | After burst-interleave group end / multi-frame failure |

**Observation 1.4.a:** This flag is the implicit state machine. It's a single bool guarding a binary decision (path 1 vs path 2). With 9 writers across 2 files there's no single source of truth for "what mode should the receiver be in next."

**Observation 1.4.b:** Several of these are inside conditional blocks (warm-handoff eligible, multi-CW expected). A SyncMode enum with explicit transition table would be more legible.

---

### 1.5 `correlation_pos_` advance semantics

Six distinct advance patterns:

| Pattern | Use | Reset target |
|---|---|---|
| Init / overflow | Cold start, ring overflow | `write_pos_` (skip stale data) |
| CORRELATION_STEP (4800) | Silence between attempts, mismatched RMS | current + step |
| Frame past | After successful or failed decode | sync_position + frame_len |
| `falseOFDMLockAdvance()` | Invalid LTS, training fail | next probable LTS position |
| Warm-plan window end | Warm-sync window scanned | `warm_narrow_end_abs` (absolute, converted to ring) |
| Backtrack 9600 | Before correlation runs | current − SEARCH_BACKTRACK |

**Observation 1.5.a:** Backtrack is intentional (to catch chirps that started before correlation_pos_ was last advanced). But interacts oddly with warm-window mode: in warm mode, the window is centered on `next_expected`, but the backtrack still happens, so the effective search range can overlap into old samples that were already scanned. Possible source of repeated false-locks.

---

### 1.6 Thresholds — what guards what

| Constant | Value | File:line | Guards |
|---|---|---|---|
| `CORR_DETECT_THRESHOLD` | 0.15 | (waveform-local) | Chirp peak (paths 1, 3, 4) |
| Coherent min_confidence | 0.90 | signal_policy:128 | Light LTS for QPSK / BPSK |
| Coherent weak_floor | 0.85 | signal_policy:129 | (Never invoked — see Obs 1.6.a) |
| Narrowband min_confidence | 0.50 | signal_policy:131 | Light LTS for OFDM_NARROW |
| Narrowband weak_floor | 0.40 | signal_policy:132 | OFDM_NARROW weak-accept |
| Connected wideband min_confidence | 0.52 | signal_policy:134 | Light LTS for differential (DQPSK/D8PSK) |
| Connected wideband weak_floor | 0.45 | signal_policy:135 | Differential weak-accept |
| `kConnectedOFDMLightSyncRelaxStreak` | 5 | signal_policy:97 | After 5 rejects: progressively lower min_confidence |
| `kConnectedOFDMLightSyncRescueStreak` | 8 | signal_policy:98 | After 8 rejects: extreme rescue weak_floor=0.35 |
| `kConnectedOFDMLightSyncRelaxFloor` | 0.40 | signal_policy:99 | Floor for the relax progression |
| `kConnectedOFDMLightSyncRescueFloor` | 0.35 | signal_policy:100 | Rescue weak_floor |
| `kMinWarmWindowConfidence` | 0.25 | frame_arrival_policy:16 | warm window activation |
| `kMinDegradedWindowConfidence` | 0.05 | frame_arrival_policy:17 | DEGRADED window activation |
| `kS16WarmHandoffMinCorrelation` | 0.55 | sync_acquisition:660 | §16 env-gated coherent override |
| `kMinLLRForSingleCWDecode` | 1.5 | (control-first peek) | LLR sanity gate |

**Observation 1.6.a:** Coherent weak_floor=0.85 is never invoked. The `evaluateLightSyncCandidate` weak-accept branch (line 195-200) is gated on `!is_coherent`. So coherent has no weak-accept rescue at all — a corr=0.85 is rejected the same as corr=0.20. Either delete the weak_floor constant or wire the gate. Today's behaviour is "0.85 weak floor is documentation of intent, not behaviour."

**Observation 1.6.b:** The narrow-window threshold reduction (lines 138-150) only applies to `!is_coherent && connected && !is_narrowband`. Coherent QPSK never gets the narrow-window discount. This is the root cause of the §16 v1 failure: the 0.90 threshold persists even when the window narrows by 41× (LIGHT_SEARCH_SIZE/candidate_span ≈ 9600/4416 ≈ 2.2× → 3.4 dB threshold reduction would be physically justified).

> ⚠ **Both window-reduction figures are wrong — corrected in §8.2.1.** Actual `candidate_span=2176`,
> so reduction ≈ **4.41×** → ~**6.4 dB**, and the `√r` law would drop 0.90 → ~0.43 (below the 0.52
> differential floor). The "41×" is also self-inconsistent with "3.4 dB." See §8.4 on why the `√r`
> law itself is suspect, and §8.3.2 (path-5 already gives coherent the 0.52 threshold when
> `expect_full_ofdm_anchor_=true`, so the 0.90 wall is not universal).

**Observation 1.6.c:** Three separate "give up" thresholds: relax (5), rescue (8), and the implicit infinite via `warm_sync_phase_=RECOVERY` (which deactivates warm-sync entirely after 4 misses per `kWarmSyncMissesBeforeRecovery`). They use different state vars (`sync_reject_streak_` vs `consecutive_sync_misses_`) tracking similar events.

---

### 1.7 The four `noteFrameArrival*` call sites

> ⚠ **There are 4 success sites, not 3 — corrected in §8.2.4.** Missing `ofdm_decode:1978`, which
> fires per-block in the burst-continuation loop. Obs 1.7.a's "only the last frame arrival fires"
> is therefore only half-true.

**Success (writes phase=WARM, conf, next_expected):**
- After BURST_HEADER decode (`streaming_ofdm_decode.cpp:780`)
- After valid control frame (`streaming_ofdm_decode.cpp:947`)
- After burst-interleave multi-frame success (`streaming_ofdm_decode.cpp:1816`)
- Per block in the burst-continuation loop (`streaming_ofdm_decode.cpp:1978`)

**Miss (decays phase, conf, increments misses):**
- In the light-search loop when no LTS found (`streaming_sync_acquisition.cpp:929`)
- After frame consumed with invalid training (`streaming_ofdm_decode.cpp:474`)

**Observation 1.7.a:** The burst-interleave success path at line 1816 fires *once* for the whole group (all 6 frames decoded as a unit). The §16.11 instrumentation found that this means the per-frame state machine is NOT driven during a burst body — only the last frame "arrival" fires. End-of-group phase reads as COLD until that single event lands.

**Observation 1.7.b:** No success event fires for the data group's group-start data frame as a SEPARATE event from the body's deinterleave success. So `noteFrameArrivalSuccess` never sees the predicted cadence between BURST_HEADER and data — `expected_frame_gap_samples_` doesn't get re-calibrated mid-burst.

---

## 2. Architectural intents (reverse-engineered)

After 18+ iteration rounds, the implementation has accumulated band-aids. Underneath, there are only **four real intents**:

### Intent A — Cold acquisition

Don't know where the sender is or what their CFO is. Do an exhaustive dual-chirp scan (~2.5 s window). Chirp threshold is 0.15 because the autocorrelation noise floor of HF audio is ~0.05-0.10. Output: position + coarse CFO from up/down gap error. Refines the timing further via the LTS that follows the chirp.

**Triggered:** disconnected mode, mode change, recovery from RECOVERY phase.

**Maps to current code:** path 1 (`detectSync` full chirp+LTS) and path 4 (narrow_waveform fallback).

### Intent B — Warm steady-state

We have CFO and a recent timing reference. The next frame should arrive within ±N samples of a predicted position. Search a narrow window (±20-50 ms) using LTS-only correlation. Threshold can drop because the search-space false-positive volume shrinks geometrically with window narrowing (Bonferroni argument).

**Triggered:** connected, prior frame decoded cleanly, fading_index low enough to trust prediction.

**Maps to current code:** path 2 (`detectDataSync`) when `expect_full_ofdm_anchor_=false`. Threshold-narrowing logic exists for non-coherent (lines 138-150) but coherent is excluded — Observation 1.6.b is the root cause of §16 pain.

### Intent C — Graceful re-acquisition

Warm prediction failed once or twice. Widen the window, relax the threshold, but don't go all the way back to cold (which costs 2+ seconds). Recover within a few hundred ms.

**Triggered:** sync miss while warm_sync_phase_ ≠ COLD.

**Maps to current code:** the `sync_reject_streak_` progression (5+ → relax, 8+ → rescue) AND the warm_sync_phase WARM→DEGRADED→RECOVERY progression. **Two parallel implementations of the same intent**, using different state variables, different thresholds, different counters. This is the #1 source of accumulated complexity.

### Intent D — Forced re-anchor

Sender just changed mode (PING→CONNECT, MODE_CHANGE), or we suspect drift accumulated. Demand a full chirp+LTS even though we're connected. One-shot.

**Triggered:** mode change, MODE_CHANGE protocol event, multi-frame failure, BURST_HEADER consume (today's "reset" path).

**Maps to current code:** `expect_full_ofdm_anchor_ = true` flip at 5 of the 9 locations in §1.4.

---

## 3. Classification — intentional vs band-aid

### Intentional (correct, keep)

- Cold dual-chirp acquisition with 0.15 threshold (Intent A).
- LTS-only light sync once warm (Intent B).
- Threshold scaling with window-narrowing (lines 138-150) — *correct in principle, wrong in scope* (excludes coherent).
- `last_cfo_` survives `resetFrameArrivalTrackingLocked` — the CFO IS a warm-hand-off asset and should survive (Intent B/C). The "reset" name lies, but the behaviour is right.
- BURST_HEADER descriptor provides authoritative group params — §14.27 fix is correct.
- Half-duplex anti-replay via `search_floor_abs_` — prevents re-detecting our own past TX.

### Band-aid (compensating for a real problem elsewhere)

- The coherent 0.90 threshold (Observation 1.6.b). Logic correct in spirit (coherent LTS can't DD-recover stale phase) but the value is hand-tuned. The principled fix is window-narrowing — same as non-coherent. Threshold drops naturally once Intent B is unified across modulations.
- The `sync_reject_streak_` relax progression (5/8). Parallel implementation of Intent C using a different counter. The principled fix is to unify the miss-counting state machine (only one of these needs to exist).
- The "weak accept" rescue at corr ≥ 0.35 (`kConnectedOFDMLightSyncRescueFloor`). Same family as relax — Intent C reinvention. Could be deleted once the principled Intent C is in place.
- Multiple `expect_full_ofdm_anchor_=true` flips (1.4). Implicit state machine; principled fix is an explicit SyncMode enum.
- The BURST_HEADER consume path resetting frame arrival tracking. §16 specifically attempts to NOT do this. The reset is a band-aid for "we don't know how to keep warm state across a control-frame decode that switches profile."
- The control-first peek (path 6). Defensive demod to filter false locks; works but side-effects coherent state. Mitigated by pending_total_cw_ which is a §14.27 band-aid for the side-effect.

### Compensating-band-aids on band-aids

- The `pending_descriptor_rate_change_` deferral to next processBuffer() — added because configure() was being called mid-decode and SIGSEGV'd. Threading band-aid.
- The `unknown_frame_uses_control_sync_threshold = false` literal (line 662) — variable name suggests an alternative path existed. Code claims it's always false. Either delete the variable or revive the path.
- The §16 v1/v2/v3 work itself — env-gated overlay attempting to disable a chain of resets. Will be unnecessary once the reset chain is removed.

---

## 4. The 3-5 real architectural intents — proposed clarification

The implementation is approximately right but obscured. A principled re-statement:

```
enum class SyncMode {
  COLD,           // Don't know position or CFO. Full chirp+LTS, wide window, threshold 0.15.
  WARM,           // Known CFO, predicted position. Light LTS, narrow window, threshold scaled.
  WARM_RELAXED,   // Recent warm miss. Widened window, lowered threshold.
  RE_ACQUIRE,     // N consecutive misses. Full chirp+LTS again, but keep last_cfo_ as seed.
};
```

Transition rules:

| From | Event | To |
|---|---|---|
| COLD | Successful chirp+LTS decode | WARM |
| WARM | Successful frame arrival in expected window | WARM (refresh confidence) |
| WARM | Sync miss | WARM_RELAXED |
| WARM | Force-anchor protocol event (MODE_CHANGE, BURST_HEADER if eligible — see Intent D) | RE_ACQUIRE |
| WARM_RELAXED | Successful frame arrival | WARM |
| WARM_RELAXED | N more misses | RE_ACQUIRE |
| RE_ACQUIRE | Chirp+LTS lock | WARM |
| RE_ACQUIRE | Persistent failure (10+ misses) | COLD |

One threshold-policy function:

```cpp
struct SyncThresholdPolicy {
  float min_confidence;
  float weak_floor;
  bool narrow_window;
  size_t window_samples;  // candidate search span
};

SyncThresholdPolicy syncThresholds(
  SyncMode mode,
  ModulationClass mod_class,   // {COHERENT, DIFFERENTIAL, NARROWBAND}
  size_t expected_window_samples
);
```

The function applies window-narrowing reduction uniformly across modulations (today's coherent exclusion is removed). The `WARM_RELAXED` mode is the only place threshold-relax-by-streak lives.

State variables collapse from 12 to ~6:

```
SyncMode mode;
float warm_confidence;     // 0..1, refreshed on success, decayed on miss
size_t next_expected_sample;
size_t correlation_pos;
size_t search_floor;
float cached_cfo;
```

The 9 `expect_full_ofdm_anchor_` flip locations become 5 explicit `setSyncMode(...)` calls with names that say what they mean.

---

## 5. Recommended refactor approach

This is a multi-session workstream. Suggested order:

**Phase 1: Centralize threshold policy.** Move all the constants into one struct, parameterize by `(mode, mod_class, expected_window_samples)`. Apply window-narrowing reduction uniformly. Default-off behind a build flag; verify against current behaviour with `tests/test_signal_policy.cpp`. **Lowest risk; biggest legibility win.**

**Phase 2: Collapse the miss-counter state machines.** One `consecutive_sync_misses_` field; delete `sync_reject_streak_`. Verify the relax progression (5/8 thresholds) translates 1:1 from streak-based to miss-based. The `evaluateLightSyncCandidate` function shrinks to ~10 lines.

> ⚠ **Unsafe as written — see §8.5.** The two counters run on different clocks (per-candidate-reject
> vs per-frame-miss) and `sync_reject_streak_` drives a third knob (the RMS gate @sync_acquisition:469)
> with no warm-phase equivalent. There is no 1:1 translation. They are not redundant (§6 Q1).

**Phase 3: Explicit SyncMode enum.** Replace `expect_full_ofdm_anchor_ + warm_sync_active_ + warm_sync_phase_` with `SyncMode`. Map each of the 9 flip locations to a named state transition. The §16 warm-handoff knob becomes a transition rule, not an override.

**Phase 4: Unify the noteFrameArrival* calls into a single `recordFrameOutcome(success_or_miss, position, quality)` API**. Hides the state-machine updates from callers. The burst-group case (§16.11 finding 1) becomes a single `recordFrameOutcome(success, group_start_sample, group_end_sample, quality)` call.

**Phase 5: Sync-layer audit of waveform code.** Inside `OFDMChirpWaveform::detectDataSync()` etc., verify the LTS detector returns the *correct peak position* under the new policy. The §16.13 v3 stall at 8/11 groups was likely caused by peak interpolation landing 30-50 samples off — needs investigation at the detector layer.

**Phase 6: Multi-seed verification.** All four SyncMode transitions exercised on Good@20, Moderate@20, AWGN@10/15/20. Honest pass/fail on multi-seed runs.

**Phase 7: §16 / §17 re-attempt.** With the unified state machine, the §16.4 tiered fallback is ~30 lines instead of a multi-session DSP epic.

---

## 6. Questions for independent review

(For Codex / second reviewer.)

1. **Is the "4 architectural intents" reverse-engineering accurate?** Specifically: is `sync_reject_streak_` doing something the warm-sync state machine can't, or are they truly redundant?

2. **Window-narrowing threshold reduction (Observation 1.6.b):** is the 41× window reduction → 3.4 dB threshold drop physically justified for coherent QPSK? The current code applies this only to differential. The §16 v1 failure suggests it should apply to all light-LTS modes.

3. **CFO survival across reset (Observation 1.2.c):** is `last_cfo_` actually safe to carry across `resetFrameArrivalTrackingLocked` without quality gating? The 4 writers (§1.3) don't all validate. Could a bad pilot-tracking update poison the next chirp-handoff?

4. **Half-duplex turnaround timing:** the `expected_frame_gap_samples_` is set externally and only one writer exists (`setExpectedFrameGapSamples`). For real-radio half-duplex, the gap *varies* (T/R relay + protocol latency). Should the cadence prediction adapt per-frame, or is the variance small enough to ignore?

5. **Coherence-time bound for "WARM":** the §16.3 design says warm CFO is valid for ~5-10 s on Good (0.1 Hz Doppler) and ~1-2 s on Moderate (0.5 Hz Doppler). The current code has no TTL on `last_cfo_`. Should the state machine have a `WARM → COLD` timeout independent of misses?

6. **Refactor risk:** sections of `streaming_sync_acquisition.cpp` (~1010 lines) and `streaming_ofdm_decode.cpp` (~3054 lines) interact through shared state on the audio thread. Threading is fragile (§14.36 SIGSEGV history). Is the Phase 1-7 sequence safe to take one phase per commit, or does the threading require a bigger atomic refactor?

7. **What did 18 Codex rounds miss?** Of the band-aids listed in §3, which are likely covering a real underlying bug that would re-surface after the refactor? Particularly: control-first peek (does the false-lock-storm problem still exist absent that defensive demod?).

---

## 7. Status

- Audit doc: **this file**, `docs/SYNC_SUBSYSTEM_AUDIT_2026_05_29.md`.
- Production path unchanged. §15 tone-burst ACK (1.60 kbps Good@20) is the current shipping lever.
- Safe revert tag: `safe-revert-pre-s16-2026-05-28` (commit `3ac74f1`).
- §16.8 step 2 v1/v2/v3 work env-gated default-OFF, committed as `9861b26` for record.
- Independent re-review done in-house (Opus 4.8) instead of Codex — see §8. Inventory corrected;
  architecture thesis stands.
- Next: Phase 1 refactor proposal, built on the §8-corrected inventory (re-derive §1.3/§1.4/§1.7
  line numbers from the file first), with the §8.4 detection-theory law replacing the `√r` heuristic.

---

## 8. Re-review and corrections (2026-05-29, Opus 4.8)

Independent re-verification of §1–§5 against the code at HEAD. Confirmed the audit
commit (`e81cc11`) was docs-only and `git diff e81cc11 HEAD -- src` is empty, so the
code is byte-identical to what the original audit read — every discrepancy below is an
audit inaccuracy, not version drift.

**Verdict:** the architecture thesis (§2 four intents, the implicit `expect_full_ofdm_anchor_`
state machine, the two parallel re-acquire mechanisms, the SyncMode-enum / unified-threshold
proposal) is sound and should drive the refactor. But the quantitative inventory in §1.3, §1.4,
§1.6, §1.7 is unreliable and **must be re-read, not trusted, before executing §5.** Acting on the
numbers as written would silently drop live behavior.

### 8.1 Confirmed correct

- §1.6 threshold table: line numbers exact against `streaming_signal_policy.hpp`
  (0.90/0.85 @128-129, 0.52/0.45 @134-135, relax/rescue 5/8 @97-100) and
  `streaming_frame_arrival_policy.hpp` (0.25/0.05 @16-17). The actual filenames are
  `streaming_signal_policy.hpp` / `streaming_frame_arrival_policy.hpp` (the table abbreviates).
- **Obs 1.6.a (coherent `weak_floor=0.85` is dead): confirmed strongly.** `weak_floor` has
  *zero* read sites outside the policy header, and `evaluateLightSyncCandidate`'s weak-accept
  branch is gated `!is_coherent` (signal_policy:196). For coherent, corr=0.85 is rejected
  identically to corr=0.20.
- **Obs 1.6.b core (coherent excluded from window-narrowing): confirmed** — signal_policy:138
  gates the discount on `!is_coherent`. (The *numbers* are wrong — see §8.2.)
- §16 band-aid: `kS16WarmHandoffMinCorrelation = 0.55f` @sync_acquisition:660 is exact, and is
  indeed a hand-coded patch for the coherent-exclusion gap.
- Obs 1.3.a pilot-CFO writer **count** (5 sites) is correct: 4 in `streaming_ofdm_decode.cpp`
  + 1 in `streaming_burst_interleave.cpp`.

### 8.2 Verified errors (do not act on the original numbers)

1. **Window-reduction math (Obs 1.6.b and §6 Q2) is wrong twice.** WARM
   `candidate_span = kDefaultTightWindowSamples·2 + kWarmSearchSlackSamples = 960·2 + 256 = 2176`
   (frame_arrival_policy:13,15,175), so the reduction is `LIGHT_SEARCH_SIZE/2176 = 9600/2176 ≈
   **4.41×** → 10·log10(4.41) ≈ **6.4 dB**`, not "9600/4416 ≈ 2.2×" and not "41×" (which is
   self-inconsistent with the "3.4 dB" beside it — 41× would be 16 dB). The code's law
   `deriveNarrowWindowMagnitudeThreshold` reduces algebraically to **`τ_narrow = τ_wide/√r`**, so
   applying it to coherent would drop 0.90 → 0.90/√4.41 ≈ **0.43** — *below* the 0.52 differential
   floor (see §8.4 for why that law is itself suspect).

2. **§1.4 flip-site count is 9; actual is 11.** `expect_full_ofdm_anchor_ =` assigns at
   decoder:655(false), 696(false), **700(true)**, 710(false), **861(`=preserve_full_anchor`)**,
   **1180(false, hard reset)**, ofdm_decode:866(false), 875(true), 954(true), 1819(false),
   1829(true). The audit double-lists 655 (it is a single `=false`; the connect-`true` is at 700)
   and **misses decoder:861 and decoder:1180.** Note 861 is the actual reconfigure-time warm-handoff
   *keeper* (`preserve_full_anchor`); the audit attributed the keeper role to ofdm_decode:866.

3. **§1.3 pilot-CFO writer line numbers are all wrong.** Actual `last_cfo_.store(...)` pilot sites:
   **1191, 1238, 1642, 1931** in `streaming_ofdm_decode.cpp` + **436** in
   `streaming_burst_interleave.cpp` (audit said 1108/1155/1559/1848 + 435 — off by ~80 lines each).
   The count of 5 holds; the line numbers do not.

4. **§1.7 success-arrival count is 3; actual is 4** — missing `ofdm_decode:1978`. And **Obs 1.7.a
   is only half-true:** line 1816 fires once for the deinterleaved group, but the burst-continuation
   loop (sync_acquisition... ofdm_decode:1849-1988) fires `noteFrameArrivalSuccess` **per block**
   at 1978. The per-frame state machine *is* driven during the continuation body.

### 8.3 Missed entirely

1. **A third `sync_reject_streak_` knob** at `streaming_sync_acquisition.cpp:469`: at streak≥8 it
   relaxes the **RMS activity gate** (the silence-skip pre-gate), not the correlation threshold —
   and unlike the signal_policy relax(5)/rescue(8), it is **not** gated `!is_coherent`, so it is
   live for coherent QPSK. So `sync_reject_streak_` drives *three* knobs (min_confidence,
   weak_floor, RMS gate), only the first two of which are differential-only.

2. **Path-5 already mitigates the coherent-0.90 problem.** The full-anchor fallback
   (sync_acquisition:739, `unknown_frame_uses_control_sync_threshold=false`) passes
   `is_coherent=false`, so coherent frames get the **0.52** differential threshold whenever
   `expect_full_ofdm_anchor_=true`. The 0.90 wall bites only on the *pure warm path* (path 2),
   not universally as Obs 1.6.b implies.

### 8.4 PHY point not raised in §1–§5

`deriveNarrowWindowMagnitudeThreshold` implements `τ_narrow = τ_wide/√r`. §3 calls window-narrowing
"correct in principle," but detection theory for a normalized correlation statistic says holding
per-window P_fa constant under an r-fold reduction in trials gives a **subtractive** shift in the
*squared* threshold, `τ_narrow² ≈ τ_wide² − ln(r)/(c·L)`, not a divisive `√r`. The `√r` law is an
unvalidated heuristic that likely **over-relaxes** (0.90→0.43 at r=4.4, into false-lock territory).
Phase 1 ("unify window-narrowing across modulations") would propagate this law to coherent. The
principled fix is to fit the threshold-vs-r law to the empirical ρ-under-noise distribution, then
unify — not to widen the existing `if` scope around a suspect formula.

### 8.5 Impact on the §5 refactor plan

- **Phase 2 ("delete `sync_reject_streak_`, translates 1:1 to `consecutive_sync_misses_`") is
  unsafe as written.** The two counters run on different clocks (per-candidate-reject inside the
  detector loop vs per-frame-miss in the arrival policy); `sync_reject_streak_` drives three knobs
  including the RMS gate (§8.3.1) with no warm-phase equivalent; and for coherent the two
  threshold-relax mechanisms are not both active. Answer to §6 Q1: they are **not** redundant.
- **Phase 3 must map 11 flip sites, not 9** — including the easily-missed conditional keeper
  (decoder:861) and hard reset (decoder:1180).
- **Phase 4's premise (burst group = one arrival event) is incomplete** — the continuation body
  fires per-block (ofdm_decode:1978).

### 8.6 Status

- Re-review verified by re-reading the code at HEAD; no code changes in this commit.
- The §5 phase ordering is still the right shape; the corrected inventory above is the input to
  Phase 1. Recommend re-deriving every §1.3/§1.4/§1.7 line number from the file before Phase 3.

---

## 9. Sync consolidation refactor — objective, empirical 5-head map, plan (2026-05-29)

This section supersedes the "is the refactor needed?" question with an empirical answer: a
single-session attempt to make warm-handoff (§16) clean by point-fixing produced **five distinct
failure heads**, each fix exposing the next — the v1/v2/v3 pattern, reproduced and now mapped.
Decision (user, 2026-05-29): **do the consolidation refactor**, keeping the warm-handoff objective.

### 9.1 Objective (do NOT lose this)

**Shorten per-group airtime.** A burst group is `[BURST_HEADER descriptor (control frame, carries a
full chirp+LTS)] + [group-start DATA] + [body DATA]`. The descriptor's chirp is on the wire **every
group** regardless. The win is to stop sending a *second* full chirp+LTS as the **group-start DATA**
preamble and use **light LTS** there, anchored on the descriptor's chirp. Saving ≈ one full
chirp+LTS (~1.4 s) per group on the ~9.4 s group cycle. Reliability-first: clean first attempts go
light; fades fall back to a full anchor (escalation). This is the right objective; the refactor must
deliver it without the multi-path thrash below.

### 9.2 The empirical 5-head map (seed 1 Good@20, warm-handoff ON, file-only)

Seed 1 is normally trivial (seed 2 is the rough one), so every degradation below is a warm-handoff
state/path bug, NOT fade.

1. **Cadence misprediction.** `next_expected` pinned at the BURST_HEADER end (gap term
   `expected_frame_gap_samples_` is **never set** — no caller of `setExpectedFrameGapSamples`), so
   the next group was predicted ~8.4 s early (`error=403288`). Confirmed in BRAVO log.
2. **Coherent 0.90 gate (Obs 1.6.b).** Real warm group-start DATA arrived at corr 0.54 and was
   rejected; the 0.55 warm override missed by 0.01. Lowering to 0.50 → 11/11 but **640 bps / 319 s**
   (vs ~1600 bps baseline): a slow mask, not a fix.
3. **Descriptor-chirp re-arm (PROVEN GOOD).** `expect_full_ofdm_anchor_` was set false after group 0
   and never re-armed, so BRAVO stopped looking for the per-group descriptor chirp. Re-arming it
   after each delivered group (`streaming_burst_interleave.cpp` end-of-group) made BRAVO catch the
   chirp anchor again (0.83–0.88) — **keep this in the refactor.**
4. **Legacy 100 ms short re-anchor (commit `66db2d8`).** The superseded §16.2 group-boundary
   strategy, enabled by fading-class negotiation *concurrently* with warm-handoff; its detector
   fired on noise (corr≈0.16) — 10 garbage fires/run. Disabled under warm-handoff at the live
   chokepoint `ModemEngine::syncAdaptiveShortDataPreamble`. **Mutually exclusive with warm-handoff —
   the refactor must own the group boundary with one strategy.**
5. **Chirp-detector group-boundary unreliability (THE DEEP ONE).** Even with re-arm and the short
   re-anchor gone, `detectSync` repeatedly logged `Full OFDM anchor not found` for a descriptor
   chirp that IS on the wire, falling to path-5 light at 0.61–0.79 (35 path-5 fires/run). This is
   the audit's Phase 5 — the chirp/LTS detector does not reliably return the descriptor-chirp peak
   at a connected group boundary. **This is the crux the refactor must solve; it is not a threshold
   or competing-path issue.** Investigate: is the connected-data control-profile `generatePreamble`
   chirp the same dual-chirp `detectSync` is tuned for? Is the 120 k search window aligned to the
   descriptor arrival after the inter-group gap? Is the peak interpolation off (§5 Phase 5 note)?

### 9.3 What's proven and reusable

- Re-arm descriptor chirp per group (head 3) — correct mechanism.
- Short re-anchor disabled under warm-handoff (head 4) — correct; the path is legacy.
- §16.4 escalation + resend-full-chirp (Fix A/B) — correct as the deep-fade safety net; maps to the
  `RE_ACQUIRE` SyncMode state.
- All committed env-gated behind `ULTRA_S16_WARM_HANDOFF` (default OFF) as the **v4 checkpoint**;
  production (§15 tone-burst, 1.60 kbps) is unaffected.

### 9.4 Refactor design (per §4) + tonight's targets

**Core move (user, 2026-05-29): REDUCE the sync entry points, don't reconcile them.** The §1.1
inventory has SIX entry paths; the 5 failure heads are mostly *competition between these paths*.
For the one-way burst transport (SR-ARQ ripped, short re-anchor legacy) the minimal set is **two
detectors**:

| Keep | Role |
|---|---|
| `detectSync` (chirp+LTS) | COLD handshake **and** the per-group BURST_HEADER descriptor anchor (one detector, two callers) |
| `detectDataSync` (light LTS) | contiguous data within a group only |

Cut / fold the rest:
- `detectShortDataSync` (path 3, short re-anchor): **delete** from the burst path — legacy `66db2d8`.
- Full-anchor fallback (path 5): **becomes the defined `WARM_RELAXED` transition**, not a 0.52
  catch-all that fires 35×/run.
- Control-first peek (path 6): **fold** into the descriptor-anchor decode (the §14.27 descriptor
  already declares the next group's params; the peek's defensive role shrinks).
- `narrow_waveform` (path 4): orthogonal (OFDM_NARROW only); leave untouched.

Then build the `SyncMode {COLD, WARM, WARM_RELAXED, RE_ACQUIRE}` machine and one
`syncThresholds(mode, mod_class, expected_window)` policy (§4) over that 2-detector base. Specific
to the objective:

- **One group-boundary strategy:** descriptor chirp = the per-group anchor (WARM→re-acquire the
  chirp each group), light LTS = contiguous data only. Delete/disable the short re-anchor and
  constrain path-5 to a defined role (not a catch-all).
- **Warm-regime coherent threshold:** 0.90 only for COLD; a noise-floor-aware threshold for
  WARM/contiguous data, derived (§8.4) rather than the √r over-relaxation or a magic 0.50.
- **Fix the chirp detector at the boundary (Phase 5, head 5):** this is now the top-priority
  unknown; the SyncMode work is wasted if `detectSync` can't reliably catch the descriptor chirp.
- Collapse the 11 `expect_full_ofdm_anchor_` flips + 3 miss counters into explicit transitions.

### 9.5 Phase order (revised from §5 by tonight's evidence)

1. **Phase 5 FIRST (was last): chirp-detector group-boundary reliability.** Head 5 is load-bearing;
   prove `detectSync` reliably returns the descriptor-chirp peak at a connected boundary before any
   state-machine work. Build an offline harness if needed.
2. Centralize threshold policy (warm-regime coherent threshold; remove the magic floors).
3. SyncMode enum replacing `expect_full_ofdm_anchor_` + `warm_sync_active_` + `warm_sync_phase_`;
   descriptor-chirp anchor as the WARM per-group transition; RE_ACQUIRE = escalation.
4. Remove legacy competing paths (short re-anchor) entirely on the burst path; SR-ARQ is already
   ripped — verify no dead enable paths remain.
5. Collapse miss counters; unify `noteFrameArrival*` (audit Phase 4; remember the 4th site @1978).
6. Multi-seed GUI verification: **seed 1 must go clean AND fast (> baseline 1600 bps), seed 2** as
   the hard gate; AWGN no-regress.

### 9.6 Status

- v4 checkpoint committed env-gated (default OFF); production safe.
- Next session starts at Phase 5 (chirp-detector reliability), the proven crux.
