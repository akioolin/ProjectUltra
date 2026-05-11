#include "diagnostics/audio_ring.hpp"
#include "diagnostics/event_buffer.hpp"
#include "diagnostics/diagnostics_recorder.hpp"
#include "diagnostics/report_bundle.hpp"
#include "helpers/temp_dir.hpp"

#include <algorithm>
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

void test_tombstone_parser() {
    ultra::test::TempDir tmp("ultra_diag_tomb");
    CHECK(tmp.valid(), "temp dir should be available");

    auto missing = ultra::diagnostics::DiagnosticsRecorder::parseTombstoneFile(tmp.child("missing"));
    CHECK(!missing.valid, "missing tombstone should not parse as valid");

    const auto corrupt_path = tmp.child("corrupt");
    {
        std::ofstream out(corrupt_path);
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
    test_bundle_layout();
    test_tombstone_parser();

    if (tests_failed != 0) {
        std::cout << "Diagnostics tests failed: " << tests_failed << "\n";
        return 1;
    }
    std::cout << "Diagnostics tests passed: " << tests_run << "\n";
    return 0;
}
