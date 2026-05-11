#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ultra::diagnostics {

struct SessionSummaryOptions {
    std::string app_name = "projectultra";
    std::string station_role;
    std::string local_callsign;
    std::string peer_callsign;
    std::string build_version;
    std::string build_commit;
    std::string build_os;
    bool include_callsigns = false;
};

struct SessionSummaryResult {
    bool ok = false;
    std::string outcome;
    std::string text;
    std::vector<std::string> operator_log_lines;
    std::string error;
};

SessionSummaryOptions defaultSessionSummaryOptions();
SessionSummaryResult summarizeSessionJsonl(const std::string& jsonl,
                                           const SessionSummaryOptions& options);
SessionSummaryResult summarizeSessionJournal(const std::filesystem::path& journal_path,
                                             const SessionSummaryOptions& options);
bool writeSessionSummary(const std::filesystem::path& output_path,
                         const SessionSummaryResult& summary,
                         std::string* error = nullptr);

} // namespace ultra::diagnostics
