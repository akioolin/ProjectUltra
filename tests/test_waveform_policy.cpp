#include "protocol/waveform_selection.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
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
    // R3/4 / R2/3 — unchanged SNR>=25 gates (not re-measured 2026-05-21).
    CHECK(selectOFDMCodeRate(25.0f, 0.00f) == CodeRate::R3_4,
          "AWGN in-band SNR25 should allow R3/4");
    CHECK(selectOFDMCodeRate(25.0f, 0.12f) == CodeRate::R2_3,
          "near-AWGN in-band SNR25 with slight fading should fall back to R2/3");

    // R1/2 — new per-fading thresholds (2026-05-21 floor recalibration,
    // PAPR-OFF baseline, +2 dB margin above measured reliable floor).
    //
    // AWGN gate at SNR >= 12 dB:
    CHECK(selectOFDMCodeRate(11.9f, 0.00f) == CodeRate::R1_4,
          "AWGN in-band SNR=11.9 should stay R1/4 (just under R1/2 gate)");
    CHECK(selectOFDMCodeRate(12.0f, 0.00f) == CodeRate::R1_2,
          "AWGN in-band SNR=12 should promote to R1/2");
    CHECK(selectOFDMCodeRate(21.7f, 0.00f) == CodeRate::R1_2,
          "AWGN in-band SNR=21.7 should use R1/2 (well above gate)");

    // Good fading gate at SNR >= 14 dB:
    CHECK(selectOFDMCodeRate(13.9f, 0.30f) == CodeRate::R1_4,
          "good fading in-band SNR=13.9 should stay R1/4 (just under R1/2 gate)");
    CHECK(selectOFDMCodeRate(14.0f, 0.30f) == CodeRate::R1_2,
          "good fading in-band SNR=14 should promote to R1/2");
    CHECK(selectOFDMCodeRate(20.0f, 0.30f) == CodeRate::R1_2,
          "good fading in-band SNR=20 should use R1/2 (well above gate)");

    // Moderate fading gate at SNR >= 18 dB:
    CHECK(selectOFDMCodeRate(17.9f, 0.90f) == CodeRate::R1_4,
          "moderate fading in-band SNR=17.9 should stay R1/4 (just under R1/2 gate)");
    CHECK(selectOFDMCodeRate(18.0f, 0.90f) == CodeRate::R1_2,
          "moderate fading in-band SNR=18 should promote to R1/2");
    CHECK(selectOFDMCodeRate(25.0f, 0.90f) == CodeRate::R1_2,
          "moderate fading in-band SNR=25 should use R1/2 (well above gate)");

    // Heavy+ fading (>= 1.10) — R1/4 only at all SNRs.
    CHECK(selectOFDMCodeRate(25.0f, 1.20f) == CodeRate::R1_4,
          "heavy fading should fall back to R1/4");
    CHECK(selectOFDMCodeRate(40.0f, 1.20f) == CodeRate::R1_4,
          "heavy fading at very high SNR should still stay R1/4");
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

