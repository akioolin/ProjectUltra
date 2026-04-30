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
#include "gui/startup_trace.hpp"
#include "waveform/ofdm_cox_waveform.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
#include "fec/frame_interleaver.hpp"  // Frame-level interleaving for 4-CW frames
#include "fec/burst_interleaver.hpp"  // Burst-level long interleaver (4-frame groups)
#include "ultra/fec.hpp"              // LDPCDecoder for robust single-CW decode
#include "fec/ldpc_codec.hpp"         // getRecommendedIterations
#include "ultra/logging.hpp"
#include "ultra/timing_profiler.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>

namespace ultra {
namespace gui {

namespace v2 = protocol::v2;

namespace {

// Return a conservative 1-CW control-frame sample requirement for connected OFDM.
// If data profile is high-order, the robust control profile (DQPSK R1/4) may need
// more symbols than the current data profile.
size_t getOFDMControlFrameSamples(IWaveform* waveform, Modulation data_mod, CodeRate data_rate) {
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

    constexpr int pilot_spacing = 10;  // Differential DQPSK control profile
    const int pilot_count = (carriers + pilot_spacing - 1) / pilot_spacing;
    const int data_carriers = std::max(1, carriers - pilot_count);
    const int bits_per_symbol = data_carriers * 2;  // DQPSK
    const int data_symbols = (v2::LDPC_CODEWORD_BITS + bits_per_symbol - 1) / bits_per_symbol;
    const size_t robust_samples = static_cast<size_t>(2 + data_symbols) *
                                  static_cast<size_t>(samples_per_symbol);

    return std::max(default_samples, robust_samples);
}

}  // namespace

// ============================================================================
// DEBUG: Buffer snapshot for external analysis
// ============================================================================
// Dumps buffer contents to .f32 files at key sample counts
// Use: sox -t f32 -r 48000 -c 1 snapshot_*.f32 snapshot_*.wav
// Or: audacity can import raw 32-bit float

static bool g_debug_dumps_enabled = false;  // Set to true for debugging
static const char* g_dump_prefix = "/tmp/sd_debug";  // Dump file prefix

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

StreamingDecoder::StreamingDecoder() {
    startupTrace("StreamingDecoder", "ctor-enter");
    buffer_.resize(MAX_BUFFER_SAMPLES, 0.0f);
    startupTrace("StreamingDecoder", "buffer-resized");
    waveform_ = WaveformFactory::create(protocol::WaveformMode::MC_DPSK);
    startupTrace("StreamingDecoder", "waveform-created");
    interleaver_ = std::make_unique<ChannelInterleaver>(16, v2::LDPC_CODEWORD_BITS);
    startupTrace("StreamingDecoder", "interleaver-created");
    codec_ = fec::CodecFactory::create(fec::CodecType::LDPC, CodeRate::R1_4);
    startupTrace("StreamingDecoder", "codec-created");

    LOG_MODEM(INFO, "StreamingDecoder: Initialized (buffer=%zu samples)", MAX_BUFFER_SAMPLES);
    startupTrace("StreamingDecoder", "ctor-exit");
}

StreamingDecoder::~StreamingDecoder() {
    stop();
}

void StreamingDecoder::setBurstInterleaveGroupSize(int size) {
    burst_group_size_ = std::clamp(size, 2, 8);
}

// ============================================================================
// AUDIO THREAD - Just buffer samples, nothing else
// ============================================================================

void StreamingDecoder::feedAudio(const float* samples, size_t count) {
    if (!samples || count == 0) return;

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

    // Check for buffer overflow - would we overwrite unsearched data?
    // Calculate how much unsearched data we have
    size_t unsearched;
    if (write_pos_ >= correlation_pos_) {
        unsearched = write_pos_ - correlation_pos_;
    } else {
        unsearched = MAX_BUFFER_SAMPLES - correlation_pos_ + write_pos_;
    }

    // Guard against pointer drift. If correlation_pos_ gets ahead of write_pos_
    // in wrapped space, unsearched can appear "almost full" and trigger false
    // overflow storms. Snap search pointer to newest audio in that case.
    if (total_fed_ >= MAX_BUFFER_SAMPLES &&
        unsearched > (MAX_BUFFER_SAMPLES - CORR_INVARIANT_GUARD)) {
        correlation_pos_ = write_pos_;
        setSearchFloorLocked(total_fed_);
        unsearched = 0;
        LOG_MODEM(WARN, "[%s] Correlation pointer drift detected, resetting search cursor",
                  log_prefix_.c_str());
    }

    // If adding these samples would overflow, drop backlog aggressively enough
    // to recover in one step. Small drops (~1k) cause repeated overflow storms.
    if (unsearched + count >= MAX_BUFFER_SAMPLES) {
        const size_t incoming_total = unsearched + count;
        const size_t target_after_write = std::min(OVERFLOW_RECOVERY_KEEP, MAX_BUFFER_SAMPLES - 1);
        size_t need_to_drop = 0;
        if (incoming_total > target_after_write) {
            need_to_drop = std::min(unsearched, incoming_total - target_after_write);
        }

        correlation_pos_ = (correlation_pos_ + need_to_drop) % MAX_BUFFER_SAMPLES;
        setSearchFloorLocked(ringPosToAbsoluteLocked(correlation_pos_));

        // Once overloaded, any in-flight frame context is stale. Force a clean
        // resync from current audio instead of chasing old sync positions.
        bool reset_decode_state = false;
        if (state_ != DecoderState::SEARCHING) {
            state_ = DecoderState::SEARCHING;
            pending_total_cw_ = 0;
            burst_blocks_decoded_ = 0;
            burst_soft_buffer_.clear();
            reset_decode_state = true;
        }

        overflow_events_++;
        {
            std::lock_guard<std::mutex> slock(stats_mutex_);
            stats_.buffer_overflows = overflow_events_;
            stats_.overflow_samples_dropped += need_to_drop;
            if (reset_decode_state) {
                stats_.overflow_state_resets++;
            }
        }
        if (overflow_events_ <= 3 || (overflow_events_ % 25) == 0) {
            LOG_MODEM(WARN, "[%s] Buffer overflow, dropped %zu unsearched samples (corr_pos=%zu, keep=%zu, state_reset=%d, total=%llu)",
                      log_prefix_.c_str(), need_to_drop, correlation_pos_, target_after_write,
                      reset_decode_state ? 1 : 0,
                      static_cast<unsigned long long>(overflow_events_));
        }
    }

    // Write samples to circular buffer
    for (size_t i = 0; i < count; i++) {
        buffer_[write_pos_] = samples[i];
        write_pos_ = (write_pos_ + 1) % MAX_BUFFER_SAMPLES;
    }

    total_fed_ += count;

    // Update backlog telemetry for UI/CLI diagnostics.
    size_t unsearched_after;
    if (write_pos_ >= correlation_pos_) {
        unsearched_after = write_pos_ - correlation_pos_;
    } else {
        unsearched_after = MAX_BUFFER_SAMPLES - correlation_pos_ + write_pos_;
    }
    float backlog_ms = (static_cast<float>(unsearched_after) * 1000.0f) / 48000.0f;
    size_t used_samples = std::min(total_fed_, MAX_BUFFER_SAMPLES);
    float fill_pct = 100.0f * static_cast<float>(used_samples) /
                     static_cast<float>(MAX_BUFFER_SAMPLES);
    {
        std::lock_guard<std::mutex> slock(stats_mutex_);
        stats_.current_unsearched_samples = unsearched_after;
        stats_.peak_unsearched_samples = std::max<uint64_t>(stats_.peak_unsearched_samples,
                                                            static_cast<uint64_t>(unsearched_after));
        stats_.backlog_ms = backlog_ms;
        stats_.peak_backlog_ms = std::max(stats_.peak_backlog_ms, backlog_ms);
        stats_.buffer_fill_percent = fill_pct;
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
    }
}

float StreamingDecoder::applyCFOPreCorrection(std::vector<float>& samples, float cfo_hz,
                                               size_t absolute_start_sample) {
    if (std::abs(cfo_hz) < 0.05f || samples.empty()) {
        pre_correction_cfo_ = 0.0f;
        return 0.0f;  // Skip if negligible
    }

    // Convert to analytic signal (real + j*Hilbert) for proper frequency shift
    HilbertTransform hilbert(65);
    auto analytic = hilbert.process(SampleSpan(samples.data(), samples.size()));

    // Apply CFO correction: multiply analytic signal by exp(-j*2π*CFO*t)
    // Phase formula matches toBaseband(): phase_inc = -2π × CFO / fs per sample
    const float fs = 48000.0f;
    float phase_inc = -2.0f * static_cast<float>(M_PI) * cfo_hz / fs;
    float phase = -2.0f * static_cast<float>(M_PI) * cfo_hz *
                  static_cast<float>(absolute_start_sample) / fs;
    // Wrap to [-π, π]
    phase = std::fmod(phase, 2.0f * static_cast<float>(M_PI));
    if (phase > static_cast<float>(M_PI)) phase -= 2.0f * static_cast<float>(M_PI);
    if (phase < -static_cast<float>(M_PI)) phase += 2.0f * static_cast<float>(M_PI);

    // Apply rotation and take real part
    size_t len = std::min(samples.size(), analytic.size());
    for (size_t i = 0; i < len; i++) {
        Complex correction(std::cos(phase), std::sin(phase));
        samples[i] = (analytic[i] * correction).real();
        phase += phase_inc;
        if (phase > static_cast<float>(M_PI)) phase -= 2.0f * static_cast<float>(M_PI);
        else if (phase < -static_cast<float>(M_PI)) phase += 2.0f * static_cast<float>(M_PI);
    }

    pre_correction_cfo_ = cfo_hz;
    LOG_MODEM(DEBUG, "[%s] CFO pre-correction: %.2f Hz, abs_start=%zu, %zu samples",
              log_prefix_.c_str(), cfo_hz, absolute_start_sample, len);
    return cfo_hz;
}

size_t StreamingDecoder::ringPosToAbsoluteLocked(size_t ring_pos) const {
    if (total_fed_ < MAX_BUFFER_SAMPLES) {
        return ring_pos;
    }

    const size_t oldest_abs = total_fed_ - MAX_BUFFER_SAMPLES;
    const size_t oldest_pos = write_pos_;
    const size_t offset = (ring_pos >= oldest_pos)
        ? (ring_pos - oldest_pos)
        : (MAX_BUFFER_SAMPLES - oldest_pos + ring_pos);
    return oldest_abs + offset;
}

size_t StreamingDecoder::absoluteToRingLocked(size_t abs_pos) const {
    if (total_fed_ < MAX_BUFFER_SAMPLES) {
        return std::min(abs_pos, total_fed_) % MAX_BUFFER_SAMPLES;
    }

    const size_t oldest_abs = total_fed_ - MAX_BUFFER_SAMPLES;
    abs_pos = std::clamp(abs_pos, oldest_abs, total_fed_);
    return (write_pos_ + (abs_pos - oldest_abs)) % MAX_BUFFER_SAMPLES;
}

void StreamingDecoder::setSearchFloorLocked(size_t abs_pos) {
    const size_t oldest_abs = (total_fed_ > MAX_BUFFER_SAMPLES)
        ? (total_fed_ - MAX_BUFFER_SAMPLES)
        : 0;
    search_floor_abs_ = std::clamp(abs_pos, oldest_abs, total_fed_);
    search_floor_abs_valid_ = true;
}

void StreamingDecoder::searchForSync() {
    if (!waveform_) return;

    // Save generation counter - if reset() is called during our search,
    // we'll detect it and discard our results
    uint32_t gen_at_start = reset_generation_.load();

    // Get preamble size from waveform
    size_t preamble = static_cast<size_t>(waveform_->getPreambleSamples());

    // Search buffer sizing depends on sync mode:
    // - Chirp sync (disconnected): needs ~120k samples for dual chirp correlation
    // - Light sync (connected, LTS): needs only ~24k samples (LTS is ~1024 samples)
    // Using a smaller buffer when connected cuts per-hop latency from ~2.8s to <1s
    constexpr size_t CHIRP_MAX_SEARCH = 120000;   // ~2.5s for dual chirp detection
    constexpr size_t LIGHT_SEARCH_SIZE = 9600;    // ~0.20s for connected LTS-only detection

    size_t chirp_min_search = std::min(preamble + 65000, CHIRP_MAX_SEARCH);
    bool use_light_search = connected_ && waveform_->supportsDataPreamble();
    size_t min_search = use_light_search ? LIGHT_SEARCH_SIZE : chirp_min_search;

    std::vector<float> search_buffer;
    size_t search_start;

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        float audio_sec = total_fed_ / 48000.0f;

        // Need minimum samples before we can search
        if (total_fed_ < min_search) {
            static int skip_count = 0;
            if (++skip_count % 50 == 1)
                LOG_MODEM(INFO, "[%s] searchForSync: SKIP not enough samples, total=%.2fs, need=%.2fs",
                          log_prefix_.c_str(), audio_sec, min_search / 48000.0f);
            return;
        }

        // Initialize correlation_pos_ if needed
        if (correlation_pos_ == 0 && total_fed_ > 0) {
            if (total_fed_ < MAX_BUFFER_SAMPLES) {
                correlation_pos_ = 0;
            } else {
                correlation_pos_ = write_pos_;
            }
        }

        // Calculate unsearched data available
        size_t unsearched;
        if (write_pos_ >= correlation_pos_) {
            unsearched = write_pos_ - correlation_pos_;
        } else {
            unsearched = MAX_BUFFER_SAMPLES - correlation_pos_ + write_pos_;
        }

        // Need at least min_search unsearched samples
        if (unsearched < min_search) {
            static int skip_count2 = 0;
            if (++skip_count2 % 50 == 1)
                LOG_MODEM(INFO, "[%s] searchForSync: SKIP unsearched=%zu < min=%zu, total=%.2fs, corr_pos=%zu",
                          log_prefix_.c_str(), unsearched, min_search, audio_sec, correlation_pos_);
            return;
        }

        // Quick RMS check for signal presence
        float rms = 0.0f;
        for (size_t i = 0; i < 1000; i++) {
            float s = buffer_[(correlation_pos_ + i) % MAX_BUFFER_SAMPLES];
            rms += s * s;
        }
        rms = std::sqrt(rms / 1000.0f);

        // OTA-connected mode can run at lower absolute amplitudes than simulator
        // defaults. Use an adaptive gate so valid low-level frames are not skipped.
        float rms_gate = CORR_NOISE_THRESHOLD;
        if (connected_ && waveform_->supportsDataPreamble()) {
            float noise_floor = std::max(0.001f, noise_floor_);
            if (rms < noise_floor * 3.0f) {
                noise_floor_ = 0.98f * noise_floor + 0.02f * rms;
            } else {
                noise_floor_ = 0.995f * noise_floor + 0.005f * rms;
            }

            // Typical OTA values observed around 0.02-0.04 RMS; keep floor low enough
            // to avoid starving detectDataSync() while still skipping true silence.
            rms_gate = std::clamp(noise_floor_ * 2.2f, 0.015f, 0.040f);
            if (sync_reject_streak_ >= 8) {
                float relax = std::min(0.010f,
                                       0.001f * static_cast<float>(sync_reject_streak_ - 7));
                rms_gate = std::max(0.012f, rms_gate - relax);
            }
        }

        if (rms < rms_gate) {
            // No signal - advance by small step (100ms = 4800 samples)
            static int rms_skip_count = 0;
            if (++rms_skip_count % 10 == 1)
                LOG_MODEM(INFO, "[%s] searchForSync: RMS skip, rms=%.4f < %.3f, corr_pos=%zu, total=%.2fs",
                          log_prefix_.c_str(), rms, rms_gate, correlation_pos_, audio_sec);
            correlation_pos_ = (correlation_pos_ + CORRELATION_STEP) % MAX_BUFFER_SAMPLES;
            return;
        }

        // Signal detected - log before running correlation (only occasionally to reduce spam)
        static int run_log_count = 0;
        if (++run_log_count % 10 == 1) {
            LOG_MODEM(INFO, "[%s] searchForSync: RUNNING correlation, rms=%.4f, corr_pos=%zu, total=%.2fs",
                      log_prefix_.c_str(), rms, correlation_pos_, audio_sec);
        }

        // Signal present - back up search start to catch chirp that might have started
        // in the lead-in silence. The TX lead-in is ~150ms (7200 samples), so we should
        // back up at least that much to ensure the chirp START is in our search window.
        // FIX: We may have skipped past the chirp start during low-RMS phases.
        constexpr size_t SEARCH_BACKTRACK = 9600; // Back up slightly more than lead-in

        if (correlation_pos_ >= SEARCH_BACKTRACK) {
            search_start = correlation_pos_ - SEARCH_BACKTRACK;
        } else if (total_fed_ < MAX_BUFFER_SAMPLES) {
            // Buffer hasn't wrapped yet, start from beginning
            search_start = 0;
        } else {
            // Buffer wrapped, handle underflow
            search_start = (MAX_BUFFER_SAMPLES + correlation_pos_ - SEARCH_BACKTRACK) % MAX_BUFFER_SAMPLES;
        }

        // Do not let the backtrack window re-enter audio that a previous decode
        // already consumed. On sustained OFDM ACK traffic, searching the tail of a
        // just-decoded 1-CW control frame can find false LTS-like peaks; those
        // false locks then escalate into expensive 4-CW LDPC attempts and delay
        // real ACKs long enough to trigger ARQ retransmission storms.
        if (search_floor_abs_valid_) {
            const size_t oldest_abs = (total_fed_ > MAX_BUFFER_SAMPLES)
                ? (total_fed_ - MAX_BUFFER_SAMPLES)
                : 0;
            if (search_floor_abs_ < oldest_abs) {
                search_floor_abs_ = oldest_abs;
            }
            if (search_floor_abs_ > total_fed_) {
                search_floor_abs_ = total_fed_;
            }

            size_t search_start_abs = ringPosToAbsoluteLocked(search_start);
            if (search_start_abs < search_floor_abs_) {
                if (total_fed_ - search_floor_abs_ < min_search) {
                    static int floor_wait_count = 0;
                    if (++floor_wait_count % 50 == 1) {
                        LOG_MODEM(INFO,
                                  "[%s] searchForSync: SKIP post-frame floor, available=%zu < min=%zu",
                                  log_prefix_.c_str(), total_fed_ - search_floor_abs_, min_search);
                    }
                    return;
                }
                search_start = absoluteToRingLocked(search_floor_abs_);
            }
        }

        search_buffer.resize(min_search);
        for (size_t i = 0; i < min_search; i++) {
            search_buffer[i] = buffer_[(search_start + i) % MAX_BUFFER_SAMPLES];
        }

        // Advance by small step (100ms = 4800 samples) for accurate detection
        correlation_pos_ = (correlation_pos_ + CORRELATION_STEP) % MAX_BUFFER_SAMPLES;
    }

