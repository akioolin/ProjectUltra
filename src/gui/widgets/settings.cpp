#include "settings.hpp"
#include "imgui.h"
#include "gui/startup_trace.hpp"
#ifdef ULTRA_HAVE_LIBHAMLIB
#include "ptt/hamlib_rig_list.hpp"
#endif
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <windows.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define MKDIR(path) mkdir(path, 0755)
#endif

namespace ultra {
namespace gui {

AppSettings::AppSettings() {
    startupTrace("AppSettings", "ctor");
}

// Get default settings file path
std::string AppSettings::getDefaultPath() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata) {
        return std::string(appdata) + "\\ProjectUltra\\settings.ini";
    }
    return "settings.ini";
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.config/ultra/settings.ini";
    }
    return "settings.ini";
#endif
}

// Get platform-specific Downloads folder
std::string AppSettings::getDefaultDownloadsPath() {
#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        return std::string(userprofile) + "\\Downloads";
    }
    return ".";
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/Downloads";
    }
    return ".";
#endif
}

// Get effective receive directory (default to Downloads if not set)
std::string AppSettings::getReceiveDirectory() const {
    if (receive_directory[0] != '\0') {
        return std::string(receive_directory);
    }
    return getDefaultDownloadsPath();
}

// Helper to create directory if it doesn't exist
static void ensureDirectory(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        std::string dir = path.substr(0, pos);
        // Create parent directories recursively
        for (size_t i = 0; i < dir.size(); i++) {
            if (dir[i] == '/' || dir[i] == '\\') {
                std::string subdir = dir.substr(0, i);
                if (!subdir.empty()) {
                    MKDIR(subdir.c_str());
                }
            }
        }
        MKDIR(dir.c_str());
    }
}

static void copyBounded(char* dst, size_t dst_size, const std::string& value) {
    if (!dst || dst_size == 0) {
        return;
    }
    std::strncpy(dst, value.c_str(), dst_size - 1);
    dst[dst_size - 1] = '\0';
}

template <size_t N>
static size_t boundedCStringLen(const char (&buf)[N]) {
    const void* term = std::memchr(buf, '\0', N);
    return term ? static_cast<size_t>(static_cast<const char*>(term) - buf) : N;
}

static GuiPttMode normalizePttMode(int mode) {
    switch (static_cast<GuiPttMode>(mode)) {
        case GuiPttMode::SerialRTS:
        case GuiPttMode::SerialDTR:
        case GuiPttMode::Cat:
        case GuiPttMode::HamlibBuiltin:
            return static_cast<GuiPttMode>(mode);
        case GuiPttMode::None:
        default:
            return GuiPttMode::None;
    }
}

static const char* pttModeToConfigString(GuiPttMode mode) {
    switch (mode) {
        case GuiPttMode::SerialRTS: return "serial_rts";
        case GuiPttMode::SerialDTR: return "serial_dtr";
        case GuiPttMode::Cat: return "cat";
        case GuiPttMode::HamlibBuiltin: return "hamlib_builtin";
        case GuiPttMode::None:
        default: return "none";
    }
}

static GuiPttMode parsePttModeString(const std::string& value) {
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (v == "serial_rts" || v == "serial-rts" || v == "rts") {
        return GuiPttMode::SerialRTS;
    }
    if (v == "serial_dtr" || v == "serial-dtr" || v == "dtr") {
        return GuiPttMode::SerialDTR;
    }
    if (v == "cat" || v == "hamlib" || v == "rigctld") {
        return GuiPttMode::Cat;
    }
    if (v == "hamlib_builtin" || v == "hamlib-built-in" || v == "builtin" ||
        v == "built_in" || v == "direct") {
        return GuiPttMode::HamlibBuiltin;
    }
    if (v == "1") {
        return GuiPttMode::SerialRTS;
    }
    if (v == "2") {
        return GuiPttMode::SerialDTR;
    }
    if (v == "3") {
        return GuiPttMode::Cat;
    }
    if (v == "4") {
        return GuiPttMode::HamlibBuiltin;
    }
    return GuiPttMode::None;
}

static int normalizeHamlibPttMethod(int method) {
    if (method < 0 || method > 3) {
        return 1;
    }
    return method;
}

static const char* hamlibPttMethodToConfigString(int method) {
    switch (normalizeHamlibPttMethod(method)) {
        case 0: return "vox";
        case 1: return "cat";
        case 2: return "dtr";
        case 3: return "rts";
        default: return "cat";
    }
}

static int parseHamlibPttMethodString(const std::string& value) {
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (v == "vox" || v == "0") return 0;
    if (v == "cat" || v == "rig" || v == "1") return 1;
    if (v == "dtr" || v == "2") return 2;
    if (v == "rts" || v == "3") return 3;
    return 1;
}

