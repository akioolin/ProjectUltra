#include "ptt/cat_ptt_driver.hpp"
#include "ptt/serial_ptt_driver.hpp"
#include "tnc/socket_compat.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ultra::ptt;
using ultra::tnc::socket_t;

namespace {

int passed = 0;
int failed = 0;
int skipped = 0;

#define CHECK(cond, msg)                                      \
    do {                                                      \
        if (!(cond)) {                                        \
            std::cout << "FAIL: " << msg << "\n";             \
            ++failed;                                         \
            return;                                           \
        }                                                     \
    } while (0)

void pass(const char* name) {
    ++passed;
    std::cout << "PASS: " << name << "\n";
}

void skip(const char* name, const std::string& reason) {
    ++skipped;
    std::cout << "SKIP: " << name << " (" << reason << ")\n";
}

struct FakeSerialBackend final : public ISerialPttBackend {
    struct LineSet {
        ultra::gui::SerialPttLine line = ultra::gui::SerialPttLine::RTS;
        bool asserted = false;
    };

    bool open_result = true;
    bool set_line_result = true;
    bool open_state = false;
    std::string opened_port;
    int opened_baud = 0;
    std::vector<LineSet> line_sets;

    bool open(const std::string& port_name, int baud_rate) override {
        if (!open_result) {
            return false;
        }
        open_state = true;
        opened_port = port_name;
        opened_baud = baud_rate;
        return true;
    }

    void close() override {
        open_state = false;
    }

    bool isOpen() const override {
        return open_state;
    }

    bool matches(const std::string& port_name, int baud_rate) const override {
        return open_state && opened_port == port_name && opened_baud == baud_rate;
    }

    bool setLine(ultra::gui::SerialPttLine line, bool asserted) override {
        if (!set_line_result) {
            return false;
        }
        line_sets.push_back({line, asserted});
        return true;
    }
};

void test_serial_driver_preserves_rts_active_high_mapping() {
    auto backend = std::make_unique<FakeSerialBackend>();
    FakeSerialBackend* fake = backend.get();

    PttConfig config;
    config.mode = PttMode::Serial;
    config.serial_port = "COM3";
    config.serial_baud = 9600;
    config.serial_line = SerialLine::RTS;
    config.serial_inactive_high = false;

    SerialPttDriver driver(config, std::move(backend));
    IPttDriver& iface = driver;

    CHECK(iface.open(), "serial driver open should succeed");
    CHECK(iface.setKey(PttKey::On), "serial driver assert should succeed");
    CHECK(iface.setKey(PttKey::Off), "serial driver release should succeed");
    CHECK(fake->opened_port == "COM3", "serial port should pass through unchanged");
    CHECK(fake->opened_baud == 9600, "serial baud should pass through unchanged");
    CHECK(fake->line_sets.size() == 3, "open/on/off should set the line three times");
    CHECK(fake->line_sets[0].line == ultra::gui::SerialPttLine::RTS &&
          !fake->line_sets[0].asserted, "open should initialize RTS inactive-low");
    CHECK(fake->line_sets[1].line == ultra::gui::SerialPttLine::RTS &&
          fake->line_sets[1].asserted, "PTT on should assert RTS high");
    CHECK(fake->line_sets[2].line == ultra::gui::SerialPttLine::RTS &&
          !fake->line_sets[2].asserted, "PTT off should release RTS low");
    pass("SerialPttDriver: RTS active-high wire behavior preserved through IPttDriver");
}

void test_serial_driver_preserves_dtr_inverted_mapping() {
    auto backend = std::make_unique<FakeSerialBackend>();
    FakeSerialBackend* fake = backend.get();

    PttConfig config;
    config.mode = PttMode::Serial;
    config.serial_port = "/dev/ttyUSB0";
    config.serial_baud = 4800;
    config.serial_line = SerialLine::DTR;
    config.serial_inactive_high = true;

    SerialPttDriver driver(config, std::move(backend));
    IPttDriver& iface = driver;

    CHECK(iface.open(), "serial driver open should succeed");
    CHECK(iface.setKey(PttKey::On), "serial driver assert should succeed");
    CHECK(iface.setKey(PttKey::Off), "serial driver release should succeed");
    CHECK(fake->opened_port == "/dev/ttyUSB0", "POSIX serial path should pass through unchanged");
    CHECK(fake->opened_baud == 4800, "serial baud should pass through unchanged");
    CHECK(fake->line_sets.size() == 3, "open/on/off should set the line three times");
    CHECK(fake->line_sets[0].line == ultra::gui::SerialPttLine::DTR &&
          fake->line_sets[0].asserted, "open should initialize DTR inactive-high");
    CHECK(fake->line_sets[1].line == ultra::gui::SerialPttLine::DTR &&
          !fake->line_sets[1].asserted, "inverted PTT on should drive DTR low");
    CHECK(fake->line_sets[2].line == ultra::gui::SerialPttLine::DTR &&
          fake->line_sets[2].asserted, "inverted PTT off should drive DTR high");
    pass("SerialPttDriver: DTR inverted wire behavior preserved through IPttDriver");
}

class MockRigctldServer {
public:
    MockRigctldServer() = default;
    ~MockRigctldServer() { stop(); }

