#include "app.hpp"
#include "startup_trace.hpp"
#include "imgui.h"
#include "ultra/logging.hpp"
#include "sim/hf_channel.hpp"
#include <SDL.h>
#include <cstring>
#include <cmath>
#include <fstream>
#include <cstdarg>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <limits>
#include <deque>
#include <vector>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(dir) _mkdir(dir)
#else
#define MKDIR(dir) mkdir(dir, 0755)
#endif

namespace ultra {
namespace gui {

// File logger for GUI debugging - writes to logs/gui.log next to binary
// ALL logging (including modem, protocol, etc.) goes to this file
static FILE* g_gui_log_file = nullptr;
static bool g_log_initialized = false;
static std::string g_gui_log_path;

static void initLog() {
    if (g_log_initialized) return;
    g_log_initialized = true;

#ifdef _WIN32
    auto tryOpenLog = [](const char* path) -> FILE* {
        if (!path || path[0] == '\0') {
            return nullptr;
        }

        const char* slash_back = std::strrchr(path, '\\');
        const char* slash_fwd = std::strrchr(path, '/');
        const char* sep = slash_back;
        if (!sep || (slash_fwd && slash_fwd > sep)) {
            sep = slash_fwd;
        }
        if (sep) {
            std::string dir(path, static_cast<size_t>(sep - path));
            if (!dir.empty()) {
                MKDIR(dir.c_str());
            }
        }

        return std::fopen(path, "w");
    };

    if (!g_gui_log_file) {
        g_gui_log_file = tryOpenLog("logs\\gui.log");
        if (g_gui_log_file) {
            g_gui_log_path = "logs\\gui.log";
        }
    }

    if (!g_gui_log_file) {
        g_gui_log_file = tryOpenLog("gui.log");
        if (g_gui_log_file) {
            g_gui_log_path = "gui.log";
        }
    }

    if (!g_gui_log_file) {
        const char* temp = std::getenv("TEMP");
        if (temp && temp[0] != '\0') {
            std::string temp_path = std::string(temp) + "\\ProjectUltra\\gui.log";
            g_gui_log_file = tryOpenLog(temp_path.c_str());
            if (g_gui_log_file) {
                g_gui_log_path = temp_path;
            }
        }
    }
#else
    auto tryOpenLog = [](const std::filesystem::path& path) -> FILE* {
        std::error_code ec;
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        return fopen(path.string().c_str(), "w");
    };

    std::vector<std::filesystem::path> candidates;
    candidates.emplace_back(std::filesystem::path("logs") / "gui.log");
    candidates.emplace_back("gui.log");
    if (const char* temp = std::getenv("TMPDIR")) {
        candidates.emplace_back(std::filesystem::path(temp) / "projectultra_gui.log");
    }
    candidates.emplace_back("/tmp/projectultra_gui.log");

    for (const auto& path : candidates) {
        g_gui_log_file = tryOpenLog(path);
        if (g_gui_log_file) {
            g_gui_log_path = path.string();
            break;
        }
    }
#endif

    if (g_gui_log_file) {
        // Redirect ALL logging (modem, protocol, etc.) to this file
        ultra::setLogFile(g_gui_log_file);
        ultra::log(ultra::LogLevel::INFO, "GUI", "File logger initialized: %s",
                   g_gui_log_path.c_str());
    }
}

static void guiLog(const char* fmt, ...) {
    initLog();
    if (!g_gui_log_file) return;

    // Use the same timestamp format as the global logger
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - ultra::g_log_start_time).count();
    int secs = static_cast<int>(elapsed / 1000);
    int ms = static_cast<int>(elapsed % 1000);

    // Format message
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    fprintf(g_gui_log_file, "[%3d.%03d][INFO ][GUI  ] %s\n", secs, ms, buf);
    fflush(g_gui_log_file);
}

// Convert fading index to channel quality string
// Thresholds aligned with waveform_selection.hpp (2026-02-03)
// Combined index = freq_cv + temporal_cv (includes Doppler spread measurement)
// Calibrated: AWGN ~0.04, Good ~0.62, Moderate ~0.90, Poor ~0.82
static const char* fadingToQuality(float fading) {
    if (fading < 0.15f) return "AWGN";
    if (fading < 0.65f) return "Good";
    if (fading < 1.10f) return "Moderate";
    return "Poor";
}

// Same as above but also sets color for GUI display
static const char* fadingToQualityWithColor(float fading, ImVec4& color) {
    if (fading < 0.15f) {
        color = ImVec4(0.0f, 1.0f, 0.5f, 1.0f);  // Cyan
        return "AWGN";
    } else if (fading < 0.65f) {
        color = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);  // Green
        return "Good";
    } else if (fading < 1.10f) {
        color = ImVec4(0.8f, 0.8f, 0.0f, 1.0f);  // Yellow
        return "Moderate";
    } else {
        color = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);  // Orange
        return "Poor";
    }
}

// User-friendly waveform name (hides internal variants like OFDM_COX/OFDM_CHIRP)
static const char* waveformDisplayName(protocol::WaveformMode mode) {
    switch (mode) {
        case protocol::WaveformMode::MC_DPSK: return "MC-DPSK";
        case protocol::WaveformMode::MFSK: return "MFSK";
        case protocol::WaveformMode::OTFS_EQ:
        case protocol::WaveformMode::OTFS_RAW: return "OTFS";
        case protocol::WaveformMode::OFDM_CHIRP:
        case protocol::WaveformMode::OFDM_COX:
        default: return "OFDM";
    }
}

static uint32_t safeFileSizeBytes(const std::string& path) {
    std::error_code ec;
    uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > static_cast<uintmax_t>(std::numeric_limits<uint32_t>::max())) {
        return 0;
    }
    return static_cast<uint32_t>(size);
}

template <size_t N>
static size_t boundedCStringLen(const char (&buf)[N]) {
    const void* term = std::memchr(buf, '\0', N);
    return term ? static_cast<size_t>(static_cast<const char*>(term) - buf) : N;
}

static float codeRateValue(CodeRate rate) {
    switch (rate) {
        case CodeRate::R1_4: return 0.25f;
        case CodeRate::R1_2: return 0.50f;
        case CodeRate::R2_3: return 2.0f / 3.0f;
        case CodeRate::R3_4: return 0.75f;
        default: return 0.25f;
    }
}

static float modulationBitsPerSymbol(Modulation mod) {
    switch (mod) {
        case Modulation::BPSK: return 1.0f;
        case Modulation::QPSK:
        case Modulation::DQPSK: return 2.0f;
        case Modulation::D8PSK:
        case Modulation::QAM8: return 3.0f;
        case Modulation::QAM16: return 4.0f;
        case Modulation::QAM32: return 5.0f;
        case Modulation::QAM64: return 6.0f;
        default: return 1.0f;
    }
}

static float modeEfficiency(Modulation mod, CodeRate rate) {
    return modulationBitsPerSymbol(mod) * codeRateValue(rate);
}

static const char* adaptationDirection(Modulation from_mod, CodeRate from_rate,
                                       Modulation to_mod, CodeRate to_rate) {
    float from_eff = modeEfficiency(from_mod, from_rate);
    float to_eff = modeEfficiency(to_mod, to_rate);
    if (to_eff > from_eff + 0.05f) return "improving";
    if (to_eff < from_eff - 0.05f) return "degrading";
    return "changing";
}

App::App() : App(Options{}) {}

