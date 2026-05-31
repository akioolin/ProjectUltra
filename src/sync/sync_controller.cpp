#include "sync/sync_controller.hpp"

#include "ultra/logging.hpp"   // LOG_MODEM (shared logging header; src/sync already uses LOG_SYNC)

// SyncController — SCAFFOLD implementation (2026-05-31).
//
// These are intentionally inert stubs: the object exists and compiles, but it is NOT yet wired
// into the decode loop, so behavior is unchanged. The migration (docs/SYNC_ACQUISITION_FIX_PLAN
// _2026_05_31.md §7.5) fills these in step by step, flag-gated:
//   1. detect()             ← move the detector dispatch from streaming_sync_acquisition.cpp
//   2. reportFrameOutcome() ← the WARM position+LDPC acceptance + COLD/WARM/RE_ACQUIRE transitions
//   3. noteGroupBoundary()  ← the descriptor-anchor re-arm + cadence seed
// Until then nothing calls these.

namespace ultra {
namespace sync {

void SyncController::reset(protocol::WaveformMode mode, IWaveform* wf, bool is_coherent) {
    mode_ = SyncMode::COLD;
    waveform_mode_ = mode;
    waveform_ = wf;
    is_coherent_ = is_coherent;
    last_cfo_ = 0.0f;
    frame_arrival_confidence_ = 0.0f;
    consecutive_sync_misses_ = 0;
    next_expected_frame_sample_ = 0;
    next_expected_frame_sample_valid_ = false;
    expected_frame_gap_samples_ = 0;
    have_burst_descriptor_ = false;
    last_burst_descriptor_ = {};
}

SyncDecision SyncController::detect(SampleSpan buffer, size_t buffer_len, size_t buffer_abs_start) {
    // SCAFFOLD: detector dispatch not migrated yet — returns "no decision".
    (void)buffer;
    (void)buffer_len;
    (void)buffer_abs_start;
    SyncDecision d;
    d.mode = mode_;
    return d;
}

void SyncController::reportFrameOutcome(bool ldpc_ok, size_t frame_end_abs) {
    // SCAFFOLD: position+LDPC acceptance + state transitions land here.
    (void)ldpc_ok;
    (void)frame_end_abs;
}

void SyncController::noteGroupBoundary(size_t descriptor_end_abs, size_t expected_frame_gap_samples) {
    next_expected_frame_sample_ = descriptor_end_abs;
    expected_frame_gap_samples_ = expected_frame_gap_samples;
    next_expected_frame_sample_valid_ = true;
}

// --- arrival-tracking transition logic (§7.4 A2) ---------------------------------------------
// Moved verbatim from StreamingDecoder::{resetFrameArrivalTrackingLocked, noteFrameArrival
// SuccessLocked (minus the connected_/OFDM_CHIRP guard, which stays in the decoder forwarder),
// noteFrameArrivalSyncMissLocked}. Same computations, same order, same log format/prefix → the
// behavior + log output are byte-identical; only the home moved.

void SyncController::resetFrameArrivalTracking() {
    warm_sync_active_ = false;
    warm_sync_phase_ = frame_arrival_policy::WarmSyncPhase::COLD;
    next_expected_frame_sample_valid_ = false;
    next_expected_frame_sample_ = 0;
    frame_arrival_confidence_ = 0.0f;
    consecutive_sync_misses_ = 0;
    last_frame_arrival_valid_ = false;
    last_frame_start_sample_ = 0;
    last_frame_end_sample_ = 0;
    last_frame_arrival_error_valid_ = false;
    last_frame_arrival_error_samples_ = 0;
}

void SyncController::noteFrameArrivalSuccess(size_t frame_start_abs, size_t frame_end_abs) {
    if (!next_expected_frame_sample_valid_ &&
        !expect_full_ofdm_anchor_ &&
        expectedFrameGapSamples() == 0) {
        return;
    }

    const auto previous_phase = warm_sync_phase_;
    const auto update = frame_arrival_policy::updateOnSuccessfulFrame(
        next_expected_frame_sample_valid_,
        next_expected_frame_sample_,
        frame_arrival_confidence_,
        frame_start_abs,
        frame_end_abs,
        expectedFrameGapSamples());

    next_expected_frame_sample_valid_ = true;
    warm_sync_active_ = true;
    warm_sync_phase_ = frame_arrival_policy::phaseAfterSuccessfulFrame();
    next_expected_frame_sample_ = update.next_expected_frame_sample;
    frame_arrival_confidence_ = update.confidence;
    consecutive_sync_misses_ = update.consecutive_sync_misses;
    last_frame_arrival_valid_ = true;
    last_frame_start_sample_ = frame_start_abs;
    last_frame_end_sample_ = frame_end_abs;
    last_frame_arrival_error_valid_ = update.has_arrival_error;
    last_frame_arrival_error_samples_ = update.arrival_error_samples;

    if (update.has_arrival_error) {
        LOG_MODEM(DEBUG, "[%s] warm-sync arrival: start=%zu end=%zu next=%zu error=%lld confidence=%.2f",
                  log_prefix_.c_str(), frame_start_abs, frame_end_abs,
                  next_expected_frame_sample_,
                  static_cast<long long>(last_frame_arrival_error_samples_),
                  frame_arrival_confidence_);
    } else {
        LOG_MODEM(DEBUG, "[%s] warm-sync arrival seeded: start=%zu end=%zu next=%zu confidence=%.2f",
                  log_prefix_.c_str(), frame_start_abs, frame_end_abs,
                  next_expected_frame_sample_, frame_arrival_confidence_);
    }

    if (previous_phase != warm_sync_phase_) {
        LOG_MODEM(INFO, "[%s] warm-sync state: %s -> %s",
                  log_prefix_.c_str(),
                  frame_arrival_policy::warmSyncPhaseName(previous_phase),
                  frame_arrival_policy::warmSyncPhaseName(warm_sync_phase_));
    }
}

void SyncController::noteFrameArrivalSyncMiss() {
    const auto previous_phase = warm_sync_phase_;
    consecutive_sync_misses_ = frame_arrival_policy::incrementSyncMisses(consecutive_sync_misses_);
    frame_arrival_confidence_ =
        frame_arrival_policy::confidenceAfterSyncMiss(frame_arrival_confidence_);

    if (next_expected_frame_sample_valid_ && last_frame_arrival_valid_) {
        const size_t last_duration =
            last_frame_end_sample_ >= last_frame_start_sample_
                ? (last_frame_end_sample_ - last_frame_start_sample_)
                : 0;
        const size_t cadence = last_duration + expectedFrameGapSamples();
        if (cadence > 0) {
            next_expected_frame_sample_ += cadence;
        }
    }

    warm_sync_phase_ = frame_arrival_policy::phaseAfterSyncMiss(consecutive_sync_misses_);
    if (warm_sync_phase_ == frame_arrival_policy::WarmSyncPhase::RECOVERY) {
        warm_sync_active_ = false;
        next_expected_frame_sample_valid_ = false;
        frame_arrival_confidence_ = 0.0f;
    }

    if (previous_phase != warm_sync_phase_) {
        LOG_MODEM(INFO, "[%s] warm-sync state: %s -> %s (misses=%d)",
                  log_prefix_.c_str(),
                  frame_arrival_policy::warmSyncPhaseName(previous_phase),
                  frame_arrival_policy::warmSyncPhaseName(warm_sync_phase_),
                  consecutive_sync_misses_);
    }
}

}  // namespace sync
}  // namespace ultra
