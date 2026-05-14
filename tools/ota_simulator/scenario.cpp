#include "ota_simulator/scenario.hpp"

#include "replay/json_util.hpp"
#include "sim/cli_enums.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
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

ScenarioEvent::Type parseEventType(const std::string& value) {
    const std::string v = json::lowerCopy(json::trim(value));
    if (v == "inject_audio") return ScenarioEvent::Type::InjectAudio;
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

ScenarioEvent parseEvent(const std::string& object, size_t index) {
    const std::string context = "events[" + std::to_string(index) + "]";
    ScenarioEvent event;
    event.t_s = requireNumber(object, "t_s", context);
    if (event.t_s < 0.0) {
        throw std::runtime_error(context + ".t_s must be non-negative");
    }
    event.type = parseEventType(requireString(object, "type", context));

    if (event.type == ScenarioEvent::Type::InjectAudio) {
        event.file = requireString(object, "file", context);
        event.gain_db = requireNumber(object, "gain_db", context);
    } else if (event.type == ScenarioEvent::Type::Assert) {
        if (auto state = json::stringValue(object, "state")) {
            event.assert_state = parseConnectionStateStrict(*state);
        }
        if (auto tx = json::rawObjectValue(object, "tx_frame_within")) {
            event.assert_tx_frame_within = parseTxFrameWithin(*tx);
        }
        if (!event.assert_state && !event.assert_tx_frame_within) {
            throw std::runtime_error(context + " assert must specify state or tx_frame_within");
        }
    }

    return event;
}

OutputConfig parseOutput(const std::string& object) {
    OutputConfig output;
    output.tx_capture = requireString(object, "tx_capture", "output");
    output.session_log = requireString(object, "session_log", "output");
    return output;
}

}  // namespace

const char* eventTypeName(ScenarioEvent::Type type) {
    switch (type) {
        case ScenarioEvent::Type::InjectAudio: return "inject_audio";
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
    if (scenario.version != 1) {
        throw std::runtime_error("unsupported scenario version " +
                                 std::to_string(scenario.version));
    }
    scenario.endpoint = parseEndpoint(requireObject(text, "endpoint", "scenario"));

    if (auto bed = json::rawObjectValue(text, "noise_bed")) {
        scenario.noise_bed = parseNoiseBed(*bed);
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
        auto event = parseEvent(event_objects[i], i);
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
        scenario.output = parseOutput(*output);
    }
    return scenario;
}

}  // namespace ultra::tools::ota
