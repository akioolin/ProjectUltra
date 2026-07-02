# Known Bugs

Last updated: 2026-07-01

## Purpose
Track only currently relevant issues that can affect reliability, throughput, or release quality.
Fixed/obsolete historical deep dives belong in `docs/CHANGELOG.md`.

## Active Issues

### BUG-BURST-HEADNULL-DROP: group-head fade null → clean mid-group frames silently dropped (no decode attempt, no counter, no ACK credit) → whole-group RTO saga
- Status: **OPEN — root-caused in code + reproduced in logs (2026-07-01 audit, adversarially verified).** Found via `fable_analysis/09_WHY_STUCK_AT_2000_2026_07_01.md` §3.2.
- **What:** burst accumulation is marker-gated — only the group-start frame (negated-LTS marker) can enter accumulation (`streaming_ofdm_decode.cpp:~1090` marker check, `:1271` entry). When a fade null covers the group head (LTS + frame 1), frames 2..N arrive clean (observed corr 0.95-0.96, LTS SNR 26.7-27.0 dB), are sync-accepted (`accepted connected DATA sync fallback`), fail the 1-CW control-first peek, and are consumed by the **log-less** mid-burst re-search (`streaming_ofdm_decode.cpp:1042-1052`): no 8-CW decode attempt, no counter, no partial-ACK credit. Each occurrence costs a full sender RTO (~24.5 s) + a whole-group resend (~8 s). Observed: Good@20 seed-42 stock run lost 83 s on one group (3 resend attempts; ~20 s of clean 27 dB frames received and discarded) = ~23% of the transfer.
- **Why the silent path exists (do NOT naively fall through):** the legacy control→data profile fall-through double-demod **poisoned the burst's shared coherent channel estimate** (§14.24/§14.25, comment at `streaming_ofdm_decode.cpp:1021-1029`); the gate was also narrowed for BUG-TNC-B2F-001. A fix must re-demodulate from the ring at the data profile (fresh estimate), not fall through.
- **Fix path:** (1) FREE: add a counter/log for "sync-accepted data-profile frame consumed without decode attempt" (today it is invisible). (2) The descriptor already gives the receiver the group geometry — enter accumulation from ANY group-member sync with the head erasure-marked, converting the saga into a 1-frame nack. (3) Interim mitigation is the RX group-timeout fast-NACK, which requires the BURST_HEADER *and* ≥1 decoded data frame — the head-null case defeats it.

### BUG-ACK-STAIRCASE-FADE-BIN: SNR-adaptive fast tone-ACK never engages on fading channels — bin edges are AWGN-calibrated but fed fade-effective SNR
- Status: **OPEN — measured occupancy 2026-07-01 (sim + rig).** The §15.5 staircase (`tone_burst_constants.hpp:99-105`) picks 12 ms symbols (324 ms ACK) at ≥18 dB, but the receiver feeds it the cached fade-effective in-band SNR (`app.cpp:645-651`), which reads ~16-17 dB at Good@20 (fading/Jensen penalty). Measured: sim Good@20 16QAM 29/29 ACKs at 675 ms, stock 26/26 at 675 ms (only forced QPSK R3/4 reached 324 ms, 20/25); **rig MPG@20: 30/30 at 675 ms**. The hardware-proven fast rung is dormant at the operating point it was built for: ~350 ms/cycle ≈ 4-5% goodput, taxing the short-burst dense rung (16QAM) hardest.
- **Fix:** express the staircase bin edges in fade-effective terms (~16 dB edge for the 12 ms rung) or feed the basis it was calibrated on; validate ACK FER at the new edge on Watterson before shipping (each lost ACK costs a full RTO — same trap class as the 12 ms design review of 2026-06-12). Same disease family as #74/#58: fade-aware measurements fed into AWGN-calibrated thresholds.

### BUG-CONNECT-SNR-VARIANCE (#58 completion): the connect-time SNR is a single 170 ms snapshot on a ~4 s-fade channel → ~10 dB pick-to-pick spread; the +2 dB basis correction fixes the BIAS, not the VARIANCE
- Status: **FIX IMPLEMENTED (increment 2, 2026-07-02, default-ON) — pending rig
  connect-spread re-measure.** The designed data-aided estimator LANDED
  (`updateDataAidedSNREstimate`, `multi_carrier_dpsk.hpp`; routed in
  `populateDecodeMetrics` via `ULTRA_CONNECT_DATA_AIDED_SNR`, default-ON, `=0` opts out;
  decode-then-measure — routed only when the frame's LDPC decode succeeded, else the
  training snapshot). Differential-level (unit-phasor chord error vs the config-driven
  DBPSK/DQPSK constellation, ~0.2 s block averaging ≪ Tc), so it needs NO static channel
  reconstruction and is drift-immune across the multi-second frame. Calibration derived,
  not tuned: magnitude normalization cancels the differential +3.01 dB; geometry-computed
  non-orthogonal-carrier ICI (−29 dB/carrier @8×1024, matches measured excess) subtracted;
  (k−2)/k inverse-chi-square block correction; +0.5 dB measured residual. GATED in
  test_mcdpsk_snr_calibration: AWGN within 1 dB at 0/5/10/15/20/25 dB (measured
  +0.5/−0.1/−0.1/−0.0/+0.4/+0.1). Both values logged per frame
  (`MC-DPSK SNR: training=X data_aided=Y (routed=...)`) for the rig spread comparison.
- Rig MPG@20 evidence (one evening, 6 connects): snapshots
  18.2 / 17.0 / 14.6 / 14.5 / 12.4 / **8.4** while the chirp-quality proxy read 22-27
  throughout. The 8.4 connect survived to OFDM only by 0.4 dB (sel=10.4 vs Good floor 10) and
  picked **QPSK R1/2** — half throughput on a channel that carries R2/3. Deeper dips than the
  +2 average penalty WILL occasionally cross into the MC-DPSK branch (the residual coin-flip).
- **Root cause was architectural, not the estimator** (AWGN-accurate to 30 dB,
  test_mcdpsk_snr_calibration): `updateTrainingSNREstimate` measures ONLY the ~170 ms training
  preamble = one fade state (Tc ≈ 4.2 s at Good); the CONNECT frame's 4 CWs span ~7.1 s ≈
  1.7 Tc and are fully decoded before the pick → the whole-frame estimate is fade-AVERAGED
  by construction at zero handshake latency.
- **Remaining (do NOT close yet):** (1) rig connect-spread re-measure (expect the ~10 dB
  spread to collapse to ~±1-2 dB); (2) re-evaluate shrinking the +2 `connectSelectionSnrDb`
  basis correction (the Jensen penalty of a fade-AVERAGED effective SNR is smaller) —
  deliberately untouched in increment 2.

### BUG-QAM16-RIG-LEVEL-BUDGET (investigation 3b RESOLVED 2026-07-02): rig 16QAM is FADE-TROUGH-LIMITED at the current level calibration — not a code defect; the fix is a level/operator lever
- **FIX IN VALIDATION (2026-07-02, software-ALC — CHANGELOG entry of same date):** the "level
  lever" is now CLOSED-LOOP instead of operator-manual. Receiver measures per-burst data-RMS
  over chain noise + crest factor (`[ALC-RX]` / `LEVEL ADVISORY:` log lines, thresholds
  `ULTRA_ALC_LOW_DB`=12 / `ULTRA_ALC_CLIP_CF_DB`=6.5); a 2-bit drive advisory rides the
  tone-burst ACK (bits [30..31], **WIRE-BREAKING — lockstep builds only**); sender walks
  tx_drive +0.5 dB/−2 dB within [baseline, 0.85] on connected OFDM data bursts only
  (`ULTRA_SOFTWARE_ALC=0` disables the loop). Sim-proven no-op at reference levels (gate PASS,
  zero `ALC:` moves). **PENDING: rig A/B** — expect the loop to walk 0.5→~0.85 (+4.6 dB) at
  MPG@20, lifting arriving data SNR from ~6-7 dB toward the ~11-12 dB the 16QAM rungs need;
  then re-run the 16QAM ladder. Grep `ALC: tx_drive` (sender) + `\[ALC-RX\]` (receiver).
- Status: **DIAGNOSED via wire capture + rung falsification (overnight 07-02).** The Mac-input
  ffmpeg captures (paired QPSK vs 16QAM forced runs, MPG@20) show IDENTICAL level structure for
  both mods: data segments ~0.077-0.079 RMS, anchors ~0.16-0.18, noise floor ~0.037 → the data
  arrives at only **~6-7 dB broadband wire SNR**, while anchors ride 6-7 dB hotter (per-burst
  PEAK normalization: OFDM crest ~14.3 dB eats average power). No 16QAM-specific TX defect.
- **Rung falsification:** 16QAM R1/2 (4-5 dB more margin than R2/3, sim-clean 5/5 at Good@18)
  ALSO fails to complete on the rig — decode is **bimodal** (13 groups 8/8 flawless, 13 groups
  0/8 dead at median 22.7 dB effective): the fade TROUGHS at this wire level kill any dense
  constellation whole-group; the crests pass 16QAM perfectly. QPSK's phase-only margins bridge
  the troughs. 16QAM transfers PROGRESS (~26 KB of clean groups in 480 s) but can't finish at
  ~50% group loss.
- **Dead end tested + reverted same night:** coherent-OFDM PAPR soft-clip (recover average power
  under peak normalization). Sim A/B: EVM cost >> benefit for 16QAM at every depth (9 dB target:
  181 vs 30 deint-fails, hard FAIL; 12 dB: 78 fails, 2210->1320). `ULTRA_COHERENT_PAPR_DB`
  ships default-0 (off) as an experiment knob only.
