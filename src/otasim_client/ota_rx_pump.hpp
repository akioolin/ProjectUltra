#pragma once

#include "otasim_client/ota_audio_backend.hpp"

#include <cstddef>
#include <vector>

namespace ultra::otasim_client {

// Single source of truth for draining OTASim RX audio into a consumer.
//
// OTASim serves a CONTINUOUS, wall-clock-faithful RX line — silence included (as
// zero blocks on a clean channel, noise on a faded one). getRxSamples() is a
// real-time PULL: an empty read means "caught up to the current medium position,
// nothing new YET", NOT "silence to fabricate". So this drains ONLY real samples
// (chunked, break-on-empty) and NEVER zero-pads. Padding would inject samples the
// medium never produced, pushing the decoder's sample-clock off the session clock
// — the bug that broke TNC multi-group burst transfer (docs/CHANGELOG.md
// 2026-06-02: spurious CFO on a zero-CFO channel from fabricated filler).
//
// Both frontends call this so the feed discipline is structurally identical and
// cannot drift: the GUI (App::pollOtaRx) and the headless ultra_tnc tick loop.
// `consume(samples)` runs once per non-empty chunk — feed the modem, and whatever
// else the frontend needs (record, waterfall, monitor). Returns total samples
// drained. The caller owns any locking; this does none.
template <typename Consume>
inline std::size_t drainOtaRx(OtaAudioBackend& ota, Consume&& consume,
                              std::size_t chunk_samples = 2048,
                              int max_chunks = 8) {
    std::size_t total = 0;
    for (int i = 0; i < max_chunks; ++i) {
        std::vector<float> samples = ota.getRxSamples(chunk_samples);
        if (samples.empty()) {
            break;  // real-time pull: caught up, nothing new yet — do NOT fabricate
        }
        total += samples.size();
        consume(samples);
    }
    return total;
}

}  // namespace ultra::otasim_client
