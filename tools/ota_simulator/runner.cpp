#include "ota_simulator/runner.hpp"

#include "io/wav_io.hpp"
#include "ota_simulator/scripted_audio_port.hpp"
#include "ota_simulator/session_log.hpp"
#include "psk/multi_carrier_dpsk.hpp"
#include "protocol/frame_v2.hpp"
#include "sim/simulated_station.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace ultra::tools::ota {
namespace {

namespace fs = std::filesystem;
namespace v2 = ultra::protocol::v2;

struct TxFrameRecord {
    double t_s = 0.0;
    std::string frame_type;
    int seq = -1;
};

std::string resolveInputPath(const std::string& scenario_path,
                             const std::string& path) {
    fs::path p(path);
    if (p.is_absolute() || fs::exists(p)) {
        return p.string();
    }
    fs::path base = fs::path(scenario_path).parent_path() / p;
    if (fs::exists(base)) {
        return base.string();
    }
    return path;
}

MultiCarrierDPSKConfig mcDpskConfigFor(Modulation modulation) {
    if (modulation == Modulation::DQPSK) {
        return mc_dpsk_presets::level8();
    }
    if (modulation == Modulation::DBPSK) {
        return mc_dpsk_presets::robust_mid();
    }
    throw std::runtime_error("ota_simulator v1 supports only DQPSK or DBPSK MC-DPSK initial_mode");
}

std::string frameTypeFromResult(const gui::DecodeResult& result,
                                const std::string& ping_like_tx_type) {
    if (result.is_ping) {
        return ping_like_tx_type;
    }
    if (!result.frame_data.empty()) {
        auto header = v2::parseHeader(result.frame_data);
        if (header.valid) {
            return v2::frameTypeToString(header.type);
        }
    }
    return result.success ? v2::frameTypeToString(result.frame_type) : "DECODE_FAIL";
}

int frameSeqFromResult(const gui::DecodeResult& result) {
    if (!result.frame_data.empty()) {
        auto header = v2::parseHeader(result.frame_data);
        if (header.valid) {
            return header.seq;
        }
    }
    return -1;
}

class TxMonitor {
public:
    TxMonitor(const InitialMode& mode, std::string ping_like_tx_type)
        : ping_like_tx_type_(std::move(ping_like_tx_type)) {
        decoder_.setLogPrefix("ota_simulator_tx");
        decoder_.setMCDPSKConfig(mcDpskConfigFor(mode.modulation));
        decoder_.setMode(protocol::WaveformMode::MC_DPSK, false);
        decoder_.setDataMode(mode.modulation, mode.code_rate);
    }

    ~TxMonitor() {
        decoder_.stop();
    }

    void feed(const std::vector<float>& samples, SessionLog& log,
              std::vector<TxFrameRecord>& frames) {
        if (samples.empty()) {
            return;
        }
        trackActivity(samples);
        decoder_.feedAudio(samples.data(), samples.size());
        samples_fed_ += samples.size();
        decoder_.processBuffer();
        while (decoder_.hasFrame()) {
            auto result = decoder_.getFrame();
            const std::string frame_type = frameTypeFromResult(result, ping_like_tx_type_);
            const int seq = frameSeqFromResult(result);
            uint64_t event_sample = samples_fed_;
            if (!activity_starts_.empty()) {
                event_sample = activity_starts_.front();
                activity_starts_.clear();
            }
            const double t_s = static_cast<double>(event_sample) /
                               static_cast<double>(ScriptedAudioPort::kSampleRate);
            frames.push_back(TxFrameRecord{t_s, frame_type, seq});
            log.writeTxFrame(t_s, frame_type, seq, result);
        }
    }

private:
    void trackActivity(const std::vector<float>& samples) {
        constexpr float kActivityRms = 0.006f;
        double sum_sq = 0.0;
        for (float s : samples) {
            sum_sq += static_cast<double>(s) * static_cast<double>(s);
        }
        const float rms = samples.empty()
            ? 0.0f
            : static_cast<float>(std::sqrt(sum_sq / static_cast<double>(samples.size())));
        if (rms >= kActivityRms && !tx_active_) {
            activity_starts_.push_back(samples_fed_);
            tx_active_ = true;
        } else if (rms < kActivityRms) {
            tx_active_ = false;
        }
    }

