#pragma once

#include "protocol/connection.hpp"
#include "protocol/frame_v2.hpp"

#include <optional>
#include <map>
#include <string>
#include <vector>

namespace ultra::tools::ota {

struct InitialMode {
    protocol::WaveformMode waveform = protocol::WaveformMode::MC_DPSK;
    Modulation modulation = Modulation::DQPSK;
    CodeRate code_rate = CodeRate::R1_4;
};

struct EndpointConfig {
    std::string callsign;
    std::string peer_callsign;
    protocol::ConnectionState initial_state = protocol::ConnectionState::DISCONNECTED;
    InitialMode initial_mode;
    bool auto_accept = false;
};

struct NoiseBedConfig {
    std::string file;
    bool loop = false;
    double target_rms = 0.0;
};

struct ChannelConfig {
    std::optional<NoiseBedConfig> noise_bed;
    std::optional<double> snr_db;
};

struct TxFrameWithinAssert {
    std::string frame_type;
    double since_t_s = 0.0;
    double max_age_s = 0.0;
    std::optional<int> seq;
};

struct ScenarioEvent {
    enum class Type {
        InjectAudio,
        Command,
        Assert,
        Wait,
    };

    Type type = Type::Wait;
    double t_s = 0.0;
    std::string endpoint;

    std::string file;
    double gain_db = 0.0;

    std::string action;
    std::string peer_callsign;
    std::string text;

    std::optional<protocol::ConnectionState> assert_state;
    std::optional<TxFrameWithinAssert> assert_tx_frame_within;
    std::optional<std::string> assert_received_message_contains;
};

struct OutputConfig {
    std::string tx_capture = "out_tx.wav";
    std::string alice_tx_capture = "out_alice_tx.wav";
    std::string bob_tx_capture = "out_bob_tx.wav";
    std::string session_log = "out_session.jsonl";
};

struct Scenario {
    int version = 1;
    std::string source_path;
    EndpointConfig endpoint;
    std::map<std::string, EndpointConfig> endpoints;
    std::optional<NoiseBedConfig> noise_bed;
    std::optional<ChannelConfig> channel;
    double duration_s = 0.0;
    std::vector<ScenarioEvent> events;
    OutputConfig output;
};

Scenario loadScenario(const std::string& path);

const char* eventTypeName(ScenarioEvent::Type type);
std::string connectionStateName(protocol::ConnectionState state);
protocol::ConnectionState parseConnectionStateStrict(const std::string& value);

}  // namespace ultra::tools::ota
