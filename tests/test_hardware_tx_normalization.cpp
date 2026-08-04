#include "gui/modem/modem_engine.hpp"
#include "ota_channel_core/models.hpp"
#include "protocol/connection_policy.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/tx_burst_normalization.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

namespace channel = ultra::ota_channel_core;
namespace gui = ultra::gui;
namespace protocol = ultra::protocol;
namespace sim = ultra::sim;
namespace v2 = ultra::protocol::v2;

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

float samplePeak(const std::vector<float>& samples) {
    float peak = 0.0f;
    for (float sample : samples) {
        peak = std::max(peak, std::abs(sample));
    }
    return peak;
}

double maxVectorDelta(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) {
        return std::numeric_limits<double>::infinity();
    }
    double max_delta = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        max_delta = std::max(max_delta,
                             std::abs(static_cast<double>(a[i]) -
                                      static_cast<double>(b[i])));
    }
    return max_delta;
}

std::vector<float> makeSyntheticBurst(float desired_peak,
                                      uint32_t seed,
                                      size_t active_samples =
                                          sim::kTxBurstMinimumActiveSamples + 997) {
    constexpr size_t kPrefix = 128;
    constexpr size_t kSuffix = 192;
    std::vector<float> samples(kPrefix + active_samples + kSuffix, 0.0f);

    uint32_t state = seed;
    for (size_t i = 0; i < active_samples; ++i) {
        state = 1664525u * state + 1013904223u;
        const uint32_t bits = (state >> 8) & 0xffffu;
        float sample = (static_cast<float>(bits) / 32767.5f) - 1.0f;
        if (std::abs(sample) < 0.05f) {
            sample = sample < 0.0f ? -0.05f : 0.05f;
        }
        samples[kPrefix + i] = sample;
    }
    samples[kPrefix] = 0.33f;
    samples[kPrefix + active_samples - 1] = -0.29f;

    const float peak = samplePeak(samples);
    const float scale = desired_peak / peak;
    for (float& sample : samples) {
        sample *= scale;
    }
    return samples;
}

std::vector<uint8_t> payload(size_t n, uint32_t seed) {
    std::vector<uint8_t> out(n);
    uint32_t state = seed;
    for (auto& byte : out) {
        state = 1664525u * state + 1013904223u;
        byte = static_cast<uint8_t>((state >> 16) & 0xffu);
    }
    return out;
}

void configureConnectedEngine(gui::ModemEngine& engine,
                              ultra::CodeRate rate,
                              int cw_count,
                              ultra::Modulation modulation = ultra::Modulation::DQPSK) {
    engine.setWaveformMode(protocol::WaveformMode::OFDM_CHIRP);
    engine.setDataMode(modulation, rate);
    engine.setConnected(true);
    engine.setHandshakeComplete(true);
    engine.setFixedFrameCodewords(cw_count);
}

std::vector<uint8_t> fixedDataFrame(ultra::CodeRate rate,
                                    int cw_count,
                                    uint16_t seq,
                                    uint32_t seed) {
    const size_t cap = v2::getFixedFramePayloadCapacity(rate, cw_count);
    auto frame = v2::makeFixedDataFrame("ALPHA", "BRAVO", seq,
                                        payload(cap, seed), rate, cw_count);
    return frame.serialize();
}

std::vector<float> connectedData(ultra::CodeRate rate, int cw_count) {
    gui::ModemEngine engine;
    configureConnectedEngine(engine, rate, cw_count);
    (void)engine.transmit(fixedDataFrame(rate, cw_count, 1, 0x1111u));
    return engine.transmit(fixedDataFrame(rate, cw_count, 2, 0x2222u));
}

std::vector<float> connectedAck(ultra::CodeRate session_rate, int cw_count) {
    gui::ModemEngine engine;
    configureConnectedEngine(engine, session_rate, cw_count);
    (void)engine.transmit(fixedDataFrame(session_rate, cw_count, 1, 0x3333u));
    const auto ack = v2::ControlFrame::makeAck("BRAVO", "ALPHA", 1);
    return engine.transmit(ack.serialize());
}

