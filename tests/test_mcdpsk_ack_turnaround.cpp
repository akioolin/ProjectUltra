#include "protocol/connection.hpp"
#include "protocol/connection_policy.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace ultra;
using namespace ultra::protocol;

namespace ultra {
namespace protocol {

struct ConnectionAdaptiveTestAccess {
    static void makeConnectedMCDPSK(Connection& c,
                                    Modulation mod,
                                    CodeRate rate,
                                    int carriers,
                                    int samples_per_symbol,
                                    int cw_count) {
        c.local_call_ = "BRAVO";
        c.remote_call_ = "ALPHA";
        c.state_ = ConnectionState::CONNECTED;
        c.is_initiator_ = false;
        c.handshake_confirmed_ = true;
        c.negotiated_mode_ = WaveformMode::MC_DPSK;
        c.data_modulation_ = mod;
        c.data_code_rate_ = rate;
        c.data_frame_cw_count_ = cw_count;
        c.config_.mc_dpsk_num_carriers = carriers;
        c.config_.mc_dpsk_samples_per_symbol = samples_per_symbol;
        c.arq_.setCallsigns(c.local_call_, c.remote_call_);
        c.configureArqForCurrentDataMode();
    }

    static int arqAckRepeatCount(const Connection& c) {
        return c.arq_.getAckRepeatCount();
    }

};

}  // namespace protocol
}  // namespace ultra

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

uint32_t physicalAckBurstMs(const connection_policy::MCDPSKFrameTiming& timing) {
    return connection_policy::kMCDPSKDualChirpPreambleMs +
           timing.ack_ms +
           connection_policy::kMCDPSKInterFrameGuardMs;
}

void mcdpskAckDoesNotRepeatIntoNextDataTurn() {
    constexpr int kCarriers = 8;
    constexpr int kSamplesPerSymbol = 1024;
    constexpr int kDataCodewords = 4;

    const auto timing = connection_policy::mcDpskFrameTiming(
        Modulation::DQPSK, kCarriers, kSamplesPerSymbol, kDataCodewords);
    const uint32_t ack_burst_ms = physicalAckBurstMs(timing);
    const uint32_t old_repeat_delay_ms =
        std::clamp<uint32_t>(timing.ack_ms + 250u, 750u, 3000u);

    require(ack_burst_ms >= 2300 && ack_burst_ms <= 2500,
            "regression setup should model the observed ~2.4s MC-DPSK ACK burst");
    require(old_repeat_delay_ms < ack_burst_ms,
            "old DQPSK ACK repeat would be queued before the primary ACK finished");

    Connection c;
    ConnectionAdaptiveTestAccess::makeConnectedMCDPSK(
        c, Modulation::DQPSK, CodeRate::R1_4,
        kCarriers, kSamplesPerSymbol, kDataCodewords);
    require(ConnectionAdaptiveTestAccess::arqAckRepeatCount(c) == 1,
            "connected MC-DPSK DQPSK ARQ must send only the primary ACK");
}

}  // namespace

int main() {
    mcdpskAckDoesNotRepeatIntoNextDataTurn();
    std::cout << "MC-DPSK ACK turnaround regression passed\n";
    return 0;
}
