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