    // DEBUG: Dump the search buffer on first few searches
    static int search_dump_count = 0;
    if (g_debug_dumps_enabled && search_dump_count < 5) {
        char label[64];
        snprintf(label, sizeof(label), "search_%d_pos%zu", search_dump_count, search_start);

        // Dump search buffer to file
        char filename[256];
        snprintf(filename, sizeof(filename), "%s_%s.f32", g_dump_prefix, label);
        std::ofstream file(filename, std::ios::binary);
        if (file) {
            file.write(reinterpret_cast<const char*>(search_buffer.data()),
                       search_buffer.size() * sizeof(float));
            file.close();

            // Compute stats
            float rms = 0, max_val = 0;
            for (size_t i = 0; i < std::min(search_buffer.size(), size_t(10000)); i++) {
                rms += search_buffer[i] * search_buffer[i];
                max_val = std::max(max_val, std::abs(search_buffer[i]));
            }
            rms = std::sqrt(rms / std::min(search_buffer.size(), size_t(10000)));

            LOG_MODEM(DEBUG, "StreamingDecoder: Search #%d: dumped %zu samples to %s (start=%zu, RMS=%.4f, peak=%.4f)",
                      search_dump_count, search_buffer.size(), filename, search_start, rms, max_val);
        }
        search_dump_count++;
    }

    // Search for sync (no lock held - this is the slow part)
    auto search_start_time = std::chrono::steady_clock::now();

    waveform_->reset();
    SyncResult sync_result;
    bool found = false;

    // When connected, use light sync only (LTS training symbols, no chirp).
    // TX sends LTS-only preamble when connected — chirp fallback can NEVER work
    // because there is no chirp in the signal. Reject false positives where data
    // autocorrelation produces spurious peaks (observed up to 0.63). Real LTS
    // correlation is always >0.81 even on moderate fading.
    // Coherent modes need higher sync quality — badly-synced frames always fail
    // because stale LTS phases can't be recovered by DD tracking alone.
    const bool is_coherent = (current_modulation_ == Modulation::QPSK ||
                              current_modulation_ == Modulation::BPSK);
    const float fading_hint = last_fading_index_.load();
    const float snr_hint = last_snr_.load();
    // Narrowband LTS has ~35% of wideband energy (21 vs 59 carriers) → lower correlation peak
    bool is_narrowband = (mode_ == protocol::WaveformMode::OFDM_NARROW);
    // LTS sync thresholds.
    // True LTS peaks: 0.85-0.99 (clean), but Moderate SNR12 hardware traces
    // show real tail DATA can dip to ~0.52-0.56. Data autocorrelation noise is
    // usually 0.20-0.45; candidates admitted near the low end still pass the
    // downstream LLR/LDPC gates before they can be accepted as frames.
    float light_sync_min_confidence;
    float weak_sync_floor;
    if (is_coherent) {
        light_sync_min_confidence = 0.90f;
        weak_sync_floor = 0.85f;
    } else if (is_narrowband) {
        light_sync_min_confidence = 0.50f;
        weak_sync_floor = 0.40f;
    } else if (connected_) {
        light_sync_min_confidence = 0.52f;
        weak_sync_floor = 0.45f;
    } else {
        light_sync_min_confidence = 0.65f;
        weak_sync_floor = 0.55f;
    }

    if (!is_coherent && connected_ && sync_reject_streak_ >= 5) {
        float extra_relax = std::min(0.10f,
                                     0.02f * static_cast<float>(sync_reject_streak_ - 4));
        light_sync_min_confidence = std::max(0.45f, light_sync_min_confidence - extra_relax);
    }

    if (connected_ && waveform_->supportsDataPreamble()) {
        float known_cfo = last_cfo_.load();
        found = waveform_->detectDataSync(
            SampleSpan(search_buffer.data(), search_buffer.size()),
            sync_result, known_cfo, CORR_DETECT_THRESHOLD);

        // Reject clear false positives (noise floor is ~0.2-0.4)
        bool weak_accept = false;
        if (found && sync_result.correlation < light_sync_min_confidence) {
            bool allow_weak_accept = !is_coherent &&
                                     connected_ &&
                                     sync_reject_streak_ >= 3 &&
                                     sync_result.correlation >= std::max(weak_sync_floor,
                                                                          light_sync_min_confidence - 0.08f);
            if (allow_weak_accept) {
                weak_accept = true;
                LOG_MODEM(INFO, "[%s] DATA sync weak-accepted (corr=%.2f < %.2f, streak=%llu)",
                          log_prefix_.c_str(), sync_result.correlation, light_sync_min_confidence,
                          static_cast<unsigned long long>(sync_reject_streak_));
            } else {
                sync_reject_streak_++;
                LOG_MODEM(INFO, "[%s] DATA sync rejected (corr=%.2f < %.2f, streak=%llu)",
                          log_prefix_.c_str(), sync_result.correlation, light_sync_min_confidence,
                          static_cast<unsigned long long>(sync_reject_streak_));
                found = false;
            }
        }

        if (found) {
            if (weak_accept) {
                sync_reject_streak_ = std::max<uint64_t>(1, sync_reject_streak_ / 2);
            } else {
                sync_reject_streak_ = 0;
            }
            LOG_MODEM(INFO, "[%s] DATA sync detected (training only, known CFO=%.1f Hz, corr=%.2f)",
                      log_prefix_.c_str(), known_cfo, sync_result.correlation);
        }
        // No chirp fallback — TX sends LTS only when connected, chirp won't be found
    } else {
        // Use full sync detection with chirp (wideband)
        found = waveform_->detectSync(
            SampleSpan(search_buffer.data(), search_buffer.size()),
            sync_result, CORR_DETECT_THRESHOLD);

        // Dual-listen: if wideband didn't find anything, try narrowband chirp
        if (!found && !connected_) {
            // Lazy-init narrowband waveform on first use
            if (!narrow_waveform_initialized_) {
                // Create narrowband MC-DPSK waveform for chirp detection
                // Uses 1250-1750 Hz chirp, 4 carriers @ 1300-1700 Hz
                narrow_waveform_ = WaveformFactory::createNarrowbandMCDPSK();
                narrow_waveform_initialized_ = true;
                LOG_MODEM(INFO, "[%s] Dual-listen: narrowband MC-DPSK waveform initialized",
                          log_prefix_.c_str());
            }

            if (narrow_waveform_) {
                SyncResult narrow_result;
                bool narrow_found = narrow_waveform_->detectSync(
                    SampleSpan(search_buffer.data(), search_buffer.size()),
                    narrow_result, CORR_DETECT_THRESHOLD);

                if (narrow_found) {
                    found = true;
                    sync_result = narrow_result;
                    detected_bandwidth_ = BandwidthMode::NARROW;
                    // Switch main waveform to narrowband MC-DPSK so CONNECT frames decode correctly
                    waveform_ = WaveformFactory::createNarrowbandMCDPSK();
                    LOG_MODEM(INFO, "[%s] Dual-listen: NARROWBAND chirp detected! corr=%.3f, CFO=%.1f Hz, switched to narrowband MC-DPSK",
                              log_prefix_.c_str(), narrow_result.correlation, narrow_result.cfo_hz);
                }
            }
        }

        // If wideband found something, mark as wide
        if (found && detected_bandwidth_ != BandwidthMode::NARROW) {
            detected_bandwidth_ = BandwidthMode::WIDE;
        }
    }

    auto search_end_time = std::chrono::steady_clock::now();
    float search_ms = std::chrono::duration<float, std::milli>(search_end_time - search_start_time).count();

    // Check if reset() was called during our search - if so, discard results
    if (reset_generation_.load() != gen_at_start) {
        LOG_MODEM(INFO, "[%s] searchForSync: ABORTED - reset() called during search", log_prefix_.c_str());
        return;
    }

    // Log timing: total_fed_ tells us how much audio has arrived
    float audio_sec = total_fed_ / 48000.0f;
    if (found || search_ms > 100) {  // Log if found or if search was slow
        LOG_MODEM(INFO, "[%s] searchForSync: audio=%.2fs, search=%.1fms, found=%d, corr=%.3f",
                  log_prefix_.c_str(), audio_sec, search_ms, found ? 1 : 0, sync_result.correlation);
    }