void test_qam16_selection_rung() {
    // Coherent QAM16 descriptor ladder. R3/4 is enabled for AWGN20 by
    // descriptor gate; GOOD fading uses the coherent-QPSK workhorse rung.
    Modulation mod;
    CodeRate rate;

    // True AWGN at the R3/4 rung gate -> coherent QAM16 R3/4.
    recommendDataMode(20.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.05f);
    CHECK(mod == Modulation::QAM16, "AWGN in-band SNR=20 should select coherent QAM16");
    CHECK(rate == CodeRate::R3_4, "AWGN QAM16 SNR20 should select the active R3/4 rung");

    recommendDataMode(16.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.05f);
    CHECK(mod == Modulation::QAM16 && rate == CodeRate::R1_4,
          "AWGN in-band SNR=16 should promote to QAM16 R1/4");

    recommendDataMode(18.9f, WaveformMode::OFDM_CHIRP, mod, rate, 0.05f);
    CHECK(mod == Modulation::QAM16 && rate == CodeRate::R1_4,
          "AWGN in-band SNR=18.9 should stay on the proven QAM16 R1/4 rung");

    recommendDataMode(19.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.05f);
    CHECK(mod == Modulation::QAM16 && rate == CodeRate::R1_2,
          "AWGN in-band SNR=19 should promote to QAM16 R1/2");

    recommendDataMode(19.4f, WaveformMode::OFDM_CHIRP, mod, rate, 0.05f);
    CHECK(mod == Modulation::QAM16 && rate == CodeRate::R1_2,
          "AWGN in-band SNR=19.4 should stay on the proven QAM16 R1/2 rung");

    recommendDataMode(19.5f, WaveformMode::OFDM_CHIRP, mod, rate, 0.05f);
    CHECK(mod == Modulation::QAM16 && rate == CodeRate::R2_3,
          "AWGN in-band SNR=19.5 should promote to QAM16 R2/3");

    recommendDataMode(19.6f, WaveformMode::OFDM_CHIRP, mod, rate, 0.05f);
    CHECK(mod == Modulation::QAM16 && rate == CodeRate::R2_3,
          "AWGN in-band SNR=19.6 should stay on the proven QAM16 R2/3 rung");

    recommendDataMode(19.7f, WaveformMode::OFDM_CHIRP, mod, rate, 0.05f);
    CHECK(mod == Modulation::QAM16 && rate == CodeRate::R3_4,
          "AWGN in-band SNR=19.7 should promote to QAM16 R3/4");

    // Just under the gate stays differential.
    recommendDataMode(15.9f, WaveformMode::OFDM_CHIRP, mod, rate, 0.05f);
    CHECK(mod == Modulation::DQPSK,
          "AWGN in-band SNR=15.9 (just under QAM16 gate) stays differential");

    // GOOD fading at the measured QPSK floor selects coherent QPSK R2/3.
    recommendDataMode(20.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.40f);
    CHECK(mod == Modulation::QPSK,
          "good fading in-band SNR=20 should select coherent QPSK");
    CHECK(rate == CodeRate::R2_3, "good fading QPSK workhorse rung uses R2/3");

    recommendDataMode(17.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.40f);
    CHECK(mod == Modulation::DQPSK && rate == CodeRate::R1_2,
          "good fading in-band SNR=17 should stay differential below the QPSK floor");

    recommendDataMode(20.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.79f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R2_3,
          "GOOD-lobby estimator spread should still allow QPSK R2/3");

    recommendDataMode(20.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.80f);
    CHECK(mod == Modulation::DQPSK,
          "above GOOD-lobby estimator margin should fall back to DQPSK");

    recommendDataMode(16.9f, WaveformMode::OFDM_CHIRP, mod, rate, 0.40f);
    CHECK(mod == Modulation::DQPSK,
          "good fading in-band SNR=16.9 should fall back to DQPSK");
    CHECK(rate == CodeRate::R1_2,
          "good fading below QPSK floor should keep the existing OFDM R1/2 rate");

    // Moderate/poor fading remain differential.
    recommendDataMode(20.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.90f);
    CHECK(mod == Modulation::DQPSK,
          "moderate fading in-band SNR=20 should stay differential");

    recommendDataMode(25.0f, WaveformMode::OFDM_CHIRP, mod, rate, 1.20f);
    CHECK(mod == Modulation::DQPSK,
          "poor fading should stay differential even at high SNR");

    // DQPSK guard (good/snr12) preserved — sub-coherent-floor path untouched.
    recommendDataMode(12.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.40f);
    CHECK(mod == Modulation::DQPSK, "good/snr12 guard: differential preserved");
}

