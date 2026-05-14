#include "gui/modem/streaming_decoder.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ultra;
using namespace ultra::gui;

namespace {

#ifndef PROJECT_SOURCE_DIR
#define PROJECT_SOURCE_DIR "."
#endif

uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(p[1] << 8);
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool readExact(std::istream& in, void* dst, size_t len) {
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(len));
    return static_cast<size_t>(in.gcount()) == len;
}

std::vector<float> loadPcm16WavMono(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open " + path);
    }

    uint8_t riff[12] = {};
    if (!readExact(in, riff, sizeof(riff)) ||
        std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "WAVE", 4) != 0) {
        throw std::runtime_error("not a RIFF/WAVE file: " + path);
    }

    std::vector<uint8_t> fmt;
    std::vector<uint8_t> data;
    while (in) {
        uint8_t header[8] = {};
        if (!readExact(in, header, sizeof(header))) {
            break;
        }
        const uint32_t size = readLe32(header + 4);
        std::vector<uint8_t> chunk(size);
        if (size > 0 && !readExact(in, chunk.data(), size)) {
            throw std::runtime_error("truncated WAV chunk in " + path);
        }
        if (size & 1u) {
            in.seekg(1, std::ios::cur);
        }

        if (std::memcmp(header, "fmt ", 4) == 0) {
            fmt = std::move(chunk);
        } else if (std::memcmp(header, "data", 4) == 0) {
            data = std::move(chunk);
        }
    }

    if (fmt.size() < 16 || data.empty()) {
        throw std::runtime_error("WAV missing fmt/data chunk: " + path);
    }
    const uint16_t format = readLe16(fmt.data());
    const uint16_t channels = readLe16(fmt.data() + 2);
    const uint32_t sample_rate = readLe32(fmt.data() + 4);
    const uint16_t block_align = readLe16(fmt.data() + 12);
    const uint16_t bits = readLe16(fmt.data() + 14);
    if (format != 1 || bits != 16 || channels == 0 || sample_rate != 48000) {
        throw std::runtime_error("expected 48 kHz PCM16 WAV: " + path);
    }

    const size_t frames = data.size() / block_align;
    std::vector<float> samples;
    samples.reserve(frames);
    for (size_t frame = 0; frame < frames; ++frame) {
        const uint8_t* base = data.data() + frame * block_align;
        int32_t sum = 0;
        for (uint16_t ch = 0; ch < channels; ++ch) {
            const int16_t pcm = static_cast<int16_t>(readLe16(base + ch * 2));
            sum += pcm;
        }
        samples.push_back(static_cast<float>(sum) /
                          (32768.0f * static_cast<float>(channels)));
    }
    return samples;
}

struct Fixture {
    const char* name;
    bool expected_ping;
    bool expect_path1;
    bool expect_path2;
};

struct Observed {
    bool callback_ping = false;
    bool queue_ping = false;
    DecodeResult result;
};

Observed runDecoder(const std::vector<float>& samples) {
    StreamingDecoder decoder;
    decoder.setLogPrefix("PingDetectorTest");
    decoder.setMode(protocol::WaveformMode::MC_DPSK, false);
    decoder.setDataMode(Modulation::DQPSK, CodeRate::R1_4);

    Observed observed;
    decoder.setFrameCallback([&](const DecodeResult& result) {
        if (result.is_ping) {
            observed.callback_ping = true;
            observed.result = result;
        }
    });

    constexpr size_t kChunk = 4800;
    for (size_t pos = 0; pos < samples.size(); pos += kChunk) {
        const size_t len = std::min(kChunk, samples.size() - pos);
        decoder.feedAudio(samples.data() + pos, len);
        decoder.processBuffer();
        while (decoder.hasFrame()) {
            auto result = decoder.getFrame();
            if (result.is_ping) {
                observed.queue_ping = true;
                observed.result = result;
            }
        }
    }

    return observed;
}

void printMetrics(const Fixture& fixture, const Observed& observed) {
    const bool observed_ping = observed.callback_ping || observed.queue_ping;
    std::cout << "[PingDetector] " << fixture.name
              << " expected=" << (fixture.expected_ping ? "PING" : "NO_PING")
              << " observed=" << (observed_ping ? "PING" : "NO_PING");

    if (observed_ping) {
        const auto& r = observed.result;
        std::cout << " path1=" << (r.ping_by_silence ? 1 : 0)
                  << " path2=" << (r.ping_by_chirp_lock ? 1 : 0)
                  << " ratio=" << r.ping_data_to_training_ratio
                  << " train_rms=" << r.ping_training_rms
                  << " data_rms=" << r.ping_data_rms
                  << " chirp_corr=" << r.ping_chirp_corr
                  << " gap_error=" << r.ping_gap_error_samples
                  << " ldpc_attempted=" << (r.ping_ldpc_attempted ? 1 : 0);
        if (r.ping_ldpc_attempted) {
            std::cout << " ldpc_ok=" << (r.ping_ldpc_decode_succeeded ? 1 : 0)
                      << " magic_valid=" << (r.ping_ldpc_magic_valid ? 1 : 0);
        } else {
            std::cout << " ldpc_ok=skipped magic_valid=skipped";
        }
    } else {
        std::cout << " path1=0 path2=0 ratio=n/a train_rms=n/a data_rms=n/a"
                  << " chirp_corr=n/a gap_error=n/a ldpc_attempted=0"
                  << " ldpc_ok=0 magic_valid=0";
    }
    std::cout << "\n";
}

bool checkFixture(const Fixture& fixture) {
    const std::string path = std::string(PROJECT_SOURCE_DIR) +
        "/tests/fixtures/ota_ping/" + fixture.name;
    const auto samples = loadPcm16WavMono(path);
    const auto observed = runDecoder(samples);
    const bool observed_ping = observed.callback_ping || observed.queue_ping;

    printMetrics(fixture, observed);

    if (observed.callback_ping != fixture.expected_ping) {
        std::cout << "FAIL: frame callback ping mismatch for " << fixture.name << "\n";
        return false;
    }
    if (observed_ping != fixture.expected_ping) {
        std::cout << "FAIL: ping classification mismatch for " << fixture.name << "\n";
        return false;
    }
    if (fixture.expected_ping && fixture.expect_path1 &&
        !observed.result.ping_by_silence) {
        std::cout << "FAIL: PATH 1 did not fire for " << fixture.name << "\n";
        return false;
    }
    if (fixture.expected_ping && fixture.expect_path2 &&
        !observed.result.ping_by_chirp_lock) {
        std::cout << "FAIL: PATH 2 did not fire for " << fixture.name << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const std::vector<Fixture> fixtures = {
        {"ota_ping_1.wav", true, false, true},
        {"ota_ping_2.wav", true, false, true},
        {"sim_ping_awgn_snr15.wav", true, true, false},
        {"ota_noise_no_ping.wav", false, false, false},
    };

    bool ok = true;
    for (const auto& fixture : fixtures) {
        ok = checkFixture(fixture) && ok;
    }

    if (!ok) {
        return 1;
    }
    std::cout << "PingDetector: " << fixtures.size() << "/" << fixtures.size()
              << " fixtures passed\n";
    return 0;
}