    if (found) {
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        sync_position_ = (search_start + sync_result.start_sample) % MAX_BUFFER_SAMPLES;

        // Convert ring-buffer index to absolute sample index.
        // Needed so waveform CFO phase starts from true stream time, not local window offset.
        auto ringPosToAbsolute = [this](size_t ring_pos) -> size_t {
            if (total_fed_ < MAX_BUFFER_SAMPLES) {
                // Buffer has not wrapped yet: ring index == absolute sample index.
                return ring_pos;
            }

            const size_t oldest_abs = total_fed_ - MAX_BUFFER_SAMPLES;
            const size_t oldest_pos = write_pos_;  // write_pos_ points to oldest sample after wrap
            const size_t offset = (ring_pos >= oldest_pos)
                ? (ring_pos - oldest_pos)
                : (MAX_BUFFER_SAMPLES - oldest_pos + ring_pos);
            return oldest_abs + offset;
        };

        // Anti-replay: reject sync at same position as last decoded frame (circular distance)
        if (last_decoded_sync_pos_ != SIZE_MAX) {
            size_t d1 = (sync_position_ >= last_decoded_sync_pos_)
                ? (sync_position_ - last_decoded_sync_pos_)
                : (MAX_BUFFER_SAMPLES - last_decoded_sync_pos_ + sync_position_);
            size_t dist = std::min(d1, MAX_BUFFER_SAMPLES - d1);
            if (dist < 200) {
                LOG_MODEM(INFO, "[%s] Anti-replay: duplicate sync at pos=%zu (prev=%zu), skipping",
                          log_prefix_.c_str(), sync_position_, last_decoded_sync_pos_);
                constexpr size_t SEARCH_BACKTRACK = 9600;
                correlation_pos_ = (sync_position_ + SEARCH_BACKTRACK + CORRELATION_STEP) % MAX_BUFFER_SAMPLES;
                return;
            }
        }

        // Provide absolute training position to waveform so initial CFO phase is aligned.
        if (waveform_) {
            const size_t abs_training_pos = ringPosToAbsolute(sync_position_);
            waveform_->setAbsoluteTrainingPosition(abs_training_pos);
        }

        // CFO handling: On fading channels, chirp-based CFO measurement can be corrupted
        // by multipath (peaks shift differently for up vs down chirp).
        // When connected, trust the established CFO and limit drift.
        float new_cfo = sync_result.cfo_hz;
        float known_cfo = last_cfo_.load();

        if (connected_ && std::abs(known_cfo) > 0.01f) {
            // Limit CFO change to ±1 Hz per frame (oscillator drift is slow)
            constexpr float MAX_CFO_DRIFT_HZ = 1.0f;
            float cfo_diff = new_cfo - known_cfo;
            if (std::abs(cfo_diff) > MAX_CFO_DRIFT_HZ) {
                LOG_MODEM(INFO, "[%s] CFO sanity: measured=%.1f, known=%.1f, diff=%.1f > %.1f, using known",
                          log_prefix_.c_str(), new_cfo, known_cfo, cfo_diff, MAX_CFO_DRIFT_HZ);
                new_cfo = known_cfo;  // Trust established CFO over noisy measurement
            }
        }

        sync_cfo_ = new_cfo;
        sync_snr_ = estimateSNRFromChirp(sync_result.correlation, noise_floor_);
        sync_correlation_ = sync_result.correlation;
        sync_start_time_ = std::chrono::steady_clock::now();
        pending_total_cw_ = 0;

        state_ = DecoderState::SYNC_FOUND;

        last_snr_.store(sync_snr_);
        last_cfo_.store(sync_cfo_);

        LOG_MODEM(INFO, "[%s] SYNC at pos=%zu, CFO=%.1f Hz, SNR=%.1f dB",
                  log_prefix_.c_str(), sync_position_, sync_cfo_, sync_snr_);

        // NOTE: Do NOT advance correlation_pos_ past the frame here.
        // It was already advanced by CORRELATION_STEP at line 323 during search.
        // The post-decode skip at decodeCurrentFrame() line 718 handles advancing
        // past the decoded frame when we return to SEARCHING.
        //
        // Previously, this code jumped correlation_pos_ past the entire frame,
        // which could place it AHEAD of write_pos_ in circular buffer space
        // (especially after buffer wraps). This caused feedAudio()'s overflow
        // check to compute unsearched ≈ buffer_size, triggering spurious
        // buffer overflows and data loss during async decode.
    }
}

void StreamingDecoder::checkIfReadyToDecode() {
    if (!waveform_) {
        state_ = DecoderState::SEARCHING;
        return;
    }

    // How many samples do we have from sync position?
    size_t available;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        if (write_pos_ >= sync_position_) {
            available = write_pos_ - sync_position_;
        } else {
            available = MAX_BUFFER_SAMPLES - sync_position_ + write_pos_;
        }
    }

    // Check timeout
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - sync_start_time_).count();

    if (elapsed > FRAME_TIMEOUT_MS) {
        LOG_MODEM(WARN, "[%s] Frame timeout after %lld ms", log_prefix_.c_str(), (long long)elapsed);
        state_ = DecoderState::SEARCHING;
        return;
    }

    // Calculate how much we need — must match decodeCurrentFrame() buffer sizing
    bool is_ofdm_here = protocol::isOFDMMode(mode_);
    size_t needed;
    if (pending_total_cw_ > 0) {
        // We know exact CW count from CW0 peek — get exact sample count
        needed = static_cast<size_t>(waveform_->getMinSamplesForCWCount(pending_total_cw_));
    } else if (is_ofdm_here && connected_) {
        // Burst-interleaved frames MUST use full 4-CW buffer (marker is one-shot consumed by process())
        bool burst_latched = use_burst_interleave_ && waveform_ && waveform_->wasBurstInterleaved();
        if (!burst_latched) {
            // 1-CW peek first — escalate to 4-CW if CW0 indicates data frame
            needed = getOFDMControlFrameSamples(waveform_.get(), current_modulation_, code_rate_);
        } else {
            needed = static_cast<size_t>(waveform_->getMinSamplesForFrame());
        }
    } else {
        // MC-DPSK or disconnected: 1-CW minimum for header peek
        needed = static_cast<size_t>(waveform_->getMinSamplesForControlFrame());
    }

    if (available >= needed) {
        state_ = DecoderState::DECODING;
    }
}

// Observability counters for robust decode paths (check via debugger or periodic log)
static std::atomic<int> g_robust_retry_hits{0};   // CW0 peek: retry succeeded after initial fail
static std::atomic<int> g_salvage_hits{0};         // 1-CW control salvaged from 4-CW path

// LLR pre-screen for 1-CW decode probes. Returns the mean |LLR| across the
// first cw_size soft bits. Hardware Good-fading traces showed real ACKs can
// dip to |llr|_avg ~= 1.8, while obvious false locks cluster around <= 1.0.
// Keep this gate looser than the 4-CW data escalation gate so control frames
// get a chance to use their R1/4 LDPC protection under fades.
//
// Used at the control-first ACK path and CW0 peek call sites — see
// streaming_decoder.cpp:1130-area and :1294-area. Skipping the LDPC call
// when avg < threshold saves ~85ms per false sync (the decoder's 1 +
// max_retries iteration sweep).
static float computeLLRAbsAvg(const float* bits, size_t count) {
    if (count == 0) return 0.0f;
    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i) sum += std::abs(bits[i]);
    return sum / static_cast<float>(count);
}

// Threshold tuned from Mac<->Pi hardware Good-fading traces: 2.0 skipped real
// tail ACKs at |LLR|~=1.8 and caused deterministic ARQ timeouts. 1.5 still
// avoids the very low-confidence false locks that caused decode backlog.
static constexpr float MIN_LLR_FOR_1CW_DECODE = 1.5f;

static bool hasInvalidOFDMTraining(IWaveform* waveform, bool is_ofdm, bool connected,
                                   float& lts_signal_power, float& lts_channel_mag) {
    lts_signal_power = 1.0f;
    lts_channel_mag = 1.0f;
    if (!waveform || !is_ofdm || !connected) {
        return false;
    }

    lts_signal_power = waveform->getLastLTSSignalPower();
    lts_channel_mag = waveform->getLastLTSChannelMagnitude();
    if (!std::isfinite(lts_signal_power) || !std::isfinite(lts_channel_mag)) {
        return true;
    }

    // Real hardware ACK/DATA frames show LTS signal power orders of magnitude
    // above this. False locks on silence/noise report signal≈0 and |H|≈0, but
    // can still produce clipped soft bits that pass LLR-only gates.
    constexpr float MIN_LTS_SIGNAL_POWER = 1.0e-3f;
    constexpr float MIN_LTS_CHANNEL_MAG = 5.0e-2f;
    return lts_signal_power < MIN_LTS_SIGNAL_POWER &&
           lts_channel_mag < MIN_LTS_CHANNEL_MAG;
}

// Robust single-CW LDPC decode with Phase 0 decoder diversity (up to 4 retries)
// Uses standalone LDPCDecoder for setMinSumFactor (not available via ICodec interface)
// Pattern matches decodeFixedFrame() Phase 0 (frame_v2.cpp:1378-1395)
//
// max_retries: caps the retry budget (0..4). Default 4 = full diversity sweep.
//   ControlFirst (ACK decode) calls this with max_retries=2 — ACK loss is
//   recoverable via ARQ, but ACK decode CPU isn't. See profiling plan v5.
//
// call_site: routes per-call-site retry-attempt histogram. See timing_profiler.hpp.
static std::pair<bool, Bytes> robustDecodeSingleCW(
    const float* cw_data, size_t cw_size, CodeRate rate, const char* log_prefix = nullptr,
    ultra::timing::SingleCWCallSite call_site = ultra::timing::SingleCWCallSite::Default,
    int max_retries = 4)
{
    ultra::timing::ScopedTimer _profile_(
        ultra::timing::globalDecoderProfile().single_cw_decode_total);

    // Reuse one LDPCDecoder instance per code rate per thread. Constructing
    // an LDPCDecoder calls buildMatrix() which expands the IEEE 802.11n
    // parity-check matrix — that's expensive (~50ms+ on Pi 5). For ACK-heavy
    // workloads (~1000 ACK decodes per 50 KB transfer at hardware speeds)
    // the construction cost dominates the actual decode work.
    //
    // thread_local is safe here because the decode thread is the only caller
    // of this function. The cache is keyed by rate — if a future caller
    // ever uses a different rate from the same thread, we re-construct.
    struct CachedDecoder {
        std::unique_ptr<LDPCDecoder> decoder;
        CodeRate rate = static_cast<CodeRate>(-1);
    };
    static thread_local CachedDecoder cache;
    if (!cache.decoder || cache.rate != rate) {
        cache.decoder = std::make_unique<LDPCDecoder>(rate);
        cache.rate = rate;
    }
    LDPCDecoder& decoder = *cache.decoder;
    decoder.setMaxIterations(fec::LDPCCodec::getRecommendedIterations(rate));
    decoder.setMinSumFactor(0.9375f);

    auto decoded = decoder.decodeSoft(std::span<const float>(cw_data, cw_size));
    bool ok = decoder.lastDecodeSuccess();

    int attempts_used = 0;  // 0 = first try; 1..max_retries = retry idx; -1 = exhausted

    if (!ok) {
        static constexpr float factors[] = {0.875f, 0.75f, 0.625f, 0.5f};
        if (max_retries < 0) max_retries = 0;
        if (max_retries > 4) max_retries = 4;
        for (int retry = 0; retry < max_retries && !ok; retry++) {
            decoder.setMinSumFactor(factors[retry]);
            decoded = decoder.decodeSoft(std::span<const float>(cw_data, cw_size));
            ok = decoder.lastDecodeSuccess();
            if (ok) {
                attempts_used = retry + 1;
                g_robust_retry_hits.fetch_add(1, std::memory_order_relaxed);
                if (log_prefix) {
                    LOG_MODEM(INFO, "[%s] Robust CW0: RETRY OK (factor=%.3f, iters=%d, total_hits=%d)",
                              log_prefix, factors[retry], decoder.lastIterations(),
                              g_robust_retry_hits.load(std::memory_order_relaxed));
                }
            }
        }
        if (!ok) attempts_used = -1;  // exhausted budget
    }

    // Bump per-call-site retry histogram
    {
        auto& dp = ultra::timing::globalDecoderProfile();
        ultra::timing::SingleCWHistogram* hist = nullptr;
        switch (call_site) {
            case ultra::timing::SingleCWCallSite::ControlFirst:
                hist = &dp.robust_cw_control_first; break;
            case ultra::timing::SingleCWCallSite::Cw0Peek:
                hist = &dp.robust_cw_cw0_peek; break;
            case ultra::timing::SingleCWCallSite::Default:
            default:
                hist = &dp.robust_cw_default; break;
        }
        hist->record(attempts_used);
    }

    Bytes data;
    if (ok) data.assign(decoded.begin(), decoded.end());
    return {ok, data};
}

