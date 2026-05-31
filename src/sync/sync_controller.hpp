#pragma once

// SyncController — the single owner of OFDM sync STATE and the acquisition DECISION.
//
// Target architecture for the sync-acquisition refactor (docs/SYNC_ACQUISITION_FIX_PLAN_2026_05_31.md
// §7, building on docs/SYNC_SUBSYSTEM_AUDIT_2026_05_29.md §9). It collapses the current sprawl —
// detectors in chirp_sync/waveforms, orchestration in streaming_sync_acquisition.cpp, 12 loose
// state members on StreamingDecoder, policy in signal_policy/arrival_policy, 6 entry points, one
// flag flipped in 11 places — into ONE object with a 3-state machine, two detectors it CALLS, and
// one acceptance rulebook.
//
// What it OWNS:        the SyncMode, the timing prediction, confidence, miss counters, last_cfo,
//                      the which-detector/which-window/accept-or-not decision, the acceptance rule.
// What it does NOT own: the DSP detectors (IWaveform::detectSync / detectDataSync, chirp_sync —
//                      it CALLS them) and the demod + LDPC decode (the loop runs those and reports
//                      the outcome back via reportFrameOutcome).
//
// The core fix lives here, expressed once: in WARM contiguous-data, acceptance is
// position + LDPC (the `tentative` + reportFrameOutcome pair), NOT an LTS-correlation gate. The
// 0.90/0.52/0.50 correlation thresholds apply only to COLD / RE_ACQUIRE. MC-DPSK needs no special
// case: supportsDataPreamble()==false ⇒ the WARM "light LTS" call falls back to the full chirp.
//
// MIGRATION STATUS: SCAFFOLD (2026-05-31). Declarations + state only; not yet wired into the
// decode loop. Subsequent flag-gated steps move the state in, wrap the existing logic byte-
// identical, then land the WARM position+LDPC acceptance, then delete the dead paths. The whole
// migration is behind ULTRA_S16_WARM_HANDOFF until the AWGN@10 floor probe + no-regress pass.

