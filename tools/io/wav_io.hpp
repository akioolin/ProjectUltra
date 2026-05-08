#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ultra::tools::io {

constexpr uint32_t kWavTargetSampleRate = 48000;

struct LoadedWav {
    std::string path;
    std::vector<float> samples_48k;
    uint32_t source_rate = 0;
    uint16_t source_channels = 0;
    uint16_t source_bits = 0;
    uint16_t source_format = 0;
};

LoadedWav loadWavMono48k(const std::string& path);

bool writeWavF32Mono(const std::string& path,
                     const std::vector<float>& samples,
                     uint32_t sample_rate = kWavTargetSampleRate);

}  // namespace ultra::tools::io
