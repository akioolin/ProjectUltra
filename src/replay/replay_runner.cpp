#include "replay/replay_runner.hpp"

#include "gui/modem/streaming_decoder.hpp"
#include "io/wav_io.hpp"
#include "ultra/ofdm_link_adaptation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace ultra::replay {
namespace {

using ultra::gui::DecodeResult;
using ultra::gui::StreamingDecoder;

ModeSpec modeFromDecoderConfig(const StreamingDecoder::DecoderConfig& cfg,
                               const ModeSpec& fallback,
                               int64_t t_ms) {
    ModeSpec mode = fallback;
    mode.t_ms = t_ms;
    mode.waveform = cfg.mode;
    mode.has_waveform = true;
    mode.modulation = cfg.modulation;
    mode.has_modulation = true;
    mode.code_rate = cfg.code_rate;
    mode.has_code_rate = true;
    mode.connected = fallback.connected;
    mode.has_connected = fallback.has_connected;
    return mode;
}

ModemConfig ofdmConfigForMode(protocol::WaveformMode waveform,
                              Modulation modulation,
                              CodeRate code_rate) {
    ModemConfig config = (waveform == protocol::WaveformMode::OFDM_NARROW)
        ? presets::narrowbandOFDM()
        : presets::balanced();
    config.modulation = modulation;
    config.code_rate = code_rate;
    config.use_pilots = true;
    config.pilot_spacing =
        ofdm_link_adaptation::recommendedPilotSpacing(modulation, code_rate);
    return config;
}

void applyMode(StreamingDecoder& decoder, const ModeSpec& mode) {
    const auto waveform = mode.has_waveform ? mode.waveform : protocol::WaveformMode::MC_DPSK;
    const auto modulation = mode.has_modulation ? mode.modulation : Modulation::DQPSK;
    const auto code_rate = mode.has_code_rate ? mode.code_rate : CodeRate::R1_4;
    const bool connected = mode.has_connected ? mode.connected
        : (waveform != protocol::WaveformMode::MC_DPSK);

    if (mode.has_cw_count) {
        decoder.setFixedFrameCodewords(mode.cw_count);
    }
    if (mode.has_burst_group_size) {
        decoder.setBurstInterleaveGroupSize(mode.burst_group_size);
    }

    if (connected && protocol::isOFDMMode(waveform)) {
        auto config = ofdmConfigForMode(waveform, modulation, code_rate);
        decoder.setConnectedOFDMMode(waveform, config, modulation, code_rate);
        decoder.setBurstInterleave(waveform == protocol::WaveformMode::OFDM_CHIRP);
    } else {
        decoder.setMode(waveform, connected);
        if (protocol::isOFDMMode(waveform)) {
            auto config = ofdmConfigForMode(waveform, modulation, code_rate);
            decoder.setOFDMConfig(config);
        }
        decoder.setDataMode(modulation, code_rate);
        if (!connected || waveform != protocol::WaveformMode::OFDM_CHIRP) {
            decoder.setBurstInterleave(false);
        }
    }

    if (mode.has_cfo_hz) {
        decoder.setKnownCFO(mode.cfo_hz);
    }
}

FrameObservation observationFromDecodeResult(const DecodeResult& result,
                                             const ModeSpec& current_mode,
                                             int64_t t_ms) {
    FrameObservation frame;
    frame.origin = FrameObservation::Origin::Replay;
    frame.t_ms = t_ms;
    frame.decode_failed = !result.success;
    frame.mode = current_mode;
    frame.source_event = "replay.decoder";
    frame.snr_db = result.snr_db;
    frame.cfo_hz = result.cfo_hz;
    frame.sync_corr = result.sync_correlation;
    frame.fading_index = result.lts_fading_index;
    frame.cw_ok = result.codewords_ok;
    frame.cw_failed = result.codewords_failed;
    frame.total_cw = result.codewords_ok + result.codewords_failed;

    if (result.is_ping) {
        frame.frame_type = "PING";
        return frame;
    }

    auto header = protocol::v2::parseHeader(result.frame_data);
    if (header.valid) {
        frame.frame_seq = header.seq;
        frame.has_frame_seq = true;
        frame.frame_type = protocol::v2::frameTypeToString(header.type);
        frame.payload_len = header.payload_len;
        frame.total_cw = header.total_cw;
    } else if (result.frame_type != protocol::v2::FrameType::PROBE ||
               !result.frame_data.empty()) {
        frame.frame_type = protocol::v2::frameTypeToString(result.frame_type);
    }
    return frame;
}

std::filesystem::path audioPathForSide(const Bundle& bundle, const std::string& side) {
    if (side == "rx") {
        return bundle.rx_audio_path;
    }
    if (side == "tx") {
        return bundle.tx_audio_path;
    }
    throw std::runtime_error("audio side must be rx or tx");
}

} // namespace

