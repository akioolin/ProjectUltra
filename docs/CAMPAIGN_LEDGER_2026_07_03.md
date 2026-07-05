# Overnight campaign: 3000 bps Good@20 — 2026-07-02/03

Baseline (main 8a771f9, 5-cell gate): g42=1940 g43=1210 g7=1310 | AWGN ref=3370 | VARA target=3086 (rig MPG@20)

## Tracks
- A: sim lever A/B campaign (Phase 0 accounting -> ranked levers -> paired 3-seed A/Bs)
- B: rig MPG@20 ladder bench, both ends lockstep on 8a771f9

## Log
- 22:5x Phase-0 accounting workflow launched (wdk77my64). Pi5 updated to 8a771f9, rebuild running.

## Rig bench MPG@20 (main 8a771f9, Pi5->Mac, 50KB incompressible, md5-verified)
- Run1: 1.02-1.04 kbps, 393-403s, CRC ok, md5 OK. Rungs: R2/3@49 -> R3/4@127 -> 16QAM@153 (2 MC timeouts) -> COLLAPSE (base frozen, blind retx) -> ESCAPE-drop@250 (5 stuck retx) -> QPSK R3/4@273 -> done. The g43 collapse signature live on HW; user watched it.
- Run2: 1.95 kbps, 210.0s, CRC ok, md5 OK. R2/3@31 -> R3/4@146 -> 16QAM@172 (1 MC retry) HELD to completion; 16QAM era ~3.4-3.9 kbps. Collapse-vs-hold variance is the rig story: 1.03 vs 1.95.
- Run3: 1.53 kbps, 267.8s, CRC ok, md5 OK. Fast climb (R3/4@79), 16QAM probe@132 -> healthy fast demote@145 (13s, NACK path worked), re-climb@221 held. Baseline mean ~1.50, spread 1.03-1.95.
- Run4a: HANDSHAKE FAIL — one-way: Mac RX'd CONNECT + went CONNECTED, Pi5 decoded 0/10 CONNECT_ACKs (34s apart, 356s) -> "Connect failed after 10 attempts". Chirp path alive (PONG ok), 4-CW data Mac->Pi5 dead. Appeared between run3 and run4 (no rebuilds). Retrying with full restarts; tone-test if it repeats.
- Run4b: 1.01 kbps, 406.8s, CRC ok. MODE_CHANGE_ACK loss x9 (16QAM MC seen 9x by Mac). ROOT CAUSE FOUND: Mac->Pi5 runs ~6-9dB light (peer_snr 7-13.5 vs 17-23 reverse); Mac TX = built-in headphone out, reboot reset output volume to 70. Raised 70->85 for run 5 (probe via peer_snr). tx_drive symmetric 0.5/0.5 (ALC semantics change NOT the cause).
- Run5 (vol 85): 1.77 kbps, 231.7s, CRC ok, md5 OK. CONTROL HEALED: 0 MODE_CHANGE retries (was 1-9). peer_snr 12.8-15.8 (was 7-13.5). Still ~4dB asym -> probing vol 100 (run 6).
- Run6 (vol 100): 2.17 kbps, 188.6s, CRC ok, md5 OK — NIGHT BEST. peer_snr symmetric (14.7-22.5, trough 7.2), 1 MC retry, 16QAM by t=109 held to done. Volume 100 = the setting.
- Run7 (vol 100): 1.28 kbps, 319.3s, CRC ok, md5 OK. Fade-luck low (120s trough saga at 75%, 5 MC duplicates — control-frame fragility persists in troughs even at good level -> piggyback-rate-signal lever stays relevant).
== RIG PHASE DONE: 7 runs, 6 CRC-clean byte-exact, 1 handshake fail (level). Clean-level (vol100/85): 1.77/2.17/1.28, mean ~1.74 kbps. Degraded-level (vol70): 1.03/1.95/1.53/1.01. First-ever ladder-on-HW: auto-climbs to 16QAM, escape-drop fires live, level calibration recovered + memorialized. Gap to VARA 3086: ~56%. ==

