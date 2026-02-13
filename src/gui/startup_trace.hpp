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

inline void startupTrace(const char* component, const char* phase) {
#ifdef _WIN32
    static char g_trace_path[1024] = {0};
    static bool g_path_initialized = false;
    static HANDLE g_trace_handle = INVALID_HANDLE_VALUE;

    if (!g_path_initialized) {
        const char* env_path = std::getenv("ULTRA_STARTUP_LOG");
        if (env_path && env_path[0] != '\0') {
            std::snprintf(g_trace_path, sizeof(g_trace_path), "%s", env_path);
        } else {
            std::snprintf(g_trace_path, sizeof(g_trace_path), "startup_trace.log");
        }
        g_path_initialized = true;
    }

    if (g_trace_path[0] == '\0') {
        return;
    }

    if (g_trace_handle == INVALID_HANDLE_VALUE) {
        g_trace_handle = CreateFileA(g_trace_path,
                                     FILE_APPEND_DATA,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr,
                                     OPEN_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL,
                                     nullptr);
        if (g_trace_handle == INVALID_HANDLE_VALUE) {
            return;
        }
    }

    static uint64_t seq = 0;
    unsigned long long t = static_cast<unsigned long long>(++seq);
    char line[512];
    int len = std::snprintf(line, sizeof(line), "[%llu][STARTUP][%s] %s\r\n",
                            t,
                            component ? component : "<unknown>",
                            phase ? phase : "<unknown>");
    if (len <= 0) {
        return;
    }
    if (len > static_cast<int>(sizeof(line))) {
        len = static_cast<int>(sizeof(line));
    }

    DWORD written = 0;
    WriteFile(g_trace_handle, line, static_cast<DWORD>(len), &written, nullptr);
    FlushFileBuffers(g_trace_handle);
#else
    (void)component;
    (void)phase;
#endif
}

}  // namespace gui
}  // namespace ultra
