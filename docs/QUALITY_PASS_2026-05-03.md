# Quality pass — 2026-05-03 morning

Goal: improve test coverage, fix code-quality issues from a Codex review,
and harden recently-shipped TNC + HARQ code.

## What shipped (5 commits)

| Commit | What |
|---|---|
| `aee927b` | 7 new tests + dropped 3 unused includes |
| `86eea04` | Reverted redundant tick-side BUFFER staging poll |
| `3a6943a` | 8 Codex-review fixes (safety, tests, comments) |
| `7f365f1` | Reject non-AWGN inject-channel + archive Codex review |

## Coverage delta on touched files

| File | Functions | Lines | Branches |
|---|---:|---:|---:|
| `src/fec/soft_combine.cpp` | 93.33% → **100.00%** | 84.14% → **93.79%** | 69.57% → **84.78%** |
| `src/tnc/tnc_bridge.cpp` | 46.27% → **53.73%** | 62.36% → **71.82%** | 66.38% → **70.83%** |

Project-wide gate stayed above thresholds:
- Functions 65.22% → 65.64%
- Lines 60.37% → 60.65%
- Branches 48.46% → 48.58%

## Test counts

| Suite | Before | After | Δ |
|---|---:|---:|---:|
| `test_soft_combine` | 10 | 16 | +6 |
| `test_tnc_session` | 98 | 100 | +2 |
| `test_tnc_bridge` | 17 | 23 | +6 |
| **Total new tests** | | | **+14** |

`ctest --output-on-failure -j 4` still 34/34 passing.

## Codex findings addressed (8 of 22)

Critical:
- ~~#1 setConnectionChangedCallback test missing start()~~ — already fixed in earlier iteration

Medium (autonomous-safe):
- #3 ultra_tnc: skip config loading on --help / --list-audio-devices
- #6 inject_channel non-AWGN values now rejected loudly
- #8 postPTT() callbacks no longer fire under ptt_mutex_ (deadlock window closed)
- #12 RX corrupt-deflate test added
- #14 cmdStats null-guards cmd_emit_
- #18 carrier_count_hash comment now matches implementation
- #19 saturation test asserts exact implementation cap
- #20 SoftCombineBuffer lifecycle edges (4 new tests)

Cosmetic:
- #22 trailing whitespace stripped from PAT_VARA_AUDIT.md

## Codex findings deferred (need broader judgment)

Each documented in `docs/CODEX_QUALITY_REVIEW_2026-05-03.md`:

- #2 extract config helpers to a testable target
- #4 strict bool/int parsing helpers
- #5 explicit negative CLI flags
- #7 PTT setLine() initial failure handling
- #9 sink ownership lifetime race
- #11 BUFFER 0 immediacy vs rate-limit
- #13 `flushDataTxBuffer()` extraction for testability
- #15 sendBinary error propagation through ModemAdapter
- #16 HARQ key construction extraction to testable helper
- #17 HARQ when CW0 fails (architectural)
- #21 UltraTNCStation threading model

## Bottom line

The night's quality pass closed the highest-leverage Codex findings,
added 14 targeted tests against tonight's new code, and bumped
coverage on the most-changed files. The deferred items are
documented in repo history, not lost.

If the next session wants to continue: pick #16 (extract HARQ key
construction) — it's the bridge between the SoftCombineBuffer unit
tests and the streaming decoder integration, and would unlock
direct testing of the most subtle PHY parameter combinations
without driving full audio-decode flows.