## R3/4 crest rung A/B (ULTRA_QAM16_R34=1)
- Rawgn: 3640 NEW RECORD (16qam_r34 61%, 0 escapes) | R42: 1910 (r34 24%, 1 escape — the flagged mod-only same-rate escape EXECUTED, CRC-clean) | R7: 1650 (never asserted R3/4)
- Verdict: knob-gated keep, default-OFF. AWGN ceiling +120; Good-neutral (crest too short for streak).

## Window-16 confirmation pass (defaults, 2nd runs)
- V42=2040 V43=1770 V7=1640, all CRC-clean. Per-seed means: g42~2100 g43~1835 g7~1645 -> Good@20 mean 1487->~1860 (+25%), 8/8 clean cells. Gains REPLICATE.

## Wire-format rig bench (window 16, vol 100)
- W1: 1.52 kbps, 269.5s, CRC ok, md5 OK. Wire format PROVEN on HW. 1 MC retry on 16QAM climb; mid-transfer demote saga (3 dup MCs).
- W2: 1.21 kbps, 338.4s, CRC ok, md5 OK. Deep-trough run (peer_snr 3.2 at t=61), demote+reclimb cycle, several MC retries.
- W3: DEGENERATE — entry pick MC-DPSK DBPSK R1/4 at peer_snr=3.9/fading 0.65 (dial 20!). The #58 snapshot-variance failure at its worst: one deep-trough handshake reading -> ~90bps rung for a 50KB file. Killed (would exceed the 600s window). THE motivating specimen for the handshake-aggregation fix.
- W4: 1.25 kbps, 328.0s, CRC ok, md5 OK. THE MC-TIMER EVIDENCE: R3/4 MC x3 @18.5s spacing, 16QAM MC x3 @18s — the borrowed burst-ACK timeout confirmed on the wire; ~74s/328s = MC dead-air. W tally: 1.52/1.21/[degen]/1.25.

## MC-timer + pacing A/B (all CRC-clean)
- T43 (ratiometric timer only): 1540 (ref 1835, within noise), 6 MC retries at ~5s spacing (was 18.5s).
- P42 pacing: 2340 (+11%, best g42 yet) | P43: 1510 | Pmod: 910 (down, within noise) -> pacing knobs stay OFF; rig collapse eras = proving ground.
- Committed + pushed to feat/coherent-window-16. ConnectionAdaptive test updated (pinned the old borrowed timer).

