#pragma once

#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <cctype>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <mutex>
#endif

// Windows headers define ERROR as a macro - undefine it
#ifdef ERROR
#undef ERROR
#endif

namespace ultra {

// Start time for relative timestamps
inline auto g_log_start_time = std::chrono::steady_clock::now();

// Log output file (nullptr = stderr)
inline FILE* g_log_file = nullptr;
#ifdef _WIN32
inline SRWLOCK g_log_lock = SRWLOCK_INIT;
#else
inline std::mutex g_log_mutex;
#endif

// Thread-local station tag for multi-station debug (e.g. "ALPHA" or "BRAVO")
inline thread_local const char* g_log_station_tag = nullptr;

inline void setLogStationTag(const char* tag) { g_log_station_tag = tag; }

// Log levels
enum class LogLevel {
    NONE = 0,
    ERROR = 1,
    WARN = 2,
    INFO = 3,
    DEBUG = 4,
    TRACE = 5
};

// Global log level - can be changed at runtime
// Default console logging is operator-facing. Debug/trace output must be
// requested explicitly from the front-end binaries.
inline LogLevel g_log_level = LogLevel::INFO;

// Log category enable flags for fine-grained control
struct LogCategories {
    bool operator_events = true; // Startup, session, mode, PTT
    bool audio = true;           // Audio device setup
    bool tnc = true;             // TNC shell/server
    bool demod = true;           // Demodulator
    bool modem = true;           // Modem engine/protocol internals
    bool ldpc = false;           // LDPC codec (very verbose)
    bool sync = true;            // Sync detection
    bool channel = false;        // Channel estimation
    bool other = true;           // Legacy/ad-hoc categories
};

inline LogCategories g_log_categories;
inline bool g_log_category_filter_enabled = false;

inline std::string normalizeLogName(std::string name) {
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) {
        name.erase(0, 1);
    }
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
        name.pop_back();
    }
    for (char& c : name) {
        if (c == '-') c = '_';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name;
}

inline bool parseLogLevel(const std::string& name, LogLevel& out) {
    const std::string level = normalizeLogName(name);
    if (level == "error") { out = LogLevel::ERROR; return true; }
    if (level == "warn" || level == "warning") { out = LogLevel::WARN; return true; }
    if (level == "info") { out = LogLevel::INFO; return true; }
    if (level == "debug") { out = LogLevel::DEBUG; return true; }
    if (level == "trace") { out = LogLevel::TRACE; return true; }
    return false;
}

inline const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::ERROR: return "error";
        case LogLevel::WARN:  return "warn";
        case LogLevel::INFO:  return "info";
        case LogLevel::DEBUG: return "debug";
        case LogLevel::TRACE: return "trace";
        default: return "none";
    }
}

// Set log level
inline void setLogLevel(LogLevel level) {
    g_log_level = level;
}

inline void setOperatorLogProfile() {
    g_log_category_filter_enabled = true;
    g_log_categories.operator_events = true;
    g_log_categories.audio = true;
    g_log_categories.tnc = true;
    g_log_categories.demod = false;
    g_log_categories.modem = false;
    g_log_categories.ldpc = false;
    g_log_categories.sync = false;
    g_log_categories.channel = false;
    g_log_categories.other = false;
}

inline void setDeveloperLogProfile() {
    g_log_category_filter_enabled = true;
    g_log_categories.operator_events = true;
    g_log_categories.audio = true;
    g_log_categories.tnc = true;
    g_log_categories.demod = true;
    g_log_categories.modem = true;
    g_log_categories.ldpc = true;
    g_log_categories.sync = true;
    g_log_categories.channel = true;
    g_log_categories.other = true;
}

inline void clearLogCategories() {
    g_log_category_filter_enabled = true;
    g_log_categories.operator_events = false;
    g_log_categories.audio = false;
    g_log_categories.tnc = false;
    g_log_categories.demod = false;
    g_log_categories.modem = false;
    g_log_categories.ldpc = false;
    g_log_categories.sync = false;
    g_log_categories.channel = false;
    g_log_categories.other = false;
}