    gui::StreamingDecoder decoder_;
    std::string ping_like_tx_type_;
    uint64_t samples_fed_ = 0;
    bool tx_active_ = false;
    std::deque<uint64_t> activity_starts_;
};

bool frameMatches(const TxFrameRecord& frame, const TxFrameWithinAssert& assertion) {
    if (frame.frame_type != assertion.frame_type) {
        return false;
    }
    if (assertion.seq && frame.seq != *assertion.seq) {
        return false;
    }
    const double end_t = assertion.since_t_s + assertion.max_age_s;
    return frame.t_s >= assertion.since_t_s && frame.t_s <= end_t;
}

std::string describeTxAssert(const TxFrameWithinAssert& assertion) {
    std::ostringstream oss;
    oss << "tx_frame_within frame_type=" << assertion.frame_type
        << " window=[" << assertion.since_t_s << ","
        << (assertion.since_t_s + assertion.max_age_s) << "]";
    if (assertion.seq) {
        oss << " seq=" << *assertion.seq;
    }
    return oss.str();
}

bool evaluateAssert(const ScenarioEvent& event,
                    const SimulatedStation& station,
                    const std::vector<TxFrameRecord>& tx_frames,
                    SessionLog& log,
                    bool check_state,
                    bool check_tx) {
    bool ok = true;
    if (check_state && event.assert_state) {
        const auto actual = station.getConnectionState();
        ok = ok && (actual == *event.assert_state);
        std::ostringstream desc;
        desc << "state expected=" << connectionStateName(*event.assert_state)
             << " actual=" << connectionStateName(actual);
        log.writeAssert(event.t_s, actual == *event.assert_state, desc.str());
    }

    if (check_tx && event.assert_tx_frame_within) {
        const auto& assertion = *event.assert_tx_frame_within;
        const bool found = std::any_of(tx_frames.begin(), tx_frames.end(),
                                       [&](const TxFrameRecord& frame) {
                                           return frameMatches(frame, assertion);
                                       });
        ok = ok && found;
        log.writeAssert(event.t_s, found, describeTxAssert(assertion));
    }
    return ok;
}

void validateScenarioSupport(const Scenario& scenario) {
    if (scenario.endpoint.initial_state == protocol::ConnectionState::CONNECTING ||
        scenario.endpoint.initial_state == protocol::ConnectionState::CONNECTED) {
        throw std::runtime_error(
            "ota_simulator v1 does not support initial_state " +
            connectionStateName(scenario.endpoint.initial_state));
    }
    if (scenario.endpoint.initial_mode.waveform != protocol::WaveformMode::MC_DPSK) {
        throw std::runtime_error("ota_simulator v1 supports only MC_DPSK initial_mode");
    }
    (void)mcDpskConfigFor(scenario.endpoint.initial_mode.modulation);
}

}  // namespace

