# ProjectUltra - HF Sound Modem

## FRESH SESSION? START HERE

**If this is a new/fresh session, do this FIRST before any work:**

1. **Read AI collaboration playbook:** `cat docs/AI_COLLABORATION.md` - **MANDATORY** - How to work with Codex (the other AI on this project), when to involve it, brief format, verification gates, autonomous-mode rules
2. **Read project goals:** `cat docs/PROJECT_GOALS.md` - Mission, priorities, and task filter
3. **Read current agent/project state:** `cat docs/AGENT_CURRENT_STATE.md` - Current automation and handoff context
4. **Check known bugs:** `cat docs/KNOWN_BUGS.md` - Active bugs you must not re-discover
5. **Check recent changes:** `git log --oneline -10` - See recent commits

**Before modifying ANY code, read:**
- `docs/AI_COLLABORATION.md` - Required workflow with Codex for non-trivial changes
- `docs/PROJECT_GOALS.md` - Mission, priorities, throughput/reliability targets, and agent task rule
- `docs/INVARIANTS.md` - Critical rules that MUST NOT be violated (causes subtle bugs if ignored)

**Autonomous agent work:**
- Use `docs/AGENTIC_DEVELOPMENT.md` and one task file in `agents/queue/`; do not work from an open-ended prompt.
- Use `docs/PROJECT_GOALS.md` to keep work aligned with the modem mission and current priorities.
- Use `docs/AGENT_TASK_BACKLOG.md` for approved task candidates and acceptance criteria.
- Use `docs/AGENT_DEDICATED_ENV_MACOS.md` for MacBook dedicated-agent setup.
- Use `docs/AGENT_CURRENT_STATE.md` to recover compacted/lost agent-system context.
- Run `./agents/run_local_gate.sh` before claiming a task is done.
- Use `./agents/run_hardware_smoke.sh` for PHY/ARQ/audio-path changes and respect the hardware lock.
- Do not grant agents unrestricted shell access; use repo-scoped allowlists from `agents/permissions/`.

**This project has durable documentation files.** They exist because context was lost repeatedly, causing rework. USE THEM.

---

## Hardware Audio Calibration - Mac <-> Pi 5 Test Rig

**Current known-good calibration (2026-04-29):**
- Mac USB soundcard: `Sound Blaster Play! 3`
- Pi USB soundcard: ALSA card 0, `USB Audio Device`
- Mac volume: output `71`, input `60`
- Pi mixer: `Speaker` 65% (`-13.00 dB`), `Mic Capture` 57% (`+8.00 dB`), `Auto Gain Control` off
- Synthetic channel hardware tests: use `--inject --inject-gain 0.70`

**Apply calibration before hardware tests:**
```bash
osascript -e 'set volume input volume 60' -e 'set volume output volume 70'
ssh -i "$HOME/.ssh/id_pi5" math@pi5tester \
  "amixer -D default sset 'Auto Gain Control' off && \
   amixer -D default sset 'Speaker' 65% && \
   amixer -D default sset 'Mic' capture 57%"
```

**Verify raw audio before modem tests:**
```bash
SSH_KEY="$HOME/.ssh/id_pi5" ./tools/check_hw_audio_path.sh
```

Expected calibrated raw levels:
- Pi -> Mac: RMS about `0.124`, peak about `0.303`, per-channel rough frequency near `1 kHz`
- Mac -> Pi: RMS about `0.249`, peak about `0.408`, per-channel rough frequency near `1 kHz`
- Acceptable target: RMS `0.05-0.25`, peak `0.15-0.80`
- Too hot: peak above `0.90`; this risks ADC/DAC clipping and invalid fading-test results
- Too low/silent: RMS near `0.0003` or below; check cable/device selection

Last verified captures:
- `/tmp/ultra_audio_path_20260429_220218/pi_to_mac_capture.wav`
- `/tmp/ultra_audio_path_20260429_220218/mac_to_pi_capture.wav`

Last post-calibration modem sweep:
- Good injected, 1 KB, R1/2, SNR 20/15/12: pass, `0` retx, `/tmp/ultra_hw_20260429_220250`, `/tmp/ultra_hw_20260429_220341`, `/tmp/ultra_hw_20260429_220445`
- Moderate injected, 1 KB, R1/2, SNR 20/15/12: pass with `4/7/8` retx, `/tmp/ultra_hw_20260429_220538`, `/tmp/ultra_hw_20260429_220717`, `/tmp/ultra_hw_20260429_220828`
- Moderate injected, 1 KB, R1/4, SNR 15/12: pass with `8/8` retx, `/tmp/ultra_hw_20260429_221117`, `/tmp/ultra_hw_20260429_221240`
- Good injected, 5 KB, R1/2, SNR 15: pass with `13` timeout retx, `/tmp/ultra_hw_20260429_221423`

Post ACK/control robustness patch checks:
- AWGN injected, 1 KB, R1/2, SNR 15: pass, `0` retx, `/tmp/ultra_hw_20260429_222350`
- Good injected, 1 KB, R1/2, SNR 15: pass, `0` retx, `/tmp/ultra_hw_20260429_222920`
- Moderate injected, 1 KB, R1/2, SNR 15: pass, `0` retx, `/tmp/ultra_hw_20260429_223017`
- Moderate injected, 1 KB, R1/4, SNR 15: pass, `0` retx, `/tmp/ultra_hw_20260429_223253`
- Moderate injected, 1 KB, R1/2, SNR 12: pass with `7` retx, `/tmp/ultra_hw_20260429_223113`
- Moderate injected, 1 KB, R1/4, SNR 12: pass with `5` retx, `/tmp/ultra_hw_20260429_223754`
- Good injected, 5 KB, R1/2, SNR 15: pass with `4` retx, `/tmp/ultra_hw_20260429_223926`