- **The fix is a LEVEL lever, not code:** ~+4-5 dB of arriving data SNR moves the troughs above
  16QAM R1/2's floor (tx_drive 0.5->~0.8 and/or IONOS CH-gain re-staging — needs the operator at
  the IONOS CF/level panel per the 2026-06-15 calibration method; peaks must stay under the
  1800 mVpp input clip). Alternates if level can't move: cw16 (raises per-bit efficiency, same
  trough problem), fade-phase-aware scheduling (research). The prior ANCHOR-COLLAPSE observation
  (07-01 afternoon: corr 0.95->0.2) did NOT reproduce in any of 6 subsequent 16QAM runs — kept
  below as historical until seen again.

### BUG-QAM16-RIG-ANCHOR-COLLAPSE: 16QAM bursts stop being ACQUIRED on the real rig (sync corr 0.95 → 0.2) while their PHY decodes clean — root cause NOT isolated
- Status: **OPEN — observed 2026-07-01 on IONOS MPG@20 (Mac↔Pi5, HEAD a81725d), forced 16QAM R2/3.** The transfer decoded 9+ groups cleanly (**77/77 deinterleave SUCCESS, 0 FAILED — zero fade-damage tax**), then fell into a persistent no-delivery saga: 117 nack + 74 timeout retx, no completion in 480 s. Sender escalates full-anchor resends; receiver pinned in `Full-anchor wait rejected DATA fallback (corr=0.34 < 0.50)`.
- **The discriminating signature:** same channel, minutes apart — QPSK R3/4 run sync-corr modes 0.92-0.99 (delivered 1.62 kbps clean); 16QAM run corr modes **0.16-0.29**. The constellation doesn't fail; the *acquisition* of its bursts does. Burst-erasure gate hits: 0.
- **Candidate mechanisms (untested):** (a) TX-side PAPR — 16QAM+cross-frame-interleave raises burst crest factor → per-burst hardware normalization lowers average power → the in-burst anchor chirp sinks toward the RX noise floor (the 2026-06-16 IONOS "high-PAPR data below the anchor" mechanism; `ULTRA_SIM_PAPR_PENALTY` exists because OTASim cannot exhibit this); (b) RX-side — decode/search backlog during the saga (#56 class) + the full-anchor-wait rejection threshold locking the pair into mutual starvation.
- **Discriminating experiment:** paired QPSK-vs-16QAM rig runs logging the Pi5 per-burst normalization factor (AUDIO category) and the Mac chirp-segment RMS/corr per burst; if 16QAM's normalization factor is materially lower, it's (a) — mitigations: anchor power boost within the normalization budget, PAPR reduction (clip/tone-reserve) on the data symbols, or descriptor-profile robustness. If not, instrument the full-anchor-wait state residency.
- **Impact:** blocks the entire 16QAM-on-hardware path (the only route to 3000 bps) regardless of the sim-side damage work. Sim CANNOT reproduce (fidelity gap — document per SIMULATOR FIDELITY rules). See `fable_analysis/09_WHY_STUCK_AT_2000_2026_07_01.md` §4.

### BUG-ACK-TIMEOUT-DOUBLECOUNT: unified burst ACK deadline counts burst airtime ~twice (+ phantom reanchor term) → every timeout saga pays ~8-10 s extra dead air
- Status: **OPEN (promoted from CHANGELOG 2026-06-19 follow-up; quantified 2026-07-01).** `unifiedBurstAckTimeoutMs` (`connection_policy.hpp:811-847`) adds `burst_ms` AND `physical_sack_hold_ms = max(configured, burstAirtime+30)` where the configured term on Good coherent is the full window-8 hold (12.1 s, `connection.cpp:3122-3127`), plus a phantom +100 ms/frame short-reanchor the encoder no longer emits (`connection_policy.hpp:558-568` vs `streaming_encoder.cpp:155`). QPSK R3/4 g5 deadline ≈ 24.5 s vs true clean RTT ≈ 9-9.5 s.
- **Floor (do not cut below):** the deadline back-stops the receiver's wall-clock group-timeout fast-NACK (8000×n/4 ms, `streaming_decoder.hpp:814` — itself an adaptivity violation: wall-clock, not mod/rate/z-derived) and rig worst-case turnaround (measured ~5.8 s post-burst latency; #56 RXQ backlog). Documented floor ~14-17 s pending rig calibration; the 2026-06-19 premature-resend incident is the regression to avoid. **Move both timers together.**

### BUG-MCDPSK-ACK-COLLISION: tone-burst partial-SACK fires < one MC-DPSK frame airtime → half-duplex collision livelock — **FIX IMPLEMENTED (2026-06-30), pending lossy-rig validation**
- Status: **ROOT-CAUSED + REPRODUCED on the live IONOS rig + FIX IMPLEMENTED + ctest-clean (OFDM byte-identical). Pending: lossy-channel rig A/B (the faithful gate runs clean → no holes → can't exercise it; also confounded by BUG-MCDPSK-FILE-COMPLETION which blocks *completion* regardless).** Surfaced once #74 let MC-DPSK connect + transfer at low SNR (rig MPG@8, DQPSK R1/4, window=5).
- **Fix (implemented):** the real mechanism is the tone-burst PARTIAL (hole-bearing) SACK sliding timer (`selective_repeat_arq.cpp` ~622), hardcoded to `kToneBurstPartialSackDelayMs = 1500 ms` — correct for OFDM (short frames) but < one MC-DPSK frame airtime (3691 ms), so it fired while the sender was still transmitting a trailing failed frame. Made it configurable (`setToneBurstPartialSackDelayMs`, default 1500 → OFDM byte-identical) and the Connection scales it to `max(1500, timing.data_ms + 1000)` ≈ 4.7 s for MC-DPSK so the SACK lands in the inter-burst gap and the sender does a FAST retransmit instead of a timeout whole-window resend. Carrier-sense (defer until the channel is heard idle) is the fully radio-correct generalization + would also cover the rare multi-trailing-hole case; this airtime-scaled guard is the targeted fix.
- **What:** on a hole (a frame in the window fails to decode), the transfer livelocks: the SENDER retransmits the whole window on its 31.6 s RTO (`cause=timeout`, NEVER on a NACK), the RECEIVER keeps re-sending the same SACK (`group_seq=35 frame_mask=0x02`), they collide forever → 10/10 retries → DISCONNECT. The clean path (no holes) works fine — groups advance every ~21 s. Only the REPAIR path collides.
- **Trace (rig, MPG@8):** Pi5 (sender) TX bursts at 408.3 / 439.8 / 471.4 s (each 896256 samples ≈ **18.7 s**, period ~31.5 s); Mac (receiver) ACKs at 420.6 / 452.0 / 483.6 s (period ~31.5 s). Every ACK lands ~12 s INSIDE a sender burst → sender mid-TX (deaf) → never hears the NACK → times out → resends → next ACK lands in that burst too. Both ends on the same ~31.5 s period, phase-offset to collide.
- **Root cause:** `selective_repeat_arq.cpp:1747` — `guard_half_duplex_repeat = (bitmap == 0) && !sack_has_final`. The half-duplex peer-burst guard is DELIBERATELY disabled for hole-bearing SACKs (`bitmap != 0`) so repair feedback is "prompt." Correct for OFDM (short bursts → sender stops & listens fast), WRONG for MC-DPSK: the window burst is ~18.7 s so the "prompt" NACK fires straight into the sender's transmission. The RTO was made rate-agnostic (`computeMCDPSKAckTimeoutMs` scales with the 3691 ms frame) but the ACK-repeat guard was NOT — `setAckRepeatPeerBurstGuardMs(arq_.getSackDelay())` = **30 ms**, vs an 18.7 s burst.
- **Fix (next session):** peer-burst-guard the hole-SACK repeat ALSO on the long-burst path — delay it past the sender's window-burst airtime (`connection_policy::mcDpskBurstAirtimeMs ≈ 18.7 s`, already computed) so the NACK lands in the inter-burst gap. Rate-agnostic = guard derived from the actual burst airtime, not the 30 ms sack_delay. The fully radio-correct version is **carrier-sense** (don't key up while the peer is heard); the burst-airtime-scaled guard is the minimal targeted fix. Needs rig A/B on a lossy channel (collision only appears on a hole). Likely interacts with BUG-MCDPSK-FILE-COMPLETION (a persistent hole that never repairs).

### BUG-MCDPSK-FILE-COMPLETION: MC-DPSK file never finalizes — sender ACK RTO under-budgets the RTT → FINAL chunk never reached — **FIXED + GATE-PROVEN + HW-PROVEN on IONOS (2026-06-30)**
- Status: **ROOT-CAUSED (workflow + adversarial verification) + FIXED + ctest-clean + FAITHFUL-GATE PROVEN + LIVE-IONOS PROVEN.** gui_qso good@7 1KB MC-DPSK (ULTRA_ROBUST_IDLE_PING=1 to clear the handshake floor, DBPSK R1/4): **FILE_CRC_OK_COUNT=2, ALPHA_FILE_DONE_COUNT=1, RESULT=PASS** (was FILE_CRC_OK=0 / resend-forever / FAIL). GOODPUT=10 bps (~11 min/1KB — the #71 SPEED lever, not this bug). Pre-existing (#73); surfaced once the handshake fixes let MC-DPSK connect + the ACK-collision fix (33ccade) widened the receiver hold.
- **HW proof (2026-06-30, IONOS MPG@8 Good, Pi5→Mac, commit 9579a1a both ends, ULTRA_ROBUST_IDLE_PING=1 + ULTRA_CONNECT_RATIOMETRIC_SNR=1):** ladder correctly dropped to **MC-DPSK DBPSK R1/4** (mcdpsk_in_band effective SNR 0.9–5.1 dB read < 10 → not OFDM), **ARQ window=3 timeout=43.6 s** (the fixed RTO; old ~30 s < 37.9 s RTT), both tone-burst ACKs `bitmap=0x0` (**0 retx, 0 holes**), sender `[FILE] Transfer complete (37.4 s)`, receiver `Received OK (568 bytes, CRC ok)` → **md5 65f0ced2…b0de byte-identical**. The old build blind-resent forever here; now it finalizes cleanly. Goodput 0.12–0.20 kbps = #71.
- **NOT an assembly/flag/routing bug** (the earlier hypothesis): the receiver path is byte-correct and identical to OFDM (every delivered seq → "Processed file transfer payload"; FINAL/MORE_FRAG plumbing sound; file chunks are FrameType::DATA → `file_transfer_.processPayload`, NOT the binary DATA_START/CONT/END path). The real cause is a **sender-side ACK-RTO defect**: `computeMCDPSKAckTimeoutMs` was passed `arq_.getSackDelay()=30 ms` instead of the **6376 ms receiver tone-burst SACK hold** (that 33ccade added), AND omitted the **~16 s receiver serial-decode latency** (#56). So the RTO (30.1 s) < the measured half-duplex RTT (**37.9 s**) → the sender blind-resends the whole window before the ACK lands (all 21 retx `cause=timeout`) → doubled airtime → the FINAL chunk (~seq 32 of 33) is never reached in a bounded session → `checkAndFinalizeReceive` never fires → `file-recv=0`. This bug and BUG-MCDPSK-ACK-COLLISION are the SAME bug (33ccade widened the hold without updating the sender deadline).
- **Fix (CHANGELOG 2026-06-30):** `computeMCDPSKAckTimeoutMs` now budgets the full physical RTT (`2·tx_burst + receiver_sack_hold + ack + turnaround` ≈ 43.5 s > 37.9 s), lower clamp lifted to the physical floor; Connection computes the hold ONCE and feeds both the receiver setter and the sender RTO. Completion now reliable in an unbounded session.
- **Remaining (separate lever, NOT this bug):** SPEED. RTT ~38 s × ~11 windows ≈ **~7 min for 1 KB** (the #56 decode latency + 32-byte chunks / window=3 = ~33 seqs). Practical MC-DPSK files need #71 (DQPSK rung / bigger chunk / window). Repro: `/tmp/v72_nat7`; completion proof: `/tmp/v73_complete`.

### BUG-MCDPSK-CONTROL-BAUD: CONNECT_ACK shipped at the data rung's mod/baud → handshake strand on sps≠1024 rungs — **FIXED (#72, 2026-06-29, CHANGELOG)**
- Standardized MC-DPSK on sps=1024 + routed handshake-negotiation frames through the DBPSK control profile. Forced rungs now CONNECT; no regression. See CHANGELOG.

### BUG-HANDSHAKE-PING-FLOOR: low-SNR PING/CONNECT classifier starves → handshake never connects below ~15 dB Good (caps ALL operation) — **FIXED, DEFAULT-ON (2026-07-01) — RE-OPENED for the mid-SNR SIM window (see re-open note)**
- **RE-OPEN NOTE (2026-07-01 evening, found by the #58 Good@12 boundary probe):** at SIM
  reference levels good@12 the PING's noise-flooded gap reads data_rms **0.23** — ABOVE the
  a81725d high-SNR-churn gate `kPingChirpLockMaxDataRMS=0.16` — so the robust chirp-lock emit
  is SUPPRESSED (chirp corr 0.66-0.75 solid, ratio 0.78 "data-bearing", path1=0 path2=0, no
  PONG, 5/5 PING timeouts, 2/2 seeds). This is the documented "0.16 is absolute → level-fragile"
  caveat biting: the gate separates rig PING gaps (0.04-0.09) from good@20 false-syncs
  (0.28-0.33), but the good@10-15 SIM window falls in between. The a81725d "PROVEN good@10/12"
  claim predates the 0.16 gate and no longer holds. Rig low-SNR (MPM@8 etc.) unaffected. Fix as
  already noted: noise-floor-RELATIVE emit gate. Until then, sim work at good@10-15 cannot
  connect out-of-box.
- Status: **FIXED + DEFAULT-ON (rig path); mid-SNR sim window re-opened by the emit gate.** `ULTRA_ROBUST_IDLE_PING` promoted DEFAULT-ON (opt-out `=0`) after all three sub-issues were closed: (1) initiator #27 (bare_chirp_expected_=FALSE during CONNECTING), (2) responder starvation (STAGE2 expects-CONNECT window), (3) high-SNR churn (data_rms≤0.16 emit gate). VERIFIED: good@20 sim PASS with the emit active (2/2 PING/PONG, 1860 bps); rig **MPM@8 pure-default (zero env vars) connects + delivers CRC-clean** (2/2). With #74 (ratiometric) also default-on, the low-SNR path is now the shipped default on a real radio. See CHANGELOG 2026-07-01.
- **What:** a PING is a bare chirp with no data (`encodePing`→`generatePreamble`). The receiver tells a PING from a CONNECT with a LEVEL test (`data/training RMS ratio < 0.5`, abs floor 0.16). At low SNR broadband noise floods the PING's silent gap (ratio 0.68–0.88 > 0.5) → real PING reads as a faded CONNECT → waits for a 4-CW frame that never comes → no PONG → never connects. The chirp itself locks solid (corr 0.6–0.75). Floor map (faithful gate): never connects awgn@6/8 good@8/10/12; marginal good@15; reliable good@20. The published 5 dB AWGN *data* floor never caught this (`measure_ack_fer` skips the live handshake).
- **Fix (env-gated):** when the knob is ON, a solid chirp-lock with a low-LLR (false-lock-rejected) frame emits the PING on chirp signature alone; gated on `bare_chirp_expected_` (FALSE during CONNECTING) so a faded CONNECT_ACK isn't mis-PONGed (#27-safe on the initiator). PROVEN: good@10/12 never-connect→PASS, good@20 no-regress.
- **STAGE2 (2026-07-01, LANDED — responder hole CLOSED):** on PONG-TX the responder now sets `bare_chirp_expected_`=FALSE + arms a ~20 s expects-CONNECT window (app.cpp), mirroring the initiator's PROBING→CONNECTING disarm, then the tick re-arms. Sound for any window: while CONNECTING the initiator re-SENDS CONNECT (never re-PINGs, connection.cpp:2421-2442) and a stray PONG to a CONNECTING initiator is a no-op + half-duplex-inaudible, so worst case is occasional harmless waste, never permanent starvation.
- **Remaining before default-ON (the NEW blocker):** flipping `ULTRA_ROBUST_IDLE_PING` default-ON REGRESSES the faithful sim gate at good@20 — the near-silent HIGH-SNR PONG's residual pushes the data/train ratio just above 0.5 (data_bearing) with low LLR + a real chirp, so the robust emit fires SPURIOUSLY → PING/PONG churn (11/9) → ~30 s connect → gate overrun. Isolation-proven: same build with `=0` → good@20 PASS (1860 bps); STAGE2 alone is clean. FIX NEEDED: a high-SNR-safe emit gate (e.g. a higher data_bearing floor separating a noise-FLOODED low-SNR PING (ratio 0.68–0.88) from high-SNR PONG residual (~0.5–0.6)). Also: throughput below ~8 dB is a separate lever (#71), not this bug.

### BUG-DOPPLER-COHERENCE-MODECHANGE-WIPE: rate-change wipes the Good/Moderate coherence pool — precondition before enabling ULTRA_RATE_ADAPT
- Status: **OPEN but GATED-INERT (no default-path impact).** Tracked by the 2026-06-16 four-tier
  review of the Doppler-coherence discriminator (CHANGELOG 2026-06-16, design doc §11).
- **What:** the discriminator is hosted in `StreamingDecoder` so it survives the per-group OFDM
  demodulator recreation during a normal (fixed-rate) burst transfer — and it does (GUI-proven:
  good 0.70 [GOOD] 27/27, moderate −0.11 [MODERATE/POOR] 78/78). BUT a MODE_CHANGE recreates more
  than the demodulator; the coherence verdict reverts to the blind `fading_index` during the
  ~30 s re-convergence window after a rate move.
- **Why it doesn't bite today:** mid-stream rate moves originate only from the `ULTRA_RATE_ADAPT`
  (default-OFF) machinery. At CONNECT the coherence is always invalid (no OFDM data pooled), so the
  rate pick is byte-identical to today → zero default-path regression.
- **Fix before enabling ULTRA_RATE_ADAPT:** persist `coherence_score_`/`coherence_valid_` across the
  MODE_CHANGE (carry at the Connection layer, or keep the estimator pool across the rebuild), AND
  route the CW-count/negotiate sites through `connection_policy::coherenceAdjustedFadingIndex`. See
  `docs/CHANNEL_DISCRIMINATOR_DESIGN_2026_06_15.md` §11 follow-up #2.

### BUG-IONOS-PI5-CHEAP-DAC: Pi5→Mac handshake one-way — cheap-card carrier JITTER (root cause REFINED 2026-06-15) — partial software mitigation landed
- Status: **OPEN (hardware), root cause REFINED + software mitigation landed 2026-06-15.** First
  Mac↔IONOS↔Pi5 bringup. After the CONNECT-decode code fix (CHANGELOG 2026-06-14), Mac→Pi5 completes
  (CONNECT decodes 4/4, Pi5 CONNECTED) but Pi5→Mac CONNECT_ACK never decodes → initiator never
  CONNECTED → no file transfer.
- **UPDATE 2026-06-16 — when the handshake DOES complete (Mac→Pi5, MPG), the FILE TRANSFER stalls,
  and the proximate mechanism is now isolated from the real receiver log (`/tmp/pi5_full.log`):**
  the chirp anchor decodes but burst data frames 2–6 are ERASED every group at broadband RMS
  0.0038–0.0145 (all under the absolute `BURST_ERASURE_RMS_THRESHOLD=0.015`) → all-zero-LLR group →
  `header invalid` → ARQ retransmit → stall. The 0.015 gate was implicitly "~25 dB below the SIM
  anchor (0.27)"; at the cheap card's ~6× lower RX operating level it became only ~5 dB below the
  anchor and erased recoverable frames. **FIX A landed (UNCOMMITTED, see CHANGELOG 2026-06-16):** the
  gate is now operating-level-RELATIVE (`max(0.055*anchor, 0.005)`), zero sim regression, keeps 16/20
  of the erased frames on IONOS. **Hardware note (user, 2026-06-16): the MPG logs are the CLEAN Fe-Pi
  HAT, not the old USB card** — so the deficit is codec-INDEPENDENT (high-PAPR data below the anchor +
  lower RX operating level + Good fade nulls), and on a clean codec the KEPT frames should DECODE, so
  FIX A is likely the actual fix (the cheap-card per-carrier-LLR / `CHEAP_CARD_ROBUSTNESS_PLAN.md` work
  applies only to the old USB card). Re-test Mac→Pi5 MPG with `ULTRA_BURST_RMS_DIAG=1` to confirm the
  anchor/level + that frames are KEPT and decode; if marginal, raise RX gain and/or use R2/3 (the
  morning run negotiated the fragile R3/4). Decisive proof is rig-only.
- **UPDATE 2026-06-15 — the "bad clock, swap the card" diagnosis was RE-EXAMINED and largely REFUTED;
  the real lever is carrier JITTER, and a software mitigation now recovers the slow-jitter regime.**
  In-sim impairment discrimination through the real encode→AWGN→decode path (`test_mcdpsk_clock_offset`,
  CHANGELOG 2026-06-15) showed: (1) sample-CLOCK offset is NOT the bottleneck — R1/4 LDPC decodes CRC-
  clean to ±1000 ppm with or without correction, and the prior "−1800..−3000 ppm wandering" figure was a
  measurement artifact (a crystal cannot wander ~1200 ppm run-to-run; the values match an integer ALSA
  buffer-period drop = USB starvation on the headless Pi); (2) band TILT is NOT the bottleneck (frequency
  diversity + LDPC ride through 20 dB); (3) the cheap DAC's ±7 Hz carrier JITTER IS the bottleneck (it
  exceeds the DQPSK ±45°/symbol decision margin → payload garbage, chirp survives). The MC-DPSK demod now
  has clock-offset + carrier-jitter tracking (default on) that recovers SLOW jitter and is a proven no-op
  / no-harm otherwise. So this is no longer purely an equipment fault — re-test with the current build
  before concluding a card swap is needed.
- **LIVE IONOS BRINGUP 2026-06-15 — DEFINITIVE ISOLATION (the re-test above, run on real hardware).**
  Worked the full Mac↔IONOS↔Pi5 path knob-by-knob and eliminated everything except the card:
  - **Wiring:** one direction (Pi5→Mac) was dead (Mac RX rms 0.0004 = digital silence) until a CABLE
    SWAP — then both directions carried signal. (So part of the original failure was a bad cable, not
    the modem at all.)
  - **IONOS input CLIPPING (the big one):** the Pi5's transmit was overdriving the IONOS input — the
    panel read `Lvl=2000 mvp-p` RED with `CF=1.01 [0.05 dB] (PEP/Pavg)` = the multi-carrier signal
    arriving as a HARD-CLIPPED SQUARE WAVE (a clean MC-DPSK signal is CF≈10 dB). That clipping, not the
    DAC, was garbling the CONNECT_ACK at high level. Gain staging: the IONOS CH-IN gain (factory default
    1; had been run at 5–20) and the **Pi5 ALSA `Speaker` volume are the real level knobs** — see tx_drive
    note below. Nominal IONOS input max is 1800 mvp-p; the modem's ~10–12 dB crest factor means target the
    *average* well under that (peaks clip first).
  - **CONNECT_ACK→PING misclassification (software, FIXED):** once clipping was removed, the Mac decoded
    the low-level CONNECT_ACK to garbage AND then mis-classified it as a PING via the
    `pre_ldpc_llr_reject` path (ldpc_attempted=false short-circuits `ping_by_chirp_lock`), firing BEFORE
    the 06-14 ratiometric 4-CW wait gate. FIXED (CHANGELOG 2026-06-15): a ratiometric guard lets a
    data-bearing (ratio≈1) MC-DPSK frame fall through to the full 4-CW decode instead of being pinged.
    HW-confirmed the Mac now attempts the 4-CW decode (`got 2592 soft bits`); PingDetector + 16/16
    regression still pass.
  - **Level matched, decode STILL fails → THE CARD.** With wiring, clipping, and classification all
    fixed, swept the Mac's RX: at 0.08 (low), **0.16 (matched to the Pi5's working 0.17), and 0.20**, the
    4-CW CONNECT_ACK decodes to GARBAGE every time — while the SoundBlaster→Pi5 CONNECT decodes fine at
    0.17. Same level, clean, green, no clipping; only the transmitting card differs. **Definitive: the
    cheap Pi5 USB dongle's TX is the limit** (its measured tilt+distortion+jitter), beyond what the
    clock/slow-jitter trackers recover.
  - **tx_drive is NOT a software bug (correction of an in-flight claim).** On the Pi5, changing tx_drive
    (0.10/0.20/0.70) barely moved the level — but the AUDIO-category log shows the per-burst hardware
    normalization is APPLIED, not bypassed (no `normalization bypassed fragment` warning), and tx_drive
    DID work on the Mac (0.7 clipped to 2000). So the software is correct; the Pi5 cheap card's analog
    output just doesn't respond linearly to the digital level (saturation/compression — same hardware
    trait as the decode distortion).
  - **VARA paradox / forward path:** the user runs this exact dongle with a commercial HF modem, so the
    card is usable by a *robust-enough* handshake. Our 4-CW DQPSK R1/4 CONNECT_ACK simply isn't as
    robust. Two paths: (a) swap to the SGTL5000 I2S HAT (on hand) for an immediate working QSO;
    (b) earn cheap-card tolerance in software — DBPSK control frames (double the phase margin vs jitter)
    + per-carrier SNR weighting (handle tilt) + more control-frame FEC. See docs/CHEAP_CARD_ROBUSTNESS_PLAN.
- **Root cause (tone-test measured, /tmp/measure_pi5_tx.sh + measure_mac_tx.sh + analyze_tone.py):**
  the Pi5's cheap "USB Audio Device" **DAC/playback** is the problem. Same IONOS, same two cards,
  TX/RX roles swapped — only the transmitting card differs:
  | metric | Pi5 cheap card TX (fails) | Mac SoundBlaster TX (clean) |
  | band flatness across 0.5-2.6 kHz | **14.8 dB tilt** | 5.7 dB |
  | 2nd-harmonic distortion | **-17.8 dB** | -36.7 dB |
  | freq accuracy (jitter) | **±7 Hz** | ±0.5 Hz |
  The cheap DAC's 15 dB band tilt starves the edge MC-DPSK carriers + the distortion/jitter smear the
  DPSK phase → the multi-carrier 4-CW payload fails to decode, while the robust band-sweeping chirp
  shrugs it off (CFO≈0, chirp decodes) — hence PING works, CONNECT payload doesn't. The cheap card's
  **ADC/record is fine** (reverse dir clean), so it's specifically the TX path.
- Of the three measured TX impairments, the discrimination (above) shows tilt is absorbed by diversity
  and a stable clock offset is absorbed by LDPC; the **jitter** is what crosses the DQPSK margin. The
  measurement was also taken on the Pi5 host, so part of the apparent jitter/tilt may be Pi-side USB
  starvation rather than the DAC itself (re-measure driving the cheap card from an unloaded host to
  separate DAC-intrinsic defects from host starvation — recommended experiment).
- **Fix order (revised 2026-06-15):** (1) re-test Pi5→Mac with the current build — the new MC-DPSK
  clock+jitter tracking may already close it if the real jitter is slow; (2) if it still fails, capture
  a Pi5-TX CONNECT recording and decode it offline to confirm jitter rate vs an ALSA-drop discontinuity;
  (3) swap the Pi5's cheap USB soundcard as a BACKSTOP only if the residual jitter is fast/large enough
  to be below the trackable regime. Earlier "balance the L/R levels" framing was wrong/incomplete. HW
  knobs the bringup harness sets: Pi5 `callsign=PI5`, `tx_drive=0.7`, ALSA Speaker −3 dBFS; Mac
  `tx_drive=0.7`, input vol 60. Run the gui_qso gate LID-OPEN on an unloaded machine (lid/throttle →
  ALPHA audio starvation `SKIP unsearched < min` → false FAIL).

### BUG-TNC-B2F-002: post-burst non-burst FF frame not delivered (blocks bulk-accum-to-burst B2F) — LAYERED
- Status: **FIXED 2026-06-05.** ROOT CAUSE (LAYER 2, finally isolated): the ENCODER did not revert
  its LDPC lifting-z after a burst. A burst lifts the encoder to z=81 (`setLDPCLiftingZ(81)` in
  transmitBurst); nothing reverted it, so the next non-burst frame on the `transmit()` path (the FF
  terminator, a chat line, an SR-ARQ repair) was still encoded at **z=81 (~106 880 samples)** while
  the receiver — which DOES revert at group-end — decoded it as **z=27 (~17 920 samples)**. BRAVO
  read the first ~17 % of a z=81 frame as a z=27 frame → saturated-magnitude/random-sign LLRs
  (`|llr|=20`, `llr_avg≈0`) → LDPC 0/CW → never ACKed → stall. Fix: revert the encoder to z=27 for
  every non-burst frame (`modem_engine.cpp` transmit() dispatch). Plus the TNC accumulation was made
  spec-correct (honest VARA `BUFFER` — see CHANGELOG 2026-06-05) so the body no longer striped across
  burst+interactive transports (the out-of-order corruption). 20 KB JPEG now delivers byte-identical
  with a CLEAN teardown on the plain (no-bulk) path, GUI-verified. Layers, peeled in order:
  - LAYER 0 (sync acquisition): FIXED 2026-06-04 (catch-up drain + full-anchor buffer, CHANGELOG).
  - LAYER 1 (§14.24 gate drop): **FIXED 2026-06-05** — `have_burst_descriptor_` now drops at
    group-end (`finalizeBurstGroup`, streaming_burst_interleave.cpp) instead of persisting the whole
    connection, so the trailing FF is no longer gated as mid-burst. The FF now REACHES the decoder
    (`Chirp detected … escalating to N CWs`). Verified no multi-group regression: gui_bidir AWGN@20
    7/7 each way, 0 CWfail.
  - LAYER 2 (post-burst mode-revert — **REMAINING**): the FF demodulates to garbage — saturated
    magnitude, random sign (`CW FAIL, llr_avg≈0.3, |llr|=20`). RULED OUT as root (traced 2026-06-05):
    NOT a buffer/coordinate bug — `process()` calls `processPresynced(samples, 2)` which skips 2
    training symbols from the buffer START, and `sync_position_ = search_start + start_sample` points
    AT the training (the `training_start=63839` in the log is only the CFO phase ref, and cfo=0 here);
    NOT the CW-count (2 vs sent 1) — that fallback is a SYMPTOM: the CW0 header peek can't read a
    valid header BECAUSE the demod is already garbage. ROOT: post-burst, BRAVO does not fully revert
    to NON-burst interactive mode. The FF is a small non-burst interactive frame that on the normal
    (no-preceding-burst) path delivers fine via LIGHT-LTS + the proven non-burst decode; but here it
    is sent with a FULL preamble (post-burst `forceNextFrameFullPreamble` / `expect_full_ofdm_anchor_`
    re-anchor) and decoded through the full-anchor burst-data path, which mis-estimates/mis-structures
    it → garbage soft bits. LAYER 1's latch-clear reverted ONE piece of burst state
    (`have_burst_descriptor_`); the remaining burst-data-mode state (the full-anchor expectation, the
    data-decode profile, `fixed_frame_codewords_`) is not reverted, so the trailing FF is treated as
    burst data. Fix = complete the post-burst revert so the trailing interactive frame decodes as a
    normal non-burst frame — a COORDINATED encoder+decoder change (both ends must agree the burst is
    over and drop back to the light-LTS interactive PHY). This is the production form of the
    declared-but-unwired `traffic_profile.hpp` TrafficClass system (File vs Chat/Control). Deliberate
    work, must not regress the burst path; do not land blind against hanging-run verification.
  - LAYER 2 SIDESTEPPED (2026-06-05): rather than fix the broken non-burst full-anchor demod, the
    TNC now ROUTES the trailing FF through the BURST path too (`bulk_burst_started_`,
    tnc_session.cpp): once a body has bursted, trailing flushes stay burst, reusing the PROVEN
    burst descriptor+group decode (same path that delivers the body 10/10). This made the image
    deliver byte-intact over burst.
  - LAYER 3 (clean teardown — **REMAINING**): the FF is a tiny (3-byte) 1-frame burst. It DELIVERS
    (BRAVO `Received OK (3 bytes)`), but its tone-burst GROUP-ACK does not get back to ALPHA
    (`group-ACK timeout`), so ALPHA's `file_transfer_` stays SENDING/busy. The next trailing flush
    then fails `sendFile` "already in progress" and the bulk-accum retries every quiet period (~65
    wasted re-stages of the same 3 bytes) → the sender's `connect` never exits cleanly (the message
    IS delivered; only the sender-side close hangs). Root: tiny / turn-flip-boundary 1-frame burst
    groups don't ARQ-complete on the sender side (related to the old "Issue 2 turn-flip" ACK
    reliability). Fix candidates: (a) make tiny/last-group tone-burst ACKs reliable at the turn
    boundary; (b) coalesce body+FF into ONE burst (under-report BUFFER 0 early so PAT writes the FF
    before the body transmits, then burst both — risk: premature Flush close); (c) the LAYER-2 PHY
    fix so the FF can go non-burst. An idle-gated flush (only flush when `getTxBackloggBytes()==0`)
    was added but does NOT cover this (a tiny burst reports ~0 remaining bytes while file_transfer_
    is still busy on the ARQ).
