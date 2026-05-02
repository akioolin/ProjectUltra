#include "tnc/tnc_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using ultra::tnc::ModemAdapter;
using ultra::tnc::State;
using ultra::tnc::TNCServer;
using ultra::tnc::TNCServerConfig;

namespace {

int sendFlags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Predicate>
void waitUntil(Predicate predicate, int timeout_ms, const std::string& message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error(message);
}

struct FakeModemAdapter : ModemAdapter {
    struct ConnectCall {
        std::string src;
        std::string dst;
    };

    mutable std::mutex mutex;
    std::vector<std::vector<std::string>> set_mycall_calls;
    std::vector<int> set_bandwidth_calls;
    std::vector<bool> set_listen_calls;
    std::vector<ConnectCall> start_connect_calls;
    int disconnect_calls = 0;
    int abort_calls = 0;
    std::vector<std::vector<uint8_t>> send_binary_calls;
    int backlog_bytes = 0;
    int snr_db = 0;
    int bitrate_bps = 0;
    State state = State::IDLE;

    void setMyCall(const std::vector<std::string>& calls) override {
        std::lock_guard<std::mutex> lock(mutex);
        set_mycall_calls.push_back(calls);
    }

    void setBandwidth(int hz) override {
        std::lock_guard<std::mutex> lock(mutex);
        set_bandwidth_calls.push_back(hz);
    }

    void setListen(bool on) override {
        std::lock_guard<std::mutex> lock(mutex);
        set_listen_calls.push_back(on);
    }

    void startConnect(const std::string& src, const std::string& dst) override {
        std::lock_guard<std::mutex> lock(mutex);
        start_connect_calls.push_back({src, dst});
    }

    void disconnect() override {
        std::lock_guard<std::mutex> lock(mutex);
        ++disconnect_calls;
    }

    void abort() override {
        std::lock_guard<std::mutex> lock(mutex);
        ++abort_calls;
    }

    void sendBinary(const std::vector<uint8_t>& bytes) override {
        std::lock_guard<std::mutex> lock(mutex);
        send_binary_calls.push_back(bytes);
    }

    int getTxBackloggBytes() const override {
        std::lock_guard<std::mutex> lock(mutex);
        return backlog_bytes;
    }

    int getCurrentSNR_db() const override {
        std::lock_guard<std::mutex> lock(mutex);
        return snr_db;
    }

    int getCurrentBitrate_bps() const override {
        std::lock_guard<std::mutex> lock(mutex);
        return bitrate_bps;
    }

    State getState() const override {
        std::lock_guard<std::mutex> lock(mutex);
        return state;
    }

    size_t sendBinaryCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return send_binary_calls.size();
    }

    std::vector<uint8_t> lastBinary() const {
        std::lock_guard<std::mutex> lock(mutex);
        if (send_binary_calls.empty()) {
            return {};
        }
        return send_binary_calls.back();
    }

    size_t connectCallCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return start_connect_calls.size();
    }

    int disconnectCallCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return disconnect_calls;
    }

    size_t myCallCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return set_mycall_calls.size();
    }
};

class TestClient {
public:
    TestClient() = default;
    explicit TestClient(uint16_t port) {
        connectTo(port);
    }

