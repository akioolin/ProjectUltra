<!--
ProjectUltra PR template. Keep entries short and concrete.
Strike out sections that are clearly inapplicable rather than padding them.
Do not push directly to `main`; PRs require human review.
-->

## Summary

<!-- One paragraph: what this PR changes and why. Name the user-visible behavior, not the diff. -->

## Source

- Task or issue: <!-- BUG-XXX, #123, links, or "human-authored" -->

## Risk Category

Pick exactly one primary category. Add secondaries only if a reviewer must look at them too.

- [ ] docs
- [ ] tooling (scripts, CI helpers, repo config — no production source change)
- [ ] test-only (new/changed tests, fixtures, harnesses)
- [ ] PHY (waveform, DSP, sync, OFDM/PSK demod, CFO)
- [ ] ARQ (selective repeat, stop-and-wait, ACK/NACK, retransmission, file transfer)
- [ ] LDPC / FEC (encoder, decoder, interleaver, rate policy)
- [ ] audio (audio engine, device I/O)
- [ ] security (permissions, secrets handling, network surface)

## Required Gates

Run from the repo root. Paste the result for each gate that applies.

- [ ] Build + unit/regression: `cmake --build build -j4 && ctest --test-dir build --output-on-failure -j4` — result:
- [ ] Targeted CTest subset (if narrower): `ctest --test-dir build -R '<pattern>' --output-on-failure` — result:
- [ ] Coverage (Tier 0/Tier 1 source change): `./scripts/coverage_report.sh` — line / function / branch deltas vs baseline:
- [ ] **Faithful gate (any PHY / ARQ / LDPC / audio / waveform-policy / fade / throughput claim):**
      `tools/gui_qso_scenario.sh --channel good --snr-db 20 --seed 42 --expect-rate R3/4 --file-kb 21 --out /tmp/X`
      — result (`RESULT=`, `FILE_CRC_OK_COUNT`, `GOODPUT_BPS`), or `not required because <reason>`

If a required gate is intentionally skipped, justify it on one line. "Not run" without a reason blocks merge.

## Throughput / Robustness Evidence

Required for any PHY / ARQ / LDPC / audio / waveform-policy change. Otherwise write `not applicable: <reason>`.

| Profile (channel, SNR, rate, payload) | Command | Before (commit / metric) | After (commit / metric) |
|---|---|---|---|
| | | | |

Report at minimum: frame/CW success, retransmission count, throughput (bps), and seed(s). Link to log paths under `/tmp/ultra_*`.

## Invariants & Calibration

- [ ] No change to documented invariants in `docs/INVARIANTS.md`, or this PR updates that doc and `docs/CHANGELOG.md` with the rationale.
- [ ] LDPC, ARQ, sync, and CFO feedback loops are not weakened.

## Security & Privacy

- [ ] No `/tmp/ultra_*` or other run-time logs committed.
- [ ] No credentials, tokens, SSH keys, or `.env` material committed.
- [ ] No host-specific paths (e.g. `/Users/<name>`, private hostnames) hardcoded in source or tests.
- [ ] If permissions / CI tokens changed, called out explicitly below:

<!-- describe permission or token surface changes, or write "none" -->

## Rollback

- Revert plan: <!-- e.g. "git revert <sha>; rebuild; rerun ctest" or "feature is gated by <flag>, disable to roll back" -->
- Data/state migrations to undo: <!-- "none" if pure code -->

## Residual Risks & Follow-ups

- Risks accepted in this PR: <!-- e.g. "Moderate fading R1/2 still has 2.4 avg retx; not regressed but not fixed" -->
- Suggested follow-up tasks: <!-- BUG-XXX / backlog refs; "none" is OK -->
