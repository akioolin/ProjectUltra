#define ULTRA_SIM_STATION_TEST_HOOKS
#include "sim/simulated_station.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr size_t kFrameSamples = 50000;
constexpr size_t kFrameCount = 8;
constexpr size_t kAudioChainLimitSamples = SimulatedStation::SAMPLE_RATE * 100 / 1000;
constexpr size_t kMaxCallbacks = 4000;

std::vector<float> frameSamples(size_t index) {
    const float value = 0.05f + static_cast<float>(index) * 0.005f;
    return std::vector<float>(kFrameSamples, value);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

class SinkAudioPort : public AudioPort {
public:
    std::vector<float> pullRx(size_t count) override {
        return std::vector<float>(count, 0.0f);
    }

    void queueTx(const std::vector<float>&) override {}
};

SimulatedStation makeStation() {
    return SimulatedStation(
        "ALPHA",
        std::make_unique<SinkAudioPort>(),
        OFDMConfigPreset::Default,
        mc_dpsk_presets::robust_mid(),
        ConnectionConfig{});
}

void burstBacklogDoesNotBecomeAudioBacklog() {
    auto station = makeStation();

    for (size_t i = 0; i < kFrameCount; ++i) {
        station.testQueueTxSamples(frameSamples(i), "frame_type=DATA seq=" + std::to_string(i));
    }

    require(station.pttState() == PttState::RX,
            "logical enqueue must not key PTT before an audio pull");
    require(station.testTxLogicalDepth() == 1,
            "only the first logical TX should be ready for the next key-up");
    require(station.testDeferredTxDepth() == kFrameCount - 1,
            "ARQ-window backlog should wait as logical deferred submissions");

    std::vector<float> callback(SimulatedStation::SAMPLES_PER_CALLBACK, 0.0f);
    size_t peak_audio_pending = 0;
    size_t emitted_total = 0;
    require(station.txEmittedSampleClock() == 0,
            "emitted-sample clock should start at zero");

    for (size_t cb = 0; cb < kMaxCallbacks && emitted_total < kFrameSamples * kFrameCount; ++cb) {
        station.testAdvanceRadioSamples(SimulatedStation::SAMPLES_PER_CALLBACK);
        station.testFlushDeferredTxIfReady();

        auto result = station.pullTxSamples(callback.data(), callback.size());
        peak_audio_pending = std::max(peak_audio_pending, result.audio_chain_pending_samples);
        emitted_total += result.emitted_samples;
        require(station.txEmittedSampleClock() == emitted_total,
                "emitted-sample clock should count waveform samples, not idle callbacks");

        require(result.audio_chain_pending_samples <= kAudioChainLimitSamples,
                "TX_pending exceeded the 100 ms audio-chain bound");

        station.testMarkRxObserved();
        station.testFlushDeferredTxIfReady();
    }

    require(emitted_total == kFrameSamples * kFrameCount,
            "all queued frame-sized submissions should eventually go on air");
    require(station.txEmittedSampleClock() == kFrameSamples * kFrameCount,
            "final emitted-sample clock should equal actual queued waveform length");
    require(peak_audio_pending <= SimulatedStation::SAMPLES_PER_CALLBACK,
            "pull-clocked audio chain should expose at most one callback as pending");

    auto idle_result = station.pullTxSamples(callback.data(), callback.size());
    require(idle_result.emitted_samples == 0,
            "idle pull should not emit waveform samples");
    require(station.txEmittedSampleClock() == kFrameSamples * kFrameCount,
            "idle pull should not advance emitted-sample clock");
}

}  // namespace

int main() {
    burstBacklogDoesNotBecomeAudioBacklog();
    std::cout << "radio realism TX_pending stayed callback-bounded under ARQ-like backlog\n";
    return 0;
}
