# Known Bugs

Last updated: 2026-05-23

## Purpose
Track only currently relevant issues that can affect reliability, throughput, or release quality.
Fixed/obsolete historical deep dives belong in `docs/CHANGELOG.md`.

## Active Issues

### BUG-FINACK-001: Final-group ACK loss → sender infinite-resends the last group; no clean transfer close
- Status: OPEN (2026-05-31). Folded into thread C (ladder/ARQ rework) — see
  `docs/OFDM_COHERENT_ONLY_DECISION_2026_05_31.md §5`. Pre-existing (NOT introduced by the
  2026-05-31 carrier-LDPC / z-latch work); exposed by a fade landing on the completion ACK.
- Area: burst completion handshake — `Connection` GROUP_ACK path + the "payload drained →
  auto-disconnect" trigger; receiver duplicate-group handling in `onGroupReceived` / burst assembler.
- Reported by operator (live GUI), coherent QPSK R1/4 Moderate@14 seed 777:
  "it received the file and alpha is still sending."
- Repro / evidence (`/tmp/zmode_mod14_qpsk_s777b`): Bravo logs `Received …10240 bytes, CRC ok`
  at 282 s (file COMPLETE). Alpha sits at `cursor=10240/10240, resent=1, resend_left=0` and
  re-emits `TX Burst: 6 frames` for `group_seq=18` every ~10 s (ack_timeout) indefinitely; the
  scenario only ends via `exit_after`. seed 42 escaped it solely because its final ACK happened
  to land and `auto-disconnect (payload drained)` fired.
- Root cause: the completion handshake has no robust close. The last group's **GROUP_ACK was lost
  in a fade**, so the sender never learns the receiver has it and resends forever; "payload drained
  → disconnect" never triggers because the sender still considers the final group unacked. When the
  sender resends an **already-delivered** group, the receiver does **not re-ACK** — it tries to
  re-decode the duplicate (which here also hits the fade-lost-descriptor z=27 path →
  `0/6 max_iters=0`, a red herring) and stays silent. The file delivered fine; only the close fails.
- Impact: on real hardware, wasted airtime (sender keeps keying the last group) and a transfer that
  never cleanly terminates without an external timeout. Half-duplex final-ACK fragility.
- Fix (thread C): group-level **duplicate detection + re-ACK** — a resend of an already-delivered
  group re-emits the GROUP_ACK **without** re-decoding. Plus a proper **FILE_END / completion
  handshake** so the transfer terminates on "receiver has the whole file" (FILE_END → FILE_END_ACK →
  DISCONNECT), not on a single per-group ACK happening to land. (Note: §7.6 z-consolidation would
  *partly* help IF the re-ACK were decode-gated — but the robust fix is decode-independent.)
- Verification: a fade-timed seed where the last GROUP_ACK is lost (seed 777 Moderate@14) must still
  reach a clean DISCONNECT (no infinite group-18 resend); `ALPHA_DISCONNECTED_COUNT>0`, RESULT=PASS.

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

### BUG-HARNESS-002: measure_ack_fer fidelity defects (under-piloting + broken AWGN burst path)
- Status: PARTIALLY FIXED (2026-05-29). The pilot-spacing defect is fixed; the AWGN
  burst_chunk defect is documented, not yet fixed.
- Defect 1 (FIXED): `makeOFDMConfig` hardcoded `cfg.pilot_spacing = 10`, while
  production uses `ofdm_link_adaptation::recommendedPilotSpacing(mod,rate)` = 5 for
  R1/2 & R2/3, 8 for R3/4. This under-piloted coherent high-order mods by ~2× and
  made 16QAM look structurally worse than production would. Fixed to call
  `recommendedPilotSpacing` (`tools/measure_ack_fer.cpp`). NOTE: re-test showed the
  under-piloting was NOT the cause of 16QAM folding (folds at spacing 5 too) — but
  the harness should still match production.
- Defect 2 (KNOWN): `--config burst_chunk --channel awgn` returns 0 frames recovered
  for all mods (qpsk/qam8/qam16, all DD modes) — the offline AWGN burst path does not
  match the GUI/OTASim AWGN path (where these mods decode fine). So measure_ack_fer
  AWGN burst_chunk numbers are not usable; use the GUI for AWGN.
