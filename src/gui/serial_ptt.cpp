#include "serial_ptt.hpp"
#include "ultra/logging.hpp"

#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <cstdio>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

namespace ultra {
namespace gui {

namespace {

#ifndef _WIN32
speed_t baudToSpeed(int baud_rate) {
    switch (baud_rate) {
        case 1200: return B1200;
        case 2400: return B2400;
        case 4800: return B4800;
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default: return B9600;
    }
}
#endif

#ifdef _WIN32
std::string normalizePortPath(const std::string& port_name) {
    if (port_name.empty()) {
        return port_name;
    }

    if (port_name.rfind("\\\\.\\", 0) == 0) {
        return port_name;
    }

    std::string upper = port_name;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    if (upper.rfind("COM", 0) == 0) {
        return "\\\\.\\" + port_name;
    }

    return port_name;
}
#endif

} // namespace

SerialPttController::~SerialPttController() {
    close();
}

bool SerialPttController::isOpen() const {
#ifdef _WIN32
    return handle_ != INVALID_HANDLE_VALUE;
#else
    return fd_ >= 0;
#endif
}

bool SerialPttController::matches(const std::string& port_name, int baud_rate) const {
    return isOpen() && port_name_ == port_name && baud_rate_ == baud_rate;
}

bool SerialPttController::open(const std::string& port_name, int baud_rate) {
    if (port_name.empty()) {
        LOG_MODEM(ERROR, "PTT: serial port is empty");
        return false;
    }

    int safe_baud = (baud_rate > 0) ? baud_rate : 9600;
    if (matches(port_name, safe_baud)) {
        return true;
    }

    close();

#ifdef _WIN32
    std::string port_path = normalizePortPath(port_name);
    handle_ = CreateFileA(port_path.c_str(),
                          GENERIC_READ | GENERIC_WRITE,
                          0,
                          nullptr,
                          OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL,
                          nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        LOG_MODEM(ERROR, "PTT: failed to open '%s' (err=%lu)", port_path.c_str(), GetLastError());
        return false;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle_, &dcb)) {
        LOG_MODEM(ERROR, "PTT: GetCommState failed (err=%lu)", GetLastError());
        close();
        return false;
    }

    dcb.BaudRate = static_cast<DWORD>(safe_baud);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;

    if (!SetCommState(handle_, &dcb)) {
        LOG_MODEM(ERROR, "PTT: SetCommState failed (err=%lu)", GetLastError());
        close();
        return false;
    }

    COMMTIMEOUTS timeouts{};
    SetCommTimeouts(handle_, &timeouts);

    port_name_ = port_name;
    baud_rate_ = safe_baud;
    LOG_MODEM(INFO, "PTT: opened serial port '%s' @ %d", port_name_.c_str(), baud_rate_);
    return true;
#else
    fd_ = ::open(port_name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        LOG_MODEM(ERROR, "PTT: failed to open '%s' (%s)", port_name.c_str(), std::strerror(errno));
        return false;
    }

    termios tio{};
    if (tcgetattr(fd_, &tio) == 0) {
        cfmakeraw(&tio);
        speed_t speed = baudToSpeed(safe_baud);
        cfsetispeed(&tio, speed);
        cfsetospeed(&tio, speed);
        tio.c_cflag |= (CLOCAL | CREAD);
        tcsetattr(fd_, TCSANOW, &tio);
    }

    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
    }

    port_name_ = port_name;
    baud_rate_ = safe_baud;
    LOG_MODEM(INFO, "PTT: opened serial port '%s' @ %d", port_name_.c_str(), baud_rate_);
    return true;
#endif
}

void SerialPttController::close() {
    if (!isOpen()) {
        return;
    }

#ifdef _WIN32
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
#else
    ::close(fd_);
    fd_ = -1;
#endif
    port_name_.clear();
    baud_rate_ = 0;
}

bool SerialPttController::setLine(SerialPttLine line, bool asserted) {
    if (!isOpen()) {
        return false;
    }

#ifdef _WIN32
    DWORD fn = 0;
    if (line == SerialPttLine::DTR) {
        fn = asserted ? SETDTR : CLRDTR;
    } else {
        fn = asserted ? SETRTS : CLRRTS;
    }

    if (!EscapeCommFunction(handle_, fn)) {
        LOG_MODEM(ERROR, "PTT: EscapeCommFunction failed (err=%lu)", GetLastError());
        return false;
    }
    return true;
#else
    int status = 0;
    if (ioctl(fd_, TIOCMGET, &status) != 0) {
        LOG_MODEM(ERROR, "PTT: ioctl(TIOCMGET) failed (%s)", std::strerror(errno));
        return false;
    }

    int bit = (line == SerialPttLine::DTR) ? TIOCM_DTR : TIOCM_RTS;
    if (asserted) {
        status |= bit;
    } else {
        status &= ~bit;
    }

    if (ioctl(fd_, TIOCMSET, &status) != 0) {
        LOG_MODEM(ERROR, "PTT: ioctl(TIOCMSET) failed (%s)", std::strerror(errno));
        return false;
    }
    return true;
#endif
}

} // namespace gui
} // namespace ultra
