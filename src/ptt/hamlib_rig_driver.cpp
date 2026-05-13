#include "ptt/hamlib_rig_driver.hpp"

#include "diagnostics/diagnostics_recorder.hpp"
#include "ultra/logging.hpp"

#include <hamlib/rig.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace ultra::ptt {
namespace {

constexpr auto kOpenWait = std::chrono::seconds(8);
constexpr auto kCommandWait = std::chrono::seconds(3);
constexpr size_t kMaxQueueDepth = 4;

std::once_flag g_hamlib_init_once;

void initializeHamlib() {
    rig_set_debug(RIG_DEBUG_WARN);
    (void)rig_load_all_backends();
}

std::string hamlibError(int rc) {
    const char* text = rigerror(rc);
    if (text && text[0] != '\0') {
        return text;
    }
    return "Hamlib error " + std::to_string(rc);
}

int normalizedBaud(int baud) {
    return baud > 0 ? baud : 9600;
}

ptt_type_t hamlibPttType(HamlibPttMethod method) {
    switch (method) {
    case HamlibPttMethod::DTR:
        return RIG_PTT_SERIAL_DTR;
    case HamlibPttMethod::RTS:
        return RIG_PTT_SERIAL_RTS;
    case HamlibPttMethod::Cat:
        return RIG_PTT_RIG;
    case HamlibPttMethod::Vox:
    default:
        return RIG_PTT_NONE;
    }
}

void copyHamlibPath(char (&dst)[HAMLIB_FILPATHLEN], const std::string& value) {
    std::strncpy(dst, value.c_str(), HAMLIB_FILPATHLEN - 1);
    dst[HAMLIB_FILPATHLEN - 1] = '\0';
}

} // namespace

HamlibRigDriver::HamlibRigDriver(PttConfig config)
    : config_(std::move(config)) {}

HamlibRigDriver::~HamlibRigDriver() {
    close();
}

bool HamlibRigDriver::open() {
    auto completion = std::make_shared<std::promise<bool>>();
    std::future<bool> future = completion->get_future();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!startWorkerLocked()) {
            return false;
        }
    }

    if (!enqueue(Command{CommandType::Open, PttKey::Off, completion})) {
        return false;
    }

    if (future.wait_for(kOpenWait) != std::future_status::ready) {
        setLastError("Hamlib rig open timed out");
        return false;
    }
    return future.get();
}

bool HamlibRigDriver::startTelemetry() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!startWorkerLocked()) {
            return false;
        }
    }
    (void)enqueue(Command{CommandType::Open, PttKey::Off, {}});
    return true;
}

void HamlibRigDriver::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !worker_.joinable()) {
            return;
        }
        running_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (Command& command : queue_) {
        if (command.completion) {
            command.completion->set_value(false);
        }
    }
    queue_.clear();
}

bool HamlibRigDriver::isOpen() const {
    return connected_.load(std::memory_order_acquire);
}

bool HamlibRigDriver::setKey(PttKey state) {
    if (config_.hamlib_ptt_method == HamlibPttMethod::Vox) {
        return true;
    }
    return enqueue(Command{CommandType::SetKey, state, {}});
}

