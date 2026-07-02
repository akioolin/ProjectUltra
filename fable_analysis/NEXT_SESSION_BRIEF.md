# Kickoff brief for follow-up sessions (paste this to start)

> You are continuing the 3000-bps Good@20 campaign. The CURRENT analysis is
> `fable_analysis/09_WHY_STUCK_AT_2000_2026_07_01.md` — read it FIRST (docs 00-08 are
> the June historical record; several of their numbers are corrected in 09).
> 1. Run the CLAUDE.md fresh-session protocol (AI_COLLABORATION, PROJECT_GOALS,
>    KNOWN_BUGS, git log).
> 2. Work ONE item from 09 §5's ranked plan per session. As of 2026-07-01 the order is:
>    (1) #58 connect-time coin-flip fix (reliability first — rig stock at MPG@20 was a
>        zero-delivery coin-flip: DBPSK R1/4 ~94 bps nominal / 0 bytes vs QPSK R2/3 1.5k),
>    (2) BUG-ACK-STAIRCASE-FADE-BIN re-bin (+4-5%, trivially safe, validate ACK FER at
>        the new edge on Watterson first),
>    (3) BUG-BURST-HEADNULL-DROP counter, then recovery,
>    (3b) BUG-QAM16-RIG-ANCHOR-COLLAPSE diagnosis — GATES all hardware 16QAM work
>        (one instrumented paired rig session: per-burst normalization factor +
>        chirp-segment RMS/corr decides PAPR-vs-backlog),
>    (4) HARQ provisional keys — BUILT but flipped DEFAULT-OFF same evening (rig
>        poison-loop: real fade LLRs are confidently-wrong, combine has no standalone
>        fallback; sim's 0/212-clean was a fidelity artifact). NEXT INCREMENT: the
>        combine-then-fail STANDALONE RETRY in decodeFixedFrame (fresh-only LLR pass on
>        combined-and-failed CWs) — makes ALL combining harm-free by construction, then
>        re-evaluate the default with a rig A/B,
>    (5) cw16 frames for 16QAM (`kMaxFixedFrameCodewords` 8→16; 5-frame groups — 6 needs item 6; HW pends 3b),
>    (6) budget-aware anchor-skip (+6-8% QPSK),
>    (7) joint RTO/group-timeout tightening (floor 14-17 s — do NOT go lower),
>    (8) margin-aware 16QAM laddering.
> 3. Hard rules unchanged: every measured claim via `tools/gui_qso_scenario.sh`,
>    sequential runs, ≥3 paired seeds (±25% single-run noise), check 09 §6's dead-end
>    list BEFORE proposing anything (notably: `ULTRA_BURST_GROUP_FRAMES` is a verified
>    NO-OP on the file path; rate/constellation climbing without damage work is a tie;
>    predictive channel-class gating is disproved on rig).
> 4. Update CHANGELOG / KNOWN_BUGS / MODEM_INFRASTRUCTURE_MAP in the same change.

## Success criteria (unchanged)

Adaptive ladder, Good@20, 5/5 seeds, 100 KB, sequential: delivered ≥ ~2 800 by GOODPUT_BPS
(≥ 3 086 by mid-transfer slope), no link-deaths, damage ≤ ~10%, plus a real-CFO/ppm
spot-check — AND the rig at MPG@20 must deliver the same rung reliably out-of-box
(no coin-flip, no acquisition collapse).
