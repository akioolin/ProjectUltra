#include "gui/modem/idle_noise_snr_estimator.hpp"
#include "protocol/frame_v2.hpp"
#include "sim/cli_enums.hpp"
#include "sim/channel_calibration.hpp"
#include "sim/simulated_station.hpp"
#include "ultra/fec.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using ultra::Bytes;
using ultra::CodeRate;
using ultra::ModemConfig;
using ultra::Modulation;
using ultra::OFDMChirpWaveform;
using ultra::SampleSpan;
using ultra::Samples;
namespace v2 = ultra::protocol::v2;

namespace {

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

const char* channelName(::ChannelType channel) {
    switch (channel) {
        case ::ChannelType::AWGN: return "AWGN";
        case ::ChannelType::GOOD: return "GOOD";
        case ::ChannelType::MODERATE: return "MODERATE";
        default: return "UNKNOWN";
    }
}

ModemConfig makeConfig() {
    ModemConfig cfg;
    cfg.fft_size = 1024;
    cfg.num_carriers = 59;
    cfg.sample_rate = 48000;
    cfg.center_freq = 1500;
    cfg.cp_mode = ultra::CyclicPrefixMode::MEDIUM;
    cfg.modulation = Modulation::DQPSK;
    cfg.code_rate = CodeRate::R1_2;
    cfg.use_pilots = true;
    cfg.pilot_spacing =
        ultra::ofdm_link_adaptation::recommendedPilotSpacing(cfg.modulation, cfg.code_rate);
    return cfg;
}

struct TxFrame {
    Samples samples;
    size_t signal_start = 0;
    size_t frame_samples = 0;
};

TxFrame buildTxFrame(const ModemConfig& cfg) {
    OFDMChirpWaveform waveform(cfg);
    waveform.configure(cfg.modulation, cfg.code_rate);

    Bytes payload(32);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xffu);
    }

    const auto frame = v2::DataFrame::makeData("ALPHA", "BRAVO", 1, payload, cfg.code_rate);
    const Bytes encoded = v2::encodeFixedFrame(frame.serialize(), cfg.code_rate);

    TxFrame tx;
    tx.signal_start = 48000;
    tx.samples.resize(tx.signal_start, 0.0f);

    Samples preamble = waveform.generateDataPreamble();
    Samples data = waveform.modulate(encoded);
    tx.frame_samples = preamble.size() + data.size();
    tx.samples.insert(tx.samples.end(), preamble.begin(), preamble.end());
    tx.samples.insert(tx.samples.end(), data.begin(), data.end());
    tx.samples.resize(tx.samples.size() + 48000, 0.0f);
    return tx;
}

struct EstimateResult {
    bool ready = false;
    bool valid = false;
    float snr_db = 0.0f;
};

EstimateResult measureSNR(const ModemConfig& cfg, const TxFrame& tx,
                          ::ChannelType channel, float snr_db, uint32_t seed) {
    ::SimulatedChannel sim;
    sim.setSeed(seed);
    sim.configure(snr_db, channel);
    sim.transmitFromA(tx.samples);
    const std::vector<float> rx = sim.receiveForB(tx.samples.size());

    EstimateResult result;
    if (tx.signal_start + tx.frame_samples > rx.size()) {
        return result;
    }

    OFDMChirpWaveform rx_waveform(cfg);
    rx_waveform.configure(cfg.modulation, cfg.code_rate);
    rx_waveform.setFrequencyOffset(0.0f);
    rx_waveform.setAbsoluteTrainingPosition(tx.signal_start);

    result.ready = rx_waveform.process(
        SampleSpan(rx.data() + tx.signal_start, tx.frame_samples));
    result.valid = rx_waveform.hasLastOFDMBroadbandSNREstimate();
    result.snr_db = rx_waveform.getLastOFDMBroadbandSNREstimate();
    return result;
}