void test_bootstrap_caps() {
    CHECK(capInitialOFDMRate(19.6f, 0.04f, CodeRate::R3_4) == CodeRate::R2_3,
          "initial R3/4 should cap to R2/3 below the AWGN20 rung");
    CHECK(capInitialOFDMRate(19.7f, 0.04f, CodeRate::R3_4) == CodeRate::R3_4,
          "initial R3/4 should be kept for the AWGN20 QAM16 rung");
    CHECK(capInitialOFDMRate(20.0f, 0.05f, CodeRate::R2_3) == CodeRate::R2_3,
          "initial R2/3 should be kept for the AWGN20 QAM16 rung");
    CHECK(capInitialOFDMRate(30.0f, 0.12f, CodeRate::R2_3) == CodeRate::R1_2,
          "initial R2/3 should cap to R1/2 with fading evidence");
    CHECK(capInitialOFDMRate(20.0f, 0.40f, CodeRate::R2_3, Modulation::QPSK) ==
              CodeRate::R2_3,
          "initial QPSK R2/3 should keep the measured Good20 workhorse rung");
    CHECK(capInitialOFDMRate(20.0f, 0.40f, CodeRate::R2_3, Modulation::DQPSK) ==
              CodeRate::R1_2,
          "initial differential R2/3 should still cap without a measured Good20 rung");
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

    // High-SNR AWGN selects the highest proven QAM16 AWGN rung. GOOD fading
    // selects the measured coherent-QPSK workhorse rung instead of QAM16.
    recommendDataMode(37.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.00f);
    CHECK(mod == Modulation::QAM16, "high-SNR AWGN should select coherent QAM16");
    CHECK(rate == CodeRate::R3_4, "high-SNR AWGN QAM16 uses the active R3/4 rung");

    recommendDataMode(32.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::QPSK, "good-fading in-band SNR32 should select coherent QPSK");
    CHECK(rate == CodeRate::R2_3, "good-fading in-band SNR32 QPSK uses R2/3");

    recommendDataMode(30.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::QPSK, "good-fading in-band SNR30 should select coherent QPSK");
    CHECK(rate == CodeRate::R2_3, "good-fading in-band SNR30 QPSK uses R2/3");

    recommendDataMode(28.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::QPSK, "good-fading in-band SNR28 should select coherent QPSK");
    CHECK(rate == CodeRate::R2_3, "good-fading in-band SNR28 QPSK uses R2/3");

    // AWGN in-band SNR=21.7 is above the coherent QAM16 R3/4 AWGN gate. Below
    // 16 dB AWGN it remains differential.
    recommendDataMode(21.7f, WaveformMode::OFDM_CHIRP, mod, rate, 0.04f);
    CHECK(mod == Modulation::QAM16, "AWGN in-band SNR=21.7 selects coherent QAM16");
    CHECK(rate == CodeRate::R3_4, "AWGN in-band SNR=21.7 QAM16 uses R3/4");

    // SNR=19 GOOD fading: below the measured QPSK R2/3 gate and no longer
    // allowed to select QAM16 on the multipath path.
    recommendDataMode(19.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::DQPSK, "in-band SNR=19 good fading should fall back to DQPSK");
    CHECK(rate == CodeRate::R1_2, "in-band SNR=19 good fading should keep R1/2 DQPSK");

    recommendDataMode(16.9f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::DQPSK, "in-band SNR=16.9 good fading falls back to DQPSK");
    CHECK(rate == CodeRate::R1_2, "in-band SNR=16.9 good fading keeps R1/2 DQPSK");

    // Just under the R1/2 Good gate at SNR=13.9 dB.
    recommendDataMode(13.9f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(rate == CodeRate::R1_4, "in-band SNR=13.9 good fading should stay R1/4");

    // Moderate fading: D8PSK rejected even at high SNR - falls back to DQPSK.
    recommendDataMode(30.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.90f);
    CHECK(mod == Modulation::DQPSK, "moderate fading should reject D8PSK");
    CHECK(rate == CodeRate::R1_2, "DQPSK moderate fading uses R1/2");

    recommendDataMode(12.0f, WaveformMode::MC_DPSK, mod, rate, 0.90f);
    CHECK(mod == Modulation::DQPSK, "MC-DPSK should use DQPSK");
    CHECK(rate == CodeRate::R1_4, "MC-DPSK should use R1/4");
}

void test_ofdm_chirp_qam16_configuration() {
    OFDMChirpWaveform waveform;
    waveform.configure(Modulation::QAM16, CodeRate::R2_3);

    CHECK(waveform.getModulation() == Modulation::QAM16,
          "OFDM_CHIRP should keep QAM16 instead of falling back to DQPSK");
    CHECK(waveform.getCodeRate() == CodeRate::R2_3,
          "OFDM_CHIRP QAM16 configuration should keep requested rate");
    CHECK(waveform.getConfig().use_pilots,
          "OFDM_CHIRP QAM16 should use pilot-aided coherent demodulation");
    CHECK(waveform.getPilotSpacing() == 5,
          "OFDM_CHIRP QAM16 R2/3 should use dense coherent pilot spacing");
    CHECK(waveform.getCapabilities().requires_pilots,
          "OFDM_CHIRP capabilities should advertise pilots when QAM16 is active");
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
    test_qam16_selection_rung();
    test_bootstrap_caps();
    test_waveform_recommendations();
    test_data_mode_policy();
    test_ofdm_chirp_qam16_configuration();
    test_waveform_factory();

    if (tests_failed != 0) {
        std::cout << "Waveform policy: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "Waveform policy: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
