#include "diagnostics/audio_ring.hpp"
#include "diagnostics/event_buffer.hpp"
#include "diagnostics/diagnostics_recorder.hpp"
#include "diagnostics/report_bundle.hpp"
#include "diagnostics/session_summary.hpp"
#include "helpers/temp_dir.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg) \
    do { \
        ++tests_run; \
        if (!(cond)) { \
            ++tests_failed; \
            std::cout << "FAIL: " << msg << "\n"; \
            return; \
        } \
    } while (0)

void test_audio_ring_drop_counter() {
    ultra::diagnostics::Pcm16Ring ring(4, 48000);
    const std::vector<float> first = {0.0f, 0.1f, 0.2f};
    ring.push(ultra::SampleSpan(first.data(), first.size()));
    CHECK(ring.samplesDropped() == 0, "ring should not drop before capacity");

    const std::vector<float> second = {0.3f, 0.4f, 0.5f, 0.6f};
    ring.push(ultra::SampleSpan(second.data(), second.size()));
    auto snap = ring.snapshot();
    CHECK(snap.samples.size() == 4, "snapshot should retain capacity samples");
    CHECK(snap.samples_written == 7, "written count should include all pushed samples");
    CHECK(snap.samples_dropped == 3, "drop counter should count overwritten tail samples");
}

void test_event_buffer_wraparound() {
    ultra::diagnostics::EventBuffer buffer(3);
    buffer.push(ultra::diagnostics::makeDiagEvent("test", "one"));
    buffer.push(ultra::diagnostics::makeDiagEvent("test", "two"));
    buffer.push(ultra::diagnostics::makeDiagEvent("test", "three"));
    buffer.push(ultra::diagnostics::makeDiagEvent("test", "four"));

    auto lines = buffer.snapshotLines();
    CHECK(lines.size() == 3, "event buffer should retain bounded tail");
    CHECK(buffer.eventsWritten() == 4, "event write count should include wrapped event");
    CHECK(buffer.eventsDropped() == 1, "event drop count should record wrap");
    CHECK(lines.front().find("\"event\":\"two\"") != std::string::npos,
          "oldest retained event should be second event");
    CHECK(lines.back().find("\"event\":\"four\"") != std::string::npos,
          "newest retained event should be fourth event");
}

void test_event_buffer_incremental_snapshot() {
    ultra::diagnostics::EventBuffer buffer(4);
    buffer.push(ultra::diagnostics::makeDiagEvent("test", "one"));
    buffer.push(ultra::diagnostics::makeDiagEvent("test", "two"));

    uint64_t next = 1;
    uint64_t dropped = 0;
    auto first = buffer.snapshotLinesFrom(next, &next, &dropped);
    CHECK(first.size() == 2, "incremental snapshot should read initial lines");
    CHECK(next == 3, "incremental snapshot should advance next sequence");
    CHECK(dropped == 0, "incremental snapshot should not drop retained lines");

    buffer.push(ultra::diagnostics::makeDiagEvent("test", "three"));
    auto second = buffer.snapshotLinesFrom(next, &next, &dropped);
    CHECK(second.size() == 1, "incremental snapshot should read only new line");
    CHECK(second.front().find("\"event\":\"three\"") != std::string::npos,
          "incremental snapshot should return newest event");
}

void test_bundle_layout() {
    ultra::test::TempDir tmp("ultra_diag_bundle");
    CHECK(tmp.valid(), "temp dir should be available");

    ultra::diagnostics::AudioRingSnapshot rx;
    rx.sample_rate = 48000;
    rx.samples = {0, 1, -1, 100};

    ultra::diagnostics::BundleBuildInput input;
    input.output_path = tmp.child("report.zip");
    input.staging_dir = tmp.child("staging");
    input.manifest_json = "{\"schema\":1,\"session_id\":\"abc\"}\n";
    input.events_jsonl = "{\"event\":\"session.state\"}\n";
    input.rx_audio = rx;
    input.config_json = "{}\n";
    input.operator_log = "log\n";
    input.system_json = "{}\n";
    input.operator_note = "note\n";
    input.replay_readme = "replay\n";

    std::string error;
    CHECK(ultra::diagnostics::buildReportBundle(input, &error), error.c_str());
    auto summary = ultra::diagnostics::inspectReportBundle(input.output_path);
    CHECK(summary.ok, summary.error.c_str());
    CHECK(std::find(summary.entries.begin(), summary.entries.end(), "manifest.json") != summary.entries.end(),
          "manifest should be present");
    CHECK(std::find(summary.entries.begin(), summary.entries.end(), "events/session.jsonl") != summary.entries.end(),
          "events JSONL should be present");
    CHECK(std::find(summary.entries.begin(), summary.entries.end(), "audio/rx_48k_s16.wav") != summary.entries.end(),
          "RX WAV should be present");
    CHECK(summary.manifest_json.find("\"session_id\":\"abc\"") != std::string::npos,
          "manifest should be extractable");
}