std::vector<float> connectFrame() {
    gui::ModemEngine engine;
    const auto frame = v2::ConnectFrame::makeConnect(
        "ALPHA", "BRAVO",
        protocol::ModeCapabilities::ALL | protocol::ModeCapabilities::PHY_MASK_V1,
        static_cast<uint8_t>(protocol::WaveformMode::AUTO),
        static_cast<uint8_t>(ultra::Modulation::AUTO),
        static_cast<uint8_t>(ultra::CodeRate::AUTO),
        0);
    return engine.transmit(frame.serialize());
}

struct BurstSpec {
    std::string label;
    std::function<std::vector<float>()> build;
};

std::vector<float> modemPing() {
    gui::ModemEngine engine;
    return engine.transmitPing();
}

std::vector<float> modemPong() {
    gui::ModemEngine engine;
    return engine.transmitPong();
}

std::vector<BurstSpec> operationalBursts() {
    constexpr auto ofdm = protocol::WaveformMode::OFDM_CHIRP;
    const int r14_cw =
        protocol::connection_policy::recommendCWCount(ultra::CodeRate::R1_4, ofdm);
    const int r12_cw =
        protocol::connection_policy::recommendCWCount(ultra::CodeRate::R1_2, ofdm);

    return {
        {"PING", modemPing},
        {"PONG", modemPong},
        {"CONNECT", connectFrame},
        {"OFDM data R1/4", [=] { return connectedData(ultra::CodeRate::R1_4, r14_cw); }},
        {"OFDM data R1/2", [=] { return connectedData(ultra::CodeRate::R1_2, r12_cw); }},
        {"ACK", [=] { return connectedAck(ultra::CodeRate::R1_2, r12_cw); }},
    };
}

void proof1_apiCorrectness() {
    constexpr float kTarget = 0.5f;
    constexpr float kTolerance = 0.001f;
    std::mt19937 rng(0x48415744u);
    std::uniform_real_distribution<float> peaks(0.01f, 1.5f);

    for (int i = 0; i < 100; ++i) {
        std::vector<float> burst = makeSyntheticBurst(peaks(rng),
                                                      0x1000u + static_cast<uint32_t>(i));
        const std::vector<float> original = burst;
        const auto rms_measure = sim::measureTxBurstInBandRms(burst);
        const float peak_before = samplePeak(burst);

        const auto hw = sim::normalizeTxBurstForHardware(burst, kTarget);
        const float measured_after = samplePeak(burst);
        const float expected_gain = kTarget / peak_before;
        const double gain_tol = std::max(1.0e-6, std::abs(expected_gain) * 1.0e-6);

        CHECK(!hw.burst_fragment_warning, "Proof1 synthetic burst is whole");
        CHECK(hw.active_begin == rms_measure.active_begin &&
                  hw.active_end == rms_measure.active_end &&
                  hw.active_samples == rms_measure.active_samples,
              "Proof1 active-region detection matches RMS measurement");
        CHECK(std::abs(hw.peak_after_gain - kTarget) <= kTolerance,
              "Proof1 reported post peak matches target");
        CHECK(std::abs(measured_after - kTarget) <= kTolerance,
              "Proof1 measured post peak matches target");
        CHECK(std::abs(hw.gain_to_target - expected_gain) <= gain_tol,
              "Proof1 gain equals target / pre-peak");
        CHECK(measured_after <= kTarget + kTolerance,
              "Proof1 no clipping above target");

        double shape_error = 0.0;
        for (size_t n = 0; n < burst.size(); ++n) {
            shape_error = std::max(shape_error,
                                   std::abs(static_cast<double>(burst[n]) -
                                            static_cast<double>(original[n]) *
                                                hw.gain_to_target));
        }
        CHECK(shape_error <= 1.0e-5, "Proof1 waveform shape preserved");
    }
}

