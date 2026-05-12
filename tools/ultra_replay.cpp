#include "replay/bundle_loader.hpp"
#include "replay/divergence_report.hpp"
#include "replay/event_timeline.hpp"
#include "replay/replay_runner.hpp"

#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

namespace {

struct Args {
    std::filesystem::path bundle_path;
    std::filesystem::path json_path;
    bool realtime = false;
    std::string audio_side = "rx";
};

void printUsage() {
    std::cout
        << "ultra_replay --bundle <report.zip> [--json out.json] [--realtime] "
           "[--audio-side rx|tx]\n\n"
        << "Replays a ProjectUltra diagnostics bundle through the live StreamingDecoder\n"
        << "and compares replay frame outcomes against events/session.jsonl.\n\n"
        << "Default replay is as-fast-as-possible because most triage targets are\n"
        << "decode-chain reproducibility bugs. --realtime sleeps at audio pace, which\n"
        << "is slower but useful when investigating timing, backlog, or scheduler\n"
        << "coupling.\n";
}

std::optional<std::string> needValue(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        std::cerr << "Missing value for " << argv[i] << "\n";
        return std::nullopt;
    }
    return std::string(argv[++i]);
}

bool parseArgs(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            std::exit(0);
        } else if (arg == "--bundle") {
            auto v = needValue(i, argc, argv);
            if (!v) return false;
            args.bundle_path = *v;
        } else if (arg == "--json") {
            auto v = needValue(i, argc, argv);
            if (!v) return false;
            args.json_path = *v;
        } else if (arg == "--realtime") {
            args.realtime = true;
        } else if (arg == "--audio-side") {
            auto v = needValue(i, argc, argv);
            if (!v) return false;
            args.audio_side = *v;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }

    if (args.bundle_path.empty()) {
        std::cerr << "Need --bundle <report.zip>\n";
        return false;
    }
    if (args.audio_side != "rx" && args.audio_side != "tx") {
        std::cerr << "--audio-side must be rx or tx\n";
        return false;
    }
    return true;
}

bool writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parseArgs(argc, argv, args)) {
        printUsage();
        return 1;
    }

    try {
        auto bundle = ultra::replay::loadBundle(args.bundle_path);
        auto live = ultra::replay::parseEventTimeline(
            bundle.events_jsonl, bundle.initial_mode, bundle.initial_mode_available);
        live.warnings.insert(live.warnings.end(), bundle.warnings.begin(), bundle.warnings.end());

        ultra::replay::ReplayOptions options;
        options.audio_side = args.audio_side;
        options.realtime = args.realtime;

        auto replay = ultra::replay::runReplay(bundle, live, options);
        auto report = ultra::replay::compareTimelines(live, replay);

        std::cout << ultra::replay::renderTextReport(report);
        if (!args.json_path.empty()) {
            if (!writeText(args.json_path, ultra::replay::renderJsonReport(report))) {
                std::cerr << "ultra_replay: failed to write JSON report: "
                          << args.json_path << "\n";
                return 1;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "ultra_replay: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
