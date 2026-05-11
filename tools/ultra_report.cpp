#include "diagnostics/diagnostics_recorder.hpp"
#include "diagnostics/report_bundle.hpp"
#include "diagnostics/redaction.hpp"
#include "diagnostics/session_summary.hpp"
#include "ultra/build_info.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

void usage(std::ostream& out) {
    out << "ultra_report [--list | --create [--note TEXT] [--include-callsigns] | "
           "--summary SESSION|newest [--include-callsigns] | --inspect PATH | --replay-prep PATH]\n";
}

struct ReportEntry {
    fs::path path;
    fs::file_time_type time;
    uintmax_t size = 0;
};

struct SessionEntry {
    std::string id;
    fs::path journal_path;
    fs::path summary_path;
    fs::file_time_type time;
};

std::vector<ReportEntry> listReports() {
    std::vector<ReportEntry> out;
    std::error_code ec;
    const fs::path dir = ultra::diagnostics::DiagnosticsRecorder::defaultDiagnosticsDir() / "reports";
    if (!fs::exists(dir, ec)) {
        return out;
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const auto path = entry.path();
        if (path.filename().string().rfind("ultra-report-", 0) != 0) {
            continue;
        }
        out.push_back({path, entry.last_write_time(ec), entry.file_size(ec)});
    }
    std::sort(out.begin(), out.end(), [](const ReportEntry& a, const ReportEntry& b) {
        return a.time > b.time;
    });
    return out;
}

std::vector<SessionEntry> listSessions() {
    std::vector<SessionEntry> out;
    std::error_code ec;
    const fs::path dir = ultra::diagnostics::DiagnosticsRecorder::defaultDiagnosticsDir() / "sessions";
    if (!fs::exists(dir, ec)) {
        return out;
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const auto path = entry.path();
        const std::string filename = path.filename().string();
        if (filename.rfind("session-", 0) != 0 || path.extension() != ".jsonl") {
            continue;
        }
        fs::path summary = path;
        summary.replace_extension(".txt");
        out.push_back({path.stem().string(), path, summary, entry.last_write_time(ec)});
    }
    std::sort(out.begin(), out.end(), [](const SessionEntry& a, const SessionEntry& b) {
        return a.time > b.time;
    });
    return out;
}

