// StreamingDecoder - Continuous correlation receiver
//
// Architecture:
// - feedAudio(): Audio thread writes to ring buffer (fast, no processing)
// - processBuffer(): Decode thread runs state machine
//
// State machine:
//   SEARCHING → SYNC_FOUND → DECODING → SEARCHING
//
// Continuous correlation:
// - Search with small steps (100ms) to catch chirps quickly
// - Use RMS check to skip empty sections faster
// - FFT-based correlation for speed when signal present

#include "streaming_decoder.hpp"
#include "streaming_buffer_policy.hpp"
#include "streaming_decode_policy.hpp"
#include "streaming_decoder_debug.hpp"
#include "streaming_frame_arrival_policy.hpp"
#include "streaming_frame_policy.hpp"
#include "streaming_signal_policy.hpp"
#include "gui/startup_trace.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
#include "fec/frame_interleaver.hpp"  // Frame-level interleaving for fixed-CW frames
#include "fec/burst_interleaver.hpp"  // Burst-level long interleaver
#include "ultra/fec.hpp"              // LDPCDecoder for robust single-CW decode
#include "fec/ldpc_codec.hpp"         // getRecommendedIterations
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
namespace arrival_policy = streaming_frame_arrival_policy;
namespace frame_policy = streaming_frame_policy;
namespace signal_policy = streaming_signal_policy;

namespace {

bool isFixedFrameCwCount(int cw_count) {
    return cw_count >= v2::kMinFixedFrameCodewords &&
           cw_count <= v2::kMaxFixedFrameCodewords;
}

size_t mcDpskBitsPerSymbol(const MultiCarrierDPSKConfig& config) {
    return static_cast<size_t>(std::max(1, config.num_carriers * config.bits_per_symbol));
}

size_t validateBufferCapacity(size_t capacity) {
    if (capacity < StreamingDecoder::kMinimumBufferSamples) {
        throw std::invalid_argument("StreamingDecoder buffer capacity is smaller than the sync search window");
    }
    return capacity;
}

// Return a conservative 1-CW control-frame sample requirement for connected OFDM.
// Coherent data profiles use coherent QPSK R1/4 control; differential data
// profiles keep DQPSK R1/4 control. Either control profile can require more
// symbols than the current data profile.
size_t getOFDMControlFrameSamples(IWaveform* waveform,
                                  Modulation data_mod,
                                  CodeRate data_rate,
                                  bool coherent_control_enabled) {
    if (!waveform) {
        return 0;
    }

    size_t default_samples = static_cast<size_t>(waveform->getMinSamplesForControlFrame());
    if (data_mod == Modulation::DQPSK && data_rate == CodeRate::R1_4) {
        return default_samples;
    }

    // Avoid waveform reconfigure here (it recreates internal DSP state and can
    // clear constellation history). Estimate robust control size analytically.
    const int carriers = waveform->getCarrierCount();
    const int samples_per_symbol = waveform->getSamplesPerSymbol();
    if (carriers <= 0 || samples_per_symbol <= 0) {
        return default_samples;
    }

    return decode_policy::estimateRobustOFDMControlSamples(
        default_samples, data_mod, data_rate, carriers, samples_per_symbol,
        coherent_control_enabled);
}

}  // namespace

// ============================================================================
// DEBUG: Buffer snapshot for external analysis
// ============================================================================
// Dumps buffer contents to .f32 files at key sample counts
// Use: sox -t f32 -r 48000 -c 1 snapshot_*.f32 snapshot_*.wav
// Or: audacity can import raw 32-bit float

static void dumpBufferSnapshot(const std::vector<float>& buffer, size_t write_pos,
                                size_t total_fed, const std::string& label) {
    if (!g_debug_dumps_enabled) return;

    // Create filename with label and sample count
    char filename[256];
    snprintf(filename, sizeof(filename), "%s_%s_%zu.f32", g_dump_prefix, label.c_str(), total_fed);

    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        LOG_MODEM(WARN, "StreamingDecoder: Failed to create dump file: %s", filename);
        return;
    }

    // Dump entire buffer in linear order (unwrap circular buffer)
    // Start from oldest data (write_pos) and wrap around
    size_t buf_size = buffer.size();
    size_t valid_samples = std::min(total_fed, buf_size);

    // For simplicity, dump from position 0 to write_pos (most recent data)
    // This is what the search sees
    if (total_fed < buf_size) {
        // Buffer hasn't wrapped yet - dump from 0 to write_pos
        file.write(reinterpret_cast<const char*>(buffer.data()), write_pos * sizeof(float));
    } else {
        // Buffer wrapped - dump from write_pos to end, then 0 to write_pos
        file.write(reinterpret_cast<const char*>(buffer.data() + write_pos),
                   (buf_size - write_pos) * sizeof(float));
        file.write(reinterpret_cast<const char*>(buffer.data()),
                   write_pos * sizeof(float));
    }

    file.close();

    // Also compute and log some stats
    float rms = 0, max_val = 0;
    for (size_t i = 0; i < valid_samples && i < 10000; i++) {
        size_t idx = (total_fed < buf_size) ? i : ((write_pos + i) % buf_size);
        float s = buffer[idx];
        rms += s * s;
        max_val = std::max(max_val, std::abs(s));
    }
    rms = std::sqrt(rms / std::min(valid_samples, size_t(10000)));

    LOG_MODEM(DEBUG, "StreamingDecoder: Dumped %s: %zu samples to %s (RMS=%.4f, peak=%.4f)",
              label.c_str(), valid_samples, filename, rms, max_val);
}

