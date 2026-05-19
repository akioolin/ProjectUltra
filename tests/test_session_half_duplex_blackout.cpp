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
using ultra::ota_channel_core::StationTxAudioState;

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
    config.station_cap = 2;
    auto session = std::make_unique<SessionContext>(std::move(config));
    assert(session->registerStation("alice"));
    assert(session->registerStation("bob"));
    return session;
}

void enqueueTick(SessionContext& session,
                 const std::vector<float>& alice_tx,
                 StationTxAudioState alice_state,
                 const std::vector<float>& bob_tx,
                 StationTxAudioState bob_state) {
    assert(session.enqueueTransmit("alice", alice_tx, alice_state));
    assert(session.enqueueTransmit("bob", bob_tx, bob_state));
}

}  // namespace

int main() {
    auto legacy = makeSession("legacy");
    const size_t tick_samples = legacy->sessionTickSamples();
    const auto alice_signal = constant(tick_samples, 0.25f);
    const auto bob_signal = constant(tick_samples, -0.5f);
    const auto silence = constant(tick_samples, 0.0f);

    enqueueTick(*legacy,
                alice_signal,
                {.tx_state_valid = false, .tx_active = true},
                bob_signal,
                {.tx_state_valid = false, .tx_active = true});
    auto tick = legacy->advanceSessionClock();
    auto rx_blocks = legacy->drainReceiveOutbox();
    const auto* alice_rx = findBlock(rx_blocks, "alice", tick.start_sample);
    const auto* bob_rx = findBlock(rx_blocks, "bob", tick.start_sample);
    assert(alice_rx);
    assert(bob_rx);
    assertVectorNear(alice_rx->samples, bob_signal);
    assertVectorNear(bob_rx->samples, alice_signal);

    auto half_duplex = makeSession("half-duplex");

    enqueueTick(*half_duplex,
                alice_signal,
                {.tx_state_valid = true, .tx_active = true},
                silence,
                {.tx_state_valid = true, .tx_active = false});
    tick = half_duplex->advanceSessionClock();
    rx_blocks = half_duplex->drainReceiveOutbox();
    alice_rx = findBlock(rx_blocks, "alice", tick.start_sample);
    bob_rx = findBlock(rx_blocks, "bob", tick.start_sample);
    assert(alice_rx);
    assert(bob_rx);
    assertVectorNear(alice_rx->samples, silence);
    assertVectorNear(bob_rx->samples, alice_signal);

    enqueueTick(*half_duplex,
                alice_signal,
                {.tx_state_valid = true, .tx_active = true},
                bob_signal,
                {.tx_state_valid = true, .tx_active = true});
    tick = half_duplex->advanceSessionClock();
    rx_blocks = half_duplex->drainReceiveOutbox();
    alice_rx = findBlock(rx_blocks, "alice", tick.start_sample);
    bob_rx = findBlock(rx_blocks, "bob", tick.start_sample);
    assert(alice_rx);
    assert(bob_rx);
    assertVectorNear(alice_rx->samples, silence);
    assertVectorNear(bob_rx->samples, silence);

    enqueueTick(*half_duplex,
                silence,
                {.tx_state_valid = true, .tx_active = false},
                bob_signal,
                {.tx_state_valid = true, .tx_active = true});
    tick = half_duplex->advanceSessionClock();
    rx_blocks = half_duplex->drainReceiveOutbox();
    alice_rx = findBlock(rx_blocks, "alice", tick.start_sample);
    bob_rx = findBlock(rx_blocks, "bob", tick.start_sample);
    assert(alice_rx);
    assert(bob_rx);
    assertVectorNear(alice_rx->samples, bob_signal);
    assertVectorNear(bob_rx->samples, silence);

    std::cout << "session half-duplex TX-state blackout preserves legacy clients and "
                 "blocks peer audio during local TX\n";
    return 0;
}