std::string readTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::string summaryOutcome(const fs::path& summary_path) {
    const std::string text = readTextFile(summary_path);
    const std::string needle = "Session outcome:";
    const size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        return "summary_pending";
    }
    size_t start = pos + needle.size();
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    size_t end = text.find('\n', start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string sizeText(uintmax_t bytes) {
    char buf[64];
    if (bytes >= 1024u * 1024u) {
        std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024u) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

int createReport(const std::string& note, bool include_callsigns) {
    auto& recorder = ultra::diagnostics::DiagnosticsRecorder::instance();
    ultra::diagnostics::SessionMeta meta;
    meta.app_name = "ultra_report";
    meta.station_role = "manual";
    meta.config_json =
        std::string("{\"app\":\"ultra_report\",\"version\":\"") +
        ultra::kBuildVersion + "\",\"git_commit\":\"" + ultra::kBuildGitCommit + "\"}";
    recorder.start(meta);
    recorder.emitText("operator", "session.state", "{\"state\":\"manual_snapshot\"}");
    recorder.emitText("operator", "waveform.negotiated", "{\"waveform\":\"unknown\"}");
    ultra::diagnostics::ReportOptions options;
    options.note = note;
    options.include_callsigns = include_callsigns;
    auto report = recorder.freeze(ultra::diagnostics::FreezeReason::Manual, options);
    recorder.stop();
    if (!report.ok) {
        std::cerr << "report creation failed: " << report.error << "\n";
        return 1;
    }
    std::cout << report.path << "\n";
    return 0;
}

std::optional<SessionEntry> findSession(const std::string& selector) {
    std::error_code ec;
    if (selector != "newest") {
        fs::path direct(selector);
        if (fs::exists(direct, ec)) {
            fs::path summary = direct;
            summary.replace_extension(".txt");
            return SessionEntry{direct.stem().string(), direct, summary, fs::last_write_time(direct, ec)};
        }
    }
    const auto sessions = listSessions();
    if (sessions.empty()) {
        return std::nullopt;
    }
    if (selector == "newest") {
        return sessions.front();
    }
    for (const auto& s : sessions) {
        if (s.id == selector || s.id.find(selector) != std::string::npos) {
            return s;
        }
    }
    return std::nullopt;
}

int renderSummary(const std::string& selector, bool include_callsigns) {
    auto session = findSession(selector);
    if (!session) {
        std::cerr << "session not found: " << selector << "\n";
        return 1;
    }
    auto options = ultra::diagnostics::defaultSessionSummaryOptions();
    options.app_name = "ultra_report";
    options.station_role = "report";
    options.include_callsigns = include_callsigns;
    auto summary = ultra::diagnostics::summarizeSessionJournal(session->journal_path, options);
    if (!summary.ok) {
        std::cerr << "summary failed: " << summary.error << "\n";
        return 1;
    }
    std::string error;
    if (!ultra::diagnostics::writeSessionSummary(session->summary_path, summary, &error)) {
        std::cerr << "summary write failed: " << error << "\n";
        return 1;
    }
    std::cout << summary.text;
    return 0;
}

int inspectReport(const fs::path& path) {
    auto summary = ultra::diagnostics::inspectReportBundle(path);
    if (!summary.ok) {
        std::cerr << "inspect failed: " << summary.error << "\n";
        return 1;
    }
    std::cout << "Archive: " << path << "\n";
    std::cout << "Entries:\n";
    for (const auto& entry : summary.entries) {
        std::cout << "  " << entry << "\n";
    }
    std::cout << "\nManifest:\n" << summary.manifest_json;
    return 0;
}

int replayPrep(const fs::path& path) {
    auto summary = ultra::diagnostics::inspectReportBundle(path);
    if (!summary.ok) {
        std::cerr << "replay-prep failed: " << summary.error << "\n";
        return 1;
    }
    std::cout << "Replay prep: archive is readable.\n"
              << "Use session_decode support for bundle ingest when it lands.\n";
    for (const auto& entry : summary.entries) {
        if (entry == "audio/rx_48k_s16.wav") {
            std::cout << "RX audio present: audio/rx_48k_s16.wav\n";
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc <= 1) {
        usage(std::cerr);
        return 1;
    }

    std::string note;
    bool include_callsigns = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        }
        if (arg == "--version") {
            std::cout << "ProjectUltra " << ultra::kBuildVersion
                      << " commit=" << ultra::kBuildGitCommit
                      << " dirty=" << (ultra::kBuildDirty ? "true" : "false")
                      << " built=" << ultra::kBuildTimeUtc
                      << " os=" << ultra::kBuildOS << "\n";
            return 0;
        }
        if (arg == "--include-callsigns") {
            include_callsigns = true;
            continue;
        }
        if (arg == "--list") {
            const auto sessions = listSessions();
            const auto reports = listReports();
            if (sessions.empty() && reports.empty()) {
                std::cout << "No local diagnostics found in "
                          << ultra::diagnostics::DiagnosticsRecorder::defaultDiagnosticsDir()
                          << "\n";
                return 0;
            }
            if (!sessions.empty()) {
                std::cout << "Sessions:\n";
                for (const auto& s : sessions) {
                    std::cout << "  " << summaryOutcome(s.summary_path) << "  "
                              << s.id << "  " << s.summary_path << "\n";
                }
            }
            if (!reports.empty()) {
                if (!sessions.empty()) {
                    std::cout << "\n";
                }
                std::cout << "Reports:\n";
            }
            for (const auto& r : reports) {
                std::cout << "  " << sizeText(r.size) << "  " << r.path << "\n";
            }
            return 0;
        }
        if (arg == "--create") {
            for (int j = i + 1; j < argc; ++j) {
                const std::string next = argv[j];
                if (next == "--note" && j + 1 < argc) {
                    note = argv[++j];
                } else if (next == "--include-callsigns") {
                    include_callsigns = true;
                } else {
                    std::cerr << "unknown --create option: " << next << "\n";
                    return 1;
                }
            }
            return createReport(note, include_callsigns);
        }
        if (arg == "--summary") {
            if (i + 1 >= argc) {
                std::cerr << "missing session id for --summary\n";
                return 1;
            }
            const std::string selector = argv[++i];
            for (int j = i + 1; j < argc; ++j) {
                const std::string next = argv[j];
                if (next == "--include-callsigns") {
                    include_callsigns = true;
                } else {
                    std::cerr << "unknown --summary option: " << next << "\n";
                    return 1;
                }
            }
            return renderSummary(selector, include_callsigns);
        }
        if (arg == "--inspect") {
            if (i + 1 >= argc) {
                std::cerr << "missing path for --inspect\n";
                return 1;
            }
            return inspectReport(argv[i + 1]);
        }
        if (arg == "--replay-prep") {
            if (i + 1 >= argc) {
                std::cerr << "missing path for --replay-prep\n";
                return 1;
            }
            return replayPrep(argv[i + 1]);
        }
        std::cerr << "unknown option: " << arg << "\n";
        usage(std::cerr);
        return 1;
    }
    return 0;
}