void StreamingDecoder::decodeCurrentFrame() {
    if (!waveform_) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            correlation_pos_ = (sync_position_ + 4800) % MAX_BUFFER_SAMPLES;
        }
        state_ = DecoderState::SEARCHING;
        return;
    }

    bool is_ofdm = protocol::isOFDMMode(mode_);

    // Determine how many samples to copy from buffer
    // Strategy depends on mode and state:
    // - pending_total_cw_ > 0: exact size from prior CW0 peek
    // - Connected OFDM, first pass: full 4-CW buffer (most frames are data, frame-interleaved)
    // - MC-DPSK: 1-CW for peek (MC-DPSK getMinSamplesForFrame() == 1 CW by design)
    // - Disconnected: full frame (always MC-DPSK for handshake)
    size_t frame_len;
    if (pending_total_cw_ > 0) {
        frame_len = static_cast<size_t>(waveform_->getMinSamplesForCWCount(pending_total_cw_));
    } else if (is_ofdm && connected_) {
        // Burst-interleaved frames use full 4-CW buffer; otherwise 1-CW peek first
        bool burst_latched = use_burst_interleave_ && waveform_ && waveform_->wasBurstInterleaved();
        if (!burst_latched) {
            frame_len = getOFDMControlFrameSamples(waveform_.get(), current_modulation_, code_rate_);
        } else {
            frame_len = static_cast<size_t>(waveform_->getMinSamplesForFrame());
        }
    } else {
        // MC-DPSK or disconnected: 1-CW peek
        frame_len = static_cast<size_t>(waveform_->getMinSamplesForControlFrame());
    }

    // Copy frame samples from buffer
    std::vector<float> frame_buffer;
    size_t frame_sync_abs = 0;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        frame_sync_abs = ringPosToAbsoluteLocked(sync_position_);

        size_t available;
        if (write_pos_ >= sync_position_) {
            available = write_pos_ - sync_position_;
        } else {
            available = MAX_BUFFER_SAMPLES - sync_position_ + write_pos_;
        }

        frame_len = std::min(frame_len, available);

        frame_buffer.resize(frame_len);
        for (size_t i = 0; i < frame_len; i++) {
            frame_buffer[i] = buffer_[(sync_position_ + i) % MAX_BUFFER_SAMPLES];
        }
    }

    // CFO pre-correction: remove known CFO from raw samples so the entire
    // demodulation chain sees a clean, CFO-free signal.  The waveform/demodulator
    // is then told CFO=0 and only needs to handle small residuals.
    if (is_ofdm && !frame_buffer.empty()) {
        applyCFOPreCorrection(frame_buffer, sync_cfo_, frame_sync_abs);
    }

    // After pre-correction, tell waveform CFO=0 (already removed from samples).
    // For non-OFDM (MC-DPSK), no pre-correction — pass original sync_cfo_.
    const float decode_cfo = (is_ofdm && std::abs(pre_correction_cfo_) > 0.01f)
                              ? 0.0f : sync_cfo_;

    if (frame_buffer.empty()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            correlation_pos_ = (sync_position_ + 4800) % MAX_BUFFER_SAMPLES;
            setSearchFloorLocked(frame_sync_abs + 4800);
        }
        state_ = DecoderState::SEARCHING;
        return;
    }

    // Invalid connected-OFDM LTS locks are often silence/payload autocorr peaks
    // just before the next real LTS. Advancing by a whole frame can skip the real
    // frame; advance only far enough to avoid re-locking the same false peak.
    auto advancePastFalseOFDMLock = [&]() {
        size_t advance = 1024;
        if (waveform_) {
            const int data_preamble = waveform_->getDataPreambleSamples();
            if (data_preamble > 0) {
                advance = std::max<size_t>(512, static_cast<size_t>(data_preamble) / 2);
            }
        }
        advance = std::min(advance, frame_len);

        std::lock_guard<std::mutex> lock(buffer_mutex_);
        correlation_pos_ = (sync_position_ + advance) % MAX_BUFFER_SAMPLES;
        setSearchFloorLocked(frame_sync_abs + advance);
    };

    // PING is a disconnected MC-DPSK chirp-only presence probe. Do not run this
    // RMS heuristic for connected OFDM light-preamble frames: valid data/control
    // frames must be accepted or rejected by the demodulator + CRC/FEC path.
    const bool allow_ping_detection = !connected_ && mode_ == protocol::WaveformMode::MC_DPSK;
    if (allow_ping_detection) {
        // For MC-DPSK: training=4096, ref=512, so data starts at 4608.
        // Check after training to distinguish chirp-only PING from real frames.
        constexpr size_t training_skip = 4608;

        float training_rms = 0.0f;
        size_t train_len = std::min(training_skip, frame_buffer.size());
        if (train_len > 0) {
            for (size_t i = 0; i < train_len; i++) {
                training_rms += frame_buffer[i] * frame_buffer[i];
            }
            training_rms = std::sqrt(training_rms / train_len);
        }

        float rms = 0.0f;
        size_t check_start = std::min(training_skip, frame_buffer.size());
        size_t check_len = std::min(frame_buffer.size() - check_start, size_t(5000));
        if (check_len > 0) {
            for (size_t i = 0; i < check_len; i++) {
                rms += frame_buffer[check_start + i] * frame_buffer[check_start + i];
            }
            rms = std::sqrt(rms / check_len);
        }

        // PING detection: use ratio of data RMS to training RMS.
        // PING (chirp only): data region is noise-only; data frames carry energy.
        float rms_ratio = (training_rms > 0.001f) ? rms / training_rms : 0.0f;
        bool is_ping = rms_ratio < 0.5f;

        LOG_MODEM(INFO, "[%s] PING check: RMS=%.4f, train_RMS=%.4f, ratio=%.3f (threshold=0.5), sync_pos=%zu",
                  log_prefix_.c_str(), rms, training_rms, rms_ratio, sync_position_);

        if (is_ping) {
            LOG_MODEM(INFO, "[%s] PING detected (RMS=%.4f), SNR=%.1f dB, CFO=%.1f Hz",
                      log_prefix_.c_str(), rms, sync_snr_, sync_cfo_);

            DecodeResult ping;
            ping.success = true;
            ping.is_ping = true;
            ping.frame_type = v2::FrameType::PING;
            ping.snr_db = sync_snr_;
            ping.cfo_hz = sync_cfo_;

            {
                std::lock_guard<std::mutex> qlock(queue_mutex_);
                frame_queue_.push(ping);
            }

            if (ping_callback_) ping_callback_(sync_snr_, sync_cfo_);

            {
                std::lock_guard<std::mutex> slock(stats_mutex_);
                stats_.pings_received++;
            }

            // Skip past the PING.
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                size_t min_frame = static_cast<size_t>(waveform_->getMinSamplesForFrame());
                correlation_pos_ = (sync_position_ + min_frame) % MAX_BUFFER_SAMPLES;
                setSearchFloorLocked(frame_sync_abs + min_frame);
                last_decoded_sync_pos_ = sync_position_;
            }

            state_ = DecoderState::SEARCHING;
            return;
        }
    }

    // Connected OFDM control-first hypothesis:
    // Try demodulating as DQPSK R1/4 control before using data profile.
    // This protects ACK/NACK decode when data modulation is higher order.
    bool first_pass_ofdm_peek = (pending_total_cw_ == 0 && is_ofdm && connected_
                                 && frame_len <= getOFDMControlFrameSamples(
                                     waveform_.get(), current_modulation_, code_rate_));
    if (first_pass_ofdm_peek) {
        constexpr size_t CONTROL_LDPC_BLOCK = v2::LDPC_CODEWORD_BITS;
        Modulation saved_mod = current_modulation_;
        CodeRate saved_rate = code_rate_;
        bool switched_profile = (saved_mod != Modulation::DQPSK || saved_rate != CodeRate::R1_4);

        if (switched_profile) {
            waveform_->configure(Modulation::DQPSK, CodeRate::R1_4);
        }

        waveform_->setFrequencyOffset(decode_cfo);
        bool control_ok = waveform_->process(SampleSpan(frame_buffer.data(), frame_buffer.size()));
        if (control_ok) {
            float lts_signal_power = 1.0f;
            float lts_channel_mag = 1.0f;
            if (hasInvalidOFDMTraining(waveform_.get(), is_ofdm, connected_,
                                       lts_signal_power, lts_channel_mag)) {
                LOG_MODEM(INFO, "[%s] False control chirp lock rejected: "
                          "lts_signal=%.6f, |H|_avg=%.4f — re-searching",
                          log_prefix_.c_str(), lts_signal_power, lts_channel_mag);
                if (switched_profile) {
                    waveform_->configure(saved_mod, saved_rate);
                }
                advancePastFalseOFDMLock();
                state_ = DecoderState::SEARCHING;
                return;
            }

            captureConstellationSnapshot();
            auto control_soft_bits = waveform_->getSoftBits();
            if (control_soft_bits.size() >= CONTROL_LDPC_BLOCK) {
                // LLR pre-screen: skip the 1-CW decode (~85ms incl. retries)
                // when the soft bits look like noise. Real ACKs cluster well
                // above this threshold; false-sync attempts cluster near 1.
                std::pair<bool, Bytes> control_decode = {false, {}};
                const float llr_avg = computeLLRAbsAvg(
                    control_soft_bits.data(), CONTROL_LDPC_BLOCK);
                if (llr_avg < MIN_LLR_FOR_1CW_DECODE) {
                    ultra::timing::globalDecoderProfile()
                        .low_llr_1cw_skipped_control_first
                        .fetch_add(1, std::memory_order_relaxed);
                    // Record skipped calls under fail for threshold telemetry.
                    ultra::timing::globalDecoderProfile()
                        .llr_dist_control_first.record(llr_avg, false);
                } else {
                    ultra::timing::ScopedTimer _profile_(
                        ultra::timing::globalDecoderProfile().control_first_1cw);
                    // Reduced retry budget (2 instead of 4) for ACK decode path:
                    // ACK loss is recoverable via ARQ, but the 4-retry sweep was
                    // costing ~22ms × 2 extra attempts × hundreds of ACKs per
                    // transfer. Profiling plan v5 + ChatGPT 5.5 review.
                    control_decode = robustDecodeSingleCW(
                        control_soft_bits.data(), CONTROL_LDPC_BLOCK,
                        CodeRate::R1_4, log_prefix_.c_str(),
                        ultra::timing::SingleCWCallSite::ControlFirst,
                        /*max_retries=*/2);
                    ultra::timing::globalDecoderProfile()
                        .llr_dist_control_first.record(llr_avg, control_decode.first);
                }
                auto [ok_r14, data_r14] = control_decode;
                size_t bpc_r14 = v2::getBytesPerCodeword(CodeRate::R1_4);

                if (ok_r14 && data_r14.size() >= 4
                    && data_r14[0] == 0x55 && data_r14[1] == 0x4C) {
                    if (data_r14.size() > bpc_r14) data_r14.resize(bpc_r14);
                    auto hdr = v2::parseHeader(data_r14);
                    if (hdr.valid && hdr.total_cw == 1 && v2::isControlFrame(hdr.type)) {
                        DecodeResult control_result;
                        control_result.success = true;
                        control_result.frame_data = data_r14;
                        control_result.frame_type = hdr.type;
                        control_result.snr_db = sync_snr_;
                        control_result.cfo_hz = sync_cfo_;
                        control_result.codewords_ok = 1;
                        control_result.codewords_failed = 0;

                        {
                            std::lock_guard<std::mutex> qlock(queue_mutex_);
                            frame_queue_.push(control_result);
                        }
                        if (frame_callback_) {
                            frame_callback_(control_result);
                        }
                        {
                            std::lock_guard<std::mutex> slock(stats_mutex_);
                            stats_.frames_decoded++;
                        }

                        last_fading_index_.store(waveform_->getFadingIndex());

                        if (switched_profile) {
                            waveform_->configure(saved_mod, saved_rate);
                        }

                        {
                            std::lock_guard<std::mutex> lock(buffer_mutex_);
                            correlation_pos_ = (sync_position_ + frame_len) % MAX_BUFFER_SAMPLES;
                            setSearchFloorLocked(frame_sync_abs + frame_len);
                            last_decoded_sync_pos_ = sync_position_;
                        }

                        LOG_MODEM(INFO, "[%s] OFDM control-profile decode SUCCESS (%s seq=%d)",
                                  log_prefix_.c_str(), v2::frameTypeToString(hdr.type), hdr.seq);
                        state_ = DecoderState::SEARCHING;
                        return;
                    }
                }
            }
        }

        if (switched_profile) {
            waveform_->configure(saved_mod, saved_rate);
        }
    }

    // Data frame - process audio to get soft bits
    waveform_->setFrequencyOffset(decode_cfo);

    auto decode_start = std::chrono::steady_clock::now();
    bool ok = waveform_->process(SampleSpan(frame_buffer.data(), frame_buffer.size()));

    if (!ok) {
        LOG_MODEM(DEBUG, "[%s] process() failed", log_prefix_.c_str());
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            correlation_pos_ = (sync_position_ + frame_len) % MAX_BUFFER_SAMPLES;
            setSearchFloorLocked(frame_sync_abs + frame_len);
        }
        state_ = DecoderState::SEARCHING;
        return;
    }

    float lts_signal_power = 1.0f;
    float lts_channel_mag = 1.0f;
    if (hasInvalidOFDMTraining(waveform_.get(), is_ofdm, connected_,
                               lts_signal_power, lts_channel_mag)) {
        LOG_MODEM(INFO, "[%s] False chirp lock rejected: "
                  "lts_signal=%.6f, |H|_avg=%.4f — re-searching",
                  log_prefix_.c_str(), lts_signal_power, lts_channel_mag);
        advancePastFalseOFDMLock();
        state_ = DecoderState::SEARCHING;
        return;
    }

    captureConstellationSnapshot();

    auto soft_bits = waveform_->getSoftBits();
    if (soft_bits.empty()) {
        LOG_MODEM(DEBUG, "[%s] getSoftBits() returned empty", log_prefix_.c_str());
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            correlation_pos_ = (sync_position_ + frame_len) % MAX_BUFFER_SAMPLES;
            setSearchFloorLocked(frame_sync_abs + frame_len);
        }
        state_ = DecoderState::SEARCHING;
        return;
    }

    // Reject false chirp locks before paying for full LDPC decode. Low average
    // confidence is a reliable false-lock signature. A near-zero population is
    // only decisive when it dominates the CW; valid short/tail DATA frames can
    // show a modest near-zero tail after a strong LTS and still decode cleanly.
    constexpr float MIN_PRESYNC_LLR = 1.5f;
    constexpr float NEAR_ZERO_LLR = 0.1f;
    constexpr float MAX_ERASURE_LIKE_FRACTION = 0.30f;
    {
        const size_t llr_n = std::min(soft_bits.size(), size_t(648));
        float llr_sum = 0.0f;
        size_t near_zero_count = 0;
        for (size_t i = 0; i < llr_n; ++i) {
            const float llr_abs = std::abs(soft_bits[i]);
            llr_sum += llr_abs;
            if (llr_abs <= NEAR_ZERO_LLR) {
                ++near_zero_count;
            }
        }
        const float llr_avg = llr_n > 0 ? llr_sum / static_cast<float>(llr_n) : 0.0f;
        const float near_zero_fraction = llr_n > 0
            ? static_cast<float>(near_zero_count) / static_cast<float>(llr_n)
            : 1.0f;
        if (llr_avg < MIN_PRESYNC_LLR || near_zero_fraction > MAX_ERASURE_LIKE_FRACTION) {
            LOG_MODEM(INFO, "[%s] False chirp lock rejected: |llr|_avg=%.2f, "
                      "near_zero=%zu/%zu (%.1f%%), soft_bits=%zu — re-searching",
                      log_prefix_.c_str(), llr_avg, near_zero_count, llr_n,
                      near_zero_fraction * 100.0f, soft_bits.size());
            advancePastFalseOFDMLock();
            state_ = DecoderState::SEARCHING;
            return;
        }
    }

    LOG_MODEM(INFO, "[%s] Got %zu soft bits (%zu samples), proceeding to decode",
              log_prefix_.c_str(), soft_bits.size(), frame_buffer.size());

    last_fading_index_.store(waveform_->getFadingIndex());

    // Check for burst interleave marker (negated LTS detected by waveform)
    // Only for connected OFDM_CHIRP when burst interleaving is enabled
    bool burst_marker = use_burst_interleave_ && connected_ && is_ofdm
                        && mode_ == protocol::WaveformMode::OFDM_CHIRP
                        && waveform_->wasBurstInterleaved();

    if (burst_marker) {
        LOG_MODEM(INFO, "[%s] Burst interleave marker detected, entering accumulation",
                  log_prefix_.c_str());

        // Initialize accumulation state with first frame's soft bits
        burst_soft_buffer_.clear();
        burst_soft_buffer_.push_back(std::move(soft_bits));
        burst_min_block_ = static_cast<size_t>(waveform_->getMinSamplesForFrame());
        burst_next_pos_ = (sync_position_ + frame_len) % MAX_BUFFER_SAMPLES;
        burst_snr_ = sync_snr_;
        burst_cfo_ = sync_cfo_;
        burst_start_time_ = std::chrono::steady_clock::now();

        // Feed back CFO from first frame (add pre-correction amount back)
        float residual_cfo = waveform_->estimatedCFO();
        float corrected_cfo = pre_correction_cfo_ + residual_cfo;
        float current_cfo = last_cfo_.load();
        constexpr float MAX_PILOT_CFO_DRIFT_HZ_B = 2.0f;
        float drift = corrected_cfo - current_cfo;
        if (std::abs(drift) > MAX_PILOT_CFO_DRIFT_HZ_B) {
            corrected_cfo = current_cfo + std::copysign(MAX_PILOT_CFO_DRIFT_HZ_B, drift);
        }
        last_cfo_.store(corrected_cfo);
        burst_cfo_ = corrected_cfo;

        state_ = DecoderState::BURST_ACCUMULATING;
        return;  // processBuffer() will call accumulateBurstFrames() on next iteration
    }

    // Feed back pilot-corrected CFO to cached value (OFDM only).
    // MC-DPSK does not have pilot-based tracking; keep chirp-derived CFO.
    if (is_ofdm) {
        // After pre-correction, the demodulator sees near-zero CFO.
        // waveform_->estimatedCFO() returns the RESIDUAL (pre-correction error).
        // Add back the pre-correction amount to get the true total CFO.
        float residual_cfo = waveform_->estimatedCFO();
        float corrected_cfo = pre_correction_cfo_ + residual_cfo;
        float current_cfo = last_cfo_.load();

        if (connected_) {
            constexpr float MAX_PILOT_CFO_DRIFT_HZ = 2.0f;
            float drift = corrected_cfo - current_cfo;
            if (std::abs(drift) > MAX_PILOT_CFO_DRIFT_HZ) {
                LOG_MODEM(WARN, "[%s] Pilot CFO drift clamped: %.2f → %.2f Hz (drift=%.2f, max=%.1f)",
                          log_prefix_.c_str(), current_cfo, corrected_cfo, drift, MAX_PILOT_CFO_DRIFT_HZ);
                corrected_cfo = current_cfo + std::copysign(MAX_PILOT_CFO_DRIFT_HZ, drift);
            }
        }

        if (std::abs(corrected_cfo - current_cfo) > 0.1f) {
            LOG_MODEM(INFO, "[%s] CFO updated: %.2f → %.2f Hz (pre_corr=%.2f + residual=%.2f)",
                      log_prefix_.c_str(), current_cfo, corrected_cfo,
                      pre_correction_cfo_, residual_cfo);
        }
        last_cfo_.store(corrected_cfo);
        sync_cfo_ = corrected_cfo;
    }

    constexpr size_t LDPC_BLOCK = v2::LDPC_CODEWORD_BITS;
    CodeRate rate = connected_ ? code_rate_ : CodeRate::R1_4;

    // ========================================================================
    // CW0 peek for MC-DPSK (non-OFDM, 1-CW buffer)
    // MC-DPSK starts with 1-CW buffer, needs to check total_cw before decode
    // ========================================================================

    if (pending_total_cw_ == 0 && !is_ofdm && soft_bits.size() >= LDPC_BLOCK) {
        std::vector<float> cw0(soft_bits.begin(), soft_bits.begin() + LDPC_BLOCK);
        codec_->setRate(rate);
        auto [peek_ok, peek_data] = codec_->decode(cw0);

        if (peek_ok && peek_data.size() >= 4 && peek_data[0] == 0x55 && peek_data[1] == 0x4C) {
            auto hdr = v2::parseHeader(peek_data);
            if (hdr.valid && hdr.total_cw > 1) {
                int avail_cw = static_cast<int>(soft_bits.size() / LDPC_BLOCK);
                if (avail_cw < hdr.total_cw) {
                    pending_total_cw_ = hdr.total_cw;
                    state_ = DecoderState::SYNC_FOUND;
                    LOG_MODEM(INFO, "[%s] CW0 peek: need %d CWs (have %d) — waiting for %d samples",
                              log_prefix_.c_str(), hdr.total_cw, avail_cw,
                              waveform_->getMinSamplesForCWCount(hdr.total_cw));
                    return;
                }
            }
        }
    }

    // ========================================================================
    // CW0 peek for connected OFDM (1-CW initial buffer)
    // Was: try a 1-CW R1/4 decode + code_rate_ fallback to short-circuit
    //      control frames or read the multi-CW total_cw from the header.
    // Now: removed the decode attempts. Profiling (LLR distribution histogram,
    //      see plan v5) showed the R1/4 1-CW decode at this site succeeds 0
    //      times in 1300+ calls across multiple seeds, even at |LLR|>=6.
    //      Real 1-CW control frames are always R1/4 (hardened) and are
    //      caught upstream by the control-first hypothesis at line ~1136.
    //      Cost of the dead branch: ~36 s of decode CPU per 50 KB transfer.
    // Kept: LLR pre-empt as a coarse false-sync gate and the 4-CW escalation
    //       that drives the actual data-frame decode.
    // ========================================================================
    if (pending_total_cw_ == 0 && is_ofdm && connected_
        && soft_bits.size() >= LDPC_BLOCK && soft_bits.size() < 2 * LDPC_BLOCK) {

        // Check LLR quality before expensive 4-CW escalation.
        // False syncs (e.g. from fading-corrupted LTS) cluster near
        // |llr|_avg <= 1.0. Moderate SNR12 tail frames can be real at ~1.7,
        // so this gate stays permissive and lets LDPC be the final arbiter.
        // Escalating on garbage wastes ~1.5s on 4x failed LDPC (50 iters each),
        // blocking the decoder from processing real frames arriving in the buffer.
        float llr_abs_sum = 0.0f;
        size_t llr_count = std::min(soft_bits.size(), LDPC_BLOCK);
        for (size_t i = 0; i < llr_count; ++i) {
            llr_abs_sum += std::abs(soft_bits[i]);
        }
        float llr_abs_avg = llr_abs_sum / static_cast<float>(llr_count);

        constexpr float MIN_LLR_FOR_ESCALATION = 1.5f;
        if (llr_abs_avg < MIN_LLR_FOR_ESCALATION) {
            ultra::timing::globalDecoderProfile()
                .low_llr_escalation_skipped.fetch_add(1, std::memory_order_relaxed);
            LOG_MODEM(INFO, "[%s] OFDM CW0 peek: |llr|_avg=%.1f too low — skipping 4-CW escalation (likely false sync)",
                      log_prefix_.c_str(), llr_abs_avg);
            advancePastFalseOFDMLock();
            state_ = DecoderState::SEARCHING;
            return;
        }

        pending_total_cw_ = v2::FIXED_FRAME_CODEWORDS;  // 4
        state_ = DecoderState::SYNC_FOUND;
        LOG_MODEM(INFO, "[%s] OFDM CW0 peek: |llr|_avg=%.1f, escalating to %d CWs",
                  log_prefix_.c_str(), llr_abs_avg, pending_total_cw_);
        return;
    }

    // Decode the frame using the soft bits
    DecodeResult result = decodeFrame(soft_bits, sync_snr_, sync_cfo_);

    // ========================================================================
    // Small-frame recovery for OFDM connected mode:
    // If full 4-CW buffer decode failed, the frame might be a small non-data
    // frame (e.g. DISCONNECT = 2 CWs at R1/2) where trailing noise symbols
    // degraded LLR quality. Retry with 1-CW peek to determine actual size.
    // ========================================================================

    if (!result.success && is_ofdm && connected_) {
        size_t one_cw_s = static_cast<size_t>(waveform_->getMinSamplesForControlFrame());
        if (one_cw_s <= frame_buffer.size()) {
            waveform_->setFrequencyOffset(decode_cfo);
            if (waveform_->process(SampleSpan(frame_buffer.data(), one_cw_s))) {
                captureConstellationSnapshot();
            }
            auto short_bits = waveform_->getSoftBits();

            if (short_bits.size() >= LDPC_BLOCK) {
                // Try R1/4 first (control frames hardened), then code_rate_ fallback
                auto trySmallFrame = [&](CodeRate sr) -> bool {
                    auto [ok2, data2] = robustDecodeSingleCW(
                        short_bits.data(), LDPC_BLOCK, sr, log_prefix_.c_str());
                    if (!ok2 || data2.size() < 4 || data2[0] != 0x55 || data2[1] != 0x4C)
                        return false;
                    auto hdr2 = v2::parseHeader(data2);
                    if (hdr2.valid && hdr2.total_cw == 1) {
                        // 1-CW control frame — decode via decodeFrame (has R1/4 fast-path)
                        result = decodeFrame(short_bits, sync_snr_, sync_cfo_);
                        frame_len = one_cw_s;
                        return true;
                    } else if (hdr2.valid && hdr2.total_cw > 1 && hdr2.total_cw < v2::FIXED_FRAME_CODEWORDS) {
                        // Variable-CW frame (2-3 CWs) — reprocess with exact size
                        size_t exact_size = static_cast<size_t>(
                            waveform_->getMinSamplesForCWCount(hdr2.total_cw));
                        exact_size = std::min(exact_size, frame_buffer.size());

                        LOG_MODEM(INFO, "[%s] Small-frame recovery: reprocessing %zu samples (%d CWs)",
                                  log_prefix_.c_str(), exact_size, hdr2.total_cw);
                        waveform_->setFrequencyOffset(decode_cfo);
                        if (waveform_->process(SampleSpan(frame_buffer.data(), exact_size))) {
                            captureConstellationSnapshot();
                        }
                        auto recovered_bits = waveform_->getSoftBits();
                        result = decodeFrame(recovered_bits, sync_snr_, sync_cfo_);
                        frame_len = exact_size;
                        return true;
                    }
                    return false;
                };

                if (!trySmallFrame(CodeRate::R1_4) && rate != CodeRate::R1_4) {
                    trySmallFrame(rate);
                }
            }
        }
    }

    // Multi-candidate light-sync recovery (connected OFDM):
    // If decode fails at the detected sync point, retry nearby timing candidates.
    // detectDataSync() scans with coarse steps, and fading can shift the best
    // decode point by a few samples even when correlation looks valid.
    if (!result.success && result.codewords_ok == 0 && is_ofdm && connected_) {
        // Keep this recovery path tight. Moderate-fading hardware traces showed
        // low-confidence syncs can pass the LLR gate, then eight full 4-CW LDPC
        // retries burn several seconds with zero recoveries and trigger ARQ
        // timeouts. Nearby timing retry is still useful for clean, high-corr
        // locks, but beyond +/-8 samples the candidate is usually a bad lock.
        constexpr float MIN_CORR_FOR_SYNC_RECOVERY = 0.80f;
        const int retry_deltas[] = {8, -8};
        bool recovered = false;
        int recovered_delta = 0;
        uint64_t recovery_attempts = 0;

        const bool allow_sync_recovery = sync_correlation_ >= MIN_CORR_FOR_SYNC_RECOVERY;
        if (!allow_sync_recovery) {
            LOG_MODEM(INFO, "[%s] Multi-candidate sync recovery skipped: corr=%.2f < %.2f",
                      log_prefix_.c_str(), sync_correlation_, MIN_CORR_FOR_SYNC_RECOVERY);
        }

        auto ringPosToAbsolute = [this](size_t ring_pos) -> size_t {
            if (total_fed_ < MAX_BUFFER_SAMPLES) {
                return ring_pos;
            }
            const size_t oldest_abs = total_fed_ - MAX_BUFFER_SAMPLES;
            const size_t oldest_pos = write_pos_;
            const size_t offset = (ring_pos >= oldest_pos)
                ? (ring_pos - oldest_pos)
                : (MAX_BUFFER_SAMPLES - oldest_pos + ring_pos);
            return oldest_abs + offset;
        };

        if (allow_sync_recovery) {
            for (int delta : retry_deltas) {
                recovery_attempts++;
                size_t retry_sync = (sync_position_ + MAX_BUFFER_SAMPLES + delta) % MAX_BUFFER_SAMPLES;

                std::vector<float> retry_buffer;
                size_t retry_len = frame_len;
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    size_t available;
                    if (write_pos_ >= retry_sync) {
                        available = write_pos_ - retry_sync;
                    } else {
                        available = MAX_BUFFER_SAMPLES - retry_sync + write_pos_;
                    }
                    retry_len = std::min(retry_len, available);
                    if (retry_len == 0) {
                        continue;
                    }
                    retry_buffer.resize(retry_len);
                    for (size_t i = 0; i < retry_len; ++i) {
                        retry_buffer[i] = buffer_[(retry_sync + i) % MAX_BUFFER_SAMPLES];
                    }
                }

                // Pre-correct CFO on retry buffer too
                if (is_ofdm && std::abs(pre_correction_cfo_) > 0.01f) {
                    applyCFOPreCorrection(retry_buffer, sync_cfo_, ringPosToAbsolute(retry_sync));
                }

                waveform_->reset();
                waveform_->setAbsoluteTrainingPosition(ringPosToAbsolute(retry_sync));
                waveform_->setFrequencyOffset(decode_cfo);
                bool retry_ok = waveform_->process(SampleSpan(retry_buffer.data(), retry_buffer.size()));
                if (!retry_ok) {
                    continue;
                }
                captureConstellationSnapshot();
                auto retry_bits = waveform_->getSoftBits();
                if (retry_bits.empty()) {
                    continue;
                }

                auto retry_result = decodeFrame(retry_bits, sync_snr_, sync_cfo_);
                if (!(retry_result.success || retry_result.codewords_ok > 0)) {
                    continue;
                }

                LOG_MODEM(INFO, "[%s] Multi-candidate sync recovery: delta=%+d samples succeeded",
                          log_prefix_.c_str(), delta);

                // Keep CFO tracking consistent with the accepted retry candidate.
                float corrected_cfo = waveform_->estimatedCFO();
                float current_cfo = last_cfo_.load();
                constexpr float MAX_PILOT_CFO_DRIFT_HZ = 2.0f;
                float drift = corrected_cfo - current_cfo;
                if (std::abs(drift) > MAX_PILOT_CFO_DRIFT_HZ) {
                    corrected_cfo = current_cfo + std::copysign(MAX_PILOT_CFO_DRIFT_HZ, drift);
                }
                last_cfo_.store(corrected_cfo);
                sync_cfo_ = corrected_cfo;
                last_fading_index_.store(waveform_->getFadingIndex());

                sync_position_ = retry_sync;
                frame_len = retry_len;
                result = std::move(retry_result);
                recovered_delta = delta;
                recovered = true;
                break;
            }
        }

        if (recovery_attempts > 0) {
            std::lock_guard<std::mutex> slock(stats_mutex_);
            stats_.sync_recovery_attempts += recovery_attempts;
            if (recovered) {
                stats_.sync_recovery_successes++;
                switch (recovered_delta) {
                    case 8: stats_.sync_recovery_delta_p8++; break;
                    case -8: stats_.sync_recovery_delta_m8++; break;
                    case 16: stats_.sync_recovery_delta_p16++; break;
                    case -16: stats_.sync_recovery_delta_m16++; break;
                    case 24: stats_.sync_recovery_delta_p24++; break;
                    case -24: stats_.sync_recovery_delta_m24++; break;
                    case 32: stats_.sync_recovery_delta_p32++; break;
                    case -32: stats_.sync_recovery_delta_m32++; break;
                    default: break;
                }
            }
        }

        if (!recovered) {
            LOG_MODEM(DEBUG, "[%s] Multi-candidate sync recovery: no nearby offset decoded",
                      log_prefix_.c_str());
        }
    }

    auto decode_end = std::chrono::steady_clock::now();
    float ms = std::chrono::duration<float, std::milli>(decode_end - decode_start).count();

    {
        std::lock_guard<std::mutex> slock(stats_mutex_);
        if (result.success) stats_.frames_decoded++;
        else stats_.frames_failed++;
        stats_.avg_decode_time_ms = 0.9f * stats_.avg_decode_time_ms + 0.1f * ms;
    }

    if (result.success || result.codewords_ok > 0) {
        {
            std::lock_guard<std::mutex> qlock(queue_mutex_);
            frame_queue_.push(result);
        }
        if (result.success && frame_callback_) frame_callback_(result);

        LOG_MODEM(INFO, "[%s] StreamingDecoder: Frame decoded, %d/%d CWs, SNR=%.1f dB, CFO=%.1f Hz",
                  log_prefix_.c_str(), result.codewords_ok, result.codewords_ok + result.codewords_failed,
                  sync_snr_, sync_cfo_);
    } else {
        LOG_MODEM(WARN, "[%s] StreamingDecoder: Decode failed (cw_ok=%d, cw_fail=%d, is_ping=%d)",
                  log_prefix_.c_str(), result.codewords_ok, result.codewords_failed, result.is_ping ? 1 : 0);
    }

    // Calculate consumed samples based on actual frame content
    // For non-data frames (control/connect), use exact sample count to avoid eating into next frame
    bool is_non_data_frame = false;
    if (result.success && result.frame_data.size() >= 3) {
        auto ft = static_cast<v2::FrameType>(result.frame_data[2]);
        is_non_data_frame = v2::isControlFrame(ft) || v2::isConnectFrame(ft);
    }

    size_t consumed = frame_len;
    if (result.success && is_non_data_frame && is_ofdm) {
        // Use exact sample count for the actual CW count
        int actual_cw = result.codewords_ok + result.codewords_failed;
        if (actual_cw > 0) {
            size_t exact_consumed = static_cast<size_t>(waveform_->getMinSamplesForCWCount(actual_cw));
            if (exact_consumed < consumed) {
                LOG_MODEM(INFO, "[%s] Non-data frame (%d CWs): advancing %zu samples (not %zu)",
                          log_prefix_.c_str(), actual_cw, exact_consumed, consumed);
                consumed = exact_consumed;
            }
        }
    }
    size_t next_block_pos = (sync_position_ + consumed) % MAX_BUFFER_SAMPLES;
    size_t next_search_abs = frame_sync_abs + consumed;

    // Legacy fixed-offset continuation is only safe for a physical burst.
    // Marker-based bursts are handled earlier by BURST_ACCUMULATING; ordinary
    // ARQ-refilled frames must re-acquire sync because hardware audio latency
    // can insert gaps between frames. Treating those gaps as contiguous burst
    // payload decodes the wrong samples, wastes LDPC time, and misses the real
    // next frame.
    if (result.success && connected_ && is_ofdm && !is_non_data_frame
        && use_burst_interleave_ && waveform_ && waveform_->wasBurstInterleaved()) {
        size_t min_block = static_cast<size_t>(waveform_->getMinSamplesForFrame());

        // Loop to decode multiple burst continuation blocks
        while (burst_blocks_decoded_ < MAX_BURST_BLOCKS) {
            // Check if there are enough samples for another block
            size_t next_available;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                if (write_pos_ >= next_block_pos) {
                    next_available = write_pos_ - next_block_pos;
                } else {
                    next_available = MAX_BUFFER_SAMPLES - next_block_pos + write_pos_;
                }
            }

            if (next_available < min_block) break;  // Not enough samples, burst over

            // Copy next block samples
            std::vector<float> next_block(min_block);
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                for (size_t i = 0; i < min_block; i++) {
                    next_block[i] = buffer_[(next_block_pos + i) % MAX_BUFFER_SAMPLES];
                }
            }

            // Check RMS energy to detect if there's actually a signal
            float next_rms = 0.0f;
            size_t burst_check_start = std::min(size_t(1024), min_block);  // Skip training area
            size_t burst_check_len = std::min(min_block - burst_check_start, size_t(5000));
            if (burst_check_len > 0) {
                for (size_t i = 0; i < burst_check_len; i++) {
                    next_rms += next_block[burst_check_start + i] * next_block[burst_check_start + i];
                }
                next_rms = std::sqrt(next_rms / burst_check_len);
            }

            constexpr float BURST_ENERGY_THRESHOLD = 0.04f;
            if (next_rms < BURST_ENERGY_THRESHOLD) break;  // No energy, burst over

            // Energy present - try to decode as continuation block
            burst_blocks_decoded_++;
            LOG_MODEM(INFO, "[%s] Burst continuation: block %d, RMS=%.4f, pos=%zu",
                      log_prefix_.c_str(), burst_blocks_decoded_, next_rms, next_block_pos);

            waveform_->setFrequencyOffset(decode_cfo);
            bool next_ok = waveform_->process(SampleSpan(next_block.data(), next_block.size()));

            if (!next_ok) break;  // Process failed, burst over
            captureConstellationSnapshot();

            auto next_soft_bits = waveform_->getSoftBits();
            if (next_soft_bits.empty()) break;

            // LLR sanity check: if the soft bits look like noise (typical
            // sign of demodulating into the next frame's chirp/training
            // area, or into silence between frames), bail before paying
            // ~600ms of LDPC retry attempts that all fail. Real data
            // frames at usable SNR show |LLR|_avg >> 2; lost-frame attempts
            // collapse to |LLR|_avg < 1. See hardware-test analysis in
            // commit message — saves ~600ms per lost frame which lets
            // chirp-search lock on to the next real frame promptly.
            constexpr float MIN_BURST_CONT_LLR = 2.0f;
            float burst_llr_sum = 0.0f;
            const size_t burst_llr_n = std::min(next_soft_bits.size(), size_t(648));
            for (size_t i = 0; i < burst_llr_n; ++i) {
                burst_llr_sum += std::abs(next_soft_bits[i]);
            }
            const float burst_llr_avg = burst_llr_n > 0
                ? burst_llr_sum / static_cast<float>(burst_llr_n) : 0.0f;
            if (burst_llr_avg < MIN_BURST_CONT_LLR) {
                LOG_MODEM(INFO, "[%s] Burst continuation: bail at block %d "
                          "(|llr|_avg=%.2f < %.2f, no real frame here)",
                          log_prefix_.c_str(), burst_blocks_decoded_,
                          burst_llr_avg, MIN_BURST_CONT_LLR);
                break;
            }

            // Update CFO from pilot tracking
            float next_corrected_cfo = waveform_->estimatedCFO();
            float cfo_drift = next_corrected_cfo - sync_cfo_;
            constexpr float MAX_BURST_CFO_DRIFT_HZ = 2.0f;
            if (std::abs(cfo_drift) > MAX_BURST_CFO_DRIFT_HZ) {
                next_corrected_cfo = sync_cfo_ + std::copysign(MAX_BURST_CFO_DRIFT_HZ, cfo_drift);
            }
            sync_cfo_ = next_corrected_cfo;
            last_cfo_.store(next_corrected_cfo);
            last_fading_index_.store(waveform_->getFadingIndex());

            // Decode the continuation block
            DecodeResult next_result = decodeFrame(next_soft_bits, sync_snr_, sync_cfo_);

            {
                std::lock_guard<std::mutex> slock(stats_mutex_);
                if (next_result.success) stats_.frames_decoded++;
                else stats_.frames_failed++;
            }

            if (next_result.success || next_result.codewords_ok > 0) {
                {
                    std::lock_guard<std::mutex> qlock(queue_mutex_);
                    frame_queue_.push(next_result);
                }
                if (next_result.success && frame_callback_) frame_callback_(next_result);

                LOG_MODEM(INFO, "[%s] Burst block %d decoded: %d/%d CWs",
                          log_prefix_.c_str(), burst_blocks_decoded_,
                          next_result.codewords_ok, next_result.codewords_ok + next_result.codewords_failed);
            }

            // Advance position for next iteration (or final correlation_pos_)
            sync_position_ = next_block_pos;
            next_block_pos = (next_block_pos + min_block) % MAX_BUFFER_SAMPLES;
            next_search_abs += min_block;

            // If decode failed completely, stop the burst
            if (!next_result.success && next_result.codewords_ok == 0) break;
        }
    }

    // Burst over (or non-burst) - skip past everything we decoded and return to SEARCHING
    burst_blocks_decoded_ = 0;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        correlation_pos_ = (next_block_pos) % MAX_BUFFER_SAMPLES;
        setSearchFloorLocked(next_search_abs);
        last_decoded_sync_pos_ = sync_position_;
    }

    state_ = DecoderState::SEARCHING;
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

    mode_ = mode;
    connected_ = connected;

    if (mode == protocol::WaveformMode::MC_DPSK) {
        waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_carriers_);
    } else {
        waveform_ = WaveformFactory::create(mode);
    }

    size_t bps = mc_dpsk_carriers_ * 2;
    if (protocol::isOFDMMode(mode)) {
        bps = 60;
    }
    interleaver_ = std::make_unique<ChannelInterleaver>(bps, v2::LDPC_CODEWORD_BITS);

    state_ = DecoderState::SEARCHING;
    pending_total_cw_ = 0;
    sync_reject_streak_ = 0;
    constellation_cache_.clear();
    constellation_cache_time_ = std::chrono::steady_clock::time_point{};

    // Clear burst interleave state on mode change
    burst_soft_buffer_.clear();
    use_burst_interleave_ = false;  // Re-enabled by caller if needed

    // CRITICAL: Reset correlation_pos_ to current write position
    // Otherwise we'll search old data from previous mode
    correlation_pos_ = write_pos_;
    setSearchFloorLocked(total_fed_);

    LOG_MODEM(INFO, "StreamingDecoder: Mode=%s (%s), reset corr_pos=%zu",
              protocol::waveformModeToString(mode), connected ? "connected" : "disconnected", correlation_pos_);
}

