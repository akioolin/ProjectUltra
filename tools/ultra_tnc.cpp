#include "gui/audio_engine.hpp"
#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "gui/serial_ptt.hpp"
#include "protocol/frame_v2.hpp"
#include "protocol/protocol_engine.hpp"
#include "tnc/tnc_bridge.hpp"
#include "tnc/tnc_server.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "waveform/ofdm_cox_waveform.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using ultra::Bytes;
using ultra::CodeRate;
using ultra::ModemConfig;
using ultra::Modulation;
using ultra::gui::DecodeResult;
using ultra::gui::StreamingDecoder;
using ultra::gui::StreamingEncoder;
using ultra::protocol::ConnectionState;
using ultra::protocol::WaveformMode;
namespace v2 = ultra::protocol::v2;

std::atomic<bool> g_stop_requested{false};

void handleSignal(int) {
    g_stop_requested.store(true, std::memory_order_release);
}

enum class OFDMConfigPreset {
    Default,
    Nvis
};

struct Config {
    std::string audio_output;
    std::string audio_input;
    std::string bind_address = "127.0.0.1";
    uint16_t port = 8300;
    std::string callsign = "NOCALL";
    bool inject_channel = false;
    std::string inject_channel_type = "awgn";
    float snr_db = 20.0f;
    CodeRate forced_rate = CodeRate::AUTO;
    Modulation forced_mod = Modulation::AUTO;
    OFDMConfigPreset ofdm_config = OFDMConfigPreset::Default;
    // Hardware PTT via serial line. Empty port = disabled (rely on VOX
    // or an external CAT controller). When set, ultra_tnc opens the
    // serial device and toggles RTS/DTR on each PTT transition.
    std::string ptt_serial_port;
    int ptt_serial_baud = 9600;
    std::string ptt_serial_line = "rts";    // "rts" or "dtr"
    bool ptt_inactive_high = false;          // true = inverted (some radios)
    bool help = false;
    bool list_audio = false;
};

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool isNoneDevice(const std::string& device) {
    return lower(device) == "none";
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

std::optional<CodeRate> parseCodeRate(const std::string& value) {
    const std::string v = lower(value);
    if (v == "auto") return CodeRate::AUTO;
    if (v == "r1_4") return CodeRate::R1_4;
    if (v == "r1_2") return CodeRate::R1_2;
    if (v == "r2_3") return CodeRate::R2_3;
    if (v == "r3_4") return CodeRate::R3_4;
    return std::nullopt;
}

std::optional<Modulation> parseModulation(const std::string& value) {
    const std::string v = lower(value);
    if (v == "auto") return Modulation::AUTO;
    if (v == "dqpsk") return Modulation::DQPSK;
    if (v == "d8psk") return Modulation::D8PSK;
    if (v == "dbpsk") return Modulation::DBPSK;
    if (v == "qpsk") return Modulation::QPSK;
    if (v == "bpsk") return Modulation::BPSK;
    if (v == "qam16") return Modulation::QAM16;
    if (v == "qam32") return Modulation::QAM32;
    if (v == "qam64") return Modulation::QAM64;
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
        << "  --inject-channel [type]     Apply simple TX-side AWGN before audio output\n"
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

// Apply one "key = value" pair from a config file or env. Mirrors the
// CLI flag set so a user can point ultra_tnc at a config file and never
// touch the command line again.
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
        cfg.inject_channel = (value == "true" || value == "1" || value == "yes");
        if (!cfg.inject_channel && !value.empty() && value != "false" &&
            value != "0" && value != "no") {
            cfg.inject_channel = true;
            cfg.inject_channel_type = lower(value);
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
        try { cfg.ptt_serial_baud = std::stoi(value); } catch (...) { return false; }
    } else if (key == "ptt_serial_line" || key == "ptt-serial-line") {
        const std::string line = lower(value);
        if (line != "rts" && line != "dtr") return false;
        cfg.ptt_serial_line = line;
    } else if (key == "ptt_inactive_high" || key == "ptt-inactive-high") {
        cfg.ptt_inactive_high = (value == "true" || value == "1" || value == "yes");
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
        // Strip comments + trim
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

// Search for a default config file in conventional locations. Returns
// empty string if none found. Order: ./ultra_tnc.conf, then
// $XDG_CONFIG_HOME/ultra_tnc/config, then ~/.config/ultra_tnc/config.
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
    // First pass: scan for --config explicitly. Apply it before CLI flags
    // so flags override file values.
    std::string explicit_config;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            explicit_config = argv[i + 1];
            break;
        }
    }
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
            // Already consumed in the first pass, just skip its value.
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
                cfg.inject_channel_type = lower(argv[++i]);
            }
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
            try {
                cfg.ptt_serial_baud = std::stoi(*value);
            } catch (...) {
                std::cerr << "Invalid --ptt-serial-baud value\n";
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
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }
    return true;
}