## SNR-pool validation (#58 increment 3)
- ctest green. OFF42 1950 PASS. ON42 1680 PASS entry R2/3 (N=1 identity). ON43 1460 PASS entry R1/4 (= s43's historical single-snapshot pick; sim N=1 -> pool inert by design; rig retries give N>=2).
- SAFE7 FAIL attributed AWAY from the pool: knobs-OFF control fails identically (PING-floor sim window, pre-pick). Low-SNR safety = unit-proven; rig MPM@8 later.

## W5 (full stack, FAILED -> fixed): MC-timer livelock found + closed
- Entry: peer_snr=19.3 (local_measured — pool + label fix working), clean OFDM R2/3 entry.
- REGRESSION (mine): airtime-only ~5s MC retry < rig control-ACK RTT (receiver decode backlog unmodeled) -> every retry keyed onto the ACK in flight -> 72 MC receptions, no move ever committed, window blown. First run on the 5s timer (W1-W4 ran the old 18.5s).
- FIX 9a48e43: floor = burst deadline/2 (~9s wideband, ratiometric both terms). ctest green. Pi5 rebuilding (build4).
- Sentinel note: -10.0 renders raw in the MODE_CHANGE guiLog line (status-bar n/a gate works; cosmetic residual noted).
- W5b (timer floor 9s): livelock GONE (11 MCs, moves commit ~9s) BUT hard stall at 25% from the FIRST mid-transfer move to cancel (500s); sender saw clean groups/climbs, zero retx logged. Sim cells with identical code+knobs passed -> rig-timing-dependent. peer_fading frozen on wire (freshness fix missed the fading byte — filed). Bisect: W6 pins ULTRA_MODE_CHANGE_RETRY_MS=18500 (exact W1-W4 retry behavior), all else identical.
- W6 (timer pinned 18500): CLEAN — 1.88 kbps, CRC ok, 5 MCs. Regression bracketed to retry cadence. Timer default RESTORED to full deadline (it IS ratiometric); kept 4 retries + env pin. Entry 6.2->R1/2 (trough snapshot, ladder healed in ~90s).
- W7: 1.68 kbps CRC ok, 4 MCs, entry 12.9->R2/3, clean climb. W8: 1.69 kbps CRC ok, 7 MCs, clean.
== FULL-STACK TALLY: {W6 1.88, W7 1.68, W8 1.69} median 1.69 vs old-wire {1.52/1.21/1.25} median 1.25 = +35% ON HARDWARE. Zero failures/storms/stalls in the final config. Entry distribution (all W): 12.9/14.2/[3.9 degen pre-pool]/17.9/19.3/17.2/6.2/12.9 — defer knob would have caught W6's 6.2 (sub-OFDM? 6.2+5=11.2>10 Good floor -> R1/2 entry, not sub-OFDM; defer wouldn't fire; ladder healed in 90s. W3-class DBPSK entries = defer's target). ==

## Entry-cap lever (ULTRA_ENTRY_CAP_R34, default-OFF) + fading-byte freshness
- aa64563 fading byte freshness (same 3*Tc contract). Entry-cap sim: E42 2610 PASS (campaign-best g42 cell; gate silent as designed), E43 1960 PASS. Rig A/B next (W9-W11).

## Entry-cap rig batch (W9-W11, all knobs)
- W9 1.47 (entry 12.3), W10 1.70 (entry 13.2 at MODERATE 0.66 -> saturation bound carried it into OFDM LIVE), W11 1.79 (entry 16.9, 3 MCs, cleanest run of campaign). Median 1.70.
- Entry-cap gate never fired (readings < 18.2) — kept ON for future batches; margin revisit possible with more connects.
- Time-to-16QAM: 134/118/117s (was 150-190 pre-stack). fading n/a renders live (W9/W10). All md5-exact.

## All-knobs batch W12-W14 (band ROUGHENED this hour)
- W12 1.05 (entry 14.3 Good, no clean groups till t=271), W13 0.99 (entry R1/4 @10.4/Mod-0.66 via saturation bound), W14 1.22 (entry R1/4 @9.8/Mod-0.74, bound again). Median 1.05.
- HONEST read: 2/3 entries Moderate-classified vs 0/6 in the W6-W11 hour — the channel worsened; lever effects unmeasurable in grind conditions (no promotes to accelerate). All entries honest, ladder correct, 14/14 md5-exact since the level fix.
- These grind eras are pacing's target -> W15-W17 with pacing knobs added.
- W15 (six knobs): WINDOW FAIL (not corrupt) — entry R1/4 @9.8/Mod-0.67, deep collapse (13+ zero-progress rounds), PACING FIRED AS DESIGNED (24 engagements, 5s Tc-derived holds), escape correctly floor-blocked at R1/4. 50KB@R1/4-grind needs ~600-800s > the 600s sender window. Band still rough.
- W16 (six knobs): ENTRY CAP FIRED FIRST TIME — 19.4 reading -> R3/4 entry -> 16QAM in 90s (fastest ever). Then rough-band collapse from ~26KB (R3/4<->16QAM ping-pong, ~330s zero-progress era, pacing 10 engagements), WINDOW FAIL at 648s. Pi5 log captured for collapse forensics.
- W17: 1.46 CRC ok — R1/4 saturation-bound entry, clean climb (16QAM @192s), pacing 0 (no collapse). Batch {W15 fail, W16 fail, W17 1.46}.
== DAY TALLY: Good-hour stack median 1.70 {1.88/1.68/1.69}; rough-hour {1.05/0.99/1.22/fail/fail/1.46}; 15/17 delivered, ALL byte-exact, 0 corruption. Levers all field-active: entry cap fired (W16 19.4->R3/4->16QAM in 90s FASTEST EVER), saturation-bound entries x4, pacing engages in collapses, n/a freshness live. ==
- W18: HANDSHAKE FAIL (one-way, run-4a class — Mac connected@31, Pi5 CONNECT_ACK-deaf in the rough band). Known class, salvage never engaged (pre-transfer). Relaunching.
- W18b: 1.00 transfer / 0.96 session, CRC ok. Entry R1/4 @7.0/Mod-0.66 (lowest honest OFDM entry). PROMOTE-CARRY visible in the wild: R2/3->R3/4->16QAM in 68s once clean (24s last rung vs ~45s old arithmetic). SALVAGE 0 (no collision topology).
- W19: 1.57 transfer / 1.47 session, CRC ok. Entry 13.7->R2/3, 16QAM@122 (4 MC-ACK retries), SALVAGE 0.
- W20: 1.00 transfer / 0.96 session, CRC ok — **SALVAGE FIRED x9 AND THE TRANSFER COMPLETED**: the W16 seq-collision topology recurred (one-way ACK loss through rate moves) and the below-window salvage + straddle-merge delivered every byte. W16 stranded; W20 finalized. Interim fix FIELD-PROVEN same day.
== SALVAGE BATCH: {W18 hs-fail, W18b 1.00/0.96, W19 1.57/1.47, W20 1.00/0.96+9-salvages}. Rough band persisted all afternoon. ==
== HANDOFF ITEM (next session): the structural epoch fix for BUG-ARQ-SEQ-COLLISION (move-epoch on DATA frames echoed in ACKs — wire change, design from the KNOWN_BUGS entry + wg9vqd61j forensics). ==
- W21: 1.15 transfer / 1.09 session, CRC ok. Crest entry 17.8/Good-0.29 -> immediate dip (demote R1/2 @65). Band oscillating. SALVAGE 0.
- W22: 1.65 transfer / 1.54 session, CRC ok. Entry 11.8->R2/3, 16QAM@135, 1 MC retry. Band improving — seven-knob Good-hour batch forming.
- W23: 1.17 transfer / 1.11 session, CRC ok. SALVAGE x3 (2nd field engagement in 4 runs — the collision topology is COMMON here; structural epoch fix priority up, salvage default-ON case building). Band oscillating.
- W24: 1.40 transfer / 1.32 session, CRC ok. Bound entry 9.9/Mod-0.72->R1/4, carry double-climb R3/4->16QAM in 36s. Rolling {1.15/1.65/1.17/1.40} median 1.29, band oscillating. 24/24 completed = byte-exact.
- W25: 1.29 transfer / 1.22 session, CRC ok. Bound entry 9.4/Mod-0.67, 16QAM@230. Rolling {1.15/1.65/1.17/1.40/1.29}. 25/25 byte-exact.
- W26: 1.79 transfer / 1.67 session, CRC ok — TIES RIG RECORD. Entry 12.9->R2/3, 16QAM@134, clean. Rolling {1.15/1.65/1.17/1.40/1.29/1.79}. 26/26 byte-exact.
- W27: WINDOW FAIL at 956s (deep trough; entry 9.3->R1/2, first move @512). SALVAGE x14 — 3rd field engagement; kept the transfer coherent (75% delivered in-order) but the clock won. No corruption.
- W28: 1.40 transfer / 1.32 session, CRC ok. Bound entry 8.3/Mod-0.72, carry climb 16QAM@200. 27/27 completed byte-exact.

## CORRECTION (user): IONOS = Watterson AUDIO SIM (Winlink team), statistically STATIONARY at MPG@20
- All "band roughened / ionosphere shifted" framing RETRACTED. The morning-vs-afternoon clustering (entries 12.9-19.3 -> 8.3-10.4, deliveries 1.5-1.9 -> ~1.0-1.4) must be: fade-realization luck (Watterson long-tail epochs), OUR chain drifting (volumes verified stable 100/1.00; Pi5 CFO-birdie lines = 28 in current run — live suspect), or an afternoon KNOB hurting (promote-carry/pacing/entry-cap: sim-clean, rig sample too small to acquit).
- PROTOCOL CHANGE: sampling loop becomes an ABLATION — alternate config A (morning four-knob: SALVAGE+POOL+WIRE_FRESH+nothing-else... note morning W6-W11 ran POOL+WIRE_FRESH only) vs config B (seven-knob) on successive runs; pairs accumulate until the difference resolves or is bounded by noise.
- W29 [B/7-knob]: 1.16 transfer / 1.10 session, CRC ok, SALVAGE x2 (4th engagement). 16QAM@312 (slow half).
- W30 [A/3-knob]: 1.78 transfer / 1.66 session, CRC ok. 16QAM@116 clean. Strong A sample.
- W31 [B/7-knob]: 1.08 transfer / 1.02 session, CRC ok, SALVAGE x5 (5th engagement). Entry 9.4->R1/2 low draw, grind till 297. PAIR1: A 1.78(e12.9) vs B 1.08(e9.4) — entry draws unmatched, realization dominates; continue pairs.
- W32 [A] attempt1: HANDSHAKE FAIL (one-way CONNECT_ACK loss, 3rd today ~10% of attempts — control-robustness datapoint). Relaunching.
- W32 [A] attempt2: 0.74 transfer / 0.71 session, CRC ok — deep fade (demote R1/2@229), SALVAGE x8 ON AN A-CONFIG RUN (collision topology = any demote under ACK loss, not knob-B-specific). Delivered at 555.9s/600.
- W33 [B/7-knob]: **1.83 transfer / 1.69 session, CRC ok — NEW RIG RECORD**. Entry 16.2->R2/3, 75% by 200s.
== ABLATION VERDICT (2 pairs): winners SWAP across pairs (A1.78/B1.08, A0.74/B1.83) — Watterson realization variance >> config delta at feasible sample sizes. STOPPED. All knobs KEPT (no evidence of harm; salvage/carry/bound have direct positive evidence). Decisive next lever = the epoch wire fix. ==
- W34 [keep-alive]: 1.65 transfer / 1.55 session, CRC ok. 16QAM@171. 31/31 byte-exact.
- W35 [keep-alive]: 1.33 transfer / 1.26 session, CRC ok, SALVAGE x8 (7th engagement). 32/32 byte-exact.
- W36 [keep-alive]: 1.04 transfer / 0.99 session, CRC ok, SALVAGE x9 (8th engagement). Entry 8.7->R1/2 low draw. 33/33 byte-exact.
- W37 [keep-alive]: 1.28 transfer / 1.21 session, CRC ok, SALVAGE x15 (9th engagement, heaviest). 34/34 byte-exact.
== SALVAGE DEFAULT-ON DECISION: 9/9 field engagements positive, ~60 frames rescued, zero anomalies, unit-tested — flipping per house convention (rig-proven). ==
- W38: 0.37 transfer (1095.7s!) CRC ok — an ~18min trough survived with SALVAGE x30 (heaviest ever; 10th engagement). The user watched this run live ("all messed up") — it's the epoch fix's motivating exhibit: repeat MODE_CHANGEs + R1/2 grind + sentinel renders. 35/35 byte-exact. Display fixes staged (sentinel n/a in [MODE] line; dial-equivalent SNR in status bar).

## EPOCH-ARMED BATCH (ULTRA_ARQ_MOVE_EPOCH=1 both ends, c094e5f)
- W39: 1.38 transfer / 1.31 session, CRC ok. SALVAGE 0, stale-epoch 0 — clean run, epoch dormant, NO regression with the wire change live. 36/36 byte-exact.
- W40-epoch: 1.51 transfer / 1.42 session, CRC ok. SALVAGE 0, stale-epoch 0. 37/37 byte-exact.
- W41-epoch: **1.90 transfer / 1.76 session, CRC ok — NEW RIG RECORDS (both windows)**. Entry 16.9, single-shot MCs throughout, SALVAGE 3 (benign dupes, idempotent), stale-epoch 0. 38/38 byte-exact.
== EPOCH BATCH: {1.38, 1.51, 1.90} all clean, no regression with the wire change live; positive-proof case (trough-crossing regrid) not yet drawn — knob stays on. ==
- W42-epoch: 1.34 transfer / 1.27 session, CRC ok, clean (single-shot MCs, one dip demote). 39/39 byte-exact.
- W43-epoch: 1.25 transfer / 1.19 session, CRC ok. SALVAGE x9 same-epoch (benign RTO-race dupes — with epoch armed the salvage's catches are all dupe-class, consistent w/ design), stale-epoch 0. 40/40 byte-exact.
- W44-epoch: 0.75 transfer / 0.72 session, CRC ok. Deep-fade grinder (first move @446, 4 MC retries), SALVAGE x18 same-epoch dupes. 41/41 byte-exact.
- W45-epoch: **1.97 transfer / 1.82 session — NEW RIG RECORDS**. R3/4@62, 16QAM@88 single-shot, 50% by 143s. SALVAGE x9 dupes. 42/42 byte-exact.
- W46-epoch: **2.08 transfer / 1.91 session — FIRST 2.0+ ON THE RIG**. Entry 16.3, 16QAM@106, clean. 43/43 byte-exact. Day: 1.25 -> 2.08.
- W47-epoch: 1.96 transfer / 1.81 session, CRC ok, single-shot MCs, SALVAGE 0. 44/44 byte-exact. Good stretch: 1.97/2.08/1.96 last three.
- W48 attempt1: HANDSHAKE FAIL (one-way CONNECT_ACK, 4th today ~9%). Relaunch.
- W48b-epoch: 1.65 transfer / 1.53 session, CRC ok. Entry 9.2->R1/2, textbook carry climb to 16QAM@217. 45/45 byte-exact.
- W49 (killed mid-flight per user STOP at ~25%): THE MOTIVATING EXHIBIT — entry R1/4 @11.4/fading-0.66-Moderate at dial-20 Good (user screenshot). Ladder recovered by t=174 (16QAM) but the misclassified entry cost ~100s of R1/4 crawl. Fading-pool fix in flight.
== LOOP STOPPED by user 20:35. Rig parked. Fix cycle: fading pooling at entry + dial-20 calibration doc + sidebar dial-equivalent display. ==
- V1 (pre-margin build, the user's 2nd screenshot): 1.05 CRC ok — entry R1/4 @11.3/0.69-Moderate = the "before" specimen. Margin fix 2996e37 deployed after it; affine basis + piggyback design in flight.

## AFFINE VALIDATION BATCH (a425658 + full stack)
- V2: 1.42 transfer / 1.34 session, CRC ok. **ENTRY 9.3 -> R2/3 Good (affine working — was R1/2 pre-fix)**, 16QAM@105, 2 MC retries on the climb, SALVAGE 5 dupes.
- V3: 1.60 transfer / 1.49 session, CRC ok. **ENTRY 7.3 -> R2/3 Good (clamp region working)**, 16QAM@226. SALVAGE 0.
- V4: 0.74 transfer / 0.71 session, CRC ok. ENTRY 13.6 -> R2/3 Good ✓. Deep-fade epoch (peer_fading 0.65 @300s, demote R1/2), SALVAGE x18. 47/47 byte-exact.
- V5: **2.18 transfer / 1.99 session, CRC ok — rig records at the time**. ENTRY 13.5 -> R2/3 Good ✓ (batch 4/4), 16QAM@95, done 188.2s. 48/48.
== AFFINE BATCH: entry gate 4/4 R2/3 Good across readings 7.3-13.6 (two sub-10 draws correct — pre-fix R1/2; baseline false-Moderate 18.8%). NOTE: whole V-batch ran at Mac output volume 75 (drift; calibrated=100, restored for D-batch). ==
== Cross-env checks: sim good@20 affine entry 13.7->R2/3 = same band as rig (calibration doc §8); sim good@14 UNMEASURABLE (pre-existing handshake floor). Low-dial leg falls to the IONOS multi-dial sweep (user must set the dial). ==

## DESCRIPTOR-SWITCH RIG BATCH (ULTRA_DESCRIPTOR_MODE_SWITCH=1 + standing set, both ends 9d7d47e)
- D1: 1.37 transfer / 1.30 session, CRC ok. ENTRY 10.1 -> R2/3 ✓ (5/5). **FIRST HW DESC-SWITCH adopt** (climb R2/3->R3/4 @101.6, [MODE] local_measured/n-a signature — DESC INFO lines don't pass the GUI log filter); collapse-escape R3/4->R2/3 correctly legacy (1 retry). SALVAGE 1. 49/49.
- D2: **2.43 transfer / 2.20 session, CRC ok — ALL-TIME RIG RECORDS**. ENTRY 10.1 -> R2/3 ✓ (6/6). BOTH climbs via descriptor (R3/4@109, 16QAM@143), ZERO mid-transfer control exchanges/retries/salvages. Done 168.4s. 50/50.
- D3: 0.50 transfer / 0.47 session, CRC ok @813.2s — survival run, genuine Moderate epoch (peer_fading 0.71). 10 descriptor adopts (incl. R1/2->R2/3 recovery @641), SALVAGE 33, clean disconnect. 51/51.
== D-BATCH: 13 adopts / 0 failures / 0 stale-epoch / 0 corruption. THE FINDING: climbs now ~free -> crest probing -> the ESCAPE side (collapse + legacy exchange) is the measured bottleneck (D3: 4 climb-escape cycles in 90s). Phase 2 (receiver rung command, tone-ACK pad bits 42-43) is the quantified next lever; interim candidate = fading-class-gated re-climb streak. ==

## F-BATCH (2026-07-04 morning — forensic arc + tail-window + Phase-3 + crest rung armed)
F3 **2.50-RECORD** / F4 1.28 / F5 2.17 / F6 1.63 / F7 1.73 / F8b 1.71 (F8 killed at a 2.4-eff fade-snapshot entry, first miss in 18). All CRC byte-exact (61/61 lifetime). Every 16QAM entry clean (5/5 first-group 9/9); craters resolve in 3.3-4.1s via receiver command; first live Phase-3 descriptor escape (F7: QAM16 R3/4 escape via DESC-SWITCH); backstop NACK saves observed. The mechanism layer is DONE — remaining variance is fade-draw luck; remaining levers: crest-window frequency (fade-averaged climb gating), arming robustness in fades, turnaround trim.
- F9-F16 (fast-crest f713064 era): F13 **2.12** (3-group R3/4 finish) / F10 1.70 / others 1.36-1.73 in a persistent chop epoch. Firsts: HW 16QAM R3/4 hop+delivery, graded DownOne landings (multiple), WAITING-REBASE voice fired 2x live (cured E1-class stalls in-run), backstop NACK saves, fast-crest 1-group hops. Ladder now oscillates the top two rungs at 3.3-4.1s per move, rarely touching QPSK in chop. Entries 25/25 R2/3 since affine. 69/69 byte-exact.
- F17-F26b (ALC saga arc): F17 0.96 grind -> F18 KILLED = ALC RUNAWAY ROOT-CAUSED (drive ratchet to 0.85 cap, no equilibrium on TX compression) -> guard 7b0953c -> F19 1.77 / F20 2.00 -> drive correlation (F22 Pi5 log: 0.50->0.85 walk; R3/4 craters >0.70) -> ULTRA_ALC_MAX_DRIVE knob 5b55c5a -> **F24 KNEE CONFIRMED (six-group R3/4 ride @ fading 0.09, 1.89 through a deep trough)** -> STANDING CONFIG §7.2. Entry misses: 3/38 (2 fade-snapshot + 1 AWGN-class affine blind spot, queued). F25 void (launch group-kill gotcha, §7). 78/78 byte-exact.
- F27-F32 (final policy layer): F27 1.25 exposed the partial-crater latency (2x ~40s) -> DownOne-on-2-bad-groups (b97df76) -> FIRED LIVE F28 (8/9 partial -> demote 3.6s). F29 zombie (one-way CONNECT_ACK loss + responder half-open FOREVER) -> 240s half-open timeout landed. F30 1.68 / F31 1.40 (survived a 60s total blackout, 7x 0/9) / F32 1.50. Entries 42/42; 82/82 byte-exact. The chop epoch has held ~4h; the stack rides everything; records await a calm hour.
- F33-F44 (standing config, sustained chop epoch): 1.08-1.85 band (F35 1.85 6th-best; F34 first R3/4 ENTRY via entry-cap at an AWGN window, 16QAM@72.8s fastest ever); two ~60s total blackouts survived (voice+backstop+escape chain); entries 54/54 R2/3+ since affine; 94/94 byte-exact. The stack is stable — variance is pure fade-draw.
- SELF-ECHO STALL FIX (bfe5676, the 2.50->1.44 root): forensic wrp84o66o found the sender went deaf to the crater-demote tone-ACK (decoding its own burst echo -> 120k blind-search spiral -> 2xRTO collapse 36s). Fix: suppress re-anchor while the ACK monitor is armed (sender-in-ACK-listen, peer sends tone only). VALIDATED F65-F68: median 1.44->1.69 (+17%), 0 spirals, crater recovery 36s->3.5s. Residual = crater frequency (16QAM R2/3 zero fade-headroom); next = headroom-gated climb.
- F75-F82 (2026-07-05 overnight, Opus): ACK-listen tone-lock guard (7752d60) -> **F75 2.62 RECORD**; gapless monitor sweep + forensics (6340f51) after F76/F77 exposed the tail-window ACK-miss (BUG-POSTTX-ACK-MISS). 10-run validation batch (guard+gapless): F78 0.99 / F79 1.10 / F80 1.50 / F81 1.61 / F82 1.45 (choppy 00:30-01:15 epoch) — EVERY RTO forensically classified genuine fade (monitor detect->consume 105/105 across runs, 0 expired-undetected windows, 0 misses); 130/130 lifetime byte-exact. 16QAM-entry experiment (20f6006) REJECTED: cold entry craters 2/2 (no warm estimate) — climb-first is structural.
- F83-F88 (batch close + flips, 2026-07-05 ~02:05): batch 10/10 md5-EXACT median 1.24 (rough epoch), ~280 ack exchanges 0 misses, 0 expired-undetected, every RTO=genuine fade (HEADNULL forward-loss class quantified ~200s/run worst = next lever). DEFAULTS FLIPPED e2f6096: ULTRA_ACKLISTEN_SUPPRESS_OFDM + ULTRA_ACK_MONITOR_GAPLESS now ON (=0 opt-out); F88 confirm run env-free PASS (defaults active). 136/136 lifetime byte-exact.
- F89-F109 (2026-07-05 day session, Opus + user live-catching): FIVE root causes shipped (escape cap faa6cc3, amnesty 4d9195d, LLR shape 8230fea, ack-SNR median 1535ef7, anchor-wait preserve 35a8b7f) + late-join 9465d00 + repeat-if-silent 244af49/corrected + mid-rung ladder (ULTRA_ENABLE_QAM16_LADDER entry + ULTRA_QAM16_DEMOTE_MIDRUNG landings, 3e08d25). Epoch-stats workflow PROVED the chain never changed (broadband ~24dB flat across the user's reboot; F75's 2.62 was the 89%-clean tail of a ~75%-clean-median distribution). Consolidation batch (midday chop): median 1.02; morning same-config 1.5-1.8. 153/153 lifetime byte-exact. VERDICT: reaction mechanics now clean + fully instrumented; the swings are a CONTROL-LOOP MISMATCH (10s decision quantum vs Tc~2-4s fade cycle, coarse rungs, per-switch tax) — smoothness requires structure: (A) adapt-faster-than-Tc (shorter groups/cw16/leaner turnaround) or (B) sit-and-absorb (margin rung + deep interleave + HARQ accumulation). Both documented; the user picks the next build.
- F110 + cw16 (2026-07-05 ~13:00, a612c61): **cw16 SHIPPED AND PROVEN** — kMaxFixedFrameCodewords 8->16, QAM16 baseline 16 (ULTRA_QAM16_CW16) with the coherence walk intact, TWO duplicated fec/ clamps fixed (Frame+BurstInterleaver hardcoded 1..8 — first cw16 TX threw + 46s silent no-TX; found in-sim same-run). Faithful gate good@20 s42: **PASS 2670** (prior same-seed 1730-1990 = +35-55%). Rig F110: 9 cw16 adopts / 20 16-CW decodes clean (PAPR concern dead), 1.29 chop-bound. THE NUMBER TO CHASE: F75's calm evening window x cw16 multiplier = 3.4-3.7 projected. Full env in handoff; 154/154 lifetime byte-exact.
