#pragma once

#include "tnc_events.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ultra::tnc {

// Snapshot of modem-side counters and current PHY config, returned by
// ModemAdapter::getStats(). Surfaced to operators via the STATS command.
// Keep this lean; deeper instrumentation belongs in cli_simulator's exit
// summary, not the live TNC API.
struct ModemStats {
    int frames_sent = 0;
    int frames_received = 0;
    int retransmissions = 0;
    int timeouts = 0;
    int failed = 0;             // exceeded max retries
    int out_of_order = 0;
    std::string code_rate;      // e.g. "R1_2", "AUTO"
    std::string modulation;     // e.g. "DQPSK"
    std::string waveform;       // e.g. "OFDM_CHIRP", "MC_DPSK"
    int snr_db = 0;
    std::string snr_source;      // e.g. "ofdm_broadband", "idle_in_band"
    int bitrate_bps = 0;
    int tx_backlog_bytes = 0;
};

class ModemAdapter {
public:
    virtual ~ModemAdapter() = default;

    virtual void setMyCall(const std::vector<std::string>& calls) = 0;
    virtual void setBandwidth(int hz) = 0;
    virtual void setListen(bool on) = 0;
    virtual void startConnect(const std::string& src, const std::string& dst) = 0;
    virtual void disconnect() = 0;
    virtual void abort() = 0;
    // Returns true if the bytes were accepted into the engine's TX
    // pipeline. False means the engine refused (queue full, not in
    // CONNECTED, etc.); the caller should keep its staging buffer and
    // retry on the next quiet period rather than dropping the bytes.
    virtual bool sendBinary(const std::vector<uint8_t>& bytes) = 0;

    virtual int getTxBackloggBytes() const = 0;
    virtual int getCurrentSNR_db() const = 0;
    virtual int getCurrentBitrate_bps() const = 0;
    virtual State getState() const { return State::IDLE; }
    virtual ModemStats getStats() const { return {}; }
};

} // namespace ultra::tnc
