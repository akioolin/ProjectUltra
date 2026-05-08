# Known Bugs

Last updated: 2026-04-26

## Purpose
Track only currently relevant issues that can affect reliability, throughput, or release quality.
Fixed/obsolete historical deep dives belong in `docs/CHANGELOG.md`.

## Active Issues

### BUG-CARRIER-LDPC-001: SP4 redirect interleaver wiring breaks hardware ACK return path
- Status: ROOT CAUSE LOCALIZED, RUNTIME PATH DISABLED 2026-05-08
- Area: `src/gui/modem/streaming_encoder.{cpp,hpp}` + `src/gui/modem/streaming_decoder.cpp` + `src/waveform/ofdm_chirp_waveform.cpp`
- Symptoms (hardware Mac↔Pi5 audio loopback):
  - Connection + handshake completed cleanly (PING/PONG, CONNECT/CONNECT_ACK, MODE_CHANGE/ACK).
  - Data phase failed: 5 frames sent A→B, **0 ACKs decoded back at A**, 15 retx timeouts, file transfer timeout (124 s budget exceeded).
  - `frame_success=100%` reported by ARQ counter (no FAIL frames seen at A's RX) — the ACKs simply never arrived as decodable audio.
  - Reproducible on AWGN injected SNR=15 R1/2 1KB; same pattern on Good and Moderate fading.
- Bisect:
  - `d8aa2ce` (BUG-RATE-001 hardening, 2026-05-06): PASS
  - `7bdf352` (phase-2 SP3, OFDM mask plumbing): PASS
  - `35997a9` (phase-2 SP4 first commit, RX-local per-carrier erasure): PASS
  - `08ed189` (phase-2 SP4 redirect r2-r4, control-frame gate + interleaver wiring): **FAIL** ← regression introduced here
- Mitigation in tree (2026-05-08):
  - `StreamingEncoder::use_carrier_ldpc_interleaver_` default = `false` (was `true`).
  - `StreamingDecoder::processWaveformForCodewords` now hard-codes `allow_carrier_ldpc = false`.
  - Per-carrier RX erasure gate (`allow_rx_erasure`) is preserved on its own — only the CarrierLDPC v1 *permutation* is disabled.
  - All compiled infrastructure remains in place so the bug can be reproduced and root-caused without re-landing the change.
- Verification of the mitigation:
  - ctest 38/38 (CarrierLDPC unit tests still pass — they exercise the round-trip in isolation).
  - `./agents/run_hardware_smoke.sh` PASS on AWGN / Good / Moderate at SNR=15 R1/2 1KB.
- Open questions for the proper fix:
  - Math + unit tests indicate TX-forward and RX-inverse permutations are inverses. Hardware shows they are not in the coupled runtime. Hypothesis to investigate: TX applies permutation regardless of `Ncw` (commit message says "for Ncw=2..8" but encoder enable flag is set unconditionally on `createWaveform`); RX applies inverse only for connected+OFDM frames. PING/PONG are MC-DPSK (no permutation), so they unblock the early gate, but ACK frames sent in connected OFDM mode at Ncw=1 might be permuted on TX and not de-permuted on RX (or vice versa) depending on which side's encoder/decoder state is observed.
  - The synthetic-notch A/B win documented in 08ed189 was on `cli_simulator` self-loopback (no real audio path), where the ChannelInjector + identical clocks may mask the failure mode that hardware exposes.
  - The synthetic AWGN deterministic SHA256 match in the 08ed189 commit message proves only that TX bytes are unchanged when erasure does not fire — it does not prove RX-side permutation correctness when it does.
- Owner: investigate after a clean review of the SP2 interleaver math vs the 08ed189 wiring; cross-reference with Codex who co-authored 08ed189.

### BUG-CTRL-001: Control path is still the bottleneck in aggressive fading profiles
- Status: IN_PROGRESS (handshake leg fixed 2026-04-26)
- Area: OFDM connected mode (ACK/SACK/control reliability)
- Symptoms:
  - Data codewords decode but ACK reception misses trigger avoidable retransmits/timeouts.
  - Most visible with aggressive profiles (for example D8PSK R1/2), but can still appear on weaker seeds in other OFDM rates.
- Impact:
  - Throughput tail collapse on bad seeds.
  - File transfer latency variance much larger than message transfer variance.
- Current mitigations already in tree:
  - R1/4 control profile for OFDM control frames.
  - ACK repeat/coalescing and improved ARQ observability.
  - `DISCONNECT_SEQ` protection against stale data ACK being mistaken as disconnect ACK.
  - **Proactive CONNECT_ACK retransmission (responder side)** — covers handshake-leg
    losses at OFDM data modes. Auto-mode baseline at SNR=15 moderate (DQPSK R1/2)
    went from 4/5 → 5/5 message tests and 2/3 → 3/3 file 2048 tests on 5/3-seed
    samples. See CHANGELOG 2026-04-26.
- Remaining work:
  - Connected-mode tail variance under aggressive forced profiles (D8PSK R1/2,
    DQPSK R3/4) when channel falls outside auto-selector envelope — these aren't
    auto-selected, so they bite only operators forcing modes manually.
  - BRAVO missing the initiator's CONNECT (opposite leg of the same race) is rare
    on the production envelope but lacks a comparable fast retry — initiator's
    `connect_timeout_ms = 60000` is far longer than the cli_simulator harness's
    30s PHASE 1 budget, so harness exposure of this case looks like seed noise.

### BUG-CFO-001: OFDM two-stage CFO refinement remains incomplete
- Status: OPEN
- Area: `src/ofdm/demodulator.cpp`
- Evidence:
  - TODO at `src/ofdm/demodulator.cpp:1307` for proper two-stage CFO (CP/frequency-domain refinement).
- Impact:
  - CFO handling works for current tested profiles but remains less robust than desired for broader OTA variation.
- Next steps:
  - Implement and validate two-stage CFO refinement with seeded regression + OTA logs.

## Release Blockers

An issue is release-blocking if it causes any of:
- reproducible data loss,
- deterministic disconnect deadlock,
- non-deterministic gate failure in default mode ladder.

Current blockers:
- None identified for `0.2.1-alpha` default ladder.

## Recently Fixed (Short List)

- 2026-05-05: BUG-RATE-001 fixed — adaptive MODE_CHANGE panic-downshift on short Watterson-Good transfers. Hysteresis (`ADAPTIVE_PRESSURE_WINDOWS_FOR_DOWNGRADE = 2`) + lockout reduction (`ADAPTIVE_POST_DOWNGRADE_LOCKOUT_MS` 15 s → 5 s). 5-seed reproducer now 5/5 PASS with worst-case throughput improved 444 → 684 bps (no panic downgrade). See CHANGELOG 2026-05-05.
- 2026-02-12: GUI immediate TX abort control (`STOP TX`) added.
- 2026-02-12: GUI telemetry split into PHY vs effective goodput, plus ARQ health view.
- 2026-02-11: OTA control-path hardening and bootstrap safety updates.
- 2026-02-06 to 2026-02-10: ACK/control-frame decoding and ARQ robustness fixes.

For full details and commit-level history, use:
- `docs/CHANGELOG.md`