void StreamingDecoder::setMCDPSKCarriers(int n) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (mc_dpsk_carriers_ == n) return;
    mc_dpsk_carriers_ = n;
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        waveform_ = WaveformFactory::createMCDPSK(n);
        interleaver_ = std::make_unique<ChannelInterleaver>(n * 2, v2::LDPC_CODEWORD_BITS);
    }
}

void StreamingDecoder::setOFDMConfig(const ModemConfig& config) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    // Store carrier counts regardless of current mode (used when switching to OFDM later)
    ofdm_carriers_ = config.num_carriers;
    ofdm_data_carriers_ = config.getDataCarriers();

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
        waveform_ = std::make_unique<OFDMNvisWaveform>(config);
        LOG_MODEM(INFO, "StreamingDecoder: OFDM_COX config set (FFT=%d, carriers=%d)",
                  config.fft_size, config.num_carriers);
    }

    // Update interleaver for new carrier count (using current modulation)
    size_t bps = ofdm_data_carriers_ * getBitsPerSymbol(current_modulation_);
    interleaver_ = std::make_unique<ChannelInterleaver>(bps, v2::LDPC_CODEWORD_BITS);
}

void StreamingDecoder::setConnectedOFDMMode(protocol::WaveformMode mode,
                                            const ModemConfig& config,
                                            Modulation mod,
                                            CodeRate rate) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    mode_ = mode;
    connected_ = true;
    code_rate_ = rate;
    current_modulation_ = mod;
    ofdm_carriers_ = config.num_carriers;
    ofdm_data_carriers_ = config.getDataCarriers();

    if (mode_ == protocol::WaveformMode::OFDM_CHIRP) {
        waveform_ = std::make_unique<OFDMChirpWaveform>(config);
    } else if (mode_ == protocol::WaveformMode::OFDM_NARROW) {
        waveform_ = std::make_unique<OFDMChirpWaveform>(config, protocol::WaveformMode::OFDM_NARROW);
    } else {
        waveform_ = std::make_unique<OFDMNvisWaveform>(config);
    }

    if (waveform_) {
        waveform_->configure(mod, rate);
    }

    // Query waveform for effective pilot layout after configure().
    int pilot_spacing = waveform_ ? waveform_->getPilotSpacing() : 0;
    if (pilot_spacing > 0) {
        int pilot_count = (ofdm_carriers_ + pilot_spacing - 1) / pilot_spacing;
        ofdm_data_carriers_ = ofdm_carriers_ - pilot_count;
    } else {
        ofdm_data_carriers_ = ofdm_carriers_;
    }

    size_t bps = ofdm_data_carriers_ * getBitsPerSymbol(mod);
    interleaver_ = std::make_unique<ChannelInterleaver>(bps, v2::LDPC_CODEWORD_BITS);

    state_ = DecoderState::SEARCHING;
    pending_total_cw_ = 0;
    sync_reject_streak_ = 0;
    constellation_cache_.clear();
    constellation_cache_time_ = std::chrono::steady_clock::time_point{};
    burst_soft_buffer_.clear();
    correlation_pos_ = write_pos_;
    setSearchFloorLocked(total_fed_);

    LOG_MODEM(INFO, "StreamingDecoder: connected OFDM mode=%s, mod=%s, rate=%s, carriers=%d data=%d bps=%zu",
              protocol::waveformModeToString(mode_),
              modulationToString(mod), codeRateToString(rate),
              ofdm_carriers_, ofdm_data_carriers_, bps);
}

