# 08 — Stale claims & new bug-smells found by this audit

Per project rules ("treat comments/docs/constants as claims to verify"), everything
below was code-verified on 2026-06-12. Items marked 🐛 are candidate KNOWN_BUGS entries;
items marked 📜 are doc corrections (fix CLAUDE.md / the named doc / the infrastructure
map together, per the keep-the-map-live rule).

## 🐛 Code bug-smells (new, from this audit)

1. **R5/6 pilot-spacing fall-through** — `recommendedPilotSpacing` has no `R5_6` case;
   the top ladder rung gets dense "rough channel" pilots → +1.5% instead of +11% over
   R3/4 (`include/ultra/ofdm_link_adaptation.hpp:46-64`; ladder rung added 05-28
   `rate_controller.hpp:79-94`). See 05 §2 for the fix + TX/RX sync caveat.
2. **Sticky escape-drop ceiling never reset (uncommitted WIP)** —
   `RateController::noteRungFailed()` sets a never-re-probed ceiling;
   `rate_controller_.reset()` has zero call sites; one transient fade caps the session
   forever (`rate_controller.hpp:188-199`, `connection.hpp:619`). Fix before commit.
3. **Phantom re-anchor in the burst budget model** — the model charges (n−1)×100 ms
   for a short re-anchor the encoder no longer emits ("R4: removed",
   `streaming_encoder.cpp:139` vs `connection_policy.hpp:461-483`) → groups pinned at
   5 frames where 6 fit the 8600 ms ceiling.
4. **Tone-ACK SNR staircase unwired on TX** — detector scans 12 ms/sym first, constants
   exist, but every TX call uses the 25 ms default arg → 675 ms ACKs at 20 dB where
   324 ms would decode (`modem_engine.hpp:97-100`, `app.cpp:626`,
   `tone_burst_ack_monitor.hpp:54-59`).
5. **`ULTRA_LDPC_Z=81` env footgun** — bypasses the unified-path gate that the
   surrounding comment says is required; would desync CW geometry
   (`connection.cpp:3032,3065-3096`). Neuter or gate it.
6. **CARRIER_ADAPTIVE_K is constant-modulus-biased** — the per-carrier |equalized| EMA
   "instability" inflation misreads 16QAM's 3 amplitude rings as channel instability:
   systematic ~2× noise-var inflation on every carrier even on clean AWGN, while losing
   fading contrast exactly for the modulation that needs it
   (`ofdm_symbol_demap.cpp:303-346`, `demodulator_constants.hpp:97`). The dd_qam16
   adaptivity-smell pattern, again.
7. **16QAM excluded from the relative-fade null gate** — `relativeFadeNoiseInflation`
   (the only deep-relative-null LLR deflation at high mean SNR) is scoped to QPSK/QAM8
   only (`channel_equalizer_equalize.cpp:450-456`) — the modulation that dies on
   relative nulls has the weakest null protection. (Historical reason: stacking
   regressed AWGN@30 — re-test together with the 02 calibration fix.)
8. **LLR scale conventions inconsistent across demappers** — QPSK exact; 8PSK 4×
   overconfident (scale 2/nv vs exact 1/(2nv)); QAM16 ~3.16× (missing 1/√10 factor)
   (`soft_demap.hpp:56,79,91-99`). Benign to scale-invariant min-sum EXCEPT it hits the
   MAX_LLR=20 clip 3-4× earlier, destroying inter-carrier CSI dynamic range at high SNR.
9. **MMSE bias uncorrected for amplitude bits** — equalizer output mean is β·x,
   β=|H|²/(|H|²+σ²); PSK sign bits cancel β, but the 16QAM ring slicer threshold stays
   at 2/√10 while the constellation shrinks by β → deterministic ring-bit errors on
   low-γ carriers with zero noise (`channel_equalizer_equalize.cpp:506`,
   `demodulator_constants.hpp:104`).
10. **Harness watchdog vs future modulation ladder** — a legitimate QPSK→8PSK promotion
    would be killed as `unexpected_data_mode` (`tools/gui_qso_scenario.sh:216-231`).

## 📜 Stale numerology / doc corrections

1. **Three symbol-length models coexist**: air-true 1120 samples (CP=96,
   `types.hpp:327-343`, what the modulator/demodulator radiate); protocol model 1152
   (CP=128, `connection_policy.hpp:19-21`, `waveform_selection.hpp:68` — all
   budget/timeout math ~2.9% pessimistic); dead fallback 1280
   (`ofdm_chirp_waveform.cpp:1078-1083`). Comments claiming "MEDIUM at 1024 FFT = 256
   samples = 5.3 ms" are false (`types.hpp:276,416`).
2. **`docs/COMPETITIVE_BENCHMARK_TARGET.md:69-76` raw-rate table is stale** — it
   reverse-engineers exactly to the retired DQPSK-era geometry (1152-sample symbols,
   legacy 4/6-pilot layouts). Honest current numbers: QPSK R3/4 **3279**, 8PSK R3/4
   **4918**, 16QAM R3/4 **6557** raw (see 01 §3).