App::App(const Options& opts) : options_(opts), sim_ui_visible_(opts.enable_sim) {
    ultra::gui::startupTrace("App", "ctor-body-enter");
    ultra::gui::startupTrace("App", "gui-log-enter");
    guiLog("=== GUI Started ===");
    ultra::gui::startupTrace("App", "gui-log-exit");
    // Load persistent settings
    ultra::gui::startupTrace("App", "settings-load-enter");
    settings_.load();
    ultra::gui::startupTrace("App", "settings-load-exit");

    ultra::gui::startupTrace("App", "presets-balanced-enter");
    config_ = presets::balanced();
    ultra::gui::startupTrace("App", "presets-balanced-exit");

    if (!options_.disable_waterfall) {
        ultra::gui::startupTrace("App", "waterfall-create-begin");
        waterfall_ = std::make_unique<WaterfallWidget>();
        ultra::gui::startupTrace("App", "waterfall-create-end");
    } else {
        guiLog("Waterfall disabled by startup option");
    }

    // Initialize protocol with saved callsign
    ultra::gui::startupTrace("App", "callsign-init-enter");
    if (boundedCStringLen(settings_.callsign) > 0) {
        protocol_.setLocalCallsign(settings_.callsign);
        modem_.setLogPrefix(settings_.callsign);
    }
    ultra::gui::startupTrace("App", "callsign-init-exit");

    // Set up raw data callback for protocol layer
    ultra::gui::startupTrace("App", "set-raw-callback-enter");
    modem_.setRawDataCallback([this](const Bytes& data) {
        guiLog("Our modem decoded %zu bytes", data.size());
        // Update protocol layer with current SNR and fading before processing frame
        // In simulation mode, use the known simulation SNR (DPSK doesn't measure SNR)
        // In real mode, use measured SNR from OFDM demodulator
        float snr_db = simulation_enabled_ ? simulation_snr_db_ : modem_.getStats().snr_db;
        float fading = modem_.getFadingIndex();
        protocol_.setMeasuredSNR(snr_db);
        protocol_.setChannelQuality(snr_db, fading);
        protocol_.onRxData(data);
        updateAdaptiveAdvisory(snr_db, fading);
    });
    ultra::gui::startupTrace("App", "set-raw-callback-exit");

    // Set up status callback to show codeword progress in RX log
    ultra::gui::startupTrace("App", "set-status-callback-enter");
    modem_.setStatusCallback([this](const std::string& status) {
        rx_log_.push_back(status);
    });
    ultra::gui::startupTrace("App", "set-status-callback-exit");

    // Set up protocol engine callbacks
    ultra::gui::startupTrace("App", "protocol-callbacks-enter");
    protocol_.setTxDataCallback([this](const Bytes& data) {
        // When protocol layer wants to transmit, convert to audio
        auto samples = modem_.transmit(data);
        if (!samples.empty()) {
            if (simulation_enabled_) {
                // Add PTT noise once at start of transmission (100-300ms)
                std::uniform_int_distribution<size_t> ptt_dist(4800, 14400);
                size_t ptt_samples = ptt_dist(sim_rng_);

                float typical_rms = 0.1f;
                float snr_linear = std::pow(10.0f, simulation_snr_db_ / 10.0f);
                float noise_power = (typical_rms * typical_rms) / snr_linear;
                float noise_stddev = std::sqrt(noise_power);
                std::normal_distribution<float> noise_dist(0.0f, noise_stddev);

                std::vector<float> ptt_noise(ptt_samples);
                for (float& s : ptt_noise) s = noise_dist(sim_rng_);

                // Mark TX active (include PTT noise in duration)
                size_t total_samples = ptt_samples + samples.size();
                size_t tx_duration_ms = (total_samples * 1000) / 48000;
                tx_in_progress_ = true;
                tx_end_time_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(tx_duration_ms + 100);

                // Queue PTT noise + signal for real-time streaming
                std::lock_guard<std::mutex> lock(our_tx_pending_mutex_);
                our_tx_pending_.insert(our_tx_pending_.end(), ptt_noise.begin(), ptt_noise.end());
                our_tx_pending_.insert(our_tx_pending_.end(), samples.begin(), samples.end());
                guiLog("SIM: Queued %zu TX samples (+ %zu PTT noise) for streaming", samples.size(), ptt_samples);
            } else {
                // Normal mode: send to real audio device (it streams at 48kHz)
                // Mute RX, stop capture, and clear all buffers to prevent acoustic feedback
                // (speaker → microphone would cause us to decode our own TX)
                audio_.setRxMuted(true);   // Prevent callback from feeding modem
                audio_.stopCapture();       // Stop SDL audio capture
                audio_.clearRxBuffer();     // Clear audio engine buffer
                modem_.clearRxBuffer();     // Clear modem decoder buffer
                size_t tx_duration_ms = (samples.size() * 1000) / 48000;
                tx_in_progress_ = true;
                tx_end_time_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(tx_duration_ms + 100);
                if (waterfall_) {
                    waterfall_->addSamples(samples.data(), samples.size());
                }
                audio_.queueTxSamples(samples);
            }
        }
    });

    // Burst TX callback - encode multiple frames as single OFDM burst
    protocol_.setTransmitBurstCallback([this](const std::vector<Bytes>& frames) {
        auto samples = modem_.transmitBurst(frames);
        if (!samples.empty()) {
            if (simulation_enabled_) {
                size_t tx_duration_ms = (samples.size() * 1000) / 48000;
                tx_in_progress_ = true;
                tx_end_time_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(tx_duration_ms + 100);
                std::lock_guard<std::mutex> lock(our_tx_pending_mutex_);
                our_tx_pending_.insert(our_tx_pending_.end(), samples.begin(), samples.end());
                guiLog("SIM: Queued burst of %zu frames (%zu samples)", frames.size(), samples.size());
            } else {
                audio_.setRxMuted(true);
                audio_.stopCapture();
                audio_.clearRxBuffer();
                modem_.clearRxBuffer();
                size_t tx_duration_ms = (samples.size() * 1000) / 48000;
                tx_in_progress_ = true;
                tx_end_time_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(tx_duration_ms + 100);
                if (waterfall_) {
                    waterfall_->addSamples(samples.data(), samples.size());
                }
                audio_.queueTxSamples(samples);
            }
        }
    });
    ultra::gui::startupTrace("App", "protocol-callbacks-mid1");

    protocol_.setMessageReceivedCallback([this](const std::string& from, const std::string& text) {
        // Received a message via ARQ
        std::string msg = "[RX " + from + "] " + text;
        rx_log_.push_back(msg);
        if (rx_log_.size() > MAX_RX_LOG) {
            rx_log_.pop_front();
        }
    });
    ultra::gui::startupTrace("App", "protocol-callbacks-mid2");

    protocol_.setConnectionChangedCallback([this](protocol::ConnectionState state, const std::string& info) {
        guiLog("Connection state changed: %d (%s)", static_cast<int>(state), info.c_str());

        // Update modem engine connection state (affects waveform selection)
        // Stay "connected" during DISCONNECTING so we can receive the ACK via OFDM
        bool modem_connected = (state == protocol::ConnectionState::CONNECTED ||
                                state == protocol::ConnectionState::DISCONNECTING);
        modem_.setConnected(modem_connected);

        std::string msg;
        switch (state) {
            case protocol::ConnectionState::PROBING:
                resetAdaptiveAdvisory();
                msg = "[SYS] Probing " + info + "...";
                break;
            case protocol::ConnectionState::CONNECTING:
                resetAdaptiveAdvisory();
                msg = "[SYS] Connecting to " + info + "...";
                break;
            case protocol::ConnectionState::CONNECTED:
                resetAdaptiveAdvisory();
                msg = "[SYS] Connected to " + info;  // info contains remote callsign
                break;
            case protocol::ConnectionState::DISCONNECTING:
                msg = "[SYS] Disconnecting...";
                break;
            case protocol::ConnectionState::DISCONNECTED:
                resetAdaptiveAdvisory();
                if (info.find("timeout") != std::string::npos) {
                    msg = "[FAILED] " + info;  // Make failures more visible
                } else {
                    msg = "[SYS] Disconnected" + (info.empty() ? "" : ": " + info);
                }
                // Reset waveform mode to OFDM when disconnected
                modem_.setWaveformMode(protocol::WaveformMode::OFDM_COX);
                // Reset connect waveform to DPSK for next connection attempt
                modem_.setConnectWaveform(protocol::WaveformMode::MC_DPSK);
                break;
        }
        rx_log_.push_back(msg);
        if (rx_log_.size() > MAX_RX_LOG) {
            rx_log_.pop_front();
        }
    });
    ultra::gui::startupTrace("App", "protocol-callbacks-mid3");

    protocol_.setIncomingCallCallback([this](const std::string& from) {
        pending_incoming_call_ = from;
        std::string msg = "[SYS] Incoming call from " + from;
        rx_log_.push_back(msg);
        if (rx_log_.size() > MAX_RX_LOG) {
            rx_log_.pop_front();
        }
    });
    ultra::gui::startupTrace("App", "protocol-callbacks-mid4");

    // PING TX callback - protocol wants to send a fast presence probe
    protocol_.setPingTxCallback([this]() {
        guiLog("TX PING: Probing for remote station...");
        auto samples = modem_.transmitPing();
        if (!samples.empty()) {
            if (simulation_enabled_) {
                // Queue for sim
                std::lock_guard<std::mutex> lock(our_tx_pending_mutex_);
                our_tx_pending_.insert(our_tx_pending_.end(), samples.begin(), samples.end());
                guiLog("SIM: Queued %zu PING samples", samples.size());
            } else {
                // Send to real audio - mute RX to prevent acoustic feedback
                audio_.setRxMuted(true);
                audio_.stopCapture();
                audio_.clearRxBuffer();
                modem_.clearRxBuffer();
                size_t tx_duration_ms = (samples.size() * 1000) / 48000;
                tx_in_progress_ = true;
                tx_end_time_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(tx_duration_ms + 100);
                if (waterfall_) {
                    waterfall_->addSamples(samples.data(), samples.size());
                }
                audio_.queueTxSamples(samples);
            }
        }
    });

    // PING received callback - someone is probing us
    protocol_.setPingReceivedCallback([this]() {
        guiLog("RX PING: Incoming probe, sending PONG...");
        auto samples = modem_.transmitPong();
        if (!samples.empty()) {
            if (simulation_enabled_) {
                // Queue for sim
                std::lock_guard<std::mutex> lock(our_tx_pending_mutex_);
                our_tx_pending_.insert(our_tx_pending_.end(), samples.begin(), samples.end());
                guiLog("SIM: Queued %zu PONG samples", samples.size());
            } else {
                // Send to real audio - mute RX to prevent acoustic feedback
                audio_.setRxMuted(true);
                audio_.stopCapture();
                audio_.clearRxBuffer();
                modem_.clearRxBuffer();
                size_t tx_duration_ms = (samples.size() * 1000) / 48000;
                tx_in_progress_ = true;
                tx_end_time_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(tx_duration_ms + 100);
                if (waterfall_) {
                    waterfall_->addSamples(samples.data(), samples.size());
                }
                audio_.queueTxSamples(samples);
            }
        }
    });
    ultra::gui::startupTrace("App", "protocol-callbacks-mid5");

    // Wire up modem ping detection to protocol
    modem_.setPingReceivedCallback([this](float snr) {
        // In simulation mode, use the configured SNR instead of detected SNR
        // (chirp detection sees clean loopback signal, not the simulated channel)
        float display_snr = simulation_enabled_ ? simulation_snr_db_ : snr;

        // Note: Fading index not shown here - it's only reliable after decoding data frames
        // Check state to show appropriate message
        if (protocol_.getState() == protocol::ConnectionState::PROBING) {
            guiLog("RX PONG: Remote station responded! (SNR=%.1f dB)", display_snr);
            // Add to message log so user sees it in the app
            char buf[80];
            snprintf(buf, sizeof(buf), "[PONG] Station responded (SNR=%.0f dB)", display_snr);
            rx_log_.push_back(buf);
            if (rx_log_.size() > MAX_RX_LOG) rx_log_.pop_front();
        } else {
            guiLog("MODEM: Detected PING/PONG (SNR=%.1f dB)", display_snr);
        }
        protocol_.onPingReceived();
    });
    ultra::gui::startupTrace("App", "protocol-callbacks-mid6");

    protocol_.setDataModeChangedCallback([this](Modulation mod, CodeRate rate, float snr_db, float peer_fading) {
        // Update modem engine with new data mode
        modem_.setDataMode(mod, rate);
        resetAdaptiveAdvisory();

        // Local estimate for operator visibility/debugging.
        auto waveform = modem_.getWaveformMode();
        float local_fading = modem_.getFadingIndex();

        const char* local_quality = fadingToQuality(local_fading);
        bool peer_fading_valid = (peer_fading >= 0.0f);
        const char* peer_quality = peer_fading_valid ? fadingToQuality(peer_fading) : "n/a";
        char peer_fading_text[32];
        if (peer_fading_valid) {
            snprintf(peer_fading_text, sizeof(peer_fading_text), "%.2f %s", peer_fading, peer_quality);
        } else {
            snprintf(peer_fading_text, sizeof(peer_fading_text), "n/a");
        }
        const char* wf_name = waveformDisplayName(waveform);
        guiLog("MODE_CHANGE: %s %s %s (peer_snr=%.1f dB, peer_fading=%s, local_fading=%.2f %s)",
               wf_name, modulationToString(mod), codeRateToString(rate),
               snr_db, peer_fading_text,
               local_fading, local_quality);

        // Format display with waveform info and channel quality
        char buf[200];
        if (waveform == protocol::WaveformMode::MC_DPSK) {
            snprintf(buf, sizeof(buf),
                     "[MODE] MC-DPSK 8 carriers %s (peer SNR=%d dB, peer fading=%s, local fading=%.2f %s)",
                     codeRateToString(rate), static_cast<int>(snr_db),
                     peer_fading_text,
                     local_fading, local_quality);
        } else {
            snprintf(buf, sizeof(buf),
                     "[MODE] %s %s %s (peer SNR=%d dB, peer fading=%s, local fading=%.2f %s)",
                     wf_name, modulationToString(mod), codeRateToString(rate),
                     static_cast<int>(snr_db), peer_fading_text,
                     local_fading, local_quality);
        }
        rx_log_.push_back(buf);
        if (rx_log_.size() > MAX_RX_LOG) {
            rx_log_.pop_front();
        }

        // Advisory-only peer view (does not change mode yet).
        if (peer_fading_valid) {
            Modulation peer_mod = mod;
            CodeRate peer_rate = rate;
            protocol::recommendDataMode(snr_db, waveform, peer_mod, peer_rate, peer_fading);
            bool peer_change = (peer_mod != mod || peer_rate != rate);

            char adpt_buf[220];
            if (peer_change) {
                const char* direction = adaptationDirection(mod, rate, peer_mod, peer_rate);
                snprintf(adpt_buf, sizeof(adpt_buf),
                         "[ADPT] Peer reports %s conditions (SNR=%.1f dB, F.I.=%.2f): %s -> %s %s",
                         direction, snr_db, peer_fading,
                         direction, modulationToString(peer_mod), codeRateToString(peer_rate));
            } else {
                snprintf(adpt_buf, sizeof(adpt_buf),
                         "[ADPT] Peer reports stable conditions (SNR=%.1f dB, F.I.=%.2f): keep %s %s",
                         snr_db, peer_fading, modulationToString(mod), codeRateToString(rate));
            }

            guiLog("%s", adpt_buf);
            rx_log_.push_back(adpt_buf);
            if (rx_log_.size() > MAX_RX_LOG) {
                rx_log_.pop_front();
            }
        }
    });
    ultra::gui::startupTrace("App", "protocol-callbacks-mid7");

    // Waveform mode negotiation callback (OFDM, DPSK, MFSK switching)
    protocol_.setModeNegotiatedCallback([this](protocol::WaveformMode mode) {
        std::string mode_name;
        switch (mode) {
            case protocol::WaveformMode::MC_DPSK: mode_name = "MC-DPSK 8 carriers"; break;
            case protocol::WaveformMode::MFSK: mode_name = "MFSK"; break;
            case protocol::WaveformMode::OTFS_EQ: mode_name = "OTFS"; break;
            case protocol::WaveformMode::OTFS_RAW: mode_name = "OTFS"; break;
            case protocol::WaveformMode::OFDM_CHIRP: mode_name = "OFDM"; break;
            case protocol::WaveformMode::OFDM_COX: mode_name = "OFDM"; break;
            default: mode_name = "OFDM"; break;
        }
        guiLog("WAVEFORM_CHANGE: %s", mode_name.c_str());

        // Update modem engine with new waveform mode
        modem_.setWaveformMode(mode);

        std::string msg = "[WAVEFORM] " + mode_name;
        rx_log_.push_back(msg);
        if (rx_log_.size() > MAX_RX_LOG) {
            rx_log_.pop_front();
        }
    });
    ultra::gui::startupTrace("App", "protocol-callbacks-mid8");

    // Connect waveform fallback callback (DPSK -> MFSK when connection attempts fail)
    protocol_.setConnectWaveformChangedCallback([this](protocol::WaveformMode mode) {
        const char* mode_name = (mode == protocol::WaveformMode::MFSK) ? "MFSK" : "DPSK";
        guiLog("CONNECT_WAVEFORM: Switching to %s for connection attempts", mode_name);
        modem_.setConnectWaveform(mode);
    });
    ultra::gui::startupTrace("App", "protocol-callbacks-mid9");

    // Handshake confirmed callback - now safe to use negotiated waveform
    protocol_.setHandshakeConfirmedCallback([this]() {
        guiLog("HANDSHAKE: Confirmed, switching to negotiated waveform");
        modem_.setHandshakeComplete(true);
    });
    ultra::gui::startupTrace("App", "protocol-callbacks-mid10");

    // File transfer callbacks
    protocol_.setFileProgressCallback([this](const protocol::FileTransferProgress& p) {
        // Start timing on first progress (for receiving files)
        if (last_progress_milestone_ == 0 && !p.is_sending) {
            file_transfer_start_time_ = std::chrono::steady_clock::now();
        }

        // Log progress milestones (25%, 50%, 75%)
        int pct = static_cast<int>(p.percentage());
        int milestone = (pct / 25) * 25;  // Round down to 25, 50, 75
        if (milestone > 0 && milestone < 100 && milestone > last_progress_milestone_) {
            last_progress_milestone_ = milestone;
            std::string msg = "[FILE] " + std::string(p.is_sending ? "TX" : "RX") +
                              " " + std::to_string(p.transferred_bytes) + "/" +
                              std::to_string(p.total_bytes) + " bytes (" +
                              std::to_string(milestone) + "%)";
            rx_log_.push_back(msg);
            if (rx_log_.size() > MAX_RX_LOG) rx_log_.pop_front();
        }
    });
    ultra::gui::startupTrace("App", "protocol-callbacks-mid11");

    protocol_.setFileReceivedCallback([this](const std::string& path, bool success) {
        last_progress_milestone_ = 0;  // Reset for next transfer
        auto duration = std::chrono::steady_clock::now() - file_transfer_start_time_;
        float seconds = std::chrono::duration<float>(duration).count();
        uint32_t file_bytes = success ? safeFileSizeBytes(path) : 0;

        std::string msg;
        if (success) {
            if (seconds > 0.0f && file_bytes > 0) {
                last_effective_goodput_bps_ = (8.0f * static_cast<float>(file_bytes)) / seconds;
                last_goodput_label_ = "RX file";
            }
            char buf[320];
            if (last_goodput_label_ == "RX file" && last_effective_goodput_bps_ > 0.0f) {
                snprintf(buf, sizeof(buf), "[FILE] Received: %s (%.1fs, %.2f kbps)",
                         path.c_str(), seconds, last_effective_goodput_bps_ / 1000.0f);
            } else {
                snprintf(buf, sizeof(buf), "[FILE] Received: %s (%.1fs)", path.c_str(), seconds);
            }
            msg = buf;
            last_received_file_ = path;
        } else {
            msg = "[FILE] Receive failed";
        }
        rx_log_.push_back(msg);
        if (rx_log_.size() > MAX_RX_LOG) {
            rx_log_.pop_front();
        }
    });
    ultra::gui::startupTrace("App", "protocol-callbacks-mid12");

    protocol_.setFileSentCallback([this](bool success, const std::string& error) {
        last_progress_milestone_ = 0;  // Reset for next transfer
        auto duration = std::chrono::steady_clock::now() - file_transfer_start_time_;
        float seconds = std::chrono::duration<float>(duration).count();

        std::string msg;
        if (success) {
            if (seconds > 0.0f && pending_file_tx_payload_bytes_ > 0) {
                last_effective_goodput_bps_ =
                    (8.0f * static_cast<float>(pending_file_tx_payload_bytes_)) / seconds;
                last_goodput_label_ = "TX file";
            }
            char buf[196];
            if (last_goodput_label_ == "TX file" && last_effective_goodput_bps_ > 0.0f) {
                snprintf(buf, sizeof(buf), "[FILE] Transfer complete (%.1fs, %.2f kbps)",
                         seconds, last_effective_goodput_bps_ / 1000.0f);
            } else {
                snprintf(buf, sizeof(buf), "[FILE] Transfer complete (%.1fs)", seconds);
            }
            msg = buf;
        } else {
            msg = "[FILE] Transfer failed: " + error;
        }
        pending_file_tx_payload_bytes_ = 0;
        rx_log_.push_back(msg);
        if (rx_log_.size() > MAX_RX_LOG) {
            rx_log_.pop_front();
        }
    });
    ultra::gui::startupTrace("App", "protocol-callbacks-exit");

    // Set receive directory from settings (defaults to Downloads folder)
    ultra::gui::startupTrace("App", "set-rx-dir-enter");
    protocol_.setReceiveDirectory(settings_.getReceiveDirectory());
    ultra::gui::startupTrace("App", "set-rx-dir-exit");

    // Configure waterfall display
    ultra::gui::startupTrace("App", "waterfall-config-enter");
    if (waterfall_) {
        waterfall_->setSampleRate(48000.0f);
        waterfall_->setFrequencyRange(0.0f, 3000.0f);
        waterfall_->setDynamicRange(-60.0f, 0.0f);
    }
    ultra::gui::startupTrace("App", "waterfall-config-exit");

    // Settings window callbacks
    ultra::gui::startupTrace("App", "settings-callbacks-enter");
    settings_window_.setCallsignChangedCallback([this](const std::string& call) {
        protocol_.setLocalCallsign(call);
        modem_.setLogPrefix(call);
        settings_.save();
    });

    settings_window_.setAudioResetCallback([this]() {
        if (radio_rx_enabled_) {
            stopRadioRx();
        }
        audio_.stopPlayback();
        audio_.stopCapture();
        audio_.closeInput();
        audio_.closeOutput();
        audio_.shutdown();
        audio_initialized_ = false;

        initAudio();
        if (audio_initialized_) {
            audio_.setOutputGain(settings_.tx_drive);
            std::string output_dev = getOutputDeviceName();
            audio_.openOutput(output_dev);
            audio_.startPlayback();
            startRadioRx();
        }
    });

    settings_window_.setClosedCallback([this]() {
        settings_.save();

        if (radio_rx_enabled_) {
            stopRadioRx();
        }
        audio_.stopPlayback();
        audio_.stopCapture();
        audio_.closeInput();
        audio_.closeOutput();

        if (audio_initialized_) {
            audio_.setOutputGain(settings_.tx_drive);
            std::string output_dev = getOutputDeviceName();
            audio_.openOutput(output_dev);
            audio_.startPlayback();
            startRadioRx();
        }
    });

    settings_window_.setFilterChangedCallback([this](bool enabled, float center, float bw, int taps) {
        FilterConfig filter_config;
        filter_config.enabled = enabled;
        filter_config.center_freq = center;
        filter_config.bandwidth = bw;
        filter_config.taps = taps;
        modem_.setFilterConfig(filter_config);
        settings_.save();
    });

    settings_window_.setReceiveDirChangedCallback([this](const std::string& dir) {
        protocol_.setReceiveDirectory(dir);
        settings_.save();
    });

    settings_window_.setExpertSettingsChangedCallback([this](uint8_t waveform, uint8_t modulation, uint8_t code_rate) {
        // Apply forced settings to protocol (used on next connect)
        protocol_.setPreferredMode(static_cast<protocol::WaveformMode>(waveform));
        protocol_.setForcedModulation(static_cast<Modulation>(modulation));
        protocol_.setForcedCodeRate(static_cast<CodeRate>(code_rate));
        settings_.save();
    });
    ultra::gui::startupTrace("App", "settings-callbacks-exit");

    // Apply initial expert settings from loaded config
    ultra::gui::startupTrace("App", "apply-expert-enter");
    protocol_.setPreferredMode(static_cast<protocol::WaveformMode>(settings_.forced_waveform));
    protocol_.setForcedModulation(static_cast<Modulation>(settings_.forced_modulation));
    protocol_.setForcedCodeRate(static_cast<CodeRate>(settings_.forced_code_rate));
    ultra::gui::startupTrace("App", "apply-expert-exit");

    // Apply initial filter settings from loaded config
    ultra::gui::startupTrace("App", "apply-filter-enter");
    FilterConfig initial_filter;
    initial_filter.enabled = settings_.filter_enabled;
    initial_filter.center_freq = settings_.filter_center;
    initial_filter.bandwidth = settings_.filter_bandwidth;
    initial_filter.taps = settings_.filter_taps;
    modem_.setFilterConfig(initial_filter);
    audio_.setOutputGain(settings_.tx_drive);
    ultra::gui::startupTrace("App", "apply-filter-exit");

    // Initialize virtual station only when simulator UI is shown and startup is not constrained.
    // In safe-startup mode we defer this until the user enables simulation.
    if (sim_ui_visible_ && !options_.safe_startup) {
        ultra::gui::startupTrace("App", "init-virtual-enter");
        initVirtualStation();
        ultra::gui::startupTrace("App", "init-virtual-exit");
    }

    // Auto-initialize audio on startup unless safe-startup mode is requested.
    // This avoids crashing on fragile audio stacks during process bring-up.
    if (!options_.safe_startup) {
        ultra::gui::startupTrace("App", "init-audio-enter");
        initAudio();
        if (audio_initialized_) {
            std::string output_dev = getOutputDeviceName();
            audio_.openOutput(output_dev);
            audio_.startPlayback();
            startRadioRx();
        }
        ultra::gui::startupTrace("App", "init-audio-exit");
    } else {
        guiLog("Safe startup enabled: deferred audio/simulator initialization");
    }
    ultra::gui::startupTrace("App", "ctor-body-exit");
}

