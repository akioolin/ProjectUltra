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
    // COHERENT-ONLY LADDER (kCoherentLadder, 2026-06-02). Auto rate selection is
    // one ladder walked high->low; QAM16 and the QPSK R3/4 rung are DISABLED on
    // auto (kRungDisabledDb), so they are NEVER auto-selected. The only enabled
    // rungs per class are:
    //   AWGN     : R1/4 (>=8), R1/2 (>=10). R2/3 / R3/4 / QAM16 disabled.
    //   GOOD     : R1/2 (>=10), R2/3 (>=15). R1/4 below 10. R3/4 / QAM16 disabled.
    //   MODERATE : R1/4 (>=14), R1/2 (>=18). R2/3 / R3/4 / QAM16 disabled.
    //   POOR (>=1.10): R1/4 always (defensive; Poor routes to MC-DPSK upstream).

    // AWGN: R2/3 and R3/4 are disabled, so even high SNR tops out at R1/2.
    CHECK(selectOFDMCodeRate(25.0f, 0.00f) == CodeRate::R1_2,
          "AWGN in-band SNR25 tops out at R1/2 (R2/3 / R3/4 disabled on auto)");
    CHECK(selectOFDMCodeRate(40.0f, 0.00f) == CodeRate::R1_2,
          "AWGN even at very high SNR stays R1/2 (no QAM16/R3/4 auto promotion)");
    // Near-AWGN slight fading (0.12 < 0.15) is still AWGN class -> R1/2, not R2/3.
    CHECK(selectOFDMCodeRate(25.0f, 0.12f) == CodeRate::R1_2,
          "near-AWGN (fading 0.12, AWGN class) in-band SNR25 stays R1/2, not R2/3");

    // R1/2 transition gates (2026-06-02 monotonicity update): AWGN 10, GOOD 10, MODERATE 18.
    //
    // AWGN gate at SNR >= 10 dB (R1/4 below):
    CHECK(selectOFDMCodeRate(9.9f, 0.00f) == CodeRate::R1_4,
          "AWGN in-band SNR=9.9 should stay R1/4 (just under R1/2 gate)");
    CHECK(selectOFDMCodeRate(10.0f, 0.00f) == CodeRate::R1_2,
          "AWGN in-band SNR=10 should promote to R1/2 (R1/2 reliable @ AWGN 10, monotonicity)");
    CHECK(selectOFDMCodeRate(21.7f, 0.00f) == CodeRate::R1_2,
          "AWGN in-band SNR=21.7 should use R1/2 (well above gate)");

    // Good fading gate at SNR >= 10 dB; R2/3 enabled at >= 15 dB.
    CHECK(selectOFDMCodeRate(9.9f, 0.30f) == CodeRate::R1_4,
          "good fading in-band SNR=9.9 should stay R1/4 (just under R1/2 gate)");
    CHECK(selectOFDMCodeRate(10.0f, 0.30f) == CodeRate::R1_2,
          "good fading in-band SNR=10 should promote to R1/2 (measured reliable @ Good 10)");
    CHECK(selectOFDMCodeRate(14.9f, 0.30f) == CodeRate::R1_2,
          "good fading in-band SNR=14.9 should stay R1/2 (just under R2/3 gate)");
    CHECK(selectOFDMCodeRate(15.0f, 0.30f) == CodeRate::R2_3,
          "good fading in-band SNR=15 should promote to QPSK R2/3 (measured 2026-06-02)");
    CHECK(selectOFDMCodeRate(20.0f, 0.30f) == CodeRate::R2_3,
          "good fading in-band SNR=20 should use R2/3 (well above gate, R3/4 disabled)");

    // Moderate fading gate at SNR >= 18 dB; R2/3 disabled on moderate.
    CHECK(selectOFDMCodeRate(17.9f, 0.90f) == CodeRate::R1_4,
          "moderate fading in-band SNR=17.9 should stay R1/4 (just under R1/2 gate)");
    CHECK(selectOFDMCodeRate(18.0f, 0.90f) == CodeRate::R1_2,
          "moderate fading in-band SNR=18 should promote to R1/2");
    CHECK(selectOFDMCodeRate(25.0f, 0.90f) == CodeRate::R1_2,
          "moderate fading in-band SNR=25 should top out at R1/2 (R2/3 disabled)");

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

