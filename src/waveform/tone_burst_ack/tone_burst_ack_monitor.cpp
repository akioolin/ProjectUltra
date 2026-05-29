// tone_burst_ack_monitor.cpp — sliding-window ACK detection driver.

#include "tone_burst_ack_monitor.hpp"

#include "ultra/logging.hpp"

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
    armed_ = false;
    arm_deadline_stream_offset_ = 0;
}

void ToneBurstAckMonitor::arm(size_t window_samples) {
    const uint64_t new_deadline = total_samples_fed_ + window_samples;
    if (armed_ && new_deadline < arm_deadline_stream_offset_) {
        // Calling arm() with a SHORTER window than current — keep the
        // larger one (the caller may already be expecting an ACK from a
        // previous burst with a later deadline).
        return;
    }
    armed_ = true;
    arm_deadline_stream_offset_ = new_deadline;
    LOG_MODEM(DEBUG, "ToneBurstAckMonitor armed: window_samples=%zu fed=%llu deadline=%llu buf_size=%zu",
              window_samples, (unsigned long long)total_samples_fed_,
              (unsigned long long)arm_deadline_stream_offset_, buffer_.size());
    // Reset the cadence counter so the FIRST detection pass happens at the
    // current sample position + armed-cadence interval, not whenever the
    // background cadence next fires. This gives the first detection a
    // bounded latency right after arm().
    last_detect_pos_in_buffer_ =
        (total_samples_fed_ >= buffer_start_stream_offset_)
            ? static_cast<size_t>(total_samples_fed_ - buffer_start_stream_offset_)
            : 0;
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

    // §15 step 4d-late: pick the active cadence based on armed state.
    //   armed:    detect_interval_samples_armed (tight, low latency)
    //   not armed (and armed_only): no cadence — runDetectionPass is gated
    //   not armed (always-on): detect_interval_samples (background polling)
    if (armed_ && total_samples_fed_ >= arm_deadline_stream_offset_) {
        // Window elapsed without a successful decode — disarm. The
        // protocol layer's existing ack_timeout will handle the
        // retransmit; we go idle until armed again.
        armed_ = false;
    }
    if (!armed_ && cfg_.armed_only) {
        // Idle: skip detection entirely. No CPU work on the audio thread.
        return;
    }
    const size_t cadence = armed_ ? cfg_.detect_interval_samples_armed
                                   : cfg_.detect_interval_samples;
    if (cadence == 0) return;

    // Trigger detection passes at the active cadence. The "position" we
    // compare against is the cumulative samples fed (absolute, not
    // buffer-relative, so backlogged audio doesn't trigger a flood).
    while (true) {
        const uint64_t next_trigger_stream =
            buffer_start_stream_offset_ + last_detect_pos_in_buffer_ +
            cadence;
        if (next_trigger_stream > total_samples_fed_) break;
        last_detect_pos_in_buffer_ =
            next_trigger_stream - buffer_start_stream_offset_;
        runDetectionPass();
        // If a successful decode disarmed us inside runDetectionPass(), or
        // the window elapsed, stop the inner loop here.
        if (cfg_.armed_only && !armed_) break;
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

        // §15 step 4d-late: disarm on successful decode. The protocol
        // layer's burst transport advances and will arm() again before
        // the next expected ACK.
        armed_ = false;

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
