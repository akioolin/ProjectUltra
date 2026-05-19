#define ULTRA_SIM_STATION_TEST_HOOKS
#include "sim/simulated_station.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr size_t kFirstChunkSamples = SimulatedStation::SAMPLES_PER_CALLBACK;
constexpr size_t kSecondChunkSamples = SimulatedStation::SAMPLES_PER_CALLBACK;
constexpr size_t kTrSwitchSamples = SimulatedStation::SAMPLE_RATE * 20 / 1000;
constexpr size_t kCooldownSamples = SimulatedStation::SAMPLE_RATE * 100 / 1000;

std::vector<float> samples(size_t count, float value) {
    return std::vector<float>(count, value);
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
    ConnectionConfig config;
    config.pong_tx_delay_ms = 0;
    config.post_connect_data_delay_ms = 0;
    config.ack_tx_delay_ms = 0;
    return SimulatedStation(
        "ALPHA",
        std::make_unique<SinkAudioPort>(),
        OFDMConfigPreset::Default,
        mc_dpsk_presets::robust_mid(),
        config);
}

void continuationChunkBypassesRecoveryGate() {
    auto station = makeStation();

    station.testQueueTxSamples(samples(kFirstChunkSamples, 0.10f),
                               "frame=7 chunk=1/2");
    require(station.pttState() == PttState::TX, "first chunk should key TX");
    require(station.testTxQueueDepth() == kFirstChunkSamples,
            "first chunk should enter local TX queue");

    const size_t drained = station.testDrainLocalTxSamples(kFirstChunkSamples);
    require(drained == kFirstChunkSamples, "first chunk should drain fully");
    require(station.testTxQueueDepth() == 0,
            "local TX queue should be empty after draining first chunk");
    require(station.pttState() == PttState::TX_TR_SWITCH,
            "radio should be in post-TX recovery after first chunk drains");

    station.testQueueTxSamples(samples(kSecondChunkSamples, 0.20f),
                               "frame=7 chunk=2/2");

    require(station.pttState() == PttState::TX,
            "continuation chunk should keep the same TX keyed");
    require(station.testDeferredTxDepth() == 0,
            "continuation chunk must not enter deferred queue");
    require(station.testTxQueueDepth() == kSecondChunkSamples,
            "continuation chunk should append immediately to local TX queue");
}

void deferredLogicalSubmissionsFlushOnePerRadioKeyup() {
    auto station = makeStation();

    station.testQueueTxSamples(samples(kFirstChunkSamples, 0.30f),
                               "frame=seed");
    require(station.testDrainLocalTxSamples(kFirstChunkSamples) == kFirstChunkSamples,
            "seed frame should drain fully");
    station.testAdvanceRadioSamples(kTrSwitchSamples);
    require(station.pttState() == PttState::TX_COOLDOWN,
            "radio should be in cooldown after T/R switch");

    station.testQueueTxSamples(samples(120, 0.40f), "frame=A");
    station.testQueueTxSamples(samples(130, 0.50f), "frame=B");
    station.testQueueTxSamples(samples(140, 0.60f), "frame=C");
    require(station.testDeferredTxDepth() == 3,
            "three new logical submissions should be deferred during cooldown");
    require(station.testTxQueueDepth() == 0,
            "cooldown submissions should not enter local TX queue immediately");

    station.testAdvanceRadioSamples(kCooldownSamples);
    require(station.pttState() == PttState::RX,
            "radio should be RX-ready after cooldown expires");

    station.testFlushDeferredTxIfReady();
    require(station.pttState() == PttState::TX,
            "one deferred logical submission should key TX");
    require(station.testDeferredTxDepth() == 2,
            "only one deferred logical submission should flush per radio key-up");
    require(station.testTxQueueDepth() == 120,
            "first deferred logical submission should be the only queued audio");
}

}  // namespace

int main() {
    continuationChunkBypassesRecoveryGate();
    deferredLogicalSubmissionsFlushOnePerRadioKeyup();
    std::cout << "deferred TX fragmentation regression passed\n";
    return 0;
}