StreamingDecoder::StreamingDecoder(size_t buffer_capacity_samples)
    : buffer_capacity_samples_(validateBufferCapacity(buffer_capacity_samples)),
      uses_default_buffer_capacity_(buffer_capacity_samples_ == kDefaultBufferSamples) {
    startupTrace("StreamingDecoder", "ctor-enter");
    buffer_.resize(buffer_capacity_samples_, 0.0f);
    startupTrace("StreamingDecoder", "buffer-resized");
    waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);
    startupTrace("StreamingDecoder", "waveform-created");
    interleaver_ = std::make_unique<ChannelInterleaver>(
        mcDpskBitsPerSymbol(mc_dpsk_config_), v2::LDPC_CODEWORD_BITS);
    startupTrace("StreamingDecoder", "interleaver-created");
    codec_ = fec::CodecFactory::create(fec::CodecType::LDPC, CodeRate::R1_4);
    startupTrace("StreamingDecoder", "codec-created");

    LOG_MODEM(INFO, "StreamingDecoder: Initialized (buffer=%zu samples)", buffer_capacity_samples_);

    // §15 step 4b: tone-burst ACK monitor. Production-tuned config keeps
    // the audio-thread CPU bounded:
    //   - Detection cadence 1.5 s (72,000 samples @ 48 kHz). The burst is
    //     675 ms baseline so a 1.5 s cadence still guarantees the buffer
    //     contains a complete burst between ticks; we don't need finer.
    //   - Single symbol duration {25 ms} for now. The 50 ms / 100 ms low-SNR
    //     rungs are deferred until SNR negotiation tells us we need them
    //     (step 4d-late). Each extra duration roughly doubles per-tick CPU.
    //   - Coarse sweep step 32 samples (vs the test default 8). At 4× the
    //     stride the detector still finds the burst (the burst is 32400
    //     samples; missing by 32 samples is < 0.1% of a symbol — Goertzel
    //     barely notices). 4× less work per tick.
    //   - Tighter buffer (90 k samples ≈ 1.9 s) since we only need to hold
    //     one burst + margin at the chosen cadence.
    //
    // Combined: ~16× CPU reduction vs the prior config. Audio thread no
    // longer jitters during a transfer.
    //
    // Default callback logs only; step 4d-iii routes detections to the
    // protocol layer.
    {
        ultra::waveform::tone_burst_ack::ToneBurstAckMonitor::Config tba_cfg;
        tba_cfg.detect_interval_samples = 72000;  // 1.5 s @ 48 kHz
        tba_cfg.symbol_durations_ms = {
            ultra::waveform::tone_burst_ack::kBaselineSymbolMs,  // 25 ms only
        };
        tba_cfg.sweep_step_samples = 32;
        tba_cfg.buffer_capacity_samples = 90000;  // ~1.9 s
        tone_burst_monitor_ =
            ultra::waveform::tone_burst_ack::ToneBurstAckMonitor(tba_cfg);
        // Install default log-only callback. Step 4d replaces.
        tone_burst_monitor_.setCallback(
            [this](const ultra::waveform::tone_burst_ack::ToneBurstAckDetection& d) {
                LOG_MODEM(INFO,
                          "[%s] ToneBurstAck monitor: detected group_seq=%u type=%s "
                          "frame_mask=0x%02X rate_hint=%u peak=%.1f symbol_ms=%u "
                          "hamming_corrected=%d stream_offset=%llu",
                          log_prefix_.c_str(),
                          static_cast<unsigned>(d.payload.group_seq),
                          d.payload.type ==
                                  ultra::waveform::tone_burst_ack::AckType::Nack
                              ? "NACK"
                              : "ACK",
                          static_cast<unsigned>(d.payload.frame_mask),
                          static_cast<unsigned>(d.payload.rate_hint),
                          d.correlation_peak,
                          static_cast<unsigned>(d.symbol_ms_used),
                          d.hamming_corrected_blocks,
                          static_cast<unsigned long long>(d.detected_stream_offset));
            });
    }
    startupTrace("StreamingDecoder", "ctor-exit");
}

void StreamingDecoder::setToneBurstAckCallback(ToneBurstAckCallback cb) {
    tone_burst_monitor_.setCallback(std::move(cb));
}

StreamingDecoder::~StreamingDecoder() {
    stop();
}

void StreamingDecoder::setBurstInterleaveGroupSize(int size) {
    burst_group_size_ = ofdm_link_adaptation::sanitizeBurstGroupSize(size);
}

void StreamingDecoder::observeIdleNoiseCandidate(const float* samples, size_t count) {
    if (!samples || count == 0) {
        return;
    }

    // Idle classification deliberately reuses the StreamingDecoder acquisition
    // state: this is called only after SEARCHING-state audio has gone through the
    // existing chirp/LTS detector and produced no lock. If any sync/decode state
    // is active, the samples are not an idle-noise observation.
    if (state_ != DecoderState::SEARCHING ||
        pending_total_cw_ != 0 ||
        mc_burst_pending_frame_) {
        return;
    }

    idle_noise_snr_estimator_.observeIdleAudio(samples, count);
}

void StreamingDecoder::resetFrameArrivalTrackingLocked() {
    warm_sync_active_ = false;
    warm_sync_phase_ = arrival_policy::WarmSyncPhase::COLD;
    next_expected_frame_sample_valid_ = false;
    next_expected_frame_sample_ = 0;
    frame_arrival_confidence_ = 0.0f;
    consecutive_sync_misses_ = 0;
    last_frame_arrival_valid_ = false;
    last_frame_start_sample_ = 0;
    last_frame_end_sample_ = 0;
    last_frame_arrival_error_valid_ = false;
    last_frame_arrival_error_samples_ = 0;
}

void StreamingDecoder::noteFrameArrivalSuccess(size_t frame_start_abs,
                                               size_t frame_end_abs) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    noteFrameArrivalSuccessLocked(frame_start_abs, frame_end_abs);
}

void StreamingDecoder::noteFrameArrivalSuccessLocked(size_t frame_start_abs,
                                                     size_t frame_end_abs) {
    if (!connected_ || mode_ != protocol::WaveformMode::OFDM_CHIRP) {
        return;
    }
    if (!next_expected_frame_sample_valid_ &&
        !expect_full_ofdm_anchor_ &&
        expected_frame_gap_samples_ == 0) {
        return;
    }

    const auto previous_phase = warm_sync_phase_;
    const auto update = arrival_policy::updateOnSuccessfulFrame(
        next_expected_frame_sample_valid_,
        next_expected_frame_sample_,
        frame_arrival_confidence_,
        frame_start_abs,
        frame_end_abs,
        expected_frame_gap_samples_);

    next_expected_frame_sample_valid_ = true;
    warm_sync_active_ = true;
    warm_sync_phase_ = arrival_policy::phaseAfterSuccessfulFrame();
    next_expected_frame_sample_ = update.next_expected_frame_sample;
    frame_arrival_confidence_ = update.confidence;
    consecutive_sync_misses_ = update.consecutive_sync_misses;
    last_frame_arrival_valid_ = true;
    last_frame_start_sample_ = frame_start_abs;
    last_frame_end_sample_ = frame_end_abs;
    last_frame_arrival_error_valid_ = update.has_arrival_error;
    last_frame_arrival_error_samples_ = update.arrival_error_samples;

    if (update.has_arrival_error) {
        LOG_MODEM(DEBUG, "[%s] warm-sync arrival: start=%zu end=%zu next=%zu error=%lld confidence=%.2f",
                  log_prefix_.c_str(), frame_start_abs, frame_end_abs,
                  next_expected_frame_sample_,
                  static_cast<long long>(last_frame_arrival_error_samples_),
                  frame_arrival_confidence_);
    } else {
        LOG_MODEM(DEBUG, "[%s] warm-sync arrival seeded: start=%zu end=%zu next=%zu confidence=%.2f",
                  log_prefix_.c_str(), frame_start_abs, frame_end_abs,
                  next_expected_frame_sample_, frame_arrival_confidence_);
    }

    if (previous_phase != warm_sync_phase_) {
        LOG_MODEM(INFO, "[%s] warm-sync state: %s -> %s",
                  log_prefix_.c_str(),
                  arrival_policy::warmSyncPhaseName(previous_phase),
                  arrival_policy::warmSyncPhaseName(warm_sync_phase_));
    }
}

