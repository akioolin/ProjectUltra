#pragma once

#include "diagnostics/audio_ring.hpp"
#include "diagnostics/event_buffer.hpp"
#include "diagnostics/report_bundle.hpp"
#include "diagnostics/redaction.hpp"
#include "diagnostics/session_summary.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

struct SessionSummaryPath {
    std::filesystem::path path;
    std::filesystem::path journal_path;
    bool ok = false;
    std::string outcome;
    std::string text;
    std::vector<std::string> operator_log_lines;
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

    void ensureSessionActive();
    SessionSummaryPath finishSession(const std::string& reason = {},
                                     bool include_callsigns = false);
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
    std::filesystem::path currentJournalPath() const { return session_journal_path_; }
    std::filesystem::path currentSummaryPath() const { return session_summary_path_; }
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
    void beginNewSessionLocked(bool reset_buffers);
    SessionSummaryPath finishSessionLocked(const std::string& reason,
                                           bool include_callsigns);
    void writerLoop();
    bool openJournalLocked();
    void closeJournalLocked();
    bool appendJournalLinesLocked(bool fsync_checkpoint);
    bool drainJournal(bool fsync_checkpoint);
    void fsyncJournalLocked();
    void flushSessionSnapshot();
    SessionSummaryOptions summaryOptions(bool include_callsigns) const;
    std::string manifestJson(FreezeReason reason,
                             const ReportOptions& options,
                             const AudioRingSnapshot& rx,
                             const std::optional<AudioRingSnapshot>& tx) const;
    std::string systemJson() const;
    std::filesystem::path makeReportPath() const;
    void cleanupStorage();

    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex io_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> rx_audio_enabled_{false};
    std::atomic<bool> tx_audio_enabled_{false};

    SessionMeta meta_;
    std::string session_id_;
    std::string session_start_utc_compact_;
    std::string session_file_stem_;
    std::filesystem::path diagnostics_dir_;
    std::filesystem::path session_dir_;
    std::filesystem::path session_journal_path_;
    std::filesystem::path session_summary_path_;
    std::optional<TombstoneInfo> pending_tombstone_;
    SessionSummaryPath last_summary_;

    AudioRing audio_ring_;
    EventBuffer events_{EventBuffer::kDefaultCapacity};
    uint64_t journal_next_sequence_ = 1;
    uint64_t journal_dropped_events_ = 0;
    size_t journal_lines_since_sync_ = 0;
    bool session_finished_ = false;
#ifndef _WIN32
    int journal_fd_ = -1;
#else
    std::ofstream journal_stream_;
#endif
    std::atomic<bool> writer_stop_{false};
    std::thread writer_thread_;
};

} // namespace ultra::diagnostics
