#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

namespace ultra {
namespace gui {

inline void startupTrace(const char* component, const char* phase) {
#ifdef _WIN32
    static std::mutex trace_mutex;
    std::lock_guard<std::mutex> lock(trace_mutex);

    namespace fs = std::filesystem;

    std::string line;
    {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
        char buf[512];
        std::snprintf(
            buf, sizeof(buf), "[%lld][STARTUP][%s] %s\n",
            static_cast<long long>(ms),
            component ? component : "<unknown>",
            phase ? phase : "<unknown>"
        );
        line = buf;
    }

    std::string env_log;
    if (const char* p = std::getenv("ULTRA_STARTUP_LOG")) {
        env_log = p;
    }

    fs::path log_path;
    if (!env_log.empty()) {
        log_path = fs::path(env_log);
    } else if (const char* temp = std::getenv("TEMP")) {
        log_path = fs::path(temp) / "ProjectUltra" / "startup.log";
    } else {
        log_path = fs::path("startup.log");
    }

    std::error_code ec;
    if (!log_path.parent_path().empty()) {
        fs::create_directories(log_path.parent_path(), ec);
    }

    if (FILE* f = std::fopen(log_path.string().c_str(), "a")) {
        std::fwrite(line.data(), 1, line.size(), f);
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

