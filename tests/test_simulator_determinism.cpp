#include "ota_channel_core/models.hpp"
#include "ota_channel_core/session_context.hpp"
#include "ota_simulator_service/audio_plane.hpp"

#include <cassert>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace channel = ultra::ota_channel_core;
namespace service = ultra::ota_simulator_service;

namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

struct SimResult {
    int retx = 0;
    int frames_sent = 0;
    int frame_success = 0;
    uint64_t rx_hash = 0;
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "SimulatorDeterminism failed: " << message << "\n";
        std::abort();
    }
}

uint64_t hashSamples(std::span<const float> samples) {
    uint64_t hash = kFnvOffset;
    for (float sample : samples) {
        const uint32_t bits = std::bit_cast<uint32_t>(sample);
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= static_cast<uint8_t>((bits >> shift) & 0xffu);
            hash *= kFnvPrime;
        }
    }
    return hash;
}

std::vector<float> payloadFrame() {
    std::vector<float> payload(480, 0.0f);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = 0.35f * std::sin(static_cast<float>(i) * 0.03125f);
    }
    return payload;
}

SimResult runSameSeedCell(int arrival_delay_ticks) {
    channel::SessionConfig cfg;
    cfg.session_id = "determinism";
    cfg.display_name = "determinism";
    cfg.default_channel_model = channel::ChannelType::AWGN;
    cfg.default_snr_db = 10.0f;
    cfg.seed = 200;

    channel::SessionContext session(cfg);
    require(session.registerStation("ALPHA"), "register ALPHA");
    require(session.registerStation("BRAVO"), "register BRAVO");

    std::vector<float> bravo_rx;
    const auto drain_bravo = [&](const channel::SessionClockTick& tick) {
        for (const auto& block : tick.rx_blocks) {
            if (block.station_id == "BRAVO") {
                bravo_rx.insert(bravo_rx.end(), block.samples.begin(), block.samples.end());
            }
        }
    };

    for (int i = 0; i < arrival_delay_ticks; ++i) {
        drain_bravo(session.advanceSessionClock());
    }

    const uint64_t scheduled_start_sample = 4800;
    const auto payload = payloadFrame();
    require(session.submitTransmit("ALPHA", scheduled_start_sample, payload),
            "submit timestamped ALPHA payload");

    while (session.sessionClockSamples() < scheduled_start_sample + payload.size() + 480) {
        drain_bravo(session.advanceSessionClock());
    }

    const size_t begin = static_cast<size_t>(scheduled_start_sample);
    const size_t end = begin + payload.size();
    require(bravo_rx.size() >= end, "BRAVO RX contains scheduled payload window");

    double payload_energy = 0.0;
    for (size_t i = begin; i < end; ++i) {
        payload_energy += static_cast<double>(bravo_rx[i]) *
                          static_cast<double>(bravo_rx[i]);
    }

    SimResult result;
    result.frames_sent = 1;
    result.frame_success = payload_energy > 1.0 ? 100 : 0;
    result.retx = result.frame_success == 100 ? 0 : 1;
    result.rx_hash = hashSamples(bravo_rx);
    return result;
}

void checkAwgnSampleIndexing() {
    channel::RngRoot root(200);
    const uint32_t seed = root.childSeed("determinism:awgn");
    channel::AWGNChannelModel whole_model(10.0f, seed);
    channel::AWGNChannelModel split_model(10.0f, seed);

    std::vector<float> input(960, 0.0f);
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = 0.25f * std::sin(static_cast<float>(i) * 0.05f);
    }

    const uint64_t start_sample = 123456;
    const auto whole = whole_model.process(input, start_sample);

    std::vector<float> split;
    std::vector<float> first(input.begin(), input.begin() + 480);
    std::vector<float> second(input.begin() + 480, input.end());
    auto first_out = split_model.process(first, start_sample);
    (void)split_model.process(std::vector<float>(128, 0.0f), 42);
    auto second_out = split_model.process(second, start_sample + first.size());
    split.insert(split.end(), first_out.begin(), first_out.end());
    split.insert(split.end(), second_out.begin(), second_out.end());

    require(whole.size() == split.size(), "AWGN split size");
    for (size_t i = 0; i < whole.size(); ++i) {
        require(whole[i] == split[i], "AWGN sample-indexed split mismatch");
    }
}

