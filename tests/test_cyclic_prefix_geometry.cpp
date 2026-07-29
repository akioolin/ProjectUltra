// Pins the cyclic-prefix geometry so prose can never drift from the arithmetic again.
//
// WHY THIS EXISTS (2026-07-28). Three comments in include/ultra/types.hpp documented the
// MEDIUM cyclic prefix as "256 samples = 5.3ms at 1024 FFT". The code computes
// base_cp * (fft_size / 512) = 48 * 2 = 96 samples = 2.00 ms — wrong by 2.67x. That is not
// cosmetic: ITU Poor has a 2.0 ms second path, so production (presets::balanced -> MEDIUM)
// actually runs with ZERO cyclic-prefix margin on Poor, while the false comment implied
// ~3 ms of headroom. Anyone reasoning about out-of-CP ISI from the comment reached the
// opposite conclusion from the truth.
//
// It also pins the PRODUCTION-vs-HARNESS divergence that was found at the same time:
// presets::balanced() (GUI / gui_qso / production) uses MEDIUM = 96 samples, while
// tools/measure_ack_fer.cpp uses LONG = 128. Every FER floor and ladder anchor in the repo
// was measured on the harness geometry, i.e. a 33% longer CP than production ships. This
// test does not resolve which is correct — it makes the divergence impossible to forget.

#include "ultra/types.hpp"

#include <cstdlib>
#include <iostream>

using namespace ultra;

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

uint32_t cpFor(CyclicPrefixMode mode, uint32_t fft_size) {
    ModemConfig cfg;
    cfg.fft_size = fft_size;
    cfg.cp_mode = mode;
    return cfg.getCyclicPrefix();
}

// 48 kHz is the modem's only sample rate (docs/ + ModemConfig default).
float cpMs(CyclicPrefixMode mode, uint32_t fft_size) {
    return static_cast<float>(cpFor(mode, fft_size)) * 1000.0f / 48000.0f;
}

void testBaseValuesAt512() {
    CHECK(cpFor(CyclicPrefixMode::SHORT, 512) == 32, "SHORT base @512 == 32");
    CHECK(cpFor(CyclicPrefixMode::MEDIUM, 512) == 48, "MEDIUM base @512 == 48");
    CHECK(cpFor(CyclicPrefixMode::LONG, 512) == 64, "LONG base @512 == 64");
}

void testScalingAt1024() {
    // getCyclicPrefix scales by (fft_size / 512), so 1024 DOUBLES every base value.
    CHECK(cpFor(CyclicPrefixMode::SHORT, 1024) == 64, "SHORT @1024 == 64 samples");
    CHECK(cpFor(CyclicPrefixMode::MEDIUM, 1024) == 96, "MEDIUM @1024 == 96 samples");
    CHECK(cpFor(CyclicPrefixMode::LONG, 1024) == 128, "LONG @1024 == 128 samples");

    // The three corrected comments, in milliseconds.
    CHECK(std::abs(cpMs(CyclicPrefixMode::SHORT, 1024) - 1.3333f) < 0.01f,
          "SHORT @1024 == 1.33 ms");
    CHECK(std::abs(cpMs(CyclicPrefixMode::MEDIUM, 1024) - 2.0f) < 0.01f,
          "MEDIUM @1024 == 2.00 ms (NOT the 5.3 ms the old comment claimed)");
    CHECK(std::abs(cpMs(CyclicPrefixMode::LONG, 1024) - 2.6667f) < 0.01f,
          "LONG @1024 == 2.67 ms");
}

void testMediumIsNotFivePointThree() {
    // The specific false claim, pinned so it cannot come back.
    CHECK(cpFor(CyclicPrefixMode::MEDIUM, 1024) != 256,
          "MEDIUM @1024 is NOT 256 samples (the retracted comment)");
    CHECK(cpMs(CyclicPrefixMode::MEDIUM, 1024) < 3.0f,
          "MEDIUM @1024 is NOT 5.3 ms (the retracted comment)");
}

void testProductionPresetGeometry() {
    // presets::balanced() is what the GUI, gui_qso_scenario.sh and production all run.
    ModemConfig prod = presets::balanced();
    CHECK(prod.fft_size == 1024, "balanced() uses a 1024 FFT");
    CHECK(prod.cp_mode == CyclicPrefixMode::MEDIUM, "balanced() uses MEDIUM cp_mode");
    CHECK(prod.getCyclicPrefix() == 96, "PRODUCTION cyclic prefix == 96 samples");

    const float prod_cp_ms = static_cast<float>(prod.getCyclicPrefix()) * 1000.0f / 48000.0f;
    CHECK(std::abs(prod_cp_ms - 2.0f) < 0.01f, "PRODUCTION cyclic prefix == 2.00 ms");

    // ITU Poor's second path is 2.0 ms (src/ota_channel_core/models.cpp poor()).
    // Production therefore has ZERO margin there. This is a documented fact, not a
    // requirement — the assertion exists so that if anyone changes the production CP,
    // they are forced to notice they changed the Poor-channel ISI situation.
    CHECK(prod_cp_ms <= 2.0f + 0.01f,
          "production CP does not exceed ITU Poor's 2.0 ms delay (zero margin, by design)");
}

void testHarnessDivergenceIsRealAndKnown() {
    // tools/measure_ack_fer.cpp sets cp_mode = LONG. Every FER floor and every ladder
    // anchor in the repo was measured with THIS geometry, not the production one.
    const uint32_t harness_cp = cpFor(CyclicPrefixMode::LONG, 1024);
    const uint32_t production_cp = presets::balanced().getCyclicPrefix();
    CHECK(harness_cp == 128, "measure_ack_fer geometry (LONG @1024) == 128 samples");
    CHECK(harness_cp != production_cp,
          "harness CP DIVERGES from production CP -- known, tracked, unresolved");
    CHECK(harness_cp * 100 / production_cp == 133,
          "harness CP is 33% longer than production");
}

}  // namespace

int main() {
    testBaseValuesAt512();
    testScalingAt1024();
    testMediumIsNotFivePointThree();
    testProductionPresetGeometry();
    testHarnessDivergenceIsRealAndKnown();

    std::cout << (tests_failed ? "FAILED" : "PASS") << ": cyclic-prefix geometry ("
              << (tests_run - tests_failed) << "/" << tests_run << ")\n";
    return tests_failed ? 1 : 0;
}