inline bool setLogCategory(const std::string& raw_name, bool enabled = true) {
    const std::string name = normalizeLogName(raw_name);
    g_log_category_filter_enabled = true;
    if (name == "all") {
        g_log_categories.operator_events = enabled;
        g_log_categories.audio = enabled;
        g_log_categories.tnc = enabled;
        g_log_categories.demod = enabled;
        g_log_categories.modem = enabled;
        g_log_categories.ldpc = enabled;
        g_log_categories.sync = enabled;
        g_log_categories.channel = enabled;
        g_log_categories.other = enabled;
        return true;
    }
    if (name == "none") {
        clearLogCategories();
        return true;
    }
    if (name == "operator" || name == "op" || name == "events") {
        g_log_categories.operator_events = enabled; return true;
    }
    if (name == "audio") { g_log_categories.audio = enabled; return true; }
    if (name == "tnc") { g_log_categories.tnc = enabled; return true; }
    if (name == "demod" || name == "demodulator") { g_log_categories.demod = enabled; return true; }
    if (name == "modem" || name == "mod" || name == "protocol" || name == "arq") {
        g_log_categories.modem = enabled; return true;
    }
    if (name == "ldpc" || name == "fec") { g_log_categories.ldpc = enabled; return true; }
    if (name == "sync") { g_log_categories.sync = enabled; return true; }
    if (name == "chan" || name == "channel") { g_log_categories.channel = enabled; return true; }
    if (name == "other") { g_log_categories.other = enabled; return true; }
    return false;
}

