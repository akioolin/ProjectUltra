#include "sim/channel_snr_probe.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace ultra::sim;

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

void check_channel(::ChannelType channel_type) {
    constexpr float kToleranceDb = 1.5f;
    const std::vector<float> snrs = {20.0f, 15.0f, 10.0f, 5.0f, 0.0f, -5.0f};

    for (float snr_db : snrs) {
        ChannelSNRProbeConfig cfg;
        cfg.channel_type = channel_type;
        cfg.snr_db = snr_db;
        cfg.seed = 0x5A17u;

        const ChannelSNRProbeResult r = runChannelSNRProbe(cfg);
        std::cout << std::fixed << std::setprecision(2)
                  << channelSNRProbeName(channel_type)
                  << " configured=" << r.configured_snr_db
                  << " measured=" << r.measured_snr_db
                  << " delta=" << r.delta_db
                  << " signal=" << r.measured_signal_rms
                  << " noise=" << r.measured_noise_rms
                  << " signal_window=" << r.signal_window_rms
                  << " signal_component=" << r.signal_component_rms
                  << " noise_samples=" << r.noise_samples
                  << "\n";

        CHECK(std::isfinite(r.measured_snr_db), "measured SNR should be finite");
        CHECK(std::abs(r.delta_db) <= kToleranceDb,
              std::string(channelSNRProbeName(channel_type)) +
                  " configured SNR should match measured in-band SNR");
    }
}

}  // namespace

int main() {
    ultra::setOperatorLogProfile();

    check_channel(::ChannelType::AWGN);
    check_channel(::ChannelType::GOOD);
    check_channel(::ChannelType::MODERATE);
    check_channel(::ChannelType::POOR);

    if (tests_failed == 0) {
        std::cout << "PASS: Channel SNR calibration (" << tests_run << " checks)\n";
        return 0;
    }
    std::cout << "FAIL: " << tests_failed << "/" << tests_run << " checks failed\n";
    return 1;
}
