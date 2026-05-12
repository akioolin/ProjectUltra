#pragma once

#include "ptt_driver.hpp"
#include "tnc/socket_compat.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
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
    void workerLoop();
    bool waitBeforeReconnect();
    void drainQueued(bool ok);

    ConnectResult connectSocket() const;
    bool sendPttCommand(PttKey state, std::string& error);
    bool sendAll(const std::string& command, std::chrono::steady_clock::time_point deadline,
                 std::string& error);
    bool readLine(std::string& line, std::chrono::steady_clock::time_point deadline,
                  std::string& error);
    bool waitForSocket(short events, std::chrono::steady_clock::time_point deadline,
                       std::string& error) const;
    void closeCurrentSocket();

    void setLastError(std::string error);

    PttConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Command> queue_;
    std::thread worker_;
    bool running_ = false;
    std::string last_error_;
    int reconnect_delay_ms_ = 1000;

    ultra::tnc::socket_t socket_ = ultra::tnc::kInvalidSocket;
    std::atomic<bool> connected_{false};
    ultra::tnc::WinsockInit winsock_;
};

} // namespace ultra::ptt
