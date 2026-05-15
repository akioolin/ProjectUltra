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
        << "  ota_simulator run --scenario FILE.json [--save-rx-audio]\n";
}

std::string deriveRxPath(const std::string& tx_path) {
    const std::string needle = "_tx";
    const auto pos = tx_path.rfind(needle);
    if (pos != std::string::npos) {
        return tx_path.substr(0, pos) + "_rx" + tx_path.substr(pos + needle.size());
    }
    const auto dot = tx_path.rfind('.');
    if (dot != std::string::npos) {
        return tx_path.substr(0, dot) + "_rx" + tx_path.substr(dot);
    }
    return tx_path + "_rx.wav";
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
    bool save_rx_audio = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--scenario") {
            scenario_path = requireValue(i, argc, argv, arg);
        } else if (arg == "--save-rx-audio") {
            save_rx_audio = true;
        } else {
            throw std::runtime_error("unknown run argument: " + arg);
        }
    }
    if (scenario_path.empty()) {
        throw std::runtime_error("run requires --scenario");
    }
    auto scenario = ultra::tools::ota::loadScenario(scenario_path);
    if (save_rx_audio) {
        if (scenario.output.alice_rx_capture.empty()) {
            scenario.output.alice_rx_capture =
                deriveRxPath(scenario.output.alice_tx_capture);
        }
        if (scenario.output.bob_rx_capture.empty()) {
            scenario.output.bob_rx_capture =
                deriveRxPath(scenario.output.bob_tx_capture);
        }
    }
    return ultra::tools::ota::runScenario(scenario);
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