void StreamingDecoder::noteFrameArrivalSyncMissLocked() {
    const auto previous_phase = warm_sync_phase_;
    consecutive_sync_misses_ = arrival_policy::incrementSyncMisses(consecutive_sync_misses_);
    frame_arrival_confidence_ =
        arrival_policy::confidenceAfterSyncMiss(frame_arrival_confidence_);

    if (next_expected_frame_sample_valid_ && last_frame_arrival_valid_) {
        const size_t last_duration =
            last_frame_end_sample_ >= last_frame_start_sample_
                ? (last_frame_end_sample_ - last_frame_start_sample_)
                : 0;
        const size_t cadence = last_duration + expected_frame_gap_samples_;
        if (cadence > 0) {
            next_expected_frame_sample_ += cadence;
        }
    }

    warm_sync_phase_ = arrival_policy::phaseAfterSyncMiss(consecutive_sync_misses_);
    if (warm_sync_phase_ == arrival_policy::WarmSyncPhase::RECOVERY) {
        warm_sync_active_ = false;
        next_expected_frame_sample_valid_ = false;
        frame_arrival_confidence_ = 0.0f;
    }

    if (previous_phase != warm_sync_phase_) {
        LOG_MODEM(INFO, "[%s] warm-sync state: %s -> %s (misses=%d)",
                  log_prefix_.c_str(),
                  arrival_policy::warmSyncPhaseName(previous_phase),
                  arrival_policy::warmSyncPhaseName(warm_sync_phase_),
                  consecutive_sync_misses_);
    }
}