Corrected two-sided Pi/Mac rebuild checks:
- Good injected, 1 KB, R1/2, SNR 15: pass, `0` retx, `/tmp/ultra_hw_20260429_224520`
- Moderate injected, 1 KB, R1/2, SNR 15: pass, `0` retx, `/tmp/ultra_hw_20260429_224612`
- Good injected, 5 KB, R1/2, SNR 15: pass with `4` timeout retx, `/tmp/ultra_hw_20260429_224658`; BRAVO failed the original seq32-35 data burst (`CW[0..3]: FAIL`) and decoded the retransmissions, so this is data-side loss, not ACK/control loss.

Final ACK/control + burst/data-acquisition checks:
- AWGN injected, 1 KB, R1/2, SNR 15: pass, `0` retx, `/tmp/ultra_hw_20260429_230135`
- Good injected, 5 KB, R1/2, SNR 15: pass, `0` retx, `/tmp/ultra_hw_20260429_225150`
- Moderate injected, 5 KB, R1/2, SNR 15: pass, `0` retx, `/tmp/ultra_hw_20260429_225916`
- Moderate injected, 1 KB, R1/2, SNR 12: pass, `0` retx, `/tmp/ultra_hw_20260429_225822`

Interpretation of the 2026-04-29 robustness work:
- ACK/control decode is healthy in AWGN, Good SNR15, Moderate SNR15, and the SNR12 Moderate canary: cumulative ACKs repeat when profile ACK diversity is enabled, and the 1-CW control LLR gate admits real fading ACKs down to `|LLR|_avg ~= 1.5`.
- The 5 KB Good residual was a burst-interleaver receiver bug, not LDPC weakness: a physical burst block at `RMS=0.0390` was below the old hard `0.0400` gate, aborting the whole 4-frame group. The decoder now demodulates weak blocks down to `0.015` and only inserts zero-LLR erasures below that, so one weak physical block does not become four ARQ retransmissions.
- The SNR12 Moderate tail retry was a data acquisition gate issue: real tail DATA can arrive around `corr ~= 0.52-0.56` and `|LLR|_avg ~= 1.7`. Connected DQPSK data sync and 4-CW escalation now admit those candidates while the false-lock LLR/near-zero gates still reject obvious noise.

**Important distinction:** hardware gain staging does not replace injected-channel headroom. The Watterson injector can generate samples above full scale before the soundcard. Keep `--inject-gain 0.70` unless a new calibration sweep proves a different value.

---

## CRITICAL RULES (Never Violate)

**Test binaries:**
- ALWAYS run from `build/` directory: `./build/cli_simulator`, NOT `./cli_simulator`
- `cli_simulator` - **PRIMARY** test tool for full protocol with light preamble in connected mode
  - Tests: PING/PONG → CONNECT → MODE_CHANGE → DATA (4CW frame interleaved) → DISCONNECT
  - Command: `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test 2>&1 | tee /tmp/test_output.log`
- `test_waveform_simple` - Quick single-frame sanity checks (NOT for connected mode testing)
- `ctest --test-dir build` - default maintained unit/regression gate
- `tests/regression_matrix.sh` - wrapper for default CTest; `--full` also runs maintained light-sync sweep when `cli_simulator` exists
- Obsolete direct-modem/QPSK harnesses were removed from `tests/`; use Git history for archaeology

**MC-DPSK invariants:**
- ALWAYS call `mc_dpsk_demodulator_->reset()` at start of `rxDecodeDPSK()`
- ALWAYS call `setCFO(frame.cfo_hz)` to reset CFO accumulation between frames
- CFO from chirp detection is TRUSTED over training-based CFO estimation

**OFDM CFO invariants (see `docs/CFO_CORRECTION_FLOW.md` for full details):**
- Chirp CFO is coarse — on fading channels it can be wrong by ±2 Hz
- LTS residual CFO estimation REFINES chirp CFO per frame (threshold 0.3 Hz)
- CFO feedback loop: after demodulation, corrected CFO propagates back to cached `last_cfo_`
- NEVER remove the feedback in `ofdm_chirp_waveform.cpp:process()` or `streaming_decoder.cpp`
- Without feedback, wrong chirp CFO re-injects on every frame → progressive phase drift → CW failures

**Testing invariants:**
- Use SINGLE ModemEngine instance for entire audio stream (continuous RX)
- Buffer limit: MAX_PENDING_SAMPLES = 960000 (20 seconds at 48kHz)
- **DEFAULT unit/regression gate:** `ctest --test-dir build --output-on-failure -j4`
- **PRIMARY regression test:** `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test 2>&1 | tee /tmp/test_output.log`
- `cli_simulator` is the ONLY tool that tests the full protocol with light preamble, two-station interaction, and proper connected-mode configuration. `test_waveform_simple` is for quick single-frame sanity checks only.
- **ALWAYS use `| tee /tmp/test_output.log`** when running tests - tests take minutes and we need full output for debugging

---

## TWO OPERATING MODES - CRITICAL ARCHITECTURE

**The modem has TWO completely different operating modes based on SNR:**

### Mode 1: MC-DPSK (below wide-OFDM floor, or heavy fading)
- **When:** in-band SNR is below the wide-OFDM ladder floor, or heavy fading conditions make OFDM inappropriate
- **Waveform:** Multi-Carrier DPSK with chirp sync
- **ARQ:** Stop-and-wait (window=1) - send ONE frame, wait for ACK
- **Frame format:** Variable codewords, simple sequential encoding
- **Control frames:** 20 bytes (ACK, NACK, etc.) - NO patching, encode as-is
- **Data frames:** Variable CWs based on payload size
- **Interleaving:** NONE (no frame interleaving, no channel interleaving)
- **Preamble:** ALWAYS full chirp preamble (no light sync)
- **Key files:** `decodeMCDPSKFrame()` in streaming_decoder.cpp

