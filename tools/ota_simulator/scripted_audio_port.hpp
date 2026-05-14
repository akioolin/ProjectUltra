#pragma once

#include "sim/simulated_station.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ultra::tools::ota {

class ScriptedAudioPort : public AudioPort {
public:
    static constexpr uint32_t kSampleRate = 48000;

    void setNoiseBed(std::vector<float> bed, bool loop, double target_rms);
    void scheduleInject(double t_s, std::vector<float> clip, double gain_db);
    void reserveTxSamples(size_t samples);

    std::vector<float> pullRx(size_t count) override;
    void queueTx(const std::vector<float>& samples) override;

    std::vector<float> capturedTx() const;
    std::vector<float> capturedTxSince(size_t& cursor) const;
    uint64_t rxSampleCursor() const { return rx_cursor_; }

private:
    struct Injection {
        uint64_t start_sample = 0;
        std::vector<float> samples;
        float gain = 1.0f;
    };

    std::vector<float> noise_bed_;
    bool noise_loop_ = false;
    bool has_noise_ = false;
    std::vector<Injection> injections_;
    uint64_t rx_cursor_ = 0;

    mutable std::mutex tx_mutex_;
    std::vector<float> tx_capture_;
};

}  // namespace ultra::tools::ota
