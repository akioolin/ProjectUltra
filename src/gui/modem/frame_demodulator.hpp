#pragma once

#include <cstddef>
#include <vector>

namespace ultra {
namespace gui {

// FrameDemodulator — the per-frame RX demodulation stage(s), carved out of the StreamingDecoder
// god-class [component-decomposition-plan #6].
//
// SEED scope (this slice): the CFO PRE-CORRECTION stage — apply the tracked CFO to a frame's raw
// samples (a Hilbert analytic-signal frequency shift) so the waveform demod sees a near-zero
// residual. It owns `pre_correction_cfo_` (how much CFO was pre-applied), which CFOTracker's
// pilot-feedback (`ingestPilotResidual`) consumes to add the pre-corrected amount back. So the seam
// is a clean producer/consumer: FrameDemodulator produces it, CFOTracker consumes it.
//
// It grows (follow-on carves) to own the rest of the per-frame demod orchestration — frame extract
// -> pre-correct -> waveform.process -> LDPC (decodeFrame) — and eventually the per-frame demod
// params (sync_cfo_/sync_snr_/sync_correlation_).
class FrameDemodulator {
public:
    // Hilbert analytic-signal CFO pre-correction on a frame's samples (in place). Sub-0.75 Hz offsets
    // are left to the demodulator's baseband path (the finite analytic filter would ripple the LTS).
    // Returns the CFO actually applied (0 if skipped) and records it as preCorrectionCfo().
    float applyCFOPreCorrection(std::vector<float>& samples, float cfo_hz,
                                size_t absolute_start_sample, const char* log_prefix);

    // The CFO the last applyCFOPreCorrection() applied — read by the decode-CFO decision and added
    // back by the pilot feedback. Zeroed at frame boundaries where no pre-correction ran.
    float preCorrectionCfo() const { return pre_correction_cfo_; }
    void  resetPreCorrection() { pre_correction_cfo_ = 0.0f; }

private:
    float pre_correction_cfo_ = 0.0f;  // CFO used for last pre-correction (for feedback adjustment)
};

}  // namespace gui
}  // namespace ultra
