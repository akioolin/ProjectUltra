#include "protocol/protocol_engine.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/types.hpp"

#include <array>
#include <iostream>
#include <queue>

using namespace ultra::protocol;
using ultra::Bytes;

namespace {

int tests_run = 0;
int tests_passed = 0;

#define TEST(name) \
    do { std::cout << "  Testing " << name << "... " << std::flush; ++tests_run; } while (0)

#define PASS() \
    do { std::cout << "PASS\n"; ++tests_passed; } while (0)

#define FAIL(msg) \
    do { std::cout << "FAIL: " << msg << "\n"; return false; } while (0)

class SimulatedChannel {
public:
    SimulatedChannel(ProtocolEngine& station_a, ProtocolEngine& station_b)
        : station_a_(station_a), station_b_(station_b) {
        station_a_.setTxDataCallback([this](const Bytes& data, bool) {
            recordFrame(data);
            pending_b_.push(data);
        });

        station_b_.setTxDataCallback([this](const Bytes& data, bool) {
            recordFrame(data);
            pending_a_.push(data);
        });
    }

    void deliver() {
        while (!pending_a_.empty()) {
            station_a_.onRxData(pending_a_.front());
            pending_a_.pop();
        }
        while (!pending_b_.empty()) {
            station_b_.onRxData(pending_b_.front());
            pending_b_.pop();
        }
    }

    void tick(uint32_t ms) {
        station_a_.tick(ms);
        station_b_.tick(ms);
    }

    void run(int cycles, uint32_t tick_ms = 100) {
        for (int i = 0; i < cycles; ++i) {
            deliver();
            tick(tick_ms);
        }
    }

    int frameCount(v2::FrameType type) const {
        return frame_type_counts_[static_cast<size_t>(static_cast<uint8_t>(type))];
    }

private:
    void recordFrame(const Bytes& data) {
        const auto header = v2::parseHeader(data);
        if (!header.valid) {
            return;
        }
        ++frame_type_counts_[static_cast<size_t>(static_cast<uint8_t>(header.type))];
    }

    ProtocolEngine& station_a_;
    ProtocolEngine& station_b_;
    std::queue<Bytes> pending_a_;
    std::queue<Bytes> pending_b_;
    std::array<int, 256> frame_type_counts_{};
};

bool noConnectFailures(const ProtocolEngine& station_a, const ProtocolEngine& station_b) {
    return station_a.getStats().connects_failed == 0 &&
           station_b.getStats().connects_failed == 0;
}

bool test_connect_during_probing_is_accepted() {
    TEST("CONNECT during PROBING is accepted");

    ConnectionConfig config;
    config.auto_accept = true;

    ProtocolEngine station_a(config);
    ProtocolEngine station_b(config);
    station_a.setLocalCallsign("ALPHA");
    station_b.setLocalCallsign("BRAVO");

    SimulatedChannel channel(station_a, station_b);

    station_a.setPingTxCallback([]() {
        // Keep ALPHA in PROBING; this test injects BRAVO's CONNECT directly.
    });

    if (!station_a.connect("BRAVO")) {
        FAIL("ALPHA failed to enter PROBING");
    }
    if (station_a.getState() != ConnectionState::PROBING) {
        FAIL("ALPHA should be PROBING before inbound CONNECT");
    }

    if (!station_b.connect("ALPHA")) {
        FAIL("BRAVO failed to send CONNECT");
    }
    if (station_b.getState() != ConnectionState::CONNECTING) {
        FAIL("BRAVO should be CONNECTING after direct CONNECT");
    }

    channel.run(10, 100);

    if (!station_a.isConnected()) {
        FAIL("ALPHA did not accept inbound CONNECT while PROBING");
    }
    if (!station_b.isConnected()) {
        FAIL("BRAVO did not receive CONNECT_ACK");
    }
    if (channel.frameCount(v2::FrameType::CONNECT_ACK) != 1) {
        FAIL("expected exactly one CONNECT_ACK");
    }
    if (!noConnectFailures(station_a, station_b)) {
        FAIL("connects_failed incremented during PROBING call collision");
    }

    PASS();
    return true;
}

bool test_simultaneous_connect_uses_callsign_tiebreaker() {
    TEST("simultaneous CONNECT uses callsign tiebreaker");

    ConnectionConfig config;
    config.auto_accept = true;

    ProtocolEngine station_a(config);
    ProtocolEngine station_b(config);
    station_a.setLocalCallsign("ALPHA");
    station_b.setLocalCallsign("BRAVO");

    SimulatedChannel channel(station_a, station_b);

    if (!station_a.connect("BRAVO")) {
        FAIL("ALPHA failed to send CONNECT");
    }
    if (!station_b.connect("ALPHA")) {
        FAIL("BRAVO failed to send CONNECT");
    }
    if (station_a.getState() != ConnectionState::CONNECTING ||
        station_b.getState() != ConnectionState::CONNECTING) {
        FAIL("both stations should start in CONNECTING");
    }

    channel.run(10, 100);

    if (!station_a.isConnected() || !station_b.isConnected()) {
        FAIL("simultaneous CONNECT did not converge");
    }
    if (channel.frameCount(v2::FrameType::CONNECT_ACK) != 1) {
        FAIL("expected exactly one CONNECT_ACK after tiebreaker");
    }
    if (channel.frameCount(v2::FrameType::CONNECT_NAK) != 0) {
        FAIL("tiebreaker should not send CONNECT_NAK");
    }
    if (!noConnectFailures(station_a, station_b)) {
        FAIL("connects_failed incremented during simultaneous CONNECT");
    }

    PASS();
    return true;
}

} // namespace

int main() {
    std::cout << "=== Connection Call-Collision Tests ===\n\n";

    test_connect_during_probing_is_accepted();
    test_simultaneous_connect_uses_callsign_tiebreaker();

    std::cout << "\n=== Results: " << tests_passed << "/" << tests_run
              << " passed ===\n";
    return tests_passed == tests_run ? 0 : 1;
}