- Defect 3 (KNOWN, 2026-05-29): the **Z=81 / N=1944 (long-LDPC burst keystone) path
  decodes to 0% in measure_ack_fer — even noiseless QPSK** — on BOTH the frame path
  (`--config data4_full` + `ULTRA_LDPC_Z=81`) and the burst path
  (`--config burst_chunk` + `ULTRA_LDPC_Z=81`). Verified Good@60: qpsk/qam16 = 0/100
  at Z=81 vs 91/84 at Z=27; confirmed pre-existing (reproduces with all genie edits
  git-stashed). ROOT CAUSE: `ULTRA_LDPC_Z=81` changes the ENCODER codeword size and
  the decoder's *block size* (→1944, `streaming_decoder.cpp:641`), but
  `decodeFixedFrame`'s `ldpc_z` argument is sourced ONLY from the burst descriptor
  (`last_burst_descriptor_.lifting_z`, `streaming_ofdm_decode.cpp:2876-2889`), NOT
  from the env. With no BURST_HEADER on the wire (frame path never sends one; the
  burst_chunk harness only enables the descriptor in its `wrong_group` branch), the
  decoder accumulates 1944 soft bits but decodes them with a **Z=27 matrix** →
  guaranteed failure. Production (GUI burst transport) IS unaffected: it transmits the
  BURST_HEADER with `lifting_z=81`, so the decoder learns Z and decodes correctly
  (GUI-proven CRC-clean ~3.1 kbps Good@20). CONSEQUENCE: **measure_ack_fer cannot
  validly screen the production burst config (Z=81); all offline Z=81 numbers are
  garbage.** The valid offline data is Z=27 only.