int runScenario(const Scenario& scenario) {
    validateScenarioSupport(scenario);

    auto port = std::make_unique<ScriptedAudioPort>();
    ScriptedAudioPort* port_ptr = port.get();
    port_ptr->reserveTxSamples(static_cast<size_t>(
        std::ceil(scenario.duration_s * ScriptedAudioPort::kSampleRate)) +
        ScriptedAudioPort::kSampleRate);

    if (scenario.noise_bed) {
        const std::string path = resolveInputPath(scenario.source_path, scenario.noise_bed->file);
        auto wav = io::loadWavMono48k(path);
        port_ptr->setNoiseBed(std::move(wav.samples_48k),
                              scenario.noise_bed->loop,
                              scenario.noise_bed->target_rms);
    }

    for (const auto& event : scenario.events) {
        if (event.type != ScenarioEvent::Type::InjectAudio) {
            continue;
        }
        const std::string path = resolveInputPath(scenario.source_path, event.file);
        auto wav = io::loadWavMono48k(path);
        port_ptr->scheduleInject(event.t_s, std::move(wav.samples_48k), event.gain_db);
    }

    SessionLog log(scenario.output.session_log);
    std::vector<TxFrameRecord> tx_frames;
    std::vector<ScenarioEvent> pending_tx_asserts;
    size_t tx_cursor = 0;
    int assertion_failures = 0;

    const auto mc_config = mcDpskConfigFor(scenario.endpoint.initial_mode.modulation);
    SimulatedStation station(scenario.endpoint.callsign, std::move(port),
                             OFDMConfigPreset::Default, mc_config);
    station.setForcedModulation(scenario.endpoint.initial_mode.modulation);
    station.setForcedCodeRate(scenario.endpoint.initial_mode.code_rate);
    station.setRxDecodeResultCallback([&](const gui::DecodeResult& result) {
        log.writeRxFrame(station.getSimTime(), result);
    });

    const std::string ping_like_tx_type =
        scenario.endpoint.initial_state == protocol::ConnectionState::PROBING
            ? "PING"
            : "PONG";
    TxMonitor tx_monitor(scenario.endpoint.initial_mode, ping_like_tx_type);

    station.start();
    if (scenario.endpoint.initial_state == protocol::ConnectionState::PROBING) {
        station.connect(scenario.endpoint.peer_callsign);
    }

    auto last_state = station.getConnectionState();
    log.writeState(0.0, last_state);

    const auto start = std::chrono::steady_clock::now();
    size_t event_index = 0;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed =
            std::chrono::duration<double>(now - start).count();
        if (elapsed >= scenario.duration_s) {
            break;
        }

        station.tick();
        auto new_tx = port_ptr->capturedTxSince(tx_cursor);
        tx_monitor.feed(new_tx, log, tx_frames);

        const auto state = station.getConnectionState();
        if (state != last_state) {
            log.writeState(elapsed, state);
            last_state = state;
        }

        while (event_index < scenario.events.size() &&
               scenario.events[event_index].t_s <= elapsed) {
            const auto& event = scenario.events[event_index];
            if (event.type == ScenarioEvent::Type::InjectAudio) {
                log.writeInject(event.t_s, event.file, event.gain_db);
            } else if (event.type == ScenarioEvent::Type::Wait) {
                log.writeNote(event.t_s, "wait", "{}");
            } else if (event.type == ScenarioEvent::Type::Assert) {
                if (!evaluateAssert(event, station, tx_frames, log, true, false)) {
                    ++assertion_failures;
                }
                if (event.assert_tx_frame_within) {
                    pending_tx_asserts.push_back(event);
                }
            }
            ++event_index;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    station.stop();
    auto new_tx = port_ptr->capturedTxSince(tx_cursor);
    tx_monitor.feed(new_tx, log, tx_frames);

    while (event_index < scenario.events.size()) {
        const auto& event = scenario.events[event_index];
        if (event.type == ScenarioEvent::Type::Assert) {
            if (!evaluateAssert(event, station, tx_frames, log, true, false)) {
                ++assertion_failures;
            }
            if (event.assert_tx_frame_within) {
                pending_tx_asserts.push_back(event);
            }
        } else if (event.type == ScenarioEvent::Type::InjectAudio) {
            log.writeInject(event.t_s, event.file, event.gain_db);
        } else {
            log.writeNote(event.t_s, "wait", "{}");
        }
        ++event_index;
    }

    for (const auto& event : pending_tx_asserts) {
        if (!evaluateAssert(event, station, tx_frames, log, false, true)) {
            ++assertion_failures;
        }
    }

    if (!io::writeWavF32Mono(scenario.output.tx_capture, port_ptr->capturedTx())) {
        throw std::runtime_error("failed to write TX capture: " + scenario.output.tx_capture);
    }

    if (assertion_failures != 0) {
        std::cerr << "ota_simulator: " << assertion_failures
                  << " assertion(s) failed; see " << scenario.output.session_log << "\n";
        return 1;
    }

    std::cout << "ota_simulator: scenario passed; wrote "
              << scenario.output.tx_capture << " and "
              << scenario.output.session_log << "\n";
    return 0;
}

}  // namespace ultra::tools::ota
