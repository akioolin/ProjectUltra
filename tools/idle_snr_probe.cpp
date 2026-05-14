#include "gui/modem/idle_noise_snr_estimator.hpp"
#include "gui/modem/streaming_decoder.hpp"
#include "sim/cli_enums.hpp"
#include "sim/simulated_station.hpp"
#include "ultra/logging.hpp"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

struct Args {
    float snr_db = 15.0f;
    ::ChannelType channel = ::ChannelType::AWGN;
    uint32_t seed = 42;
    size_t idle_samples = 48000 * 4;
    bool header = true;
    bool streaming = false;
};

void usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [--snr DB] [--channel awgn|good|moderate|poor|flutter]\n"
        << "       [--seed N] [--idle-samples N] [--streaming] [--no-header]\n";
}

const char* channelName(::ChannelType channel) {
    switch (channel) {
        case ::ChannelType::AWGN: return "AWGN";
        case ::ChannelType::GOOD: return "GOOD";
        case ::ChannelType::MODERATE: return "MODERATE";
        case ::ChannelType::POOR: return "POOR";
        case ::ChannelType::FLUTTER: return "FLUTTER";
        default: return "UNKNOWN";
    }
}

bool parseArgs(int argc, char** argv, Args& args) {
    namespace cli = ultra::tools::cli;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--snr") {
            const char* v = need("--snr");
            if (!v) return false;
            args.snr_db = std::stof(v);
        } else if (arg == "--channel") {
            const char* v = need("--channel");
            if (!v) return false;
            auto parsed = cli::parseChannelType(v);
            if (!parsed) {
                std::cerr << "Unknown channel: " << v << "\n";
                return false;
            }
            args.channel = *parsed;
        } else if (arg == "--seed") {
            const char* v = need("--seed");
            if (!v) return false;
            args.seed = static_cast<uint32_t>(std::stoul(v));
        } else if (arg == "--idle-samples") {
            const char* v = need("--idle-samples");
            if (!v) return false;
            args.idle_samples = std::stoull(v);
        } else if (arg == "--streaming") {
            args.streaming = true;
        } else if (arg == "--no-header") {
            args.header = false;
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parseArgs(argc, argv, args)) {
        return 1;
    }

    ultra::setLogLevel(ultra::LogLevel::ERROR);

    ::SimulatedChannel sim;
    sim.setSeed(args.seed);
    sim.configure(args.snr_db, args.channel);

    std::vector<float> tx(args.idle_samples, 0.0f);
    sim.transmitFromA(tx);
    std::vector<float> rx = sim.receiveForB(args.idle_samples);

    ultra::gui::IdleNoiseSNREstimator::Snapshot s;
    if (args.streaming) {
        ultra::gui::StreamingDecoder decoder;
        constexpr size_t kChunk = 4800;
        for (size_t off = 0; off < rx.size(); off += kChunk) {
            const size_t n = std::min(kChunk, rx.size() - off);
            decoder.feedAudio(rx.data() + off, n);
            decoder.processBuffer();
        }
        s = decoder.getIdleNoiseSNRSnapshot();
    } else {
        ultra::gui::IdleNoiseSNREstimator estimator;
        estimator.observeIdleAudio(rx.data(), rx.size());
        s = estimator.snapshot();
    }

    if (args.header) {
        std::cout << "channel,configured_snr,seed,source,valid,idle_snr_db,delta_db,"
                  << "instant_snr_db,filtered_noise_rms,normalized_noise_rms,"
                  << "fir_energy,enbw_hz,windows\n";
    }

    const float delta = s.snr_db - args.snr_db;
    std::cout << channelName(args.channel) << ","
              << std::fixed << std::setprecision(2)
              << args.snr_db << ","
              << args.seed << ","
              << (args.streaming ? "streaming" : "direct") << ","
              << (s.valid ? 1 : 0) << ","
              << s.snr_db << ","
              << delta << ","
              << s.latest_instant_snr_db << ","
              << s.filtered_noise_rms << ","
              << s.normalized_noise_rms << ","
              << std::setprecision(8) << s.fir_energy << ","
              << std::setprecision(2) << s.equivalent_noise_bandwidth_hz << ","
              << s.windows_observed << "\n";

    return s.valid ? 0 : 2;
}
