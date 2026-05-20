#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "ota_channel_core/channel.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/logging.hpp"
#include "ultra/types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using ultra::Bytes;
namespace gui = ultra::gui;
namespace protocol = ultra::protocol;
namespace v2 = ultra::protocol::v2;

constexpr int kSampleRate = 48000;
constexpr size_t kPumpChunkSamples = 4800;
constexpr size_t kLightSearchSamples = 9600;
constexpr size_t kFullSearchSamples = 120000;

enum class MeasureConfig {
    AckLight,
    Data4Light,
    AckFull,
    WarmSyncLight,
};

struct Args {
    float snr_db = 0.0f;
    MeasureConfig config = MeasureConfig::AckLight;
    std::string config_name;
    uint64_t seed = 0;
    int n = 0;
};

struct Counts {
    int sync_fail = 0;
    int decode_fail = 0;
    int crc_fail = 0;
    int pass = 0;
};

struct FrameTrial {
    Bytes frame_bytes;
    std::vector<float> tx_samples;
};

struct TrialOutcome {
    bool sync_seen = false;
    bool decoder_failed = false;
    bool saw_result = false;
    gui::DecodeResult result;
};

bool bytesMatch(const Bytes& a, const Bytes& b);
void classify(const TrialOutcome& outcome, const Bytes& expected, Counts& counts);

void usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0
        << " --snr <db> --config <ack_light|data4_light|ack_full|warm_sync_light>"
        << " --seed <u64> --n <frames>\n";
}

MeasureConfig parseConfig(const std::string& name) {
    if (name == "ack_light") {
        return MeasureConfig::AckLight;
    }
    if (name == "data4_light") {
        return MeasureConfig::Data4Light;
    }
    if (name == "ack_full") {
        return MeasureConfig::AckFull;
    }
    if (name == "warm_sync_light") {
        return MeasureConfig::WarmSyncLight;
    }
    throw std::runtime_error("unknown --config: " + name);
}

Args parseArgs(int argc, char** argv) {
    Args args;
    bool have_snr = false;
    bool have_config = false;
    bool have_seed = false;
    bool have_n = false;

    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        auto requireValue = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + opt);
            }
            return argv[++i];
        };

        if (key == "--snr") {
            args.snr_db = std::stof(requireValue("--snr"));
            have_snr = true;
        } else if (key == "--config") {
            args.config_name = requireValue("--config");
            args.config = parseConfig(args.config_name);
            have_config = true;
        } else if (key == "--seed") {
            args.seed = std::stoull(requireValue("--seed"));
            have_seed = true;
        } else if (key == "--n") {
            args.n = std::stoi(requireValue("--n"));
            have_n = true;
        } else if (key == "--help" || key == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + key);
        }
    }

    if (!have_snr || !have_config || !have_seed || !have_n || args.n <= 0) {
        usage(argv[0]);
        throw std::runtime_error("missing required argument");
    }
    return args;
}

ultra::ModemConfig makeOFDMConfig() {
    ultra::ModemConfig cfg;
    cfg.fft_size = 1024;
    cfg.num_carriers = 59;
    cfg.sample_rate = kSampleRate;
    cfg.center_freq = 1500.0f;
    cfg.cp_mode = ultra::CyclicPrefixMode::LONG;
    cfg.modulation = ultra::Modulation::DQPSK;
    cfg.code_rate = ultra::CodeRate::R1_4;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 10;
    return cfg;
}

void configureEncoder(gui::StreamingEncoder& encoder) {
    const auto ofdm = makeOFDMConfig();
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(ofdm);
    encoder.setDataMode(ultra::Modulation::DQPSK, ultra::CodeRate::R1_4);
    encoder.setFixedFrameCodewords(v2::kDefaultFixedFrameCodewords);
}

void configureDecoder(gui::StreamingDecoder& decoder, MeasureConfig config) {
    const auto ofdm = makeOFDMConfig();
    if (config == MeasureConfig::AckFull) {
        decoder.setMode(protocol::WaveformMode::OFDM_CHIRP, false);
        decoder.setOFDMConfig(ofdm);
        decoder.setDataMode(ultra::Modulation::DQPSK, ultra::CodeRate::R1_4);
    } else {
        decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                     ofdm,
                                     ultra::Modulation::DQPSK,
                                     ultra::CodeRate::R1_4);
    }
    decoder.setFixedFrameCodewords(v2::kDefaultFixedFrameCodewords);
}