void StreamingDecoder::writeSamplesToRingLocked(const float* samples, size_t count) {
    if (uses_default_buffer_capacity_) {
        for (size_t i = 0; i < count; i++) {
            buffer_[write_pos_] = samples[i];
            write_pos_ = (write_pos_ + 1) % kDefaultBufferSamples;
        }
        return;
    }

    const size_t capacity = buffer_capacity_samples_;
    for (size_t i = 0; i < count; i++) {
        buffer_[write_pos_] = samples[i];
        write_pos_ = (write_pos_ + 1) % capacity;
    }
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
size_t StreamingDecoder::wrapCustomRingIndexLocked(size_t value) const {
    return value % buffer_capacity_samples_;
}

size_t StreamingDecoder::getOFDMControlFrameSamplesForCurrentMode() const {
    return getOFDMControlFrameSamples(waveform_.get(), current_modulation_, code_rate_,
                                      coherent_ofdm_control_profile_enabled_);
}

// ============================================================================
// AUDIO THREAD - Just buffer samples, nothing else
// ============================================================================

void StreamingDecoder::feedAudio(const float* samples, size_t count) {
    if (!samples || count == 0) return;

    // §15 step 4b: feed the always-on tone-burst ACK monitor. Runs BEFORE
    // the OFDM buffer_mutex_ lock so the monitor never blocks OFDM decode
    // (and the monitor has its own internal storage; no mutex needed since
    // feedAudio is called from a single audio thread). The monitor's
    // callback fires synchronously here on the audio thread — keep it
    // cheap (log-only in 4b, lightweight queue-push in 4d).
    tone_burst_monitor_.feedAudio(samples, count);

    // Audio-activity instrumentation: detect "transmission arrived" events
    // independent of the chirp-search path. We measure RMS over each incoming
    // chunk and log low->high and high->low transitions. Each transmission
    // (chirp + data) should produce one low->high then one high->low. Compare
    // these to successful sync events to know if frames are arriving at the
    // soundcard but the chirp-search isn't catching them.
    {
        float sum_sq = 0.0f;
        for (size_t i = 0; i < count; ++i) sum_sq += samples[i] * samples[i];
        const float chunk_rms = std::sqrt(sum_sq / static_cast<float>(count));
        constexpr float ACTIVITY_GATE_HIGH = 0.030f;  // signal threshold
        constexpr float ACTIVITY_GATE_LOW  = 0.010f;  // silence threshold
        bool was_active = audio_activity_.load(std::memory_order_relaxed);
        if (!was_active && chunk_rms >= ACTIVITY_GATE_HIGH) {
            audio_activity_.store(true, std::memory_order_relaxed);
            uint64_t evt = audio_activity_events_.fetch_add(1, std::memory_order_relaxed) + 1;
            LOG_MODEM(INFO, "[%s] AudioActivity #%llu: ARRIVED rms=%.3f total_fed=%.2fs",
                      log_prefix_.c_str(),
                      static_cast<unsigned long long>(evt),
                      chunk_rms,
                      total_fed_ / 48000.0f);
        } else if (was_active && chunk_rms < ACTIVITY_GATE_LOW) {
            audio_activity_.store(false, std::memory_order_relaxed);
            // No log on departure to keep noise down — "arrived" events are enough.
        }
    }

    std::lock_guard<std::mutex> lock(buffer_mutex_);

    size_t prev_total = total_fed_;

    auto overflow = buffer_policy::planOverflowRecovery(
        write_pos_, correlation_pos_, total_fed_, count, buffer_capacity_samples_,
        CORR_INVARIANT_GUARD, OVERFLOW_RECOVERY_KEEP);

    if (overflow.pointer_drift_detected) {
        correlation_pos_ = write_pos_;
        setSearchFloorLocked(total_fed_);
        LOG_MODEM(WARN, "[%s] Correlation pointer drift detected, resetting search cursor",
                  log_prefix_.c_str());
    }

    // If adding these samples would overflow, drop backlog aggressively enough
    // to recover in one step. Small drops (~1k) cause repeated overflow storms.
    if (overflow.overflow) {
        correlation_pos_ = overflow.new_correlation_pos;
        setSearchFloorLocked(ringPosToAbsoluteLocked(correlation_pos_));

        // Once overloaded, any in-flight frame context is stale. Force a clean
        // resync from current audio instead of chasing old sync positions.
        bool reset_decode_state = false;
        if (state_ != DecoderState::SEARCHING) {
            state_ = DecoderState::SEARCHING;
            pending_total_cw_ = 0;
            burst_blocks_decoded_ = 0;
            burst_soft_buffer_.clear();
            burst_metric_templates_.clear();
            mc_burst_pending_frame_ = false;
            mc_burst_pending_soft_bits_.clear();
            reset_decode_state = true;
        }

        overflow_events_++;
        {
            std::lock_guard<std::mutex> slock(stats_mutex_);
            stats_.buffer_overflows = overflow_events_;
            stats_.overflow_samples_dropped += overflow.samples_to_drop;
            if (reset_decode_state) {
                stats_.overflow_state_resets++;
            }
        }
        if (overflow_events_ <= 3 || (overflow_events_ % 25) == 0) {
            LOG_MODEM(WARN, "[%s] Buffer overflow, dropped %zu unsearched samples (corr_pos=%zu, keep=%zu, state_reset=%d, total=%llu)",
                      log_prefix_.c_str(), overflow.samples_to_drop, correlation_pos_,
                      overflow.target_after_write,
                      reset_decode_state ? 1 : 0,
                      static_cast<unsigned long long>(overflow_events_));
        }
    }

    writeSamplesToRingLocked(samples, count);

    total_fed_ += count;

    // Update backlog telemetry for UI/CLI diagnostics.
    const auto backlog = buffer_policy::computeBacklog(
        write_pos_, correlation_pos_, total_fed_, buffer_capacity_samples_, 48000.0f);
    {
        std::lock_guard<std::mutex> slock(stats_mutex_);
        stats_.current_unsearched_samples = backlog.unsearched_samples;
        stats_.peak_unsearched_samples = std::max<uint64_t>(stats_.peak_unsearched_samples,
                                                            static_cast<uint64_t>(backlog.unsearched_samples));
        stats_.backlog_ms = backlog.backlog_ms;
        stats_.peak_backlog_ms = std::max(stats_.peak_backlog_ms, backlog.backlog_ms);
        stats_.buffer_fill_percent = backlog.fill_percent;
    }

    // DEBUG: Dump buffer snapshots at key sample counts
    // These help analyze what audio is actually in the buffer
    // Chirp structure: 7200 lead-in + 24000 up + 4800 gap + 24000 down + 4800 trail = 64800
    // So we should have full chirp around 72000 samples (64800 + margin)
    auto crossedThreshold = [prev_total, this](size_t threshold) {
        return prev_total < threshold && total_fed_ >= threshold;
    };

    if (crossedThreshold(24000)) {
        dumpBufferSnapshot(buffer_, write_pos_, total_fed_, "early_24k");
    }
    if (crossedThreshold(48000)) {
        dumpBufferSnapshot(buffer_, write_pos_, total_fed_, "mid_48k");
    }
    if (crossedThreshold(72000)) {
        dumpBufferSnapshot(buffer_, write_pos_, total_fed_, "full_chirp_72k");
    }
    if (crossedThreshold(96000)) {
        dumpBufferSnapshot(buffer_, write_pos_, total_fed_, "late_96k");
    }

    // Log at key thresholds to track audio arrival
    float audio_sec = total_fed_ / 48000.0f;
    auto crossedSecond = [prev_total, this](float sec) {
        size_t threshold = static_cast<size_t>(sec * 48000);
        return prev_total < threshold && total_fed_ >= threshold;
    };

    // Log every 0.5 seconds of audio
    if (crossedSecond(0.5f) || crossedSecond(1.0f) || crossedSecond(1.5f) ||
        crossedSecond(2.0f) || crossedSecond(2.5f) || crossedSecond(3.0f)) {
        float rms = 0.0f;
        for (size_t i = 0; i < count; i++) rms += samples[i] * samples[i];
        rms = std::sqrt(rms / count);
        LOG_MODEM(INFO, "[%s] feed: audio=%.2fs, RMS=%.4f",
                  log_prefix_.c_str(), audio_sec, rms);
    }

    // Wake up decode thread
    new_data_available_ = true;
    data_cv_.notify_one();
}

// ============================================================================
// DECODE THREAD - State machine for continuous correlation
// ============================================================================

void StreamingDecoder::processBuffer() {
    // Wait for new data or timeout
    bool woke_with_data = false;
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        woke_with_data = data_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
            return shutdown_.load() || new_data_available_;
        });
        if (shutdown_.load()) return;
        if (!woke_with_data) return;  // Timeout with no new audio; avoid stale re-search churn
        new_data_available_ = false;  // Clear flag after waking
        // §14.36: apply pending waveform changes BEFORE any decode work in
        // this processBuffer iteration. The actual configure() / waveform_
        // reconstruction (replaces modulator_/demodulator_/chirp_sync_
        // unique_ptrs) runs here at a clean boundary — never mid-decode.
        // Connected-OFDM-mode rebuild MUST run before descriptor-driven rate
        // change (it constructs the waveform_ that the rate change then
        // reconfigures). Both routes through deferred pending so RX never
        // crashes in HilbertTransform::process when ALPHA adapts its rate.
        applyPendingConnectedOFDMMode();
        applyPendingDescriptorDataMode();
    }

    switch (state_) {
        case DecoderState::SEARCHING:
            searchForSync();
            break;

        case DecoderState::SYNC_FOUND:
            checkIfReadyToDecode();
            break;

        case DecoderState::DECODING:
            decodeCurrentFrame();
            break;

        case DecoderState::BURST_ACCUMULATING:
            accumulateBurstFrames();
            break;

        case DecoderState::MCDPSK_BURST_CONTINUING:
            continueMCDPSKBurst();
            break;
    }
}

bool StreamingDecoder::hasFrame() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return !frame_queue_.empty();
}

DecodeResult StreamingDecoder::getFrame() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (frame_queue_.empty()) return DecodeResult{};
    DecodeResult r = std::move(frame_queue_.front());
    frame_queue_.pop();
    return r;
}

// ============================================================================
// MODE CONTROL
// ============================================================================

