#include "ota_channel_core/models.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace channel = ultra::ota_channel_core;

namespace {

void assertNear(float actual, float expected, float eps = 1.0e-5f) {
    const float tolerance = eps * std::max(1.0f, std::abs(expected));
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << "assertNear failed actual=" << actual
                  << " expected=" << expected
                  << " tolerance=" << tolerance << "\n";
    }
    assert(std::abs(actual - expected) <= tolerance);
}

void assertNear(const std::vector<float>& actual,
                const std::vector<float>& expected,
                float eps = 1.0e-5f) {
    assert(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        assertNear(actual[i], expected[i], eps);
    }
}

float rms(const std::vector<float>& samples) {
    double sum = 0.0;
    for (float sample : samples) {
        sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}

}  // namespace

int main() {
    const float high = std::sqrt(1.75f);
    const std::vector<float> unit_rms_loop{0.5f, -0.5f, high, -high};

    {
        const float snr_db = 10.0f;
        channel::RealHfLoopChannelModel model(snr_db, unit_rms_loop, 3);
        const auto output = model.process(std::vector<float>(4, 0.0f));
        const float scale = output[0] / -high;
        assert(scale > 0.0f);

        assertNear(rms(output), scale);
        assertNear(output, {
            -high * scale,
            0.5f * scale,
            -0.5f * scale,
            high * scale,
        });

        model.setSNR(20.0f);
        const auto rerun = model.process(std::vector<float>(2, 0.0f));
        const float new_scale = rerun[0] / -high;
        assertNear(new_scale / scale, std::pow(10.0f, -10.0f / 20.0f), 1.0e-5f);
        assertNear(rerun, {
            -high * new_scale,
            0.5f * new_scale,
        });
    }

    {
        channel::RealHfLoopChannelModel model(12.0f, unit_rms_loop, 2);
        const auto output = model.process(std::vector<float>(6, 0.0f));
        const float scale = output[0] / high;
        assert(scale > 0.0f);
        assertNear(output, {
            high * scale,
            -high * scale,
            0.5f * scale,
            -0.5f * scale,
            high * scale,
            -high * scale,
        });
    }

    {
        channel::RealHfLoopChannelModel empty(12.0f, std::vector<float>{});
        const std::vector<float> input{0.25f, -0.75f, 0.0f};
        assertNear(empty.process(input), input, 0.0f);
    }

    {
        const auto parsed = channel::parseChannelType("real_hf_loop");
        assert(parsed && *parsed == channel::ChannelType::REAL_HF_LOOP);
        assert(std::string(channel::channelTypeName(channel::ChannelType::REAL_HF_LOOP)) ==
               "real_hf_loop");
    }

    std::cout << "real_hf_loop channel scaling and wrap behavior verified\n";
    return 0;
}
