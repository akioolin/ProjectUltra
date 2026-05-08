// StreamingDecoder module

#include "streaming_decoder.hpp"
#include "streaming_buffer_policy.hpp"
#include "streaming_decode_policy.hpp"
#include "streaming_frame_policy.hpp"
#include "streaming_signal_policy.hpp"
#include "gui/startup_trace.hpp"
#include "waveform/ofdm_cox_waveform.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
#include "fec/frame_interleaver.hpp"
#include "fec/burst_interleaver.hpp"
#include "ultra/fec.hpp"
#include "fec/ldpc_codec.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/timing_profiler.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <stdexcept>

namespace ultra {
namespace gui {

namespace v2 = protocol::v2;
namespace buffer_policy = streaming_buffer_policy;
namespace decode_policy = streaming_decode_policy;
namespace frame_policy = streaming_frame_policy;
namespace signal_policy = streaming_signal_policy;

// ============================================================================
// BURST INTERLEAVE ACCUMULATION
// ============================================================================

void StreamingDecoder::accumulateBurstFrames() {
    const int burst_group_size = std::max(2, burst_group_size_);
    const int burst_timeout_ms =
        static_cast<int>(BURST_TIMEOUT_MS_BASE * (burst_group_size / 4.0f));

    // Timeout check
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - burst_start_time_).count();
    if (elapsed > burst_timeout_ms) {
        LOG_MODEM(WARN, "[%s] Burst group timeout: got %zu/%d frames",
                  log_prefix_.c_str(), burst_soft_buffer_.size(), burst_group_size);
        // Discard — TX used 4-frame interleaving, partial is undecodable
        {
            std::lock_guard<std::mutex> slock(stats_mutex_);
            stats_.frames_failed += burst_group_size;
        }
        burst_soft_buffer_.clear();
        burst_metric_templates_.clear();
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            correlation_pos_ = burst_next_pos_;
        }
        state_ = DecoderState::SEARCHING;
        return;
    }

    // Try to demodulate next frame
    auto result = tryDemodulateNextBurstFrame();

    if (result == BurstFrameResult::FAILED) {
        // Hard failure (energy lost or process error) — abort immediately
        LOG_MODEM(WARN, "[%s] Burst group aborted: hard failure at frame %zu/%d",
                  log_prefix_.c_str(), burst_soft_buffer_.size() + 1, burst_group_size);
        {
            std::lock_guard<std::mutex> slock(stats_mutex_);
            stats_.frames_failed += burst_group_size;
        }
        burst_soft_buffer_.clear();
        burst_metric_templates_.clear();
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            correlation_pos_ = burst_next_pos_;
        }
        state_ = DecoderState::SEARCHING;
        return;
    }

    if (result == BurstFrameResult::WAITING) {
        return;  // Not enough samples yet — come back on next processBuffer() tick
    }

    // SUCCESS — check if group complete
    if (static_cast<int>(burst_soft_buffer_.size()) == burst_group_size) {
        finalizeBurstGroup();
        burst_soft_buffer_.clear();
        burst_metric_templates_.clear();
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            correlation_pos_ = burst_next_pos_;
        }
        state_ = DecoderState::SEARCHING;
    }
    // else: still accumulating, return and wait for next processBuffer() call
}