ReplayTimeline runReplay(const Bundle& bundle,
                         const ParsedTimeline& live,
                         const ReplayOptions& options) {
    ReplayTimeline replay;
    const auto audio_path = audioPathForSide(bundle, options.audio_side);
    if (!std::filesystem::exists(audio_path)) {
        throw std::runtime_error("selected audio file is missing: " + audio_path.string());
    }

    auto wav = tools::io::loadWavMono48k(audio_path.string());
    if (wav.samples_48k.empty()) {
        replay.warnings.push_back("selected audio file has no samples; replay produced no frames");
        return replay;
    }
    if (wav.source_rate != tools::io::kWavTargetSampleRate) {
        replay.warnings.push_back("audio was resampled to 48 kHz for replay");
    }

    const size_t block_samples = std::max<size_t>(1, options.block_samples);
    StreamingDecoder decoder;
    decoder.setLogPrefix("replay");

    ModeSpec current_mode = live.initial_mode;
    current_mode.t_ms = bundle.audio_start_t_ms;
    applyMode(decoder, current_mode);

    size_t next_mode = 0;
    auto applyDueModes = [&](int64_t t_ms) {
        while (next_mode < live.mode_events.size() &&
               live.mode_events[next_mode].t_ms <= t_ms) {
            current_mode = live.mode_events[next_mode];
            applyMode(decoder, current_mode);
            replay.mode_events.push_back(current_mode);
            ++next_mode;
        }
    };

    auto drainFrames = [&](int64_t t_ms) {
        while (decoder.hasFrame()) {
            auto result = decoder.getFrame();
            auto cfg_mode = modeFromDecoderConfig(decoder.getConfig(), current_mode, t_ms);
            auto frame = observationFromDecodeResult(result, cfg_mode, t_ms);
            replay.frames.push_back(std::move(frame));
        }
    };

    const size_t drain_samples = static_cast<size_t>(
        (static_cast<int64_t>(std::max(0, options.drain_ms)) *
         static_cast<int64_t>(tools::io::kWavTargetSampleRate)) / 1000);
    const size_t total_samples = wav.samples_48k.size() + drain_samples;

    size_t cursor = 0;
    std::vector<float> block;
    block.reserve(block_samples);

    while (cursor < total_samples) {
        const int64_t t_ms = bundle.audio_start_t_ms +
            static_cast<int64_t>((cursor * 1000ull) / tools::io::kWavTargetSampleRate);
        applyDueModes(t_ms);

        const size_t count = std::min(block_samples, total_samples - cursor);
        block.assign(count, 0.0f);
        for (size_t i = 0; i < count; ++i) {
            const size_t idx = cursor + i;
            if (idx < wav.samples_48k.size()) {
                block[i] = wav.samples_48k[idx];
            }
        }

        decoder.feedAudio(block.data(), block.size());
        decoder.processBuffer();
        drainFrames(t_ms);
        cursor += count;

        if (options.realtime) {
            const auto sleep_us = static_cast<int64_t>(
                (count * 1000000ull) / tools::io::kWavTargetSampleRate);
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        }
    }

    applyDueModes(bundle.audio_start_t_ms +
                  static_cast<int64_t>((total_samples * 1000ull) /
                                       tools::io::kWavTargetSampleRate));
    drainFrames(bundle.audio_start_t_ms +
                static_cast<int64_t>((total_samples * 1000ull) /
                                     tools::io::kWavTargetSampleRate));
    decoder.stop();

    if (next_mode < live.mode_events.size()) {
        replay.warnings.push_back(
            std::to_string(live.mode_events.size() - next_mode) +
            " mode events occurred after available audio and were not applied");
    }
    return replay;
}

} // namespace ultra::replay
