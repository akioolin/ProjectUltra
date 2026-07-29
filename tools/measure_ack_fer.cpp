#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "ota_channel_core/channel.hpp"
#include "ofdm/genie_tx_capture.hpp"
#include "ofdm/genie_true_h.hpp"
#include "ofdm/pilot_pattern.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
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
    int frame_cw = static_cast<int>(v2::kDefaultFixedFrameCodewords);  // codewords per frame
    bool burst_interleave = true; // cross-frame deep interleave ON/OFF (the A/B)
    bool burst_descriptor = false; // §14.17: emit BURST_HEADER descriptor + mis-set
                                   // decoder group size to prove the descriptor fixes it
    // 2026-07-29 diag: perfect-CSI bound. Runs every frame TWICE -- once through a
    // shadow channel with the same seed at very high SNR (so its LTS estimate is
    // the true H), then through the real channel with that H injected. Both passes
    // are pumped by an identical, fixed sample budget so the two fade trajectories
    // stay in lockstep for the whole run. See src/ofdm/genie_true_h.hpp.
    bool genie_true_h = false;
    // Shadow-channel SNR. Setting this EQUAL to --snr is the oracle's own null
    // control: same seed + same SNR => bit-identical signal => both passes sync
    // identically => injection must be a no-op and reproduce baseline exactly.
    // Any deviation there is a defect in the oracle, not a channel finding.
    float genie_clean_snr_db = 60.0f;
    // 2026-07-29 Phase 0 payoff: measure the LTS channel estimate against
    // SIMULATOR TRUTH and report NMSE. Requires ULTRA_CHANNEL_DOPPLER_HZ=0 so the
    // taps are frozen and truth H is time-invariant -- otherwise the tap state
    // read after the frame is not the state the LTS saw.
    bool chest_nmse = false;
    int ldpc_z = 27;               // LDPC lifting size: 27 (N=648) or 81 (N=1944, burst
                                   // long-LDPC keystone). Sets the encoder member via
                                   // setLDPCLiftingZ so the codeword size AND the
                                   // BURST_HEADER descriptor agree; the RX learns Z=81
                                   // from the wire descriptor (BUG-HARNESS-002 Defect 3).
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
        } else if (key == "--frame-cw") {
            args.frame_cw = std::stoi(requireValue("--frame-cw"));
        } else if (key == "--burst-interleave") {
            args.burst_interleave = std::stoi(requireValue("--burst-interleave")) != 0;
        } else if (key == "--burst-descriptor") {
            args.burst_descriptor = std::stoi(requireValue("--burst-descriptor")) != 0;
        } else if (key == "--genie-true-h") {
            args.genie_true_h = std::stoi(requireValue("--genie-true-h")) != 0;
        } else if (key == "--chest-nmse") {
            args.chest_nmse = std::stoi(requireValue("--chest-nmse")) != 0;
        } else if (key == "--genie-clean-snr") {
            args.genie_clean_snr_db = std::stof(requireValue("--genie-clean-snr"));
        } else if (key == "--ldpc-z") {
            args.ldpc_z = std::stoi(requireValue("--ldpc-z"));
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
    // 2026-05-29: use the PRODUCTION adaptive pilot spacing (was hardcoded 10,
    // which under-piloted coherent high-order mods by ~2x vs production's 5/8 and
    // made 16QAM look structurally broken on fading — a harness-fidelity bug).
    cfg.pilot_spacing = ultra::ofdm_link_adaptation::recommendedPilotSpacing(mod, rate);
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
    encoder.setFixedFrameCodewords(args.frame_cw);
    encoder.setCarrierLdpcInterleaver(args.carrier_interleave);
    // Sets the member so the actual codeword size AND the BURST_HEADER descriptor's
    // announced lifting_z agree (the production API the connection layer uses
    // per-burst). Without this, Z=81 codewords get announced/decoded as Z=27 → 0%.
    encoder.setLDPCLiftingZ(static_cast<uint8_t>(args.ldpc_z == 81 ? 81 : 27));
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
    decoder.setFixedFrameCodewords(args.frame_cw);
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
                                            args.frame_cw);
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
                      bool expect_full_anchor,
                      // 2026-07-29 diag: when true, pump the FULL sample budget
                      // regardless of early success. The genie mode runs two
                      // channels with the same seed and they only stay in
                      // lockstep if both are advanced by identical sample counts;
                      // the normal early-break would desynchronize their fades.
                      bool fixed_budget = false) {
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

        if (!fixed_budget && outcome.saw_result && outcome.result.success) {
            break;
        }
        received += take;
    }

    for (int i = 0;
         i < 8 && (fixed_budget || !(outcome.saw_result && outcome.result.success));
         ++i) {
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


// === Estimator NMSE against simulator truth -- NOT YET TRUSTWORTHY ==========
//
// STATUS 2026-07-29: THE TOOL IS WRONG. Do not quote its NMSE. Proved by its own
// identity-channel control, which is the first thing any future work here must run:
//
//   ULTRA_CHANNEL_DELAY_MS=0 ULTRA_CHANNEL_DOPPLER_HZ=0 measure_ack_fer \
//       --snr 20 --config data4_full --channel good --mod qam16 --rate r2_3 \
//       --seed 7 --n 20 --chest-nmse 1
//
// With both overrides the Watterson taps collapse to 1 and multipath is disabled, so
// the channel is LITERALLY IDENTITY -- truth H is the constant 1 at every carrier,
// confirmed by the dump (|truth| = 1 exactly). Decode is 20/20 PERFECT. A receiver
// cannot decode 16QAM R2/3 flawlessly on an identity channel with a bad channel
// estimate, so the estimate is good by construction. This comparison nevertheless
// reports NMSE 4.40 (+6.4 dB). Therefore the fault is in THIS CODE, not the
// estimator and not the channel.
//
// What the dump shows on that identity channel: |est| is flat to about +/-15%
// (consistent with estimation noise), but arg(est/truth) scatters by tens of degrees
// in a pattern no single delay reproduces. Since a delay is the only mechanism that
// produces smooth phase progression across carriers, the residual is NOT a window
// offset -- and note the delay search already scans a full alias period, so it would
// have found one. Plumbing the receiver's absolute FFT-window index out (the
// remaining Phase 0 item) would therefore NOT fix this.
//
// Live candidates, none yet eliminated: the frequency smoothing applied to
// channel_estimate immediately before the capture site (channel_equalizer_lts.cpp,
// "LTS H freq-smooth"); whether the stored H is in the flat/de-sloped domain rather
// than the raw Y/X domain; and whether pilot-bearing carriers are stored under a
// different convention from data carriers.
//
// The identity-channel control is the cheap gate: get it to report NMSE ~ 1/SNR
// (about 0.01 at 20 dB) BEFORE trusting any reading on a real channel.
//
// WHAT IT DOES ESTABLISH (both real, both reproducible with --chest-nmse 1 and
// ULTRA_CHEST_NMSE_DUMP=1, on a static two-path channel via ULTRA_CHANNEL_DOPPLER_HZ=0):
//   * MAGNITUDE tracks truth to about +/-12% across the whole 140-2906 Hz band.
//   * The estimate CANNOT follow truth into a deep null -- at f=984 Hz truth is
//     0.0347 while the scaled estimate sits near 0.20. That is correct behaviour
//     for a noise-limited estimator, not a defect.
//
// THE OBSTRUCTION (proved, not suspected). The receiver's FFT-window offset is
// UNOBSERVABLE MODULO fft_size from the carrier grid. A delay of exactly N samples
// is a circular shift, so exp(-j*2*pi*k*N/N) = 1 for every integer bin k. Measured
// with ULTRA_CHEST_DELAY_SCAN=1: the residual is exactly periodic in 1024 samples --
// identical to six significant figures at d = -1920, -896, 128, 1152, 2176 (all
// 7.49705) and at d = -2048, -1024, 0, 1024, 2048 (all 24.1742). The documented RX
// sync offset is up to ~0.7 symbol (~796 samples, equalize.cpp:462-465), i.e. inside
// one alias period and NOT recoverable by search.
//
// Even at the best in-period delay the residual stays near 3.0 (the value for
// roughly uncorrelated phase), so a SECOND misalignment remains unisolated --
// candidates: the frequency-smoothing applied to channel_estimate just before the
// capture site, and whether the stored H is in the flat/de-sloped domain.
//
// TO FINISH: export the receiver's actual absolute FFT-window sample index (and the
// smoothing state) alongside the captured estimate instead of inferring the ramp.
// The symbol-timing formula is already written down in
// src/ofdm/symbol_timing_contract.hpp; what is missing is plumbing the live value out.
//
// === Phase 0 payoff: estimator NMSE against simulator truth =================
//
// THE question the whole estimator thread turns on: how far is our channel
// estimate from the true channel? Two oracles tried to answer it indirectly and
// both were invalid. This measures it directly.
//
// DOMAIN CONTRACT (pinned by tests/test_channel_truth_h.cpp): remove a global
// complex scale and a linear phase ramp, and NOTHING more. Those two are what an
// OFDM receiver absorbs (phase reference, FFT-window timing). Removing less
// reports a wrong answer; removing more hides real estimator error.
struct ChestNmseAccum {
    double sum_nmse = 0.0;
    double sum_delay = 0.0;
    long frames = 0;
};

const ultra::ota_channel_core::WattersonChannel* wattersonOf(
    const ultra::ota_channel_core::SimulatedChannel& channel) {
    const auto* model = channel.modelAToBForDiagnostics();
    const auto* watterson =
        dynamic_cast<const ultra::ota_channel_core::WattersonChannelModel*>(model);
    return watterson ? &watterson->channelForDiagnostics() : nullptr;
}

void accumulateChestNmse(const ultra::ota_channel_core::SimulatedChannel& channel,
                         const ultra::ModemConfig& cfg,
                         ChestNmseAccum& acc) {
    namespace gtrue = ultra::ofdm::genie_true_h;
    const auto& est = gtrue::buffer();
    if (est.empty()) {
        return;
    }
    const auto* w = wattersonOf(channel);
    if (w == nullptr) {
        return;  // AWGN path is not a Watterson model; no taps to read.
    }

    std::vector<int> all, data, pilot;
    std::vector<size_t> dlog, plog;
    std::vector<bool> ispilot;
    std::vector<ultra::Complex> pseq;
    ultra::ofdm_pilots::buildCarrierPattern(cfg, 0, all, data, pilot, dlog, plog,
                                            ispilot, pseq);

    std::vector<double> freqs;
    std::vector<std::complex<double>> e, t;
    for (int idx : all) {
        if (static_cast<size_t>(idx) >= est.size()) {
            return;  // geometry mismatch; refuse to report a number
        }
        const float f = ultra::ofdm_pilots::carrierFrequencyHz(cfg, idx);
        const auto th = w->trueFrequencyResponse(f);
        if (std::abs(th) < 1e-9f || std::abs(est[idx]) < 1e-12f) {
            continue;
        }
        freqs.push_back(static_cast<double>(f));
        e.emplace_back(est[idx].real(), est[idx].imag());
        t.emplace_back(th.real(), th.imag());
    }
    if (freqs.size() < 8) {
        return;
    }
    if (acc.frames == 0 && std::getenv("ULTRA_CHEST_NMSE_DUMP")) {
        std::cerr << "# chest dump: est.size=" << est.size()
                  << " cfg.fft=" << cfg.fft_size << " Nc=" << cfg.num_carriers
                  << " carriers_used=" << freqs.size() << "\n";
        // Unoccupied bins first: if these hold data too, the array is not what we
        // think it is. DC(0) is the skipped centre; 100/500/900 are outside the
        // occupied sets (1..30, 995..1023).
        for (int probe : {0, 100, 500, 900}) {
            std::cerr << "#   UNOCCUPIED bin " << probe << " |est|="
                      << std::abs(est[probe]) << "\n";
        }
        // ADJACENT carriers -- sampling every 6th aliases a phase ramp into
        // apparent scatter, which is how this looked like non-ramp noise.
        for (size_t i = 0; i < freqs.size() && i < 12; ++i) {
            std::cerr << "#   f=" << freqs[i]
                      << "  |est|=" << std::abs(e[i])
                      << "  |truth|=" << std::abs(t[i])
                      << "  arg(est/truth)=" 
                      << std::arg(e[i] / t[i]) * 180.0 / M_PI << " deg\n";
        }
    }

    // Remove the linear phase ramp by SEARCHING over candidate delays.
    //
    // An unwrap-based fit CANNOT work here and that is not a tuning detail: the RX
    // sync convention sits up to ~0.7 symbol off the TX grid (measured +796 samples
    // on ITU Good, channel_equalizer_equalize.cpp:462-465). At 46.875 Hz carrier
    // spacing, 800 samples is 2*pi*800/1024 = 4.9 rad of phase step PER CARRIER --
    // more than a full turn -- so the ramp aliases and no unwrapping can recover it.
    // Measured that way this returned NMSE +15.1 dB (estimate and truth unrelated).
    // A search over delay is immune to the aliasing.
    auto residualForDelay = [&](double d_samples, std::complex<double>* alpha_out) {
        std::complex<double> snum(0, 0);
        double sden = 0.0;
        std::vector<std::complex<double>> aligned(freqs.size());
        for (size_t i = 0; i < freqs.size(); ++i) {
            const double a = -2.0 * M_PI * freqs[i] * d_samples / 48000.0;
            aligned[i] = e[i] * std::complex<double>(std::cos(-a), std::sin(-a));
            snum += std::conj(t[i]) * aligned[i];
            sden += std::norm(t[i]);
        }
        if (sden <= 0.0) {
            return 1e30;
        }
        const std::complex<double> alpha = snum / sden;
        if (std::abs(alpha) < 1e-12) {
            return 1e30;
        }
        double num = 0.0, dnm = 0.0;
        for (size_t i = 0; i < freqs.size(); ++i) {
            const std::complex<double> scaled = aligned[i] / alpha;
            num += std::norm(scaled - t[i]);
            dnm += std::norm(t[i]);
        }
        if (alpha_out) {
            *alpha_out = alpha;
        }
        return num / dnm;
    };

    // Coarse sweep over +/- one symbol (fft + CP), then refine.
    double best_d = 0.0;
    double best_r = 1e30;
    if (std::getenv("ULTRA_CHEST_DELAY_SCAN")) {
        std::cerr << "# delay scan (residual vs candidate delay):\n";
        for (double d = -2560.0; d <= 2560.0; d += 64.0) {
            std::cerr << "#   d=" << d << "  resid=" << residualForDelay(d, nullptr)
                      << "\n";
        }
    }
    for (double d = -1200.0; d <= 1200.0; d += 0.5) {
        const double r = residualForDelay(d, nullptr);
        if (r < best_r) {
            best_r = r;
            best_d = d;
        }
    }
    for (double d = best_d - 1.0; d <= best_d + 1.0; d += 0.02) {
        const double r = residualForDelay(d, nullptr);
        if (r < best_r) {
            best_r = r;
            best_d = d;
        }
    }

    acc.sum_nmse += best_r;
    acc.sum_delay += best_d;
    ++acc.frames;
}

Counts measure(const Args& args) {
    gui::StreamingEncoder encoder;
    configureEncoder(encoder, args);

    gui::StreamingDecoder decoder;
    configureDecoder(decoder, args);

    ultra::ota_channel_core::SimulatedChannel channel;
    channel.setSeed(args.seed);
    channel.configure(args.snr_db, args.channel);

    // 2026-07-29 diag: shadow channel + decoder for the perfect-CSI bound. Same
    // seed and channel type => same fade realization; kGenieCleanSnrDb is high
    // enough that the LTS estimate taken from it is the true H to well below any
    // level that could matter. Noise is left ENABLED rather than switched off so
    // the model draws the same number of RNG samples per call as the real
    // channel, which is what keeps the two fade trajectories aligned.
    std::unique_ptr<gui::StreamingDecoder> clean_decoder;
    std::unique_ptr<ultra::ota_channel_core::SimulatedChannel> clean_channel;
    if (args.genie_true_h) {
        clean_decoder = std::make_unique<gui::StreamingDecoder>();
        configureDecoder(*clean_decoder, args);
        clean_channel = std::make_unique<ultra::ota_channel_core::SimulatedChannel>();
        clean_channel->setSeed(args.seed);
        clean_channel->configure(args.genie_clean_snr_db, args.channel);
    }

    std::mt19937_64 payload_rng(args.seed ^ 0xA6E22C15D9B3F1A5ull);
    Counts counts;
    ChestNmseAccum chest_acc;

    for (int i = 0; i < args.n; ++i) {
        if (ultra::genie::txCapture().enabled) {
            ultra::genie::txCapture().reportIfDebug("prev frame");
            ultra::genie::txCapture().reset();
        }
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

        // 2026-07-29 diag: clean pass first -- capture the noiseless H over this
        // frame's fade realization -- then the real pass with that H injected.
        if (args.genie_true_h) {
            // NB: alias must not be named `genie` -- ultra::genie already exists
            // (genie_tx_capture.hpp) and would shadow-collide here.
            namespace gtrue = ultra::ofdm::genie_true_h;
            gtrue::buffer().clear();
            gtrue::mode() = gtrue::Mode::Capture;
            runFrame(*clean_decoder, *clean_channel, trial.tx_samples, full_preamble,
                     /*reset_decoder=*/true, /*expect_full_anchor=*/expect_anchor,
                     /*fixed_budget=*/true);
            gtrue::mode() = gtrue::Mode::Inject;
            auto outcome = runFrame(decoder, channel, trial.tx_samples, full_preamble,
                                    /*reset_decoder=*/true,
                                    /*expect_full_anchor=*/expect_anchor,
                                    /*fixed_budget=*/true);
            gtrue::mode() = gtrue::Mode::Off;
            classify(outcome, trial.frame_bytes, counts);
            continue;
        }

        if (args.chest_nmse) {
            namespace gtrue = ultra::ofdm::genie_true_h;
            gtrue::buffer().clear();
            {   // Accept only the pass matching the TRANSMITTED profile. Without this
                // the control-first QPSK R1/4 peek is captured instead of the data pass.
                const auto want = makeOFDMConfig(args.mod, args.rate);
                auto& x = gtrue::expect();
                x.active = true;
                x.pilot_spacing = static_cast<uint32_t>(want.pilot_spacing);
                x.modulation = static_cast<int>(want.modulation);
                x.num_carriers = static_cast<uint32_t>(want.num_carriers);
                x.fft_size = static_cast<uint32_t>(want.fft_size);
            }
            gtrue::mode() = gtrue::Mode::Capture;   // capture only; never inject
            auto outcome = runFrame(decoder, channel, trial.tx_samples, full_preamble,
                                    /*reset_decoder=*/true,
                                    /*expect_full_anchor=*/expect_anchor);
            gtrue::mode() = gtrue::Mode::Off;
            accumulateChestNmse(channel, makeOFDMConfig(args.mod, args.rate), chest_acc);
            classify(outcome, trial.frame_bytes, counts);
            continue;
        }

        auto outcome = runFrame(decoder, channel, trial.tx_samples, full_preamble,
                                /*reset_decoder=*/true,
                                /*expect_full_anchor=*/expect_anchor);
        classify(outcome, trial.frame_bytes, counts);
    }

    // 2026-07-29 diag: null control. If captures or injections is 0 the oracle
    // never engaged and any "no effect" reading is meaningless.
    if (args.chest_nmse) {
        const double mean = chest_acc.frames
            ? chest_acc.sum_nmse / static_cast<double>(chest_acc.frames) : -1.0;
        std::cerr << "# chest_nmse rejected_profile_captures="
                  << ultra::ofdm::genie_true_h::stats().rejected_profile
                  << " frames=" << chest_acc.frames
                  << " mean_nmse=" << mean
                  << " mean_nmse_db=" << (mean > 0 ? 10.0 * std::log10(mean) : 0.0)
                  << " mean_fitted_delay_samples="
                  << (chest_acc.frames
                          ? chest_acc.sum_delay / static_cast<double>(chest_acc.frames)
                          : 0.0)
                  << "\n";
    }
    if (args.genie_true_h) {
        const auto& s = ultra::ofdm::genie_true_h::stats();
        std::cerr << "# genie_true_h: captures=" << s.captures
                  << " injections=" << s.injections
                  << " misses=" << s.misses
                  << " mean_nmse_vs_replaced="
                  << (s.nmse_count ? s.nmse_sum / static_cast<double>(s.nmse_count) : -1.0)
                  << "\n";
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
        // Fresh capture per chunk: encoder pushes during encode, decoder reads in the
        // same data-symbol order during decode.
        if (ultra::genie::txCapture().enabled) {
            ultra::genie::txCapture().reportIfDebug("prev chunk");
            ultra::genie::txCapture().reset();
        }
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
        // ULTRA_MEASURE_BURST_NO_ANCHOR_WAIT=1 (default OFF, diagnostic): skip the
        // full-anchor demand so the LIGHT-LTS marked group start (data-sync corr
        // ~0.26, below the 0.52 full-anchor threshold) is accepted and burst
        // accumulation can arm on the production warm-handoff preamble.
        {
            const char* e = std::getenv("ULTRA_MEASURE_BURST_NO_ANCHOR_WAIT");
            if (!(e != nullptr && e[0] == '1')) {
                decoder.expectFullOFDMAnchorOnce();
            }
        }

        // §14.17 self-describing burst: when enabled, the encoder emits a full-anchor
        // BURST_HEADER descriptor declaring the group params. To prove the descriptor
        // actually drives the decoder (the cross-station 0/8 fix), deliberately
        // mis-configure the decoder here: wrong group size + interleave OFF. If the
        // descriptor works, the RX reconfigures itself and still recovers the group.
        // The BURST_HEADER descriptor is REQUIRED for Z=81: the RX learns the lifting
        // size only from the wire descriptor (decodeFixedFrame ldpc_z <- last_burst_
        // descriptor_.lifting_z). With Z=81 and no descriptor the RX decodes N=1944
        // soft bits with a Z=27 matrix → 0% (BUG-HARNESS-002 Defect 3). So force the
        // descriptor on whenever Z=81 (production always sends it for burst), and also
        // when the §14.17 descriptor-proof test is requested.
        const bool emit_descriptor = args.burst_descriptor || args.ldpc_z == 81;
        if (emit_descriptor) {
            encoder.setBurstDescriptorEnabled(true);
            encoder.setBurstDescriptorIdentity("ALPHA", "BRAVO");
        }
        if (args.burst_descriptor) {
            // §14.17 proof: deliberately mis-config the RX; if the descriptor works
            // the RX reconfigures itself and still recovers the group.
            const int wrong_group = (group >= 4) ? 2 : 8;
            decoder.setBurstInterleaveGroupSize(wrong_group);
            decoder.setBurstInterleave(false);
        }

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
                                                args.frame_cw);
            Bytes ser = frame.serialize();
            frames.push_back(ser);
            expected.push_back(std::move(ser));
        }

        // HARNESS FIDELITY (2026-07-29): FULL chirp+LTS anchor at the group start.
        //
        // Without this the harness could never exercise the burst-transport RX path
        // at all. encodeBurstLight's default group-start preamble is the §16.8
        // warm-handoff LIGHT LTS, whose data-sync correlation reads ~0.26 — below the
        // 0.52 full-anchor threshold that decoder.expectFullOFDMAnchorOnce() (set
        // above, per chunk) enforces. So the marked group-start frame was REJECTED
        // three times and the decoder instead accepted a LATER, unmarked member frame
        // (corr 0.95). wasBurstInterleaved() is therefore false at the burst_marker
        // test (streaming_ofdm_decode.cpp:1568), "entering accumulation" never fires,
        // and every frame is decoded as an isolated frame — measured: 0 "Burst frame
        // N/M demodulated" lines in a full DEBUG trace of a burst_chunk run.
        //
        // Production does not have this problem: it forces the full anchor on the
        // session's first burst (forceNextFrameFullPreamble on entering connected
        // OFDM) and on every resend. Each chunk here builds a FRESH encoder, i.e. a
        // fresh session, so the production-equivalent latch must be armed per chunk.
        // Uses the group-start-specific latch, which the BURST_HEADER descriptor's
        // encodeFrame cannot consume.
        //
        // Env-gated (ULTRA_MEASURE_BURST_FULL_ANCHOR=1, default OFF) so the historical
        // burst_chunk numbers stay reproducible and the two regimes can be A/B'd.
        {
            const char* e = std::getenv("ULTRA_MEASURE_BURST_FULL_ANCHOR");
            if (e != nullptr && e[0] == '1') {
                encoder.forceNextBurstGroupStartFullPreamble();
            }
        }

        const auto tx = encoder.encodeBurstLight(frames);
        if (tx.empty()) {
            throw std::runtime_error("burst encoder produced no samples");
        }
        channel.transmitFromA(tx);

        std::vector<Bytes> recovered;
        bool any_sync = false;

        // HARNESS FIDELITY (2026-07-29): COLLECT THE BURST-TRANSPORT DELIVERY.
        //
        // finalizeBurstGroup routes decoded frames two different ways
        // (streaming_burst_interleave.cpp:1244): with burst_transport_rx_ set — the
        // production default, and the ONLY regime in which the burst-transport RX
        // path runs at all — successful logical frames are collected into
        // burst_group_frames and delivered SOLELY through burst_group_callback_; the
        // per-frame frame_queue_ push is deliberately suppressed so the file group is
        // not double-processed through onRxData. This harness never set that callback,
        // so every burst-delivered frame was dropped on the floor and
        // frames_recovered read 0 no matter how well the group decoded. Measured:
        // AWGN@60 16QAM R2/3, --burst-descriptor 1 → 60 "decode SUCCESS" / 0 "decode
        // FAILED" in the DEBUG trace, and frames_recovered = 0/32. The decode was
        // perfect; the harness was not listening.
        decoder.setBurstGroupCallback(
            [&recovered](uint16_t /*group_seq*/, const std::vector<Bytes>& group_frames,
                         bool /*all_ok*/, float /*quality*/, uint16_t /*frame_mask*/,
                         bool /*interleaved*/, uint8_t /*group_size*/) {
                for (const auto& f : group_frames) {
                    recovered.push_back(f);
                }
            });

        // HARNESS FIDELITY (2026-07-28): two floors on how long we pump.
        //
        // (1) kFullSearchSamples. The full-anchor chirp search refuses to run until it
        //     has 2.5 s of unsearched audio ("searchForSync: SKIP unsearched<min"), so a
        //     short burst (e.g. --frame-cw 1, 121344 samples) never got searched at all
        //     and reported chunk_sync_fail on EVERY chunk. Measured 5/5 -> 0/5.
        //
        // (2) The trailing allowance. The burst decoder re-decodes each frame several
        //     times (control-first peek, header peek, authoritative pass, sync-recovery
        //     retries) and runs behind live, so it still emits frames well after the
        //     last transmitted sample. The old 8 chunks (0.8 s) truncated that backlog
        //     and UNDER-REPORTED recovery. Measured, QPSK R3/4 Good@60 seed 7 n=10:
        //     8 -> 60/80, 16 -> 70/80, and 32/64/128/256 -> 70/80 (saturated). 16 it is
        //     — a measurement floor, not a tuning knob.
        constexpr size_t kTrailingPumpChunks = 16;
        const size_t target = std::max(tx.size() + kTrailingPumpChunks * kPumpChunkSamples,
                                       kFullSearchSamples + kTrailingPumpChunks * kPumpChunkSamples);
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
        ultra::LogLevel log_level = ultra::LogLevel::ERROR;
        if (const char* env = std::getenv("ULTRA_LOG_LEVEL")) {
            ultra::parseLogLevel(env, log_level);
        }
        ultra::setLogLevel(log_level);
        const Args args = parseArgs(argc, argv);

        // 2026-05-29 (BUG-HARNESS-002 Defect 3 fix): the decoder learns the LDPC
        // lifting size ONLY from the env ULTRA_LDPC_Z or the burst descriptor; the
        // descriptor-consume path does not fire reliably in this harness, so for a
        // controlled Z=81 (N=1944 long-LDPC burst keystone) screen we set the env
        // here (decoder block-size getter + decodeFixedFrame ldpc_z both read it) to
        // match the encoder member set via setLDPCLiftingZ in configureEncoder. This
        // mirrors production, where the connection layer fixes Z on BOTH ends.
        if (args.ldpc_z == 81) {
#ifdef _WIN32
            _putenv_s("ULTRA_LDPC_Z", "81");
#else
            setenv("ULTRA_LDPC_Z", "81", /*overwrite=*/1);
#endif
        }

        // 2026-05-29 diag: enable the true per-symbol data-aided channel genie
        // (H=Y/X) process-wide. Off unless ULTRA_GENIE_DATA_AIDED=1. Both the
        // frame path (measure) and burst path (measureBurst) reset per iteration.
        {
            const char* env = std::getenv("ULTRA_GENIE_DATA_AIDED");
            ultra::genie::txCapture().enabled = (env && std::atoi(env) == 1);
        }

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
