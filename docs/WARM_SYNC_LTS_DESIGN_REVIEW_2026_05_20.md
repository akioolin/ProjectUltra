# Warm-Sync LTS Design Review - 2026-05-20

## Verdict

**AGREE WITH REFINEMENTS - proceed to Phase 1 after applying the refinements below.**

The core thesis is correct: the production failure is light-preamble LTS acquisition, not LDPC. The current receiver searches connected OFDM light preambles with a 9600-sample outer window (`LIGHT_SEARCH_SIZE`, about 200 ms) and then rejects candidates below the connected threshold of 0.52. That is incompatible with the measured SNR=12 LTS correlations of 0.20-0.34 in `docs/ACK_FRAME_FER_BASELINE_2026_05_20.md`.

The important refinements are:

1. **M4 is mandatory and should move before M1-M3.** Current production TX paths use light preamble for every non-handshake OFDM frame once connected and handshake-complete. There is no special full OFDM chirp+LTS anchor for the first OFDM frame. Without that anchor, warm-sync timing cannot bootstrap at marginal SNR.
2. **The current connection flow is not exactly the CLAUDE.md MODE_CHANGE sequence.** The code embeds the initial waveform/data mode in `CONNECT_ACK` and applies it immediately. `MODE_CHANGE` still exists for later adaptive changes, but initial OFDM use does not require a separate MODE_CHANGE in the current code.
3. **M1 must expose timing hints without coupling the decoder to ARQ internals.** The decoder should track absolute sample positions and warm-sync state locally, while protocol/engine layers provide coarse expected-arrival timing where available.
4. **M2 must control the outer streaming search, not only the waveform-local LTS loop.** `detectDataSync()` already searches locally around a signal start; the new expected-arrival window must decide which absolute samples the streaming decoder extracts and accepts.
5. **M3 threshold values must be derived and tested.** The existing `sync_reject_streak_` relaxation is heuristic. Narrow-window thresholds should be separate policy inputs tied to the candidate count / false-positive budget and backed by a noise-only test.
6. **M7 belongs in `connection_policy::selectLadderRung()` / `recommendWaveformAndRate()`.** `selectOFDMCodeRate()` chooses a rate only after OFDM has already been selected.

## Architectural Facts Verified

- Full OFDM chirp+LTS preamble generation exists in `OFDMChirpWaveform::generatePreamble()` (`src/waveform/ofdm_chirp_waveform.cpp:265-280`).
- Light OFDM LTS-only preamble generation exists in `OFDMChirpWaveform::generateDataPreamble()` (`src/waveform/ofdm_chirp_waveform.cpp:282-290`), with `getDataPreambleSamples()` returning two OFDM symbols (`src/waveform/ofdm_chirp_waveform.cpp:918-922`).
- Connected streaming search uses `LIGHT_SEARCH_SIZE = 9600` samples and switches to light sync when connected and the waveform supports data preamble (`src/gui/modem/streaming_sync_acquisition.cpp:169-179`).
- Connected light sync calls `detectDataSync()` with the cached CFO and then applies `streaming_signal_policy::evaluateLightSyncCandidate()` (`src/gui/modem/streaming_sync_acquisition.cpp:416-443`).
- Current connected OFDM light sync deliberately has no chirp fallback (`src/gui/modem/streaming_sync_acquisition.cpp:400-452`).
- Current connected DQPSK OFDM threshold is `min_confidence = 0.52`, with heuristic relaxation floors at 0.40 and 0.35 (`src/gui/modem/streaming_signal_policy.hpp:89-132`).
- `detectDataSync()` is Schmidl-Cox-style LTS autocorrelation over analytic samples, with coarse step 8 and fine refinement around the best peak (`src/waveform/ofdm_chirp_waveform.cpp:388-537`).
- CFO warm state exists, but it is used after sync acquisition: `StreamingDecoder::applyCFOPreCorrection()` is applied during frame decode (`src/gui/modem/streaming_sync_acquisition.cpp:36-71`, `src/gui/modem/streaming_ofdm_decode.cpp:274-284`), and OFDM pilot/LTS residual feedback updates `last_cfo_` after demodulation (`src/gui/modem/streaming_ofdm_decode.cpp:759-781`).
- The first OFDM frame is not currently anchored. GUI, TNC, and simulator TX paths select light preamble for non-handshake OFDM whenever connected and handshake-complete (`src/gui/modem/modem_engine.cpp:378-383`, `tools/ultra_tnc.cpp:720-723`, `tools/sim/simulated_station.hpp:1440-1452`).
- Current initial data mode comes from `CONNECT_ACK`, not a mandatory separate initial MODE_CHANGE. The initiator applies `initial_modulation`, `initial_code_rate`, and `data_frame_cw_count` from CONNECT_ACK (`src/protocol/connection_handlers.cpp:374-388`), while the responder builds CONNECT_ACK with the negotiated waveform and initial mode (`src/protocol/connection_handlers.cpp:297-315`). `MODE_CHANGE` remains implemented for later updates (`src/protocol/connection_handlers.cpp:490-525`).

