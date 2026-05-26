# Chain-Validation Harness — Scope (2026-05-26)

## Why
~6 months of AI iteration on this codebase means subtle, compensating breakage may
exist that end-to-end pass/fail hides (a high-SNR LDPC margin masks an estimator
error; a downstream band-aid hides an upstream bug). We need a **re-runnable gate that
proves the signal chain is sound across the whole matrix** (rate × waveform × mod ×
channel × SNR), not a single QSO that returns one number.

Trigger: a worry that the σ² LLR fix (or anything else) silently broke part of the
chain. First probe already found the *measurement tool* `test_waveform_simple` lying
(phantom leading-CW failures on every OFDM frame) — see
`project_test_waveform_simple_cw0_artifact`. **Validate the instrument before trusting it.**

## What already exists (build ON, do NOT rebuild)
- `test_watterson_proof` — channel model to ITU-R F.1487 (EXCELLENT).
- `test_layers` — per-stage FFT/IFFT, mixer, sync, channel-est.
- LDPC encode↔decode identity, interleaver round-trip, FFT identity, soft-demap sign/mag.
- SNR-calibration tests (±1.5 dB), Wiener-interpolator MSE on known smooth channel.
- `cli_simulator --diag-genie-{channel,sigma,timing,no-clip}` + `--diag-attribution`
  + `ULTRA_FAILURE_ATTRIBUTION=1` — per-stage oracle taps, BUT **QAM16-only and the
  channel genie is invalid LS** (untrusted; see `project_qam16_failure_attribution_2026_05_25`).

## The 5 gaps (where today's worry lives)
1. No unified, re-runnable **chain-integrity gate** — pieces are scattered ctests + ad-hoc flags.
2. No **clean-channel bit-exact identity sweep** across the full mod×rate matrix on the *trusted* path.
3. No **LLR reliability diagram** — does |LLR| match the actual bit error rate? (the stage the σ² fix changed; nothing validates it).
4. Existing **genies are QAM16-only / partly broken** — must be fixed + validated before trust, and generalized off QAM16.
5. Production **dense-comb channel estimator** has no known-H MSE oracle; **CFO estimator** has no known-CFO sweep.

## Phased plan
- **Phase 0 (START NOW, Claude):** clean-channel identity gate on the trusted `cli_simulator`
  path. Force every {mod}×{rate} at high-SNR AWGN, 0 CFO; assert full delivery, failed=0,
  retx≈0, no CW failures. Anything failing here is a STRUCTURAL bug, localized. Script:
  `tools/chain_validation_gate.sh`. Output: a pass/fail matrix.
- **Phase 1 (Claude→Codex counter-check):** LLR reliability diagram — bin measured |LLR|
  against observed pre-FEC bit error rate per constellation; a calibrated demapper sits on
  the diagonal. Directly validates the σ² fix. Reuse `ULTRA_FAILURE_ATTRIBUTION` taps.
- **Phase 2 (Codex, needs sandbox):** fix + validate + generalize the genies off QAM16:
  known-H channel-est MSE oracle (dense-comb production estimator), known-CFO sweep oracle,
  known-timing-offset oracle. Each genie must itself pass an identity check (genie on a
  perfect channel = perfect output) before we trust its readings.
- **Phase 3:** wire Phase 0–2 into one `tests/chain_integrity_gate.sh` + a ctest target so
  "100% running properly" is provable on demand and runs before every commit on critical code.

## Rules
Trusted path = `cli_simulator` (real connected protocol), NOT `test_waveform_simple`.
Forced mod/rate is correct here (dev/proof exception — we WANT to exercise each rung).
Whole-matrix, multi-seed, no magic. Every genie validated before trust. Codex counter-checks
all chain-bug findings before action. (`feedback_per_stage_oracle_chain_validation`,
`feedback_assume_broken_prove_globally`, `feedback_claude_findings_need_codex_verification`)
