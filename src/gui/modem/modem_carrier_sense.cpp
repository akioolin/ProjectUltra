// modem_carrier_sense.cpp - Half-duplex turnaround timing for ModemEngine
//
// Carrier sense (listen-before-talk) is owned by the shared ChannelBusyDetector
// (src/audio/channel_busy_detector.cpp), reached by simulator/TNC stations via
// AudioPort::isChannelIdleFor(). The old fixed-threshold ModemEngine energy
// detector was an unwired dead stub and was removed; only the half-duplex
// turnaround timing remains here.

#include "modem_engine.hpp"
#include <chrono>

namespace ultra {
namespace gui {

bool ModemEngine::isTurnaroundActive() const {
    if (turnaround_delay_ms_ == 0) return false;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_rx_complete_time_).count();
    return elapsed < turnaround_delay_ms_;
}

uint32_t ModemEngine::getTurnaroundRemaining() const {
    if (turnaround_delay_ms_ == 0) return 0;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_rx_complete_time_).count();
    if (elapsed >= turnaround_delay_ms_) return 0;
    return static_cast<uint32_t>(turnaround_delay_ms_ - elapsed);
}

} // namespace gui
} // namespace ultra
