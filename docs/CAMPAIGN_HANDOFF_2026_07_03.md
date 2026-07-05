# 3000 bps Campaign — Continuation Brief (2026-07-03)

**Purpose:** complete handoff so ANY capable agent/model can continue this campaign without context.
Read this + `docs/CONNECT_ENTRY_CALIBRATION_2026_07_03.md` + `docs/MODE_SWITCH_PIGGYBACK_DESIGN_2026_07_03.md` + `docs/KNOWN_BUGS.md` (top entries) + the session ledger `/tmp/campaign_3000/RESULTS.md` (copy it somewhere durable if /tmp is at risk).

## 1. Mission and scoreboard
Goal: **3000 bps delivered** on IONOS MPG@20 (a Watterson-Good AUDIO SIMULATOR by the Winlink team — statistically STATIONARY; never attribute run-cluster drift to "band conditions"; suspects are fade-realization luck, our hardware chain, or a code change). Benchmark: VARA = 3086 bps (transfer-phase delivered).
State at handoff: rig **1.25 → 1.70 median → 2.08 peak** (records: 2.08 transfer / 1.91 session, W46); sim AWGN record 3640; ~45 rig transfers, **every completed one md5-byte-exact, zero corruption ever**. Branch `feat/coherent-window-16` ≈ 24 commits, all validated or knob-gated; **origin/main push pending user approval** (contains TWO wire-breaking changes: 16-bit SACK mask always-on; move-epoch when knob-on — both ends must run the same build, and they already do).

## 2. The techniques that produced every gain (USE THESE)
1. **Measure first (Phase-0 budget accounting):** before optimizing, decompose a real run's wall time into productive TX / retx / ACK-turnaround / control dead-air / climb-time from BOTH ends' logs. The biggest measured pool is the next lever. Never optimize an unmeasured loss.
2. **Knob-gate everything:** every lever ships behind an env knob, default-OFF **byte-identical** (static-lambda getenv, read-once). Flip defaults only on rig proof (example: salvage flipped after 9/9 field saves).
3. **Paired A/B on the faithful gate, sequential only:** `tools/gui_qso_scenario.sh --channel good --snr-db 20 --seed N --expect-mod any --expect-rate R2/3 --file-kb 50 --out /tmp/X`. NEVER run two cells (or a cell + a build, or a cell + a rig run) concurrently — concurrency produced phantom failures that cost hours. Single-cell noise is ±25-30%; only multi-seed consistent direction counts; resolving <0.3 kbps config deltas needs 10+ pairs (usually not worth it — prefer structural levers with mechanism-level evidence).
4. **Bisect one variable at a time** (the W5→W5b→W6 timer bisect is the template): when a regression appears, change exactly one thing per rig run, let the run verdict pick the branch.
5. **Two-sided log forensics with adversarial verification:** for any data-loss/stall mystery, reconstruct the timeline from BOTH ends' logs (clocks differ ~5-6s), get the mechanism to byte-exact arithmetic (e.g. the 648-byte seq-collision hole = 9×(456−384)), then have an independent skeptic try to REFUTE it before acting. Mechanisms that survive refutation get fixes; hypotheses that don't get discarded.
6. **Design-doc-then-implement for wire/ARQ changes** (the epoch is the template): full design with failure-mode table first, review the risky rule explicitly (e.g. rx re-anchor: naive re-anchor-to-seq provably recreates the disease — the rebase-flag + ack-silent interregnum has zero fabrication), then implement knob-gated.
7. **Calibrate from your own data, never invent constants:** entry stats live in the calibration doc (48 dial-20 entries: readings μ12.4 σ3.14; fading σ0.129; false-Moderate 18.8%). Single-dial calibration is DEGENERATE for slope — a 2-3 dial sweep is the principled successor. Cost asymmetries justify direction (false-Moderate ≈ 150s crawl vs false-Good ≈ 16s demote → classify up when uncertain).
8. **Honest failure accounting:** window-expiry ≠ corruption; handshake-fail rate ~9% (one-way CONNECT_ACK loss) is its own tracked class; salvage lines with same-epoch = benign dupes.

