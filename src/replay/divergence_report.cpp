#include "replay/divergence_report.hpp"

#include "replay/json_util.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace ultra::replay {
namespace {

struct ModeSeqKey {
    int seq = -1;
    std::string mode;

    bool operator<(const ModeSeqKey& other) const {
        if (seq != other.seq) {
            return seq < other.seq;
        }
        return mode < other.mode;
    }
};

std::string matchKindString(MatchKind kind) {
    switch (kind) {
        case MatchKind::Exact: return "YES";
        case MatchKind::Divergent: return "DIVERGENT";
        case MatchKind::LiveOnly: return "LIVE_ONLY";
        case MatchKind::ReplayOnly: return "REPLAY_ONLY";
    }
    return "UNKNOWN";
}

std::string fmtInt(int value) {
    return value >= 0 ? std::to_string(value) : "--";
}

std::string fmtMs(int64_t value) {
    return value >= 0 ? std::to_string(value) : "--";
}

bool comparableEqual(int a, int b) {
    return a < 0 || b < 0 || a == b;
}

bool comparableEqual(const std::string& a, const std::string& b) {
    return a.empty() || b.empty() || a == b;
}

void appendReason(std::string& reason, const std::string& item) {
    if (!reason.empty()) {
        reason += ", ";
    }
    reason += item;
}

std::string compareFrame(const FrameObservation& live,
                         const FrameObservation& replay) {
    std::string reason;
    if (live.decode_failed != replay.decode_failed) {
        appendReason(reason, "outcome mismatch");
    }
    if (compactModeLabel(live.mode) != compactModeLabel(replay.mode)) {
        appendReason(reason, "mode mismatch");
    }
    if (!comparableEqual(live.frame_type, replay.frame_type)) {
        appendReason(reason, "frame type mismatch");
    }
    if (!comparableEqual(live.total_cw, replay.total_cw)) {
        appendReason(reason, "total CW mismatch");
    }
    if (!comparableEqual(live.cw_ok, replay.cw_ok)) {
        appendReason(reason, "CW ok mismatch");
    }
    if (!comparableEqual(live.cw_failed, replay.cw_failed)) {
        appendReason(reason, "CW failed mismatch");
    }
    if (!comparableEqual(live.payload_len, replay.payload_len)) {
        appendReason(reason, "payload length mismatch");
    }
    return reason;
}

std::vector<FrameObservation> keyedFrames(const std::vector<FrameObservation>& frames,
                                          size_t* unkeyed) {
    std::vector<FrameObservation> out;
    if (unkeyed) {
        *unkeyed = 0;
    }
    for (const auto& f : frames) {
        if (f.has_frame_seq) {
            out.push_back(f);
        } else if (unkeyed) {
            ++(*unkeyed);
        }
    }
    return out;
}

std::map<ModeSeqKey, FrameObservation> mapByModeSeq(
    const std::vector<FrameObservation>& frames) {
    std::map<ModeSeqKey, FrameObservation> out;
    for (const auto& f : frames) {
        out[{f.frame_seq, compactModeLabel(f.mode)}] = f;
    }
    return out;
}

std::map<int, std::vector<FrameObservation>> groupBySeq(
    const std::vector<FrameObservation>& frames) {
    std::map<int, std::vector<FrameObservation>> out;
    for (const auto& f : frames) {
        out[f.frame_seq].push_back(f);
    }
    return out;
}

struct ModeAggregate {
    int live_total = 0;
    int live_ok = 0;
    int replay_total = 0;
    int replay_ok = 0;
    int divergent = 0;
};

std::map<std::string, ModeAggregate> aggregateByMode(const DivergenceReport& report) {
    std::map<std::string, ModeAggregate> out;
    for (const auto& row : report.rows) {
        if (row.has_live) {
            auto& agg = out[compactModeLabel(row.live.mode)];
            agg.live_total++;
            if (!row.live.decode_failed) {
                agg.live_ok++;
            }
            if (row.kind == MatchKind::Divergent) {
                agg.divergent++;
            }
        }
        if (row.has_replay) {
            auto& agg = out[compactModeLabel(row.replay.mode)];
            agg.replay_total++;
            if (!row.replay.decode_failed) {
                agg.replay_ok++;
            }
            if (row.kind == MatchKind::Divergent && !row.has_live) {
                agg.divergent++;
            }
        }
    }
    return out;
}

struct MetricDelta {
    int seq = -1;
    std::string metric;
    float live = 0.0f;
    float replay = 0.0f;
    float delta = 0.0f;
};

std::vector<MetricDelta> worstMetricDeltas(const DivergenceReport& report) {
    std::vector<MetricDelta> deltas;
    auto add = [&](const DivergenceRow& row, const char* name,
                   const std::optional<float>& live,
                   const std::optional<float>& replay) {
        if (!live || !replay) {
            return;
        }
        deltas.push_back({row.frame_seq, name, *live, *replay, *replay - *live});
    };
    for (const auto& row : report.rows) {
        if (!row.has_live || !row.has_replay || !row.has_frame_seq) {
            continue;
        }
        add(row, "LLR_avg", row.live.llr_abs_mean, row.replay.llr_abs_mean);
        add(row, "sync_corr", row.live.sync_corr, row.replay.sync_corr);
    }
    std::sort(deltas.begin(), deltas.end(), [](const auto& a, const auto& b) {
        return std::abs(a.delta) > std::abs(b.delta);
    });
    if (deltas.size() > 5) {
        deltas.resize(5);
    }
    return deltas;
}

std::string timelineString(const std::vector<ModeSpec>& events,
                           const ModeSpec* initial = nullptr) {
    std::ostringstream out;
    bool wrote = false;
    if (initial) {
        out << compactModeLabel(*initial) << " (t=0)";
        wrote = true;
    }
    for (const auto& mode : events) {
        if (wrote) {
            out << " -> ";
        }
        out << compactModeLabel(mode) << " (t="
            << std::fixed << std::setprecision(1)
            << (static_cast<double>(mode.t_ms) / 1000.0) << ")";
        wrote = true;
    }
    if (!wrote) {
        out << "(none)";
    }
    return out.str();
}

void jsonFrame(std::ostringstream& out, const FrameObservation& frame) {
    out << "{";
    out << "\"t_ms\":" << frame.t_ms << ",";
    out << "\"seq\":";
    if (frame.has_frame_seq) out << frame.frame_seq; else out << "null";
    out << ",\"outcome\":\"" << (frame.decode_failed ? "decode.fail" : "frame.rx") << "\"";
    out << ",\"mode\":\"" << json::escape(compactModeLabel(frame.mode)) << "\"";
    out << ",\"frame_type\":\"" << json::escape(frame.frame_type) << "\"";
    out << ",\"payload_len\":" << frame.payload_len;
    out << ",\"total_cw\":" << frame.total_cw;
    out << ",\"cw_ok\":" << frame.cw_ok;
    out << ",\"cw_failed\":" << frame.cw_failed;
    auto optNum = [&](const char* key, const std::optional<float>& v) {
        out << ",\"" << key << "\":";
        if (v) out << *v; else out << "null";
    };
    optNum("snr_db", frame.snr_db);
    optNum("fading_index", frame.fading_index);
    optNum("sync_corr", frame.sync_corr);
    optNum("cfo_hz", frame.cfo_hz);
    optNum("llr_abs_mean", frame.llr_abs_mean);
    out << "}";
}

} // namespace