## Milestone Evaluation

### M1: Frame-Arrival-Time Tracking

**Agree with refinement. Risk: medium-high.**

The decoder already maintains absolute sample accounting through `total_fed_`, `ringPosToAbsoluteLocked()`, `absoluteToRingLocked()`, and `search_floor_abs_` (`src/gui/modem/streaming_sync_acquisition.cpp:128-157`, `src/gui/modem/streaming_decoder.hpp:400-419`). It does not currently maintain next-frame prediction or warm-sync confidence (`src/gui/modem/streaming_decoder.hpp:408-487`).

Refinement: add sample-domain warm timing state to `StreamingDecoder`, but do not make the decoder know ARQ policy directly. A practical interface is a small public setter for expected-arrival hints and tolerance in samples, plus decoder-owned updates after successful decodes. Protocol/engine layers can call that setter later; unit tests can drive it directly.

### M2: Narrow-Window LTS Detection

**Agree with refinement. Risk: high.**

The plan is directionally correct, but the implementation point is the streaming extraction/search cursor. `searchForSync()` currently backtracks 9600 samples, copies `min_search` samples, and advances by `CORRELATION_STEP` (`src/gui/modem/streaming_sync_acquisition.cpp:308-361`). `detectDataSync()` then performs an internal local search from `signal_start` over roughly 4 to 8 OFDM symbols (`src/waveform/ofdm_chirp_waveform.cpp:400-463`).

Refinement: narrow-window mode should choose an absolute search start and sample count around `next_expected_frame_sample_`, then pass that bounded buffer to `detectDataSync()`. The existing wide 200 ms path must remain the fallback for cold/degraded/recovery states.

### M3: Lowered Threshold Within Narrow Window

**Agree with refinement. Risk: high.**

This is the critical behavior change. Current policy constants are centralized and tested (`src/gui/modem/streaming_signal_policy.hpp:89-177`, `tests/test_streaming_signal_policy.cpp:81-140`), so policy extension is the right place.

Refinement: the new threshold must not be just another `sync_reject_streak_` relaxation. Add explicit narrow-window thresholds and a helper that derives or documents the threshold from the wide/narrow candidate-count ratio. Keep cold/wide thresholds unchanged. Add a noise-only test that exercises the streaming detector for at least 60 seconds of noise, not only the waveform function, because the RMS gate and search cursor materially affect false locks.

### M4: First-OFDM-Frame Chirp Anchoring

**Mandatory; move before M1-M3. Risk: medium.**

This is not already implemented. Current TX paths send light OFDM immediately after connected handshake for all non-handshake frames (`src/gui/modem/modem_engine.cpp:378-383`, `tools/ultra_tnc.cpp:720-723`, `tools/sim/simulated_station.hpp:1440-1452`). The encoder has both full and light APIs (`src/gui/modem/streaming_encoder.cpp:211-299`), so this should be a local state/policy change, not a wire-format change.

Refinement: introduce a one-shot "next OFDM frame uses full preamble" flag when entering OFDM connected mode or after waveform family changes. Clear it after the first non-handshake OFDM TX. The RX side should treat a successful full OFDM frame as the warm-sync anchor.

### M5: Matched Filter + CFO Pre-Correction for LTS

**Agree as conditional, likely needed for SNR=10. Risk: medium-high.**

The plan correctly identifies existing CFO state. However, current pre-correction is applied after sync, during decode (`src/gui/modem/streaming_ofdm_decode.cpp:274-284`), while light acquisition itself still uses uncorrected samples plus magnitude autocorrelation.

Refinement: implement matched filtering as an additional score in `OFDMChirpWaveform::detectDataSync()`, not as a replacement for the Schmidl-Cox metric. Reuse or expose the same LTS template generated by the modulator/demodulator path (`src/ofdm/modulator.cpp:608-658`, `src/ofdm/ofdm_demodulator_setup.cpp:120-154`). Combining by `max()` is safer initially than a weighted sum because it preserves the existing Schmidl-Cox behavior when cached CFO is wrong.

