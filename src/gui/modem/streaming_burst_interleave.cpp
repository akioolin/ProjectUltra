// StreamingDecoder module

#include "streaming_decoder.hpp"
#include "streaming_buffer_policy.hpp"
#include "streaming_decode_policy.hpp"
#include "streaming_frame_policy.hpp"
#include "protocol/connection_policy.hpp"  // software-ALC thresholds (RxLevelVerdict)
#include "sync/signal_policy.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
#include "fec/frame_interleaver.hpp"
#include "fec/burst_interleaver.hpp"
#include "ultra/fec.hpp"
#include "fec/ldpc_codec.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/phy_diagnostics.hpp"
#include "ultra/timing_profiler.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace ultra {
namespace gui {

namespace v2 = protocol::v2;
namespace signal_policy = ::ultra::sync::signal_policy;
namespace arrival_policy = ::ultra::sync::frame_arrival_policy;

namespace {

const char* boolDigit(bool value) {
    return value ? "1" : "0";
}

std::string burstFrameTypeAndSeq(const DecodeResult& result) {
    if (!result.success || result.frame_data.empty()) {
        return "frame_type=- seq=-";
    }

    auto header = v2::parseHeader(result.frame_data);
    if (!header.valid) {
        return "frame_type=invalid seq=-";
    }

    std::ostringstream oss;
    oss << "frame_type=" << v2::frameTypeToString(header.type)
        << " seq=" << header.seq;
    return oss.str();
}

}  // namespace

// ============================================================================
// BURST INTERLEAVE ACCUMULATION
// ============================================================================

void StreamingDecoder::clearBurstDiagnostics() {
    burst_physical_diag_.clear();
    burst_diag_group_start_abs_ = 0;
}

void StreamingDecoder::beginBurstDiagnosticsGroup(size_t abs_start_sample,
                                                  const std::vector<float>& soft_bits,
                                                  float rms,
                                                  float pre_cfo_hz,
                                                  float residual_cfo_hz,
                                                  float accepted_cfo_hz) {
    if (!ultra::phyDiagnosticsEnabled()) {
        return;
    }

    burst_diag_group_index_ = burst_diag_next_group_index_++;
    burst_diag_group_start_abs_ = abs_start_sample;
    burst_physical_diag_.clear();

    std::ostringstream oss;
    oss << "event=burst_group_begin"
        << " station=" << log_prefix_
        << " group=" << burst_diag_group_index_
        << " group_size=" << std::max(2, burst_group_size_)
        << " cw=" << fixed_frame_codewords_
        << " start_abs=" << abs_start_sample
        << " start_sec=" << (static_cast<double>(abs_start_sample) / 48000.0)
        << " seed_cfo_hz=" << burst_cfo_;
    ultra::phyDiagLine(oss.str());

    appendBurstPhysicalDiagnostics(abs_start_sample, soft_bits, rms,
                                   pre_cfo_hz, residual_cfo_hz,
                                   accepted_cfo_hz,
                                   /*erasure=*/false, /*process_ok=*/true);
}

void StreamingDecoder::appendBurstPhysicalDiagnostics(size_t abs_start_sample,
                                                      const std::vector<float>& soft_bits,
                                                      float rms,
                                                      float pre_cfo_hz,
                                                      float residual_cfo_hz,
                                                      float accepted_cfo_hz,
                                                      bool erasure,
                                                      bool process_ok) {
    if (!ultra::phyDiagnosticsEnabled()) {
        return;
    }

    BurstPhysicalDiag diag;
    diag.abs_start_sample = abs_start_sample;
    diag.rms = rms;
    diag.pre_cfo_hz = pre_cfo_hz;
    diag.residual_cfo_hz = residual_cfo_hz;
    diag.accepted_cfo_hz = accepted_cfo_hz;
    diag.erasure = erasure;
    diag.process_ok = process_ok;

    const auto llr_quality = signal_policy::evaluatePreSyncLLR(
        soft_bits.empty() ? nullptr : soft_bits.data(),
        soft_bits.size(), soft_bits.size());
    diag.mean_abs_llr = llr_quality.mean_abs;
    diag.near_zero_fraction = llr_quality.near_zero_fraction;

    if (waveform_) {
        diag.fading_index = waveform_->getFadingIndex();
        diag.lts_signal_power = waveform_->getLastLTSSignalPower();
        diag.lts_channel_magnitude = waveform_->getLastLTSChannelMagnitude();
        diag.timing_offset_samples = waveform_->getLastTimingOffsetSamples();
    }

    const size_t physical_index = burst_physical_diag_.size();
    burst_physical_diag_.push_back(diag);

    std::ostringstream oss;
    oss << "event=burst_phys"
        << " station=" << log_prefix_
        << " group=" << burst_diag_group_index_
        << " phys=" << physical_index
        << " abs=" << diag.abs_start_sample
        << " sec=" << (static_cast<double>(diag.abs_start_sample) / 48000.0)
        << " rms=" << diag.rms
        << " llr_mean_abs=" << diag.mean_abs_llr
        << " llr_near_zero=" << diag.near_zero_fraction
        << " erasure=" << boolDigit(diag.erasure)
        << " process_ok=" << boolDigit(diag.process_ok)
        << " pre_cfo_hz=" << diag.pre_cfo_hz
        << " residual_cfo_hz=" << diag.residual_cfo_hz
        << " accepted_cfo_hz=" << diag.accepted_cfo_hz
        << " fading=" << diag.fading_index
        << " lts_signal=" << diag.lts_signal_power
        << " lts_mag=" << diag.lts_channel_magnitude
        << " timing_offset=" << diag.timing_offset_samples;
    ultra::phyDiagLine(oss.str());
}

void StreamingDecoder::logBurstDiagnosticsAbort(const char* reason,
                                                size_t collected_frames) {
    if (!ultra::phyDiagnosticsEnabled()) {
        return;
    }
    std::ostringstream oss;
    oss << "event=burst_group_abort"
        << " station=" << log_prefix_
        << " group=" << burst_diag_group_index_
        << " reason=" << (reason ? reason : "-")
        << " collected=" << collected_frames
        << " group_size=" << std::max(2, burst_group_size_)
        << " start_abs=" << burst_diag_group_start_abs_
        << " start_sec=" << (static_cast<double>(burst_diag_group_start_abs_) / 48000.0);
    ultra::phyDiagLine(oss.str());
}

void StreamingDecoder::accumulateBurstFrames() {
    const int burst_group_size = std::max(2, burst_group_size_);
    // Group timeout — AIRTIME-DERIVED (2026-07-02, replaces the fixed
    // BURST_TIMEOUT_MS_BASE x group/4 wall-clock constant, an adaptivity-rule
    // violation: it ignored mod/rate/cw/z, over-budgeting short 16QAM frames
    // and under-budgeting long DBPSK ones). The timer starts at the FIRST data
    // frame's decode, so the remaining span is (group-1) frames of airtime;
    // burst_min_block_ is THIS group's measured samples-per-frame (cached from
    // frame 1), so the budget is ratiometric across every waveform/modulation
    // by construction: remaining airtime x1.5 + 3 s decode/jitter margin,
    // floored at the legacy 8 s so slow-CPU hosts keep their old slack.
    const int burst_timeout_ms = [&]() -> int {
        if (burst_min_block_ > 0) {
            const float remaining_ms =
                static_cast<float>(burst_group_size - 1) *
                (static_cast<float>(burst_min_block_) * 1000.0f / 48000.0f);
            return std::max(static_cast<int>(BURST_TIMEOUT_MS_BASE),
                            static_cast<int>(remaining_ms * 1.5f + 3000.0f));
        }
        return static_cast<int>(BURST_TIMEOUT_MS_BASE *
                                (burst_group_size / 4.0f));
    }();

    // Timeout check
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - burst_start_time_).count();
    if (elapsed > burst_timeout_ms) {
        LOG_MODEM(WARN, "[%s] Burst group timeout: got %zu/%d frames",
                  log_prefix_.c_str(), burst_soft_buffer_.size(), burst_group_size);
        logBurstDiagnosticsAbort("timeout", burst_soft_buffer_.size());
        // FAST-NACK (ULTRA_UNIFIED_SEQ): a timed-out group means we decoded the
        // BURST_HEADER but couldn't acquire/decode the data frames behind it (a fade
        // landed on the data-frame window). DELIVER a FAILED group (0 frames) so the
        // connection re-emits its current SACK and the sender resends NOW — instead of
        // silently discarding and leaving the sender to eat its FULL ack timeout (the
        // "missing logic from the original burst path"). Drop the descriptor latch so the
        // next BURST_HEADER re-sets it.
        if (burst_transport_rx_ && burst_group_callback_) {
            sync_controller_.have_burst_descriptor_ = false;
            burst_group_callback_(last_burst_group_seq_, std::vector<Bytes>{},
                                  /*all_ok=*/false, /*quality=*/0.0f, /*frame_mask=*/0,
                                  use_burst_interleave_,
                                  static_cast<uint8_t>(burst_group_size));
        }
        // Discard — TX used 4-frame interleaving, partial is undecodable
        {
            std::lock_guard<std::mutex> slock(stats_mutex_);
            stats_.frames_failed += burst_group_size;
        }
        burst_soft_buffer_.clear();
        burst_metric_templates_.clear();
        clearBurstDiagnostics();
        {
            std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
            sync_controller_.ring_.correlation_pos_ = burst_next_pos_;
        }
        state_ = DecoderState::SEARCHING;
        return;
    }

