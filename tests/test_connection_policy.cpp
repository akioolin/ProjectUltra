#include "protocol/connection_policy.hpp"
#include "protocol/selective_repeat_arq_policy.hpp"

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
    CHECK(classifyChannel(0.14f) == ChannelClassification::AWGN,
          "channel classifier should preserve AWGN threshold");
    CHECK(classifyChannel(0.15f) == ChannelClassification::GOOD,
          "channel classifier should enter Good at 0.15 fading index");
    CHECK(classifyChannel(0.65f) == ChannelClassification::MODERATE,
          "channel classifier should enter Moderate at 0.65 fading index");
    CHECK(classifyChannel(1.10f) == ChannelClassification::POOR,
          "channel classifier should enter Poor at 1.10 fading index");

    CHECK(modeToCapabilityBit(WaveformMode::OFDM_CHIRP) == ModeCapabilities::OFDM_CHIRP,
          "OFDM_CHIRP capability bit");
    CHECK(modeToCapabilityBit(WaveformMode::OFDM_NARROW) == ModeCapabilities::OFDM_NARROW,
          "OFDM_NARROW capability bit");
    CHECK(modeToCapabilityBit(WaveformMode::MC_DPSK) == ModeCapabilities::MC_DPSK,
          "MC_DPSK capability bit");
    CHECK(modeToCapabilityBit(WaveformMode::AUTO) == 0, "AUTO has no capability bit");
}

