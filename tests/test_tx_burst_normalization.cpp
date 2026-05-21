#include "gui/modem/idle_noise_snr_estimator.hpp"
#include "gui/modem/modem_engine.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "ota_channel_core/channel.hpp"
#include "ota_channel_core/models.hpp"
#include "protocol/connection_policy.hpp"
#include "protocol/frame_v2.hpp"
#include "psk/multi_carrier_dpsk.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/tx_burst_normalization.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace {

namespace channel = ultra::ota_channel_core;
namespace gui = ultra::gui;
namespace protocol = ultra::protocol;
namespace sim = ultra::sim;
namespace v2 = ultra::protocol::v2;

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg) \
    do { \
        ++tests_run; \
        if (!(cond)) { \
            ++tests_failed; \
            std::cout << "FAIL: " << msg << "\n"; \
        } \
    } while (0)

struct PathSpec {
    std::string label;
    size_t expected_total_samples = 0;
    size_t expected_active_samples = 0;
    double expected_pre_delta_db = 0.0;
    std::function<std::vector<float>()> build;
};

struct PathResult {
    PathSpec spec;
    sim::TxBurstRmsMeasurement pre;
    sim::TxBurstRmsMeasurement normalization;
    sim::TxBurstRmsMeasurement post;
    size_t post_sample_count = 0;
    double pre_delta_db = 0.0;
    double post_delta_db = 0.0;
};

double dbPowerRatio(double numerator, double denominator) {
    if (numerator <= 0.0 || denominator <= 0.0) {
        return -std::numeric_limits<double>::infinity();
    }
    return 10.0 * std::log10(numerator / denominator);
}

double dbRmsRatio(double numerator, double denominator) {
    if (numerator <= 0.0 || denominator <= 0.0) {
        return -std::numeric_limits<double>::infinity();
    }
    return 20.0 * std::log10(numerator / denominator);
}

std::vector<uint8_t> payload(size_t n, uint32_t seed) {
    std::vector<uint8_t> out(n);
    uint32_t state = seed;
    for (auto& byte : out) {
        state = 1664525u * state + 1013904223u;
        byte = static_cast<uint8_t>((state >> 16) & 0xffu);
    }
    return out;
}

void configureConnectedEngine(gui::ModemEngine& engine,
                              ultra::CodeRate rate,
                              int cw_count) {
    engine.setWaveformMode(protocol::WaveformMode::OFDM_CHIRP);
    engine.setDataMode(ultra::Modulation::DQPSK, rate);
    engine.setConnected(true);
    engine.setHandshakeComplete(true);
    engine.setFixedFrameCodewords(cw_count);
}

std::vector<uint8_t> fixedDataFrame(ultra::CodeRate rate,
                                    int cw_count,
                                    uint16_t seq,
                                    uint32_t seed) {
    const size_t cap = v2::getFixedFramePayloadCapacity(rate, cw_count);
    auto frame = v2::makeFixedDataFrame("ALPHA", "BRAVO", seq,
                                        payload(cap, seed), rate, cw_count);
    return frame.serialize();
}

std::vector<float> connectedData(ultra::CodeRate rate, int cw_count) {
    gui::ModemEngine engine;
    configureConnectedEngine(engine, rate, cw_count);
    (void)engine.transmit(fixedDataFrame(rate, cw_count, 1, 0x1111u));
    return engine.transmit(fixedDataFrame(rate, cw_count, 2, 0x2222u));
}

std::vector<float> connectedAck(ultra::CodeRate session_rate, int cw_count) {
    gui::ModemEngine engine;
    configureConnectedEngine(engine, session_rate, cw_count);
    (void)engine.transmit(fixedDataFrame(session_rate, cw_count, 1, 0x3333u));
    const auto ack = v2::ControlFrame::makeAck("BRAVO", "ALPHA", 1);
    return engine.transmit(ack.serialize());
}

std::vector<float> modemPing() {
    gui::ModemEngine engine;
    return engine.transmitPing();
}

std::vector<float> connectFrame() {
    gui::ModemEngine engine;
    const auto frame = v2::ConnectFrame::makeConnect(
        "ALPHA", "BRAVO",
        protocol::ModeCapabilities::ALL | protocol::ModeCapabilities::PHY_MASK_V1,
        static_cast<uint8_t>(protocol::WaveformMode::AUTO),
        static_cast<uint8_t>(ultra::Modulation::AUTO),
        static_cast<uint8_t>(ultra::CodeRate::AUTO),
        0);
    return engine.transmit(frame.serialize());
}

