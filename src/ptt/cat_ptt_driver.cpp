#include "ptt/cat_ptt_driver.hpp"

#include "ultra/logging.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <netdb.h>
#include <sys/types.h>
#endif

#ifndef POLLHUP
#define POLLHUP 0
#endif

namespace ultra::ptt {
namespace {

constexpr auto kCommandTimeout = std::chrono::milliseconds(1500);
constexpr auto kTestCommandWait = std::chrono::milliseconds(2200);
constexpr size_t kMaxQueueDepth = 4;

std::string trimLine(std::string line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    return line;
}

int remainingMs(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return std::max<int64_t>(1, ms) > static_cast<int64_t>(std::numeric_limits<int>::max())
               ? std::numeric_limits<int>::max()
               : static_cast<int>(std::max<int64_t>(1, ms));
}

std::string socketErrorString(int error) {
#ifdef _WIN32
    char* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = FormatMessageA(flags, nullptr, static_cast<DWORD>(error), 0,
                                     reinterpret_cast<LPSTR>(&message), 0, nullptr);
    std::string out = (len > 0 && message) ? std::string(message, len)
                                           : ("WinSock error " + std::to_string(error));
    if (message) {
        LocalFree(message);
    }
    return trimLine(out);
#else
    return std::strerror(error);
#endif
}

int lastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool isConnectInProgress() {
#ifdef _WIN32
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
#else
    return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

int sendNoSignalFlag() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

} // namespace

CatPttDriver::CatPttDriver(PttConfig config)
    : config_(std::move(config)) {}

CatPttDriver::~CatPttDriver() {
    close();
}

bool CatPttDriver::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return true;
    }
    if (!winsock_.ok()) {
        last_error_ = "WinSock initialization failed";
        LOG_ERROR("OPERATOR", "PTT: CAT %s", last_error_.c_str());
        return false;
    }
    if (config_.cat_host.empty()) {
        last_error_ = "rigctld host is empty";
        LOG_ERROR("OPERATOR", "PTT: CAT %s", last_error_.c_str());
        return false;
    }

    ConnectResult connected = connectSocket();
    if (ultra::tnc::isInvalidSocket(connected.socket)) {
        last_error_ = "failed to connect to rigctld at " + config_.cat_host + ":" +
                      std::to_string(config_.cat_port) + ": " + connected.error;
        LOG_ERROR("OPERATOR", "PTT: CAT %s", last_error_.c_str());
        return false;
    }

    socket_ = connected.socket;
    connected_.store(true, std::memory_order_release);
    std::string error;
    if (!sendPttCommand(PttKey::Off, error)) {
        closeCurrentSocket();
        last_error_ = error;
        LOG_ERROR("OPERATOR", "PTT: CAT %s", last_error_.c_str());
        return false;
    }

    running_ = true;
    reconnect_delay_ms_ = 1000;
    last_error_.clear();
    worker_ = std::thread(&CatPttDriver::workerLoop, this);
    LOG_INFO("OPERATOR", "PTT: CAT connected to rigctld %s:%u",
             config_.cat_host.c_str(), static_cast<unsigned>(config_.cat_port));
    return true;
}

void CatPttDriver::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !worker_.joinable()) {
            closeCurrentSocket();
            drainQueued(false);
            return;
        }
        running_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    closeCurrentSocket();
    drainQueued(false);
}

bool CatPttDriver::isOpen() const {
    return connected_.load(std::memory_order_acquire);
}

bool CatPttDriver::setKey(PttKey state) {
    return enqueue(state, {});
}

bool CatPttDriver::testCycle() {
    if (!isOpen() && !open()) {
        return false;
    }

    auto on_completion = std::make_shared<std::promise<bool>>();
    std::future<bool> on_future = on_completion->get_future();
    if (!enqueue(PttKey::On, on_completion)) {
        return false;
    }
    if (on_future.wait_for(kTestCommandWait) != std::future_status::ready ||
        !on_future.get()) {
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto off_completion = std::make_shared<std::promise<bool>>();
    std::future<bool> off_future = off_completion->get_future();
    if (!enqueue(PttKey::Off, off_completion)) {
        return false;
    }
    return off_future.wait_for(kTestCommandWait) == std::future_status::ready &&
           off_future.get();
}

std::string CatPttDriver::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

bool CatPttDriver::enqueue(PttKey state, std::shared_ptr<std::promise<bool>> completion) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            last_error_ = "CAT PTT is not open";
            if (completion) {
                completion->set_value(false);
            }
            return false;
        }

        if (!completion) {
            queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                        [](const Command& cmd) {
                                            return !cmd.completion;
                                        }),
                         queue_.end());
        }

        if (queue_.size() >= kMaxQueueDepth) {
            last_error_ = "CAT PTT command queue is full";
            LOG_ERROR("OPERATOR", "PTT: %s", last_error_.c_str());
            if (completion) {
                completion->set_value(false);
            }
            return false;
        }

        queue_.push_back(Command{state, std::move(completion)});
    }
    cv_.notify_one();
    return true;
}