inline bool setLogCategories(const std::string& spec) {
    clearLogCategories();
    size_t start = 0;
    bool saw_token = false;
    while (start <= spec.size()) {
        const size_t comma = spec.find(',', start);
        std::string token = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        token = normalizeLogName(token);
        if (!token.empty()) {
            saw_token = true;
            if (!setLogCategory(token, true)) {
                return false;
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return saw_token;
}

inline bool logCategoryEquals(const char* value, const char* expected) {
    if (!value || !expected) return false;
    while (std::isspace(static_cast<unsigned char>(*value))) ++value;
    while (*value && *expected) {
        char a = *value++;
        char b = *expected++;
        if (a == '-') a = '_';
        if (b == '-') b = '_';
        a = static_cast<char>(std::tolower(static_cast<unsigned char>(a)));
        b = static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
        if (a != b) return false;
    }
    while (std::isspace(static_cast<unsigned char>(*value))) ++value;
    return *value == '\0' && *expected == '\0';
}

inline bool isLogCategoryEnabled(const char* category) {
    if (!g_log_category_filter_enabled) return true;
    if (logCategoryEquals(category, "operator") ||
        logCategoryEquals(category, "op") ||
        logCategoryEquals(category, "events")) return g_log_categories.operator_events;
    if (logCategoryEquals(category, "audio")) return g_log_categories.audio;
    if (logCategoryEquals(category, "tnc")) return g_log_categories.tnc;
    if (logCategoryEquals(category, "demod") ||
        logCategoryEquals(category, "demodulator")) return g_log_categories.demod;
    if (logCategoryEquals(category, "modem") ||
        logCategoryEquals(category, "mod") ||
        logCategoryEquals(category, "protocol") ||
        logCategoryEquals(category, "arq")) return g_log_categories.modem;
    if (logCategoryEquals(category, "ldpc") ||
        logCategoryEquals(category, "fec")) return g_log_categories.ldpc;
    if (logCategoryEquals(category, "sync")) return g_log_categories.sync;
    if (logCategoryEquals(category, "chan") ||
        logCategoryEquals(category, "channel")) return g_log_categories.channel;
    return g_log_categories.other;
}

inline bool shouldLog(LogLevel level, const char* category) {
    return level <= g_log_level &&
           (level <= LogLevel::WARN || isLogCategoryEnabled(category));
}

// Set log output file (call with nullptr to use stderr)
inline void setLogFile(FILE* file) {
#ifdef _WIN32
    AcquireSRWLockExclusive(&g_log_lock);
    g_log_file = file;
    ReleaseSRWLockExclusive(&g_log_lock);
#else
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_file = file;
#endif
}

// Core logging function
inline void log(LogLevel level, const char* category, const char* format, ...) {
    if (level > g_log_level) return;
    if (level > LogLevel::WARN && !isLogCategoryEnabled(category)) return;

#ifdef _WIN32
    AcquireSRWLockShared(&g_log_lock);
    FILE* out = g_log_file;  // No stderr fallback on GUI subsystem builds.
    if (!out) {
        ReleaseSRWLockShared(&g_log_lock);
        return;
    }
#else
    std::lock_guard<std::mutex> lock(g_log_mutex);
    FILE* out = g_log_file ? g_log_file : stderr;
#endif

    // Get elapsed time in milliseconds
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_log_start_time).count();
    int secs = static_cast<int>(elapsed / 1000);
    int ms = static_cast<int>(elapsed % 1000);

    const char* level_str = "";
    switch (level) {
        case LogLevel::ERROR: level_str = "ERROR"; break;
        case LogLevel::WARN:  level_str = "WARN "; break;
        case LogLevel::INFO:  level_str = "INFO "; break;
        case LogLevel::DEBUG: level_str = "DEBUG"; break;
        case LogLevel::TRACE: level_str = "TRACE"; break;
        default: break;
    }

    if (g_log_station_tag && g_log_station_tag[0]) {
        fprintf(out, "[%3d.%03d][%s][%s] [%s] ", secs, ms, level_str, category, g_log_station_tag);
    } else {
        fprintf(out, "[%3d.%03d][%s][%s] ", secs, ms, level_str, category);
    }

    va_list args;
    va_start(args, format);
    vfprintf(out, format, args);
    va_end(args);

    fprintf(out, "\n");
    fflush(out);
#ifdef _WIN32
    ReleaseSRWLockShared(&g_log_lock);
#endif
}

// Convenience macros - these compile to nothing when ULTRA_LOG_DISABLE is defined
#ifdef ULTRA_LOG_DISABLE

#define LOG_ERROR(cat, fmt, ...)
#define LOG_WARN(cat, fmt, ...)
#define LOG_INFO(cat, fmt, ...)
#define LOG_DEBUG(cat, fmt, ...)
#define LOG_TRACE(cat, fmt, ...)

#else

#define LOG_ERROR(cat, fmt, ...) \
    ultra::log(ultra::LogLevel::ERROR, cat, fmt, ##__VA_ARGS__)

#define LOG_WARN(cat, fmt, ...) \
    ultra::log(ultra::LogLevel::WARN, cat, fmt, ##__VA_ARGS__)

#define LOG_INFO(cat, fmt, ...) \
    ultra::log(ultra::LogLevel::INFO, cat, fmt, ##__VA_ARGS__)

#define LOG_DEBUG(cat, fmt, ...) \
    do { if (ultra::g_log_level >= ultra::LogLevel::DEBUG) \
        ultra::log(ultra::LogLevel::DEBUG, cat, fmt, ##__VA_ARGS__); } while(0)

#define LOG_TRACE(cat, fmt, ...) \
    do { if (ultra::g_log_level >= ultra::LogLevel::TRACE) \
        ultra::log(ultra::LogLevel::TRACE, cat, fmt, ##__VA_ARGS__); } while(0)

#endif

// Category-specific logging macros
#define LOG_DEMOD(level, fmt, ...) \
    do { if (ultra::shouldLog(ultra::LogLevel::level, "DEMOD")) LOG_##level("DEMOD", fmt, ##__VA_ARGS__); } while(0)

#define LOG_MODEM(level, fmt, ...) \
    do { if (ultra::shouldLog(ultra::LogLevel::level, "MODEM")) LOG_##level("MODEM", fmt, ##__VA_ARGS__); } while(0)

#define LOG_LDPC(level, fmt, ...) \
    do { if (ultra::shouldLog(ultra::LogLevel::level, "LDPC")) LOG_##level("LDPC", fmt, ##__VA_ARGS__); } while(0)

#define LOG_SYNC(level, fmt, ...) \
    do { if (ultra::shouldLog(ultra::LogLevel::level, "SYNC")) LOG_##level("SYNC", fmt, ##__VA_ARGS__); } while(0)

#define LOG_CHAN(level, fmt, ...) \
    do { if (ultra::shouldLog(ultra::LogLevel::level, "CHAN")) LOG_##level("CHAN", fmt, ##__VA_ARGS__); } while(0)

} // namespace ultra