class UltraTNCStation {
public:
    UltraTNCStation(const Config& cfg,
                    ultra::protocol::ProtocolEngine& engine,
                    ultra::gui::AudioEngine& audio,
                    ultra::tnc::TNCBridge& bridge)
        : cfg_(cfg),
          engine_(engine),
          audio_(audio),
          bridge_(bridge),
          rng_(12345),
          noise_(0.0f, 1.0f) {
        configureModem();
        setupCallbacks();
    }

    ~UltraTNCStation() {
        stop();
    }

    bool start() {
        if (running_.load()) {
            return true;
        }

        const bool use_output = !isNoneDevice(cfg_.audio_output);
        const bool use_input = !isNoneDevice(cfg_.audio_input);

        if (use_output || use_input) {
            if (!audio_.initialize()) {
                std::cerr << "AudioEngine init failed\n";
                return false;
            }
        }

        if (use_output) {
            if (!audio_.openOutput(cfg_.audio_output)) {
                std::cerr << "Failed to open audio output '" << cfg_.audio_output << "'\n";
                return false;
            }
            audio_.startPlayback();
            output_enabled_ = true;
        }

        if (use_input) {
            audio_.setInputCaptureMode(ultra::gui::AudioEngine::InputCaptureMode::Queue);
            if (!audio_.openInput(cfg_.audio_input)) {
                std::cerr << "Failed to open audio input '" << cfg_.audio_input << "'\n";
                return false;
            }
            audio_.startCapture();
            input_enabled_ = true;
        }

        running_.store(true);
        decode_thread_ = std::thread(&UltraTNCStation::decodeLoop, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }

        decoder_.stop();
        if (decode_thread_.joinable()) {
            decode_thread_.join();
        }

        if (input_enabled_) {
            audio_.stopCapture();
            audio_.closeInput();
            input_enabled_ = false;
        }
        if (output_enabled_) {
            audio_.stopPlayback();
            audio_.closeOutput();
            output_enabled_ = false;
        }
        audio_.shutdown();
    }

    void tick(uint32_t elapsed_ms) {
        engine_.tick(elapsed_ms);

        if (input_enabled_) {
            auto samples = audio_.getRxSamples(4096);
            if (!samples.empty()) {
                decoder_.feedAudio(samples);
            }
        }

        bridge_.tick(elapsed_ms);
    }

private:
    static constexpr int kSampleRate = 48000;

    const Config& cfg_;
    ultra::protocol::ProtocolEngine& engine_;
    ultra::gui::AudioEngine& audio_;
    ultra::tnc::TNCBridge& bridge_;

    StreamingEncoder encoder_;
    StreamingDecoder decoder_;

    ModemConfig base_ofdm_config_;
    ModemConfig ofdm_config_;
    WaveformMode tx_waveform_mode_ = WaveformMode::MC_DPSK;
    WaveformMode negotiated_waveform_ = WaveformMode::MC_DPSK;
    Modulation data_modulation_ = Modulation::DQPSK;
    CodeRate data_code_rate_ = CodeRate::R1_4;
    float last_cfo_hz_ = 0.0f;

    std::atomic<bool> running_{false};
    std::thread decode_thread_;
    bool input_enabled_ = false;
    bool output_enabled_ = false;
    bool handshake_complete_ = false;
    bool connected_ = false;

    std::mt19937 rng_;
    std::normal_distribution<float> noise_;

    ModemConfig createOFDMConfig() const {
        ModemConfig cfg;
        if (cfg_.ofdm_config == OFDMConfigPreset::Nvis) {
            cfg = ultra::OFDMNvisWaveform::createNvisMode()->getConfig();
        } else {
            ultra::OFDMNvisWaveform default_cox;
            cfg = default_cox.getConfig();
        }

        cfg.sample_rate = kSampleRate;
        cfg.center_freq = 1500.0f;
        cfg.modulation = data_modulation_;
        cfg.code_rate = data_code_rate_;
        cfg.use_pilots = true;
        cfg.pilot_spacing =
            ultra::ofdm_link_adaptation::recommendedPilotSpacing(cfg.modulation, cfg.code_rate);
        return cfg;
    }

