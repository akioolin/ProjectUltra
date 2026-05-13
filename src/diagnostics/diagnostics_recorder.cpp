#include "diagnostics/diagnostics_recorder.hpp"

#include "ultra/build_info.hpp"
#include "ultra/logging.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

namespace ultra::diagnostics {

namespace fs = std::filesystem;

namespace {

#ifndef _WIN32
int g_tombstone_fd = -1;
std::atomic<bool> g_tombstone_written{false};
char g_signal_session_id[64] = "unknown";

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
#ifdef SIGBUS
        case SIGBUS: return "SIGBUS";
#endif
        default: return "SIGNAL";
    }
}

void writeAllSignalSafe(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = ::write(fd, data, len);
        if (n <= 0) {
            return;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
}

bool writeAllFd(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = ::write(fd, data, len);
        if (n <= 0) {
            return false;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

void appendUnsigned(char*& p, char* end, unsigned long long value) {
    char tmp[32];
    int i = 0;
    do {
        tmp[i++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value && i < static_cast<int>(sizeof(tmp)));
    while (i > 0 && p < end) {
        *p++ = tmp[--i];
    }
}

void crashSignalHandler(int sig) {
    if (g_tombstone_fd >= 0 && !g_tombstone_written.exchange(true, std::memory_order_acq_rel)) {
        char buf[768];
        char* p = buf;
        char* end = buf + sizeof(buf);
        auto put = [&](const char* s) {
            while (*s && p < end) *p++ = *s++;
        };
        put("schema=1\nkind=signal\nsignal=");
        put(signalName(sig));
        put("\nsession_id=");
        put(g_signal_session_id);
        put("\ntimestamp_epoch=");
        struct timespec ts {};
        if (::clock_gettime(CLOCK_REALTIME, &ts) == 0) {
            appendUnsigned(p, end, static_cast<unsigned long long>(ts.tv_sec));
        } else {
            put("0");
        }
        put("\nstack=unavailable_async_signal_safe\n");
        writeAllSignalSafe(g_tombstone_fd, buf, static_cast<size_t>(p - buf));
        ::fsync(g_tombstone_fd);
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

std::string utcCompactNow() {
    std::time_t now = std::time(nullptr);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &now);
#else
    gmtime_r(&now, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm_utc);
    return buf;
}

std::string utcIsoNow() {
    std::time_t now = std::time(nullptr);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &now);
#else
    gmtime_r(&now, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return buf;
}

std::string randomHex(size_t n) {
    static std::atomic<uint64_t> seq{0};
    uint64_t seed = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    seed ^= (seq.fetch_add(1, std::memory_order_relaxed) + 0x9e3779b97f4a7c15ULL);
    std::mt19937_64 rng(seed);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = kHex[rng() & 0xfu];
    }
    return out;
}

std::string readTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool writeTextFile(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

uintmax_t directorySize(const fs::path& dir) {
    std::error_code ec;
    uintmax_t total = 0;
    if (!fs::exists(dir, ec)) return 0;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            total += it->file_size(ec);
        }
    }
    return total;
}

} // namespace

DiagnosticsRecorder::~DiagnosticsRecorder() {
    stop();
}

DiagnosticsRecorder& DiagnosticsRecorder::instance() {
    static DiagnosticsRecorder recorder;
    return recorder;
}

fs::path DiagnosticsRecorder::defaultDiagnosticsDir() {
    if (const char* override_dir = std::getenv("ULTRA_DIAGNOSTICS_DIR")) {
        if (override_dir[0] != '\0') {
            return fs::path(override_dir);
        }
    }
    auto writableOrDevFallback = [](const fs::path& candidate) {
        std::error_code ec;
        fs::create_directories(candidate, ec);
        if (!ec) {
            return candidate;
        }
        return fs::path("diagnostics");
    };
    if (const char* home = std::getenv("HOME")) {
#ifdef __APPLE__
        return writableOrDevFallback(fs::path(home) / "Library" / "Application Support" /
                                     "ProjectUltra" / "diagnostics");
#else
        if (const char* xdg = std::getenv("XDG_STATE_HOME")) {
            if (xdg[0] != '\0') {
                return writableOrDevFallback(fs::path(xdg) / "projectultra" / "diagnostics");
            }
        }
        return writableOrDevFallback(fs::path(home) / ".local" / "state" /
                                     "projectultra" / "diagnostics");
#endif
    }
    return fs::path("diagnostics");
}

void DiagnosticsRecorder::start(SessionMeta meta) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    meta_ = std::move(meta);
    diagnostics_dir_ = defaultDiagnosticsDir();

    std::error_code ec;
    fs::create_directories(diagnostics_dir_ / "sessions", ec);
    fs::create_directories(diagnostics_dir_ / "reports", ec);

    rx_audio_enabled_.store(hasAudioConsent(), std::memory_order_release);
    tx_audio_enabled_.store(false, std::memory_order_release);
    writer_stop_.store(false, std::memory_order_release);
    beginNewSessionLocked(true);
    running_.store(true, std::memory_order_release);

    detectPendingTombstone();
    prepareCrashTombstone();
    installCrashHandlers();

    emitText("app", "app.start", "{\"phase\":\"start\"}");
    emitText("session", "session.state", "{\"state\":\"started\"}");
    writer_thread_ = std::thread(&DiagnosticsRecorder::writerLoop, this);
    cleanupStorage();
}

void DiagnosticsRecorder::stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    if (!session_finished_) {
        emitText("session", "session.state", "{\"state\":\"stopped\"}");
        (void)finishSessionLocked("app_stop", false);
    }
    writer_stop_.store(true, std::memory_order_release);
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
    flushSessionSnapshot();
    {
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        closeJournalLocked();
    }
    running_.store(false, std::memory_order_release);
#ifndef _WIN32
    if (g_tombstone_fd >= 0) {
        ::close(g_tombstone_fd);
        g_tombstone_fd = -1;
    }
    if (!g_tombstone_written.load(std::memory_order_acquire) && !diagnostics_dir_.empty()) {
        std::error_code ec;
        fs::remove(diagnostics_dir_ / "tombstone.pending", ec);
    }
#endif
}