void proof2_operatorTargets() {
    constexpr float kTolerance = 0.001f;
    std::vector<float> burst = makeSyntheticBurst(0.82f, 0x2200u);
    for (float target : {0.3f, 0.5f, 0.7f}) {
        const auto hw = sim::normalizeTxBurstForHardware(burst, target);
        CHECK(std::abs(hw.peak_after_gain - target) <= kTolerance,
              "Proof2 sequential target reported");
        CHECK(std::abs(samplePeak(burst) - target) <= kTolerance,
              "Proof2 sequential target measured");
    }

    // 2026-07-02 software-ALC: the normalizer's upper clamp is the ALC ceiling
    // (kSoftwareAlcMaxPeakTarget = 0.85), not the 0.7 operator-settings ceiling —
    // the closed loop may legitimately request (0.7, 0.85] and is guarded by the
    // receiver's crest-factor clip detector. Operator settings stay clamped to
    // 0.7 upstream (settings.cpp).
    std::vector<float> high = makeSyntheticBurst(0.4f, 0x2210u);
    const auto high_result = sim::normalizeTxBurstForHardware(high, 1.5f);
    CHECK(std::abs(high_result.target_peak - sim::kSoftwareAlcMaxPeakTarget) <= kTolerance,
          "Proof2 high target clamps to ALC ceiling");
    CHECK(std::abs(samplePeak(high) - sim::kSoftwareAlcMaxPeakTarget) <= kTolerance,
          "Proof2 high clamp peak measured");

    // The ALC ceiling itself passes through un-clamped (the loop's top rung).
    std::vector<float> alc_top = makeSyntheticBurst(0.4f, 0x2230u);
    const auto alc_top_result =
        sim::normalizeTxBurstForHardware(alc_top, sim::kSoftwareAlcMaxPeakTarget);
    CHECK(std::abs(alc_top_result.target_peak - sim::kSoftwareAlcMaxPeakTarget) <= kTolerance,
          "Proof2 ALC ceiling target accepted");
    CHECK(std::abs(samplePeak(alc_top) - sim::kSoftwareAlcMaxPeakTarget) <= kTolerance,
          "Proof2 ALC ceiling peak measured");

    std::vector<float> low = makeSyntheticBurst(0.4f, 0x2220u);
    const auto low_result = sim::normalizeTxBurstForHardware(low, 0.01f);
    CHECK(std::abs(low_result.target_peak - sim::kHardwareTxMinPeakTarget) <= kTolerance,
          "Proof2 low target clamps to min");
    CHECK(std::abs(samplePeak(low) - sim::kHardwareTxMinPeakTarget) <= kTolerance,
          "Proof2 low clamp peak measured");
}

void proof3_burstBoundaryInvariant() {
    std::vector<float> fragment(480);
    for (size_t i = 0; i < fragment.size(); ++i) {
        fragment[i] = (i % 2 == 0) ? 0.2f : -0.2f;
    }
    const std::vector<float> original = fragment;

    const auto hw = sim::normalizeTxBurstForHardware(fragment, 0.5f);
    CHECK(hw.burst_fragment_warning, "Proof3 fragment warning set");
    CHECK(hw.active_samples == fragment.size(), "Proof3 fragment active count");
    CHECK(maxVectorDelta(fragment, original) == 0.0,
          "Proof3 fragment samples unchanged");
}

void proof4_burstTypePeakConsistency() {
    constexpr float kTarget = 0.5f;
    constexpr float kTolerance = 0.001f;

    std::cout << "\nProof4 hardware burst peak table\n";
    std::cout << "burst,active_samples,peak_before,gain,peak_after,target,fragment\n";
    std::cout << std::fixed << std::setprecision(6);

    for (const BurstSpec& spec : operationalBursts()) {
        std::vector<float> samples = spec.build();
        CHECK(!samples.empty(), spec.label + " generated samples");
        const auto hw = sim::normalizeTxBurstForHardware(samples, kTarget);
        std::cout << spec.label << ","
                  << hw.active_samples << ","
                  << hw.peak_before_gain << ","
                  << hw.gain_to_target << ","
                  << hw.peak_after_gain << ","
                  << hw.target_peak << ","
                  << (hw.burst_fragment_warning ? "yes" : "no") << "\n";
        CHECK(!hw.burst_fragment_warning, spec.label + " whole burst");
        CHECK(std::abs(hw.peak_after_gain - kTarget) <= kTolerance,
              spec.label + " post peak matches target");
        CHECK(std::abs(samplePeak(samples) - kTarget) <= kTolerance,
              spec.label + " measured post peak matches target");
    }
}