- Symptom: with `ULTRA_TNC_BULK_ACCUM=1`, a PAT B2F body now bursts (z=81) and decodes CRC-clean,
  but the trailing non-burst `FF` terminator never reaches PAT (`Receiving … offset 0`, mailbox
  empty). PAT retries the whole message (`100%→0%→100%`) → the run HANGS instead of disconnecting.
- Root cause: after acquiring the post-burst full-anchor chirp (now succeeds, `up_pos` small),
  BRAVO does a control-first peek (1 CW) then processes the frame with the **burst-data geometry**
  it's still holding — `samples=10080` (burst frame size), reconfigures to the QPSK R1/2 *data*
  profile — but ALPHA's `FF` is a NON-burst frame (29 B → 106880 samples, full preamble, ~2 CW at
  R1/4). Geometry/profile mismatch → the FF never completes → not routed to the data port. The
  plain non-burst path WORKS when NOT preceded by a burst (the pre-bulk-accum image delivered),
  so the burst-left decoder state (`have_burst_descriptor_` set per group at
  `streaming_ofdm_decode.cpp:762`, the burst frame geometry, warm-sync next-group expectation) is
  what mis-decodes the trailing non-burst frame. There is no end-of-burst signal, so the receiver
  stays in burst-decode mode.
- SURGICAL root cause (fully traced 2026-06-05, with debug logging since reverted):
  1. The BODY delivers fine. Confirmed via temporary logs in `onFileReceived`/`onModemDataReceived`:
     the burst body decodes CRC-clean and BRAVO-TNC delivers all 12215 bytes out its data port to
     PAT-BRAVO (`state=CONNECTED, marker=raw, data_out=1`). The POLLOUT flush is correct. In B2F the
     sender writes body → `Flush()` → THEN the `FF` terminator, so PAT-BRAVO HAS the body and is
     waiting only for the FF (its `offset 0` line is just stale; VARA `Read` has no deadline so it
     waits forever, no timeout).
  2. The FF (a 1-CW non-burst frame after the burst) is DROPPED at the §14.24 gate
     `streaming_ofdm_decode.cpp:1013-1022`: `if ((use_burst_interleave_ || burst_transport_rx_) &&
     connected_ && have_burst_descriptor_) { re-search; return; }`. The control-first peek decodes
     the FF's CW0 at R1/4-control profile → garbage (the FF is DATA, not control) → reaches this
     gate → re-searched away.
  3. THE LATCH: `have_burst_descriptor_` is set on the first BURST_HEADER
     (`streaming_ofdm_decode.cpp:762`) and PERSISTS for the WHOLE connection — by deliberate design
     it is cleared ONLY on disconnect (`streaming_decoder.cpp:537-551`; it is the RX source-of-truth
     for the descriptor-declared z=81, must survive across groups+ACKs within a transfer). So after
     the body burst it stays set forever and the gate drops EVERY post-burst non-burst frame.