static std::vector<std::string> detectSerialPorts() {
    std::vector<std::string> ports;
#ifdef _WIN32
    // Enumerate the registry path Windows populates with currently-connected
    // serial devices (USB-serial adapters, built-in UARTs, BT-SPP, etc.).
    // Each value's data is the friendly device name (e.g. "COM3"). Same
    // approach common radio applications use. Falls back to nothing on failure;
    // the operator can still type a port name into the text input.
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0,
                      KEY_READ,
                      &hKey) == ERROR_SUCCESS) {
        DWORD index = 0;
        for (;;) {
            char value_name[256];
            DWORD value_name_len = sizeof(value_name);
            DWORD type = 0;
            BYTE data[256];
            DWORD data_len = sizeof(data);
            LONG rc = RegEnumValueA(hKey, index, value_name, &value_name_len,
                                    nullptr, &type, data, &data_len);
            if (rc == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (rc != ERROR_SUCCESS) {
                break;
            }
            if (type == REG_SZ && data_len > 0) {
                size_t len = data_len;
                // Strip trailing NUL if present.
                while (len > 0 && data[len - 1] == '\0') {
                    --len;
                }
                if (len > 0) {
                    ports.emplace_back(reinterpret_cast<const char*>(data), len);
                }
            }
            ++index;
        }
        RegCloseKey(hKey);
    }
#else
    const char* prefixes[] = {
#ifdef __APPLE__
        "/dev/cu.",
        "/dev/tty."
#else
        "/dev/ttyUSB",
        "/dev/ttyACM",
        "/dev/serial/by-id/"
#endif
    };

    for (const char* prefix : prefixes) {
        std::filesystem::path base(prefix);
        std::filesystem::path dir = base.parent_path();
        const std::string stem = base.filename().string();
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) {
                break;
            }
            const std::string path = entry.path().string();
            if (path.rfind(prefix, 0) == 0 ||
                (!stem.empty() && entry.path().filename().string().rfind(stem, 0) == 0)) {
                ports.push_back(path);
            }
        }
    }
#endif
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    return ports;
}

// Save settings to INI file
bool AppSettings::save(const std::string& path) const {
    std::string filepath = path.empty() ? getDefaultPath() : path;
    ensureDirectory(filepath);

    std::ofstream file(filepath);
    if (!file.is_open()) {
        return false;
    }

    file << "[Station]\n";
    file << "callsign=" << callsign << "\n";
    file << "grid_square=" << grid_square << "\n";
    file << "name=" << name << "\n";

    file << "\n[Radio]\n";
    const GuiPttMode mode = normalizePttMode(ptt_mode);
    const bool serial_ptt = mode == GuiPttMode::SerialRTS || mode == GuiPttMode::SerialDTR;
    const int legacy_line = mode == GuiPttMode::SerialDTR ? 0 : 1;
    file << "rig_model=" << rig_model << "\n";
    file << "rig_port=" << ptt_serial_port << "\n";
    file << "rig_baud=" << ptt_serial_baud << "\n";
    file << "use_cat_ptt=" << (serial_ptt ? "1" : "0") << "\n";
    file << "ptt_serial_line=" << legacy_line << "\n";
    file << "ptt_mode=" << pttModeToConfigString(mode) << "\n";
    file << "ptt_serial_port=" << ptt_serial_port << "\n";
    file << "ptt_serial_baud=" << ptt_serial_baud << "\n";
    file << "ptt_invert=" << (ptt_invert ? "1" : "0") << "\n";
    file << "ptt_cat_host=" << ptt_cat_host << "\n";
    file << "ptt_cat_port=" << ptt_cat_port << "\n";
    file << "ptt_hamlib_model_id=" << ptt_hamlib_model_id << "\n";
    file << "ptt_hamlib_model=" << ptt_hamlib_model << "\n";
    file << "ptt_hamlib_port=" << ptt_hamlib_port << "\n";
    file << "ptt_hamlib_baud=" << ptt_hamlib_baud << "\n";
    file << "ptt_hamlib_method=" << hamlibPttMethodToConfigString(ptt_hamlib_method) << "\n";

    file << "\n[Audio]\n";
    file << "input_device=" << input_device << "\n";
    file << "output_device=" << output_device << "\n";
    file << "tx_delay_ms=" << tx_delay_ms << "\n";
    file << "tx_tail_ms=" << tx_tail_ms << "\n";
    file << "tx_drive=" << tx_drive << "\n";

    file << "\n[Filter]\n";
    file << "enabled=" << (filter_enabled ? "1" : "0") << "\n";
    file << "center=" << filter_center << "\n";
    file << "bandwidth=" << filter_bandwidth << "\n";
    file << "taps=" << filter_taps << "\n";

    file << "\n[FileTransfer]\n";
    file << "receive_directory=" << receive_directory << "\n";

    file << "\n[Expert]\n";
    file << "forced_waveform=" << static_cast<int>(forced_waveform) << "\n";
    file << "forced_modulation=" << static_cast<int>(forced_modulation) << "\n";
    file << "forced_code_rate=" << static_cast<int>(forced_code_rate) << "\n";

    return true;
}