void StreamingDecoder::setMode(protocol::WaveformMode mode, bool connected) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    if (mode_ == mode && connected_ == connected) return;

    const bool waveform_mode_changed = mode_ != mode;
    mode_ = mode;
    connected_ = connected;

    if (waveform_mode_changed) {
        if (mode == protocol::WaveformMode::MC_DPSK) {
            waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);
        } else {
            waveform_ = WaveformFactory::create(mode);
        }
        if (waveform_) {
            waveform_->setCarrierMask(carrier_mask_);
        }

        size_t bps = mcDpskBitsPerSymbol(mc_dpsk_config_);
        if (protocol::isOFDMMode(mode)) {
            bps = 60;
        }
        // Match the encoder's z-aware interleaver block size. 2026-05-28 Phase 2:
        // primary source is the active burst descriptor's lifting_z; falls back
        // to env override and then v2::LDPC_CODEWORD_BITS (648) when no descriptor
        // has been seen yet (cold start / no active burst).
        const size_t ldpc_codeword_bits_ci = [this]() -> size_t {
            if (const char* env = std::getenv("ULTRA_LDPC_Z")) {
                if (std::atoi(env) == 81) return 1944;
            }
            if (have_burst_descriptor_ && last_burst_descriptor_.lifting_z == 81) {
                return 1944;
            }
            return v2::LDPC_CODEWORD_BITS;
        }();
        interleaver_ = std::make_unique<ChannelInterleaver>(bps, ldpc_codeword_bits_ci);
    }

    state_ = DecoderState::SEARCHING;
    pending_total_cw_ = 0;
    sync_reject_streak_ = 0;
    expect_full_ofdm_anchor_ = false;
    sync_from_warm_timed_window_ = false;
    resetFrameArrivalTrackingLocked();
    constellation_cache_.clear();
    constellation_cache_time_ = std::chrono::steady_clock::time_point{};

    // Clear burst interleave state on mode change
    burst_soft_buffer_.clear();
    burst_metric_templates_.clear();
    mc_burst_pending_frame_ = false;
    mc_burst_pending_soft_bits_.clear();
    use_burst_interleave_ = false;  // Re-enabled by caller if needed

    // CRITICAL: Reset correlation_pos_ to current write position
    // Otherwise we'll search old data from previous mode
    correlation_pos_ = write_pos_;
    setSearchFloorLocked(total_fed_);

    LOG_MODEM(INFO, "StreamingDecoder: Mode=%s (%s), reset corr_pos=%zu",
              protocol::waveformModeToString(mode), connected ? "connected" : "disconnected", correlation_pos_);
}

void StreamingDecoder::setCarrierMask(uint64_t active_mask) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    carrier_mask_ = active_mask;
    if (waveform_) {
        waveform_->setCarrierMask(active_mask);
    }
}

void StreamingDecoder::setCarrierLdpcInterleaver(bool enable) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    use_carrier_ldpc_interleaver_ = enable;
    if (waveform_) {
        waveform_->setCarrierLdpcInterleaverEnabled(enable);
    }
}

void StreamingDecoder::expectFullOFDMAnchorOnce() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (!connected_ || mode_ != protocol::WaveformMode::OFDM_CHIRP) {
        expect_full_ofdm_anchor_ = false;
        return;
    }

    expect_full_ofdm_anchor_ = true;
    sync_reject_streak_ = 0;
    LOG_MODEM(INFO, "StreamingDecoder: expecting full OFDM chirp+LTS timing anchor");
}

void StreamingDecoder::clearFullOFDMAnchorExpectation() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (expect_full_ofdm_anchor_) {
        LOG_MODEM(INFO, "StreamingDecoder: clearing pending full OFDM DATA anchor");
    }
    expect_full_ofdm_anchor_ = false;
}

bool StreamingDecoder::expectsFullOFDMAnchorForTesting() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return expect_full_ofdm_anchor_;
}

void StreamingDecoder::setMCDPSKCarriers(int n) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (mc_dpsk_carriers_ == n) return;
    mc_dpsk_carriers_ = n;
    mc_dpsk_config_.num_carriers = n;
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);
        interleaver_ = std::make_unique<ChannelInterleaver>(
            mcDpskBitsPerSymbol(mc_dpsk_config_), v2::LDPC_CODEWORD_BITS);
    }
}

void StreamingDecoder::setMCDPSKConfig(const MultiCarrierDPSKConfig& config) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    const bool changed =
        mc_dpsk_config_.num_carriers != config.num_carriers ||
        mc_dpsk_config_.samples_per_symbol != config.samples_per_symbol ||
        mc_dpsk_config_.bits_per_symbol != config.bits_per_symbol ||
        mc_dpsk_config_.freq_low != config.freq_low ||
        mc_dpsk_config_.freq_high != config.freq_high ||
        mc_dpsk_config_.chirp_f_start != config.chirp_f_start ||
        mc_dpsk_config_.chirp_f_end != config.chirp_f_end ||
        mc_dpsk_config_.chirp_duration_ms != config.chirp_duration_ms ||
        mc_dpsk_config_.use_dual_chirp != config.use_dual_chirp;
    if (!changed) return;

    mc_dpsk_config_ = config;
    mc_dpsk_carriers_ = config.num_carriers;
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);
        interleaver_ = std::make_unique<ChannelInterleaver>(
            mcDpskBitsPerSymbol(mc_dpsk_config_), v2::LDPC_CODEWORD_BITS);
    }

    LOG_MODEM(INFO, "StreamingDecoder: MC-DPSK config carriers=%d sps=%d bits/sym=%d raw=%.1f bps",
              mc_dpsk_config_.num_carriers, mc_dpsk_config_.samples_per_symbol,
              mc_dpsk_config_.bits_per_symbol, mc_dpsk_config_.getRawBitRate());
}

