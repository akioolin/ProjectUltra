#include "protocol/waveform_selection.hpp"
#include "waveform/waveform_factory.hpp"
#include "ultra/types.hpp"

#include <cmath>
#include <iostream>

using namespace ultra;
using namespace ultra::protocol;

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

void test_ofdm_rate_thresholds() {
    CHECK(selectOFDMCodeRate(20.0f, 0.00f) == CodeRate::R3_4,
          "AWGN SNR20 should allow R3/4");
    CHECK(selectOFDMCodeRate(15.0f, 0.00f) == CodeRate::R2_3,
          "near-AWGN SNR15 should allow R2/3");
    CHECK(selectOFDMCodeRate(15.0f, 0.30f) == CodeRate::R1_2,
          "good fading SNR15 should use R1/2");
    CHECK(selectOFDMCodeRate(15.0f, 0.90f) == CodeRate::R1_2,
          "moderate fading SNR15 should use R1/2");
    CHECK(selectOFDMCodeRate(15.0f, 1.20f) == CodeRate::R1_4,
          "heavy fading should fall back to R1/4");
}

void test_narrow_data_mode() {
    // OFDM_NARROW: DQPSK only. R1/2 promoted in clean-enough conditions
    // (relaxed 2026-05-03 after cli_simulator sweeps); R1/4 elsewhere.
    Modulation mod;
    CodeRate rate;

    // Hard floor: SNR=8 good fading must stay at R1/4 (CLAUDE.md baseline).
    recommendDataMode(8.0f, WaveformMode::OFDM_NARROW, mod, rate, 0.40f);
    CHECK(mod == Modulation::DQPSK, "narrow forces DQPSK");
    CHECK(rate == CodeRate::R1_4, "narrow SNR=8 good fading must stay R1/4 (baseline)");

    // AWGN path: SNR>=8 + fading<0.15 → R1/2
    recommendDataMode(8.0f, WaveformMode::OFDM_NARROW, mod, rate, 0.05f);
    CHECK(rate == CodeRate::R1_2, "narrow SNR=8 AWGN should allow R1/2");

    // Good-fading path: SNR>=10 + fading<0.65 → R1/2
    recommendDataMode(10.0f, WaveformMode::OFDM_NARROW, mod, rate, 0.40f);
    CHECK(rate == CodeRate::R1_2, "narrow SNR=10 good fading should allow R1/2");

    recommendDataMode(15.0f, WaveformMode::OFDM_NARROW, mod, rate, 0.40f);
    CHECK(rate == CodeRate::R1_2, "narrow SNR=15 good fading should allow R1/2");

    // Just under SNR threshold should drop to R1/4
    recommendDataMode(9.9f, WaveformMode::OFDM_NARROW, mod, rate, 0.40f);
    CHECK(rate == CodeRate::R1_4, "narrow SNR=9.9 good fading should drop to R1/4");

    // Moderate fading should always stay R1/4
    recommendDataMode(15.0f, WaveformMode::OFDM_NARROW, mod, rate, 0.90f);
    CHECK(rate == CodeRate::R1_4, "narrow moderate fading should stay R1/4");
}

void test_bootstrap_caps() {
    CHECK(capInitialOFDMRate(22.0f, 0.00f, CodeRate::R3_4) == CodeRate::R2_3,
          "initial R3/4 should cap to R2/3 below 24 dB");
    CHECK(capInitialOFDMRate(25.0f, 0.00f, CodeRate::R3_4) == CodeRate::R3_4,
          "initial R3/4 should be kept on near-ideal SNR25");
    CHECK(capInitialOFDMRate(20.0f, 0.12f, CodeRate::R2_3) == CodeRate::R1_2,
          "initial R2/3 should cap to R1/2 with fading evidence");
}

void test_waveform_recommendations() {
    auto low = recommendWaveformAndRate(8.0f, 0.00f);
    CHECK(low.waveform == WaveformMode::MC_DPSK, "SNR8 should use MC-DPSK");
    CHECK(low.rate == CodeRate::R1_4, "MC-DPSK recommendation should be R1/4");

    auto awgn = recommendWaveformAndRate(20.0f, 0.00f);
    CHECK(awgn.waveform == WaveformMode::OFDM_CHIRP, "AWGN SNR20 should use OFDM_CHIRP");
    CHECK(awgn.rate == CodeRate::R3_4, "AWGN SNR20 should recommend R3/4");

    auto moderate = recommendWaveformAndRate(15.0f, 0.90f);
    CHECK(moderate.waveform == WaveformMode::OFDM_CHIRP,
          "moderate fading SNR15 should still use OFDM_CHIRP");
    CHECK(moderate.rate == CodeRate::R1_2,
          "moderate fading SNR15 should recommend R1/2");
}

void test_data_mode_policy() {
    Modulation mod = Modulation::AUTO;
    CodeRate rate = CodeRate::AUTO;

    recommendDataMode(27.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.00f);
    CHECK(mod == Modulation::DQPSK, "OFDM data mode should remain DQPSK at high SNR");
    CHECK(rate == CodeRate::R3_4, "near-AWGN SNR27 should use R3/4");

    recommendDataMode(18.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::DQPSK, "fading OFDM data mode should remain DQPSK");
    CHECK(rate == CodeRate::R1_2, "good fading SNR18 should use R1/2");

    recommendDataMode(12.0f, WaveformMode::MC_DPSK, mod, rate, 0.90f);
    CHECK(mod == Modulation::DQPSK, "MC-DPSK should use DQPSK");
    CHECK(rate == CodeRate::R1_4, "MC-DPSK should use R1/4");
}

void test_waveform_factory() {
    for (auto mode : WaveformFactory::getAvailableModes()) {
        auto waveform = WaveformFactory::create(mode);
        CHECK(static_cast<bool>(waveform), "available waveform should be constructible");
        CHECK(WaveformFactory::isSupported(mode), "available waveform should be supported");
        CHECK(!WaveformFactory::getModeName(mode).empty(), "available waveform should have a name");
        CHECK(WaveformFactory::getMinSNR(mode) > -20.0f, "available waveform should have sane min SNR");
        CHECK(WaveformFactory::getMaxThroughput(mode) > 0.0f, "available waveform should have throughput");
    }

    CHECK(!WaveformFactory::isSupported(WaveformMode::MFSK),
          "reserved MFSK should not be advertised as supported");
    CHECK(!WaveformFactory::isSupported(WaveformMode::OTFS_EQ),
          "reserved OTFS_EQ should not be advertised as supported");
    CHECK(!WaveformFactory::isSupported(WaveformMode::OTFS_RAW),
          "reserved OTFS_RAW should not be advertised as supported");
    CHECK(WaveformFactory::recommendMCDPSKCarriers(-3.0f) == 5,
          "low-SNR MC-DPSK carrier recommendation");
    CHECK(WaveformFactory::recommendMCDPSKCarriers(15.0f) == 20,
          "high-SNR MC-DPSK carrier recommendation");
}

}  // namespace

int main() {
    test_ofdm_rate_thresholds();
    test_narrow_data_mode();
    test_bootstrap_caps();
    test_waveform_recommendations();
    test_data_mode_policy();
    test_waveform_factory();

    if (tests_failed != 0) {
        std::cout << "Waveform policy: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "Waveform policy: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
