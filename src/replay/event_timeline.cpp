#include "replay/event_timeline.hpp"

#include "replay/json_util.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace ultra::replay {
namespace {

struct ParsedEvent {
    uint64_t seq = 0;
    int64_t t_ms = -1;
    int64_t ts_epoch_ms = -1;
    std::string ts_utc;
    std::string component;
    std::string event;
    std::string fields;
};

std::optional<int> firstIntField(const std::string& fields,
                                 std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (auto v = json::intValue(fields, key)) {
            return static_cast<int>(*v);
        }
    }
    return std::nullopt;
}

std::optional<float> firstFloatField(const std::string& fields,
                                     std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (auto v = json::numberValue(fields, key)) {
            return static_cast<float>(*v);
        }
    }
    return std::nullopt;
}

std::optional<std::string> firstStringField(const std::string& fields,
                                            std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (auto v = json::stringValue(fields, key)) {
            return *v;
        }
    }
    return std::nullopt;
}

std::optional<ParsedEvent> parseEventLine(const std::string& line,
                                          int64_t first_ts_epoch_ms) {
    ParsedEvent e;
    if (auto seq = json::intValue(line, "seq")) {
        e.seq = static_cast<uint64_t>(std::max<int64_t>(0, *seq));
    }
    if (auto t_ms = json::intValue(line, "t_ms")) {
        e.t_ms = *t_ms;
    }
    if (auto ts = json::stringValue(line, "ts_utc")) {
        e.ts_utc = *ts;
        if (auto parsed = json::parseUtcTimestampMs(*ts)) {
            e.ts_epoch_ms = *parsed;
            if (e.t_ms < 0 && first_ts_epoch_ms >= 0) {
                e.t_ms = e.ts_epoch_ms - first_ts_epoch_ms;
            }
        }
    }
    auto component = json::stringValue(line, "component");
    auto event = json::stringValue(line, "event");
    if (!component || !event) {
        return std::nullopt;
    }
    e.component = *component;
    e.event = *event;
    if (auto fields = json::rawObjectValue(line, "fields")) {
        e.fields = *fields;
    } else {
        e.fields = "{}";
    }
    return e;
}

int64_t firstTimestampMs(const std::string& jsonl) {
    std::istringstream in(jsonl);
    std::string line;
    while (std::getline(in, line)) {
        line = json::trim(line);
        if (line.empty()) {
            continue;
        }
        if (auto ts = json::stringValue(line, "ts_utc")) {
            if (auto parsed = json::parseUtcTimestampMs(*ts)) {
                return *parsed;
            }
        }
    }
    return -1;
}

ModeSpec modeSpecFromFields(const ParsedEvent& event) {
    ModeSpec out;
    out.t_ms = event.t_ms < 0 ? 0 : event.t_ms;
    out.source_event = event.component + "." + event.event;

    if (auto wf = firstStringField(event.fields, {"waveform", "mode", "phy", "waveform_mode"})) {
        if (auto parsed = parseWaveformMode(*wf)) {
            out.waveform = *parsed;
            out.has_waveform = true;
        }
    }
    if (auto mod = firstStringField(event.fields, {"mod", "modulation"})) {
        if (auto parsed = parseModulation(*mod)) {
            out.modulation = *parsed;
            out.has_modulation = true;
        }
    }
    if (auto rate = firstStringField(event.fields, {"rate", "code_rate"})) {
        if (auto parsed = parseCodeRate(*rate)) {
            out.code_rate = *parsed;
            out.has_code_rate = true;
        }
    }
    if (auto cw = firstIntField(event.fields, {"cw", "cw_count", "fixed_cw", "total_cw"})) {
        out.cw_count = std::max(1, *cw);
        out.has_cw_count = true;
    }
    if (auto cfo = firstFloatField(event.fields, {"cfo_hz", "known_cfo_hz"})) {
        out.cfo_hz = *cfo;
        out.has_cfo_hz = true;
    }
    if (auto group = firstIntField(event.fields, {"burst_group_size", "burst_group"})) {
        out.burst_group_size = std::max(2, *group);
        out.has_burst_group_size = true;
    }
    if (auto state = firstStringField(event.fields, {"state"})) {
        const std::string s = json::lowerCopy(*state);
        if (s == "connected") {
            out.connected = true;
            out.has_connected = true;
        } else if (s == "disconnected" || s == "listen" || s == "listening") {
            out.connected = false;
            out.has_connected = true;
            if (s == "disconnected" || s == "listen" || s == "listening") {
                out.waveform = protocol::WaveformMode::MC_DPSK;
                out.has_waveform = true;
            }
        }
    }

    if ((event.event == "waveform.negotiated" || event.event == "mode.apply") &&
        !out.has_connected) {
        out.connected = true;
        out.has_connected = true;
    }
    return out;
}