    void configureModem() {
        engine_.setLocalCallsign(cfg_.callsign);
        engine_.setMeasuredSNR(cfg_.snr_db);
        if (cfg_.forced_mod != Modulation::AUTO) {
            engine_.setForcedModulation(cfg_.forced_mod);
        }
        if (cfg_.forced_rate != CodeRate::AUTO) {
            engine_.setForcedCodeRate(cfg_.forced_rate);
        }
        // Chase-combining HARQ: stores soft LLRs from failed decodes
        // and sums them into subsequent retransmission attempts. The
        // per-frame buffer overhead is small; the decode-success
        // payoff on retransmissions is measurable. Defaults to ON
        // because retx are rare on a clean channel (zero overhead)
        // and welcome on a noisy one (faster recovery).
        engine_.setSoftCombiningHARQ(true);

        base_ofdm_config_ = createOFDMConfig();
        ofdm_config_ = base_ofdm_config_;

        encoder_.setOFDMConfig(ofdm_config_);
        encoder_.setMode(tx_waveform_mode_);
        encoder_.setDataMode(data_modulation_, data_code_rate_);
        encoder_.setMCDPSKCarriers(8);
        encoder_.setFixedFrameCodewords(v2::kDefaultFixedFrameCodewords);

        decoder_.setLogPrefix(cfg_.callsign);
        decoder_.setMode(WaveformMode::MC_DPSK, false);
        decoder_.setMCDPSKCarriers(8);
        decoder_.setFixedFrameCodewords(v2::kDefaultFixedFrameCodewords);
        decoder_.setSoftCombineBuffer(engine_.softCombineBuffer());
    }

    void setupCallbacks() {
        engine_.setTxDataCallback([this](const Bytes& data) {
            queueTx(transmitFrame(data));
        });

        engine_.setTransmitBurstCallback([this](const std::vector<Bytes>& frames) {
            queueTx(transmitBurst(frames));
        });

        engine_.setPingTxCallback([this]() {
            queueTx(transmitPing());
        });

        engine_.setPingReceivedCallback([this]() {
            queueTx(transmitPing());
        });

        engine_.setDataModeChangedCallback([this](Modulation mod, CodeRate rate,
                                                  float peer_snr_db, float peer_fading) {
            (void)peer_snr_db;
            (void)peer_fading;
            setDataMode(mod, rate);
        });

        engine_.setModeNegotiatedCallback([this](WaveformMode mode) {
            negotiated_waveform_ = mode;
        });

        engine_.setConnectWaveformChangedCallback([this](WaveformMode mode) {
            if (mode == WaveformMode::OFDM_NARROW) {
                encoder_.setNarrowbandControl(true);
            }
        });

        engine_.setHandshakeConfirmedCallback([this]() {
            handshake_complete_ = true;
        });

        bridge_.setConnectionChangedCallback([this](ConnectionState state, const std::string&) {
            if (state == ConnectionState::CONNECTED) {
                setConnected(true);
            } else if (state == ConnectionState::DISCONNECTED) {
                setConnected(false);
            }
        });

        bridge_.setPreferredWaveformChangedCallback([this](WaveformMode mode) {
            encoder_.setNarrowbandControl(mode == WaveformMode::OFDM_NARROW);
        });

        decoder_.setFrameCallback([this](const DecodeResult& result) {
            handleDecodedFrame(result);
        });

        decoder_.setPingCallback([this](float snr_db, float cfo_hz) {
            engine_.setMeasuredSNR(snr_db);
            last_cfo_hz_ = cfo_hz;
            if (decoder_.getDetectedBandwidth() == ultra::BandwidthMode::NARROW) {
                encoder_.setNarrowbandControl(true);
                engine_.setNarrowbandOverride(WaveformMode::OFDM_NARROW);
            }
            engine_.onPingReceived();
        });
    }

    void handleDecodedFrame(const DecodeResult& result) {
        if (!result.success || result.is_ping || result.frame_data.empty()) {
            return;
        }

        last_cfo_hz_ = result.cfo_hz;
        auto header = v2::parseHeader(result.frame_data);
        if (header.valid && !v2::isAddressedToCallsign(header, engine_.getLocalCallsign())) {
            return;
        }

        const float fading_index = decoder_.getLastFadingIndex();
        engine_.setChannelQuality(result.snr_db, fading_index);
        engine_.onRxData(result.frame_data);
    }

    void setWaveformMode(WaveformMode mode) {
        tx_waveform_mode_ = mode;
        encoder_.setOFDMConfig(ofdm_config_);
        encoder_.setMode(mode);
        encoder_.setDataMode(data_modulation_, data_code_rate_);
        encoder_.setMCDPSKCarriers(8);
    }

