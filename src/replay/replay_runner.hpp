#pragma once

#include "replay/bundle_loader.hpp"
#include "replay/divergence_report.hpp"

#include <cstddef>
#include <string>

namespace ultra::replay {

struct ReplayOptions {
    std::string audio_side = "rx";
    bool realtime = false;
    size_t block_samples = 960;
    int drain_ms = 3000;
};

ReplayTimeline runReplay(const Bundle& bundle,
                         const ParsedTimeline& live,
                         const ReplayOptions& options);

} // namespace ultra::replay
