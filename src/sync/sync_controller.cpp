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

    const auto previous_phase = derivePhase();
    const auto update = frame_arrival_policy::updateOnSuccessfulFrame(
        next_expected_frame_sample_valid_,
        next_expected_frame_sample_,
        frame_arrival_confidence_,
        frame_start_abs,
        frame_end_abs,
        expectedFrameGapSamples());

    next_expected_frame_sample_valid_ = true;
    warm_sync_active_ = true;
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

    const auto new_phase = derivePhase();
    if (previous_phase != new_phase) {
        LOG_MODEM(INFO, "[%s] warm-sync state: %s -> %s",
                  log_prefix_.c_str(),
                  frame_arrival_policy::warmSyncPhaseName(previous_phase),
                  frame_arrival_policy::warmSyncPhaseName(new_phase));
    }
}

void SyncController::noteFrameArrivalSyncMiss() {
    const auto previous_phase = derivePhase();
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

    if (consecutive_sync_misses_ >= frame_arrival_policy::kWarmSyncMissesBeforeRecovery) {
        warm_sync_active_ = false;
        next_expected_frame_sample_valid_ = false;
        frame_arrival_confidence_ = 0.0f;
    }

    const auto new_phase = derivePhase();
    if (previous_phase != new_phase) {
        LOG_MODEM(INFO, "[%s] warm-sync state: %s -> %s (misses=%d)",
                  log_prefix_.c_str(),
                  frame_arrival_policy::warmSyncPhaseName(previous_phase),
                  frame_arrival_policy::warmSyncPhaseName(new_phase),
                  consecutive_sync_misses_);
    }
}

// --- connected-data light-LTS acceptance (§7.4 chunk B) --------------------------------------
// Moved verbatim from StreamingDecoder::searchForSync (the `if (!found)` acceptance block):
// signal_policy::evaluateLightSyncCandidate + the ULTRA_S16_WARM_HANDOFF warm-override +
// the WARM position-gating, folded into one verdict. Same computations, same order, same
// log format/prefix → byte-identical behavior + log output; only the home moved. The decoder
// still runs the detector, builds the search buffer, and applies the position gate to
// sync_result (start_sample/cfo_hz/correlation); this owns the sync_reject_streak_ update.
LightSyncAcceptance SyncController::acceptLightSyncCandidate(
    bool detector_found, float correlation, bool is_coherent, bool connected,
    float known_cfo, size_t search_start, size_t search_window_len,
    const signal_policy::LightSyncThresholds& thresholds) {
    LightSyncAcceptance result;

    // Reject clear false positives (noise floor is ~0.2-0.4)
    auto sync_decision = signal_policy::evaluateLightSyncCandidate(
        detector_found, correlation, is_coherent, connected,
        sync_reject_streak_, thresholds);
    // §16.8 step 2 (ULTRA_S16_WARM_HANDOFF): the coherent-QPSK
    // sync threshold is 0.90 because stale LTS phases can't be
    // recovered by DD tracking alone. In the warm-handoff regime we
    // are NOT stale — the BURST_HEADER just decoded with a fresh full
    // chirp+LTS anchor and seeded last_cfo_. This override is a
    // BACKSTOP for a group-start DATA frame whose light-LTS dips just
    // under 0.90 right after a known-good anchor. The PRIMARY fix for
    // group-boundary acquisition is re-arming the descriptor chirp
    // anchor every group (streaming_burst_interleave.cpp end-of-group),
    // which keeps the contiguous data correlating high (~0.91); this
    // override should rarely fire once that anchor is used.
    constexpr float kS16WarmHandoffMinCorrelation = 0.55f;
    const bool s16_warm_override =
        is_coherent &&
        derivePhase() == frame_arrival_policy::WarmSyncPhase::WARM &&
        sync_decision.rejected && detector_found &&
        correlation >= kS16WarmHandoffMinCorrelation;
    if (s16_warm_override) {
        LOG_MODEM(INFO,
            "[%s] s16-warm-handoff: ACCEPT light-LTS sync corr=%.2f "
            "(WARM phase, conf=%.2f, threshold-floor=%.2f); coherent "
            "0.90 gate bypassed",
            log_prefix_.c_str(), correlation,
            frame_arrival_confidence_, kS16WarmHandoffMinCorrelation);
        sync_decision.found = true;
        sync_decision.rejected = false;
        sync_decision.next_reject_streak = 0;
    } else if (detector_found && correlation < thresholds.min_confidence) {
        if (sync_decision.weak_accept) {
            LOG_MODEM(INFO, "[%s] DATA sync weak-accepted (corr=%.2f < %.2f, streak=%llu)",
                      log_prefix_.c_str(), correlation,
                      thresholds.min_confidence,
                      static_cast<unsigned long long>(sync_reject_streak_));
        } else if (sync_decision.rejected) {
            LOG_MODEM(INFO, "[%s] DATA sync rejected (corr=%.2f < %.2f, streak=%llu)",
                      log_prefix_.c_str(), correlation,
                      thresholds.min_confidence,
                      static_cast<unsigned long long>(sync_decision.next_reject_streak));
        }
    }

    // §16.8 WARM position-gating (low-SNR fix, 2026-05-31 — see
    // docs/SYNC_ACQUISITION_FIX_PLAN_2026_05_31.md). At low SNR the warm light-LTS
    // correlation floors at NOISE (~0.15 measured at DQPSK R1/4 AWGN@10), so the frame is
    // UNFINDABLE by search and the normal gate rejects it — even though the cadence
    // prediction is correct (descriptor-seeded, contiguous frames) and the data decodes
    // (legacy: 776 CW on the same signal). The audit §9.7 fix: in WARM with the narrow
    // predicted window, do NOT gate on LTS correlation — PROCESS at the predicted position
    // and let LDPC be the acceptance decision (the LTS there still gives the channel
    // estimate H). detectDataSync's reported position is noise here, so we use
    // next_expected. Engages ONLY when the normal correlation path already failed, so
    // higher-SNR locks (which find the true peak) are byte-identical. On a misprediction
    // the frame's LDPC simply fails → existing NACK / §16.4 full-chirp escalation handles
    // it. (§16.8 warm-handoff — now unconditional; promoted past ULTRA_S16_WARM_HANDOFF.)
    if (!sync_decision.found &&
        derivePhase() == frame_arrival_policy::WarmSyncPhase::WARM &&
        thresholds.narrow_expected_window &&
        next_expected_frame_sample_valid_ &&
        next_expected_frame_sample_ >= search_start &&
        (next_expected_frame_sample_ - search_start) < search_window_len) {
        result.position_gated = true;
        result.position_gate_abs = next_expected_frame_sample_;
        result.position_gate_cfo = known_cfo;
        sync_decision.found = true;
        sync_decision.rejected = false;
        sync_decision.next_reject_streak = 0;
        LOG_MODEM(INFO,
            "[%s] WARM position-gated: processing predicted frame at abs=%llu "
            "(light-LTS corr below noise floor; cadence-located, LDPC validates)",
            log_prefix_.c_str(),
            static_cast<unsigned long long>(next_expected_frame_sample_));
    }

    result.found = sync_decision.found;
    sync_reject_streak_ = sync_decision.next_reject_streak;
    return result;
}