void test_session_summary_connected_fixture() {
    const std::string jsonl =
        "{\"schema\":1,\"seq\":1,\"ts_utc\":\"2026-05-11T14:32:18Z\",\"component\":\"session\",\"event\":\"session.state\",\"privacy\":\"redacted\",\"fields\":{\"state\":\"started\"}}\n"
        "{\"schema\":1,\"seq\":2,\"ts_utc\":\"2026-05-11T14:32:20Z\",\"component\":\"session\",\"event\":\"session.state\",\"privacy\":\"redacted\",\"fields\":{\"state\":\"connected\"}}\n"
        "{\"schema\":1,\"seq\":3,\"ts_utc\":\"2026-05-11T14:32:21Z\",\"component\":\"protocol\",\"event\":\"waveform.negotiated\",\"privacy\":\"redacted\",\"fields\":{\"waveform\":\"OFDM_CHIRP\",\"mod\":\"DQPSK\",\"rate\":\"R1/2\",\"cw\":4}}\n"
        "{\"schema\":1,\"seq\":4,\"ts_utc\":\"2026-05-11T14:32:30Z\",\"component\":\"phy\",\"event\":\"frame.rx\",\"privacy\":\"redacted\",\"fields\":{\"snr_db\":15.4,\"cfo_hz\":-3.1,\"fading\":0.41,\"cw_ok\":4,\"cw_failed\":0}}\n"
        "{\"schema\":1,\"seq\":5,\"ts_utc\":\"2026-05-11T14:32:40Z\",\"component\":\"protocol\",\"event\":\"file.transfer\",\"privacy\":\"redacted\",\"fields\":{\"direction\":\"tx\",\"path\":\"mission_brief.pdf\",\"bytes\":4712,\"seconds\":12.0,\"success\":true}}\n"
        "{\"schema\":1,\"seq\":6,\"ts_utc\":\"2026-05-11T14:32:50Z\",\"component\":\"protocol\",\"event\":\"session.stats\",\"privacy\":\"redacted\",\"fields\":{\"arq_frames_sent\":5,\"arq_retransmissions\":0,\"arq_timeouts\":0,\"arq_failed\":0}}\n"
        "{\"schema\":1,\"seq\":7,\"ts_utc\":\"2026-05-11T14:33:31Z\",\"component\":\"session\",\"event\":\"session.state\",\"privacy\":\"redacted\",\"fields\":{\"state\":\"disconnected\",\"reason\":\"peer DISCONNECT_ACK\"}}\n"
        "{\"schema\":1,\"seq\":8,\"ts_utc\":\"2026-05-11T14:33:31Z\",\"component\":\"session\",\"event\":\"session.finished\",\"privacy\":\"redacted\",\"fields\":{\"reason\":\"peer DISCONNECT_ACK\"}}\n";

    auto options = ultra::diagnostics::defaultSessionSummaryOptions();
    options.build_version = "0.3.1";
    options.build_commit = "a328d70df2d5";
    options.build_os = "Darwin-arm64";
    options.local_callsign = "N0CALL";
    auto summary = ultra::diagnostics::summarizeSessionJsonl(jsonl, options);
    CHECK(summary.ok, summary.error.c_str());
    CHECK(summary.outcome == "CONNECTED", "connected fixture should summarize as connected");
    CHECK(summary.text.find("Local callsign:  REDACTED") != std::string::npos,
          "summary should redact callsign by default");
    CHECK(summary.text.find("Mode at end:     OFDM_CHIRP DQPSK R1/2") != std::string::npos,
          "summary should include final mode");
    CHECK(summary.text.find("File transfer:   sent mission_brief.pdf (4712 bytes), CRC OK") != std::string::npos,
          "summary should reduce file transfer event");
    CHECK(summary.text.find("Decode failures: 0 (no CW failures observed)") != std::string::npos,
          "summary should report clean decode failures");
}

