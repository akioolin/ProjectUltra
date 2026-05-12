#include "ptt/ptt_driver_factory.hpp"

#include "ptt/cat_ptt_driver.hpp"
#ifdef ULTRA_HAVE_LIBHAMLIB
#include "ptt/hamlib_rig_driver.hpp"
#endif
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
    case PttMode::HamlibBuiltin:
        return "hamlib_builtin";
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

const char* hamlibPttMethodName(HamlibPttMethod method) {
    switch (method) {
    case HamlibPttMethod::Vox:
        return "vox";
    case HamlibPttMethod::Cat:
        return "cat";
    case HamlibPttMethod::DTR:
        return "dtr";
    case HamlibPttMethod::RTS:
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
    case PttMode::HamlibBuiltin:
#ifdef ULTRA_HAVE_LIBHAMLIB
        return std::make_unique<HamlibRigDriver>(config);
#else
        return std::make_unique<NullPttDriver>();
#endif
    case PttMode::None:
    default:
        return std::make_unique<NullPttDriver>();
    }
}

} // namespace ultra::ptt
