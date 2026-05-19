#include "sim/simulated_station.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

constexpr size_t kCount = 16;
constexpr size_t kTrSwitchSamples = 48000 * 20 / 1000;
constexpr size_t kCooldownSamples = 48000 * 100 / 1000;

std::vector<float> signal(float base) {
    std::vector<float> out(kCount);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = base + static_cast<float>(i) * 0.01f;
    }
    return out;
}

void assertVectorNear(const std::vector<float>& actual,
                      const std::vector<float>& expected) {
    assert(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        assert(std::abs(actual[i] - expected[i]) < 1.0e-6f);
    }
}

void assertSilence(const std::vector<float>& actual) {
    assert(actual.size() == kCount);
    for (float sample : actual) {
        assert(sample == 0.0f);
    }
}

class BufferedClientAudioPort : public AudioPort {
public:
    void pushRx(std::vector<float> samples) {
        rx_.insert(rx_.end(), samples.begin(), samples.end());
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

void verifyVirtualAudioPortRadioGate() {
    SimulatedChannel channel;
    VirtualAudioPort port(channel, /*is_station_a=*/true);
    RadioPttStateMachine ptt;
    port.attachRadioState(&ptt);

    const auto during_tx = signal(0.10f);
    channel.transmitFromB(during_tx);
    ptt.noteTxQueued(kCount);
    assertSilence(port.pullRx(kCount));

    ptt.noteTxDrained(kCount);
    const auto during_tr_switch = signal(0.30f);
    channel.transmitFromB(during_tr_switch);
    assertSilence(port.pullRx(kCount));

    ptt.advanceSamples(kTrSwitchSamples);
    const auto during_cooldown = signal(0.50f);
    channel.transmitFromB(during_cooldown);
    assertVectorNear(port.pullRx(kCount), during_cooldown);

    ptt.advanceSamples(kCooldownSamples);
    const auto during_rx = signal(0.70f);
    channel.transmitFromB(during_rx);
    assertVectorNear(port.pullRx(kCount), during_rx);
}

void verifyBufferedClientRadioGate() {
    BufferedClientAudioPort port;
    RadioPttStateMachine ptt;
    port.attachRadioState(&ptt);

    ptt.noteTxQueued(kCount);
    port.pushRx(signal(0.20f));
    assertSilence(port.pullRx(kCount));

    ptt.noteTxDrained(kCount);
    ptt.advanceSamples(kTrSwitchSamples);
    const auto during_cooldown = signal(0.40f);
    port.pushRx(during_cooldown);
    assertVectorNear(port.pullRx(kCount), during_cooldown);
}

}  // namespace

int main() {
    verifyVirtualAudioPortRadioGate();
    verifyBufferedClientRadioGate();
    std::cout << "client audio ports consume then silence RX only during local TX/TR switch\n";
    return 0;
}
