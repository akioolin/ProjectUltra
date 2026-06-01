#pragma once

#include "sync/signal_policy.hpp"   // PilotCFOUpdate (return type of ingestPilotResidual)

#include <atomic>

namespace ultra {
namespace sync {

// CFOTracker — the single owner of the RX *tracked* carrier-frequency-offset state (the cached
// "best estimate" CFO that each frame's demod starts from). [component-decomposition-plan #7]
//
// Boundary: the chirp coarse-CFO (ChirpSync's dual-chirp gap) and the LTS/pilot residual
// (ChannelEqualizer) are ESTIMATORS — they also do timing / channel-estimation, so the CFO math
// stays in them. They FEED this tracker. CFOTracker owns the cached value, the chirp-seed drift
// clamp (`signal_policy::limitConnectedCFODrift`), and the per-frame pilot-feedback combine
// (`signal_policy::combinePilotCFO`).
//
// ⚠ FEEDBACK INVARIANT (docs/CFO_CORRECTION_FLOW.md §"Invariants" #4): after each frame is
// demodulated, the pilot-corrected CFO MUST be stored back here so the NEXT frame starts from the
// corrected value. Without it a wrong chirp CFO re-injects every frame → progressive phase drift →
// CW failures on fading. Never drop the store-after-demod.
//
// §7 C-CFO-1 (relocation): begins as the byte-identical relocation of the former
// `SyncController::last_cfo_` atomic — `tracked()`/`store()`/`reset()` are the exact `.load()`/
// `.store(v)`/`= 0` semantics it had. Follow-on slices (C-CFO-2/3) absorb the chirp-seed and the
// pilot-feedback arbitration into `seedFromChirp()` / `ingestPilotResidual()` so the policy-fn calls
// + the store live here instead of being woven through the decode paths.
class CFOTracker {
public:
    // The current tracked CFO (Hz). Read on acquisition (as the known CFO) and before each demod.
    float tracked() const { return cfo_.load(); }

    // §7 C-CFO-2: arbitrate a chirp-measured CFO against the tracked value. On a connected link a
    // multipath-distorted chirp can read a false CFO, so the per-frame drift is clamped to the
    // established estimate (signal_policy::limitConnectedCFODrift, logging the clamp). Returns the
    // accepted CFO; does NOT store (the caller applies any diag override, then stores the final value).
    float seedFromChirp(float measured_cfo, bool connected, const char* log_prefix) const;

    // §7 C-CFO-3: ingest the per-frame pilot/LTS residual — combine it (and the pre-correction that
    // was applied to the demod) with `current` via signal_policy::combinePilotCFO, then STORE the
    // accepted result as the new tracked CFO (this IS the feedback invariant — centralized here so a
    // call site can't drop the store). Returns the full update so the caller does its own logging /
    // sync_cfo_ / burst_cfo_ tail. `current` is the per-site baseline (tracked()/burst_cfo_/sync_cfo_).
    signal_policy::PilotCFOUpdate ingestPilotResidual(
        float pre_correction, float residual, float current, bool clamp_drift);

    // Store a CFO value — the chirp seed, or the post-demod pilot-corrected feedback (the invariant).
    // ATOMIC: touched by the RX + control threads, exactly like the former SyncController::last_cfo_.
    void store(float cfo_hz) { cfo_.store(cfo_hz); }

    // Reset to 0 for a new connection / mode change.
    void reset() { cfo_.store(0.0f); }

private:
    std::atomic<float> cfo_{0.0f};
};

}  // namespace sync
}  // namespace ultra
