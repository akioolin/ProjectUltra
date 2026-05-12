#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ultra::ptt {

enum class PttKey {
    Off,
    On
};

enum class PttMode {
    None,
    Serial,
    Cat,
    HamlibBuiltin
};

enum class SerialLine {
    DTR,
    RTS
};

enum class HamlibPttMethod {
    Vox,
    Cat,
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
    int cat_frequency_poll_ms = 1500;

    int hamlib_model_id = 1;
    std::string hamlib_rig_port;
    int hamlib_baud = 9600;
    HamlibPttMethod hamlib_ptt_method = HamlibPttMethod::Cat;
    int hamlib_frequency_poll_ms = 1500;
};

inline bool operator==(const PttConfig& lhs, const PttConfig& rhs) {
    return lhs.mode == rhs.mode &&
           lhs.serial_port == rhs.serial_port &&
           lhs.serial_baud == rhs.serial_baud &&
           lhs.serial_line == rhs.serial_line &&
           lhs.serial_inactive_high == rhs.serial_inactive_high &&
           lhs.cat_host == rhs.cat_host &&
           lhs.cat_port == rhs.cat_port &&
           lhs.cat_frequency_poll_ms == rhs.cat_frequency_poll_ms &&
           lhs.hamlib_model_id == rhs.hamlib_model_id &&
           lhs.hamlib_rig_port == rhs.hamlib_rig_port &&
           lhs.hamlib_baud == rhs.hamlib_baud &&
           lhs.hamlib_ptt_method == rhs.hamlib_ptt_method &&
           lhs.hamlib_frequency_poll_ms == rhs.hamlib_frequency_poll_ms;
}

inline bool operator!=(const PttConfig& lhs, const PttConfig& rhs) {
    return !(lhs == rhs);
}

class IPttDriver {
public:
    struct RadioFrequencyState {
        std::optional<int64_t> hz;
        bool connected = false;
        bool stale = false;
    };

    virtual ~IPttDriver() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual bool setKey(PttKey state) = 0;
    virtual bool testCycle() = 0;
    virtual std::string lastError() const = 0;

    virtual bool startTelemetry() { return false; }
    virtual bool testCat() { return open(); }
    virtual std::optional<int64_t> currentFrequencyHz() const { return std::nullopt; }
    virtual RadioFrequencyState radioFrequencyState() const { return {}; }
};

const char* pttModeName(PttMode mode);
const char* serialLineName(SerialLine line);
const char* hamlibPttMethodName(HamlibPttMethod method);

} // namespace ultra::ptt
