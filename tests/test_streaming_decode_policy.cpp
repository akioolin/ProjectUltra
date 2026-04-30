#include "gui/modem/streaming_decode_policy.hpp"

#include <iostream>

using namespace ultra;
using namespace ultra::gui::streaming_decode_policy;

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

void test_robust_ofdm_control_samples() {
    CHECK(estimateRobustOFDMControlSamples(8000, Modulation::DQPSK, CodeRate::R1_4, 59, 1152) == 8000,
          "native DQPSK R1/4 control profile should use waveform default");

    const size_t robust = estimateRobustOFDMControlSamples(
        8000, Modulation::D8PSK, CodeRate::R2_3, 59, 1152);
    CHECK(robust == 10368, "high-rate data mode should reserve enough samples for robust DQPSK control");

    CHECK(estimateRobustOFDMControlSamples(12000, Modulation::D8PSK, CodeRate::R2_3, 59, 1152) == 12000,
          "robust estimate should never reduce waveform default");
    CHECK(estimateRobustOFDMControlSamples(8000, Modulation::D8PSK, CodeRate::R2_3, 0, 1152) == 8000,
          "invalid carrier count should fall back to waveform default");
    CHECK(estimateRobustOFDMControlSamples(8000, Modulation::D8PSK, CodeRate::R2_3, 59, 0) == 8000,
          "invalid symbol size should fall back to waveform default");
}

void test_decode_sample_requirement_selection() {
    auto pending = selectDecodeSampleRequirement(
        4, true, true, true, true,
        44000, 11000, 50000, 9000);
    CHECK(pending.samples == 44000, "known pending CW count should use exact pending sample count");
    CHECK(pending.mode == DecodeSampleMode::PendingCodewords, "pending mode");

    auto ofdm_peek = selectDecodeSampleRequirement(
        0, true, true, true, false,
        0, 11000, 50000, 9000);
    CHECK(ofdm_peek.samples == 11000, "connected OFDM should first use robust control peek");
    CHECK(ofdm_peek.mode == DecodeSampleMode::ConnectedOFDMPeek, "OFDM peek mode");

    auto burst = selectDecodeSampleRequirement(
        0, true, true, true, true,
        0, 11000, 50000, 9000);
    CHECK(burst.samples == 50000, "latched burst marker should require full frame samples");
    CHECK(burst.mode == DecodeSampleMode::ConnectedOFDMBurst, "burst mode");

    auto burst_disabled = selectDecodeSampleRequirement(
        0, true, true, false, true,
        0, 11000, 50000, 9000);
    CHECK(burst_disabled.samples == 11000, "burst marker should be ignored when burst interleave is disabled");

    auto mcdpsk = selectDecodeSampleRequirement(
        0, false, true, false, false,
        0, 11000, 50000, 9000);
    CHECK(mcdpsk.samples == 9000, "MC-DPSK should use control/header peek samples");
    CHECK(mcdpsk.mode == DecodeSampleMode::ControlPeek, "MC-DPSK control mode");

    auto disconnected_ofdm = selectDecodeSampleRequirement(
        0, true, false, true, true,
        0, 11000, 50000, 9000);
    CHECK(disconnected_ofdm.samples == 9000, "disconnected path should use control/header peek samples");
}

}  // namespace

int main() {
    test_robust_ofdm_control_samples();
    test_decode_sample_requirement_selection();

    if (tests_failed != 0) {
        std::cout << "StreamingDecodePolicy: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "StreamingDecodePolicy: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
