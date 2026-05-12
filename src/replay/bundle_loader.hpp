#pragma once

#include "replay/event_timeline.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ultra::replay {

struct Bundle {
    Bundle() = default;
    ~Bundle();
    Bundle(const Bundle&) = delete;
    Bundle& operator=(const Bundle&) = delete;
    Bundle(Bundle&& other) noexcept;
    Bundle& operator=(Bundle&& other) noexcept;

    std::filesystem::path archive_path;
    std::filesystem::path extraction_dir;
    std::filesystem::path manifest_path;
    std::filesystem::path events_path;
    std::filesystem::path rx_audio_path;
    std::filesystem::path tx_audio_path;
    std::string manifest_json;
    std::string events_jsonl;
    ModeSpec initial_mode;
    bool initial_mode_available = false;
    int64_t audio_start_t_ms = 0;
    bool audio_start_assumed = true;
    uint32_t sample_rate = 48000;
    uint64_t rx_samples = 0;
    uint64_t rx_dropped_samples = 0;
    std::vector<std::string> entries;
    std::vector<std::string> warnings;

private:
    void cleanup() noexcept;
};

Bundle loadBundle(const std::filesystem::path& archive_path);

} // namespace ultra::replay