void DiagnosticsRecorder::beginNewSessionLocked(bool reset_buffers) {
    std::lock_guard<std::mutex> io_lock(io_mutex_);
    closeJournalLocked();

    session_id_ = randomHex(12);
    session_start_utc_compact_ = utcCompactNow();
    const std::string short_id = session_id_.substr(0, std::min<size_t>(6, session_id_.size()));
    session_file_stem_ = "session-" + session_start_utc_compact_ + "-" + short_id;
    session_dir_ = diagnostics_dir_ / "sessions" / session_id_;
    session_journal_path_ = diagnostics_dir_ / "sessions" / (session_file_stem_ + ".jsonl");
    session_summary_path_ = diagnostics_dir_ / "sessions" / (session_file_stem_ + ".txt");

    std::error_code ec;
    fs::create_directories(session_dir_, ec);
    fs::create_directories(session_journal_path_.parent_path(), ec);

    if (reset_buffers) {
        audio_ring_.reset(meta_.sample_rate);
        events_.reset(EventBuffer::kDefaultCapacity);
    }
    journal_next_sequence_ = reset_buffers ? 1 : events_.eventsWritten() + 1;
    journal_dropped_events_ = 0;
    journal_lines_since_sync_ = 0;
    session_finished_ = false;
    last_summary_ = SessionSummaryPath{};
    last_summary_.journal_path = session_journal_path_;
    openJournalLocked();
}

void DiagnosticsRecorder::ensureSessionActive() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!running_.load(std::memory_order_acquire) || !session_finished_) {
        return;
    }
    beginNewSessionLocked(false);
    prepareCrashTombstone();
    emitText("session", "session.state", "{\"state\":\"started\"}");
}

SessionSummaryPath DiagnosticsRecorder::finishSession(const std::string& reason,
                                                      bool include_callsigns) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    return finishSessionLocked(reason, include_callsigns);
}

