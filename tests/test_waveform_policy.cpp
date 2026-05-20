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
    CHECK(selectOFDMCodeRate(21.7f, 0.00f) == CodeRate::R1_4,
          "AWGN-12 in-band SNR should stay R1/4");
    CHECK(selectOFDMCodeRate(25.0f, 0.00f) == CodeRate::R3_4,
          "AWGN in-band SNR25 should allow R3/4");
    CHECK(selectOFDMCodeRate(25.0f, 0.12f) == CodeRate::R2_3,
          "near-AWGN in-band SNR25 with slight fading should fall back to R2/3");
    CHECK(selectOFDMCodeRate(25.0f, 0.30f) == CodeRate::R1_2,
          "good fading in-band SNR25 should use R1/2");
    CHECK(selectOFDMCodeRate(25.0f, 0.90f) == CodeRate::R1_2,
          "moderate fading in-band SNR25 should use R1/2");
    CHECK(selectOFDMCodeRate(20.0f, 0.30f) == CodeRate::R1_4,
          "good fading in-band SNR20 should stay at R1/4");
    CHECK(selectOFDMCodeRate(25.0f, 1.20f) == CodeRate::R1_4,
          "heavy fading should fall back to R1/4");
}

void test_narrow_data_mode() {
    // OFDM_NARROW: DQPSK only. R1/2 promoted in clean-enough conditions
    // (relaxed 2026-05-03 after cli_simulator sweeps); R1/4 elsewhere.
    Modulation mod;
    CodeRate rate;

    // Hard floor: in-band SNR=18 good fading must stay at R1/4 (CLAUDE.md baseline).
    recommendDataMode(18.0f, WaveformMode::OFDM_NARROW, mod, rate, 0.40f);
    CHECK(mod == Modulation::DQPSK, "narrow forces DQPSK");
    CHECK(rate == CodeRate::R1_4, "narrow in-band SNR=18 good fading must stay R1/4");

    // AWGN path: in-band SNR>=18 + fading<0.15 -> R1/2
    recommendDataMode(18.0f, WaveformMode::OFDM_NARROW, mod, rate, 0.05f);
    CHECK(rate == CodeRate::R1_2, "narrow in-band SNR=18 AWGN should allow R1/2");

    // Good-fading path: in-band SNR>=20 + fading<0.65 -> R1/2
    recommendDataMode(20.0f, WaveformMode::OFDM_NARROW, mod, rate, 0.40f);
    CHECK(rate == CodeRate::R1_2, "narrow in-band SNR=20 good fading should allow R1/2");

    recommendDataMode(25.0f, WaveformMode::OFDM_NARROW, mod, rate, 0.40f);
    CHECK(rate == CodeRate::R1_2, "narrow in-band SNR=25 good fading should allow R1/2");

    // Just under SNR threshold should drop to R1/4
    recommendDataMode(19.9f, WaveformMode::OFDM_NARROW, mod, rate, 0.40f);
    CHECK(rate == CodeRate::R1_4, "narrow in-band SNR=19.9 good fading should drop to R1/4");

    // Moderate fading should always stay R1/4
    recommendDataMode(25.0f, WaveformMode::OFDM_NARROW, mod, rate, 0.90f);
    CHECK(rate == CodeRate::R1_4, "narrow moderate fading should stay R1/4");
}

void test_bootstrap_caps() {
    CHECK(capInitialOFDMRate(33.0f, 0.00f, CodeRate::R3_4) == CodeRate::R2_3,
          "initial R3/4 should cap to R2/3 below in-band 34 dB");
    CHECK(capInitialOFDMRate(34.0f, 0.00f, CodeRate::R3_4) == CodeRate::R3_4,
          "initial R3/4 should be kept on near-ideal in-band SNR34");
    CHECK(capInitialOFDMRate(30.0f, 0.12f, CodeRate::R2_3) == CodeRate::R1_2,
          "initial R2/3 should cap to R1/2 with fading evidence");
}