FrameObservation frameFromFields(const ParsedEvent& event,
                                 const ModeSpec& current_mode,
                                 FrameObservation::Origin origin) {
    FrameObservation frame;
    frame.origin = origin;
    frame.t_ms = event.t_ms;
    frame.decode_failed = event.event == "decode.fail";
    frame.mode = current_mode;
    frame.source_event = event.component + "." + event.event;

    if (auto seq = firstIntField(event.fields, {"frame_seq", "frame_sequence", "seq"})) {
        frame.frame_seq = *seq;
        frame.has_frame_seq = true;
    }
    if (auto ft = firstStringField(event.fields, {"frame_type", "type"})) {
        frame.frame_type = *ft;
    }
    if (auto bytes = firstIntField(event.fields, {"frame_bytes", "bytes"})) {
        frame.frame_bytes = *bytes;
    }
    if (auto payload = firstIntField(event.fields, {"payload_len", "payload_bytes"})) {
        frame.payload_len = *payload;
    }
    if (auto total = firstIntField(event.fields, {"total_cw", "cw_total"})) {
        frame.total_cw = *total;
    }
    if (auto ok = firstIntField(event.fields, {"cw_ok", "codewords_ok"})) {
        frame.cw_ok = *ok;
    }
    if (auto failed = firstIntField(event.fields, {"cw_failed", "codewords_failed"})) {
        frame.cw_failed = *failed;
    }
    frame.snr_db = firstFloatField(event.fields, {"snr_db", "lts_snr_db"});
    frame.fading_index = firstFloatField(event.fields, {"fading_index", "fading"});
    frame.sync_corr = firstFloatField(event.fields, {"sync_corr", "sync_correlation"});
    frame.cfo_hz = firstFloatField(event.fields, {"cfo_hz", "residual_cfo_hz"});
    frame.llr_abs_mean = firstFloatField(event.fields, {"llr_abs_mean", "llr_avg"});

    auto patch = modeSpecFromFields(event);
    if (patch.has_waveform || patch.has_modulation || patch.has_code_rate ||
        patch.has_cw_count || patch.has_connected) {
        frame.mode = mergeMode(frame.mode, patch);
    }
    return frame;
}

bool isModeEvent(const ParsedEvent& event) {
    return event.event == "waveform.negotiated" ||
           event.event == "mode.apply" ||
           event.event == "session.state";
}

bool isFrameEvent(const ParsedEvent& event) {
    return event.event == "frame.rx" || event.event == "decode.fail";
}

} // namespace

std::optional<protocol::WaveformMode> parseWaveformMode(std::string value) {
    value = json::upperCopy(json::trim(value));
    std::replace(value.begin(), value.end(), '_', '-');
    if (value == "MC-DPSK" || value == "MCDPSK" || value == "MC DPSK") {
        return protocol::WaveformMode::MC_DPSK;
    }
    if (value == "OFDM-CHIRP" || value == "OFDMCHIRP") {
        return protocol::WaveformMode::OFDM_CHIRP;
    }
    if (value == "OFDM-NARROW" || value == "OFDMNARROW" || value == "NARROW") {
        return protocol::WaveformMode::OFDM_NARROW;
    }
    if (value == "OFDM-COX" || value == "OFDMC0X" || value == "OFDMCOX" ||
        value == "OFDM") {
        return protocol::WaveformMode::OFDM_CHIRP;
    }
    if (value == "AUTO") {
        return protocol::WaveformMode::AUTO;
    }
    return std::nullopt;
}