// Load settings from INI file
bool AppSettings::load(const std::string& path) {
    std::string filepath = path.empty() ? getDefaultPath() : path;

    std::ifstream file(filepath);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    bool ptt_mode_seen = false;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == '[') {
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Station settings
        if (key == "callsign") {
            copyBounded(callsign, sizeof(callsign), value);
        } else if (key == "grid_square") {
            copyBounded(grid_square, sizeof(grid_square), value);
        } else if (key == "name") {
            copyBounded(name, sizeof(name), value);
        }
        // Radio settings
        else if (key == "rig_model") {
            copyBounded(rig_model, sizeof(rig_model), value);
        } else if (key == "rig_port") {
            copyBounded(rig_port, sizeof(rig_port), value);
            if (ptt_serial_port[0] == '\0') {
                copyBounded(ptt_serial_port, sizeof(ptt_serial_port), value);
            }
        } else if (key == "rig_baud") {
            rig_baud = std::atoi(value.c_str());
            if (rig_baud > 0) {
                ptt_serial_baud = rig_baud;
            }
        } else if (key == "use_cat_ptt") {
            use_cat_ptt = (value == "1" || value == "true");
        } else if (key == "ptt_serial_line") {
            ptt_serial_line = std::atoi(value.c_str());
            if (ptt_serial_line < 0 || ptt_serial_line > 1) {
                ptt_serial_line = 0;
            }
        } else if (key == "ptt_mode") {
            ptt_mode = static_cast<int>(parsePttModeString(value));
            ptt_mode_seen = true;
        } else if (key == "ptt_serial_port") {
            copyBounded(ptt_serial_port, sizeof(ptt_serial_port), value);
            copyBounded(rig_port, sizeof(rig_port), value);
        } else if (key == "ptt_serial_baud") {
            ptt_serial_baud = std::atoi(value.c_str());
            if (ptt_serial_baud <= 0) {
                ptt_serial_baud = 9600;
            }
            rig_baud = ptt_serial_baud;
        } else if (key == "ptt_invert") {
            ptt_invert = (value == "1" || value == "true");
        } else if (key == "ptt_cat_host") {
            copyBounded(ptt_cat_host, sizeof(ptt_cat_host), value);
        } else if (key == "ptt_cat_port") {
            ptt_cat_port = std::atoi(value.c_str());
            if (ptt_cat_port <= 0 || ptt_cat_port > 65535) {
                ptt_cat_port = 4532;
            }
        } else if (key == "ptt_hamlib_model_id") {
            ptt_hamlib_model_id = std::atoi(value.c_str());
            if (ptt_hamlib_model_id <= 0) {
                ptt_hamlib_model_id = 1;
            }
        } else if (key == "ptt_hamlib_model") {
            copyBounded(ptt_hamlib_model, sizeof(ptt_hamlib_model), value);
        } else if (key == "ptt_hamlib_port") {
            copyBounded(ptt_hamlib_port, sizeof(ptt_hamlib_port), value);
        } else if (key == "ptt_hamlib_baud") {
            ptt_hamlib_baud = std::atoi(value.c_str());
            if (ptt_hamlib_baud <= 0) {
                ptt_hamlib_baud = 9600;
            }
        } else if (key == "ptt_hamlib_method") {
            ptt_hamlib_method = parseHamlibPttMethodString(value);
        }
        // Audio settings
        else if (key == "input_device") {
            copyBounded(input_device, sizeof(input_device), value);
        } else if (key == "output_device") {
            copyBounded(output_device, sizeof(output_device), value);
        } else if (key == "tx_delay_ms") {
            tx_delay_ms = std::atoi(value.c_str());
        } else if (key == "tx_tail_ms") {
            tx_tail_ms = std::atoi(value.c_str());
        } else if (key == "tx_drive") {
            tx_drive = std::strtof(value.c_str(), nullptr);
        }
        // Filter settings
        else if (key == "enabled") {
            filter_enabled = (value == "1" || value == "true");
        } else if (key == "center") {
            filter_center = std::strtof(value.c_str(), nullptr);
        } else if (key == "bandwidth") {
            filter_bandwidth = std::strtof(value.c_str(), nullptr);
        } else if (key == "taps") {
            filter_taps = std::atoi(value.c_str());
        }
        // File transfer settings
        else if (key == "receive_directory") {
            copyBounded(receive_directory, sizeof(receive_directory), value);
        }
        // Expert settings
        else if (key == "forced_waveform") {
            forced_waveform = static_cast<uint8_t>(std::atoi(value.c_str()));
        } else if (key == "forced_modulation") {
            forced_modulation = static_cast<uint8_t>(std::atoi(value.c_str()));
        } else if (key == "forced_code_rate") {
            forced_code_rate = static_cast<uint8_t>(std::atoi(value.c_str()));
        }
    }

    if (!ptt_mode_seen && use_cat_ptt) {
        ptt_mode = (ptt_serial_line == 0)
                       ? static_cast<int>(GuiPttMode::SerialDTR)
                       : static_cast<int>(GuiPttMode::SerialRTS);
    }

    if (!std::isfinite(tx_drive)) {
        tx_drive = ultra::sim::kHardwareTxDefaultPeakTarget;
    }
    tx_drive = std::clamp(tx_drive,
                          ultra::sim::kHardwareTxMinPeakTarget,
                          ultra::sim::kHardwareTxMaxPeakTarget);

    return true;
}

SettingsWindow::SettingsWindow() {
    startupTrace("SettingsWindow", "ctor");
}

