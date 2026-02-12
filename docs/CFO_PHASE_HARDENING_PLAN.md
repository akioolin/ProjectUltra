# CFO Phase Hardening Plan (Pre-Demod Correction Path)

Date: 2026-02-12
Status: Completed (Phase 1, Phase 2, and Phase 3 implemented and validated)
Owner: DSP/Modem path

## Objective

Harden CFO correction for connected OFDM flows so chirp/LTS-derived CFO is applied with the correct phase reference before demodulation, and maintain consistency when residual CFO triggers LTS reprocessing.

Primary target: reduce sync-induced decode failures and control-path ACK loss driven by CFO/phase mismatch on fading channels.

## Current Findings (Code Reality)

1. CFO is already corrected before demod in OFDM paths:
   - `OFDMChirpWaveform::process()` calls `setFrequencyOffsetWithPhase()` before `processPresynced()`.
   - `OFDMDemodulator::Impl::toBaseband()` applies per-sample phase rotation using `freq_offset_hz` and `freq_correction_phase`.

2. The absolute training-position hook exists but is not wired:
   - `IWaveform::setAbsoluteTrainingPosition(size_t)` exists.
   - `StreamingDecoder` never calls it.
   - OFDM waveforms currently compute initial phase from buffer-relative training offsets.

3. Residual CFO reprocess path resets phase origin to zero:
   - In `estimateChannelFromLTS()`, if residual CFO is detected, code reprocesses training after `mixer.reset(); freq_correction_phase = 0.0f;`
   - This can lose the initial phase baseline for the current frame.

## Implementation Plan

### Phase 1: Absolute Training Position Plumbing (Implement now)

1. `StreamingDecoder`:
   - On successful sync, compute absolute sample index for `sync_position_`.
   - Call `waveform_->setAbsoluteTrainingPosition(abs_sync_training_start)`.
   - Keep this per-frame and independent from ring-buffer wrap.

2. `OFDMChirpWaveform` and `OFDMNvisWaveform`:
   - Override `setAbsoluteTrainingPosition(size_t)`.
   - Store absolute training start for phase initialization.
   - In `process()`, use absolute training sample for initial CFO phase.
   - Fallback to legacy relative offset only if absolute position is not provided.

Acceptance:
- Build succeeds.
- Logs show absolute phase reference being used in OFDM process path.
Implementation note (2026-02-12):
- Done. `StreamingDecoder` now maps ring index -> absolute sample index at sync and calls `waveform_->setAbsoluteTrainingPosition(...)`.
- Done. `OFDMChirpWaveform` and `OFDMNvisWaveform` now consume absolute training position for initial CFO phase.

### Phase 2: Residual CFO Reprocess Phase Baseline (Implement now)

1. `channel_equalizer.cpp`:
   - Capture `freq_correction_phase` at training-start before first LTS pass.
   - If residual CFO correction triggers reprocess, reset mixer and restore that captured phase baseline (not zero).

Acceptance:
- Build succeeds.
- Residual CFO correction path preserves phase continuity assumptions.
Implementation note (2026-02-12):
- Done. LTS residual CFO reprocess now restores the captured training-start phase baseline instead of forcing phase=0.

### Phase 3: Connected LTS Sync Metric Upgrade (Next step)

1. Replace/augment current real-only training autocorrelation in `detectDataSync()` with CFO-aware metric (complex/phase-compensated), rather than cosine-only normalization.
2. Validate detection robustness under ±20..30 Hz CFO and fading.

Acceptance:
- Lower sync reject streaks on fading + nonzero CFO.
- No regression in clean AWGN runs.
Implementation note (2026-02-12):
- Done. `OFDMChirpWaveform::detectDataSync()` now uses Hilbert/analytic complex correlation for LTS detection instead of real-only autocorrelation.
- Done. Burst-marker sign detection now uses CFO-phase-compensated complex correlation (`best_p * exp(-j*phi_cfo)`), improving robustness when CFO is non-zero.
- Quick sanity validation (5 seeds, SNR=20, good fading, R2/3): 5/5 PASS.

## Validation Matrix (Post-Implementation)

1. Simulator (connected OFDM-CHIRP):
   - SNR 20, fading good/moderate, rate DQPSK R2/3, seeds 30.
   - Add nonzero CFO scenario (fixed and random, if exposed in harness).

2. Metrics to compare:
   - First-attempt frame success.
   - ACK reception success.
   - Retransmissions/timeouts.
   - Sync reject streak count.
   - Logged CFO evolution (chirp -> residual -> pilot corrected).

3. OTA sanity check:
   - Confirm `[MODE]` / logs do not show unstable CFO jumps frame-to-frame under stable channel.

## Risks / Notes

1. Do not apply a second external pre-rotation in front of OFDM demod unless carefully gated; OFDM demod already performs CFO correction internally.
2. MC-DPSK path is separate and should not be changed by this phase plan.
3. Keep behavior backward-compatible for paths that do not provide absolute training position.

## Completion Evidence (2026-02-12)

1. Deterministic chain verification:
   - `./tests/verify_cfo_chain.sh --cfo 50 --channel awgn --snr 20 --seed 42`
   - Result: pass, with applied correction matching expected `-CFO` within tolerance across all dumps.
2. Regression sanity:
   - `./build/cli_simulator --snr 20 --fading good --rate r1_2 --seed 42 --test`
   - Result: pass (no regression from hardening changes).
3. Internal proof path:
   - `ULTRA_DUMP_CFO_PREFIX` + `ULTRA_DUMP_CFO_CALLS` dump pre/post corrected samples from `toBaseband()` for direct offline validation.