StreamingDecoder::BurstFrameResult StreamingDecoder::tryDemodulateNextBurstFrame() {
    const int burst_group_size = std::max(2, burst_group_size_);
    auto pushMetricTemplate = [&](float residual_cfo_hz) {
        DecodeResult metrics;
        populateDecodeMetrics(metrics, protocol::isOFDMMode(mode_), residual_cfo_hz);
        burst_metric_templates_.push_back(metrics);
    };

    // Check available samples at burst_next_pos_
    size_t next_available;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (write_pos_ >= burst_next_pos_) {
            next_available = write_pos_ - burst_next_pos_;
        } else {
            next_available = buffer_capacity_samples_ - burst_next_pos_ + write_pos_;
        }
    }

    if (next_available < burst_min_block_) {
        return BurstFrameResult::WAITING;
    }

    // Copy samples from circular buffer
    std::vector<float> block(burst_min_block_);
    size_t block_start_pos = burst_next_pos_;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        for (size_t i = 0; i < burst_min_block_; i++) {
            block[i] = buffer_[wrapRingIndexLocked(burst_next_pos_ + i)];
        }
    }

    // Energy check. In a marked burst-interleaved group, a weak/missing
    // physical frame is exactly what the burst interleaver is meant to absorb:
    // represent it as zero-confidence LLR erasures and keep collecting the
    // rest of the group. Aborting here converts one faded physical block into
    // four lost ARQ frames.
    float next_rms = 0.0f;
    size_t check_start = std::min(size_t(1024), burst_min_block_);
    size_t check_len = std::min(burst_min_block_ - check_start, size_t(5000));
    if (check_len > 0) {
        for (size_t i = 0; i < check_len; i++) {
            next_rms += block[check_start + i] * block[check_start + i];
        }
        next_rms = std::sqrt(next_rms / check_len);
    }
    constexpr float BURST_ERASURE_RMS_THRESHOLD = 0.015f;
    if (next_rms < BURST_ERASURE_RMS_THRESHOLD) {
        LOG_MODEM(WARN, "[%s] Burst frame %zu/%d: energy lost (RMS=%.4f), inserting erasure",
                  log_prefix_.c_str(), burst_soft_buffer_.size() + 1,
                  burst_group_size, next_rms);
        burst_soft_buffer_.emplace_back(
            static_cast<size_t>(fec::BurstInterleaver::bitsPerFrame(fixed_frame_codewords_)),
            0.0f);
        pushMetricTemplate(0.0f);
        burst_next_pos_ = wrapRingIndexLocked(burst_next_pos_ + burst_min_block_);
        return BurstFrameResult::SUCCESS;
    }

    // Pre-correct CFO on burst block
    bool is_ofdm_burst = protocol::isOFDMMode(mode_);
    float burst_pre_cfo = 0.0f;
    size_t abs_burst = burst_next_pos_;
    if (is_ofdm_burst) {
        if (total_fed_ >= buffer_capacity_samples_) {
            const size_t oldest_abs = total_fed_ - buffer_capacity_samples_;
            const size_t oldest_pos = write_pos_;
            const size_t offset = (burst_next_pos_ >= oldest_pos)
                ? (burst_next_pos_ - oldest_pos)
                : (buffer_capacity_samples_ - oldest_pos + burst_next_pos_);
            abs_burst = oldest_abs + offset;
        }
        burst_pre_cfo = applyCFOPreCorrection(block, burst_cfo_, abs_burst);
    }

    // Demodulate (CFO=0 after pre-correction, or original burst_cfo_ if no pre-correction)
    float burst_decode_cfo = (std::abs(burst_pre_cfo) > 0.01f) ? 0.0f : burst_cfo_;
    waveform_->setFrequencyOffset(burst_decode_cfo);
    bool ok = processWaveformForCodewords(
        SampleSpan(block.data(), block.size()), fixed_frame_codewords_);
    if (!ok) {
        LOG_MODEM(WARN, "[%s] Burst frame %zu/%d: process() failed, inserting erasure",
                  log_prefix_.c_str(), burst_soft_buffer_.size() + 1, burst_group_size);
        burst_soft_buffer_.emplace_back(
            static_cast<size_t>(fec::BurstInterleaver::bitsPerFrame(fixed_frame_codewords_)),
            0.0f);
        pushMetricTemplate(0.0f);
        burst_next_pos_ = wrapRingIndexLocked(burst_next_pos_ + burst_min_block_);
        return BurstFrameResult::SUCCESS;
    }
    captureConstellationSnapshot();

    const float timing_offset = waveform_->getLastTimingOffsetSamples();
    constexpr float kBurstContinuationRetryThreshold = 48.0f;
    constexpr float kBurstContinuationRetryMax = 320.0f;
    if (std::abs(timing_offset) >= kBurstContinuationRetryThreshold &&
        std::abs(timing_offset) <= kBurstContinuationRetryMax) {
        const int sample_correction = static_cast<int>(std::lround(timing_offset));
        const size_t corrected_pos = wrapRingIndexLocked(
            block_start_pos + buffer_capacity_samples_ + sample_correction);
        const size_t corrected_abs =
            (sample_correction >= 0)
                ? abs_burst + static_cast<size_t>(sample_correction)
                : (abs_burst > static_cast<size_t>(-sample_correction)
                       ? abs_burst - static_cast<size_t>(-sample_correction)
                       : 0);

        bool have_corrected_block = false;
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            size_t corrected_available;
            if (write_pos_ >= corrected_pos) {
                corrected_available = write_pos_ - corrected_pos;
            } else {
                corrected_available = buffer_capacity_samples_ - corrected_pos + write_pos_;
            }
            have_corrected_block = corrected_available >= burst_min_block_;
            if (have_corrected_block) {
                block.assign(burst_min_block_, 0.0f);
                for (size_t i = 0; i < burst_min_block_; i++) {
                    block[i] = buffer_[wrapRingIndexLocked(corrected_pos + i)];
                }
            }
        }

        if (have_corrected_block) {
            burst_pre_cfo = is_ofdm_burst ? applyCFOPreCorrection(block, burst_cfo_, corrected_abs) : 0.0f;
            burst_decode_cfo = (std::abs(burst_pre_cfo) > 0.01f) ? 0.0f : burst_cfo_;
            waveform_->setAbsoluteTrainingPosition(corrected_abs);
            waveform_->setFrequencyOffset(burst_decode_cfo);
            bool retry_ok = processWaveformForCodewords(
                SampleSpan(block.data(), block.size()), fixed_frame_codewords_);
            if (retry_ok) {
                block_start_pos = corrected_pos;
                abs_burst = corrected_abs;
                LOG_MODEM(WARN, "[%s] Burst continuation timing retry frame %zu/%d: %.1f samples",
                          log_prefix_.c_str(), burst_soft_buffer_.size() + 1,
                          burst_group_size, timing_offset);
                captureConstellationSnapshot();
            } else {
                LOG_MODEM(WARN, "[%s] Burst continuation timing retry failed frame %zu/%d: %.1f samples",
                          log_prefix_.c_str(), burst_soft_buffer_.size() + 1,
                          burst_group_size, timing_offset);
            }
        }
    }

    auto soft = waveform_->getSoftBits();
    if (soft.empty()) {
        LOG_MODEM(WARN, "[%s] Burst frame %zu/%d: empty soft bits, inserting erasure",
                  log_prefix_.c_str(), burst_soft_buffer_.size() + 1, burst_group_size);
        burst_soft_buffer_.emplace_back(
            static_cast<size_t>(fec::BurstInterleaver::bitsPerFrame(fixed_frame_codewords_)),
            0.0f);
        pushMetricTemplate(waveform_ ? waveform_->estimatedCFO() : 0.0f);
        burst_next_pos_ = wrapRingIndexLocked(burst_next_pos_ + burst_min_block_);
        return BurstFrameResult::SUCCESS;
    }

    // Update CFO from pilot tracking (add pre-correction amount back)
    const float residual_cfo = waveform_->estimatedCFO();
    const auto cfo_update = signal_policy::combinePilotCFO(
        burst_pre_cfo, residual_cfo, burst_cfo_, /*clamp_drift=*/true);
    burst_cfo_ = cfo_update.accepted_cfo;
    last_cfo_.store(cfo_update.accepted_cfo);
    last_fading_index_.store(waveform_->getFadingIndex());

    burst_soft_buffer_.push_back(std::move(soft));
    pushMetricTemplate(residual_cfo);
    burst_next_pos_ = wrapRingIndexLocked(block_start_pos + burst_min_block_);

    LOG_MODEM(INFO, "[%s] Burst frame %zu/%d demodulated, RMS=%.4f",
              log_prefix_.c_str(), burst_soft_buffer_.size(),
              burst_group_size, next_rms);
    return BurstFrameResult::SUCCESS;
}

