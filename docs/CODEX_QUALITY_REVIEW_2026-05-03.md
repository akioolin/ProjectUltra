# Codex Quality Findings

Scope: reviewed `8f7a43c..aee927b` with focus on TNC session/bridge, `ultra_tnc`, HARQ soft-combine, and the new tests. Per instruction, I did not run tests.

## Ranked Findings

1. Severity: critical
   What's wrong/missing: The new bridge test "setConnectionChangedCallback proxies to engine" appears to be written against behavior the bridge does not have. `setConnectionChangedCallback()` stores a user callback; it is only invoked after `start()` wires the fake engine callback through `wirePECallbacks()`. The test emits directly on the fake engine without starting the bridge, so `connection_cb` is unset and `fired` should remain false.
   File:line: `tests/test_tnc_bridge.cpp:444`
   Suggested fix shape: Call `h.bridge.start()` before `h.engine.emitConnection(...)`, or rename/rewrite the test to assert the actual bridge contract instead of a pass-through contract.
   Tonight: autonomous/safe.

2. Severity: medium
   What's wrong/missing: `ultra_tnc` argument parsing and config loading have no unit coverage. The new config path, default search path, CLI-overrides-config behavior, PTT fields, and `--list-audio-devices` flag are all in the binary source and are not reachable from existing tests.
   File:line: `tools/ultra_tnc.cpp:178`, `tools/ultra_tnc.cpp:232`, `tools/ultra_tnc.cpp:284`; `tests/CMakeLists.txt:34`
   Suggested fix shape: Move `Config`, `applyConfigKey`, `loadConfigFile`, `findDefaultConfigFile`, and `parseArgs` into a small testable helper target. Add temp-file tests for valid config, unknown keys, invalid values, CLI override precedence, and missing `--config` values.
   Tonight: autonomous/safe.

3. Severity: medium
   What's wrong/missing: `parseArgs()` loads default or explicit config before honoring `--help` or `--list-audio-devices`. A malformed `./ultra_tnc.conf` can make `ultra_tnc --help` fail, which is a bad operator experience and makes recovery harder.
   File:line: `tools/ultra_tnc.cpp:284`, `tools/ultra_tnc.cpp:294`, `tools/ultra_tnc.cpp:300`, `tools/ultra_tnc.cpp:320`, `tools/ultra_tnc.cpp:322`
   Suggested fix shape: First pass should detect `--help` and `--list-audio-devices` and skip config loading for those modes. Add tests with a deliberately bad default config present.
   Tonight: autonomous/safe.

4. Severity: medium
   What's wrong/missing: Config parsing accepts or mis-parses bad booleans and integers. Examples: `ptt_inactive_high = maybe` silently becomes false; uppercase `TRUE` is not handled consistently; `std::stoi` accepts partial strings like `9600abc`; negative PTT baud is accepted by CLI/config and later silently coerced to 9600 by serial open.
   File:line: `tools/ultra_tnc.cpp:191`, `tools/ultra_tnc.cpp:217`, `tools/ultra_tnc.cpp:223`, `tools/ultra_tnc.cpp:403`; `src/gui/serial_ptt.cpp:86`
   Suggested fix shape: Add shared `parseBoolStrict()` and `parsePositiveIntStrict()` helpers with full-string consumption, case-insensitive booleans, and range checks. Use them for both config and CLI.
   Tonight: autonomous/safe.

5. Severity: medium
   What's wrong/missing: The help text promises CLI flags override config values, but some boolean settings cannot be overridden back to false from the CLI. If config sets `ptt_inactive_high = true`, there is no `--ptt-active-high` / `--no-ptt-inactive-high` equivalent. Same shape for `inject_channel`.
   File:line: `tools/ultra_tnc.cpp:140`, `tools/ultra_tnc.cpp:191`, `tools/ultra_tnc.cpp:421`
   Suggested fix shape: Add explicit negative flags or switch boolean options to `--flag <true|false>` equivalents in the config/parser table. Test config true plus CLI false.
   Tonight: autonomous/safe.

6. Severity: medium
   What's wrong/missing: `inject_channel_type` is dead configuration. CLI/config can store a channel type, but transmit only checks `cfg_.inject_channel` and always applies AWGN.
   File:line: `tools/ultra_tnc.cpp:59`, `tools/ultra_tnc.cpp:358`, `tools/ultra_tnc.cpp:812`
   Suggested fix shape: Either remove the optional type from usage/config and delete the field, or implement dispatch for the supported channel types. If keeping only AWGN, reject non-AWGN values.
   Tonight: autonomous/safe.

7. Severity: medium
   What's wrong/missing: Hardware PTT setup ignores `setLine()` failures and does not explicitly force the line inactive on shutdown. Startup can print "Hardware PTT enabled" even if the initial inactive write failed, and teardown relies on closing the serial handle rather than asserting a known inactive state.
   File:line: `tools/ultra_tnc.cpp:893`, `tools/ultra_tnc.cpp:897`; `src/gui/serial_ptt.cpp:180`
   Suggested fix shape: Check the initial `setLine()` return and fail startup or clearly disable hardware PTT. Wrap the callback in a small RAII owner that sets inactive before close. Log callback failures with enough context.
   Tonight: autonomous/safe for code; hardware validation deferred.