DivergenceReport compareTimelines(const ParsedTimeline& live,
                                  const ReplayTimeline& replay) {
    DivergenceReport report;
    report.live_mode_events = live.mode_events;
    report.replay_mode_events = replay.mode_events;
    report.warnings = live.warnings;
    report.warnings.insert(report.warnings.end(), replay.warnings.begin(), replay.warnings.end());
    report.summary.live_frames = live.live_frames.size();
    report.summary.replay_frames = replay.frames.size();

    size_t unkeyed_live = 0;
    size_t unkeyed_replay = 0;
    auto live_keyed = keyedFrames(live.live_frames, &unkeyed_live);
    auto replay_keyed = keyedFrames(replay.frames, &unkeyed_replay);
    report.summary.unkeyed_live = unkeyed_live;
    report.summary.unkeyed_replay = unkeyed_replay;
    report.summary.comparable_live_frames = live_keyed.size();
    report.summary.comparable_replay_frames = replay_keyed.size();

    auto live_by_seq = groupBySeq(live_keyed);
    auto replay_by_seq = groupBySeq(replay_keyed);
    auto live_by_mode_seq = mapByModeSeq(live_keyed);
    auto replay_by_mode_seq = mapByModeSeq(replay_keyed);

    std::set<int> seqs;
    for (const auto& [seq, _] : live_by_seq) {
        seqs.insert(seq);
    }
    for (const auto& [seq, _] : replay_by_seq) {
        seqs.insert(seq);
    }

    for (int seq : seqs) {
        const auto live_it = live_by_seq.find(seq);
        const auto replay_it = replay_by_seq.find(seq);
        const bool live_unique = live_it != live_by_seq.end() && live_it->second.size() == 1;
        const bool replay_unique = replay_it != replay_by_seq.end() && replay_it->second.size() == 1;

        if (live_unique && replay_unique) {
            DivergenceRow row;
            row.frame_seq = seq;
            row.has_frame_seq = true;
            row.live = live_it->second.front();
            row.replay = replay_it->second.front();
            row.has_live = true;
            row.has_replay = true;
            row.reason = compareFrame(row.live, row.replay);
            row.kind = row.reason.empty() ? MatchKind::Exact : MatchKind::Divergent;
            report.rows.push_back(std::move(row));
            continue;
        }

        std::set<ModeSeqKey> keys;
        if (live_it != live_by_seq.end()) {
            for (const auto& f : live_it->second) {
                keys.insert({seq, compactModeLabel(f.mode)});
            }
        }
        if (replay_it != replay_by_seq.end()) {
            for (const auto& f : replay_it->second) {
                keys.insert({seq, compactModeLabel(f.mode)});
            }
        }

        for (const auto& key : keys) {
            const auto l = live_by_mode_seq.find(key);
            const auto r = replay_by_mode_seq.find(key);
            DivergenceRow row;
            row.frame_seq = seq;
            row.has_frame_seq = true;
            if (l != live_by_mode_seq.end()) {
                row.live = l->second;
                row.has_live = true;
            }
            if (r != replay_by_mode_seq.end()) {
                row.replay = r->second;
                row.has_replay = true;
            }
            if (row.has_live && row.has_replay) {
                row.reason = compareFrame(row.live, row.replay);
                row.kind = row.reason.empty() ? MatchKind::Exact : MatchKind::Divergent;
            } else if (row.has_live) {
                row.kind = MatchKind::LiveOnly;
                row.reason = "no replay frame with same sequence/mode";
            } else {
                row.kind = MatchKind::ReplayOnly;
                row.reason = "no live frame with same sequence/mode";
            }
            report.rows.push_back(std::move(row));
        }
    }

    std::sort(report.rows.begin(), report.rows.end(), [](const auto& a, const auto& b) {
        if (a.has_frame_seq != b.has_frame_seq) {
            return a.has_frame_seq > b.has_frame_seq;
        }
        if (a.frame_seq != b.frame_seq) {
            return a.frame_seq < b.frame_seq;
        }
        return a.reason < b.reason;
    });

    for (const auto& row : report.rows) {
        switch (row.kind) {
            case MatchKind::Exact: report.summary.exact_matches++; break;
            case MatchKind::Divergent: report.summary.divergent++; break;
            case MatchKind::LiveOnly: report.summary.live_only++; break;
            case MatchKind::ReplayOnly: report.summary.replay_only++; break;
        }
    }
    return report;
}