### Mode 2: OFDM (in-band SNR >= 10 dB AWGN, with fading margins)
- **When:** AWGN >=10 dB, Good >=12 dB, Moderate >=14 dB, Poor >=18 dB, with acceptable fading
- **Waveform:** OFDM with chirp or Schmidl-Cox sync
- **ARQ:** Selective Repeat (window=8) - send up to 8 frames before waiting
- **Frame format:** Fixed 4-codeword frames for data, 1-codeword for control
- **Control frames:** 20 bytes, 1 CW, no frame interleaving (fast ACK)
- **Data frames:** 4 CWs with frame interleaving
- **Interleaving:** Frame-level interleaving (spreads CWs), optional channel interleaving
- **Preamble:** Light preamble (LTS only) for data after handshake
- **Key files:** `decodeFixedFrame()` in frame_v2.cpp

### Mode 3: OFDM_NARROW (in-band SNR ~13-20 dB, 500 Hz bandwidth)
- **When:** Narrowband chirp detected (1250-1750 Hz), low SNR where wideband fails
- **Waveform:** OFDM with narrowband chirp sync, FFT=2048, 21 carriers, 492 Hz BW
- **ARQ:** Stop-and-wait (window=1) - same as MC-DPSK due to long frame times (~3s)
- **Frame format:** Same as wideband OFDM (4 CW data, 1 CW control)
- **Interleaving:** Frame-level + optional channel interleaving (same as OFDM)
- **Preamble:** Light preamble (LTS only) for data after handshake
- **Dual-listen:** RX listens for both wideband AND narrowband chirps when idle
- **ARQ:** Selective-repeat **window=3** (changed 2026-05-03 from stop-and-wait, then bumped from 2→3 after wall-clock A/B); ~96% throughput improvement on the documented R1/4 baseline. ACK timeout scaled to cover the 3-frame burst (~14 s).
- **Throughput:** ~200 bps (R1/4) to ~450 bps (R1/2) — ~5× slower than wideband but works at 7.5 dB lower SNR

### Mode Selection Flow
1. **Connection starts:** MC-DPSK for PING/PONG/CONNECT (wideband or narrowband chirp)
2. **Dual-listen:** RX detects chirp type → sets BandwidthMode (WIDE or NARROW)
3. **After CONNECT_ACK:** SNR is measured, mode is negotiated
   - Wideband AWGN-like SNR < 10: Stay in MC-DPSK
   - Wideband AWGN-like SNR >= 10: Switch to OFDM_CHIRP R1/4
   - Good/Moderate/Poor fading use 12/14/18 dB OFDM entry floors respectively
   - Narrowband detected: Switch to OFDM_NARROW
4. **enterConnected():** Sets ARQ window based on mode (1 for MC-DPSK/NARROW, 4 for wideband OFDM)
5. **StreamingEncoder/Decoder:** Check `mode_` to use correct path, `isOFDMMode()` for OFDM family

### NEVER MIX THESE:
- MC-DPSK frames through OFDM encoder (corrupts control frames)
- OFDM interleaving on MC-DPSK data
- Light preamble with MC-DPSK
- Window > 1 with MC-DPSK

---

**Performance Requirements (post-2026-05-20 warm-sync LTS verification):**
| Mode | Channel | In-band SNR floor | Confidence |
|------|---------|-----|---------------------|
| MC-DPSK R1/4 | AWGN | **5 dB** | 3/3 seeds cli_simulator + OTASim fixture `OTASimulatorTwoEndpointMCDPSKLowSNR` |
| MC-DPSK R1/4 | Moderate fading | 19.6 | pre-audit, not re-measured |
| OFDM_CHIRP R1/4 | AWGN | **10 dB** | warm-sync LTS FER: 4.875% at 10 dB (n=800), 0.167% at 12 dB (n=600), 0% at 14-20 dB |
| OFDM_CHIRP R1/4 | Good fading | **15 dB** | 3/3 seeds cli_simulator + `DecodeBenchReplay` fixture |
| OFDM_CHIRP R1/4 | Moderate fading | **15 dB** | 1-seed OTASim (Mod ≈ Good at this rate — FEC absorbs the difference) |
| OFDM_CHIRP R1/2 | AWGN | **14 dB** | 1-seed OTASim (boundary, ~40 retx but ARQ recovers) |
| OFDM_CHIRP R1/2 | Good fading | **14 dB** | 1-seed OTASim |
| OFDM_CHIRP R1/2 | Moderate fading | **18-22 dB** (unrefined) | 1-seed OTASim — 22 passes, 18 fails; bisect 19/20/21 pending |
| OFDM_NARROW R1/4 | AWGN / Good | 17.6 / 17.6 | pre-audit |

Higher rates (R2/3, R3/4, QPSK) — see historical section below; not re-measured against the new floor.

**⚠️ What these floors measure — read this before quoting them:**

The QSO floor and the raw-frame floor are now separately documented. The
`cli_simulator` / OTASim floor still measures a full session:
PING/PONG/CONNECT handshake with **full chirp+LTS preamble**, then post-handshake
data frames with **light preamble (LTS-only)** and selective-repeat ARQ. The
`warm_sync_light` FER harness measures the raw connected-mode light frame after
a full OFDM chirp+LTS anchor has seeded warm timing state.

Quantified by `docs/ACK_FRAME_FER_BASELINE_2026_05_20.md` (24 cells × 600 frames
AWGN, isolated-frame measurement):
- 4-CW data frame with **light preamble**: 100% FER at SNR=8/10/12, 31% FER at
  SNR=14, ~0% at SNR≥16
- 1-CW ACK with **light preamble**: 100% FER at SNR=8/10/12, 29% FER at SNR=14,
  ~0% at SNR≥16
- 1-CW ACK with **full chirp+LTS preamble**: 0.3% FER at SNR=12, 3.8% at SNR=10,
  68% at SNR=8

