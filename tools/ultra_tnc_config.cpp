#include "ultra_tnc_config.hpp"

#include "protocol/frame_v2.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <ostream>
#include <vector>

namespace ultra::tnc::config {

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool isNoneDevice(const std::string& device) {
    return lower(device) == "none";
}

bool parsePositiveIntStrict(const std::string& text, int& out, int min_v, int max_v) {
    if (text.empty()) return false;
    if (text[0] == '-' || text[0] == '+') return false;
    try {
        size_t parsed = 0;
        long long value = std::stoll(text, &parsed, 10);
        if (parsed != text.size()) return false;
        if (value < min_v || value > max_v) return false;
        out = static_cast<int>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseBoolStrict(const std::string& text, bool& out) {
    std::string lc = lower(text);
    if (lc == "true" || lc == "1" || lc == "yes" || lc == "on") { out = true; return true; }
    if (lc == "false" || lc == "0" || lc == "no" || lc == "off") { out = false; return true; }
    return false;
}

bool parseUint16(const std::string& text, uint16_t& out) {
    try {
        size_t parsed = 0;
        int value = std::stoi(text, &parsed, 10);
        if (parsed != text.size() || value < 0 || value > 65535) {
            return false;
        }
        out = static_cast<uint16_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<float> parseFloat(const std::string& text) {
    try {
        size_t parsed = 0;
        float value = std::stof(text, &parsed);
        if (parsed != text.size()) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<ultra::CodeRate> parseCodeRate(const std::string& value) {
    const std::string v = lower(value);
    if (v == "auto") return ultra::CodeRate::AUTO;
    if (v == "r1_4") return ultra::CodeRate::R1_4;
    if (v == "r1_2") return ultra::CodeRate::R1_2;
    if (v == "r2_3") return ultra::CodeRate::R2_3;
    if (v == "r3_4") return ultra::CodeRate::R3_4;
    return std::nullopt;
}

std::optional<ultra::Modulation> parseModulation(const std::string& value) {
    const std::string v = lower(value);
    if (v == "auto") return ultra::Modulation::AUTO;
    if (v == "dqpsk") return ultra::Modulation::DQPSK;
    if (v == "d8psk") return ultra::Modulation::D8PSK;
    if (v == "dbpsk") return ultra::Modulation::DBPSK;
    if (v == "qpsk") return ultra::Modulation::QPSK;
    if (v == "bpsk") return ultra::Modulation::BPSK;
    if (v == "qam16") return ultra::Modulation::QAM16;
    if (v == "qam32") return ultra::Modulation::QAM32;
    if (v == "qam64") return ultra::Modulation::QAM64;
    return std::nullopt;
}

void printUsage(std::ostream& out) {
    out << "ultra_tnc [options]\n"
        << "  --config <path>             Read options from a config file\n"
        << "                              (key = value, # comments). CLI\n"
        << "                              flags override config-file values.\n"
        << "  --audio-output <name>       SDL audio output device, or none\n"
        << "  --audio-input <name>        SDL audio input device, or none\n"
        << "  --port <N>                  TNC command port (default: 8300; data=N+1)\n"
        << "  --bind <addr>               Bind address (default: 127.0.0.1)\n"
        << "  --callsign <call>           Default callsign (default: NOCALL)\n"
        << "  --inject-channel [awgn]     Apply simple TX-side AWGN before audio output\n"
        << "                              (only awgn is implemented; other types rejected)\n"
        << "  --no-inject-channel         Override config inject_channel=true back to false\n"
        << "  --snr <db>                  SNR for channel injection and mode reports\n"
        << "  --rate <auto|r1_4|r1_2|r2_3|r3_4>\n"
        << "  --mod <auto|dqpsk|d8psk|dbpsk|qpsk|bpsk|qam16|qam32|qam64>\n"
        << "  --ofdm-config <default|nvis>\n"
        << "  --ptt-serial-port <path>    Serial device for hardware PTT\n"
        << "                              (e.g. /dev/cu.usbserial or COM3)\n"
        << "  --ptt-serial-baud <N>       Serial baud (default: 9600; line\n"
        << "                              toggling works at any baud)\n"
        << "  --ptt-serial-line <rts|dtr> Which line keys TX (default: rts)\n"
        << "  --ptt-inactive-high         Some radios invert; default is\n"
        << "                              inactive=low / asserted=high\n"
        << "  --ptt-active-high           Override config ptt_inactive_high=true\n"
        << "                              back to default polarity (also: --no-ptt-inactive-high)\n"
        << "  --list-audio-devices        Print available SDL audio devices and exit\n"
        << "  --help\n"
        << "\n"
        << "Config file format (key=value, one per line, # comments):\n"
        << "  audio_output = USB Audio CODEC\n"
        << "  audio_input  = USB Audio CODEC\n"
        << "  callsign     = N0CALL\n"
        << "  port         = 8300\n"
        << "  ptt_serial_port = /dev/cu.usbserial-FT001\n"
        << "  ptt_serial_line = rts\n"
        << "\n"
        << "Default config search path: ./ultra_tnc.conf, then\n"
        << "  $XDG_CONFIG_HOME/ultra_tnc/config (or ~/.config/ultra_tnc/config).\n";
}

bool applyConfigKey(const std::string& key, const std::string& value, Config& cfg) {
    if (key == "audio_output" || key == "audio-output") {
        cfg.audio_output = value;
    } else if (key == "audio_input" || key == "audio-input") {
        cfg.audio_input = value;
    } else if (key == "port") {
        if (!parseUint16(value, cfg.port) ||
            cfg.port == std::numeric_limits<uint16_t>::max()) return false;
    } else if (key == "bind" || key == "bind_address") {
        cfg.bind_address = value;
    } else if (key == "callsign") {
        cfg.callsign = ultra::protocol::sanitizeCallsign(value);
        if (cfg.callsign.empty()) return false;
    } else if (key == "inject_channel" || key == "inject-channel") {
        if (parseBoolStrict(value, cfg.inject_channel)) {
            // Plain boolean, done.
        } else {
            // Channel-type values are accepted as a synonym for "true"
            // for forward-compat with cli_simulator's spelling, but
            // only AWGN is actually implemented here. Reject anything
            // else loudly rather than silently using AWGN.
            const std::string lc = lower(value);
            if (lc != "awgn") {
                std::cerr << "ultra_tnc only supports inject_channel=awgn|true|false; "
                             "got '" << value << "'\n";
                return false;
            }
            cfg.inject_channel = true;
            cfg.inject_channel_type = lc;
        }
    } else if (key == "snr" || key == "snr_db") {
        auto parsed = parseFloat(value);
        if (!parsed) return false;
        cfg.snr_db = *parsed;
    } else if (key == "rate") {
        auto parsed = parseCodeRate(value);
        if (!parsed) return false;
        cfg.forced_rate = *parsed;
    } else if (key == "mod" || key == "modulation") {
        auto parsed = parseModulation(value);
        if (!parsed) return false;
        cfg.forced_mod = *parsed;
    } else if (key == "ofdm_config" || key == "ofdm-config") {
        const std::string preset = lower(value);
        if (preset == "default") cfg.ofdm_config = OFDMConfigPreset::Default;
        else if (preset == "nvis") cfg.ofdm_config = OFDMConfigPreset::Nvis;
        else return false;
    } else if (key == "ptt_serial_port" || key == "ptt-serial-port") {
        cfg.ptt_serial_port = value;
    } else if (key == "ptt_serial_baud" || key == "ptt-serial-baud") {
        if (!parsePositiveIntStrict(value, cfg.ptt_serial_baud, 50, 4000000)) return false;
    } else if (key == "ptt_serial_line" || key == "ptt-serial-line") {
        const std::string line = lower(value);
        if (line != "rts" && line != "dtr") return false;
        cfg.ptt_serial_line = line;
    } else if (key == "ptt_inactive_high" || key == "ptt-inactive-high") {
        if (!parseBoolStrict(value, cfg.ptt_inactive_high)) return false;
    } else {
        std::cerr << "Unknown config key: " << key << "\n";
        return false;
    }
    return true;
}

bool loadConfigFile(const std::string& path, Config& cfg) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        auto trim = [](std::string& s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        };
        trim(line);
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            std::cerr << "Config " << path << ":" << line_no << ": missing '='\n";
            return false;
        }
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        trim(key); trim(value);
        if (key.empty()) continue;
        if (!applyConfigKey(key, value, cfg)) {
            std::cerr << "Config " << path << ":" << line_no << ": bad value for '"
                      << key << "': " << value << "\n";
            return false;
        }
    }
    return true;
}

std::string findDefaultConfigFile() {
    std::vector<std::string> candidates = {"ultra_tnc.conf"};
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        candidates.push_back(std::string(xdg) + "/ultra_tnc/config");
    }
    if (const char* home = std::getenv("HOME")) {
        candidates.push_back(std::string(home) + "/.config/ultra_tnc/config");
    }
    for (const auto& path : candidates) {
        std::ifstream in(path);
        if (in) return path;
    }
    return {};
}

bool parseArgs(int argc, char** argv, Config& cfg) {
    std::string explicit_config;
    bool needs_config = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h" || arg == "--list-audio-devices") {
            needs_config = false;
            break;
        }
    }
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            explicit_config = argv[i + 1];
            break;
        }
    }
    if (needs_config) {
        if (!explicit_config.empty()) {
            if (!loadConfigFile(explicit_config, cfg)) {
                std::cerr << "Failed to load --config " << explicit_config << "\n";
                return false;
            }
        } else {
            const std::string default_path = findDefaultConfigFile();
            if (!default_path.empty()) {
                if (!loadConfigFile(default_path, cfg)) {
                    std::cerr << "Failed to load default config " << default_path << "\n";
                    return false;
                }
                std::cout << "Loaded config: " << default_path << "\n";
            }
        }
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const char* name) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (arg == "--help" || arg == "-h") {
            cfg.help = true;
        } else if (arg == "--list-audio-devices") {
            cfg.list_audio = true;
        } else if (arg == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --config\n";
                return false;
            }
            ++i;
        } else if (arg == "--audio-output") {
            auto value = requireValue("--audio-output");
            if (!value) return false;
            cfg.audio_output = *value;
        } else if (arg == "--audio-input") {
            auto value = requireValue("--audio-input");
            if (!value) return false;
            cfg.audio_input = *value;
        } else if (arg == "--port") {
            auto value = requireValue("--port");
            if (!value || !parseUint16(*value, cfg.port) ||
                cfg.port == std::numeric_limits<uint16_t>::max()) {
                std::cerr << "Invalid --port value (must leave room for data port N+1)\n";
                return false;
            }
        } else if (arg == "--bind") {
            auto value = requireValue("--bind");
            if (!value) return false;
            cfg.bind_address = *value;
        } else if (arg == "--callsign") {
            auto value = requireValue("--callsign");
            if (!value) return false;
            cfg.callsign = ultra::protocol::sanitizeCallsign(*value);
            if (cfg.callsign.empty()) {
                std::cerr << "Invalid --callsign value\n";
                return false;
            }
        } else if (arg == "--inject-channel") {
            cfg.inject_channel = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                const std::string type = lower(argv[++i]);
                if (type != "awgn") {
                    std::cerr << "ultra_tnc --inject-channel only supports awgn; "
                                 "got '" << type << "'\n";
                    return false;
                }
                cfg.inject_channel_type = type;
            }
        } else if (arg == "--no-inject-channel") {
            cfg.inject_channel = false;
        } else if (arg == "--snr") {
            auto value = requireValue("--snr");
            auto parsed = value ? parseFloat(*value) : std::nullopt;
            if (!parsed) {
                std::cerr << "Invalid --snr value\n";
                return false;
            }
            cfg.snr_db = *parsed;
        } else if (arg == "--rate") {
            auto value = requireValue("--rate");
            auto parsed = value ? parseCodeRate(*value) : std::nullopt;
            if (!parsed) {
                std::cerr << "Unknown code rate (use auto, r1_4, r1_2, r2_3, r3_4)\n";
                return false;
            }
            cfg.forced_rate = *parsed;
        } else if (arg == "--mod") {
            auto value = requireValue("--mod");
            auto parsed = value ? parseModulation(*value) : std::nullopt;
            if (!parsed) {
                std::cerr << "Unknown modulation\n";
                return false;
            }
            cfg.forced_mod = *parsed;
        } else if (arg == "--ofdm-config") {
            auto value = requireValue("--ofdm-config");
            if (!value) return false;
            const std::string preset = lower(*value);
            if (preset == "default") {
                cfg.ofdm_config = OFDMConfigPreset::Default;
            } else if (preset == "nvis") {
                cfg.ofdm_config = OFDMConfigPreset::Nvis;
            } else {
                std::cerr << "Unknown OFDM config (use default or nvis)\n";
                return false;
            }
        } else if (arg == "--ptt-serial-port") {
            auto value = requireValue("--ptt-serial-port");
            if (!value) return false;
            cfg.ptt_serial_port = *value;
        } else if (arg == "--ptt-serial-baud") {
            auto value = requireValue("--ptt-serial-baud");
            if (!value) return false;
            if (!parsePositiveIntStrict(*value, cfg.ptt_serial_baud, 50, 4000000)) {
                std::cerr << "Invalid --ptt-serial-baud value (must be 50..4000000)\n";
                return false;
            }
        } else if (arg == "--ptt-serial-line") {
            auto value = requireValue("--ptt-serial-line");
            if (!value) return false;
            const std::string line = lower(*value);
            if (line != "rts" && line != "dtr") {
                std::cerr << "--ptt-serial-line must be rts or dtr\n";
                return false;
            }
            cfg.ptt_serial_line = line;
        } else if (arg == "--ptt-inactive-high") {
            cfg.ptt_inactive_high = true;
        } else if (arg == "--ptt-active-high" || arg == "--no-ptt-inactive-high") {
            cfg.ptt_inactive_high = false;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }
    return true;
}

}  // namespace ultra::tnc::config