SessionSummaryPath DiagnosticsRecorder::finishSessionLocked(const std::string& reason,
                                                            bool include_callsigns) {
    SessionSummaryPath result;
    result.journal_path = session_journal_path_;
    result.path = session_summary_path_;
    if (!running_.load(std::memory_order_acquire)) {
        result.error = "diagnostics recorder is not running";
        return result;
    }
    if (session_finished_) {
        return last_summary_;
    }

    const AudioRingSnapshot rx = audio_ring_.snapshotRx();
    if (!rx.samples.empty()) {
        double sum_sq = 0.0;
        double peak = 0.0;
        for (int16_t sample : rx.samples) {
            const double normalized = static_cast<double>(sample) / 32768.0;
            sum_sq += normalized * normalized;
            peak = std::max(peak, std::abs(normalized));
        }
        const double rms = std::sqrt(sum_sq / static_cast<double>(rx.samples.size()));
        char fields[192];
        std::snprintf(fields, sizeof(fields),
                      "{\"samples\":%zu,\"dropped_samples\":%llu,"
                      "\"rms\":%.4f,\"peak\":%.4f}",
                      rx.samples.size(),
                      static_cast<unsigned long long>(rx.samples_dropped),
                      rms,
                      peak);
        emitText("audio", "audio.stats", fields);
    }

    char fields[384];
    std::snprintf(fields, sizeof(fields),
                  "{\"reason\":\"%s\"}",
                  jsonEscape(reason).c_str());
    emitText("session", "session.finished", fields);
    drainJournal(true);

    auto summary = summarizeSessionJournal(session_journal_path_, summaryOptions(include_callsigns));
    if (!summary.ok) {
        result.error = summary.error;
        return result;
    }

    std::string error;
    if (!writeSessionSummary(session_summary_path_, summary, &error)) {
        result.error = error;
        return result;
    }

    result.ok = true;
    result.outcome = summary.outcome;
    result.text = summary.text;
    result.operator_log_lines = summary.operator_log_lines;
    session_finished_ = true;
    last_summary_ = result;
    cleanupStorage();
    return result;
}

void DiagnosticsRecorder::writerLoop() {
    auto last_snapshot = std::chrono::steady_clock::now();
    while (!writer_stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!running_.load(std::memory_order_acquire)) {
            continue;
        }
        drainJournal(false);
        const auto now = std::chrono::steady_clock::now();
        if (now - last_snapshot >= std::chrono::seconds(5)) {
            flushSessionSnapshot();
            last_snapshot = now;
        }
    }
    drainJournal(true);
}

bool DiagnosticsRecorder::openJournalLocked() {
    if (session_journal_path_.empty()) {
        return false;
    }
#ifndef _WIN32
    if (journal_fd_ >= 0) {
        return true;
    }
    journal_fd_ = ::open(session_journal_path_.string().c_str(),
                         O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC,
                         0644);
    return journal_fd_ >= 0;
#else
    if (journal_stream_.is_open()) {
        return true;
    }
    journal_stream_.open(session_journal_path_, std::ios::binary | std::ios::app);
    return journal_stream_.good();
#endif
}

void DiagnosticsRecorder::closeJournalLocked() {
#ifndef _WIN32
    if (journal_fd_ >= 0) {
        ::fsync(journal_fd_);
        ::close(journal_fd_);
        journal_fd_ = -1;
    }
#else
    if (journal_stream_.is_open()) {
        journal_stream_.flush();
        journal_stream_.close();
    }
#endif
}

void DiagnosticsRecorder::fsyncJournalLocked() {
#ifndef _WIN32
    if (journal_fd_ >= 0) {
        ::fsync(journal_fd_);
    }
#else
    if (journal_stream_.is_open()) {
        journal_stream_.flush();
    }
#endif
    journal_lines_since_sync_ = 0;
}

bool DiagnosticsRecorder::appendJournalLinesLocked(bool fsync_checkpoint) {
    if (session_journal_path_.empty() || !openJournalLocked()) {
        return false;
    }
    uint64_t next = journal_next_sequence_;
    uint64_t dropped = 0;
    auto lines = events_.snapshotLinesFrom(journal_next_sequence_, &next, &dropped);
    journal_dropped_events_ += dropped;
    if (lines.empty()) {
        if (fsync_checkpoint) {
            fsyncJournalLocked();
        }
        journal_next_sequence_ = next;
        return true;
    }

    std::string payload;
    size_t total = 0;
    for (const auto& line : lines) {
        total += line.size() + 1;
    }
    payload.reserve(total);
    for (const auto& line : lines) {
        payload += line;
        payload += '\n';
    }

#ifndef _WIN32
    if (!writeAllFd(journal_fd_, payload.data(), payload.size())) {
        return false;
    }
#else
    journal_stream_.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (!journal_stream_.good()) {
        return false;
    }
#endif
    journal_next_sequence_ = next;
    journal_lines_since_sync_ += lines.size();
    if (fsync_checkpoint || journal_lines_since_sync_ >= 64) {
        fsyncJournalLocked();
    }
    return true;
}