The old isolated-frame baseline remains important: cold light-preamble ACK and
4-CW data frames were 100% FER at 8/10/12 dB because the LTS-only detector had
to search a 200 ms window with a 0.52 acceptance threshold. In the connected
warm-sync regime, the receiver first decodes a full OFDM chirp+LTS anchor, then
uses frame-arrival timing to narrow the expected LTS window and lower the
threshold by the corresponding false-positive window reduction. The verified
raw connected ACK light FER is now 4.875% at 10 dB, 0.167% at 12 dB, and 0% at
14-20 dB AWGN (`docs/WARM_SYNC_LTS_VERIFICATION_2026_05_20.md`).

Sweep methodology (2026-05-19): `cli_simulator --ota-host 127.0.0.1:50051 --ota-alpha-token admin_tok --ota-bravo-token bravo_tok` against the running OTASim server, walking SNR down until a `TEST FAILED` cell. Single-seed cells are floor *locators*, not statistical floors — multi-seed verification is still TODO for the 1-seed entries.

**Current production state (post-2026-05-20 warm-sync LTS work, branch `feat/warm-sync-lts-2026-05-20`):**

The 2026-05-19 audit plus 2026-05-20 warm-sync LTS work moved floors:
- MC-DPSK R1/4 AWGN floor: **18 dB → 5 dB** (-13 dB)
- OFDM R1/4 AWGN floor: **18 dB → 10 dB** (-8 dB)
- OFDM R1/4 Good fading floor: **18 dB → 15 dB** (-3 dB, locked in DecodeBenchReplay fixture)
- OFDM R1/4 Moderate fading floor: **24.6 dB → 15 dB** (-9 dB, 1-seed)
- OFDM R1/2 AWGN floor: **24.6 dB → 14 dB** (-10 dB, 1-seed)
- OFDM R1/2 Good fading floor: **24.6 dB → 14 dB** (-10 dB, 1-seed)
- OFDM R1/2 Moderate fading floor: **24.6 dB → 18-22 dB** (-3 to -7 dB, 1-seed, unrefined)

Verification (2026-05-19):
- `ctest --test-dir build --output-on-failure -j1`: **86/86 PASS**
- Multi-seed cli_simulator (3 seeds × 3 cells): **9/9 PASS** at MC-DPSK SNR=5, OFDM R1/4 SNR=12 AWGN, OFDM R1/4 SNR=15 Good
- Single-seed OTASim sweep: 7 cells located floors for the rate × fading combos above

Verification (2026-05-20):
- `warm_sync_light` ACK FER: SNR 10/12/14/16/18/20 AWGN, n=800 at 10/14 and n=600 elsewhere, results in `docs/data/warm_sync_lts_verification_2026_05_20.csv`
- cli_simulator AWGN matrix: SNR 10/12/14 seeds 42/43/44 all pass (SNR 10 retx 0/0/0; SNR >=12 retx 0), and SNR 16/18/20/24 seed 42 all pass retx 0.
- The first natural connected OFDM frame in each direction is a full chirp+LTS timing anchor; subsequent connected OFDM frames use light LTS preambles with warm timing state and TX-turnaround reply prediction. No unsolicited protocol KEEPALIVE anchor is emitted.

Calibration baseline:
- Simulated AWGN calibration: `SimulatedChannel` synthetic AWGN is continuous at RX,
  sized from `StreamingEncoder::encodePing()` measured in-band RMS `0.3048` (after
  101-tap 50-2950 Hz receive FIR), per Layer 1 audit commit `7bbf22d`. The older
  broadband reference `0.3180724` is preserved for analytical use only.
- SNR convention: operator-facing channel knobs, idle meter, OFDM LTS/pilot
  meter, and rate selector all use receiver in-band SNR (3 kHz noise BW), per
  the unified-SNR work in commits `580e3b5`, `9a54402`, `f3d0c03`, `def3f6a`.
- Watterson channel: CFO uses analytic-signal (Hilbert) shifter per Layer 2
  audit (`bf0939a`), replacing the legacy custom passband down-mix/re-mix.
- Rate selector: only physical SNR sources (IDLE_IN_BAND, OFDM_BROADBAND) feed
  rate selection per Layer 3 audit (`2133b89`).

Historical / pre-audit measurements (not re-verified at the new floor, but still
valid at the SNRs listed):
- Two-endpoint continuous-AWGN v2 QSO sweep (DQPSK R1/4 auto): in-band +29.6,
  +24.6, +19.6, +14.6, +9.6, +6.6, and +4.6 dB pass; +1.6 dB fails
  connection/message/disconnect assertions. One-dB refinement: +3.6 dB passes,
  +2.6 dB fails the 30 s connected-state assertions and the 60 s message
  assertion.
