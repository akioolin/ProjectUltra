#pragma once

#include "diagnostics/audio_ring.hpp"
#include "diagnostics/event_buffer.hpp"
#include "diagnostics/report_bundle.hpp"
#include "diagnostics/redaction.hpp"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ultra::diagnostics {

struct SessionMeta {
    std::string app_name = "projectultra";
    std::string station_role;
    std::string callsign;
    std::string config_json = "{}";
    int sample_rate = AudioRing::kDefaultSampleRate;
};

enum class FreezeReason {
    Manual,
    Signal,
    CrashPreviousSession,
    FaultTriggered,
};

struct ReportOptions {
    std::string note;
    bool include_tx_audio = false;
    bool include_callsigns = false;
};

struct ReportPath {
    std::filesystem::path path;
    bool ok = false;
    std::string error;
};

struct TombstoneInfo {
    bool valid = false;
    std::string signal_name;
    std::string session_id;
    std::string timestamp;
    std::string raw;
    std::string error;
};

class DiagnosticsRecorder {
public:
    static DiagnosticsRecorder& instance();

    void start(SessionMeta meta);
    void stop();

    void recordRx(SampleSpan samples) noexcept;
    void recordTx(SampleSpan samples) noexcept;
    void emit(const DiagEvent& event) noexcept;
    void emitText(const char* component,
                  const char* event,
                  const char* fields_json = "{}",
                  const char* privacy = "redacted") noexcept;

    ReportPath freeze(FreezeReason reason, ReportOptions options = {});

    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    bool isRxAudioRecordingEnabled() const noexcept {
        return rx_audio_enabled_.load(std::memory_order_acquire);
    }
    bool isTxAudioRecordingEnabled() const noexcept {
        return tx_audio_enabled_.load(std::memory_order_acquire);
    }
    void setTxAudioRecordingEnabled(bool enabled) noexcept {
        tx_audio_enabled_.store(enabled, std::memory_order_release);
    }

    const std::string& sessionId() const { return session_id_; }
    std::filesystem::path diagnosticsDir() const { return diagnostics_dir_; }
    std::optional<TombstoneInfo> pendingTombstone() const;

    bool hasAudioConsent() const;
    bool grantAudioConsent();
    std::filesystem::path consentPath() const;

    static std::filesystem::path defaultDiagnosticsDir();
    static TombstoneInfo parseTombstoneFile(const std::filesystem::path& path);
    static const char* freezeReasonName(FreezeReason reason);

private:
    DiagnosticsRecorder() = default;
    ~DiagnosticsRecorder();

    void installCrashHandlers();
    void prepareCrashTombstone();
    void detectPendingTombstone();
    void writerLoop();
    void flushSessionSnapshot();
    std::string manifestJson(FreezeReason reason,
                             const ReportOptions& options,
                             const AudioRingSnapshot& rx,
                             const std::optional<AudioRingSnapshot>& tx) const;
    std::string systemJson() const;
    std::filesystem::path makeReportPath() const;
    void cleanupStorage();

    mutable std::mutex lifecycle_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> rx_audio_enabled_{false};
    std::atomic<bool> tx_audio_enabled_{false};

    SessionMeta meta_;
    std::string session_id_;
    std::filesystem::path diagnostics_dir_;
    std::filesystem::path session_dir_;
    std::optional<TombstoneInfo> pending_tombstone_;

    AudioRing audio_ring_;
    EventBuffer events_{EventBuffer::kDefaultCapacity};
    std::atomic<bool> writer_stop_{false};
    std::thread writer_thread_;
};

} // namespace ultra::diagnostics
