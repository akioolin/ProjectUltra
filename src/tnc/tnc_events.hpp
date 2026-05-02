#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ultra::tnc {

enum class State : uint8_t {
    IDLE,
    READY,
    LISTENING,
    CONNECTING,
    CONNECTED,
    DISCONNECTING
};

struct ConnectInfo {
    std::string source;
    std::string dest;
    int bandwidth_hz = 2300;
};

enum class TNCEventType : uint8_t {
    Pending,
    CancelPending,
    Connected,
    Disconnected,
    PTT,
    BufferLevel,
    SNR,
    Bitrate,
    IAmAlive,
    IncomingCall,
    DataReceived
};

struct TNCEvent {
    TNCEventType type = TNCEventType::Disconnected;
    ConnectInfo connect;
    std::string peer;
    std::vector<uint8_t> data;
    bool ptt_on = false;
    int bytes = 0;
    float snr_db = 0.0f;
    int bitrate_bps = 0;
};

const char* stateToString(State state);

} // namespace ultra::tnc