- MC-DPSK: WORKING - 100% at in-band SNR≈19.6 (legacy configured 10) with moderate fading
- OFDM_CHIRP DQPSK R1/4 AWGN: WORKING - 100% at in-band SNR≈24.6 and 29.6 (legacy 15/20), 0 retries
- OFDM_CHIRP DQPSK R1/4 Good fading in-band SNR≈24.6 (legacy 15): WORKING - 100% (0 retries, 0 failures)
- OFDM_CHIRP DQPSK R1/4 Good fading in-band SNR≈19.6 (legacy 10): WORKING - 30/30 seeds PASS (avg 1.5 retx, 100% delivery)
- OFDM_CHIRP DQPSK R1/4 Moderate fading in-band SNR≈24.6 (legacy 15): WORKING - 5/5 seeds PASS (avg 1.4 retx, 100% delivery)
- OFDM_CHIRP DQPSK R1/2 AWGN: WORKING - 100% at in-band SNR≈24.6 and 29.6 (legacy 15/20), 0 retries
- OFDM_CHIRP DQPSK R1/2 Good fading: WORKING - 100% at in-band SNR≈24.6 (legacy 15), 5/5 seeds, 0 retries
- OFDM_CHIRP DQPSK R1/2 Moderate fading in-band SNR≈24.6 (legacy 15): WORKING - 5/5 seeds PASS (avg 2.4 retx, 100% delivery)
- OFDM_CHIRP DQPSK R2/3 AWGN: WORKING - 100% at in-band SNR≈29.6 (legacy 20), 0 retries
- OFDM_CHIRP DQPSK R2/3 Good fading in-band SNR≈29.6 (legacy 20): WORKING - 30/30 seeds PASS, 0 retransmissions
- OFDM_CHIRP DQPSK R2/3 Good fading in-band SNR≈24.6 (legacy 15): WORKING - 10/10 seeds PASS (avg 1.5 retx, 100% delivery)
- OFDM_CHIRP QPSK R1/2 AWGN: WORKING - 100% at in-band SNR≈29.6 (legacy 20), 0 retries
- OFDM_CHIRP QPSK R1/2 Good fading: WORKING - avg 95% frame success at in-band SNR≈29.6 (legacy 20), 30-seed survey, all messages delivered via ARQ
- OFDM_CHIRP QPSK R2/3 AWGN: WORKING - 100% at in-band SNR≈29.6 (legacy 20), 0 retries
- OFDM_CHIRP QPSK R2/3 Good fading: WORKING - 5/5 seeds PASS (2 seeds had retx, 3 clean)
- OFDM_CHIRP DQPSK R3/4 AWGN: WORKING - 100% at in-band SNR≈29.6 (legacy 20), 10/10 seeds, 0 retries
- OFDM_CHIRP DQPSK R3/4 Good fading: NOT RECOMMENDED (23 retx / 5 seeds — AWGN only)
- 1-CW ACK frames: WORKING - control frames use 1 CW (3x faster ACK)
- Variable-CW frames: WORKING - CONNECT/DISCONNECT use exact CW count (2 at R1/2, 3 at R1/4)
- OFDM_NARROW DQPSK R1/4 AWGN: WORKING - 100% at in-band SNR≈17.6 (legacy 8), 0 retransmissions
- OFDM_NARROW DQPSK R1/4 Good fading in-band SNR≈17.6 (legacy 8): WORKING - 100% data decode, 93% ACK, all messages delivered via ARQ
- OFDM_COX: FORCEABLE/LEGACY ONLY - implementation exists and can be
  selected explicitly, but the production auto ladder does not choose it
  until it has its own reliability gate.
- OTFS/MFSK: RESERVED ONLY - not in the production build or default capabilities
- cli_simulator: FULLY WORKING - all phases pass on AWGN and fading

**Auto rate selection ladder (2026-05-20):**
`src/protocol/waveform_selection.hpp::selectOFDMCodeRate()` is the
single source of truth for OFDM code-rate selection. Wideband waveform entry
floors are in `src/protocol/connection_policy.hpp`: AWGN 10 dB, Good 12 dB,
Moderate 14 dB, Poor 18 dB. Boundary tests live in
`tests/test_waveform_policy.cpp` and `tests/test_connection_policy.cpp`; do not
duplicate the full threshold table here.

**Temporal fading measurement (2026-02-03):**
- `getFadingIndex()` now combines freq_cv (multipath) + temporal_cv (Doppler spread)
- temporal_cv measures per-carrier magnitude variance over ~40+ symbols (~0.4s)
- Good channels (0.1Hz Doppler) show low temporal_cv (~0.03-0.30)
- Moderate channels (0.5Hz Doppler) show high temporal_cv (~0.40-0.55)
- **Trailing silence bug found and fixed**: `demodulateSoft()` was demodulating ~9 silence symbols
  at end of long frames (131 symbols, only 122 valid). This caused temporal_cv=0.27 on AWGN.
  Now detects and excludes trailing silence using energy-based threshold (20% of reference).
- Calibrated combined fading index values:
  - AWGN: ~0.04 (freq_cv ~0.003, temporal_cv ~0.032)
  - Good fading: ~0.62 (freq_cv ~0.32, temporal_cv ~0.30)
  - Moderate fading: ~0.90 (freq_cv ~0.42, temporal_cv ~0.49)
  - Poor fading: ~0.82 (freq_cv ~0.33, temporal_cv ~0.49)
- All `isFading()` thresholds updated from 0.4 to 0.75 across codebase
- Waveform selection thresholds recalibrated: AWGN < 0.15, Good < 0.75, Moderate < 1.10
- OFDM internal fading thresholds (LLR scaling, two-pass) NOT changed — they use
  pilot-variance-based `last_fading_index` on a separate scale

**Fading channel notes (2026-03-15):**
- Fading index now combines freq_cv + temporal_cv for better Good vs Moderate separation
- OFDM internal uses separate `last_fading_index` from pilot variance (~0.15-0.50)
- LLR scaling (1 + 10×fading²) applied when OFDM fading_index > 0.15 to prevent overconfident wrong bits
- DQPSK two-pass DISABLED; D8PSK two-pass threshold 0.30
- D8PSK two-pass uses `last_fading_index` from pilot variance (NOT computeFadingIndex() which returns 0 after sync)
- Light sync confidence threshold=0.8 (raised from 0.5) to reject marginal timing syncs on fading channels
- CFO drift limited to ±1 Hz per frame when connected (multipath can cause false CFO readings)
- **CFO feedback loop** (2026-02-03): Pilot-corrected CFO propagates back to StreamingDecoder's cached value
- **LTS residual CFO** (2026-02-03): Detects and corrects chirp CFO errors >0.3 Hz from training symbols
- **CPE correction for differential modes** (2026-03-15): Per-symbol common phase error tracking
  now enabled for DQPSK/D8PSK (was coherent-only). Estimates phase drift from pilots each symbol,
  clamps to ±15° for differential (prevents overcorrection from noisy fading pilots).
  This keeps channel_estimate phase tracking the actual channel, improving MMSE equalization.
  Safe for DQPSK: CPE cancels in diff = eq[n] × conj(eq[n-1]) since both use same corrected H.
