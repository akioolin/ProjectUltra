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

## Codex findings closed in the second batch (2026-05-03 late)

After the first batch, the user asked to keep going. Closed:

- #2 extract config helpers to a testable target
  → `tools/ultra_tnc_config.{hpp,cpp}` + `tests/test_ultra_tnc_config.cpp`
  (39 tests, ultra_tnc.cpp shrank from ~1020 to ~566 lines)
- #4 strict bool/int parsing helpers
  → `parsePositiveIntStrict`, `parseBoolStrict` in the new config TU
- #5 explicit negative CLI flags
  → `--no-inject-channel`, `--ptt-active-high` (also `--no-ptt-inactive-high`)
- #7 PTT setLine() initial failure handling
  → startup aborts if initial inactive set fails; mid-session failures logged
- #11 BUFFER 0 immediacy vs rate-limit
  → BUFFER 0 transitions bypass the 1 s rate limit; non-zero stays throttled
- #13 `flushDataTxBuffer()` extraction
  → `TNCSession::encodePayloadForWire(payload, compression_enabled)` static
  helper + 5 direct unit tests (raw, below-threshold, deflate, expand-fallback,
  empty)
- #16 HARQ key construction extraction
  → `SoftCombineBuffer::makeKey(HarqKeyInputs)` + 6 unit tests covering
  carrier_count_hash distinguishing waveform mode and data-carrier count

## Codex findings still deferred (per Codex's own guidance)

- #9 sink ownership lifetime race (Codex: "deferred unless touching bridge
  threading; needs careful review")
- #15 sendBinary error propagation through ModemAdapter (Codex:
  "interface change needs broader judgment")
- #17 HARQ when CW0 fails — architectural (Codex: "needs human/protocol judgment")
- #21 UltraTNCStation threading model (Codex: "needs careful concurrency design")

## Bottom line (final)

Across both batches: **18 of 22 Codex findings closed**. ctest 35/35
green, cli_simulator SNR15/good/R1/4 PASS at 100% frame success.
Remaining 4 items are explicitly Codex-flagged as needing human
architectural judgment, not autonomous-safe.

Net additions: 1 new test target (`UltraTNCConfig`), 65+ new tests
across `test_soft_combine`, `test_tnc_session`, `test_tnc_bridge`,
`test_ultra_tnc_config`. ultra_tnc.cpp split into runtime + config
units; soft-combine HARQ key building now unit-testable; TNC payload
encoding now unit-testable; BUFFER 0 truly immediate for Pat Flush().