8. Severity: medium
   What's wrong/missing: `TNCBridge::postPTT()` invokes sink and hardware callbacks while called under `ptt_mutex_` from `onAudioQueueState()`, `abort()`, and `stop()`. A slow serial call or a reentrant callback can stall bridge ticking or deadlock.
   File:line: `src/tnc/tnc_bridge.cpp:477`, `src/tnc/tnc_bridge.cpp:539`
   Suggested fix shape: Compute the PTT state transition under the mutex, release it, then call the event sink and copied hardware callback. Add a reentrant/clearing callback test.
   Tonight: autonomous/safe.

9. Severity: medium
   What's wrong/missing: Event-sink ownership has a lifetime race. `attachServer(nullptr)` and `attachEventSink()` reset `owned_event_sink_` before/while publishing the new atomic pointer; a concurrent bridge callback can load a dangling pointer.
   File:line: `src/tnc/tnc_bridge.cpp:165`, `src/tnc/tnc_bridge.cpp:176`
   Suggested fix shape: Store `nullptr` with release semantics before resetting owned storage, or protect sink replacement and callback load/use with a mutex/shared ownership model.
   Tonight: deferred unless touching bridge threading; needs careful review.

10. Severity: medium
    What's wrong/missing: BUFFER staging accounting is not covered by tests. The changed behavior includes local `data_tx_buffer_` in unsolicited BUFFER and `BUFFER` command snapshots, but tests only cover raw modem backlog with no staged TCP data.
    File:line: `src/tnc/tnc_session.cpp:332`, `src/tnc/tnc_session.cpp:403`, `src/tnc/tnc_session.cpp:742`; `tests/test_tnc_session.cpp:498`, `tests/test_tnc_session.cpp:762`
    Suggested fix shape: Add session tests that enter CONNECTED, call `handleDataBytes()` without ticking past 200 ms, then assert `BUFFER` and `onModemBufferLevel(0)` report staged bytes rather than zero. Add a server-level data-socket variant if time permits.
    Tonight: autonomous/safe.

11. Severity: medium
    What's wrong/missing: The bridge comment says backlog transitions to BUFFER 0 are immediate, but `TNCSession` rate-limits changed buffer levels for up to one second. This may be fine, but the comment and behavior diverge on a Pat-sensitive path.
    File:line: `src/tnc/tnc_bridge.cpp:362`; `src/tnc/tnc_session.cpp:347`, `src/tnc/tnc_session.cpp:389`
    Suggested fix shape: Add an integrated bridge/server/session test for nonzero backlog -> zero backlog under the default rate limit. If Pat needs immediate zero, special-case zero or shorten the pending flush path; otherwise fix the comment.
    Tonight: deferred; needs product/protocol judgment.

12. Severity: medium
    What's wrong/missing: RX deflate failure is silently dropped and untested. The behavior is defensible, but corrupt compressed payloads are a correctness-critical edge and should not regress into forwarding garbage.
    File:line: `src/tnc/tnc_session.cpp:309`; `tests/test_tnc_session.cpp:677`
    Suggested fix shape: Add a test for `{0x01, bad, bytes}` that asserts no `data_out` call. Consider a debug counter/log if operators need visibility into corrupt peer payloads.
    Tonight: autonomous/safe.

13. Severity: nice-to-have
    What's wrong/missing: `flushDataTxBuffer()` does compression decision, marker assembly, modem send, and buffer mutation in one private method. Existing tests cover it only through connected state and timers.
    File:line: `src/tnc/tnc_session.cpp:417`
    Suggested fix shape: Extract a pure helper like `encodePayloadForWire(payload, compression_enabled)` returning wire bytes and maybe a mode enum. Keep `flushDataTxBuffer()` as state mutation plus send. Unit-test raw, small, compressible, uncompressible, and compression failure cases directly.
    Tonight: autonomous/safe, but not required tonight unless compression is being touched.

14. Severity: medium
    What's wrong/missing: `cmdStats()` bypasses the guarded emit helpers and calls `cmd_emit_` directly. A session constructed without a command emitter would throw `std::bad_function_call`, unlike the rest of the emit path.
    File:line: `src/tnc/tnc_session.cpp:818`, `src/tnc/tnc_session.cpp:836`
    Suggested fix shape: Add `emitStats(const ModemStats&)` or guard `if (cmd_emit_)`. Add one null-emitter test if the constructor contract allows empty callbacks.
    Tonight: autonomous/safe.

15. Severity: medium
    What's wrong/missing: Send failure is structurally impossible to handle at the TNC session layer. `ProtocolEnginePort::sendBinary()` returns bool, but `ModemAdapter::sendBinary()` is void, `TNCBridge::sendBinary()` ignores the bool, and `TNCSession` clears staging unconditionally after calling it.
    File:line: `src/tnc/tnc_bridge.hpp:50`; `src/tnc/modem_adapter.hpp:40`; `src/tnc/tnc_bridge.cpp:280`; `src/tnc/tnc_session.cpp:440`
    Suggested fix shape: Either propagate a bool through `ModemAdapter` and keep staged bytes on failure, or explicitly document/log that send cannot fail in CONNECTED state. Add a fake-engine reject test.
    Tonight: deferred; interface change needs broader judgment.

