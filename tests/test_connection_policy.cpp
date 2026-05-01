#include "protocol/connection_policy.hpp"

#include <cmath>
#include <iostream>

using namespace ultra;
using namespace ultra::protocol;
using namespace ultra::protocol::connection_policy;

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

void test_fading_labels_and_capabilities() {
    CHECK(std::string(fadingLabel(0.00f)) == "AWGN", "AWGN fading label");
    CHECK(std::string(fadingLabel(0.30f)) == "Good", "Good fading label");
    CHECK(std::string(fadingLabel(0.80f)) == "Moderate", "Moderate fading label");
    CHECK(std::string(fadingLabel(1.20f)) == "Poor", "Poor fading label");

    CHECK(modeToCapabilityBit(WaveformMode::OFDM_COX) == ModeCapabilities::OFDM_COX,
          "OFDM_COX capability bit");
    CHECK(modeToCapabilityBit(WaveformMode::OFDM_CHIRP) == ModeCapabilities::OFDM_CHIRP,
          "OFDM_CHIRP capability bit");
    CHECK(modeToCapabilityBit(WaveformMode::OFDM_NARROW) == ModeCapabilities::OFDM_NARROW,
          "OFDM_NARROW capability bit");
    CHECK(modeToCapabilityBit(WaveformMode::MC_DPSK) == ModeCapabilities::MC_DPSK,
          "MC_DPSK capability bit");
    CHECK(modeToCapabilityBit(WaveformMode::AUTO) == 0, "AUTO has no capability bit");
}

void test_wide_ofdm_timing_and_timeout() {
    auto dqpsk = wideOFDMFrameTiming(Modulation::DQPSK, CodeRate::R1_2);
    CHECK(dqpsk.data_symbols == 27, "DQPSK R1/2 wide OFDM data symbols");
    CHECK(dqpsk.ack_symbols == 9, "DQPSK R1/2 wide OFDM ACK symbols");
    CHECK(dqpsk.data_ms == 648, "DQPSK R1/2 wide OFDM data frame ms");
    CHECK(dqpsk.ack_ms == 216, "DQPSK R1/2 wide OFDM ACK frame ms");

    CHECK(computeWideOFDMAckTimeoutMs(Modulation::DQPSK, CodeRate::R1_2, 4, 120, 2) == 8000,
          "wide OFDM window=4 timeout should clamp to hardware-safe floor");

    CHECK(computeWideOFDMAckTimeoutMs(Modulation::DQPSK, CodeRate::R1_2, 8, 2712, 2) == 13020,
          "wide OFDM window=8 timeout should cover the full burst and ACK path");

    auto d8psk = wideOFDMFrameTiming(Modulation::D8PSK, CodeRate::R1_2);
    CHECK(d8psk.data_symbols == 19, "D8PSK R1/2 wide OFDM data symbols");
    CHECK(d8psk.ack_symbols == 7, "D8PSK R1/2 wide OFDM ACK symbols");
}

void test_narrow_ofdm_timing_and_timeout() {
    auto narrow = narrowOFDMFrameTiming(Modulation::DQPSK);
    CHECK(narrow.data_symbols == 74, "narrow OFDM DQPSK data symbols");
    CHECK(narrow.ack_symbols == 20, "narrow OFDM DQPSK ACK symbols");
    CHECK(narrow.data_ms == 3453, "narrow OFDM DQPSK data frame ms");
    CHECK(narrow.ack_ms == 933, "narrow OFDM DQPSK ACK frame ms");
    CHECK(computeNarrowOFDMAckTimeoutMs(Modulation::DQPSK) == 7165,
          "narrow OFDM DQPSK ACK timeout");
}

void test_ofdm_profile_selection() {
    CHECK(isNearAwgnOFDM(0.00f, 15.0f), "near-AWGN threshold should include SNR15");
    CHECK(!isNearAwgnOFDM(0.30f, 15.0f), "near-AWGN fading threshold is strict");
    CHECK(!isNearAwgnOFDM(0.00f, 14.9f), "near-AWGN SNR threshold is strict");

    CHECK(ofdmWindowSize(true) == kWideOFDMWindowFrames, "near-AWGN OFDM window size");
    CHECK(ofdmWindowSize(false) == kWideOFDMWindowFrames, "fading OFDM window size");
    CHECK(ofdmAckBatchSize(true) == 0, "near-AWGN ACK batch disabled");
    CHECK(ofdmAckBatchSize(false) == 0, "fading ACK batch sentinel");

    auto default_sack = ofdmSackDelays(false, 4, 648);
    CHECK(default_sack.delay_ms == 120 && default_sack.short_delay_ms == 0,
          "default OFDM SACK delay profile");
    auto near_sack = ofdmSackDelays(true, kWideOFDMWindowFrames, 648);
    CHECK(near_sack.delay_ms == 2712 && near_sack.short_delay_ms == 120,
          "near-AWGN OFDM uses burst-tail SACK delay profile");

    auto default_ack = ofdmAckRepeatProfile(Modulation::DQPSK, CodeRate::R1_2, false);
    CHECK(default_ack.count == 2 && default_ack.delay_ms == 220,
          "default OFDM ACK repeat profile");
    auto near_ack = ofdmAckRepeatProfile(Modulation::DQPSK, CodeRate::R1_2, true);
    CHECK(near_ack.count == 2 && near_ack.delay_ms == 220,
          "near-AWGN ACK repeat profile");
    auto d8psk_ack = ofdmAckRepeatProfile(Modulation::D8PSK, CodeRate::R1_2, true);
    CHECK(d8psk_ack.count == 2 && d8psk_ack.delay_ms == 220,
          "D8PSK ACK repeat profile should use conservative default");
}

void test_negotiated_mode_selection() {
    const uint8_t all = ModeCapabilities::ALL;
    CHECK(selectNegotiatedMode(all, all, WaveformMode::MC_DPSK, WaveformMode::AUTO,
                               WaveformMode::AUTO, 20.0f, 0.0f) == WaveformMode::MC_DPSK,
          "remote explicit preference should win when common");

    CHECK(selectNegotiatedMode(all, all, WaveformMode::AUTO, WaveformMode::OFDM_NARROW,
                               WaveformMode::OFDM_CHIRP, 20.0f, 0.0f) == WaveformMode::OFDM_NARROW,
          "narrowband override should outrank local preference");

    CHECK(selectNegotiatedMode(all, all, WaveformMode::AUTO, WaveformMode::AUTO,
                               WaveformMode::OFDM_CHIRP, 20.0f, 0.0f) == WaveformMode::OFDM_CHIRP,
          "local preference should win when common");

    CHECK(selectNegotiatedMode(all, ModeCapabilities::MC_DPSK, WaveformMode::AUTO,
                               WaveformMode::AUTO, WaveformMode::AUTO,
                               20.0f, 0.0f) == WaveformMode::MC_DPSK,
          "fallback should select common maintained mode when recommendation unavailable");

    CHECK(selectNegotiatedMode(0, ModeCapabilities::MC_DPSK, WaveformMode::AUTO,
                               WaveformMode::AUTO, WaveformMode::AUTO,
                               20.0f, 0.0f) == WaveformMode::OFDM_COX,
          "no common modes should preserve OFDM_COX fallback");
}

}  // namespace

int main() {
    test_fading_labels_and_capabilities();
    test_wide_ofdm_timing_and_timeout();
    test_narrow_ofdm_timing_and_timeout();
    test_ofdm_profile_selection();
    test_negotiated_mode_selection();

    if (tests_failed != 0) {
        std::cout << "ConnectionPolicy: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "ConnectionPolicy: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
