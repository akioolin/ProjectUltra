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

    auto dqpsk_6cw = wideOFDMFrameTiming(Modulation::DQPSK, CodeRate::R1_2, 6);
    auto dqpsk_8cw = wideOFDMFrameTiming(Modulation::DQPSK, CodeRate::R1_2, 8);
    CHECK(dqpsk_6cw.data_symbols == 39, "DQPSK R1/2 6-CW wide OFDM data symbols");
    CHECK(dqpsk_8cw.data_symbols == 51, "DQPSK R1/2 8-CW wide OFDM data symbols");
    CHECK(dqpsk_6cw.data_ms > dqpsk.data_ms && dqpsk_8cw.data_ms > dqpsk_6cw.data_ms,
          "wide OFDM data duration should scale with fixed-frame CW count");

    CHECK(computeWideOFDMAckTimeoutMs(Modulation::DQPSK, CodeRate::R1_2, 4, 120, 2) == 8000,
          "wide OFDM window=4 timeout should clamp to hardware-safe floor");

    CHECK(computeWideOFDMAckTimeoutMs(Modulation::DQPSK, CodeRate::R1_2, 8, 120, 1) == 8000,
          "wide OFDM window=8 timeout should keep default-CW floor after burst accounting");
    CHECK(computeWideOFDMAckTimeoutMs(Modulation::DQPSK, CodeRate::R1_2, 8, 120, 1, 6) == 9224,
          "wide OFDM 6-CW ACK timeout should cover the longer burst");
    CHECK(computeWideOFDMAckTimeoutMs(Modulation::DQPSK, CodeRate::R1_2, 8, 120, 1, 8) == 11528,
          "wide OFDM 8-CW ACK timeout should cover the longer burst");

    const auto sack_4cw = ofdmSackDelays(true, kHighThroughputOFDMWindowFrames, dqpsk.data_ms);
    const uint32_t timeout_4cw = computeWideOFDMAckTimeoutMs(
        Modulation::DQPSK, CodeRate::R1_2, kHighThroughputOFDMWindowFrames,
        sack_4cw.delay_ms, 1);
    CHECK(timeout_4cw == 16000,
          "wide OFDM window=16 4-CW timeout should preserve the current ceiling");

    const auto sack_6cw = ofdmSackDelays(true, kHighThroughputOFDMWindowFrames, dqpsk_6cw.data_ms);
    const uint32_t timeout_6cw = computeWideOFDMAckTimeoutMs(
        Modulation::DQPSK, CodeRate::R1_2, kHighThroughputOFDMWindowFrames,
        sack_6cw.delay_ms, 1, 6);
    const uint32_t min_6cw_ack_path =
        static_cast<uint32_t>(kHighThroughputOFDMWindowFrames) * dqpsk_6cw.data_ms +
        sack_6cw.delay_ms + dqpsk_6cw.ack_ms;
    CHECK(timeout_6cw >= min_6cw_ack_path,
          "wide OFDM window=16 6-CW timeout should cover burst plus ACK path");

    const auto sack_8cw = ofdmSackDelays(true, kHighThroughputOFDMWindowFrames, dqpsk_8cw.data_ms);
    const uint32_t timeout_8cw = computeWideOFDMAckTimeoutMs(
        Modulation::DQPSK, CodeRate::R1_2, kHighThroughputOFDMWindowFrames,
        sack_8cw.delay_ms, 1, 8);
    const uint32_t min_8cw_ack_path =
        static_cast<uint32_t>(kHighThroughputOFDMWindowFrames) * dqpsk_8cw.data_ms +
        sack_8cw.delay_ms + dqpsk_8cw.ack_ms;
    CHECK(timeout_8cw >= min_8cw_ack_path,
          "wide OFDM window=16 8-CW timeout should cover burst plus ACK path");

    const auto connect_ack_r23 =
        connectAckRetransmitDelayMs(WaveformMode::OFDM_CHIRP,
                                    Modulation::DQPSK,
                                    CodeRate::R2_3);
    const auto dqpsk_r23 = wideOFDMFrameTiming(Modulation::DQPSK, CodeRate::R2_3);
    const uint32_t first_group_r23_ms =
        kResponderHandshakeFailSafeMs +
        kBurstInterleaveGroupFrames * dqpsk_r23.data_ms;
    CHECK(connect_ack_r23 > first_group_r23_ms,
          "OFDM CONNECT_ACK rescue retry should wait for first burst group");
    CHECK(connectAckRetransmitDelayMs(WaveformMode::MC_DPSK,
                                      Modulation::DQPSK,
                                      CodeRate::R1_4) == kConnectAckLegacyRetransmitMs,
          "non-OFDM CONNECT_ACK retry should keep legacy delay");

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
    auto narrow_8cw = narrowOFDMFrameTiming(Modulation::DQPSK, 8);
    CHECK(narrow_8cw.data_ms > narrow.data_ms,
          "narrow OFDM data duration should scale with fixed-frame CW count");
    CHECK(computeNarrowOFDMAckTimeoutMs(Modulation::DQPSK) == 7165,
          "narrow OFDM DQPSK ACK timeout (window=1, default)");
    CHECK(computeNarrowOFDMAckTimeoutMs(Modulation::DQPSK, 8) >
              computeNarrowOFDMAckTimeoutMs(Modulation::DQPSK),
          "narrow OFDM ACK timeout should scale with fixed-frame CW count");

    // Selective-repeat window=2/3 added 2026-05-03 per Codex audit. The
    // timeout has to cover the full multi-frame TX burst plus ACK
    // turnaround — without scaling, the ARQ would fire while later
    // frames are still on-air and trigger a phantom timeout retry.
    const uint32_t w1 = computeNarrowOFDMAckTimeoutMs(Modulation::DQPSK, 4, 1);
    const uint32_t w2 = computeNarrowOFDMAckTimeoutMs(Modulation::DQPSK, 4, 2);
    const uint32_t w3 = computeNarrowOFDMAckTimeoutMs(Modulation::DQPSK, 4, 3);
    CHECK(w1 == 7165, "window=1 timeout matches default behavior");
    CHECK(w2 > w1, "window=2 timeout scales above window=1");
    CHECK(w3 > w2, "window=3 timeout scales above window=2");
    // For 4-CW DQPSK narrow, data_ms=3453, ack_ms=933, so:
    //   window=2: tx_burst_ms = 2*3453 = 6906; timeout = 6906 + 2*933
    //             + 120 + max(700, 1726) = 10618 ms
    //   window=3: tx_burst_ms = 3*3453 = 10359; timeout = 10359 + 2*933
    //             + 120 + max(700, 1726) = 14071 ms (current narrow default)
    CHECK(w2 >= 10000 && w2 <= 11000, "window=2 timeout ~10.6 s");
    CHECK(w3 >= 13500 && w3 <= 14500, "window=3 timeout ~14.0 s (current narrow default)");
}

