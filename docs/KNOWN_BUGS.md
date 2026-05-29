# Known Bugs

Last updated: 2026-05-23

## Purpose
Track only currently relevant issues that can affect reliability, throughput, or release quality.
Fixed/obsolete historical deep dives belong in `docs/CHANGELOG.md`.

## Active Issues

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
  - **Airtime-derived ACK/retransmit RTO (commit `d182751`, 2026-05-23, backlog #119).**
    Replaced the `[8000,12000]` magic timeout clamp with an RTO derived from burst
    airtime + carrier-sense/SACK coalesce + ACK airtime, so the sender no longer
    times out before a half-duplex-deferred ACK can physically return. Eliminated
    premature timeout-retransmission of already-delivered frames on a clean channel
    (AWGN SNR20 16QAM R1/2: 5/10 seeds → 0/10). See CHANGELOG 2026-05-23. NOTE: a
    longer honest RTO may add idle wait at window boundaries on clean channels —
    quantify under #121 (recoverable dead air) before further RTO tuning.
- Remaining work:
  - Connected-mode tail variance under aggressive forced profiles (D8PSK R1/2,
    DQPSK R3/4) when channel falls outside auto-selector envelope — these aren't
    auto-selected, so they bite only operators forcing modes manually.
  - BRAVO missing the initiator's CONNECT (opposite leg of the same race) is rare
    on the production envelope but lacks a comparable fast retry — initiator's
    `connect_timeout_ms = 60000` is far longer than the cli_simulator harness's
    30s PHASE 1 budget, so harness exposure of this case looks like seed noise.

### BUG-NACK-001: Burst GROUP_NACK sent over MC-DPSK handshake waveform, not the tone-burst
- Status: FIXED (2026-05-29, branch feat/oneway-arch-2026-05-27, commit pending). Verified
  seed 2 Good@20 warm-ON: tone-burst NACK emits=1, MC-DPSK NACK=0, MC-DPSK TX post-connect=0,
  file PASS 11/11 CRC-clean, run ~34 s faster (171 s vs 205 s).
- Area: burst transport failure path (`Connection::onGroupReceived`, the `!all_ok` branch)
- Reported by operator (waterfall observation), seed 2 Good@20 file transfer:
  - "Bravo issues a NACK using the old NACK system, not the tone-burst."
  - "After group 0's initial failure there is a weird MC-DPSK signal repassing on the waterfall."
- Root cause (BOTH symptoms are one bug): on a failed group the receiver sent
  `transmitFrame(makeGroupNack(...))`, a 20-byte control frame over the CURRENT
  waveform. For group 0 `handshake_complete_` is still false (it is set later, in
  the `all_ok` path the NACK branch `return`s before reaching), so the frame went
  out as the **MC-DPSK handshake waveform = 149760 samples ≈ 3.1 s** on the air —
  the "weird MC-DPSK signal" — instead of the §15 tone-burst (675 ms). The §15
  work routed the success GROUP_ACK to the tone-burst but left the failure
  GROUP_NACK on the legacy control-frame path.
- Impact:
  - ~4.6× slower NACK (3.1 s vs 675 ms) → slower deep-fade resend recovery (a chunk
    of seed 2's latency); off-waveform MC-DPSK energy mid-OFDM transfer.
- Fix: emit a NACK-type tone-burst (`AckType::Nack`, whole-group missing mask) from
  the `!all_ok` branch, mirroring `setSendGroupAck`. The sender's `onToneBurstAck`
  already maps a NACK-type tone-burst to `burst_transport_.onGroupNack` (resend
  now), so the entire receive path already existed — only the emit was wrong.
  Falls back to the OFDM `makeGroupNack` frame if the tone-burst callback is absent.
- Verification: seed 2 warm-ON GUI re-run — expect 0 MC-DPSK TX bursts after
  connect and tone-burst NACK emits > 0, file still CRC-clean.

### BUG-8PSK-001: Decision-directed tracking corrupts the 8PSK channel estimate on fading
- Status: FIXED (2026-05-29, channel-adaptive DD gate). DD cascade removed; see
  `docs/8PSK_GOOD_FADING_DIAGNOSIS_2026_05_29.md` and
  `docs/SYSTEM_PICTURE_FADE_SURVIVABILITY_2026_05_29.md`.
- Fix: `use_coherent_dd` now also requires `last_fading_index < 0.15`
  (`channel_equalizer_pilot.cpp`) — DD off on frequency-selective/faded frames
  (where its wrong decisions poison H), on for AWGN/flat frames (where it's
  safe). Threshold from measured data (AWGN ≤0.07, Good ~0.34 median) and equal
  to the existing LLR-scaling "faded" boundary. Env override `ULTRA_DD_FADING_MAX`
  (default 0.15); `ULTRA_COHERENT_DD_OFF=1` force-off. A per-symbol pilot-anchor
  innovation gate was tried first and removed — ineffective (a wrong-decision
  rotation and a legit between-pilot interpolation error are indistinguishable
  per-symbol; flat across a 4× tightness sweep).
- Verification: GUI 8PSK AWGN30 PASS 2330 bps 0 CW fail (DD stays on, no
  regression); Good@20 cascade gone (DD-on 83–125 CW fails/no delivery →
  adaptive: seed 42 PASS, seed 43 delivered CRC-clean). Offline (measure_ack_fer
  qam8 Good@20): adaptive == DD-off (46/120 chunks) vs DD-on 41/120.
- RESIDUAL (separate, NOT this bug): 8PSK is too marginal a rung for Good@20 —
  delivers slowly with heavy resends and fails on hard fades (seed 44: 0
  delivery). QPSK is ~2× more survivable there (offline 106/150 vs 52/150
  chunks). This is a rung-selection design item (adaptive mod choice per
  channel), tracked in `docs/SYSTEM_PICTURE_FADE_SURVIVABILITY_2026_05_29.md`,
  not a DD bug.
- Original diagnosis (root-cause history):
- Area: `src/ofdm/channel_equalizer_pilot.cpp` (`use_coherent_dd`, ON for QAM8/QAM16)
- Symptom: forced 8PSK (QAM8) R3/4 delivers perfectly on clean AWGN (2330 bps) but
  fails on Good@20 fading — heavy resends, frequently no delivery, confident-WRONG
  bits (strong |llr|≈15-20, LDPC parity fails 50-99 unsat). Bimodal per group (6/6
  or 0/6, never partial).
- Root cause (experimentally confirmed by five-way elimination — chain/fade/demap/
  static-H all ruled out): decision-directed channel tracking feeds 8PSK's occasional
  wrong hard-decisions (its 22.5° boundaries are tight on a fading channel) back into
  the H estimate, poisoning it → cascade of confident-wrong bits. `ULTRA_COHERENT_DD_OFF=1`
  flips 8PSK Good@20 from FAIL → PASS. The base LTS+pilot H is fine (DD-off uses it and
  delivers); a genie perfect-H with DD *on* still failed (DD corrupts even a perfect H).
  Matches the code's own comment warning DD "poisons H on bad decisions during fades."
- Impact: 8PSK is the promotion lever toward 3000 bps; this blocks it on fading.
- Fix direction: reliability-gate DD (only DD a symbol whose EVM is well inside the
  95% noise radius — the code comment's own proposal), or gate DD off for 8PSK on
  fading; pair with channel-adaptive promotion (8PSK on clean/mild, QPSK on deep fade).
- Residual after DD-off: still marginal (heavy resends) — irreducible
  frequency-selective phase near spectral nulls; ARQ/adaptive-rate handles the tail.

### BUG-HARNESS-001: gui_qso_scenario.sh hard-aborts on auto-negotiated mode != --expect-mod
- Status: KNOWN (2026-05-29; harness limitation, not a modem bug).
- Symptom: on a channel where the auto rate-ladder legitimately promotes (e.g. AWGN30
  → 16QAM), running with `--expect-mod QPSK` makes `hard_failure_reason` abort the run
  at the MODE_CHANGE (`REASON=unexpected_data_mode`), ~25 s in, 0 file-transfer
  attempts. Falsely looks like a QPSK/AWGN modem failure (it is not — forced QPSK on
  AWGN30 delivers fine).
- Fix direction: when rate is not locked/forced, treat `--expect-mod` as advisory
  (accept the auto-selected mode) instead of hard-aborting.

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

## Fixed Bugs

- 2026-05-13: BUG-PING-DETECTOR-001 fixed - real-HF PINGs now classify via additive chirp-lock plus LDPC-invalid PATH 2 while preserving the clean-cable/AWGN RMS-silence PATH 1.
- 2026-05-13: BUG-TNC-SESSION-001 fixed — R1 added the full RX decoder/session reset on disconnect; R2 added audio-producer quiesce/drain plus a reset-generation guard for in-flight decode callbacks, so a persistent `ultra_tnc` starts the next back-to-back PAT CONNECT from a fresh modem session boundary without stale capture backlog or stale cursor commits.
- 2026-05-08: BUG-CARRIER-LDPC-001 fixed — CarrierLDPC v1 was a new OFDM coded-bit wire image, but SP4 enabled the runtime as a local modem default instead of a negotiated peer capability. A partially upgraded Mac↔Pi pair applied the TX permutation on one endpoint while the peer decoded legacy ordering, producing the AWGN R1/2 1KB 0-ACK/15-timeout failure. The repair uses the existing `PHY_MASK_V1` capability: modern-modern CONNECT/CONNECT_ACK enables CarrierLDPC on both TX/RX; modern-legacy leaves the legacy ordering active. Synchronized upgraded hardware now passes AWGN / Good / Moderate at SNR=15 R1/2 1KB with DATA `Ncw=8` active and ACK/control `Ncw=1` inactive.
- 2026-05-08: BUG-BENCH-001 resolved — committed `fixtures/*.wav` are valid
  post-CONNECT DATA fixtures. They decode cleanly with
  `./build/decode_bench --mode bench --connected ...`; the earlier `0`-frame
  result came from running the bench in disconnected control-search mode.
- 2026-05-05: BUG-RATE-001 fixed — adaptive MODE_CHANGE panic-downshift on short Watterson-Good transfers. Hysteresis (`ADAPTIVE_PRESSURE_WINDOWS_FOR_DOWNGRADE = 2`) + lockout reduction (`ADAPTIVE_POST_DOWNGRADE_LOCKOUT_MS` 15 s → 5 s). 5-seed reproducer now 5/5 PASS with worst-case throughput improved 444 → 684 bps (no panic downgrade). See CHANGELOG 2026-05-05.
- 2026-02-12: GUI immediate TX abort control (`STOP TX`) added.
- 2026-02-12: GUI telemetry split into PHY vs effective goodput, plus ARQ health view.
- 2026-02-11: OTA control-path hardening and bootstrap safety updates.
- 2026-02-06 to 2026-02-10: ACK/control-frame decoding and ARQ robustness fixes.

For full details and commit-level history, use:
- `docs/CHANGELOG.md`
