#include "sync/cfo_tracker.hpp"

#include "sync/signal_policy.hpp"   // limitConnectedCFODrift + kMaxSyncCFODriftHz
#include "ultra/logging.hpp"        // LOG_MODEM

namespace ultra {
namespace sync {

// §7 C-CFO-2: moved verbatim from the post-found CFO-handling block of
// StreamingDecoder::searchForSync (the `limitConnectedCFODrift` clamp + its "CFO sanity" log).
// Same policy call, same log, same return (measured when un-clamped, accepted when clamped) →
// byte-identical; only the home moved. The caller still applies the genie-timing diag override and
// stores the final value back into the tracker.
float CFOTracker::seedFromChirp(float measured_cfo, bool connected, const char* log_prefix) const {
    const float known = cfo_.load();
    const auto decision = signal_policy::limitConnectedCFODrift(connected, measured_cfo, known);
    if (decision.clamped) {
        LOG_MODEM(INFO, "[%s] CFO sanity: measured=%.1f, known=%.1f, diff=%.1f > %.1f, using known",
                  log_prefix, measured_cfo, known, decision.diff_hz,
                  signal_policy::kMaxSyncCFODriftHz);
        return decision.accepted_cfo;  // Trust established CFO over noisy measurement
    }
    return measured_cfo;
}

// §7 C-CFO-3: moved from the per-decode-path "feed back pilot-corrected CFO to cached value" blocks.
// combinePilotCFO + the store(accepted) were repeated at 5 sites; centralizing the store here makes
// the feedback invariant (docs/CFO_CORRECTION_FLOW.md) un-droppable. The caller keeps its
// site-specific tail (logs / sync_cfo_ / burst_cfo_) using the returned update. Byte-identical: same
// combine, same stored value; the store just happens inside this call instead of a few lines later
// (nothing reads the tracker in between).
signal_policy::PilotCFOUpdate CFOTracker::ingestPilotResidual(
    float pre_correction, float residual, float current, bool clamp_drift) {
    const auto update = signal_policy::combinePilotCFO(pre_correction, residual, current, clamp_drift);
    cfo_.store(update.accepted_cfo);
    return update;
}

}  // namespace sync
}  // namespace ultra
