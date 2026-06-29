#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "ota_channel_core/channel.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/logging.hpp"

#include <algorithm>
#include <cmath>
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
                 v2::FrameType expected_type,
                 bool awgn_channel = false,
                 float channel_snr_db = 20.0f) {
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
    // #72: handshake-negotiation frames (CONNECT/CONNECT_ACK) ride the fixed DBPSK
    // control profile — like OFDM's QPSK control profile — so they are peer-decodable
    // regardless of the negotiated DATA constellation. In production the receiver
    // decodes them while still at the default DBPSK control state (the data mode is
    // applied only AFTER the frame decodes). Mirror that: decode CONNECT at DBPSK.
    const bool handshake_frame = (expected_type == v2::FrameType::CONNECT ||
                                  expected_type == v2::FrameType::CONNECT_ACK);
    decoder.setDataMode(handshake_frame ? Modulation::DBPSK : modulation,
                        CodeRate::R1_4);

    bool callback_seen = false;
    DecodeResult decoded;
    decoder.setFrameCallback([&](const DecodeResult& result) {
        if (result.success) {
            callback_seen = true;
            decoded = result;
        }
    });

    auto audio = withSilence(samples);
    if (awgn_channel) {
        ultra::ota_channel_core::SimulatedChannel channel;
        channel.setSeed(0x5a17u);
        channel.configure(channel_snr_db, ultra::ota_channel_core::ChannelType::AWGN);
        channel.transmitFromA(audio);
        audio = channel.receiveForB(audio.size());
    }
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
    if (decoded.snr_source != SNRSource::MCDPSK_IN_BAND ||
        !std::isfinite(decoded.snr_db) || decoded.snr_db <= 1.0f) {
        std::cout << "FAIL: " << name << " MC-DPSK connected SNR invalid: "
                  << decoded.snr_db << " dB ("
                  << snrSourceToString(decoded.snr_source) << ")\n";
        return false;
    }
    std::cout << name << " MC-DPSK connected SNR="
              << decoded.snr_db << " dB ("
              << snrSourceToString(decoded.snr_source) << ")\n";

    return true;
}

bool runLowAmplitudePing(const char* name,
                         const MultiCarrierDPSKConfig& mc_config,
                         float amplitude_scale) {
    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::MC_DPSK);
    encoder.setMCDPSKConfig(mc_config);

    auto samples = encoder.encodePing();
    if (samples.empty()) {
        std::cout << "FAIL: " << name << " encoder produced no PING samples\n";
        return false;
    }
    for (auto& s : samples) {
        s *= amplitude_scale;
    }

    StreamingDecoder decoder;
    decoder.setLogPrefix("TEST");
    decoder.setMode(protocol::WaveformMode::MC_DPSK, false);
    decoder.setMCDPSKConfig(mc_config);

    bool ping_callback_seen = false;
    decoder.setPingCallback([&](float, float) {
        ping_callback_seen = true;
    });

    auto audio = withSilence(samples);
    feedInChunks(decoder, audio);

    bool ping_frame_seen = false;
    while (decoder.hasFrame()) {
        auto result = decoder.getFrame();
        if (result.success && result.is_ping) {
            ping_frame_seen = true;
            break;
        }
    }

    if (!ping_callback_seen || !ping_frame_seen) {
        std::cout << "FAIL: " << name << " low-amplitude PING was not detected\n";
        return false;
    }

    return true;
}

bool runContinuousBurstLoopback() {
    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::MC_DPSK);
    encoder.setMCDPSKConfig(mc_dpsk_presets::level8());
    encoder.setDataMode(Modulation::DQPSK, CodeRate::R1_4);

    const std::vector<size_t> payload_sizes{32, 50, 50};
    std::vector<Bytes> serialized_frames;
    for (uint16_t i = 0; i < 3; ++i) {
        Bytes payload(payload_sizes[i]);
        for (size_t j = 0; j < payload.size(); ++j) {
            payload[j] = static_cast<uint8_t>(0x30 + i * 17 + j);
        }
        auto frame = v2::DataFrame::makeData("ALPHA", "BRAVO",
                                             static_cast<uint16_t>(40 + i),
                                             payload, CodeRate::R1_4);
        frame.flags = static_cast<uint8_t>(
            v2::Flags::VERSION_V2 |
            (i + 1 < 3 ? v2::Flags::MORE_FRAG : v2::Flags::NONE));
        serialized_frames.push_back(frame.serialize());
    }

    auto samples = encoder.encodeBurstLight(serialized_frames);
    if (samples.empty()) {
        std::cout << "FAIL: continuous burst encoder produced no samples\n";
        return false;
    }

    StreamingDecoder decoder;
    decoder.setLogPrefix("TEST");
    decoder.setMode(protocol::WaveformMode::MC_DPSK, true);
    decoder.setMCDPSKConfig(mc_dpsk_presets::level8());
    decoder.setDataMode(Modulation::DQPSK, CodeRate::R1_4);

    auto audio = withSilence(samples);
    feedInChunks(decoder, audio);

    std::vector<Bytes> decoded_frames;
    while (decoder.hasFrame()) {
        auto result = decoder.getFrame();
        if (result.success && v2::isDataFrame(result.frame_type)) {
            decoded_frames.push_back(result.frame_data);
        }
    }

    if (decoded_frames.size() != serialized_frames.size()) {
        std::cout << "FAIL: continuous burst decoded " << decoded_frames.size()
                  << " frames, expected " << serialized_frames.size() << "\n";
        return false;
    }

    for (size_t i = 0; i < serialized_frames.size(); ++i) {
        if (decoded_frames[i] != serialized_frames[i]) {
            std::cout << "FAIL: continuous burst frame " << i << " mismatch\n";
            return false;
        }
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
    if (!runLoopback("standard awgn12 data", mc_dpsk_presets::level8(), Modulation::DQPSK,
                     data_serialized, v2::FrameType::DATA, true, 12.0f)) return 1;
    if (!runLoopback("robust_low data", mc_dpsk_presets::robust_low(), Modulation::DBPSK,
                     data_serialized, v2::FrameType::DATA)) return 1;
    if (!runLoopback("robust_low connect", mc_dpsk_presets::robust_low(), Modulation::DBPSK,
                     connect_serialized, v2::FrameType::CONNECT)) return 1;
    if (!runLoopback("robust_mid data", mc_dpsk_presets::robust_mid(), Modulation::DBPSK,
                     data_serialized, v2::FrameType::DATA)) return 1;
    if (!runLoopback("robust_mid connect", mc_dpsk_presets::robust_mid(), Modulation::DBPSK,
                     connect_serialized, v2::FrameType::CONNECT)) return 1;
    if (!runLoopback("robust data", mc_dpsk_presets::robust(), Modulation::DQPSK,
                     data_serialized, v2::FrameType::DATA)) return 1;
    if (!runLoopback("robust connect", mc_dpsk_presets::robust(), Modulation::DQPSK,
                     connect_serialized, v2::FrameType::CONNECT)) return 1;
    if (!runContinuousBurstLoopback()) return 1;
    if (!runLowAmplitudePing("robust low-amplitude ping", mc_dpsk_presets::robust(),
                             0.030f)) return 1;

    std::cout << "Streaming MC-DPSK loopback: PASS\n";
    return 0;
}
