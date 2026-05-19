#include "gui/modem/streaming_frame_policy.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace ultra::gui::streaming_frame_policy;

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

#define CHECK_CLOSE(actual, expected, tol, msg) \
    CHECK(std::abs((actual) - (expected)) <= (tol), msg)

void test_rms_and_ping_detection() {
    CHECK(rms(nullptr, 4) == 0.0f, "null RMS span should be safe");
    const float values[] = {1.0f, -1.0f, 1.0f, -1.0f};
    CHECK_CLOSE(rms(values, 4), 1.0f, 0.0001f, "RMS of unit samples");

    std::vector<float> ping(kPingTrainingSkipSamples + kPingRMSCheckSamples, 0.0f);
    for (size_t i = 0; i < kPingTrainingSkipSamples; ++i) {
        ping[i] = 1.0f;
    }
    for (size_t i = kPingTrainingSkipSamples; i < ping.size(); ++i) {
        ping[i] = 0.2f;
    }
    auto ping_decision = evaluatePingRMS(ping.data(), ping.size());
    CHECK(ping_decision.is_ping, "low data/training RMS ratio should be ping");
    CHECK_CLOSE(ping_decision.ratio, 0.2f, 0.0001f, "ping RMS ratio");

    std::vector<float> data(kPingTrainingSkipSamples + kPingRMSCheckSamples, 1.0f);
    auto data_decision = evaluatePingRMS(data.data(), data.size());
    CHECK(!data_decision.is_ping, "data energy after training should not be ping");
    CHECK_CLOSE(data_decision.ratio, 1.0f, 0.0001f, "data RMS ratio");

    auto silent = evaluatePingRMS(nullptr, 0);
    CHECK(silent.is_ping, "silence should classify as ping-compatible chirp-only frame");
}

void test_ping_chirp_lock_fallback() {
    std::vector<float> busy_band(kPingTrainingSkipSamples + kPingRMSCheckSamples, 1.0f);

    auto no_lock = evaluatePingFrame(
        busy_band.data(), busy_band.size(),
        kPingTrainingSkipSamples, kPingRMSCheckSamples,
        kPingCorrFloor - 0.001f, 83.0f,
        false, false);
    CHECK(!no_lock.is_ping, "chirp fallback should reject below correlation floor");

    auto wide_gap = evaluatePingFrame(
        busy_band.data(), busy_band.size(),
        kPingTrainingSkipSamples, kPingRMSCheckSamples,
        kPingCorrFloor, kPingMaxGapError + 1.0f,
        false, false);
    CHECK(!wide_gap.is_ping, "chirp fallback should reject loose dual-chirp gap");

    auto valid_frame = evaluatePingFrame(
        busy_band.data(), busy_band.size(),
        kPingTrainingSkipSamples, kPingRMSCheckSamples,
        kPingCorrFloor, 145.0f,
        true, true);
    CHECK(!valid_frame.is_ping, "valid LDPC frame should not classify as ping");

    auto locked_no_frame = evaluatePingFrame(
        busy_band.data(), busy_band.size(),
        kPingTrainingSkipSamples, kPingRMSCheckSamples,
        kPingCorrFloor, -145.0f,
        false, false);
    CHECK(locked_no_frame.is_ping, "chirp-locked LDPC failure should classify as ping");
    CHECK(!locked_no_frame.ping_by_silence, "busy-band fallback should not rely on RMS silence");
    CHECK(locked_no_frame.ping_by_chirp_lock, "busy-band fallback should use chirp lock");

    auto low_snr_ping_after_llr_reject = evaluatePingFrame(
        busy_band.data(), busy_band.size(),
        kPingTrainingSkipSamples, kPingRMSCheckSamples,
        0.908f, 0.0f,
        false, false);
    CHECK(low_snr_ping_after_llr_reject.is_ping,
          "measured SNR15 chirp lock plus rejected data frame should classify as ping");
    CHECK(low_snr_ping_after_llr_reject.ping_by_chirp_lock,
          "measured SNR15 fallback should depend on chirp lock");
}