void SyncController::seedArrivalAfterDelay(size_t total_fed_abs, size_t delay_samples,
                                           float confidence) {
    const auto previous_phase = derivePhase();
    warm_sync_active_ = true;
    next_expected_frame_sample_valid_ = true;
    next_expected_frame_sample_ = total_fed_abs + delay_samples;
    frame_arrival_confidence_ =
        frame_arrival_policy::clampConfidence(std::max(frame_arrival_confidence_, confidence));
    consecutive_sync_misses_ = 0;
    last_frame_arrival_error_valid_ = false;
    last_frame_arrival_error_samples_ = 0;

    LOG_MODEM(DEBUG,
              "[%s] warm-sync arrival seeded from local TX: now=%zu delay=%zu next=%zu confidence=%.2f",
              log_prefix_.c_str(), total_fed_abs, delay_samples,
              next_expected_frame_sample_, frame_arrival_confidence_);
    const auto new_phase = derivePhase();
    if (previous_phase != new_phase) {
        LOG_MODEM(INFO, "[%s] warm-sync state: %s -> %s",
                  log_prefix_.c_str(),
                  frame_arrival_policy::warmSyncPhaseName(previous_phase),
                  frame_arrival_policy::warmSyncPhaseName(new_phase));
    }
}

// --- warm-window planning (§7.4 chunk-B tail) ------------------------------------------------
// Moved verbatim from StreamingDecoder::searchForSync (the s16_skip_short_lead +
// expected_sync_search_sample + planWarmSearchWindow + short-reanchor-lead adjustment). Same
// computations, same order, no log output → byte-identical. The decoder still derives the ring
// values (oldest_abs / correlation_abs) it passes in and does the actual buffer extraction.
// R4: the short-chirp re-anchor was removed (superseded by warm-handoff, now default), so the
// warm search anchors directly at next_expected_frame_sample_ — no lead shift, no post-adjust.
frame_arrival_policy::WarmSearchWindowPlan SyncController::planWarmSearch(
    bool use_light_search, size_t total_fed, size_t oldest_abs,
    bool search_floor_valid, size_t search_floor_abs, size_t correlation_abs,
    size_t symbol_samples, size_t correlation_step) {
    return frame_arrival_policy::planWarmSearchWindow(
        use_light_search,
        warm_sync_active_,
        next_expected_frame_sample_valid_,
        next_expected_frame_sample_,
        frame_arrival_confidence_,
        consecutive_sync_misses_,
        total_fed,
        oldest_abs,
        search_floor_valid,
        search_floor_abs,
        correlation_abs,
        symbol_samples,
        correlation_step);
}

}  // namespace sync
}  // namespace ultra