void test_session_summary_failed_handshake_fixture() {
    const std::string jsonl =
        "{\"schema\":1,\"seq\":1,\"ts_utc\":\"2026-05-11T15:00:00Z\",\"component\":\"session\",\"event\":\"session.state\",\"privacy\":\"redacted\",\"fields\":{\"state\":\"started\"}}\n"
        "{\"schema\":1,\"seq\":2,\"ts_utc\":\"2026-05-11T15:00:01Z\",\"component\":\"session\",\"event\":\"session.state\",\"privacy\":\"redacted\",\"fields\":{\"state\":\"probing\"}}\n"
        "{\"schema\":1,\"seq\":3,\"ts_utc\":\"2026-05-11T15:00:02Z\",\"component\":\"protocol\",\"event\":\"ping.tx\",\"privacy\":\"redacted\",\"fields\":{\"kind\":\"ping\"}}\n"
        "{\"schema\":1,\"seq\":4,\"ts_utc\":\"2026-05-11T15:00:15Z\",\"component\":\"protocol\",\"event\":\"ping.tx\",\"privacy\":\"redacted\",\"fields\":{\"kind\":\"ping\"}}\n"
        "{\"schema\":1,\"seq\":5,\"ts_utc\":\"2026-05-11T15:00:32Z\",\"component\":\"session\",\"event\":\"session.state\",\"privacy\":\"redacted\",\"fields\":{\"state\":\"disconnected\",\"reason\":\"No response\"}}\n"
        "{\"schema\":1,\"seq\":6,\"ts_utc\":\"2026-05-11T15:00:32Z\",\"component\":\"session\",\"event\":\"session.finished\",\"privacy\":\"redacted\",\"fields\":{\"reason\":\"No response\"}}\n";

    auto summary = ultra::diagnostics::summarizeSessionJsonl(
        jsonl, ultra::diagnostics::defaultSessionSummaryOptions());
    CHECK(summary.ok, summary.error.c_str());
    CHECK(summary.outcome == "TIMED_OUT", "no-response fixture should summarize as timed out");
    CHECK(summary.text.find("no PONG received") != std::string::npos,
          "failed handshake summary should explain missing PONG");
    CHECK(summary.text.find("verify peer is listening") != std::string::npos,
          "failed handshake summary should include operator next steps");
}

void test_recorder_writes_journal_and_summary() {
    ultra::test::TempDir tmp("ultra_diag_recorder");
    CHECK(tmp.valid(), "temp dir should be available");
#ifndef _WIN32
    setenv("ULTRA_DIAGNOSTICS_DIR", tmp.path().string().c_str(), 1);
#endif

    ultra::diagnostics::SessionMeta meta;
    meta.app_name = "test";
    meta.station_role = "unit";
    meta.callsign = "N0CALL";
    auto& recorder = ultra::diagnostics::DiagnosticsRecorder::instance();
    recorder.start(meta);
    recorder.emitText("session", "session.state", "{\"state\":\"connected\"}");
    recorder.emitText("protocol", "waveform.negotiated",
                      "{\"waveform\":\"OFDM_CHIRP\",\"mod\":\"DQPSK\",\"rate\":\"R1/2\"}");
    auto summary = recorder.finishSession("unit_test_clean");
    CHECK(summary.ok, summary.error.c_str());
    CHECK(std::filesystem::exists(summary.journal_path), "journal should be written to disk");
    CHECK(std::filesystem::exists(summary.path), "summary should be written to disk");
    CHECK(summary.text.find("Session outcome: CONNECTED") != std::string::npos,
          "recorder summary should include outcome");
    recorder.stop();
#ifndef _WIN32
    unsetenv("ULTRA_DIAGNOSTICS_DIR");
#endif
}

void test_tombstone_parser() {
    ultra::test::TempDir tmp("ultra_diag_tomb");
    CHECK(tmp.valid(), "temp dir should be available");

    auto missing = ultra::diagnostics::DiagnosticsRecorder::parseTombstoneFile(tmp.child("missing"));
    CHECK(!missing.valid, "missing tombstone should not parse as valid");

    const auto corrupt_path = tmp.child("corrupt");
    {
        std::ofstream out(corrupt_path, std::ios::binary);
        out << "not a tombstone\n";
    }
    auto corrupt = ultra::diagnostics::DiagnosticsRecorder::parseTombstoneFile(corrupt_path);
    CHECK(!corrupt.valid, "corrupt tombstone should not parse as valid");

    const auto valid_path = tmp.child("valid");
    {
        std::ofstream out(valid_path);
        out << "schema=1\nsignal=SIGSEGV\nsession_id=abc123\ntimestamp_epoch=42\n";
    }
    auto valid = ultra::diagnostics::DiagnosticsRecorder::parseTombstoneFile(valid_path);
    CHECK(valid.valid, "valid tombstone should parse");
    CHECK(valid.signal_name == "SIGSEGV", "signal should be parsed");
    CHECK(valid.session_id == "abc123", "session id should be parsed");
}

} // namespace

int main() {
    test_audio_ring_drop_counter();
    test_event_buffer_wraparound();
    test_event_buffer_incremental_snapshot();
    test_bundle_layout();
    test_session_summary_connected_fixture();
    test_session_summary_failed_handshake_fixture();
    test_recorder_writes_journal_and_summary();
    test_tombstone_parser();

    if (tests_failed != 0) {
        std::cout << "Diagnostics tests failed: " << tests_failed << "\n";
        return 1;
    }
    std::cout << "Diagnostics tests passed: " << tests_run << "\n";
    return 0;
}
