# Overnight Carrier-Sense Report - 2026-05-22

Branch: `feat/16qam-promotion-2026-05-21`

Pushed: no

## Landed Commits

1. `207fbe1 Fix carrier-sense noise-floor absorption`
2. `d06c329 Document disabled DQPSK two-pass path`

## Primary Fix

Root cause confirmed before the fix: AWGN QAM16 R1/2 SNR20 5 KB seed42 had one half-duplex collision. BRAVO keyed an ACK inside ALPHA's data burst because the adaptive carrier-sense noise floor had learned ALPHA's sustained carrier as background. The measured colliding ACK commit saw `ch_idle=1` at `ch_rms=0.261589`, while true idle was about `0.028` and the modem reference in-band RMS is about `0.305`.

The landed fix adds an optional absolute RMS ceiling for samples admitted into `ChannelBusyDetector`'s adaptive noise-floor estimator. This does not scale the busy decision threshold upward. It prevents near-reference modem carriers from entering the noise-floor history while still allowing calibrated noisy-idle samples below the ceiling to seed low-SNR operation.

Simulation wiring is local-signal based only:

- DBPSK uses a ceiling at `0.75 * kModemReferenceInBandRms` for handshake and low-rung MC-DPSK carrier sense.
- QAM modes enable a ceiling at `0.77 * kModemReferenceInBandRms` only after the local receiver decodes a low-fading frame (`last_fading_index <= 0.05`).
- No peer transmit state or out-of-band channel state is used.

## Primary Verification

AWGN hard gate, QAM16 R1/2 SNR20:

| File | Seed | Result |
| --- | --- | --- |
| 5 KB | 42 | `rc=0`, overlaps `0`, ARQ `19/0/0`, E2E `1840 bps` |
| 5 KB | 43 | `rc=0`, overlaps `0`, ARQ `19/0/0`, E2E `1840 bps` |
| 5 KB | 44 | `rc=0`, overlaps `0`, ARQ `19/0/0`, E2E `1840 bps` |
| 20 KB | 42 | `rc=0`, overlaps `0`, ARQ `71/0/0`, E2E `2169 bps` |
| 20 KB | 43 | `rc=0`, overlaps `0`, ARQ `71/0/0`, E2E `2170 bps` |
| 20 KB | 44 | `rc=0`, overlaps `0`, ARQ `71/0/0`, E2E `2169 bps` |

No-regression cells:

- DQPSK Good/SNR12 1 KB seed42: negotiated `OFDM-CHIRP DQPSK R1/4 cw=4`, on-air `391 bps`, E2E `221 bps`, ALPHA ARQ `frames_sent=20 retransmissions=4 timeouts=4`.
- QAM16 Good/SNR20 20 KB seed42: on-air `2330 bps`, E2E `1411 bps`, ALPHA ARQ `71/4/1`.
- QAM16 Good/SNR20 20 KB seed43: on-air `1882 bps`, E2E `754 bps`, ALPHA ARQ `71/24/19`. A clean temporary revert on this checkout measured the same `754 bps, 24/19`, so this is not a regression from current branch state. Older docs list `853 bps, 23/19` for this cell, but that was not the current checkout baseline.
- Determinism: two consecutive AWGN QAM16 R1/2 SNR20 5 KB seed42 runs both produced on-air `2187`, E2E `1840`, overlaps `0`, and ARQ `19/0/0`.

Low-SNR idle-preservation gate, 1 KB seed42 auto-negotiation:

| Channel | SNR | Negotiated | Result |
| --- | --- | --- | --- |
| Good | 5 | MC-DPSK DBPSK R1/4, 2048 sps | connected, verified, E2E `13 bps`, ARQ `33/1/1` |
| Good | 6 | MC-DPSK DBPSK R1/4, 1024 sps | connected, verified, E2E `12 bps`, ARQ `33/27/22` |
| Good | 7 | MC-DPSK DBPSK R1/4, 1024 sps | connected, verified, E2E `12 bps`, ARQ `33/28/20` |
| Good | 8 | MC-DPSK DBPSK R1/4, 1024 sps | connected, verified, E2E `12 bps`, ARQ `33/28/21` |
| Good | 9 | MC-DPSK DBPSK R1/4, 1024 sps | connected, verified, E2E `12 bps`, ARQ `33/28/21` |
| Good | 10 | MC-DPSK DBPSK R1/4, 1024 sps | connected, verified, E2E `13 bps`, ARQ `33/24/18` |
| AWGN | 5 | MC-DPSK DBPSK R1/4, 2048 sps | connected, verified, E2E `14 bps`, ARQ `33/0/0` |
| AWGN | 6 | MC-DPSK DBPSK R1/4, 1024 sps | connected, verified, E2E `37 bps`, ARQ `33/0/0` |
| AWGN | 7 | MC-DPSK DBPSK R1/4, 1024 sps | connected, verified, E2E `37 bps`, ARQ `33/0/0` |
| AWGN | 8 | MC-DPSK DBPSK R1/4, 1024 sps | connected, verified, E2E `37 bps`, ARQ `33/0/0` |
| AWGN | 9 | MC-DPSK DBPSK R1/4, 1024 sps | connected, verified, E2E `37 bps`, ARQ `33/0/0` |
| AWGN | 10 | OFDM-CHIRP DQPSK R1/4 | connected, verified, E2E `382 bps`, ARQ `20/0/0` |

CTest:

- `./build/tests/test_channel_busy_detector`: passed.
- `ctest --test-dir build -R ChannelBusyDetector --output-on-failure`: passed.
- `ctest --test-dir build -j4 --output-on-failure`: passed `92/92` before the primary commit.

## Tried And Reverted

Rejected variants:

- Threshold-scaled current RMS shortcut: no effect, because the adaptive threshold had already scaled with the inflated noise floor.
- Global cap/freeze plus demod hold: fixed AWGN but changed the DQPSK guard to `376/20/5/3` instead of the exact `391/20/4/4`.
- Static/global sample ceiling: fixed AWGN but changed DQPSK timeout behavior to `391/20/8/8`.
- QAM16-only sample ceiling applied immediately: fixed AWGN and DQPSK, but regressed QAM16 Good seed42 to about `913 bps, 13 retx, 10 timeouts`.
- QAM16 ceiling factors `0.84`, `0.80`, and `0.77` without local fading gating: either left AWGN overlaps or hurt Good fading.
- QAM low-fading streak gating: streak `>=8` failed AWGN; streak `>=3` with `<=0.03` passed 5 KB but failed 20 KB AWGN with seed42 `18` timeouts and `2` overlaps, seed43 `8` timeouts and `3` overlaps, and seed44 `18` timeouts and `4` overlaps.

All rejected attempts were reverted before the landed commits.

## Secondary Cleanup

`d06c329` removes the dead `dqpsk_two_pass_enabled_` flag. The DQPSK two-pass invocation remains intentionally disabled in `ofdm_symbol_demap.cpp`; the helper is retained only for supervised experiments. `CLAUDE.md` now states:

`DQPSK two-pass DISABLED; D8PSK two-pass threshold 0.30`

Verification after this cleanup:

- `cmake --build build -j4`: passed.
- `ctest --test-dir build -j4 --output-on-failure`: passed `92/92`.

## Remaining Roadblocks

- The current fix uses a locally decoded low-fading QAM frame as the safe signal to enable QAM carrier sample rejection. That is principled and local, but it is still a proxy for the preferred receiver-state freeze. A future supervised pass should expose a direct demodulator "in-frame/acquiring" signal to `ChannelBusyDetector` so training can freeze during the full acquisition/decoding interval, not only after successful low-fading frame decode.
- QAM16 Good/SNR20 seed43 baseline in this checkout is lower than the older throughput docs (`754 bps, 24/19` measured now vs older `853 bps, 23/19`). The carrier-sense fix does not worsen it, but the doc drift should be reconciled separately before using that cell as a fixed numeric release bar.
- No attempt was made on the explicitly deferred risky items: R1/4 LDPC matrix changes, sigma-squared LLR recalibration, or idle_in_band +7.2 dB rate-selection calibration.

## Recommended Next Supervised Step

Add an explicit demodulator-to-carrier-sense training gate: freeze or ceiling-gate adaptive noise-floor updates while the local receiver is synchronized, acquiring, or actively decoding a candidate frame. Keep the current absolute ceiling as the first-principles safety net against full-power carrier absorption, and rerun the same AWGN, low-SNR idle, DQPSK, QAM Good, determinism, and full-ctest gates.