App::~App() {
    // Stop simulator threads first
    stopSimulator();

    settings_.save();
    audio_.shutdown();

    // Write recording to file if -rec was enabled
    if (options_.record_audio && !recorded_samples_.empty()) {
        writeRecordingToFile();
    }
}

void App::writeRecordingToFile() {
    std::ofstream file(options_.record_path, std::ios::binary);
    if (file.is_open()) {
        file.write(reinterpret_cast<const char*>(recorded_samples_.data()),
                   recorded_samples_.size() * sizeof(float));
        guiLog("Recording saved: %s (%zu samples, %.1f seconds)",
               options_.record_path.c_str(),
               recorded_samples_.size(),
               recorded_samples_.size() / 48000.0f);
    } else {
        guiLog("ERROR: Failed to save recording to %s", options_.record_path.c_str());
    }
}

void App::initVirtualStation() {
    if (virtual_modem_) {
        return;
    }

    // Create virtual station's modem
    virtual_modem_ = std::make_unique<ModemEngine>();

    // Set up virtual station's protocol
    virtual_protocol_.setLocalCallsign(virtual_callsign_);
    virtual_modem_->setLogPrefix(virtual_callsign_);
    virtual_protocol_.setAutoAccept(true);  // Auto-accept incoming calls
    virtual_protocol_.setReceiveDirectory(settings_.getReceiveDirectory());  // Save files to same dir

    // Virtual station TX → queue for real-time streaming → our RX
    virtual_protocol_.setTxDataCallback([this](const Bytes& data) {
        guiLog("SIM: Virtual station TX %zu bytes", data.size());
        auto samples = virtual_modem_->transmit(data);

        // Add PTT noise once at start of transmission (100-300ms)
        std::uniform_int_distribution<size_t> ptt_dist(4800, 14400);
        size_t ptt_samples = ptt_dist(sim_rng_);

        float typical_rms = 0.1f;
        float snr_linear = std::pow(10.0f, simulation_snr_db_ / 10.0f);
        float noise_power = (typical_rms * typical_rms) / snr_linear;
        float noise_stddev = std::sqrt(noise_power);
        std::normal_distribution<float> noise_dist(0.0f, noise_stddev);

        std::vector<float> ptt_noise(ptt_samples);
        for (float& s : ptt_noise) s = noise_dist(sim_rng_);

        guiLog("SIM: Virtual modem produced %zu samples (+ %zu PTT noise), queuing for stream", samples.size(), ptt_samples);

        // Queue PTT noise + signal for real-time streaming
        std::lock_guard<std::mutex> lock(virtual_tx_pending_mutex_);
        virtual_tx_pending_.insert(virtual_tx_pending_.end(), ptt_noise.begin(), ptt_noise.end());
        virtual_tx_pending_.insert(virtual_tx_pending_.end(), samples.begin(), samples.end());
    });

    // Virtual station burst TX callback
    virtual_protocol_.setTransmitBurstCallback([this](const std::vector<Bytes>& frames) {
        guiLog("SIM: Virtual station burst TX %zu frames", frames.size());
        auto samples = virtual_modem_->transmitBurst(frames);
        guiLog("SIM: Virtual burst produced %zu samples", samples.size());
        std::lock_guard<std::mutex> lock(virtual_tx_pending_mutex_);
        virtual_tx_pending_.insert(virtual_tx_pending_.end(), samples.begin(), samples.end());
    });

    // Virtual modem RX → virtual protocol
    virtual_modem_->setRawDataCallback([this](const Bytes& data) {
        guiLog("SIM: Virtual modem decoded %zu bytes", data.size());
        // Use simulation SNR and fading from virtual modem
        // The virtual station sees the same channel as our station
        float fading = virtual_modem_->getFadingIndex();
        virtual_protocol_.setMeasuredSNR(simulation_snr_db_);
        virtual_protocol_.setChannelQuality(simulation_snr_db_, fading);
        virtual_protocol_.onRxData(data);
    });

    // Log virtual station events
    virtual_protocol_.setConnectionChangedCallback([this](protocol::ConnectionState state, const std::string& info) {
        guiLog("SIM: Virtual station connection state: %d (%s)", static_cast<int>(state), info.c_str());

        // Update virtual modem engine connection state
        bool connected = (state == protocol::ConnectionState::CONNECTED);
        virtual_modem_->setConnected(connected);

        std::string msg = "[SIM] ";
        switch (state) {
            case protocol::ConnectionState::CONNECTED:
                msg += "Virtual station connected";
                break;
            case protocol::ConnectionState::DISCONNECTED:
                msg += "Virtual station disconnected";
                // Reset waveform mode to OFDM when disconnected
                virtual_modem_->setWaveformMode(protocol::WaveformMode::OFDM_COX);
                break;
            default:
                return;  // Don't log intermediate states
        }
        rx_log_.push_back(msg);
        if (rx_log_.size() > MAX_RX_LOG) {
            rx_log_.pop_front();
        }
    });

    virtual_protocol_.setDataModeChangedCallback([this](Modulation mod, CodeRate rate, float snr_db, float peer_fading) {
        // Update virtual modem engine with new data mode
        virtual_modem_->setDataMode(mod, rate);

        // Show [SIM-MODE] line so user can see what the responder actually measured
        auto waveform = virtual_modem_->getWaveformMode();
        float local_fading = virtual_modem_->getFadingIndex();
        const char* local_quality = fadingToQuality(local_fading);
        bool peer_fading_valid = (peer_fading >= 0.0f);
        const char* peer_quality = peer_fading_valid ? fadingToQuality(peer_fading) : "n/a";
        char peer_fading_text[32];
        if (peer_fading_valid) {
            snprintf(peer_fading_text, sizeof(peer_fading_text), "%.2f %s", peer_fading, peer_quality);
        } else {
            snprintf(peer_fading_text, sizeof(peer_fading_text), "n/a");
        }
        const char* wf_name = waveformDisplayName(waveform);

        guiLog("SIM: Virtual MODE_CHANGE: %s %s %s (peer_snr=%.1f dB, peer_fading=%s, local_fading=%.2f %s)",
               wf_name, modulationToString(mod), codeRateToString(rate),
               snr_db, peer_fading_text,
               local_fading, local_quality);

        char buf[200];
        snprintf(buf, sizeof(buf),
                 "[SIM-MODE] %s %s %s (peer SNR=%d dB, peer fading=%s, local fading=%.2f %s)",
                 wf_name, modulationToString(mod), codeRateToString(rate),
                 static_cast<int>(snr_db), peer_fading_text,
                 local_fading, local_quality);
        rx_log_.push_back(buf);
        if (rx_log_.size() > MAX_RX_LOG) rx_log_.pop_front();
    });

    virtual_protocol_.setModeNegotiatedCallback([this](protocol::WaveformMode mode) {
        const char* mode_name = "OFDM";
        switch (mode) {
            case protocol::WaveformMode::MC_DPSK: mode_name = "MC-DPSK"; break;
            case protocol::WaveformMode::MFSK: mode_name = "MFSK"; break;
            case protocol::WaveformMode::OTFS_EQ: mode_name = "OTFS-EQ"; break;
            case protocol::WaveformMode::OTFS_RAW: mode_name = "OTFS-RAW"; break;
            default: mode_name = "OFDM"; break;
        }
        guiLog("SIM: Virtual WAVEFORM_CHANGE: %s", mode_name);
        // Update virtual modem engine with new waveform mode
        virtual_modem_->setWaveformMode(mode);
    });

    // Connect waveform fallback for virtual station
    virtual_protocol_.setConnectWaveformChangedCallback([this](protocol::WaveformMode mode) {
        const char* mode_name = (mode == protocol::WaveformMode::MFSK) ? "MFSK" : "DPSK";
        guiLog("SIM: Virtual CONNECT_WAVEFORM: Switching to %s", mode_name);
        virtual_modem_->setConnectWaveform(mode);
    });

    // Virtual station handshake confirmed callback
    virtual_protocol_.setHandshakeConfirmedCallback([this]() {
        guiLog("SIM: Virtual HANDSHAKE confirmed");
        virtual_modem_->setHandshakeComplete(true);
    });

    // Virtual station file transfer callbacks
    virtual_protocol_.setFileReceivedCallback([this](const std::string& path, bool success) {
        std::string msg = "[SIM] ";
        if (success) {
            msg += "Received file: " + path;
        } else {
            msg += "File receive failed";
        }
        rx_log_.push_back(msg);
        if (rx_log_.size() > MAX_RX_LOG) rx_log_.pop_front();
    });

    virtual_protocol_.setFileSentCallback([this](bool success, const std::string& error) {
        std::string msg = "[SIM] ";
        if (success) {
            msg += "File sent successfully";
        } else {
            msg += "File send failed: " + error;
        }
        rx_log_.push_back(msg);
        if (rx_log_.size() > MAX_RX_LOG) rx_log_.pop_front();
    });

    virtual_protocol_.setMessageReceivedCallback([this](const std::string& from, const std::string& text) {
        // Virtual station received our message - it could auto-reply here
        // For now, just log that it received the message
        guiLog("SIM: Virtual station received msg from %s: %s", from.c_str(), text.c_str());
    });

    // Virtual station PING TX callback
    virtual_protocol_.setPingTxCallback([this]() {
        guiLog("SIM: Virtual station TX PING");
        auto samples = virtual_modem_->transmitPing();
        if (!samples.empty()) {
            std::lock_guard<std::mutex> lock(virtual_tx_pending_mutex_);
            virtual_tx_pending_.insert(virtual_tx_pending_.end(), samples.begin(), samples.end());
        }
    });

    // Virtual station PING received callback - respond with PONG
    virtual_protocol_.setPingReceivedCallback([this]() {
        guiLog("SIM: Virtual station received PING, sending PONG");
        auto samples = virtual_modem_->transmitPong();
        if (!samples.empty()) {
            std::lock_guard<std::mutex> lock(virtual_tx_pending_mutex_);
            virtual_tx_pending_.insert(virtual_tx_pending_.end(), samples.begin(), samples.end());
        }
    });

    // Wire up virtual modem ping detection to virtual protocol
    virtual_modem_->setPingReceivedCallback([this](float snr) {
        guiLog("SIM: Virtual modem detected PING/PONG (SNR=%.1f dB)", snr);
        virtual_protocol_.onPingReceived();
    });

    guiLog("Virtual station initialized: callsign=%s", virtual_callsign_.c_str());
}

