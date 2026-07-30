// StreamingDecoder module

#include "ofdm/genie_tx_capture.hpp"
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
#include "ofdm/iterative_chest.hpp"
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

    // LATE-JOIN group-end inference (design §3.4 + F1): a late-joined group's true end
    // cannot be counted (we do not know how many head frames died), and the airtime-
    // derived timeout below budgets a FULL group from ITS start — waiting it out here
    // would idle ~RTO-long and defeat the join. The slicer's WAITING is the "no new
    // member on the wire" sentinel; ~2 frame-times + margin of member silence after the
    // last catch means the key-down ended — tail-anchor and finalize NOW.
    if (late_join_head_missing_ && !burst_soft_buffer_.empty()) {
        const auto member_idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - late_join_last_frame_time_).count();
        const long frame_ms =
            (burst_min_block_ > 0)
                ? static_cast<long>(burst_min_block_) * 1000 / 48000
                : 1500;
        if (member_idle_ms > frame_ms * 2 + 1000) {
            LOG_MODEM(WARN,
                      "[%s] [LATE-JOIN] member silence %lld ms (~%ld ms/frame) — group "
                      "ended; tail-anchored finalize with %zu caught frame(s)",
                      log_prefix_.c_str(), static_cast<long long>(member_idle_ms),
                      frame_ms, burst_soft_buffer_.size());
            finalizeBurstGroup();
            burst_soft_buffer_.clear();
            burst_predecoded_.clear();
            descriptor_group_size_locked_ = false;
            burst_metric_templates_.clear();
            clearBurstDiagnostics();
            {
                std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
                sync_controller_.ring_.correlation_pos_ = burst_next_pos_;
            }
            state_ = DecoderState::SEARCHING;
            return;
        }
    }

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
        burst_predecoded_.clear();
        descriptor_group_size_locked_ = false;  // group ended/aborted — cfg writes may apply again
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
            burst_predecoded_.clear();
            descriptor_group_size_locked_ = false;  // group ended/aborted — cfg writes may apply again
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

        // SUCCESS — stamp the late-join member clock (group-end inference above).
        late_join_last_frame_time_ = std::chrono::steady_clock::now();
        // Check if group complete
        if (static_cast<int>(burst_soft_buffer_.size()) == burst_group_size) {
            finalizeBurstGroup();
            burst_soft_buffer_.clear();
            burst_predecoded_.clear();
            descriptor_group_size_locked_ = false;  // group ended/aborted — cfg writes may apply again
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

// LATE-JOIN (ULTRA_DESC_ARMED_ACCUM, docs/DESC_ARMED_ACCUMULATION_DESIGN_2026_07_05.md §3):
// the group HEAD (marker frame / descriptor anchor) died in a fade, so accumulation never
// armed — but THIS sync-accepted frame is a live group member carrying 1/N of every
// logical codeword (cross-frame interleave) or one whole independent logical frame
// (interleave-off). Arm the NORMAL accumulation/slicer machinery AT this frame using the
// latched descriptor geometry (the decoder's profile state is sticky from the latch);
// finalizeBurstGroup tail-anchors the caught run with leading erasures. §14.24 honored by
// construction: data-profile demod only, per-frame light-LTS channel estimate, fresh group
// seed (no control-profile probe, no stale shared estimate).
// Returns false when a full data frame is not yet in the ring (the caller keeps the legacy
// drop path; the NEXT member sync retries the join).
bool StreamingDecoder::lateJoinBurstAccumulation(size_t frame_sync_abs) {
    if (!waveform_) return false;
    const size_t data_block = static_cast<size_t>(
        waveform_->getMinSamplesForCWCount(fixed_frame_codewords_));
    if (data_block == 0) return false;
    std::vector<float> block;
    {
        std::lock_guard<std::mutex> lock(sync_controller_.ring_.buffer_mutex_);
        size_t available;
        if (sync_controller_.ring_.write_pos_ >= sync_position_) {
            available = sync_controller_.ring_.write_pos_ - sync_position_;
        } else {
            available = sync_controller_.ring_.buffer_capacity_samples_ - sync_position_ +
                        sync_controller_.ring_.write_pos_;
        }
        if (available < data_block) return false;
        block.assign(data_block, 0.0f);
        for (size_t i = 0; i < data_block; ++i) {
            block[i] = sync_controller_.ring_.buffer_[
                sync_controller_.ring_.wrapRingIndexLocked(sync_position_ + i)];
        }
    }
    float sum_sq = 0.0f;
    for (float s : block) sum_sq += s * s;
    const float join_rms = std::sqrt(sum_sq / static_cast<float>(block.size()));

    const float tracked_cfo = cfo_tracker_.tracked();
    const float pre_cfo = frame_demodulator_.applyCFOPreCorrection(
        block, tracked_cfo, frame_sync_abs, log_prefix_.c_str());
    waveform_->setAbsoluteTrainingPosition(frame_sync_abs);
    waveform_->setFrequencyOffset((std::abs(pre_cfo) > 0.01f) ? 0.0f : tracked_cfo);
    ultra::genie::eqTrace().site = "late-join";
    const bool ok = processWaveformForCodewords(
        SampleSpan(block.data(), block.size()), fixed_frame_codewords_);

    // Arm the group state exactly like the marker path (a failed member joins as an
    // in-place erasure — the join itself still buys the REST of the group).
    pending_total_cw_ = 0;
    burst_soft_buffer_.clear();
    burst_predecoded_.clear();
    burst_metric_templates_.clear();
    descriptor_group_size_locked_ = false;
    burst_min_block_ = data_block;
    burst_snr_ = sync_snr_;
    burst_cfo_ = tracked_cfo;
    burst_start_time_ = std::chrono::steady_clock::now();
    late_join_last_frame_time_ = burst_start_time_;
    burst_anchor_rms_ = join_rms;
    burst_level_sum_sq_ = 0.0;
    burst_level_sample_count_ = 0;
    burst_level_peak_ = 0.0f;

    auto soft = ok ? waveform_->getSoftBits() : std::vector<float>{};
    if (ok && !soft.empty()) {
        const float residual = waveform_->estimatedCFO();
        const auto cfo_update = cfo_tracker_.ingestPilotResidual(
            pre_cfo, residual, tracked_cfo, /*clamp_drift=*/true);
        burst_cfo_ = cfo_update.accepted_cfo;
        burst_soft_buffer_.push_back(std::move(soft));
        burst_predecoded_.emplace_back();  // group-start frame: finalize decodes it
        DecodeResult m;
        populateDecodeMetrics(m, protocol::isOFDMMode(mode_), residual);
        burst_metric_templates_.push_back(m);
        accumulateBurstCarrierGamma();
        beginBurstDiagnosticsGroup(frame_sync_abs, burst_soft_buffer_.back(), join_rms,
                                   pre_cfo, residual, burst_cfo_);
    } else {
        burst_soft_buffer_.emplace_back(
            static_cast<size_t>(fec::BurstInterleaver::bitsPerFrame(fixed_frame_codewords_)),
            0.0f);
        burst_predecoded_.emplace_back();  // erasure: no pre-decode
        DecodeResult m;
        populateDecodeMetrics(m, protocol::isOFDMMode(mode_), 0.0f);
        burst_metric_templates_.push_back(m);
        beginBurstDiagnosticsGroup(frame_sync_abs, burst_soft_buffer_.back(), join_rms,
                                   pre_cfo, 0.0f, burst_cfo_);
    }
    late_join_head_missing_ = true;
    headnull_resync_drop_count_ = 0;
    burst_next_pos_ =
        sync_controller_.ring_.wrapRingIndexLocked(sync_position_ + data_block);
    // F176/F221 ack gate (late-join: head unknown — conservative full-group
    // extent from THIS member's actual frame length; refreshed per frame).
    burst_data_start_abs_ = static_cast<uint64_t>(frame_sync_abs);
    burst_air_end_abs_.store(
        static_cast<uint64_t>(frame_sync_abs) +
            static_cast<uint64_t>(std::max(2, burst_group_size_)) *
                static_cast<uint64_t>(data_block),
        std::memory_order_relaxed);
    LOG_MODEM(WARN,
              "[%s] [LATE-JOIN] descriptor-armed accumulation from mid-group member "
              "(member_decode=%d cw=%d declared_group=%d) — head erasure-filled at finalize",
              log_prefix_.c_str(), ok ? 1 : 0, fixed_frame_codewords_,
              std::max(2, burst_group_size_));
    // BUG-BURST-STALE-GEOMETRY: a late-join IS a group start. Without this stamp a
    // late-joined group leaves a stale cadence reference and the next missed
    // descriptor would be mis-classified as a post-RTO resend.
    last_group_start_abs_ = static_cast<uint64_t>(frame_sync_abs);
    state_ = DecoderState::BURST_ACCUMULATING;
    return true;
}

void StreamingDecoder::refreshBurstAirEnd() {
    // F221: geometry-true ack gate — N x the group's actual per-frame samples.
    if (burst_data_start_abs_ == 0 || burst_min_block_ == 0) {
        return;
    }
    burst_air_end_abs_.store(
        burst_data_start_abs_ +
            static_cast<uint64_t>(std::max(2, burst_group_size_)) *
                static_cast<uint64_t>(burst_min_block_),
        std::memory_order_relaxed);
}

void StreamingDecoder::accumulateBurstCarrierGamma() {
    if (!waveform_) return;
    auto g = waveform_->getCarrierGammaSnapshot();
    if (g.empty()) return;
    // A group's FIRST frame (buffer holds one entry) resets the accumulator —
    // self-managing across aborted/failed groups without touching every clear
    // site.
    if (burst_soft_buffer_.size() <= 1 || burst_gamma_sum_.size() != g.size()) {
        burst_gamma_sum_.assign(g.size(), 0.0);
        burst_gamma_frames_ = 0;
    }
    for (size_t i = 0; i < g.size(); ++i) {
        burst_gamma_sum_[i] += static_cast<double>(g[i]);
    }
    ++burst_gamma_frames_;
}

void StreamingDecoder::finalizeGroupCarrierGammas() {
    last_group_carrier_gammas_.clear();
    if (burst_gamma_frames_ == 0 || burst_gamma_sum_.empty()) {
        return;
    }
    std::vector<float> mean(burst_gamma_sum_.size());
    double sum_all = 0.0;
    for (size_t i = 0; i < mean.size(); ++i) {
        mean[i] = static_cast<float>(burst_gamma_sum_[i] /
                                     static_cast<double>(burst_gamma_frames_));
        sum_all += mean[i];
    }
    // SCALE ANCHORING (brief §3): the demod gamma lives on the FFT-bin scale;
    // the anchor table on the receiver in-band scale. Normalize so the group's
    // mean gamma equals its measured OFDM broadband SNR — the flat-channel
    // identity then holds on the anchor table's own scale, per group, with no
    // calibration constant. Median across the group's per-frame estimates.
    std::vector<float> bb;
    for (const auto& m : burst_metric_templates_) {
        if (m.has_ofdm_broadband_snr_db && std::isfinite(m.ofdm_broadband_snr_db)) {
            bb.push_back(m.ofdm_broadband_snr_db);
        }
    }
    std::vector<float> mean_raw_for_diag;
    if (std::getenv("ULTRA_GAMMA_DOMAIN_LOG") != nullptr) {
        mean_raw_for_diag = mean;   // snapshot BEFORE renormalization
    }
    if (!bb.empty() && sum_all > 0.0) {
        std::nth_element(bb.begin(), bb.begin() + bb.size() / 2, bb.end());
        // kOfdmLegacyAnchorScaleOffsetDb: the EESM anchor table was measured on
        // the pre-2026-07-07 estimator scale (see connection_policy.hpp).
        const float med_db = bb[bb.size() / 2] +
            protocol::connection_policy::ofdmAnchorScaleOffsetDb();
        const double target = std::pow(10.0, static_cast<double>(med_db) / 10.0);
        const double mean_lin = sum_all / static_cast<double>(mean.size());
        if (mean_lin > 1e-12) {
            const float scale = static_cast<float>(target / mean_lin);
            for (auto& v : mean) v *= scale;
        }
    }
    // 2026-07-29 diag: the PER calibration in calibration/ofdm_per_v1.csv was fitted
    // against RAW getCarrierGammaSnapshot() gamma, but this vector is renormalized to
    // the in-band scale (and carries ofdmAnchorScaleOffsetDb). Applying the raw-fitted
    // table here without accounting for that would be a domain mismatch -- the fifth of
    // this class found today. Log both means so the shift is MEASURED, not assumed.
    if (std::getenv("ULTRA_GAMMA_DOMAIN_LOG") != nullptr) {
        double raw_sum = 0.0;
        for (size_t i = 0; i < mean_raw_for_diag.size(); ++i) {
            raw_sum += mean_raw_for_diag[i];
        }
        double norm_sum = 0.0;
        for (float v : mean) {
            norm_sum += v;
        }
        const size_t n = mean.size();
        if (n > 0 && !mean_raw_for_diag.empty()) {
            const double raw_db =
                10.0 * std::log10(std::max(raw_sum / mean_raw_for_diag.size(), 1e-12));
            const double norm_db = 10.0 * std::log10(std::max(norm_sum / n, 1e-12));
            LOG_MODEM(WARN, "GAMMA-DOMAIN raw_mean=%.2f dB norm_mean=%.2f dB shift=%+.2f dB",
                      raw_db, norm_db, norm_db - raw_db);
        }
    }
    last_group_carrier_gammas_ = std::move(mean);
    burst_gamma_sum_.clear();
    burst_gamma_frames_ = 0;
}

StreamingDecoder::BurstFrameResult StreamingDecoder::tryDemodulateNextBurstFrame() {
    const int burst_group_size = std::max(2, burst_group_size_);
    // channel_evidence_ok: the burst path's REAL per-frame decode verdict. The metrics
    // template's own .success is false by construction here, so without this the coherence
    // estimator receives nothing at all on the file path (see populateDecodeMetrics).
    auto pushMetricTemplate = [&](float residual_cfo_hz, bool channel_evidence_ok = false) {
        DecodeResult metrics;
        populateDecodeMetrics(metrics, protocol::isOFDMMode(mode_), residual_cfo_hz,
                              channel_evidence_ok);
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
        burst_predecoded_.emplace_back();  // erasure: no pre-decode
        pushMetricTemplate(0.0f);
        appendBurstPhysicalDiagnostics(abs_burst, burst_soft_buffer_.back(), next_rms,
                                       0.0f, 0.0f, burst_cfo_,
                                       /*erasure=*/true, /*process_ok=*/false);
        burst_next_pos_ = sync_controller_.ring_.wrapRingIndexLocked(burst_next_pos_ + burst_min_block_);
        refreshBurstAirEnd();
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

    // ULTRA_ITERATIVE_CHEST (default OFF) — post-FEC data-aided channel estimation.
    // Latched per group (knob read in startBurstDataAidedChest()); armed only on the
    // non-interleaved burst-transport path, where each PHYSICAL frame IS one logical
    // frame. With ULTRA_BURST_INTERLEAVE=1 a codeword spans every frame of the group,
    // so no frame's X can be reconstructed until the WHOLE group decodes — the
    // feature is structurally unavailable and stays off.
    // The origin is the frame's absolute sample position: it both sets the ABSOLUTE
    // Wiener time axis (a carried observation must not appear at dt = 0) and binds
    // the retained receive grid to this frame so a stale Y can never meet a fresh X.
    {
        // Knob re-read once per frame (~350-650 ms of airtime): never a hot path,
        // and NOT a function-local `static const`, so a test can toggle it live.
        data_aided_chest_enabled_ = ultra::ofdm::iterativeChestEnabled();
        const bool da_on = data_aided_chest_enabled_ && !use_burst_interleave_ &&
                           burst_transport_rx_ && protocol::isOFDMMode(mode_);
        waveform_->setDataAidedFeedbackEnabled(da_on);
        waveform_->setChannelHistoryFrameOrigin(static_cast<long long>(abs_burst));
        // Nothing to carry into the first frame we demodulate for this group.
        waveform_->armChannelHistoryCarry(da_on && !burst_soft_buffer_.empty());
    }
    ultra::genie::eqTrace().site = "burst-frame";
    bool ok = processWaveformForCodewords(
        SampleSpan(block.data(), block.size()), fixed_frame_codewords_);
    if (!ok) {
        LOG_MODEM(WARN, "[%s] Burst frame %zu/%d: process() failed, inserting erasure",
                  log_prefix_.c_str(), burst_soft_buffer_.size() + 1, burst_group_size);
        burst_soft_buffer_.emplace_back(
            static_cast<size_t>(fec::BurstInterleaver::bitsPerFrame(fixed_frame_codewords_)),
            0.0f);
        burst_predecoded_.emplace_back();  // erasure: no pre-decode
        pushMetricTemplate(0.0f);
        appendBurstPhysicalDiagnostics(abs_burst, burst_soft_buffer_.back(), next_rms,
                                       burst_pre_cfo, 0.0f, burst_cfo_,
                                       /*erasure=*/true, /*process_ok=*/false);
        burst_next_pos_ = sync_controller_.ring_.wrapRingIndexLocked(burst_next_pos_ + burst_min_block_);
        refreshBurstAirEnd();
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
            // The retry re-slices the SAME frame at a corrected position; the
            // retained receive grid is rebuilt, so re-announce its origin.
            waveform_->setChannelHistoryFrameOrigin(static_cast<long long>(corrected_abs));
            ultra::genie::eqTrace().site = "burst-timing-retry";
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
        burst_predecoded_.emplace_back();  // erasure: no pre-decode
        pushMetricTemplate(waveform_ ? waveform_->estimatedCFO() : 0.0f);
        accumulateBurstCarrierGamma();
        appendBurstPhysicalDiagnostics(abs_burst, burst_soft_buffer_.back(), next_rms,
                                       burst_pre_cfo,
                                       waveform_ ? waveform_->estimatedCFO() : 0.0f,
                                       burst_cfo_,
                                       /*erasure=*/true, /*process_ok=*/true);
        burst_next_pos_ = sync_controller_.ring_.wrapRingIndexLocked(burst_next_pos_ + burst_min_block_);
        refreshBurstAirEnd();
        return BurstFrameResult::SUCCESS;
    }

    // Update CFO from pilot tracking (add pre-correction amount back)
    const float residual_cfo = waveform_->estimatedCFO();
    const auto cfo_update = cfo_tracker_.ingestPilotResidual(
        burst_pre_cfo, residual_cfo, burst_cfo_, /*clamp_drift=*/true);
    burst_cfo_ = cfo_update.accepted_cfo;
    last_fading_index_.store(waveform_->getFadingIndex());

    burst_soft_buffer_.push_back(std::move(soft));
    // EARLY-FRAME-DECODE (2026-07-05, ULTRA_EARLY_FRAME_DECODE, default ON): for a
    // NON-interleaved group this physical frame IS one logical frame — LDPC it NOW,
    // in the inter-frame idle, so finalize only decodes the group-start + erasure
    // frames before the ACK emits (decode-tail off the turnaround; also smooths RX
    // CPU — no LDPC burst at group end). Decoded with the CURRENT burst_cfo_
    // (per-frame-fresh; finalize would use the end-of-group value). Same HARQ
    // index/save-restore discipline as the finalize loop; frame stats are counted
    // at finalize only (once per frame).
    static const bool kEarlyFrameDecode = [] {
        const char* e = std::getenv("ULTRA_EARLY_FRAME_DECODE");
        return !(e && e[0] == '0');
    }();
    if (kEarlyFrameDecode && !use_burst_interleave_ && burst_transport_rx_) {
        const int idx = static_cast<int>(burst_soft_buffer_.size()) - 1;
        const int saved_pending_total_cw = pending_total_cw_;
        pending_total_cw_ = fixed_frame_codewords_;
        burst_logical_index_ = idx;
        PredecodedFrame pre;
        pre.result = decodeFrame(burst_soft_buffer_.back(), burst_snr_, burst_cfo_);
        pre.valid = true;
        burst_logical_index_ = -1;
        pending_total_cw_ = saved_pending_total_cw;
        // ULTRA_ITERATIVE_CHEST: the verdict for THIS frame is in and the
        // demodulator still holds THIS frame's receive grid — the only moment the
        // two exist together. Re-modulate the verified bytes to X, form H = Y/X and
        // push it into the channel history so the NEXT frame of the group starts
        // from a measured channel instead of from its own LTS alone. Refused
        // silently (returns 0) unless the frame origin matches, so a late/finalize
        // decode can never pair a stale Y with a fresh X.
        if (!pre.result.data_aided_air_bytes.empty()) {
            const size_t obs =
                waveform_->ingestDataAidedFrame(pre.result.data_aided_air_bytes);
            LOG_MODEM(DEBUG,
                      "[%s] ITERATIVE_CHEST: frame %zu fed back %zu data-aided "
                      "channel observations",
                      log_prefix_.c_str(), burst_soft_buffer_.size(), obs);
        }
        burst_predecoded_.push_back(std::move(pre));
    } else {
        burst_predecoded_.emplace_back();  // finalize decodes it
    }
    // The frame DEMODULATED cleanly here (we reached the healthy push path), so its LTS
    // channel estimate is legitimate channel evidence even though the template's .success
    // is not yet set. Where a pre-decode ran, use its actual verdict.
    const bool frame_evidence_ok =
        burst_predecoded_.empty() ? true
                                  : (!burst_predecoded_.back().valid ||
                                     burst_predecoded_.back().result.success);
    pushMetricTemplate(residual_cfo, frame_evidence_ok);
    accumulateBurstCarrierGamma();
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
    // NOISE REFERENCE = the FLOOR (min over the estimator's last kFloorWindowCount windows),
    // not the latest window. `normalized_noise_rms` is whatever the MOST RECENT window measured,
    // so a single contaminated window becomes the reference instantly and sticks.
    //
    // Measured 2026-07-25 (rig, MPG@20, xfer_1): data_rms held rock-steady at 0.073-0.085 while
    // the reference ratcheted 0.0295 -> 0.0826 -> 0.0699 -> 0.1380 and LATCHED at 0.1380 for 18
    // of 25 groups, driving headroom to -4.4 dB and a permanent false LOW verdict. At
    // data_rms=0.0824 on a 20 dB S:N channel the true noise is ~0.0082, so 0.1380 was +24.5 dB
    // above anything the channel could produce — it was SIGNAL: the contaminating windows come
    // from the un-gated idle observation during an active transfer (own ACK tail, multipath echo
    // of the burst just received, or a burst the search failed to lock). Cross-checked against
    // the other meters on the same transfer: LTS usable 10.6-18.5 dB and EVM median 9.9 dB both
    // healthy; the ALC was the only meter claiming a problem.
    //
    // A noise FLOOR is the correct statistic for exactly this reason: contamination can only ADD
    // power, so it can never pull a minimum down. The estimator already computes it
    // (Snapshot::floor_noise_rms); this simply consumes the right field. Fall back to the legacy
    // field before the ring has populated. Erring toward OK is also the safe direction — a false
    // LOW makes the sender ramp drive into compression on a real radio, which actively harms.
    const float alc_noise_ref = (idle.floor_noise_rms > 0.0f) ? idle.floor_noise_rms
                                                              : idle.normalized_noise_rms;
    const float headroom_db = 20.0f * std::log10(
        data_rms / std::max(alc_noise_ref, 1e-9f));
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
              "[%s] [ALC-RX] data_rms=%.4f noise_rms=%.4f (latest %.4f) headroom_db=%.1f "
              "cf_db=%.1f peak=%.3f samples=%zu verdict=%s",
              log_prefix_.c_str(), data_rms, alc_noise_ref, idle.normalized_noise_rms, headroom_db,
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
    // ULTRA_ITERATIVE_CHEST: a data-aided observation is only valid inside one
    // continuous acquisition. The group is over — drop the retained grid and the
    // carried history so nothing leaks into the next group's estimate. No-op when
    // the feature was never armed.
    if (waveform_) {
        waveform_->setDataAidedFeedbackEnabled(false);
    }

    const int burst_group_size = std::max(2, burst_group_size_);

    // LATE-JOIN tail-anchor (design §3.4): the caught run has exact RELATIVE order (the
    // fixed-stride slicer preserves it, missed members already in-place erasures); the
    // unknown absolute offset is anchored to the TAIL — the group-end inference fires
    // ~2 frame-times after the last catch, consistent with the tail having survived.
    // The missing HEAD becomes leading zero-LLR erasures (exactly the representation
    // the deinterleaver + LDPC already absorb). A wrong anchor (tail also nulled, F2)
    // fails per-CW LDPC/CRC ⇒ prompt NACK — degradation is impossible by construction.
    if (late_join_head_missing_) {
        late_join_head_missing_ = false;
        const size_t declared = static_cast<size_t>(burst_group_size);
        if (burst_soft_buffer_.size() < declared) {
            const size_t missing = declared - burst_soft_buffer_.size();
            const std::vector<float> erasure(
                static_cast<size_t>(
                    fec::BurstInterleaver::bitsPerFrame(fixed_frame_codewords_)),
                0.0f);
            burst_soft_buffer_.insert(burst_soft_buffer_.begin(), missing, erasure);
            DecodeResult m;
            populateDecodeMetrics(m, protocol::isOFDMMode(mode_), 0.0f);
            burst_metric_templates_.insert(burst_metric_templates_.begin(), missing, m);
            // group_seq inference (design F5): the latched seq is the PREVIOUS group's
            // (this group's descriptor died with the head); a NEW group is +1. Resends
            // carry full-chirp anchors and rarely head-null; a wrong guess only affects
            // tone-ack dedup/crater matching for one event — the decoded frames' own
            // seq headers drive the actual SACK.
            last_burst_group_seq_ =
                static_cast<uint8_t>((last_burst_group_seq_ + 1u) & 0x3Fu);
            LOG_MODEM(WARN,
                      "[%s] [LATE-JOIN] tail-anchored finalize: %zu caught + %zu head "
                      "erasure(s) = %d declared (group_seq inferred %u)",
                      log_prefix_.c_str(), declared - missing, missing, burst_group_size,
                      static_cast<unsigned>(last_burst_group_seq_));
        }
    }

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
        burst_predecoded_.clear();
        descriptor_group_size_locked_ = false;  // group ended/aborted — cfg writes may apply again
        return;
    } catch (...) {
        LOG_MODEM(ERROR,
                  "[%s] BurstInterleaver::deinterleave threw UNKNOWN exception — group dropped",
                  log_prefix_.c_str());
        burst_soft_buffer_.clear();
        burst_predecoded_.clear();
        descriptor_group_size_locked_ = false;  // group ended/aborted — cfg writes may apply again
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

    // EARLY-FRAME-DECODE consumption: FAIL-SAFE — the pre-decode cache is used
    // ONLY when the group is non-interleaved AND the cache exactly mirrors the
    // logical frame list (size match; late-join head insertion grows the buffer
    // without the cache, so it mismatches and falls back to the full decode by
    // construction). A missed invalidation degrades to old behavior, never a
    // wrong decode.
    const bool predecode_ok =
        !use_burst_interleave_ &&
        burst_predecoded_.size() == logical_soft.size() &&
        static_cast<int>(logical_soft.size()) == burst_group_size;
    for (int i = 0; i < burst_group_size; i++) {
        DecodeResult result;
        if (predecode_ok && burst_predecoded_[static_cast<size_t>(i)].valid) {
            result = std::move(burst_predecoded_[static_cast<size_t>(i)].result);
        } else {
            const int saved_pending_total_cw = pending_total_cw_;
            pending_total_cw_ = fixed_frame_codewords_;
            // HARQ provisional keys: expose the logical position to buildHarqKey
            // (same save/set/restore pattern as pending_total_cw_); -1 outside the
            // burst finalize loop doubles as the burst-path-only gate.
            burst_logical_index_ = i;
            result = decodeFrame(logical_soft[i], burst_snr_, burst_cfo_);
            burst_logical_index_ = -1;
            pending_total_cw_ = saved_pending_total_cw;
        }
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
            // F147: substantive peer-TX evidence (see stampRxSubstantive).
            stampRxSubstantive();
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

            // STALE-CONFIG CRATER SUSPECT (2026-07-29, diagnostic only, no behaviour change).
            //
            // docs/F163_TIME_BUDGET_2026_07_06.md §3 sinks 1/3/4: a mode change announced on
            // a 0.21 s LIGHT descriptor can be missed, after which the receiver decodes the
            // whole group at its STALE config. Adoption then waits for a full-anchor RTO
            // resend -- 24.5-25.8 s measured versus 3-7 s when the descriptor decodes. Three
            // occurrences in one 51,200 B transfer cost ~129 s of a 435 s window, all flagged
            // NEW (unfixed) there.
            //
            // THE DISCRIMINATOR. A stale-config decode and a deep fade BOTH produce
            // all-codewords-failed, so a failure count cannot separate them. What separates
            // them is that a config mismatch keeps HEALTHY ACQUISITION while every codeword
            // dies, whereas a genuine fade takes the SNR and the correlation down with it.
            //
            // FIELDS: use only ones this path actually populates. A first version of this
            // check gated on `sync_quality_db >= 15` and `llr_mean_abs < 0.15`, taking both
            // numbers from the F163 report -- and was DEAD BY CONSTRUCTION, because on this
            // path sync_quality_db is 0 on every frame and llr_mean_abs runs 11-17, not
            // 0.06-0.08. Those report values came from the rig's own log format, a different
            // provenance. Measured here instead (Moderate @20, 22 frames): cratered frames
            // carry ofdm_internal_snr_db ~18 dB and sync_corr ~0.86 while failing all 12 CWs.
            //
            // NO LLR THRESHOLD ON PURPOSE. Cratered frames did show depressed llr_mean_abs
            // (11.46, 11.95) against 13.7-17.2 for partially-successful ones, but that is two
            // crater samples from one run -- far too thin to set a classifier boundary on.
            // The condition below is the physically motivated part (total crater despite good
            // acquisition); llr_mean_abs is already on this line, so a threshold can be
            // derived once enough occurrences are collected. Detect the CANDIDATE, report the
            // metric, do not pretend to classify yet.
            constexpr float kStaleCfgMinInternalSnrDb = 12.0f;
            constexpr float kStaleCfgMinSyncCorr = 0.70f;
            const bool all_cw_failed =
                result.codewords_ok == 0 && result.codewords_failed > 0;
            const bool acquisition_healthy =
                std::isfinite(result.ofdm_internal_snr_db) &&
                result.ofdm_internal_snr_db >= kStaleCfgMinInternalSnrDb &&
                result.sync_correlation >= kStaleCfgMinSyncCorr;
            if (all_cw_failed && acquisition_healthy) {
                oss << " stale_config_suspect=1"
                    << " configured_cw=" << fixed_frame_codewords_
                    << " group_size=" << burst_group_size_;
            }
            ultra::phyDiagLine(oss.str());
        }
    }

    finalizeGroupCarrierGammas();  // ready BEFORE the group callback reads it
    burst_air_end_abs_.store(0, std::memory_order_relaxed);  // F176: group over
    burst_data_start_abs_ = 0;
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
        // #%u ordinal = monotonic delivered-group counter (see burst_group_ordinal_):
        // group_seq is 0 for every group on the unified-seq path, so the ordinal is what
        // tells you the transfer is ADVANCING rather than wedged.
        ++burst_group_ordinal_;
        LOG_MODEM(INFO,
                  "[%s] Burst #%u (group_seq=%u) delivered as unit: %d/%d logical OK (all_ok=%d) "
                  "max_iters=%d quality=%.2f",
                  log_prefix_.c_str(), burst_group_ordinal_, last_burst_group_seq_, logical_ok,
                  burst_group_size, all_ok ? 1 : 0, group_max_iters, quality);
        // BUG-ANCHOR-CFO-KILL: group outcome owns the warm-CFO certificate. A
        // delivered group PROVES the tracked CFO (its frames decoded with it); a
        // 0/N group revokes it — the per-frame pilot residuals were ingested
        // before any LDPC verdict and may have walked the tracker (measured
        // -0.10 -> +0.29 across a crater stretch), so the next full anchor must
        // re-center from the chirp (cold arm) instead of keeping a poisoned warm
        // value. Partial groups leave the certificate untouched.
        if (all_ok) {
            cfo_tracker_.certifyWarm();
        } else if (logical_ok == 0) {
            cfo_tracker_.revokeWarm();
        }
        anchored_burst_backstop_armed_ = false;  // F165: standard ack path ran
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
