#pragma once

#include "protocol/frame_v2.hpp"
#include "protocol/connection_policy.hpp"
#include "protocol/waveform_selection.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/types.hpp"

namespace ultra {
namespace gui {
namespace adaptive_reanchor_policy {

inline float shortReanchorChirpDurationMs() {
    return static_cast<float>(
        protocol::connection_policy::wideOFDMShortReanchorChirpDurationMs());
}

inline bool shouldUseShortReanchor(protocol::WaveformMode waveform,
                                   Modulation modulation,
                                   float fading_index) {
    return protocol::connection_policy::shouldUseWideOFDMShortReanchor(
        waveform, modulation, fading_index);
}

}  // namespace adaptive_reanchor_policy
}  // namespace gui
}  // namespace ultra