void StreamingDecoder::setDataMode(Modulation mod, CodeRate rate) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    code_rate_ = rate;
    current_modulation_ = mod;
    if (waveform_) waveform_->configure(mod, rate);

    // After configure(), the waveform has updated pilot config
    // Query waveform for actual pilot_spacing (coherent modes use denser pilots)
    if (mode_ != protocol::WaveformMode::MC_DPSK && waveform_) {
        int pilot_spacing = waveform_->getPilotSpacing();
        if (pilot_spacing > 0) {
            int pilot_count = (ofdm_carriers_ + pilot_spacing - 1) / pilot_spacing;
            ofdm_data_carriers_ = ofdm_carriers_ - pilot_count;
        } else {
            ofdm_data_carriers_ = ofdm_carriers_;
        }
    }

    // Update interleaver for new modulation
    // Use data carriers (not total) to account for pilot overhead
    int carriers = (mode_ == protocol::WaveformMode::MC_DPSK) ? mc_dpsk_carriers_ : ofdm_data_carriers_;
    size_t bps = carriers * getBitsPerSymbol(mod);
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
    size_t used = std::min(total_fed_, MAX_BUFFER_SAMPLES);
    return 100.0f * used / MAX_BUFFER_SAMPLES;
}

DecoderStats StreamingDecoder::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