void test_false_lock_advance() {
    CHECK(falseOFDMLockAdvanceSamples(10000, 0) == kDefaultFalseLockAdvanceSamples,
          "missing preamble estimate should use default false-lock advance");
    CHECK(falseOFDMLockAdvanceSamples(10000, 800) == kMinFalseLockAdvanceSamples,
          "short preamble should respect minimum false-lock advance");
    CHECK(falseOFDMLockAdvanceSamples(10000, 3000) == 1500,
          "false-lock advance should use half data preamble when larger");
    CHECK(falseOFDMLockAdvanceSamples(700, 3000) == 700,
          "false-lock advance should not exceed copied frame length");
}

void test_control_first_peek_policy() {
    CHECK(shouldRunControlFirstOFDMPeek(0, true, true, 9000, 9000),
          "connected OFDM first pass within control size should peek");
    CHECK(!shouldRunControlFirstOFDMPeek(1, true, true, 9000, 9000),
          "pending codewords should not run control-first peek");
    CHECK(!shouldRunControlFirstOFDMPeek(0, false, true, 9000, 9000),
          "non-OFDM should not run OFDM control-first peek");
    CHECK(!shouldRunControlFirstOFDMPeek(0, true, false, 9000, 9000),
          "disconnected path should not run connected control-first peek");
    CHECK(!shouldRunControlFirstOFDMPeek(0, true, true, 9001, 9000),
          "full-frame sample count should not run control-first peek");
}

void test_sync_recovery_gate() {
    CHECK(!allowSyncRecovery(kMinSyncRecoveryCorrelation - 0.001f),
          "sync recovery should reject below threshold");
    CHECK(allowSyncRecovery(kMinSyncRecoveryCorrelation),
          "sync recovery should allow exact threshold");
    CHECK(allowSyncRecovery(kMinSyncRecoveryCorrelation + 0.1f),
          "sync recovery should allow strong correlation");
}

void test_non_data_frame_detection() {
    std::vector<uint8_t> too_short = {0x55, 0x4c};
    CHECK(!isNonDataFrame(true, too_short.data(), too_short.size()),
          "short frame should not classify as non-data");
    CHECK(!isNonDataFrame(false, nullptr, 0),
          "failed decode should not classify as non-data");

    std::vector<uint8_t> ack = {0x55, 0x4c,
        static_cast<uint8_t>(ultra::protocol::v2::FrameType::ACK)};
    CHECK(isNonDataFrame(true, ack.data(), ack.size()), "ACK should classify as non-data");

    std::vector<uint8_t> connect = {0x55, 0x4c,
        static_cast<uint8_t>(ultra::protocol::v2::FrameType::CONNECT)};
    CHECK(isNonDataFrame(true, connect.data(), connect.size()),
          "CONNECT should classify as non-data");

    std::vector<uint8_t> data = {0x55, 0x4c,
        static_cast<uint8_t>(ultra::protocol::v2::FrameType::DATA)};
    CHECK(!isNonDataFrame(true, data.data(), data.size()), "DATA should not classify as non-data");
}

void test_consumed_samples_policy() {
    CHECK(consumedSamplesForDecodedFrame(true, true, true, 1, 12000, 4000) == 4000,
          "successful OFDM non-data frame should use exact CW sample count");
    CHECK(consumedSamplesForDecodedFrame(true, true, true, 1, 12000, 14000) == 12000,
          "exact count should not extend consumed samples");
    CHECK(consumedSamplesForDecodedFrame(true, true, true, 0, 12000, 4000) == 12000,
          "zero codewords should keep copied frame length");
    CHECK(consumedSamplesForDecodedFrame(true, false, true, 1, 12000, 4000) == 12000,
          "non-OFDM path should keep copied frame length");
    CHECK(consumedSamplesForDecodedFrame(true, true, false, 1, 12000, 4000) == 12000,
          "data frame should keep copied frame length");
    CHECK(consumedSamplesForDecodedFrame(false, true, true, 1, 12000, 4000) == 12000,
          "failed decode should keep copied frame length");
}

}  // namespace

int main() {
    test_rms_and_ping_detection();
    test_ping_chirp_lock_fallback();
    test_false_lock_advance();
    test_control_first_peek_policy();
    test_sync_recovery_gate();
    test_non_data_frame_detection();
    test_consumed_samples_policy();

    if (tests_failed != 0) {
        std::cout << "StreamingFramePolicy: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "StreamingFramePolicy: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
