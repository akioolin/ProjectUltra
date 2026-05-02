#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/modem.hpp"

#include <cmath>
#include <iostream>

using namespace ultra;
using namespace ultra::ofdm_link_adaptation;

namespace {

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg) \
    do { \
        ++tests_run; \
        if (!(cond)) { \
            ++tests_failed; \
            std::cout << "FAIL: " << msg << "\n"; \
            return; \
        } \
    } while (0)

void test_pilot_spacing_policy() {
    CHECK(recommendedPilotSpacing(Modulation::DQPSK, CodeRate::R1_4) == 10,
          "DQPSK R1/4 should use the robust differential pilot profile");
    CHECK(recommendedPilotSpacing(Modulation::DQPSK, CodeRate::R3_4) == 15,
          "DQPSK R3/4 should reduce pilot overhead");
    CHECK(recommendedPilotSpacing(Modulation::D8PSK, CodeRate::R1_2) == 8,
          "D8PSK R1/2 should use denser pilots than DQPSK");
    CHECK(recommendedPilotSpacing(Modulation::QPSK, CodeRate::R1_2) == 5,
          "Coherent QPSK should use dense pilots");
    CHECK(recommendedPilotSpacing(Modulation::QAM16, CodeRate::R3_4) == 8,
          "High-rate coherent modes should reduce pilot overhead");
    CHECK(recommendedPilotSpacing(Modulation::QAM32, CodeRate::R3_4) == 5,
          "QAM32 R3/4 should keep dense pilots for hardware channel tracking");
    CHECK(recommendedPilotSpacing(Modulation::QAM64, CodeRate::R3_4) == 5,
          "QAM64 R3/4 should keep dense pilots for hardware channel tracking");
}

void test_carrier_geometry() {
    CHECK(pilotCount(59, 10) == 6, "59 carriers with spacing 10 should reserve 6 pilots");
    CHECK(pilotCount(59, 15) == 4, "59 carriers with spacing 15 should reserve 4 pilots");
    CHECK(pilotCount(59, 5) == 12, "59 carriers with spacing 5 should reserve 12 pilots");
    CHECK(pilotCount(59, 0) == 0, "invalid pilot spacing should not divide by zero");
    CHECK(pilotCount(0, 10) == 0, "zero carriers should have zero pilots");

    CHECK(dataCarrierCount(59, false, 10) == 59, "disabled pilots should leave all carriers for data");
    CHECK(dataCarrierCount(59, true, 10) == 53, "59/10 pilot geometry should leave 53 data carriers");
    CHECK(dataCarrierCount(59, true, 15) == 55, "59/15 pilot geometry should leave 55 data carriers");
    CHECK(dataCarrierCount(59, true, 5) == 47, "59/5 pilot geometry should leave 47 data carriers");
    CHECK(dataCarrierCount(59, true, 0) == 59, "invalid pilot spacing should behave like no pilots");
    CHECK(dataCarrierCount(1, true, 1) == 1, "positive carrier plans should keep at least one data carrier");
    CHECK(dataCarrierCount(0, true, 1) == 0, "invalid carrier plans should remain invalid");
}

void test_bits_per_symbol_geometry() {
    CHECK(bitsPerOFDMSymbol(59, true, 10, Modulation::DBPSK) == 53,
          "DBPSK bits per OFDM symbol");
    CHECK(bitsPerOFDMSymbol(59, true, 10, Modulation::DQPSK) == 106,
          "DQPSK bits per OFDM symbol");
    CHECK(bitsPerOFDMSymbol(59, true, 10, Modulation::D8PSK) == 159,
          "D8PSK bits per OFDM symbol");
    CHECK(bitsPerOFDMSymbol(59, true, 10, Modulation::QAM16) == 212,
          "QAM16 bits per OFDM symbol");
    CHECK(bitsPerOFDMSymbol(0, true, 10, Modulation::DQPSK) == 0,
          "invalid carrier count should not produce a usable interleaver geometry");
}

void test_modem_config_geometry() {
    ModemConfig cfg;
    cfg.num_carriers = 59;
    cfg.sample_rate = 48000;
    cfg.fft_size = 1024;
    cfg.cp_mode = CyclicPrefixMode::LONG;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 10;

    CHECK(cfg.getDataCarriers() == 53, "ModemConfig should use ceil-style pilot accounting");

    cfg.pilot_spacing = 0;
    CHECK(cfg.getDataCarriers() == 59, "ModemConfig should guard invalid pilot spacing");

    cfg.num_carriers = 1;
    cfg.pilot_spacing = 1;
    CHECK(cfg.getDataCarriers() == 1, "ModemConfig should preserve one data carrier for tiny configs");

    cfg.num_carriers = 0;
    CHECK(cfg.getDataCarriers() == 0, "ModemConfig should preserve invalid zero-carrier configs");
}

void test_rate_estimate_uses_shared_geometry() {
    ModemConfig cfg;
    cfg.num_carriers = 59;
    cfg.sample_rate = 48000;
    cfg.fft_size = 1024;
    cfg.cp_mode = CyclicPrefixMode::LONG;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 10;

    const float estimated = calculateMaxDataRate(cfg, Modulation::DQPSK, CodeRate::R1_2);
    const float expected = static_cast<float>(cfg.getDataCarriers()) *
                           static_cast<float>(getBitsPerSymbol(Modulation::DQPSK)) *
                           getCodeRateValue(CodeRate::R1_2) *
                           cfg.getSymbolRate() *
                           0.9f;

    CHECK(std::abs(estimated - expected) < 0.01f,
          "calculateMaxDataRate should use ModemConfig carrier geometry");
}

void test_burst_group_policy() {
    CHECK(sanitizeBurstGroupSize(1) == 2, "burst group should clamp low values");
    CHECK(sanitizeBurstGroupSize(4) == 4, "burst group should preserve valid values");
    CHECK(sanitizeBurstGroupSize(99) == 8, "burst group should clamp high values");
    CHECK(recommendedBurstGroupSize(Modulation::D8PSK, CodeRate::R2_3, 0.5f) == 8,
          "D8PSK high-fading profile should use full burst groups");
    CHECK(recommendedBurstGroupSize(Modulation::DQPSK, CodeRate::R1_2, 0.5f) == 8,
          "DQPSK should use full burst groups");
}

}  // namespace

int main() {
    test_pilot_spacing_policy();
    test_carrier_geometry();
    test_bits_per_symbol_geometry();
    test_modem_config_geometry();
    test_rate_estimate_uses_shared_geometry();
    test_burst_group_policy();

    if (tests_failed != 0) {
        std::cout << "OFDMLinkAdaptation: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "OFDMLinkAdaptation: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
