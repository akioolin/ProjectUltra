# DESIGN BRIEF — GROUP-SIZE LEVER (amortize the fixed per-cycle turnaround)

Branch context: unrelated WIP branch is checked out; this brief targets `main`-era code (line refs verified today). Fixed constants used throughout: data slot = 59,360 samples = 1236.7 ms (sp8 grid, rung-independent; F163 sample-exact on 38 bursts); full descriptor+anchor = 67,680 samples = 1.41 s; turnaround = 1.96 s current healthy median (F187, post-F176) vs 3.4 s F163-era; fixed overhead per cycle = 3.37 s today.

---

## 1. CURRENT POLICY (exact)

Group size is **derived, not set**. `Connection::burstAirtimeBudgetFrames(max_frames)` (src/protocol/connection.cpp:5059-5111), called as `burstAirtimeBudgetFrames(arq_.getWindowSize())` from `prepareUnifiedBurstWindow()` (connection.cpp:5135-5145):

```
n = max { n <= W_arq :  1200 + n*data_ms + (n-1)*reanchor_ms  <=  kMaxBurstAirtimeMs }
```

- `kMaxBurstAirtimeMs = 8600`, env `ULTRA_MAX_BURST_AIRTIME_MS` hard-clamped **[5000, 12000]** (connection.cpp:5083-5092). The comment codifies the 2026-06-07 20-seed Good@16 sweep: groups 5 and 6 **tied** on goodput (~1400 bps); 5 chosen on PA-duty/fade/SACK grounds at 1.4 kbps-era throughput.
- `data_ms` via `wideOFDMBurstAirtimeMs` → `wideOFDMSymbolsForCodewords = 2 + ceil(cw·648·z/27 / bits_per_symbol)`, symbol = 24 ms (connection_policy.hpp:1088-1099, 1181-1203). At the duration-normalized Good rungs (QPSK cw8 / 8PSK cw12 / 16QAM cw16, all 51 data symbols on the sp8 grid) this models **1272 ms** vs 1236.7 ms actual.
- `reanchor_ms = 100` charged whenever `shouldUseWideOFDMShortReanchor` is true (coherent + fading; connection.cpp:5093-5097) — **a phantom**: the encoder's adaptive short-chirp re-anchor was removed ("R4: the adaptive short-chirp re-anchor was removed (superseded by warm-handoff, now default)", src/gui/modem/streaming_encoder.cpp:193), and F163's sample-exact airtime model S(N) = N×59,360 + 67,680 confirms nothing rides between frames.
- Result today: Good verdict → 1200 + 5×1272 + 4×100 = 7960 ≤ 8600; 6th = 9332 > 8600 → **N=5**. Moderate verdict (coherence walk shrinks to cw10/cw7, 816/768 ms) → **N=8**. ARQ window (16 for coherent ≥R2/3, `coherentOFDMWindowOverride` default-ON, connection_policy.hpp:992-1030) and the 16-bit tone-ACK SACK mask (`kToneBurstAckWindowCapFrames=16`, hpp:84; wire mask tone_burst_constants.hpp:224) both already permit 16 — **only the airtime ceiling pins Good-era groups at 5**.
- Timers already scale: `unifiedBurstAckTimeoutMs(cap)` is set before submit (connection.cpp:5139-5144) and every term is frames/mod/rate/cw/z-derived (connection_policy.hpp:1459-1510); the ACK listen window floors to it (connection.cpp:5147-5161). The tone ACK is 34 symbols regardless of N ≤ 16 — bigger groups do **not** lengthen the ACK.

Economics (measured constants): efficiency = N×1.237/(N×1.237 + 3.37) → N=5: 64.7%, N=8: 74.6% (**+15.3% raw**), N=12: 81.5%, N=16: 85.4%. Fade-taxed (p_loss ≈ 0.02·N): +11.6% at N=8, peak +13.8% at N*≈10.6, flat-to-negative ≥12; N=8 captures ~84% of the available gain and is the largest N that never loses across the observed 5-20% loss range (+1.7% even at 20%). Only 16QAM R2/3 at N≥8 crosses 3.0 kbps expected.

## 2. THE CHANGE — one derivation fix + one ceiling move, streak-gated

**Target: N=8 real frames (11.10 s key-down, 77% duty) at the duration-normalized rungs.** N=10-12 is rejected for now: flat gain over N=8 (+2.2%), negative in rough epochs (−12.7% at 20% loss), inexpressible without raising the [5000,12000] clamp (a PA-duty decision), and it breaks the 600k backstop and the 25 s CCA relearn margin.

Minimal edit set — no per-rung special cases, no wire change, no clamp change:

