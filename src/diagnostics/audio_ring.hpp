#pragma once

#include "ultra/types.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace ultra::diagnostics {

struct AudioRingSnapshot {
    int sample_rate = 48000;
    std::vector<int16_t> samples;
    uint64_t samples_written = 0;
    uint64_t samples_dropped = 0;
};

class Pcm16Ring {
public:
    Pcm16Ring() = default;
    explicit Pcm16Ring(size_t capacity_samples, int sample_rate = 48000);

    Pcm16Ring(const Pcm16Ring&) = delete;
    Pcm16Ring& operator=(const Pcm16Ring&) = delete;

    void reset(size_t capacity_samples, int sample_rate = 48000);
    void push(SampleSpan samples) noexcept;
    void clear() noexcept;

    AudioRingSnapshot snapshot() const;

    size_t capacitySamples() const noexcept { return capacity_samples_; }
    int sampleRate() const noexcept { return sample_rate_; }
    uint64_t samplesWritten() const noexcept {
        return write_seq_.load(std::memory_order_acquire);
    }
    uint64_t samplesDropped() const noexcept {
        return dropped_samples_.load(std::memory_order_acquire);
    }

private:
    static int16_t floatToPcm16(float sample) noexcept;

    std::unique_ptr<std::atomic<int16_t>[]> data_;
    size_t capacity_samples_ = 0;
    int sample_rate_ = 48000;
    std::atomic<uint64_t> write_seq_{0};
    std::atomic<uint64_t> dropped_samples_{0};
};

class AudioRing {
public:
    static constexpr int kDefaultSampleRate = 48000;
    // Pre-alpha: bias toward capturing full multi-minute QSOs (handshake
    // retries + 20 KB file transfer + cooldown) so offline replay/spectrogram
    // analysis has the whole story, not just the last 2 min. Drop back to a
    // smaller window before operator-facing release.
    static constexpr int kDefaultWindowSeconds = 600;          // 10 min/ring
    static constexpr size_t kHardCapBytes = 256u * 1024u * 1024u;  // 256 MB cap → up to ~22 min/ring

    AudioRing() = default;

    void reset(int sample_rate = kDefaultSampleRate,
               int rx_window_seconds = kDefaultWindowSeconds,
               int tx_window_seconds = kDefaultWindowSeconds);

    void pushRx(SampleSpan samples) noexcept { rx_.push(samples); }
    void pushTx(SampleSpan samples) noexcept { tx_.push(samples); }

    AudioRingSnapshot snapshotRx() const { return rx_.snapshot(); }
    AudioRingSnapshot snapshotTx() const { return tx_.snapshot(); }

    uint64_t rxDroppedSamples() const noexcept { return rx_.samplesDropped(); }
    uint64_t txDroppedSamples() const noexcept { return tx_.samplesDropped(); }

private:
    static size_t boundedCapacitySamples(int sample_rate, int seconds);

    Pcm16Ring rx_;
    Pcm16Ring tx_;
};

bool writeWavPcm16(const std::filesystem::path& path, const AudioRingSnapshot& audio);

} // namespace ultra::diagnostics