StreamingDecoder::DecoderConfig StreamingDecoder::getConfig() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    DecoderConfig cfg;
    cfg.mode = mode_;
    cfg.modulation = current_modulation_;
    cfg.code_rate = code_rate_;
    cfg.num_carriers = ofdm_carriers_;
    cfg.data_carriers = ofdm_data_carriers_;
    cfg.bits_per_symbol = ofdm_data_carriers_ * getBitsPerSymbol(current_modulation_);

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
    return std::min(total_fed_, MAX_BUFFER_SAMPLES);
}

bool StreamingDecoder::isSynced() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return state_ == DecoderState::SYNC_FOUND || state_ == DecoderState::DECODING
        || state_ == DecoderState::BURST_ACCUMULATING;
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
    samples_since_sync_ = 0;
    total_fed_ = 0;
    feed_iter_ = 0;
    overflow_events_ = 0;
    sync_reject_streak_ = 0;
    state_ = DecoderState::SEARCHING;
    pending_total_cw_ = 0;
    burst_blocks_decoded_ = 0;
    burst_soft_buffer_.clear();
    constellation_cache_.clear();
    constellation_cache_time_ = std::chrono::steady_clock::time_point{};
    use_burst_interleave_ = false;
    new_data_available_ = false;
    last_decoded_sync_pos_ = SIZE_MAX;
    search_floor_abs_ = 0;
    search_floor_abs_valid_ = false;

    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    if (waveform_) waveform_->reset();

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
    last_cfo_.store(0.0f);
    last_fading_index_.store(0.0f);
}

void StreamingDecoder::stop() {
    shutdown_.store(true);
    data_cv_.notify_all();
}

// ============================================================================
// HELPERS
// ============================================================================

float StreamingDecoder::estimateSNRFromChirp(float corr, float /*noise*/) {
    float snr = (corr - 0.15f) / 0.03f;
    return std::max(-5.0f, std::min(30.0f, snr));
}

// ============================================================================
// MC-DPSK: Simple sequential codeword decode (no frame interleaving)
// ============================================================================
DecodeResult StreamingDecoder::decodeMCDPSKFrame(const std::vector<float>& soft_bits,
                                                   CodeRate rate, size_t bytes_per_cw,
                                                   float snr, float cfo) {
    DecodeResult result;
    result.snr_db = snr;
    result.cfo_hz = cfo;

    constexpr size_t LDPC_BLOCK = v2::LDPC_CODEWORD_BITS;

    if (soft_bits.size() < LDPC_BLOCK) return result;

    codec_->setRate(rate);

    // Decode CW0 (header codeword)
    std::vector<float> cw0_bits(soft_bits.begin(), soft_bits.begin() + LDPC_BLOCK);
    auto [ok0, data0] = codec_->decode(cw0_bits);

    if (!ok0 || data0.size() < 2 || data0[0] != 0x55 || data0[1] != 0x4C) {
        LOG_MODEM(INFO, "[%s] MC-DPSK: CW0 decode failed (ok=%d, size=%zu, magic=0x%02X%02X)",
                  log_prefix_.c_str(), ok0 ? 1 : 0, data0.size(),
                  data0.size() >= 1 ? data0[0] : 0, data0.size() >= 2 ? data0[1] : 0);
        return result;
    }

    // Truncate to bytes_per_cw if needed
    if (data0.size() > bytes_per_cw) data0.resize(bytes_per_cw);

    // Parse header
    auto hdr = v2::parseHeader(data0);
    if (!hdr.valid) {
        // Log CRC details for control frames (ACK, etc.)
        if (data0.size() >= 20) {
            uint16_t received_crc = (static_cast<uint16_t>(data0[18]) << 8) | data0[19];

            // Full 18 bytes
            uint8_t full18[18];
            for (int i = 0; i < 18; i++) full18[i] = data0[i];
            uint16_t calc18 = v2::ControlFrame::calculateCRC(full18, 18);

            // Print all 20 bytes for inspection
            LOG_MODEM(INFO, "[%s] MC-DPSK: CRC fail - rcv=0x%04X calc18=0x%04X",
                      log_prefix_.c_str(), received_crc, calc18);
            LOG_MODEM(INFO, "[%s] MC-DPSK: bytes[0-9]  = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                      log_prefix_.c_str(),
                      data0[0], data0[1], data0[2], data0[3], data0[4],
                      data0[5], data0[6], data0[7], data0[8], data0[9]);
            LOG_MODEM(INFO, "[%s] MC-DPSK: bytes[10-19] = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                      log_prefix_.c_str(),
                      data0[10], data0[11], data0[12], data0[13], data0[14],
                      data0[15], data0[16], data0[17], data0[18], data0[19]);
        } else {
            LOG_MODEM(INFO, "[%s] MC-DPSK: Header invalid - data too small (%zu bytes)",
                      log_prefix_.c_str(), data0.size());
        }
        return result;
    }

    result.frame_type = hdr.type;
    result.codewords_ok = 1;

    // 1-CW frame (control frame like ACK, PROBE, etc.)
    if (hdr.total_cw == 1) {
        result.success = true;
        result.frame_data = data0;
        LOG_MODEM(DEBUG, "[%s] MC-DPSK: Control frame (1 CW) decoded", log_prefix_.c_str());
        return result;
    }

    // Multi-CW frame - decode remaining codewords sequentially
    int total_cw = hdr.total_cw;
    int avail_cw = static_cast<int>(soft_bits.size() / LDPC_BLOCK);

    if (avail_cw < total_cw) {
        // Not enough codewords available
        result.frame_data = data0;
        LOG_MODEM(DEBUG, "[%s] MC-DPSK: Need %d CWs, have %d - partial", log_prefix_.c_str(), total_cw, avail_cw);
        return result;
    }

    // Set up codeword status for reassembly
    v2::CodewordStatus cw_status;
    cw_status.decoded.resize(total_cw, false);
    cw_status.data.resize(total_cw);
    cw_status.decoded[0] = true;
    cw_status.data[0] = data0;

    // Decode CW1+
    for (int i = 1; i < total_cw; i++) {
        size_t off = i * LDPC_BLOCK;
        std::vector<float> bits(soft_bits.begin() + off, soft_bits.begin() + off + LDPC_BLOCK);

        auto [ok, data] = codec_->decode(bits);
        if (ok && data.size() >= bytes_per_cw) {
            data.resize(bytes_per_cw);
            cw_status.decoded[i] = true;
            cw_status.data[i] = data;
            result.codewords_ok++;
        } else {
            result.codewords_failed++;
        }
    }

    if (cw_status.allSuccess()) {
        result.success = true;
        result.frame_data = cw_status.reassemble();
        LOG_MODEM(DEBUG, "[%s] MC-DPSK: %d/%d CWs decoded OK", log_prefix_.c_str(), total_cw, total_cw);
    } else {
        LOG_MODEM(DEBUG, "[%s] MC-DPSK: %d/%d CWs failed", log_prefix_.c_str(),
                  result.codewords_failed, total_cw);
    }

    return result;
}

