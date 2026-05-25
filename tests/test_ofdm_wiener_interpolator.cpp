#include "../src/ofdm/pilot_pattern.hpp"
#include "../src/ofdm/wiener_interpolator.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ultra;

namespace {

int tests_passed = 0;
int tests_failed = 0;

#define TEST(name) \
    std::cout << "Testing " << name << "... "; \
    try

#define PASS() \
    std::cout << "PASS\n"; \
    ++tests_passed

#define FAIL(msg) \
    std::cout << "FAIL: " << msg << "\n"; \
    ++tests_failed

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ModemConfig makeConfig() {
    ModemConfig cfg;
    cfg.modulation = Modulation::QAM16;
    cfg.code_rate = CodeRate::R1_4;
    cfg.num_carriers = 59;
    cfg.fft_size = 1024;
    cfg.sample_rate = 48000;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 5;
    cfg.scattered_pilots = true;
    return cfg;
}

Complex knownChannel(size_t symbol, size_t logical, size_t carriers) {
    const float x = 2.0f * ofdm_wiener::kPi *
                    static_cast<float>(logical) /
                    static_cast<float>(carriers);
    const Complex freq(1.0f + 0.20f * std::cos(x),
                       0.16f * std::sin(x));
    const float phase = 0.015f * static_cast<float>(symbol);
    const Complex time(std::cos(phase), std::sin(phase));
    return time * freq;
}

void test_scattered_2d_wiener_recovers_smooth_channel() {
    TEST("scattered-pilot separable Wiener recovers a known smooth channel") {
        const ModemConfig cfg = makeConfig();
        const size_t target_symbol = 9;
        const float symbol_period_s =
            static_cast<float>(cfg.getSymbolDuration()) /
            static_cast<float>(cfg.sample_rate);
        const float carrier_spacing_hz =
            static_cast<float>(cfg.sample_rate) /
            static_cast<float>(cfg.fft_size);

        std::vector<std::vector<ofdm_wiener::Observation1D>> histories(cfg.num_carriers);
        for (size_t s = 0; s <= target_symbol; ++s) {
            std::vector<int> all_fft, data_fft, pilot_fft;
            std::vector<size_t> data_logical, pilot_logical;
            std::vector<bool> is_pilot;
            std::vector<Complex> pilot_sequence;
            ofdm_pilots::buildCarrierPattern(cfg, s, all_fft, data_fft, pilot_fft,
                                             data_logical, pilot_logical,
                                             is_pilot, pilot_sequence);
            for (size_t logical : pilot_logical) {
                histories[logical].push_back(ofdm_wiener::Observation1D{
                    static_cast<float>(s),
                    knownChannel(s, logical, cfg.num_carriers),
                    0.001f});
            }
        }

        std::vector<ofdm_wiener::Observation1D> freq_obs;
        freq_obs.reserve(cfg.num_carriers);
        for (size_t logical = 0; logical < cfg.num_carriers; ++logical) {
            const auto time_est = ofdm_wiener::estimate1D(
                histories[logical],
                static_cast<float>(target_symbol),
                4,
                [&](float delta_symbols) {
                    return ofdm_wiener::timeCorrelation(
                        delta_symbols, symbol_period_s, 0.5f);
                });
            require(time_est.valid, "missing time-domain Wiener estimate");
            freq_obs.push_back(ofdm_wiener::Observation1D{
                static_cast<float>(logical),
                time_est.value,
                std::max(time_est.error_var, 0.001f)});
        }

        double mse = 0.0;
        size_t count = 0;
        for (size_t logical = 0; logical < cfg.num_carriers; ++logical) {
            const auto freq_est = ofdm_wiener::estimate1D(
                freq_obs,
                static_cast<float>(logical),
                16,
                [&](float delta_logical) {
                    return ofdm_wiener::frequencyCorrelation(
                        delta_logical, carrier_spacing_hz, 1.0e-3f);
                });
            require(freq_est.valid, "missing frequency-domain Wiener estimate");
            const Complex err =
                freq_est.value - knownChannel(target_symbol, logical, cfg.num_carriers);
            mse += std::norm(err);
            ++count;
        }

        const double rms = std::sqrt(mse / static_cast<double>(count));
        require(rms < 0.08, "known-channel RMS error exceeded tolerance");
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

}  // namespace

int main() {
    test_scattered_2d_wiener_recovers_smooth_channel();

    std::cout << "Passed: " << tests_passed << "\n";
    std::cout << "Failed: " << tests_failed << "\n";
    return tests_failed == 0 ? 0 : 1;
}