void test_ofdm_profile_selection() {
    CHECK(isNearAwgnOFDM(0.00f, 15.0f), "near-AWGN threshold should include SNR15");
    CHECK(isNearAwgnOFDM(0.14f, 15.0f), "near-AWGN fading threshold should allow R2/3 cutoff margin");
    CHECK(!isNearAwgnOFDM(0.15f, 15.0f), "near-AWGN fading threshold should match R2/3 cutoff");
    CHECK(!isNearAwgnOFDM(0.30f, 15.0f), "near-AWGN fading threshold is strict");
    CHECK(!isNearAwgnOFDM(0.00f, 14.9f), "near-AWGN SNR threshold is strict");
    CHECK(isHighThroughputOFDM(0.30f, 15.0f), "Good fading SNR15 should use high-throughput OFDM window");
    CHECK(!isHighThroughputOFDM(0.65f, 15.0f), "Moderate fading should not use high-throughput OFDM window yet");
    CHECK(!isHighThroughputOFDM(0.30f, 14.9f), "high-throughput OFDM SNR threshold is strict");

    CHECK(isHighThroughputOFDMMode(Modulation::DQPSK, CodeRate::R1_2),
          "DQPSK R1/2 should use high-throughput OFDM mode");
    CHECK(!isHighThroughputOFDMMode(Modulation::DQPSK, CodeRate::R1_4),
          "DQPSK R1/4 should keep default OFDM mode");
    CHECK(ofdmWindowSize(Modulation::DQPSK, CodeRate::R1_2) == kHighThroughputOFDMWindowFrames,
          "high-throughput OFDM window size");
    CHECK(ofdmWindowSize(Modulation::DQPSK, CodeRate::R1_4) == kWideOFDMWindowFrames,
          "default OFDM window size");
    CHECK(ofdmWindowSize(Modulation::DQPSK, CodeRate::R2_3, true) == kHighThroughputOFDMWindowFrames,
          "R2/3 can use two burst groups only near AWGN");
    CHECK(ofdmWindowSize(Modulation::DQPSK, CodeRate::R2_3, false) == kWideOFDMWindowFrames,
          "legacy R2/3 window helper should stay conservative on fading channels");
    CHECK(ofdmWindowSizeForChannel(Modulation::DQPSK, CodeRate::R2_3, 0.30f, 15.0f)
              == kWideOFDMWindowFrames,
          "R2/3 should keep one burst group on good fading at SNR15");
    CHECK(ofdmWindowSizeForChannel(Modulation::DQPSK, CodeRate::R2_3, 0.05f, 15.0f)
              == kHighThroughputOFDMWindowFrames,
          "R2/3 can use two burst groups only on near-AWGN channels");
    CHECK(ofdmWindowSizeForChannel(Modulation::DQPSK, CodeRate::R2_3, 0.80f, 15.0f)
              == kWideOFDMWindowFrames,
          "R2/3 should keep one burst group on moderate fading");
    CHECK(ofdmWindowSizeForChannel(Modulation::DQPSK, CodeRate::R3_4, 0.30f, 15.0f)
              == kWideOFDMWindowFrames,
          "R3/4 remains near-AWGN only for two burst groups");
    CHECK(ofdmWindowSize(Modulation::DQPSK, CodeRate::R1_2, false) == kHighThroughputOFDMWindowFrames,
          "R1/2 keeps the high-throughput OFDM window in fading");
    CHECK(!shouldPadHighRateFadingBurst(Modulation::DQPSK, CodeRate::R2_3, false, 1),
          "single high-rate fading frame should not be padded");
    CHECK(shouldPadHighRateFadingBurst(Modulation::DQPSK, CodeRate::R2_3, false, 2),
          "partial high-rate fading burst should be padded");
    CHECK(shouldPadHighRateFadingBurst(Modulation::DQPSK, CodeRate::R2_3, false, 7),
          "short high-rate fading burst should fill the interleaver group");
    CHECK(!shouldPadHighRateFadingBurst(Modulation::DQPSK, CodeRate::R2_3, false, 8),
          "full high-rate fading burst should not be padded");
    CHECK(shouldPadHighRateFadingBurst(Modulation::DQPSK, CodeRate::R2_3, false, 9),
          "multi-group high-rate fading tail should be padded");
    CHECK(!shouldPadHighRateFadingBurst(Modulation::DQPSK, CodeRate::R2_3, true, 7),
          "near-AWGN high-rate burst should not be padded");
    CHECK(!shouldPadHighRateFadingBurst(Modulation::DQPSK, CodeRate::R1_2, false, 7),
          "R1/2 fading burst should not use speculative high-rate padding");
    // After 2026-05-04 D8PSK ladder re-enable, D8PSK R2/3 is now a
    // speculative high-rate OFDM mode (same window/padding policy as
    // DQPSK R2/3). Padding fires for partial high-rate fading bursts.
    CHECK(shouldPadHighRateFadingBurst(Modulation::D8PSK, CodeRate::R2_3, false, 7),
          "D8PSK R2/3 fading partial burst should pad like DQPSK R2/3");
    CHECK(!shouldPadHighRateFadingBurst(Modulation::D8PSK, CodeRate::R1_2, false, 7),
          "D8PSK R1/2 is high-throughput non-speculative, no padding");
    CHECK(!shouldPadHighRateFadingBurst(Modulation::QPSK, CodeRate::R2_3, false, 7),
          "non-(DQPSK/D8PSK) high-rate burst should not use padding policy");
    CHECK(!shouldPadBurstInterleaveGroup(1),
          "single frame does not need burst-interleaver padding");
    CHECK(shouldPadBurstInterleaveGroup(7),
          "partial burst-interleaver group should be padded");
    CHECK(!shouldPadBurstInterleaveGroup(kBurstInterleaveGroupFrames),
          "complete burst-interleaver group should not be padded");
    CHECK(ofdmAckBatchSize(true) == 0, "near-AWGN ACK batch disabled");
    CHECK(ofdmAckBatchSize(false) == 0, "fading ACK batch sentinel");

    auto default_sack = ofdmSackDelays(false, 4, 648);
    CHECK(default_sack.delay_ms == 120 && default_sack.short_delay_ms == 0,
          "default OFDM SACK delay profile");
    auto near_sack = ofdmSackDelays(true, kWideOFDMWindowFrames, 648);
    CHECK(near_sack.delay_ms == 120 && near_sack.short_delay_ms == 120,
          "single-group high-throughput OFDM can ACK at burst tail");
    auto wide_sack = ofdmSackDelays(true, kHighThroughputOFDMWindowFrames, 648);
    CHECK(wide_sack.delay_ms == 5304 && wide_sack.short_delay_ms == 120,
          "two-group high-throughput OFDM defers SACK until burst tail");

    auto default_ack = ofdmAckRepeatProfile(Modulation::DQPSK, CodeRate::R1_2, false);
    CHECK(default_ack.count == 2 && default_ack.delay_ms == 220,
          "default OFDM ACK repeat profile");
    auto near_ack = ofdmAckRepeatProfile(Modulation::DQPSK, CodeRate::R1_2, true);
    CHECK(near_ack.count == 1 && near_ack.delay_ms == 220,
          "near-AWGN ACK repeat profile");
    auto d8psk_ack = ofdmAckRepeatProfile(Modulation::D8PSK, CodeRate::R1_2, true);
    CHECK(d8psk_ack.count == 1 && d8psk_ack.delay_ms == 220,
          "near-AWGN D8PSK ACK repeat profile");
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

void test_recommend_cw_count() {
    // Wide OFDM: rate-based promotion (R1/2, R2/3, R3/4 → 8; R1/4 → 4).
    CHECK(recommendCWCount(CodeRate::R1_2, WaveformMode::OFDM_CHIRP) == 8,
          "wide R1/2 → 8 (the +50% throughput win)");
    CHECK(recommendCWCount(CodeRate::R2_3, WaveformMode::OFDM_CHIRP) == 8,
          "wide R2/3 → 8");
    CHECK(recommendCWCount(CodeRate::R3_4, WaveformMode::OFDM_CHIRP) == 8,
          "wide R3/4 → 8");
    CHECK(recommendCWCount(CodeRate::R1_4, WaveformMode::OFDM_CHIRP) ==
              v2::kDefaultFixedFrameCodewords,
          "wide R1/4 stays at default 4 (low-SNR robustness)");

    // OFDM_COX shares the wide policy.
    CHECK(recommendCWCount(CodeRate::R1_2, WaveformMode::OFDM_COX) == 8,
          "OFDM_COX R1/2 follows the wide policy");

    // OFDM_NARROW: always default 4. Narrow R1/2 frames are ~6 s at CW=8;
    // window=3 burst would be ~18 s, exceeding typical narrow good-fading
    // coherence (~10 s). 3-seed sim A/B at SNR=8 R1/2 confirmed CW=8
    // hit 1/3 catastrophic FAIL (240 s timeout) while CW=4 was 3/3 PASS.
    for (auto rate : {CodeRate::R1_4, CodeRate::R1_2, CodeRate::R2_3, CodeRate::R3_4}) {
        CHECK(recommendCWCount(rate, WaveformMode::OFDM_NARROW) ==
                  v2::kDefaultFixedFrameCodewords,
              "narrow always caps at default 4 (fade-coherence limit)");
    }
}

}  // namespace

int main() {
    test_fading_labels_and_capabilities();
    test_wide_ofdm_timing_and_timeout();
    test_narrow_ofdm_timing_and_timeout();
    test_ofdm_profile_selection();
    test_negotiated_mode_selection();
    test_recommend_cw_count();

    if (tests_failed != 0) {
        std::cout << "ConnectionPolicy: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "ConnectionPolicy: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
