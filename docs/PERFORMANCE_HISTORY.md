# Performance & Calibration History (archive)

Moved out of `CLAUDE.md` on 2026-05-29 to keep the project instructions lean.
This file is a **measurement log / archaeology** — point of truth for *current*
floors is the compact table in `CLAUDE.md` plus the live docs it references
(`docs/KNOWN_BUGS.md`, `docs/CHANGELOG.md`, `docs/ACK_FRAME_FER_BASELINE_2026_05_20.md`,
`docs/WARM_SYNC_LTS_VERIFICATION_2026_05_20.md`). Single-seed and pre-audit
entries are floor *locators*, not statistical floors.

---

## Hardware Mac↔Pi5 rig — calibration run logs (2026-04-29)

Calibrated raw captures:
- `/tmp/ultra_audio_path_20260429_220218/pi_to_mac_capture.wav`
- `/tmp/ultra_audio_path_20260429_220218/mac_to_pi_capture.wav`

Post-calibration modem sweep:
- Good injected, 1 KB, R1/2, SNR 20/15/12: pass, `0` retx (`/tmp/ultra_hw_20260429_220250`, `_220341`, `_220445`)
- Moderate injected, 1 KB, R1/2, SNR 20/15/12: pass with `4/7/8` retx (`_220538`, `_220717`, `_220828`)
- Moderate injected, 1 KB, R1/4, SNR 15/12: pass with `8/8` retx (`_221117`, `_221240`)
- Good injected, 5 KB, R1/2, SNR 15: pass with `13` timeout retx (`_221423`)

Post ACK/control robustness patch:
- AWGN 1 KB R1/2 SNR15: pass 0 retx (`_222350`); Good 1 KB R1/2 SNR15: pass 0 retx (`_222920`)
- Moderate 1 KB R1/2 SNR15: pass 0 retx (`_223017`); Moderate 1 KB R1/4 SNR15: pass 0 retx (`_223253`)
- Moderate 1 KB R1/2 SNR12: pass 7 retx (`_223113`); Moderate 1 KB R1/4 SNR12: pass 5 retx (`_223754`)
- Good 5 KB R1/2 SNR15: pass 4 retx (`_223926`)

Corrected two-sided Pi/Mac rebuild:
- Good 1 KB R1/2 SNR15: pass 0 retx (`_224520`); Moderate 1 KB R1/2 SNR15: pass 0 retx (`_224612`)
- Good 5 KB R1/2 SNR15: pass 4 timeout retx (`_224658`); BRAVO failed the original seq32-35 burst (`CW[0..3]: FAIL`) and decoded the retransmissions → data-side loss, not ACK/control loss.

Final ACK/control + burst/data-acquisition:
- AWGN 1 KB R1/2 SNR15: pass 0 retx (`_230135`); Good 5 KB R1/2 SNR15: pass 0 retx (`_225150`)
- Moderate 5 KB R1/2 SNR15: pass 0 retx (`_225916`); Moderate 1 KB R1/2 SNR12: pass 0 retx (`_225822`)

Interpretation of the 2026-04-29 robustness work:
- ACK/control decode healthy in AWGN, Good SNR15, Moderate SNR15, SNR12 Moderate canary: cumulative ACKs repeat when profile ACK diversity is on, and the 1-CW control LLR gate admits real fading ACKs down to `|LLR|_avg ≈ 1.5`.
- The 5 KB Good residual was a burst-interleaver RX bug, not LDPC weakness: a physical block at `RMS=0.0390` was below the old hard `0.0400` gate, aborting the whole 4-frame group. Decoder now demodulates weak blocks down to `0.015` and only inserts zero-LLR erasures below that.
- The SNR12 Moderate tail retry was a data-acquisition gate issue: real tail DATA can arrive at `corr ≈ 0.52-0.56`, `|LLR|_avg ≈ 1.7`. Connected DQPSK data sync + 4-CW escalation now admit those while false-lock/near-zero gates still reject noise.

---

## Floor table (post-2026-05-20 warm-sync LTS) — full version

