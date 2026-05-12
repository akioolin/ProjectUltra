#include "ptt/ptt_driver_factory.hpp"

#include "ptt/cat_ptt_driver.hpp"
#include "ptt/null_ptt_driver.hpp"
#include "ptt/serial_ptt_driver.hpp"

namespace ultra::ptt {

const char* pttModeName(PttMode mode) {
    switch (mode) {
    case PttMode::None:
        return "none";
    case PttMode::Serial:
        return "serial";
    case PttMode::Cat:
        return "cat";
    }
    return "unknown";
}

const char* serialLineName(SerialLine line) {
    switch (line) {
    case SerialLine::DTR:
        return "dtr";
    case SerialLine::RTS:
        return "rts";
    }
    return "unknown";
}

std::unique_ptr<IPttDriver> createPttDriver(const PttConfig& config) {
    switch (config.mode) {
    case PttMode::Serial:
        return std::make_unique<SerialPttDriver>(config);
    case PttMode::Cat:
        return std::make_unique<CatPttDriver>(config);
    case PttMode::None:
    default:
        return std::make_unique<NullPttDriver>();
    }
}

} // namespace ultra::ptt