- Fix direction (NOT done — deliberate cross-engine change, must not regress §14.24 / z-lifecycle):
  clear `have_burst_descriptor_` when the burst FILE TRANSFER COMPLETES (the clean "burst is over"
  signal — preserves in-burst z-sizing). The completion signal lives in the ProtocolEngine
  (`FileTransferController::on_received_` → `Connection::setFileReceivedCallback`), but the latch
  lives in the ModemEngine's `StreamingDecoder`; there is no existing ProtocolEngine→ModemEngine
  path, so this needs new wiring (e.g. a `StreamingDecoder::clearBurstDescriptor()` invoked from the
  file-received callback at the app/TNC layer that holds both engines). Do NOT shortcut with
  `&& waveform_->wasBurstInterleaved()` on the gate — mid-burst noise has a ~50/50 marker and would
  fall through to the data decode, re-introducing the §14.24 shared-estimate poisoning.
- BONUS bug found: bulk-accum (BRAVO side) also hoards PAT-BRAVO's small CONTROL sends (BUFFER 50 on
  its SID/FS+) — the cap should apply only to bulk bodies, not tiny control writes.
- Repro: `ULTRA_TNC_BULK_ACCUM=1 /tmp/b2f_pat/run_image.sh` (BOUND with a timeout — it HANGS, PAT
  retries 100%→0%→100% forever). Diagnostics: VARA_DEBUG=1 on PAT for its data RX;
  `ULTRA_S16_TRACE_WARM_WINDOW=1` + BRAVO `--log-category …,sync`; mailbox
  `/tmp/pat_bravo_mbox/BRAVO/in/*.b2f`.