#include "waveform/waveform_interface.hpp"   // IWaveform, SyncResult, SampleSpan
#include "protocol/frame_v2.hpp"             // protocol::WaveformMode
#include "sync/frame_arrival_policy.hpp"     // WarmSyncPhase + warm-sync timing helpers
#include "sync/signal_policy.hpp"            // LightSyncThresholds + light-sync acceptance

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ultra {
namespace sync {

// The three (and only three) acquisition states. Replaces the streaming_frame_arrival_policy::
// WarmSyncPhase {COLD,WARM,DEGRADED,RECOVERY} + warm_sync_active_ + expect_full_ofdm_anchor_
// tangle with one explicit machine.
enum class SyncMode {
    COLD,        // no timing lock — full chirp+LTS, wide window, strict threshold
    WARM,        // locked + predicting — group boundary re-anchors on the descriptor chirp;
                 // contiguous data accepted by position + LDPC (no correlation gate)
    RE_ACQUIRE,  // lost (LDPC failed N× at the predicted position) — force a full chirp next anchor
};

// One detect() tick's decision for the decode loop.
struct SyncDecision {
    bool   found     = false;  // a real lock happened (COLD / group-boundary chirp)
    bool   tentative = false;  // WARM contiguous: process at `pos`, let LDPC judge (NOT accepted yet)
    size_t pos       = 0;      // absolute sample position where the frame starts
    float  cfo       = 0.0f;   // carrier-frequency offset to apply
    SyncMode mode    = SyncMode::COLD;
};

// The connected-data ("light LTS") acceptance verdict for one detect tick.
// Returned by acceptLightSyncCandidate(): folds signal_policy::evaluateLightSyncCandidate,
// the ULTRA_S16_WARM_HANDOFF warm-override, and the WARM position-gating into one decision.
// When position_gated, the decoder maps position_gate_abs → sync_result.start_sample
// (= position_gate_abs - search_start), applies position_gate_cfo, and zeroes correlation.
struct LightSyncAcceptance {
    bool   found = false;             // accept this candidate as a frame start
    bool   position_gated = false;    // WARM position-gating fired (light-LTS corr at noise)
    size_t position_gate_abs = 0;     // absolute sample to process at (when position_gated)
    float  position_gate_cfo = 0.0f;  // known CFO to apply at the gated position
};

class SyncController {
public:
    // Reset to COLD for a new connection / mode change. `wf` is the active waveform whose detectors
    // we call; `is_coherent` selects the COLD/RE_ACQUIRE strict thresholds.
    void reset(protocol::WaveformMode mode, IWaveform* wf, bool is_coherent);

    // Per decode-loop tick: decide which detector/window and run it.
    //  COLD / group boundary : found=true only on a real chirp lock.
    //  WARM contiguous       : tentative=true at the predicted position (the loop demods there;
    //                          LDPC is the real accept/reject).
    SyncDecision detect(SampleSpan buffer, size_t buffer_len, size_t buffer_abs_start);

    // The loop calls this AFTER it demods + LDPC-decodes the frame at SyncDecision.pos.
    // This is the position+LDPC acceptance + the state transitions (advance prediction on ok;
    // count a miss and escalate WARM→RE_ACQUIRE after N consecutive failures).
    void reportFrameOutcome(bool ldpc_ok, size_t frame_end_abs);

    // A new burst group started (fresh descriptor anchor expected); seeds the WARM cadence.
    void noteGroupBoundary(size_t descriptor_end_abs, size_t expected_frame_gap_samples);

    // --- arrival-tracking transition logic (§7.4 A2; moved verbatim from StreamingDecoder) ---
    // These own the warm-sync phase machine + cadence prediction + confidence. They fold into
    // detect()/reportFrameOutcome() in the detect-dispatch chunk; kept as a faithful trio for now.
    // Callers hold StreamingDecoder::buffer_mutex_ (the *Locked convention); these do not lock.
    //   resetFrameArrivalTracking : connection/mode reset → COLD, clears prediction + memory.
    //   noteFrameArrivalSuccess   : a frame decoded at (start,end) → advance prediction, WARM.
    //   noteFrameArrivalSyncMiss  : a predicted frame missed → decay confidence, escalate phase.
    void resetFrameArrivalTracking();
    void noteFrameArrivalSuccess(size_t frame_start_abs, size_t frame_end_abs);
    void noteFrameArrivalSyncMiss();

    // Seed the WARM cadence from a known local-TX turnaround (the predicted next frame
    // arrives `delay_samples` after `total_fed_abs`) — used when this station's own TX
    // tells us when the peer's reply group will land. Sibling to the noteFrameArrival*
    // trio; moved verbatim from StreamingDecoder::seedExpectedFrameArrivalAfterSamples (the
    // connected_/OFDM_CHIRP guard + lock stay in the decoder forwarder).
    void seedArrivalAfterDelay(size_t total_fed_abs, size_t delay_samples, float confidence);

    // The connected-data light-LTS acceptance decision (§7.4 chunk B; moved verbatim from
    // StreamingDecoder::searchForSync). Folds signal_policy::evaluateLightSyncCandidate, the
    // ULTRA_S16_WARM_HANDOFF warm-override, and the WARM position-gating into one verdict, and
    // updates the owned sync_reject_streak_. The decoder still runs the detector and builds the
    // search buffer; it hands the candidate (detector_found, correlation) + window geometry
    // (search_start, search_window_len) here, then applies the returned position gate to
    // sync_result. `connected`/`is_coherent` mirror the decoder's connected_ / coherent-data flag.
    LightSyncAcceptance acceptLightSyncCandidate(
        bool detector_found, float correlation, bool is_coherent, bool connected,
        float known_cfo, size_t search_start, size_t search_window_len,
        const signal_policy::LightSyncThresholds& thresholds);

    // The warm-window PLANNING decision (§7.4 chunk-B tail; moved verbatim from
    // StreamingDecoder::searchForSync). Computes the s16 skip-short-lead + the expected
    // search anchor from the owned warm-sync state, calls frame_arrival_policy::
    // planWarmSearchWindow, and applies the short-reanchor-lead adjustment — returning the
    // window plan the decoder acts on (wait / activate / build buffer). The decoder still
    // owns the ring buffer: it derives oldest_abs / correlation_abs and does the actual
    // extraction. Caller holds StreamingDecoder::buffer_mutex_ (reads the same warm-sync
    // fields the noteFrameArrival* trio writes under that lock); this does not lock.
    frame_arrival_policy::WarmSearchWindowPlan planWarmSearch(
        bool use_light_search, bool use_short_reanchor_search,
        size_t short_reanchor_lead_samples, size_t total_fed, size_t oldest_abs,
        bool search_floor_valid, size_t search_floor_abs, size_t correlation_abs,
        size_t symbol_samples, size_t correlation_step);

    void setLogPrefix(const std::string& prefix) { log_prefix_ = prefix; }

    SyncMode mode() const { return mode_; }
    bool isWarm() const { return mode_ == SyncMode::WARM; }
    float lastCfo() const { return last_cfo_.load(); }

    // Phase-D prep: the 4-state WarmSyncPhase is provably a PURE FUNCTION of (warm_sync_active_,
    // consecutive_sync_misses_) — the transitions set it via phaseAfterSyncMiss(misses)/
    // phaseAfterSuccessfulFrame(), and active is cleared exactly when misses hits
    // kWarmSyncMissesBeforeRecovery. derivePhase() recomputes it; the equivalence
    // (stored == derivePhase()) is asserted after every transition and validated deterministically
    // by test_sync_controller_phase. The §7 collapse removes the stored enum and keeps this as the
    // basis for the 3-state SyncMode (phase==WARM ⟺ active && misses<kWarmSyncMissesBeforeDegraded;
    // DEGRADED wide-window ⟺ misses>=that; RECOVERY ⟺ !active && misses>=kWarmSyncMissesBeforeRecovery).
    frame_arrival_policy::WarmSyncPhase derivePhase() const {
        if (!warm_sync_active_) {
            return consecutive_sync_misses_ >= frame_arrival_policy::kWarmSyncMissesBeforeRecovery
                       ? frame_arrival_policy::WarmSyncPhase::RECOVERY
                       : frame_arrival_policy::WarmSyncPhase::COLD;
        }
        if (consecutive_sync_misses_ >= frame_arrival_policy::kWarmSyncMissesBeforeRecovery)
            return frame_arrival_policy::WarmSyncPhase::RECOVERY;
        if (consecutive_sync_misses_ >= frame_arrival_policy::kWarmSyncMissesBeforeDegraded)
            return frame_arrival_policy::WarmSyncPhase::DEGRADED;
        return frame_arrival_policy::WarmSyncPhase::WARM;
    }

    // Burst declared-z (§7.6): the single RX source of truth for "what LDPC lifting
    // size did this transfer's BURST_HEADER declare?". The per-frame extraction z is
    // DERIVED from (frame-class, declared-z) in the behavioral phase — never toggled.
    int activeBurstLiftingZ() const {
        return (have_burst_descriptor_ && last_burst_descriptor_.lifting_z == 81) ? 81 : 27;
    }

    // --- migration accessors (shell-move §7.5#1) ---------------------------------
    // Temporary getters/setters so StreamingDecoder can move its state in member-by-
    // member while the orchestration still lives there. Each folds into
    // detect()/reportFrameOutcome()/noteGroupBoundary() as that logic migrates.
    size_t expectedFrameGapSamples() const { return expected_frame_gap_samples_; }
    void setExpectedFrameGapSamples(size_t samples) { expected_frame_gap_samples_ = samples; }

    // Last-frame arrival memory (read-only; written only by noteFrameArrival* / seedArrivalAfterDelay
    // / resetFrameArrivalTracking). Accessors let the decoder build its FrameArrivalSnapshot and log
    // without touching the now-private fields.
    bool    lastFrameArrivalValid() const { return last_frame_arrival_valid_; }
    size_t  lastFrameStartSample() const { return last_frame_start_sample_; }
    size_t  lastFrameEndSample() const { return last_frame_end_sample_; }
    bool    lastFrameArrivalErrorValid() const { return last_frame_arrival_error_valid_; }
    int64_t lastFrameArrivalErrorSamples() const { return last_frame_arrival_error_samples_; }

    // --- TRANSITIONAL PUBLIC shell-move state (refactor §7.5#1) -------------------
    // These were StreamingDecoder members; relocated here verbatim so the (still-
    // external) orchestration reads/writes them as `sync_controller_.<name>`. They
    // are public ONLY during the shell-move and get re-privatized behind
    // detect()/reportFrameOutcome()/noteGroupBoundary() in the behavioral phase.
    // KEEP names identical to the old StreamingDecoder members (mechanical move).
    uint64_t sync_reject_streak_ = 0;   // consecutive COLD/RE_ACQUIRE light-sync rejects
    size_t   next_expected_frame_sample_ = 0;        // predicted next-frame absolute sample
    bool     next_expected_frame_sample_valid_ = false;
    float    frame_arrival_confidence_ = 0.0f;
    int      consecutive_sync_misses_ = 0;
    size_t   expected_frame_gap_samples_ = 0;        // cadence gap (§1.2 never-set bug)
    bool     expect_full_ofdm_anchor_ = false;       // force a full chirp on the next anchor
                                                     // (the 11-flip flag; becomes SyncMode in Phase D)
    bool     warm_sync_active_ = false;              // in the warm (locked+predicting) regime
                                                     // (collapses into SyncMode::WARM in Phase D)

    // Warm-sync phase machine (shell-moved 2026-05-31, §7.4 un-defer). Still the 4-state
    // WarmSyncPhase; collapses into SyncMode in the behavioral phase (Phase D). The transition
    // logic that drives it lives on the controller (noteFrameArrival* / seedArrivalAfterDelay);
    // the decoder still reads it for the snapshot + trace-log, so it stays public until Phase D.
    frame_arrival_policy::WarmSyncPhase warm_sync_phase_ =
        frame_arrival_policy::WarmSyncPhase::COLD;
    // (last-frame arrival memory is now PRIVATE — see below — read via the lastFrame* accessors.)
    // CFO acquisition state (§7.7#1). ATOMIC — touched by RX + control threads; the
    // CFO feedback loop (.load()/.store()) routes through here.
    std::atomic<float> last_cfo_{0.0f};

    // Burst z-state (§7.6): the transfer's declared LDPC-lifting descriptor. Latch
    // PERSISTS across the transfer (a fade-lost descriptor still decodes at the
    // declared z); reset() → COLD clears it. The per-frame extraction z is DERIVED
    // from (frame-class, this) in the behavioral phase, not a standalone toggle.
    bool have_burst_descriptor_ = false;
    protocol::v2::ControlFrame::BurstHeaderInfo last_burst_descriptor_{};

private:
    // Last-frame arrival memory (RE-PRIVATIZED §7.4: written only by noteFrameArrival* /
    // seedArrivalAfterDelay / resetFrameArrivalTracking; read by the decoder via the lastFrame*
    // accessors). NOT Phase-D-collapsing — this is genuine arrival state the controller keeps.
    bool     last_frame_arrival_valid_ = false;
    size_t   last_frame_start_sample_ = 0;
    size_t   last_frame_end_sample_ = 0;
    bool     last_frame_arrival_error_valid_ = false;
    int64_t  last_frame_arrival_error_samples_ = 0;

    // --- migrated from StreamingDecoder (audit §1.2) — the single home for this state ---
    SyncMode mode_ = SyncMode::COLD;
    protocol::WaveformMode waveform_mode_ = protocol::WaveformMode::OFDM_CHIRP;
    IWaveform* waveform_ = nullptr;        // borrowed; detectors live here (NOT owned)
    bool  is_coherent_ = false;

    // WARM→RE_ACQUIRE escalation threshold (consecutive predicted-position LDPC failures).
    static constexpr int kReacquireAfterMisses = 2;

    // --- Phase-D prep (TEMPORARY validation, §7 collapse) ------------------------------------
    // debugCheckPhaseInvariant() asserts the stored warm_sync_phase_ == derivePhase() after every
    // transition (logs a one-line PHASE-DERIVE-MISMATCH WARN on divergence — should NEVER fire).
    // derivePhase() is public (see above) so test_sync_controller_phase can drive all four states.
    void debugCheckPhaseInvariant(const char* where) const;

    // Log prefix mirrored from StreamingDecoder (e.g. "[BRAVO]") so warm-sync log lines keep
    // their station tag now that the transition logic emits them from here.
    std::string log_prefix_ = "StreamingDecoder";
};

}  // namespace sync
}  // namespace ultra
