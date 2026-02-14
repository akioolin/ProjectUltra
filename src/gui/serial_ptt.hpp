#pragma once

#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ultra {
namespace gui {

enum class SerialPttLine {
    DTR = 0,
    RTS = 1
};

class SerialPttController {
public:
    SerialPttController() = default;
    ~SerialPttController();

    bool open(const std::string& port_name, int baud_rate);
    void close();
    bool isOpen() const;
    bool matches(const std::string& port_name, int baud_rate) const;

    bool setLine(SerialPttLine line, bool asserted);

    const std::string& portName() const { return port_name_; }
    int baudRate() const { return baud_rate_; }

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif

    std::string port_name_;
    int baud_rate_ = 0;
};

} // namespace gui
} // namespace ultra