void CatPttDriver::workerLoop() {
    for (;;) {
        Command cmd;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !running_ || !queue_.empty();
            });
            if (!running_ && queue_.empty()) {
                break;
            }
            cmd = std::move(queue_.front());
            queue_.pop_front();
        }

        bool completed = false;
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!running_) {
                    break;
                }
                if (!cmd.completion && !queue_.empty()) {
                    cmd = std::move(queue_.back());
                    queue_.clear();
                }
            }

            if (ultra::tnc::isInvalidSocket(socket_)) {
                ConnectResult connected = connectSocket();
                if (ultra::tnc::isInvalidSocket(connected.socket)) {
                    setLastError("CAT PTT reconnect failed: " + connected.error);
                    LOG_ERROR("OPERATOR", "PTT: %s", lastError().c_str());
                    if (!cmd.completion && waitBeforeReconnect()) {
                        continue;
                    }
                    break;
                }
                socket_ = connected.socket;
                connected_.store(true, std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    reconnect_delay_ms_ = 1000;
                }
                LOG_INFO("OPERATOR", "PTT: CAT reconnected to rigctld %s:%u",
                         config_.cat_host.c_str(), static_cast<unsigned>(config_.cat_port));
            }

            std::string error;
            if (sendPttCommand(cmd.state, error)) {
                if (cmd.completion) {
                    cmd.completion->set_value(true);
                }
                completed = true;
                break;
            }

            setLastError(error);
            LOG_ERROR("OPERATOR", "PTT: CAT %s", error.c_str());
            closeCurrentSocket();

            if (cmd.completion) {
                cmd.completion->set_value(false);
                completed = true;
                break;
            }

            if (!waitBeforeReconnect()) {
                break;
            }
        }

        if (!completed && cmd.completion) {
            cmd.completion->set_value(false);
        }
    }

    closeCurrentSocket();
    drainQueued(false);
}

bool CatPttDriver::waitBeforeReconnect() {
    int delay_ms = 1000;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        delay_ms = reconnect_delay_ms_;
        reconnect_delay_ms_ = std::min(reconnect_delay_ms_ * 2, 10000);
    }

    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(delay_ms), [this] {
        return !running_ || !queue_.empty();
    });
    return running_;
}

void CatPttDriver::drainQueued(bool ok) {
    for (Command& cmd : queue_) {
        if (cmd.completion) {
            cmd.completion->set_value(ok);
        }
    }
    queue_.clear();
}

CatPttDriver::ConnectResult CatPttDriver::connectSocket() const {
    ConnectResult result;
    const std::string port = std::to_string(config_.cat_port);

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    const int gai = ::getaddrinfo(config_.cat_host.c_str(), port.c_str(), &hints, &addresses);
    if (gai != 0) {
#ifdef _WIN32
        result.error = "getaddrinfo failed: " + std::to_string(gai);
#else
        result.error = std::string("getaddrinfo failed: ") + gai_strerror(gai);
#endif
        return result;
    }

    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> cleanup(addresses, freeaddrinfo);
    for (addrinfo* ai = addresses; ai; ai = ai->ai_next) {
        ultra::tnc::socket_t candidate = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (ultra::tnc::isInvalidSocket(candidate)) {
            result.error = socketErrorString(lastSocketError());
            continue;
        }

#ifdef SO_NOSIGPIPE
        int no_sigpipe = 1;
        (void)::setsockopt(candidate, SOL_SOCKET, SO_NOSIGPIPE,
                           reinterpret_cast<const char*>(&no_sigpipe),
                           static_cast<ultra::tnc::socket_len_t>(sizeof(no_sigpipe)));
#endif

        if (!ultra::tnc::setNonblocking(candidate)) {
            result.error = "failed to set non-blocking mode: " + socketErrorString(lastSocketError());
            ultra::tnc::closeSocket(candidate);
            continue;
        }

        const int rc = ::connect(candidate, ai->ai_addr,
                                 static_cast<ultra::tnc::socket_len_t>(ai->ai_addrlen));
        if (rc == 0) {
            result.socket = candidate;
            result.error.clear();
            return result;
        }

        if (!isConnectInProgress()) {
            result.error = socketErrorString(lastSocketError());
            ultra::tnc::closeSocket(candidate);
            continue;
        }

        const auto deadline = std::chrono::steady_clock::now() + kCommandTimeout;
        pollfd fd {};
        fd.fd = candidate;
        fd.events = POLLOUT;
        std::string wait_error;
        bool writable = false;
        while (std::chrono::steady_clock::now() < deadline) {
            fd.revents = 0;
            const int poll_rc = ultra::tnc::pollSockets(&fd, 1, remainingMs(deadline));
            if (poll_rc > 0) {
                writable = (fd.revents & (POLLOUT | POLLERR)) != 0;
                break;
            }
            if (poll_rc == 0) {
                wait_error = "connect timed out";
                break;
            }
            if (ultra::tnc::isInterruptedSocketError()) {
                continue;
            }
            wait_error = socketErrorString(lastSocketError());
            break;
        }
        if (!writable) {
            result.error = wait_error.empty() ? "connect timed out" : wait_error;
            ultra::tnc::closeSocket(candidate);
            continue;
        }

        int so_error = 0;
        ultra::tnc::socket_len_t so_len = sizeof(so_error);
        if (::getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&so_error), &so_len) != 0 ||
            so_error != 0) {
            result.error = socketErrorString(so_error != 0 ? so_error : lastSocketError());
            ultra::tnc::closeSocket(candidate);
            continue;
        }

        result.socket = candidate;
        result.error.clear();
        return result;
    }

    if (result.error.empty()) {
        result.error = "no usable address";
    }
    return result;
}