    bool start() {
        if (!winsock_.ok()) {
            error_ = "WinSock init failed";
            return false;
        }
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ultra::tnc::isInvalidSocket(listener_)) {
            error_ = "socket failed";
            return false;
        }

        int reuse = 1;
        (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
                           reinterpret_cast<const char*>(&reuse),
                           static_cast<ultra::tnc::socket_len_t>(sizeof(reuse)));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
            error_ = "inet_pton failed";
            ultra::tnc::closeSocket(listener_);
            listener_ = ultra::tnc::kInvalidSocket;
            return false;
        }
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            error_ = std::string("bind failed: ") + std::strerror(errno);
            ultra::tnc::closeSocket(listener_);
            listener_ = ultra::tnc::kInvalidSocket;
            return false;
        }
        if (::listen(listener_, 1) != 0) {
            error_ = std::string("listen failed: ") + std::strerror(errno);
            ultra::tnc::closeSocket(listener_);
            listener_ = ultra::tnc::kInvalidSocket;
            return false;
        }

        ultra::tnc::socket_len_t len = sizeof(addr);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            error_ = "getsockname failed";
            ultra::tnc::closeSocket(listener_);
            listener_ = ultra::tnc::kInvalidSocket;
            return false;
        }
        port_ = ntohs(addr.sin_port);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&MockRigctldServer::loop, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel) &&
            !thread_.joinable()) {
            return;
        }
        if (!ultra::tnc::isInvalidSocket(listener_)) {
            ultra::tnc::shutdownSocket(listener_);
            ultra::tnc::closeSocket(listener_);
            listener_ = ultra::tnc::kInvalidSocket;
        }
        if (!ultra::tnc::isInvalidSocket(client_)) {
            ultra::tnc::shutdownSocket(client_);
            ultra::tnc::closeSocket(client_);
            client_ = ultra::tnc::kInvalidSocket;
        }
        if (port_ != 0) {
            wakeAccept();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    uint16_t port() const { return port_; }
    const std::string& error() const { return error_; }

    bool waitForCommandCount(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return commands_.size() >= count;
        });
    }

    std::vector<std::string> commands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }

private:
    void loop() {
        sockaddr_in peer {};
        ultra::tnc::socket_len_t peer_len = sizeof(peer);
        client_ = ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (ultra::tnc::isInvalidSocket(client_)) {
            return;
        }

        std::string pending;
        char buffer[128];
        while (running_.load(std::memory_order_acquire)) {
            const auto got = ultra::tnc::recvSocket(client_, buffer, sizeof(buffer), 0);
            if (got <= 0) {
                break;
            }
            pending.append(buffer, buffer + got);
            size_t newline = std::string::npos;
            while ((newline = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, newline);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                pending.erase(0, newline + 1);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    commands_.push_back(line);
                }
                cv_.notify_all();

                std::string response = "RPRT -1\n";
                if (line == "T 0" || line == "T 1") {
                    response = "RPRT 0\n";
                } else if (line == "t") {
                    response = "0\n";
                }
                (void)ultra::tnc::sendSocket(client_, response.data(), response.size(), 0);
            }
        }
    }

    void wakeAccept() const {
        socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ultra::tnc::isInvalidSocket(fd)) {
            return;
        }
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        (void)inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        (void)::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ultra::tnc::closeSocket(fd);
    }

    ultra::tnc::WinsockInit winsock_;
    socket_t listener_ = ultra::tnc::kInvalidSocket;
    socket_t client_ = ultra::tnc::kInvalidSocket;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::string> commands_;
    std::string error_;
};

void test_cat_driver_mock_server() {
    const char* name = "CatPttDriver: mock rigctld T 1/T 0 protocol and non-blocking enqueue";
    MockRigctldServer server;
    if (!server.start()) {
        if (server.error().find("Operation not permitted") != std::string::npos) {
            skip(name, "localhost TCP bind is not permitted in this sandbox");
            return;
        }
        CHECK(false, std::string("mock rigctld start failed: ") + server.error());
    }

    PttConfig config;
    config.mode = PttMode::Cat;
    config.cat_host = "127.0.0.1";
    config.cat_port = server.port();

    CatPttDriver driver(config);
    CHECK(driver.open(), std::string("CAT driver open failed: ") + driver.lastError());
    CHECK(server.waitForCommandCount(1, std::chrono::seconds(2)),
          "CAT open should send initial T 0");

    const auto start = std::chrono::steady_clock::now();
    CHECK(driver.setKey(PttKey::On), "CAT setKey(On) enqueue failed");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::milliseconds(100), "CAT setKey should be non-blocking");
    CHECK(server.waitForCommandCount(2, std::chrono::seconds(2)),
          "mock rigctld did not receive T 1");

    CHECK(driver.setKey(PttKey::Off), "CAT setKey(Off) enqueue failed");
    CHECK(server.waitForCommandCount(3, std::chrono::seconds(2)),
          "mock rigctld did not receive T 0");

    const std::vector<std::string> commands = server.commands();
    CHECK(commands.size() >= 3, "expected at least three CAT commands");
    CHECK(commands[0] == "T 0", "CAT open should initialize PTT off");
    CHECK(commands[1] == "T 1", "CAT setKey(On) should send T 1");
    CHECK(commands[2] == "T 0", "CAT setKey(Off) should send T 0");

    driver.close();
    server.stop();
    pass(name);
}

#ifndef _WIN32

std::string findExecutable(const char* name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) {
        return {};
    }
    std::stringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        std::string path = dir + "/" + name;
        if (::access(path.c_str(), X_OK) == 0) {
            return path;
        }
    }
    return {};
}

uint16_t reserveTcpPort() {
    ultra::tnc::WinsockInit winsock;
    if (!winsock.ok()) {
        return 0;
    }
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ultra::tnc::isInvalidSocket(fd)) {
        return 0;
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        ultra::tnc::closeSocket(fd);
        return 0;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ultra::tnc::closeSocket(fd);
        return 0;
    }
    ultra::tnc::socket_len_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ultra::tnc::closeSocket(fd);
        return 0;
    }
    uint16_t port = ntohs(addr.sin_port);
    ultra::tnc::closeSocket(fd);
    return port;
}

class RigctldProcess {
public:
    ~RigctldProcess() { stop(); }