bool DiagnosticsRecorder::drainJournal(bool fsync_checkpoint) {
    std::lock_guard<std::mutex> io_lock(io_mutex_);
    return appendJournalLinesLocked(fsync_checkpoint);
}

void DiagnosticsRecorder::flushSessionSnapshot() {
    drainJournal(false);

    fs::path session_dir;
    fs::path journal_path;
    {
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        session_dir = session_dir_;
        journal_path = session_journal_path_;
    }
    if (session_dir.empty()) {
        return;
    }

    std::string events_jsonl = readTextFile(journal_path);
    if (events_jsonl.empty()) {
        events_jsonl = events_.snapshotJsonl();
    }
    writeTextFile(session_dir / "events" / "session.jsonl", events_jsonl);
    writeTextFile(session_dir / "config" / "effective_config.redacted.json",
                  redactConfigJson(meta_.config_json, {}));
    writeTextFile(session_dir / "system" / "system.json", systemJson());
    auto rx = audio_ring_.snapshotRx();
    if (!rx_audio_enabled_.load(std::memory_order_acquire)) {
        rx.samples.clear();
    }
    (void)writeWavPcm16(session_dir / "audio" / "rx_48k_s16.wav", rx);
    if (tx_audio_enabled_.load(std::memory_order_acquire)) {
        (void)writeWavPcm16(session_dir / "audio" / "tx_48k_s16.wav",
                            audio_ring_.snapshotTx());
    }
}

void DiagnosticsRecorder::recordRx(SampleSpan samples) noexcept {
    if (running_.load(std::memory_order_acquire) &&
        rx_audio_enabled_.load(std::memory_order_acquire)) {
        audio_ring_.pushRx(samples);
    }
}

void DiagnosticsRecorder::recordTx(SampleSpan samples) noexcept {
    if (running_.load(std::memory_order_acquire) &&
        tx_audio_enabled_.load(std::memory_order_acquire)) {
        audio_ring_.pushTx(samples);
    }
}

void DiagnosticsRecorder::emit(const DiagEvent& event) noexcept {
    if (running_.load(std::memory_order_acquire)) {
        events_.push(event);
    }
}

void DiagnosticsRecorder::emitText(const char* component,
                                   const char* event,
                                   const char* fields_json,
                                   const char* privacy) noexcept {
    emit(makeDiagEvent(component, event, fields_json, privacy));
}

ReportPath DiagnosticsRecorder::freeze(FreezeReason reason, ReportOptions options) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    ReportPath result;
    if (!running_.load(std::memory_order_acquire)) {
        result.error = "diagnostics recorder is not running";
        return result;
    }

    emitText("fault", reason == FreezeReason::FaultTriggered ? "fault.triggered" : "report.freeze",
             "{\"requested\":true}");

    const AudioRingSnapshot rx = audio_ring_.snapshotRx();
    std::optional<AudioRingSnapshot> tx;
    if (options.include_tx_audio && tx_audio_enabled_.load(std::memory_order_acquire)) {
        tx = audio_ring_.snapshotTx();
    }

    const fs::path report_path = makeReportPath();
    const fs::path staging = diagnostics_dir_ / "staging" / session_id_;
    char created_fields[384];
    std::snprintf(created_fields, sizeof(created_fields),
                  "{\"path\":\"%s\",\"reason\":\"%s\"}",
                  report_path.string().c_str(), freezeReasonName(reason));
    emitText("report", "report.created", created_fields);
    drainJournal(true);

    std::string events_jsonl = readTextFile(session_journal_path_);
    if (events_jsonl.empty()) {
        events_jsonl = events_.snapshotJsonl();
    }

    BundleBuildInput input;
    input.output_path = report_path;
    input.staging_dir = staging;
    input.manifest_json = manifestJson(reason, options, rx, tx);
    input.events_jsonl = events_jsonl;
    input.rx_audio = rx;
    input.tx_audio = tx;
    input.config_json = redactConfigJson(meta_.config_json, {options.include_callsigns});
    input.operator_log = "ProjectUltra diagnostics bundle created locally.\n";
    input.system_json = systemJson();
    input.operator_note = options.note;
    input.replay_readme =
        "This bundle is a local ProjectUltra field report. "
        "Replay ingest is forward-compatible; use session_decode support when available.\n";

    std::string error;
    if (!buildReportBundle(input, &error)) {
        result.error = error;
        return result;
    }
    result.path = report_path;
    result.ok = true;
    cleanupStorage();
    return result;
}