std::string renderTextReport(const DivergenceReport& report) {
    std::ostringstream out;
    out << "=== ultra_replay divergence report ===\n\n";
    out << "seq | t_ms_live | live_outcome | replay_outcome | match\n";
    out << "----+-----------+--------------+----------------+------\n";
    for (const auto& row : report.rows) {
        const std::string live = row.has_live ? frameOutcomeLabel(row.live) : "--";
        const std::string replay = row.has_replay ? frameOutcomeLabel(row.replay) : "--";
        out << std::setw(3) << (row.has_frame_seq ? fmtInt(row.frame_seq) : "--")
            << " | " << std::setw(9)
            << (row.has_live ? fmtMs(row.live.t_ms) : "--")
            << " | " << live
            << " | " << replay
            << " | " << matchKindString(row.kind);
        if (!row.reason.empty() && row.kind != MatchKind::Exact) {
            out << " (" << row.reason << ")";
        }
        out << "\n";
    }

    const auto& s = report.summary;
    out << "\nSummary:\n";
    out << "Total live frames:    " << s.live_frames << "\n";
    out << "Total replay frames:  " << s.replay_frames;
    if (s.replay_frames >= s.live_frames) {
        out << " (+" << (s.replay_frames - s.live_frames) << " vs live)";
    } else {
        out << " (-" << (s.live_frames - s.replay_frames) << " vs live)";
    }
    out << "\n";
    out << "Exact matches:        " << s.exact_matches << "\n";
    out << "Divergent:            " << s.divergent << "\n";
    out << "Live-only:            " << s.live_only << "\n";
    out << "Replay-only sync:     " << s.replay_only << "\n";
    if (s.unkeyed_live || s.unkeyed_replay) {
        out << "Unkeyed frames:       live " << s.unkeyed_live
            << ", replay " << s.unkeyed_replay
            << " (not sequence-correlated)\n";
    }

    out << "\nMode timeline divergence:\n";
    out << "  Live:   " << timelineString(report.live_mode_events) << "\n";
    out << "  Replay: " << timelineString(report.replay_mode_events) << "\n";
    out << (report.live_mode_events.size() == report.replay_mode_events.size()
                ? "  (matched event count)\n"
                : "  (event count differs)\n");

    out << "\nPer-mode aggregate:\n";
    for (const auto& [mode, agg] : aggregateByMode(report)) {
        out << "  " << mode << ": live " << agg.live_ok << "/" << agg.live_total
            << " replay " << agg.replay_ok << "/" << agg.replay_total;
        if (agg.divergent == 0) {
            out << " match";
        } else {
            out << " " << agg.divergent << " frames diverged";
        }
        out << "\n";
    }

    auto worst = worstMetricDeltas(report);
    out << "\nWorst per-CW divergences:\n";
    if (worst.empty()) {
        out << "  (no paired LLR/sync metrics available)\n";
    } else {
        for (const auto& d : worst) {
            out << "  seq=" << d.seq << " " << d.metric
                << " live=" << std::fixed << std::setprecision(2) << d.live
                << " replay=" << d.replay
                << " delta=" << d.delta << "\n";
        }
    }

    if (!report.warnings.empty()) {
        out << "\nWarnings:\n";
        for (const auto& warning : report.warnings) {
            out << "  - " << warning << "\n";
        }
    }
    return out.str();
}

