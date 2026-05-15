#include "protocol/protocol_engine.hpp"

#include <iostream>
#include <string>

using namespace ultra::protocol;

namespace {

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg) \
    do { \
        ++tests_run; \
        if (!(cond)) { \
            ++tests_failed; \
            std::cout << "FAIL: " << msg << "\n"; \
            return; \
        } \
    } while (0)

ProtocolEngine makeStation(uint32_t pong_delay_ms) {
    ConnectionConfig config;
    config.pong_tx_delay_ms = pong_delay_ms;
    return ProtocolEngine(config);
}

void test_pong_tx_callback_is_deferred() {
    constexpr uint32_t kDelayMs = 25;
    ProtocolEngine station = makeStation(kDelayMs);

    int pong_callbacks = 0;
    station.setPingReceivedCallback([&]() {
        ++pong_callbacks;
    });

    station.onPingReceived();
    CHECK(pong_callbacks == 0, "PONG callback should not fire synchronously");

    station.tick(kDelayMs - 1);
    CHECK(pong_callbacks == 0, "PONG callback should wait through delay-1");

    station.tick(1);
    CHECK(pong_callbacks == 1, "PONG callback should fire at configured delay");

    station.tick(kDelayMs);
    CHECK(pong_callbacks == 1, "deferred callback should fire once");
}

void test_pong_tx_callback_cancels_when_connecting() {
    constexpr uint32_t kDelayMs = 50;
    ProtocolEngine station = makeStation(kDelayMs);
    station.setLocalCallsign("W1ABC");
    station.setPingTxCallback([]() {});

    int pong_callbacks = 0;
    station.setPingReceivedCallback([&]() {
        ++pong_callbacks;
    });

    station.onPingReceived();
    station.tick(20);
    CHECK(pong_callbacks == 0, "PONG callback should still be pending before connect");

    CHECK(station.connect("K2DEF"), "connect should leave DISCONNECTED");
    CHECK(station.getState() == ConnectionState::PROBING, "station should be probing after connect");

    station.tick(kDelayMs);
    CHECK(pong_callbacks == 0, "PONG callback should cancel after leaving DISCONNECTED");
}

void test_reping_restarts_pong_tx_delay() {
    constexpr uint32_t kDelayMs = 30;
    ProtocolEngine station = makeStation(kDelayMs);

    int pong_callbacks = 0;
    station.setPingReceivedCallback([&]() {
        ++pong_callbacks;
    });

    station.onPingReceived();
    station.tick(20);
    CHECK(pong_callbacks == 0, "first PING should still be pending");

    station.onPingReceived();
    station.tick(kDelayMs - 1);
    CHECK(pong_callbacks == 0, "second PING should restart the full delay");

    station.tick(1);
    CHECK(pong_callbacks == 1, "restarted delay should fire exactly once");
}

void test_zero_pong_tx_delay_fires_immediately() {
    ProtocolEngine station = makeStation(0);

    int pong_callbacks = 0;
    station.setPingReceivedCallback([&]() {
        ++pong_callbacks;
    });

    station.onPingReceived();
    CHECK(pong_callbacks == 1, "zero delay should fire callback immediately");

    station.tick(1);
    CHECK(pong_callbacks == 1, "zero delay should not leave pending callback");
}

bool runCase(const std::string& name) {
    if (name == "deferred") {
        test_pong_tx_callback_is_deferred();
    } else if (name == "cancel_on_connect") {
        test_pong_tx_callback_cancels_when_connecting();
    } else if (name == "reping_restarts") {
        test_reping_restarts_pong_tx_delay();
    } else if (name == "zero_delay") {
        test_zero_pong_tx_delay_fires_immediately();
    } else {
        std::cout << "Unknown test case: " << name << "\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        if (!runCase(argv[1])) {
            return 1;
        }
    } else if (argc == 1) {
        test_pong_tx_callback_is_deferred();
        test_pong_tx_callback_cancels_when_connecting();
        test_reping_restarts_pong_tx_delay();
        test_zero_pong_tx_delay_fires_immediately();
    } else {
        std::cout << "Usage: " << argv[0] << " [deferred|cancel_on_connect|reping_restarts|zero_delay]\n";
        return 1;
    }

    if (tests_failed != 0) {
        std::cout << "ConnectionPongDelay: " << tests_failed << "/" << tests_run
                  << " failed\n";
        return 1;
    }

    std::cout << "ConnectionPongDelay: " << tests_run << "/" << tests_run
              << " passed\n";
    return 0;
}
