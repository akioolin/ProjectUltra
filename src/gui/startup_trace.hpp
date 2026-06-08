#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Prevent Windows ERROR macro from corrupting LOG_* macros in translation units
#ifdef ERROR
#undef ERROR
#endif
#endif

namespace ultra {
namespace gui {

// Historical Windows-only startup instrumentation: it appended to startup_trace.log while
// diagnosing a GUI cold-start hang (now resolved). Reduced to a no-op so no startup_trace.log is
// ever created. Kept as a stub so the historical call sites still compile, and the _WIN32 block
// above still shields LOG_* from the Windows ERROR macro for the translation units that include it.
inline void startupTrace(const char* component, const char* phase) {
    (void)component;
    (void)phase;
}

}  // namespace gui
}  // namespace ultra
