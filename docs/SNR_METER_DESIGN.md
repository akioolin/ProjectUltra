# Calibrated Absolute-SNR Meter — Design Doc

## Multi-perspective stack (mandatory, verbatim per CLAUDE.md)

All work in this workstream operates from the four-tier stack:

1. **PHY theorist (primary)** — channel coding, OFDM/MC-DPSK theory, ARQ
   under fading, calibrated LLRs under a documented noise model,
   per-carrier SNR with documented reference, information-theoretic
   limits.
2. **Real-time DSP systems engineer (mandatory secondary)** —
   implementation discipline.
3. **Veteran HF operator (mandatory tertiary)** — what a real operator
   reads off a real radio's S-meter; what number is useful at 2 AM in a
   noisy shack.
4. **First-principles physics escape hatch** when the three disagree.

Heuristic patches without a principled justification under all three
mandatory lenses are not acceptable for merge.

## The problem (what we know today, 2026-05-14)

The modem currently has three SNR-shaped numbers, **none of which is a
calibrated absolute broadband-SNR meter**:

| Source | Behavior | Bias observed today |
|--------|----------|---------------------|
| `chirp_snr` | 1 s dual-chirp correlation → linear map → clamp `[-5, 30]` dB. Has ~47 dB processing gain so the correlation saturates above ~5 dB honest broadband SNR. | Reads 25–28 dB across honest SNR +5..+25. Saturated. |
| `pilot_snr` | Per-symbol pilot-residual variance accumulator (`channel_equalizer_pilot.cpp:460`). | Probe AWGN: +3.5 dB high. Protocol AWGN DATA: ~1 dB low. Protocol handshake: 5–8 dB low. |
| `lts_snr` | LTS training-symbol noise estimate (`estimatedSNR()` on the OFDM waveform). | Probe AWGN: +9 dB high. Protocol AWGN DATA: +4-7 dB high. Protocol Good15 DATA: varies 11–24 dB (instantaneous fade). |

Today's `frame.rx.snr_db` is the saturated chirp value. The mode-ladder
in `selectOFDMCodeRate(snr_db, fading_index)` discriminates almost
entirely on `fading_index` (which **is** calibrated and honest) — the
`snr_db` threshold gate never actually triggers because the value is
always pinned at the top of its clamp.

Consequences today:

- Operator-facing GUI shows a meaningless number.
- MODE_CHANGE handshake exchanges meaningless SNR (both endpoints lie
  symmetrically, so it doesn't break agreement, but no decisions can be
  made on it).
- Any future feature that needs honest absolute SNR (per-carrier bit
  loading, mixed-modulation, dynamic ARQ tuning, threshold-driven
  recovery strategies) is blocked.

The 2026-05-14 Phase 2+5 experiments confirmed that switching
`frame.rx.snr_db` to either pilot or LTS would only swap one wrong
reading for a different wrong reading. **The fix is not a substitution;
it is a measurement-quality project.**

## What "broadband channel SNR" must mean (PHY-theorist definition)

To call any number an honest SNR, the noise model must be documented.
The proposed reference, consistent with the channel-RMS calibration
(commits `1036969`, `8015495`):

- **Signal power reference.** TX RMS at `kModemReferenceRms = 0.3180724`
  (already measured from `StreamingEncoder::encodePing()` and locked).
