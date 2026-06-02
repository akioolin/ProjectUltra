#pragma once

#include "ultra/fec.hpp"             // ChannelInterleaver
#include "fec/codec_factory.hpp"     // fec::CodecPtr (ICodec)

#include <cstddef>
#include <memory>
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

    // --- decode primitives (§7 C-FD-2a: relocated from StreamingDecoder) ----------------------
    // TRANSITIONAL PUBLIC during the carve: the decode methods (decodeFrame/decodeMCDPSKFrame) still
    // live on the decoder and drive these as frame_demodulator_.codec_/interleaver_, and the
    // mode/rate machinery reconfigures them in place. They privatize behind FrameDemodulator methods
    // once decodeFrame moves in (FD-2b). The LDPC codec (rate-switchable) + the channel interleaver.
    fec::CodecPtr codec_;
    std::unique_ptr<ChannelInterleaver> interleaver_;

private:
    float pre_correction_cfo_ = 0.0f;  // CFO used for last pre-correction (for feedback adjustment)
};

}  // namespace gui
}  // namespace ultra
