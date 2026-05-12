#include "diagnostics/audio_ring.hpp"
#include "diagnostics/report_bundle.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "helpers/temp_dir.hpp"
#include "replay/bundle_loader.hpp"
#include "replay/divergence_report.hpp"
#include "replay/event_timeline.hpp"
#include "replay/replay_runner.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace v2 = ultra::protocol::v2;

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

ultra::replay::ModeSpec mode(int64_t t_ms = 0) {
    ultra::replay::ModeSpec m;
    m.t_ms = t_ms;
    m.waveform = ultra::protocol::WaveformMode::OFDM_CHIRP;
    m.has_waveform = true;
    m.modulation = ultra::Modulation::D8PSK;
    m.has_modulation = true;
    m.code_rate = ultra::CodeRate::R3_4;
    m.has_code_rate = true;
    m.cw_count = 4;
    m.has_cw_count = true;
    m.connected = true;
    m.has_connected = true;
    return m;
}

ultra::replay::FrameObservation frame(int seq, int cw_ok, bool fail = false) {
    ultra::replay::FrameObservation f;
    f.origin = ultra::replay::FrameObservation::Origin::Live;
    f.t_ms = 1000 + seq * 10;
    f.frame_seq = seq;
    f.has_frame_seq = true;
    f.decode_failed = fail;
    f.mode = mode();
    f.frame_type = fail ? "" : "DATA";
    f.payload_len = 180;
    f.total_cw = 4;
    f.cw_ok = cw_ok;
    f.cw_failed = 4 - cw_ok;
    f.snr_db = 15.0f;
    f.sync_corr = 0.90f;
    f.llr_abs_mean = 2.0f;
    return f;
}

void appendSilence(std::vector<float>& samples, size_t count) {
    samples.insert(samples.end(), count, 0.0f);
}

ultra::diagnostics::AudioRingSnapshot snapshotFromFloatAudio(
    const std::vector<float>& samples) {
    ultra::diagnostics::Pcm16Ring ring(samples.size() + 16, 48000);
    ring.push(ultra::SampleSpan(samples.data(), samples.size()));
    return ring.snapshot();
}

ultra::replay::ParsedTimeline liveWith(std::vector<ultra::replay::FrameObservation> frames) {
    ultra::replay::ParsedTimeline live;
    live.initial_mode = mode();
    live.initial_mode_assumed = false;
    live.mode_events.push_back(mode(12300));
    live.live_frames = std::move(frames);
    return live;
}

ultra::replay::ReplayTimeline replayWith(std::vector<ultra::replay::FrameObservation> frames) {
    ultra::replay::ReplayTimeline replay;
    for (auto& f : frames) {
        f.origin = ultra::replay::FrameObservation::Origin::Replay;
    }
    replay.mode_events.push_back(mode(12300));
    replay.frames = std::move(frames);
    return replay;
}