EstimateResult measureIdleSNR(::ChannelType channel, float snr_db, uint32_t seed) {
    constexpr size_t kIdleSamples = 48000 * 4;

    ::SimulatedChannel sim;
    sim.setSeed(seed);
    sim.configure(snr_db, channel);

    std::vector<float> tx(kIdleSamples, 0.0f);
    sim.transmitFromA(tx);
    const std::vector<float> rx = sim.receiveForB(tx.size());

    ultra::gui::IdleNoiseSNREstimator estimator;
    estimator.observeIdleAudio(rx.data(), rx.size());
    const auto snapshot = estimator.snapshot();

    EstimateResult result;
    result.ready = true;
    result.valid = snapshot.valid;
    result.snr_db = snapshot.idle_in_band_snr_db;
    return result;
}

void checkChannel(::ChannelType channel, float tolerance_db) {
    const ModemConfig cfg = makeConfig();
    const TxFrame tx = buildTxFrame(cfg);
    const std::vector<float> snrs = {-5.0f, 0.0f, 5.0f, 10.0f, 15.0f, 20.0f};
    const std::vector<uint32_t> seeds = {1u, 2u, 3u, 4u, 5u};

    for (float snr_db : snrs) {
        double sum = 0.0;
        double idle_sum = 0.0;
        int count = 0;
        int idle_count = 0;
        for (uint32_t seed : seeds) {
            const EstimateResult r = measureSNR(cfg, tx, channel, snr_db, seed);
            CHECK(r.ready, std::string(channelName(channel)) + " probe should process");
            CHECK(r.valid, std::string(channelName(channel)) + " probe should produce SNR");
            CHECK(std::isfinite(r.snr_db),
                  std::string(channelName(channel)) + " SNR should be finite");
            if (r.ready && r.valid && std::isfinite(r.snr_db)) {
                sum += r.snr_db;
                ++count;
            }

            if (channel == ::ChannelType::AWGN) {
                const EstimateResult idle = measureIdleSNR(channel, snr_db, seed);
                CHECK(idle.valid, "AWGN idle estimator should produce SNR");
                CHECK(std::isfinite(idle.snr_db), "AWGN idle SNR should be finite");
                if (idle.valid && std::isfinite(idle.snr_db)) {
                    idle_sum += idle.snr_db;
                    ++idle_count;
                }
            }
        }

        CHECK(count == static_cast<int>(seeds.size()),
              std::string(channelName(channel)) + " should produce all seed estimates");
        if (count == 0) {
            continue;
        }

        const float measured = static_cast<float>(sum / static_cast<double>(count));
        const float expected = snr_db +
            static_cast<float>(ultra::sim::kModemBroadbandToInBandSnrOffsetDb);
        const float delta = measured - expected;
        std::cout << std::fixed << std::setprecision(2)
                  << channelName(channel)
                  << " configured=" << snr_db
                  << " expected_in_band=" << expected
                  << " measured=" << measured
                  << " delta=" << delta
                  << " tolerance=" << tolerance_db
                  << "\n";

        CHECK(std::abs(delta) <= tolerance_db,
              std::string(channelName(channel)) +
                  " modem SNR meter should match configured in-band SNR");

        if (channel == ::ChannelType::AWGN) {
            CHECK(idle_count == static_cast<int>(seeds.size()),
                  "AWGN should produce all idle seed estimates");
            if (idle_count > 0) {
                const float idle_measured =
                    static_cast<float>(idle_sum / static_cast<double>(idle_count));
                const float estimator_delta = measured - idle_measured;
                std::cout << "AWGN configured=" << snr_db
                          << " idle=" << idle_measured
                          << " ofdm=" << measured
                          << " estimator_delta=" << estimator_delta
                          << "\n";
                CHECK(std::abs(estimator_delta) <= 1.5f,
                      "AWGN idle and OFDM in-band SNR should agree within 1.5 dB");
            }
        }
    }
}

}  // namespace

int main() {
    ultra::setLogLevel(ultra::LogLevel::ERROR);

    checkChannel(::ChannelType::AWGN, 1.5f);
    checkChannel(::ChannelType::GOOD, 3.0f);
    checkChannel(::ChannelType::MODERATE, 3.0f);

    if (tests_failed == 0) {
        std::cout << "PASS: Modem SNR meter calibration (" << tests_run << " checks)\n";
        return 0;
    }
    std::cout << "FAIL: " << tests_failed << "/" << tests_run << " checks failed\n";
    return 1;
}
