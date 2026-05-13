#include "diagnostics/session_summary.hpp"

#include "diagnostics/redaction.hpp"
#include "ultra/build_info.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>

namespace ultra::diagnostics {

namespace fs = std::filesystem;

namespace {

struct ParsedEvent {
    std::string ts_utc;
    std::string component;
    std::string event;
    std::string privacy;
    std::string fields;
};

struct ModeState {
    std::string waveform;
    std::string modulation;
    std::string rate;
    int cw = 0;
};

struct FileEvent {
    std::string direction;
    std::string path;
    uint64_t bytes = 0;
    double seconds = 0.0;
    bool success = false;
    bool has_success = false;
    std::string error;
};

std::string readTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool writeTextFile(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

std::string lowerCopy(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool parseJsonStringAt(const std::string& text, size_t quote, std::string* out) {
    if (quote >= text.size() || text[quote] != '"') {
        return false;
    }
    std::string value;
    bool escaped = false;
    for (size_t i = quote + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (escaped) {
            switch (c) {
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                default: value += c; break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            if (out) {
                *out = value;
            }
            return true;
        } else {
            value += c;
        }
    }
    return false;
}

std::optional<size_t> findJsonValue(const std::string& object, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = object.find(needle, pos)) != std::string::npos) {
        size_t colon = pos + needle.size();
        while (colon < object.size() && std::isspace(static_cast<unsigned char>(object[colon]))) {
            ++colon;
        }
        if (colon < object.size() && object[colon] == ':') {
            size_t value = colon + 1;
            while (value < object.size() && std::isspace(static_cast<unsigned char>(object[value]))) {
                ++value;
            }
            return value;
        }
        pos += needle.size();
    }
    return std::nullopt;
}

std::optional<std::string> jsonStringValue(const std::string& object, const std::string& key) {
    const auto value = findJsonValue(object, key);
    if (!value || *value >= object.size() || object[*value] != '"') {
        return std::nullopt;
    }
    std::string out;
    if (!parseJsonStringAt(object, *value, &out)) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::string> jsonRawObjectValue(const std::string& object, const std::string& key) {
    const auto value = findJsonValue(object, key);
    if (!value || *value >= object.size() || object[*value] != '{') {
        return std::nullopt;
    }
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = *value; i < object.size(); ++i) {
        const char c = object[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return object.substr(*value, i - *value + 1);
            }
        }
    }
    return std::nullopt;
}

std::optional<double> jsonNumberValue(const std::string& object, const std::string& key) {
    const auto value = findJsonValue(object, key);
    if (!value || *value >= object.size()) {
        return std::nullopt;
    }
    const char* begin = object.c_str() + *value;
    char* end = nullptr;
    const double parsed = std::strtod(begin, &end);
    if (end == begin || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<int64_t> jsonIntValue(const std::string& object, const std::string& key) {
    const auto n = jsonNumberValue(object, key);
    if (!n) {
        return std::nullopt;
    }
    return static_cast<int64_t>(*n);
}

std::optional<bool> jsonBoolValue(const std::string& object, const std::string& key) {
    const auto value = findJsonValue(object, key);
    if (!value) {
        return std::nullopt;
    }
    if (object.compare(*value, 4, "true") == 0) {
        return true;
    }
    if (object.compare(*value, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

std::optional<ParsedEvent> parseEventLine(const std::string& line) {
    ParsedEvent e;
    auto ts = jsonStringValue(line, "ts_utc");
    auto component = jsonStringValue(line, "component");
    auto event = jsonStringValue(line, "event");
    if (!ts || !component || !event) {
        return std::nullopt;
    }
    e.ts_utc = *ts;
    e.component = *component;
    e.event = *event;
    e.privacy = jsonStringValue(line, "privacy").value_or("redacted");
    e.fields = jsonRawObjectValue(line, "fields").value_or("{}");
    return e;
}

std::time_t parseIsoUtc(const std::string& ts) {
    if (ts.size() < 19) {
        return 0;
    }
    std::tm tm{};
    if (std::sscanf(ts.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
        return 0;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = 0;
#ifdef _WIN32
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

std::string titleTime(const std::string& ts) {
    if (ts.size() >= 19) {
        std::string out = ts.substr(0, 10);
        out += ' ';
        out += ts.substr(11, 8);
        out += " UTC";
        return out;
    }
    std::time_t now = std::time(nullptr);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &now);
#else
    gmtime_r(&now, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm_utc);
    return buf;
}

std::string oneDecimal(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", value);
    return buf;
}

std::string twoDecimal(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", value);
    return buf;
}

std::string hzText(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f Hz", value);
    return buf;
}

std::string medianText(std::vector<double> values, int decimals = 1) {
    if (values.empty()) {
        return "n/a";
    }
    std::sort(values.begin(), values.end());
    const double median = values[values.size() / 2];
    return decimals == 2 ? twoDecimal(median) : oneDecimal(median);
}

double medianValue(std::vector<double> values) {
    if (values.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

std::string displayCallsign(const std::string& callsign, bool include) {
    if (callsign.empty()) {
        return include ? "unknown" : "REDACTED";
    }
    return include ? callsign : "REDACTED";
}

std::string modeText(const ModeState& mode) {
    std::string out = mode.waveform.empty() ? "unknown" : mode.waveform;
    if (!mode.modulation.empty()) {
        out += " ";
        out += mode.modulation;
    }
    if (!mode.rate.empty()) {
        out += " ";
        out += mode.rate;
    }
    return out;
}

std::string timelineEntry(const ModeState& mode, const std::string& ts, const std::string& first_ts) {
    std::string out = modeText(mode);
    const std::time_t t0 = parseIsoUtc(first_ts);
    const std::time_t t1 = parseIsoUtc(ts);
    if (t0 > 0 && t1 >= t0) {
        out += " @ +";
        out += oneDecimal(std::difftime(t1, t0));
        out += " s";
    }
    return out;
}

void maybeAddTimeline(std::vector<std::string>& timeline, const std::string& entry) {
    if (!entry.empty() && (timeline.empty() || timeline.back() != entry)) {
        timeline.push_back(entry);
    }
}

std::string join(const std::vector<std::string>& items, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) {
            out += sep;
        }
        out += items[i];
    }
    return out;
}

std::string basename(const std::string& path) {
    if (path.empty()) {
        return "file";
    }
    return fs::path(path).filename().string();
}

std::string buildLine(const SessionSummaryOptions& options) {
    const std::string version = options.build_version.empty() ? kBuildVersion : options.build_version;
    const std::string commit = options.build_commit.empty() ? kBuildGitCommit : options.build_commit;
    const std::string os = options.build_os.empty() ? kBuildOS : options.build_os;
    std::string short_commit = commit;
    if (short_commit.size() > 12) {
        short_commit.resize(12);
    }
    return version + " commit=" + short_commit + " os=" + os;
}

} // namespace

SessionSummaryOptions defaultSessionSummaryOptions() {
    SessionSummaryOptions options;
    options.build_version = kBuildVersion;
    options.build_commit = kBuildGitCommit;
    options.build_os = kBuildOS;
    return options;
}

SessionSummaryResult summarizeSessionJsonl(const std::string& jsonl,
                                           const SessionSummaryOptions& options) {
    SessionSummaryResult result;
    std::istringstream input(jsonl);
    std::string line;
    std::vector<ParsedEvent> events;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        if (auto parsed = parseEventLine(line)) {
            events.push_back(std::move(*parsed));
        }
    }

    if (events.empty()) {
        result.error = "session journal has no parseable events";
        return result;
    }

    std::string first_ts = events.front().ts_utc;
    std::string last_ts = events.back().ts_utc;
    bool saw_probing = false;
    bool saw_connecting = false;
    bool saw_connected = false;
    bool saw_disconnected = false;
    bool saw_timeout = false;
    bool saw_fault = false;
    bool saw_mode_change_failure = false;
    std::string disconnect_reason;
    int ping_tx = 0;
    int pong_rx = 0;
    double last_rx_snr = std::numeric_limits<double>::quiet_NaN();

    ModeState mode;
    std::vector<std::string> timeline;
    std::vector<double> snr_values;
    std::vector<double> cfo_values;
    std::vector<double> fading_values;
    int decode_fail_events = 0;
    int cw_failed_total = 0;
    int audio_overruns = 0;
    int64_t audio_dropped_samples = 0;
    double audio_rms = std::numeric_limits<double>::quiet_NaN();
    double audio_peak = std::numeric_limits<double>::quiet_NaN();
    std::vector<std::string> fault_reasons;
    std::vector<FileEvent> file_events;

    int arq_frames_sent = 0;
    int arq_retx = 0;
    int arq_timeouts = 0;
    int arq_failed = 0;

    // PTT/CAT lifecycle accumulator: surface whether the rig actually
    // opened, how many PTT keys cycled, and the last error (if any).
    std::string ptt_backend;
    int ptt_keyed_count = 0;
    int ptt_error_count = 0;
    bool ptt_opened = false;
    bool ptt_open_failed = false;
    std::string ptt_first_error;
    std::string ptt_test_cat_result;

    for (const ParsedEvent& e : events) {
        last_ts = e.ts_utc;
        if (auto snr = jsonNumberValue(e.fields, "snr_db")) {
            if (std::isfinite(*snr)) {
                snr_values.push_back(*snr);
                last_rx_snr = *snr;
            }
        }
        if (auto cfo = jsonNumberValue(e.fields, "cfo_hz")) {
            if (std::isfinite(*cfo)) {
                cfo_values.push_back(*cfo);
            }
        }
        if (auto fading = jsonNumberValue(e.fields, "fading")) {
            if (std::isfinite(*fading)) {
                fading_values.push_back(*fading);
            }
        }
        if (auto fading = jsonNumberValue(e.fields, "fading_index")) {
            if (std::isfinite(*fading)) {
                fading_values.push_back(*fading);
            }
        }

        if (e.event == "session.state") {
            const std::string state = lowerCopy(jsonStringValue(e.fields, "state").value_or(""));
            if (state == "probing") {
                saw_probing = true;
            } else if (state == "connecting") {
                saw_connecting = true;
            } else if (state == "connected") {
                saw_connected = true;
            } else if (state == "disconnected") {
                saw_disconnected = true;
            }
            disconnect_reason = jsonStringValue(e.fields, "reason").value_or(disconnect_reason);
            const std::string reason_l = lowerCopy(disconnect_reason);
            if (reason_l.find("timeout") != std::string::npos ||
                reason_l.find("no response") != std::string::npos) {
                saw_timeout = true;
            }
        } else if (e.event == "session.finished") {
            disconnect_reason = jsonStringValue(e.fields, "reason").value_or(disconnect_reason);
        } else if (e.event == "session.stats") {
            arq_frames_sent = static_cast<int>(jsonIntValue(e.fields, "arq_frames_sent").value_or(arq_frames_sent));
            arq_retx = static_cast<int>(jsonIntValue(e.fields, "arq_retransmissions").value_or(arq_retx));
            arq_timeouts = static_cast<int>(jsonIntValue(e.fields, "arq_timeouts").value_or(arq_timeouts));
            arq_failed = static_cast<int>(jsonIntValue(e.fields, "arq_failed").value_or(arq_failed));
        } else if (e.event == "waveform.negotiated") {
            bool changed = false;
            if (auto waveform = jsonStringValue(e.fields, "waveform")) {
                mode.waveform = *waveform;
                changed = true;
            }
            if (auto mod = jsonStringValue(e.fields, "mod")) {
                mode.modulation = *mod;
                changed = true;
            }
            if (auto rate = jsonStringValue(e.fields, "rate")) {
                mode.rate = *rate;
                changed = true;
            }
            if (auto cw = jsonIntValue(e.fields, "cw")) {
                mode.cw = static_cast<int>(*cw);
            }
            if (changed) {
                maybeAddTimeline(timeline, timelineEntry(mode, e.ts_utc, first_ts));
            }
        } else if (e.event == "decode.fail") {
            ++decode_fail_events;
            cw_failed_total += static_cast<int>(jsonIntValue(e.fields, "cw_failed").value_or(0));
        } else if (e.event == "audio.overrun") {
            ++audio_overruns;
            audio_dropped_samples += jsonIntValue(e.fields, "dropped_samples").value_or(0);
        } else if (e.event == "audio.stats") {
            audio_rms = jsonNumberValue(e.fields, "rms").value_or(audio_rms);
            audio_peak = jsonNumberValue(e.fields, "peak").value_or(audio_peak);
        } else if (e.event == "fault.triggered") {
            saw_fault = true;
            std::string reason = jsonStringValue(e.fields, "reason").value_or(
                jsonStringValue(e.fields, "source").value_or("unspecified"));
            fault_reasons.push_back(reason);
            if (lowerCopy(reason).find("mode_change") != std::string::npos) {
                saw_mode_change_failure = true;
            }
        } else if (e.event == "ping.tx") {
            ++ping_tx;
        } else if (e.event == "ping.rx") {
            ++pong_rx;
            if (auto snr = jsonNumberValue(e.fields, "snr_db")) {
                last_rx_snr = *snr;
            }
        } else if (e.event == "ptt.opened") {
            ptt_opened = true;
            if (auto backend = jsonStringValue(e.fields, "backend")) {
                ptt_backend = *backend;
            }
        } else if (e.event == "ptt.open_failed") {
            ptt_open_failed = true;
            if (auto backend = jsonStringValue(e.fields, "backend")) {
                ptt_backend = *backend;
            }
            if (ptt_first_error.empty()) {
                ptt_first_error = jsonStringValue(e.fields, "error").value_or("");
            }
        } else if (e.event == "ptt.keyed") {
            ++ptt_keyed_count;
        } else if (e.event == "ptt.error") {
            ++ptt_error_count;
            if (ptt_first_error.empty()) {
                ptt_first_error = jsonStringValue(e.fields, "error").value_or("");
            }
        } else if (e.event == "ptt.test_cat") {
            ptt_test_cat_result = jsonStringValue(e.fields, "result").value_or("");
            if (ptt_test_cat_result != "ok" && ptt_first_error.empty()) {
                ptt_first_error = jsonStringValue(e.fields, "error").value_or("");
            }
        } else if (e.event == "file.transfer") {
            FileEvent f;
            f.direction = jsonStringValue(e.fields, "direction").value_or("");
            f.path = jsonStringValue(e.fields, "path").value_or(
                jsonStringValue(e.fields, "filename").value_or(""));
            f.bytes = static_cast<uint64_t>(jsonIntValue(e.fields, "bytes").value_or(0));
            f.seconds = jsonNumberValue(e.fields, "seconds").value_or(0.0);
            if (auto success = jsonBoolValue(e.fields, "success")) {
                f.success = *success;
                f.has_success = true;
            }
            f.error = jsonStringValue(e.fields, "error").value_or("");
            file_events.push_back(std::move(f));
        }
    }

    const std::time_t t0 = parseIsoUtc(first_ts);
    const std::time_t t1 = parseIsoUtc(last_ts);
    const double wall_s = (t0 > 0 && t1 >= t0) ? std::difftime(t1, t0) : 0.0;

    if (!saw_connected && (saw_probing || saw_connecting)) {
        result.outcome = saw_timeout ? "TIMED_OUT" : "FAILED_HANDSHAKE";
    } else if (saw_connected && (saw_timeout || arq_failed > 0)) {
        result.outcome = "DROPPED";
    } else if (saw_connected) {
        result.outcome = "CONNECTED";
    } else {
        result.outcome = "NO_LINK";
    }

    std::string channel = "not measured";
    if (!snr_values.empty()) {
        auto minmax = std::minmax_element(snr_values.begin(), snr_values.end());
        channel = "SNR " + oneDecimal(*minmax.first) + "-" + oneDecimal(*minmax.second) +
                  " dB (median " + medianText(snr_values) + ")";
        if (!cfo_values.empty()) {
            auto cfo_minmax = std::minmax_element(cfo_values.begin(), cfo_values.end());
            const double cfo_med = medianValue(cfo_values);
            const double half_span = (*cfo_minmax.second - *cfo_minmax.first) * 0.5;
            channel += ", CFO " + hzText(cfo_med) + " (+/-" + oneDecimal(half_span) + ")";
        }
        if (!fading_values.empty()) {
            channel += ", fading index " + medianText(fading_values, 2);
        }
    }

    std::string decode_line;
    if (decode_fail_events == 0 && cw_failed_total == 0) {
        decode_line = "0 (no CW failures observed)";
    } else {
        decode_line = std::to_string(decode_fail_events) + " events, " +
                      std::to_string(cw_failed_total) + " failed CWs";
    }

    std::string audio_line = std::to_string(audio_overruns) + " overruns";
    if (audio_dropped_samples > 0) {
        audio_line += ", " + std::to_string(audio_dropped_samples) + " dropped samples";
    }
    if (std::isfinite(audio_rms)) {
        audio_line += ", RMS " + twoDecimal(audio_rms);
    }
    if (std::isfinite(audio_peak)) {
        audio_line += ", peak " + twoDecimal(audio_peak);
    }

    std::string file_line = "none observed";
    if (!file_events.empty()) {
        const FileEvent& f = file_events.back();
        const std::string verb = f.direction == "rx" ? "received" :
                                 f.direction == "tx" ? "sent" : "transferred";
        file_line = verb + " " + basename(f.path);
        if (f.bytes > 0) {
            file_line += " (" + std::to_string(f.bytes) + " bytes)";
        }
        if (f.has_success) {
            file_line += f.success ? ", CRC OK" : ", failed";
        }
        if (!f.error.empty()) {
            file_line += " (" + f.error + ")";
        }
    }

    std::string faults_line = "none";
    if (!fault_reasons.empty()) {
        faults_line = join(fault_reasons, ", ");
    }

    std::string disconnect_line;
    if (!saw_connected && (saw_probing || saw_connecting)) {
        disconnect_line = "not established";
    } else if (result.outcome == "DROPPED") {
        disconnect_line = disconnect_reason.empty() ? "dropped" : disconnect_reason;
    } else if (saw_disconnected) {
        disconnect_line = disconnect_reason.empty() ? "clean" : "clean (" + disconnect_reason + ")";
    } else {
        disconnect_line = "session stopped";
    }

    std::vector<std::string> suggestions;
    if (!saw_connected && (saw_probing || saw_connecting)) {
        std::string ping_text = ping_tx > 0 ? "PINGed " + std::to_string(ping_tx) + " times" :
                                "Connection attempt did not complete";
        if (wall_s > 0.0) {
            ping_text += " over " + oneDecimal(wall_s) + " s";
        }
        if (pong_rx == 0) {
            ping_text += ", no PONG received.";
        } else {
            ping_text += ", PONG received but CONNECT did not complete.";
        }
        if (std::isfinite(last_rx_snr)) {
            ping_text += " Last RX SNR estimate: " + oneDecimal(last_rx_snr) + " dB.";
        } else {
            ping_text += " Last RX SNR estimate: -inf (no chirp detected).";
        }
        suggestions.push_back(ping_text);
        suggestions.push_back("Suggested next steps: verify peer is listening, verify audio path.");
    }
    if (saw_connected && mode.waveform == "MC_DPSK" && !snr_values.empty()) {
        const double snr_med = medianValue(snr_values);
        if (std::isfinite(snr_med) && snr_med >= 12.0) {
            suggestions.push_back(
                "Stayed on MC-DPSK despite SNR near +" + oneDecimal(snr_med) +
                " dB. Possible causes: peer rejected MODE_CHANGE, or the SNR estimate window was too short.");
        }
    }
    if (saw_mode_change_failure) {
        suggestions.push_back("Mode change attempts failed; inspect peer support and control-frame decode reliability.");
    }

    const std::string mode_at_end = modeText(mode);
    const std::string timeline_text = timeline.empty() ? "not negotiated" : join(timeline, " -> ");
    const std::string commit = options.build_commit.empty() ? kBuildGitCommit : options.build_commit;

    std::ostringstream out;
    out << "ProjectUltra session report - " << titleTime(first_ts) << "\n"
        << "Build:           " << buildLine(options) << "\n"
        << "Local callsign:  " << displayCallsign(options.local_callsign, options.include_callsigns) << "\n"
        << "Peer callsign:   " << displayCallsign(options.peer_callsign, options.include_callsigns) << "\n\n"
        << "Session outcome: " << result.outcome << "\n"
        << "Wall time:       " << oneDecimal(wall_s) << " s\n"
        << "Mode at end:     " << mode_at_end << "\n"
        << "Mode timeline:   " << timeline_text << "\n\n"
        << "File transfer:   " << file_line << "\n"
        << "ARQ:             " << arq_frames_sent << " frames sent, "
        << arq_retx << " retx, " << arq_timeouts << " timeouts, "
        << arq_failed << " failed\n"
        << "Channel:         " << channel << "\n"
        << "Decode failures: " << decode_line << "\n"
        << "Audio:           " << audio_line << "\n"
        << "Faults:          " << faults_line << "\n";

    // PTT/CAT line: omit when there are no PTT events at all (operator
    // not using CAT). Otherwise summarize backend + outcome.
    if (ptt_opened || ptt_open_failed || ptt_keyed_count || ptt_error_count ||
        !ptt_test_cat_result.empty()) {
        std::ostringstream ptt_line;
        ptt_line << (ptt_backend.empty() ? "unknown" : ptt_backend);
        if (ptt_opened) {
            ptt_line << ", opened OK";
        } else if (ptt_open_failed) {
            ptt_line << ", open FAILED";
        }
        if (ptt_keyed_count > 0) {
            ptt_line << ", " << ptt_keyed_count << " key event(s)";
        }
        if (!ptt_test_cat_result.empty()) {
            ptt_line << ", Test CAT: " << ptt_test_cat_result;
        }
        if (ptt_error_count > 0) {
            ptt_line << ", " << ptt_error_count << " error(s)";
        }
        if (!ptt_first_error.empty()) {
            ptt_line << " — \"" << ptt_first_error << "\"";
        }
        out << "PTT/CAT:         " << ptt_line.str() << "\n";
    }

    out << "\nDisconnect:      " << disconnect_line << "\n";

    if (!suggestions.empty()) {
        out << "\n";
        for (const auto& suggestion : suggestions) {
            out << suggestion << "\n";
        }
    }

    (void)commit;
    result.text = out.str();
    result.operator_log_lines = {
        "Session outcome: " + result.outcome,
        "Wall time:       " + oneDecimal(wall_s) + " s",
        "Mode at end:     " + mode_at_end,
        "Decode failures: " + decode_line,
    };
    result.ok = true;
    return result;
}

SessionSummaryResult summarizeSessionJournal(const fs::path& journal_path,
                                             const SessionSummaryOptions& options) {
    const std::string jsonl = readTextFile(journal_path);
    if (jsonl.empty()) {
        SessionSummaryResult result;
        result.error = "session journal is empty or unreadable";
        return result;
    }
    return summarizeSessionJsonl(jsonl, options);
}

bool writeSessionSummary(const fs::path& output_path,
                         const SessionSummaryResult& summary,
                         std::string* error) {
    if (!summary.ok) {
        if (error) {
            *error = summary.error.empty() ? "summary was not generated" : summary.error;
        }
        return false;
    }
    if (!writeTextFile(output_path, summary.text)) {
        if (error) {
            *error = "failed to write summary";
        }
        return false;
    }
    return true;
}

} // namespace ultra::diagnostics
