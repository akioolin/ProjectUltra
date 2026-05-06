# Session 2026-05-05 night — status report for morning review

## TL;DR

- ✅ **Rate-adapter hysteresis (BUG-RATE-001) shipped on branch
  `experimental/rate-adapter-hardening`** — committed `d8aa2ce`,
  hardware-validated 5/5 seeds, worst-case throughput on the panic
  seed improved 444 → 684 bps (+54 %), no panic-downshift fires.
  Ready to merge to main.
- ❌ **Backlog #5 (per-carrier bit loading) abandoned for tonight**
  after three rounds of failed attempts. Per AI_COLLABORATION
  autonomous-mode rules ("4+ rounds without convergence ⇒ stop and
  write a status report"), I'm reporting back rather than burning
  more rounds. Branch `experimental/per-carrier-bit-loading` is now
  reset to the rate-adapter tip; the failed phase-2a work is
  preserved at tag
  `experimental/per-carrier-attempt-1-failed-2026-05-05` for
  forensic review.

## What I did tonight (in order)

### 1. Bug filed: BUG-RATE-001
Documented the panic-downshift behavior on short Watterson-Good
SNR=15 transfers — 1/5 seeds at 444 bps with R1/2→R1/4 panic.
Filed under `docs/KNOWN_BUGS.md`.

### 2. Two parallel branches off main `e5e1901`
- `experimental/rate-adapter-hardening` (backlog #4 + BUG-RATE-001)
- `experimental/per-carrier-bit-loading` (backlog #5 phase-1 → 2a)

Worktrees in `../ProjectUltra-rate-adapter` and
`../ProjectUltra-per-carrier`.

### 3. Rate-adapter — Codex round 1 → ✅ ship
- Brief: `/tmp/rate_adapter_findings.md`
- Codex implemented hysteresis (2 consecutive eval windows of retry
  pressure required) + lockout reduction (15 s → 5 s).
- ctest: 35/35 pass (incl. new regression).
- Hardware: 5/5 seeds PASS at 5 KB Good SNR=15. Worst seed 444 →
  684 bps (+54 %), no panic. Median unchanged (1,440 bps).
- Committed `d8aa2ce` with full CHANGELOG entry, `BUG-RATE-001`
  marked fixed in `KNOWN_BUGS.md`.

### 4. Per-carrier — three rounds, all failed
After each round: hardware test on Mac↔Pi5, 20 KB Watterson Good
SNR=15, 5 seeds. Compared to baseline 1,631 bps (median), 0 retx.

**Round 1 — RX-only LLR scaling, aggressive 0.1× floor**
- Multiplied LLRs by `[0.1 .. 1.0]` based on `|H_k| / median(|H|)`
  before LDPC decode.
- ctest pass.
- Hardware: 4/5 seeds at 1,628–1,706 bps (+0 to +5 %).
  **1/5 seeds at 1,054 bps (−35 %)** with 16 retx.

**Round 2 — same approach, gentler 0.4× floor**
- Re-tuned floor to 0.4×, threshold to 0.5× / 0.25×.
- ctest pass.
- Hardware: 1/5 seeds at 1,703 bps (+5 %), 1/5 flat. **2/5 seeds
  catastrophic (469 bps and 686 bps), 1/5 outright TEST FAILED.**
- Diagnosis: with floor at 0.4×, partially-bad LLRs are kept at
  40 % strength → LDPC over-trusts wrong bits → decode fails.
  Lowering the floor (round 1) helps in some channels but
  introduces near-erasures that hurt others. The whole approach is
  a heuristic on top of an already-calibrated soft demap.

**Round 3 — Principled redesign: TX-aware closed-loop carrier mask**
- Wrote a PhD-perspective brief that identified round 1/2 as
  double-counting fade (equalizer + soft-demap already weight by
  `|H_k|² / σ²_k`) and the median-of-|H| reference as physically
  meaningless.
- Codex implemented: wire-protocol extension (MODE_CHANGE 20→28 B,
  1→2 CW; 8-byte carrier mask field), per-carrier
  `γ_k = |Ĥ_k|² / σ̂²_k` IIR estimator with hysteretic recommendation,
  TX-side carrier nulling, RX-side LLR=0 erasure insertion.
  +953 lines across 29 files.
- ctest pass.
- AWGN check (after merging in rate-adapter fix): 2,288 bps,
  0 retx — **no AWGN regression**, behavior identical to baseline
  when no mask is recommended.
- Hardware Good SNR=15 5-seed: **3/5 catastrophic (455–1,057 bps,
  72–184 retx).** Worse than rounds 1 and 2.

## Why round 3 failed (architectural insight)

This is the important part to read.

The closed-loop architecture cannot work over Watterson Good
multipath because **the control-loop bandwidth is slower than the
channel's fade rate**:

| Quantity | Approximate value |
|----------|------------------:|
| MODE_CHANGE round-trip latency (28 B, 2 CW + ACK + boundary) | 5–15 s |
| Watterson Good Doppler spread | 0.1 Hz (coherence ~10 s) |
| Per-carrier γ_k swing on Good fading | ±5 dB on sub-second timescales |
| Closed-loop bandwidth from MODE_CHANGE pacing | ≈ 0.1 Hz |

The mask flaps every 5–15 s because each MODE_CHANGE round-trip
is itself comparable to the channel's coherence time. By the time
the mask is applied, it's already stale. During each transition,
TX has the new mask while RX still applies the old one (or vice
versa) → bit-position alignment breaks → LDPC fails → ARQ pile-up
→ rate-adapter bumps to R1/4 → mask oscillation continues.

Seed-1 log (`/tmp/ultra_hw_20260505_224724/A.log`) shows the
classic flap: ALPHA sets `mask=0xFFE01FFFFFFFF87F`, reverts to
all-ones 5 s later, BRAVO independently sets a different
`mask=0xFFFFFFFFFFF87FFF`, ALPHA panic-downshifts at backlog=97
frames, and the mask continues bouncing for the rest of the
session.

**The architectural answer is one of:**

1. **Frame-local mask, no signaling.** Both sides derive the mask
   from each frame's own LTS preamble using the same documented
   rule. No round-trip. RX of frame N sees the same channel
   instant TX used to choose the mask. Requires reciprocity
   assumption — fine for half-duplex HF where the reverse channel
   was just measured. This is the right phase-2a redesign, but
   it's a different change from what we just tried.
2. **Off-loop carrier nulling at TX only, RX inserts erasures
   based on its own per-carrier SNR estimate.** No agreement
   needed because TX nulling produces literal zero-amplitude on
   masked carriers; RX detects "no signal" empirically per carrier
   and inserts 0-LLR erasures locally. Asymmetric: TX picks mask
   from its own measurement of the *previous reverse-direction*
   frame.
3. **Defer #5 until #4 (per-burst rate adaptation) is in place
   first.** A finer-grained whole-frame rate adaptation might
   close enough of the fading gap that per-carrier work has a
   smaller payoff to chase.

Option 1 (frame-local, no signaling) is the principled answer and
matches how commercial-grade DSP HF modems implement bit-loading
in practice. It would require:
- A tested reciprocity assumption (does our half-duplex setup
  satisfy it within tolerance?).
- Bit-exact identical mask-derivation rules on both sides.
- An LDPC-side awareness of per-frame-variable puncturing pattern.

This is non-trivial. Probably 2-3 weeks of careful design + tests,
not an overnight Codex round.

## What I did NOT change tonight

- `main` branch unchanged, no push to origin.
- README, top-level CLAUDE.md numbers unchanged (the rate-adapter
  fix doesn't change the auto-rate ladder; the per-carrier work
  was rolled back).
- The README `End-to-end measured` table still has the empty 5 KB
  Good cell that started this whole investigation. Once
  rate-adapter merges to main, that table can be filled with the
  validated 5-seed numbers (1,440 bps median, 911 bps worst-case
  pre-fix → 1,440 median, 684 bps worst-case post-fix).

## Files of interest

| Path | Purpose |
|------|---------|
| `experimental/rate-adapter-hardening` (branch, commit `d8aa2ce`) | Ready to merge |
| `experimental/per-carrier-attempt-1-failed-2026-05-05` (tag) | Forensic only |
| `docs/CHANGELOG.md` (rate-adapter branch) | Full BUG-RATE-001 writeup |
| `docs/KNOWN_BUGS.md` (rate-adapter branch) | BUG-RATE-001 marked fixed |
| `/tmp/rate_adapter_findings.md` | Round-1 brief that worked |
| `/tmp/per_carrier_findings.md` | Round-1 brief (LLR scaling) |
| `/tmp/per_carrier_round2_findings.md` | Round-2 brief (LLR scaling tuned) |
| `/tmp/carrier_mask_findings.md` | Round-3 brief (TX-aware mask) |
| `/tmp/codex_*.log` | Each Codex round's full transcript |
| `/tmp/phase2a_good_5seeds.txt` | Round-3 hardware results |
| `/tmp/per_carrier_r2_5seeds.txt` | Round-2 hardware results |
| `/tmp/per_carrier_5seeds.txt` | Round-1 hardware results |

## Suggested morning agenda

1. Review `experimental/rate-adapter-hardening` commit `d8aa2ce`.
   If happy, fast-forward `main` and push.
2. Decide direction for backlog #5: option 1 (frame-local mask) is
   probably worth attempting *after* understanding whether
   half-duplex reciprocity actually holds on our hardware harness
   (a separate measurement task).
3. Update `docs/MODEM_IMPROVEMENT_BACKLOG.md` item #5 with the
   architectural insight from this session — the "closed-loop via
   MODE_CHANGE" approach should be marked as not viable so a
   future agent doesn't retry it.

— end —