void test_bundle_loader_and_timeline_parse() {
    ultra::test::TempDir tmp("ultra_replay_bundle");
    CHECK(tmp.valid(), "temp dir should be available");

    ultra::diagnostics::AudioRingSnapshot rx;
    rx.sample_rate = 48000;
    rx.samples = {0, 100, -100, 0};

    ultra::diagnostics::BundleBuildInput input;
    input.output_path = tmp.child("report.zip");
    input.staging_dir = tmp.child("staging");
    input.manifest_json =
        "{\"schema\":1,\"session_id\":\"abc\","
        "\"initial_mode\":{\"waveform\":\"MC-DPSK\",\"mod\":\"DQPSK\",\"rate\":\"R1/4\",\"connected\":false},"
        "\"audio\":{\"sample_rate\":48000,\"rx_samples\":4,\"rx_dropped_samples\":0,\"start_t_ms\":250}}\n";
    input.events_jsonl =
        "{\"schema\":1,\"seq\":1,\"ts_utc\":\"2026-05-11T14:32:20.000Z\","
        "\"t_ms\":0,\"component\":\"session\",\"event\":\"session.state\","
        "\"privacy\":\"redacted\",\"fields\":{\"state\":\"listening\"}}\n"
        "{\"schema\":1,\"seq\":2,\"ts_utc\":\"2026-05-11T14:32:32.300Z\","
        "\"t_ms\":12300,\"component\":\"protocol\",\"event\":\"waveform.negotiated\","
        "\"privacy\":\"redacted\",\"fields\":{\"waveform\":\"OFDM_CHIRP\",\"mod\":\"D8PSK\",\"rate\":\"R3/4\",\"cw\":4}}\n"
        "{\"schema\":1,\"seq\":3,\"ts_utc\":\"2026-05-11T14:32:38.342Z\","
        "\"t_ms\":18342,\"component\":\"phy\",\"event\":\"frame.rx\","
        "\"privacy\":\"redacted\",\"fields\":{\"waveform\":\"OFDM_CHIRP\",\"frame_type\":\"DATA\",\"seq\":17,"
        "\"payload_len\":180,\"total_cw\":4,\"cw_ok\":4,\"cw_failed\":0,\"snr_db\":15.8,"
        "\"fading_index\":0.41,\"sync_corr\":0.92,\"cfo_hz\":-3.1,\"llr_abs_mean\":2.4}}\n";
    input.rx_audio = rx;
    input.config_json = "{}\n";
    input.operator_log = "log\n";
    input.system_json = "{}\n";
    input.operator_note = "note\n";
    input.replay_readme = "replay\n";

    std::string error;
    CHECK(ultra::diagnostics::buildReportBundle(input, &error), error.c_str());
    auto bundle = ultra::replay::loadBundle(input.output_path);
    CHECK(bundle.initial_mode_available, "initial mode should parse from manifest");
    CHECK(bundle.audio_start_t_ms == 250, "audio start offset should parse");
    CHECK(!bundle.audio_start_assumed, "audio start offset should not be assumed");
    CHECK(std::filesystem::exists(bundle.rx_audio_path), "rx audio should be extracted");

    auto timeline = ultra::replay::parseEventTimeline(
        bundle.events_jsonl, bundle.initial_mode, bundle.initial_mode_available);
    CHECK(timeline.mode_events.size() == 2, "session state plus waveform event should form mode events");
    CHECK(timeline.live_frames.size() == 1, "one live frame should parse");
    CHECK(timeline.live_frames.front().has_frame_seq, "live frame seq should parse");
    CHECK(timeline.live_frames.front().frame_seq == 17, "live frame seq value should match");
}

void test_clean_session_matches_exactly() {
    auto live = liveWith({frame(17, 4), frame(18, 4)});
    auto replay = replayWith({frame(17, 4), frame(18, 4)});
    auto report = ultra::replay::compareTimelines(live, replay);
    CHECK(report.summary.live_frames == 2, "clean fixture should count live frames");
    CHECK(report.summary.replay_frames == 2, "clean fixture should count replay frames");
    CHECK(report.summary.exact_matches == 2, "clean fixture should match exactly");
    CHECK(report.summary.divergent == 0, "clean fixture should not diverge");
    CHECK(ultra::replay::renderTextReport(report).find("Exact matches:        2") != std::string::npos,
          "text report should include exact match count");
}

void test_runner_replays_clean_mcdpsk_bundle() {
    ultra::test::TempDir tmp("ultra_replay_runner");
    CHECK(tmp.valid(), "temp dir should be available");

    constexpr uint16_t seq = 17;
    ultra::Bytes payload(32);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>('A' + (i % 26));
    }
    auto data = v2::DataFrame::makeData("ALPHA", "BRAVO", seq,
                                        payload, ultra::CodeRate::R1_4);

    ultra::gui::StreamingEncoder encoder;
    encoder.setMode(ultra::protocol::WaveformMode::MC_DPSK);
    encoder.setMCDPSKConfig(ultra::mc_dpsk_presets::level8());
    encoder.setDataMode(ultra::Modulation::DQPSK, ultra::CodeRate::R1_4);
    auto encoded = encoder.encodeFrame(data.serialize());
    CHECK(!encoded.empty(), "streaming encoder should produce replay fixture audio");

    std::vector<float> audio;
    appendSilence(audio, 48000);
    audio.insert(audio.end(), encoded.begin(), encoded.end());
    appendSilence(audio, 96000);

    ultra::diagnostics::BundleBuildInput input;
    input.output_path = tmp.child("runner.zip");
    input.staging_dir = tmp.child("runner_staging");
    input.manifest_json =
        "{\"schema\":1,\"session_id\":\"runner\","
        "\"initial_mode\":{\"waveform\":\"MC-DPSK\",\"mod\":\"DQPSK\","
        "\"rate\":\"R1/4\",\"cw\":1,\"connected\":true},"
        "\"audio\":{\"sample_rate\":48000,\"rx_samples\":" +
        std::to_string(audio.size()) + ",\"start_t_ms\":0}}\n";
    input.events_jsonl =
        "{\"schema\":1,\"seq\":1,\"t_ms\":0,\"component\":\"session\","
        "\"event\":\"session.state\",\"privacy\":\"redacted\","
        "\"fields\":{\"state\":\"connected\"}}\n"
        "{\"schema\":1,\"seq\":2,\"t_ms\":0,\"component\":\"protocol\","
        "\"event\":\"waveform.negotiated\",\"privacy\":\"redacted\","
        "\"fields\":{\"waveform\":\"MC-DPSK\",\"mod\":\"DQPSK\","
        "\"rate\":\"R1/4\",\"cw\":1}}\n"
        "{\"schema\":1,\"seq\":3,\"t_ms\":1800,\"component\":\"phy\","
        "\"event\":\"frame.rx\",\"privacy\":\"redacted\",\"fields\":{"
        "\"waveform\":\"MC-DPSK\",\"frame_type\":\"DATA\",\"seq\":" +
        std::to_string(seq) + ",\"payload_len\":" +
        std::to_string(payload.size()) + ",\"total_cw\":" +
        std::to_string(data.total_cw) + "}}\n";
    input.rx_audio = snapshotFromFloatAudio(audio);
    input.config_json = "{}\n";
    input.operator_log = "log\n";
    input.system_json = "{}\n";
    input.operator_note = "note\n";
    input.replay_readme = "replay\n";

    std::string error;
    CHECK(ultra::diagnostics::buildReportBundle(input, &error), error.c_str());

    auto bundle = ultra::replay::loadBundle(input.output_path);
    auto live = ultra::replay::parseEventTimeline(
        bundle.events_jsonl, bundle.initial_mode, bundle.initial_mode_available);
    ultra::replay::ReplayOptions options;
    options.block_samples = 4800;
    options.drain_ms = 3000;
    auto replay = ultra::replay::runReplay(bundle, live, options);
    auto report = ultra::replay::compareTimelines(live, replay);

    CHECK(report.summary.live_frames == 1, "runner fixture should have one live frame");
    CHECK(report.summary.replay_frames == 1, "runner fixture should have one replay frame");
    CHECK(report.summary.exact_matches == 1, "runner fixture should match exactly");
}