## 3. Rig runbook (exact)
- Pi5: `ssh -i ~/.ssh/id_pi5 math@192.168.160.163` (hostname pi5tnc; alias does not resolve). Update: `git fetch && git merge --ff-only origin/feat/coherent-window-16 && cmake --build build -j4` (log to /tmp/pi5_buildN.log; ~10 min ARM).
- **Gotchas that mimic dead links:** macOS reboot resets output volume (TX = built-in headphone jack) → set `osascript -e 'set volume output volume 100'`; Pi5 Fe-Pi sink must be 1.0 (`wpctl set-volume 57 1.0`). Diagnose asymmetry via wire `peer_snr` vs local readings. `logs/gui.log` (cwd-relative) is the real trace, NOT stdout. `pkill -f '[u]ltra_gui'` (bracket avoids self-kill).
- Launch (Mac receiver): `(ENV… nohup ./build/ultra_gui --auto-accept &)` from repo root. (Pi5 sender): `DISPLAY=:0 XAUTHORITY=/home/math/.Xauthority ENV… nohup ./build/ultra_gui --auto-connect MAC --connect-delay 8 --auto-send-file ~/bench/r50k.bin --auto-disconnect-after 600 &`. Bench file md5 `bc8c74571d2277ce7f194620ded9319b`; received lands in Mac `~/Downloads/r50k.bin` — md5 EVERY run.
- Monitor greps: entry = first `MODE_CHANGE:.*local_measured` line (reading + fading class + rung); `SALVAGE below-window` count; `stale-epoch` count; MC retries = duplicate MODE_CHANGE receptions; completion = `[FILE] Received .* CRC ok`.
- Current standing knob set (both ends): `ULTRA_CONNECT_AFFINE_BASIS=1 ULTRA_ARQ_MOVE_EPOCH=1 ULTRA_RETX_TROUGH_PACING=1 ULTRA_COLLAPSE_ESCAPE_ROUNDS=2 ULTRA_PROMOTE_EMA_CARRY=1 ULTRA_ENTRY_CAP_R34=1 ULTRA_CONNECT_SNR_POOL=1 ULTRA_WIRE_SNR_FRESH=1` (salvage is default-ON already).

