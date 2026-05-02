#include "tnc/tnc_server.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <utility>

namespace ultra::tnc {
namespace {

constexpr int kBacklog = 4;
constexpr int kPollTimeoutMs = 100;
constexpr uint32_t kSessionTickMs = 100;
constexpr uint32_t kDefaultIAmAliveMs = 60000;
constexpr uint32_t kDefaultBufferRateMs = 1000;
constexpr size_t kReadBufferSize = 4096;
constexpr size_t kMaxControlLine = 4096;

int sendFlags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

bool isWouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

void closeFd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

void closeClientFd(int& fd) {
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
        fd = -1;
    }
}

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void setCloseOnExec(int fd) {
    const int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

void applySocketOptions(int fd) {
    const int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

uint16_t getBoundPort(int fd) {
    sockaddr_in addr {};
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

uint16_t nextPort(uint16_t port) {
    return port == std::numeric_limits<uint16_t>::max() ? 1 : static_cast<uint16_t>(port + 1);
}

bool parseBindAddress(const std::string& bind_address, in_addr& out) {
    if (inet_pton(AF_INET, bind_address.c_str(), &out) == 1) {
        return true;
    }
    if (bind_address == "localhost") {
        return inet_pton(AF_INET, "127.0.0.1", &out) == 1;
    }
    return false;
}

int openListenerSocket(const std::string& bind_address, uint16_t port) {
    in_addr parsed_addr {};
    if (!parseBindAddress(bind_address, parsed_addr)) {
        return -1;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    setCloseOnExec(fd);
    applySocketOptions(fd);

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr = parsed_addr;
    addr.sin_port = htons(port);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(fd, kBacklog) != 0 ||
        !setNonBlocking(fd)) {
        int close_fd = fd;
        closeFd(close_fd);
        return -1;
    }

    return fd;
}

bool prepareAcceptedSocket(int fd) {
    setCloseOnExec(fd);
    applySocketOptions(fd);
    return setNonBlocking(fd);
}

std::chrono::steady_clock::duration nextTimerDelay(uint32_t accum_ms, uint32_t interval_ms) {
    if (interval_ms == 0 || accum_ms >= interval_ms) {
        return std::chrono::milliseconds(0);
    }
    return std::chrono::milliseconds(interval_ms - accum_ms);
}

} // namespace

TNCServer::TNCServer(ModemAdapter& modem, TNCServerConfig config)
    : modem_(modem),
      config_(std::move(config)) {
    resetSession();
}

TNCServer::~TNCServer() {
    stop();
}

bool TNCServer::start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_.load()) {
        return false;
    }

    signal(SIGPIPE, SIG_IGN);
    stop_requested_.store(false);
    real_tick_accum_ms_ = 0;
    iamalive_override_accum_ms_ = 0;
    buffer_override_accum_ms_ = 0;
    cmd_line_buffer_.clear();
    cmd_tx_buffer_.clear();
    data_tx_buffer_.clear();
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        modem_events_.clear();
    }
    resetSession();

    if (!openWakeupPipe()) {
        closeServerFds();
        return false;
    }

    if (!openListeners()) {
        closeServerFds();
        return false;
    }

    running_.store(true);
    reactor_thread_ = std::thread(&TNCServer::reactorLoop, this);
    return true;
}

void TNCServer::stop() {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    if (!running_.load() && !reactor_thread_.joinable()) {
        return;
    }

    stop_requested_.store(true);
    wakeReactor();
    std::thread local_thread;
    if (reactor_thread_.joinable()) {
        local_thread = std::move(reactor_thread_);
    }
    lock.unlock();

    if (local_thread.joinable()) {
        local_thread.join();
    }

    running_.store(false);
}

bool TNCServer::isRunning() const {
    return running_.load();
}

uint16_t TNCServer::getCmdPort() const {
    return bound_cmd_port_.load();
}

uint16_t TNCServer::getDataPort() const {
    return bound_data_port_.load();
}

const TNCSession& TNCServer::getSession() const {
    return *session_;
}

void TNCServer::postModemConnected(const std::string& src, const std::string& dst, int bw) {
    TNCEvent event;
    event.type = TNCEventType::Connected;
    event.connect.source = src;
    event.connect.dest = dst;
    event.connect.bandwidth_hz = bw;
    enqueueModemEvent(std::move(event));
}

