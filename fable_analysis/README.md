# fable_analysis/ — deep audit of the path to 3086 bps on Good@20

Produced 2026-06-12 by Claude Fable 5 at the user's request: *"check the core
modulation and speed throughput and see what was missed [vs the ~3086 bps market
leader at 3 kHz, Good fading, 20 dB], and save the findings so a smaller model can
work from them."*

> **⚠ 2026-07-01 UPDATE — READ `09_WHY_STUCK_AT_2000_2026_07_01.md` FIRST.** The
> re-audit at HEAD supersedes the 06-12 roadmap's Phase-2a/2b framing: QPSK is measured
> AT its (corrected) ceiling and closed; 16QAM R2/3 already flies 8-frame bursts and its
> gap is a fade-retx tax, not caps; three new defects are filed (head-null silent drop,
> ACK-staircase fade-bin, RTO double-count); the rig's out-of-box mode pick at MPG@20 is
> a zero-delivery coin-flip (~16× nominal) (#58). Docs 00-08 remain the June historical record — several of their
> numbers (ceilings, group sizes, "16QAM beats QPSK") are corrected in 09.

## Read in this order

| Doc | What it settles |
|---|---|
| **`09_WHY_STUCK_AT_2000_2026_07_01.md`** | **CURRENT: the verified ceiling model, the sync/fading verdict, 3 new defects, the corrected path to 3000** |
| `00_EXECUTIVE_SUMMARY.md` | The headline (stale 16QAM verdict — it passes now), the ranked "what was missed" list, the path |
| `01_GAP_DECOMPOSITION_AND_RUNG_MATH.md` | Air-true numerology, raw rung table, why QPSK is excluded, route arithmetic, benchmark fairness (0.61 dB, small-file penalty) |
| `02_LLR_CALIBRATION_THE_MISSED_FIX.md` | The missing ε²_H LLR term (structural fact) + live A/B that demoted it to a margins fix; the honest counter-evidence trail |
| `03_CHANNEL_TRACKING_AND_PHASE_BUDGET.md` | Estimator pipeline as-implemented, Good@20 phase budget vs modulation margins, estimator upgrade ranking, data-aided-genie desync root cause + fix |
| `04_AIRTIME_EFFICIENCY_LEDGER.md` | Where 38% of every cycle goes; the levers with adversarial corrections (both big ones are builds, not knobs); explicit non-levers |
| `05_RATE_LADDER_AND_ADAPTATION_GAPS.md` | No modulation rungs exist; R5/6 pilot bug; classifier gap; WIP sticky-ceiling risk; harness watchdog trap |
| `06_ROADMAP_TO_3086.md` | Phased plan with verification gates and arithmetic sanity table |
| `07_VERIFICATION_RUNS.md` | This session's live GUI-gate runs (the 16QAM/8PSK re-anchor) + reproduction protocol |
| `08_STALE_DOCS_AND_BUGS_REGISTER.md` | Every stale claim and new bug-smell found, with file:line — feed into KNOWN_BUGS/CHANGELOG/the infrastructure map |

## Methodology (and the quality bar for follow-up work)

1. **11-agent parallel survey** over OFDM numerology, LLR/demap math, phase tracking,
   8PSK status, ladder + WIP diff, airtime budget, frame overhead, channel estimation,
   docs history (anchors + dead-end register), goodput accounting, channel-model/SNR
   parity. Everything file:line-cited; comments/docs treated as claims to verify.
2. **8-agent adversarial verification** of the load-bearing claims before they entered
   these docs. Two top-3 levers were materially corrected by this pass (04) and the
   dead-end distinction for the LLR proposal was both confirmed AND counter-evidenced
   (02 §3). Do the same for anything you build from this folder.
3. **Live experiments on the faithful gate** (sequential `gui_qso_scenario.sh` runs) —
   which overturned the campaign's central assumption same-day (07).

**Standing rules that bit previous sessions and were re-confirmed here:**
- Re-anchor any measured wall after intervening fixes; diagnoses decay (the May-29
  16QAM verdict silently expired within ~10 days).
- Never benchmark parallel/loaded (wall-clock pacing artifact); never trust the 21 KB
  gate for steady-state numbers (use 100 KB + mid-transfer slope, 01 §1).
- 5-seed minimum {42,43,44,7,2} for any rung claim; seed-42-only numbers in 07 are
  marked as such.
- Apply CLAUDE.md's four-tier perspective stack and hard physical guards (half-duplex,
  PA duty, Shannon/rung ceilings, coherence, no shared timebase) to every change.

## One-paragraph state of the world (2026-06-12, post-sweeps)

The modem's best Good@20 today is 1290-1910 bps (adaptive QPSK, 94-96% of its own
protocol ceiling — the structure, not the receiver, caps it). The May-29 "16QAM
undecodable" wall is GONE (16QAM R1/2: 1860 bps, zero CW fails — also at its protocol
ceiling), but the live rung sweeps showed every rung with raw headroom (8PSK R3/4,
16QAM R2/3, 16QAM R3/4) is damage-bound by Good's ~23% instantaneous null erasure
against 25-33% FEC parity — a reliability cliff between R1/2 and R2/3. So the road to
3 086 is two co-equal workstreams: the airtime levers (04; raise the ~2 030 ceiling)
AND the dense-rung margins work (02/03; make one ≥R2/3 rung clean), plus ladder
modulation rungs to select it (05). Together: ~3 430-4 170 ceiling, comfortably above
the leader's 3 086, within the channel's 3 764 genie envelope.
