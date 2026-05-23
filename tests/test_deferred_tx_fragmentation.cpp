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
constexpr size_t kPostTxAckListenSamples =
    SimulatedStation::SAMPLE_RATE * SimulatedStation::POST_TX_ACK_LISTEN_MS / 1000;

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
    require(station.pttState() == PttState::RX,
            "queued samples should not deafen RX until they reach the audio port");
    require(station.testTxQueueDepth() == kFirstChunkSamples,
            "first chunk should enter local TX queue");

    const size_t drained = station.testDrainLocalTxSamples(kFirstChunkSamples);
    require(drained == kFirstChunkSamples, "first chunk should drain fully");
    require(station.pttState() == PttState::TX,
            "active drained samples should key TX for that audio block");
    require(station.testTxQueueDepth() == 0,
            "local TX queue should be empty after draining first chunk");
    station.testObserveIdleTxBlock();
    require(station.pttState() == PttState::TX_TR_SWITCH,
            "radio should enter post-TX recovery on the first idle audio block");

    station.testQueueTxSamples(samples(kSecondChunkSamples, 0.20f),
                               "frame=7 chunk=2/2");

    require(station.pttState() == PttState::TX_TR_SWITCH,
            "queued continuation waits for its active audio block before keying TX");
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
    station.testObserveIdleTxBlock();
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
    require(station.testDeferredTxDepth() == 3,
            "post-TX ACK listen window should hold deferred TX briefly after RX opens");
    require(station.testTxQueueDepth() == 0,
            "post-TX ACK listen should keep local TX queue empty");

    station.testAdvanceRadioSamples(kPostTxAckListenSamples);
    station.testFlushDeferredTxIfReady();
    require(station.pttState() == PttState::RX,
            "flushing deferred audio queues samples without deafening RX first");
    require(station.testDeferredTxDepth() == 2,
            "only one deferred logical submission should flush per radio key-up");
    require(station.testTxQueueDepth() == 120,
            "first deferred logical submission should be the only queued audio");
}

void handshakeConnectUsesDeferredCarrierSenseGate() {
    auto station = makeStation();

    station.testQueueTxSamples(samples(120, 0.40f), "frame_type=CONNECT seq=0");
    require(station.pttState() == PttState::RX,
            "CONNECT deferral should not key PTT before the audio loop flush");
    require(station.testDeferredTxDepth() == 1,
            "CONNECT should wait for a fresh audio-loop carrier-sense observation");
    require(station.testTxQueueDepth() == 0,
            "CONNECT should not enter the local TX queue synchronously");

    station.testFlushDeferredTxIfReady();
    require(station.testDeferredTxDepth() == 1,
            "CONNECT should not flush before a fresh RX observation");
    require(station.testTxQueueDepth() == 0,
            "CONNECT should remain out of the local TX queue before RX refresh");

    station.testMarkRxObserved();
    station.testFlushDeferredTxIfReady();
    require(station.testDeferredTxDepth() == 0,
            "CONNECT should flush once carrier sense is fresh and idle");
    require(station.testTxQueueDepth() == 120,
            "CONNECT should enter the local TX queue after deferred flush");
}

void deferredArqAcksCoalesceToLatestState() {
    auto station = makeStation();

    station.testQueueTxSamples(samples(kFirstChunkSamples, 0.30f),
                               "frame=seed");
    require(station.testDrainLocalTxSamples(kFirstChunkSamples) == kFirstChunkSamples,
            "seed frame should drain fully");
    station.testObserveIdleTxBlock();
    station.testAdvanceRadioSamples(kTrSwitchSamples);
    require(station.pttState() == PttState::TX_COOLDOWN,
            "radio should be in cooldown before ACK deferral");

    station.testQueueTxSamples(samples(120, 0.40f), "frame_type=ACK seq=1");
    require(station.testDeferredTxDepth() == 1,
            "first carrier-sensed ACK should be deferred");

    station.testQueueTxSamples(samples(130, 0.50f), "frame_type=ACK seq=1");
    require(station.testDeferredTxDepth() == 1,
            "newer SACK state should replace older deferred ACK/SACK");

    station.testAdvanceRadioSamples(kCooldownSamples);
    station.testAdvanceRadioSamples(kPostTxAckListenSamples);
    station.testFlushDeferredTxIfReady();
    require(station.testDeferredTxDepth() == 0,
            "coalesced ACK should be the only deferred submission to flush");
    require(station.testTxQueueDepth() == 130,
            "latest deferred ACK/SACK waveform should be preserved");
}

}  // namespace

int main() {
    continuationChunkBypassesRecoveryGate();
    deferredLogicalSubmissionsFlushOnePerRadioKeyup();
    handshakeConnectUsesDeferredCarrierSenseGate();
    deferredArqAcksCoalesceToLatestState();
    std::cout << "deferred TX fragmentation regression passed\n";
    return 0;
}