void TNCServer::postModemDisconnected() {
    TNCEvent event;
    event.type = TNCEventType::Disconnected;
    enqueueModemEvent(std::move(event));
}

void TNCServer::postModemPTT(bool on) {
    TNCEvent event;
    event.type = TNCEventType::PTT;
    event.ptt_on = on;
    enqueueModemEvent(std::move(event));
}

void TNCServer::postModemDataReceived(std::vector<uint8_t> bytes) {
    TNCEvent event;
    event.type = TNCEventType::DataReceived;
    event.data = std::move(bytes);
    enqueueModemEvent(std::move(event));
}

void TNCServer::postModemBufferLevel(int bytes) {
    TNCEvent event;
    event.type = TNCEventType::BufferLevel;
    event.bytes = bytes;
    enqueueModemEvent(std::move(event));
}

void TNCServer::postModemSNR(float db) {
    TNCEvent event;
    event.type = TNCEventType::SNR;
    event.snr_db = db;
    enqueueModemEvent(std::move(event));
}

void TNCServer::postModemBitrate(int bps) {
    TNCEvent event;
    event.type = TNCEventType::Bitrate;
    event.bitrate_bps = bps;
    enqueueModemEvent(std::move(event));
}

void TNCServer::postModemIncomingCall(std::string peer) {
    TNCEvent event;
    event.type = TNCEventType::IncomingCall;
    event.peer = std::move(peer);
    enqueueModemEvent(std::move(event));
}

void TNCServer::reactorLoop() {
    auto last_timer = std::chrono::steady_clock::now();

    while (!stop_requested_.load()) {
        std::vector<pollfd> poll_fds;
        std::vector<PollTarget> poll_targets;

        auto add_fd = [&](int fd, short events, PollTarget target) {
            if (fd >= 0) {
                poll_fds.push_back({fd, events, 0});
                poll_targets.push_back(target);
            }
        };

        add_fd(cmd_listener_fd_, POLLIN, PollTarget::CmdListener);
        add_fd(data_listener_fd_, POLLIN, PollTarget::DataListener);
        add_fd(cmd_client_fd_,
               static_cast<short>(POLLIN | POLLERR | POLLHUP | POLLNVAL |
                                  (cmd_tx_buffer_.empty() ? 0 : POLLOUT)),
               PollTarget::CmdClient);
        add_fd(data_client_fd_,
               static_cast<short>(POLLIN | POLLERR | POLLHUP | POLLNVAL |
                                  (data_tx_buffer_.empty() ? 0 : POLLOUT)),
               PollTarget::DataClient);
        add_fd(wakeup_read_fd_, POLLIN, PollTarget::Wakeup);

        int timeout_ms = kPollTimeoutMs;
        if (config_.iamalive_interval_ms != kDefaultIAmAliveMs && config_.iamalive_interval_ms > 0) {
            const auto delay = nextTimerDelay(iamalive_override_accum_ms_, config_.iamalive_interval_ms);
            timeout_ms = std::min(timeout_ms, static_cast<int>(
                                                  std::chrono::duration_cast<std::chrono::milliseconds>(delay).count()));
        }
        if (config_.buffer_rate_limit_ms != kDefaultBufferRateMs && config_.buffer_rate_limit_ms > 0) {
            const auto delay = nextTimerDelay(buffer_override_accum_ms_, config_.buffer_rate_limit_ms);
            timeout_ms = std::min(timeout_ms, static_cast<int>(
                                                  std::chrono::duration_cast<std::chrono::milliseconds>(delay).count()));
        }

        const int poll_rc = poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), timeout_ms);
        if (poll_rc < 0 && errno != EINTR) {
            break;
        }

        for (size_t i = 0; i < poll_fds.size(); ++i) {
            const short events = poll_fds[i].revents;
            if (events == 0) {
                continue;
            }

            switch (poll_targets[i]) {
            case PollTarget::CmdListener:
                if (poll_fds[i].fd == cmd_listener_fd_ && (events & POLLIN)) {
                    onCmdListenerReady();
                }
                break;
            case PollTarget::DataListener:
                if (poll_fds[i].fd == data_listener_fd_ && (events & POLLIN)) {
                    onDataListenerReady();
                }
                break;
            case PollTarget::CmdClient:
                if (poll_fds[i].fd == cmd_client_fd_) {
                    if (events & POLLIN) {
                        onCmdClientReady();
                    }
                    if (poll_fds[i].fd == cmd_client_fd_ && (events & POLLOUT)) {
                        if (!flushClientOutput(cmd_client_fd_, cmd_tx_buffer_)) {
                            evictCmdClient();
                        }
                    }
                    if (poll_fds[i].fd == cmd_client_fd_ && (events & (POLLERR | POLLHUP | POLLNVAL))) {
                        evictCmdClient();
                    }
                }
                break;
            case PollTarget::DataClient:
                if (poll_fds[i].fd == data_client_fd_) {
                    if (events & POLLIN) {
                        onDataClientReady();
                    }
                    if (poll_fds[i].fd == data_client_fd_ && (events & POLLOUT)) {
                        if (!flushClientOutput(data_client_fd_, data_tx_buffer_)) {
                            evictDataClient();
                        }
                    }
                    if (poll_fds[i].fd == data_client_fd_ && (events & (POLLERR | POLLHUP | POLLNVAL))) {
                        evictDataClient();
                    }
                }
                break;
            case PollTarget::Wakeup:
                if (poll_fds[i].fd == wakeup_read_fd_ && (events & POLLIN)) {
                    drainWakeupPipe();
                }
                break;
            }
        }

        drainModemQueue();

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_timer).count();
        if (elapsed_ms > 0) {
            const uint32_t clamped_elapsed =
                elapsed_ms > std::numeric_limits<uint32_t>::max()
                    ? std::numeric_limits<uint32_t>::max()
                    : static_cast<uint32_t>(elapsed_ms);
            onTimerTick(clamped_elapsed);
            last_timer = now;
        }

        flushAllOutputs();
    }

    closeServerFds();
    running_.store(false);
}