### BUG-TNC-B2F-001: bidirectional B2F stalls — the NON-BURST short path (the actual message path) was dead at RX + ACK
- Status: **FIXED for the message path** (2026-06-03). Issues 1, 3, 4 fixed; Issue 2 (burst
  turn-flip) remains but is **off the B2F message path** (Winlink messages are small → non-burst).
- Symptom: PAT↔PAT Winlink message over `ultra_tnc`/OTASim connects + completes the B2F
  handshake, then never delivers. **Reproduces single-machine** (shared clock) → NOT a
  cross-machine timing problem. (Earlier framing chased the burst path because an A/B test
  forced `kInteractiveMaxBytes=0`; the DESIGN routes small messages to the non-burst path.)
- Root cause (Issue 1, fixed `c27aa45`): the one-way burst bypass skipped the ISS/IRS turn
  gate → both B2F stations keyed up uncoordinated and collided. Fixed via `half_duplex_interactive_`
  turn-gating. Serialization verified correct.
- **Root cause (Issue 3, fixed 2026-06-03): the receiver synced cleanly (corr=1.0, SNR 28 dB)
  yet DROPPED every non-burst DATA frame.** When `burst_transport_rx_` became the unconditional
  default (ULTRA_BURST_TRANSPORT removed 2026-06-02), the §14.24 control-peek-fail re-search at
  `streaming_ofdm_decode.cpp:~1004` started discarding ALL non-burst multi-CW DATA as
  "burst-regime noise": a non-burst `sendBinary` frame carries no BURST_HEADER descriptor →
  `pending_total_cw_=0` → it enters the 1-CW control peek, fails it, and was re-searched away.
  Fix: gate the re-search on `sync_controller_.have_burst_descriptor_` (only mid-burst, where a
  real burst's SHARED channel estimate is at risk); a standalone non-burst frame falls through
  to the legacy data decode → delivered via SR-ARQ.
- **Root cause (Issue 4, fixed 2026-06-03): the responder ACK'd in the wrong waveform.** The B2F
  responder pre-confirms `handshake_confirmed_` in `enterConnected` (so it can speak first),
  which skipped the only site that fires `on_handshake_confirmed_()` → the modem's
  `handshake_complete_` stayed false (reset by `setConnected()`, with `setWaveformMode`→OFDM
  landing afterwards), so BOB keyed its SR-ARQ ACK in **MC-DPSK** while ALICE listened in OFDM
  → ACK never landed → ALICE retransmitted to the retry cap. Fix: re-fire `on_handshake_confirmed_()`
  once on the responder's first decoded frame (guaranteed past `setConnected`+`setWaveformMode`),
  switching TX onto the negotiated OFDM data waveform.
- Verified: `tests/test_ultra_tnc_sim_audio` (two real `ultra_tnc --sim-audio` over OTASim).
  Default = bulk burst file (8192 B, CRC-clean — no regression). `ULTRA_TNC_TEST_NONBURST=1` =
  bidirectional 300 B short message, **CRC-clean BOTH directions** (was: forward dropped entirely).
- **End-to-end VERIFIED (2026-06-03):** real PAT↔PAT Winlink B2F, single machine
  (OTASim → 2×`ultra_tnc` → 2×PAT). `pat connect varahf:///BRAVO` ran the FULL exchange clean —
  banner → proposal → `FS +` → body 0%→100% → `FF`/`>FQ` → disconnect — and the message landed in
  BRAVO's inbox. The prior session stalled at the proposal; now it delivers.
- Root cause (Issue 2, still open): the BURST path needs full chirp+LTS **anchor re-acquisition
  on every turn-flip**; a burst transfer whose turn flips mid-stream re-acquires at corr~0.27.
  Not on the message path; relevant only to bidirectional *bulk* over one connection.
- Full diagnosis + repro: `docs/TNC_B2F_HALFDUPLEX_FINDINGS_2026_06_03.md`.

### BUG-FINACK-001: Final-group ACK loss → sender infinite-resends the last group; no clean transfer close
- Status: **FIX LANDED, UNVALIDATED** (2026-06-02). Decode-independent re-ACK implemented
  in `connection.cpp::onBurstGroupReceived` (the `!all_ok` else branch now routes a resent
  ALREADY-DELIVERED group into the controller's existing `onGroupReceived`→`seqLess`→re-ACK
  path instead of dropping it). Builds; burst/ARQ unit tests pass (StopWaitARQ,
  SelectiveRepeatARQ, ConnectionAdaptive, ToneBurstAckMonitor/Watterson). **NOT yet validated
  on a triggering run** — needs a fade-timed seed where the final GROUP_ACK is genuinely lost
  so the re-ACK actually fires and the transfer closes cleanly on-air (re-ACK fired 0× on the
  post-fix runs because their final ACK happened to land). PRODUCTION-VISIBLE: the GUI/sweep
  quick-kill (scenario_passed now PASSes on delivery + pkills) MASKS this in the harness, but
  real hardware will still show the wasted-airtime infinite last-group resend until validated.
  Was OPEN since 2026-05-31, folded into thread C (`docs/OFDM_COHERENT_ONLY_DECISION_2026_05_31.md §5`).
  Pre-existing; exposed by a fade landing on the completion ACK.
- **Still TODO for the robust fix:** a FILE_END / completion handshake (FILE_END → FILE_END_ACK
  → DISCONNECT) so the close doesn't depend on a single per-group ACK landing at all. The re-ACK
  breaks the infinite loop; the handshake is the belt-and-suspenders close.
- Area: burst completion handshake — `Connection` GROUP_ACK path + the "payload drained →
  auto-disconnect" trigger; receiver duplicate-group handling in `onGroupReceived` / burst assembler.
- Reported by operator (live GUI), coherent QPSK R1/4 Moderate@14 seed 777:
  "it received the file and alpha is still sending."
- Repro / evidence (`/tmp/zmode_mod14_qpsk_s777b`): Bravo logs `Received …10240 bytes, CRC ok`
  at 282 s (file COMPLETE). Alpha sits at `cursor=10240/10240, resent=1, resend_left=0` and
  re-emits `TX Burst: 6 frames` for `group_seq=18` every ~10 s (ack_timeout) indefinitely; the
  scenario only ends via `exit_after`. seed 42 escaped it solely because its final ACK happened
  to land and `auto-disconnect (payload drained)` fired.
- Root cause: the completion handshake has no robust close. The last group's **GROUP_ACK was lost
  in a fade**, so the sender never learns the receiver has it and resends forever; "payload drained
  → disconnect" never triggers because the sender still considers the final group unacked. When the
  sender resends an **already-delivered** group, the receiver does **not re-ACK** — it tries to
  re-decode the duplicate (which here also hits the fade-lost-descriptor z=27 path →
  `0/6 max_iters=0`, a red herring) and stays silent. The file delivered fine; only the close fails.
- Impact: on real hardware, wasted airtime (sender keeps keying the last group) and a transfer that
  never cleanly terminates without an external timeout. Half-duplex final-ACK fragility.
- Fix (thread C): group-level **duplicate detection + re-ACK** — a resend of an already-delivered
  group re-emits the GROUP_ACK **without** re-decoding. Plus a proper **FILE_END / completion
  handshake** so the transfer terminates on "receiver has the whole file" (FILE_END → FILE_END_ACK →
  DISCONNECT), not on a single per-group ACK happening to land. (Note: §7.6 z-consolidation would
  *partly* help IF the re-ACK were decode-gated — but the robust fix is decode-independent.)
- Verification: a fade-timed seed where the last GROUP_ACK is lost (seed 777 Moderate@14) must still
  reach a clean DISCONNECT (no infinite group-18 resend); `ALPHA_DISCONNECTED_COUNT>0`, RESULT=PASS.

### BUG-CTRL-001: Control path is still the bottleneck in aggressive fading profiles
- Status: IN_PROGRESS (handshake leg fixed 2026-04-26)
- Area: OFDM connected mode (ACK/SACK/control reliability)
- Symptoms:
  - Data codewords decode but ACK reception misses trigger avoidable retransmits/timeouts.
  - Most visible with aggressive profiles (for example D8PSK R1/2), but can still appear on weaker seeds in other OFDM rates.
- Impact:
  - Throughput tail collapse on bad seeds.
  - File transfer latency variance much larger than message transfer variance.
- Current mitigations already in tree:
  - R1/4 control profile for OFDM control frames.
  - ACK repeat/coalescing and improved ARQ observability.
  - `DISCONNECT_SEQ` protection against stale data ACK being mistaken as disconnect ACK.
  - **Proactive CONNECT_ACK retransmission (responder side)** — covers handshake-leg
    losses at OFDM data modes. Auto-mode baseline at SNR=15 moderate (DQPSK R1/2)
    went from 4/5 → 5/5 message tests and 2/3 → 3/3 file 2048 tests on 5/3-seed
    samples. See CHANGELOG 2026-04-26.
  - **Airtime-derived ACK/retransmit RTO (commit `d182751`, 2026-05-23, backlog #119).**
    Replaced the `[8000,12000]` magic timeout clamp with an RTO derived from burst
    airtime + carrier-sense/SACK coalesce + ACK airtime, so the sender no longer
    times out before a half-duplex-deferred ACK can physically return. Eliminated
    premature timeout-retransmission of already-delivered frames on a clean channel
    (AWGN SNR20 16QAM R1/2: 5/10 seeds → 0/10). See CHANGELOG 2026-05-23. NOTE: a
    longer honest RTO may add idle wait at window boundaries on clean channels —
    quantify under #121 (recoverable dead air) before further RTO tuning.
- Remaining work:
  - Connected-mode tail variance under aggressive forced profiles (D8PSK R1/2,
    DQPSK R3/4) when channel falls outside auto-selector envelope — these aren't
    auto-selected, so they bite only operators forcing modes manually.
  - BRAVO missing the initiator's CONNECT (opposite leg of the same race) is rare
    on the production envelope but lacks a comparable fast retry — initiator's
    `connect_timeout_ms = 60000` is far longer than the cli_simulator harness's
    30s PHASE 1 budget, so harness exposure of this case looks like seed noise.

### BUG-NACK-001: Burst GROUP_NACK sent over MC-DPSK handshake waveform, not the tone-burst
- Status: FIXED (2026-05-29, branch feat/oneway-arch-2026-05-27, commit pending). Verified
  seed 2 Good@20 warm-ON: tone-burst NACK emits=1, MC-DPSK NACK=0, MC-DPSK TX post-connect=0,
  file PASS 11/11 CRC-clean, run ~34 s faster (171 s vs 205 s).
- Area: burst transport failure path (`Connection::onGroupReceived`, the `!all_ok` branch)
- Reported by operator (waterfall observation), seed 2 Good@20 file transfer:
  - "Bravo issues a NACK using the old NACK system, not the tone-burst."
  - "After group 0's initial failure there is a weird MC-DPSK signal repassing on the waterfall."
- Root cause (BOTH symptoms are one bug): on a failed group the receiver sent
  `transmitFrame(makeGroupNack(...))`, a 20-byte control frame over the CURRENT
  waveform. For group 0 `handshake_complete_` is still false (it is set later, in
  the `all_ok` path the NACK branch `return`s before reaching), so the frame went
  out as the **MC-DPSK handshake waveform = 149760 samples ≈ 3.1 s** on the air —
  the "weird MC-DPSK signal" — instead of the §15 tone-burst (675 ms). The §15
  work routed the success GROUP_ACK to the tone-burst but left the failure
  GROUP_NACK on the legacy control-frame path.
- Impact:
  - ~4.6× slower NACK (3.1 s vs 675 ms) → slower deep-fade resend recovery (a chunk
    of seed 2's latency); off-waveform MC-DPSK energy mid-OFDM transfer.
- Fix: emit a NACK-type tone-burst (`AckType::Nack`, whole-group missing mask) from
  the `!all_ok` branch, mirroring `setSendGroupAck`. The sender's `onToneBurstAck`
  already maps a NACK-type tone-burst to `burst_transport_.onGroupNack` (resend
  now), so the entire receive path already existed — only the emit was wrong.
  Falls back to the OFDM `makeGroupNack` frame if the tone-burst callback is absent.
- Verification: seed 2 warm-ON GUI re-run — expect 0 MC-DPSK TX bursts after
  connect and tone-burst NACK emits > 0, file still CRC-clean.

### BUG-8PSK-001: Decision-directed tracking corrupts the 8PSK channel estimate on fading
- Status: FIXED (2026-05-29, channel-adaptive DD gate). DD cascade removed; see
  `docs/8PSK_GOOD_FADING_DIAGNOSIS_2026_05_29.md` and
  `docs/SYSTEM_PICTURE_FADE_SURVIVABILITY_2026_05_29.md`.
- Fix: `use_coherent_dd` now also requires `last_fading_index < 0.15`
  (`channel_equalizer_pilot.cpp`) — DD off on frequency-selective/faded frames
  (where its wrong decisions poison H), on for AWGN/flat frames (where it's
  safe). Threshold from measured data (AWGN ≤0.07, Good ~0.34 median) and equal
  to the existing LLR-scaling "faded" boundary. Env override `ULTRA_DD_FADING_MAX`
  (default 0.15); `ULTRA_COHERENT_DD_OFF=1` force-off. A per-symbol pilot-anchor
  innovation gate was tried first and removed — ineffective (a wrong-decision
  rotation and a legit between-pilot interpolation error are indistinguishable
  per-symbol; flat across a 4× tightness sweep).
- Verification: GUI 8PSK AWGN30 PASS 2330 bps 0 CW fail (DD stays on, no
  regression); Good@20 cascade gone (DD-on 83–125 CW fails/no delivery →
  adaptive: seed 42 PASS, seed 43 delivered CRC-clean). Offline (measure_ack_fer
  qam8 Good@20): adaptive == DD-off (46/120 chunks) vs DD-on 41/120.
- RESIDUAL (separate, NOT this bug): 8PSK is too marginal a rung for Good@20 —
  delivers slowly with heavy resends and fails on hard fades (seed 44: 0
  delivery). QPSK is ~2× more survivable there (offline 106/150 vs 52/150
  chunks). This is a rung-selection design item (adaptive mod choice per
  channel), tracked in `docs/SYSTEM_PICTURE_FADE_SURVIVABILITY_2026_05_29.md`,
  not a DD bug.
- Original diagnosis (root-cause history):
- Area: `src/ofdm/channel_equalizer_pilot.cpp` (`use_coherent_dd`, ON for QAM8/QAM16)
- Symptom: forced 8PSK (QAM8) R3/4 delivers perfectly on clean AWGN (2330 bps) but
  fails on Good@20 fading — heavy resends, frequently no delivery, confident-WRONG
  bits (strong |llr|≈15-20, LDPC parity fails 50-99 unsat). Bimodal per group (6/6
  or 0/6, never partial).
- Root cause (experimentally confirmed by five-way elimination — chain/fade/demap/
  static-H all ruled out): decision-directed channel tracking feeds 8PSK's occasional
  wrong hard-decisions (its 22.5° boundaries are tight on a fading channel) back into
  the H estimate, poisoning it → cascade of confident-wrong bits. `ULTRA_COHERENT_DD_OFF=1`
  flips 8PSK Good@20 from FAIL → PASS. The base LTS+pilot H is fine (DD-off uses it and
  delivers); a genie perfect-H with DD *on* still failed (DD corrupts even a perfect H).
  Matches the code's own comment warning DD "poisons H on bad decisions during fades."
- Impact: 8PSK is the promotion lever toward 3000 bps; this blocks it on fading.
- Fix direction: reliability-gate DD (only DD a symbol whose EVM is well inside the
  95% noise radius — the code comment's own proposal), or gate DD off for 8PSK on
  fading; pair with channel-adaptive promotion (8PSK on clean/mild, QPSK on deep fade).
- Residual after DD-off: still marginal (heavy resends) — irreducible
  frequency-selective phase near spectral nulls; ARQ/adaptive-rate handles the tail.

### BUG-HARNESS-001: gui_qso_scenario.sh hard-aborts on auto-negotiated mode != --expect-mod
- Status: KNOWN (2026-05-29; harness limitation, not a modem bug).
- Symptom: on a channel where the auto rate-ladder legitimately promotes (e.g. AWGN30
  → 16QAM), running with `--expect-mod QPSK` makes `hard_failure_reason` abort the run
  at the MODE_CHANGE (`REASON=unexpected_data_mode`), ~25 s in, 0 file-transfer
  attempts. Falsely looks like a QPSK/AWGN modem failure (it is not — forced QPSK on
  AWGN30 delivers fine).
- Fix direction: when rate is not locked/forced, treat `--expect-mod` as advisory
  (accept the auto-selected mode) instead of hard-aborting.

### BUG-HARNESS-002: measure_ack_fer fidelity defects (under-piloting + broken AWGN burst path)
- Status: PARTIALLY FIXED (2026-05-29). The pilot-spacing defect is fixed; the AWGN
  burst_chunk defect is documented, not yet fixed.
- Defect 1 (FIXED): `makeOFDMConfig` hardcoded `cfg.pilot_spacing = 10`, while
  production uses `ofdm_link_adaptation::recommendedPilotSpacing(mod,rate)` = 5 for
  R1/2 & R2/3, 8 for R3/4. This under-piloted coherent high-order mods by ~2× and
  made 16QAM look structurally worse than production would. Fixed to call
  `recommendedPilotSpacing` (`tools/measure_ack_fer.cpp`). NOTE: re-test showed the
  under-piloting was NOT the cause of 16QAM folding (folds at spacing 5 too) — but
  the harness should still match production.
- Defect 2 (KNOWN): `--config burst_chunk --channel awgn` returns 0 frames recovered
  for all mods (qpsk/qam8/qam16, all DD modes) — the offline AWGN burst path does not
  match the GUI/OTASim AWGN path (where these mods decode fine). So measure_ack_fer
  AWGN burst_chunk numbers are not usable; use the GUI for AWGN.
- Defect 3 (KNOWN, 2026-05-29): the **Z=81 / N=1944 (long-LDPC burst keystone) path
  decodes to 0% in measure_ack_fer — even noiseless QPSK** — on BOTH the frame path
  (`--config data4_full` + `ULTRA_LDPC_Z=81`) and the burst path
  (`--config burst_chunk` + `ULTRA_LDPC_Z=81`). Verified Good@60: qpsk/qam16 = 0/100
  at Z=81 vs 91/84 at Z=27; confirmed pre-existing (reproduces with all genie edits
  git-stashed). ROOT CAUSE: `ULTRA_LDPC_Z=81` changes the ENCODER codeword size and
  the decoder's *block size* (→1944, `streaming_decoder.cpp:641`), but
  `decodeFixedFrame`'s `ldpc_z` argument is sourced ONLY from the burst descriptor
  (`last_burst_descriptor_.lifting_z`, `streaming_ofdm_decode.cpp:2876-2889`), NOT
  from the env. With no BURST_HEADER on the wire (frame path never sends one; the
  burst_chunk harness only enables the descriptor in its `wrong_group` branch), the
  decoder accumulates 1944 soft bits but decodes them with a **Z=27 matrix** →
  guaranteed failure. Production (GUI burst transport) IS unaffected: it transmits the
  BURST_HEADER with `lifting_z=81`, so the decoder learns Z and decodes correctly
  (GUI-proven CRC-clean ~3.1 kbps Good@20). CONSEQUENCE: **measure_ack_fer cannot
  validly screen the production burst config (Z=81); all offline Z=81 numbers are
  garbage.** The valid offline data is Z=27 only.
- DEEPER ROOT CAUSE (2026-05-29, attempted fix found it is MULTI-LAYER — NOT a quick
  fix): even with the encoder emitting Z=81 (via `setLDPCLiftingZ`, added as
  `--ldpc-z 81`) AND the decoder env-forced to Z=81 (block-size getter +
  `decodeFixedFrame` ldpc_z both read `ULTRA_LDPC_Z`), Z=81 STILL decodes 0% — even a
  1-CW frame, noiseless. Two more layers:
  (1) the waveform's soft-bit-grab size (`active_ldpc_block_size`, used by
      `getSoftBits`) is bumped to 1944 ONLY by `setActiveLDPCLiftingZ()`, which fires
      ONLY on descriptor-consume (streaming_ofdm_decode.cpp:763). With no descriptor
      the decoder grabs 648 bits of a 1944-bit codeword ("Got 648 soft bits,
      proceeding to decode") → garbage.
  (2) the descriptor-consume itself does not fire in the harness: the BURST_HEADER (a
      DQPSK-R1/4 control frame) is routed to the DATA decode path (line 1124), not the
      CONTROL path (line 661, which recognizes `FrameType::BURST_HEADER`), so it is
      mis-decoded as a failed data frame and dropped — never consumed (verified:
      `--burst-descriptor 1` gives 0% at BOTH Z=27 and Z=81 on Good@60).
  KEY INSIGHT (user): the BURST_HEADER is the SINGLE self-describing conveyor of
  lifting_z AND cw_per_frame AND group_size AND interleave flags together — the
  decoder is designed to learn the whole burst geometry from the wire in one shot, so
  piecemeal patching (env for Z, setFixedFrameCodewords for cw) cannot reproduce
  production. The ONLY faithful fix is to make descriptor-consume fire in the harness
  (route the BURST_HEADER to the control decode path). That is real plumbing across
  the control-detection + block-size + descriptor-consume layers — deferred. The
  `--ldpc-z 81` scaffolding (encoder emits + announces Z=81; env wires the decoder
  decode matrix + block-size getter) is committed as a correct PARTIAL toward that fix.
- Impact (updated): characterize ANY 16QAM-on-production-burst (Z=81) claim on the
  real-time GUI; the offline harness cannot do it until descriptor-consume is fixed.
- Impact: measure_ack_fer is a fast *screen* for relative offline comparisons on the
  Good path at **Z=27 only**; its AWGN path and its entire Z=81 path are not faithful.
  Confirm any fade/throughput conclusion — and ANY 16QAM-on-production-burst claim —
  on the real-time GUI per the project's standing rule.

### BUG-CFO-001: OFDM two-stage CFO refinement remains incomplete
- Status: OPEN
- Area: `src/ofdm/demodulator.cpp`
- Evidence:
  - TODO at `src/ofdm/demodulator.cpp:1307` for proper two-stage CFO (CP/frequency-domain refinement).
- Impact:
  - CFO handling works for current tested profiles but remains less robust than desired for broader OTA variation.
- Next steps:
  - Implement and validate two-stage CFO refinement with seeded regression + OTA logs.

### BUG-TEARDOWN-001: RX decode thread UAF crash on disconnect/shutdown (SIGSEGV in HilbertTransform::process)
- Status: OPEN (surfaced 2026-05-31; PRE-EXISTING — identical faulting stack in crash
  reports from 2026-05-28, predating the SyncController work).
- Symptom: `ultra_gui` (receiver side) SIGSEGVs at end-of-transfer / disconnect / shutdown.
  EXC_BAD_ACCESS / KERN_INVALID_ADDRESS. Faulting thread = the RX decode thread:
  `HilbertTransform::process` ← `OFDMChirpWaveform::detectDataSync` ← `StreamingDecoder::
  searchForSync` ← `processBuffer` ← `ModemEngine::rxDecodeLoop`. The receiver's log ends
  abruptly right after `Disconnected ... (Remote disconnected)` + `SR-ARQ: Reset`, with NO
  `RX decode thread stopped` line (cf. the sender, which tears down cleanly).
- Does NOT corrupt results: the file delivers CRC-clean BEFORE teardown (e.g. QPSK R1/4
  AWGN@10 seed42 → RESULT=PASS, GOODPUT=380, 0 retx, then crash on teardown).
- Root cause (CONFIRMED from both racer stacks 2026-05-31): on disconnect, the MAIN thread
  synchronously rebuilds the waveform while the RX decode thread is mid-demod on the old one:
  - WRITER (main thread, frees/rebuilds): `Connection::enterDisconnected` → `ModemEngine::
    setConnected(false)` → `StreamingEncoder::setDataMode` → `MCDPSKWaveform::initComponents`
    → `new MultiCarrierDPSKModulator` → `new ChirpSync` → `ChirpSync::generateTemplate`.
  - READER (Thread 22, faults): `rxDecodeLoop` → `StreamingDecoder::searchForSync` →
    `OFDMChirpWaveform::detectDataSync` → `HilbertTransform::process` → UAF (`KERN_INVALID_
    ADDRESS`, small offset = deref into freed/rebuilt waveform memory).
  The §14.36 "crash fix v3" (`applyPendingConnectedOFDMMode`) deferred only CONNECTED-mode
  reconstruction to a safe boundary in the RX loop; the DISCONNECT reconfig
  (`setConnected(false)`→`setDataMode`→waveform rebuild) is NOT synchronized with the RX
  thread at all. `stopRxDecodeThread()` joins, but `enterDisconnected` runs the rebuild on the
  protocol/main thread WITHOUT first quiescing the still-running RX `detectDataSync`.
- Why it matters for the SyncController refactor: this is exactly the §7.7#4 lifecycle-
  discipline scope the refactor must own. It also POLLUTES the GUI gate's crash signal —
  every gate run crashes at teardown, so a refactor-introduced crash can't be cleanly
  distinguished from this one. Fixing it first gives the behavioral phase a clean crash
  baseline.
- Fix direction: synchronize disconnect/shutdown waveform reconfig with the RX thread —
  either stop+join (or quiesce under `reset_generation_` + `buffer_mutex_`) BEFORE the
  waveform/HilbertTransform is reconfigured on disconnect, or route the disconnect reconfig
  through the same deferred safe-boundary the connected path uses. Fold into the
  SyncController lifecycle ownership (`reset()` discipline).

## Release Blockers

An issue is release-blocking if it causes any of:
- reproducible data loss,
- deterministic disconnect deadlock,
- non-deterministic gate failure in default mode ladder.

Current blockers:
- None identified for `0.2.1-alpha` default ladder.

## Fixed Bugs

- 2026-05-13: BUG-PING-DETECTOR-001 fixed - real-HF PINGs now classify via additive chirp-lock plus LDPC-invalid PATH 2 while preserving the clean-cable/AWGN RMS-silence PATH 1.
- 2026-05-13: BUG-TNC-SESSION-001 fixed — R1 added the full RX decoder/session reset on disconnect; R2 added audio-producer quiesce/drain plus a reset-generation guard for in-flight decode callbacks, so a persistent `ultra_tnc` starts the next back-to-back PAT CONNECT from a fresh modem session boundary without stale capture backlog or stale cursor commits.
- 2026-05-08: BUG-CARRIER-LDPC-001 fixed — CarrierLDPC v1 was a new OFDM coded-bit wire image, but SP4 enabled the runtime as a local modem default instead of a negotiated peer capability. A partially upgraded Mac↔Pi pair applied the TX permutation on one endpoint while the peer decoded legacy ordering, producing the AWGN R1/2 1KB 0-ACK/15-timeout failure. The repair uses the existing `PHY_MASK_V1` capability: modern-modern CONNECT/CONNECT_ACK enables CarrierLDPC on both TX/RX; modern-legacy leaves the legacy ordering active. Synchronized upgraded hardware now passes AWGN / Good / Moderate at SNR=15 R1/2 1KB with DATA `Ncw=8` active and ACK/control `Ncw=1` inactive.
- 2026-05-08: BUG-BENCH-001 resolved — committed `fixtures/*.wav` are valid
  post-CONNECT DATA fixtures. They decode cleanly with
  `./build/decode_bench --mode bench --connected ...`; the earlier `0`-frame
  result came from running the bench in disconnected control-search mode.
- 2026-05-05: BUG-RATE-001 fixed — adaptive MODE_CHANGE panic-downshift on short Watterson-Good transfers. Hysteresis (`ADAPTIVE_PRESSURE_WINDOWS_FOR_DOWNGRADE = 2`) + lockout reduction (`ADAPTIVE_POST_DOWNGRADE_LOCKOUT_MS` 15 s → 5 s). 5-seed reproducer now 5/5 PASS with worst-case throughput improved 444 → 684 bps (no panic downgrade). See CHANGELOG 2026-05-05.
- 2026-02-12: GUI immediate TX abort control (`STOP TX`) added.
- 2026-02-12: GUI telemetry split into PHY vs effective goodput, plus ARQ health view.
- 2026-02-11: OTA control-path hardening and bootstrap safety updates.
- 2026-02-06 to 2026-02-10: ACK/control-frame decoding and ARQ robustness fixes.

For full details and commit-level history, use:
- `docs/CHANGELOG.md`