double txEnergyForStation(const channel::SessionClockTick& tick, std::string_view station_id) {
    double energy = 0.0;
    for (const auto& block : tick.tx_blocks) {
        if (block.station_id == station_id) {
            for (float sample : block.samples) {
                energy += static_cast<double>(sample) * static_cast<double>(sample);
            }
        }
    }
    return energy;
}

double rxEnergyForStation(const channel::SessionClockTick& tick, std::string_view station_id) {
    double energy = 0.0;
    for (const auto& block : tick.rx_blocks) {
        if (block.station_id == station_id) {
            for (float sample : block.samples) {
                energy += static_cast<double>(sample) * static_cast<double>(sample);
            }
        }
    }
    return energy;
}

std::unique_ptr<channel::SessionContext> makeAwgnSession(std::string session_id) {
    channel::SessionConfig cfg;
    cfg.session_id = std::move(session_id);
    cfg.display_name = cfg.session_id;
    cfg.default_channel_model = channel::ChannelType::AWGN;
    cfg.default_snr_db = 10.0f;
    cfg.seed = 200;

    auto session = std::make_unique<channel::SessionContext>(cfg);
    require(session->registerStation("ALPHA"), "register bridge ALPHA");
    require(session->registerStation("BRAVO"), "register bridge BRAVO");
    return session;
}

void checkLocalClockBridgeRegression() {
    auto broken = makeAwgnSession("bridge-broken");
    (void)broken->advanceSessionClock();
    require(broken->sessionClockSamples() == broken->sessionTickSamples(),
            "diagnostic pre-tick did not advance");
    const auto payload = payloadFrame();
    require(broken->submitTransmit("ALPHA", 0, payload), "submit local start directly");
    const auto dropped_tick = broken->advanceSessionClock();
    require(txEnergyForStation(dropped_tick, "ALPHA") == 0.0,
            "direct local start unexpectedly reached mixer");

    service::LeaseAudioClockBridge bridge;
    auto bridged = makeAwgnSession("bridge-fixed");
    (void)bridged->advanceSessionClock();

    const std::vector<float> prime(8, 0.0f);
    const uint64_t prime_earliest =
        bridged->sessionClockSamples() + bridged->sessionTickSamples();
    auto scheduled = bridge.push(0, prime, prime_earliest);
    require(scheduled.size() == 1, "prime scheduled");
    require(scheduled[0].start_sample == prime_earliest,
            "prime mapped to session clock");
    require(bridged->submitTransmit("ALPHA", scheduled[0].start_sample, scheduled[0].samples),
            "submit bridged prime");

    while (bridged->sessionClockSamples() < 4800) {
        (void)bridged->advanceSessionClock();
    }

    const uint64_t payload_earliest =
        bridged->sessionClockSamples() + bridged->sessionTickSamples();
    scheduled = bridge.push(prime.size(), payload, payload_earliest);
    require(scheduled.size() == 1, "payload scheduled");
    require(scheduled[0].start_sample == payload_earliest,
            "payload escaped stale prime session time");
    require(bridged->submitTransmit("ALPHA", scheduled[0].start_sample, scheduled[0].samples),
            "submit bridged payload");

    channel::SessionClockTick payload_tick;
    while (bridged->sessionClockSamples() <= scheduled[0].start_sample) {
        payload_tick = bridged->advanceSessionClock();
    }
    require(payload_tick.start_sample == scheduled[0].start_sample,
            "payload tick reached");
    require(txEnergyForStation(payload_tick, "ALPHA") > 20.0,
            "bridged payload missing from mixer");
    require(rxEnergyForStation(payload_tick, "BRAVO") > 50.0,
            "bridged payload missing from receiver");
}

}  // namespace

int main() {
    checkAwgnSampleIndexing();
    checkLocalClockBridgeRegression();

    // Acceptance gate for /tmp/simulator_determinism_findings.md Round 1:
    // same AWGN seed/SNR with different packet arrival ticks must produce
    // bit-identical harness metrics.
    const SimResult reference = runSameSeedCell(0);
    for (int trial = 1; trial < 10; ++trial) {
        const SimResult got = runSameSeedCell(trial);
        require(got.retx == reference.retx, "retx varied");
        require(got.frames_sent == reference.frames_sent, "frames_sent varied");
        require(got.frame_success == reference.frame_success, "frame_success varied");
        require(got.rx_hash == reference.rx_hash, "RX samples varied");
    }

    std::cout << "SimulatorDeterminism retx=" << reference.retx
              << " frames_sent=" << reference.frames_sent
              << " frame_success=" << reference.frame_success
              << " rx_hash=" << reference.rx_hash << "\n";
    return 0;
}