## 4. What landed today (all on the branch; map/CHANGELOG have file:line)
Wide coherent window 16 (default; +36% rig) · SNR pool + defer + wire freshness (#58 inc 3) · fading pooled + entry classification shrink (σ/√N) · calibrated entry basis (readings 8.55-17.55 → sel 19.55 → **R2/3 entry at dial 20** — the user's core requirement) · ratiometric MODE_CHANGE timer saga (full deadline is correct; 4 retries) · fading-aware MC-ACK repeats ×3 (the "5 retx per climb" fix) · promote-EMA-carry · entry-cap R3/4 · trough pacing · below-window FILE salvage (default-ON, 10 field saves) · **move-epoch wire fix** (structural seq-collision cure, knob) · R3/4 crest rung (AWGN 3640) · honest SNR displays (eff + ~dial).

## 5. In-flight / queue (in priority order)
1. **Validation batch V2-V5** (affine + all knobs): PASS = entries classify Good AND land R2/3+ at readings ≥8.55; MC retries ≤2/move. Then report to user.
2. **Descriptor mode switch Phase 1** (`ULTRA_DESCRIPTOR_MODE_SWITCH`, agent implementing per the design doc): commit locally at burst boundary, descriptor announces, epoch guards seqs — kills the control exchange for rate moves. Build+ctest AFTER the batch; sim A/B; rig batch (metric: move dead-air ~0, no adopt failures).
3. **Phase 2** receiver rung command (tone-ACK pad bits 42-43).
4. **Low-dial calibration leg** (MPG@10/12 rig runs) before affine default-ON; ideally a 2-3 dial sweep to fit a true slope.
5. Default-flip candidates when rig-proven: move-epoch, affine basis, pool knobs (then trim the env set).
6. Main push + PR (user's call). The morning after: crest-riding economics (16QAM R3/4 on hardware) — the last stretch to 3000.

## 6. Physics you must not re-litigate
- The data-aided estimator reads EFFECTIVE SNR; it compresses on fading (differential Doppler-EVM floor) — readings 6-19 at dial 20 are CORRECT measurements; calibration maps them, never "fix the estimator".
- The peer cannot ACK until its decode backlog drains → control retry timers must cover the full burst deadline (~18.5s); shorter timers livelock (proven twice).
- Half-duplex: retries key down ON TOP of the ACK in flight. Demote is cheap (1 bad group), climb is expensive (streaks) → bias entries/classification UP.
- 50KB against a 600s window is marginal below ~R1/2 average — window-fails in deep fades are physics, not bugs.

---

## 7. CURRENT EXACT RUNBOOK (2026-07-04 ~11:50 — the state any successor model reruns from)

Build: BOTH ends on origin/feat/coherent-window-16 (Mac local `main` tracks it; push via
`git push origin main:feat/coherent-window-16`). Pi5 update: `ssh -i ~/.ssh/id_pi5
math@192.168.160.163 'cd ~/ProjectUltra && git fetch origin && git merge --ff-only
origin/feat/coherent-window-16 && cmake --build build -j4'` (~10 min ARM; NEVER build the
Mac while a rig run is live).

**THE 12-KNOB STANDING ENV (both ends, identical, all validated today):**
```
ULTRA_R34_FAST_CREST=1 ULTRA_QAM16_R34=1 ULTRA_RX_RATE_CMD=1 \
ULTRA_DESCRIPTOR_MODE_SWITCH=1 ULTRA_CONNECT_AFFINE_BASIS=1 ULTRA_ARQ_MOVE_EPOCH=1 \
ULTRA_RETX_TROUGH_PACING=1 ULTRA_COLLAPSE_ESCAPE_ROUNDS=2 ULTRA_PROMOTE_EMA_CARRY=1 \
ULTRA_ENTRY_CAP_R34=1 ULTRA_CONNECT_SNR_POOL=1 ULTRA_WIRE_SNR_FRESH=1
```
(Wire/semantics-lockstep set: DESCRIPTOR_MODE_SWITCH, RX_RATE_CMD, ARQ_MOVE_EPOCH — both
ends or neither. The rest are sender/receiver-local but run them everywhere. Everything is
default-OFF in code; this env IS the campaign configuration.)

**Mac receiver launch** (repo root; check volume EVERY launch — it drifts):
```
osascript -e 'set volume output volume 100'   # MUST be 100 (GOTCHA: drifts to 63-75)
(ENV… nohup ./build/ultra_gui --auto-accept --log-categories operator,modem,sync \
  > /tmp/campaign_3000/fN_mac_stdout.log 2>&1 &)
```
**Pi5 sender launch** (pkill in a SEPARATE ssh first; </dev/null is REQUIRED or the
launch dies; never pkill+launch in one ssh session — exit 255):
```
ssh -i ~/.ssh/id_pi5 math@192.168.160.163 'pkill -x ultra_gui'
ssh -i ~/.ssh/id_pi5 math@192.168.160.163 'cd ~/ProjectUltra && DISPLAY=:0 \
  XAUTHORITY=/home/math/.Xauthority ENV… nohup ./build/ultra_gui --auto-connect MAC \
  --connect-delay 8 --auto-send-file ~/bench/r50k.bin --auto-disconnect-after 900 \
  --log-categories operator,modem,sync </dev/null >/tmp/fN_pi5_stdout.log 2>&1 & \
  sleep 3; pgrep -x ultra_gui'
```
**Early-check monitor** (line-buffered end-to-end; NEVER pipe through `cut` — it buffers
and the monitor goes silent):
```
tail -F logs/gui.log | grep --line-buffered -E \
 "MODE_CHANGE:.*local_measured|DESC-SWITCH adopt|delivered as unit: 0/|ESCAPE-drop.*via|WAITING-REBASE|exhausted unarmed|Received .*CRC ok|Disconnected|MC-DPSK DBPSK R1/4"
```
**Per-completion loop:** md5 ~/Downloads/r50k.bin == bc8c74571d2277ce7f194620ded9319b →
cp logs/gui.log /tmp/campaign_3000/rig_FN_mac.log → mv the received file → append verdict
to /tmp/campaign_3000/RESULTS.md → pkill both ends → relaunch. **Kill-don't-wait:** an
entry line showing MC-DPSK or R1/4 at dial 20 (reading <7 with fading >0.6 in the window)
is a fade-snapshot miss (~2/30 tonight) — kill both ends, relaunch, ledger it.
Every ~4-5 runs: append to docs/CAMPAIGN_LEDGER_2026_07_03.md, commit docs, push.

**Success signatures (healthy run):** entry `MODE_CHANGE:.*local_measured` → QPSK R2/3
Good (29/29 tonight, readings 6.2-16.1); descriptor climbs (`adopt QPSK R3/4` ~70-120s,
`adopt 16QAM R2/3` ~90-230s); first group after ANY adopt delivers N/M>0 within ~7s
(the mode-hop cursor fix — a silent no-delivery stall here is a REGRESSION); craters
(`delivered as unit: 0/N`) resolve via an adopt within ~4s (receiver rung command);
`WAITING-REBASE` voices are SAVES not errors; `exhausted unarmed` backstop NACKs are
SAVES. Failure smells: >15s with no adopt after a crater; any `TX: Handshake mode` after
the CONNECT_ACK; MC-DPSK frames mid-session; the same MODE_CHANGE line repeating.

**Scoreboard at handoff-refresh:** records 2.50 (F3) / 2.17 (F5) / 2.12 (F13) / 2.00
(F20, clean-drive); 73/73 lifetime byte-exact; entries 29/29 R2/3 since the affine fix.
Clean-drive era (post-ALC-guard 7b0953c): 1.77, 2.00 — both above the prior chop band.

**Open watch items:** (a) whether 16QAM R3/4 sustains at healthy drive or the Pi5 cheap
card's distortion floor caps it (~3-4 R3/4 probes/run, most crater; graded landings keep
probes ~net-positive — if a calm-epoch run still can't hold R3/4, the last stretch to
3000 is EQUIPMENT: a cleaner USB card); (b) fade-snapshot entries ~2/30 (#58 fix =
fade-averaged connect SNR, queued); (c) queued structural: descriptor-armed accumulation
(HEADNULL), predicted-anchor timed window, fallback hygiene (all in the 16QAM-verdict
fix list, KNOWN_BUGS cross-refs). Main-merge PR remains the user's call.

### §7.1 ENV AMENDMENT (2026-07-04 ~12:10, F23+): ALC frozen, crest rung parked
F19-F22 forensics (full chain in RESULTS.md/ledger): the software-ALC has NO EQUILIBRIUM
on the Pi5 cheap card — LOW readings while delivering keep walking tx_drive up (0.50 ->
the 0.85 cap every long run); the only DOWN trigger is an RX clip signature that TX
compression never produces; 16QAM R3/4 craters cluster above ~0.70 drive (0/9 even at
fading 0.11-0.21). **F23+ env = the §7 12-knob set MINUS ULTRA_QAM16_R34 and
ULTRA_R34_FAST_CREST, PLUS ULTRA_SOFTWARE_ALC=0** (drive fixed at baseline — the config
that set D2's 2.43). This measures the clean 16QAM-R2/3 ceiling. QUEUED REDESIGN:
extremum-seeking ALC (quality-derived DOWN, perturb-and-observe) — until then the ALC
stays OFF on this hardware. R3/4 on this rig is an EQUIPMENT question (cleaner USB card)
— re-arm both crest knobs after any card swap or at fixed drive <=0.65 for an A/B.

### §7.2 ENV AMENDMENT-2 (2026-07-04 ~12:25, F24 verdict — SUPERSEDES §7.1): capped ALC
F24 CONFIRMED the knee theory: with `ULTRA_ALC_MAX_DRIVE=0.70` (new knob, 5b55c5a) the
R3/4 hop at fading 0.09 ran SIX groups (four consecutive 9/9) vs ~1-in-4 survival at
walked-up drive — the card is fine below ~0.70; the 0.85 code ceiling was the R3/4
killer. **STANDING CONFIG (F24+): the §7 12-knob set (crest knobs INCLUDED again) PLUS
ULTRA_ALC_MAX_DRIVE=0.70. Drop the §7.1 ULTRA_SOFTWARE_ALC=0.** F24: 1.89 through a deep
trough, 5th best. Extremum-seeking ALC redesign remains queued (the cap is the
per-hardware pragmatic form).
GOTCHA (F25 post-mortem): NEVER combine the Mac GUI launch with the (lingering) Pi5 ssh
in ONE shell command — a runner timeout SIGKILLs the whole process group and reaps the
backgrounded GUI (nohup shields SIGHUP, not a group kill; the GUI died at exactly the
60 s timeout mark, no crash report). One command per launch, always.

### §7.3 STANDING CONFIG UPDATE (2026-07-04 ~18:10, Opus): add the R3/4 CALM-GATE
F3's 2.50 = a zero-crater 16QAM R2/3 cruise that never probed R3/4 (pre-crest-rung). The
crest rung (24c6f48/f713064) probes R3/4 on backward-looking `quality`; in chop each probe
craters + re-climbs = tax. A/B (afternoon chop): crest-blind median 1.25 < crest-OFF
{1.43,1.62,0.44}=1.43 ≈ calm-gate 1.49 (F56, 0 probes fired — gate suppressed chop-probing
as designed). **STANDING CONFIG (F56+): §7.2 knobs PLUS ULTRA_R34_CALM_FADING=0.30** (gate
R3/4 walk on coherence-adjusted fading <= 0.30; commit a4ffee1, default-OFF/byte-identical
without the knob). This recovers F3's chop cruise while keeping R3/4 upside in genuine calm
(fading < 0.30). To replicate 2.50 needs BOTH this config AND a calm channel window (~90s
zero-crater) — the latter is pure IONOS sampling (stationary; F3-class draws recur).

### §7.4 STANDING CONFIG UPDATE (2026-07-05 ~00:00, Opus): ACK-LISTEN TONE-LOCK GUARD — all-time record 2.62
The "self-echo" story is CORRECTED (see KNOWN_BUGS BUG-ACKLISTEN-TONE-FALSELOCK + CHANGELOG
2026-07-05): the sender's warm data-sync detector was S&C-false-locking on the PEER'S TONE
ACK (sc~0.9/mf~0.1; self-echo disproven — OTASim mixer excludes self, solo-station control
heard nothing, rig capture stopped during TX). Fix 7752d60 suppresses both warm data-sync
acceptance paths while the tone monitor is armed (dual-chirp stays live). Rig F75: first ACK
9.9s (was 28.5s), 9.56s metronomic cadence, 0 resends, 0 craters, **2.62 kbps RECORD**.
**STANDING CONFIG (F75+): the §7.3 set PLUS ULTRA_ACKLISTEN_SUPPRESS_OFDM=1** (both ends;
sender-effective). ULTRA_ENTRY_QAM16_SNR exists (20f6006) but stays OFF — cold 16QAM entry
is marginal (F73: quality 0.35, ladder collapse); the QPSK-first climb warms the equalizer.
Queued: multi-seed F76+ validation of the guard; bfe5676 "SELF-ECHO" log-line rename.

### §7.5 OVERNIGHT VALIDATION + DEFAULT FLIPS (2026-07-05 ~02:00, Opus — user mandate: "fix, 10 runs, verify every retx, commit, maybe default-on")
BUG-POSTTX-ACK-MISS root-caused + fixed (6340f51): the tone monitor's tail-window sweep
never scanned audio deeper than ~520ms into a single feedAudio append (post-TX capture-
resume backlog) — the peer's fast ACK landed there: captured, fed, never scanned (~19s
RTO ×2/run in F76/F77). Fix = gapless armed sweep (high-water anchored, induction-proven)
+ permanent monitor forensics (arm/detect/expiry INFO lines with max_chunk classification).
**10-RUN BATCH (F78-F87, guard+gapless, rough 00:30-01:50 epoch): 10/10 delivered
md5-EXACT (135/135 lifetime), median 1.24 (draws 0.93-1.61), ~280 ack exchanges 0 misses,
0 expired-undetected windows, EVERY RTO forensically classified genuine fade (forward-path
group losses = the open HEADNULL class, quantified at up to ~200s/run — the next lever).**
**DEFAULTS FLIPPED (both ends, =0 opts out): ULTRA_ACKLISTEN_SUPPRESS_OFDM,
ULTRA_ACK_MONITOR_GAPLESS.** The 12-knob standing set remains env-gated — flipping 14
defaults at once overnight would be un-bisectable; candidates for the next flips (weeks of
rig proof each): ARQ_MOVE_EPOCH, CONNECT_AFFINE_BASIS, the pool knobs. STANDING ENV
(F88+) = the §7.3 set (the two flipped knobs may be dropped from the env at will).

### §7.6 THE 2026-07-05 MORNING ARC (Opus, user live-catching): five more root causes, all shipped
User watched runs live and caught, in order: (1) collapse-escape cascade to R1/4 through a
20s null while deliveries ran q=0.99 → **ULTRA_ESCAPE_EPISODE_CAP=1** (faa6cc3) +
**ULTRA_TROUGH_AMNESTY=1** (4d9195d, restore pre-trough rung on first progress); re-climb
penalty cut (**ULTRA_QAM16_RECLIMB_COOLDOWN=1 ULTRA_QAM16_CLIMB_STREAK=1
ULTRA_RATE_CLIMB_STREAK=1**). (2) "bursts decent, receiver not acking" → the modulation-blind
pre-LDPC LLR guard shredding real 16QAM head frames (mean 0.94/near_zero 18.5%) →
**ULTRA_LLR_REJECT_SHAPE=1** (8230fea, shape-based rejection). (3) The HEADNULL class →
**ULTRA_DESC_ARMED_ACCUM=1** late-join accumulation (9465d00, design doc
DESC_ARMED_ACCUMULATION_DESIGN_2026_07_05.md): first firings recovered a WHOLE 5-frame sim
group and 3/4 on the rig. (4) F98 cascade: one 4/5 partial wrote 9.4 dB over a 22 dB channel
→ 1.7s ACK → phantom demote → 54s blackout → **ULTRA_ACK_SNR_MEDIAN=1** (1535ef7,
median-of-5 staircase) + the RANK-1 forensic (workflow wmx7okiz2): the tone-ACK TX
echo-clear reset DISARMED the armed full-anchor wait every ACK →
**ULTRA_PRESERVE_ANCHOR_WAIT=1** (35a8b7f). Entry-16QAM (ULTRA_ENTRY_QAM16_SNR, 20f6006)
tested twice, cold-decode marginal — keep OFF.
**STANDING ENV (F99+) = §7.3 set + ULTRA_ESCAPE_EPISODE_CAP=1 ULTRA_TROUGH_AMNESTY=1
ULTRA_RATE_CLIMB_STREAK=1 ULTRA_QAM16_RECLIMB_COOLDOWN=1 ULTRA_QAM16_CLIMB_STREAK=1
ULTRA_LLR_REJECT_SHAPE=1 ULTRA_DESC_ARMED_ACCUM=1 ULTRA_ACK_SNR_MEDIAN=1
ULTRA_PRESERVE_ANCHOR_WAIT=1** (guard+gapless are defaults since e2f6096).
F99 (all on): 1.52 EXACT (144/144), all losses classified — residual = ~2/run TONE-FADE
(ack captured at healthy rms, gaplessly scanned, genuinely undecodable; confirmed F76/F77/
F99). **NEXT LEVER (designed, NOT built): decorrelated ACK repeat-if-silent** — copy 2 at
+~1.5s (≥Tc) gated on CCA-quiet. ⚠ A naive timed repeat is FORBIDDEN: the sender's next
burst turns around ~1-2s after copy 1, so an unconditional +1.5s copy would blank the
incoming anchor with our own TX and reintroduce the head-null class. If copy 1 survived,
the arriving burst self-gates the repeat; if it died, the channel is silent and copy 2 is
safe. Needs the CCA query wired on the RX ack path (production isChannelBusy is unwired —
see memory). Default-flip candidates now rig-proven for the next batch: DESC_ARMED_ACCUM,
PRESERVE_ANCHOR_WAIT, ACK_SNR_MEDIAN, LLR_REJECT_SHAPE.

### §7.7 NEXT BUILD (designed 2026-07-05 ~13:30): 8PSK revival — the cheap-card-proof rung
cw16 SHIPPED+PROVEN (sim 2670 seed42 = +35-55%; IONOS 5-run median 1.34 vs cw8 1.02 chop
= +31%). NEXT: revive coherent 8PSK. The demapper EXISTS (soft_demap.hpp demap8PSK, same
Gray geometry as proven D8PSK); the killer is BUG-8PSK-001 — the dd_qam16_* decision-
directed tracker slices 16QAM amplitude rings and corrupts the 8PSK estimate on fading
(THE canonical CLAUDE.md adaptivity case). CASE: (1) constant envelope = immune to the
Pi5 cheap-card compression that craters 16QAM above drive 0.70 (the ALC saga's root);
(2) +3.6dB-over-QPSK margin = the sweet spot at broadband ~24dB w/ swings; 8PSK R2/3
(~3.0k net) slots between 16QAM R1/2 (2.45) and R2/3 (3.3) at BETTER fade robustness
(|H|-magnitude errors don't move phase boundaries); (3) cw12 normalizes to 1272ms (cap
already 16). WORK: make the DD tracker constellation-generic (phase-only slicing for
8PSK is SIMPLER than 16QAM), ladder rungs + floor measurement, cw12 rule. EQUALIZER-CORE
surgery — open it on a FRESH session, INVARIANTS first. Also queued: fade-predictive
rate hints (path-A step 3), HARQ combine-then-standalone-retry (step 4).
