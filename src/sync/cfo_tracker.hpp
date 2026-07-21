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

    // §7 C-CFO-2: arbitrate a chirp-measured CFO against the tracked value, using the chirp's
    // CORRELATION as confidence. A multipath-distorted (low-correlation) chirp reads a false CFO,
    // so a large jump is rejected to the tracked value — at EVERY stage including the pre-connect
    // PING, so a phantom never establishes — and an already-established CFO is additionally
    // protected on a connected link (signal_policy::limitConnectedCFODrift, logging the clamp).
    // Returns the accepted CFO; does NOT store (the caller applies any diag override, then stores).
    float seedFromChirp(float measured_cfo, float correlation, bool connected,
                        const char* log_prefix) const;

    // BUG-ANCHOR-CFO-KILL (2026-07-05): seed for a full dual-chirp re-anchor on a
    // CONNECTED OFDM link. Once the tracker has been pilot-refined since the last
    // reset, the chirp is a TIMING event only — the warm tracked CFO (<0.1 Hz) is
    // kept and the fade-jittered gap estimate (sigma ~0.3-1.15 Hz) discarded
    // (signal_policy::connectedAnchorCFOSeed). Before the first pilot refine the
    // cold rules apply. Does NOT store (caller applies diag overrides, then
    // stores — a warm-kept seed stores tracked->tracked, a no-op, so the CFO
    // feedback invariant is untouched).
    float seedFromChirpConnectedAnchor(float measured_cfo, float correlation,
                                       const char* log_prefix) const;

    // True once ingestPilotResidual refined the estimate since the last reset.
    bool pilotSeeded() const { return pilot_seeded_.load(); }

    // BUG-ANCHOR-CFO-KILL completion: the warm-keep is only safe while the warm
    // value is PROVEN — burst-frame pilot residuals are ingested before any LDPC
    // verdict exists (cross-frame interleave decodes at group end), so a crater
    // stretch feeds the tracker noise and it random-walks (measured -0.10 ->
    // +0.29 across a crater run once the chirp stopped re-centering it). A
    // DELIVERED group certifies the warm estimate; a 0/N group revokes the
    // certificate, so the next full anchor takes the chirp again and re-centers.
    void certifyWarm() {
        certified_cfo_.store(cfo_.load());  // snapshot the PROVEN CFO (this group decoded with it)
        has_certified_.store(true);
        pilot_seeded_.store(true);
    }
    void revokeWarm() { pilot_seeded_.store(false); }

    // CHEAP RE-ANCHOR (ULTRA_CHEAP_REANCHOR): restore the last CERTIFIED CFO — the value
    // a delivered group proved — WITHOUT a full dual-chirp re-anchor. A fade is an
    // amplitude event: warm TIMING survives it; only the pilot-tracked CFO may have
    // WALKED (noisy pilots ingested before the LDPC verdict, BUG-ANCHOR-CFO-KILL). The
    // full chirp (~1.2 s) re-acquires timing too — overkill. Rolling the CFO back to the
    // last proven value un-poisons it for free, so warm sync can retry the next group
    // without the chirp tax. Returns false if nothing certified yet (keep current).
    bool rollbackToCertified() {
        if (!has_certified_.load()) return false;
        cfo_.store(certified_cfo_.load());
        return true;
    }

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

    // Reset to 0 for a new connection / mode change (clears the pilot-refined mark).
    void reset() {
        cfo_.store(0.0f);
        pilot_seeded_.store(false);
        has_certified_.store(false);
        certified_cfo_.store(0.0f);
    }

private:
    std::atomic<float> cfo_{0.0f};
    std::atomic<bool> pilot_seeded_{false};
    std::atomic<float> certified_cfo_{0.0f};  // last DELIVERED-group CFO (cheap re-anchor)
    std::atomic<bool> has_certified_{false};
};

}  // namespace sync
}  // namespace ultra
