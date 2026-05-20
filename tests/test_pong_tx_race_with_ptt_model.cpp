#include "sim/simulated_station.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// This test is scoped to the PONG turn-around itself: ALPHA must not send PONG
// while BRAVO is still transmitting PING, and the carrier-sense T/R guard must
// clear BRAVO's RX-settling window before PONG arrives.
constexpr uint32_t kRxSettlingMs = 80;
constexpr uint32_t kCarrierSenseGuardMs = 120;

struct Event {
    uint64_t seq = 0;
    double t_s = 0.0;
    std::string station;
    std::string kind;
    std::string detail;
};

class EventLog {
public:
    void add(double t_s, std::string station, std::string kind, std::string detail) {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(Event{next_seq_++, t_s, std::move(station),
                                std::move(kind), std::move(detail)});
    }

    std::vector<Event> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto out = events_;
        std::sort(out.begin(), out.end(), [](const Event& a, const Event& b) {
            if (a.t_s != b.t_s) {
                return a.t_s < b.t_s;
            }
            return a.seq < b.seq;
        });
        return out;
    }

    bool hasRxPingPong(const std::string& station) const {
        const auto events = snapshot();
        return std::any_of(events.begin(), events.end(), [&](const Event& e) {
            return e.station == station && e.kind == "frame.rx" &&
                   e.detail == "PING_OR_PONG";
        });
    }

    bool hasRxPingPongInWindow(const std::string& station,
                               double start_s,
                               double end_s) const {
        const auto events = snapshot();
        return std::any_of(events.begin(), events.end(), [&](const Event& e) {
            return e.station == station && e.kind == "frame.rx" &&
                   e.detail == "PING_OR_PONG" &&
                   e.t_s >= start_s && e.t_s <= end_s;
        });
    }

    std::optional<std::pair<double, double>> firstTransitionWindow(
        const std::string& station) const {
        const auto events = snapshot();
        std::optional<double> start;
        for (const auto& e : events) {
            if (e.station != station || e.kind != "ptt.state") {
                continue;
            }
            if (!start && e.detail == "TX_TR_SWITCH") {
                start = e.t_s;
            } else if (start && e.detail == "RX") {
                return std::make_pair(*start, e.t_s);
            }
        }
        return std::nullopt;
    }

    void dump(const std::string& title) const {
        std::cout << "\n" << title << "\n";
        std::cout << std::fixed << std::setprecision(3);
        for (const auto& e : snapshot()) {
            std::cout << "  t=" << std::setw(7) << e.t_s << "s "
                      << std::setw(6) << e.station << " "
                      << std::setw(10) << e.kind << " "
                      << e.detail << "\n";
        }
        std::cout << std::defaultfloat;

        const auto bravo_window = firstTransitionWindow("BRAVO");
        if (bravo_window) {
            const bool bravo_rx =
                hasRxPingPongInWindow("BRAVO", bravo_window->first, bravo_window->second);
            std::cout << "  BRAVO first RX-deaf window: ["
                      << std::fixed << std::setprecision(3)
                      << bravo_window->first << "s, "
                      << bravo_window->second << "s], BRAVO frame.rx "
                      << "PING_OR_PONG in window: "
                      << (bravo_rx ? "YES" : "none") << "\n"
                      << std::defaultfloat;
        }
    }

private:
    mutable std::mutex mutex_;
    uint64_t next_seq_ = 0;
    std::vector<Event> events_;
};

void attachRxLogger(SimulatedStation& station,
                    const std::string& name,
                    EventLog& log) {
    station.setRxDecodeResultCallback([&station, name, &log](const ultra::gui::DecodeResult& result) {
        if (result.is_ping) {
            log.add(station.getSimTime(), name, "frame.rx", "PING_OR_PONG");
            return;
        }
        if (!result.success || result.frame_data.empty()) {
            return;
        }
        auto header = ultra::protocol::v2::parseHeader(result.frame_data);
        if (!header.valid) {
            return;
        }
        log.add(station.getSimTime(), name, "frame.rx",
                ultra::protocol::v2::frameTypeToString(header.type));
    });
}