DecodeResult StreamingDecoder::decodeFrame(const std::vector<float>& soft_bits, float snr, float cfo) {
    DecodeResult result;
    result.snr_db = snr;
    result.cfo_hz = cfo;

    constexpr size_t LDPC_BLOCK = v2::LDPC_CODEWORD_BITS;
    constexpr size_t FRAME_INTERLEAVE_BITS = fec::FrameInterleaver::TOTAL_FRAME_BITS;  // 2592

    if (soft_bits.size() < LDPC_BLOCK) return result;

    CodeRate rate = connected_ ? code_rate_ : CodeRate::R1_4;
    size_t bytes_per_cw = v2::getBytesPerCodeword(rate);
    codec_->setRate(rate);

    // Channel interleaving only applies to OFDM modes, NOT MC-DPSK
    bool is_ofdm = protocol::isOFDMMode(mode_);
    bool apply_channel_deinterleave = use_channel_interleave_ && is_ofdm;

    // Helper to deinterleave a codeword if needed
    auto deinterleave_cw = [&](const std::vector<float>& cw) -> std::vector<float> {
        if (apply_channel_deinterleave) {
            return interleaver_->deinterleave(cw);
        }
        return cw;
    };

    // ========================================================================
    // MC-DPSK: Simple sequential decode (no frame interleaving ever)
    // OFDM: "Try Both" strategy with frame interleaving for 4-CW frames
    // ========================================================================

    // For MC-DPSK, ALWAYS use simple sequential decode - skip all frame interleaving logic
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        return decodeMCDPSKFrame(soft_bits, rate, bytes_per_cw, snr, cfo);
    }

    // ========================================================================
    // OFDM: "Try Both" Strategy for Frame Interleaving
    // ========================================================================
    // 1. First, try to decode first 648 bits as non-interleaved CW0
    // 2. If it's a valid 1-CW control frame → done
    // 3. If decode fails OR it's a 4-CW frame → try frame-interleaved decode
    // ========================================================================

    // Skip single-CW control probes when we already know this is a 4-CW data
    // frame. Burst-interleaved logical frames and frames latched by CW0 peek
    // are data-only; probing them as R1/4 control burns LDPC time on the Pi
    // before the real fixed-frame decode starts.
    const bool known_4cw =
        (pending_total_cw_ == v2::FIXED_FRAME_CODEWORDS) ||
        (use_burst_interleave_ && waveform_ && waveform_->wasBurstInterleaved());

    // R1/4 fast-path: control frames are always encoded at R1/4 (hardened)
    // Try R1/4 first — if it's a valid 1-CW control frame, return immediately.
    if (!known_4cw && rate != CodeRate::R1_4 && soft_bits.size() >= LDPC_BLOCK) {
        codec_->setRate(CodeRate::R1_4);
        std::vector<float> cw0_r14(soft_bits.begin(), soft_bits.begin() + LDPC_BLOCK);
        std::pair<bool, Bytes> probe;
        {
            ultra::timing::ScopedTimer _profile_(
                ultra::timing::globalDecoderProfile().ofdm_cw0_probe_decode);
            probe = codec_->decode(cw0_r14);
        }
        auto [ok_r14, data_r14] = probe;
        size_t bpc_r14 = v2::getBytesPerCodeword(CodeRate::R1_4);

        if (ok_r14 && data_r14.size() >= 2
            && data_r14[0] == 0x55 && data_r14[1] == 0x4C) {
            if (data_r14.size() > bpc_r14) data_r14.resize(bpc_r14);
            auto hdr_r14 = v2::parseHeader(data_r14);
            if (hdr_r14.valid && hdr_r14.total_cw == 1) {
                LOG_MODEM(INFO, "[%s] R1/4 control fast-path OK", log_prefix_.c_str());
                result.success = true;
                result.codewords_ok = 1;
                result.frame_data = data_r14;
                result.frame_type = hdr_r14.type;
                return result;
            }
        }
        // Restore rate for remaining decode paths
        codec_->setRate(rate);
    }

    // Step 1: Try to decode CW0 RAW (no channel deinterleave)
    // Control frames (ACK etc.) are never channel-interleaved, so probe without it.
    // If this is a 4-CW data frame (which IS interleaved), CW0 will likely fail here
    // and we'll fall through to decodeFixedFrame() which handles deinterleaving internally.
    //
    // Skip the probe entirely when we already know it's a 4-CW interleaved data frame:
    //   (a) prior CW0 peek set pending_total_cw_ to FIXED_FRAME_CODEWORDS, or
    //   (b) burst-interleave marker was latched (frame is part of a burst-interleaved
    //       group, definitely 4-CW data, never a control frame).
    // This was identified as redundant work in profiling plan v5 + ChatGPT review.
    std::pair<bool, Bytes> raw_probe;
    bool ok0 = false;
    Bytes data0;
    if (known_4cw) {
        ultra::timing::globalDecoderProfile()
            .raw_cw0_probe_skipped.fetch_add(1, std::memory_order_relaxed);
        // Fall through directly to frame-interleaved decode below.
    } else {
        std::vector<float> cw0_bits(soft_bits.begin(), soft_bits.begin() + LDPC_BLOCK);
        {
            ultra::timing::ScopedTimer _profile_(
                ultra::timing::globalDecoderProfile().ofdm_cw0_probe_decode);
            raw_probe = codec_->decode(cw0_bits);
        }
        ok0 = raw_probe.first;
        data0 = std::move(raw_probe.second);
    }

    bool try_frame_interleave = known_4cw;  // skip probe → go straight to interleaved decode

    if (ok0 && data0.size() >= 2 && data0[0] == 0x55 && data0[1] == 0x4C) {
        // CW0 decoded and has valid magic - check if it's a control frame
        if (data0.size() > bytes_per_cw) data0.resize(bytes_per_cw);

        auto hdr = v2::parseHeader(data0);
        if (hdr.valid) {
            result.frame_type = hdr.type;

            if (hdr.total_cw == 1) {
                // === Control frame (1 CW) - no frame interleaving ===
                LOG_MODEM(DEBUG, "[%s] Control frame decoded (1 CW)", log_prefix_.c_str());
                result.success = true;
                result.codewords_ok = 1;
                result.frame_data = data0;
                return result;
            } else if (hdr.total_cw == v2::FIXED_FRAME_CODEWORDS) {
                // === Multi-CW frame with 4 CWs - try frame interleaving ===
                LOG_MODEM(DEBUG, "[%s] Header shows 4 CWs - trying frame deinterleave", log_prefix_.c_str());
                try_frame_interleave = true;
            } else {
                // === Multi-CW frame with != 4 CWs - old format, no frame interleaving ===
                // Fall through to legacy decode path
                LOG_MODEM(DEBUG, "[%s] Multi-CW frame (%d CWs) - legacy decode", log_prefix_.c_str(), hdr.total_cw);
            }
        }
    } else {
        // CW0 decode failed or invalid magic - might be interleaved
        LOG_MODEM(DEBUG, "[%s] CW0 decode failed/invalid - trying frame deinterleave", log_prefix_.c_str());
        try_frame_interleave = true;
    }

    // Step 2: Try frame-interleaved decode if needed
    if (try_frame_interleave && soft_bits.size() >= FRAME_INTERLEAVE_BITS) {
        LOG_MODEM(DEBUG, "[%s] Attempting 4-CW frame deinterleave decode", log_prefix_.c_str());

        // Use v2::decodeFixedFrame which handles frame + channel deinterleaving + LDPC decode
        // Channel deinterleaving restores the original bit order within each CW
        // Only enable for OFDM modes (MC-DPSK doesn't use channel interleaving)
        size_t bps = ofdm_data_carriers_ * getBitsPerSymbol(current_modulation_);
        const auto _profile_fs_start_ = std::chrono::steady_clock::now();
        auto cw_status = v2::decodeFixedFrame(soft_bits, rate, apply_channel_deinterleave, bps);
        // Record duration into failed_4cw_after_peek if this attempt failed.
        // Note: this duration also counts inside decode_fixed_frame_total
        // (and ldpc_cw_total inside that) — it's an approximate subset.
        if (!cw_status.allSuccess()) {
            const uint64_t _profile_fs_us_ = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - _profile_fs_start_).count());
            ultra::timing::globalDecoderProfile()
                .failed_4cw_after_peek.addSample(_profile_fs_us_);
        }

        result.codewords_ok = 0;
        result.codewords_failed = 0;
        for (int i = 0; i < v2::FIXED_FRAME_CODEWORDS; ++i) {
            if (cw_status.decoded[i]) {
                result.codewords_ok++;
            } else {
                result.codewords_failed++;
            }
        }

        if (cw_status.allSuccess()) {
            result.success = true;
            result.frame_data = cw_status.reassemble();

            if (result.frame_data.empty()) {
                // LDPC said 4/4 OK but reassemble failed — likely LDPC false positive
                result.success = false;
                LOG_MODEM(WARN, "[%s] Frame deinterleave: 4/4 CWs OK but reassemble FAILED (LDPC false positive?)",
                          log_prefix_.c_str());
            } else {
                // Parse header to get frame type
                if (result.frame_data.size() >= 3) {
                    result.frame_type = static_cast<v2::FrameType>(result.frame_data[2]);
                }
            }

            LOG_MODEM(INFO, "[%s] Frame deinterleave decode SUCCESS (%d/%d CWs, data=%zu bytes)",
                      log_prefix_.c_str(), result.codewords_ok, v2::FIXED_FRAME_CODEWORDS,
                      result.frame_data.size());
            return result;
        } else {
            LOG_MODEM(DEBUG, "[%s] Frame deinterleave decode FAILED (%d/%d CWs)",
                      log_prefix_.c_str(), result.codewords_ok, v2::FIXED_FRAME_CODEWORDS);

            // Step 2b: If frame deinterleave failed, check if it's a 1-CW control frame
            // Catches ACK frames that were escalated to 4-CW by a failed peek
            // Try R1/4 first (control frames hardened), then code_rate_ fallback
            {
                auto trySalvage = [&](CodeRate sr) -> bool {
                    size_t bpc = v2::getBytesPerCodeword(sr);
                    auto [rec_ok, rec_data] = robustDecodeSingleCW(
                        soft_bits.data(), LDPC_BLOCK, sr, log_prefix_.c_str());
                    if (rec_ok && rec_data.size() >= 2
                        && rec_data[0] == 0x55 && rec_data[1] == 0x4C) {
                        if (rec_data.size() > bpc) rec_data.resize(bpc);
                        auto hdr = v2::parseHeader(rec_data);
                        if (hdr.valid && hdr.total_cw == 1) {
                            g_salvage_hits.fetch_add(1, std::memory_order_relaxed);
                            LOG_MODEM(INFO, "[%s] Salvaged 1-CW control (rate=%d, total_hits=%d)",
                                      log_prefix_.c_str(), static_cast<int>(sr),
                                      g_salvage_hits.load(std::memory_order_relaxed));
                            result.success = true;
                            result.codewords_ok = 1;
                            result.codewords_failed = 0;
                            result.frame_data = rec_data;
                            result.frame_type = hdr.type;
                            return true;
                        }
                    }
                    return false;
                };

                if (trySalvage(CodeRate::R1_4))
                    return result;
                if (rate != CodeRate::R1_4 && trySalvage(rate))
                    return result;
            }
        }
    }

    // Step 3: Legacy decode path (non-interleaved multi-CW frames)
    // This handles old-format frames or when frame interleaving is disabled
    if (ok0 && data0.size() >= 2 && data0[0] == 0x55 && data0[1] == 0x4C) {
        if (data0.size() > bytes_per_cw) data0.resize(bytes_per_cw);
        result.codewords_ok = 1;

        auto hdr = v2::parseHeader(data0);
        if (!hdr.valid) return result;

        result.frame_type = hdr.type;
        int total_cw = hdr.total_cw;
        int avail_cw = static_cast<int>(soft_bits.size() / LDPC_BLOCK);

        if (avail_cw < total_cw) {
            result.frame_data = data0;
            return result;
        }

        v2::CodewordStatus cw_status;
        cw_status.decoded.resize(total_cw, false);
        cw_status.data.resize(total_cw);
        cw_status.decoded[0] = true;
        cw_status.data[0] = data0;

        for (int i = 1; i < total_cw; i++) {
            size_t off = i * LDPC_BLOCK;
            std::vector<float> bits(soft_bits.begin() + off, soft_bits.begin() + off + LDPC_BLOCK);
            bits = deinterleave_cw(bits);

            auto [ok, data] = codec_->decode(bits);
            if (ok && data.size() >= bytes_per_cw) {
                data.resize(bytes_per_cw);
                cw_status.decoded[i] = true;
                cw_status.data[i] = data;
                result.codewords_ok++;
            } else {
                result.codewords_failed++;
            }
        }

        if (cw_status.allSuccess()) {
            result.success = true;
            result.frame_data = cw_status.reassemble();
        }
    }

    return result;
}

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

    // Check available samples at burst_next_pos_
    size_t next_available;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (write_pos_ >= burst_next_pos_) {
            next_available = write_pos_ - burst_next_pos_;
        } else {
            next_available = MAX_BUFFER_SAMPLES - burst_next_pos_ + write_pos_;
        }
    }

    if (next_available < burst_min_block_) {
        return BurstFrameResult::WAITING;
    }

    // Copy samples from circular buffer
    std::vector<float> block(burst_min_block_);
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        for (size_t i = 0; i < burst_min_block_; i++) {
            block[i] = buffer_[(burst_next_pos_ + i) % MAX_BUFFER_SAMPLES];
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
        burst_soft_buffer_.emplace_back(fec::BurstInterleaver::BITS_PER_FRAME, 0.0f);
        burst_next_pos_ = (burst_next_pos_ + burst_min_block_) % MAX_BUFFER_SAMPLES;
        return BurstFrameResult::SUCCESS;
    }

    // Pre-correct CFO on burst block
    bool is_ofdm_burst = protocol::isOFDMMode(mode_);
    float burst_pre_cfo = 0.0f;
    if (is_ofdm_burst) {
        size_t abs_burst = burst_next_pos_;
        if (total_fed_ >= MAX_BUFFER_SAMPLES) {
            const size_t oldest_abs = total_fed_ - MAX_BUFFER_SAMPLES;
            const size_t oldest_pos = write_pos_;
            const size_t offset = (burst_next_pos_ >= oldest_pos)
                ? (burst_next_pos_ - oldest_pos)
                : (MAX_BUFFER_SAMPLES - oldest_pos + burst_next_pos_);
            abs_burst = oldest_abs + offset;
        }
        burst_pre_cfo = applyCFOPreCorrection(block, burst_cfo_, abs_burst);
    }

    // Demodulate (CFO=0 after pre-correction, or original burst_cfo_ if no pre-correction)
    float burst_decode_cfo = (std::abs(burst_pre_cfo) > 0.01f) ? 0.0f : burst_cfo_;
    waveform_->setFrequencyOffset(burst_decode_cfo);
    bool ok = waveform_->process(SampleSpan(block.data(), block.size()));
    if (!ok) {
        LOG_MODEM(WARN, "[%s] Burst frame %zu/%d: process() failed, inserting erasure",
                  log_prefix_.c_str(), burst_soft_buffer_.size() + 1, burst_group_size);
        burst_soft_buffer_.emplace_back(fec::BurstInterleaver::BITS_PER_FRAME, 0.0f);
        burst_next_pos_ = (burst_next_pos_ + burst_min_block_) % MAX_BUFFER_SAMPLES;
        return BurstFrameResult::SUCCESS;
    }
    captureConstellationSnapshot();

    auto soft = waveform_->getSoftBits();
    if (soft.empty()) {
        LOG_MODEM(WARN, "[%s] Burst frame %zu/%d: empty soft bits, inserting erasure",
                  log_prefix_.c_str(), burst_soft_buffer_.size() + 1, burst_group_size);
        burst_soft_buffer_.emplace_back(fec::BurstInterleaver::BITS_PER_FRAME, 0.0f);
        burst_next_pos_ = (burst_next_pos_ + burst_min_block_) % MAX_BUFFER_SAMPLES;
        return BurstFrameResult::SUCCESS;
    }

    // Update CFO from pilot tracking (add pre-correction amount back)
    float residual_cfo = waveform_->estimatedCFO();
    float corrected_cfo = burst_pre_cfo + residual_cfo;
    float drift = corrected_cfo - burst_cfo_;
    constexpr float MAX_BURST_CFO_DRIFT_HZ = 2.0f;
    if (std::abs(drift) > MAX_BURST_CFO_DRIFT_HZ) {
        corrected_cfo = burst_cfo_ + std::copysign(MAX_BURST_CFO_DRIFT_HZ, drift);
    }
    burst_cfo_ = corrected_cfo;
    last_cfo_.store(corrected_cfo);
    last_fading_index_.store(waveform_->getFadingIndex());

    burst_soft_buffer_.push_back(std::move(soft));
    burst_next_pos_ = (burst_next_pos_ + burst_min_block_) % MAX_BUFFER_SAMPLES;

    LOG_MODEM(INFO, "[%s] Burst frame %zu/%d demodulated, RMS=%.4f",
              log_prefix_.c_str(), burst_soft_buffer_.size(),
              burst_group_size, next_rms);
    return BurstFrameResult::SUCCESS;
}

void StreamingDecoder::finalizeBurstGroup() {
    const int burst_group_size = std::max(2, burst_group_size_);
    LOG_MODEM(INFO, "[%s] Burst group complete (%d frames), deinterleaving...",
              log_prefix_.c_str(), burst_group_size);

    auto logical_soft = fec::BurstInterleaver::deinterleave(burst_soft_buffer_);

    for (int i = 0; i < burst_group_size; i++) {
        const int saved_pending_total_cw = pending_total_cw_;
        pending_total_cw_ = v2::FIXED_FRAME_CODEWORDS;
        DecodeResult result = decodeFrame(logical_soft[i], burst_snr_, burst_cfo_);
        pending_total_cw_ = saved_pending_total_cw;

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

// Legacy methods - do nothing but required by header
bool StreamingDecoder::runCorrelationSearch(size_t) { return false; }
bool StreamingDecoder::tryDecodeFrame() { return false; }
std::vector<float> StreamingDecoder::copySamplesFrom(size_t, size_t) { return {}; }
size_t StreamingDecoder::samplesAvailableFrom(size_t) const { return 0; }
bool StreamingDecoder::isPingOnly(const std::vector<float>&, size_t) { return false; }
void StreamingDecoder::updateNoiseFloor(const float*, size_t) {}

} // namespace gui
} // namespace ultra