void proof5_pathDisjointness() {
    std::vector<float> burst = makeSyntheticBurst(0.85f, 0x5151u,
                                                  sim::kTxBurstMinimumActiveSamples + 3456);
    std::vector<float> sim_copy = burst;
    std::vector<float> hw_copy = burst;

    const auto sim_pre = sim::measureTxBurstInBandRms(sim_copy);
    const auto sim_norm = sim::normalizeTxBurstToReference(sim_copy);
    const auto sim_post = sim::measureTxBurstInBandRms(sim_copy);
    const auto hw_norm = sim::normalizeTxBurstForHardware(hw_copy, 0.5f);

    CHECK(!sim_norm.burst_fragment_warning, "Proof5 simulator path whole burst");
    CHECK(!hw_norm.burst_fragment_warning, "Proof5 hardware path whole burst");
    CHECK(hw_norm.active_begin == sim_pre.active_begin &&
              hw_norm.active_end == sim_pre.active_end &&
              hw_norm.active_samples == sim_pre.active_samples,
          "Proof5 shared active-region contract");
    CHECK(std::abs(sim_post.in_band_rms - channel::kModemReferenceInBandRms) <= 0.0005f,
          "Proof5 simulator path reaches reference RMS");
    CHECK(std::abs(samplePeak(hw_copy) - 0.5f) <= 0.001f,
          "Proof5 hardware path reaches target peak");

    std::vector<float> hw_repeat = burst;
    std::vector<float> sim_repeat = burst;
    const auto hw_repeat_norm = sim::normalizeTxBurstForHardware(hw_repeat, 0.5f);
    const auto sim_repeat_norm = sim::normalizeTxBurstToReference(sim_repeat);
    CHECK(std::abs(hw_repeat_norm.gain_to_target - hw_norm.gain_to_target) <= 1.0e-6f,
          "Proof5 hardware gain repeatable after simulator path");
    CHECK(std::abs(sim_repeat_norm.gain_to_reference - sim_norm.gain_to_reference) <= 1.0e-6f,
          "Proof5 simulator gain repeatable after hardware path");
    CHECK(maxVectorDelta(hw_copy, hw_repeat) <= 1.0e-6,
          "Proof5 hardware output independent of simulator path");
    CHECK(maxVectorDelta(sim_copy, sim_repeat) <= 1.0e-6,
          "Proof5 simulator output independent of hardware path");
}

