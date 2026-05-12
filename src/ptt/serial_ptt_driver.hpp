#pragma once

#include "ptt_driver.hpp"
#include "gui/serial_ptt.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace ultra::ptt {

class ISerialPttBackend {
public:
    virtual ~ISerialPttBackend() = default;

    virtual bool open(const std::string& port_name, int baud_rate) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool matches(const std::string& port_name, int baud_rate) const = 0;
    virtual bool setLine(gui::SerialPttLine line, bool asserted) = 0;
};

class SerialPttControllerBackend final : public ISerialPttBackend {
public:
    bool open(const std::string& port_name, int baud_rate) override;
    void close() override;
    bool isOpen() const override;
    bool matches(const std::string& port_name, int baud_rate) const override;
    bool setLine(gui::SerialPttLine line, bool asserted) override;

private:
    gui::SerialPttController controller_;
};

class SerialPttDriver final : public IPttDriver {
public:
    explicit SerialPttDriver(PttConfig config);
    SerialPttDriver(PttConfig config, std::unique_ptr<ISerialPttBackend> backend);
    ~SerialPttDriver() override;

    bool open() override;
    void close() override;
    bool isOpen() const override;
    bool setKey(PttKey state) override;
    bool testCycle() override;
    std::string lastError() const override;

private:
    gui::SerialPttLine configuredLine() const;
    bool lineStateFor(PttKey state) const;
    void setLastError(std::string error);

    PttConfig config_;
    std::unique_ptr<ISerialPttBackend> backend_;
    mutable std::mutex mutex_;
    std::string last_error_;
};

} // namespace ultra::ptt