const char* DiagnosticsRecorder::freezeReasonName(FreezeReason reason) {
    switch (reason) {
        case FreezeReason::Manual: return "manual";
        case FreezeReason::Signal: return "signal";
        case FreezeReason::CrashPreviousSession: return "previous_session_crash";
        case FreezeReason::FaultTriggered: return "fault_triggered";
        default: return "unknown";
    }
}

fs::path DiagnosticsRecorder::makeReportPath() const {
    const std::string short_id = session_id_.substr(0, std::min<size_t>(6, session_id_.size()));
    const std::string name = "ultra-report-" + utcCompactNow() + "-" + short_id + ".zip";
    return diagnostics_dir_ / "reports" / name;
}

std::string DiagnosticsRecorder::manifestJson(FreezeReason reason,
                                              const ReportOptions& options,
                                              const AudioRingSnapshot& rx,
                                              const std::optional<AudioRingSnapshot>& tx) const {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": 1,\n"
        << "  \"created_utc\": \"" << utcIsoNow() << "\",\n"
        << "  \"session_id\": \"" << jsonEscape(session_id_) << "\",\n"
        << "  \"reason\": \"" << freezeReasonName(reason) << "\",\n"
        << "  \"app_name\": \"" << jsonEscape(meta_.app_name) << "\",\n"
        << "  \"station_role\": \"" << jsonEscape(meta_.station_role) << "\",\n"
        << "  \"privacy\": {\"callsigns_included\": "
        << (options.include_callsigns ? "true" : "false")
        << ", \"rx_audio_consent\": "
        << (rx_audio_enabled_.load(std::memory_order_acquire) ? "true" : "false")
        << ", \"tx_audio_included\": " << (tx.has_value() ? "true" : "false") << "},\n"
        << "  \"build\": {\"version\": \"" << kBuildVersion
        << "\", \"git_commit\": \"" << kBuildGitCommit
        << "\", \"release_tag\": \"" << kBuildReleaseTag
        << "\", \"build_os\": \"" << kBuildOS
        << "\", \"build_time_utc\": \"" << kBuildTimeUtc
        << "\", \"dirty\": " << (kBuildDirty ? "true" : "false") << "},\n"
        << "  \"audio\": {\"sample_rate\": " << rx.sample_rate
        << ", \"rx_samples\": " << rx.samples.size()
        << ", \"rx_dropped_samples\": " << rx.samples_dropped
        // The first RX sample in the bundle represents the (samples_dropped)-th
        // sample ever captured — i.e. it occurred this many ms after recorder
        // start. ultra_replay uses this to align audio with the JSONL
        // timeline when a session has been running longer than the audio
        // ring window.
        << ", \"rx_start_t_ms\": "
        << (rx.sample_rate > 0
             ? (rx.samples_dropped * 1000ULL) / static_cast<uint64_t>(rx.sample_rate)
             : 0ULL)
        << ", \"tx_samples\": " << (tx ? tx->samples.size() : 0)
        << ", \"tx_dropped_samples\": " << (tx ? tx->samples_dropped : 0)
        << ", \"tx_start_t_ms\": "
        << (tx && tx->sample_rate > 0
             ? (tx->samples_dropped * 1000ULL) / static_cast<uint64_t>(tx->sample_rate)
             : 0ULL) << "},\n"
        << "  \"events\": {\"written\": " << events_.eventsWritten()
        << ", \"dropped\": " << events_.eventsDropped() << "}\n"
        << "}\n";
    return out.str();
}

std::string DiagnosticsRecorder::systemJson() const {
    std::ostringstream out;
    out << "{\n"
        << "  \"build_os\": \"" << kBuildOS << "\",\n"
        << "  \"version\": \"" << kBuildVersion << "\",\n"
        << "  \"git_commit\": \"" << kBuildGitCommit << "\",\n"
        << "  \"dirty\": " << (kBuildDirty ? "true" : "false") << "\n"
        << "}\n";
    return out.str();
}

