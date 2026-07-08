# SNR Calibration — Handoff Plan (2026-07-08, Fable 5 → Opus 4.8)

**Mission: every SNR number in this modem is either accurate to ±1 dB against a
defined truth, or visibly labeled for what it is. No estimator without a
calibration test. No consumer on an undocumented scale.**

Read first: `docs/CHANGELOG.md` entries dated 2026-07-07 (five of them),
`docs/KNOWN_BUGS.md` → BUG-PHYSICAL-SNR-RIG-REF, and memory topics
`project_ofdm_snr_recalibration_2026_07_07`, `project_two_snr_model_ionos_bench_2026_07_07`.

---

## 0. State as of addabb9 (what is DONE and proven)

| Estimator | State | Proof |
|---|---|---|
| Idle meter (IDLE_IN_BAND) | accurate | `ChannelIdleNoiseSNRCalibration` CTest; reads 9.6–10.0 at true 10 |
| MC-DPSK training (MCDPSK_IN_BAND fallback) | accurate in sim; on rig reads *effective* (see §5) | `MCDPSKSnrCalibration` |
| MC-DPSK data-aided (MCDPSK_IN_BAND primary) | accurate ±1 dB AWGN 0–25 | same CTest gates it |
| OFDM broadband/LTS (OFDM_BROADBAND) | **recalibrated tonight**: bias −0.4/−0.3/−0.2/−0.2 at 6/10/15/20 (was +3.4/+2.9/+2.6/+2.5) | **new** `OFDMSnrCalibration` CTest (4.5 s) |
| Physical/channel readout ("channel~X (beta)") | **defective** — see §2 | BUG-PHYSICAL-SNR-RIG-REF |
| Sync quality (SYNC_QUALITY) | correlation proxy, not an SNR; correctly excluded everywhere | routing pinned by `test_snr_source_routing` |

The estimator fix: `src/ofdm/channel_equalizer_lts.cpp` (both paths of
`updateLastSNREstimate`) — geometric conversion `per_carrier × 2·Ncar/N`
replaced a reference-power assumption (+2.758 structural), and LS `|h|²`
estimation noise is now subtracted (+0.4–0.9 SNR-dependent, grew in fades).

**Quarantine constant:** `kOfdmLegacyAnchorScaleOffsetDb = 2.758f`
(`src/protocol/connection_policy.hpp`, top). Three consumers were tuned on the
old (biased) scale and consume `reading + offset` until re-measured:
1. RX-authority feed — `src/gui/modem/modem_protocol_binding.hpp` (~line 121)
2. EESM gamma anchoring — `src/gui/modem/streaming_burst_interleave.cpp` (~line 505)
3. Tone-ACK staircase — `src/gui/app.cpp` (~line 700)
Grep the constant to find all sites. **They must be deleted TOGETHER with the
re-measure (§3), never one at a time.**

---

## 1. Tighten the OFDM calibration gate  (30 min, do FIRST)

`tests/test_ofdm_snr_calibration.cpp` currently asserts at
`ULTRA_OFDM_SNR_CAL_TOL_DB` default 4.0 (was set loose so it passed pre-fix).
Post-fix bias is ≤0.4 dB. Change the default to **1.5**, run
`ctest --test-dir build -R OFDMSnrCalibration --output-on-failure`, commit.
This makes the truth-matrix an actual regression gate.

Harness subtleties documented in the test header — do not "simplify" them away:
- `SimulatedChannel::normalizeTxBurstToReference` pins the OFDM LTS/data section
  ~2.7 dB below the burst reference (peak-normalized ~10 dB-PAPR burst duty
  split). The test disables it and scales the MEASURED SECTION to
  `kModemReferenceInBandRms` so dial == truth by construction.
- Truth must be injected on the SIGNAL side, never by moving the noise dial.

## 2. Fix the physical/channel readout properly  (½ day)

Filed as BUG-PHYSICAL-SNR-RIG-REF. Current beta computes
`(P_train − N)/N` in the decoder's ping-check lambda
(`src/gui/modem/streaming_ofdm_decode.cpp`, `evaluatePingDecision`) with
`N = sync_noise_ref_rms_²` (burst-time inter-chirp gap, measured at sync-found
in `src/gui/modem/streaming_sync_acquisition.cpp` — diagnostic log line
"burst-noise ref" exists). Defects: −1.7 dB definition offset, handshake-only
latch (stale mid-transfer), one impossible rig reading (4.4 < usable).

