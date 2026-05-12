#pragma once

#include "ptt_driver.hpp"
#include "tnc/socket_compat.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ultra::ptt {

class CatPttDriver final : public IPttDriver {
public:
    explicit CatPttDriver(PttConfig config);
    ~CatPttDriver() override;

    bool open() override;
    void close() override;
    bool isOpen() const override;
    bool setKey(PttKey state) override;
    bool testCycle() override;
    std::string lastError() const override;
    bool startTelemetry() override;
    std::optional<int64_t> currentFrequencyHz() const override;
    RadioFrequencyState radioFrequencyState() const override;

private:
    struct Command {
        PttKey state = PttKey::Off;
        std::shared_ptr<std::promise<bool>> completion;
    };

    struct ConnectResult {
        ultra::tnc::socket_t socket = ultra::tnc::kInvalidSocket;
        std::string error;
    };

    bool enqueue(PttKey state, std::shared_ptr<std::promise<bool>> completion);
    bool validateConfigLocked();
    void workerLoop();
    void handleCommand(Command cmd);
    void pollFrequencyOnce();
    std::chrono::milliseconds frequencyPollInterval() const;
    bool waitBeforeReconnect();
    void drainQueued(bool ok);

    ConnectResult connectSocket() const;
    bool sendPttCommand(PttKey state, std::string& error);
    bool readFrequency(int64_t& frequency_hz, bool& transport_ok, std::string& error);
    bool sendAll(const std::string& command, std::chrono::steady_clock::time_point deadline,
                 std::string& error);
    bool readLine(std::string& line, std::chrono::steady_clock::time_point deadline,
                  std::string& error);
    bool waitForSocket(short events, std::chrono::steady_clock::time_point deadline,
                       std::string& error) const;
    void closeCurrentSocket();

    void setLastError(std::string error);
    void setFrequency(int64_t frequency_hz);
    void markFrequencyStale();

    PttConfig config_;
    mutable std::mutex mutex_;
    mutable std::mutex frequency_mutex_;
    std::condition_variable cv_;
    std::deque<Command> queue_;
    std::thread worker_;
    bool running_ = false;
    std::string last_error_;
    int reconnect_delay_ms_ = 1000;

    ultra::tnc::socket_t socket_ = ultra::tnc::kInvalidSocket;
    std::atomic<bool> connected_{false};
    std::optional<int64_t> frequency_hz_;
    bool frequency_stale_ = false;
    ultra::tnc::WinsockInit winsock_;
};

} // namespace ultra::ptt
