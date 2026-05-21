#include "sim/simulated_station.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

namespace {

constexpr size_t kCount = 480;

std::vector<float> samples(float value) {
    return std::vector<float>(kCount, value);
}

class BufferedAudioPort : public AudioPort {
public:
    explicit BufferedAudioPort(ultra::audio::ChannelBusyDetectorConfig config = {})
        : AudioPort(config) {}

    void pushRx(std::vector<float> in) {
        rx_.insert(rx_.end(), in.begin(), in.end());
    }

    std::vector<float> pullRx(size_t count) override {
        const size_t take = std::min(count, rx_.size());
        std::vector<float> out(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(take));
        rx_.erase(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(take));
        return shapeRxForLocalRadio(std::move(out), count);
    }

    void queueTx(const std::vector<float>&) override {}

private:
    std::vector<float> rx_;
};

void observe(BufferedAudioPort& port, float rms) {
    port.pushRx(samples(rms));
    (void)port.pullRx(kCount);
}

void observeAt(BufferedAudioPort& port,
               const std::vector<float>& in,
               uint64_t start_sample) {
    port.setCarrierSenseSampleClock(start_sample);
    port.pushRx(in);
    (void)port.pullRx(in.size());
}

void waitBlocksUntilRxRmsDrops() {
    BufferedAudioPort port;
    observe(port, 0.100f);
    assert(!port.isChannelIdle());

    auto waiter = std::async(std::launch::async, [&port]() {
        return port.waitForChannelIdle(/*guard_ms=*/20, /*max_wait_ms=*/1000);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(waiter.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);

    for (int i = 0; i < 6; ++i) {
        observe(port, 0.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(waiter.get());
    assert(port.isChannelIdleFor(20));
}

void localTxBlackoutKeepsPortBusyEvenWithSilentRx() {
    BufferedAudioPort port;
    RadioPttStateMachine ptt;
    port.attachRadioState(&ptt);

    ptt.noteTxQueued(kCount);
    observe(port, 0.0f);
    assert(!port.isChannelIdle());

    auto waiter = std::async(std::launch::async, [&port]() {
        return port.waitForChannelIdle(/*guard_ms=*/10, /*max_wait_ms=*/1000);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(waiter.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);

    ptt.noteTxDrained(kCount);
    ptt.advanceSamples(48000 * 20 / 1000);
    ptt.advanceSamples(48000 * 100 / 1000);

    for (int i = 0; i < 5; ++i) {
        observe(port, 0.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(waiter.get());
}

void otasimCarrierSenseLearnsCalibratedGoodChannelIdleNoise() {
    BufferedAudioPort port(virtualAudioCarrierSenseConfig());
    ultra::ota_channel_core::ChannelConfig config{
        .type = ChannelType::GOOD,
        .snr_db = 12.0f,
        .seed = 42,
        .sample_rate = SimulatedStation::SAMPLE_RATE};
    ultra::ota_channel_core::RngRoot root(config.seed);
    auto channel = ultra::ota_channel_core::createChannelModel(
        config, root, "test:otasim:carrier_sense_idle_noise");

    std::vector<float> zeros(kCount, 0.0f);
    std::vector<float> rx;
    for (int block = 0; block < 40; ++block) {
        const uint64_t start_sample = static_cast<uint64_t>(block) * kCount;
        channel->process(zeros, start_sample, rx);
        observeAt(port, rx, start_sample);
    }

    port.setCarrierSenseSampleClock(40ULL * kCount);
    assert(port.channelRms() > 0.01f);
    assert(port.isChannelIdleFor(SimulatedStation::DEFAULT_TR_GUARD_MS));
}

}  // namespace

int main() {
    waitBlocksUntilRxRmsDrops();
    localTxBlackoutKeepsPortBusyEvenWithSilentRx();
    otasimCarrierSenseLearnsCalibratedGoodChannelIdleNoise();
    std::cout << "AudioPort carrier sense tests passed\n";
    return 0;
}
