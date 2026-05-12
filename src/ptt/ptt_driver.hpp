#pragma once

#include <cstdint>
#include <string>

namespace ultra::ptt {

enum class PttKey {
    Off,
    On
};

enum class PttMode {
    None,
    Serial,
    Cat
};

enum class SerialLine {
    DTR,
    RTS
};

struct PttConfig {
    PttMode mode = PttMode::None;

    std::string serial_port;
    int serial_baud = 9600;
    SerialLine serial_line = SerialLine::RTS;
    bool serial_inactive_high = false;

    std::string cat_host = "127.0.0.1";
    uint16_t cat_port = 4532;
};

inline bool operator==(const PttConfig& lhs, const PttConfig& rhs) {
    return lhs.mode == rhs.mode &&
           lhs.serial_port == rhs.serial_port &&
           lhs.serial_baud == rhs.serial_baud &&
           lhs.serial_line == rhs.serial_line &&
           lhs.serial_inactive_high == rhs.serial_inactive_high &&
           lhs.cat_host == rhs.cat_host &&
           lhs.cat_port == rhs.cat_port;
}

inline bool operator!=(const PttConfig& lhs, const PttConfig& rhs) {
    return !(lhs == rhs);
}

class IPttDriver {
public:
    virtual ~IPttDriver() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual bool setKey(PttKey state) = 0;
    virtual bool testCycle() = 0;
    virtual std::string lastError() const = 0;
};

const char* pttModeName(PttMode mode);
const char* serialLineName(SerialLine line);

} // namespace ultra::ptt
