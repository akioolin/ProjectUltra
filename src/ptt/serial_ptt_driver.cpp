#include "ptt/serial_ptt_driver.hpp"

#include "ultra/logging.hpp"

#include <chrono>
#include <utility>
#include <thread>

namespace ultra::ptt {

bool SerialPttControllerBackend::open(const std::string& port_name, int baud_rate) {
    return controller_.open(port_name, baud_rate);
}

void SerialPttControllerBackend::close() {
    controller_.close();
}

bool SerialPttControllerBackend::isOpen() const {
    return controller_.isOpen();
}

bool SerialPttControllerBackend::matches(const std::string& port_name, int baud_rate) const {
    return controller_.matches(port_name, baud_rate);
}

bool SerialPttControllerBackend::setLine(gui::SerialPttLine line, bool asserted) {
    return controller_.setLine(line, asserted);
}

SerialPttDriver::SerialPttDriver(PttConfig config)
    : SerialPttDriver(std::move(config), std::make_unique<SerialPttControllerBackend>()) {}

SerialPttDriver::SerialPttDriver(PttConfig config, std::unique_ptr<ISerialPttBackend> backend)
    : config_(std::move(config)),
      backend_(std::move(backend)) {}

SerialPttDriver::~SerialPttDriver() {
    close();
}

bool SerialPttDriver::open() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!backend_) {
        setLastError("serial PTT backend is not available");
        LOG_ERROR("OPERATOR", "PTT: %s", last_error_.c_str());
        return false;
    }

    if (config_.serial_port.empty()) {
        setLastError("serial PTT port is empty");
        LOG_ERROR("OPERATOR", "PTT: %s", last_error_.c_str());
        return false;
    }

    const int baud = (config_.serial_baud > 0) ? config_.serial_baud : 9600;
    if (!backend_->matches(config_.serial_port, baud)) {
        backend_->close();
        if (!backend_->open(config_.serial_port, baud)) {
            setLastError("failed to open serial PTT port '" + config_.serial_port + "'");
            LOG_ERROR("OPERATOR", "PTT: %s", last_error_.c_str());
            return false;
        }
    }

    if (!backend_->setLine(configuredLine(), config_.serial_inactive_high)) {
        setLastError("failed to initialize serial PTT line to inactive state");
        LOG_ERROR("OPERATOR", "PTT: %s", last_error_.c_str());
        backend_->close();
        return false;
    }

    last_error_.clear();
    return true;
}

void SerialPttDriver::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (backend_) {
        backend_->close();
    }
}

bool SerialPttDriver::isOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backend_ && backend_->isOpen();
}

bool SerialPttDriver::setKey(PttKey state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!backend_ || !backend_->isOpen()) {
        setLastError("serial PTT is not open");
        return false;
    }

    if (!backend_->setLine(configuredLine(), lineStateFor(state))) {
        setLastError(state == PttKey::On
                         ? "failed to assert serial PTT line"
                         : "failed to release serial PTT line");
        LOG_ERROR("OPERATOR", "PTT: %s", last_error_.c_str());
        return false;
    }

    last_error_.clear();
    return true;
}

bool SerialPttDriver::testCycle() {
    if (!isOpen() && !open()) {
        return false;
    }
    if (!setKey(PttKey::On)) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return setKey(PttKey::Off);
}

std::string SerialPttDriver::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

gui::SerialPttLine SerialPttDriver::configuredLine() const {
    return config_.serial_line == SerialLine::DTR
               ? gui::SerialPttLine::DTR
               : gui::SerialPttLine::RTS;
}

bool SerialPttDriver::lineStateFor(PttKey state) const {
    return state == PttKey::On ? !config_.serial_inactive_high
                               : config_.serial_inactive_high;
}

void SerialPttDriver::setLastError(std::string error) {
    last_error_ = std::move(error);
}

} // namespace ultra::ptt
