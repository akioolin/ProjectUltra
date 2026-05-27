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
    Data4Full,
    AckFull,
    WarmSyncLight,
    BurstChunk,
};

struct Args {
    float snr_db = 0.0f;
    MeasureConfig config = MeasureConfig::AckLight;
    std::string config_name;
    uint64_t seed = 0;
    int n = 0;
    // Diversity-sweep parameters (default to the legacy DQPSK R1/4 AWGN behavior
    // so existing invocations are byte-identical).
    ultra::Modulation mod = ultra::Modulation::DQPSK;
    ultra::CodeRate rate = ultra::CodeRate::R1_4;
    ultra::ota_channel_core::ChannelType channel =
        ultra::ota_channel_core::ChannelType::AWGN;
    bool carrier_interleave = false;
    std::string mod_name = "dqpsk";
    std::string rate_name = "r1_4";
    std::string channel_name = "awgn";
    // Burst-chunk (file-class keystone) parameters.
    int group = 0;               // frames per burst group (chunk length); clamps to [2,8]
    bool burst_interleave = true; // cross-frame deep interleave ON/OFF (the A/B)
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
        << " --snr <db> --config <ack_light|data4_light|data4_full|ack_full|warm_sync_light|burst_chunk>"
        << " --seed <u64> --n <frames|chunks>"
        << " [--channel awgn|good|moderate|poor]"
        << " [--mod dqpsk|qpsk|d8psk|qam8|qam16]"
        << " [--rate r1_4|r1_2|r2_3|r3_4]"
        << " [--carrier-interleave 0|1]"
        << " [--group <2..8>] [--burst-interleave 0|1]\n"
        << "CSV (frame): snr,config,channel,mod,rate,ci,seed,n,sync_fail,decode_fail,crc_fail,pass\n"
        << "CSV (burst): snr,config,channel,mod,rate,bi,group,seed,chunks,frames_total,"
           "frames_recovered,chunks_complete,chunk_sync_fail\n";
}

MeasureConfig parseConfig(const std::string& name) {
    if (name == "ack_light") {
        return MeasureConfig::AckLight;
    }
    if (name == "data4_light") {
        return MeasureConfig::Data4Light;
    }
    if (name == "data4_full") {
        return MeasureConfig::Data4Full;
    }
    if (name == "burst_chunk") {
        return MeasureConfig::BurstChunk;
    }
    if (name == "ack_full") {
        return MeasureConfig::AckFull;
    }
    if (name == "warm_sync_light") {
        return MeasureConfig::WarmSyncLight;
    }
    throw std::runtime_error("unknown --config: " + name);
}

ultra::Modulation parseMod(const std::string& s) {
    if (s == "dqpsk") return ultra::Modulation::DQPSK;
    if (s == "qpsk") return ultra::Modulation::QPSK;  // coherent
    if (s == "d8psk") return ultra::Modulation::D8PSK;
    if (s == "qam8") return ultra::Modulation::QAM8;
    if (s == "qam16") return ultra::Modulation::QAM16;
    throw std::runtime_error("unknown --mod: " + s);
}

ultra::CodeRate parseRate(const std::string& s) {
    if (s == "r1_4") return ultra::CodeRate::R1_4;
    if (s == "r1_2") return ultra::CodeRate::R1_2;
    if (s == "r2_3") return ultra::CodeRate::R2_3;
    if (s == "r3_4") return ultra::CodeRate::R3_4;
    throw std::runtime_error("unknown --rate: " + s);
}