// ========================================
// Simplified Simulator (single thread model like cli_simulator)
// ========================================

std::vector<float> App::applyChannelEffects(const std::vector<float>& samples, int direction) {
    if (samples.empty()) return samples;

    std::vector<float> result = samples;

    // Select channel for this direction (independent fading per direction, like cli_simulator)
    auto& channel = (direction == 0) ? sim_channel_a_to_b_ : sim_channel_b_to_a_;

    // Apply fading/multipath if not AWGN
    // Use persistent channel so fading state evolves continuously across frames
    if (simulation_channel_type_ > 0) {
        // Recreate channels only when type changes (not per-frame)
        if (!channel || sim_channel_active_type_ != simulation_channel_type_) {
            sim::WattersonChannel::Config cfg;
            switch (simulation_channel_type_) {
                case 1:  // Good
                    cfg = sim::itu_r_f1487::good(simulation_snr_db_);
                    break;
                case 2:  // Moderate
                    cfg = sim::itu_r_f1487::moderate(simulation_snr_db_);
                    break;
                case 3:  // Poor
                    cfg = sim::itu_r_f1487::poor(simulation_snr_db_);
                    break;
                default:
                    cfg = sim::itu_r_f1487::good(simulation_snr_db_);
                    break;
            }
            // Disable noise in WattersonChannel - we'll add it with fixed reference below
            cfg.noise_enabled = false;
            // Use different seeds per direction (like cli_simulator: 42, 43)
            sim_channel_a_to_b_ = std::make_unique<sim::WattersonChannel>(cfg, 42);
            sim_channel_b_to_a_ = std::make_unique<sim::WattersonChannel>(cfg, 43);
            sim_channel_active_type_ = simulation_channel_type_;
        }

        SampleSpan input(result.data(), result.size());
        result = channel->process(input);
    } else if (sim_channel_a_to_b_ || sim_channel_b_to_a_) {
        // Switched to AWGN — release fading channels
        sim_channel_a_to_b_.reset();
        sim_channel_b_to_a_.reset();
        sim_channel_active_type_ = -1;
    }

    // Apply AWGN with fixed reference signal power (matches cli_simulator)
    // Using fixed reference ensures consistent SNR regardless of signal amplitude
    if (simulation_snr_db_ < 50.0f) {
        float snr_linear = std::pow(10.0f, simulation_snr_db_ / 10.0f);
        constexpr float REFERENCE_SIGNAL_POWER = 0.01f;  // Same as cli_simulator
        float noise_stddev = std::sqrt(REFERENCE_SIGNAL_POWER / snr_linear);
        std::normal_distribution<float> noise_dist(0.0f, noise_stddev);

        for (float& s : result) {
            s += noise_dist(sim_rng_);
        }
    }

    return result;
}

