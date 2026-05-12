#pragma once

#include "replay/event_timeline.hpp"

#include <iosfwd>
#include <string>
#include <vector>

namespace ultra::replay {

struct ReplayTimeline {
    std::vector<ModeSpec> mode_events;
    std::vector<FrameObservation> frames;
    std::vector<std::string> warnings;
};

enum class MatchKind {
    Exact,
    Divergent,
    LiveOnly,
    ReplayOnly,
};

struct DivergenceRow {
    MatchKind kind = MatchKind::Exact;
    int frame_seq = -1;
    bool has_frame_seq = false;
    FrameObservation live;
    bool has_live = false;
    FrameObservation replay;
    bool has_replay = false;
    std::string reason;
};

struct DivergenceSummary {
    size_t live_frames = 0;
    size_t replay_frames = 0;
    size_t comparable_live_frames = 0;
    size_t comparable_replay_frames = 0;
    size_t exact_matches = 0;
    size_t divergent = 0;
    size_t live_only = 0;
    size_t replay_only = 0;
    size_t unkeyed_live = 0;
    size_t unkeyed_replay = 0;
};

struct DivergenceReport {
    DivergenceSummary summary;
    std::vector<DivergenceRow> rows;
    std::vector<ModeSpec> live_mode_events;
    std::vector<ModeSpec> replay_mode_events;
    std::vector<std::string> warnings;
};

DivergenceReport compareTimelines(const ParsedTimeline& live,
                                  const ReplayTimeline& replay);
std::string renderTextReport(const DivergenceReport& report);
std::string renderJsonReport(const DivergenceReport& report);

} // namespace ultra::replay