    void setDataMode(Modulation mod, CodeRate rate) {
        data_modulation_ = mod;
        data_code_rate_ = rate;

        ofdm_config_.modulation = mod;
        ofdm_config_.code_rate = rate;
        ofdm_config_.use_pilots = true;
        ofdm_config_.pilot_spacing = ultra::ofdm_link_adaptation::recommendedPilotSpacing(mod, rate);

        if (tx_waveform_mode_ != WaveformMode::MC_DPSK) {
            encoder_.setOFDMConfig(ofdm_config_);
            encoder_.setMode(tx_waveform_mode_);
        }
        encoder_.setDataMode(mod, rate);

        if (negotiated_waveform_ != WaveformMode::MC_DPSK) {
            decoder_.setOFDMConfig(ofdm_config_);
        }
        decoder_.setDataMode(mod, rate);
    }

    void setConnected(bool connected) {
        if (connected_ == connected) {
            return;
        }

        connected_ = connected;
        if (connected_) {
            if (negotiated_waveform_ == WaveformMode::OFDM_NARROW) {
                ofdm_config_ = ultra::presets::narrowbandOFDM();
            } else {
                ofdm_config_ = base_ofdm_config_;
            }
            ofdm_config_.modulation = data_modulation_;
            ofdm_config_.code_rate = data_code_rate_;
            ofdm_config_.use_pilots = true;
            ofdm_config_.pilot_spacing =
                ultra::ofdm_link_adaptation::recommendedPilotSpacing(data_modulation_, data_code_rate_);

            if (negotiated_waveform_ != WaveformMode::MC_DPSK) {
                setWaveformMode(negotiated_waveform_);
                decoder_.setConnectedOFDMMode(negotiated_waveform_, ofdm_config_,
                                              data_modulation_, data_code_rate_);
                decoder_.setKnownCFO(last_cfo_hz_);
                if (negotiated_waveform_ == WaveformMode::OFDM_CHIRP) {
                    encoder_.setBurstInterleave(true);
                    decoder_.setBurstInterleave(true);
                }
            } else {
                decoder_.setMode(WaveformMode::MC_DPSK, true);
                decoder_.setDataMode(data_modulation_, data_code_rate_);
                decoder_.setKnownCFO(last_cfo_hz_);
            }
        } else {
            decoder_.setMode(WaveformMode::MC_DPSK, false);
            encoder_.setBurstInterleave(false);
            decoder_.setBurstInterleave(false);
            negotiated_waveform_ = WaveformMode::MC_DPSK;
            handshake_complete_ = false;
            ofdm_config_ = base_ofdm_config_;
            setWaveformMode(WaveformMode::MC_DPSK);
        }
    }

    std::vector<float> transmitFrame(const Bytes& data) {
        if (data.empty()) {
            return {};
        }

        bool is_handshake_frame = false;
        if (data.size() >= 3) {
            const uint8_t frame_type = data[2];
            is_handshake_frame = (frame_type == 0x12 || frame_type == 0x13);
        }

        const WaveformMode saved_mode = encoder_.getMode();
        const CodeRate saved_rate = encoder_.getCodeRate();

        if (is_handshake_frame) {
            encoder_.setMode(WaveformMode::MC_DPSK);
            encoder_.setDataMode(Modulation::DQPSK, CodeRate::R1_4);
        }

        const bool is_mc_dpsk = encoder_.getMode() == WaveformMode::MC_DPSK;
        const bool use_light = !is_handshake_frame && !is_mc_dpsk && connected_ && handshake_complete_;
        std::vector<float> samples = use_light ? encoder_.encodeFrameLight(data)
                                               : encoder_.encodeFrame(data);

        if (is_handshake_frame) {
            encoder_.setMode(saved_mode);
            encoder_.setDataMode(data_modulation_, saved_rate);
        }

        return samples;
    }

    std::vector<float> transmitBurst(const std::vector<Bytes>& frames) {
        if (frames.empty()) {
            return {};
        }
        if (tx_waveform_mode_ != WaveformMode::MC_DPSK) {
            encoder_.setMode(tx_waveform_mode_);
            encoder_.setDataMode(data_modulation_, data_code_rate_);
        }
        return encoder_.encodeBurstLight(frames);
    }

    std::vector<float> transmitPing() {
        return encoder_.encodePing();
    }

    void queueTx(std::vector<float> samples) {
        if (samples.empty() || !output_enabled_) {
            return;
        }

        if (cfg_.inject_channel) {
            applyAwgn(samples);
        }

        audio_.queueTxSamples(samples);
    }