void test_coherent_ladder_selection() {
    // COHERENT-ONLY LADDER (kCoherentLadder, 2026-06-02). The OFDM band auto-selects
    // ALWAYS coherent QPSK now; QAM16 and the QPSK R3/4 rung are DISABLED on auto
    // (kRungDisabledDb) — they are reachable only via ULTRA_FORCE_* (not set here).
    // recommendDataMode walks the ladder high->low and returns the first rung the
    // (snr, fading_class) clears. mod is ALWAYS QPSK for OFDM_CHIRP.
    Modulation mod;
    CodeRate rate;

    // --- AWGN class (fading < 0.15): R1/4 (>=8), R1/2 (>=10); R2/3/R3/4/QAM16 OFF ---
    // The OLD behavior promoted QAM16 R1/4/R1/2/R2/3/R3/4 at AWGN 16/19/19.5/19.7.
    // The NEW behavior NEVER auto-selects QAM16 and tops out at QPSK R1/2.
    recommendDataMode(20.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.05f);
    CHECK(mod == Modulation::QPSK, "AWGN in-band SNR=20 selects coherent QPSK (never QAM16)");
    CHECK(rate == CodeRate::R1_2, "AWGN in-band SNR=20 tops out at QPSK R1/2 (R2/3/R3/4 disabled)");

    recommendDataMode(25.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.05f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R1_2,
          "AWGN in-band SNR=25 stays QPSK R1/2 (no QAM16/R3/4 auto promotion)");

    recommendDataMode(16.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.05f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R1_2,
          "AWGN in-band SNR=16 is coherent QPSK R1/2 (was QAM16 R1/4 on old ladder)");

    recommendDataMode(9.9f, WaveformMode::OFDM_CHIRP, mod, rate, 0.05f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R1_4,
          "AWGN in-band SNR=9.9 stays QPSK R1/4 (just under the R1/2 gate)");

    recommendDataMode(10.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.05f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R1_2,
          "AWGN in-band SNR=10 promotes to coherent QPSK R1/2");

    // --- GOOD class (0.15 <= fading < 0.65): R1/2 (>=10), R2/3 (>=15); R1/4 below 10 ---
    // The OLD behavior promoted QPSK R3/4 at Good@20; NEW tops out at QPSK R2/3.
    recommendDataMode(20.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.40f);
    CHECK(mod == Modulation::QPSK, "good fading in-band SNR=20 selects coherent QPSK");
    CHECK(rate == CodeRate::R2_3,
          "good fading in-band SNR=20 tops out at QPSK R2/3 (R3/4 disabled, was R3/4 on old ladder)");

    recommendDataMode(15.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.40f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R2_3,
          "good fading in-band SNR=15 promotes to QPSK R2/3 (measured 2026-06-02)");

    recommendDataMode(14.9f, WaveformMode::OFDM_CHIRP, mod, rate, 0.40f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R1_2,
          "good fading in-band SNR=14.9 stays QPSK R1/2 (just under the R2/3 gate)");

    recommendDataMode(14.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.40f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R1_2,
          "good fading in-band SNR=14 promotes to QPSK R1/2");

    recommendDataMode(17.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.40f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R2_3,
          "good fading in-band SNR=17 is coherent QPSK R2/3 (above the R2/3 gate)");

    recommendDataMode(9.9f, WaveformMode::OFDM_CHIRP, mod, rate, 0.40f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R1_4,
          "good fading in-band SNR=9.9 stays QPSK R1/4 (just under the R1/2 gate)");

    // good/snr10: R1/2 reliable @ Good 10 (measured) — the OFDM band runs QPSK R1/2 there.
    recommendDataMode(10.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.40f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R1_2,
          "good/snr10: OFDM band promotes to coherent QPSK R1/2 (measured reliable @ Good 10)");

    // --- MODERATE class (0.65 <= fading < 1.10): R1/4 (>=14), R1/2 (>=18); R2/3 OFF ---
    recommendDataMode(20.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.90f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R1_2,
          "moderate fading in-band SNR=20 selects coherent QPSK R1/2 (R2/3 disabled)");

    recommendDataMode(14.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.90f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R1_4,
          "moderate fading in-band SNR=14 selects coherent QPSK R1/4 at the entry floor");

    // --- POOR class (fading >= 1.10): always defensive QPSK R1/4 ---
    // Poor routes to MC-DPSK at the SELECTION layer (kOFDMEntryFloorPoorDb unreachable;
    // verified in the ConnectionPolicy test). recommendDataMode given OFDM+Poor still
    // returns coherent QPSK R1/4 — a defensive don't-happen case, never differential.
    recommendDataMode(25.0f, WaveformMode::OFDM_CHIRP, mod, rate, 1.20f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R1_4,
          "OFDM+Poor (a don't-happen: Poor routes to MC-DPSK) is coherent QPSK R1/4");
}

