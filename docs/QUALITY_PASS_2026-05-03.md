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
- #9 sink ownership lifetime race
  → `event_sink_` switched from (atomic raw pointer + unique_ptr) to a
  mutex-guarded `shared_ptr`. Readers snapshot a copy and drop the lock
  before invoking sink methods, so a concurrent attachServer/attachEventSink
  cannot pull the rug. External raw-pointer sinks wrapped with no-op
  deleter to preserve external ownership semantics.
- #10 BUFFER staging accounting test
  → Added `onModemBufferLevel(0)` test asserting staging bytes are still
  reported (BUFFER N stays nonzero until staging actually flushes).
- #11 BUFFER 0 immediacy vs rate-limit
  → BUFFER 0 transitions bypass the 1 s rate limit; non-zero stays throttled
- #13 `flushDataTxBuffer()` extraction
  → `TNCSession::encodePayloadForWire(payload, compression_enabled)` static
  helper + 5 direct unit tests (raw, below-threshold, deflate, expand-fallback,
  empty)
- #15 sendBinary error propagation
  → `ModemAdapter::sendBinary()` now returns bool; `TNCBridge::sendBinary()`
  propagates engine result; `flushDataTxBuffer()` only clears staging on
  success so engine refusals don't silently drop bytes Pat counted.
- #16 HARQ key construction extraction
  → `SoftCombineBuffer::makeKey(HarqKeyInputs)` + 6 unit tests covering
  carrier_count_hash distinguishing waveform mode and data-carrier count
- #17 HARQ key-build instrumentation (instrumentation phase)
  → `harq_key_build_success` / `harq_key_build_failed` counters in
  `DecoderProfile`. Surfaces in cli_simulator end-of-test summary with
  miss-rate %. Provides the data needed to decide whether a session-
  context fallback key is worth designing.

## Codex finding still deferred (per Codex's own guidance)

- #21 UltraTNCStation threading model — Codex: "needs careful concurrency
  design." Decoder-thread vs main-loop racing on `encoder_`, `decoder_`,
  `connected_`, `handshake_complete_`, `negotiated_waveform_`,
  `last_cfo_hz_`, AWGN RNG. Pick one model (queue everything onto main
  tick, or guard with a station mutex); both choices have non-trivial
  ripple effects through the existing callback paths.

## Bottom line (final)

**21 of 22 Codex findings closed across two batches.** Only the
UltraTNCStation threading-model rework is left, and Codex itself
flagged that one as needing human concurrency design — not
autonomous-safe.

ctest 35/35 green throughout. cli_simulator SNR15/good/R1/4 PASS
at 100% frame success after every commit.

Net additions across both batches:
- 1 new test target (`UltraTNCConfig` — 39 tests)
- ~70 new tests across `test_soft_combine`, `test_tnc_session`,
  `test_tnc_bridge`, `test_ultra_tnc_config`
- ultra_tnc.cpp split into runtime + config units (~455 lines moved)
- Soft-combine HARQ key construction now unit-testable
- TNC payload encoding now unit-testable
- BUFFER 0 truly immediate for Pat Flush() unblock
- `sendBinary` failure path no longer silently drops staged bytes
- `event_sink_` lifetime race closed
- HARQ key-build success/fail counters wired for next-session analysis
