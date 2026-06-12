# Kickoff brief for follow-up sessions (paste this to start)

> You are continuing the 3086-bps Good@20 campaign from the 2026-06-12 Fable audit.
> 1. Run the CLAUDE.md fresh-session protocol (AI_COLLABORATION, PROJECT_GOALS,
>    KNOWN_BUGS, git log).
> 2. Read `fable_analysis/README.md`, then `00_EXECUTIVE_SUMMARY.md`, then ONLY the
>    doc for the phase below. Treat every claim in the folder as re-verifiable
>    (file:line citations are provided) but do not re-derive settled analysis.
> 3. Work EXACTLY ONE phase this session: **Phase ___** from `06_ROADMAP_TO_3086.md`.
> 4. Hard rules: every measured claim goes through `tools/gui_qso_scenario.sh`,
>    sequential runs only, 5 seeds {42,43,44,7,2} for any rung/anchor claim; check
>    the dead-end register (07/04 §3 + the docs-history table) before proposing
>    anything; do NOT enable `ULTRA_LLR_NOISE_EMP_FLOOR` (net-negative + segfault,
>    08 §0); update CHANGELOG/KNOWN_BUGS/MODEM_INFRASTRUCTURE_MAP in the same change.
> 5. Before building on ANY measured wall older than a week, re-run its cell first
>    (the May-29 16QAM verdict silently expired — 02's lesson box).

## Phase order and session-sized chunks

- **Phase 0a:** 5-seed sweeps: 16QAM R1/2, 8PSK R3/4 (and Good@18 brackets). Pure
  measurement; updates 07's table.
- **Phase 0b:** R2/3 anomaly A/B (`ULTRA_R23_PILOT_SPACING=8`, 3 seeds each way) +
  bisect which commit fixed 16QAM (checkout around be0bbce/c384b6a, re-run the 07
  run-2 cell); rewrite the stale 16QAM diagnosis doc.
- **Phase 0c:** one-line fixes + gates: R5/6 pilot case, phantom re-anchor removal
  (group 6), sticky-ceiling reset, EMP_FLOOR ASAN.
- **Phase 1:** ladder (mod,rate) rungs (05 + 06 Phase 1) — harness watchdog option
  FIRST, then kCoherentLadder anchors, then RateController generalization.
- **Phase 2a:** one lever per session (04): short-anchor descriptor; 13.33 ms ACK
  rung (both ends + FER cells); frame_mask/window widen.
- **Phase 2b:** margins work (06 Phase 2b; 02 §5 production form; 03 §4 genie fix
  first — it is the oracle). Iterate with falsifiers; expect multiple sessions.

## Success criteria (the campaign is done when)

Adaptive ladder, Good@20, 5/5 seeds, 100 KB file, sequential: delivered goodput
≥ 3 086 bps by mid-transfer slope (and ≥ ~2 800 by the as-is GOODPUT_BPS), no
link-deaths, damage ≤ ~10%, and a spot-check run with real CFO/ppm impairments
documented (or the gap explicitly caveated).