| Mode | Channel | In-band SNR floor | Confidence |
|------|---------|-----|---------------------|
| MC-DPSK R1/4 | AWGN | **5 dB** | 3/3 seeds cli_simulator + OTASim fixture `OTASimulatorTwoEndpointMCDPSKLowSNR` |
| MC-DPSK R1/4 | Moderate fading | 19.6 | pre-audit, not re-measured |
| OFDM_CHIRP R1/4 | AWGN | **10 dB** | warm-sync LTS FER: 4.875% @10 (n=800), 0.167% @12 (n=600), 0% @14-20 |
| OFDM_CHIRP R1/4 | Good fading | **15 dB** | historical: 3/3 seeds cli_simulator + DecodeBenchReplay fixture — **both retired**; re-establish on `gui_qso_scenario.sh` during the ladder rework |
| OFDM_CHIRP R1/4 | Moderate fading | **15 dB** | 1-seed OTASim (Mod ≈ Good at this rate — FEC absorbs the difference) |
| OFDM_CHIRP R1/2 | AWGN | **14 dB** | 1-seed OTASim (boundary, ~40 retx but ARQ recovers) |
| OFDM_CHIRP R1/2 | Good fading | **14 dB** | 1-seed OTASim |
| OFDM_CHIRP R1/2 | Moderate fading | **18-22 dB** (unrefined) | 1-seed OTASim — 22 passes, 18 fails; 19/20/21 bisect pending |
| OFDM_NARROW R1/4 | AWGN / Good | 17.6 / 17.6 | pre-audit |

### What these floors measure (read before quoting)

The QSO floor and the raw-frame floor are separately documented. The
`cli_simulator`/OTASim floor measures a full session: PING/PONG/CONNECT with
**full chirp+LTS preamble**, then post-handshake data with **light preamble
(LTS-only)** and selective-repeat ARQ. The `warm_sync_light` FER harness measures
the raw connected-mode light frame after a full OFDM chirp+LTS anchor seeded warm
timing state.

`docs/ACK_FRAME_FER_BASELINE_2026_05_20.md` (24 cells × 600 frames AWGN, isolated):
- 4-CW data, **light preamble**: 100% FER @8/10/12, 31% @14, ~0% @≥16
- 1-CW ACK, **light preamble**: 100% FER @8/10/12, 29% @14, ~0% @≥16
- 1-CW ACK, **full chirp+LTS preamble**: 0.3% @12, 3.8% @10, 68% @8

Cold light-preamble frames were 100% FER at 8/10/12 dB because the LTS-only
detector searched a 200 ms window at 0.52 acceptance. In the connected warm-sync
regime the RX first decodes a full chirp+LTS anchor, then narrows the expected LTS
window by frame-arrival timing and lowers the threshold by the matching
false-positive window reduction. Verified raw connected ACK light FER: 4.875% @10,
0.167% @12, 0% @14-20 dB (`docs/WARM_SYNC_LTS_VERIFICATION_2026_05_20.md`).

Sweep methodology (2026-05-19): `cli_simulator --ota-host 127.0.0.1:50051
--ota-alpha-token admin_tok --ota-bravo-token bravo_tok` against the OTASim
server, walking SNR down to a `TEST FAILED` cell.

### Floor moves from the 2026-05-19 audit + 2026-05-20 warm-sync LTS work

- MC-DPSK R1/4 AWGN: 18 → **5 dB**; OFDM R1/4 AWGN: 18 → **10 dB**
- OFDM R1/4 Good: 18 → **15 dB** (was locked in DecodeBenchReplay — now retired, re-establish on GUI gate); Moderate: 24.6 → **15 dB** (1-seed)
- OFDM R1/2 AWGN: 24.6 → **14 dB** (1-seed); Good: 24.6 → **14 dB** (1-seed); Moderate: 24.6 → **18-22 dB** (1-seed, unrefined)

Verification (2026-05-19): `ctest` 86/86 PASS; multi-seed cli_simulator 9/9 PASS
at MC-DPSK SNR=5, OFDM R1/4 SNR=12 AWGN, OFDM R1/4 SNR=15 Good; single-seed OTASim
sweep located 7 cells. Verification (2026-05-20): `warm_sync_light` ACK FER SNR
10/12/14/16/18/20 AWGN in `docs/data/warm_sync_lts_verification_2026_05_20.csv`;
cli_simulator AWGN matrix SNR 10/12/14 seeds 42/43/44 all pass.

---

## Historical / pre-audit QSO measurements (valid at the SNRs listed, not re-verified at new floor)

Legacy `--snr` knob ≈ in-band +10 dB (e.g. legacy 15 ≈ in-band 24.6).