void test_bootstrap_caps() {
    // Bootstrap cap = min(candidate, ladder_rate_for(snr,fading)) by code-rate value,
    // then never above R1/2 (the adaptive loop climbs from R1/2 once ACK quality
    // arrives). On the coherent-only ladder the auto rate never exceeds R2/3, and the
    // bootstrap ceiling pins everything to R1/2 — so QAM16/R3/4 can never be a start
    // rate. The 4-arg overload no longer depends on modulation (returns candidate only
    // when ULTRA_FORCE_DATA_RATE is set, which the tests do not set).
    CHECK(capInitialOFDMRate(19.6f, 0.04f, CodeRate::R3_4) == CodeRate::R1_2,
          "initial R3/4 caps to the R1/2 bootstrap ceiling on AWGN");
    CHECK(capInitialOFDMRate(19.7f, 0.04f, CodeRate::R3_4) == CodeRate::R1_2,
          "initial R3/4 caps to R1/2 (QAM16/R3/4 never an auto start rate)");
    CHECK(capInitialOFDMRate(20.0f, 0.05f, CodeRate::R2_3) == CodeRate::R1_2,
          "initial R2/3 caps to the R1/2 bootstrap ceiling on AWGN");
    CHECK(capInitialOFDMRate(30.0f, 0.12f, CodeRate::R2_3) == CodeRate::R1_2,
          "initial R2/3 caps to R1/2 with near-AWGN evidence");
    CHECK(capInitialOFDMRate(20.0f, 0.40f, CodeRate::R2_3, Modulation::QPSK) ==
              CodeRate::R1_2,
          "initial QPSK R2/3 caps to the R1/2 bootstrap ceiling on good fading");
    CHECK(capInitialOFDMRate(20.0f, 0.40f, CodeRate::R2_3, Modulation::DQPSK) ==
              CodeRate::R1_2,
          "coherent-only band: modulation no longer changes the bootstrap cap");
    // Below the R1/2 gate (AWGN 10) the cap follows the ladder floor (R1/4), not the ceiling.
    CHECK(capInitialOFDMRate(9.5f, 0.05f, CodeRate::R3_4) == CodeRate::R1_4,
          "AWGN below the R1/2 gate caps to the QPSK R1/4 ladder floor");
}