- Good fading: **100% CW success** at R2/3 SNR=15 (10/10 seeds, enabled by CPE correction)
- Moderate fading: R1/4 avg 1.4 retx (5/5 seeds), R1/2 avg 2.4 retx (5/5 seeds) — up from ~89% CW

**OTFS/MFSK production status (2026-04-30):**
- OTFS prototype code was removed from the production build. The tested implementation was not competitive with OFDM_CHIRP on fading and required research-grade DD-domain equalization to justify keeping it.
- MFSK enum/capability values are reserved for wire compatibility, but there is no maintained production implementation. Do not advertise or negotiate MFSK.
- Default `ModeCapabilities::ALL` advertises implemented modes:
  OFDM_COX, MC_DPSK, OFDM_CHIRP, and OFDM_NARROW. This is a capability
  bitmap, not the auto-selection ladder. The production auto ladder chooses
  MC_DPSK below SNR 10 and OFDM_CHIRP at SNR 10+; it does not auto-select
  OFDM_COX.

**Recommendation:** Use OFDM_CHIRP with DQPSK. Rate selection is
automatic via `selectOFDMCodeRate()`; keep the exact thresholds in
`src/protocol/waveform_selection.hpp` and the boundary tests in
`tests/test_waveform_policy.cpp`.

**FFT note:**
- PocketFFT is vendored in `thirdparty/pocketfft/`.
- It is header-only and BSD-3 licensed; no external FFT runtime or package is required.
- Chirp detection remains FFT-accelerated for the small 512/1024-point modem transforms.

**StreamingDecoder design:**
- Expects audio fed at real-time rate (or close to it)
- Has overflow protection if audio fed faster than processing
- Mode switches reset correlation_pos_ to start searching new data

**Pending Improvements (2026-02-03):**
- **CFO Pre-Correction for LTS Sync:** Light preamble LTS sync detection still happens BEFORE CFO
  correction in StreamingDecoder. The LTS residual fix (in demodulator) corrects CFO after sync,
  but pre-correcting audio samples in StreamingDecoder would improve LTS detection reliability
  on fading+CFO channels. Cost: O(N) complex multiply per sample - very cheap.
- **Per-Symbol Pilot Tracking:** IMPLEMENTED and active in `channel_equalizer.cpp:424-694`.
  Every data symbol updates H via LS pilot estimation, alpha-smoothed tracking (alpha=0.8
  for differential w/ pilots), residual CFO tracking, and timing recovery from pilot phase slope.
  Pilots spaced every 10 carriers (~6 pilots across 59 carriers). R1/2 still struggles on
  moderate fading due to insufficient LDPC redundancy, not missing pilot tracking.

---

## Important Rules

- **Operate under the multi-perspective stack at all times.** All technical answers, design decisions, code reviews, and Codex briefs must be written from the *combined and mandatory* standpoint of:

  1. **PHY theorist** (primary) — PhD-level HF modem researcher: channel coding, OFDM/MC-DPSK theory, ARQ, channel estimation under fading, calibrated LLRs under a documented noise model, per-carrier SNR with documented reference, channel-reciprocity assumptions stated explicitly, information-theoretic limits.
  2. **Real-time DSP systems engineer** (mandatory secondary) — implementation discipline: fixed-/floating-point numerics, FFT/PLL/AGC/equalizer pipelines, buffer management, multi-threaded audio paths, lifecycle/state-machine correctness, resource cleanup across session boundaries, profiling and hot-path discipline.
  3. **Veteran HF operator** (mandatory tertiary) — field reality: ALC and audio gain staging on real radios, antenna mismatch, QSB/QRM, multipath QSO behavior, what an operator does between calls, what failure modes are tolerable vs unacceptable in a shift, defaults and UX at 2 AM in a noisy shack. **Two hard physical constraints that bound every timing/throughput/ARQ design — never violate them:**
     - **Half-duplex:** one channel, one frequency. A station CANNOT transmit and receive at once. The sender MUST stop transmitting (T/R turnaround) and go to RX to hear the ACK — so you can NOT "pipeline" data across the ACK gap the way a full-duplex link (TCP) does. The gap where the receiver sends its ACK is the *other station's turn to talk*, not reclaimable sender airtime. Efficiency comes from minimizing the NUMBER of turnarounds (longer bursts, infrequent coalesced ACKs) and the OVERHEAD per turnaround (realistic ~15–30 ms T/R, lean ACK), NOT from continuous transmission.
     - **PA duty cycle / thermal:** a real PA (esp. 100 W finals) cannot key down continuously — it overheats/derates. The rig must "catch air" (cool) between transmissions; data modes are duty-bounded. Bursts cannot be made arbitrarily long, and any throughput that depends on ~100% duty / near-continuous key-down is a HARDWARE-IMPOSSIBLE CHEAT — reject it. State the assumed duty-cycle ceiling explicitly and keep mandatory cooling/turnaround gaps. (A simulator that "achieves" throughput by ignoring half-duplex or PA duty is producing a number a real radio can never reproduce.)
  4. **First-principles physics escape hatch** — when the three perspectives disagree, fall back to the underlying physics/information-theory and let the model arbitrate.

  All three mandatory perspectives apply to *every* non-trivial change — not "pick the most relevant one." Heuristic patches and "tweak the threshold" fixes are tolerated only as labeled prototypes; the principled equivalent (justified under all three lenses) must replace them before merge. Quick prototypes are fine for exploration, but must be labeled as such and validated against the principled formulation before merge.

- **Codex must be invoked under the same multi-perspective stack.** Every brief sent to Codex (`/tmp/<topic>_findings.md` and `/tmp/codex_<topic>_prompt.txt`) must restate the four-tier stack (PHY theorist + DSP systems engineer + veteran HF operator + first-principles escape hatch) verbatim at the top, on every round including resumes, and instruct Codex to reject heuristic patches that don't have a principled justification under all three mandatory lenses. Code review requests (`codex review --uncommitted`) must include the same framing.

