#include "ptt/hamlib_rig_driver.hpp"
#include "ptt/hamlib_rig_list.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace ultra::ptt;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(cond, msg)                                      \
    do {                                                      \
        if (!(cond)) {                                        \
            std::cout << "FAIL: " << msg << "\n";             \
            ++failed;                                         \
            return;                                           \
        }                                                     \
    } while (0)

void pass(const char* name) {
    ++passed;
    std::cout << "PASS: " << name << "\n";
}

bool waitForFrequency(HamlibRigDriver& driver, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (driver.currentFrequencyHz().has_value()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void test_rig_list_contains_dummy() {
    const char* name = "HamlibRigList: enumerates dummy rig model";
    const auto* dummy = findHamlibRigByModelId(1);
    CHECK(dummy != nullptr, "Hamlib model 1 dummy rig should be enumerated");
    CHECK(!dummy->model_name.empty(), "dummy rig should have a displayable model name");
    pass(name);
}

void test_dummy_rig_open_freq_and_ptt() {
    const char* name = "HamlibRigDriver: dummy rig opens, polls frequency, and cycles CAT PTT";

    PttConfig config;
    config.mode = PttMode::HamlibBuiltin;
    config.hamlib_model_id = 1;
    // Hamlib 4.7.x interprets a non-empty port string for the dummy rig as
    // a TCP hostname. Leaving it blank skips that resolution path.
    config.hamlib_rig_port = "";
    config.hamlib_baud = 9600;
    config.hamlib_ptt_method = HamlibPttMethod::Cat;
    config.hamlib_frequency_poll_ms = 50;

    HamlibRigDriver driver(config);
    CHECK(driver.open(), std::string("open failed: ") + driver.lastError());
    CHECK(driver.isOpen(), "driver should report open after rig_open");
    CHECK(driver.testCat(), std::string("Test CAT failed: ") + driver.lastError());
    CHECK(waitForFrequency(driver, std::chrono::seconds(2)),
          "dummy rig should return a frequency");

    const auto start = std::chrono::steady_clock::now();
    CHECK(driver.setKey(PttKey::On), std::string("setKey(On) failed: ") + driver.lastError());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::milliseconds(100), "setKey(On) should not block caller");

    CHECK(driver.testCycle(), std::string("PTT test cycle failed: ") + driver.lastError());
    driver.close();
    CHECK(!driver.isOpen(), "driver should report closed after close");
    pass(name);
}

} // namespace

int main() {
    test_rig_list_contains_dummy();
    test_dummy_rig_open_freq_and_ptt();

    std::cout << "\nHamlib rig driver tests: " << passed << " passed, "
              << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
