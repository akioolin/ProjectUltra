#include "sync/sync_controller.hpp"

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
    arrival_confidence_ = 0.0f;
    consecutive_misses_ = 0;
    next_expected_abs_ = 0;
    next_expected_valid_ = false;
    expected_frame_gap_ = 0;
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
    next_expected_abs_ = descriptor_end_abs;
    expected_frame_gap_ = expected_frame_gap_samples;
    next_expected_valid_ = true;
}

}  // namespace sync
}  // namespace ultra
