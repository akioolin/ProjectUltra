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

        // State transitions consume one wakeup each; feedAudio has already made
        // new data available for this call, so one process pass per chunk keeps
        // the test deterministic without wall-clock sleeps.
        while (decoder.hasFrame()) {
            return true;
        }
    }
    return decoder.hasFrame();
}

}  // namespace

int main() {
    setLogLevel(LogLevel::ERROR);

    const Bytes payload = {
        0x55, 0x4c, 0x54, 0x52, 0x41, 0x20, 0x4d, 0x43,
        0x44, 0x50, 0x53, 0x4b, 0x20, 0x4f, 0x4b
    };

    auto tx_frame = v2::DataFrame::makeData("ALPHA", "BRAVO", 7, payload, CodeRate::R1_4);
    const Bytes serialized = tx_frame.serialize();

    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::MC_DPSK);
    encoder.setMCDPSKCarriers(8);
    encoder.setDataMode(Modulation::DQPSK, CodeRate::R1_4);

    auto samples = encoder.encodeFrame(serialized);
    if (samples.empty()) {
        std::cout << "FAIL: encoder produced no samples\n";
        return 1;
    }

    StreamingDecoder decoder;
    decoder.setLogPrefix("TEST");
    decoder.setMode(protocol::WaveformMode::MC_DPSK, true);
    decoder.setMCDPSKCarriers(8);
    decoder.setDataMode(Modulation::DQPSK, CodeRate::R1_4);

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
        std::cout << "FAIL: streaming decoder did not recover MC-DPSK frame\n";
        return 1;
    }
    if (decoded.frame_type != v2::FrameType::DATA) {
        std::cout << "FAIL: decoded frame type was not DATA\n";
        return 1;
    }

    auto parsed = v2::DataFrame::deserialize(decoded.frame_data);
    if (!parsed) {
        std::cout << "FAIL: decoded DATA frame did not parse\n";
        return 1;
    }
    if (parsed->seq != 7) {
        std::cout << "FAIL: sequence mismatch\n";
        return 1;
    }
    if (parsed->payload != payload) {
        std::cout << "FAIL: payload mismatch\n";
        return 1;
    }

    std::cout << "Streaming MC-DPSK loopback: PASS\n";
    return 0;
}