void test_waveform_recommendations() {
    auto low = recommendWaveformAndRate(9.9f, 0.00f);
    CHECK(low.waveform == WaveformMode::MC_DPSK, "AWGN below in-band SNR10 should use MC-DPSK");
    CHECK(low.rate == CodeRate::R1_4, "MC-DPSK recommendation should be R1/4");

    auto awgn = recommendWaveformAndRate(10.0f, 0.00f);
    CHECK(awgn.waveform == WaveformMode::OFDM_CHIRP, "AWGN in-band SNR10 should use OFDM_CHIRP");
    CHECK(awgn.rate == CodeRate::R1_4, "AWGN in-band SNR10 should recommend R1/4");

    auto good_below_floor = recommendWaveformAndRate(11.9f, 0.30f);
    CHECK(good_below_floor.waveform == WaveformMode::MC_DPSK,
          "good fading below in-band SNR12 should keep MC-DPSK margin");

    auto moderate_floor = recommendWaveformAndRate(14.0f, 0.90f);
    CHECK(moderate_floor.waveform == WaveformMode::OFDM_CHIRP,
          "moderate fading in-band SNR14 should use OFDM_CHIRP");
    CHECK(moderate_floor.rate == CodeRate::R1_4,
          "moderate fading in-band SNR14 should recommend R1/4");

    auto moderate = recommendWaveformAndRate(25.0f, 0.90f);
    CHECK(moderate.waveform == WaveformMode::OFDM_CHIRP,
          "moderate fading in-band SNR25 should still use OFDM_CHIRP");
    CHECK(moderate.rate == CodeRate::R1_2,
          "moderate fading in-band SNR25 should recommend R1/2");
}

void test_data_mode_policy() {
    Modulation mod = Modulation::AUTO;
    CodeRate rate = CodeRate::AUTO;

    // D8PSK gates use the unified in-band meter. AWGN-12 in-band (~22 dB)
    // must remain below all D8PSK promotion thresholds.

    // High-SNR AWGN: D8PSK R3/4
    recommendDataMode(37.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.00f);
    CHECK(mod == Modulation::D8PSK, "high-SNR AWGN should promote to D8PSK");
    CHECK(rate == CodeRate::R3_4, "near-AWGN in-band SNR37 should use R3/4");

    recommendDataMode(32.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::D8PSK, "good-fading in-band SNR32 should be D8PSK");
    CHECK(rate == CodeRate::R1_2, "good-fading in-band SNR32 D8PSK uses R1/2");

    recommendDataMode(30.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::DQPSK, "good-fading in-band SNR30 stays DQPSK");
    CHECK(rate == CodeRate::R1_2, "good-fading in-band SNR30 should use R1/2 with DQPSK");

    recommendDataMode(28.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::DQPSK, "good-fading in-band SNR28 stays DQPSK");
    CHECK(rate == CodeRate::R1_2, "good-fading in-band SNR28 should use R1/2 with DQPSK");

    // AWGN-12 in-band regression: must not promote to D8PSK R2/3.
    recommendDataMode(21.7f, WaveformMode::OFDM_CHIRP, mod, rate, 0.04f);
    CHECK(mod == Modulation::DQPSK, "AWGN-12 in-band stays DQPSK");
    CHECK(rate == CodeRate::R1_4, "AWGN-12 in-band uses DQPSK R1/4");

    recommendDataMode(19.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::DQPSK, "below-floor in-band SNR19 falls back to DQPSK");
    CHECK(rate == CodeRate::R1_4, "DQPSK fallback at in-band SNR19 uses R1/4");

    // Moderate fading: D8PSK rejected even at high SNR - falls back to DQPSK.
    recommendDataMode(30.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.90f);
    CHECK(mod == Modulation::DQPSK, "moderate fading should reject D8PSK");
    CHECK(rate == CodeRate::R1_2, "DQPSK moderate fading uses R1/2");

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
    CHECK(WaveformFactory::recommendMCDPSKCarriers(25.0f) == 20,
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