ultra::ota_channel_core::ChannelType parseChannel(const std::string& s) {
    using CT = ultra::ota_channel_core::ChannelType;
    if (s == "awgn") return CT::AWGN;
    if (s == "good") return CT::GOOD;
    if (s == "moderate") return CT::MODERATE;
    if (s == "poor") return CT::POOR;
    throw std::runtime_error("unknown --channel: " + s);
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
        } else if (key == "--mod") {
            args.mod_name = requireValue("--mod");
            args.mod = parseMod(args.mod_name);
        } else if (key == "--rate") {
            args.rate_name = requireValue("--rate");
            args.rate = parseRate(args.rate_name);
        } else if (key == "--channel") {
            args.channel_name = requireValue("--channel");
            args.channel = parseChannel(args.channel_name);
        } else if (key == "--carrier-interleave") {
            args.carrier_interleave = std::stoi(requireValue("--carrier-interleave")) != 0;
        } else if (key == "--group") {
            args.group = std::stoi(requireValue("--group"));
        } else if (key == "--burst-interleave") {
            args.burst_interleave = std::stoi(requireValue("--burst-interleave")) != 0;
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

ultra::ModemConfig makeOFDMConfig(ultra::Modulation mod, ultra::CodeRate rate) {
    ultra::ModemConfig cfg;
    cfg.fft_size = 1024;
    cfg.num_carriers = 59;
    cfg.sample_rate = kSampleRate;
    cfg.center_freq = 1500.0f;
    cfg.cp_mode = ultra::CyclicPrefixMode::LONG;
    cfg.modulation = mod;
    cfg.code_rate = rate;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 10;
    return cfg;
}

bool usesFullPreamble(MeasureConfig config) {
    return config == MeasureConfig::AckFull || config == MeasureConfig::Data4Full;
}

void configureEncoder(gui::StreamingEncoder& encoder, const Args& args) {
    const auto ofdm = makeOFDMConfig(args.mod, args.rate);
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(ofdm);
    encoder.setDataMode(args.mod, args.rate);
    encoder.setFixedFrameCodewords(v2::kDefaultFixedFrameCodewords);
    encoder.setCarrierLdpcInterleaver(args.carrier_interleave);
}

void configureDecoder(gui::StreamingDecoder& decoder, const Args& args) {
    const auto ofdm = makeOFDMConfig(args.mod, args.rate);
    // Only 1-CW control frames decode on the cold setMode() path. 4-CW DATA
    // frames are a connected-mode construct, so Data4Full uses the connected
    // decoder too — its full chirp preamble is admitted via
    // expectFullOFDMAnchorOnce() (see measure()), giving reliable sync at any
    // fade depth WITHOUT the cold-light-sync selection bias that hides the
    // deep-fade decode failures we are trying to measure.
    if (args.config == MeasureConfig::AckFull) {
        decoder.setMode(protocol::WaveformMode::OFDM_CHIRP, false);
        decoder.setOFDMConfig(ofdm);
        decoder.setDataMode(args.mod, args.rate);
    } else {
        decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                     ofdm,
                                     args.mod,
                                     args.rate);
    }
    decoder.setFixedFrameCodewords(v2::kDefaultFixedFrameCodewords);
    decoder.setCarrierLdpcInterleaver(args.carrier_interleave);
}

Bytes randomPayload(std::mt19937_64& rng, size_t size) {
    Bytes payload(size);
    for (uint8_t& b : payload) {
        b = static_cast<uint8_t>(rng() & 0xFFu);
    }
    return payload;
}