void App::startSimulator() {
    if (sim_thread_running_) return;
    if (!virtual_modem_) {
        guiLog("SIM: Cannot start simulator - virtual modem is not initialized");
        return;
    }

    guiLog("SIM: Starting simulator");

    // Switch both modems to synchronous mode — sim loop drives decode directly,
    // no separate decode thread. This prevents buffer overflows during LDPC decode
    // (same model as cli_simulator: feed + process in lockstep).
    modem_.setSynchronousMode(true);
    virtual_modem_->setSynchronousMode(true);

    sim_thread_running_ = true;
    sim_thread_ = std::thread(&App::simulationLoop, this);
}

void App::stopSimulator() {
    if (!sim_thread_running_) return;

    guiLog("SIM: Stopping simulator");
    sim_thread_running_ = false;

    if (sim_thread_.joinable()) sim_thread_.join();

    // Restore async decode mode for real audio operation
    modem_.setSynchronousMode(false);
    if (virtual_modem_) {
        virtual_modem_->setSynchronousMode(false);
    }

    // Clear buffers
    {
        std::lock_guard<std::mutex> lock(our_tx_pending_mutex_);
        our_tx_pending_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(virtual_tx_pending_mutex_);
        virtual_tx_pending_.clear();
    }

    guiLog("SIM: Simulator stopped");
}

void App::simulationLoop() {
    guiLog("SIM: Simulation loop started");

    constexpr size_t CHUNK_SIZE = 480;  // 10ms at 48kHz for waterfall display
    // Audio streaming: feed samples at REAL-TIME rate to match StreamingDecoder expectations
    // Loop runs every 10ms, feed 480 samples = 10ms of audio at 48kHz
    // This matches how real audio arrives and how cli_simulator works
    constexpr size_t SAMPLES_PER_TICK = 480;  // 10ms at 48kHz
    auto last_protocol_tick = std::chrono::steady_clock::now();

    // Intermediate buffers for gradual streaming
    std::vector<float> our_channel_buffer;     // Our TX -> channel -> virtual RX
    std::vector<float> virtual_channel_buffer; // Virtual TX -> channel -> our RX

    while (sim_thread_running_) {
        if (sim_drop_local_tx_requested_.exchange(false)) {
            size_t dropped_pending = 0;
            {
                std::lock_guard<std::mutex> lock(our_tx_pending_mutex_);
                dropped_pending = our_tx_pending_.size();
                our_tx_pending_.clear();
            }
            size_t dropped_in_flight = our_channel_buffer.size();
            our_channel_buffer.clear();
            tx_in_progress_ = false;
            guiLog("SIM: STOP TX NOW dropped %zu queued + %zu in-flight TX samples",
                   dropped_pending, dropped_in_flight);
        }

        bool a_to_b_active = false;  // Track activity per direction
        bool b_to_a_active = false;

        // === Our TX -> Channel buffer (queue all new samples) ===
        // Direction 0: our station -> virtual station (independent fading channel)
        {
            std::lock_guard<std::mutex> lock(our_tx_pending_mutex_);
            if (!our_tx_pending_.empty()) {
                guiLog("SIM: Queued %zu TX samples for streaming", our_tx_pending_.size());
                // Apply channel effects and queue for gradual streaming
                auto noisy = applyChannelEffects(our_tx_pending_, 0);
                our_channel_buffer.insert(our_channel_buffer.end(), noisy.begin(), noisy.end());
                // Show on waterfall
                for (size_t i = 0; i < our_tx_pending_.size(); i += CHUNK_SIZE) {
                    size_t chunk_size = std::min(CHUNK_SIZE, our_tx_pending_.size() - i);
                    if (waterfall_) {
                        waterfall_->addSamples(our_tx_pending_.data() + i, chunk_size);
                    }
                }
                // Record if enabled
                if (recording_enabled_) {
                    recorded_samples_.insert(recorded_samples_.end(), noisy.begin(), noisy.end());
                }
                our_tx_pending_.clear();
            }
        }

        // === Channel buffer -> Virtual RX (stream gradually) ===
        if (!our_channel_buffer.empty()) {
            a_to_b_active = true;
            size_t to_feed = std::min(SAMPLES_PER_TICK, our_channel_buffer.size());
            virtual_modem_->feedAudio(our_channel_buffer.data(), to_feed);
            virtual_modem_->processRxBuffer();
            our_channel_buffer.erase(our_channel_buffer.begin(), our_channel_buffer.begin() + to_feed);
        }

        // === Virtual TX -> Channel buffer (queue all new samples) ===
        // Direction 1: virtual station -> our station (independent fading channel)
        {
            std::lock_guard<std::mutex> lock(virtual_tx_pending_mutex_);
            if (!virtual_tx_pending_.empty()) {
                guiLog("SIM: Queued %zu RX samples for streaming", virtual_tx_pending_.size());
                // Apply channel effects and queue for gradual streaming
                auto noisy = applyChannelEffects(virtual_tx_pending_, 1);
                virtual_channel_buffer.insert(virtual_channel_buffer.end(), noisy.begin(), noisy.end());
                // Record if enabled
                if (recording_enabled_) {
                    recorded_samples_.insert(recorded_samples_.end(), noisy.begin(), noisy.end());
                }
                virtual_tx_pending_.clear();
            }
        }

        // === Channel buffer -> Our RX (stream gradually) ===
        if (!virtual_channel_buffer.empty()) {
            b_to_a_active = true;
            size_t to_feed = std::min(SAMPLES_PER_TICK, virtual_channel_buffer.size());
            modem_.feedAudio(virtual_channel_buffer.data(), to_feed);
            modem_.processRxBuffer();
            virtual_channel_buffer.erase(virtual_channel_buffer.begin(), virtual_channel_buffer.begin() + to_feed);
        }

        // Check if TX finished
        if (tx_in_progress_ && std::chrono::steady_clock::now() >= tx_end_time_) {
            tx_in_progress_ = false;
        }

        // Tick virtual protocol (~60Hz)
        auto now = std::chrono::steady_clock::now();
        auto protocol_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_protocol_tick).count();
        if (protocol_elapsed >= 16) {
            virtual_protocol_.tick(protocol_elapsed);
            last_protocol_tick = now;
        }

        // Evolve idle channels and feed noise to modems without active signal.
        // CRITICAL: Each direction's fading channel must evolve continuously by
        // processing silence, even when only the other direction has traffic.
        // Without this, the channel freezes during idle periods and
        // retransmissions hit the same deep fade repeatedly (frozen channel bug).
        {
            constexpr size_t IDLE_SAMPLES_PER_TICK = 480;  // 10ms at 48kHz

            if (!a_to_b_active) {
                // A→B channel idle: evolve fading and feed noise to virtual modem
                std::vector<float> silence(IDLE_SAMPLES_PER_TICK, 0.0f);
                auto noise = applyChannelEffects(silence, 0);
                virtual_modem_->feedAudio(noise);
                virtual_modem_->processRxBuffer();
            }
            if (!b_to_a_active) {
                // B→A channel idle: evolve fading and feed noise to our modem
                std::vector<float> silence(IDLE_SAMPLES_PER_TICK, 0.0f);
                auto noise = applyChannelEffects(silence, 1);
                modem_.feedAudio(noise);
                modem_.processRxBuffer();
            }
        }

        // Sleep 10ms to match real-time audio rate (480 samples / 48kHz = 10ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    guiLog("SIM: Simulation loop stopped");
}

void App::initAudio() {
    if (audio_initialized_) return;

    if (!audio_.initialize()) {
        return;
    }

    // Enumerate devices
    input_devices_ = audio_.getInputDevices();
    output_devices_ = audio_.getOutputDevices();

    // Populate settings window device lists
    settings_window_.input_devices = input_devices_;
    settings_window_.output_devices = output_devices_;

    audio_initialized_ = true;
}

void App::sendMessage() {
    // Not used in current implementation - messages sent via protocol
}

void App::onDataReceived(const std::string& text) {
    if (!text.empty()) {
        rx_log_.push_back("[RX] " + text);
        if (rx_log_.size() > MAX_RX_LOG) {
            rx_log_.pop_front();
        }
    }
}

void App::resetAdaptiveAdvisory() {
    std::lock_guard<std::mutex> lock(adapt_mutex_);
    adapt_snr_window_.clear();
    adapt_fading_window_.clear();
    adapt_candidate_valid_ = false;
    adapt_candidate_hits_ = 0;
    adapt_virtual_mode_valid_ = false;
    adapt_upgrade_hold_logged_ = false;
}

