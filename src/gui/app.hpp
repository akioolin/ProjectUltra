#pragma once

#include "ultra/types.hpp"
#include "widgets/constellation.hpp"
#include "widgets/controls.hpp"
#include "widgets/status.hpp"
#include "widgets/settings.hpp"
#include "widgets/waterfall.hpp"
#include "widgets/file_browser.hpp"
#include "audio_engine.hpp"
#include "serial_ptt.hpp"
#include "modem/modem_engine.hpp"
#include "protocol/protocol_engine.hpp"
#include "sim/hf_channel.hpp"

#include <vector>
#include <complex>
#include <random>
#include <string>
#include <deque>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>

namespace ultra {
namespace gui {

class App {
public:
    // Flags for developer features
    struct Options {
        bool enable_sim = false;      // -sim: Show simulation UI
        bool record_audio = false;    // -rec: Record all audio to file
        bool safe_startup = false;    // Defer heavyweight init (audio/sim) until needed
        bool disable_waterfall = false; // Skip waterfall construction (startup safety)
        std::string record_path = "sim_recording.f32";  // Recording output base path

        // Monitor mode: skip the full PING/CONNECT handshake and force
        // the decoder into a specific waveform/rate. Useful for OTA
        // monitoring (e.g. recording a friend's transmission via
        // WebSDR + replaying through a virtual audio cable into the
        // GUI to see live waterfall + decoded frames).
        // - monitor_mode = "" (default): normal operation
        // - monitor_mode = "ofdm_chirp" / "ofdm_narrow": force connected OFDM
        // - monitor_mode = "mc_dpsk": force MC-DPSK
        std::string monitor_mode;
        std::string monitor_rate = "r1_4";   // Used when monitor_mode is OFDM
        std::string monitor_modulation = "dqpsk";
    };

    App();  // Default constructor
    explicit App(const Options& opts);
    ~App();

    void render();

private:
    // Widgets
    ConstellationWidget constellation_;
    ControlsWidget controls_;
    StatusWidget status_;
    SettingsWindow settings_window_;
    std::unique_ptr<WaterfallWidget> waterfall_;
    FileBrowser file_browser_;

    // Persistent settings
    AppSettings settings_;

    // Real modem engine
    AudioEngine audio_;
    ModemEngine modem_;

    // Modem state
    ModemConfig config_;
    ultra::ModemStats stats_;  // From types.hpp for status widget

    // Operate mode state
    char tx_text_buffer_[256] = "Hello from ProjectUltra!";
    std::deque<std::string> rx_log_;
    mutable std::mutex rx_log_mutex_;
    static const size_t MAX_RX_LOG = 20;
    bool audio_initialized_ = false;
    bool deferred_audio_auto_init_pending_ = false;
    uint32_t deferred_audio_auto_init_deadline_ms_ = 0;
    int deferred_audio_auto_init_attempts_ = 0;
    bool deferred_audio_wait_logged_ = false;
    bool deferred_radio_rx_start_pending_ = false;
    uint32_t deferred_radio_rx_start_deadline_ms_ = 0;
    uint32_t deferred_radio_rx_start_timeout_ms_ = 0;
    int deferred_radio_rx_start_attempts_ = 0;
    uint32_t render_frames_seen_ = 0;
    std::atomic<bool> tx_in_progress_{false};  // Thread-safe TX flag for waterfall control
    std::chrono::steady_clock::time_point tx_end_time_;  // When current TX finishes

    // Radio mode state
    std::vector<std::string> input_devices_;
    std::vector<std::string> output_devices_;
    bool ptt_active_ = false;      // Push-to-talk state
    bool ptt_release_pending_ = false;
    uint32_t ptt_release_deadline_ms_ = 0;
    bool radio_rx_enabled_ = false; // RX capture running
    uint32_t radio_rx_started_ms_ = 0;
    bool radio_rx_warmup_logged_ = false;
    bool radio_rx_first_chunk_logged_ = false;
    uint32_t radio_rx_no_data_deadline_ms_ = 0;
    int radio_rx_rearm_attempts_ = 0;
    bool radio_rx_rearm_exhausted_logged_ = false;
    std::string radio_rx_active_device_;
    bool radio_rx_force_queue_mode_ = false;
    bool radio_rx_output_prime_attempted_ = false;

    // ARQ Protocol state
    protocol::ProtocolEngine protocol_;
    SerialPttController serial_ptt_;
    char remote_callsign_[16] = "";
    std::string pending_incoming_call_;  // Callsign of incoming caller
    uint32_t last_tick_time_ = 0;

    // Adaptive mode advisory (log-only; does not change live mode yet)
    std::deque<float> adapt_snr_window_;
    std::deque<float> adapt_fading_window_;
    std::mutex adapt_mutex_;
    bool adapt_candidate_valid_ = false;
    Modulation adapt_candidate_mod_ = Modulation::DQPSK;
    CodeRate adapt_candidate_rate_ = CodeRate::R1_4;
    int adapt_candidate_hits_ = 0;
    bool adapt_virtual_mode_valid_ = false;
    Modulation adapt_virtual_mod_ = Modulation::DQPSK;
    CodeRate adapt_virtual_rate_ = CodeRate::R1_4;
    std::chrono::steady_clock::time_point adapt_last_virtual_switch_;
    bool adapt_upgrade_hold_logged_ = false;
    Modulation adapt_upgrade_hold_mod_ = Modulation::DQPSK;
    CodeRate adapt_upgrade_hold_rate_ = CodeRate::R1_4;
    static constexpr size_t ADAPT_WINDOW_FRAMES = 5;
    static constexpr int ADAPT_DOWNGRADE_WINDOWS = 2;
    static constexpr int ADAPT_UPGRADE_WINDOWS = 4;
    static constexpr int ADAPT_UPGRADE_HOLD_MS = 8000;