void StreamingDecoder::setOFDMConfig(const ModemConfig& config) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    // Store carrier counts regardless of current mode (used when switching to OFDM later)
    ofdm_carriers_ = config.num_carriers;
    ofdm_data_carriers_ = ofdm_link_adaptation::dataCarrierCount(
        static_cast<int>(config.num_carriers),
        config.use_pilots,
        static_cast<int>(config.pilot_spacing));

    // Only recreate waveform if currently in an OFDM mode.
    // When in MC-DPSK mode (e.g., disconnected waiting for PINGs), do NOT replace
    // the MC-DPSK waveform — that would break chirp-based sync detection.
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        LOG_MODEM(INFO, "StreamingDecoder: OFDM config stored (mode=MC-DPSK, not replacing waveform)");
        return;
    }

    if (mode_ == protocol::WaveformMode::OFDM_CHIRP) {
        waveform_ = std::make_unique<OFDMChirpWaveform>(config);
        LOG_MODEM(INFO, "StreamingDecoder: OFDM_CHIRP config set (FFT=%d, carriers=%d)",
                  config.fft_size, config.num_carriers);
    } else if (mode_ == protocol::WaveformMode::OFDM_NARROW) {
        waveform_ = std::make_unique<OFDMChirpWaveform>(config, protocol::WaveformMode::OFDM_NARROW);
        LOG_MODEM(INFO, "StreamingDecoder: OFDM_NARROW config set (FFT=%d, carriers=%d)",
                  config.fft_size, config.num_carriers);
    } else {
        LOG_MODEM(WARN, "StreamingDecoder: unexpected OFDM mode %d; defaulting to OFDM-CHIRP",
                  static_cast<int>(mode_));
        waveform_ = std::make_unique<OFDMChirpWaveform>(config);
    }
    if (waveform_) {
        waveform_->setCarrierMask(carrier_mask_);
    }

    // Update interleaver for new carrier count (using current modulation)
    size_t bps = static_cast<size_t>(ofdm_data_carriers_) * getBitsPerSymbol(current_modulation_);
    interleaver_ = std::make_unique<ChannelInterleaver>(bps, v2::LDPC_CODEWORD_BITS);
}

void StreamingDecoder::setConnectedOFDMMode(protocol::WaveformMode mode,
                                            const ModemConfig& config,
                                            Modulation mod,
                                            CodeRate rate) {
    // §14.36 crash fix v3 (2026-05-28): defer the waveform_ reconstruction
    // to the safe top-of-processBuffer boundary. Inline construction races
    // the RX thread (which releases buffer_mutex_ before searchForSync) and
    // SIGSEGVs in HilbertTransform::process — same root cause as v2 setDataMode.
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    pending_connected_ofdm_mode_ = mode;
    pending_connected_ofdm_config_ = config;
    pending_connected_ofdm_mod_ = mod;
    pending_connected_ofdm_rate_ = rate;
    pending_connected_ofdm_change_ = true;
}

void StreamingDecoder::applyPendingConnectedOFDMMode() {
    // Caller already holds buffer_mutex_ and we are at the safe boundary
    // (top of processBuffer, before any decode work in this iteration).
    if (!pending_connected_ofdm_change_) return;
    pending_connected_ofdm_change_ = false;
    const protocol::WaveformMode mode = pending_connected_ofdm_mode_;
    const ModemConfig& config = pending_connected_ofdm_config_;
    const Modulation mod = pending_connected_ofdm_mod_;
    const CodeRate rate = pending_connected_ofdm_rate_;

    const bool preserve_full_anchor =
        expect_full_ofdm_anchor_ || mode == protocol::WaveformMode::OFDM_CHIRP;

    mode_ = mode;
    connected_ = true;
    code_rate_ = rate;
    current_modulation_ = mod;
    ofdm_carriers_ = config.num_carriers;
    ofdm_data_carriers_ = ofdm_link_adaptation::dataCarrierCount(
        static_cast<int>(config.num_carriers),
        config.use_pilots,
        static_cast<int>(config.pilot_spacing));

    if (mode_ == protocol::WaveformMode::OFDM_CHIRP) {
        waveform_ = std::make_unique<OFDMChirpWaveform>(config);
    } else if (mode_ == protocol::WaveformMode::OFDM_NARROW) {
        waveform_ = std::make_unique<OFDMChirpWaveform>(config, protocol::WaveformMode::OFDM_NARROW);
    } else {
        waveform_ = std::make_unique<OFDMChirpWaveform>(config);
    }

    if (waveform_) {
        waveform_->configure(mod, rate);
        waveform_->setCarrierMask(carrier_mask_);
    }

    // Query waveform for effective pilot layout after configure().
    int pilot_spacing = waveform_ ? waveform_->getPilotSpacing() : 0;
    ofdm_data_carriers_ = ofdm_link_adaptation::dataCarrierCount(
        ofdm_carriers_, pilot_spacing > 0, pilot_spacing);

    size_t bps = static_cast<size_t>(ofdm_link_adaptation::bitsPerOFDMSymbol(
        ofdm_carriers_, pilot_spacing > 0, pilot_spacing, mod));
    interleaver_ = std::make_unique<ChannelInterleaver>(bps, v2::LDPC_CODEWORD_BITS);

    state_ = DecoderState::SEARCHING;
    pending_total_cw_ = 0;
    sync_reject_streak_ = 0;
    expect_full_ofdm_anchor_ = preserve_full_anchor;
    sync_from_warm_timed_window_ = false;
    resetFrameArrivalTrackingLocked();
    constellation_cache_.clear();
    constellation_cache_time_ = std::chrono::steady_clock::time_point{};
    burst_soft_buffer_.clear();
    burst_metric_templates_.clear();
    mc_burst_pending_frame_ = false;
    mc_burst_pending_soft_bits_.clear();
    correlation_pos_ = write_pos_;
    setSearchFloorLocked(total_fed_);

    LOG_MODEM(INFO, "StreamingDecoder: connected OFDM mode=%s, mod=%s, rate=%s, carriers=%d data=%d bps=%zu",
              protocol::waveformModeToString(mode_),
              modulationToString(mod), codeRateToString(rate),
              ofdm_carriers_, ofdm_data_carriers_, bps);
    if (expect_full_ofdm_anchor_) {
        LOG_MODEM(INFO, "StreamingDecoder: connected OFDM config armed full chirp+LTS timing anchor");
    }
}

void StreamingDecoder::setDataMode(Modulation mod, CodeRate rate) {
    // §14.36 crash fix v2 (2026-05-28): processBuffer RELEASES buffer_mutex_
    // before calling searchForSync (line 493). If we apply configure() here
    // (which replaces modulator_/demodulator_/chirp_sync_ unique_ptrs), the
    // RX thread mid-searchForSync runs with stale pointers -> SIGSEGV in
    // HilbertTransform::process (ultra_gui-2026-05-28-005140.ips, alpha
    // adaptive R3/4->R2/3 at 45.138s, crashed at 45.142s).
    //
    // Route ALL configure() calls through the defer-pending channel — the
    // actual configure() runs at the TOP of processBuffer (post-wait, before
    // any decode work). Same channel the BURST_HEADER intercept uses; setting
    // the same fields from setDataMode is correct (last write wins, the
    // controller's latest decision is what we want for the next iteration).
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    pending_descriptor_mod_ = mod;
    pending_descriptor_rate_ = rate;
    pending_descriptor_rate_change_ = true;
}

