#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/logging.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace ultra;
using namespace ultra::gui;
namespace v2 = ultra::protocol::v2;

namespace {

std::vector<float> withSilence(const std::vector<float>& frame) {
    std::vector<float> audio;
    audio.resize(48000, 0.0f);  // 1s lead-in lets chirp search establish noise floor.
    audio.insert(audio.end(), frame.begin(), frame.end());
    audio.resize(audio.size() + 96000, 0.0f);  // trailing room for final decode.
    return audio;
}

bool feedInChunks(StreamingDecoder& decoder, const std::vector<float>& audio) {
    constexpr size_t kChunk = 4800;  // 100 ms audio chunks.
    for (size_t pos = 0; pos < audio.size(); pos += kChunk) {
        const size_t len = std::min(kChunk, audio.size() - pos);
        decoder.feedAudio(audio.data() + pos, len);
        decoder.processBuffer();
    }
    return decoder.hasFrame();
}

bool runLoopback(const char* name,
                 const MultiCarrierDPSKConfig& mc_config,
                 Modulation modulation,
                 const Bytes& serialized,
                 v2::FrameType expected_type) {
    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::MC_DPSK);
    encoder.setMCDPSKConfig(mc_config);
    encoder.setDataMode(modulation, CodeRate::R1_4);

    auto samples = encoder.encodeFrame(serialized);
    if (samples.empty()) {
        std::cout << "FAIL: " << name << " encoder produced no samples\n";
        return false;
    }

    StreamingDecoder decoder;
    decoder.setLogPrefix("TEST");
    decoder.setMode(protocol::WaveformMode::MC_DPSK, true);
    decoder.setMCDPSKConfig(mc_config);
    decoder.setDataMode(modulation, CodeRate::R1_4);

    bool callback_seen = false;
    DecodeResult decoded;
    decoder.setFrameCallback([&](const DecodeResult& result) {
        if (result.success) {
            callback_seen = true;
            decoded = result;
        }
    });

    auto audio = withSilence(samples);
    feedInChunks(decoder, audio);

    while (decoder.hasFrame()) {
        auto result = decoder.getFrame();
        if (result.success) {
            callback_seen = true;
            decoded = result;
            break;
        }
    }

    if (!callback_seen) {
        std::cout << "FAIL: " << name << " streaming decoder did not recover MC-DPSK frame\n";
        return false;
    }
    if (decoded.frame_type != expected_type) {
        std::cout << "FAIL: " << name << " decoded frame type mismatch\n";
        return false;
    }

    if (expected_type == v2::FrameType::DATA && decoded.frame_data != serialized) {
        std::cout << "FAIL: " << name << " frame payload mismatch\n";
        return false;
    }
    if (expected_type == v2::FrameType::CONNECT &&
        !v2::ConnectFrame::deserialize(decoded.frame_data)) {
        std::cout << "FAIL: " << name << " CONNECT frame did not parse\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    setLogLevel(LogLevel::ERROR);

    const Bytes payload = {
        0x55, 0x4c, 0x54, 0x52, 0x41, 0x20, 0x4d, 0x43,
        0x44, 0x50, 0x53, 0x4b, 0x20, 0x4f, 0x4b
    };
    auto data_frame = v2::DataFrame::makeData("ALPHA", "BRAVO", 7, payload, CodeRate::R1_4);
    const Bytes data_serialized = data_frame.serialize();
    auto connect_frame = v2::ConnectFrame::makeConnect(
        "ALPHA", "BRAVO", static_cast<uint8_t>(protocol::WaveformMode::MC_DPSK),
        static_cast<uint8_t>(Modulation::DBPSK), static_cast<uint8_t>(CodeRate::R1_4));
    const Bytes connect_serialized = connect_frame.serialize();

    if (!runLoopback("standard data", mc_dpsk_presets::level8(), Modulation::DQPSK,
                     data_serialized, v2::FrameType::DATA)) return 1;
    if (!runLoopback("standard connect", mc_dpsk_presets::level8(), Modulation::DQPSK,
                     connect_serialized, v2::FrameType::CONNECT)) return 1;
    if (!runLoopback("robust_low data", mc_dpsk_presets::robust_low(), Modulation::DBPSK,
                     data_serialized, v2::FrameType::DATA)) return 1;
    if (!runLoopback("robust_low connect", mc_dpsk_presets::robust_low(), Modulation::DBPSK,
                     connect_serialized, v2::FrameType::CONNECT)) return 1;
    if (!runLoopback("robust_mid data", mc_dpsk_presets::robust_mid(), Modulation::DBPSK,
                     data_serialized, v2::FrameType::DATA)) return 1;
    if (!runLoopback("robust_mid connect", mc_dpsk_presets::robust_mid(), Modulation::DBPSK,
                     connect_serialized, v2::FrameType::CONNECT)) return 1;

    std::cout << "Streaming MC-DPSK loopback: PASS\n";
    return 0;
}
