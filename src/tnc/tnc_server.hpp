#pragma once

#include "tnc_session.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ultra::tnc {

struct TNCServerConfig {
    uint16_t cmd_port = 8300;
    uint16_t data_port = 8301;
    std::string bind_address = "127.0.0.1";
    uint32_t iamalive_interval_ms = 60000;
    uint32_t buffer_rate_limit_ms = 1000;
};

class TNCServer {
public:
    explicit TNCServer(ModemAdapter& modem, TNCServerConfig config = {});
    ~TNCServer();

    TNCServer(const TNCServer&) = delete;
    TNCServer& operator=(const TNCServer&) = delete;

    bool start();
    void stop();

    bool isRunning() const;
    uint16_t getCmdPort() const;
    uint16_t getDataPort() const;
    const TNCSession& getSession() const;

    void postModemConnected(const std::string& src, const std::string& dst, int bw);
    void postModemDisconnected();
    void postModemPTT(bool on);
    void postModemDataReceived(std::vector<uint8_t> bytes);
    void postModemBufferLevel(int bytes);
    void postModemSNR(float db);
    void postModemBitrate(int bps);
    void postModemIncomingCall(std::string peer);

private:
    enum class PollTarget : uint8_t {
        CmdListener,
        DataListener,
        CmdClient,
        DataClient,
        Wakeup
    };

    void reactorLoop();

    void onCmdListenerReady();
    void onDataListenerReady();
    void onCmdClientReady();
    void onDataClientReady();
    void onTimerTick(uint32_t elapsed_ms);

    void evictCmdClient();
    void evictDataClient();
    void emitToCmdClient(std::string_view line);
    void emitToDataClient(const std::vector<uint8_t>& bytes);

    bool openListeners();
    bool openWakeupPipe();
    void closeServerFds();
    void resetSession();

    void enqueueModemEvent(TNCEvent event);
    void wakeReactor();
    void drainWakeupPipe();
    void drainModemQueue();
    void dispatchModemEvent(const TNCEvent& event);

    void processControlBytes(const uint8_t* bytes, size_t size);
    void flushAllOutputs();
    bool flushClientOutput(int fd, std::vector<uint8_t>& buffer);

    ModemAdapter& modem_;
    TNCServerConfig config_;
    std::unique_ptr<TNCSession> session_;

    std::thread reactor_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<uint16_t> bound_cmd_port_{0};
    std::atomic<uint16_t> bound_data_port_{0};
    std::mutex lifecycle_mutex_;

    int cmd_listener_fd_ = -1;
    int data_listener_fd_ = -1;
    int cmd_client_fd_ = -1;
    int data_client_fd_ = -1;
    int wakeup_read_fd_ = -1;
    int wakeup_write_fd_ = -1;

    std::string cmd_line_buffer_;
    std::vector<uint8_t> cmd_tx_buffer_;
    std::vector<uint8_t> data_tx_buffer_;

    std::mutex queue_mutex_;
    std::deque<TNCEvent> modem_events_;

    uint32_t real_tick_accum_ms_ = 0;
    uint32_t iamalive_override_accum_ms_ = 0;
    uint32_t buffer_override_accum_ms_ = 0;
};

} // namespace ultra::tnc
