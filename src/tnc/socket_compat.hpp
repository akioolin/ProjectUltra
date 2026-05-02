#pragma once

#include <cstddef>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace ultra::tnc {

using socket_t = SOCKET;
using socket_io_result_t = int;
using socket_len_t = int;
using poll_count_t = ULONG;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;

#ifndef POLLIN
#define POLLIN POLLRDNORM
#endif
#ifndef POLLOUT
#define POLLOUT POLLWRNORM
#endif

#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace ultra::tnc {

using socket_t = int;
using socket_io_result_t = ssize_t;
using socket_len_t = socklen_t;
using poll_count_t = nfds_t;
constexpr socket_t kInvalidSocket = -1;

#endif

class WinsockInit {
public:
    WinsockInit();
    ~WinsockInit();

    WinsockInit(const WinsockInit&) = delete;
    WinsockInit& operator=(const WinsockInit&) = delete;

    bool ok() const;

private:
    bool initialized_ = false;
};

bool isInvalidSocket(socket_t socket);
int closeSocket(socket_t socket);
int shutdownSocket(socket_t socket);
int pollSockets(pollfd* fds, poll_count_t count, int timeout_ms);
bool setNonblocking(socket_t socket);
bool socketPair(socket_t pair[2]);

socket_io_result_t recvSocket(socket_t socket, void* buffer, size_t size, int flags);
socket_io_result_t sendSocket(socket_t socket, const void* buffer, size_t size, int flags);
socket_io_result_t readSocketPair(socket_t socket, void* buffer, size_t size);
socket_io_result_t writeSocketPair(socket_t socket, const void* buffer, size_t size);

bool isInterruptedSocketError();
bool isWouldBlockSocketError();

} // namespace ultra::tnc
