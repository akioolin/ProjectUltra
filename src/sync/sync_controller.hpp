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

#include <cstddef>

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

    SyncMode mode() const { return mode_; }
    bool isWarm() const { return mode_ == SyncMode::WARM; }
    float lastCfo() const { return last_cfo_; }

    // --- migration accessors (shell-move §7.5#1) ---------------------------------
    // Temporary getters/setters so StreamingDecoder can move its state in member-by-
    // member while the orchestration still lives there. Each folds into
    // detect()/reportFrameOutcome()/noteGroupBoundary() as that logic migrates.
    size_t expectedFrameGapSamples() const { return expected_frame_gap_samples_; }
    void setExpectedFrameGapSamples(size_t samples) { expected_frame_gap_samples_ = samples; }

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

private:
    // --- migrated from StreamingDecoder (audit §1.2) — the single home for this state ---
    SyncMode mode_ = SyncMode::COLD;
    protocol::WaveformMode waveform_mode_ = protocol::WaveformMode::OFDM_CHIRP;
    IWaveform* waveform_ = nullptr;        // borrowed; detectors live here (NOT owned)
    bool  is_coherent_ = false;
    float last_cfo_ = 0.0f;                // was StreamingDecoder::last_cfo_ (atomic move pending)

    // WARM→RE_ACQUIRE escalation threshold (consecutive predicted-position LDPC failures).
    static constexpr int kReacquireAfterMisses = 2;
};

}  // namespace sync
}  // namespace ultra
