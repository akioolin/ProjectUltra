// tone_burst_ack_monitor.cpp — sliding-window ACK detection driver.

#include "tone_burst_ack_monitor.hpp"

#include <algorithm>
#include <cstring>

namespace ultra {
namespace waveform {
namespace tone_burst_ack {

ToneBurstAckMonitor::ToneBurstAckMonitor() : ToneBurstAckMonitor(Config{}) {}

ToneBurstAckMonitor::ToneBurstAckMonitor(Config cfg) : cfg_(std::move(cfg)) {
    buffer_.reserve(cfg_.buffer_capacity_samples);
}

float ToneBurstAckMonitor::idealCostasPeak(uint32_t symbol_ms) {
    const uint32_t spp = (kSampleRate * symbol_ms) / 1000u;
    // Goertzel magnitude for matched tone of amplitude A over N samples: N·A/2.
    const float per_symbol = (static_cast<float>(spp) / 2.0f) * kToneAmplitude;
    return per_symbol * static_cast<float>(kCostasSymbols);
}

void ToneBurstAckMonitor::reset() {
    buffer_.clear();
    buffer_start_stream_offset_ = total_samples_fed_;
    last_detect_pos_in_buffer_ = 0;
    last_detection_stream_offset_ = static_cast<uint64_t>(-1);
}

void ToneBurstAckMonitor::feedAudio(const float* samples, size_t count) {
    if (count == 0 || samples == nullptr) return;

    // Append to buffer.
    buffer_.insert(buffer_.end(), samples, samples + count);
    total_samples_fed_ += count;

    // If we exceeded capacity, drop the oldest samples. Maintain the
    // invariant that buffer_start_stream_offset_ tracks buffer_[0]'s
    // position in the overall feed stream.
    if (buffer_.size() > cfg_.buffer_capacity_samples) {
        const size_t excess = buffer_.size() - cfg_.buffer_capacity_samples;
        buffer_.erase(buffer_.begin(), buffer_.begin() + excess);
        buffer_start_stream_offset_ += excess;
        // last_detect_pos_in_buffer_ is relative to buffer_[0]; shift it.
        if (last_detect_pos_in_buffer_ > excess) {
            last_detect_pos_in_buffer_ -= excess;
        } else {
            last_detect_pos_in_buffer_ = 0;
        }
    }

    // Trigger detection passes at the configured cadence. The "position"
    // we compare against is the cumulative samples fed (cadence is
    // absolute, not buffer-relative, so backlogged audio doesn't trigger
    // a flood of passes).
    while (true) {
        const uint64_t next_trigger_stream =
            buffer_start_stream_offset_ + last_detect_pos_in_buffer_ +
            cfg_.detect_interval_samples;
        if (next_trigger_stream > total_samples_fed_) break;
        last_detect_pos_in_buffer_ =
            next_trigger_stream - buffer_start_stream_offset_;
        runDetectionPass();
    }
}

void ToneBurstAckMonitor::runDetectionPass() {
    // For each candidate symbol duration, try to detect+decode in the
    // current buffer. Take the FIRST CRC-passing decode (the §15.5
    // staircase implicitly suggests trying short first, then longer if
    // not found — but the dominant channel duration is set by negotiated
    // SNR, so a single duration usually wins).
    for (uint32_t symbol_ms : cfg_.symbol_durations_ms) {
        const uint32_t spp = (kSampleRate * symbol_ms) / 1000u;
        const size_t needed = static_cast<size_t>(kTotalSymbols) * spp;
        if (buffer_.size() < needed) continue;
        const auto r = detector_.detectAndDecode(buffer_.data(), buffer_.size(),
                                                  symbol_ms, cfg_.sweep_step_samples);
        if (!r.decode.payload.has_value()) continue;
        if (!r.decode.stats.crc_ok) continue;
        // Peak-magnitude gate: rejects rare cases where Costas+Hamming+CRC
        // all happen to pass on a spurious low-energy match (e.g. trying
        // 12 ms duration on a 25 ms burst that aliases into a noise-floor
        // peak). Real bursts have peaks at ~30%+ of ideal even at SNR=-10.
        const float min_peak =
            idealCostasPeak(symbol_ms) * cfg_.min_peak_fraction;
        if (r.costas_correlation_peak < min_peak) continue;

        const uint64_t detected_stream_offset =
            buffer_start_stream_offset_ +
            static_cast<uint64_t>(r.detected_offset_samples);

        // Duplicate-detection suppression: drop if the previous detection
        // landed within suppress_window_samples of this one.
        if (last_detection_stream_offset_ != static_cast<uint64_t>(-1)) {
            const uint64_t diff = (detected_stream_offset > last_detection_stream_offset_)
                ? detected_stream_offset - last_detection_stream_offset_
                : last_detection_stream_offset_ - detected_stream_offset;
            if (diff < cfg_.suppress_window_samples) {
                ++suppressed_attempts_;
                return;
            }
        }

        last_detection_stream_offset_ = detected_stream_offset;
        ++detections_emitted_;

        if (callback_) {
            ToneBurstAckDetection d;
            d.payload = *r.decode.payload;
            d.detected_stream_offset = detected_stream_offset;
            d.correlation_peak = r.costas_correlation_peak;
            d.hamming_corrected_blocks = r.decode.stats.hamming_corrected_blocks;
            d.symbol_ms_used = symbol_ms;
            callback_(d);
        }

        // Drop everything in the buffer up to past the decoded burst so
        // we don't re-scan the same audio on the next cadence tick. Leave
        // ~50 ms of trailing samples as guard.
        const size_t consume_until = r.detected_offset_samples + needed;
        const size_t guard = static_cast<size_t>(kSampleRate) * 50u / 1000u;
        const size_t drop = (consume_until > guard) ? (consume_until - guard) : 0;
        if (drop > 0 && drop <= buffer_.size()) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + drop);
            buffer_start_stream_offset_ += drop;
            last_detect_pos_in_buffer_ =
                (last_detect_pos_in_buffer_ > drop) ? last_detect_pos_in_buffer_ - drop : 0;
        }
        return;
    }
}

}  // namespace tone_burst_ack
}  // namespace waveform
}  // namespace ultra