void test_ladder_rung_selection() {
    const auto robust_low = ladderRungForId(LadderRungId::ROBUST_LOW);
    CHECK(robust_low.waveform == WaveformMode::MC_DPSK, "Robust-Low waveform");
    CHECK(robust_low.modulation == Modulation::DBPSK, "Robust-Low modulation");
    CHECK(robust_low.samples_per_symbol == 2048, "Robust-Low SPS");
    CHECK(robust_low.cw_count == 3, "Robust-Low CW count");

    const auto robust_mid = ladderRungForId(LadderRungId::ROBUST_MID);
    CHECK(robust_mid.waveform == WaveformMode::MC_DPSK, "Robust-Mid waveform");
    CHECK(robust_mid.modulation == Modulation::DBPSK, "Robust-Mid modulation");
    CHECK(robust_mid.samples_per_symbol == 1024, "Robust-Mid SPS");
    CHECK(robust_mid.cw_count == 3, "Robust-Mid CW count");

    const auto robust = ladderRungForId(LadderRungId::ROBUST);
    CHECK(robust.waveform == WaveformMode::MC_DPSK, "Robust waveform");
    CHECK(robust.modulation == Modulation::DQPSK, "Robust modulation");
    CHECK(robust.samples_per_symbol == 1024, "Robust SPS");
    CHECK(robust.cw_count == v2::kDefaultFixedFrameCodewords, "Robust CW count");

    const auto standard = ladderRungForId(LadderRungId::STANDARD);
    CHECK(standard.waveform == WaveformMode::MC_DPSK, "Standard waveform");
    CHECK(standard.modulation == Modulation::DQPSK, "Standard modulation");
    CHECK(standard.samples_per_symbol == 512, "Standard SPS");

    CHECK(ladderRungForId(LadderRungId::OFDM_CHIRP).waveform == WaveformMode::OFDM_CHIRP,
          "OFDM_CHIRP rung waveform");
    CHECK(std::string(ladderRungIdToString(LadderRungId::ROBUST_LOW)) == "Robust-Low",
          "Robust-Low rung string");

    CHECK(selectLadderRung(6.9f, ChannelClassification::MODERATE).id ==
              LadderRungId::ROBUST_LOW,
          "Moderate below in-band 7 dB selects Robust-Low");
    CHECK(selectLadderRung(7.0f, ChannelClassification::MODERATE).id ==
              LadderRungId::ROBUST_MID,
          "Moderate in-band 7 dB boundary selects Robust-Mid");
    CHECK(selectLadderRung(13.9f, ChannelClassification::MODERATE).id ==
              LadderRungId::ROBUST_MID,
          "Moderate below in-band 14 dB stays Robust-Mid");
    CHECK(selectLadderRung(14.0f, ChannelClassification::MODERATE).id ==
              LadderRungId::OFDM_CHIRP,
          "Moderate in-band 14 dB boundary selects OFDM_CHIRP");

    CHECK(selectLadderRung(9.9f, ChannelClassification::AWGN).id ==
              LadderRungId::ROBUST_MID,
          "AWGN below in-band 10 dB stays MC-DPSK");
    CHECK(selectLadderRung(10.0f, ChannelClassification::AWGN).id ==
              LadderRungId::OFDM_CHIRP,
          "AWGN in-band 10 dB boundary selects OFDM_CHIRP");
    CHECK(selectLadderRung(11.9f, ChannelClassification::GOOD).id ==
              LadderRungId::ROBUST_MID,
          "Good fading below in-band 12 dB stays MC-DPSK");
    CHECK(selectLadderRung(12.0f, ChannelClassification::GOOD).id ==
              LadderRungId::OFDM_CHIRP,
          "Good fading in-band 12 dB boundary selects OFDM_CHIRP");
    CHECK(selectLadderRung(16.9f, ChannelClassification::POOR).id ==
              LadderRungId::ROBUST_MID,
          "Poor fading keeps extra margin before Robust");
    CHECK(selectLadderRung(17.0f, ChannelClassification::POOR).id ==
              LadderRungId::ROBUST,
          "Poor fading keeps Robust below the OFDM_CHIRP floor");
    CHECK(selectLadderRung(18.0f, ChannelClassification::POOR).id ==
              LadderRungId::OFDM_CHIRP,
          "Poor fading delays OFDM_CHIRP until in-band 18 dB");

    CHECK(selectLadderRung(10.0f, 0.80f).id == LadderRungId::ROBUST_MID,
          "fading-index overload selects Moderate Robust-Mid at in-band 10 dB");
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

    CHECK(kWideOFDMFullAnchorExtraMs == 1200,
          "wide OFDM full chirp anchor should add 1.2 s to multi-frame bursts");
    CHECK(wideOFDMBurstAirtimeMs(Modulation::DQPSK, CodeRate::R1_2, 1) == dqpsk.data_ms,
          "single wide OFDM light frame should not include a burst chirp anchor");

    const uint32_t w8_burst_ms = wideOFDMBurstAirtimeMs(
        Modulation::DQPSK, CodeRate::R1_2, 8);
    CHECK(w8_burst_ms == 8 * dqpsk.data_ms + kWideOFDMFullAnchorExtraMs,
          "wide OFDM multi-frame burst airtime should include the first-frame chirp anchor");

    const uint32_t w8_sack_delay = wideOFDMSackDelayMs(
        Modulation::DQPSK, CodeRate::R1_2, 8);
    CHECK(w8_sack_delay == w8_burst_ms + kCarrierSenseSackCoalesceMs,
          "wide OFDM SACK delay should hold ACKs through a physical sender window");

    const auto qpsk_r23_8cw = wideOFDMFrameTiming(Modulation::QPSK, CodeRate::R2_3, 8);
    const uint32_t qpsk_r23_w8_burst_ms = wideOFDMBurstAirtimeMs(
        Modulation::QPSK, CodeRate::R2_3, 8, 8);
    const uint32_t qpsk_r23_w8_reanchor_burst_ms = wideOFDMBurstAirtimeMs(
        Modulation::QPSK, CodeRate::R2_3, 8, 8, kWideOFDMShortReanchorDefaultMs);
    CHECK(qpsk_r23_w8_burst_ms == 8 * qpsk_r23_8cw.data_ms + kWideOFDMFullAnchorExtraMs,
          "coherent QPSK R2/3 8-CW burst airtime should include the first-frame chirp anchor");
    CHECK(qpsk_r23_w8_reanchor_burst_ms ==
              qpsk_r23_w8_burst_ms + 7 * kWideOFDMShortReanchorDefaultMs,
          "adaptive short reanchors should extend every continuation frame in the physical burst");
    CHECK(wideOFDMSackDelayMs(Modulation::QPSK, CodeRate::R2_3, 8, 8,
                              kWideOFDMShortReanchorDefaultMs) ==
              qpsk_r23_w8_reanchor_burst_ms + kCarrierSenseSackCoalesceMs,
          "coherent fading SACK delay should hold through adaptive reanchor burst airtime");

    const auto qam16_r14 = wideOFDMFrameTiming(Modulation::QAM16, CodeRate::R1_4);
    CHECK(wideOFDMSlidingSackDelayMs(Modulation::QAM16, CodeRate::R1_4) ==
              qam16_r14.data_ms + qam16_r14.ack_ms + kCarrierSenseSackCoalesceMs,
          "wide OFDM sliding SACK delay should derive from selected data and ACK airtime");
    CHECK(wideOFDMSlidingSackDelayMs(Modulation::QAM16, CodeRate::R1_4) <
              wideOFDMSackDelayMs(Modulation::QAM16, CodeRate::R1_4, 8),
          "wide OFDM sliding SACK delay should be a burst-tail quiet interval, not a full-window hold");

    const uint32_t qam16_ack_repeat_tail =
        selective_repeat_arq_policy::ackRepeatTailGuardMs(
            qam16_r14.ack_ms,
            wideOFDMSlidingSackDelayMs(Modulation::QAM16, CodeRate::R1_4),
            80,
            3,
            true);
    CHECK(qam16_ack_repeat_tail >=
              wideOFDMSlidingSackDelayMs(Modulation::QAM16, CodeRate::R1_4) +
                  240 + selective_repeat_arq_policy::kAckRepeatMaxJitterMs +
                  qam16_r14.ack_ms,
          "wide OFDM ACK-diversity guard should cover delayed clean-ACK repeat tail");
    CHECK(qam16_ack_repeat_tail > 250,
          "wide OFDM QAM16 ACK-diversity guard must exceed the legacy fixed 250 ms holdoff");

    CHECK(computeWideOFDMAckTimeoutMs(Modulation::DQPSK, CodeRate::R1_2, 4, 120, 2) == 9446,
          "wide OFDM window=4 timeout should cover physical burst, ACK copies, and SACK holdoff");
    CHECK(computeWideOFDMAckTimeoutMs(Modulation::DQPSK, CodeRate::R1_2, 8, 120, 1) == 14414,
          "wide OFDM window=8 timeout should derive from burst airtime, not the configured short SACK");
    CHECK(computeWideOFDMAckTimeoutMs(Modulation::DQPSK, CodeRate::R1_2, 8,
                                      w8_sack_delay, 1) == 14414,
          "wide OFDM window=8 timeout should cover physical SACK holdoff");
    CHECK(computeWideOFDMAckTimeoutMs(Modulation::DQPSK, CodeRate::R1_2, 8, 120, 1, 6) == 19022,
          "wide OFDM 6-CW ACK timeout should cover the longer burst");
    CHECK(computeWideOFDMAckTimeoutMs(Modulation::DQPSK, CodeRate::R1_2, 8, 120, 1, 8) == 23630,
          "wide OFDM 8-CW ACK timeout should cover the longer burst");

    const uint32_t timeout_4cw = computeWideOFDMAckTimeoutMs(
        Modulation::DQPSK, CodeRate::R1_2, kHighThroughputOFDMWindowFrames,
        kCarrierSenseSackCoalesceMs, kCarrierSenseAckRepeatCount);
    const uint32_t min_4cw_ack_path =
        wideOFDMBurstAirtimeMs(
            Modulation::DQPSK, CodeRate::R1_2, kHighThroughputOFDMWindowFrames) +
        kCarrierSenseSackCoalesceMs + dqpsk.ack_ms;
    CHECK(timeout_4cw >= min_4cw_ack_path,
          "wide OFDM window=16 4-CW timeout should cover burst plus carrier-sensed ACK path");

    const uint32_t timeout_6cw = computeWideOFDMAckTimeoutMs(
        Modulation::DQPSK, CodeRate::R1_2, kHighThroughputOFDMWindowFrames,
        kCarrierSenseSackCoalesceMs, kCarrierSenseAckRepeatCount, 6);
    const uint32_t min_6cw_ack_path =
        wideOFDMBurstAirtimeMs(
            Modulation::DQPSK, CodeRate::R1_2, kHighThroughputOFDMWindowFrames, 6) +
        kCarrierSenseSackCoalesceMs + dqpsk_6cw.ack_ms;
    CHECK(timeout_6cw >= min_6cw_ack_path,
          "wide OFDM window=16 6-CW timeout should cover burst plus carrier-sensed ACK path");

    const uint32_t timeout_8cw = computeWideOFDMAckTimeoutMs(
        Modulation::DQPSK, CodeRate::R1_2, kHighThroughputOFDMWindowFrames,
        kCarrierSenseSackCoalesceMs, kCarrierSenseAckRepeatCount, 8);
    const uint32_t min_8cw_ack_path =
        wideOFDMBurstAirtimeMs(
            Modulation::DQPSK, CodeRate::R1_2, kHighThroughputOFDMWindowFrames, 8) +
        kCarrierSenseSackCoalesceMs + dqpsk_8cw.ack_ms;
    CHECK(timeout_8cw >= min_8cw_ack_path,
          "wide OFDM window=16 8-CW timeout should cover burst plus carrier-sensed ACK path");

    auto d8psk = wideOFDMFrameTiming(Modulation::D8PSK, CodeRate::R1_2);
    CHECK(d8psk.data_symbols == 19, "D8PSK R1/2 wide OFDM data symbols");
    CHECK(d8psk.ack_symbols == 7, "D8PSK R1/2 wide OFDM ACK symbols");

    auto qam8 = wideOFDMFrameTiming(Modulation::QAM8, CodeRate::R1_2, 8);
    CHECK(qam8.data_symbols == 39, "coherent 8PSK R1/2 8-CW wide OFDM data symbols");
    CHECK(qam8.ack_symbols == 7, "coherent 8PSK R1/2 wide OFDM ACK symbols");
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

void test_mc_dpsk_window_timing() {
    auto robust_low = mcDpskFrameTiming(Modulation::DBPSK, 8, 2048, 3);
    CHECK(robust_low.data_ms == 10752, "Robust-Low MC-DPSK 3-CW data timing");
    CHECK(robust_low.data_only_ms == 10368, "Robust-Low MC-DPSK data-only timing");
    CHECK(mcDpskBurstAirtimeMs(robust_low, 1) == 11952,
          "Robust-Low MC-DPSK physical burst timing");
    CHECK(mcDpskWindowSizeForTiming(robust_low) == 1,
          "Robust-Low MC-DPSK should keep window=1");

    auto robust_mid = mcDpskFrameTiming(Modulation::DBPSK, 8, 1024, 3);
    CHECK(robust_mid.data_ms == 5376, "Robust-Mid MC-DPSK 3-CW data timing");
    CHECK(robust_mid.data_only_ms == 5184, "Robust-Mid MC-DPSK data-only timing");
    CHECK(mcDpskBurstAirtimeMs(robust_mid, 3) == 16944,
          "Robust-Mid MC-DPSK window=3 physical burst timing");
    CHECK(mcDpskWindowSizeForTiming(robust_mid) == 3,
          "Robust-Mid MC-DPSK should use window=3");
    CHECK(computeMCDPSKAckTimeoutMs(robust_mid, 3, kCarrierSenseSackCoalesceMs,
                                    kCarrierSenseAckRepeatCount) >=
              3 * robust_mid.data_ms + robust_mid.ack_ms + kCarrierSenseSackCoalesceMs,
          "Robust-Mid ACK timeout should cover the three-frame burst and carrier-sensed ACK path");

    auto robust = mcDpskFrameTiming(Modulation::DQPSK, 8, 1024, 4);
    CHECK(robust.data_ms == 3691, "Robust MC-DPSK 4-CW data timing");
    CHECK(robust.data_only_ms == 3499, "Robust MC-DPSK data-only timing");
    CHECK(mcDpskBurstAirtimeMs(robust, 5) == 18887,
          "Robust MC-DPSK window=5 physical burst timing");
    CHECK(mcDpskWindowSizeForTiming(robust) == 5,
          "Robust MC-DPSK should use window=5");
    CHECK(kCarrierSenseAckRepeatCount == 1,
          "Robust MC-DPSK must not repeat full-preamble ACKs into the next DATA turn");

    auto standard = mcDpskFrameTiming(Modulation::DQPSK, 8, 512, 4);
    CHECK(standard.data_ms == 1845, "Standard MC-DPSK 4-CW data timing");
    CHECK(standard.data_only_ms == 1749, "Standard MC-DPSK data-only timing");
    CHECK(mcDpskBurstAirtimeMs(standard, 5) == 10041,
          "Standard MC-DPSK window=5 physical burst timing");
    CHECK(mcDpskWindowSizeForTiming(standard) == 5,
          "Standard MC-DPSK should use window=5");
    CHECK(computeMCDPSKAckTimeoutMs(standard, 5, kCarrierSenseSackCoalesceMs,
                                    kCarrierSenseAckRepeatCount) >= 18000,
          "Standard MC-DPSK window=5 timeout should retain the conservative floor");
}

void test_ofdm_profile_selection() {
    CHECK(isNearAwgnOFDM(0.00f, 25.0f), "near-AWGN threshold should include in-band SNR25");
    CHECK(isNearAwgnOFDM(0.14f, 25.0f), "near-AWGN fading threshold should allow R2/3 cutoff margin");
    CHECK(!isNearAwgnOFDM(0.15f, 25.0f), "near-AWGN fading threshold should match R2/3 cutoff");
    CHECK(!isNearAwgnOFDM(0.30f, 25.0f), "near-AWGN fading threshold is strict");
    CHECK(!isNearAwgnOFDM(0.00f, 24.9f), "near-AWGN in-band SNR threshold is strict");
    CHECK(isHighThroughputOFDM(0.30f, 25.0f), "Good fading in-band SNR25 should use high-throughput OFDM window");
    CHECK(!isHighThroughputOFDM(0.65f, 25.0f), "Moderate fading should not use high-throughput OFDM window yet");
    CHECK(!isHighThroughputOFDM(0.30f, 24.9f), "high-throughput OFDM in-band SNR threshold is strict");

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
    CHECK(ofdmWindowSizeForChannel(Modulation::DQPSK, CodeRate::R2_3, 0.30f, 25.0f)
              == kWideOFDMWindowFrames,
          "R2/3 should keep one burst group on good fading at in-band SNR25");
    CHECK(ofdmWindowSizeForChannel(Modulation::DQPSK, CodeRate::R2_3, 0.05f, 25.0f)
              == kHighThroughputOFDMWindowFrames,
          "R2/3 can use two burst groups only on near-AWGN channels");
    CHECK(ofdmWindowSizeForChannel(Modulation::DQPSK, CodeRate::R2_3, 0.80f, 25.0f)
              == kWideOFDMWindowFrames,
          "R2/3 should keep one burst group on moderate fading");
    CHECK(ofdmWindowSizeForChannel(Modulation::DQPSK, CodeRate::R3_4, 0.30f, 25.0f)
              == kWideOFDMWindowFrames,
          "R3/4 remains near-AWGN only for two burst groups");
    CHECK(ofdmWindowSize(Modulation::DQPSK, CodeRate::R1_2, false) == kHighThroughputOFDMWindowFrames,
          "R1/2 keeps the high-throughput OFDM window in fading");
    CHECK(!isBurstInterleavedOFDMMode(Modulation::QAM16, CodeRate::R1_2),
          "coherent QAM16 R1/2 should keep per-frame sync/channel tracking");
    CHECK(!isBurstInterleavedOFDMMode(Modulation::QAM16, CodeRate::R3_4),
          "coherent QAM16 R3/4 should keep per-frame sync/channel tracking");
    CHECK(isBurstInterleavedOFDMMode(Modulation::DQPSK, CodeRate::R2_3),
          "legacy speculative DQPSK high-rate burst interleaving should remain enabled");
    CHECK(!isBurstInterleavedOFDMMode(Modulation::DQPSK, CodeRate::R1_2),
          "non-speculative DQPSK R1/2 should keep existing burst-interleave behavior");
    CHECK(!isBurstInterleavedOFDMMode(Modulation::QPSK, CodeRate::R2_3),
          "non-QAM16 coherent modes should not inherit the QAM16 burst-interleave gate");
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

    CHECK(kCarrierSenseSackCoalesceMs == 30,
          "OFDM SACK policy should use a small coalescing timer, not burst-tail guessing");
    CHECK(kCarrierSenseAckRepeatCount == 1,
          "OFDM ACK policy should not schedule delayed duplicate ACK bursts");
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
                               20.0f, 0.0f) == WaveformMode::MC_DPSK,
          "no common modes should fall back to MC-DPSK (universal floor), never OFDM_COX");
}