1. **Remove the phantom re-anchor charge** (the derivation fix). The encoder no longer emits the continuation re-anchor (streaming_encoder.cpp:193), so retire the `reanchor_ms` charge at its policy source (`wideOFDMShortReanchorChirpDurationMs` / `shouldUseWideOFDMShortReanchor`, connection_policy.hpp:1101-1131) and all four call sites: connection.cpp:2917-2924, 4482-4484, 4911-4951, 5093-5097 (this also removes a small over-budget in the ACK RTO at hpp:1501). This makes the budget model match the wire (F163 S(N) = N×59,360 + 67,680). Update `docs/MODEM_INFRASTRUCTURE_MAP.md` + `docs/REMOVAL_BACKLOG.md` in the same change (mandatory repo rule). Note this is a **prerequisite**: with the phantom kept, N=8 models at 1200 + 8×1272 + 7×100 = 12,076 ms > the 12,000 clamp — unreachable even by env knob.
2. **Raise the ceiling default 8600 → 11,500 ms** (connection.cpp:5084). With the phantom removed, any C in [11,376, 12,647] yields exactly N=8 at the modeled 1272 ms frame (1200 + 8×1272 = 11,376; 9th = 12,648); 11,500 sits inside the existing clamp — the clamp and its PA rationale stay untouched. The 35 ms/frame model conservatism (2-symbol preamble booking vs the 608-sample light preamble) is harmless — the true-slot check gives the same n=8 at 11,500 — leave `data_ms` alone.
3. **Make the escalation adaptive (recommended form): streak-gated ceiling, not static.** Base ceiling stays 8600; escalate to 11,500 after **2 consecutive clean groups at the current rung** (clean = ACK with no holes); any crater/hole-heavy group resets to base. Rationale: (a) rough epochs (Moderate-class loss ~20%@5) have their knee at N≈6.7 — a static raise loses there; (b) the fade tax is per-second-of-air, so the *ceiling* is the correct single control variable across rungs (Moderate cw10 at 11,500 would derive n=12 ≈ 10.99 s air — same exposure as Good N=8 — but only in a proven-calm streak); (c) it reuses the exact evidence doctrine of the #69 reactive-skip streak (streaming_encoder.cpp:679-680) and the predictive climb; (d) cost is ~2 cycles per transfer of foregone amortization — negligible. This is one channel-adaptive derivation, ADAPTIVITY-compliant: the frame count remains derived; the only new state is a clean-group streak counter feeding which of two documented ceilings applies. If reviewers prefer zero new state, static 11,500 is defensible (N=8 never loses per the model) — but streak-gating is the recommended default given F147's "16QAM cratered 0/8" history at 8-frame Moderate groups.

## 3. MANDATORY CO-FIXES (land in the same pass)

