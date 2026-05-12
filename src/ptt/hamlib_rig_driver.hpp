#pragma once

#include "ptt_driver.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

struct s_rig;

namespace ultra::ptt {

class HamlibRigDriver final : public IPttDriver {
public:
    explicit HamlibRigDriver(PttConfig config);
    ~HamlibRigDriver() override;

    bool open() override;
    void close() override;
    bool isOpen() const override;
    bool setKey(PttKey state) override;
    bool testCycle() override;
    std::string lastError() const override;
    bool startTelemetry() override;
    bool testCat() override;
    std::optional<int64_t> currentFrequencyHz() const override;
    RadioFrequencyState radioFrequencyState() const override;

private:
    enum class CommandType {
        Open,
        SetKey,
        ReadFrequency
    };

    struct Command {
        CommandType type = CommandType::SetKey;
        PttKey state = PttKey::Off;
        std::shared_ptr<std::promise<bool>> completion;
    };

    bool startWorkerLocked();
    bool enqueue(Command command);
    bool validateConfigLocked();
    void workerLoop();
    void handleCommand(Command command);
    bool ensureRigOpen();
    bool setRigPtt(PttKey state);
    bool readFrequencyOnce();
    void closeRig();
    std::chrono::milliseconds frequencyPollInterval() const;

    void setLastError(std::string error);
    void clearLastError();
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

    s_rig* rig_ = nullptr;
    bool rig_open_ = false;
    std::atomic<bool> connected_{false};
    std::optional<int64_t> frequency_hz_;
    bool frequency_stale_ = false;
};

} // namespace ultra::ptt