- DEEPER ROOT CAUSE (2026-05-29, attempted fix found it is MULTI-LAYER — NOT a quick
  fix): even with the encoder emitting Z=81 (via `setLDPCLiftingZ`, added as
  `--ldpc-z 81`) AND the decoder env-forced to Z=81 (block-size getter +
  `decodeFixedFrame` ldpc_z both read `ULTRA_LDPC_Z`), Z=81 STILL decodes 0% — even a
  1-CW frame, noiseless. Two more layers:
  (1) the waveform's soft-bit-grab size (`active_ldpc_block_size`, used by
      `getSoftBits`) is bumped to 1944 ONLY by `setActiveLDPCLiftingZ()`, which fires
      ONLY on descriptor-consume (streaming_ofdm_decode.cpp:763). With no descriptor
      the decoder grabs 648 bits of a 1944-bit codeword ("Got 648 soft bits,
      proceeding to decode") → garbage.
  (2) the descriptor-consume itself does not fire in the harness: the BURST_HEADER (a
      DQPSK-R1/4 control frame) is routed to the DATA decode path (line 1124), not the
      CONTROL path (line 661, which recognizes `FrameType::BURST_HEADER`), so it is
      mis-decoded as a failed data frame and dropped — never consumed (verified:
      `--burst-descriptor 1` gives 0% at BOTH Z=27 and Z=81 on Good@60).
  KEY INSIGHT (user): the BURST_HEADER is the SINGLE self-describing conveyor of
  lifting_z AND cw_per_frame AND group_size AND interleave flags together — the
  decoder is designed to learn the whole burst geometry from the wire in one shot, so
  piecemeal patching (env for Z, setFixedFrameCodewords for cw) cannot reproduce
  production. The ONLY faithful fix is to make descriptor-consume fire in the harness
  (route the BURST_HEADER to the control decode path). That is real plumbing across
  the control-detection + block-size + descriptor-consume layers — deferred. The
  `--ldpc-z 81` scaffolding (encoder emits + announces Z=81; env wires the decoder
  decode matrix + block-size getter) is committed as a correct PARTIAL toward that fix.
- Impact (updated): characterize ANY 16QAM-on-production-burst (Z=81) claim on the
  real-time GUI; the offline harness cannot do it until descriptor-consume is fixed.
- Impact: measure_ack_fer is a fast *screen* for relative offline comparisons on the
  Good path at **Z=27 only**; its AWGN path and its entire Z=81 path are not faithful.
  Confirm any fade/throughput conclusion — and ANY 16QAM-on-production-burst claim —
  on the real-time GUI per the project's standing rule.

### BUG-CFO-001: OFDM two-stage CFO refinement remains incomplete
- Status: OPEN
- Area: `src/ofdm/demodulator.cpp`
- Evidence:
  - TODO at `src/ofdm/demodulator.cpp:1307` for proper two-stage CFO (CP/frequency-domain refinement).
- Impact:
  - CFO handling works for current tested profiles but remains less robust than desired for broader OTA variation.
- Next steps:
  - Implement and validate two-stage CFO refinement with seeded regression + OTA logs.

### BUG-TEARDOWN-001: RX decode thread UAF crash on disconnect/shutdown (SIGSEGV in HilbertTransform::process)
- Status: OPEN (surfaced 2026-05-31; PRE-EXISTING — identical faulting stack in crash
  reports from 2026-05-28, predating the SyncController work).
- Symptom: `ultra_gui` (receiver side) SIGSEGVs at end-of-transfer / disconnect / shutdown.
  EXC_BAD_ACCESS / KERN_INVALID_ADDRESS. Faulting thread = the RX decode thread:
  `HilbertTransform::process` ← `OFDMChirpWaveform::detectDataSync` ← `StreamingDecoder::
  searchForSync` ← `processBuffer` ← `ModemEngine::rxDecodeLoop`. The receiver's log ends
  abruptly right after `Disconnected ... (Remote disconnected)` + `SR-ARQ: Reset`, with NO
  `RX decode thread stopped` line (cf. the sender, which tears down cleanly).
- Does NOT corrupt results: the file delivers CRC-clean BEFORE teardown (e.g. QPSK R1/4
  AWGN@10 seed42 → RESULT=PASS, GOODPUT=380, 0 retx, then crash on teardown).
- Root cause (CONFIRMED from both racer stacks 2026-05-31): on disconnect, the MAIN thread
  synchronously rebuilds the waveform while the RX decode thread is mid-demod on the old one:
  - WRITER (main thread, frees/rebuilds): `Connection::enterDisconnected` → `ModemEngine::
    setConnected(false)` → `StreamingEncoder::setDataMode` → `MCDPSKWaveform::initComponents`
    → `new MultiCarrierDPSKModulator` → `new ChirpSync` → `ChirpSync::generateTemplate`.
  - READER (Thread 22, faults): `rxDecodeLoop` → `StreamingDecoder::searchForSync` →
    `OFDMChirpWaveform::detectDataSync` → `HilbertTransform::process` → UAF (`KERN_INVALID_
    ADDRESS`, small offset = deref into freed/rebuilt waveform memory).
  The §14.36 "crash fix v3" (`applyPendingConnectedOFDMMode`) deferred only CONNECTED-mode
  reconstruction to a safe boundary in the RX loop; the DISCONNECT reconfig
  (`setConnected(false)`→`setDataMode`→waveform rebuild) is NOT synchronized with the RX
  thread at all. `stopRxDecodeThread()` joins, but `enterDisconnected` runs the rebuild on the
  protocol/main thread WITHOUT first quiescing the still-running RX `detectDataSync`.
- Why it matters for the SyncController refactor: this is exactly the §7.7#4 lifecycle-
  discipline scope the refactor must own. It also POLLUTES the GUI gate's crash signal —
  every gate run crashes at teardown, so a refactor-introduced crash can't be cleanly
  distinguished from this one. Fixing it first gives the behavioral phase a clean crash
  baseline.
- Fix direction: synchronize disconnect/shutdown waveform reconfig with the RX thread —
  either stop+join (or quiesce under `reset_generation_` + `buffer_mutex_`) BEFORE the
  waveform/HilbertTransform is reconfigured on disconnect, or route the disconnect reconfig
  through the same deferred safe-boundary the connected path uses. Fold into the
  SyncController lifecycle ownership (`reset()` discipline).

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
