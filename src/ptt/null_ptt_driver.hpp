#pragma once

#include "ptt_driver.hpp"

#include <chrono>
#include <string>
#include <thread>

namespace ultra::ptt {

class NullPttDriver final : public IPttDriver {
public:
    bool open() override { return true; }
    void close() override {}
    bool isOpen() const override { return true; }
    bool setKey(PttKey) override { return true; }

    bool testCycle() override {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return true;
    }

    std::string lastError() const override { return {}; }
};

} // namespace ultra::ptt
