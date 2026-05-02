#pragma once

#include "tnc_events.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ultra::tnc {

class ModemAdapter {
public:
    virtual ~ModemAdapter() = default;

    virtual void setMyCall(const std::vector<std::string>& calls) = 0;
    virtual void setBandwidth(int hz) = 0;
    virtual void setListen(bool on) = 0;
    virtual void startConnect(const std::string& src, const std::string& dst) = 0;
    virtual void disconnect() = 0;
    virtual void abort() = 0;
    virtual void sendBinary(const std::vector<uint8_t>& bytes) = 0;

    virtual int getTxBackloggBytes() const = 0;
    virtual int getCurrentSNR_db() const = 0;
    virtual int getCurrentBitrate_bps() const = 0;
    virtual State getState() const { return State::IDLE; }
};

} // namespace ultra::tnc