- Continuous-AWGN v2 QSO (DQPSK R1/4 auto): in-band +29.6/+24.6/+19.6/+14.6/+9.6/+6.6/+4.6 pass; +1.6 fails. Refinement: +3.6 passes, +2.6 fails.
- MC-DPSK: 100% at in-band ≈19.6 with moderate fading.
- OFDM_CHIRP DQPSK R1/4 AWGN: 100% at ≈24.6/29.6, 0 retx.
- OFDM_CHIRP DQPSK R1/4 Good ≈24.6: 100% (0 retx); ≈19.6: 30/30 seeds (avg 1.5 retx).
- OFDM_CHIRP DQPSK R1/4 Moderate ≈24.6: 5/5 seeds (avg 1.4 retx).
- OFDM_CHIRP DQPSK R1/2 AWGN: 100% at ≈24.6/29.6, 0 retx.
- OFDM_CHIRP DQPSK R1/2 Good ≈24.6: 100% 5/5 seeds; Moderate ≈24.6: 5/5 seeds (avg 2.4 retx).
- OFDM_CHIRP DQPSK R2/3 AWGN: 100% at ≈29.6; Good ≈29.6: 30/30 seeds 0 retx; Good ≈24.6: 10/10 (avg 1.5 retx).
- OFDM_CHIRP QPSK R1/2 AWGN: 100% at ≈29.6; Good ≈29.6: avg 95% frame success, 30-seed, all delivered via ARQ.
- OFDM_CHIRP QPSK R2/3 AWGN: 100% at ≈29.6; Good: 5/5 seeds (2 had retx).
- OFDM_CHIRP DQPSK R3/4 AWGN: 100% at ≈29.6, 10/10 seeds; Good: NOT RECOMMENDED (23 retx / 5 seeds — AWGN only).
- OFDM_NARROW DQPSK R1/4 AWGN ≈17.6: 100% 0 retx; Good ≈17.6: 100% data, 93% ACK, all delivered via ARQ.

(2026-05-27+ one-way burst transport on Good@20 R3/4 QPSK measured ~1820 bps clean,
57% of the R3/4 ceiling; 8PSK is the promotion lever toward 3000 — see
`docs/KNOWN_BUGS.md` BUG-8PSK-001 and the warm-handoff memory notes.)

---

## Temporal fading measurement (2026-02-03)

- `getFadingIndex()` combines freq_cv (multipath) + temporal_cv (Doppler spread).
- temporal_cv = per-carrier magnitude variance over ~40+ symbols (~0.4 s).
- Good (0.1 Hz Doppler): temporal_cv ~0.03-0.30; Moderate (0.5 Hz): ~0.40-0.55.
- Trailing-silence bug fixed: `demodulateSoft()` was demodulating ~9 trailing silence symbols (131 vs 122 valid) → temporal_cv=0.27 on AWGN; now energy-thresholded (20% of reference).
- Calibrated combined fading index: AWGN ~0.04 / Good ~0.62 / Moderate ~0.90 / Poor ~0.82.
- `isFading()` thresholds 0.4 → 0.75 codebase-wide. Waveform-selection: AWGN <0.15, Good <0.75, Moderate <1.10.
- OFDM-internal fading thresholds (LLR scaling, two-pass) use a separate pilot-variance `last_fading_index` (~0.15-0.50), NOT this combined index.

## Fading channel notes (2026-03-15) — most folded into INVARIANTS / CFO_CORRECTION_FLOW

- LLR scaling `(1 + 10×fading²)` applied when OFDM `fading_index > 0.15` to prevent overconfident wrong bits.
- DQPSK two-pass DISABLED; D8PSK two-pass threshold 0.30 (uses pilot-variance `last_fading_index`, not `computeFadingIndex()` which returns 0 after sync). NOTE: see SPEC_BUGS audit — DQPSK two-pass is dead despite flag/docs.
- Light-sync confidence threshold 0.8 (raised from 0.5) to reject marginal fading syncs.
- CFO drift limited to ±1 Hz/frame when connected; CFO feedback loop + LTS residual CFO (>0.3 Hz) per `docs/CFO_CORRECTION_FLOW.md`.
- CPE correction for differential modes (DQPSK/D8PSK): per-symbol common-phase tracking from pilots, clamped ±15°. Good fading R2/3 SNR15 100% CW (10/10 seeds); Moderate R1/4 avg 1.4 retx, R1/2 avg 2.4 retx (5/5 seeds).

## Pending improvements (2026-02-03, dated — verify before acting)

- CFO pre-correction for LTS sync: light-preamble LTS detection still happens BEFORE CFO correction in StreamingDecoder; pre-correcting audio samples there would improve LTS detection on fading+CFO (O(N) complex multiply/sample).
- Per-symbol pilot tracking: IMPLEMENTED/active in `channel_equalizer.cpp` — per-data-symbol LS pilot H update, alpha-smoothed (0.8 for differential), residual CFO + timing recovery from pilot phase slope; pilots every ~10 carriers.