void TNCServer::onCmdListenerReady() {
    while (true) {
        const int fd = accept(cmd_listener_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }

        if (!prepareAcceptedSocket(fd)) {
            int close_fd = fd;
            closeClientFd(close_fd);
            continue;
        }

        evictCmdClient();
        resetSession();
        cmd_client_fd_ = fd;
        cmd_line_buffer_.clear();
        real_tick_accum_ms_ = 0;
        iamalive_override_accum_ms_ = 0;
        buffer_override_accum_ms_ = 0;
    }
}

void TNCServer::onDataListenerReady() {
    while (true) {
        const int fd = accept(data_listener_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }

        if (!prepareAcceptedSocket(fd)) {
            int close_fd = fd;
            closeClientFd(close_fd);
            continue;
        }

        evictDataClient();
        data_client_fd_ = fd;
    }
}

void TNCServer::onCmdClientReady() {
    uint8_t buffer[kReadBufferSize];
    while (cmd_client_fd_ >= 0) {
        const ssize_t n = recv(cmd_client_fd_, buffer, sizeof(buffer), 0);
        if (n > 0) {
            processControlBytes(buffer, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            evictCmdClient();
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (isWouldBlock()) {
            return;
        }
        evictCmdClient();
        return;
    }
}

void TNCServer::onDataClientReady() {
    uint8_t buffer[kReadBufferSize];
    while (data_client_fd_ >= 0) {
        const ssize_t n = recv(data_client_fd_, buffer, sizeof(buffer), 0);
        if (n > 0) {
            session_->handleDataBytes(std::vector<uint8_t>(buffer, buffer + n));
            continue;
        }
        if (n == 0) {
            evictDataClient();
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (isWouldBlock()) {
            return;
        }
        evictDataClient();
        return;
    }
}

void TNCServer::onTimerTick(uint32_t elapsed_ms) {
    auto addClamped = [](uint32_t value, uint32_t delta) {
        return value > std::numeric_limits<uint32_t>::max() - delta
                   ? std::numeric_limits<uint32_t>::max()
                   : value + delta;
    };

    real_tick_accum_ms_ = addClamped(real_tick_accum_ms_, elapsed_ms);
    while (real_tick_accum_ms_ >= kSessionTickMs) {
        session_->tick(kSessionTickMs);
        real_tick_accum_ms_ -= kSessionTickMs;
    }

    if (config_.iamalive_interval_ms != kDefaultIAmAliveMs && config_.iamalive_interval_ms > 0) {
        iamalive_override_accum_ms_ = addClamped(iamalive_override_accum_ms_, elapsed_ms);
        while (iamalive_override_accum_ms_ >= config_.iamalive_interval_ms) {
            session_->tick(kDefaultIAmAliveMs);
            iamalive_override_accum_ms_ -= config_.iamalive_interval_ms;
        }
    }

    if (config_.buffer_rate_limit_ms != kDefaultBufferRateMs && config_.buffer_rate_limit_ms > 0) {
        buffer_override_accum_ms_ = addClamped(buffer_override_accum_ms_, elapsed_ms);
        while (buffer_override_accum_ms_ >= config_.buffer_rate_limit_ms) {
            session_->tick(kDefaultBufferRateMs);
            buffer_override_accum_ms_ -= config_.buffer_rate_limit_ms;
        }
    }
}

void TNCServer::evictCmdClient() {
    closeClientFd(cmd_client_fd_);
    closeClientFd(data_client_fd_);
    cmd_line_buffer_.clear();
    cmd_tx_buffer_.clear();
    data_tx_buffer_.clear();
    resetSession();
    real_tick_accum_ms_ = 0;
    iamalive_override_accum_ms_ = 0;
    buffer_override_accum_ms_ = 0;
}

void TNCServer::evictDataClient() {
    closeClientFd(data_client_fd_);
    data_tx_buffer_.clear();
}

void TNCServer::emitToCmdClient(std::string_view line) {
    if (cmd_client_fd_ < 0 || line.empty()) {
        return;
    }
    cmd_tx_buffer_.insert(cmd_tx_buffer_.end(), line.begin(), line.end());
}

void TNCServer::emitToDataClient(const std::vector<uint8_t>& bytes) {
    if (data_client_fd_ < 0 || bytes.empty()) {
        return;
    }
    data_tx_buffer_.insert(data_tx_buffer_.end(), bytes.begin(), bytes.end());
}

bool TNCServer::openListeners() {
    closeFd(cmd_listener_fd_);
    closeFd(data_listener_fd_);
    bound_cmd_port_.store(0);
    bound_data_port_.store(0);

    const bool adjacent_ports =
        config_.data_port == 0 || (config_.cmd_port == 0 && config_.data_port == TNCServerConfig{}.data_port);
    constexpr int kOsEphemeralAttempts = 4;
    constexpr int kAdjacentScanAttempts = 4096;
    constexpr uint16_t kScanBasePort = 49152;
    constexpr uint16_t kScanPortCount = 16000;

    auto requestedCmdPortForAttempt = [&](int attempt) -> uint16_t {
        if (config_.cmd_port != 0) {
            return config_.cmd_port;
        }
        if (!adjacent_ports || attempt < kOsEphemeralAttempts) {
            return 0;
        }

        const auto seed = static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count() % kScanPortCount);
        return static_cast<uint16_t>(kScanBasePort +
                                     ((seed + static_cast<uint32_t>(attempt - kOsEphemeralAttempts)) %
                                      kScanPortCount));
    };

    const int attempts = config_.cmd_port == 0 && adjacent_ports
                             ? kOsEphemeralAttempts + kAdjacentScanAttempts
                             : 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const uint16_t requested_cmd = requestedCmdPortForAttempt(attempt);
        if (requested_cmd == std::numeric_limits<uint16_t>::max()) {
            continue;
        }

        cmd_listener_fd_ = openListenerSocket(config_.bind_address, requested_cmd);
        if (cmd_listener_fd_ < 0) {
            if (config_.cmd_port == 0 && adjacent_ports) {
                continue;
            }
            return false;
        }

        const uint16_t actual_cmd = getBoundPort(cmd_listener_fd_);
        const uint16_t requested_data = adjacent_ports ? nextPort(actual_cmd) : config_.data_port;
        data_listener_fd_ = openListenerSocket(config_.bind_address, requested_data);
        if (data_listener_fd_ >= 0) {
            bound_cmd_port_.store(actual_cmd);
            bound_data_port_.store(getBoundPort(data_listener_fd_));
            return true;
        }

        closeFd(cmd_listener_fd_);
    }

    return false;
}

bool TNCServer::openWakeupPipe() {
    closeFd(wakeup_read_fd_);
    closeFd(wakeup_write_fd_);

    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
        return false;
    }

    wakeup_read_fd_ = fds[0];
    wakeup_write_fd_ = fds[1];
    setCloseOnExec(wakeup_read_fd_);
    setCloseOnExec(wakeup_write_fd_);
    if (!setNonBlocking(wakeup_read_fd_) || !setNonBlocking(wakeup_write_fd_)) {
        closeFd(wakeup_read_fd_);
        closeFd(wakeup_write_fd_);
        return false;
    }
    return true;
}

