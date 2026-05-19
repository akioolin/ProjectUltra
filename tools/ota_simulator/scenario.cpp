#include "ota_simulator/scenario.hpp"

#include "replay/json_util.hpp"
#include "sim/cli_enums.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace ultra::tools::ota {
namespace {

namespace json = ultra::replay::json;
namespace cli = ultra::tools::cli;

std::string readTextFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open scenario: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string requireObject(const std::string& object,
                          const std::string& key,
                          const std::string& context) {
    auto value = json::rawObjectValue(object, key);
    if (!value) {
        throw std::runtime_error(context + " missing object '" + key + "'");
    }
    return *value;
}

std::string requireString(const std::string& object,
                          const std::string& key,
                          const std::string& context) {
    auto value = json::stringValue(object, key);
    if (!value) {
        throw std::runtime_error(context + " missing string '" + key + "'");
    }
    return *value;
}

double requireNumber(const std::string& object,
                     const std::string& key,
                     const std::string& context) {
    auto value = json::numberValue(object, key);
    if (!value) {
        throw std::runtime_error(context + " missing number '" + key + "'");
    }
    return *value;
}

bool requireBool(const std::string& object,
                 const std::string& key,
                 const std::string& context) {
    auto value = json::boolValue(object, key);
    if (!value) {
        throw std::runtime_error(context + " missing bool '" + key + "'");
    }
    return *value;
}

size_t requireSize(const std::string& object,
                   const std::string& key,
                   const std::string& context) {
    auto value = json::numberValue(object, key);
    if (!value) {
        throw std::runtime_error(context + " missing number '" + key + "'");
    }
    if (*value < 0.0 || std::floor(*value) != *value ||
        *value > static_cast<double>(std::numeric_limits<size_t>::max())) {
        throw std::runtime_error(context + " '" + key + "' must be a non-negative integer");
    }
    return static_cast<size_t>(*value);
}

std::string objectAt(const std::string& text, size_t object_start,
                     const std::string& context) {
    if (object_start >= text.size() || text[object_start] != '{') {
        throw std::runtime_error(context + " expected object value");
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = object_start; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return text.substr(object_start, i - object_start + 1);
            }
            if (depth < 0) {
                throw std::runtime_error(context + " has mismatched braces");
            }
        }
    }
    throw std::runtime_error(context + " is not a complete JSON object");
}