bool HamlibRigDriver::testCycle() {
    if (config_.hamlib_ptt_method == HamlibPttMethod::Vox) {
        setLastError("VOX PTT selected; no Hamlib PTT command is available to test");
        return false;
    }
    if (!isOpen() && !open()) {
        return false;
    }

    auto on_completion = std::make_shared<std::promise<bool>>();
    std::future<bool> on_future = on_completion->get_future();
    if (!enqueue(Command{CommandType::SetKey, PttKey::On, on_completion})) {
        return false;
    }
    if (on_future.wait_for(kCommandWait) != std::future_status::ready ||
        !on_future.get()) {
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto off_completion = std::make_shared<std::promise<bool>>();
    std::future<bool> off_future = off_completion->get_future();
    if (!enqueue(Command{CommandType::SetKey, PttKey::Off, off_completion})) {
        return false;
    }
    return off_future.wait_for(kCommandWait) == std::future_status::ready &&
           off_future.get();
}

bool HamlibRigDriver::testCat() {
    auto emit_result = [this](bool ok) {
        char fields[384];
        if (ok) {
            std::snprintf(fields, sizeof(fields),
                          "{\"backend\":\"hamlib_builtin\",\"result\":\"ok\"}");
        } else {
            std::snprintf(fields, sizeof(fields),
                          "{\"backend\":\"hamlib_builtin\",\"result\":\"fail\","
                          "\"error\":\"%s\"}",
                          ultra::diagnostics::jsonEscape(lastError()).c_str());
        }
        ultra::diagnostics::DiagnosticsRecorder::instance().emitText(
            "ptt", "ptt.test_cat", fields);
    };

    if (!isOpen() && !open()) {
        emit_result(false);
        return false;
    }

    auto completion = std::make_shared<std::promise<bool>>();
    std::future<bool> future = completion->get_future();
    if (!enqueue(Command{CommandType::ReadFrequency, PttKey::Off, completion})) {
        emit_result(false);
        return false;
    }
    if (future.wait_for(kCommandWait) != std::future_status::ready) {
        setLastError("Hamlib get_freq timed out");
        emit_result(false);
        return false;
    }
    const bool ok = future.get();
    emit_result(ok);
    return ok;
}

std::string HamlibRigDriver::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

std::optional<int64_t> HamlibRigDriver::currentFrequencyHz() const {
    if (!connected_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(frequency_mutex_);
    return frequency_hz_;
}

HamlibRigDriver::RadioFrequencyState HamlibRigDriver::radioFrequencyState() const {
    RadioFrequencyState state;
    state.connected = connected_.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(frequency_mutex_);
        state.hz = frequency_hz_;
        state.stale = frequency_stale_;
    }
    if (!state.connected && state.hz) {
        state.stale = true;
    }
    return state;
}

bool HamlibRigDriver::startWorkerLocked() {
    if (running_) {
        return true;
    }
    if (!validateConfigLocked()) {
        return false;
    }

    running_ = true;
    last_error_.clear();
    worker_ = std::thread(&HamlibRigDriver::workerLoop, this);
    return true;
}

bool HamlibRigDriver::enqueue(Command command) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            last_error_ = "Hamlib rig driver is not open";
            if (command.completion) {
                command.completion->set_value(false);
            }
            return false;
        }

        if (!command.completion && command.type == CommandType::SetKey) {
            queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                        [](const Command& queued) {
                                            return queued.type == CommandType::SetKey &&
                                                   !queued.completion;
                                        }),
                         queue_.end());
        }

        if (queue_.size() >= kMaxQueueDepth) {
            last_error_ = "Hamlib rig command queue is full";
            LOG_ERROR("OPERATOR", "PTT: %s", last_error_.c_str());
            if (command.completion) {
                command.completion->set_value(false);
            }
            return false;
        }

        queue_.push_back(std::move(command));
    }
    cv_.notify_one();
    return true;
}

bool HamlibRigDriver::validateConfigLocked() {
    if (config_.hamlib_model_id <= 0) {
        last_error_ = "Hamlib rig model is not selected";
        return false;
    }
    if (config_.hamlib_baud <= 0) {
        last_error_ = "Hamlib serial baud rate is invalid";
        return false;
    }
    return true;
}

void HamlibRigDriver::workerLoop() {
    auto next_frequency_poll = std::chrono::steady_clock::now();
    // Exponential backoff when ensureRigOpen() keeps failing — prevents
    // the worker from spinning a Hamlib retry storm every 1.5 s while a
    // misconfigured port (e.g. operator picked a rig but didn't enter a
    // serial port) keeps returning -6. Resets to the nominal poll
    // cadence as soon as a single open succeeds. Caps at 60 s.
    auto open_backoff = std::chrono::milliseconds(0);
    constexpr auto kBackoffStart = std::chrono::seconds(5);
    constexpr auto kBackoffMax = std::chrono::seconds(60);

    for (;;) {
        Command command;
        bool have_command = false;
        bool poll_frequency = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_until(lock, next_frequency_poll, [this] {
                return !running_ || !queue_.empty();
            });
            if (!running_) {
                break;
            }
            if (!queue_.empty()) {
                command = std::move(queue_.front());
                queue_.pop_front();
                have_command = true;
            } else if (std::chrono::steady_clock::now() >= next_frequency_poll) {
                poll_frequency = true;
                next_frequency_poll = std::chrono::steady_clock::now() + frequencyPollInterval();
            }
        }

        if (have_command) {
            handleCommand(std::move(command));
            if (rig_open_) {
                open_backoff = std::chrono::milliseconds(0);
            }
        } else if (poll_frequency) {
            if (ensureRigOpen()) {
                open_backoff = std::chrono::milliseconds(0);
                (void)readFrequencyOnce();
            } else {
                open_backoff = open_backoff.count() == 0
                    ? kBackoffStart
                    : std::min<std::chrono::milliseconds>(open_backoff * 2, kBackoffMax);
                next_frequency_poll = std::chrono::steady_clock::now() + open_backoff;
            }
        }
    }

    closeRig();
}