std::vector<float> connectAckFrame() {
    gui::ModemEngine engine;
    const auto frame = v2::ConnectFrame::makeConnectAck(
        "BRAVO", "ALPHA",
        static_cast<uint8_t>(protocol::WaveformMode::OFDM_CHIRP),
        ultra::Modulation::DQPSK,
        ultra::CodeRate::R1_4,
        15.0f,
        0.05f,
        static_cast<uint8_t>(v2::kDefaultFixedFrameCodewords),
        protocol::LadderRungId::OFDM_CHIRP);
    return engine.transmit(frame.serialize());
}

std::vector<PathSpec> operationalPaths() {
    constexpr auto ofdm = protocol::WaveformMode::OFDM_CHIRP;
    const int r14_cw =
        protocol::connection_policy::recommendCWCount(ultra::CodeRate::R1_4, ofdm);
    const int r12_cw =
        protocol::connection_policy::recommendCWCount(ultra::CodeRate::R1_2, ofdm);

    return {
        {"PING-limited handshake", 76416, 66815, -2.052, modemPing},
        {"OFDM data R1/4 light cw4", 39840, 30240, -3.352,
         [=] { return connectedData(ultra::CodeRate::R1_4, r14_cw); }},
        {"OFDM data R1/2 light cw8", 66720, 57120, -5.187,
         [=] { return connectedData(ultra::CodeRate::R1_2, r12_cw); }},
        {"OFDM ACK light R1/4 hardened", 19680, 10080, -5.097,
         [=] { return connectedAck(ultra::CodeRate::R1_2, r12_cw); }},
        {"CONNECT MC-DPSK control", 325248, 315647, -3.484, connectFrame},
        {"CONNECT_ACK MC-DPSK control", 325248, 315647, -3.494, connectAckFrame},
    };
}

PathResult measurePath(const PathSpec& spec) {
    std::vector<float> samples = spec.build();

    PathResult result;
    result.spec = spec;
    result.pre = sim::measureTxBurstInBandRms(samples);
    result.pre_delta_db = dbRmsRatio(result.pre.in_band_rms,
                                    channel::kModemReferenceInBandRms);

    const size_t original_count = samples.size();
    result.normalization = sim::normalizeTxBurstToReference(samples);
    result.post_sample_count = samples.size();
    result.post = sim::measureTxBurstInBandRms(samples);
    result.post_delta_db = dbRmsRatio(result.post.in_band_rms,
                                     channel::kModemReferenceInBandRms);
    CHECK(result.post_sample_count == original_count,
          spec.label + ": normalization should not change sample count");
    return result;
}

struct FilteredPower {
    double sum_sq = 0.0;
    size_t count = 0;

    void add(float sample) {
        sum_sq += static_cast<double>(sample) * static_cast<double>(sample);
        ++count;
    }

    double power() const {
        return count > 0 ? sum_sq / static_cast<double>(count) : 0.0;
    }
};

std::vector<float> referenceFilter(std::span<const float> samples) {
    const auto& coeffs = sim::referenceBandFirCoefficients();
    std::vector<float> delay_line(coeffs.size(), 0.0f);
    size_t delay_idx = 0;
    std::vector<float> out(samples.size(), 0.0f);
    for (size_t n = 0; n < samples.size(); ++n) {
        delay_line[delay_idx] = samples[n];
        float y = 0.0f;
        size_t j = delay_idx;
        for (size_t i = 0; i < coeffs.size(); ++i) {
            y += coeffs[i] * delay_line[j];
            if (j == 0) {
                j = coeffs.size();
            }
            --j;
        }
        delay_idx = (delay_idx + 1) % coeffs.size();
        out[n] = y;
    }
    return out;
}

struct DeliveredSnrResult {
    std::string path;
    float dialed_snr_db = 0.0f;
    double measured_snr_db = 0.0;
    double delta_db = 0.0;
    double signal_window_power = 0.0;
    double noise_window_power = 0.0;
    size_t noise_samples = 0;
};