void StreamingDecoder::finalizeBurstGroup() {
    const int burst_group_size = std::max(2, burst_group_size_);
    LOG_MODEM(INFO, "[%s] Burst group complete (%d frames), deinterleaving...",
              log_prefix_.c_str(), burst_group_size);

    auto logical_soft = fec::BurstInterleaver::deinterleave(
        burst_soft_buffer_, fixed_frame_codewords_);

    for (int i = 0; i < burst_group_size; i++) {
        const int saved_pending_total_cw = pending_total_cw_;
        pending_total_cw_ = fixed_frame_codewords_;
        DecodeResult result = decodeFrame(logical_soft[i], burst_snr_, burst_cfo_);
        pending_total_cw_ = saved_pending_total_cw;
        if (i < static_cast<int>(burst_metric_templates_.size())) {
            const auto& metrics = burst_metric_templates_[static_cast<size_t>(i)];
            result.lts_snr_db = metrics.lts_snr_db;
            result.lts_fading_index = metrics.lts_fading_index;
            result.sync_correlation = metrics.sync_correlation;
            result.lts_residual_cfo_hz = metrics.lts_residual_cfo_hz;
        } else {
            populateDecodeMetrics(result, protocol::isOFDMMode(mode_), burst_cfo_);
        }

        {
            std::lock_guard<std::mutex> slock(stats_mutex_);
            if (result.success) stats_.frames_decoded++;
            else stats_.frames_failed++;
        }

        if (result.success || result.codewords_ok > 0) {
            {
                std::lock_guard<std::mutex> qlock(queue_mutex_);
                frame_queue_.push(result);
            }
            if (result.success && frame_callback_) frame_callback_(result);
        }

        LOG_MODEM(INFO, "[%s] Burst logical frame %d/%d: %s (%d/%d CWs)",
                  log_prefix_.c_str(), i + 1, burst_group_size,
                  result.success ? "OK" : "FAIL",
                  result.codewords_ok, result.codewords_ok + result.codewords_failed);
    }
}


} // namespace gui
} // namespace ultra