SessionSummaryOptions DiagnosticsRecorder::summaryOptions(bool include_callsigns) const {
    auto options = defaultSessionSummaryOptions();
    options.app_name = meta_.app_name;
    options.station_role = meta_.station_role;
    options.local_callsign = meta_.callsign;
    options.include_callsigns = include_callsigns;
    return options;
}

bool DiagnosticsRecorder::hasAudioConsent() const {
    std::error_code ec;
    return fs::exists(consentPath(), ec);
}

bool DiagnosticsRecorder::grantAudioConsent() {
    const bool ok = writeTextFile(consentPath(),
        "ProjectUltra diagnostics RX audio capture consent granted.\n"
        "RX audio may contain third-party speech or callsigns. Delete this file to revoke.\n");
    if (ok) {
        rx_audio_enabled_.store(true, std::memory_order_release);
        emitText("privacy", "audio.consent", "{\"rx_audio\":true}");
    }
    return ok;
}

fs::path DiagnosticsRecorder::consentPath() const {
    const fs::path base = diagnostics_dir_.empty() ? defaultDiagnosticsDir() : diagnostics_dir_;
    return base / "consent" / "rx_audio_capture.accepted";
}

void DiagnosticsRecorder::detectPendingTombstone() {
    const fs::path path = diagnostics_dir_ / "tombstone.pending";
    auto info = parseTombstoneFile(path);
    if (info.valid) {
        pending_tombstone_ = info;
        emitText("fault", "fault.triggered", "{\"source\":\"previous_session_tombstone\"}");
    } else {
        pending_tombstone_.reset();
    }
}

std::optional<TombstoneInfo> DiagnosticsRecorder::pendingTombstone() const {
    return pending_tombstone_;
}

void DiagnosticsRecorder::prepareCrashTombstone() {
#ifndef _WIN32
    std::error_code ec;
    fs::create_directories(diagnostics_dir_, ec);
    const fs::path path = diagnostics_dir_ / "tombstone.pending";
    if (g_tombstone_fd >= 0) {
        ::close(g_tombstone_fd);
        g_tombstone_fd = -1;
    }
    g_tombstone_fd = ::open(path.string().c_str(),
                            O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC,
                            0644);
    std::snprintf(g_signal_session_id, sizeof(g_signal_session_id),
                  "%s", session_id_.c_str());
    g_tombstone_written.store(false, std::memory_order_release);
#endif
}

void DiagnosticsRecorder::installCrashHandlers() {
#ifndef _WIN32
    std::signal(SIGSEGV, crashSignalHandler);
    std::signal(SIGABRT, crashSignalHandler);
    std::signal(SIGILL, crashSignalHandler);
    std::signal(SIGFPE, crashSignalHandler);
#ifdef SIGBUS
    std::signal(SIGBUS, crashSignalHandler);
#endif
    std::set_terminate([] {
        if (g_tombstone_fd >= 0 &&
            !g_tombstone_written.exchange(true, std::memory_order_acq_rel)) {
            const char msg[] =
                "schema=1\nkind=terminate\nsignal=std::terminate\n"
                "session_id=unknown\ntimestamp_epoch=0\n"
                "stack=unavailable_async_signal_safe\n";
            writeAllSignalSafe(g_tombstone_fd, msg, sizeof(msg) - 1);
            ::fsync(g_tombstone_fd);
        }
        std::_Exit(3);
    });
#endif
}

TombstoneInfo DiagnosticsRecorder::parseTombstoneFile(const fs::path& path) {
    TombstoneInfo info;
    const std::string raw = readTextFile(path);
    if (raw.empty()) {
        info.error = "missing or empty tombstone";
        return info;
    }
    info.raw = raw;
    auto readValue = [&](const std::string& key) -> std::string {
        const std::string needle = key + "=";
        const size_t pos = raw.find(needle);
        if (pos == std::string::npos) return {};
        const size_t start = pos + needle.size();
        const size_t end = raw.find('\n', start);
        std::string value = raw.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!value.empty() && value.back() == '\r') {
            value.pop_back();
        }
        return value;
    };
    info.signal_name = readValue("signal");
    info.session_id = readValue("session_id");
    info.timestamp = readValue("timestamp_epoch");
    if (info.signal_name.empty() || info.session_id.empty()) {
        info.error = "corrupt tombstone";
        return info;
    }
    info.valid = true;
    return info;
}

