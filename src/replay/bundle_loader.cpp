#include "replay/bundle_loader.hpp"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz/miniz.h"
#include "replay/json_util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace ultra::replay {
namespace fs = std::filesystem;

namespace {

std::string readTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool writeBinaryFile(const fs::path& path, const void* data, size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    if (size > 0) {
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    return out.good();
}

bool safeArchivePath(const std::string& name) {
    if (name.empty() || name.front() == '/' || name.front() == '\\') {
        return false;
    }
    fs::path p(name);
    for (const auto& part : p) {
        const std::string s = part.string();
        if (s == ".." || s == "." || s.empty()) {
            return false;
        }
    }
    return true;
}

fs::path makeExtractionDir(const fs::path& archive_path) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const int pid = 0;
#else
    const int pid = static_cast<int>(::getpid());
#endif
    std::string stem = archive_path.stem().string();
    if (stem.empty()) {
        stem = "bundle";
    }
    for (char& c : stem) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) {
            c = '_';
        }
    }
    return fs::temp_directory_path() /
           ("ultra_replay_" + stem + "_" + std::to_string(pid) + "_" +
            std::to_string(now));
}

std::optional<ModeSpec> modeSpecFromJsonObject(const std::string& object) {
    ModeSpec mode;
    bool any = false;

    if (auto wf = json::stringValue(object, "waveform")) {
        if (auto parsed = parseWaveformMode(*wf)) {
            mode.waveform = *parsed;
            mode.has_waveform = true;
            any = true;
        }
    }
    if (auto wf = json::stringValue(object, "mode")) {
        if (auto parsed = parseWaveformMode(*wf)) {
            mode.waveform = *parsed;
            mode.has_waveform = true;
            any = true;
        }
    }
    if (auto mod = json::stringValue(object, "modulation")) {
        if (auto parsed = parseModulation(*mod)) {
            mode.modulation = *parsed;
            mode.has_modulation = true;
            any = true;
        }
    }
    if (auto mod = json::stringValue(object, "mod")) {
        if (auto parsed = parseModulation(*mod)) {
            mode.modulation = *parsed;
            mode.has_modulation = true;
            any = true;
        }
    }
    if (auto rate = json::stringValue(object, "rate")) {
        if (auto parsed = parseCodeRate(*rate)) {
            mode.code_rate = *parsed;
            mode.has_code_rate = true;
            any = true;
        }
    }
    if (auto rate = json::stringValue(object, "code_rate")) {
        if (auto parsed = parseCodeRate(*rate)) {
            mode.code_rate = *parsed;
            mode.has_code_rate = true;
            any = true;
        }
    }
    if (auto cw = json::intValue(object, "cw_count")) {
        mode.cw_count = static_cast<int>(std::max<int64_t>(1, *cw));
        mode.has_cw_count = true;
        any = true;
    }
    if (auto cw = json::intValue(object, "cw")) {
        mode.cw_count = static_cast<int>(std::max<int64_t>(1, *cw));
        mode.has_cw_count = true;
        any = true;
    }
    if (auto connected = json::boolValue(object, "connected")) {
        mode.connected = *connected;
        mode.has_connected = true;
        any = true;
    }
    mode.source_event = "manifest.initial_mode";
    return any ? std::optional<ModeSpec>(mode) : std::nullopt;
}

void parseManifest(Bundle& bundle) {
    if (bundle.manifest_json.empty()) {
        bundle.warnings.push_back("manifest.json is empty or missing");
        return;
    }

    if (auto audio = json::rawObjectValue(bundle.manifest_json, "audio")) {
        if (auto sample_rate = json::intValue(*audio, "sample_rate")) {
            bundle.sample_rate = static_cast<uint32_t>(std::max<int64_t>(0, *sample_rate));
        }
        if (auto rx_samples = json::intValue(*audio, "rx_samples")) {
            bundle.rx_samples = static_cast<uint64_t>(std::max<int64_t>(0, *rx_samples));
        }
        if (auto dropped = json::intValue(*audio, "rx_dropped_samples")) {
            bundle.rx_dropped_samples = static_cast<uint64_t>(std::max<int64_t>(0, *dropped));
        }
        if (auto start = json::intValue(*audio, "start_t_ms")) {
            bundle.audio_start_t_ms = *start;
            bundle.audio_start_assumed = false;
        } else if (auto start2 = json::intValue(*audio, "rx_start_t_ms")) {
            bundle.audio_start_t_ms = *start2;
            bundle.audio_start_assumed = false;
        }
    }

    if (auto top_start = json::intValue(bundle.manifest_json, "audio_start_t_ms")) {
        bundle.audio_start_t_ms = *top_start;
        bundle.audio_start_assumed = false;
    }
    if (bundle.audio_start_assumed &&
        bundle.sample_rate > 0 &&
        bundle.rx_dropped_samples > 0) {
        bundle.audio_start_t_ms = static_cast<int64_t>(
            (bundle.rx_dropped_samples * 1000ULL) /
            static_cast<uint64_t>(bundle.sample_rate));
        bundle.audio_start_assumed = false;
        bundle.warnings.push_back(
            "manifest has no audio.start_t_ms; inferred RX audio start from rx_dropped_samples");
    }
    if (bundle.audio_start_assumed) {
        bundle.warnings.push_back("manifest has no audio.start_t_ms; assuming first RX sample is t=0");
    }

    if (auto initial = json::rawObjectValue(bundle.manifest_json, "initial_mode")) {
        if (auto mode = modeSpecFromJsonObject(*initial)) {
            bundle.initial_mode = *mode;
            bundle.initial_mode_available = true;
        }
    } else if (auto initial_rx = json::rawObjectValue(bundle.manifest_json, "initial_rx_mode")) {
        if (auto mode = modeSpecFromJsonObject(*initial_rx)) {
            bundle.initial_mode = *mode;
            bundle.initial_mode_available = true;
        }
    }

    if (!bundle.initial_mode_available) {
        bundle.warnings.push_back("manifest has no initial_mode; replay will infer/assume from events");
    }
}

} // namespace

