#include "ota_simulator/scripted_audio_port.hpp"

#include <algorithm>
#include <cmath>

namespace ultra::tools::ota {
namespace {

float rmsOf(const std::vector<float>& samples) {
    if (samples.empty()) {
        return 0.0f;
    }
    double sum_sq = 0.0;
    for (float s : samples) {
        sum_sq += static_cast<double>(s) * static_cast<double>(s);
    }
    return static_cast<float>(std::sqrt(sum_sq / static_cast<double>(samples.size())));
}

float gainFromDb(double gain_db) {
    return static_cast<float>(std::pow(10.0, gain_db / 20.0));
}

}  // namespace

void ScriptedAudioPort::setNoiseBed(std::vector<float> bed, bool loop, double target_rms) {
    if (!bed.empty() && target_rms > 0.0) {
        const float current = rmsOf(bed);
        if (current > 0.0f) {
            const float gain = static_cast<float>(target_rms) / current;
            for (float& sample : bed) {
                sample *= gain;
            }
        }
    }
    noise_bed_ = std::move(bed);
    noise_loop_ = loop;
    has_noise_ = !noise_bed_.empty();
}

void ScriptedAudioPort::scheduleInject(double t_s, std::vector<float> clip, double gain_db) {
    Injection injection;
    injection.start_sample = static_cast<uint64_t>(
        std::llround(std::max(0.0, t_s) * static_cast<double>(kSampleRate)));
    injection.gain = gainFromDb(gain_db);
    injection.samples = std::move(clip);
    injections_.push_back(std::move(injection));
    std::stable_sort(injections_.begin(), injections_.end(),
                     [](const Injection& a, const Injection& b) {
                         return a.start_sample < b.start_sample;
                     });
}

void ScriptedAudioPort::reserveTxSamples(size_t samples) {
    std::lock_guard<std::mutex> lock(tx_mutex_);
    tx_capture_.reserve(samples);
}

std::vector<float> ScriptedAudioPort::pullRx(size_t count) {
    std::vector<float> out(count, 0.0f);
    const uint64_t chunk_start = rx_cursor_;
    const uint64_t chunk_end = chunk_start + count;

    if (has_noise_) {
        for (size_t i = 0; i < count; ++i) {
            const uint64_t sample = chunk_start + i;
            if (sample < noise_bed_.size()) {
                out[i] = noise_bed_[static_cast<size_t>(sample)];
            } else if (noise_loop_) {
                out[i] = noise_bed_[static_cast<size_t>(sample % noise_bed_.size())];
            }
        }
    }

    for (const auto& injection : injections_) {
        const uint64_t inj_start = injection.start_sample;
        const uint64_t inj_end = inj_start + injection.samples.size();
        if (inj_end <= chunk_start) {
            continue;
        }
        if (inj_start >= chunk_end) {
            break;
        }

        const uint64_t mix_start = std::max(chunk_start, inj_start);
        const uint64_t mix_end = std::min(chunk_end, inj_end);
        const size_t out_offset = static_cast<size_t>(mix_start - chunk_start);
        const size_t clip_offset = static_cast<size_t>(mix_start - inj_start);
        const size_t mix_count = static_cast<size_t>(mix_end - mix_start);
        for (size_t i = 0; i < mix_count; ++i) {
            out[out_offset + i] += injection.samples[clip_offset + i] * injection.gain;
        }
    }

    rx_cursor_ += count;
    return shapeRxForLocalRadio(std::move(out), count);
}

void ScriptedAudioPort::queueTx(const std::vector<float>& samples) {
    std::lock_guard<std::mutex> lock(tx_mutex_);
    tx_capture_.insert(tx_capture_.end(), samples.begin(), samples.end());
}

std::vector<float> ScriptedAudioPort::capturedTx() const {
    std::lock_guard<std::mutex> lock(tx_mutex_);
    return tx_capture_;
}

std::vector<float> ScriptedAudioPort::capturedTxSince(size_t& cursor) const {
    std::lock_guard<std::mutex> lock(tx_mutex_);
    if (cursor >= tx_capture_.size()) {
        cursor = tx_capture_.size();
        return {};
    }
    std::vector<float> out(tx_capture_.begin() + static_cast<std::ptrdiff_t>(cursor),
                           tx_capture_.end());
    cursor = tx_capture_.size();
    return out;
}

}  // namespace ultra::tools::ota