DeliveredSnrResult measureDeliveredSnr(const PathSpec& spec,
                                       float snr_db,
                                       bool normalize,
                                       uint32_t seed = 0x5151u) {
    constexpr size_t kPrefixSamples = 48000;
    constexpr size_t kSuffixSamples = 48000;
    constexpr size_t kTailGuardSamples = 4800;

    std::vector<float> burst = spec.build();
    if (normalize) {
        (void)sim::normalizeTxBurstToReference(burst);
    }
    const auto active = sim::measureTxBurstInBandRms(burst);

    std::vector<float> tx(kPrefixSamples + burst.size() + kSuffixSamples, 0.0f);
    std::copy(burst.begin(), burst.end(),
              tx.begin() + static_cast<std::ptrdiff_t>(kPrefixSamples));

    channel::AWGNChannelModel model(snr_db, seed);
    const std::vector<float> rx = model.process(tx);
    const std::vector<float> tx_band = referenceFilter(tx);
    const std::vector<float> rx_band = referenceFilter(rx);

    const size_t signal_begin = kPrefixSamples + active.active_begin;
    const size_t signal_end = kPrefixSamples + active.active_end;
    const size_t suffix_noise_begin =
        std::min(rx.size(), signal_end + kTailGuardSamples);

    FilteredPower signal;
    for (size_t i = signal_begin; i < signal_end && i < tx_band.size(); ++i) {
        signal.add(tx_band[i]);
    }

    FilteredPower noise;
    for (size_t i = 0; i < std::min(signal_begin, rx_band.size()); ++i) {
        noise.add(rx_band[i]);
    }
    for (size_t i = suffix_noise_begin; i < rx_band.size(); ++i) {
        noise.add(rx_band[i]);
    }

    DeliveredSnrResult result;
    result.path = spec.label;
    result.dialed_snr_db = snr_db;
    result.signal_window_power = signal.power();
    result.noise_window_power = noise.power();
    result.noise_samples = noise.count;
    result.measured_snr_db =
        dbPowerRatio(result.signal_window_power, result.noise_window_power);
    result.delta_db = result.measured_snr_db - static_cast<double>(snr_db);
    return result;
}

struct IdleSnrResult {
    float dialed_snr_db = 0.0f;
    double estimator_snr_db = 0.0;
    double delta_db = 0.0;
};

IdleSnrResult measureIdleSnr(float snr_db) {
    constexpr size_t kIdleSamples = 48000 * 4;
    const std::vector<uint32_t> seeds = {1u, 2u, 3u, 4u, 5u};
    double sum = 0.0;
    int count = 0;
    for (uint32_t seed : seeds) {
        channel::AWGNChannelModel model(snr_db, seed);
        const std::vector<float> rx =
            model.process(std::vector<float>(kIdleSamples, 0.0f));
        gui::IdleNoiseSNREstimator estimator;
        estimator.observeIdleAudio(rx.data(), rx.size());
        const auto snapshot = estimator.snapshot();
        if (snapshot.valid && std::isfinite(snapshot.idle_in_band_snr_db)) {
            sum += snapshot.idle_in_band_snr_db;
            ++count;
        }
    }

    IdleSnrResult result;
    result.dialed_snr_db = snr_db;
    result.estimator_snr_db =
        count > 0 ? sum / static_cast<double>(count)
                  : std::numeric_limits<double>::quiet_NaN();
    result.delta_db = result.estimator_snr_db - snr_db;
    return result;
}

ultra::ModemConfig makeOfdmProbeConfig() {
    ultra::ModemConfig cfg;
    cfg.fft_size = 1024;
    cfg.num_carriers = 59;
    cfg.sample_rate = 48000;
    cfg.center_freq = 1500.0f;
    cfg.cp_mode = ultra::CyclicPrefixMode::MEDIUM;
    cfg.modulation = ultra::Modulation::DQPSK;
    cfg.code_rate = ultra::CodeRate::R1_4;
    cfg.use_pilots = true;
    cfg.pilot_spacing =
        ultra::ofdm_link_adaptation::recommendedPilotSpacing(
            cfg.modulation, cfg.code_rate);
    return cfg;
}

struct OfdmProbeFrame {
    std::vector<float> samples;
    size_t signal_start = 0;
    size_t frame_samples = 0;
};