    bool start(uint16_t port) {
        int pipefd[2] = {-1, -1};
        if (::pipe(pipefd) != 0) {
            error_ = "pipe failed";
            return false;
        }

        pid_ = ::fork();
        if (pid_ < 0) {
            error_ = "fork failed";
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            return false;
        }

        if (pid_ == 0) {
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDERR_FILENO);
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDOUT_FILENO);
            }
            const std::string port_text = std::to_string(port);
            ::execlp("rigctld", "rigctld", "-m", "1", "-t", port_text.c_str(),
                     "-vvvvv", static_cast<char*>(nullptr));
            _exit(127);
        }

        ::close(pipefd[1]);
        stderr_fd_ = pipefd[0];
        int flags = ::fcntl(stderr_fd_, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(stderr_fd_, F_SETFL, flags | O_NONBLOCK);
        }
        return true;
    }

    void stop() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            ::waitpid(pid_, &status, 0);
            pid_ = -1;
        }
        if (stderr_fd_ >= 0) {
            ::close(stderr_fd_);
            stderr_fd_ = -1;
        }
    }

    void clearLog() {
        readAvailable();
        log_.clear();
    }

    bool waitForLog(const std::string& pattern, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            readAvailable();
            if (log_.find(pattern) != std::string::npos) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        readAvailable();
        return log_.find(pattern) != std::string::npos;
    }

    const std::string& error() const { return error_; }
    const std::string& log() const { return log_; }

private:
    void readAvailable() {
        if (stderr_fd_ < 0) {
            return;
        }
        char buffer[512];
        for (;;) {
            const ssize_t got = ::read(stderr_fd_, buffer, sizeof(buffer));
            if (got > 0) {
                log_.append(buffer, buffer + got);
                continue;
            }
            break;
        }
    }

    pid_t pid_ = -1;
    int stderr_fd_ = -1;
    std::string log_;
    std::string error_;
};

void test_cat_driver_hamlib_dummy_if_available() {
    const char* name = "CatPttDriver: optional hamlib dummy rigctld integration";
    if (findExecutable("rigctld").empty()) {
        skip(name, "rigctld not installed");
        return;
    }
    const uint16_t port = reserveTcpPort();
    if (port == 0) {
        skip(name, "could not reserve a local TCP port");
        return;
    }

    RigctldProcess rig;
    if (!rig.start(port)) {
        skip(name, rig.error());
        return;
    }

    PttConfig config;
    config.mode = PttMode::Cat;
    config.cat_host = "127.0.0.1";
    config.cat_port = port;
    CatPttDriver driver(config);

    bool opened = false;
    for (int i = 0; i < 30 && !opened; ++i) {
        opened = driver.open();
        if (!opened) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    CHECK(opened, std::string("CAT driver could not connect to rigctld dummy: ") + driver.lastError());
    rig.clearLog();

    // Hamlib's verbose log format varies across versions. 4.7.x emits
    // "set_ptt ptt=1" (space-separated) where older versions emit
    // "set_ptt: 1". Match either form so the test isn't pinned to a
    // single Hamlib release.
    CHECK(driver.setKey(PttKey::On), "CAT setKey(On) enqueue failed");
    CHECK(rig.waitForLog("set_ptt ptt=1", std::chrono::seconds(2)) ||
              rig.waitForLog("set_ptt: 1", std::chrono::seconds(0)),
          std::string("rigctld stderr did not show set_ptt enabled; log was:\n") + rig.log());
    rig.clearLog();

    CHECK(driver.setKey(PttKey::Off), "CAT setKey(Off) enqueue failed");
    CHECK(rig.waitForLog("set_ptt ptt=0", std::chrono::seconds(2)) ||
              rig.waitForLog("set_ptt: 0", std::chrono::seconds(0)),
          std::string("rigctld stderr did not show set_ptt disabled; log was:\n") + rig.log());

    driver.close();
    rig.stop();
    pass(name);
}

#else

void test_cat_driver_hamlib_dummy_if_available() {
    skip("CatPttDriver: optional hamlib dummy rigctld integration",
         "POSIX process fixture is not available on Windows");
}

#endif

} // namespace

int main() {
    test_serial_driver_preserves_rts_active_high_mapping();
    test_serial_driver_preserves_dtr_inverted_mapping();
    test_cat_driver_mock_server();
    test_cat_driver_hamlib_dummy_if_available();

    std::cout << "\nPTT driver tests: " << passed << " passed, "
              << failed << " failed, " << skipped << " skipped\n";
    return failed == 0 ? 0 : 1;
}