void test_waveform_recommendations() {
    // AWGN entry floor lowered to 8 (R1/4 clean @ AWGN 8, measured 2026-06-02).
    auto low = recommendWaveformAndRate(7.9f, 0.00f);
    CHECK(low.waveform == WaveformMode::MC_DPSK, "AWGN below in-band SNR8 should use MC-DPSK");
    CHECK(low.rate == CodeRate::R1_4, "MC-DPSK recommendation should be R1/4");

    auto awgn_floor = recommendWaveformAndRate(8.0f, 0.00f);
    CHECK(awgn_floor.waveform == WaveformMode::OFDM_CHIRP, "AWGN in-band SNR8 should use OFDM_CHIRP");
    CHECK(awgn_floor.rate == CodeRate::R1_4, "AWGN in-band SNR8 should recommend R1/4 (floor)");

    auto awgn = recommendWaveformAndRate(10.0f, 0.00f);
    CHECK(awgn.waveform == WaveformMode::OFDM_CHIRP, "AWGN in-band SNR10 should use OFDM_CHIRP");
    CHECK(awgn.rate == CodeRate::R1_2, "AWGN in-band SNR10 promotes to R1/2 (monotonicity, was R1/4)");

    // Good entry floor lowered to 10 (R1/2 reliable @ Good 10, measured).
    auto good_below_floor = recommendWaveformAndRate(9.9f, 0.30f);
    CHECK(good_below_floor.waveform == WaveformMode::MC_DPSK,
          "good fading below in-band SNR10 should keep MC-DPSK margin");

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

    // COHERENT-ONLY LADDER (2026-06-02): the OFDM band always auto-selects QPSK.
    // QAM16 / R3/4 / differential rungs are no longer auto-selected. High SNR no
    // longer climbs into QAM16; it tops out per-class at R1/2 (AWGN/Moderate) or
    // R2/3 (Good).

    // High-SNR AWGN: R2/3 and R3/4 disabled on AWGN -> tops out at QPSK R1/2.
    recommendDataMode(37.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.00f);
    CHECK(mod == Modulation::QPSK, "high-SNR AWGN selects coherent QPSK (never QAM16)");
    CHECK(rate == CodeRate::R1_2, "high-SNR AWGN tops out at QPSK R1/2 (R2/3/R3/4 disabled)");

    // High-SNR GOOD fading: R3/4 disabled -> tops out at QPSK R2/3.
    recommendDataMode(32.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::QPSK, "good-fading in-band SNR32 selects coherent QPSK");
    CHECK(rate == CodeRate::R2_3, "good-fading in-band SNR32 tops out at QPSK R2/3 (R3/4 disabled)");

    recommendDataMode(30.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::QPSK, "good-fading in-band SNR30 selects coherent QPSK");
    CHECK(rate == CodeRate::R2_3, "good-fading in-band SNR30 tops out at QPSK R2/3");

    recommendDataMode(28.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::QPSK, "good-fading in-band SNR28 selects coherent QPSK");
    CHECK(rate == CodeRate::R2_3, "good-fading in-band SNR28 tops out at QPSK R2/3");

    // AWGN in-band SNR=21.7: QAM16 never auto-selected, AWGN tops out at QPSK R1/2.
    recommendDataMode(21.7f, WaveformMode::OFDM_CHIRP, mod, rate, 0.04f);
    CHECK(mod == Modulation::QPSK, "AWGN in-band SNR=21.7 selects coherent QPSK (never QAM16)");
    CHECK(rate == CodeRate::R1_2, "AWGN in-band SNR=21.7 stays QPSK R1/2");

    // SNR=19 GOOD fading: above the R2/3 gate (>=15) -> coherent QPSK R2/3.
    recommendDataMode(19.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::QPSK, "in-band SNR=19 good fading selects coherent QPSK");
    CHECK(rate == CodeRate::R2_3, "in-band SNR=19 good fading is above the R2/3 gate");

    // SNR=16.9 GOOD fading: above the R2/3 gate (>=15) -> coherent QPSK R2/3.
    recommendDataMode(16.9f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::QPSK, "in-band SNR=16.9 good fading selects coherent QPSK");
    CHECK(rate == CodeRate::R2_3, "in-band SNR=16.9 good fading is above the R2/3 gate");

    // SNR=14.5 GOOD fading: between R1/2 (>=14) and R2/3 (>=15) gates -> QPSK R1/2.
    recommendDataMode(14.5f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(mod == Modulation::QPSK, "in-band SNR=14.5 good fading selects coherent QPSK");
    CHECK(rate == CodeRate::R1_2, "in-band SNR=14.5 good fading is QPSK R1/2 (under the R2/3 gate)");

    // Just under the R1/2 Good gate at SNR=9.9 dB -> R1/4 floor.
    recommendDataMode(9.9f, WaveformMode::OFDM_CHIRP, mod, rate, 0.30f);
    CHECK(rate == CodeRate::R1_4, "in-band SNR=9.9 good fading should stay R1/4");

    // Moderate fading at high SNR: coherent QPSK, R2/3 disabled -> R1/2.
    recommendDataMode(30.0f, WaveformMode::OFDM_CHIRP, mod, rate, 0.90f);
    CHECK(mod == Modulation::QPSK, "moderate fading high-SNR selects coherent QPSK (differential retired)");
    CHECK(rate == CodeRate::R1_2, "moderate fading coherent QPSK tops out at R1/2 (R2/3 disabled)");

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
    test_coherent_ladder_selection();
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