OfdmProbeFrame buildOfdmProbeFrame(const ultra::ModemConfig& cfg) {
    ultra::OFDMChirpWaveform waveform(cfg);
    waveform.configure(cfg.modulation, cfg.code_rate);

    const auto frame =
        v2::DataFrame::makeData("ALPHA", "BRAVO", 1,
                                payload(32, 0x7777u), cfg.code_rate);
    const ultra::Bytes encoded =
        v2::encodeFixedFrame(frame.serialize(), cfg.code_rate);

    OfdmProbeFrame tx;
    tx.signal_start = 48000;
    tx.samples.resize(tx.signal_start, 0.0f);

    const ultra::Samples preamble = waveform.generateDataPreamble();
    const ultra::Samples data = waveform.modulate(encoded);
    tx.frame_samples = preamble.size() + data.size();
    tx.samples.insert(tx.samples.end(), preamble.begin(), preamble.end());
    tx.samples.insert(tx.samples.end(), data.begin(), data.end());
    tx.samples.resize(tx.samples.size() + 48000, 0.0f);
    return tx;
}

struct LtsSnrResult {
    float dialed_snr_db = 0.0f;
    double estimator_snr_db = 0.0;
    double delta_db = 0.0;
    int valid_estimates = 0;
};

LtsSnrResult measureLtsSnr(float snr_db) {
    const ultra::ModemConfig cfg = makeOfdmProbeConfig();
    const OfdmProbeFrame tx = buildOfdmProbeFrame(cfg);
    const std::vector<uint32_t> seeds = {1u, 2u, 3u, 4u, 5u};
    double sum = 0.0;
    int count = 0;

    for (uint32_t seed : seeds) {
        channel::SimulatedChannel sim_channel;
        sim_channel.setSeed(seed);
        sim_channel.configure(snr_db, channel::ChannelType::AWGN);
        sim_channel.transmitFromA(tx.samples);
        const std::vector<float> rx = sim_channel.receiveForB(tx.samples.size());

        ultra::OFDMChirpWaveform rx_waveform(cfg);
        rx_waveform.configure(cfg.modulation, cfg.code_rate);
        rx_waveform.setFrequencyOffset(0.0f);
        rx_waveform.setAbsoluteTrainingPosition(tx.signal_start);

        const bool ready = rx_waveform.process(
            ultra::SampleSpan(rx.data() + tx.signal_start, tx.frame_samples));
        if (ready && rx_waveform.hasLastOFDMBroadbandSNREstimate()) {
            const float estimate = rx_waveform.getLastOFDMBroadbandSNREstimate();
            if (std::isfinite(estimate)) {
                sum += estimate;
                ++count;
            }
        }
    }

    LtsSnrResult result;
    result.dialed_snr_db = snr_db;
    result.valid_estimates = count;
    result.estimator_snr_db =
        count > 0 ? sum / static_cast<double>(count)
                  : std::numeric_limits<double>::quiet_NaN();
    result.delta_db = result.estimator_snr_db - snr_db;
    return result;
}

void printPathTable(const std::vector<PathResult>& results) {
    std::cout << "\nProof1 calibration table\n";
    std::cout << "path,total,active,pre_rms,pre_delta_db,post_rms,"
                 "post_delta_db,gain,peak_after_gain,peak_warn,peak_clip,"
                 "peak_clip_samples,peak_clip_rate,fragment_warn\n";
    std::cout << std::fixed << std::setprecision(6);
    for (const auto& r : results) {
        const double clip_rate =
            r.post_sample_count > 0
                ? static_cast<double>(r.normalization.peak_clip_samples) /
                      static_cast<double>(r.post_sample_count)
                : 0.0;
        std::cout << r.spec.label << ","
                  << r.spec.expected_total_samples << ","
                  << r.pre.active_samples << ","
                  << r.pre.in_band_rms << ","
                  << r.pre_delta_db << ","
                  << r.post.in_band_rms << ","
                  << r.post_delta_db << ","
                  << r.normalization.gain_to_reference << ","
                  << r.normalization.peak_after_gain << ","
                  << (r.normalization.peak_warning ? "yes" : "no") << ","
                  << (r.normalization.peak_clip_error ? "yes" : "no") << ","
                  << r.normalization.peak_clip_samples << ","
                  << clip_rate << ","
                  << (r.normalization.burst_fragment_warning ? "yes" : "no")
                  << "\n";
    }
}

