#include "ota_simulator/clip_gen.hpp"
#include "ota_simulator/runner.hpp"
#include "ota_simulator/scenario.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void usage() {
    std::cerr
        << "Usage:\n"
        << "  ota_simulator gen --frame FRAME --callsign CALL [--peer-callsign CALL] --out FILE.wav\n"
        << "  ota_simulator run --scenario FILE.json\n";
}

std::string requireValue(int& i, int argc, char** argv, const std::string& opt) {
    if (i + 1 >= argc) {
        throw std::runtime_error(opt + " requires a value");
    }
    return argv[++i];
}

int runGen(int argc, char** argv) {
    ultra::tools::ota::ClipGenOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frame") {
            options.frame = requireValue(i, argc, argv, arg);
        } else if (arg == "--callsign") {
            options.callsign = requireValue(i, argc, argv, arg);
        } else if (arg == "--peer-callsign" || arg == "--peer") {
            options.peer_callsign = requireValue(i, argc, argv, arg);
        } else if (arg == "--out") {
            options.out_path = requireValue(i, argc, argv, arg);
        } else {
            throw std::runtime_error("unknown gen argument: " + arg);
        }
    }
    return ultra::tools::ota::generateClip(options);
}

int runScenario(int argc, char** argv) {
    std::string scenario_path;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--scenario") {
            scenario_path = requireValue(i, argc, argv, arg);
        } else {
            throw std::runtime_error("unknown run argument: " + arg);
        }
    }
    if (scenario_path.empty()) {
        throw std::runtime_error("run requires --scenario");
    }
    return ultra::tools::ota::runScenario(
        ultra::tools::ota::loadScenario(scenario_path));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    try {
        const std::string command = argv[1];
        if (command == "gen") {
            return runGen(argc, argv);
        }
        if (command == "run") {
            return runScenario(argc, argv);
        }
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "ota_simulator: " << e.what() << "\n";
        return 1;
    }
}
