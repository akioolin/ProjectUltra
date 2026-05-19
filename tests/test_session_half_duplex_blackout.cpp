#include "ota_channel_core/session_context.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using ultra::ota_channel_core::ChannelType;
using ultra::ota_channel_core::SessionAudioBlock;
using ultra::ota_channel_core::SessionConfig;
using ultra::ota_channel_core::SessionContext;

namespace {

std::vector<float> constant(size_t count, float value) {
    return std::vector<float>(count, value);
}

void assertVectorNear(const std::vector<float>& actual,
                      const std::vector<float>& expected) {
    assert(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        assert(std::abs(actual[i] - expected[i]) < 1.0e-6f);
    }
}

const SessionAudioBlock* findBlock(const std::vector<SessionAudioBlock>& blocks,
                                   const std::string& station_id,
                                   uint64_t start_sample) {
    for (const auto& block : blocks) {
        if (block.station_id == station_id && block.start_sample == start_sample) {
            return &block;
        }
    }
    return nullptr;
}

std::unique_ptr<SessionContext> makeSession(const std::string& id) {
    SessionConfig config;
    config.session_id = id;
    config.display_name = id;
    config.default_channel_model = ChannelType::PASSTHROUGH;
    config.default_snr_db = 80.0f;
    config.seed = 0x5150u;
    config.station_cap = 3;
    auto session = std::make_unique<SessionContext>(std::move(config));
    assert(session->registerStation("alice"));
    assert(session->registerStation("bob"));
    assert(session->registerStation("charlie"));
    return session;
}

}  // namespace

int main() {
    auto session = makeSession("medium-mixer");
    const size_t tick_samples = session->sessionTickSamples();
    const auto alice_signal = constant(tick_samples, 0.25f);
    const auto bob_signal = constant(tick_samples, -0.5f);
    const auto charlie_signal = constant(tick_samples, 0.125f);

    assert(session->enqueueTransmit("alice", alice_signal));
    assert(session->enqueueTransmit("bob", bob_signal));
    assert(session->enqueueTransmit("charlie", charlie_signal));

    const auto tick = session->advanceSessionClock();
    const auto rx_blocks = session->drainReceiveOutbox();
    const auto* alice_rx = findBlock(rx_blocks, "alice", tick.start_sample);
    const auto* bob_rx = findBlock(rx_blocks, "bob", tick.start_sample);
    const auto* charlie_rx = findBlock(rx_blocks, "charlie", tick.start_sample);
    assert(alice_rx);
    assert(bob_rx);
    assert(charlie_rx);

    std::vector<float> alice_expected(tick_samples);
    std::vector<float> bob_expected(tick_samples);
    std::vector<float> charlie_expected(tick_samples);
    for (size_t i = 0; i < tick_samples; ++i) {
        alice_expected[i] = bob_signal[i] + charlie_signal[i];
        bob_expected[i] = alice_signal[i] + charlie_signal[i];
        charlie_expected[i] = alice_signal[i] + bob_signal[i];
    }

    assertVectorNear(alice_rx->samples, alice_expected);
    assertVectorNear(bob_rx->samples, bob_expected);
    assertVectorNear(charlie_rx->samples, charlie_expected);

    std::cout << "session medium mixer carries simultaneous stations and only removes self audio\n";
    return 0;
}