- **No guessing.** If you don't know an answer, say "I don't know" or "I need to verify X" — don't fabricate. Read the code, run the test, or ask the user. Be direct. Short answers > long ones with hedging. Length is not a proxy for quality.

- **Never mention specific competing products by name** (e.g., no "VARA", "ARDOP", "Winlink" etc.). Always refer to "industry leaders", "commercial HF modems", or "existing systems" instead.

- **MANDATORY: Document ALL fixes and changes** in `docs/CHANGELOG.md` with what was broken, what changed, how it's fixed, and test verification.

- **Track bugs in KNOWN_BUGS.md.** Add with unique ID (BUG-XXX). When fixed, move to "Fixed Bugs" section.

- **Read INVARIANTS.md before changing critical code.** Violating these causes subtle bugs.

- **Follow `docs/QUALITY_STRATEGY.md` for Tier 0/Tier 1 changes.** Do not chase fake coverage. Add meaningful tests for modem-critical behavior, remove stale code, and keep coverage gates reproducible.

- **Run the local quality gate before committing critical code:** `cmake --build build -j4 && ctest --test-dir build --output-on-failure -j4 && ./tests/regression_matrix.sh --quick && ./scripts/coverage_report.sh`

---

## SIMULATOR FIDELITY — Non-Negotiable

cli_simulator, OTASim, and channel models exist to test the modem **AS IF WE WERE A RADIO**. Every change must hold up to: *"would a real radio behave this way?"* When that fidelity breaks down, every protocol fix carries a hidden "works in simulator, unvalidated on hardware" caveat.

**Rules:**

1. **Review two layers, not one:**
   - **Code-level review:** look for places where the implementation doesn't reflect HF-radio physics — fixed-time turn-around delays, magic timing constants, immediate-fire responses without carrier sense, hardcoded noise thresholds, etc.
   - **Simulator-fidelity review:** look for places where the simulator diverges from real-radio behavior — push-vs-pull audio chain, missing ALC / AGC / PA modeling, channel-model gaps, state machines in modem-time vs wall-clock time, etc.

2. **Before every architectural change**, ask "is this how a real radio does it?" Reject the answer *"the simulator does it differently because it's a simulator."* That's the failure mode.

3. **Re-review fidelity when:**
   - A new module is added (channel model, audio path, state machine)
   - Hardware testing reveals a behavior the simulator didn't predict
   - A protocol fix ships with a "simulator-artifact-dependent" caveat
   - Before any major refactor of the simulator audio chain

4. **Real-radio reference points (verify against, do not invent):**
   - Soundcard: pull-based callbacks driven by hardware clock, bounded buffers (~50-200 ms)
   - T/R relay timing: typical 5-30 ms, specific to radio model (Icom IC-7300 ~15 ms, Yaesu FT-891 ~20 ms)
   - HF channel models: ITU-R F.1487, CCIR Report 549 propagation tables
   - Hamlib CAT latency: 100-500 ms per command
   - AGC settling, ALC peak compression, PA saturation: all real physics that must be modeled, not idealized away

5. **A test that passes in simulator but fails on hardware = simulator bug.** Fix the simulator, not the test. Document the fidelity gap if the simulator can't yet model it.

The mission: when a fix passes the simulator gate, it should pass the hardware gate too.

---

## Essential Documentation

### Priority 1: Always Read First
| Document | Purpose |
|----------|---------|
| `docs/PROJECT_GOALS.md` | Mission, priorities, throughput/reliability targets, and agent task filter |
| `docs/AGENT_CURRENT_STATE.md` | Current agent-system state and compact handoff |
| `docs/QUALITY_STRATEGY.md` | Critical-software testing, coverage, CI, and refactor policy |
| `docs/QUALITY_AUDIT.md` | Current quality baseline, coverage gaps, and hardening backlog |
| `docs/KNOWN_BUGS.md` | Active bugs - DON'T rediscover these |
| `docs/INVARIANTS.md` | 25 critical rules that MUST NOT be violated |
| `docs/ALPHA_RELEASE_GATE.md` | Release criteria and seeded gate commands |
| `docs/CHANGELOG.md` | History of all fixes - DON'T redo this work |

### Priority 2: Read When Working on Subsystem
| Document | Purpose |
|----------|---------|
| `docs/CFO_CORRECTION_FLOW.md` | **CRITICAL** - 4-stage CFO flow, fading fix, feedback loop |
| `docs/PROTOCOL_V2.md` | Frame formats, protocol flow |
| `docs/GUI_ARCHITECTURE.md` | ImGui widgets, threading, virtual station |
| `docs/AUDIO_SYSTEM.md` | SDL2 audio I/O, buffers, latency |
| `docs/CONFIGURATION_SYSTEM.md` | AppSettings, ModemConfig, presets |
| `docs/BUILD_SYSTEM.md` | CMake, dependencies, tests, coverage script |
| `docs/AGENTIC_DEVELOPMENT.md` | Bounded agent workflow, permissions, gates, and review process |

### Priority 3: Reference
| Document | Purpose |
|----------|---------|
| `docs/BUILD_SYSTEM.md` | CMake, dependencies, adding components |
| `docs/ADDING_NEW_WAVEFORM.md` | Step-by-step guide for adding future waveform implementations |
| `docs/GIT_WORKFLOW.md` | Commit strategy, branching, push policy |
| `docs/README.md` | Documentation map; archive docs are historical only |

---

## Quick Reference

### Build
```bash
mkdir build && cd build
cmake ..
make -j4
```

### GUI Application
```bash
./ultra_gui              # Normal mode
./ultra_gui -sim         # Developer mode with virtual station
./ultra_gui -sim -rec    # With audio recording
```