void proof6_keyedDurationMatchesConnectionSampleModel() {
    constexpr ultra::Modulation kMod = ultra::Modulation::QAM8;
    constexpr ultra::CodeRate kRate = ultra::CodeRate::R2_3;
    constexpr int kCW = 12;

    auto frames = [=](size_t count, uint16_t base_seq) {
        std::vector<std::vector<uint8_t>> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            out.push_back(fixedDataFrame(
                kRate, kCW, static_cast<uint16_t>(base_seq + i),
                0x6000u + static_cast<uint32_t>(i)));
        }
        return out;
    };

    gui::ModemEngine engine;
    configureConnectedEngine(engine, kRate, kCW, kMod);
    // Connected entry deliberately forces the first group-start full. Consume
    // that one-shot so the next request is the ordinary descriptor+light-DATA
    // shape Connection models for a first-attempt round.
    (void)engine.transmitBurst(frames(5, 0), 0);

    const auto light_n5 = engine.transmitBurst(frames(5, 10), 1);
    const auto full_n5 = engine.transmitBurst(
        frames(5, 20), 2, gui::BurstAnchorOptions{true, false});
    const auto light_n8 = engine.transmitBurst(frames(8, 30), 3);
    const auto full_n8 = engine.transmitBurst(
        frames(8, 40), 4, gui::BurstAnchorOptions{true, false});

    CHECK(light_n5.size() == 374080,
          "Proof6 emitted light N5 equals descriptor + 5 data slots + guards");
    CHECK(full_n5.size() == 431680,
          "Proof6 emitted full N5 adds exactly one 57,600-sample anchor");
    CHECK(light_n8.size() == 552160,
          "Proof6 emitted light N8 equals descriptor + 8 data slots + guards");
    CHECK(full_n8.size() == 609760,
          "Proof6 emitted full N8 adds exactly one 57,600-sample anchor");

    const uint64_t model_n5 =
        protocol::connection_policy::postProcessedTxSamples(
            protocol::connection_policy::wideOFDMWireBurstSamples(
                kMod, kRate, 5, kCW, 27, false, ultra::Modulation::QPSK));
    const uint64_t model_n8_full =
        protocol::connection_policy::postProcessedTxSamples(
            protocol::connection_policy::wideOFDMWireBurstSamples(
                kMod, kRate, 8, kCW, 27, true, ultra::Modulation::QPSK));
    CHECK(model_n5 == light_n5.size() && model_n8_full == full_n8.size(),
          "Proof6 shared policy helper matches ModemEngine returned sample counts");
}

void proof7_disconnectAckUsesFullAnchor() {
    constexpr ultra::Modulation kMod = ultra::Modulation::QPSK;
    constexpr ultra::CodeRate kRate = ultra::CodeRate::R3_4;
    constexpr int kCW = 3;

    gui::ModemEngine engine;
    configureConnectedEngine(engine, kRate, kCW, kMod);

    // Consume connected entry's one-shot anchor so every length below is caused
    // by the frame classifier itself, not by session-start state.
    (void)engine.transmit(fixedDataFrame(kRate, kCW, 1, 0x7000u));

    const auto ordinary_ack = engine.transmit(
        v2::ControlFrame::makeAck("BRAVO", "ALPHA", 1).serialize());
    const auto disconnect_ack = engine.transmit(
        v2::ControlFrame::makeAck(
            "BRAVO", "ALPHA", v2::DISCONNECT_SEQ).serialize());
    const auto disconnect_request = engine.transmit(
        v2::ControlFrame::makeDisconnect("BRAVO", "ALPHA").serialize());
    const auto ordinary_ack_after = engine.transmit(
        v2::ControlFrame::makeAck("BRAVO", "ALPHA", 2).serialize());

    CHECK(!ordinary_ack.empty() && !disconnect_ack.empty() &&
              !disconnect_request.empty(),
          "Proof7 connected teardown controls must encode");
    CHECK(disconnect_ack.size() == disconnect_request.size(),
          "Proof7 sentinel ACK and DISCONNECT request must use the same full anchor");
    CHECK(disconnect_ack.size() >= ordinary_ack.size() + 20000,
          "Proof7 sentinel ACK must carry a chirp+LTS anchor, not ordinary light ACK sync");
    CHECK(ordinary_ack_after.size() == ordinary_ack.size(),
          "Proof7 full teardown ACK classification must not leak into later ACKs");
}

}  // namespace

int main() {
    ultra::setLogLevel(ultra::LogLevel::ERROR);

    proof1_apiCorrectness();
    proof2_operatorTargets();
    proof3_burstBoundaryInvariant();
    proof4_burstTypePeakConsistency();
    proof5_pathDisjointness();
    proof6_keyedDurationMatchesConnectionSampleModel();
    proof7_disconnectAckUsesFullAnchor();

    if (tests_failed == 0) {
        std::cout << "\nPASS: TxBurstHardwareNormalization (" << tests_run
                  << " checks)\n";
        return 0;
    }

    std::cout << "\nFAIL: " << tests_failed << "/" << tests_run
              << " checks failed\n";
    return 1;
}