FrameTrial makeFrame(gui::StreamingEncoder& encoder,
                     const Args& args,
                     std::mt19937_64& rng,
                     int frame_index) {
    const MeasureConfig config = args.config;
    const uint16_t seq = static_cast<uint16_t>((rng() + frame_index) & 0xFFFFu);
    const bool is_data = config == MeasureConfig::Data4Light ||
                         config == MeasureConfig::Data4Full;

    FrameTrial trial;
    if (is_data) {
        const Bytes payload = randomPayload(rng, 20);
        auto frame = v2::makeFixedDataFrame("ALPHA", "BRAVO", seq, payload,
                                            args.rate,
                                            v2::kDefaultFixedFrameCodewords);
        trial.frame_bytes = frame.serialize();
        trial.tx_samples = config == MeasureConfig::Data4Full
            ? encoder.encodeFrame(trial.frame_bytes)
            : encoder.encodeFrameLight(trial.frame_bytes);
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
    configureEncoder(encoder, args);

    gui::StreamingDecoder decoder;
    configureDecoder(decoder, args);

    ultra::ota_channel_core::SimulatedChannel channel;
    channel.setSeed(args.seed);
    channel.configure(args.snr_db, args.channel);

    std::mt19937_64 payload_rng(args.seed ^ 0xA6E22C15D9B3F1A5ull);
    Counts counts;

    for (int i = 0; i < args.n; ++i) {
        const auto trial = makeFrame(encoder, args, payload_rng, i);
        if (args.config == MeasureConfig::WarmSyncLight) {
            const uint16_t anchor_seq =
                static_cast<uint16_t>((0x8000u + static_cast<unsigned>(i)) & 0xFFFFu);
            runWarmSyncTrial(encoder, decoder, channel, trial, anchor_seq, counts);
            continue;
        }

        const bool full_preamble = usesFullPreamble(args.config);
        // Data4Full decodes on the connected path; its full chirp preamble must
        // be admitted as an anchor so sync succeeds at any fade depth (isolating
        // decode-diversity from light-sync acquisition). AckFull uses the cold
        // setMode path and does not need anchor admission.
        const bool expect_anchor = args.config == MeasureConfig::Data4Full;
        auto outcome = runFrame(decoder, channel, trial.tx_samples, full_preamble,
                                /*reset_decoder=*/true,
                                /*expect_full_anchor=*/expect_anchor);
        classify(outcome, trial.frame_bytes, counts);
    }

    return counts;
}

// === Burst-chunk (file-class keystone) measurement ===========================
// THE keystone validation: send a CHUNK of N file frames as ONE burst (full
// chirp anchor + N-1 light preambles, per encodeBurstLight), with cross-frame
// deep interleave ON vs OFF, through a fading channel, and measure how many of
// the N frames RECOVER. The premise of the file-class architecture is that a
// deep fade null that kills an ISOLATED frame only NICKS each codeword when the
// codeword is spread across a chunk longer than the fade, so FEC recovers it.
// That is a block-level / cross-frame property; single-frame FER cannot show it.
struct BurstCounts {
    int chunks = 0;
    int chunk_sync_fail = 0;  // anchor never synced → whole chunk lost
    int frames_total = 0;
    int frames_recovered = 0;
    int chunks_complete = 0;  // all N frames in the chunk recovered
};

BurstCounts measureBurst(const Args& args) {
    int group = args.group > 0 ? args.group : 8;
    // Persistent channel: the Watterson fade advances continuously across chunks
    // so successive chunks sample different fade phases (a real distribution).
    ultra::ota_channel_core::SimulatedChannel channel;
    channel.setSeed(args.seed);
    channel.configure(args.snr_db, args.channel);

    std::mt19937_64 payload_rng(args.seed ^ 0xA6E22C15D9B3F1A5ull);
    BurstCounts bc;

    for (int c = 0; c < args.n; ++c) {
        // Fresh encoder + decoder per chunk → each chunk starts from idle, so
        // encodeBurstLight emits a FULL chirp anchor (reliable sync at any fade
        // depth); the channel persists (fade keeps advancing).
        gui::StreamingEncoder encoder;
        configureEncoder(encoder, args);
        encoder.setBurstInterleave(args.burst_interleave);
        encoder.setBurstInterleaveGroupSize(group);

        gui::StreamingDecoder decoder;
        configureDecoder(decoder, args);  // connected path (BurstChunk != AckFull)
        decoder.setBurstInterleave(args.burst_interleave);
        decoder.setBurstInterleaveGroupSize(group);
        decoder.expectFullOFDMAnchorOnce();

        const int clamped = encoder.getBurstInterleaveGroupSize();  // [2,8]
        std::vector<Bytes> frames;
        std::vector<Bytes> expected;
        frames.reserve(static_cast<size_t>(clamped));
        for (int f = 0; f < clamped; ++f) {
            Bytes payload = randomPayload(payload_rng, 20);
            payload[0] = static_cast<uint8_t>(c & 0xFF);
            payload[1] = static_cast<uint8_t>(f & 0xFF);  // make each frame unique
            const uint16_t seq = static_cast<uint16_t>((c * clamped + f) & 0xFFFFu);
            auto frame = v2::makeFixedDataFrame("ALPHA", "BRAVO", seq, payload,
                                                args.rate,
                                                v2::kDefaultFixedFrameCodewords);
            Bytes ser = frame.serialize();
            frames.push_back(ser);
            expected.push_back(std::move(ser));
        }

        const auto tx = encoder.encodeBurstLight(frames);
        if (tx.empty()) {
            throw std::runtime_error("burst encoder produced no samples");
        }
        channel.transmitFromA(tx);

        std::vector<Bytes> recovered;
        bool any_sync = false;
        const size_t target = tx.size() + 8 * kPumpChunkSamples;
        size_t received = 0;
        while (received < target) {
            const size_t take = std::min(kPumpChunkSamples, target - received);
            auto rx = channel.receiveForB(take);
            decoder.feedAudio(rx.data(), rx.size());
            decoder.processBuffer();
            any_sync = any_sync || decoder.isSynced();
            while (decoder.hasFrame()) {
                auto r = decoder.getFrame();
                if (r.is_ping) {
                    continue;
                }
                if (r.success) {
                    recovered.push_back(r.frame_data);
                }
            }
            received += take;
        }

        int rec = 0;
        for (const auto& e : expected) {
            for (const auto& g : recovered) {
                if (bytesMatch(e, g)) {
                    ++rec;
                    break;
                }
            }
        }

        ++bc.chunks;
        bc.frames_total += clamped;
        bc.frames_recovered += rec;
        if (rec == clamped) {
            ++bc.chunks_complete;
        }
        if (rec == 0 && !any_sync) {
            ++bc.chunk_sync_fail;
        }
    }

    return bc;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        ultra::setLogLevel(ultra::LogLevel::ERROR);
        const Args args = parseArgs(argc, argv);

        if (args.config == MeasureConfig::BurstChunk) {
            const BurstCounts bc = measureBurst(args);
            // CSV: snr,config,channel,mod,rate,bi,group,seed,chunks,frames_total,
            //      frames_recovered,chunks_complete,chunk_sync_fail
            std::cout << args.snr_db << ","
                      << args.config_name << ","
                      << args.channel_name << ","
                      << args.mod_name << ","
                      << args.rate_name << ","
                      << (args.burst_interleave ? 1 : 0) << ","
                      << (args.group > 0 ? args.group : 8) << ","
                      << args.seed << ","
                      << bc.chunks << ","
                      << bc.frames_total << ","
                      << bc.frames_recovered << ","
                      << bc.chunks_complete << ","
                      << bc.chunk_sync_fail << "\n";
            return 0;
        }

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
                  << args.channel_name << ","
                  << args.mod_name << ","
                  << args.rate_name << ","
                  << (args.carrier_interleave ? 1 : 0) << ","
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
