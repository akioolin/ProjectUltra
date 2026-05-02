#include "tnc/socket_compat.hpp"

#include <algorithm>
#include <limits>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#endif

namespace ultra::tnc {
namespace {

#ifdef _WIN32
int clampSocketIoSize(size_t size) {
    return static_cast<int>(
        std::min(size, static_cast<size_t>(std::numeric_limits<int>::max())));
}
#endif

void closeIfValid(socket_t socket) {
    if (!isInvalidSocket(socket)) {
        (void)closeSocket(socket);
    }
}

} // namespace

WinsockInit::WinsockInit() {
#ifdef _WIN32
    WSADATA data {};
    initialized_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    initialized_ = true;
#endif
}

WinsockInit::~WinsockInit() {
#ifdef _WIN32
    if (initialized_) {
        WSACleanup();
    }
#endif
}

bool WinsockInit::ok() const {
    return initialized_;
}

bool isInvalidSocket(socket_t socket) {
    return socket == kInvalidSocket;
}

int closeSocket(socket_t socket) {
#ifdef _WIN32
    return ::closesocket(socket);
#else
    return ::close(socket);
#endif
}

int shutdownSocket(socket_t socket) {
#ifdef _WIN32
    return ::shutdown(socket, SD_BOTH);
#else
    return ::shutdown(socket, SHUT_RDWR);
#endif
}

int pollSockets(pollfd* fds, poll_count_t count, int timeout_ms) {
#ifdef _WIN32
    return WSAPoll(fds, count, timeout_ms);
#else
    return ::poll(fds, count, timeout_ms);
#endif
}

bool setNonblocking(socket_t socket) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool socketPair(socket_t pair[2]) {
    pair[0] = kInvalidSocket;
    pair[1] = kInvalidSocket;

#ifdef _WIN32
    socket_t listener = kInvalidSocket;
    socket_t connector = kInvalidSocket;
    socket_t accepted = kInvalidSocket;

    listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (isInvalidSocket(listener)) {
        return false;
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 1) != 0) {
        closeIfValid(listener);
        return false;
    }

    socket_len_t addr_len = static_cast<socket_len_t>(sizeof(addr));
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
        closeIfValid(listener);
        return false;
    }

    connector = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (isInvalidSocket(connector)) {
        closeIfValid(listener);
        return false;
    }

    if (::connect(connector, reinterpret_cast<sockaddr*>(&addr), addr_len) != 0) {
        closeIfValid(connector);
        closeIfValid(listener);
        return false;
    }

    accepted = ::accept(listener, nullptr, nullptr);
    if (isInvalidSocket(accepted)) {
        closeIfValid(connector);
        closeIfValid(listener);
        return false;
    }

    closeIfValid(listener);
    pair[0] = connector;
    pair[1] = accepted;
    return true;
#else
    return ::pipe(pair) == 0;
#endif
}

socket_io_result_t recvSocket(socket_t socket, void* buffer, size_t size, int flags) {
#ifdef _WIN32
    return ::recv(socket, static_cast<char*>(buffer), clampSocketIoSize(size), flags);
#else
    return ::recv(socket, buffer, size, flags);
#endif
}

socket_io_result_t sendSocket(socket_t socket, const void* buffer, size_t size, int flags) {
#ifdef _WIN32
    return ::send(socket, static_cast<const char*>(buffer), clampSocketIoSize(size), flags);
#else
    return ::send(socket, buffer, size, flags);
#endif
}

socket_io_result_t readSocketPair(socket_t socket, void* buffer, size_t size) {
#ifdef _WIN32
    return recvSocket(socket, buffer, size, 0);
#else
    return ::read(socket, buffer, size);
#endif
}

socket_io_result_t writeSocketPair(socket_t socket, const void* buffer, size_t size) {
#ifdef _WIN32
    return sendSocket(socket, buffer, size, 0);
#else
    return ::write(socket, buffer, size);
#endif
}

bool isInterruptedSocketError() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

bool isWouldBlockSocketError() {
#ifdef _WIN32
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

} // namespace ultra::tnc