std::string renderJsonReport(const DivergenceReport& report) {
    std::ostringstream out;
    const auto& s = report.summary;
    out << "{\n";
    out << "  \"summary\": {";
    out << "\"live_frames\":" << s.live_frames;
    out << ",\"replay_frames\":" << s.replay_frames;
    out << ",\"exact_matches\":" << s.exact_matches;
    out << ",\"divergent\":" << s.divergent;
    out << ",\"live_only\":" << s.live_only;
    out << ",\"replay_only\":" << s.replay_only;
    out << ",\"unkeyed_live\":" << s.unkeyed_live;
    out << ",\"unkeyed_replay\":" << s.unkeyed_replay;
    out << "},\n";
    out << "  \"rows\": [\n";
    for (size_t i = 0; i < report.rows.size(); ++i) {
        const auto& row = report.rows[i];
        out << "    {\"match\":\"" << matchKindString(row.kind) << "\",";
        out << "\"seq\":";
        if (row.has_frame_seq) out << row.frame_seq; else out << "null";
        out << ",\"reason\":\"" << json::escape(row.reason) << "\",";
        out << "\"live\":";
        if (row.has_live) jsonFrame(out, row.live); else out << "null";
        out << ",\"replay\":";
        if (row.has_replay) jsonFrame(out, row.replay); else out << "null";
        out << "}";
        if (i + 1 != report.rows.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"warnings\": [";
    for (size_t i = 0; i < report.warnings.size(); ++i) {
        if (i) out << ",";
        out << "\"" << json::escape(report.warnings[i]) << "\"";
    }
    out << "]\n";
    out << "}\n";
    return out.str();
}

} // namespace ultra::replay
