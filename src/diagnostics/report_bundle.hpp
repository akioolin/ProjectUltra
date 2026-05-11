#pragma once

#include "diagnostics/audio_ring.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ultra::diagnostics {

struct BundleFile {
    std::string archive_path;
    std::filesystem::path source_path;
};

struct BundleBuildInput {
    std::filesystem::path output_path;
    std::filesystem::path staging_dir;
    std::string manifest_json;
    std::string events_jsonl;
    std::optional<AudioRingSnapshot> rx_audio;
    std::optional<AudioRingSnapshot> tx_audio;
    std::string config_json;
    std::string operator_log;
    std::string system_json;
    std::string operator_note;
    std::string replay_readme;
};

struct BundleInspectSummary {
    bool ok = false;
    std::string manifest_json;
    std::vector<std::string> entries;
    std::string error;
};

bool buildReportBundle(const BundleBuildInput& input, std::string* error = nullptr);
BundleInspectSummary inspectReportBundle(const std::filesystem::path& archive_path);

} // namespace ultra::diagnostics
