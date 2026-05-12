#pragma once

#include "protocol/frame_v2.hpp"
#include "ultra/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ultra::replay {

struct ModeSpec {
    int64_t t_ms = 0;
    protocol::WaveformMode waveform = protocol::WaveformMode::MC_DPSK;
    bool has_waveform = false;
    Modulation modulation = Modulation::DQPSK;
    bool has_modulation = false;
    CodeRate code_rate = CodeRate::R1_4;
    bool has_code_rate = false;
    int cw_count = 1;
    bool has_cw_count = false;
    bool connected = false;
    bool has_connected = false;
    float cfo_hz = 0.0f;
    bool has_cfo_hz = false;
    int burst_group_size = 8;
    bool has_burst_group_size = false;
    std::string source_event;
    std::string note;
};

struct FrameObservation {
    enum class Origin {
        Live,
        Replay,
    };

    Origin origin = Origin::Live;
    int64_t t_ms = -1;
    int frame_seq = -1;
    bool has_frame_seq = false;
    bool decode_failed = false;
    ModeSpec mode;
    std::string frame_type;
    int payload_len = -1;
    int total_cw = -1;
    int cw_ok = -1;
    int cw_failed = -1;
    std::optional<float> snr_db;
    std::optional<float> fading_index;
    std::optional<float> sync_corr;
    std::optional<float> cfo_hz;
    std::optional<float> llr_abs_mean;
    std::string source_event;
};

struct ParsedTimeline {
    ModeSpec initial_mode;
    bool initial_mode_assumed = true;
    std::vector<ModeSpec> mode_events;
    std::vector<FrameObservation> live_frames;
    std::vector<std::string> warnings;
};

std::string modeLabel(const ModeSpec& mode);
std::string compactModeLabel(const ModeSpec& mode);
std::string frameOutcomeLabel(const FrameObservation& frame);

std::optional<protocol::WaveformMode> parseWaveformMode(std::string value);
std::optional<Modulation> parseModulation(std::string value);
std::optional<CodeRate> parseCodeRate(std::string value);

ModeSpec mergeMode(const ModeSpec& base, const ModeSpec& patch);
ParsedTimeline parseEventTimeline(const std::string& jsonl,
                                  const ModeSpec& manifest_initial,
                                  bool manifest_initial_available);

} // namespace ultra::replay