void test_auto_data_mode_boundaries() {
    const uint8_t all = ModeCapabilities::ALL;
    Modulation mod = Modulation::AUTO;
    CodeRate rate = CodeRate::AUTO;

    WaveformMode waveform = selectNegotiatedMode(
        all, all, WaveformMode::AUTO, WaveformMode::AUTO, WaveformMode::AUTO,
        20.0f, 0.30f);
    CHECK(waveform == WaveformMode::OFDM_CHIRP,
          "GOOD fading SNR20 auto-negotiates OFDM_CHIRP");
    recommendDataMode(20.0f, waveform, mod, rate, 0.30f);
    CHECK(mod == Modulation::QPSK, "GOOD fading SNR20 auto data mode selects coherent QPSK");
    CHECK(rate == CodeRate::R2_3, "GOOD fading SNR20 auto data mode selects measured QPSK R2/3 rung");

    recommendDataMode(20.0f, waveform, mod, rate, 0.79f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R2_3,
          "GOOD-lobby estimator spread at SNR20 still selects QPSK R2/3");

    recommendDataMode(19.8f, waveform, mod, rate, 0.50f);
    CHECK(mod == Modulation::QPSK && rate == CodeRate::R2_3,
          "GOOD fading one SNR quantum below SNR20 still selects QPSK R2/3");

    recommendDataMode(19.7f, waveform, mod, rate, 0.50f);
    CHECK(mod == Modulation::DQPSK,
          "GOOD fading below the SNR quantum guard falls back to DQPSK");

    recommendDataMode(20.0f, waveform, mod, rate, 0.80f);
    CHECK(mod == Modulation::DQPSK,
          "above GOOD-lobby estimator margin falls back to DQPSK");

    waveform = selectNegotiatedMode(
        all, all, WaveformMode::AUTO, WaveformMode::AUTO, WaveformMode::AUTO,
        16.9f, 0.30f);
    CHECK(waveform == WaveformMode::OFDM_CHIRP,
          "GOOD fading SNR16.9 remains above the OFDM floor");
    recommendDataMode(16.9f, waveform, mod, rate, 0.30f);
    CHECK(mod == Modulation::DQPSK,
          "GOOD fading below the QPSK floor falls back to DQPSK");
    CHECK(rate == CodeRate::R1_2,
          "GOOD fading below the QPSK floor keeps the existing R1/2 OFDM rate");

    waveform = selectNegotiatedMode(
        all, all, WaveformMode::AUTO, WaveformMode::AUTO, WaveformMode::AUTO,
        11.9f, 0.30f);
    CHECK(waveform == WaveformMode::MC_DPSK,
          "GOOD fading below the OFDM floor keeps MC-DPSK");
    recommendDataMode(11.9f, waveform, mod, rate, 0.30f);
    CHECK(mod == Modulation::DQPSK && rate == CodeRate::R1_4,
          "MC-DPSK floor still uses DQPSK R1/4");

    waveform = selectNegotiatedMode(
        all, all, WaveformMode::AUTO, WaveformMode::AUTO, WaveformMode::AUTO,
        20.0f, 0.05f);
    CHECK(waveform == WaveformMode::OFDM_CHIRP,
          "AWGN SNR20 auto-negotiates OFDM_CHIRP");
    recommendDataMode(20.0f, waveform, mod, rate, 0.05f);
    CHECK(mod == Modulation::QAM16 && rate == CodeRate::R3_4,
          "AWGN SNR20 selects active QAM16 R3/4 rung");

    waveform = selectNegotiatedMode(
        all, all, WaveformMode::AUTO, WaveformMode::AUTO, WaveformMode::AUTO,
        20.0f, 0.90f);
    CHECK(waveform == WaveformMode::OFDM_CHIRP,
          "Moderate fading SNR20 auto-negotiates OFDM_CHIRP");
    recommendDataMode(20.0f, waveform, mod, rate, 0.90f);
    CHECK(mod == Modulation::DQPSK,
          "Moderate fading SNR20 stays DQPSK");

    waveform = selectNegotiatedMode(
        all, all, WaveformMode::AUTO, WaveformMode::AUTO, WaveformMode::AUTO,
        20.0f, 1.20f);
    CHECK(waveform == WaveformMode::OFDM_CHIRP,
          "Poor fading SNR20 reaches OFDM_CHIRP only above its floor");
    recommendDataMode(20.0f, waveform, mod, rate, 1.20f);
    CHECK(mod == Modulation::DQPSK,
          "Poor fading SNR20 stays DQPSK");
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

    // OFDM_NARROW: always default 4. Narrow R1/2 frames are ~6 s at CW=8;
    // window=3 burst would be ~18 s, exceeding typical narrow good-fading
    // coherence (~10 s). 3-seed sim A/B at SNR=8 R1/2 confirmed CW=8
    // hit 1/3 catastrophic FAIL (240 s timeout) while CW=4 was 3/3 PASS.
    for (auto rate : {CodeRate::R1_4, CodeRate::R1_2, CodeRate::R2_3, CodeRate::R3_4}) {
        CHECK(recommendCWCount(rate, WaveformMode::OFDM_NARROW) ==
                  v2::kDefaultFixedFrameCodewords,
              "narrow always caps at default 4 (fade-coherence limit)");
    }

    CHECK(recommendCWCount(Modulation::DBPSK, CodeRate::R1_4, WaveformMode::MC_DPSK) == 3,
          "Robust-Low MC-DPSK DBPSK uses 3-CW variable frames");
    CHECK(recommendCWCount(Modulation::DQPSK, CodeRate::R1_4, WaveformMode::MC_DPSK) ==
              v2::kDefaultFixedFrameCodewords,
          "standard MC-DPSK DQPSK keeps the legacy CW count");
    CHECK(recommendCWCount(Modulation::DBPSK, CodeRate::R1_4, WaveformMode::OFDM_CHIRP) ==
              v2::kDefaultFixedFrameCodewords,
          "DBPSK does not alter OFDM CW policy");

    // ITU-R F.1487 Good = 0.1 Hz Doppler -> Clarke Tc = 0.423/fD ~= 4230 ms.
    // (a1c9c34 mislabeled the 0.5 Hz Moderate value as "Good"; corrected 2026-05-26.)
    CHECK(coherenceTimeMsForDoppler(kGoodHFDesignDopplerHz) == 4230,
          "Good-HF design Doppler (0.1 Hz) gives ~4230 ms Clarke coherence time");
    CHECK(coherenceTimeMsForDoppler(kModerateHFDesignDopplerHz) == 846,
          "Moderate-HF design Doppler (0.5 Hz) gives ~846 ms Clarke coherence time");
    // Good coherent QPSK R2/3 (fading_index ~0.50): a cw=8 frame (~1392 ms) fits the
    // ~4230 ms Good coherence, so it keeps the full 8-CW throughput geometry.
    CHECK(recommendCWCountForChannel(Modulation::QPSK, CodeRate::R2_3,
                                     WaveformMode::OFDM_CHIRP, 0.50f, 20.0f) == 8,
          "Good coherent QPSK R2/3 keeps cw=8 inside the true Good coherence time");
    // Moderate (fading_index ~0.90): 0.5 Hz -> 846 ms coherence caps cw=8 (~1392 ms)
    // down to cw=4 (720 ms) so a single fade event cannot take the whole frame.
    CHECK(recommendCWCountForChannel(Modulation::QPSK, CodeRate::R2_3,
                                     WaveformMode::OFDM_CHIRP, 0.90f, 20.0f) == 4,
          "Moderate coherent QPSK R2/3 caps frame length inside coherence time");
    CHECK(recommendCWCountForChannel(Modulation::QPSK, CodeRate::R2_3,
                                     WaveformMode::OFDM_CHIRP, 0.0f, 27.0f) == 8,
          "near-AWGN coherent QPSK keeps the throughput CW count");
    CHECK(recommendCWCountForChannel(Modulation::DQPSK, CodeRate::R2_3,
                                     WaveformMode::OFDM_CHIRP, 0.50f, 20.0f) == 8,
          "differential OFDM keeps deterministic CW policy");
}