16. Severity: medium
    What's wrong/missing: HARQ key construction in `StreamingDecoder` has no direct test. `test_soft_combine` verifies map behavior, but nothing verifies that real decode state fills `sender_hash`, `seq`, `rate`, `cw_count`, modulation, interleave, and carrier hash correctly.
    File:line: `src/gui/modem/streaming_decoder.cpp:2612`; `tests/test_soft_combine.cpp:235`
    Suggested fix shape: Extract HARQ-key construction from the lambda into a testable helper that accepts decoded header plus PHY geometry. Add table tests for CHIRP/COX/NARROW, modulation changes, interleave on/off, and CW-count changes.
    Tonight: autonomous/safe if kept as a pure helper.

17. Severity: medium
    What's wrong/missing: HARQ only activates when CW0 can be decoded before combining. If the first failed attempt loses the header CW, `buildHarqKey()` returns false and the failed LLRs are not retained, so HARQ gives no benefit on header-damaged frames.
    File:line: `src/gui/modem/streaming_decoder.cpp:2635`, `src/gui/modem/streaming_decoder.cpp:2646`
    Suggested fix shape: Add counters/logging for HARQ key-build failures first. Then decide whether a provisional key can be derived from session/ARQ context, or whether this limitation is acceptable.
    Tonight: deferred; needs human/protocol judgment.

18. Severity: medium
    What's wrong/missing: The HARQ key comment says `carrier_count_hash` captures bits-per-symbol and pilot layout, but the current hash includes only `ofdm_data_carriers_` and `mode_`. Modulation is a separate key field, and pilot spacing/use-pilots are not represented.
    File:line: `src/fec/soft_combine.hpp:24`, `src/fec/soft_combine.hpp:30`; `src/gui/modem/streaming_decoder.cpp:2658`
    Suggested fix shape: Either correct the comment/name to match the actual hash, or include the full geometry that can change bit ordering: mode, data carriers, pilot spacing/use-pilots, FFT/CP if relevant, and bits-per-symbol if not relying on the separate modulation field.
    Tonight: autonomous/safe.

19. Severity: nice-to-have
    What's wrong/missing: The soft-combine saturation test is weaker than the implementation contract, and the implementation comment is internally inconsistent. Code caps at 60, but the comment says 10 attempts times |LLR|=8 equals 80 is fine; the test only asserts <=100.
    File:line: `src/fec/soft_combine.cpp:44`; `tests/test_soft_combine.cpp:125`, `tests/test_soft_combine.cpp:142`
    Suggested fix shape: Expose or duplicate a named expected cap for tests, assert the actual cap/sign behavior, and correct the comment math.
    Tonight: autonomous/safe.

20. Severity: nice-to-have
    What's wrong/missing: SoftCombine still lacks edge coverage for lifecycle knobs: `setEnabled(false)` clearing retained entries, `setTTL(0)` expiring immediately, `setMaxEntries(0)` clearing/preventing retention, empty LLR retain, and zero `sender_hash` no-op.
    File:line: `src/fec/soft_combine.cpp:62`, `src/fec/soft_combine.cpp:127`, `src/fec/soft_combine.cpp:160`, `src/fec/soft_combine.cpp:177`; `tests/test_soft_combine.cpp:295`
    Suggested fix shape: Add focused unit tests beside the new size-mismatch and enabled-accessor tests.
    Tonight: autonomous/safe.

21. Severity: medium
    What's wrong/missing: `UltraTNCStation` has unsynchronized cross-thread mutable state. The decode thread can invoke frame/ping callbacks while the main loop and engine callbacks mutate `encoder_`, `decoder_`, `connected_`, `handshake_complete_`, `negotiated_waveform_`, `last_cfo_hz_`, and the AWGN RNG.
    File:line: `tools/ultra_tnc.cpp:545`, `tools/ultra_tnc.cpp:631`, `tools/ultra_tnc.cpp:661`, `tools/ultra_tnc.cpp:717`, `tools/ultra_tnc.cpp:760`, `tools/ultra_tnc.cpp:819`
    Suggested fix shape: Pick one threading model: serialize modem/encoder/engine state changes onto the main tick thread via a queue, or add a station mutex around all shared state and encoder/decoder mode changes. Add TSAN coverage later if practical.
    Tonight: deferred; needs careful concurrency design.

22. Severity: nice-to-have
    What's wrong/missing: `git diff --check 8f7a43c..HEAD` reports trailing whitespace in new audit docs. Not runtime-critical, but it is a professionalism/lint cleanliness issue in the reviewed range.
    File:line: `docs/PAT_VARA_AUDIT.md:159`, `docs/PAT_VARA_AUDIT.md:162`, `docs/PAT_VARA_AUDIT.md:165`
    Suggested fix shape: Strip trailing spaces in those lines. Add/keep a diff-check step in CI if this matters.
    Tonight: autonomous/safe.