void App::updateAdaptiveAdvisory(float snr_db, float fading_index) {
    if (protocol_.getState() != protocol::ConnectionState::CONNECTED) {
        return;
    }
    if (!std::isfinite(snr_db) || !std::isfinite(fading_index)) {
        return;
    }
    std::lock_guard<std::mutex> lock(adapt_mutex_);

    adapt_snr_window_.push_back(snr_db);
    adapt_fading_window_.push_back(fading_index);
    if (adapt_snr_window_.size() > ADAPT_WINDOW_FRAMES) {
        adapt_snr_window_.pop_front();
    }
    if (adapt_fading_window_.size() > ADAPT_WINDOW_FRAMES) {
        adapt_fading_window_.pop_front();
    }
    if (adapt_snr_window_.size() < ADAPT_WINDOW_FRAMES ||
        adapt_fading_window_.size() < ADAPT_WINDOW_FRAMES) {
        return;
    }

    float avg_snr = 0.0f;
    for (float v : adapt_snr_window_) avg_snr += v;
    avg_snr /= static_cast<float>(adapt_snr_window_.size());

    float avg_fading = 0.0f;
    for (float v : adapt_fading_window_) avg_fading += v;
    avg_fading /= static_cast<float>(adapt_fading_window_.size());

    auto waveform = modem_.getWaveformMode();
    Modulation current_mod = protocol_.getDataModulation();
    CodeRate current_rate = protocol_.getDataCodeRate();

    if (!adapt_virtual_mode_valid_) {
        adapt_virtual_mode_valid_ = true;
        adapt_virtual_mod_ = current_mod;
        adapt_virtual_rate_ = current_rate;
        adapt_last_virtual_switch_ = std::chrono::steady_clock::now();
    }

    Modulation rec_mod = current_mod;
    CodeRate rec_rate = current_rate;
    protocol::recommendDataMode(avg_snr, waveform, rec_mod, rec_rate, avg_fading);

    Modulation eval_mod = adapt_virtual_mod_;
    CodeRate eval_rate = adapt_virtual_rate_;

    if (rec_mod == eval_mod && rec_rate == eval_rate) {
        adapt_candidate_valid_ = false;
        adapt_candidate_hits_ = 0;
        adapt_upgrade_hold_logged_ = false;
        return;
    }

    if (adapt_candidate_valid_ &&
        adapt_candidate_mod_ == rec_mod &&
        adapt_candidate_rate_ == rec_rate) {
        ++adapt_candidate_hits_;
    } else {
        adapt_candidate_valid_ = true;
        adapt_candidate_mod_ = rec_mod;
        adapt_candidate_rate_ = rec_rate;
        adapt_candidate_hits_ = 1;
    }

    bool is_upgrade = modeEfficiency(rec_mod, rec_rate) > modeEfficiency(eval_mod, eval_rate) + 0.05f;
    int required_windows = is_upgrade ? ADAPT_UPGRADE_WINDOWS : ADAPT_DOWNGRADE_WINDOWS;
    if (adapt_candidate_hits_ < required_windows) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (is_upgrade) {
        auto elapsed_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - adapt_last_virtual_switch_).count());
        int hold_remaining_ms = ADAPT_UPGRADE_HOLD_MS - elapsed_ms;
        if (hold_remaining_ms > 0) {
            if (!adapt_upgrade_hold_logged_ ||
                adapt_upgrade_hold_mod_ != rec_mod ||
                adapt_upgrade_hold_rate_ != rec_rate) {
                char hold_msg[260];
                snprintf(hold_msg, sizeof(hold_msg),
                         "[ADPT] Local improving conditions (SNR=%.1f dB, F.I.=%.2f): "
                         "hysteresis hold %.1fs before upgrade to %s %s",
                         avg_snr, avg_fading,
                         hold_remaining_ms / 1000.0f,
                         modulationToString(rec_mod), codeRateToString(rec_rate));
                guiLog("%s", hold_msg);
                rx_log_.push_back(hold_msg);
                if (rx_log_.size() > MAX_RX_LOG) {
                    rx_log_.pop_front();
                }
                adapt_upgrade_hold_logged_ = true;
                adapt_upgrade_hold_mod_ = rec_mod;
                adapt_upgrade_hold_rate_ = rec_rate;
            }
            return;
        }
    }

    adapt_upgrade_hold_logged_ = false;
    const char* direction = adaptationDirection(eval_mod, eval_rate, rec_mod, rec_rate);
    char msg[240];
    snprintf(msg, sizeof(msg),
             "[ADPT] Local %s conditions (SNR=%.1f dB, F.I.=%.2f): "
             "hysteresis allows switch %s %s -> %s %s",
             direction, avg_snr, avg_fading,
             modulationToString(eval_mod), codeRateToString(eval_rate),
             modulationToString(rec_mod), codeRateToString(rec_rate));

    guiLog("%s", msg);
    rx_log_.push_back(msg);
    if (rx_log_.size() > MAX_RX_LOG) {
        rx_log_.pop_front();
    }

    adapt_virtual_mod_ = rec_mod;
    adapt_virtual_rate_ = rec_rate;
    adapt_last_virtual_switch_ = now;
    adapt_candidate_valid_ = false;
    adapt_candidate_hits_ = 0;
}

void App::render() {
    // Keep output attenuation synchronized with the TX Drive slider.
    audio_.setOutputGain(settings_.tx_drive);

    // === DEBUG: Test signal keys (F1-F7) ===
    if (ImGui::IsKeyPressed(ImGuiKey_F1)) {
        auto tone = modem_.generateTestTone(1.0f);
        audio_.queueTxSamples(tone);
        rx_log_.push_back("[TEST] Sent 1500 Hz tone");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
        auto samples = modem_.transmitTestPattern(0);
        audio_.queueTxSamples(samples);
        rx_log_.push_back("[TEST] Sent pattern: ALL ZEROS (LDPC encoded)");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F3)) {
        auto samples = modem_.transmitTestPattern(1);
        audio_.queueTxSamples(samples);
        rx_log_.push_back("[TEST] Sent pattern: DEADBEEF (LDPC encoded)");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F7)) {
        const char* test_file = "tests/data/test_connect_data_sequence.f32";
        size_t injected = modem_.injectSignalFromFile(test_file);
        if (injected > 0) {
            rx_log_.push_back("[TEST] Injected " + std::to_string(injected) + " samples");
        } else {
            rx_log_.push_back("[TEST] Failed to inject signal");
        }
    }

    // Protocol engine tick (our protocol always ticks in main thread for UI responsiveness)
    // Virtual station's protocol ticks in its own thread (virtualProtocolLoop)
    uint32_t now = SDL_GetTicks();
    uint32_t elapsed = (last_tick_time_ == 0) ? 0 : (now - last_tick_time_);
    last_tick_time_ = now;

    if (elapsed > 0 && elapsed < 1000) {
        protocol_.tick(elapsed);
    }

    // Create main window
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("MainWindow", nullptr, window_flags);

    // Title bar
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "ProjectUltra");
    ImGui::SameLine();
    ImGui::TextDisabled("High-Speed HF Modem");

    // Settings button
    ImGui::SameLine(ImGui::GetWindowWidth() - 100);
    if (ImGui::SmallButton("Settings")) {
        settings_window_.open();
    }

    ImGui::Separator();

    // Main content area - Two column layout
    float content_height = ImGui::GetContentRegionAvail().y - 30;

    ImGui::BeginChild("ContentArea", ImVec2(0, content_height), false);

    float total_width = ImGui::GetContentRegionAvail().x;
    float left_width = total_width * 0.32f;  // Monitoring column

    // ========================================
    // LEFT COLUMN: Monitoring (Constellation + Channel Status + Waterfall)
    // ========================================
    ImGui::BeginChild("LeftPanel", ImVec2(left_width, 0), true);

    // Constellation diagram
    ImGui::BeginChild("ConstellationArea", ImVec2(0, 180), false);
    auto symbols = modem_.getConstellationSymbols();
    constellation_.render(symbols, config_.modulation);
    ImGui::EndChild();

    ImGui::Separator();

    // Compact Channel Status (horizontal layout)
    auto modem_stats = modem_.getStats();
    // In simulation mode, use the slider SNR (that's the actual channel quality)
    if (simulation_enabled_) {
        modem_stats.snr_db = simulation_snr_db_;
    }
    auto data_mod = protocol_.getDataModulation();
    auto data_rate = protocol_.getDataCodeRate();
    auto conn_stats = protocol_.getStats();
    renderCompactChannelStatus(modem_stats, data_mod, data_rate, conn_stats);

    ImGui::Separator();

    // Waterfall (uses remaining space)
    if (waterfall_) {
        waterfall_->render();
    } else {
        ImGui::TextDisabled("Waterfall disabled");
    }

    ImGui::EndChild();
    ImGui::SameLine();

    // ========================================
    // RIGHT COLUMN: Operating (Controls + Message Log)
    // ========================================
    ImGui::BeginChild("RightPanel", ImVec2(0, 0), true);
    renderOperateTab();
    ImGui::EndChild();

    ImGui::EndChild();

    // Status bar
    ImGui::Separator();
    auto mstats = modem_.getStats();
    const char* mode_str = simulation_enabled_ ? "SIMULATION" : (ptt_active_ ? "TX" : (radio_rx_enabled_ ? "RX" : "IDLE"));
    char goodput_text[96];
    if (last_effective_goodput_bps_ > 0.0f) {
        snprintf(goodput_text, sizeof(goodput_text), "%.2f kbps (%s)",
                 last_effective_goodput_bps_ / 1000.0f, last_goodput_label_.c_str());
    } else {
        snprintf(goodput_text, sizeof(goodput_text), "n/a");
    }
    ImGui::Text("Mode: %s | SNR: %.1f dB | TX: %d | RX: %d | PHY: %d bps | Goodput: %s",
                mode_str, mstats.snr_db, mstats.frames_sent, mstats.frames_received,
                mstats.throughput_bps, goodput_text);

    ImGui::End();

    // Render settings window
    if (settings_window_.isVisible() && settings_window_.input_devices.empty()) {
        if (!audio_.isInitialized()) {
            audio_.initialize();
        }
        settings_window_.input_devices = audio_.getInputDevices();
        settings_window_.output_devices = audio_.getOutputDevices();
    }
    settings_window_.render(settings_);

    // Render file browser
    if (file_browser_.render()) {
        const std::string& path = file_browser_.getSelectedPath();
        strncpy(file_path_buffer_, path.c_str(), sizeof(file_path_buffer_) - 1);
        file_path_buffer_[sizeof(file_path_buffer_) - 1] = '\0';
    }
}

std::string App::getInputDeviceName() const {
    if (strcmp(settings_.input_device, "Default") == 0 || settings_.input_device[0] == '\0') {
        return "";
    }
    return settings_.input_device;
}

std::string App::getOutputDeviceName() const {
    if (strcmp(settings_.output_device, "Default") == 0 || settings_.output_device[0] == '\0') {
        return "";
    }
    return settings_.output_device;
}