    ~TestClient() {
        close();
    }

    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    TestClient(TestClient&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)),
          rx_(std::move(other.rx_)) {}

    TestClient& operator=(TestClient&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
            rx_ = std::move(other.rx_);
        }
        return *this;
    }

    void connectTo(uint16_t port) {
        close();
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        expect(fd_ >= 0, "socket() failed");

        const int one = 1;
        (void)setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        (void)setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        expect(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1, "inet_pton failed");
        expect(::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
               "connect() failed");
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        rx_.clear();
    }

    void writeString(const std::string& text) {
        writeBytes(std::vector<uint8_t>(text.begin(), text.end()));
    }

    void writeBytes(const std::vector<uint8_t>& bytes) {
        size_t sent = 0;
        while (sent < bytes.size()) {
            const ssize_t n = send(fd_, bytes.data() + sent, bytes.size() - sent, sendFlags());
            if (n > 0) {
                sent += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            throw std::runtime_error("send() failed");
        }
    }

    bool waitReadable(int timeout_ms) {
        pollfd pfd {fd_, static_cast<short>(POLLIN | POLLERR | POLLHUP | POLLNVAL), 0};
        const int rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0 && errno == EINTR) {
            return false;
        }
        return rc > 0 && pfd.revents != 0;
    }

    std::string readUntil(char delimiter, int timeout_ms = 1000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (true) {
            const size_t pos = rx_.find(delimiter);
            if (pos != std::string::npos) {
                std::string out = rx_.substr(0, pos + 1);
                rx_.erase(0, pos + 1);
                return out;
            }

            const int remaining = remainingMs(deadline);
            expect(remaining > 0, "timed out waiting for delimiter");
            expect(waitReadable(remaining), "timed out waiting for readable socket");
            readSome();
        }
    }

    std::vector<uint8_t> readBytes(size_t count, int timeout_ms = 1000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (rx_.size() < count) {
            const int remaining = remainingMs(deadline);
            expect(remaining > 0, "timed out waiting for bytes");
            expect(waitReadable(remaining), "timed out waiting for readable data socket");
            readSome();
        }

        std::vector<uint8_t> out(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(count));
        rx_.erase(0, count);
        return out;
    }

    bool waitForClosed(int timeout_ms = 1000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        char byte = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd pfd {fd_, static_cast<short>(POLLIN | POLLERR | POLLHUP | POLLNVAL), 0};
            const int rc = poll(&pfd, 1, 20);
            if (rc <= 0) {
                continue;
            }
            const ssize_t n = recv(fd_, &byte, sizeof(byte), 0);
            if (n == 0) {
                return true;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0) {
                return true;
            }
        }
        return false;
    }

private:
    static int remainingMs(std::chrono::steady_clock::time_point deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return 0;
        }
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    }

    void readSome() {
        char buffer[512];
        const ssize_t n = recv(fd_, buffer, sizeof(buffer), 0);
        if (n > 0) {
            rx_.append(buffer, static_cast<size_t>(n));
            return;
        }
        if (n == 0) {
            throw std::runtime_error("socket closed");
        }
        if (errno == EINTR) {
            return;
        }
        throw std::runtime_error("recv() failed");
    }

    int fd_ = -1;
    std::string rx_;
};

struct ServerHarness {
    FakeModemAdapter modem;
    TNCServer server;

    explicit ServerHarness(TNCServerConfig config = makeConfig())
        : modem(),
          server(modem, std::move(config)) {
        expect(server.start(), "server failed to start");
        expect(server.getCmdPort() != 0, "cmd port was not assigned");
        expect(server.getDataPort() != 0, "data port was not assigned");
    }

    ~ServerHarness() {
        server.stop();
    }

    static TNCServerConfig makeConfig() {
        TNCServerConfig config;
        config.cmd_port = 0;
        config.data_port = 0;
        return config;
    }
};

struct Runner {
    int tests_run = 0;
    int tests_failed = 0;

    void group(const std::string& name) {
        std::cout << "\n[" << name << "]\n";
    }

    void run(const std::string& name, const std::function<void()>& body) {
        ++tests_run;
        try {
            body();
            std::cout << "  PASS " << name << "\n";
        } catch (const std::exception& ex) {
            ++tests_failed;
            std::cout << "  FAIL " << name << ": " << ex.what() << "\n";
        }
    }
};

std::string command(TestClient& client, const std::string& line) {
    client.writeString(line + "\r");
    return client.readUntil('\r');
}

bool localhostTcpBindAvailable() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    const int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        ::close(fd);
        return false;
    }

    const bool available =
        bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(fd, 1) == 0;
    ::close(fd);
    return available;
}

void expectLine(TestClient& client, const std::string& expected) {
    const std::string actual = client.readUntil('\r');
    expect(actual == expected, "line mismatch: got [" + actual + "] expected [" + expected + "]");
}

void enterConnecting(ServerHarness& harness, TestClient& cmd) {
    expect(command(cmd, "MYCALL VK2XYZ") == "OK\r", "MYCALL failed");
    expect(command(cmd, "CONNECT VK2XYZ VK2ABC") == "OK\r", "CONNECT failed");
    waitUntil([&] { return harness.modem.connectCallCount() == 1; }, 500, "startConnect not called");
}

void enterConnected(ServerHarness& harness, TestClient& cmd) {
    enterConnecting(harness, cmd);
    harness.server.postModemConnected("VK2XYZ", "VK2ABC", 2300);
    expectLine(cmd, "CONNECTED VK2XYZ VK2ABC 2300\r");
}

} // namespace