void HamlibRigDriver::handleCommand(Command command) {
    bool ok = false;
    switch (command.type) {
    case CommandType::Open:
        ok = ensureRigOpen();
        break;
    case CommandType::SetKey:
        ok = ensureRigOpen() && setRigPtt(command.state);
        break;
    case CommandType::ReadFrequency:
        ok = ensureRigOpen() && readFrequencyOnce();
        break;
    }

    if (command.completion) {
        command.completion->set_value(ok);
    }
}

bool HamlibRigDriver::ensureRigOpen() {
    if (rig_open_) {
        return true;
    }

    // Validate port path up front. Real rigs (model != 1) need a serial
    // port; if the operator hasn't configured one, Hamlib's rig_open()
    // descends into a 4-retry storm against an empty path before
    // returning -6 ("No such file or directory"). Fail fast with a
    // clear operator-facing error instead.
    if (config_.hamlib_model_id != 1 && config_.hamlib_rig_port.empty()) {
        setLastError(
            "Hamlib rig port not configured. Set the Serial Port in the "
            "Radio settings (e.g. /dev/cu.usbserial-FT001 or COM5).");
        connected_.store(false, std::memory_order_release);
        return false;
    }

    std::call_once(g_hamlib_init_once, initializeHamlib);

    if (!rig_) {
        rig_ = rig_init(static_cast<rig_model_t>(config_.hamlib_model_id));
        if (!rig_) {
            setLastError("Hamlib rig_init failed for model " +
                         std::to_string(config_.hamlib_model_id));
            connected_.store(false, std::memory_order_release);
            return false;
        }
    }

    copyHamlibPath(rig_->state.rigport.pathname, config_.hamlib_rig_port);
    rig_->state.rigport.parm.serial.rate = normalizedBaud(config_.hamlib_baud);

    const ptt_type_t ptt_type = hamlibPttType(config_.hamlib_ptt_method);
    rig_->state.pttport.type.ptt = ptt_type;
    // rig_state.ptt_type was added in Hamlib 4.7 as a convenience mirror
    // of state.pttport.type.ptt; older Linux distros (Ubuntu 24.04 ships
    // libhamlib 4.5/4.6) don't have it. The canonical assignment above
    // is what rig_open() reads; rig_state.ptt_type is derived internally
    // on versions that expose it.
    copyHamlibPath(rig_->state.pttport.pathname, config_.hamlib_rig_port);
    rig_->state.pttport.parm.serial.rate = normalizedBaud(config_.hamlib_baud);

    const int rc = rig_open(rig_);
    if (rc != RIG_OK) {
        setLastError("Hamlib rig_open failed: " + hamlibError(rc));
        {
            char fields[384];
            std::snprintf(fields, sizeof(fields),
                          "{\"backend\":\"hamlib_builtin\",\"op\":\"rig_open\","
                          "\"model_id\":%d,\"rc\":%d,\"error\":\"%s\"}",
                          config_.hamlib_model_id, rc,
                          ultra::diagnostics::jsonEscape(hamlibError(rc)).c_str());
            ultra::diagnostics::DiagnosticsRecorder::instance().emitText(
                "ptt", "ptt.open_failed", fields);
        }
        closeRig();
        return false;
    }

    rig_open_ = true;
    connected_.store(true, std::memory_order_release);
    clearLastError();
    LOG_INFO("OPERATOR", "PTT: Hamlib built-in opened model=%d port=%s baud=%d ptt=%s",
             config_.hamlib_model_id,
             config_.hamlib_rig_port.c_str(),
             normalizedBaud(config_.hamlib_baud),
             hamlibPttMethodName(config_.hamlib_ptt_method));
    {
        char fields[256];
        std::snprintf(fields, sizeof(fields),
                      "{\"backend\":\"hamlib_builtin\",\"model_id\":%d,"
                      "\"baud\":%d,\"ptt_method\":\"%s\"}",
                      config_.hamlib_model_id,
                      normalizedBaud(config_.hamlib_baud),
                      hamlibPttMethodName(config_.hamlib_ptt_method));
        ultra::diagnostics::DiagnosticsRecorder::instance().emitText(
            "ptt", "ptt.opened", fields);
    }
    return true;
}