int runPathRegression(std::vector<PathResult>* out_results) {
    constexpr double kPreToleranceDb = 0.03;
    constexpr double kPostToleranceDb = 0.05;
    const auto paths = operationalPaths();
    std::vector<PathResult> results;
    results.reserve(paths.size());

    for (const auto& path : paths) {
        PathResult r = measurePath(path);
        CHECK(r.pre.active_samples == path.expected_active_samples,
              path.label + ": active sample count should match Round 1");
        CHECK(r.spec.expected_total_samples == r.post_sample_count,
              path.label + ": total sample count should match Round 1");
        CHECK(std::abs(r.pre_delta_db - path.expected_pre_delta_db) <=
                  kPreToleranceDb,
              path.label + ": pre-normalization drift should match Round 1");
        CHECK(std::abs(r.post_delta_db) <= kPostToleranceDb,
              path.label + ": post-normalization RMS should match reference");
        CHECK(!r.normalization.burst_fragment_warning,
              path.label + ": normalization should operate on a full burst");
        if (r.normalization.peak_warning || r.normalization.peak_clip_error) {
            std::cout << "WARN: " << path.label
                      << " peak_after_gain=" << r.normalization.peak_after_gain
                      << " clip_samples=" << r.normalization.peak_clip_samples
                      << " (informational headroom guard)\n";
        }
        results.push_back(r);
    }

    printPathTable(results);
    if (out_results) {
        *out_results = std::move(results);
    }
    return tests_failed == 0 ? 0 : 1;
}

void runBurstBoundaryInvariant(const std::vector<PathResult>& path_results) {
    std::cout << "\nProof1.5 burst boundary table\n";
    std::cout << "case,total_samples,active_samples,minimum_active_samples,"
                 "fragment_warning\n";
    for (const auto& r : path_results) {
        std::cout << r.spec.label << ","
                  << r.post_sample_count << ","
                  << r.normalization.active_samples << ","
                  << sim::kTxBurstMinimumActiveSamples << ","
                  << (r.normalization.burst_fragment_warning ? "yes" : "no")
                  << "\n";
        CHECK(!r.normalization.burst_fragment_warning,
              r.spec.label + ": full operational burst must not trip fragment guard");
    }

    const PathSpec& ofdm_path = path_results[1].spec;
    const std::vector<float> burst = ofdm_path.build();
    const auto full = sim::measureTxBurstInBandRms(burst);
    const size_t fragment_count =
        std::min<size_t>(480, full.active_samples);
    std::vector<float> fragment(
        burst.begin() + static_cast<std::ptrdiff_t>(full.active_begin),
        burst.begin() + static_cast<std::ptrdiff_t>(full.active_begin + fragment_count));
    const auto fragment_measurement =
        sim::normalizeTxBurstToReference(fragment);
    std::cout << "480-sample callback fragment,"
              << fragment.size() << ","
              << fragment_measurement.active_samples << ","
              << sim::kTxBurstMinimumActiveSamples << ","
              << (fragment_measurement.burst_fragment_warning ? "yes" : "no")
              << "\n";
    CHECK(fragment_measurement.burst_fragment_warning,
          "480-sample callback fragment must trip fragment guard");
}