Bytes randomPayload(std::mt19937_64& rng, size_t size) {
    Bytes payload(size);
    for (uint8_t& b : payload) {
        b = static_cast<uint8_t>(rng() & 0xFFu);
    }
    return payload;
}

FrameTrial makeFrame(gui::StreamingEncoder& encoder,
                     MeasureConfig config,
                     std::mt19937_64& rng,
                     int frame_index) {
    const uint16_t seq = static_cast<uint16_t>((rng() + frame_index) & 0xFFFFu);

    FrameTrial trial;
    if (config == MeasureConfig::Data4Light) {
        const Bytes payload = randomPayload(rng, 20);
        auto frame = v2::makeFixedDataFrame("ALPHA", "BRAVO", seq, payload,
                                            ultra::CodeRate::R1_4,
                                            v2::kDefaultFixedFrameCodewords);
        trial.frame_bytes = frame.serialize();
        trial.tx_samples = encoder.encodeFrameLight(trial.frame_bytes);
    } else {
        const auto ack = v2::ControlFrame::makeAck("BRAVO", "ALPHA", seq);
        trial.frame_bytes = ack.serialize();
        trial.tx_samples = config == MeasureConfig::AckFull
            ? encoder.encodeFrame(trial.frame_bytes)
            : encoder.encodeFrameLight(trial.frame_bytes);
    }

    if (trial.tx_samples.empty()) {
        throw std::runtime_error("encoder produced no samples");
    }
    return trial;
}

FrameTrial makeAckFrame(gui::StreamingEncoder& encoder,
                        uint16_t seq,
                        bool full_preamble) {
    FrameTrial trial;
    const auto ack = v2::ControlFrame::makeAck("BRAVO", "ALPHA", seq);
    trial.frame_bytes = ack.serialize();
    trial.tx_samples = full_preamble
        ? encoder.encodeFrame(trial.frame_bytes)
        : encoder.encodeFrameLight(trial.frame_bytes);

    if (trial.tx_samples.empty()) {
        throw std::runtime_error("encoder produced no samples");
    }
    return trial;
}

void drainDecoder(gui::StreamingDecoder& decoder,
                  TrialOutcome& outcome,
                  uint64_t initial_frames_failed) {
    while (decoder.hasFrame()) {
        auto result = decoder.getFrame();
        if (result.is_ping) {
            continue;
        }
        outcome.saw_result = true;
        outcome.result = std::move(result);
    }

    const auto stats = decoder.getStats();
    if (stats.frames_failed > initial_frames_failed) {
        outcome.decoder_failed = true;
    }
}

TrialOutcome runFrame(gui::StreamingDecoder& decoder,
                      ultra::ota_channel_core::SimulatedChannel& channel,
                      const std::vector<float>& tx_samples,
                      bool full_preamble,
                      bool reset_decoder,
                      bool expect_full_anchor) {
    if (reset_decoder) {
        decoder.reset();
    }
    if (expect_full_anchor) {
        decoder.expectFullOFDMAnchorOnce();
    }

    TrialOutcome outcome;
    decoder.setDataSyncAcceptedCallback([&outcome](float) {
        outcome.sync_seen = true;
    });

    const auto initial_stats = decoder.getStats();
    const uint64_t initial_frames_failed = initial_stats.frames_failed;

    channel.transmitFromA(tx_samples);

    const size_t min_search = full_preamble ? kFullSearchSamples : kLightSearchSamples;
    const size_t target_samples = std::max(tx_samples.size() + 4 * kPumpChunkSamples,
                                           min_search + 4 * kPumpChunkSamples);

    size_t received = 0;
    while (received < target_samples) {
        const size_t take = std::min(kPumpChunkSamples, target_samples - received);
        auto rx = channel.receiveForB(take);
        decoder.feedAudio(rx.data(), rx.size());
        decoder.processBuffer();
        outcome.sync_seen = outcome.sync_seen || decoder.isSynced();
        drainDecoder(decoder, outcome, initial_frames_failed);

        if (outcome.saw_result && outcome.result.success) {
            break;
        }
        received += take;
    }

    for (int i = 0; i < 8 && !(outcome.saw_result && outcome.result.success); ++i) {
        auto rx = channel.receiveForB(kPumpChunkSamples);
        decoder.feedAudio(rx.data(), rx.size());
        decoder.processBuffer();
        outcome.sync_seen = outcome.sync_seen || decoder.isSynced();
        drainDecoder(decoder, outcome, initial_frames_failed);
    }

    return outcome;
}