Implementation spec (from tonight's audit agent, validated against the code):
1. Move the computation INTO `updateTrainingSNREstimate`
   (`src/psk/multi_carrier_dpsk.hpp:1077`): add
   `setNoiseReferenceRMS(float)` on the demodulator + passthrough on
   `MCDPSKWaveform`; decoder calls it with `sync_noise_ref_rms_` before decode.
   In the existing per-sample loop add a third accumulator: raw received sample
   through a third `FIRFilter::bandpass(101, 50, 2950)`, accumulate
   `rx_power`. After the loop: `physical = 10log10((p_rx − p_n)/p_n)` gated on
   `p_rx > 2·p_n` (the ≥3 dB signal-presence latch — a noise-only PING span
   must never latch; rig F228 lesson).
2. **Definition decision (make it explicitly):** training/data power is a real
   1.72 dB below the whole-ping calibration reference (chirp at amplitude 0.5
   dominates the reference; training power = 1/(2·num_carriers) per carrier).
   Either (a) payload-referenced (recommended for physics honesty) documenting
   the fixed offset from dial, or (b) dial-equivalent by adding the
   DETERMINISTIC constant `10log10(P_ping_avg/P_train)` computed from waveform
   constants — never a tuned number. For the operator display, (b) matches
   what the user expects ("dial 10 shows ~10").
3. Refresh per decoded frame (kill the handshake latch); plumb through the
   existing `last_physical_snr_db_` atomics (`streaming_decoder.hpp`).
4. Residual −0.66 dB: the noise ref reads 0.089 at a true 0.0964 in sim —
   audit the gap-window skip/FIR settle in `streaming_sync_acquisition.cpp`
   (the center-60% window and priming).
5. **Rig validation experiment (needed before trusting it on IONOS):** the
   IONOS is an S:N machine — its noise level tracks the input signal
   (`docs/references/teensy_ionos_hf_manual_rev_2.03.pdf`, Fig 1/2:
   `rms meas → S:N Mixer`). Question: does its tracker move WITHIN the 100 ms
   inter-chirp gap? Bench experiment: play a chirp train from the Pi5, measure
   gap RMS vs the post-burst noise decay curve at the Mac (the ping-loop
   method — see scratchpad `measure_snr.py` pattern in the 07-07 session).
   If the gap noise ≠ during-signal noise on IONOS, the readout needs an
   IONOS-specific caveat (real radios and OTASim are fine — constant noise).
6. Acceptance: sim awgn@10 reads 10.0±0.5 with definition (b); rig WGN@10
   reads ~9.5±1; **physical ≥ usable on every frame** (see §4).

## 3. Re-measure the anchor scale → delete the quarantine constant  (1 day, rig needed)

The mathematically clean path: since the offset exactly compensates the
structural bias, **shifting the `kCoherentLadder` `min_snr_db` column by
−2.758 and deleting all offset sites is behavior-identical on flat channels**
— then VALIDATE, because the SNR-dependent debias (intentionally not
compensated) makes fading readings honest-lower:
1. Shift the column (`src/protocol/waveform_selection.hpp:129-243`); update the
   pinned boundary tests (`tests/test_waveform_policy.cpp`).
2. Shift staircase edges 18/16 → 15.2/13.2
   (`src/waveform/tone_burst_ack/tone_burst_constants.hpp:114`) and delete the
   app.cpp offset site.
3. Delete the binding + gamma-anchoring offset sites and the constant itself.
4. Validate: full ctest; `gui_qso` good@20 seed 42 (expect PASS ~2000 bps);
   then a 5-run rig batch at MPG@20 vs the F208-F217 ledger (mean within the
   ±25% gate noise = no regression; interleaved A/B if in doubt — that is THE
   honest rig methodology, see memory `project_groupsize_shipped…`).
5. Expected behavioral delta: slightly earlier demotes in deep fades (the
   debias working); flat-channel behavior identical.

## 4. The usable ≤ channel invariant  (2 h, after §2)

Physics: effective/usable SNR can never exceed the channel's physical SNR.
Once §2 makes physical per-frame and trustworthy: where the routed usable and
the physical readout coexist (per decoded frame), if
`usable > physical + 1.0 dB`, log a `SNR-SANITY` warning and mark the frame's
reading suspect (exclude from the ConnectSnrPool and the authority feed for
that frame — one frame, not a latch). This is the user's own invariant and it
caught two real bugs tonight from a single log line. Cheap, permanent.

## 5. Fading: report the distribution, not snapshots  (½ day, UI)

All meters are now honest but each samples a ±5 dB-swinging channel at a
different instant: connect wire = trough snapshot; mid-transfer lts =
fade-peak-biased (readings exist only when frames decode → survivor-samples
peaks); beta = (currently) a stale latch. Operator answer:
1. Maintain a linear-domain EMA + percentile spread of the physical readout
   over ~10 s; display `channel ≈X ±Y dB` on the MODE line instead of
   snapshots.
2. Label the lts value for what it samples: `usable X dB (decoded frames)`.
3. The MC-DPSK training-vs-data-aided nuance: training reconstruction is
   STATIC over 144 ms and over-penalizes slow wander for a DIFFERENTIAL demod
   (a 1.2 Hz wander costs it 3 dB but costs DQPSK only ~0.3-0.5 dB across a
   21 ms symbol pair). The data-aided estimator (differential-level,
   drift-immune, `multi_carrier_dpsk.hpp:1154+`) is the correct usable meter
   and is already the preferred route (`ULTRA_CONNECT_DATA_AIDED_SNR`
   default-ON). If rig usable still reads ≥2 dB below the honest physical at
   WGN (no fades), quantify the residual with a jitter-injection sim sweep
   before touching anything.

## 6. Entry policy — the hot-entry cliff  (after §2-§5; the payoff)

F229/F230: connect entered QPSK R2/3 / R1/2 off trough snapshots (wire 2-5 dB)
via the affine fade basis (`connectSelectionSnrDb`,
`connection_policy.hpp:700-838` — maps data-aided reading + clamp(19.55−r,2,11)),
then receiver authority demoted within 60-90 s. With honest meters and the §5
fade-averaged input, re-derive the entry mapping so the FIRST rung matches
what the first 90 s can actually sustain. Success metric on the rig: fraction
of MPG@10/MPG@20 sessions with zero authority demotes in the first 2 minutes.
Do NOT re-tune the affine constants piecemeal — they are pinned in
`tests/test_connection_policy.cpp` and were calibrated as a set.

**The theoretically correct entry estimator (build THIS, not another boost):**
at Tc≈4 s a single ~2 s frame window sees half a fade cycle — the mean is
UNLEARNABLE from it (irreducible ±3-4 dB; no estimator fixes that). Decompose:
1. Noise N: stationary, from the idle estimator (minutes of observation,
   ±0.2 dB) — already accurate.
2. Mean signal power S̄: accumulate |H(t)|²-proportional power samples from
   EVERY handshake transmission (each PING/PONG chirp, each CONNECT/ACK frame,
   including retries) across the WHOLE probing+connect phase — 10-40 s spans
   multiple coherence times → a true LINEAR-domain fade average. Today only
   the final frame's window is used; the rest is discarded. The chirp is
   constant-envelope at known TX scaling → each chirp's received in-band power
   is one clean |H|² sample (the burst-time noise ref machinery already
   isolates per-event noise).
3. Entry policy: enter at mean − k·σ (k from the loss asymmetry: a low entry
   costs ~60-90 s of authority climb; a high entry costs demote thrash or a
   stall) and let the existing authority/EESM machinery climb. The current
   affine basis INFLATES a single snapshot — the inverse of the correct
   posture. Commercial HF modems converged on enter-robust/climb-fast.
Acceptance: entry-rung distribution vs the first-2-minute sustained rung on
20 rig sessions — entries at or one below sustained ≥85% of the time, never
above by ≥2 rungs.

**STATUS 2026-07-08 (first half SHIPPED, 902282d):** the PHYSICAL ENTRY CAP
(sel = min(sel, physical_mean + 2.0)) is live — good@10 entry R2/3→R1/2,
good@20 unchanged (1940 bps). **MEASURED GAP (rig F233 @MPG:10): the cap
ABSTAINS in the deepest troughs** — the physical latch correctly refuses when
the training span is within 3 dB of noise, so an entry picked off a
trough-zero wire reading (F233: "usable 0 dB" → boosted → R1/2, no physical
sample at pick time) runs uncapped exactly where the boost is most dangerous.
The chirp-accumulation half (this section's main spec) closes that: chirp
|H|² samples have ~20 dB processing gain and measure cleanly inside data
nulls. **SHIPPED 2026-07-08 (second half): every dual-chirp anchor now
contributes a physical sample (up-chirp span power vs the frame's gap noise,
≥3 dB latch) — the entry line carries `channel X±Y` AT the pick (verified
awgn@10 + good@10, both PASS, cap active at entry). RESIDUAL: chirp-referenced
samples read ~+1.9 dB vs dial on AWGN (duty-factor + reference-convention
detail) — fold into the §2 per-profile derivation test (derive
10log10(P_chirp_span/P_ping_ref) from the generator, never tune by eye); the
cap's 2.0 margin absorbs it until then.** F233 also re-confirmed the mid-transfer dial-10 stall class
(rate thrash R1/2↔R2/3 riding fade epochs, then full-anchor-wait reject
streak=1461 — the BUG-DECODE-BACKLOG/anchor family, pre-existing).

## 7. Documentation debt (surfaced by tonight's audit — fix in CLAUDE.md)

- CLAUDE.md says "Only physical SNR sources (IDLE_IN_BAND, OFDM_BROADBAND)
  feed rate selection" — **STALE**: MCDPSK_IN_BAND (data-aided) is the PRIMARY
  connect-time input (`acceptsRateSelectionSNR`, `connection.hpp:575-580`;
  pool contract `connection_policy.hpp:456-463`).
- CLAUDE.md floor table (AWGN 10/Good 12/Moderate 14/Poor 18) — **STALE**:
  `kOFDMEntryFloor*` = 8/8/14/inf (`waveform_selection.hpp:29-45`).
- Register `OFDMSnrCalibration`, the two-SNR display, and the quarantine
  constant in `docs/MODEM_INFRASTRUCTURE_MAP.md`.

---

## §8 (discovered F234, MPG@0): ACK TIME-DIVERSITY at sub-floor SNR

F234 connected AT MPG@0 (the full handshake stack working: flooded-gap PING
classified at ratio 0.926, CONNECT decoded on a fade peak, honest 0 dB entry
at MC-DPSK R1/4) and moved 5 frames — then deadlocked on a one-way ACK path:
the DATA direction has time diversity (every retry samples a new fade state,
HARQ accumulates) but each tone-burst ACK is a single 0.85 s shot — SHORTER
than one fade null (Tc≈4 s), all symbols die together, one shot per round.
Five decorrelated rounds all lost; the Pi5 re-sent the same retired frames
forever (Mac base=5, same ack ×5 over 3 min). Fix shape: when the measured
SNR reads below ~5 dB, transmit the ack as N copies spread over ≥Tc (the
existing ack-repeat machinery is close but repeats are one-shot and cancel on
peer activity — at high-retx the peer re-sending RETIRED frames is PROOF the
ack was lost, the opposite inference). Below the design floor this only
degrades gracefully — but it converts "stall at 4%" into "crawl", and the
same diversity logic helps marginal 5-8 dB links. Half-duplex: the copies
must fit the turnaround budget (veteran-operator lens: this is what tone
repetition on real HF nets is for.)

F235 (MPG@5, the exact MC-DPSK floor) sharpened the evidence: connected off a
trough-1 reading, ground ~98% of the file with the ack ledger CLEAN (one ack
per round, base 50→62 marching) — then died in the ENDGAME: the same
hole-bitmap ack for frame 64 repeated 8 rounds over ~6 min until the sender's
900 s cap fired. Single-frame endgame rounds have the least ack diversity and
the tightest half-duplex timing — the §8 fix pays there first. FLOOR MAP as
handed over: MPG@0 connect + 5 frames (ack path dead); MPG@5 connect + ~98%
(endgame ack); MPG@10 repeated full CRC deliveries. Graded degradation, every
failure named.

## Traps that cost hours tonight (do not rediscover)

1. **IONOS is an S:N machine** — noise level TRACKS the input signal. Idle
   floor ≠ burst floor there. Any idle-time noise reference is invalid on
   IONOS; use per-frame burst-time references (the inter-chirp gap machinery,
   `SyncResult.interchirp_gap_*` → `sync_noise_ref_rms_`). Real radios and
   OTASim have constant noise. The IONOS dial itself is honest and
   self-calibrating (manual archived at `docs/references/`).
2. **`UltraTncSimAudio` fails under CPU contention** — never run it
   concurrently with a GUI sim or a leftover `ota_simulator serve` daemon
   (`pkill -f 'ota_simulator serve'` first). A red there is a machine-load
   artifact until proven otherwise.
3. **Pi5 `logs/gui.log` is overwritten per launch** — `scp` it before
   relaunching or the run's evidence is gone.
4. **`grep -c` returns exit 1 on zero matches** — it silently kills `&&`
   chains in test scripts.
5. **The `mode==OFDM_CHIRP ? x : 0` pattern found THREE bugs in one night**
   (CONNECT_ACK rescue budget, ack-repeat guards, geometric gate). A protocol-
   layer sweep for that shape is a standing audit candidate.
6. **Method:** decompose ONE full protocol cycle from BOTH station logs with
   timestamps before proposing any fix; and on the rig, interleaved same-epoch
   A/B is the only honest comparison (cross-epoch batches are confounded).

## Verification ledger (what "done" means per section)

| § | Gate |
|---|---|
| 1 | `OFDMSnrCalibration` green at tol 1.5 |
| 2 | sim awgn@10 physical 10.0±0.5; rig WGN@10 ~9.5±1; per-frame refresh visible in logs |
| 3 | ctest 85/85; good@20 ≥1800 bps; 5-run MPG@20 rig batch within ledger noise; constant deleted |
| 4 | SNR-SANITY wired; zero warnings on sim AWGN; warnings correlate with fade events only |
| 5 | MODE line shows `channel ≈X ±Y dB`; no impossible orderings on 3 rig runs |
| 6 | ≥70% of rig MPG sessions: zero authority demotes in first 2 min |
| 7 | CLAUDE.md + map updated in the same commit as each change |