### M6: Warm-Sync State Machine + Graceful Degradation

**Agree with refinement. Risk: medium.**

Current state is a general decode state machine (`SEARCHING`, `SYNC_FOUND`, `DECODING`, burst states) and a heuristic `sync_reject_streak_` (`src/gui/modem/streaming_decoder.hpp:59-67`, `src/gui/modem/streaming_decoder.hpp:486`). There is no explicit COLD/WARM/DEGRADED/RECOVERY sync state.

Refinement: keep the warm-sync state orthogonal to `DecoderState`; do not overload frame decoding states. Successful full OFDM anchor or successful light frame enters WARM. Misses in a predicted window enter DEGRADED and widen tolerance. Several misses clear timing confidence and return to COLD/wide behavior. Do not remove wide search.

### M7: Waveform Selection Threshold Update

**Agree with corrected target files. Risk: medium-high.**

`selectOFDMCodeRate()` only selects rate after OFDM has already been chosen (`src/protocol/waveform_selection.hpp:53-69`). The MC-DPSK to OFDM decision currently lives in both `recommendWaveformAndRate()` (`src/protocol/waveform_selection.hpp:112-154`) and the connection ladder floors in `connection_policy::selectLadderRung()` (`src/protocol/connection_policy.hpp:141-179`), with tests covering current 18/20/22 dB boundaries (`tests/test_connection_policy.cpp:79-112`).

Refinement: lower the OFDM entry floor only after M3/M5 verification proves raw warm-light reliability at the proposed SNR. Update both policy paths and tests. At SNR=10, be careful that the full-preamble ACK baseline is 3.8 percent FER, so a target of retx <=5 is plausible only if the first full anchor plus warm light is verified over multi-seed sessions.

### M8: Verification + Documentation

**Agree with refinement. Risk: medium.**

`tools/measure_ack_fer.cpp` currently supports `ack_light`, `data4_light`, and `ack_full`, and resets the decoder for each isolated trial (`tools/measure_ack_fer.cpp:30-34`, `tools/measure_ack_fer.cpp:216-256`). It is suitable for adding `warm_sync_light`, but current isolated-frame mode does not model warm state.

Refinement: add a warm-seed sequence in the harness: full OFDM anchor frame first, then measured light frames without resetting warm timing state between anchor and measured frame unless the test intentionally starts a fresh trial. Also preserve the existing isolated-frame configs for regression comparison.

## Missed Or Reordered Work

1. Move M4 before M1-M3.
2. Add a small decoder API for expected-arrival hints rather than pulling protocol state into `StreamingDecoder`.
3. Add a policy/test milestone for false-positive math: candidate-count ratio, threshold value, and noise-only validation.
4. Update both GUI/TNC/simulator TX paths for first-OFDM full anchor, not just `StreamingEncoder`.
5. Update `CLAUDE.md` flow text: code has initial mode in CONNECT_ACK, while later MODE_CHANGE remains adaptive.

## Implementation Risk Summary

| Milestone | Risk | Primary Concern |
|---|---:|---|
| M1 | Medium-high | Timing state can fight the circular-buffer cursor if absolute sample bookkeeping is wrong. |
| M2 | High | Narrowing the wrong layer will not reduce false-positive candidates and can skip real frames. |
| M3 | High | Lower thresholds can create expensive false locks unless bounded by principled windowing and noise tests. |
| M4 | Medium | Needs consistent one-shot TX behavior across GUI, TNC, and simulator. |
| M5 | Medium-high | Matched-filter score must remain robust when cached CFO is stale or wrong. |
| M6 | Medium | State transitions must avoid flapping on isolated misses. |
| M7 | Medium-high | Policy changes can select OFDM in conditions where end-to-end sessions still fail. |
| M8 | Medium | Harness must measure warm sync, not accidentally keep testing isolated cold-light frames. |

## Proceed Conditions

Proceed to Phase 1 with these adjustments:

1. Implement M4 first as the bootstrap anchor.
2. Implement M1-M3 against absolute sample timing and explicit warm thresholds.
3. Treat M5 as required if M3 does not meet SNR=10.
4. Defer M7 until the warm-sync FER and cli_simulator multi-seed results support the lower OFDM entry floor.