- **Noise integration bandwidth.** 2.8 kHz (the modem's RF bandwidth),
  not the 24 kHz Nyquist. Operators read 2.4–3.0 kHz BW SNR on real
  radios; that is the comparable number.
- **Signal-to-noise ratio.** `SNR_dB = 10 · log10(P_signal / P_noise)`
  where both powers are measured in the same 2.8 kHz reference band.

Any measurement must trace explicitly back to these definitions or its
numbers are not interpretable.

## Architecture options

Two viable estimator architectures. Either one (or a hybrid) can hit
the calibration gate; choose during prototyping.

### Option A — Idle-noise estimator

1. Classify the current audio window as "idle" (no chirp lock, no
   MC-DPSK preamble, no OFDM sync). Reuse the chirp-locked + LDPC-failed
   classifier already shipped for `BUG-PING-DETECTOR-001`.
2. When idle: integrate audio RMS over a configurable window
   (e.g. 200 ms) in the 2.8 kHz reference band.
3. Compare to `kModemReferenceRms` to compute `SNR_dB`.
4. Smooth with an exponential moving average across idle windows.

**Pros:** independent of the demod chain; works during PING/CONNECT/
silent intervals; matches an operator's intuition ("what's the band
doing while we're not talking").

**Cons:** doesn't give a per-frame number; lags by one idle window; can
be confused by carriers from other stations sharing the band.

### Option B — Pilot/LTS normalized to broadband

1. Take the per-carrier residual variance already accumulated in
   `channel_equalizer_pilot.cpp` / `channel_equalizer_lts.cpp`.
2. Correct for OFDM processing gain: `10 · log10(N_subcarriers /
   N_active_pilots)`, and FFT-vs-symbol-energy normalization.
3. Correct for pilot-symbol amplitude (pilots are typically a different
   reference power than data symbols — this asymmetry is part of why
   today's `pilot_snr` is biased).
4. Yield a per-frame broadband SNR estimate.

**Pros:** per-frame value; useful for adaptive logic; works during
DATA bursts.

**Cons:** requires careful derivation of all normalization factors;
needs the pilot-power-vs-data-power asymmetry verified empirically;
sensitive to ICI and residual CFO contamination.

### Recommended path

Prototype both. Ship the simpler one first (probably A — idle-noise) as
the operator-display value. Add B as a per-frame refinement once
calibration is locked, used for adaptive logic and convergence
acceleration.

## Six-step workplan

### Step 1 — Document the model
File: `docs/SNR_METER_DESIGN.md` (this doc) — refine with exact
formulas during prototyping. Reject any implementation that does not
trace back to a named formula in this doc.

### Step 2 — Build the estimator(s)
Implement under feature flag (CMake option or runtime flag) so the
operator-facing field can be A/B tested without affecting production
decode. Keep the pilot/LTS diagnostic accessors that already shipped
(`getLastSNREstimate()`, `lts_snr_db`).

### Step 3 — Build the calibration gate
Pattern: `tests/test_channel_snr_calibration.cpp` already exists for
the **channel** side (proves `SimulatedChannel` applies the configured
SNR within ±1.5 dB). A new `tests/test_modem_snr_meter_calibration.cpp`
should:

1. Use the `ChannelSNRProbe` instrument to set channel SNR ∈
   {-5, 0, 5, 10, 15, 20} dB.
2. Run a fixed waveform through the modem.
3. Read the new estimator's output.
4. Assert the reading is within ±1.5 dB of the configured value across
   AWGN, Good, and Moderate Watterson channels.
5. Add as `ChannelModemSNRMeterCalibration` to the CTest suite.

A test that's not under CI lock is not load-bearing — see the
`coverage_report.sh` discipline already in place.

### Step 4 — Per-channel-type validation
Once CI gate passes AWGN: extend to Good (1 Hz Doppler, 0.5 ms delay)
and Moderate (1 Hz Doppler, 1 ms delay) Watterson presets. Allow a
documented ±3 dB tolerance under fading (instantaneous fade depth
limits how tight the bound can be — this is honest physics, not
sloppy code). Report bias and variance per cell.

### Step 5 — Protocol-context validation
Calibration probes are not enough — the QSO state machine has multiple
phases (PING, CONNECT, MODE_CHANGE, DATA) and today's evidence shows
the LTS estimator reads differently in handshake vs DATA phases. The
test must:

1. Run `cli_simulator` with each channel × {AWGN/Good/Moderate} × SNR
   ∈ {5, 10, 15, 20} dB.
2. Grep `frame.rx.snr_db` from per-frame logs by protocol phase
   (handshake vs DATA).
3. Assert the operator-facing value is honest (within the documented
   tolerance) in **all** phases.

If the estimator can't deliver a sane reading during handshake (e.g.
because the OFDM LTS hasn't run yet), the chirp-derived value must be
used as an honest fallback for that phase only, **and the GUI must
indicate it's a coarse estimate**.

### Step 6 — Hardware validation
Real radio path:
- Mac↔Pi5 calibrated cable: confirm the estimator matches the
  simulated channel within ±2 dB (cable adds ~1 dB of soundcard noise).
- Real radio with ALC enabled: document the bias caused by ALC's
  compression (the KC3VPB pushback in memory notes this is a real-
  world concern). The meter should be honest about what it reads,
  even if ALC distorts it.
- Add hardware-smoke cell with explicit SNR-reading assertion to
  `agents/run_hardware_smoke.sh` (extension of today's coverage).

## Acceptance criteria

A merge of the calibrated-meter branch is acceptable only if:

1. `ChannelModemSNRMeterCalibration` CTest gate passes ±1.5 dB on
   AWGN, ±3 dB on Good/Moderate fading.
2. Protocol-context validation (Step 5) shows operator-facing SNR within
   the documented tolerance during DATA phase in `cli_simulator` cells.
3. Pi5 hardware smoke (`agents/run_hardware_smoke.sh`) passes 3/3 and
   the new SNR-reading assertion confirms honest readings within ±2 dB
   on the calibrated cable.
4. No mode-ladder thresholds were auto-tuned. If the new honest SNR
   would re-bucket existing cells, that's a separate threshold-tuning
   workstream with its own Codex review and own multi-seed validation.
5. The estimator's formula is documented end-to-end in this design
   doc (the doc and the code must agree).

## Known traps to avoid

- **Don't trust a single sample.** Today's confusion came from a few
  log lines that happened to be in one favorable regime. The
  calibration must be validated across cells and seeds.
- **Don't conflate "correlation" with "accuracy".** Phase 1 showed
  r=0.999 correlation on AWGN, but the absolute value was off by 9 dB.
  Correlation alone is not enough.
- **Don't ship a heuristic offset.** A `+9 dB fudge factor` would
  "fix" AWGN and break Good fading. The bias is channel-dependent;
  the fix must address the underlying measurement, not patch it.
- **Don't auto-tune thresholds.** The mode-ladder thresholds were
  calibrated against the old saturated value. If the honest SNR
  changes the bucket boundaries, that's a deliberate, separately
  validated decision.

## Related artifacts

- `docs/HONEST_SNR_ESTIMATION.md` — Phase 1-5 experiments (what we
  tried 2026-05-14, why pilot and LTS substitutions were reverted).
- `tools/ofdm_snr_probe.cpp` — diagnostic probe; useful for Step 4.
- `src/sim/channel_snr_probe.hpp` — channel-side calibration probe;
  needed for Step 3.
- `tests/test_channel_snr_calibration.cpp` — pattern to copy for
  Step 3.
