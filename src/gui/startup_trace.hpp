#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace ultra {
namespace gui {

inline void startupTrace(const char* component, const char* phase) {
#ifdef _WIN32
    static char g_trace_path[1024] = {0};
    static bool g_path_initialized = false;

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

    if (FILE* f = std::fopen(g_trace_path, "a")) {
        static uint64_t seq = 0;
        unsigned long long t = static_cast<unsigned long long>(++seq);
        std::fprintf(f, "[%llu][STARTUP][%s] %s\n",
                     t,
                     component ? component : "<unknown>",
                     phase ? phase : "<unknown>");
        std::fflush(f);
        std::fclose(f);
    }
#else
    (void)component;
    (void)phase;
#endif
}

}  // namespace gui
}  // namespace ultra
