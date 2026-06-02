#pragma once

#include "ultra/fec.hpp"             // ChannelInterleaver
#include "fec/codec_factory.hpp"     // fec::CodecPtr (ICodec)

#include <memory>

namespace ultra {
namespace gui {

// FrameDecoder — the "soft bits -> frame" FEC decode stage, carved out of the StreamingDecoder
// god-class [component-decomposition-plan].
//
// Concern split: demodulation ("samples -> soft bits": the CFO pre-correction + the waveform's
// baseband/FFT/equalize) is FrameDemodulator's job; FEC decode ("soft bits -> frame": deinterleave +
// LDPC, the rate/interleave strategy) is a distinct concern and lives here. FrameDecoder owns the
// decode PRIMITIVES — the rate-switchable LDPC codec + the channel interleaver — and grows to own the
// decode ORCHESTRATION (decodeFrame / decodeMCDPSKFrame), driven by a FrameDecodeContext the decoder
// passes in (so FrameDecoder stays decoupled from the decoder's config).
//
// §7 C-FDec-1 (relocation): these primitives are transitional-public — the decode methods still live
// on the decoder and drive them as frame_decoder_.codec_/interleaver_, and the mode/rate machinery
// reconfigures them in place. They privatize behind FrameDecoder methods once decodeFrame moves in.
class FrameDecoder {
public:
    fec::CodecPtr codec_;
    std::unique_ptr<ChannelInterleaver> interleaver_;
};

}  // namespace gui
}  // namespace ultra
