#pragma once

// Phase 0 (EFFECTIVE_SINR handoff §9): "define actual symbol timestamps and all
// estimator reset rules."
//
// This is documentation, not code. The arithmetic half is already executable and
// pinned by the `CyclicPrefixGeometry` test (tests/test_cyclic_prefix_geometry.cpp);
// what was missing is a single statement of WHICH clock each quantity is on and
// WHEN estimator state disappears. Both are prerequisites for comparing a channel
// estimate against simulator truth: truth H is a function of absolute sample time,
// and an estimate is only meaningful relative to the FFT window it was measured in.
//
// ============================================================================
// 1. WHERE A SYMBOL'S FFT WINDOW STARTS
// ============================================================================
//
//   abs_fft_start(s) = A_frame + (T + s) * S + CP
//
//     A_frame  absolute sample index of the frame's anchor = its FIRST training
//              (LTS) sample. Stored as absolute_training_start_sample_ and set by
//              setAbsoluteTrainingPosition() (waveform/ofdm_chirp_waveform.cpp).
//     T        number of training symbols before the data payload.
//     s        frame-relative data-symbol index, 0-based.
//     S        symbol stride = fft_size + cyclic_prefix + symbol_guard
//              (getSymbolDuration(), include/ultra/types.hpp:349).
//     CP       cyclic prefix, skipped at the head of each symbol before the FFT
//              (channel_equalizer_baseband.cpp:175). getCyclicPrefix() =
//              base_cp * (fft_size/512); MEDIUM base 48 => 96 samples = 2.00 ms
//              at fft 1024. Production is MEDIUM (presets::balanced()).
//
// The frame-granularity form of this is written out in production code at
// channel_equalizer_equalize.cpp:466-469:
//     anchor = pc.abs_train + pc.training_symbols * pc.symbol_samples
//
// ============================================================================
// 2. THREE CAVEATS THAT WILL SILENTLY CORRUPT A TRUTH-VS-ESTIMATE COMPARISON
// ============================================================================
//
// C1. THE RX SYNC CONVENTION IS NOT SAMPLE-EXACT WITH THE TX SYMBOL GRID.
//     Measured and documented in-tree at channel_equalizer_equalize.cpp:462-465:
//     "the receiver's sync convention puts the RX position up to ~0.7 symbol away
//     from the transmitted one (measured +796 samples on ITU Good)". There is no
//     per-symbol timing recovery that removes it, so whatever residual exists at
//     acquisition is carried unchanged through every symbol of the frame and,
//     via the fixed stride, through every frame of a burst group.
//
//     CONSEQUENCE: an estimate carries a timing-induced linear phase ramp
//     exp(-j*2*pi*k*delta/N) across carriers relative to truth. This is exactly
//     what the truth-H domain contract removes (see tests/test_channel_truth_h.cpp:
//     fit out a global complex scale AND a linear phase ramp, nothing more).
//     Failing to remove it does not look like a bug -- it looks like a channel
//     estimate that is catastrophically wrong, which is precisely how the
//     cross-pass genie in src/ofdm/genie_true_h.hpp produced 0/20 against a 19/20
//     baseline on a channel with no fading at all.
//
// C2. THE WIENER TIME AXIS IS FRAME-RELATIVE, NOT ABSOLUTE.
//     Observations are stamped wiener_symbol_base_ + current_data_symbol_index_
//     (channel_equalizer_pilot.cpp:796-797, read back at :1208-1212), but
//     wiener_symbol_base_ is 0 in production -- it is only made non-zero under
//     ULTRA_ITERATIVE_CHEST, which is default-OFF (src/ofdm/iterative_chest.hpp).
//     So the effective time index RESTARTS AT 0 EVERY FRAME. Any reasoning that
//     treats it as a session-absolute clock is wrong today.
//
//     Related latent inconsistency: per_carrier_raw_obs_symbol_ is stamped with
//     the BARE current_data_symbol_index_ (channel_equalizer_pilot.cpp:810) while
//     the adjacent Wiener push adds the base. Self-consistent while the base is 0;
//     the two clocks diverge the moment it is not.
//
// C3. ABSOLUTE SAMPLE INDICES ARE PER-EPOCH, NOT SESSION-MONOTONE.
//     StreamingDecoder::reset() sets total_fed_ = 0 (streaming_decoder.cpp:1349),
//     so the absolute timeline rewinds on every reset. Truth H must therefore be
//     sampled on the SAME epoch as the estimate it is compared against.
//
// ============================================================================
// 3. WHEN ESTIMATOR STATE IS DESTROYED
// ============================================================================
//
// Per frame (the common case, and the reason estimator state does NOT accumulate
// across a burst group unless ULTRA_ITERATIVE_CHEST is on):
//   ofdm_stream_processor.cpp:920-1001  processPresynced() -- runs for EVERY OFDM
//       data/control frame. Clears channel_estimate to (1,0), SNR state,
//       noise_variance to 0.1, pilot fading stats, previous pilot phases, LMS/RLS,
//       carrier erasure/EMA, all dd_qam16_*, Wiener history (SKIPPED iff
//       wiener_carry_armed_), the data-aided grid, carrier phase, constellation.
//       CFO and phase are DELIBERATELY preserved (:940-947).
//
// Full demodulator reset:
//   ofdm_stream_processor.cpp:1211-1266  OFDMDemodulator::reset() -- everything
//       above PLUS CFO -> 0 and chirp_cfo_estimated -> false.
//   streaming_sync_acquisition.cpp:510   waveform_->reset() at the top of EVERY
//       sync search.
//   streaming_ofdm_decode.cpp:2234, :2419  retry paths; both re-arm
//       setAbsoluteTrainingPosition() and CFO afterwards.
//
// Waveform replacement (all estimator state gone, new object):
//   streaming_decoder.cpp:782-789   setMode() on a waveform-mode change. Note the
//       old waveform is DEFERRED-retired, not destroyed inline -- destroying it
//       inline was a use-after-free against the decode thread (fixed 79fe287).
//   streaming_decoder.cpp:1010-1016 applyPendingConnectedOFDMMode() rebuilds it.
//
// Turnaround:
//   modem_engine.cpp:1171-1174  the pre-TX echo-clear is SKIPPED for connected
//       OFDM TX echo (opt-out ULTRA_WARM_TURNAROUND_OFF) precisely so warm-sync
//       state and the ring timeline survive the turnaround. Re-enabling that clear
//       wipes warm-sync every turnaround.
//
// ============================================================================
// 4. KNOWN GAP (not fixed here; Phase 0 is "no production behavior change")
// ============================================================================
//
// setAbsoluteTrainingPosition() is NOT called on the normal burst-continuation
// path (streaming_burst_interleave.cpp:692-725), so absolute_training_start_sample_
// retains the GROUP-HEAD value for frames 2..N of a burst group. Consequences:
// initial_phase_rad (ofdm_chirp_waveform.cpp:961) uses a stale reference -- benign
// when Hilbert pre-correction fires, live when |CFO| < 0.75 Hz and it is skipped
// (frame_demodulator.cpp:22-26); and genie::passContext().abs_train is wrong for
// continuation frames, so [eqtrace] abs= values and the genie anchor at
// channel_equalizer_equalize.cpp:466-469 are correct ONLY for group-head frames.
//
// That last point matters for anyone building on the oracles: any per-frame
// truth-vs-estimate comparison must either restrict itself to group-head frames or
// fix the continuation-path anchor first.
namespace ultra::ofdm::symbol_timing {

// Absolute sample index at which data symbol `s` of a frame begins its FFT window.
// See §1 above for the definition of each term.
inline long long absoluteFftWindowStart(long long frame_anchor_sample,
                                        int training_symbols,
                                        int data_symbol_index,
                                        int symbol_stride_samples,
                                        int cyclic_prefix_samples) {
    return frame_anchor_sample +
           static_cast<long long>(training_symbols + data_symbol_index) *
               static_cast<long long>(symbol_stride_samples) +
           static_cast<long long>(cyclic_prefix_samples);
}

}  // namespace ultra::ofdm::symbol_timing