3. **CLAUDE.md stale facts**: "Standard FFT=512/30 carriers" (production wideband flies
   1024/59 — the "NVIS" column); OFDM entry floors "AWGN 10/Good 12/Poor 18" (code:
   8/10/14/1e9, `waveform_selection.hpp:29-38`); "per-symbol pilot tracking…residual
   CFO + timing recovery; pilots ~every 10 carriers" (CFO tracking compiled out
   `pilot.cpp:787`, no timing recovery, spacing 5/8); "in-band SNR (3 kHz noise BW)"
   (implemented reference bandwidth = RX-FIR ENBW **2606.09 Hz** → our 20 dB =
   3 kHz-convention 19.39 dB, `docs/CALIBRATION_AUDIT.md:289`).
4. **`rate_controller.hpp:9-11`** — says the receiver runs the controller; the sender
   does (`connection.cpp:1703-1705`).
5. **`waveform_selection.hpp:424-441`** — quotes retired cli_simulator D8PSK sweeps and
   a "~5 kbps" figure exceeding the rung's own raw ceiling; `connection_policy.hpp:534`
   similar. Dead text — delete on next touch.
6. **z=81 in-code claims wrong**: "~3 dB more FEC margin" (`frame_v2.hpp:565-568`;
   finite-length theory says ~0.5-0.8 dB) and "1.8× the coherence interval"
   (`connection.cpp:3068-3070`; it's ~0.1×Tc at Good's 0.1 Hz by the code's own
   formula). Also `gui_qso_scenario.sh`'s comment "ULTRA_LDPC_Z → derived by the
   traffic-class policy (81 for file bursts)" is stale — unified path pins z=27.
7. **Legacy `PilotConfig::forCodeRate` (4/6 pilots, `types.hpp:178-236`)** — dead on
   the OFDM path (zero references in src/ofdm, src/waveform). Candidate for
   REMOVAL_BACKLOG with KEEP-note for any non-OFDM users.
8. `llr_sign_flip` permanently false (`ofdm_symbol_demap.cpp:290`) — vestigial.
9. `interpolateChannel()` (DFT/CIR denoise) declared, never called; `TIMING_ALPHA`,
   `FREQ_OFFSET_ALPHA` dead on the production path (`demodulator_impl.hpp:346`,
   `demodulator_constants.hpp:67-74`).

## 🐛 Live-run finding (2026-06-12)

0. **`ULTRA_LLR_NOISE_EMP_FLOOR` knob path implicated in an ultra_gui segfault.**
   Forced 16QAM R1/2 Good@20 seed 42 WITH `ULTRA_LLR_NOISE_EMP_FLOOR=1.0`: ALPHA
   segfaulted at ~76 s, mid control-profile peek decode, right after
   "LTS channel estimate: 47 data + 12 pilot carriers"
   (run dir `/tmp/fable_q16_r12_floor`, log preserved). The IDENTICAL scenario without
   the knob ran clean to PASS (`/tmp/fable_q16_r12_base`). `hardDecision()` itself is
   total over modulations and the floor loop is size-guarded
   (`channel_equalizer_equalize.cpp:546-561`), so the crash mechanism is not obvious
   from reading — needs a debugger/ASAN session before anyone uses the knob. Since the
   knob is a default-off diagnostic, severity is low, but it blocks the EMP_FLOOR A/B
   experiments this folder recommends. (See also the 8PSK floor run in 07 — second
   discriminator data point.)

## Additional findings from the adversarial verification pass

- **`measure_ack_fer` air symbol is 1152 (CP=LONG both sides, `measure_ack_fer.cpp:226`)**
  — the offline harness matches the protocol model, NOT the production GUI air (1120).
  Offline anchors carry ~2.9% extra CP overhead + slightly more multipath margin.
- **`StreamingEncoder` constructor default is CP=LONG** (`streaming_encoder.cpp:69`);
  production always overwrites via `setOFDMConfig` (`modem_engine.cpp:81`), but any
  future direct user that skips it silently transmits 1152-sample symbols.
- **Production tone-ACK monitor scans {25 ms} only** (`streaming_decoder.cpp:176-178`);
  the multi-duration default config is test-only. Comments claiming the tone-ack route
  is "default off" are stale — `kInteractiveToneAckEnabled()` is hardcoded true
  (`connection.cpp:31,233-237`).
- **`use_coherent_dd` definition mismatch**: the equalize.cpp observation side runs
  QAM8||QAM16 unconditionally (`equalize.cpp:575-576`) while the pilot.cpp consume side
  is fading-gated (`pilot.cpp:890-903`) — observations are produced and discarded on
  fading; harmless but wasteful/confusing.
- **Budget still charges the removed short re-anchor** per continuation frame in ARQ
  airtime math (`connection.cpp:3132-3136`) — same phantom as the group-size pin.
- **Tone-ACK reserved bits (28..31) are outside the CRC-12** (covers bits 0..15 only,
  `tone_burst_payload.hpp:59-60`) — widening frame_mask needs a CRC redefinition.
- Stale pointer: `connection_policy.hpp:73` cites connection.cpp:438 for the 0x3F mask
  truncation (actual ~:245-246).

## Follow-up bookkeeping (for whoever lands fixes)

- New bugs → `docs/KNOWN_BUGS.md` with BUG-IDs; doc fixes → respective docs +
  `docs/MODEM_INFRASTRUCTURE_MAP.md` in the SAME change (mandatory project rule).
- The stale-claim corrections in CLAUDE.md §Performance-floors and §Key-Specifications
  should cite this register.