void DiagnosticsRecorder::cleanupStorage() {
    // Storage cap is 8x the audio ring cap so that a freshly created
    // ~10 MB report bundle never single-handedly trips eviction on the
    // very freeze() call that produced it. Sessions accumulate over a
    // shift's worth of operator work; reports are operator-saved
    // artifacts. Capping at 64 MB (1x AudioRing) made the cleanup eat
    // the just-saved report whenever sessions/ alone exceeded the cap.
    constexpr uintmax_t kStorageCapBytes = AudioRing::kHardCapBytes * 8;
    if (diagnostics_dir_.empty()) {
        return;
    }

    struct SessionCandidate {
        fs::path journal;
        fs::path summary;
        fs::file_time_type time;
        uintmax_t size = 0;
    };
    std::error_code ec;
    auto collectSessions = [&]() {
        std::vector<SessionCandidate> sessions;
        const fs::path sessions_dir = diagnostics_dir_ / "sessions";
        if (!fs::exists(sessions_dir, ec)) return sessions;
        for (const auto& entry : fs::directory_iterator(sessions_dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const auto path = entry.path();
            const std::string filename = path.filename().string();
            if (filename.rfind("session-", 0) != 0 || path.extension() != ".jsonl") {
                continue;
            }
            fs::path summary = path;
            summary.replace_extension(".txt");
            uintmax_t sz = entry.file_size(ec);
            uintmax_t summary_sz = fs::exists(summary, ec) ? fs::file_size(summary, ec) : 0;
            sessions.push_back({path, summary, entry.last_write_time(ec), sz + summary_sz});
        }
        return sessions;
    };

    // Pass 1: age/count limits (current session is always preserved).
    {
        std::vector<SessionCandidate> sessions = collectSessions();
        std::sort(sessions.begin(), sessions.end(),
                  [](const SessionCandidate& a, const SessionCandidate& b) {
                      return a.time > b.time;
                  });
        const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * 30);
        for (size_t i = 0; i < sessions.size(); ++i) {
            const bool too_many = i >= 100;
            const bool too_old = sessions[i].time < cutoff;
            if ((!too_many && !too_old) || sessions[i].journal == session_journal_path_) {
                continue;
            }
            fs::remove(sessions[i].journal, ec);
            fs::remove(sessions[i].summary, ec);
        }
    }

    uintmax_t total = directorySize(diagnostics_dir_);
    if (total <= kStorageCapBytes) {
        return;
    }

    // Pass 2: still over cap. Evict OLDEST sessions before touching
    // reports/. The current session is always preserved.
    {
        std::vector<SessionCandidate> sessions = collectSessions();
        std::sort(sessions.begin(), sessions.end(),
                  [](const SessionCandidate& a, const SessionCandidate& b) {
                      return a.time < b.time;
                  });
        for (const auto& s : sessions) {
            if (total <= kStorageCapBytes) break;
            if (s.journal == session_journal_path_) continue;
            fs::remove(s.journal, ec);
            fs::remove(s.summary, ec);
            total = s.size < total ? total - s.size : 0;
            emitText("diagnostics", "diagnostics.cleanup", "{\"removed\":\"old_session\"}");
        }
    }

    if (total <= kStorageCapBytes) {
        return;
    }

    // Pass 3: STILL over cap. Evict oldest reports, but never the
    // newest one (which is almost certainly the report the current
    // freeze() just created — the operator-visible artifact MUST
    // persist past cleanup).
    struct Candidate {
        fs::path path;
        fs::file_time_type time;
        uintmax_t size = 0;
    };
    std::vector<Candidate> candidates;
    const fs::path reports = diagnostics_dir_ / "reports";
    if (fs::exists(reports, ec)) {
        for (const auto& entry : fs::directory_iterator(reports, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            candidates.push_back({entry.path(), entry.last_write_time(ec), entry.file_size(ec)});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.time < b.time; });
    // Walk oldest first but stop before the single newest entry so the
    // operator's most recent save always survives.
    const size_t stop_at = candidates.empty() ? 0 : candidates.size() - 1;
    for (size_t i = 0; i < stop_at; ++i) {
        if (total <= kStorageCapBytes) break;
        const auto& c = candidates[i];
        fs::remove(c.path, ec);
        total = c.size < total ? total - c.size : 0;
        emitText("diagnostics", "diagnostics.cleanup", "{\"removed\":\"old_report\"}");
    }
}

} // namespace ultra::diagnostics