    // File transfer state
    char file_path_buffer_[512] = "";
    std::string last_received_file_;  // Path of last received file
    bool file_browser_open_ = false;
    int last_progress_milestone_ = 0;  // Last logged milestone (0, 25, 50, 75)
    std::chrono::steady_clock::time_point file_transfer_start_time_;  // For duration display
    uint32_t pending_file_tx_payload_bytes_ = 0;  // Original file size for TX goodput
    std::string pending_file_tx_path_;
    float last_effective_goodput_bps_ = 0.0f;  // Last completed file transfer goodput
    std::string last_goodput_label_ = "n/a";  // Context for last goodput sample
    int diagnostics_last_arq_failed_ = 0;

    // Diagnostics report UI
    bool diagnostics_consent_popup_opened_ = false;
    bool diagnostics_report_popup_open_ = false;
    bool diagnostics_debrief_popup_open_ = false;
    char diagnostics_note_buffer_[512] = "";
    char diagnostics_debrief_save_path_[512] = "";
    std::string diagnostics_last_report_;
    std::string diagnostics_last_summary_;
    std::string diagnostics_last_summary_path_;
    std::string diagnostics_debrief_status_;
    void renderDiagnosticsDialogs();

    // ========================================
    // Developer Options
    // ========================================
    Options options_;                           // Command-line options

    // ========================================
    // Virtual Station Simulator (requires -sim flag)
    // ========================================
    // When enabled, a virtual station (callsign "SIM") responds to your
    // transmissions through full modem simulation (LDPC, OFDM, channel effects).
    // Connect to "SIM" to test the complete protocol flow in the real UI.

    bool sim_ui_visible_ = false;               // Show simulation UI (-sim flag)
    bool simulation_enabled_ = false;           // Enable virtual station
    float simulation_snr_db_ = 20.0f;           // Simulated channel SNR
    int simulation_channel_type_ = 0;           // 0=AWGN, 1=Good, 2=Moderate, 3=Poor

    // Audio recording (requires -rec flag)
    bool recording_enabled_ = false;            // Currently recording
    std::vector<float> recorded_samples_;       // Legacy sim capture (post-channel)
    std::vector<float> recorded_rx_samples_;    // Real RX audio fed to modem
    std::vector<float> recorded_tx_samples_;    // Real TX audio queued to output
    void writeRecordingToFile();                // Save recording buffers to disk
    std::string virtual_callsign_ = "SIM";      // Virtual station's callsign

    // Virtual station's protocol and modem
    protocol::ProtocolEngine virtual_protocol_;
    std::unique_ptr<ModemEngine> virtual_modem_;

    // ========================================
    // Simplified Simulator (single thread model)
    // ========================================
    // Our TX -> channel effects -> virtual modem
    // Virtual TX -> channel effects -> our modem

    // TX pending buffers (queued by protocol TX callbacks)
    std::mutex our_tx_pending_mutex_;
    std::vector<float> our_tx_pending_;
    std::mutex virtual_tx_pending_mutex_;
    std::vector<float> virtual_tx_pending_;

    // Channel simulation RNG and persistent fading channels (one per direction)
    std::mt19937 sim_rng_{42};
    std::unique_ptr<sim::WattersonChannel> sim_channel_a_to_b_;  // Our TX -> virtual RX
    std::unique_ptr<sim::WattersonChannel> sim_channel_b_to_a_;  // Virtual TX -> our RX
    int sim_channel_active_type_ = -1;  // Track which channel type is active

    // Single simulation thread handles everything
    std::thread sim_thread_;
    std::atomic<bool> sim_thread_running_{false};
    std::atomic<bool> sim_drop_local_tx_requested_{false};

    // Virtual station initialization
    void initVirtualStation();

    // Start/stop simulator
    void startSimulator();
    void stopSimulator();

    // Main simulation loop (runs in sim_thread_)
    void simulationLoop();

    // Channel simulation (direction: 0 = our TX→virtual RX, 1 = virtual TX→our RX)
    std::vector<float> applyChannelEffects(const std::vector<float>& samples, int direction);

    // ========================================
    // UI Rendering
    // ========================================
    void renderOperateTab();
    void renderCompactChannelStatus(const LoopbackStats& stats, Modulation data_mod, CodeRate data_rate,
                                    const protocol::ConnectionStats& conn_stats);
    void initAudio();
    void appendRxLogLine(const std::string& msg);
    std::deque<std::string> snapshotRxLog() const;
    void clearRxLog();
    void stopTxNow(const char* reason);
    bool queueRealTxSamples(const std::vector<float>& samples, const char* context);
    bool ensureSerialPttReady();
    bool setSerialPtt(bool asserted, const char* reason);
    void releaseSerialPtt(const char* reason);
    void closeSerialPtt();
    void sendMessage();
    void onDataReceived(const std::string& text);
    void resetAdaptiveAdvisory();
    void updateAdaptiveAdvisory(float snr_db, float fading_index);
    bool startRadioRx();
    void stopRadioRx();
    void pollRadioRx();

    // Helper to get device name from settings (returns empty string for "Default")
    std::string getInputDeviceName() const;
    std::string getOutputDeviceName() const;
};

} // namespace gui
} // namespace ultra