void test_variable_frame_payload_capacity() {
    CHECK(v2::getVariableFramePayloadCapacity(CodeRate::R1_4, 1) == 1,
          "R1/4 variable 1-CW DATA carries only 1 payload byte");
    CHECK(v2::getVariableFramePayloadCapacity(CodeRate::R1_4, 2) == 19,
          "R1/4 variable 2-CW DATA carries 19 payload bytes");
    CHECK(v2::getVariableFramePayloadCapacity(CodeRate::R1_4, 3) == 37,
          "R1/4 variable 3-CW DATA carries 37 payload bytes");
    CHECK(v2::getVariableFramePayloadCapacity(CodeRate::R1_4, 4) == 55,
          "R1/4 variable 4-CW DATA carries 55 payload bytes");

    for (int cw = v2::kMinFixedFrameCodewords; cw <= v2::kMaxFixedFrameCodewords; ++cw) {
        const size_t cap = v2::getVariableFramePayloadCapacity(CodeRate::R1_4, cw);
        CHECK(v2::DataFrame::calculateCodewords(cap, CodeRate::R1_4) == cw,
              "variable frame capacity should fit exactly in the target CW count");
        if (cw < v2::kMaxFixedFrameCodewords) {
            CHECK(v2::DataFrame::calculateCodewords(cap + 1, CodeRate::R1_4) == cw + 1,
                  "one byte over variable capacity should require one more CW");
        }
    }
}

}  // namespace

int main() {
    test_fading_labels_and_capabilities();
    test_ladder_rung_selection();
    test_wide_ofdm_timing_and_timeout();
    test_narrow_ofdm_timing_and_timeout();
    test_mc_dpsk_window_timing();
    test_ofdm_profile_selection();
    test_negotiated_mode_selection();
    test_auto_data_mode_boundaries();
    test_recommend_cw_count();
    test_variable_frame_payload_capacity();

    if (tests_failed != 0) {
        std::cout << "ConnectionPolicy: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "ConnectionPolicy: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