std::optional<Modulation> parseModulation(std::string value) {
    value = json::upperCopy(json::trim(value));
    std::replace(value.begin(), value.end(), '-', '_');
    if (value == "DBPSK") return Modulation::DBPSK;
    if (value == "BPSK") return Modulation::BPSK;
    if (value == "DQPSK") return Modulation::DQPSK;
    if (value == "QPSK") return Modulation::QPSK;
    if (value == "D8PSK") return Modulation::D8PSK;
    if (value == "8PSK" || value == "QAM8" || value == "8QAM") return Modulation::QAM8;
    if (value == "QAM16" || value == "16QAM") return Modulation::QAM16;
    if (value == "QAM32" || value == "32QAM") return Modulation::QAM32;
    if (value == "QAM64" || value == "64QAM") return Modulation::QAM64;
    if (value == "QAM256" || value == "256QAM") return Modulation::QAM256;
    return std::nullopt;
}

std::optional<CodeRate> parseCodeRate(std::string value) {
    value = json::upperCopy(json::trim(value));
    std::replace(value.begin(), value.end(), '_', '/');
    std::replace(value.begin(), value.end(), '-', '/');
    if (value == "R1/4" || value == "1/4") return CodeRate::R1_4;
    if (value == "R1/3" || value == "1/3") return CodeRate::R1_3;
    if (value == "R1/2" || value == "1/2") return CodeRate::R1_2;
    if (value == "R2/3" || value == "2/3") return CodeRate::R2_3;
    if (value == "R3/4" || value == "3/4") return CodeRate::R3_4;
    if (value == "R5/6" || value == "5/6") return CodeRate::R5_6;
    if (value == "R7/8" || value == "7/8") return CodeRate::R7_8;
    if (value == "AUTO") return CodeRate::AUTO;
    return std::nullopt;
}

ModeSpec mergeMode(const ModeSpec& base, const ModeSpec& patch) {
    ModeSpec out = base;
    out.t_ms = patch.t_ms;
    out.source_event = patch.source_event.empty() ? base.source_event : patch.source_event;
    if (patch.has_waveform) {
        out.waveform = patch.waveform;
        out.has_waveform = true;
    }
    if (patch.has_modulation) {
        out.modulation = patch.modulation;
        out.has_modulation = true;
    }
    if (patch.has_code_rate) {
        out.code_rate = patch.code_rate;
        out.has_code_rate = true;
    }
    if (patch.has_cw_count) {
        out.cw_count = patch.cw_count;
        out.has_cw_count = true;
    }
    if (patch.has_connected) {
        out.connected = patch.connected;
        out.has_connected = true;
    }
    if (patch.has_cfo_hz) {
        out.cfo_hz = patch.cfo_hz;
        out.has_cfo_hz = true;
    }
    if (patch.has_burst_group_size) {
        out.burst_group_size = patch.burst_group_size;
        out.has_burst_group_size = true;
    }
    if (!patch.note.empty()) {
        out.note = patch.note;
    }
    return out;
}

std::string compactModeLabel(const ModeSpec& mode) {
    std::ostringstream out;
    out << protocol::waveformModeToString(mode.waveform);
    if (mode.has_modulation) {
        out << " " << modulationToString(mode.modulation);
    }
    if (mode.has_code_rate && mode.code_rate != CodeRate::AUTO) {
        out << " " << codeRateToString(mode.code_rate);
    }
    return out.str();
}

std::string modeLabel(const ModeSpec& mode) {
    std::ostringstream out;
    out << compactModeLabel(mode);
    if (mode.has_cw_count) {
        out << " cw=" << mode.cw_count;
    }
    if (mode.has_connected) {
        out << (mode.connected ? " connected" : " disconnected");
    }
    return out.str();
}