Bundle::~Bundle() {
    cleanup();
}

Bundle::Bundle(Bundle&& other) noexcept
    : archive_path(std::move(other.archive_path)),
      extraction_dir(std::move(other.extraction_dir)),
      manifest_path(std::move(other.manifest_path)),
      events_path(std::move(other.events_path)),
      rx_audio_path(std::move(other.rx_audio_path)),
      tx_audio_path(std::move(other.tx_audio_path)),
      manifest_json(std::move(other.manifest_json)),
      events_jsonl(std::move(other.events_jsonl)),
      initial_mode(std::move(other.initial_mode)),
      initial_mode_available(other.initial_mode_available),
      audio_start_t_ms(other.audio_start_t_ms),
      audio_start_assumed(other.audio_start_assumed),
      sample_rate(other.sample_rate),
      rx_samples(other.rx_samples),
      rx_dropped_samples(other.rx_dropped_samples),
      entries(std::move(other.entries)),
      warnings(std::move(other.warnings)) {
    other.extraction_dir.clear();
}

Bundle& Bundle::operator=(Bundle&& other) noexcept {
    if (this != &other) {
        cleanup();
        archive_path = std::move(other.archive_path);
        extraction_dir = std::move(other.extraction_dir);
        manifest_path = std::move(other.manifest_path);
        events_path = std::move(other.events_path);
        rx_audio_path = std::move(other.rx_audio_path);
        tx_audio_path = std::move(other.tx_audio_path);
        manifest_json = std::move(other.manifest_json);
        events_jsonl = std::move(other.events_jsonl);
        initial_mode = std::move(other.initial_mode);
        initial_mode_available = other.initial_mode_available;
        audio_start_t_ms = other.audio_start_t_ms;
        audio_start_assumed = other.audio_start_assumed;
        sample_rate = other.sample_rate;
        rx_samples = other.rx_samples;
        rx_dropped_samples = other.rx_dropped_samples;
        entries = std::move(other.entries);
        warnings = std::move(other.warnings);
        other.extraction_dir.clear();
    }
    return *this;
}

void Bundle::cleanup() noexcept {
    if (extraction_dir.empty()) {
        return;
    }
    std::error_code ec;
    fs::remove_all(extraction_dir, ec);
    extraction_dir.clear();
}

Bundle loadBundle(const fs::path& archive_path) {
    std::error_code ec;
    if (!fs::exists(archive_path, ec) || fs::file_size(archive_path, ec) == 0) {
        throw std::runtime_error("bundle archive is empty or unreadable: " + archive_path.string());
    }

    Bundle bundle;
    bundle.archive_path = archive_path;
    bundle.extraction_dir = makeExtractionDir(archive_path);
    fs::create_directories(bundle.extraction_dir, ec);
    if (ec) {
        throw std::runtime_error("failed to create extraction directory: " + ec.message());
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, archive_path.string().c_str(), 0)) {
        throw std::runtime_error("not a zip archive: " + archive_path.string());
    }

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        char name_buf[1024] = {0};
        mz_zip_reader_get_filename(&zip, i, name_buf, sizeof(name_buf));
        const std::string name(name_buf);
        if (name.empty()) {
            continue;
        }
        bundle.entries.push_back(name);
        if (name.back() == '/') {
            continue;
        }
        if (!safeArchivePath(name)) {
            mz_zip_reader_end(&zip);
            throw std::runtime_error("unsafe archive entry path: " + name);
        }

        size_t out_size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &out_size, 0);
        if (!data && out_size != 0) {
            mz_zip_reader_end(&zip);
            throw std::runtime_error("failed to extract archive entry: " + name);
        }

        const fs::path dst = bundle.extraction_dir / fs::path(name);
        const bool ok = writeBinaryFile(dst, data, out_size);
        if (data) {
            mz_free(data);
        }
        if (!ok) {
            mz_zip_reader_end(&zip);
            throw std::runtime_error("failed to write extracted archive entry: " + name);
        }
    }
    mz_zip_reader_end(&zip);
    std::sort(bundle.entries.begin(), bundle.entries.end());

    bundle.manifest_path = bundle.extraction_dir / "manifest.json";
    bundle.events_path = bundle.extraction_dir / "events" / "session.jsonl";
    bundle.rx_audio_path = bundle.extraction_dir / "audio" / "rx_48k_s16.wav";
    bundle.tx_audio_path = bundle.extraction_dir / "audio" / "tx_48k_s16.wav";

    bundle.manifest_json = readTextFile(bundle.manifest_path);
    bundle.events_jsonl = readTextFile(bundle.events_path);
    parseManifest(bundle);

    if (bundle.events_jsonl.empty()) {
        bundle.warnings.push_back("events/session.jsonl is empty or missing");
    }
    if (!fs::exists(bundle.rx_audio_path, ec)) {
        bundle.warnings.push_back("audio/rx_48k_s16.wav is missing");
    }
    return bundle;
}

} // namespace ultra::replay
