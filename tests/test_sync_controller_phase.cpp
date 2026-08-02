// test_sync_controller_phase — Phase-D equivalence guard.
//
// Validates DETERMINISTICALLY that the legacy 4-state WarmSyncPhase machine
// {COLD,WARM,DEGRADED,RECOVERY} is a PURE FUNCTION of (warm_sync_active_,
// consecutive_sync_misses_), i.e. the stored warm_sync_phase_ always equals
// derivePhase(). This is the safety proof for the §7 collapse to the 3-state
// SyncMode: the GUI fading runs (Good@12) never reach DEGRADED/RECOVERY, so the
// risky branches can only be exercised here. Drives the controller through every
// state + transition boundary (the 2/4 miss thresholds) and asserts equivalence
// after each step. Stays as a permanent regression guard on the derivation.

#include "sync/sync_controller.hpp"
#include "protocol/frame_v2.hpp"

#include <iostream>
#include "test_env_compat.hpp"

using namespace ultra;
using sync::SyncController;
using Phase = sync::frame_arrival_policy::WarmSyncPhase;

namespace {

int tests_run = 0;
int tests_failed = 0;

const char* name(Phase p) { return sync::frame_arrival_policy::warmSyncPhaseName(p); }

#define CHECK(cond, msg)                                       \
    do {                                                       \
        ++tests_run;                                           \
        if (!(cond)) {                                         \
            ++tests_failed;                                    \
            std::cout << "FAIL: " << msg << "\n";              \
        }                                                      \
    } while (0)

// §7 collapse: the stored 4-state field is gone; the phase is DERIVED from
// (warm_sync_active_, consecutive_sync_misses_). This guards the derived machine —
// derivePhase() must return the expected state for the known miss sequence, incl.
// the exact 2/4 boundaries.
void expectPhase(SyncController& sc, Phase expected, const char* where) {
    CHECK(sc.derivePhase() == expected,
          std::string("wrong phase @") + where + ": got=" + name(sc.derivePhase()) +
              " expected=" + name(expected));
}

void test_phase_march() {
    SyncController sc;
    sc.reset(protocol::WaveformMode::OFDM_CHIRP, /*wf=*/nullptr, /*is_coherent=*/true);
    sc.resetFrameArrivalTracking();
    expectPhase(sc, Phase::COLD, "after-reset");

    // Seed a warm cadence (active=true, misses=0) → WARM.
    sc.seedArrivalAfterDelay(/*total_fed_abs=*/48000, /*delay_samples=*/4800, /*confidence=*/0.8f);
    expectPhase(sc, Phase::WARM, "after-seed");

    // March consecutive sync misses across the 2/4 thresholds.
    sc.noteFrameArrivalSyncMiss();  // misses=1
    expectPhase(sc, Phase::WARM, "miss=1");
    sc.noteFrameArrivalSyncMiss();  // misses=2  -> DEGRADED (kWarmSyncMissesBeforeDegraded)
    expectPhase(sc, Phase::DEGRADED, "miss=2");
    sc.noteFrameArrivalSyncMiss();  // misses=3
    expectPhase(sc, Phase::DEGRADED, "miss=3");
    sc.noteFrameArrivalSyncMiss();  // misses=4  -> RECOVERY (active cleared)
    expectPhase(sc, Phase::RECOVERY, "miss=4");
    sc.noteFrameArrivalSyncMiss();  // misses=5  -> RECOVERY (active already false)
    expectPhase(sc, Phase::RECOVERY, "miss=5");
}

void test_recovery_back_to_warm() {
    SyncController sc;
    sc.reset(protocol::WaveformMode::OFDM_CHIRP, nullptr, true);
    sc.resetFrameArrivalTracking();
    sc.seedArrivalAfterDelay(48000, 4800, 0.8f);
    for (int i = 0; i < 4; ++i) sc.noteFrameArrivalSyncMiss();  // -> RECOVERY
    expectPhase(sc, Phase::RECOVERY, "pre-recover");

    // A successful frame must snap straight back to WARM (misses reset to 0). The
    // success path early-returns unless there is a prediction/anchor/gap; give it a gap.
    sc.setExpectedFrameGapSamples(100);
    sc.noteFrameArrivalSuccess(/*frame_start_abs=*/96000, /*frame_end_abs=*/100000);
    expectPhase(sc, Phase::WARM, "after-success-recover");
}

void test_reset_returns_cold() {
    SyncController sc;
    sc.reset(protocol::WaveformMode::OFDM_CHIRP, nullptr, true);
    sc.resetFrameArrivalTracking();
    sc.seedArrivalAfterDelay(48000, 4800, 0.8f);
    sc.noteFrameArrivalSyncMiss();
    sc.noteFrameArrivalSyncMiss();
    expectPhase(sc, Phase::DEGRADED, "pre-reset");
    sc.resetFrameArrivalTracking();
    expectPhase(sc, Phase::COLD, "after-reset2");
}

void test_clean_group_honors_announced_light_anchor() {
    SyncController sc;
    sc.reset(protocol::WaveformMode::OFDM_CHIRP, nullptr, true);
    sc.resetFrameArrivalTracking();
    sc.seedArrivalAfterDelay(48000, 4800, 0.8f);
    sc.noteFrameArrivalSyncMiss();
    sc.setNextGroupLightAnchor(true);

    sc.noteGroupDelivered(/*group_seq=*/7, /*retransmission_required=*/false);

    expectPhase(sc, Phase::WARM, "clean-group-light-anchor");
    CHECK(sc.consecutiveSyncMisses() == 0,
          "clean delivered group must refresh the warm miss counter");
    CHECK(sc.frameArrivalConfidence() >= 0.5f,
          "clean delivered group must preserve warm confidence");
    CHECK(!sc.expect_full_ofdm_anchor_,
          "clean group must honor its descriptor's NEXT_LIGHT announcement");
}

void test_failed_group_forces_full_anchor_without_cooling_sync() {
    SyncController sc;
    sc.reset(protocol::WaveformMode::OFDM_CHIRP, nullptr, true);
    sc.resetFrameArrivalTracking();
    sc.seedArrivalAfterDelay(48000, 4800, 0.8f);
    sc.noteFrameArrivalSyncMiss();
    sc.setNextGroupLightAnchor(true);  // stale announcement from the failed group

    sc.noteGroupDelivered(/*group_seq=*/8, /*retransmission_required=*/true);

    expectPhase(sc, Phase::WARM, "failed-group-full-anchor");
    CHECK(sc.consecutiveSyncMisses() == 0,
          "acquired failed group must still refresh the warm miss counter");
    CHECK(sc.frameArrivalConfidence() >= 0.5f,
          "acquired failed group must preserve warm confidence");
    CHECK(sc.expect_full_ofdm_anchor_,
          "group requiring retransmission must override stale NEXT_LIGHT and search full");
}

}  // namespace

int main() {
    // noteGroupDelivered's anchor policy is meaningful only with periodic light
    // descriptors enabled. Set before its function-local policy value is first read.
    setenv("ULTRA_ANCHOR_SKIP_K", "2", 1);
    test_phase_march();
    test_recovery_back_to_warm();
    test_reset_returns_cold();
    test_clean_group_honors_announced_light_anchor();
    test_failed_group_forces_full_anchor_without_cooling_sync();

    std::cout << (tests_failed == 0 ? "PASS" : "FAIL") << ": " << (tests_run - tests_failed)
              << "/" << tests_run << " sync-controller phase checks\n";
    return tests_failed == 0 ? 0 : 1;
}
