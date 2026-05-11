#include "diagnostics/report_bundle.hpp"

#include "miniz/miniz.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

namespace ultra::diagnostics {

namespace fs = std::filesystem;

namespace {

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

std::vector<uint8_t> readBinaryFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

} // namespace

bool buildReportBundle(const BundleBuildInput& input, std::string* error) {
    std::error_code ec;
    fs::remove_all(input.staging_dir, ec);
    fs::create_directories(input.staging_dir, ec);
    if (ec) {
        if (error) *error = "create staging failed";
        return false;
    }

    const std::vector<std::pair<std::string, std::string>> text_files = {
        {"manifest.json", input.manifest_json},
        {"events/session.jsonl", input.events_jsonl},
        {"config/effective_config.redacted.json", input.config_json},
        {"logs/operator.log", input.operator_log},
        {"system/system.json", input.system_json},
        {"notes/operator_note.txt", input.operator_note},
        {"replay/README.md", input.replay_readme},
    };
    for (const auto& [rel, text] : text_files) {
        if (!writeTextFile(input.staging_dir / rel, text)) {
            if (error) *error = "write staging text failed: " + rel;
            return false;
        }
    }
    if (input.rx_audio) {
        if (!writeWavPcm16(input.staging_dir / "audio/rx_48k_s16.wav", *input.rx_audio)) {
            if (error) *error = "write rx wav failed";
            return false;
        }
    }
    if (input.tx_audio) {
        if (!writeWavPcm16(input.staging_dir / "audio/tx_48k_s16.wav", *input.tx_audio)) {
            if (error) *error = "write tx wav failed";
            return false;
        }
    }

    std::vector<std::string> rels = {
        "manifest.json",
        "events/session.jsonl",
        "config/effective_config.redacted.json",
        "logs/operator.log",
        "system/system.json",
        "notes/operator_note.txt",
        "replay/README.md",
    };
    if (input.rx_audio) rels.push_back("audio/rx_48k_s16.wav");
    if (input.tx_audio) rels.push_back("audio/tx_48k_s16.wav");
    std::sort(rels.begin(), rels.end());

    fs::create_directories(input.output_path.parent_path(), ec);

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, input.output_path.string().c_str(), 0)) {
        if (error) *error = "open zip output failed";
        return false;
    }

    for (const auto& rel : rels) {
        auto data = readBinaryFile(input.staging_dir / rel);
        if (!mz_zip_writer_add_mem(&zip, rel.c_str(),
                                   data.empty() ? nullptr : data.data(),
                                   data.size(), MZ_DEFAULT_LEVEL)) {
            if (error) *error = "zip add failed: " + rel;
            mz_zip_writer_end(&zip);
            fs::remove(input.output_path, ec);
            return false;
        }
    }

    if (!mz_zip_writer_finalize_archive(&zip)) {
        if (error) *error = "zip finalize failed";
        mz_zip_writer_end(&zip);
        fs::remove(input.output_path, ec);
        return false;
    }
    mz_zip_writer_end(&zip);
    return true;
}

BundleInspectSummary inspectReportBundle(const fs::path& archive_path) {
    BundleInspectSummary summary;
    std::error_code ec;
    if (!fs::exists(archive_path, ec) || fs::file_size(archive_path, ec) == 0) {
        summary.error = "archive is empty or unreadable";
        return summary;
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, archive_path.string().c_str(), 0)) {
        summary.error = "not a zip archive";
        return summary;
    }

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        char name[512] = {0};
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        summary.entries.emplace_back(name);
        if (std::strcmp(name, "manifest.json") == 0) {
            size_t out_size = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &out_size, 0);
            if (data) {
                summary.manifest_json.assign(static_cast<const char*>(data), out_size);
                mz_free(data);
            }
        }
    }
    mz_zip_reader_end(&zip);

    std::sort(summary.entries.begin(), summary.entries.end());
    summary.ok = true;
    return summary;
}

} // namespace ultra::diagnostics