bool CatPttDriver::sendPttCommand(PttKey state, std::string& error) {
    const std::string command = (state == PttKey::On) ? "T 1\n" : "T 0\n";
    const auto deadline = std::chrono::steady_clock::now() + kCommandTimeout;
    if (!sendAll(command, deadline, error)) {
        return false;
    }

    std::string line;
    if (!readLine(line, deadline, error)) {
        return false;
    }
    const std::string response = trimLine(line);
    if (response == "RPRT 0") {
        return true;
    }

    error = "rigctld rejected '" + trimLine(command) + "' with '" + response + "'";
    return false;
}

bool CatPttDriver::sendAll(const std::string& command,
                           std::chrono::steady_clock::time_point deadline,
                           std::string& error) {
    size_t offset = 0;
    while (offset < command.size()) {
        const auto sent = ultra::tnc::sendSocket(socket_, command.data() + offset,
                                                 command.size() - offset, sendNoSignalFlag());
        if (sent > 0) {
            offset += static_cast<size_t>(sent);
            continue;
        }
        if (sent == 0) {
            error = "send returned 0";
            return false;
        }
        if (ultra::tnc::isInterruptedSocketError()) {
            continue;
        }
        if (ultra::tnc::isWouldBlockSocketError()) {
            if (!waitForSocket(POLLOUT, deadline, error)) {
                return false;
            }
            continue;
        }
        error = "send failed: " + socketErrorString(lastSocketError());
        return false;
    }
    return true;
}

bool CatPttDriver::readLine(std::string& line,
                            std::chrono::steady_clock::time_point deadline,
                            std::string& error) {
    line.clear();
    while (line.size() < 256) {
        char c = '\0';
        const auto got = ultra::tnc::recvSocket(socket_, &c, 1, 0);
        if (got > 0) {
            line.push_back(c);
            if (c == '\n') {
                return true;
            }
            continue;
        }
        if (got == 0) {
            error = "rigctld closed the connection";
            return false;
        }
        if (ultra::tnc::isInterruptedSocketError()) {
            continue;
        }
        if (ultra::tnc::isWouldBlockSocketError()) {
            if (!waitForSocket(POLLIN, deadline, error)) {
                return false;
            }
            continue;
        }
        error = "recv failed: " + socketErrorString(lastSocketError());
        return false;
    }
    error = "rigctld response exceeded 255 bytes";
    return false;
}

bool CatPttDriver::waitForSocket(short events,
                                 std::chrono::steady_clock::time_point deadline,
                                 std::string& error) const {
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd fd {};
        fd.fd = socket_;
        fd.events = events;
        const int rc = ultra::tnc::pollSockets(&fd, 1, remainingMs(deadline));
        if (rc > 0) {
            if (fd.revents & (POLLERR | POLLHUP)) {
                error = "rigctld socket error";
                return false;
            }
            if (fd.revents & events) {
                return true;
            }
            continue;
        }
        if (rc == 0) {
            error = "rigctld command timed out";
            return false;
        }
        if (ultra::tnc::isInterruptedSocketError()) {
            continue;
        }
        error = "poll failed: " + socketErrorString(lastSocketError());
        return false;
    }
    error = "rigctld command timed out";
    return false;
}

void CatPttDriver::closeCurrentSocket() {
    if (!ultra::tnc::isInvalidSocket(socket_)) {
        ultra::tnc::shutdownSocket(socket_);
        ultra::tnc::closeSocket(socket_);
        socket_ = ultra::tnc::kInvalidSocket;
    }
    connected_.store(false, std::memory_order_release);
}

void CatPttDriver::setLastError(std::string error) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = std::move(error);
}

} // namespace ultra::ptt