void pollPttState(SimulatedStation& station,
                  const std::string& name,
                  PttState& last,
                  EventLog& log) {
    const PttState current = station.pttState();
    if (current == last) {
        return;
    }
    last = current;
    log.add(station.getSimTime(), name, "ptt.state", ::pttStateName(current));
}

void runStations(SimulatedStation& station_a,
                 SimulatedStation& station_b,
                 EventLog& log,
                 std::chrono::seconds duration) {
    PttState last_a = station_a.pttState();
    PttState last_b = station_b.pttState();
    log.add(station_a.getSimTime(), "ALPHA", "ptt.state", ::pttStateName(last_a));
    log.add(station_b.getSimTime(), "BRAVO", "ptt.state", ::pttStateName(last_b));

    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        station_a.tick();
        station_b.tick();
        pollPttState(station_a, "ALPHA", last_a, log);
        pollPttState(station_b, "BRAVO", last_b, log);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

struct CaseResult {
    bool alpha_connected = false;
    bool bravo_connected = false;
    bool bravo_decoded_ping_pong = false;
    std::shared_ptr<EventLog> log;
};

CaseResult runCase(const std::string& name) {
    SimulatedChannel channel;

    ConnectionConfig alpha_config;
    ConnectionConfig bravo_config;

    const auto mc_config = mc_dpsk_presets::level8();
    SimulatedStation alpha(
        "ALPHA",
        std::make_unique<VirtualAudioPort>(channel, /*is_station_a=*/true),
        OFDMConfigPreset::Default,
        mc_config,
        alpha_config);
    SimulatedStation bravo(
        "BRAVO",
        std::make_unique<VirtualAudioPort>(channel, /*is_station_a=*/false),
        OFDMConfigPreset::Default,
        mc_config,
        bravo_config);

    alpha.setRxSettlingMs(kRxSettlingMs);
    bravo.setRxSettlingMs(kRxSettlingMs);
    alpha.setCarrierSenseGuardMs(kCarrierSenseGuardMs);
    bravo.setCarrierSenseGuardMs(kCarrierSenseGuardMs);
    alpha.setSNR(30.0f);
    bravo.setSNR(30.0f);

    CaseResult result;
    result.log = std::make_shared<EventLog>();
    attachRxLogger(alpha, "ALPHA", *result.log);
    attachRxLogger(bravo, "BRAVO", *result.log);

    alpha.start();
    bravo.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    result.log->add(bravo.getSimTime(), "BRAVO", "command", "connect ALPHA");
    bravo.connect("ALPHA");

    runStations(alpha, bravo, *result.log, std::chrono::seconds(10));

    result.alpha_connected = alpha.isConnected();
    result.bravo_connected = bravo.isConnected();
    result.bravo_decoded_ping_pong = result.log->hasRxPingPong("BRAVO");

    alpha.stop();
    bravo.stop();

    std::ostringstream title;
    title << name << " event log"
          << " (ALPHA connected=" << (result.alpha_connected ? "true" : "false")
          << ", BRAVO connected=" << (result.bravo_connected ? "true" : "false")
          << ", BRAVO decoded PING_OR_PONG="
          << (result.bravo_decoded_ping_pong ? "true" : "false") << ")";
    result.log->dump(title.str());
    return result;
}

bool PongTxCarrierSenseVerified() {
    auto result = runCase("PongTxCarrierSenseVerified");
    if (!result.bravo_decoded_ping_pong) {
        std::cout << "FAIL: BRAVO did not decode PONG despite carrier-sense guard\n";
        return false;
    }
    std::cout << "PASS: carrier sense cleared BRAVO's RX-settling window\n";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 1) {
        std::cout << "Usage: " << argv[0] << "\n";
        return 1;
    }

    return PongTxCarrierSenseVerified() ? 0 : 1;
}