bool HamlibRigDriver::setRigPtt(PttKey state) {
    if (config_.hamlib_ptt_method == HamlibPttMethod::Vox) {
        return true;
    }

    const int rc = rig_set_ptt(rig_, RIG_VFO_CURR,
                               state == PttKey::On ? RIG_PTT_ON : RIG_PTT_OFF);
    if (rc != RIG_OK) {
        setLastError("Hamlib set_ptt failed: " + hamlibError(rc));
        LOG_ERROR("OPERATOR", "PTT: %s", lastError().c_str());
        {
            char fields[384];
            std::snprintf(fields, sizeof(fields),
                          "{\"backend\":\"hamlib_builtin\",\"op\":\"set_ptt\","
                          "\"state\":\"%s\",\"rc\":%d,\"error\":\"%s\"}",
                          state == PttKey::On ? "on" : "off", rc,
                          ultra::diagnostics::jsonEscape(hamlibError(rc)).c_str());
            ultra::diagnostics::DiagnosticsRecorder::instance().emitText(
                "ptt", "ptt.error", fields);
        }
        return false;
    }

    clearLastError();
    {
        char fields[96];
        std::snprintf(fields, sizeof(fields),
                      "{\"backend\":\"hamlib_builtin\",\"state\":\"%s\"}",
                      state == PttKey::On ? "on" : "off");
        ultra::diagnostics::DiagnosticsRecorder::instance().emitText(
            "ptt", "ptt.keyed", fields);
    }
    return true;
}

bool HamlibRigDriver::readFrequencyOnce() {
    freq_t frequency = 0;
    const int rc = rig_get_freq(rig_, RIG_VFO_CURR, &frequency);
    if (rc != RIG_OK) {
        markFrequencyStale();
        setLastError("Hamlib get_freq failed: " + hamlibError(rc));
        return false;
    }

    setFrequency(static_cast<int64_t>(frequency));
    clearLastError();
    return true;
}

void HamlibRigDriver::closeRig() {
    if (rig_) {
        if (rig_open_) {
            (void)rig_set_ptt(rig_, RIG_VFO_CURR, RIG_PTT_OFF);
            rig_close(rig_);
        }
        rig_cleanup(rig_);
        rig_ = nullptr;
    }
    rig_open_ = false;
    connected_.store(false, std::memory_order_release);
    markFrequencyStale();
}

std::chrono::milliseconds HamlibRigDriver::frequencyPollInterval() const {
    if (config_.hamlib_frequency_poll_ms > 0) {
        return std::chrono::milliseconds(config_.hamlib_frequency_poll_ms);
    }
    return std::chrono::milliseconds(1500);
}

void HamlibRigDriver::setLastError(std::string error) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = std::move(error);
}

void HamlibRigDriver::clearLastError() {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_.clear();
}

void HamlibRigDriver::setFrequency(int64_t frequency_hz) {
    std::lock_guard<std::mutex> lock(frequency_mutex_);
    frequency_hz_ = frequency_hz;
    frequency_stale_ = false;
}

void HamlibRigDriver::markFrequencyStale() {
    std::lock_guard<std::mutex> lock(frequency_mutex_);
    if (frequency_hz_) {
        frequency_stale_ = true;
    }
}

} // namespace ultra::ptt
