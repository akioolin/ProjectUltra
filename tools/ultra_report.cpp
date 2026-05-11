#include "diagnostics/diagnostics_recorder.hpp"
#include "diagnostics/report_bundle.hpp"
#include "diagnostics/redaction.hpp"
#include "ultra/build_info.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

void usage(std::ostream& out) {
    out << "ultra_report [--list | --create [--note TEXT] | --inspect PATH | --replay-prep PATH]\n";
}

struct ReportEntry {
    fs::path path;
    fs::file_time_type time;
    uintmax_t size = 0;
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

int createReport(const std::string& note) {
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
    auto report = recorder.freeze(ultra::diagnostics::FreezeReason::Manual, options);
    recorder.stop();
    if (!report.ok) {
        std::cerr << "report creation failed: " << report.error << "\n";
        return 1;
    }
    std::cout << report.path << "\n";
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
        if (arg == "--list") {
            const auto reports = listReports();
            if (reports.empty()) {
                std::cout << "No local reports found in "
                          << (ultra::diagnostics::DiagnosticsRecorder::defaultDiagnosticsDir() / "reports")
                          << "\n";
                return 0;
            }
            for (const auto& r : reports) {
                std::cout << sizeText(r.size) << "  " << r.path << "\n";
            }
            return 0;
        }
        if (arg == "--create") {
            for (int j = i + 1; j < argc; ++j) {
                const std::string next = argv[j];
                if (next == "--note" && j + 1 < argc) {
                    note = argv[++j];
                } else {
                    std::cerr << "unknown --create option: " << next << "\n";
                    return 1;
                }
            }
            return createReport(note);
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