void test_live_decode_failed_but_replay_succeeds() {
    auto live_fail = frame(20, 0, true);
    live_fail.cw_failed = 4;
    auto replay_ok = frame(20, 4, false);
    auto report = ultra::replay::compareTimelines(liveWith({live_fail}), replayWith({replay_ok}));
    CHECK(report.summary.divergent == 1, "decode fail vs replay success should diverge");
    CHECK(!report.rows.empty(), "divergent report should have a row");
    CHECK(report.rows.front().reason.find("outcome mismatch") != std::string::npos,
          "divergence reason should identify outcome mismatch");
}

void test_replay_only_sync_is_flagged() {
    auto report = ultra::replay::compareTimelines(liveWith({frame(17, 4)}),
                                                 replayWith({frame(17, 4), frame(19, 4)}));
    CHECK(report.summary.exact_matches == 1, "shared frame should still match");
    CHECK(report.summary.replay_only == 1, "spurious replay sync should be replay-only");
    const std::string text = ultra::replay::renderTextReport(report);
    CHECK(text.find("REPLAY_ONLY") != std::string::npos,
          "text report should expose replay-only sync");
}

void test_sparse_old_jsonl_degrades_gracefully() {
    ultra::replay::ModeSpec initial;
    const std::string jsonl =
        "{\"schema\":1,\"seq\":1,\"ts_utc\":\"2026-05-11T14:32:20Z\","
        "\"component\":\"phy\",\"event\":\"frame.rx\",\"privacy\":\"redacted\","
        "\"fields\":{\"snr_db\":15.4,\"cw_ok\":4,\"cw_failed\":0}}\n";
    auto timeline = ultra::replay::parseEventTimeline(jsonl, initial, false);
    CHECK(timeline.live_frames.size() == 1, "sparse frame should still parse");
    CHECK(!timeline.live_frames.front().has_frame_seq, "sparse frame should be unkeyed");
    auto report = ultra::replay::compareTimelines(timeline, replayWith({}));
    CHECK(report.summary.unkeyed_live == 1, "unkeyed live frame should be reported");
    CHECK(!report.warnings.empty(), "missing initial mode should warn");
}

} // namespace

int main() {
    test_bundle_loader_and_timeline_parse();
    test_clean_session_matches_exactly();
    test_runner_replays_clean_mcdpsk_bundle();
    test_live_decode_failed_but_replay_succeeds();
    test_replay_only_sync_is_flagged();
    test_sparse_old_jsonl_degrades_gracefully();

    if (tests_failed != 0) {
        std::cout << "Replay tests failed: " << tests_failed << "\n";
        return 1;
    }
    std::cout << "Replay tests passed: " << tests_run << "\n";
    return 0;
}