int main() {
    signal(SIGPIPE, SIG_IGN);
    if (!localhostTcpBindAvailable()) {
        std::cout << "TNCServer integration tests skipped: localhost TCP bind is not permitted in this environment\n";
        return 0;
    }

    Runner runner;

    runner.group("Listeners");
    runner.run("binds cmd and data listeners on ephemeral adjacent ports", [] {
        ServerHarness harness;
        const uint16_t cmd_port = harness.server.getCmdPort();
        const uint16_t data_port = harness.server.getDataPort();
        expect(cmd_port != 0, "cmd port should be nonzero");
        expect(data_port == static_cast<uint16_t>(cmd_port + 1), "data port should be cmd port + 1");
        expect(harness.server.isRunning(), "server should be running");
    });

    runner.run("accepts command client and answers VERSION", [] {
        ServerHarness harness;
        TestClient cmd(harness.server.getCmdPort());
        expect(command(cmd, "VERSION") == "VARA version 4.9.0 registered\r", "VERSION response mismatch");
    });

    runner.run("accepts data client without unsolicited output", [] {
        ServerHarness harness;
        TestClient data(harness.server.getDataPort());
        expect(!data.waitReadable(40), "data client should not receive unsolicited bytes");
    });

    runner.group("Control Input");
    runner.run("line parser handles split command writes", [] {
        ServerHarness harness;
        TestClient cmd(harness.server.getCmdPort());
        cmd.writeString("VER");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        cmd.writeString("SION\r");
        expectLine(cmd, "VARA version 4.9.0 registered\r");
    });

    runner.run("MYCALL reaches modem adapter through reactor", [] {
        ServerHarness harness;
        TestClient cmd(harness.server.getCmdPort());
        expect(command(cmd, "MYCALL VK2XYZ") == "OK\r", "MYCALL response mismatch");
        waitUntil([&] { return harness.modem.myCallCount() == 1; }, 500, "setMyCall not called");
    });

    runner.group("Eviction");
    runner.run("new command client evicts old command client", [] {
        ServerHarness harness;
        TestClient first(harness.server.getCmdPort());
        expect(command(first, "MYCALL VK2XYZ") == "OK\r", "initial MYCALL failed");
        TestClient second(harness.server.getCmdPort());
        expect(first.waitForClosed(), "first command client was not closed");
        expect(command(second, "VERSION") == "VARA version 4.9.0 registered\r", "second client VERSION failed");
    });

    runner.run("command eviction resets session state", [] {
        ServerHarness harness;
        TestClient first(harness.server.getCmdPort());
        expect(command(first, "MYCALL VK2XYZ") == "OK\r", "initial MYCALL failed");
        TestClient second(harness.server.getCmdPort());
        expect(first.waitForClosed(), "first command client was not closed");
        expect(command(second, "LISTEN ON") == "WRONG\r", "MYCALL should have been cleared");
    });

    runner.run("new data client evicts old data client", [] {
        ServerHarness harness;
        TestClient first(harness.server.getDataPort());
        TestClient second(harness.server.getDataPort());
        expect(first.waitForClosed(), "first data client was not closed");
        expect(!second.waitReadable(40), "second data client should stay open and quiet");
    });

    runner.run("new command client closes paired data client", [] {
        ServerHarness harness;
        TestClient first_cmd(harness.server.getCmdPort());
        TestClient first_data(harness.server.getDataPort());
        expect(command(first_cmd, "VERSION") == "VARA version 4.9.0 registered\r", "first VERSION failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        TestClient second_cmd(harness.server.getCmdPort());
        expect(first_cmd.waitForClosed(), "first command client was not closed");
        expect(first_data.waitForClosed(), "paired data client was not closed");
        expect(command(second_cmd, "VERSION") == "VARA version 4.9.0 registered\r", "second VERSION failed");
    });

    runner.group("Timers And Posts");
    runner.run("IAMALIVE interval override emits quickly", [] {
        TNCServerConfig config = ServerHarness::makeConfig();
        config.iamalive_interval_ms = 50;
        ServerHarness harness(config);
        TestClient cmd(harness.server.getCmdPort());
        expectLine(cmd, "IAMALIVE\r");
    });

    runner.run("cross-thread modem connected post emits on command socket", [] {
        ServerHarness harness;
        TestClient cmd(harness.server.getCmdPort());
        enterConnecting(harness, cmd);
        std::thread poster([&] {
            harness.server.postModemConnected("VK2XYZ", "VK2ABC", 2300);
        });
        poster.join();
        expectLine(cmd, "CONNECTED VK2XYZ VK2ABC 2300\r");
    });

    runner.run("PTT and bitrate modem posts are marshalled", [] {
        ServerHarness harness;
        TestClient cmd(harness.server.getCmdPort());
        expect(command(cmd, "VERSION") == "VARA version 4.9.0 registered\r", "VERSION failed");
        harness.server.postModemPTT(true);
        harness.server.postModemBitrate(1200);
        expectLine(cmd, "PTT ON\r");
        expectLine(cmd, "BITRATE (0) 1200 BPS\r");
    });

    runner.run("buffer rate override flushes pending buffer update", [] {
        TNCServerConfig config = ServerHarness::makeConfig();
        config.buffer_rate_limit_ms = 40;
        ServerHarness harness(config);
        TestClient cmd(harness.server.getCmdPort());
        expect(command(cmd, "VERSION") == "VARA version 4.9.0 registered\r", "VERSION failed");
        harness.server.postModemBufferLevel(10);
        expectLine(cmd, "BUFFER 10\r");
        harness.server.postModemBufferLevel(20);
        expectLine(cmd, "BUFFER 20\r");
    });

    runner.group("Data Flow");
    runner.run("data socket bytes reach modem only while connected", [] {
        ServerHarness harness;
        TestClient cmd(harness.server.getCmdPort());
        TestClient data(harness.server.getDataPort());
        data.writeBytes({1, 2, 3});
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        expect(harness.modem.sendBinaryCount() == 0, "READY/IDLE data should be discarded");
        enterConnected(harness, cmd);
        data.writeBytes({4, 5, 6});
        waitUntil([&] { return harness.modem.sendBinaryCount() == 1; }, 500, "sendBinary not called");
        expect(harness.modem.lastBinary() == std::vector<uint8_t>({4, 5, 6}), "sendBinary payload mismatch");
    });

    runner.run("modem data post reaches data socket while connected", [] {
        ServerHarness harness;
        TestClient cmd(harness.server.getCmdPort());
        TestClient data(harness.server.getDataPort());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        enterConnected(harness, cmd);
        harness.server.postModemDataReceived({9, 8, 7, 0});
        expect(data.readBytes(4) == std::vector<uint8_t>({9, 8, 7, 0}), "data socket payload mismatch");
    });

    runner.run("DISCONNECT command and modem disconnect event complete session", [] {
        ServerHarness harness;
        TestClient cmd(harness.server.getCmdPort());
        enterConnected(harness, cmd);
        expect(command(cmd, "DISCONNECT") == "OK\r", "DISCONNECT command failed");
        waitUntil([&] { return harness.modem.disconnectCallCount() == 1; }, 500, "modem disconnect not called");
        harness.server.postModemDisconnected();
        expectLine(cmd, "DISCONNECTED\r");
    });

    runner.group("Lifecycle");
    runner.run("stop closes active clients", [] {
        FakeModemAdapter modem;
        TNCServerConfig config = ServerHarness::makeConfig();
        TNCServer server(modem, config);
        expect(server.start(), "server failed to start");
        TestClient cmd(server.getCmdPort());
        TestClient data(server.getDataPort());
        expect(command(cmd, "VERSION") == "VARA version 4.9.0 registered\r", "VERSION failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        server.stop();
        expect(cmd.waitForClosed(), "command client was not closed on stop");
        expect(data.waitForClosed(), "data client was not closed on stop");
        expect(!server.isRunning(), "server should not be running after stop");
    });

    runner.run("server can stop and restart", [] {
        FakeModemAdapter modem;
        TNCServerConfig config = ServerHarness::makeConfig();
        TNCServer server(modem, config);
        expect(server.start(), "first start failed");
        {
            TestClient cmd(server.getCmdPort());
            expect(command(cmd, "VERSION") == "VARA version 4.9.0 registered\r", "first VERSION failed");
            server.stop();
            expect(cmd.waitForClosed(), "client was not closed after first stop");
        }
        expect(server.start(), "second start failed");
        {
            TestClient cmd(server.getCmdPort());
            expect(command(cmd, "VERSION") == "VARA version 4.9.0 registered\r", "second VERSION failed");
        }
        server.stop();
    });

    std::cout << "\nTNCServer tests run: " << runner.tests_run
              << ", failures: " << runner.tests_failed << "\n";
    return runner.tests_failed == 0 ? 0 : 1;
}
