#include "ota_simulator/clip_gen.hpp"

#include "gui/modem/streaming_encoder.hpp"
#include "io/wav_io.hpp"
#include "psk/multi_carrier_dpsk.hpp"
#include "protocol/frame_v2.hpp"
#include "replay/json_util.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace ultra::tools::ota {
namespace {

namespace fs = std::filesystem;
namespace json = ultra::replay::json;
namespace v2 = ultra::protocol::v2;

std::string upperToken(std::string value) {
    std::replace(value.begin(), value.end(), '-', '_');
    for (char& c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string hexBytes(const Bytes& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

std::string sidecarPathFor(const std::string& wav_path) {
    fs::path path(wav_path);
    path.replace_extension(".clip.json");
    return path.string();
}

Bytes frameBytesFor(const std::string& frame_type,
                    const std::string& callsign,
                    const std::string& peer_callsign) {
    if (frame_type == "PING" || frame_type == "PONG") {
        return v2::PingFrame::serialize();
    }
    if (frame_type == "CONNECT") {
        return v2::ConnectFrame::makeConnect(
            callsign, peer_callsign,
            protocol::ModeCapabilities::ALL | protocol::ModeCapabilities::PHY_MASK_V1,
            static_cast<uint8_t>(protocol::WaveformMode::AUTO),
            static_cast<uint8_t>(Modulation::AUTO),
            static_cast<uint8_t>(CodeRate::AUTO),
            0).serialize();
    }
    if (frame_type == "CONNECT_ACK") {
        return v2::ConnectFrame::makeConnectAck(
            callsign, peer_callsign,
            static_cast<uint8_t>(protocol::WaveformMode::MC_DPSK),
            Modulation::DQPSK, CodeRate::R1_4,
            15.0f, 0.0f, v2::kDefaultFixedFrameCodewords).serialize();
    }
    if (frame_type == "DATA") {
        return v2::DataFrame::makeData(
            callsign, peer_callsign, 1,
            std::string("ota_simulator_data"), CodeRate::R1_4).serialize();
    }
    if (frame_type == "ACK") {
        return v2::ControlFrame::makeAck(callsign, peer_callsign, 1).serialize();
    }
    if (frame_type == "DISCONNECT") {
        return v2::ControlFrame::makeDisconnect(callsign, peer_callsign).serialize();
    }
    throw std::runtime_error("unsupported frame for gen: " + frame_type);
}

void writeSidecar(const ClipGenOptions& options,
                  const std::string& canonical_frame,
                  const Bytes& source_bytes,
                  size_t sample_count) {
    const std::string path = sidecarPathFor(options.out_path);
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to write clip sidecar: " + path);
    }
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"frame_type\": \"" << json::escape(canonical_frame) << "\",\n"
        << "  \"callsign\": \"" << json::escape(options.callsign) << "\",\n"
        << "  \"peer_callsign\": \"" << json::escape(options.peer_callsign) << "\",\n"
        << "  \"sample_rate\": 48000,\n"
        << "  \"sample_count\": " << sample_count << ",\n"
        << "  \"source_bytes_hex\": \"" << hexBytes(source_bytes) << "\"\n"
        << "}\n";
}

}  // namespace

int generateClip(const ClipGenOptions& options) {
    if (options.frame.empty() || options.callsign.empty() || options.out_path.empty()) {
        throw std::runtime_error("gen requires --frame, --callsign, and --out");
    }

    const std::string frame = upperToken(options.frame);
    const Bytes source_bytes = frameBytesFor(frame, options.callsign, options.peer_callsign);

    gui::StreamingEncoder encoder;
    encoder.setMCDPSKConfig(mc_dpsk_presets::level8());
    encoder.setMode(protocol::WaveformMode::MC_DPSK);
    encoder.setDataMode(Modulation::DQPSK, CodeRate::R1_4);

    std::vector<float> samples;
    if (frame == "PING" || frame == "PONG") {
        samples = encoder.encodePing();
    } else {
        samples = encoder.encodeFrame(source_bytes);
    }

    if (!io::writeWavF32Mono(options.out_path, samples)) {
        throw std::runtime_error("failed to write WAV: " + options.out_path);
    }
    writeSidecar(options, frame, source_bytes, samples.size());

    std::cout << "ota_simulator: wrote " << options.out_path
              << " samples=" << samples.size()
              << " frame=" << frame << "\n";
    return 0;
}

}  // namespace ultra::tools::ota