std::optional<std::string> rawArrayValue(const std::string& object,
                                         const std::string& key) {
    const auto value = json::findValue(object, key);
    if (!value || *value >= object.size() || object[*value] != '[') {
        return std::nullopt;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = *value; i < object.size(); ++i) {
        const char c = object[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '[') {
            ++depth;
        } else if (c == ']') {
            --depth;
            if (depth == 0) {
                return object.substr(*value, i - *value + 1);
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string> splitObjectArray(const std::string& array_text,
                                          const std::string& context) {
    std::vector<std::string> out;
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    size_t start = std::string::npos;

    for (size_t i = 0; i < array_text.size(); ++i) {
        const char c = array_text[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            if (depth == 0) {
                start = i;
            }
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth < 0) {
                throw std::runtime_error(context + " has mismatched braces");
            }
            if (depth == 0 && start != std::string::npos) {
                out.push_back(array_text.substr(start, i - start + 1));
                start = std::string::npos;
            }
        }
    }

    if (depth != 0 || in_string) {
        throw std::runtime_error(context + " is not a complete JSON object array");
    }
    return out;
}

std::map<std::string, std::string> splitObjectMap(const std::string& object_text,
                                                  const std::string& context) {
    std::map<std::string, std::string> out;
    if (object_text.empty() || object_text.front() != '{') {
        throw std::runtime_error(context + " must be a JSON object");
    }

    size_t pos = 1;
    while (pos < object_text.size()) {
        while (pos < object_text.size() &&
               std::isspace(static_cast<unsigned char>(object_text[pos]))) {
            ++pos;
        }
        if (pos < object_text.size() && object_text[pos] == '}') {
            return out;
        }
        if (pos < object_text.size() && object_text[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos >= object_text.size() || object_text[pos] != '"') {
            throw std::runtime_error(context + " expected string key");
        }

        std::string key;
        if (!json::parseStringAt(object_text, pos, &key)) {
            throw std::runtime_error(context + " has invalid string key");
        }
        bool escaped = false;
        ++pos;
        while (pos < object_text.size()) {
            const char c = object_text[pos++];
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                break;
            }
        }

        while (pos < object_text.size() &&
               std::isspace(static_cast<unsigned char>(object_text[pos]))) {
            ++pos;
        }
        if (pos >= object_text.size() || object_text[pos] != ':') {
            throw std::runtime_error(context + " expected ':' after key '" + key + "'");
        }
        ++pos;
        while (pos < object_text.size() &&
               std::isspace(static_cast<unsigned char>(object_text[pos]))) {
            ++pos;
        }

        std::string value = objectAt(object_text, pos,
                                     context + "['" + key + "']");
        if (!out.emplace(key, value).second) {
            throw std::runtime_error(context + " has duplicate key '" + key + "'");
        }
        pos += value.size();
    }

    throw std::runtime_error(context + " is not a complete JSON object map");
}

ScenarioEvent::Type parseEventType(const std::string& value) {
    const std::string v = json::lowerCopy(json::trim(value));
    if (v == "inject_audio") return ScenarioEvent::Type::InjectAudio;
    if (v == "command") return ScenarioEvent::Type::Command;
    if (v == "assert") return ScenarioEvent::Type::Assert;
    if (v == "wait") return ScenarioEvent::Type::Wait;
    throw std::runtime_error("unsupported event type '" + value + "'");
}

InitialMode parseInitialMode(const std::string& object) {
    InitialMode mode;
    mode.waveform = cli::requireWaveformMode(
        requireString(object, "waveform", "endpoint.initial_mode"));
    mode.modulation = cli::requireModulation(
        requireString(object, "modulation", "endpoint.initial_mode"));
    mode.code_rate = cli::requireCodeRate(
        requireString(object, "code_rate", "endpoint.initial_mode"));
    return mode;
}

EndpointConfig parseEndpoint(const std::string& object) {
    EndpointConfig endpoint;
    endpoint.callsign = requireString(object, "callsign", "endpoint");
    endpoint.peer_callsign = requireString(object, "peer_callsign", "endpoint");
    endpoint.initial_state = parseConnectionStateStrict(
        requireString(object, "initial_state", "endpoint"));
    endpoint.initial_mode = parseInitialMode(
        requireObject(object, "initial_mode", "endpoint"));
    if (auto force_data_mode = json::boolValue(object, "force_data_mode")) {
        endpoint.force_data_mode = *force_data_mode;
    }
    if (auto auto_accept = json::boolValue(object, "auto_accept")) {
        endpoint.auto_accept = *auto_accept;
    }
    return endpoint;
}

NoiseBedConfig parseNoiseBed(const std::string& object) {
    NoiseBedConfig bed;
    bed.file = requireString(object, "file", "noise_bed");
    bed.loop = requireBool(object, "loop", "noise_bed");
    bed.target_rms = requireNumber(object, "target_rms", "noise_bed");
    if (bed.target_rms < 0.0) {
        throw std::runtime_error("noise_bed.target_rms must be non-negative");
    }
    return bed;
}

ChannelConfig parseChannel(const std::string& object) {
    ChannelConfig channel;
    if (auto bed = json::rawObjectValue(object, "noise_bed")) {
        channel.noise_bed = parseNoiseBed(*bed);
    }
    if (auto snr = json::numberValue(object, "snr_db")) {
        channel.snr_db = *snr;
    }
    if (auto seed = json::numberValue(object, "seed")) {
        if (*seed < 0.0 || std::floor(*seed) != *seed ||
            *seed > static_cast<double>(std::numeric_limits<uint64_t>::max())) {
            throw std::runtime_error("channel.seed must be a non-negative integer");
        }
        channel.seed = static_cast<uint64_t>(*seed);
    }
    if (auto fading = json::stringValue(object, "fading")) {
        channel.channel_type = cli::requireChannelType(*fading);
    }
    if (auto type = json::stringValue(object, "type")) {
        channel.channel_type = cli::requireChannelType(*type);
    }
    if (auto waveform = json::stringValue(object, "force_connected_waveform")) {
        channel.force_connected_waveform =
            cli::requireWaveformMode(*waveform);
        if (*channel.force_connected_waveform == protocol::WaveformMode::AUTO) {
            throw std::runtime_error("channel.force_connected_waveform cannot be AUTO");
        }
    }
    return channel;
}

TxFrameWithinAssert parseTxFrameWithin(const std::string& object) {
    TxFrameWithinAssert out;
    out.frame_type = json::upperCopy(requireString(object, "frame_type",
                                                   "assert.tx_frame_within"));
    out.since_t_s = requireNumber(object, "since_t_s", "assert.tx_frame_within");
    out.max_age_s = requireNumber(object, "max_age_s", "assert.tx_frame_within");
    if (out.since_t_s < 0.0 || out.max_age_s < 0.0) {
        throw std::runtime_error("tx_frame_within times must be non-negative");
    }
    if (auto seq = json::intValue(object, "seq")) {
        out.seq = static_cast<int>(*seq);
    }
    return out;
}

ScenarioEvent parseEvent(const std::string& object, size_t index, int scenario_version) {
    const std::string context = "events[" + std::to_string(index) + "]";
    ScenarioEvent event;
    event.t_s = requireNumber(object, "t_s", context);
    if (event.t_s < 0.0) {
        throw std::runtime_error(context + ".t_s must be non-negative");
    }
    event.type = parseEventType(requireString(object, "type", context));

    if (scenario_version == 2 &&
        (event.type == ScenarioEvent::Type::Command ||
         event.type == ScenarioEvent::Type::Assert)) {
        event.endpoint = requireString(object, "endpoint", context);
    } else if (auto endpoint = json::stringValue(object, "endpoint")) {
        event.endpoint = *endpoint;
    }

    if (event.type == ScenarioEvent::Type::InjectAudio) {
        event.file = requireString(object, "file", context);
        event.gain_db = requireNumber(object, "gain_db", context);
    } else if (event.type == ScenarioEvent::Type::Command) {
        if (scenario_version != 2) {
            throw std::runtime_error(context + " command is only supported by scenario version 2");
        }
        event.action = json::lowerCopy(json::trim(
            requireString(object, "action", context)));
        if (event.action == "connect_to") {
            event.peer_callsign = requireString(object, "peer_callsign", context);
        } else if (event.action == "send_message") {
            event.text = requireString(object, "text", context);
        } else if (event.action == "send_file") {
            event.file_size_bytes = requireSize(object, "size_bytes", context);
            if (auto filename = json::stringValue(object, "filename")) {
                event.filename = *filename;
            }
        } else if (event.action == "disconnect") {
            // No extra fields.
        } else {
            throw std::runtime_error(context + " unsupported command action '" +
                                     event.action + "'");
        }
    } else if (event.type == ScenarioEvent::Type::Assert) {
        if (auto state = json::stringValue(object, "state")) {
            event.assert_state = parseConnectionStateStrict(*state);
        }
        if (auto tx = json::rawObjectValue(object, "tx_frame_within")) {
            event.assert_tx_frame_within = parseTxFrameWithin(*tx);
        }
        if (auto msg = json::stringValue(object, "received_message_contains")) {
            if (scenario_version != 2) {
                throw std::runtime_error(
                    context + " received_message_contains is only supported by scenario version 2");
            }
            event.assert_received_message_contains = *msg;
        }
        if (json::findValue(object, "received_file_size_at_least")) {
            if (scenario_version != 2) {
                throw std::runtime_error(
                    context + " received_file_size_at_least is only supported by scenario version 2");
            }
            event.assert_received_file_size_at_least =
                requireSize(object, "received_file_size_at_least", context);
        }
        if (json::findValue(object, "received_file_byte_exact")) {
            if (scenario_version != 2) {
                throw std::runtime_error(
                    context + " received_file_byte_exact is only supported by scenario version 2");
            }
            event.assert_received_file_byte_exact =
                requireBool(object, "received_file_byte_exact", context);
        }
        if (!event.assert_state && !event.assert_tx_frame_within &&
            !event.assert_received_message_contains &&
            !event.assert_received_file_size_at_least &&
            !event.assert_received_file_byte_exact) {
            throw std::runtime_error(
                context + " assert must specify state, tx_frame_within, "
                          "received_message_contains, received_file_size_at_least, "
                          "or received_file_byte_exact");
        }
    }

    return event;
}

OutputConfig parseOutputV1(const std::string& object) {
    OutputConfig output;
    output.tx_capture = requireString(object, "tx_capture", "output");
    output.session_log = requireString(object, "session_log", "output");
    return output;
}

OutputConfig parseOutputV2(const std::string& object) {
    OutputConfig output;
    output.alice_tx_capture = requireString(object, "alice_tx_capture", "output");
    output.bob_tx_capture = requireString(object, "bob_tx_capture", "output");
    if (auto path = json::stringValue(object, "alice_rx_capture")) {
        output.alice_rx_capture = *path;
    }
    if (auto path = json::stringValue(object, "bob_rx_capture")) {
        output.bob_rx_capture = *path;
    }
    output.session_log = requireString(object, "session_log", "output");
    return output;
}

void validateV2EndpointReferences(const Scenario& scenario) {
    for (const auto& event : scenario.events) {
        if (event.endpoint.empty()) {
            continue;
        }
        if (scenario.endpoints.find(event.endpoint) == scenario.endpoints.end()) {
            throw std::runtime_error(
                "events endpoint '" + event.endpoint + "' is not declared in endpoints");
        }
    }
}

}  // namespace

const char* eventTypeName(ScenarioEvent::Type type) {
    switch (type) {
        case ScenarioEvent::Type::InjectAudio: return "inject_audio";
        case ScenarioEvent::Type::Command: return "command";
        case ScenarioEvent::Type::Assert: return "assert";
        case ScenarioEvent::Type::Wait: return "wait";
        default: return "unknown";
    }
}

std::string connectionStateName(protocol::ConnectionState state) {
    return protocol::connectionStateToString(state);
}

protocol::ConnectionState parseConnectionStateStrict(const std::string& value) {
    std::string v = json::upperCopy(json::trim(value));
    std::replace(v.begin(), v.end(), '-', '_');
    if (v == "DISCONNECTED") return protocol::ConnectionState::DISCONNECTED;
    if (v == "PROBING") return protocol::ConnectionState::PROBING;
    if (v == "CONNECTING") return protocol::ConnectionState::CONNECTING;
    if (v == "CONNECTED") return protocol::ConnectionState::CONNECTED;
    throw std::runtime_error("unsupported connection state '" + value + "'");
}

Scenario loadScenario(const std::string& path) {
    const std::string text = json::trim(readTextFile(path));
    if (text.empty() || text.front() != '{') {
        throw std::runtime_error("scenario root must be a JSON object");
    }

    Scenario scenario;
    scenario.source_path = path;
    scenario.version = static_cast<int>(requireNumber(text, "version", "scenario"));
    if (scenario.version != 1 && scenario.version != 2) {
        throw std::runtime_error("unsupported scenario version " +
                                 std::to_string(scenario.version));
    }

    if (scenario.version == 1) {
        scenario.endpoint = parseEndpoint(requireObject(text, "endpoint", "scenario"));
    } else {
        auto endpoints = splitObjectMap(requireObject(text, "endpoints", "scenario"),
                                        "scenario.endpoints");
        if (endpoints.size() != 2) {
            throw std::runtime_error("scenario version 2 requires exactly two endpoints");
        }
        for (const auto& [name, object] : endpoints) {
            if (name.empty()) {
                throw std::runtime_error("scenario.endpoints contains an empty endpoint name");
            }
            scenario.endpoints.emplace(name, parseEndpoint(object));
        }
    }

    if (scenario.version == 1) {
        if (auto bed = json::rawObjectValue(text, "noise_bed")) {
            scenario.noise_bed = parseNoiseBed(*bed);
        }
    }
    if (scenario.version == 2) {
        if (auto channel = json::rawObjectValue(text, "channel")) {
            scenario.channel = parseChannel(*channel);
        }
    }

    scenario.duration_s = requireNumber(text, "duration_s", "scenario");
    if (scenario.duration_s <= 0.0) {
        throw std::runtime_error("scenario.duration_s must be positive");
    }

    auto events = rawArrayValue(text, "events");
    if (!events) {
        throw std::runtime_error("scenario missing events array");
    }
    auto event_objects = splitObjectArray(*events, "events");
    scenario.events.reserve(event_objects.size());
    double last_t = -1.0;
    for (size_t i = 0; i < event_objects.size(); ++i) {
        auto event = parseEvent(event_objects[i], i, scenario.version);
        if (event.t_s < last_t) {
            throw std::runtime_error("events must be monotonic by t_s");
        }
        last_t = event.t_s;
        scenario.events.push_back(std::move(event));
    }
    std::stable_sort(scenario.events.begin(), scenario.events.end(),
                     [](const ScenarioEvent& a, const ScenarioEvent& b) {
                         return a.t_s < b.t_s;
                     });

    if (auto output = json::rawObjectValue(text, "output")) {
        scenario.output = scenario.version == 1
            ? parseOutputV1(*output)
            : parseOutputV2(*output);
    } else if (scenario.version == 2) {
        scenario.output = OutputConfig{};
    }
    if (scenario.version == 2) {
        validateV2EndpointReferences(scenario);
    }
    return scenario;
}

}  // namespace ultra::tools::ota