void StreamingDecoder::applyPendingDescriptorDataMode() {
    // Caller already holds buffer_mutex_. Apply the deferred descriptor-driven
    // rate switch, if any (set by the BURST_HEADER intercept). This runs at the
    // top of processBuffer — between iterations — so the configure() can swap
    // modulator_/demodulator_/chirp_sync_ unique_ptrs without any concurrent
    // decode work using them. Idempotent + no-op when nothing's pending.
    if (!pending_descriptor_rate_change_) return;
    pending_descriptor_rate_change_ = false;
    const Modulation mod = pending_descriptor_mod_;
    const CodeRate rate = pending_descriptor_rate_;
    if (mod == current_modulation_ && rate == code_rate_) {
        return;  // already matches, nothing to do
    }
    LOG_MODEM(INFO,
              "StreamingDecoder: applying deferred descriptor rate change %s %s -> %s %s",
              modulationToString(current_modulation_), codeRateToString(code_rate_),
              modulationToString(mod), codeRateToString(rate));
    applyDataModeUnlocked(mod, rate);
}

void StreamingDecoder::applyDataModeUnlocked(Modulation mod, CodeRate rate) {
    // §14.36: callable from contexts that already hold buffer_mutex_ (e.g. the
    // BURST_HEADER intercept inside processBuffer), so the receiver can switch
    // rate from the descriptor's declaration mid-transfer.
    code_rate_ = rate;
    current_modulation_ = mod;
    if (waveform_) waveform_->configure(mod, rate);

    // After configure(), the waveform has updated pilot config
    // Query waveform for actual pilot_spacing (coherent modes use denser pilots)
    if (mode_ != protocol::WaveformMode::MC_DPSK && waveform_) {
        int pilot_spacing = waveform_->getPilotSpacing();
        ofdm_data_carriers_ = ofdm_link_adaptation::dataCarrierCount(
            ofdm_carriers_, pilot_spacing > 0, pilot_spacing);
    }

    // Update interleaver for new modulation
    // Use data carriers (not total) to account for pilot overhead
    int carriers = (mode_ == protocol::WaveformMode::MC_DPSK) ? mc_dpsk_carriers_ : ofdm_data_carriers_;
    size_t bps = static_cast<size_t>(carriers) * getBitsPerSymbol(mod);
    interleaver_ = std::make_unique<ChannelInterleaver>(bps, v2::LDPC_CODEWORD_BITS);
    LOG_MODEM(INFO, "StreamingDecoder: interleaver updated for %s (%zu bits/symbol)",
              modulationToString(mod), bps);
}

void StreamingDecoder::setCodecType(fec::CodecType type) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (codec_type_ == type) return;
    codec_type_ = type;
    codec_ = fec::CodecFactory::create(type, code_rate_);
}

// ============================================================================
// STATUS
// ============================================================================

float StreamingDecoder::getBufferFillPercent() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    size_t used = std::min(total_fed_, buffer_capacity_samples_);
    return 100.0f * used / buffer_capacity_samples_;
}

DecoderStats StreamingDecoder::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

StreamingDecoder::FrameArrivalSnapshot StreamingDecoder::getFrameArrivalSnapshot() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    FrameArrivalSnapshot snapshot;
    snapshot.warm_sync_active = warm_sync_active_;
    snapshot.warm_sync_phase = warm_sync_phase_;
    snapshot.has_prediction = next_expected_frame_sample_valid_;
    snapshot.next_expected_frame_sample = next_expected_frame_sample_;
    snapshot.frame_arrival_confidence = frame_arrival_confidence_;
    snapshot.consecutive_sync_misses = consecutive_sync_misses_;
    snapshot.has_last_frame = last_frame_arrival_valid_;
    snapshot.last_frame_start_sample = last_frame_start_sample_;
    snapshot.last_frame_end_sample = last_frame_end_sample_;
    snapshot.has_last_arrival_error = last_frame_arrival_error_valid_;
    snapshot.last_arrival_error_samples = last_frame_arrival_error_samples_;
    snapshot.expected_frame_gap_samples = expected_frame_gap_samples_;
    return snapshot;
}

void StreamingDecoder::setExpectedFrameGapSamples(size_t samples) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    expected_frame_gap_samples_ = samples;
}

void StreamingDecoder::seedExpectedFrameArrivalAfterSamples(size_t delay_samples,
                                                            float confidence) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (!connected_ || mode_ != protocol::WaveformMode::OFDM_CHIRP) {
        return;
    }

    const auto previous_phase = warm_sync_phase_;
    warm_sync_active_ = true;
    warm_sync_phase_ = arrival_policy::WarmSyncPhase::WARM;
    next_expected_frame_sample_valid_ = true;
    next_expected_frame_sample_ = total_fed_ + delay_samples;
    frame_arrival_confidence_ =
        arrival_policy::clampConfidence(std::max(frame_arrival_confidence_, confidence));
    consecutive_sync_misses_ = 0;
    last_frame_arrival_error_valid_ = false;
    last_frame_arrival_error_samples_ = 0;

    LOG_MODEM(DEBUG,
              "[%s] warm-sync arrival seeded from local TX: now=%zu delay=%zu next=%zu confidence=%.2f",
              log_prefix_.c_str(), total_fed_, delay_samples,
              next_expected_frame_sample_, frame_arrival_confidence_);
    if (previous_phase != warm_sync_phase_) {
        LOG_MODEM(INFO, "[%s] warm-sync state: %s -> %s",
                  log_prefix_.c_str(),
                  arrival_policy::warmSyncPhaseName(previous_phase),
                  arrival_policy::warmSyncPhaseName(warm_sync_phase_));
    }
}

void StreamingDecoder::setAdaptiveShortDataPreamble(bool enable) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (adaptive_short_data_preamble_ == enable) {
        return;
    }
    adaptive_short_data_preamble_ = enable;
    LOG_MODEM(INFO, "StreamingDecoder: adaptive short data re-anchor %s",
              enable ? "ENABLED" : "DISABLED");
}

void StreamingDecoder::setCoherentOFDMControlProfileEnabled(bool enable) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (coherent_ofdm_control_profile_enabled_ == enable) {
        return;
    }
    coherent_ofdm_control_profile_enabled_ = enable;
    LOG_MODEM(INFO, "StreamingDecoder: coherent OFDM control profile %s",
              enable ? "ENABLED" : "DISABLED");
}

