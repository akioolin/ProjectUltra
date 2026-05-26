#include "gui/modem/streaming_decode_policy.hpp"

#include <iostream>

using namespace ultra;
using namespace ultra::gui::streaming_decode_policy;
using namespace ultra::gui;

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

    const size_t robust_d8psk = estimateRobustOFDMControlSamples(
        8000, Modulation::D8PSK, CodeRate::R2_3, 59, 1152);
    CHECK(robust_d8psk == 10368,
          "differential high-rate data mode should reserve enough samples for DQPSK control");

    const auto qpsk_control =
        streaming_control_profile::profileForDataMode(Modulation::QPSK);
    CHECK(qpsk_control.modulation == Modulation::QPSK,
          "coherent data should use coherent QPSK control");
    CHECK(qpsk_control.rate == CodeRate::R1_4,
          "coherent control should retain hardened R1/4 FEC");
    const size_t robust_qpsk = estimateRobustOFDMControlSamples(
        8000, Modulation::QPSK, CodeRate::R2_3, 59, 1152);
    CHECK(robust_qpsk == 10368,
          "QPSK R2/3 data should reserve enough samples for QPSK R1/4 control");

    const auto qpsk_control_preconfirm =
        streaming_control_profile::profileForDataMode(Modulation::QPSK, false);
    CHECK(qpsk_control_preconfirm.modulation == Modulation::DQPSK,
          "unconfirmed OFDM handoff should retain legacy DQPSK control");
    const size_t legacy_preconfirm = estimateRobustOFDMControlSamples(
        8000, Modulation::QPSK, CodeRate::R2_3, 59, 1152, false);
    CHECK(legacy_preconfirm == 10368,
          "pre-confirmation OFDM control sizing should use legacy DQPSK R1/4 sizing");

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

void test_qam16_control_peek_is_subfixed() {
    constexpr size_t LDPC_BLOCK = 648;
    constexpr int CARRIERS = 59;
    constexpr int SAMPLES_PER_SYMBOL = 1152;
    constexpr int FIXED_CW = 4;

    const int qam16_bits_per_symbol =
        ofdm_link_adaptation::bitsPerOFDMSymbol(CARRIERS, true, 5, Modulation::QAM16);
    const size_t qam16_control_symbols =
        2 + ((LDPC_BLOCK + qam16_bits_per_symbol - 1) / qam16_bits_per_symbol);
    const size_t qam16_control_default = qam16_control_symbols * SAMPLES_PER_SYMBOL;
    const size_t robust_samples = estimateRobustOFDMControlSamples(
        qam16_control_default, Modulation::QAM16, CodeRate::R1_2,
        CARRIERS, SAMPLES_PER_SYMBOL);
    CHECK(robust_samples == 10368,
          "QAM16 data mode should use the robust coherent-QPSK control-sized peek");

    const size_t data_symbols =
        robust_samples / static_cast<size_t>(SAMPLES_PER_SYMBOL) - 2;
    const size_t rounded_soft_bits =
        (data_symbols * static_cast<size_t>(qam16_bits_per_symbol) / LDPC_BLOCK) * LDPC_BLOCK;

    CHECK(rounded_soft_bits == 2 * LDPC_BLOCK,
          "QAM16 control-sized peek should yield exactly two complete CWs");
    CHECK(hasSubFixedFrameSoftBits(rounded_soft_bits, FIXED_CW, LDPC_BLOCK),
          "two complete CWs should still escalate for a 4-CW fixed frame");
    CHECK(!hasSubFixedFrameSoftBits(FIXED_CW * LDPC_BLOCK, FIXED_CW, LDPC_BLOCK),
          "a full fixed-frame soft-bit buffer should not be treated as a peek");
}

}  // namespace

int main() {
    test_robust_ofdm_control_samples();
    test_decode_sample_requirement_selection();
    test_qam16_control_peek_is_subfixed();

    if (tests_failed != 0) {
        std::cout << "StreamingDecodePolicy: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "StreamingDecodePolicy: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