bool SettingsWindow::render(AppSettings& settings) {
    just_closed_ = false;

    if (!visible_) {
        // Check if we just closed
        if (was_visible_) {
            just_closed_ = true;
            if (on_closed_) {
                on_closed_();
            }
        }
        was_visible_ = false;
        return false;
    }

    bool changed = false;

    ImGui::SetNextWindowSize(ImVec2(450, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Settings", &visible_, ImGuiWindowFlags_NoCollapse)) {

        if (ImGui::BeginTabBar("SettingsTabs")) {

            if (ImGui::BeginTabItem("Station")) {
                renderStationTab(settings);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Radio")) {
                renderRadioTab(settings);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Audio")) {
                renderAudioTab(settings);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Expert")) {
                renderExpertTab(settings);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    // Track visibility for next frame (to detect close via X button)
    was_visible_ = visible_;

    return changed;
}

void SettingsWindow::renderStationTab(AppSettings& settings) {
    ImGui::Spacing();
    ImGui::Text("Station Information");
    ImGui::Separator();
    ImGui::Spacing();

    // Callsign
    ImGui::Text("Callsign");
    ImGui::SetNextItemWidth(150);
    char old_call[16];
    std::strncpy(old_call, settings.callsign, sizeof(old_call) - 1);
    old_call[sizeof(old_call) - 1] = '\0';

    if (ImGui::InputText("##callsign", settings.callsign, sizeof(settings.callsign),
                         ImGuiInputTextFlags_CharsUppercase)) {
        // Notify if changed
        if (strcmp(old_call, settings.callsign) != 0 && on_callsign_changed_) {
            on_callsign_changed_(settings.callsign);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Required for ARQ)");

    ImGui::Spacing();

    // Grid square
    ImGui::Text("Grid Square");
    ImGui::SetNextItemWidth(100);
    ImGui::InputText("##grid", settings.grid_square, sizeof(settings.grid_square),
                     ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    ImGui::TextDisabled("Maidenhead locator (e.g., FN35)");

    ImGui::Spacing();

    // Operator name
    ImGui::Text("Operator Name");
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("##name", settings.name, sizeof(settings.name));

    ImGui::Spacing();
    ImGui::Spacing();

    // Validation indicator
    bool valid_call = boundedCStringLen(settings.callsign) >= 3;
    if (valid_call) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Station configured");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "Enter your callsign to use ARQ mode");
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // File Transfer Settings
    ImGui::Text("File Transfer");
    ImGui::Spacing();

    ImGui::Text("Receive Directory");
    ImGui::SetNextItemWidth(-80);

    // Show placeholder with default path if empty
    std::string default_path = AppSettings::getDefaultDownloadsPath();
    std::string placeholder = "Default: " + default_path;

    char old_dir[512];
    std::strncpy(old_dir, settings.receive_directory, sizeof(old_dir) - 1);
    old_dir[sizeof(old_dir) - 1] = '\0';

    if (ImGui::InputTextWithHint("##receive_dir", placeholder.c_str(),
                                  settings.receive_directory, sizeof(settings.receive_directory))) {
        // Notify if changed
        if (strcmp(old_dir, settings.receive_directory) != 0 && on_receive_dir_changed_) {
            on_receive_dir_changed_(settings.getReceiveDirectory());
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        settings.receive_directory[0] = '\0';  // Clear to use default
        if (on_receive_dir_changed_) {
            on_receive_dir_changed_(settings.getReceiveDirectory());
        }
    }

    ImGui::TextDisabled("Leave empty to use Downloads folder");

    // Show effective path
    ImGui::Spacing();
    std::string effective = settings.getReceiveDirectory();
    ImGui::Text("Files will be saved to:");
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", effective.c_str());
}

void SettingsWindow::renderRadioTab(AppSettings& settings) {
    ImGui::Spacing();
    ImGui::Text("Rig");
    ImGui::Separator();
    ImGui::Spacing();

    GuiPttMode mode = normalizePttMode(settings.ptt_mode);

    struct RigBackendOption {
        GuiPttMode mode;
        const char* label;
    };
    const RigBackendOption backends[] = {
        {GuiPttMode::None, "None (VOX/external)"},
        {GuiPttMode::Cat, "Hamlib NET rigctl"},
        {GuiPttMode::HamlibBuiltin, "Hamlib (built-in)"},
        {GuiPttMode::SerialRTS, "Serial RTS (direct PTT)"},
        {GuiPttMode::SerialDTR, "Serial DTR (direct PTT)"}
    };

    const char* backend_label = backends[0].label;
    for (const auto& option : backends) {
        if (option.mode == mode) {
            backend_label = option.label;
            break;
        }
    }

    ImGui::Text("Rig");
    ImGui::SetNextItemWidth(280);
    if (ImGui::BeginCombo("##rig_backend", backend_label)) {
        for (const auto& option : backends) {
            const bool selected = option.mode == mode;
            if (ImGui::Selectable(option.label, selected)) {
                mode = option.mode;
                settings.ptt_mode = static_cast<int>(mode);
                settings.use_cat_ptt =
                    (mode == GuiPttMode::SerialRTS || mode == GuiPttMode::SerialDTR);
                settings.ptt_serial_line = (mode == GuiPttMode::SerialDTR) ? 0 : 1;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    const bool serial_mode = mode == GuiPttMode::SerialRTS || mode == GuiPttMode::SerialDTR;
    const bool cat_mode = mode == GuiPttMode::Cat;
    const bool hamlib_builtin_mode = mode == GuiPttMode::HamlibBuiltin;

    const char* bauds[] = { "4800", "9600", "19200", "38400", "57600", "115200" };
    int baud_values[] = { 4800, 9600, 19200, 38400, 57600, 115200 };
    auto renderBaudCombo = [&](const char* id, int& baud) {
        int baud_idx = 1;
        for (int i = 0; i < 6; ++i) {
            if (baud == baud_values[i]) {
                baud_idx = i;
                break;
            }
        }
        ImGui::SetNextItemWidth(130);
        if (ImGui::Combo(id, &baud_idx, bauds, 6)) {
            baud = baud_values[baud_idx];
        }
    };

    auto renderSerialPortInput = [&](const char* input_id, const char* combo_id,
                                     char* port, size_t port_size) {
        ImGui::SetNextItemWidth(280);
        ImGui::InputText(input_id, port, port_size);

        const std::vector<std::string> ports = detectSerialPorts();
        if (!ports.empty()) {
            ImGui::SetNextItemWidth(280);
            const char* preview = port[0] ? port : "Detected ports";
            if (ImGui::BeginCombo(combo_id, preview)) {
                for (const auto& detected : ports) {
                    const bool selected = detected == port;
                    if (ImGui::Selectable(detected.c_str(), selected)) {
                        copyBounded(port, port_size, detected);
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }
    };

    if (serial_mode) {
        ImGui::Spacing();
        ImGui::Text("Serial Port");
        renderSerialPortInput("##ptt_serial_port", "##ptt_serial_port_detected",
                              settings.ptt_serial_port, sizeof(settings.ptt_serial_port));
        copyBounded(settings.rig_port, sizeof(settings.rig_port), settings.ptt_serial_port);
        ImGui::TextDisabled("Examples: /dev/ttyUSB0, /dev/ttyACM0, COM3");

        ImGui::Spacing();
        ImGui::Text("Baud Rate");
        renderBaudCombo("##ptt_baud", settings.ptt_serial_baud);
        settings.rig_baud = settings.ptt_serial_baud;

        ImGui::Spacing();
        ImGui::Text("PTT Line");
        ImGui::SetNextItemWidth(120);
        const char* ptt_lines[] = {"DTR", "RTS"};
        int line_idx = (mode == GuiPttMode::SerialRTS) ? 1 : 0;
        if (ImGui::Combo("##ptt_line", &line_idx, ptt_lines, 2)) {
            mode = (line_idx == 1) ? GuiPttMode::SerialRTS : GuiPttMode::SerialDTR;
            settings.ptt_mode = static_cast<int>(mode);
            settings.ptt_serial_line = line_idx;
        }

        ImGui::Checkbox("Invert PTT polarity", &settings.ptt_invert);

        ImGui::Spacing();
        ImGui::TextDisabled("Rig side: set USB SEND to matching line (DTR or RTS).");
    }

    if (cat_mode) {
        ImGui::Spacing();
        ImGui::Text("rigctld Host");
        ImGui::SetNextItemWidth(220);
        ImGui::InputText("##ptt_cat_host", settings.ptt_cat_host, sizeof(settings.ptt_cat_host));

        ImGui::Spacing();
        ImGui::Text("rigctld Port");
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("##ptt_cat_port", &settings.ptt_cat_port, 1, 100);
        if (settings.ptt_cat_port < 1) {
            settings.ptt_cat_port = 1;
        } else if (settings.ptt_cat_port > 65535) {
            settings.ptt_cat_port = 65535;
        }
    }

    if (hamlib_builtin_mode) {
        ImGui::Spacing();
#ifdef ULTRA_HAVE_LIBHAMLIB
        const auto& rigs = ultra::ptt::cachedHamlibRigList();
        const ultra::ptt::HamlibRigInfo* selected_rig =
            ultra::ptt::findHamlibRigByModelId(settings.ptt_hamlib_model_id);
        std::string rig_preview = selected_rig ? selected_rig->displayName()
                                               : settings.ptt_hamlib_model;
        if (rig_preview.empty()) {
            rig_preview = "Dummy";
        }

        ImGui::Text("Rig Model");
        ImGui::SetNextItemWidth(280);
        if (ImGui::BeginCombo("##hamlib_model", rig_preview.c_str())) {
            for (const auto& rig : rigs) {
                const std::string label = rig.displayName() + "##" + std::to_string(rig.model_id);
                const bool selected = rig.model_id == settings.ptt_hamlib_model_id;
                if (ImGui::Selectable(label.c_str(), selected)) {
                    settings.ptt_hamlib_model_id = rig.model_id;
                    copyBounded(settings.ptt_hamlib_model,
                                sizeof(settings.ptt_hamlib_model),
                                rig.displayName());
                    copyBounded(settings.rig_model, sizeof(settings.rig_model),
                                rig.displayName());
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
#else
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                           "Built-in Hamlib is not enabled in this build");
#endif

        ImGui::Spacing();
        ImGui::Text("Serial Port");
        renderSerialPortInput("##hamlib_port", "##hamlib_port_detected",
                              settings.ptt_hamlib_port, sizeof(settings.ptt_hamlib_port));
        ImGui::TextDisabled("Dummy ignores this field; real radios use the CAT serial device.");

        ImGui::Spacing();
        ImGui::Text("Baud Rate");
        renderBaudCombo("##hamlib_baud", settings.ptt_hamlib_baud);

        ImGui::Spacing();
        ImGui::Text("PTT Method");
        const char* ptt_methods[] = {"VOX", "CAT", "DTR", "RTS"};
        settings.ptt_hamlib_method = normalizeHamlibPttMethod(settings.ptt_hamlib_method);
        ImGui::SetNextItemWidth(130);
        ImGui::Combo("##hamlib_ptt_method", &settings.ptt_hamlib_method, ptt_methods, 4);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (cat_test_future_.valid()) {
        if (cat_test_future_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            std::string error = cat_test_future_.get();
            if (!cat_test_timed_out_) {
                cat_test_status_ = error.empty() ? "OK" : ("Failed: " + error);
            }
        } else if (!cat_test_timed_out_ &&
                   std::chrono::steady_clock::now() >= cat_test_deadline_) {
            cat_test_timed_out_ = true;
            cat_test_status_ = "Failed: timed out";
        }
    }

    if (ptt_test_future_.valid()) {
        if (ptt_test_future_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            std::string error = ptt_test_future_.get();
            if (!ptt_test_timed_out_) {
                ptt_test_status_ = error.empty() ? "OK" : ("Failed: " + error);
            }
        } else if (!ptt_test_timed_out_ &&
                   std::chrono::steady_clock::now() >= ptt_test_deadline_) {
            ptt_test_timed_out_ = true;
            ptt_test_status_ = "Failed: timed out";
        }
    }

    const bool cat_test_running = cat_test_future_.valid();
    if (cat_test_running || !hamlib_builtin_mode) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Test CAT", ImVec2(120, 0))) {
        if (on_cat_test_) {
            AppSettings snapshot = settings;
            cat_test_status_ = "Testing...";
            cat_test_timed_out_ = false;
            cat_test_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            cat_test_future_ = std::async(std::launch::async, [cb = on_cat_test_, snapshot]() {
                return cb(snapshot);
            });
        } else {
            cat_test_status_ = "Failed: test callback unavailable";
        }
    }
    if (cat_test_running || !hamlib_builtin_mode) {
        ImGui::EndDisabled();
    }

    if (!cat_test_status_.empty()) {
        ImGui::SameLine();
        const bool ok = cat_test_status_ == "OK";
        const ImVec4 color = ok ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f)
                                : ImVec4(1.0f, 0.55f, 0.25f, 1.0f);
        ImGui::TextColored(color, "%s", cat_test_status_.c_str());
    }

    const bool test_running = ptt_test_future_.valid();
    if (test_running) {
        ImGui::BeginDisabled();
    }
    if (hamlib_builtin_mode) {
        ImGui::SameLine();
    }
    if (ImGui::Button("Test PTT", ImVec2(120, 0))) {
        if (on_ptt_test_) {
            AppSettings snapshot = settings;
            ptt_test_status_ = "Testing...";
            ptt_test_timed_out_ = false;
            ptt_test_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            ptt_test_future_ = std::async(std::launch::async, [cb = on_ptt_test_, snapshot]() {
                return cb(snapshot);
            });
        } else {
            ptt_test_status_ = "Failed: test callback unavailable";
        }
    }
    if (test_running) {
        ImGui::EndDisabled();
    }

    if (!ptt_test_status_.empty()) {
        ImGui::SameLine();
        const bool ok = ptt_test_status_ == "OK";
        const ImVec4 color = ok ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f)
                                : ImVec4(1.0f, 0.55f, 0.25f, 1.0f);
        ImGui::TextColored(color, "%s", ptt_test_status_.c_str());
    }
}

void SettingsWindow::renderAudioTab(AppSettings& settings) {
    ImGui::Spacing();
    ImGui::Text("Audio Devices");
    ImGui::Separator();
    ImGui::Spacing();

    // Output device selection
    ImGui::Text("Output Device (Speaker)");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##output_device", settings.output_device)) {
        for (const auto& dev : output_devices) {
            bool selected = (strcmp(settings.output_device, dev.c_str()) == 0);
            if (ImGui::Selectable(dev.c_str(), selected)) {
                copyBounded(settings.output_device, sizeof(settings.output_device), dev);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    // Input device selection
    ImGui::Text("Input Device (Microphone)");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##input_device", settings.input_device)) {
        for (const auto& dev : input_devices) {
            bool selected = (strcmp(settings.input_device, dev.c_str()) == 0);
            if (ImGui::Selectable(dev.c_str(), selected)) {
                copyBounded(settings.input_device, sizeof(settings.input_device), dev);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    // Rescan button
    if (ImGui::Button("Rescan Audio Devices", ImVec2(200, 0))) {
        if (on_audio_reset_) {
            on_audio_reset_();
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Refresh device list");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("TX Settings");
    ImGui::Spacing();

    // TX Delay
    ImGui::Text("TX Delay (ms)");
    ImGui::SetNextItemWidth(150);
    ImGui::SliderInt("##tx_delay", &settings.tx_delay_ms, 0, 500);
    ImGui::SameLine();
    ImGui::TextDisabled("Delay after PTT before audio");

    // TX Tail
    ImGui::Text("TX Tail (ms)");
    ImGui::SetNextItemWidth(150);
    ImGui::SliderInt("##tx_tail", &settings.tx_tail_ms, 0, 500);
    ImGui::SameLine();
    ImGui::TextDisabled("Hold PTT after audio ends");

    ImGui::Spacing();

    // TX Drive
    ImGui::Text("TX Drive Target Peak");
    ImGui::SetNextItemWidth(200);
    float tx_drive_pct = settings.tx_drive * 100.0f;
    if (ImGui::SliderFloat("##tx_drive", &tx_drive_pct,
                           ultra::sim::kHardwareTxMinPeakTarget * 100.0f,
                           ultra::sim::kHardwareTxMaxPeakTarget * 100.0f,
                           "%.0f%%")) {
        settings.tx_drive = std::clamp(tx_drive_pct / 100.0f,
                                       ultra::sim::kHardwareTxMinPeakTarget,
                                       ultra::sim::kHardwareTxMaxPeakTarget);
    }
    if (ImGui::IsItemHovered()) {
        const float dbfs = 20.0f * std::log10(std::max(
            settings.tx_drive, ultra::sim::kHardwareTxMinPeakTarget));
        ImGui::SetTooltip("Target peak %.2f full scale (%.1f dBFS). Default %.2f.",
                          settings.tx_drive,
                          dbfs,
                          ultra::sim::kHardwareTxDefaultPeakTarget);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Target peak (0.05-0.70 FS, default 0.50)");

    ImGui::Spacing();
    ImGui::Spacing();

    // Visual indicator
    ImGui::Text("TX Level Preview:");
    ImGui::ProgressBar(settings.tx_drive, ImVec2(200, 20), "");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Audio Filter Settings
    ImGui::Text("Audio Bandpass Filter");
    ImGui::Spacing();

    bool filter_changed = false;

    // Enable/disable checkbox
    if (ImGui::Checkbox("Enable Filter", &settings.filter_enabled)) {
        filter_changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Bandpass filter on TX/RX audio");

    ImGui::Spacing();

    // Only show controls if enabled
    ImGui::BeginDisabled(!settings.filter_enabled);

    // Center frequency
    ImGui::Text("Center Frequency");
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("##filter_center", &settings.filter_center, 500.0f, 3000.0f, "%.0f Hz")) {
        filter_changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Audio passband center");

    // Bandwidth
    ImGui::Text("Bandwidth");
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("##filter_bw", &settings.filter_bandwidth, 200.0f, 3000.0f, "%.0f Hz")) {
        filter_changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Total passband width");

    // Filter taps (advanced)
    ImGui::Text("Filter Taps");
    ImGui::SetNextItemWidth(150);
    if (ImGui::SliderInt("##filter_taps", &settings.filter_taps, 31, 255)) {
        // Ensure odd number of taps
        if (settings.filter_taps % 2 == 0) {
            settings.filter_taps++;
        }
        filter_changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("More = sharper cutoff");

    ImGui::EndDisabled();

    // Display passband
    ImGui::Spacing();
    float low = settings.filter_center - settings.filter_bandwidth / 2.0f;
    float high = settings.filter_center + settings.filter_bandwidth / 2.0f;
    ImGui::Text("Passband: %.0f - %.0f Hz", low, high);

    // Call callback if filter settings changed
    if (filter_changed && on_filter_changed_) {
        on_filter_changed_(settings.filter_enabled, settings.filter_center,
                          settings.filter_bandwidth, settings.filter_taps);
    }
}

void SettingsWindow::renderExpertTab(AppSettings& settings) {
    ImGui::Spacing();
    ImGui::Text("Expert Mode Settings");
    ImGui::Separator();
    ImGui::Spacing();

    // Warning message
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
        "* Leave as AUTO if you are unsure.");
    ImGui::TextDisabled("These settings force specific waveform and modulation modes.");
    ImGui::TextDisabled("AUTO lets the protocol negotiate optimal settings based on SNR.");
    ImGui::Spacing();
    ImGui::Spacing();

    bool changed = false;

    // --- Forced Waveform ---
    ImGui::Text("Forced Waveform");
    ImGui::SetNextItemWidth(200);

    // Current waveform display string
    const char* waveform_items[] = { "AUTO", "OFDM", "OFDM Narrow", "OFDM HiSpeed", "DPSK" };
    int waveform_idx = 0;  // AUTO
    if (settings.forced_waveform == 0x05) waveform_idx = 1;       // OFDM (OFDM_CHIRP)
    else if (settings.forced_waveform == 0x06) waveform_idx = 2;  // OFDM Narrow
    else if (settings.forced_waveform == 0x00) waveform_idx = 3;  // OFDM HiSpeed (OFDM_COX)
    else if (settings.forced_waveform == 0x04) waveform_idx = 4;  // DPSK

    if (ImGui::Combo("##waveform", &waveform_idx, waveform_items, 5)) {
        switch (waveform_idx) {
            case 0: settings.forced_waveform = 0xFF; break;  // AUTO
            case 1: settings.forced_waveform = 0x05; break;  // OFDM (OFDM_CHIRP)
            case 2: settings.forced_waveform = 0x06; break;  // OFDM Narrow
            case 3: settings.forced_waveform = 0x00; break;  // OFDM HiSpeed (OFDM_COX)
            case 4: settings.forced_waveform = 0x04; break;  // DPSK
        }
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("OFDM=recommended, Narrow=low SNR, DPSK=robust");

    ImGui::Spacing();

    // --- Forced Modulation ---
    ImGui::Text("Forced Modulation");
    ImGui::SetNextItemWidth(200);

    const char* modulation_items[] = { "AUTO", "DBPSK", "DQPSK", "D8PSK", "QPSK", "16QAM" };
    int mod_idx = 0;  // AUTO
    switch (settings.forced_modulation) {
        case 0:    mod_idx = 1; break;  // DBPSK
        case 2:    mod_idx = 2; break;  // DQPSK
        case 4:    mod_idx = 3; break;  // D8PSK
        case 3:    mod_idx = 4; break;  // QPSK
        case 6:    mod_idx = 5; break;  // QAM16
        default:   mod_idx = 0; break;  // AUTO
    }

    if (ImGui::Combo("##modulation", &mod_idx, modulation_items, 6)) {
        switch (mod_idx) {
            case 0: settings.forced_modulation = 0xFF; break;  // AUTO
            case 1: settings.forced_modulation = 0; break;     // DBPSK
            case 2: settings.forced_modulation = 2; break;     // DQPSK
            case 3: settings.forced_modulation = 4; break;     // D8PSK
            case 4: settings.forced_modulation = 3; break;     // QPSK
            case 5: settings.forced_modulation = 6; break;     // QAM16
        }
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("D*=differential, Q*/16QAM=coherent");

    ImGui::Spacing();

    // --- Forced Code Rate ---
    ImGui::Text("Forced Code Rate");
    ImGui::SetNextItemWidth(200);

    const char* rate_items[] = { "AUTO", "R1/4", "R1/2", "R2/3", "R3/4", "R5/6" };
    int rate_idx = 0;  // AUTO
    switch (settings.forced_code_rate) {
        case 0:    rate_idx = 1; break;  // R1/4
        case 2:    rate_idx = 2; break;  // R1/2
        case 3:    rate_idx = 3; break;  // R2/3
        case 4:    rate_idx = 4; break;  // R3/4
        case 5:    rate_idx = 5; break;  // R5/6
        default:   rate_idx = 0; break;  // AUTO
    }

    if (ImGui::Combo("##coderate", &rate_idx, rate_items, 6)) {
        switch (rate_idx) {
            case 0: settings.forced_code_rate = 0xFF; break;  // AUTO
            case 1: settings.forced_code_rate = 0; break;     // R1/4
            case 2: settings.forced_code_rate = 2; break;     // R1/2
            case 3: settings.forced_code_rate = 3; break;     // R2/3
            case 4: settings.forced_code_rate = 4; break;     // R3/4
            case 5: settings.forced_code_rate = 5; break;     // R5/6
        }
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("R1/4=robust, R5/6=fast");

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Summary of current settings
    ImGui::Text("Current Settings:");
    ImGui::Spacing();

    auto getWaveformStr = [](uint8_t w) -> const char* {
        switch (w) {
            case 0x00: return "OFDM HiSpeed";
            case 0x04: return "DPSK";
            case 0x05: return "OFDM";
            case 0x06: return "OFDM Narrow";
            case 0xFF: return "AUTO";
            default: return "Unknown";
        }
    };

    auto getModulationStr = [](uint8_t m) -> const char* {
        switch (m) {
            case 0: return "DBPSK";
            case 2: return "DQPSK";
            case 4: return "D8PSK";
            case 3: return "QPSK";
            case 6: return "16QAM";
            case 0xFF: return "AUTO";
            default: return "Unknown";
        }
    };

    auto getCodeRateStr = [](uint8_t r) -> const char* {
        switch (r) {
            case 0: return "R1/4";
            case 2: return "R1/2";
            case 3: return "R2/3";
            case 4: return "R3/4";
            case 5: return "R5/6";
            case 0xFF: return "AUTO";
            default: return "Unknown";
        }
    };

    ImGui::BulletText("Waveform: %s", getWaveformStr(settings.forced_waveform));
    ImGui::BulletText("Modulation: %s", getModulationStr(settings.forced_modulation));
    ImGui::BulletText("Code Rate: %s", getCodeRateStr(settings.forced_code_rate));

    // Call callback if settings changed
    if (changed && on_expert_settings_changed_) {
        on_expert_settings_changed_(settings.forced_waveform,
                                     settings.forced_modulation,
                                     settings.forced_code_rate);
    }
}

} // namespace gui
} // namespace ultra