void App::startRadioRx() {
    if (!audio_initialized_ || simulation_enabled_) return;

    std::string input_dev = getInputDeviceName();
    if (!audio_.openInput(input_dev)) {
        return;
    }

    audio_.setRxCallback([this](const std::vector<float>& samples) {
        modem_.feedAudio(samples);
        if (waterfall_) {
            waterfall_->addSamples(samples.data(), samples.size());
        }
    });

    audio_.setLoopbackEnabled(false);
    audio_.startCapture();
    radio_rx_enabled_ = true;
}

void App::stopRadioRx() {
    audio_.stopCapture();
    audio_.closeInput();
    radio_rx_enabled_ = false;
}

void App::stopTxNow(const char* reason) {
    size_t dropped_audio = audio_.getTxQueueSize();
    if (dropped_audio > 0) {
        audio_.clearTxQueue();
    }

    size_t dropped_sim_pending = 0;
    {
        std::lock_guard<std::mutex> lock(our_tx_pending_mutex_);
        dropped_sim_pending = our_tx_pending_.size();
        our_tx_pending_.clear();
    }

    if (simulation_enabled_ && sim_thread_running_) {
        sim_drop_local_tx_requested_ = true;
    }

    tx_in_progress_ = false;
    tx_end_time_ = std::chrono::steady_clock::time_point{};

    // Return to RX immediately after aborting TX.
    if (!simulation_enabled_ && radio_rx_enabled_ && !ptt_active_) {
        audio_.setRxMuted(false);
        if (!audio_.isCapturing()) {
            audio_.startCapture();
        }
    }

    guiLog("STOP TX NOW: reason='%s', dropped_audio=%zu, dropped_sim_pending=%zu",
           reason, dropped_audio, dropped_sim_pending);

    rx_log_.push_back("[SYS] TX stopped immediately");
    if (rx_log_.size() > MAX_RX_LOG) {
        rx_log_.pop_front();
    }
}

void App::renderCompactChannelStatus(const LoopbackStats& stats, Modulation data_mod, CodeRate data_rate,
                                     const protocol::ConnectionStats& conn_stats) {
    // Compact horizontal Channel Status display
    ImGui::BeginChild("ChannelStatus", ImVec2(0, 140), false);

    auto conn_state = protocol_.getState();

    // Row 1: Connection state + Channel Quality (only when connected) + SNR bar
    if (conn_state == protocol::ConnectionState::DISCONNECTED) {
        // Not connected - show idle state
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "IDLE");
        ImGui::SameLine();
        ImGui::TextDisabled("[Standby]");
        ImGui::SameLine();
        ImGui::Text("SNR:");
        ImGui::SameLine();
        // Empty SNR bar
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::ProgressBar(0.0f, ImVec2(-1, 16), "-- dB");
        ImGui::PopStyleColor();
    } else if (conn_state == protocol::ConnectionState::CONNECTING) {
        // Connecting - show our outgoing mode, no channel quality yet
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "CALL");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[Connecting...]");
        ImGui::SameLine();
        ImGui::Text("SNR:");
        ImGui::SameLine();
        // Animated/pulsing bar to show activity
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::ProgressBar(0.3f, ImVec2(-1, 16), "awaiting...");
        ImGui::PopStyleColor();
    } else if (conn_state == protocol::ConnectionState::DISCONNECTING) {
        // Disconnecting
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "DISC");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "[Disconnecting...]");
        ImGui::SameLine();
        ImGui::Text("SNR:");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1.0f, 0.5f, 0.2f, 1.0f));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::ProgressBar(0.5f, ImVec2(-1, 16), "closing...");
        ImGui::PopStyleColor();
    } else {
        // CONNECTED - show remote mode (their view) AND our SNR (our view)
        // Row 1: Remote's negotiated mode + implied channel condition
        auto waveform = modem_.getWaveformMode();
        const char* wf_str = waveformDisplayName(waveform);
        ImVec4 wf_color = (waveform == protocol::WaveformMode::OFDM_COX) ? ImVec4(0.4f, 0.8f, 1.0f, 1.0f) :
                          (waveform == protocol::WaveformMode::MC_DPSK) ? ImVec4(0.8f, 0.8f, 0.4f, 1.0f) :
                          (waveform == protocol::WaveformMode::MFSK) ? ImVec4(0.8f, 0.4f, 0.8f, 1.0f) :
                                                                       ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        ImGui::Text("RX:");
        ImGui::SameLine();
        ImGui::TextColored(wf_color, "%s", wf_str);
        ImGui::SameLine();

        // Show mode-appropriate settings and throughput
        float throughput_bps = 0.0f;
        ImVec4 mode_quality_color;
        const char* mode_quality = "Good";

        // Get actual channel quality from fading measurement
        float fading = modem_.getFadingIndex();
        mode_quality = fadingToQualityWithColor(fading, mode_quality_color);

        if (waveform == protocol::WaveformMode::MC_DPSK) {
            // For MC-DPSK, just show carrier count (DQPSK R1/4 is implicit)
            int carriers = modem_.getMCDPSKCarriers();
            throughput_bps = modem_.getMCDPSKThroughput();
            ImGui::Text("%d carriers", carriers);
        } else {
            // For OFDM modes, show negotiated modulation/rate
            ImGui::Text("%s %s", modulationToString(data_mod), codeRateToString(data_rate));
            throughput_bps = config_.getTheoreticalThroughput(data_mod, data_rate);
        }
        ImGui::SameLine();
        ImGui::TextColored(mode_quality_color, "[%s]", mode_quality);
        ImGui::TextDisabled("PHY ~%.1f kbps", throughput_bps / 1000.0f);

        // Row 2: Our SNR measurement
        ImVec4 sync_color = stats.synced ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
        ImGui::TextColored(sync_color, "%s", stats.synced ? "SYNC" : "----");
        ImGui::SameLine();
        ImGui::Text("SNR:");
        ImGui::SameLine();

        // SNR bar - color indicates signal strength
        float snr_normalized = stats.snr_db / 40.0f;
        snr_normalized = std::max(0.0f, std::min(1.0f, snr_normalized));
        // Color based on SNR value (green=good signal, yellow=moderate, red=weak)
        ImVec4 snr_color;
        if (stats.snr_db >= 15.0f) {
            snr_color = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);  // Green
        } else if (stats.snr_db >= 5.0f) {
            snr_color = ImVec4(0.8f, 0.8f, 0.0f, 1.0f);  // Yellow
        } else {
            snr_color = ImVec4(1.0f, 0.4f, 0.2f, 1.0f);  // Orange-red
        }
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, snr_color);
        char snr_text[16];
        snprintf(snr_text, sizeof(snr_text), "%.1f dB", stats.snr_db);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::ProgressBar(snr_normalized, ImVec2(-1, 16), snr_text);
        ImGui::PopStyleColor();
    }

    // Row 2: Waveform + Mode (for non-connected states only)
    if (conn_state == protocol::ConnectionState::DISCONNECTED) {
        // Show default connect waveform (DPSK R1/4)
        auto connect_wf = protocol_.getConnectWaveform();
        const char* wf_str = waveformDisplayName(connect_wf);
        ImGui::TextDisabled("%s R1/4 (default)", wf_str);
    } else if (conn_state == protocol::ConnectionState::CONNECTING) {
        // Show actual waveform being used for connection attempt (always R1/4)
        auto connect_wf = protocol_.getConnectWaveform();
        const char* wf_str = waveformDisplayName(connect_wf);
        ImVec4 wf_color = (connect_wf == protocol::WaveformMode::OFDM_COX) ? ImVec4(0.4f, 0.8f, 1.0f, 1.0f) :
                          (connect_wf == protocol::WaveformMode::MC_DPSK) ? ImVec4(0.8f, 0.8f, 0.4f, 1.0f) :
                          (connect_wf == protocol::WaveformMode::MFSK) ? ImVec4(0.8f, 0.4f, 0.8f, 1.0f) :
                                                                         ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        ImGui::TextColored(wf_color, "%s R1/4 (calling)", wf_str);
    }
    // Connected state already shows mode in Row 1

    // Row 3: Modem frame stats
    if (stats.frames_sent > 0 || stats.frames_received > 0) {
        ImGui::Text("TX:%d RX:%d", stats.frames_sent, stats.frames_received);
        if (stats.frames_failed > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(%d fail)", stats.frames_failed);
        }
    } else if (conn_state == protocol::ConnectionState::DISCONNECTED) {
        ImGui::TextDisabled("Ready to connect");
    }

    // Row 4: ARQ health (control-path reliability visibility)
    const auto& arq = conn_stats.arq;
    bool has_arq_activity =
        arq.frames_sent > 0 || arq.frames_received > 0 || arq.acks_sent > 0 || arq.acks_received > 0 ||
        arq.retransmissions > 0 || arq.timeouts > 0;
    if (has_arq_activity) {
        ImVec4 arq_color = (arq.failed > 0 || arq.timeouts > 0) ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) :
                           (arq.retransmissions > 0) ? ImVec4(0.9f, 0.8f, 0.3f, 1.0f) :
                           ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
        char arq_line[256];
        snprintf(arq_line, sizeof(arq_line),
                 "ARQ retx:%d to:%d fast:%d probe:%d nack:%d dupACK:%d",
                 arq.retransmissions, arq.timeouts, arq.retransmissions_fast_hole,
                 arq.retransmissions_hole_probe, arq.retransmissions_nack,
                 arq.duplicate_acks_ignored);
        ImGui::TextColored(arq_color, "%s", arq_line);
        if (arq.failed > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "fail:%d", arq.failed);
        }
    }

    ImGui::EndChild();
}