1. **Anchored-burst backstop must derive from geometry** — `kBackstopWindowSamples = 600000` fixed (src/gui/modem/streaming_sync_acquisition.cpp:329-330; sizing comment :320-322 assumes "descriptor + anchor + an 8-frame group ≈ 12.5 s"; arms at anchor detect :478-482, disarms only on BURST_HEADER consume, streaming_ofdm_decode.cpp:902). At N=8 the margin is only ~2.4 s; at N≥10 it fires **mid-burst** (10,080 + 10×59,360 = 603,680 > 600,000) and requests an ACK while the peer is keyed — the F176 geometric guard is blind in exactly this case because `burst_air_end_abs_` is only armed when a descriptor frames the group (streaming_ofdm_decode.cpp:1455-1459). Fix: derive the window from the airtime ceiling, e.g. `48 × (escalated_ceiling_ms + margin_ms)` or descriptor + mask-cap frames + slack — a constant here is the same adaptivity bug class as `dd_qam16_*`.
2. **ACK-timeout scaling — verified, no fix needed** (`unifiedBurstAckTimeoutMs`, connection_policy.hpp:1459-1510, mirrors the receiver's airtime-derived group timeout at streaming_burst_interleave.cpp:187-197; set per-burst at connection.cpp:5139-5144; listen window floors to it :5147-5161). Record in the change notes so nobody "fixes" it.
3. **CCA relearn invariant** — `noise_floor_relearn_after_ms = 25000`, sized "> 2× the burst airtime cap" (src/audio/channel_busy_detector.hpp:71-78, F129). 2×11,500 = 23,000 ≤ 25,000 — holds, barely. Add the derivation/assert tying it to the ceiling so a future ceiling raise can't silently re-open the F129 mid-burst floor-relearn crater. Note the residual: an F163-P6-style doomed back-to-back double keydown at N=8 ≈ 24 s — monitor, and treat the double-keydown descriptor-miss pathology as its own bug.
4. **Tighten `ULTRA_BURST_GROUP_FRAMES` clamp [2,32] → [2,16]** (connection_policy.hpp:100-104) — values 17-32 are accepted today and silently create un-ACKable trailing frames past the 16-bit mask (comment at :92 already says ≤16).
5. **Buffers — no resize needed at N=8**: real RX ring = 2,400,000 samples / 50 s (src/sync/sync_ring_buffer.hpp:24); `MAX_PENDING_SAMPLES=960000` (modem_engine.hpp:505) is a **dead constant** — correct the stale CLAUDE.md "20 s buffer limit" line when touched. HARQ `SoftCombineBuffer max_entries_=32` (src/fec/soft_combine.hpp:126) holds 8×2 airings comfortably; derive it as `window×2` only if/when groups go past 8.

## 4. TURNAROUND SHAVES worth taking in the same pass

Only the cheap, high-confidence ones (sum ≈ 0.25 s/cycle, an order below the group lever — do not let them gate it):

- **Staircase warm-up fix**: prime the cached OFDM_BROADBAND SNR at connect so the first ~3 ACKs run 408 ms instead of 850 ms (observed t=47-69 F187) — saves ~1.3 s once per transfer. Build path: the lock-free SNR cache read in src/gui/app.cpp:660-706.
- **ACK lead-in 150 → 50 ms** via existing knob `ULTRA_TX_ACK_LEADIN_MS` (modem_engine.cpp:712-716): −0.10 s/cycle. Rig A/B as env first (real-PA ramp unproven), codify only if ACK detect rate is unchanged.
- **Skip for now**: data lead-in trim (AGC/ALC fidelity caveat), sender armed cadence 100→50 ms (+0.5%, touch only if free), the 13.33 ms ACK rung (design + FER work), and the ~1.0 s unattributed device/feed residual (needs a Pi5-side timestamped run first — separate workstream, worth +5-11% later).

## 5. VALIDATION

1. `cmake --build build -j4 && ctest --test-dir build --output-on-failure -j4` (incl. `tests/test_connection_policy.cpp` boundary updates for the new ceiling/derivations).
2. **Gate A/B (faithful gate, paired same-seed)**: `tools/gui_qso_scenario.sh --channel good --snr-db 20 --seed {42,43,7} --file-kb 21` baseline vs change, plus one `--channel moderate` cell (checks the streak gate holds groups near base under craters). Compare per pair: `GOODPUT_BPS`, `FILE_CRC_OK_COUNT`, crater count (0/N groups), rate-switch count, retx count, and the logged group size ("Flushing burst of N frames", connection.cpp:5871). Expect ~+15% raw / ~+12% fade-taxed on Good; expect NO increase in craters-per-group. Gate noise is ±25% — paired seeds and crater counts are the reliability signal, not single-run goodput.
3. **Rig A/B vs the F188-F197 baseline ledger** (IONOS MPG@20, same file): pass-mean vs 2.04 kbps / record 2.70; log group sizes, turnaround median (expect ~1.96 s unchanged), descriptor-miss episodes (vs F163's 3/transfer), RTO stalls, and any CCA-reads-idle-during-burst (F129 signature) during 11.3 s keydowns.

## 6. RISKS (ranked) + mitigations

1. **Descriptor-loss quantum grows 5→8 frames and RTO dead-air grows with N** (F163: missed descriptors = ~129 s of ~179 s excess; the dominant measured sink). Mitigation: full-anchor descriptors already ride every group (2026-07-06 fix); track descriptor-miss count as a hard abort criterion in both A/Bs; the streak gate resets to N≈5-6 after any crater.
2. **Rough-epoch over-widening** (Moderate cw10 derives n=12 at 11,500 once the phantom is gone; knee there is ~6.7). Mitigation: streak-gated escalation (craters keep the base 8600 → n≈9 at cw10); monitor the Moderate gate cell.
3. **Backstop fires mid-burst / thin margin**. Mitigation: co-fix #1 (geometry-derived window) lands in the same commit — non-negotiable.
4. **PA duty 69% → 77%, 11.3 s keydown**. Mitigation: stays within the deliberate [5000,12000] clamp and its documented PA rationale; 11.3 s keydowns already flew implicitly on the rig in the cw7/cw10 era; cooling gap (~2 s turnaround + streak resets) preserved; state the duty ceiling in the commit message per the hard-constraint rule.
5. **CCA relearn margin erosion** (23 s vs 25 s; double-burst ≈ 24 s). Mitigation: co-fix #3 derivation/assert; file the doomed back-to-back keydown as its own bug.
6. **Erasure-gate reference staleness over an 11.3 s group** (single group-start RMS snapshot, streaming_burst_interleave.cpp:608-633, vs Good fade cycles). Mitigation: monitor over-erasure counts in the gate A/B; move to a sliding reference only if measured — not preemptively.
7. **Run-behind defeats warm-sync, aggravated by longer keydowns** (2026-06-20 finding). Mitigation: watch warm-vs-cold acquisition counts in the rig A/B.
8. **Group-timeout discards partial progress** (empty delivery, mask=0, streaming_burst_interleave.cpp:236-259) — tail-truncation probability grows with N. Mitigation: accept for N=8 (event is rare post-F176), log occurrences; fix is phase-2 alongside any N>8 work.

**Explicitly out of scope / rejected**: raising the [5000,12000] clamp (PA decision, needed only for N≥10); widening the 16-bit SACK mask (wire-breaking, needed only for N>16); `kBurstInterleaveGroupFrames=6` (interleave SPAN, orthogonal — do not touch); `kOFDMBurstAckBatchFrames=4` is dead (optional deletion via REMOVAL_BACKLOG).