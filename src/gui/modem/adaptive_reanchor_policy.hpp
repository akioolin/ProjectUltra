#pragma once

#include "protocol/frame_v2.hpp"
#include "protocol/waveform_selection.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/types.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>

namespace ultra {
namespace gui {
namespace adaptive_reanchor_policy {

inline constexpr float kDefaultShortReanchorChirpMs = 100.0f;
inline constexpr float kMinShortReanchorChirpMs = 100.0f;
inline constexpr float kMaxShortReanchorChirpMs = 300.0f;

inline float shortReanchorChirpDurationMs() {
    static const float duration_ms = [] {
        const char* value = std::getenv("ULTRA_SHORT_REANCHOR_CHIRP_MS");
        if (!value || value[0] == '\0') {
            return kDefaultShortReanchorChirpMs;
        }

        char* end = nullptr;
        const float parsed = std::strtof(value, &end);
        if (end == value || !std::isfinite(parsed)) {
            return kDefaultShortReanchorChirpMs;
        }
        return std::clamp(parsed, kMinShortReanchorChirpMs, kMaxShortReanchorChirpMs);
    }();
    return duration_ms;
}

inline bool shouldUseShortReanchor(protocol::WaveformMode waveform,
                                   Modulation modulation,
                                   float fading_index) {
    if (waveform != protocol::WaveformMode::OFDM_CHIRP ||
        !ofdm_link_adaptation::isCoherentModulation(modulation) ||
        !std::isfinite(fading_index)) {
        return false;
    }

    // Reuse the selector's calibrated AWGN/fading boundary so the trigger is
    // tied to the same measured channel class that drives the data ladder.
    return fading_index >= protocol::kQAM16AwgnFadingMax;
}

}  // namespace adaptive_reanchor_policy
}  // namespace gui
}  // namespace ultra