void App::renderOperateTab() {
    // Calculate available height for layout
    float total_height = ImGui::GetContentRegionAvail().y;

    // ========================================
    // TOP SECTION: Connection Controls (compact)
    // ========================================

    // Virtual Station Simulator (only visible with -sim flag, collapsible)
    if (sim_ui_visible_) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.4f, 0.6f, 1.0f));
        if (ImGui::CollapsingHeader("Simulator", ImGuiTreeNodeFlags_None)) {
            ImGui::PopStyleColor();
            if (ImGui::Checkbox("Enable", &simulation_enabled_)) {
                guiLog("Simulation checkbox toggled: %d", simulation_enabled_);
                if (simulation_enabled_) {
                    if (!virtual_modem_) {
                        initVirtualStation();
                    }
                    if (!virtual_modem_) {
                        simulation_enabled_ = false;
                        rx_log_.push_back("[SIM] Failed to initialize virtual station");
                        if (rx_log_.size() > MAX_RX_LOG) rx_log_.pop_front();
                        return;
                    }
                    if (radio_rx_enabled_) { stopRadioRx(); audio_.stopPlayback(); }
                    guiLog("Simulation ENABLED - virtual station: %s", virtual_callsign_.c_str());
                    rx_log_.push_back("[SIM] Simulation enabled - connect to '" + virtual_callsign_ + "'");
                    modem_.reset(); virtual_modem_->reset(); virtual_protocol_.reset();
                    if (options_.record_audio) {
                        recording_enabled_ = true; recorded_samples_.clear();
                        rx_log_.push_back("[REC] Recording enabled");
                    }
                    // Start simulation threads for realistic audio streaming
                    startSimulator();
                } else {
                    // Stop simulation threads
                    stopSimulator();
                    rx_log_.push_back("[SIM] Simulation disabled");
                    if (recording_enabled_) {
                        recording_enabled_ = false;
                        if (!recorded_samples_.empty()) {
                            writeRecordingToFile();
                            rx_log_.push_back("[REC] Saved: " + options_.record_path);
                        }
                    }
                    modem_.reset();
                    if (audio_initialized_) {
                        audio_.openOutput(getOutputDeviceName());
                        audio_.startPlayback(); startRadioRx();
                    }
                }
                if (rx_log_.size() > MAX_RX_LOG) rx_log_.pop_front();
            }
            if (simulation_enabled_) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "'%s' active", virtual_callsign_.c_str());
                if (recording_enabled_) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "[REC %.1fs]", recorded_samples_.size() / 48000.0f);
                }
                ImGui::SetNextItemWidth(100);
                ImGui::SliderFloat("SNR", &simulation_snr_db_, 0.0f, 40.0f, "%.0f dB");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90);
                const char* channel_types[] = {"AWGN", "Good", "Moderate", "Poor"};
                ImGui::Combo("##Channel", &simulation_channel_type_, channel_types, 4);
            }
        } else {
            ImGui::PopStyleColor();
        }
    }

    // Audio initialization (only when not in simulation)
    if (!simulation_enabled_ && !audio_initialized_) {
        if (ImGui::Button("Initialize Audio", ImVec2(-1, 28))) {
            initAudio();
        }
        return;
    }

    // ========================================
    // Connection Row: Callsign input + buttons
    // ========================================
    bool has_callsign = strlen(settings_.callsign) >= 3;
    auto conn_state = protocol_.getState();

    // Status line
    if (!simulation_enabled_ && !radio_rx_enabled_) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "OFFLINE");
        ImGui::SameLine();
        if (ImGui::SmallButton("Start RX")) {
            if (!audio_initialized_) initAudio();
            audio_.openOutput(getOutputDeviceName());
            audio_.startPlayback();
            startRadioRx();
        }
    } else {
        ImVec4 state_color;
        const char* state_icon;
        switch (conn_state) {
            case protocol::ConnectionState::CONNECTED:
                state_color = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
                state_icon = "CONNECTED";
                break;
            case protocol::ConnectionState::CONNECTING:
                state_color = ImVec4(1.0f, 1.0f, 0.2f, 1.0f);
                state_icon = "CONNECTING...";
                break;
            case protocol::ConnectionState::DISCONNECTING:
                state_color = ImVec4(1.0f, 1.0f, 0.2f, 1.0f);
                state_icon = "DISCONNECTING...";
                break;
            default:
                state_color = ImVec4(0.3f, 0.8f, 1.0f, 1.0f);
                state_icon = simulation_enabled_ ? "SIMULATION" : "LISTENING";
                break;
        }
        ImGui::TextColored(state_color, "%s", state_icon);
        if (conn_state == protocol::ConnectionState::CONNECTED) {
            ImGui::SameLine();
            ImGui::Text("to %s", protocol_.getRemoteCallsign().c_str());
        }
    }

    // My callsign
    if (has_callsign) {
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 100);
        ImGui::TextDisabled("My: %s", settings_.callsign);
    }

    // Connect to row
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("##remotecall", remote_callsign_, sizeof(remote_callsign_),
                     ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();

    float btn_w = 80;
    ImGui::BeginDisabled(conn_state != protocol::ConnectionState::DISCONNECTED ||
                         !has_callsign || strlen(remote_callsign_) < 3);
    if (ImGui::Button("Connect", ImVec2(btn_w, 0))) {
        guiLog("Connect clicked: simulation=%d, remote='%s'", simulation_enabled_, remote_callsign_);
        if (!simulation_enabled_ && !radio_rx_enabled_) {
            if (!audio_initialized_) initAudio();
            audio_.openOutput(getOutputDeviceName());
            audio_.startPlayback();
            startRadioRx();
        }
        protocol_.connect(remote_callsign_);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGui::BeginDisabled(conn_state == protocol::ConnectionState::DISCONNECTED);
    if (ImGui::Button("Disconnect", ImVec2(btn_w, 0))) {
        guiLog("DISCONNECT BUTTON: Pressed, modem connected_=%d, waveform_mode_=%d",
               modem_.isConnected() ? 1 : 0, static_cast<int>(modem_.getWaveformMode()));
        protocol_.disconnect();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.24f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.68f, 0.14f, 0.14f, 1.0f));
    if (ImGui::Button("STOP TX", ImVec2(110, 0))) {
        stopTxNow("operator_button");
    }
    ImGui::PopStyleColor(3);

    // Stop button for real audio
    if (!simulation_enabled_ && radio_rx_enabled_) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop RX")) {
            stopRadioRx();
            audio_.stopPlayback();
            audio_.closeOutput();
        }
    }

    // Incoming call notification
    if (!pending_incoming_call_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f),
                           "Incoming from %s!", pending_incoming_call_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Accept")) { protocol_.acceptCall(); pending_incoming_call_.clear(); }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reject")) { protocol_.rejectCall(); pending_incoming_call_.clear(); }
    }

    // Audio level meter (compact, only when RX active)
    if (!simulation_enabled_ && radio_rx_enabled_) {
        float input_level = audio_.getInputLevel();
        float input_db = (input_level > 0.0001f) ? 20.0f * log10f(input_level) : -80.0f;
        float level_normalized = (input_db + 60.0f) / 60.0f;
        level_normalized = std::max(0.0f, std::min(1.0f, level_normalized));
        ImVec4 level_color = (level_normalized > 0.8f) ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f) :
                             (level_normalized > 0.5f) ? ImVec4(1.0f, 1.0f, 0.3f, 1.0f) :
                                                         ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, level_color);
        ImGui::ProgressBar(level_normalized, ImVec2(100, 14), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("%.0fdB", input_db);
        if (modem_.isSynced()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "SIGNAL");
        }
    }

    ImGui::Separator();

    // ========================================
    // MESSAGE LOG (takes most of the space)
    // ========================================
    ImGui::Text("Message Log");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) rx_log_.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        std::string all_log;
        for (const auto& msg : rx_log_) all_log += msg + "\n";
        ImGui::SetClipboardText(all_log.c_str());
    }
    ImGui::SameLine();
    auto mstats = modem_.getStats();
    ImGui::TextDisabled("TX:%d RX:%d", mstats.frames_sent, mstats.frames_received);

    // Calculate remaining height for message log (leave space for TX and file transfer)
    float bottom_section_height = 130;  // TX input + File transfer
    float log_height = ImGui::GetContentRegionAvail().y - bottom_section_height;
    if (log_height < 100) log_height = 100;  // Minimum height

    ImGui::BeginChild("RXLogRadio", ImVec2(-1, log_height), true);
    for (const auto& msg : rx_log_) {
        ImVec4 color(0.7f, 0.7f, 0.7f, 1.0f);
        if (msg.size() >= 4 && msg.substr(0, 4) == "[TX]") {
            color = ImVec4(0.5f, 0.8f, 1.0f, 1.0f);
        } else if (msg.size() >= 3 && msg.substr(0, 3) == "[RX") {
            color = ImVec4(0.5f, 1.0f, 0.5f, 1.0f);
        } else if (msg.size() >= 4 && msg.substr(0, 4) == "[SIM") {
            color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
        } else if (msg.size() >= 4 && msg.substr(0, 4) == "[SYS") {
            color = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
        } else if (msg.find("[FAILED]") != std::string::npos) {
            color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", msg.c_str());
        ImGui::PopStyleColor();
    }
    if (!rx_log_.empty()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    // ========================================
    // BOTTOM SECTION: TX Input + File Transfer
    // ========================================
    ImGui::Separator();

    // TX Message Input
    if (tx_in_progress_ && audio_.isTxQueueEmpty()) {
        tx_in_progress_ = false;
        if (!ptt_active_ && !simulation_enabled_) {
            audio_.setRxMuted(false);  // Re-enable RX callback
            audio_.startCapture();
        }
    }

    bool can_send = !tx_in_progress_ && strlen(tx_text_buffer_) > 0 &&
                    protocol_.isConnected() && protocol_.isReadyToSend();

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90);
    bool send = ImGui::InputText("##txinput", tx_text_buffer_, sizeof(tx_text_buffer_),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();

    ImVec4 send_color = can_send ? ImVec4(0.3f, 0.6f, 0.3f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, send_color);
    ImGui::BeginDisabled(!can_send);
    if (ImGui::Button("Send##msg", ImVec2(80, 0)) || (send && can_send)) {
        std::string text(tx_text_buffer_);
        if (protocol_.sendMessage(text)) {
            rx_log_.push_back("[TX] " + text);
            if (rx_log_.size() > MAX_RX_LOG) rx_log_.pop_front();
            tx_text_buffer_[0] = '\0';
        }
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor();

    // File Transfer (compact row)
    if (protocol_.isFileTransferInProgress()) {
        auto progress = protocol_.getFileProgress();
        ImGui::TextColored(progress.is_sending ? ImVec4(0.5f, 0.8f, 1.0f, 1.0f) : ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
            "%s: %s", progress.is_sending ? "TX" : "RX", progress.filename.c_str());
        ImGui::SameLine();
        ImGui::ProgressBar(progress.percentage() / 100.0f, ImVec2(100, 16));
        ImGui::SameLine();
        // Show bytes transferred / total with appropriate units
        if (progress.total_bytes >= 1024) {
            ImGui::Text("%.1f/%.1f KB (%.0f%%)",
                progress.transferred_bytes / 1024.0f,
                progress.total_bytes / 1024.0f,
                progress.percentage());
        } else {
            ImGui::Text("%u/%u B (%.0f%%)",
                progress.transferred_bytes, progress.total_bytes,
                progress.percentage());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) protocol_.cancelFileTransfer();
    } else {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 160);
        ImGui::InputText("##filepath", file_path_buffer_, sizeof(file_path_buffer_));
        ImGui::SameLine();
        if (ImGui::Button("Browse", ImVec2(60, 0))) {
            file_browser_.setTitle("Select File");
            file_browser_.open();
        }
        ImGui::SameLine();
        bool can_send_file = protocol_.isConnected() && protocol_.isReadyToSend() &&
                             strlen(file_path_buffer_) > 0;
        ImGui::BeginDisabled(!can_send_file);
        if (ImGui::Button("Send##file", ImVec2(60, 0))) {
            last_progress_milestone_ = 0;  // Reset milestone tracker
            file_transfer_start_time_ = std::chrono::steady_clock::now();  // Start timing
            uint32_t file_bytes = safeFileSizeBytes(file_path_buffer_);
            if (protocol_.sendFile(file_path_buffer_)) {
                pending_file_tx_payload_bytes_ = file_bytes;
                rx_log_.push_back("[FILE] Sending: " + std::string(file_path_buffer_));
            } else {
                pending_file_tx_payload_bytes_ = 0;
                rx_log_.push_back("[FILE] Failed to start transfer");
            }
            if (rx_log_.size() > MAX_RX_LOG) rx_log_.pop_front();
        }
        ImGui::EndDisabled();
    }
}

} // namespace gui
} // namespace ultra