StreamingDecoder::DecoderConfig StreamingDecoder::getConfig() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    DecoderConfig cfg;
    cfg.mode = mode_;
    cfg.modulation = current_modulation_;
    cfg.code_rate = code_rate_;
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        cfg.num_carriers = mc_dpsk_config_.num_carriers;
        cfg.data_carriers = mc_dpsk_config_.num_carriers;
        cfg.bits_per_symbol = static_cast<int>(mcDpskBitsPerSymbol(mc_dpsk_config_));
    } else {
        cfg.num_carriers = ofdm_carriers_;
        cfg.data_carriers = ofdm_data_carriers_;
        cfg.bits_per_symbol = ofdm_data_carriers_ * static_cast<int>(getBitsPerSymbol(current_modulation_));
    }

    // Get pilot config from waveform (coherent modes use denser pilots)
    cfg.use_pilots = true;
    cfg.pilot_spacing = waveform_ ? waveform_->getPilotSpacing() : 10;

    // Interleaving settings
    cfg.use_channel_interleave = use_channel_interleave_;
    cfg.use_frame_interleave = protocol::isOFDMMode(mode_);

    return cfg;
}

size_t StreamingDecoder::samplesInBuffer() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return std::min(total_fed_, buffer_capacity_samples_);
}

bool StreamingDecoder::isSynced() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return state_ == DecoderState::SYNC_FOUND || state_ == DecoderState::DECODING
        || state_ == DecoderState::BURST_ACCUMULATING
        || state_ == DecoderState::MCDPSK_BURST_CONTINUING;
}

void StreamingDecoder::captureConstellationSnapshot() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (!waveform_) {
        return;
    }

    auto symbols = waveform_->getConstellationSymbols();
    if (!symbols.empty()) {
        constellation_cache_ = std::move(symbols);
        constellation_cache_time_ = std::chrono::steady_clock::now();
    }
}

std::vector<std::complex<float>> StreamingDecoder::getConstellationSymbols() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    auto now = std::chrono::steady_clock::now();

    if (waveform_) {
        auto symbols = waveform_->getConstellationSymbols();
        if (!symbols.empty()) {
            constellation_cache_ = symbols;
            constellation_cache_time_ = now;
        }
        static int log_count = 0;
        if (log_count < 10 && !symbols.empty()) {
            // Phase histogram to diagnose constellation display
            int bins[8] = {0}; // 45° bins: 0-45, 45-90, ..., 315-360
            float mag_sum = 0, mag_min = 1e9, mag_max = 0;
            for (const auto& s : symbols) {
                float phase = std::atan2(s.imag(), s.real()) * 180.0f / 3.14159265f;
                if (phase < 0) phase += 360.0f;
                int bin = static_cast<int>(phase / 45.0f) % 8;
                bins[bin]++;
                float mag = std::abs(s);
                mag_sum += mag;
                if (mag < mag_min) mag_min = mag;
                if (mag > mag_max) mag_max = mag;
            }
            float mag_avg = mag_sum / symbols.size();
            LOG_MODEM(INFO, "getConstellationSymbols: %zu symbols (mode=%d) mag=[%.3f,%.3f,%.3f] phase_hist: %d %d %d %d %d %d %d %d",
                      symbols.size(), static_cast<int>(mode_),
                      mag_min, mag_avg, mag_max,
                      bins[0], bins[1], bins[2], bins[3], bins[4], bins[5], bins[6], bins[7]);
            // Log first 10 symbols
            if (log_count < 2) {
                for (size_t i = 0; i < std::min(size_t(20), symbols.size()); i++) {
                    float ph = std::atan2(symbols[i].imag(), symbols[i].real()) * 180.0f / 3.14159265f;
                    LOG_MODEM(INFO, "  sym[%zu]: (%.4f, %.4f) mag=%.4f phase=%.1f°",
                              i, symbols[i].real(), symbols[i].imag(), std::abs(symbols[i]), ph);
                }
            }
            log_count++;
        }
        if (!symbols.empty()) {
            return symbols;
        }
    }

    // Hold last non-empty constellation briefly so GUI doesn't flicker to empty
    // during control-profile reconfiguration between frames.
    if (!constellation_cache_.empty()) {
        auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - constellation_cache_time_).count();
        if (age_ms <= CONSTELLATION_CACHE_HOLD_MS) {
            return constellation_cache_;
        }
    }
    return {};
}

Modulation StreamingDecoder::getConstellationModulation() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (waveform_) {
        return waveform_->getConstellationModulation();
    }
    return Modulation::QPSK;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void StreamingDecoder::reset() {
    // Increment generation BEFORE acquiring lock - any ongoing search will see new value
    reset_generation_.fetch_add(1);

    std::lock_guard<std::mutex> lock(buffer_mutex_);

    write_pos_ = 0;
    correlation_pos_ = 0;
    sync_position_ = 0;
    sync_correlation_ = 0.0f;
    sync_gap_error_samples_ = 0.0f;
    samples_since_sync_ = 0;
    total_fed_ = 0;
    feed_iter_ = 0;
    overflow_events_ = 0;
    sync_reject_streak_ = 0;
    expect_full_ofdm_anchor_ = false;
    sync_from_warm_timed_window_ = false;
    resetFrameArrivalTrackingLocked();
    state_ = DecoderState::SEARCHING;
    pending_total_cw_ = 0;
    burst_blocks_decoded_ = 0;
    burst_soft_buffer_.clear();
    burst_metric_templates_.clear();
    mc_burst_pending_frame_ = false;
    mc_burst_pending_soft_bits_.clear();
    constellation_cache_.clear();
    constellation_cache_time_ = std::chrono::steady_clock::time_point{};
    use_burst_interleave_ = false;
    new_data_available_ = false;
    last_decoded_sync_pos_ = SIZE_MAX;
    search_floor_abs_ = 0;
    search_floor_abs_valid_ = false;

    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    if (waveform_) waveform_->reset();
    idle_noise_snr_estimator_.reset();

    {
        std::lock_guard<std::mutex> qlock(queue_mutex_);
        while (!frame_queue_.empty()) frame_queue_.pop();
    }

    {
        std::lock_guard<std::mutex> slock(stats_mutex_);
        stats_ = DecoderStats{};
    }

    noise_floor_ = 0.001f;
    last_snr_.store(0.0f);
    last_ofdm_broadband_snr_db_valid_.store(false);
    last_ofdm_broadband_snr_db_.store(0.0f);
    last_cfo_.store(0.0f);
    last_fading_index_.store(0.0f);
}

void StreamingDecoder::stop() {
    shutdown_.store(true);
    data_cv_.notify_all();
}

} // namespace gui
} // namespace ultra
