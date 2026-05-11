#include "diagnostics/audio_ring.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

namespace ultra::diagnostics {

namespace {

void writeLe16(std::ostream& out, uint16_t v) {
    char b[2] = {
        static_cast<char>(v & 0xffu),
        static_cast<char>((v >> 8) & 0xffu),
    };
    out.write(b, sizeof(b));
}

void writeLe32(std::ostream& out, uint32_t v) {
    char b[4] = {
        static_cast<char>(v & 0xffu),
        static_cast<char>((v >> 8) & 0xffu),
        static_cast<char>((v >> 16) & 0xffu),
        static_cast<char>((v >> 24) & 0xffu),
    };
    out.write(b, sizeof(b));
}

} // namespace

Pcm16Ring::Pcm16Ring(size_t capacity_samples, int sample_rate) {
    reset(capacity_samples, sample_rate);
}

void Pcm16Ring::reset(size_t capacity_samples, int sample_rate) {
    capacity_samples_ = capacity_samples;
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    data_.reset();
    if (capacity_samples_ > 0) {
        data_ = std::make_unique<std::atomic<int16_t>[]>(capacity_samples_);
        for (size_t i = 0; i < capacity_samples_; ++i) {
            data_[i].store(0, std::memory_order_relaxed);
        }
    }
    write_seq_.store(0, std::memory_order_release);
    dropped_samples_.store(0, std::memory_order_release);
}

void Pcm16Ring::clear() noexcept {
    write_seq_.store(0, std::memory_order_release);
    dropped_samples_.store(0, std::memory_order_release);
}

int16_t Pcm16Ring::floatToPcm16(float sample) noexcept {
    if (!std::isfinite(sample)) {
        return 0;
    }
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    const float scaled = clamped >= 0.0f ? clamped * 32767.0f : clamped * 32768.0f;
    return static_cast<int16_t>(std::lrintf(scaled));
}

void Pcm16Ring::push(SampleSpan samples) noexcept {
    if (!data_ || capacity_samples_ == 0 || samples.empty()) {
        return;
    }

    uint64_t pos = write_seq_.load(std::memory_order_relaxed);
    for (float sample : samples) {
        data_[pos % capacity_samples_].store(floatToPcm16(sample), std::memory_order_relaxed);
        ++pos;
    }
    const uint64_t previous = write_seq_.exchange(pos, std::memory_order_release);
    const uint64_t previous_tail = previous > capacity_samples_ ? previous - capacity_samples_ : 0;
    const uint64_t new_tail = pos > capacity_samples_ ? pos - capacity_samples_ : 0;
    if (new_tail > previous_tail) {
        dropped_samples_.fetch_add(new_tail - previous_tail, std::memory_order_relaxed);
    }
}

AudioRingSnapshot Pcm16Ring::snapshot() const {
    AudioRingSnapshot out;
    out.sample_rate = sample_rate_;
    out.samples_written = write_seq_.load(std::memory_order_acquire);
    out.samples_dropped = dropped_samples_.load(std::memory_order_acquire);

    if (!data_ || capacity_samples_ == 0 || out.samples_written == 0) {
        return out;
    }

    const uint64_t available_u64 =
        std::min<uint64_t>(out.samples_written, static_cast<uint64_t>(capacity_samples_));
    const size_t available = static_cast<size_t>(available_u64);
    out.samples.resize(available);
    const uint64_t start = out.samples_written - available_u64;
    for (size_t i = 0; i < available; ++i) {
        out.samples[i] = data_[(start + i) % capacity_samples_].load(std::memory_order_relaxed);
    }
    return out;
}

size_t AudioRing::boundedCapacitySamples(int sample_rate, int seconds) {
    const int sr = sample_rate > 0 ? sample_rate : kDefaultSampleRate;
    const int sec = seconds > 0 ? seconds : kDefaultWindowSeconds;
    const size_t requested = static_cast<size_t>(sr) * static_cast<size_t>(sec);
    const size_t max_samples = kHardCapBytes / sizeof(int16_t) / 2u;
    return std::min(requested, max_samples);
}

void AudioRing::reset(int sample_rate, int rx_window_seconds, int tx_window_seconds) {
    rx_.reset(boundedCapacitySamples(sample_rate, rx_window_seconds), sample_rate);
    tx_.reset(boundedCapacitySamples(sample_rate, tx_window_seconds), sample_rate);
}

bool writeWavPcm16(const std::filesystem::path& path, const AudioRingSnapshot& audio) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    const uint64_t data_bytes_u64 = static_cast<uint64_t>(audio.samples.size()) * sizeof(int16_t);
    const uint32_t data_bytes = data_bytes_u64 > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(data_bytes_u64);
    const uint32_t riff_bytes = data_bytes > std::numeric_limits<uint32_t>::max() - 36u
        ? std::numeric_limits<uint32_t>::max()
        : data_bytes + 36u;
    const uint32_t byte_rate = static_cast<uint32_t>(audio.sample_rate * sizeof(int16_t));

    out.write("RIFF", 4);
    writeLe32(out, riff_bytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeLe32(out, 16);
    writeLe16(out, 1);
    writeLe16(out, 1);
    writeLe32(out, static_cast<uint32_t>(audio.sample_rate));
    writeLe32(out, byte_rate);
    writeLe16(out, sizeof(int16_t));
    writeLe16(out, 16);
    out.write("data", 4);
    writeLe32(out, data_bytes);
    if (!audio.samples.empty()) {
        out.write(reinterpret_cast<const char*>(audio.samples.data()),
                  static_cast<std::streamsize>(audio.samples.size() * sizeof(int16_t)));
    }
    return out.good();
}

} // namespace ultra::diagnostics
