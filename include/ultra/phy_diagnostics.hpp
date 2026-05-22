#pragma once

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

namespace ultra {

inline std::mutex g_phy_diag_mutex;
inline std::string g_phy_diag_log_path;
inline std::string g_phy_diag_open_path;
inline std::ofstream g_phy_diag_stream;
inline const auto g_phy_diag_start_time = std::chrono::steady_clock::now();

inline bool phyDiagnosticsEnvConfigured() {
    const char* env_path = std::getenv("ULTRA_PHY_DIAG_LOG");
    return env_path && *env_path;
}

inline std::atomic<bool> g_phy_diag_enabled{phyDiagnosticsEnvConfigured()};

inline void setPhyDiagnosticsLogPath(std::string path) {
    std::lock_guard<std::mutex> lock(g_phy_diag_mutex);
    g_phy_diag_log_path = std::move(path);
    g_phy_diag_enabled.store(!g_phy_diag_log_path.empty() || phyDiagnosticsEnvConfigured(),
                             std::memory_order_release);
    if (g_phy_diag_stream.is_open()) {
        g_phy_diag_stream.close();
    }
    g_phy_diag_open_path.clear();
    if (!g_phy_diag_log_path.empty()) {
        g_phy_diag_stream.open(g_phy_diag_log_path, std::ios::out | std::ios::trunc);
        if (g_phy_diag_stream) {
            g_phy_diag_open_path = g_phy_diag_log_path;
            g_phy_diag_stream << "# ProjectUltra PHY diagnostics\n";
            g_phy_diag_stream.flush();
        }
    }
}

inline std::string phyDiagnosticsPathLocked() {
    if (!g_phy_diag_log_path.empty()) {
        return g_phy_diag_log_path;
    }
    const char* env_path = std::getenv("ULTRA_PHY_DIAG_LOG");
    return (env_path && *env_path) ? std::string(env_path) : std::string();
}

inline bool phyDiagnosticsEnabled() {
    return g_phy_diag_enabled.load(std::memory_order_acquire);
}

inline void phyDiagLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_phy_diag_mutex);
    const std::string path = phyDiagnosticsPathLocked();
    if (path.empty()) {
        return;
    }
    if (!g_phy_diag_stream.is_open() || g_phy_diag_open_path != path) {
        if (g_phy_diag_stream.is_open()) {
            g_phy_diag_stream.close();
        }
        g_phy_diag_stream.open(path, std::ios::out | std::ios::app);
        g_phy_diag_open_path = g_phy_diag_stream ? path : std::string();
    }
    if (!g_phy_diag_stream) {
        return;
    }

    const auto now_steady = std::chrono::steady_clock::now();
    const auto diag_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now_steady - g_phy_diag_start_time).count();
    const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    g_phy_diag_stream << "diag_ms=" << diag_ms
                      << " epoch_ms=" << epoch_ms
                      << ' ' << line << '\n';
}

}  // namespace ultra
