#include "sim/simulated_station.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::chrono::milliseconds kTickInterval(10);
constexpr std::chrono::seconds kConnectTimeout(40);
constexpr std::chrono::seconds kDataTimeout(80);

template <typename Predicate>
bool runUntil(SimulatedStation& alpha,
              SimulatedStation& bravo,
              std::chrono::steady_clock::duration timeout,
              Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        alpha.tick();
        bravo.tick();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(kTickInterval);
    }

    alpha.tick();
    bravo.tick();
    return predicate();
}

void printStats(const char* label, SimulatedStation& station) {
    const auto cs = station.getConnectionStats();
    const auto ds = station.getDecoderStats();
    std::cout << label
              << " arq_sent=" << cs.arq.frames_sent
              << " arq_rcvd=" << cs.arq.frames_received
              << " acks_sent=" << cs.arq.acks_sent
              << " acks_rcvd=" << cs.arq.acks_received
              << " retx=" << cs.arq.retransmissions
              << " timeouts=" << cs.arq.timeouts
              << " failed=" << cs.arq.failed
              << " rx_decoded=" << ds.frames_decoded
              << " rx_failed=" << ds.frames_failed
              << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    const uint64_t seed = argc > 1 ? static_cast<uint64_t>(std::strtoull(argv[1], nullptr, 10))
                                   : 42;
    const float snr_db = argc > 2 ? std::strtof(argv[2], nullptr) : 20.0f;

    ultra::setLogLevel(std::getenv("MCDPSK_TIMING_VERBOSE")
                           ? ultra::LogLevel::INFO
                           : ultra::LogLevel::ERROR);

    SimulatedChannel channel;
    channel.setSeed(seed);
    channel.configure(snr_db, ChannelType::AWGN);

    ConnectionConfig alpha_config;
    ConnectionConfig bravo_config;
    alpha_config.pong_tx_delay_ms = 0;
    alpha_config.post_connect_data_delay_ms = 0;
    alpha_config.ack_tx_delay_ms = 0;
    bravo_config.pong_tx_delay_ms = 0;
    bravo_config.post_connect_data_delay_ms = 0;
    bravo_config.ack_tx_delay_ms = 0;

    SimulatedStation alpha(
        "ALPHA",
        std::make_unique<VirtualAudioPort>(channel, /*is_station_a=*/true),
        OFDMConfigPreset::Default,
        mc_dpsk_presets::robust_mid(),
        alpha_config);
    SimulatedStation bravo(
        "BRAVO",
        std::make_unique<VirtualAudioPort>(channel, /*is_station_a=*/false),
        OFDMConfigPreset::Default,
        mc_dpsk_presets::robust_mid(),
        bravo_config);

    alpha.setSNR(snr_db);
    bravo.setSNR(snr_db);
    alpha.setPreferredWaveform(WaveformMode::MC_DPSK);
    alpha.setForcedCodeRate(CodeRate::R1_4);

    std::mutex received_mutex;
    std::vector<std::string> received_messages;
    bravo.setMessageCallback([&](const std::string& msg) {
        std::lock_guard<std::mutex> lock(received_mutex);
        received_messages.push_back(msg);
    });

    alpha.start();
    bravo.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    alpha.connect("BRAVO");
    const bool connected = runUntil(alpha, bravo, kConnectTimeout, [&]() {
        return alpha.isConnected() && bravo.isConnected() &&
               alpha.isHandshakeComplete();
    });
    if (!connected) {
        std::cout << "FAIL: MC-DPSK handshake did not complete\n";
        printStats("ALPHA", alpha);
        printStats("BRAVO", bravo);
        alpha.stop();
        bravo.stop();
        return 1;
    }

    const std::vector<std::string> test_messages = {
        "Message 1 from ALPHA",
        "Message 2 from ALPHA",
    };

    alpha.sendMessages(test_messages);

    const bool delivered = runUntil(alpha, bravo, kDataTimeout, [&]() {
        std::lock_guard<std::mutex> lock(received_mutex);
        return received_messages.size() >= test_messages.size();
    });

    printStats("ALPHA", alpha);
    printStats("BRAVO", bravo);
    alpha.stop();
    bravo.stop();

    if (!delivered) {
        std::cout << "FAIL: BRAVO did not receive all MC-DPSK messages\n";
        return 1;
    }
    {
        std::lock_guard<std::mutex> lock(received_mutex);
        for (size_t i = 0; i < test_messages.size(); ++i) {
            if (received_messages[i] != test_messages[i]) {
                std::cout << "FAIL: message " << i << " mismatch\n";
                return 1;
            }
        }
    }
    if (alpha.getConnectionStats().arq.timeouts > 1 ||
        alpha.getConnectionStats().arq.retransmissions > 1) {
        std::cout << "FAIL: MC-DPSK batch exceeded low-retry envelope\n";
        return 1;
    }
    std::cout << "MC-DPSK half-duplex batch timing passed\n";
    return 0;
}