void TNCServer::closeServerFds() {
    closeClientFd(cmd_client_fd_);
    closeClientFd(data_client_fd_);
    closeFd(cmd_listener_fd_);
    closeFd(data_listener_fd_);
    closeFd(wakeup_read_fd_);
    closeFd(wakeup_write_fd_);
    cmd_line_buffer_.clear();
    cmd_tx_buffer_.clear();
    data_tx_buffer_.clear();
    bound_cmd_port_.store(0);
    bound_data_port_.store(0);
}

void TNCServer::resetSession() {
    session_ = std::make_unique<TNCSession>(
        modem_,
        [this](std::string_view line) { emitToCmdClient(line); },
        [this](const std::vector<uint8_t>& bytes) { emitToDataClient(bytes); });
}

void TNCServer::enqueueModemEvent(TNCEvent event) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        modem_events_.push_back(std::move(event));
    }
    wakeReactor();
}

void TNCServer::wakeReactor() {
    if (wakeup_write_fd_ < 0) {
        return;
    }

    const uint8_t byte = 0;
    while (true) {
        const ssize_t n = write(wakeup_write_fd_, &byte, sizeof(byte));
        if (n == 1) {
            return;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

void TNCServer::drainWakeupPipe() {
    uint8_t buffer[128];
    while (wakeup_read_fd_ >= 0) {
        const ssize_t n = read(wakeup_read_fd_, buffer, sizeof(buffer));
        if (n > 0) {
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

void TNCServer::drainModemQueue() {
    std::deque<TNCEvent> local_events;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        local_events.swap(modem_events_);
    }

    for (const TNCEvent& event : local_events) {
        dispatchModemEvent(event);
    }
}

void TNCServer::dispatchModemEvent(const TNCEvent& event) {
    switch (event.type) {
    case TNCEventType::Connected:
        session_->onModemConnected(event.connect.source, event.connect.dest, event.connect.bandwidth_hz);
        break;
    case TNCEventType::Disconnected:
        session_->onModemDisconnected();
        break;
    case TNCEventType::PTT:
        session_->onModemPTT(event.ptt_on);
        break;
    case TNCEventType::BufferLevel:
        session_->onModemBufferLevel(event.bytes);
        break;
    case TNCEventType::SNR:
        session_->onModemSNR(event.snr_db);
        break;
    case TNCEventType::Bitrate:
        session_->onModemBitrate(event.bitrate_bps);
        break;
    case TNCEventType::IncomingCall:
        session_->onModemIncomingCall(event.peer);
        break;
    case TNCEventType::DataReceived:
        session_->onModemDataReceived(event.data);
        break;
    case TNCEventType::Pending:
    case TNCEventType::CancelPending:
    case TNCEventType::IAmAlive:
        break;
    }
}

void TNCServer::processControlBytes(const uint8_t* bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        const char ch = static_cast<char>(bytes[i]);
        if (ch == '\r') {
            session_->handleControlLine(cmd_line_buffer_);
            cmd_line_buffer_.clear();
            continue;
        }
        if (ch == '\n') {
            continue;
        }
        if (cmd_line_buffer_.size() >= kMaxControlLine) {
            session_->handleControlLine("");
            cmd_line_buffer_.clear();
            continue;
        }
        cmd_line_buffer_.push_back(ch);
    }
}

void TNCServer::flushAllOutputs() {
    if (cmd_client_fd_ >= 0 && !cmd_tx_buffer_.empty() &&
        !flushClientOutput(cmd_client_fd_, cmd_tx_buffer_)) {
        evictCmdClient();
    }
    if (data_client_fd_ >= 0 && !data_tx_buffer_.empty() &&
        !flushClientOutput(data_client_fd_, data_tx_buffer_)) {
        evictDataClient();
    }
}

bool TNCServer::flushClientOutput(int fd, std::vector<uint8_t>& buffer) {
    while (!buffer.empty()) {
        const ssize_t n = send(fd, buffer.data(), buffer.size(), sendFlags());
        if (n > 0) {
            buffer.erase(buffer.begin(), buffer.begin() + n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && isWouldBlock()) {
            return true;
        }
        return false;
    }
    return true;
}

} // namespace ultra::tnc