    // #56 BURST-ACC DRAIN: demodulate ALL already-arrived in-group frames this wake instead of one
    // per ~50ms processBuffer() tick. Within a burst the sender keys down ONCE and streams the group
    // contiguously, so after the frozen-anchor SYNC the group can already be in the ring; consuming
    // one frame per wake then leaves it un-drained, trailing live audio (the measured [RXLAG] BURST_ACC
    // backlog → late ACK → rig turnaround ~3.1s vs OTASim ~0.8s). tryDemodulateNextBurstFrame()'s
    // WAITING return is the exact write_pos_-gated "next frame not on the wire yet" sentinel, so
    // draining until WAITING NEVER reads unarrived audio (it is inert/correct when the lag is inherent).
    // Guardrails: bounded by group size; every state-changing terminal returns immediately; a ~30ms
    // wall-budget yields so the audio feed thread is never starved (one LDPC frame is ~88ms, so the
    // budget caps the loop after the in-flight frame); no new locking (handler keeps copy-under-lock /
    // demod-lock-free). reset_generation_ is re-checked inside tryDemodulateNextBurstFrame each call.
    const auto drain_start = std::chrono::steady_clock::now();
    constexpr long kBurstDrainWallBudgetMs = 30;
    for (int drained = 0; drained < burst_group_size; ++drained) {
        auto result = tryDemodulateNextBurstFrame();

        if (result == BurstFrameResult::FAILED) {
            // Hard failure (energy lost or process error) — abort immediately
            LOG_MODEM(WARN, "[%s] Burst group aborted: hard failure at frame %zu/%d",
                      log_prefix_.c_str(), burst_soft_buffer_.size() + 1, burst_group_size);
            logBurstDiagnosticsAbort("hard_failure", burst_soft_buffer_.size() + 1);
            {
                std::lock_guard<std::mutex> slock(stats_mutex_);
                stats_.frames_failed += burst_group_size;
            }
            burst_soft_buffer_.clear();
            burst_metric_templates_.clear();
            clearBurstDiagnostics();
            {
                std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
                sync_controller_.ring_.correlation_pos_ = burst_next_pos_;
            }
            state_ = DecoderState::SEARCHING;
            return;
        }

        if (result == BurstFrameResult::WAITING) {
            return;  // Next frame not on the wire yet — (B) boundary; resume next tick.
        }

        // SUCCESS — check if group complete
        if (static_cast<int>(burst_soft_buffer_.size()) == burst_group_size) {
            finalizeBurstGroup();
            burst_soft_buffer_.clear();
            burst_metric_templates_.clear();
            clearBurstDiagnostics();
            {
                std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
                sync_controller_.ring_.correlation_pos_ = burst_next_pos_;
            }
            state_ = DecoderState::SEARCHING;
            return;
        }

        // Non-final SUCCESS: an already-arrived frame was reclaimed (the drainable (A) backlog).
        // Yield if we have spent the wall budget so the audio feed thread keeps running.
        const long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - drain_start).count();
        if (elapsed_ms >= kBurstDrainWallBudgetMs) {
            return;  // Stay BURST_ACCUMULATING; resume the drain next tick.
        }
    }
    // Drained group_size frames without group-complete (shouldn't happen — completion returns above);
    // fall through and resume next tick.
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
        std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
        if (sync_controller_.ring_.write_pos_ >= burst_next_pos_) {
            next_available = sync_controller_.ring_.write_pos_ - burst_next_pos_;
        } else {
            next_available = sync_controller_.ring_.buffer_capacity_samples_ - burst_next_pos_ + sync_controller_.ring_.write_pos_;
        }
    }

    // [RXLAG-DIAG / #56] A/B split discriminator (ULTRA_RX_LAG_DIAG=1, removable). ratio = how many
    // whole frames are ALREADY present at burst_next_pos_: ratio>=2 => the next frame(s) already
    // arrived = DRAINABLE backlog (the per-wake serialization is the lag); ratio~1 then WAITING =>
    // INHERENT airtime (the frame genuinely isn't on the wire yet, no drain can help). Decides whether
    // the burst drain below is the lever. Per-frame (~8/burst) — no rate limit needed.
    {
        static const bool kRxLagDiag = [] {
            const char* e = std::getenv("ULTRA_RX_LAG_DIAG");
            return e && e[0] == '1';
        }();
        if (kRxLagDiag && burst_min_block_ > 0) {
            LOG_MODEM(INFO, "[BURST_DRAIN] next_available=%.0fms ratio=%.2f frame=%zu/%d",
                      static_cast<double>(next_available) / 48.0,
                      static_cast<double>(next_available) /
                          static_cast<double>(burst_min_block_),
                      burst_soft_buffer_.size() + 1, burst_group_size);
        }
    }

    if (next_available < burst_min_block_) {
        return BurstFrameResult::WAITING;
    }

    // Copy samples from circular buffer
    std::vector<float> block(burst_min_block_);
    size_t block_start_pos = burst_next_pos_;
    {
        std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
        for (size_t i = 0; i < burst_min_block_; i++) {
            block[i] = sync_controller_.ring_.buffer_[sync_controller_.ring_.wrapRingIndexLocked(burst_next_pos_ + i)];
        }
    }

    size_t abs_burst = burst_next_pos_;
    if (sync_controller_.ring_.total_fed_ >= sync_controller_.ring_.buffer_capacity_samples_) {
        const size_t oldest_abs = sync_controller_.ring_.total_fed_ - sync_controller_.ring_.buffer_capacity_samples_;
        const size_t oldest_pos = sync_controller_.ring_.write_pos_;
        const size_t offset = (burst_next_pos_ >= oldest_pos)
            ? (burst_next_pos_ - oldest_pos)
            : (sync_controller_.ring_.buffer_capacity_samples_ - oldest_pos + burst_next_pos_);
        abs_burst = oldest_abs + offset;
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
    // Calibration instrumentation (ULTRA_BURST_RMS_DIAG): emit per-frame the gate inputs
    // for EVERY collected frame (kept and erased) so the relative-gate threshold can be set
    // from the measured kept-vs-dead separation instead of an assumed constant.
    static const bool kBurstRmsDiag = [] {
        const char* e = std::getenv("ULTRA_BURST_RMS_DIAG");
        return e && e[0] == '1';
    }();
    if (kBurstRmsDiag) {
        const float nf = sync_controller_.ring_.noise_floor_;
        LOG_MODEM(INFO,
                  "[%s] burst-rms-diag frame=%zu/%d next_rms=%.5f anchor_rms=%.5f "
                  "noise_floor=%.5f next/anchor=%.3f next/noise=%.2f",
                  log_prefix_.c_str(), burst_soft_buffer_.size() + 1, burst_group_size,
                  next_rms, burst_anchor_rms_, nf,
                  burst_anchor_rms_ > 0.0f ? next_rms / burst_anchor_rms_ : 0.0f,
                  nf > 0.0f ? next_rms / nf : 0.0f);
    }
    // Erasure gate, operating-level-RELATIVE (replaces a fixed absolute 0.015 broadband-RMS
    // floor). The old absolute value implicitly meant "~25 dB below the typical SIM anchor
    // (~0.27 RMS)". On a lower-RX-level channel (real HF, a peak-limited/cheap TX) the whole
    // burst rides several dB lower, so a fixed 0.015 becomes only a few dB below the anchor
    // and erases NORMALLY-faded, RECOVERABLE data frames. Measured on IONOS MPG (pi5_full.log):
    // the chirp anchor (frame 1) decodes but data frames 2-6 are erased at RMS 0.004-0.0145
    // every group -> all-zero-LLR -> "header invalid" -> ARQ retransmit storm -> stall, while
    // the IDENTICAL transfer passes in sim where the anchor is ~0.27 and the same frames ride
    // ~0.10-0.30. Scaling the gate off THIS group's anchor RMS (chirp-dominated, ~constant-
    // envelope, the most stable per-group RX-level reference) tracks the operating point.
    // k = 0.055 reproduces the historical 0.015 at the sim anchor 0.27 (=> zero sim regression,
    // verified) and scales down with the RX level; a small absolute floor still erases an
    // essentially-dead/missing frame in the degenerate (uncaptured/near-zero) anchor case
    // without re-imposing the absolute-level bug. This is the per-group-adaptive form of the
    // gate; the cheap-card per-carrier decodability of the now-KEPT weak frames is a separate
    // lever (see docs/KNOWN_BUGS.md BUG-IONOS-PI5-CHEAP-DAC / per-carrier LLR work).
    constexpr float kBurstErasureAnchorFrac = 0.055f;  // ~25 dB below the group anchor
    constexpr float kBurstErasureAbsFloor = 0.005f;    // essentially-silence backstop
    // ULTRA_BURST_ERASURE_ABSOLUTE=1 forces the legacy fixed 0.015 gate (A/B harness for the
    // relative-gate benefit; default uses the operating-level-relative gate).
    static const bool kBurstErasureAbsolute = [] {
        const char* e = std::getenv("ULTRA_BURST_ERASURE_ABSOLUTE");
        return e && e[0] == '1';
    }();
    const float erase_thresh =
        kBurstErasureAbsolute
            ? 0.015f
        : (std::isfinite(burst_anchor_rms_) && burst_anchor_rms_ > 0.0f)
            ? std::max(kBurstErasureAnchorFrac * burst_anchor_rms_, kBurstErasureAbsFloor)
            : 0.015f;  // no anchor captured (should not happen mid-group) -> legacy absolute
    if (next_rms < erase_thresh) {
        LOG_MODEM(WARN, "[%s] Burst frame %zu/%d: energy lost (RMS=%.4f < %.4f "
                  "[%.3f*anchor %.4f]), inserting erasure",
                  log_prefix_.c_str(), burst_soft_buffer_.size() + 1,
                  burst_group_size, next_rms, erase_thresh,
                  kBurstErasureAnchorFrac, burst_anchor_rms_);
        burst_soft_buffer_.emplace_back(
            static_cast<size_t>(fec::BurstInterleaver::bitsPerFrame(fixed_frame_codewords_)),
            0.0f);
        pushMetricTemplate(0.0f);
        appendBurstPhysicalDiagnostics(abs_burst, burst_soft_buffer_.back(), next_rms,
                                       0.0f, 0.0f, burst_cfo_,
                                       /*erasure=*/true, /*process_ok=*/false);
        burst_next_pos_ = sync_controller_.ring_.wrapRingIndexLocked(burst_next_pos_ + burst_min_block_);
        return BurstFrameResult::SUCCESS;
    }

    // Software-ALC (BUG-QAM16-RIG-LEVEL-BUDGET): this data frame passed the erasure
    // gate — fold its BROADBAND wire level (raw ring samples, pre-CFO-shift) into
    // the per-group accumulator. Kept frames only: an erasure-gated frame is mostly
    // noise and would bias the data-segment RMS toward a false LOW verdict.
    {
        double sum_sq = 0.0;
        float peak = burst_level_peak_;
        for (float s : block) {
            sum_sq += static_cast<double>(s) * static_cast<double>(s);
            const float mag = std::abs(s);
            if (mag > peak) peak = mag;
        }
        burst_level_sum_sq_ += sum_sq;
        burst_level_sample_count_ += block.size();
        burst_level_peak_ = peak;
    }

    // Pre-correct CFO on burst block
    bool is_ofdm_burst = protocol::isOFDMMode(mode_);
    float burst_pre_cfo = 0.0f;
    if (is_ofdm_burst) {
        burst_pre_cfo = frame_demodulator_.applyCFOPreCorrection(block, burst_cfo_, abs_burst, log_prefix_.c_str());
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
        appendBurstPhysicalDiagnostics(abs_burst, burst_soft_buffer_.back(), next_rms,
                                       burst_pre_cfo, 0.0f, burst_cfo_,
                                       /*erasure=*/true, /*process_ok=*/false);
        burst_next_pos_ = sync_controller_.ring_.wrapRingIndexLocked(burst_next_pos_ + burst_min_block_);
        return BurstFrameResult::SUCCESS;
    }
    captureConstellationSnapshot();

    const float timing_offset = waveform_->getLastTimingOffsetSamples();
    // Per-frame timing correction is DISABLED within a burst group. The group is
    // contiguous and sliced at a FIXED stride (burst_min_block_) off the single
    // group-start anchor, so each frame's own LTS phase-slope timing estimate —
    // unreliable on a faded frame — must NOT re-slice that frame. A noisy estimate
    // on a deep-fade frame re-sliced the FFT window to a wrong position, corrupting
    // that physical frame; across several faded frames it took down the whole
    // interleaved group (0/8) and caused ~50% burst loss on Good fading (§14.25).
    // Trusting the fixed stride instead turns a faded frame into a clean zero-LLR
    // erasure that the burst interleaver + LDPC are designed to absorb.
    constexpr bool kBurstPerFrameTimingRetry = false;
    constexpr float kBurstContinuationRetryThreshold = 48.0f;
    constexpr float kBurstContinuationRetryMax = 320.0f;
    if (kBurstPerFrameTimingRetry &&
        std::abs(timing_offset) >= kBurstContinuationRetryThreshold &&
        std::abs(timing_offset) <= kBurstContinuationRetryMax) {
        const int sample_correction = static_cast<int>(std::lround(timing_offset));
        const size_t corrected_pos = sync_controller_.ring_.wrapRingIndexLocked(
            block_start_pos + sync_controller_.ring_.buffer_capacity_samples_ + sample_correction);
        const size_t corrected_abs =
            (sample_correction >= 0)
                ? abs_burst + static_cast<size_t>(sample_correction)
                : (abs_burst > static_cast<size_t>(-sample_correction)
                       ? abs_burst - static_cast<size_t>(-sample_correction)
                       : 0);

        bool have_corrected_block = false;
        {
            std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
            size_t corrected_available;
            if (sync_controller_.ring_.write_pos_ >= corrected_pos) {
                corrected_available = sync_controller_.ring_.write_pos_ - corrected_pos;
            } else {
                corrected_available = sync_controller_.ring_.buffer_capacity_samples_ - corrected_pos + sync_controller_.ring_.write_pos_;
            }
            have_corrected_block = corrected_available >= burst_min_block_;
            if (have_corrected_block) {
                block.assign(burst_min_block_, 0.0f);
                for (size_t i = 0; i < burst_min_block_; i++) {
                    block[i] = sync_controller_.ring_.buffer_[sync_controller_.ring_.wrapRingIndexLocked(corrected_pos + i)];
                }
            }
        }

        if (have_corrected_block) {
            burst_pre_cfo = is_ofdm_burst ? frame_demodulator_.applyCFOPreCorrection(block, burst_cfo_, corrected_abs, log_prefix_.c_str()) : 0.0f;
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
        appendBurstPhysicalDiagnostics(abs_burst, burst_soft_buffer_.back(), next_rms,
                                       burst_pre_cfo,
                                       waveform_ ? waveform_->estimatedCFO() : 0.0f,
                                       burst_cfo_,
                                       /*erasure=*/true, /*process_ok=*/true);
        burst_next_pos_ = sync_controller_.ring_.wrapRingIndexLocked(burst_next_pos_ + burst_min_block_);
        return BurstFrameResult::SUCCESS;
    }

    // Update CFO from pilot tracking (add pre-correction amount back)
    const float residual_cfo = waveform_->estimatedCFO();
    const auto cfo_update = cfo_tracker_.ingestPilotResidual(
        burst_pre_cfo, residual_cfo, burst_cfo_, /*clamp_drift=*/true);
    burst_cfo_ = cfo_update.accepted_cfo;
    last_fading_index_.store(waveform_->getFadingIndex());

    burst_soft_buffer_.push_back(std::move(soft));
    pushMetricTemplate(residual_cfo);
    appendBurstPhysicalDiagnostics(abs_burst, burst_soft_buffer_.back(), next_rms,
                                   burst_pre_cfo, residual_cfo, cfo_update.accepted_cfo,
                                   /*erasure=*/false, /*process_ok=*/true);
    burst_next_pos_ = sync_controller_.ring_.wrapRingIndexLocked(block_start_pos + burst_min_block_);

    LOG_MODEM(INFO, "[%s] Burst frame %zu/%d demodulated, RMS=%.4f",
              log_prefix_.c_str(), burst_soft_buffer_.size(),
              burst_group_size, next_rms);
    return BurstFrameResult::SUCCESS;
}

// Software-ALC increment 1 (BUG-QAM16-RIG-LEVEL-BUDGET): per-burst RX level verdict.
// Inputs (all already measured on this RX):
//   data_rms  — broadband RMS over the group's KEPT data frames (frames 2..N; the
//               chirp-anchored frame 1 and erasure-gated frames are excluded),
//   noise_rms — the idle chain-noise floor (IdleNoiseSNREstimator, in-band FIR RMS,
//               measured on lock-free SEARCHING-state audio between bursts),
//   CF        — burst crest factor peak/RMS over the same kept frames.
// Fade-robustness of the LOW verdict (self-adversarial check, documented): the
// numerator is averaged over the WHOLE multi-second group (~1-2 coherence times at
// Good — fade-averaged by construction) while the denominator is the noise floor the
// receiver actually hears, which on a chain-noise-dominated link does NOT move with
// the ionospheric path gain (and under RX AGC a signal fade LIFTS the audio noise
// floor into the data segment, partially cancelling the dip — the ratio moves LESS
// than the fade depth). Residual whole-burst troughs are absorbed by the 2-burst LOW
// hysteresis (connection side) and bounded by the sender's +0.5 dB step / 0.85
// ceiling / immediate CF down-step — a spurious "up" is small, capped, and
// self-correcting.
void StreamingDecoder::computeBurstLevelVerdict() {
    namespace alc = ::ultra::protocol::connection_policy;
    // Need >= one full kept data frame and a valid idle-noise reference; otherwise
    // skip (no seq bump): an erased-out group is evidence about FADING, not LEVEL.
    if (burst_level_sample_count_ == 0 ||
        (burst_min_block_ > 0 && burst_level_sample_count_ < burst_min_block_)) {
        return;
    }
    const auto idle = idle_noise_snr_estimator_.snapshot();
    if (!idle.valid || !(idle.normalized_noise_rms > 0.0f)) {
        return;
    }
    const float data_rms = static_cast<float>(std::sqrt(
        burst_level_sum_sq_ / static_cast<double>(burst_level_sample_count_)));
    if (!(data_rms > 0.0f) || !std::isfinite(data_rms)) {
        return;
    }

    // Basis note: data_rms/peak are BROADBAND wire quantities; the noise reference is
    // the idle estimator's in-band (50-2950 Hz FIR) RMS. The modem signal is fully
    // inside the FIR passband, so the mismatch only OVERSTATES headroom when the chain
    // carries out-of-band noise — i.e. it fails toward OK/hold (never asks for drive
    // the link doesn't need).
    const float headroom_db = 20.0f * std::log10(
        data_rms / std::max(idle.normalized_noise_rms, 1e-9f));
    const float cf_db = 20.0f * std::log10(
        std::max(burst_level_peak_, 1e-9f) / data_rms);

    // CLIPPED wins over LOW: clipping is a hard fault (raising drive on a clipped
    // chain destroys frames). A clipped-AND-buried burst reads LOW first (noise
    // Gaussianizes the CF toward ~10-12 dB) — still convergent: up-steps lift the
    // level until the clip signature emerges, then the fast down-step engages.
    alc::RxLevelVerdict verdict = alc::RxLevelVerdict::OK;
    if (cf_db < alc::alcClipCrestFactorDb()) {
        verdict = alc::RxLevelVerdict::CLIPPED;
    } else if (headroom_db < alc::alcLowHeadroomDb()) {
        verdict = alc::RxLevelVerdict::LOW;
    }

    rx_level_verdict_.store(static_cast<int>(verdict), std::memory_order_relaxed);
    rx_level_verdict_seq_.fetch_add(1, std::memory_order_relaxed);

    // Per-group measurement line (rig-greppable A/B trace).
    LOG_MODEM(INFO,
              "[%s] [ALC-RX] data_rms=%.4f noise_rms=%.4f headroom_db=%.1f "
              "cf_db=%.1f peak=%.3f samples=%zu verdict=%s",
              log_prefix_.c_str(), data_rms, idle.normalized_noise_rms, headroom_db,
              cf_db, burst_level_peak_, burst_level_sample_count_,
              verdict == alc::RxLevelVerdict::CLIPPED ? "CLIPPED"
              : verdict == alc::RxLevelVerdict::LOW   ? "LOW"
                                                      : "OK");

    // Operator advisory: once per verdict change (rate-limited by state). Always
    // logs, independent of ULTRA_SOFTWARE_ALC (the knob gates only the closed loop).
    if (static_cast<int>(verdict) != rx_level_last_logged_verdict_) {
        rx_level_last_logged_verdict_ = static_cast<int>(verdict);
        switch (verdict) {
            case alc::RxLevelVerdict::LOW:
                LOG_MODEM(INFO,
                          "[%s] LEVEL ADVISORY: RX level-limited (data %.1f dB over "
                          "chain noise, need >= %.1f dB) — sender should increase drive",
                          log_prefix_.c_str(), headroom_db, alc::alcLowHeadroomDb());
                break;
            case alc::RxLevelVerdict::CLIPPED:
                LOG_MODEM(INFO,
                          "[%s] LEVEL ADVISORY: TX clipping suspected (burst crest "
                          "factor %.1f dB; healthy OFDM arrives ~9-14 dB) — sender "
                          "should reduce drive",
                          log_prefix_.c_str(), cf_db);
                break;
            case alc::RxLevelVerdict::OK:
                LOG_MODEM(INFO,
                          "[%s] LEVEL ADVISORY: RX level OK (data %.1f dB over chain "
                          "noise, CF %.1f dB)",
                          log_prefix_.c_str(), headroom_db, cf_db);
                break;
        }
    }
}

void StreamingDecoder::finalizeBurstGroup() {
    const int burst_group_size = std::max(2, burst_group_size_);
    LOG_MODEM(INFO, "[%s] Burst group complete (%d frames), deinterleaving...",
              log_prefix_.c_str(), burst_group_size);

    // Software-ALC: fold this group's level accumulation into a fresh verdict BEFORE
    // the group callback below — the callback chain feeds it to the Connection, which
    // stamps the drive advisory onto THIS group's tone-burst ACK.
    computeBurstLevelVerdict();

    // Bytes-per-codeword: 243 at z=81 (N=1944), 81 at z=27. The active z is
    // announced by the sender in BURST_HEADER payload[5] and cached on
    // last_burst_descriptor_.lifting_z — the single RX source of truth
    // (activeBurstLiftingZ()); falls back to z=27 if no descriptor decoded.
    const int ldpc_z_for_burst = activeBurstLiftingZ();
    const int bytes_per_cw_rx = (ldpc_z_for_burst == 81) ? 243 : 81;

    // 2026-05-28: wrap the deinterleave call so a size-mismatch throw (e.g.,
    // the OFDM stream processor's LDPC_BLOCK_SIZE=648 gate returned the
    // demodulator early at z=81 and the per-frame buffer is only 1326 bits
    // instead of the 3888 needed for 2x1944 at z=81) is surfaced as a logged
    // ERROR instead of an uncaught exception killing the audio thread.
    //
    // Logs every input parameter we need to diagnose the actual mismatch:
    // expected vs received bits per frame, per-frame buffer sizes, z, cw.
    std::vector<std::vector<float>> logical_soft;
    if (!use_burst_interleave_) {
        // 2026-05-29 channel-adaptive interleaver (RX decouple): the descriptor
        // declared BURST_FLAG_INTERLEAVE=0, so the encoder applied NO byte
        // permutation across the group (TX decouple, d5d8eaa). Each accumulated
        // physical frame's soft bits ARE one logical frame, already in transmission
        // order — pass them straight through. The consequence the SR-ARQ-on-Good
        // plan depends on: per-frame LDPC success is now INDEPENDENT (a 1-3 s Good
        // fade kills a couple of frames, not the whole 6-frame group), so a
        // per-frame frame_mask is meaningful and the sender can resend only the dead
        // frames + refill instead of whole-burst-resending. Interleave is reserved
        // for Moderate/Poor, where the de-permutation buys real time/freq diversity.
        logical_soft = std::move(burst_soft_buffer_);
    } else {
    try {
        logical_soft = fec::BurstInterleaver::deinterleave(
            burst_soft_buffer_, fixed_frame_codewords_, bytes_per_cw_rx);
    } catch (const std::exception& e) {
        const int expected_bits =
            fec::BurstInterleaver::bitsPerFrame(fixed_frame_codewords_, bytes_per_cw_rx);
        const int n_frames = static_cast<int>(burst_soft_buffer_.size());
        size_t min_size = burst_soft_buffer_.empty() ? 0 : SIZE_MAX;
        size_t max_size = 0;
        for (const auto& f : burst_soft_buffer_) {
            min_size = std::min(min_size, f.size());
            max_size = std::max(max_size, f.size());
        }
        LOG_MODEM(ERROR,
                  "[%s] BurstInterleaver::deinterleave THREW '%s' — "
                  "z=%d cw=%d bytes_per_cw=%d expected_bits_per_frame=%d "
                  "n_frames=%d soft_buffer_min=%zu max=%zu — group dropped",
                  log_prefix_.c_str(), e.what(),
                  ldpc_z_for_burst, fixed_frame_codewords_, bytes_per_cw_rx,
                  expected_bits, n_frames, min_size, max_size);
        // Bail out — finalize as a 0/N group failure so the burst transport
        // controller resends. Without this catch the thread dies silently
        // and the entire RX side stops responding.
        burst_soft_buffer_.clear();
        return;
    } catch (...) {
        LOG_MODEM(ERROR,
                  "[%s] BurstInterleaver::deinterleave threw UNKNOWN exception — group dropped",
                  log_prefix_.c_str());
        burst_soft_buffer_.clear();
        return;
    }
    }  // end interleave-on deinterleave branch

    int logical_ok = 0;
    int logical_fail = 0;
    // §SR-ARQ (2026-05-29): per-frame SACK. bit i set = logical frame i decoded OK.
    // Meaningful as a true per-frame mask only when the group was NOT byte-interleaved
    // (interleave-off path): then each physical frame is one independent logical frame.
    // For an interleaved group only all_ok matters (mask collapses to all-set / 0x0000).
    // 16 bits (2026-07-02) — width matches the tone-burst wire mask
    // (tone_burst_ack::kPayloadFrameMaskBits) end-to-end.
    uint16_t frame_mask = 0;
    // §14.36 Phase 5c: track the worst codeword's LDPC iteration count across the
    // whole group — the group needs ALL frames, so its decode headroom = the weakest
    // frame's. Mapped to a quality in [0,1] for the receiver's rate feedback.
    int group_max_iters = 0;
    // §14.27 burst-transport RX: accumulate the group's decoded DATA frames so the
    // whole interleaved burst is delivered as a unit (the SR-ARQ per-frame path is
    // suppressed below when burst_transport_rx_ is set).
    std::vector<Bytes> burst_group_frames;
    int physical_erasures = 0;
    int physical_process_fail = 0;
    float min_physical_llr = std::numeric_limits<float>::max();
    const bool diagnostics_enabled = ultra::phyDiagnosticsEnabled();
    if (diagnostics_enabled) {
        for (const auto& diag : burst_physical_diag_) {
            if (diag.erasure) {
                ++physical_erasures;
            }
            if (!diag.process_ok) {
                ++physical_process_fail;
            }
            min_physical_llr = std::min(min_physical_llr, diag.mean_abs_llr);
        }
    }
    if (min_physical_llr == std::numeric_limits<float>::max()) {
        min_physical_llr = 0.0f;
    }

    for (int i = 0; i < burst_group_size; i++) {
        const int saved_pending_total_cw = pending_total_cw_;
        pending_total_cw_ = fixed_frame_codewords_;
        // HARQ provisional keys: expose the logical position to buildHarqKey
        // (same save/set/restore pattern as pending_total_cw_); -1 outside the
        // burst finalize loop doubles as the burst-path-only gate.
        burst_logical_index_ = i;
        DecodeResult result = decodeFrame(logical_soft[i], burst_snr_, burst_cfo_);
        burst_logical_index_ = -1;
        pending_total_cw_ = saved_pending_total_cw;
        if (i < static_cast<int>(burst_metric_templates_.size())) {
            const auto& metrics = burst_metric_templates_[static_cast<size_t>(i)];
            result.snr_db = metrics.snr_db;
            result.snr_source = metrics.snr_source;
            result.has_idle_in_band_snr_db = metrics.has_idle_in_band_snr_db;
            result.idle_in_band_snr_db = metrics.idle_in_band_snr_db;
            result.has_ofdm_broadband_snr_db = metrics.has_ofdm_broadband_snr_db;
            result.ofdm_broadband_snr_db = metrics.ofdm_broadband_snr_db;
            result.ofdm_internal_snr_db = metrics.ofdm_internal_snr_db;
            result.sync_quality_db = metrics.sync_quality_db;
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
        if (result.success) {
            ++logical_ok;
            // Mask width = the tone-burst wire mask width (16 as of 2026-07-02);
            // frames past it can't be selectively acked (window/group sizing keeps
            // groups within it — see kToneBurstAckWindowCapFrames).
            if (i < static_cast<int>(
                        ultra::waveform::tone_burst_ack::kPayloadFrameMaskBits)) {
                frame_mask |= static_cast<uint16_t>(1u << i);
            }
        } else {
            ++logical_fail;
        }
        for (int it : result.cw_iterations) {
            group_max_iters = std::max(group_max_iters, it);
        }

        if (burst_transport_rx_) {
            // §14.27: collect the group; deliver as a unit after the loop. Suppress
            // the per-frame SR-ARQ delivery so the file group does not also
            // double-process through onRxData/processArqFrame.
            if (result.success) {
                burst_group_frames.push_back(result.frame_data);
            }
        } else if (result.success || result.codewords_ok > 0) {
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
        if (diagnostics_enabled) {
            const auto logical_llr_quality = signal_policy::evaluatePreSyncLLR(
                logical_soft[static_cast<size_t>(i)].empty()
                    ? nullptr
                    : logical_soft[static_cast<size_t>(i)].data(),
                logical_soft[static_cast<size_t>(i)].size(),
                logical_soft[static_cast<size_t>(i)].size());
            std::ostringstream oss;
            oss << "event=burst_logical"
                << " station=" << log_prefix_
                << " group=" << burst_diag_group_index_
                << " logical=" << i
                << " success=" << boolDigit(result.success)
                << ' ' << burstFrameTypeAndSeq(result)
                << " cw_ok=" << result.codewords_ok
                << " cw_fail=" << result.codewords_failed
                << " llr_mean_abs=" << logical_llr_quality.mean_abs
                << " llr_near_zero=" << logical_llr_quality.near_zero_fraction
                << " residual_cfo_hz=" << result.lts_residual_cfo_hz
                << " routed_snr_db=" << result.snr_db
                << " sync_quality_db=" << result.sync_quality_db
                << " ofdm_internal_snr_db=" << result.ofdm_internal_snr_db
                << " fading=" << result.lts_fading_index
                << " sync_corr=" << result.sync_correlation;
            ultra::phyDiagLine(oss.str());
        }
    }

    if (burst_transport_rx_ && burst_group_callback_) {
        // Deliver the whole interleaved burst as a unit. all_ok requires every
        // logical frame of the group to have decoded — a partial group is
        // undecodable for reassembly and must be whole-burst-resent (no SACK).
        const bool all_ok = (logical_ok == burst_group_size);
        // §14.36: quality = decode headroom in [0,1]; 0 if the group failed. Worst-CW
        // iteration headroom against a fixed reference (clean decodes ~0-2 iters,
        // marginal ~40-70). The sender maps this to a rate via the controller.
        constexpr float kIterRef = 80.0f;
        const float quality =
            all_ok ? std::clamp(1.0f - static_cast<float>(group_max_iters) / kIterRef,
                                0.0f, 1.0f)
                   : 0.0f;
        LOG_MODEM(INFO,
                  "[%s] Burst group_seq=%u delivered as unit: %d/%d logical OK (all_ok=%d) "
                  "max_iters=%d quality=%.2f",
                  log_prefix_.c_str(), last_burst_group_seq_, logical_ok,
                  burst_group_size, all_ok ? 1 : 0, group_max_iters, quality);
        // HARQ key telemetry (cumulative since start) — the provisional-key
        // default-ON decision rides on mismatch staying ~0 (design review).
        {
            const auto& prof = ultra::timing::globalDecoderProfile();
            LOG_MODEM(INFO,
                      "[%s] [HARQ] keys real=%llu failed=%llu provisional=%llu "
                      "mismatch=%llu fresh_rescue=%llu",
                      log_prefix_.c_str(),
                      static_cast<unsigned long long>(prof.harq_key_build_success.load()),
                      static_cast<unsigned long long>(prof.harq_key_build_failed.load()),
                      static_cast<unsigned long long>(prof.harq_key_build_provisional.load()),
                      static_cast<unsigned long long>(prof.harq_prediction_mismatch.load()),
                      static_cast<unsigned long long>(prof.harq_fresh_rescue.load()));
        }
        // Per-group provisional context is stale once the group is delivered
        // (the ARQ state advances on this very delivery) — drop it.
        burst_harq_ctx_.reset();
        burst_harq_ctx_pulled_ = false;
        // §16.8 step 1: end-of-group warm-sync snapshot. Logs the
        // state we have RIGHT NOW, before the inter-group gap erodes
        // it. Pair with the BURST_HEADER-consume snapshot at
        // streaming_ofdm_decode.cpp to chart what survives the gap.
        LOG_MODEM(INFO,
                  "[%s] s16-snapshot end-of-group group_seq=%u phase=%s misses=%d "
                  "conf=%.2f last_cfo=%.2f next_expected=%llu last_frame_end=%llu "
                  "expect_full_anchor=%d quality=%.2f",
                  log_prefix_.c_str(), last_burst_group_seq_,
                  arrival_policy::warmSyncPhaseName(sync_controller_.derivePhase()),
                  sync_controller_.consecutiveSyncMisses(),
                  sync_controller_.frameArrivalConfidence(),
                  cfo_tracker_.tracked(),
                  static_cast<unsigned long long>(sync_controller_.next_expected_frame_sample_),
                  static_cast<unsigned long long>(sync_controller_.lastFrameEndSample()),
                  sync_controller_.expect_full_ofdm_anchor_ ? 1 : 0,
                  quality);
        // Refresh warm-sync on ANY acquired group, not just all_ok (2026-05-29,
        // shipped from the SyncV2 stabilization — this is the change that does the
        // work). Reaching "delivered as unit" means BRAVO found this group's
        // descriptor chirp AND demodulated all 6 frames — i.e. warm sync WORKED — even
        // when the LDPC then failed the DATA (a deep-fade group; ARQ resends it). The
        // old code refreshed only on all_ok, so a faded group left
        // sync_controller_.frameArrivalConfidence() decaying (the per-frame state machine doesn't fire
        // during the deinterleaved burst body, §16.11) until the narrow warm window
        // deactivated → the next group's acquisition collapsed → ~90s stall or a dead
        // transfer. An acquired-but-decode-failed group keeps warm sync HEALTHY; only
        // a genuinely un-acquired group (no chirp found — never reaches here) cools it.
        // Refresh warm-sync state on every delivered group — now unconditional
        // (promoted past ULTRA_S16_WARM_HANDOFF).
        // §7 C4: the warm-sync refresh + next-group anchor re-arm now lives on the controller
        // (it owns those four warm-sync-prediction fields).
        sync_controller_.noteGroupDelivered(last_burst_group_seq_);
        burst_group_callback_(last_burst_group_seq_, burst_group_frames, all_ok, quality,
                              frame_mask, use_burst_interleave_,
                              static_cast<uint8_t>(burst_group_size));
    }

    // 2026-05-28: snap the waveform's active LDPC lifting back to legacy z=27
    // now that the data group is delivered. The descriptor's z=81 was applied
    // to the waveform on BURST_HEADER decode (Phase 3) so getMinSamplesForCWCount
    // returned the right airtime for the N=1944 data frames during the group.
    // But the SAME accessor is queried by sync-acquisition for the NEXT
    // BURST_HEADER (which is always a 1-CW short LDPC control frame at z=27).
    // Without this reset, the receiver searches for a ~3x oversized next header
    // and never re-acquires sync, stalling the multi-group transfer. The next
    // BURST_HEADER will re-set z=81 when it decodes.
    if (waveform_) {
        waveform_->setActiveLDPCLiftingZ(27);
    }

    // 2026-06-05 (BUG-TNC-B2F-002): drop the BURST_HEADER descriptor latch at group-end too,
    // mirroring the waveform z snap-back above. have_burst_descriptor_ is BOTH the §14.24
    // "mid-burst" gate (streaming_ofdm_decode.cpp:1013) AND the source of truth for
    // activeBurstLiftingZ() (sync_controller.hpp:258). It was made to persist for the WHOLE
    // connection, which diverged from the per-group z lifecycle: after a burst the latch stayed
    // set, so every trailing NON-burst frame (the Winlink-B2F FF terminator, any interactive
    // frame after a file transfer) was gated as mid-burst noise and/or mis-sized as z=81/1944
    // and never delivered. The correct lifecycle (BURST_Z_LDPC_LIFECYCLE): default z=27; lift on
    // BURST_HEADER; drop at group-end. Each next group's BURST_HEADER re-sets the latch (it is
    // emitted per-group while the file transfer is active); after the LAST group it stays clear,
    // so the trailing short-LDPC frame falls through the gate and decodes. Same decode thread as
    // the set at streaming_ofdm_decode.cpp:762, so no extra lock.
    sync_controller_.have_burst_descriptor_ = false;

    if (diagnostics_enabled) {
        std::ostringstream oss;
        oss << "event=burst_group_end"
            << " station=" << log_prefix_
            << " group=" << burst_diag_group_index_
            << " logical_ok=" << logical_ok
            << " logical_fail=" << logical_fail
            << " physical_erasures=" << physical_erasures
            << " physical_process_fail=" << physical_process_fail
            << " min_physical_llr=" << min_physical_llr
            << " start_abs=" << burst_diag_group_start_abs_
            << " start_sec=" << (static_cast<double>(burst_diag_group_start_abs_) / 48000.0);
        ultra::phyDiagLine(oss.str());
    }
}


} // namespace gui
} // namespace ultra
