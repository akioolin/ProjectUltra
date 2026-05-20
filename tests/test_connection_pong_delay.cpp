#include "protocol/protocol_engine.hpp"

#include <iostream>

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

ProtocolEngine makeStation() {
    return ProtocolEngine(ConnectionConfig{});
}

void test_pong_tx_callback_fires_immediately() {
    ProtocolEngine station = makeStation();

    int pong_callbacks = 0;
    station.setPingReceivedCallback([&]() {
        ++pong_callbacks;
    });

    station.onPingReceived();
    CHECK(pong_callbacks == 1,
          "PONG callback should fire immediately; AudioPort carrier sense gates TX");

    station.tick(1000);
    CHECK(pong_callbacks == 1, "PONG callback should not leave a pending timer");
}

void test_reping_fires_each_callback() {
    ProtocolEngine station = makeStation();

    int pong_callbacks = 0;
    station.setPingReceivedCallback([&]() {
        ++pong_callbacks;
    });

    station.onPingReceived();
    station.onPingReceived();
    CHECK(pong_callbacks == 2,
          "each inbound PING should request a PONG; AudioPort serializes the actual TX");
}

void test_connect_does_not_cancel_stale_pong_timer() {
    ProtocolEngine station = makeStation();
    station.setLocalCallsign("W1ABC");
    station.setPingTxCallback([]() {});

    int pong_callbacks = 0;
    station.setPingReceivedCallback([&]() {
        ++pong_callbacks;
    });

    station.onPingReceived();
    CHECK(pong_callbacks == 1, "inbound PING should synchronously request PONG");

    CHECK(station.connect("K2DEF"), "connect should leave DISCONNECTED");
    CHECK(station.getState() == ConnectionState::PROBING, "station should be probing after connect");

    station.tick(1000);
    CHECK(pong_callbacks == 1, "connect should not reveal a stale deferred PONG timer");
}

} // namespace

int main() {
    test_pong_tx_callback_fires_immediately();
    test_reping_fires_each_callback();
    test_connect_does_not_cancel_stale_pong_timer();

    if (tests_failed != 0) {
        std::cout << "ConnectionPongDelay: " << tests_failed << "/" << tests_run
                  << " failed\n";
        return 1;
    }

    std::cout << "ConnectionPongDelay: " << tests_run << "/" << tests_run
              << " passed\n";
    return 0;
}