    void applyAwgn(std::vector<float>& samples) {
        float signal_power = 0.0f;
        for (float sample : samples) {
            signal_power += sample * sample;
        }
        signal_power = samples.empty() ? 0.0f : signal_power / static_cast<float>(samples.size());
        const float snr_linear = std::pow(10.0f, cfg_.snr_db / 10.0f);
        const float noise_stddev = std::sqrt(std::max(signal_power, 1.0e-8f) / std::max(snr_linear, 1.0e-6f));
        for (float& sample : samples) {
            sample = std::clamp(sample + noise_(rng_) * noise_stddev, -1.0f, 1.0f);
        }
    }

    void decodeLoop() {
        while (running_.load()) {
            decoder_.processBuffer();
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parseArgs(argc, argv, cfg)) {
        printUsage(std::cerr);
        return 1;
    }

    if (cfg.help) {
        printUsage(std::cout);
        return 0;
    }

    if (cfg.list_audio) {
        ultra::gui::AudioEngine probe;
        if (!probe.initialize()) {
            std::cerr << "Failed to initialize SDL audio for device listing\n";
            return 1;
        }
        std::cout << "\n  Output devices:\n";
        for (const auto& d : probe.getOutputDevices()) std::cout << "    " << d << "\n";
        std::cout << "\n  Input devices:\n";
        for (const auto& d : probe.getInputDevices()) std::cout << "    " << d << "\n";
        std::cout << "\nUse a device's exact name in --audio-output / --audio-input\n"
                  << "or the equivalent config-file keys (audio_output / audio_input).\n";
        probe.shutdown();
        return 0;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    ultra::gui::AudioEngine audio;
    ultra::protocol::ProtocolEngine engine;
    ultra::tnc::TNCBridge bridge(engine, audio);
    UltraTNCStation station(cfg, engine, audio, bridge);

    bridge.setMyCall({cfg.callsign});
    bridge.setBandwidth(2300);

    // Hardware PTT via serial line. When the user supplies --ptt-serial-port,
    // open the serial controller and toggle RTS/DTR on each PTT transition.
    // No-op if the port is empty (relies on VOX or external CAT).
    ultra::gui::SerialPttController serial_ptt;
    if (!cfg.ptt_serial_port.empty()) {
        if (!serial_ptt.open(cfg.ptt_serial_port, cfg.ptt_serial_baud)) {
            std::cerr << "Failed to open serial PTT port: " << cfg.ptt_serial_port << "\n";
            return 1;
        }
        const ultra::gui::SerialPttLine line =
            (lower(cfg.ptt_serial_line) == "dtr")
                ? ultra::gui::SerialPttLine::DTR
                : ultra::gui::SerialPttLine::RTS;
        // Initialize line to inactive state so we don't accidentally key
        // the radio at startup.
        serial_ptt.setLine(line, cfg.ptt_inactive_high);
        const bool active_state = !cfg.ptt_inactive_high;
        bridge.setPttChangedCallback([&serial_ptt, line, active_state](bool on) {
            serial_ptt.setLine(line, on ? active_state : !active_state);
        });
        std::cout << "Hardware PTT enabled on " << cfg.ptt_serial_port
                  << " @ " << cfg.ptt_serial_baud << " baud, line="
                  << cfg.ptt_serial_line
                  << (cfg.ptt_inactive_high ? " (inverted)" : "") << "\n";
    }

    ultra::tnc::TNCServerConfig server_cfg;
    server_cfg.cmd_port = cfg.port;
    server_cfg.data_port = static_cast<uint16_t>(cfg.port + 1);
    server_cfg.bind_address = cfg.bind_address;
    ultra::tnc::TNCServer server(bridge, server_cfg);

    bridge.attachServer(&server);

    if (!station.start()) {
        return 1;
    }

    bridge.start();

    if (!server.start()) {
        std::cerr << "TNC server bind failed on " << cfg.bind_address << ":" << cfg.port << "\n";
        bridge.stop();
        station.stop();
        return 1;
    }

    std::cout << "ultra_tnc listening on " << cfg.bind_address << ":" << server.getCmdPort()
              << " (data " << server.getDataPort() << ")\n";

    auto last_tick = std::chrono::steady_clock::now();
    while (!g_stop_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick).count();
        last_tick = now;
        const uint32_t elapsed_ms = static_cast<uint32_t>(std::clamp<int64_t>(elapsed, 1, 1000));
        station.tick(elapsed_ms);
    }

    server.stop();
    bridge.stop();
    station.stop();
    return 0;
}
