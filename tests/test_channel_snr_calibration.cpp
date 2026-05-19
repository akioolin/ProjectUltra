#include "gui/modem/streaming_encoder.hpp"
#include "psk/multi_carrier_dpsk.hpp"
#include "sim/channel_snr_probe.hpp"
#include "ultra/dsp.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
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

double rmsOf(const std::vector<float>& samples) {
    const double sum_sq = std::accumulate(
        samples.begin(), samples.end(), 0.0,
        [](double sum, float sample) {
            return sum + static_cast<double>(sample) * static_cast<double>(sample);
        });
    return samples.empty() ? 0.0 :
        std::sqrt(sum_sq / static_cast<double>(samples.size()));
}

void check_reference_constants() {
    ultra::gui::StreamingEncoder encoder;
    encoder.setMCDPSKConfig(ultra::mc_dpsk_presets::level8());
    encoder.setMode(ultra::protocol::WaveformMode::MC_DPSK);
    encoder.setDataMode(ultra::Modulation::DQPSK, ultra::CodeRate::R1_4);
    const std::vector<float> ping = encoder.encodePing();

    ultra::FIRFilter filter =
        ultra::FIRFilter::bandpass(101, 50.0f, 2950.0f, 48000.0f);
    std::vector<float> in_band;
    in_band.reserve(ping.size());
    for (float sample : ping) {
        in_band.push_back(filter.process(sample));
    }

    ultra::FIRFilter coefficient_filter =
        ultra::FIRFilter::bandpass(101, 50.0f, 2950.0f, 48000.0f);
    const auto& coeffs = coefficient_filter.coefficients();
    const double fir_energy = std::accumulate(
        coeffs.begin(), coeffs.end(), 0.0,
        [](double sum, float h) {
            return sum + static_cast<double>(h) * static_cast<double>(h);
        });
    const double offset_db = 10.0 * std::log10(
        1.0 / kModemInBandNoisePowerFraction);

    std::cout << std::fixed << std::setprecision(12)
              << "reference ping_samples=" << ping.size()
              << " broadband_rms=" << rmsOf(ping)
              << " in_band_rms=" << rmsOf(in_band)
              << " fir_energy=" << fir_energy
              << " offset_db=" << offset_db
              << "\n";

    CHECK(ping.size() == 62208, "PING sample count should match calibration capture");
    CHECK(std::abs(rmsOf(ping) - kModemReferenceBroadbandRms) <= 1.0e-7,
          "broadband PING RMS should match calibration constant");
    CHECK(std::abs(rmsOf(in_band) - kModemReferenceInBandRms) <= 1.0e-7,
          "in-band PING RMS should match calibration constant");
    CHECK(std::abs(kModemReferencePower -
                   static_cast<double>(kModemReferenceInBandRms) *
                   static_cast<double>(kModemReferenceInBandRms)) <= 1.0e-12,
          "SNR reference power should use the in-band PING RMS");
    CHECK(std::abs(fir_energy - kModemInBandNoisePowerFraction) <= 1.0e-8,
          "FIR coefficient energy should match calibration constant");
    CHECK(std::abs(offset_db - kModemBroadbandToInBandSnrOffsetDb) <= 1.0e-8,
          "broadband-to-in-band SNR offset should match FIR energy");
}

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

    check_reference_constants();
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
