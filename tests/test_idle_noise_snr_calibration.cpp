#include "gui/modem/idle_noise_snr_estimator.hpp"
#include "sim/cli_enums.hpp"
#include "sim/simulated_station.hpp"
#include "ultra/logging.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

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

struct EstimateResult {
    bool valid = false;
    float snr_db = 0.0f;
    float latest_snr_db = 0.0f;
    float normalized_noise_rms = 0.0f;
    double fir_energy = 0.0;
    double enbw_hz = 0.0;
    size_t windows = 0;
};

EstimateResult measureIdleSNR(::ChannelType channel, float snr_db, uint32_t seed) {
    constexpr size_t kIdleSamples = 48000 * 4;

    ::SimulatedChannel sim;
    sim.setSeed(seed);
    sim.configure(snr_db, channel);

    std::vector<float> tx(kIdleSamples, 0.0f);
    sim.transmitFromA(tx);
    std::vector<float> rx = sim.receiveForB(kIdleSamples);

    ultra::gui::IdleNoiseSNREstimator estimator;
    estimator.observeIdleAudio(rx.data(), rx.size());
    const auto snapshot = estimator.snapshot();

    EstimateResult result;
    result.valid = snapshot.valid;
    result.snr_db = snapshot.snr_db;
    result.latest_snr_db = snapshot.latest_instant_snr_db;
    result.normalized_noise_rms = snapshot.normalized_noise_rms;
    result.fir_energy = snapshot.fir_energy;
    result.enbw_hz = snapshot.equivalent_noise_bandwidth_hz;
    result.windows = snapshot.windows_observed;
    return result;
}

void checkChannel(::ChannelType channel, float tolerance_db) {
    const std::vector<float> snrs = {-5.0f, 0.0f, 5.0f, 10.0f, 15.0f, 20.0f};
    const std::vector<uint32_t> seeds = {1u, 2u, 3u, 4u, 5u};

    for (float snr_db : snrs) {
        double sum = 0.0;
        float min_estimate = 1000.0f;
        float max_estimate = -1000.0f;
        int count = 0;
        EstimateResult last;

        for (uint32_t seed : seeds) {
            const EstimateResult r = measureIdleSNR(channel, snr_db, seed);
            last = r;
            CHECK(r.valid, std::string(channelName(channel)) + " idle SNR should be valid");
            CHECK(r.windows >= 10,
                  std::string(channelName(channel)) + " should observe multiple idle windows");
            CHECK(std::isfinite(r.snr_db),
                  std::string(channelName(channel)) + " idle SNR should be finite");
            if (r.valid && std::isfinite(r.snr_db)) {
                sum += r.snr_db;
                min_estimate = std::min(min_estimate, r.snr_db);
                max_estimate = std::max(max_estimate, r.snr_db);
                ++count;
            }
        }

        CHECK(count == static_cast<int>(seeds.size()),
              std::string(channelName(channel)) + " should produce all seed estimates");
        if (count == 0) {
            continue;
        }

        const float measured = static_cast<float>(sum / static_cast<double>(count));
        const float delta = measured - snr_db;
        std::cout << std::fixed << std::setprecision(2)
                  << channelName(channel)
                  << " configured=" << snr_db
                  << " measured=" << measured
                  << " delta=" << delta
                  << " min=" << min_estimate
                  << " max=" << max_estimate
                  << " tolerance=" << tolerance_db
                  << " norm_noise_rms=" << last.normalized_noise_rms
                  << " fir_energy=" << std::setprecision(8) << last.fir_energy
                  << " enbw_hz=" << std::setprecision(2) << last.enbw_hz
                  << "\n";

        CHECK(std::abs(delta) <= tolerance_db,
              std::string(channelName(channel)) +
                  " idle-noise SNR should match configured broadband SNR");
    }
}

}  // namespace

int main() {
    ultra::setLogLevel(ultra::LogLevel::ERROR);

    checkChannel(::ChannelType::AWGN, 1.5f);
    checkChannel(::ChannelType::GOOD, 3.0f);
    checkChannel(::ChannelType::MODERATE, 3.0f);

    if (tests_failed == 0) {
        std::cout << "PASS: Idle-noise SNR calibration (" << tests_run << " checks)\n";
        return 0;
    }
    std::cout << "FAIL: " << tests_failed << "/" << tests_run << " checks failed\n";
    return 1;
}