void runProbeTables(const std::vector<PathResult>& path_results) {
    const PathSpec& snr_path = path_results[1].spec;
    const std::vector<float> snrs = {0.0f, 5.0f, 10.0f, 15.0f, 20.0f};

    std::cout << "\nProof2 delivered SNR table\n";
    std::cout << "path,dialed_snr,measured_snr,delta,signal_window_power,"
                 "noise_window_power,noise_samples\n";
    std::cout << std::fixed << std::setprecision(6);
    for (float snr : snrs) {
        const auto r = measureDeliveredSnr(snr_path, snr, true);
        std::cout << r.path << ","
                  << r.dialed_snr_db << ","
                  << r.measured_snr_db << ","
                  << r.delta_db << ","
                  << r.signal_window_power << ","
                  << r.noise_window_power << ","
                  << r.noise_samples << "\n";
        CHECK(std::abs(r.delta_db) <= 0.3,
              "delivered SNR should match dialed within 0.3 dB");
    }

    std::cout << "\nProof3 idle estimator table\n";
    std::cout << "dialed_snr,estimator_snr,delta\n";
    for (float snr : {5.0f, 10.0f, 15.0f, 20.0f}) {
        const auto r = measureIdleSnr(snr);
        std::cout << r.dialed_snr_db << ","
                  << r.estimator_snr_db << ","
                  << r.delta_db << "\n";
        CHECK(std::abs(r.delta_db) <= 0.5,
              "idle SNR estimator should match dialed within 0.5 dB");
    }

    std::cout << "\nProof4 OFDM LTS estimator table\n";
    std::cout << "dialed_snr,estimator_snr,delta,valid_estimates\n";
    for (float snr : {5.0f, 10.0f, 15.0f, 20.0f}) {
        const auto r = measureLtsSnr(snr);
        std::cout << r.dialed_snr_db << ","
                  << r.estimator_snr_db << ","
                  << r.delta_db << ","
                  << r.valid_estimates << "\n";
        if (snr >= 10.0f) {
            CHECK(std::abs(r.delta_db) <= 0.5,
                  "OFDM LTS estimator should match dialed within 0.5 dB at >=10 dB");
        }
    }

    std::cout << "\nProof6 revised delivered SNR before/after table\n";
    std::cout << "state,path,dialed_snr,measured_snr,delta,"
                 "signal_window_power,noise_window_power,noise_samples\n";
    const auto proof6_raw = measureDeliveredSnr(snr_path, 20.0f, false, 0x606u);
    const auto proof6_normalized =
        measureDeliveredSnr(snr_path, 20.0f, true, 0x606u);
    const auto print_proof6_row = [](const char* state,
                                     const DeliveredSnrResult& row) {
        std::cout << state << ","
                  << row.path << ","
                  << row.dialed_snr_db << ","
                  << row.measured_snr_db << ","
                  << row.delta_db << ","
                  << row.signal_window_power << ","
                  << row.noise_window_power << ","
                  << row.noise_samples << "\n";
    };
    print_proof6_row("raw_equivalent_pre_fix", proof6_raw);
    print_proof6_row("normalized_post_fix", proof6_normalized);
    CHECK(std::abs(proof6_raw.delta_db - snr_path.expected_pre_delta_db) <= 0.3,
          "revised Proof6 raw delivered SNR should match R1/4 pre-fix prediction");
    CHECK(std::abs(proof6_normalized.delta_db) <= 0.3,
          "revised Proof6 normalized delivered SNR should match dialed");

    std::cout << "\nProof7 predicted vs observed table\n";
    std::cout << "path,predicted_shift,observed_pre_fix_delta,"
                 "post_fix_delta,reconciliation_delta\n";
    for (const auto& path : path_results) {
        const auto raw = measureDeliveredSnr(path.spec, 20.0f, false, 0x777u);
        const auto normalized =
            measureDeliveredSnr(path.spec, 20.0f, true, 0x777u);
        const double reconcile = raw.delta_db - path.spec.expected_pre_delta_db;
        std::cout << path.spec.label << ","
                  << path.spec.expected_pre_delta_db << ","
                  << raw.delta_db << ","
                  << normalized.delta_db << ","
                  << reconcile << "\n";
        CHECK(std::abs(reconcile) <= 0.2,
              path.spec.label + ": predicted shift should match observed raw delivered delta");
        CHECK(std::abs(normalized.delta_db) <= 0.3,
              path.spec.label + ": normalized delivered SNR should match dialed");
    }
}

}  // namespace

int main(int argc, char** argv) {
    ultra::setLogLevel(ultra::LogLevel::ERROR);

    std::vector<PathResult> path_results;
    (void)runPathRegression(&path_results);
    if (!path_results.empty()) {
        runBurstBoundaryInvariant(path_results);
    }

    const bool proof_mode = argc > 1 && std::string(argv[1]) == "--proof";
    if (proof_mode && !path_results.empty()) {
        runProbeTables(path_results);
    }

    if (tests_failed == 0) {
        std::cout << "PASS: TxBurstNormalization (" << tests_run << " checks)\n";
        return 0;
    }

    std::cout << "FAIL: " << tests_failed << "/" << tests_run
              << " checks failed\n";
    return 1;
}