### Test Tools
| Tool | Purpose | Example |
|------|---------|---------|
| `cli_simulator` | **PRIMARY** - Full protocol with two-station interaction | `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test` |
| `test_waveform_simple` | Quick single-frame sanity check | `./build/test_waveform_simple -w ofdm_chirp --snr 15` |
| `tests/regression_matrix.sh` | Maintained CTest wrapper | `./tests/regression_matrix.sh --quick` |

**Testing priority:** Use CTest for unit/regression gates and `cli_simulator` for full real testing (handshake, light preamble, data transfer, ARQ). Use `test_waveform_simple` only for quick single-frame sanity checks.

### CLI Commands
```bash
# Send PING probe
./ultra ptx ping -s MYCALL | aplay -f FLOAT_LE -r 48000

# Send message
./ultra ptx "Hello World" -s MYCALL -d THEIRCALL -o msg.f32

# Decode recording
./ultra prx recording.f32           # OFDM
./ultra prx -w dpsk recording.f32   # DPSK/PING
```

---

## Waveform Summary

| Mode | Sync Method | In-band SNR floor | Max Throughput | CFO Tolerance | Fading |
|------|-------------|-----------|----------------|---------------|--------|
| **MC-DPSK** | Dual Chirp | 5 dB AWGN (post-2026-05-19 audit) | 938 bps | ±50 Hz | Good |
| **OFDM_NARROW** | NB Chirp + LTS | ~17 dB AWGN (pre-audit) | ~450 bps (R1/2, window=3) | ±50 Hz | Good (R1/4) |
| **OFDM_CHIRP** R1/4 | Dual Chirp + LTS anchor, warm LTS data | 10 dB AWGN, 15 dB Good | 3.4 kbps | ±50 Hz | Good (R1/4) |
| **OFDM_COX** | Schmidl-Cox | Forced only | 7.9 kbps | Needs testing | Poor |
| **SC-DPSK** | Barker-13 | -8 to -3 dB | 125 bps | N/A | Good |

**Waveform Selection:**
- Poor HF channels (2ms delay): Use MC-DPSK
- Low SNR (5-10 dB) AWGN: MC-DPSK handles down to 5 dB; OFDM_CHIRP R1/4 from 10 dB up
- Moderate/Good HF: Use OFDM_CHIRP; OFDM_COX remains explicit forced/legacy only
- OFDM_NARROW retained for narrowband-detected paths
- OTFS/MFSK values are reserved only and are not production-supported

---

## Architecture Overview

```
src/
├── gui/modem/          # ModemEngine - TX/RX audio processing
├── ofdm/               # OFDM modulator/demodulator
├── psk/                # Single/Multi-carrier DPSK
├── fec/                # LDPC encoder/decoder (648-bit codewords)
├── protocol/           # Protocol v2 (PING/CONNECT/DATA/DISCONNECT)
├── sync/               # ChirpSync, Schmidl-Cox sync
└── waveform/           # IWaveform interface and implementations

tools/
├── cli_simulator.cpp   # Full protocol test
└── test_waveform_simple.cpp # Quick waveform sanity checks
```

**Key Files:**
- `src/sync/chirp_sync.hpp` - Dual chirp detection + CFO estimation
- `src/gui/modem/modem_rx_decode.cpp` - RX decode logic
- `src/psk/multi_carrier_dpsk.hpp` - MC-DPSK modulator/demodulator

---

## Key Specifications

| Parameter | Standard Mode | NVIS Mode |
|-----------|---------------|-----------|
| Sample Rate | 48,000 Hz | 48,000 Hz |
| Center Frequency | 1,500 Hz | 1,500 Hz |
| Bandwidth | ~2.8 kHz | ~2.8 kHz |
| FFT Size | 512 | 1024 |
| Carriers | 30 | 59 |
| Max Throughput | 3.4 kbps | 7.2 kbps |

---

## Known Limitations

1. **OFDM_COX default policy:** Forceable implementation exists, but it is
   not part of the production auto ladder until separately validated.
2. **Poor HF channels (2ms delay):** OFDM fails - use MC-DPSK instead
3. **MC-DPSK floor:** in-band 5 dB AWGN (3/3 seeds cli_simulator + OTASim
   `OTASimulatorTwoEndpointMCDPSKLowSNR` fixture, post-2026-05-19 audit).
   Below 5 dB is currently un-explored as a separate workstream
   (MC-DPSK speed ladder for sub-0 dB SNR remains future work).
4. **File transfer:** DATA_START/DATA_END not fully implemented

---

## Protocol v2 Flow

```
Station A                          Station B
---------                          ---------
1. PING (1s) -------------------->
   <------------------------ PONG (1s)

2. CONNECT (DPSK) --------------->
   <------------------ CONNECT_ACK (DPSK)

3. MODE_CHANGE ------------------>  (SNR-based negotiation)
   <--------------------------- ACK

4. DATA ------------------------->  (negotiated waveform)
   <--------------------------- ACK

5. DISCONNECT ------------------->
   <--------------------------- ACK
```

---

## Development Workflow

### Before Making Changes
1. Read `docs/PROJECT_GOALS.md` and confirm the task aligns with the mission
2. Read `docs/INVARIANTS.md` for the subsystem you're touching
3. Check `docs/KNOWN_BUGS.md` for related issues
4. For agent work, use `docs/AGENT_TASK_BACKLOG.md` and one queued task file

### After Making Changes
1. Run `./build/cli_simulator --snr 15 --fading good --rate r1_4 --test 2>&1 | tee /tmp/test_output.log`
2. If you fixed a bug: Add entry to `docs/CHANGELOG.md`
3. If you discovered a bug: Add entry to `docs/KNOWN_BUGS.md`
4. If project/agent state changed materially: Update `docs/AGENT_CURRENT_STATE.md`, `docs/QUALITY_AUDIT.md`, or `docs/AGENT_TASK_BACKLOG.md` as appropriate

### Commit Message Format
```
Short description (imperative mood)

- What was changed
- Why it was changed

Fixes: BUG-XXX (if applicable)
```