TrialOutcome runTrial(gui::StreamingDecoder& decoder,
                      ultra::ota_channel_core::SimulatedChannel& channel,
                      const std::vector<float>& tx_samples,
                      bool full_preamble) {
    return runFrame(decoder, channel, tx_samples, full_preamble,
                    /*reset_decoder=*/true, /*expect_full_anchor=*/false);
}

bool isExpectedSuccess(const TrialOutcome& outcome, const Bytes& expected) {
    return outcome.saw_result &&
           outcome.result.success &&
           bytesMatch(outcome.result.frame_data, expected);
}

void runWarmSyncTrial(gui::StreamingEncoder& encoder,
                      gui::StreamingDecoder& decoder,
                      ultra::ota_channel_core::SimulatedChannel& channel,
                      const FrameTrial& measured_light,
                      uint16_t anchor_seq,
                      Counts& counts) {
    const FrameTrial anchor = makeAckFrame(encoder, anchor_seq, /*full_preamble=*/true);
    auto anchor_outcome = runFrame(decoder, channel, anchor.tx_samples,
                                   /*full_preamble=*/true,
                                   /*reset_decoder=*/true,
                                   /*expect_full_anchor=*/true);
    if (!isExpectedSuccess(anchor_outcome, anchor.frame_bytes)) {
        classify(anchor_outcome, anchor.frame_bytes, counts);
        return;
    }

    auto measured_outcome = runFrame(decoder, channel, measured_light.tx_samples,
                                     /*full_preamble=*/false,
                                     /*reset_decoder=*/false,
                                     /*expect_full_anchor=*/false);
    classify(measured_outcome, measured_light.frame_bytes, counts);
}

bool bytesMatch(const Bytes& a, const Bytes& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

void classify(const TrialOutcome& outcome, const Bytes& expected, Counts& counts) {
    if (outcome.saw_result) {
        if (outcome.result.success && bytesMatch(outcome.result.frame_data, expected)) {
            ++counts.pass;
            return;
        }

        if (outcome.result.success ||
            (outcome.result.codewords_ok > 0 && outcome.result.codewords_failed == 0)) {
            ++counts.crc_fail;
            return;
        }

        ++counts.decode_fail;
        return;
    }

    if (!outcome.sync_seen) {
        ++counts.sync_fail;
    } else {
        ++counts.decode_fail;
    }
}

Counts measure(const Args& args) {
    gui::StreamingEncoder encoder;
    configureEncoder(encoder);

    gui::StreamingDecoder decoder;
    configureDecoder(decoder, args.config);

    ultra::ota_channel_core::SimulatedChannel channel;
    channel.setSeed(args.seed);
    channel.configure(args.snr_db, ultra::ota_channel_core::ChannelType::AWGN);

    std::mt19937_64 payload_rng(args.seed ^ 0xA6E22C15D9B3F1A5ull);
    Counts counts;

    for (int i = 0; i < args.n; ++i) {
        const auto trial = makeFrame(encoder, args.config, payload_rng, i);
        if (args.config == MeasureConfig::WarmSyncLight) {
            const uint16_t anchor_seq =
                static_cast<uint16_t>((0x8000u + static_cast<unsigned>(i)) & 0xFFFFu);
            runWarmSyncTrial(encoder, decoder, channel, trial, anchor_seq, counts);
            continue;
        }

        const bool full_preamble = args.config == MeasureConfig::AckFull;
        auto outcome = runTrial(decoder, channel, trial.tx_samples, full_preamble);
        classify(outcome, trial.frame_bytes, counts);
    }

    return counts;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        ultra::setLogLevel(ultra::LogLevel::ERROR);
        const Args args = parseArgs(argc, argv);
        const Counts counts = measure(args);
        const int total = counts.sync_fail + counts.decode_fail +
                          counts.crc_fail + counts.pass;
        if (total != args.n) {
            std::cerr << "internal count mismatch: got " << total
                      << " expected " << args.n << "\n";
            return 2;
        }

        std::cout << args.snr_db << ","
                  << args.config_name << ","
                  << args.seed << ","
                  << args.n << ","
                  << counts.sync_fail << ","
                  << counts.decode_fail << ","
                  << counts.crc_fail << ","
                  << counts.pass << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "measure_ack_fer: " << e.what() << "\n";
        return 1;
    }
}