std::string frameOutcomeLabel(const FrameObservation& frame) {
    if (frame.t_ms < 0 && !frame.has_frame_seq && frame.frame_type.empty() &&
        frame.cw_ok < 0 && frame.cw_failed < 0) {
        return "--";
    }

    std::ostringstream out;
    out << compactModeLabel(frame.mode);
    if (!frame.frame_type.empty()) {
        out << " " << frame.frame_type;
    } else if (frame.decode_failed) {
        out << " decode.fail";
    }
    if (frame.decode_failed && frame.frame_type.find("decode.fail") == std::string::npos) {
        out << " decode.fail";
    }
    if (frame.cw_ok >= 0) {
        out << " cw_ok=" << frame.cw_ok;
    }
    if (frame.cw_failed >= 0) {
        out << " cw_failed=" << frame.cw_failed;
    }
    return out.str();
}

ParsedTimeline parseEventTimeline(const std::string& jsonl,
                                  const ModeSpec& manifest_initial,
                                  bool manifest_initial_available) {
    ParsedTimeline timeline;
    timeline.initial_mode = manifest_initial;
    timeline.initial_mode_assumed = !manifest_initial_available;
    if (!timeline.initial_mode.has_waveform) {
        timeline.initial_mode.waveform = protocol::WaveformMode::MC_DPSK;
        timeline.initial_mode.has_waveform = true;
    }
    if (!timeline.initial_mode.has_modulation) {
        timeline.initial_mode.modulation = Modulation::DQPSK;
        timeline.initial_mode.has_modulation = true;
    }
    if (!timeline.initial_mode.has_code_rate) {
        timeline.initial_mode.code_rate = CodeRate::R1_4;
        timeline.initial_mode.has_code_rate = true;
    }
    if (!timeline.initial_mode.has_connected) {
        timeline.initial_mode.connected = false;
        timeline.initial_mode.has_connected = true;
    }
    if (!timeline.initial_mode.has_cw_count) {
        timeline.initial_mode.cw_count = 1;
        timeline.initial_mode.has_cw_count = true;
    }
    if (!manifest_initial_available) {
        timeline.warnings.push_back(
            "manifest did not declare initial mode; assuming MC-DPSK DQPSK R1/4 disconnected until timeline events or live frames say otherwise");
    }

    const int64_t first_ts = firstTimestampMs(jsonl);
    ModeSpec current = timeline.initial_mode;

    std::istringstream in(jsonl);
    std::string line;
    size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        line = json::trim(line);
        if (line.empty()) {
            continue;
        }

        auto parsed = parseEventLine(line, first_ts);
        if (!parsed) {
            timeline.warnings.push_back("skipped malformed JSONL line " + std::to_string(line_no));
            continue;
        }
        ParsedEvent event = *parsed;
        if (event.t_ms < 0) {
            event.t_ms = 0;
            timeline.warnings.push_back(
                "event line " + std::to_string(line_no) +
                " has no t_ms or parseable ts_utc; using t=0 for replay ordering");
        }

        if (isModeEvent(event)) {
            const ModeSpec patch = modeSpecFromFields(event);
            const bool changed =
                patch.has_waveform || patch.has_modulation || patch.has_code_rate ||
                patch.has_cw_count || patch.has_connected || patch.has_cfo_hz ||
                patch.has_burst_group_size;
            if (changed) {
                current = mergeMode(current, patch);
                timeline.mode_events.push_back(current);
            }
        }

        if (isFrameEvent(event)) {
            auto frame = frameFromFields(event, current, FrameObservation::Origin::Live);
            timeline.live_frames.push_back(std::move(frame));
        }
    }

    if (!manifest_initial_available && !timeline.live_frames.empty()) {
        const auto& first_frame = timeline.live_frames.front();
        if (first_frame.mode.has_waveform) {
            timeline.initial_mode = first_frame.mode;
            timeline.initial_mode.t_ms = 0;
            timeline.initial_mode.source_event = "inferred.first_live_frame";
            timeline.initial_mode.note =
                "manifest lacked initial mode; inferred from first live frame";
            timeline.warnings.push_back(
                "initial mode inferred from first live frame because manifest has no initial_mode");
        }
    }

    std::sort(timeline.mode_events.begin(), timeline.mode_events.end(),
              [](const ModeSpec& a, const ModeSpec& b) { return a.t_ms < b.t_ms; });
    return timeline;
}

} // namespace ultra::replay
