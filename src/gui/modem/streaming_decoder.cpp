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
#include "waveform/ofdm_cox_waveform.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
#include "fec/frame_interleaver.hpp"  // Frame-level interleaving for 4-CW frames
#include "ultra/logging.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>

namespace ultra {
namespace gui {

namespace v2 = protocol::v2;

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
    buffer_.resize(MAX_BUFFER_SAMPLES, 0.0f);
    waveform_ = WaveformFactory::create(protocol::WaveformMode::MC_DPSK);
    interleaver_ = std::make_unique<ChannelInterleaver>(16, v2::LDPC_CODEWORD_BITS);
    codec_ = fec::CodecFactory::create(fec::CodecType::LDPC, CodeRate::R1_4);

    LOG_MODEM(INFO, "StreamingDecoder: Initialized (buffer=%zu samples)", MAX_BUFFER_SAMPLES);
}

StreamingDecoder::~StreamingDecoder() {
    stop();
}

// ============================================================================
// AUDIO THREAD - Just buffer samples, nothing else
// ============================================================================

void StreamingDecoder::feedAudio(const float* samples, size_t count) {
    if (!samples || count == 0) return;

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

    // If adding these samples would overflow, advance correlation_pos_
    // to make room (dropping oldest unsearched data)
    if (unsearched + count >= MAX_BUFFER_SAMPLES) {
        size_t need_to_drop = (unsearched + count) - MAX_BUFFER_SAMPLES + 1000;  // margin
        correlation_pos_ = (correlation_pos_ + need_to_drop) % MAX_BUFFER_SAMPLES;
        LOG_MODEM(WARN, "[%s] Buffer overflow, dropped %zu unsearched samples (corr_pos=%zu)",
                  log_prefix_.c_str(), need_to_drop, correlation_pos_);
    }

    // Write samples to circular buffer
    for (size_t i = 0; i < count; i++) {
        buffer_[write_pos_] = samples[i];
        write_pos_ = (write_pos_ + 1) % MAX_BUFFER_SAMPLES;
    }

    total_fed_ += count;

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
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        data_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
            return shutdown_.load() || new_data_available_;
        });
        if (shutdown_.load()) return;
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
    }
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
    constexpr size_t LIGHT_SEARCH_SIZE = 24000;    // ~0.5s for LTS detection

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

        // Skip if clearly just noise (RMS < 0.05)
        if (rms < CORR_NOISE_THRESHOLD) {
            // No signal - advance by small step (100ms = 4800 samples)
            static int rms_skip_count = 0;
            if (++rms_skip_count % 10 == 1)
                LOG_MODEM(INFO, "[%s] searchForSync: RMS skip, rms=%.4f < %.2f, corr_pos=%zu, total=%.2fs",
                          log_prefix_.c_str(), rms, CORR_NOISE_THRESHOLD, correlation_pos_, audio_sec);
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
        constexpr size_t LEAD_IN_SAMPLES = 7200;  // 150ms TX lead-in
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

    // When connected, try light sync detection first (training only, no chirp)
    // This matches the TX side which uses generateDataPreamble() when connected
    // If light sync fails, fall back to full chirp sync (handles first frame or resync)
    //
    // IMPORTANT: Light sync can produce false positives from channel effects (corr ~0.3-0.4)
    // Real training symbols have autocorrelation ~0.8+ on decent channels.
    // On fading channels, low correlation (0.5-0.8) often indicates timing error
    // which causes complete frame failure. Use 0.8 threshold to force fallback
    // to chirp sync for marginal cases.
    constexpr float LIGHT_SYNC_CONFIDENCE = 0.8f;

    if (connected_ && waveform_->supportsDataPreamble()) {
        float known_cfo = last_cfo_.load();
        found = waveform_->detectDataSync(
            SampleSpan(search_buffer.data(), search_buffer.size()),
            sync_result, known_cfo, CORR_DETECT_THRESHOLD);

        // Check confidence - low correlation likely means false positive in a chirp signal
        if (found && sync_result.correlation < LIGHT_SYNC_CONFIDENCE) {
            LOG_MODEM(INFO, "[%s] DATA sync low confidence (corr=%.2f < %.2f), falling back to chirp",
                      log_prefix_.c_str(), sync_result.correlation, LIGHT_SYNC_CONFIDENCE);
            found = false;  // Reject and try chirp sync instead
        }

        if (found) {
            LOG_MODEM(INFO, "[%s] DATA sync detected (training only, known CFO=%.1f Hz, corr=%.2f)",
                      log_prefix_.c_str(), known_cfo, sync_result.correlation);
        } else if (search_buffer.size() >= chirp_min_search) {
            // Light sync failed — only try chirp fallback if buffer is large enough
            found = waveform_->detectSync(
                SampleSpan(search_buffer.data(), search_buffer.size()),
                sync_result, CORR_DETECT_THRESHOLD);
            if (found) {
                LOG_MODEM(INFO, "[%s] Fallback to CHIRP sync (CFO=%.1f Hz)",
                          log_prefix_.c_str(), sync_result.cfo_hz);
            }
        }
    } else {
        // Use full sync detection with chirp
        found = waveform_->detectSync(
            SampleSpan(search_buffer.data(), search_buffer.size()),
            sync_result, CORR_DETECT_THRESHOLD);
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
        sync_start_time_ = std::chrono::steady_clock::now();
        pending_total_cw_ = 0;

        state_ = DecoderState::SYNC_FOUND;

        last_snr_.store(sync_snr_);
        last_cfo_.store(sync_cfo_);

        LOG_MODEM(INFO, "[%s] SYNC at pos=%zu, CFO=%.1f Hz, SNR=%.1f dB",
                  log_prefix_.c_str(), sync_position_, sync_cfo_, sync_snr_);

        // Skip past the entire frame for next search
        // Use frame size (from sync_position_) to avoid re-detecting the same frame
        size_t frame_size = static_cast<size_t>(waveform_->getMinSamplesForFrame());
        size_t skip_amount = std::max(min_search, frame_size + 4800);  // frame + 100ms margin
        size_t skip_to = (sync_position_ + skip_amount) % MAX_BUFFER_SAMPLES;
        // Don't jump ahead of actual data - cap at write_pos_
        if (total_fed_ < MAX_BUFFER_SAMPLES && skip_to > write_pos_) {
            skip_to = write_pos_;
        }
        correlation_pos_ = skip_to;
    }
}

void StreamingDecoder::checkIfReadyToDecode() {
    if (!waveform_) {
        state_ = DecoderState::SEARCHING;
        return;
    }

    size_t min_frame = static_cast<size_t>(waveform_->getMinSamplesForFrame());

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

    // Calculate how much we need
    size_t needed = min_frame;
    if (pending_total_cw_ > 1) {
        size_t cw_data = (min_frame * 9) / 10;
        needed = min_frame + (pending_total_cw_ - 1) * cw_data;
    }

    if (available >= needed) {
        state_ = DecoderState::DECODING;
    }
}

void StreamingDecoder::decodeCurrentFrame() {
    if (!waveform_) {
        state_ = DecoderState::SEARCHING;
        return;
    }

    size_t min_frame = static_cast<size_t>(waveform_->getMinSamplesForFrame());

    // Copy frame samples from buffer
    std::vector<float> frame_buffer;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        size_t available;
        if (write_pos_ >= sync_position_) {
            available = write_pos_ - sync_position_;
        } else {
            available = MAX_BUFFER_SAMPLES - sync_position_ + write_pos_;
        }

        size_t frame_len = min_frame;
        if (pending_total_cw_ > 1) {
            size_t cw_data = (min_frame * 9) / 10;
            frame_len = min_frame + (pending_total_cw_ - 1) * cw_data;
        }
        frame_len = std::min(frame_len, available);

        frame_buffer.resize(frame_len);
        for (size_t i = 0; i < frame_len; i++) {
            frame_buffer[i] = buffer_[(sync_position_ + i) % MAX_BUFFER_SAMPLES];
        }
    }

    if (frame_buffer.empty()) {
        state_ = DecoderState::SEARCHING;
        return;
    }

    // Check for PING (low energy after sync = chirp only, no data)
    // For MC-DPSK: training=4096, ref=512, so data starts at 4608
    // We need to check AFTER training region to detect PING correctly
    size_t training_skip = 0;
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        training_skip = 4608;  // training + ref samples
    } else {
        // OFDM: check after preamble (2 LTS = ~1024 samples)
        training_skip = 1024;
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

    // PING threshold: noise floor ~0.01, faded data ~0.07, normal data ~0.15
    // Use 0.04 to avoid misclassifying faded data frames as PINGs
    constexpr float PING_RMS_THRESHOLD = 0.04f;
    LOG_MODEM(INFO, "[%s] PING check: RMS=%.4f (threshold=%.2f), sync_pos=%zu, check_start=%zu",
              log_prefix_.c_str(), rms, PING_RMS_THRESHOLD, sync_position_, check_start);

    if (rms < PING_RMS_THRESHOLD) {
        // PING detected
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

        // Skip past the PING (sync_position_ + min_frame)
        // Don't skip to write_pos_ - that would miss frames already in buffer
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            correlation_pos_ = (sync_position_ + min_frame) % MAX_BUFFER_SAMPLES;
        }

        state_ = DecoderState::SEARCHING;
        return;
    }

    // Data frame - decode
    waveform_->setFrequencyOffset(sync_cfo_);

    auto decode_start = std::chrono::steady_clock::now();
    bool ok = waveform_->process(SampleSpan(frame_buffer.data(), frame_buffer.size()));

    if (!ok) {
        LOG_MODEM(DEBUG, "[%s] process() failed", log_prefix_.c_str());
        state_ = DecoderState::SEARCHING;
        return;
    }

    auto soft_bits = waveform_->getSoftBits();
    if (soft_bits.empty()) {
        LOG_MODEM(DEBUG, "[%s] getSoftBits() returned empty", log_prefix_.c_str());
        state_ = DecoderState::SEARCHING;
        return;
    }
    LOG_MODEM(INFO, "[%s] Got %zu soft bits, proceeding to decode", log_prefix_.c_str(), soft_bits.size());

    last_fading_index_.store(waveform_->getFadingIndex());

    // Feed back pilot-corrected CFO to cached value
    // Chirp-based CFO can be wrong on fading channels. The demodulator uses
    // pilot tracking to refine it. Update our cached CFO so subsequent frames
    // don't re-inject the wrong chirp CFO.
    // CRITICAL: On fading channels, pilot tracking can give wildly wrong CFO
    // estimates (e.g. 31 Hz when real CFO is 0). Clamp the drift to prevent
    // the feedback loop from oscillating.
    float corrected_cfo = waveform_->estimatedCFO();
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
        LOG_MODEM(INFO, "[%s] CFO updated: %.2f → %.2f Hz (pilot-corrected)",
                  log_prefix_.c_str(), current_cfo, corrected_cfo);
    }
    last_cfo_.store(corrected_cfo);
    sync_cfo_ = corrected_cfo;

    // Check if we need more codewords
    constexpr size_t LDPC_BLOCK = v2::LDPC_CODEWORD_BITS;

    if (pending_total_cw_ == 0 && soft_bits.size() >= LDPC_BLOCK) {
        // Peek at header
        std::vector<float> cw0(soft_bits.begin(), soft_bits.begin() + LDPC_BLOCK);

        // Channel deinterleaving for OFDM modes (spreads fading errors across LDPC codeword)
        // TEMPORARILY DISABLED FOR TESTING
        bool use_interleave = false;
        (void)((mode_ == protocol::WaveformMode::OFDM_CHIRP ||
                mode_ == protocol::WaveformMode::OFDM_COX));
        if (use_interleave && interleaver_) {
            cw0 = interleaver_->deinterleave(cw0);
        }

        CodeRate rate = connected_ ? code_rate_ : CodeRate::R1_4;
        codec_->setRate(rate);
        auto [ok, data] = codec_->decode(cw0);

        if (ok && data.size() >= 4 && data[0] == 0x55 && data[1] == 0x4C) {
            auto hdr = v2::parseHeader(data);
            if (hdr.valid) {
                int avail_cw = static_cast<int>(soft_bits.size() / LDPC_BLOCK);
                if (avail_cw < hdr.total_cw) {
                    pending_total_cw_ = hdr.total_cw;
                    state_ = DecoderState::SYNC_FOUND;
                    LOG_MODEM(INFO, "[%s] Need %d codewords, have %d - waiting",
                              log_prefix_.c_str(), hdr.total_cw, avail_cw);
                    return;
                }
            }
        }
    }

    // Decode full frame
    DecodeResult result = decodeFrame(soft_bits, sync_snr_, sync_cfo_);

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

    // Skip past the frame we just decoded
    // sync_position_ is at training start (after chirp), frame_buffer.size() is frame data
    // Don't skip to write_pos_ - that would miss frames already in buffer
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        correlation_pos_ = (sync_position_ + frame_buffer.size()) % MAX_BUFFER_SAMPLES;
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
    if (mode == protocol::WaveformMode::OFDM_CHIRP || mode == protocol::WaveformMode::OFDM_COX) {
        bps = 60;
    }
    interleaver_ = std::make_unique<ChannelInterleaver>(bps, v2::LDPC_CODEWORD_BITS);

    state_ = DecoderState::SEARCHING;
    pending_total_cw_ = 0;

    // CRITICAL: Reset correlation_pos_ to current write position
    // Otherwise we'll search old data from previous mode
    correlation_pos_ = write_pos_;

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

    // Create appropriate OFDM waveform based on current mode
    if (mode_ == protocol::WaveformMode::OFDM_CHIRP) {
        waveform_ = std::make_unique<OFDMChirpWaveform>(config);
        LOG_MODEM(INFO, "StreamingDecoder: OFDM_CHIRP config set (FFT=%d, carriers=%d)",
                  config.fft_size, config.num_carriers);
    } else {
        waveform_ = std::make_unique<OFDMNvisWaveform>(config);
        LOG_MODEM(INFO, "StreamingDecoder: OFDM_COX config set (FFT=%d, carriers=%d)",
                  config.fft_size, config.num_carriers);
    }

    // Store carrier count for interleaver updates
    // Use getDataCarriers() to account for pilot overhead
    ofdm_carriers_ = config.num_carriers;
    ofdm_data_carriers_ = config.getDataCarriers();

    // Update interleaver for new carrier count (using current modulation)
    size_t bps = ofdm_data_carriers_ * getBitsPerSymbol(current_modulation_);
    interleaver_ = std::make_unique<ChannelInterleaver>(bps, v2::LDPC_CODEWORD_BITS);
}

void StreamingDecoder::setDataMode(Modulation mod, CodeRate rate) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    code_rate_ = rate;
    current_modulation_ = mod;
    if (waveform_) waveform_->configure(mod, rate);

    // After configure(), the waveform has updated pilot config
    // Recalculate data carriers based on new code rate's pilot requirement
    if (mode_ != protocol::WaveformMode::MC_DPSK && waveform_) {
        // Calculate pilot count to match OFDMChirpWaveform::configurePilotsForCodeRate()
        // ALL code rates now use pilots for per-symbol channel tracking on fading
        int pilot_spacing = 0;
        switch (rate) {
            case CodeRate::R3_4: pilot_spacing = 15; break;  // ~4 pilots
            default: pilot_spacing = 10; break;  // R2/3, R1/2, R1/4: 6 pilots
        }
        int pilot_count = (ofdm_carriers_ + pilot_spacing - 1) / pilot_spacing;
        ofdm_data_carriers_ = ofdm_carriers_ - pilot_count;
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

    // Get pilot config from the OFDM config if we have a waveform
    // Note: These defaults match OFDMChirpWaveform with 59 carriers, spacing 10
    cfg.use_pilots = true;
    cfg.pilot_spacing = 10;

    // Interleaving settings
    cfg.use_channel_interleave = use_channel_interleave_;
    cfg.use_frame_interleave = (mode_ == protocol::WaveformMode::OFDM_CHIRP ||
                                 mode_ == protocol::WaveformMode::OFDM_COX);

    return cfg;
}

size_t StreamingDecoder::samplesInBuffer() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return std::min(total_fed_, MAX_BUFFER_SAMPLES);
}

bool StreamingDecoder::isSynced() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return state_ == DecoderState::SYNC_FOUND || state_ == DecoderState::DECODING;
}

std::vector<std::complex<float>> StreamingDecoder::getConstellationSymbols() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (waveform_) {
        return waveform_->getConstellationSymbols();
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
    samples_since_sync_ = 0;
    total_fed_ = 0;
    feed_iter_ = 0;
    state_ = DecoderState::SEARCHING;
    pending_total_cw_ = 0;
    new_data_available_ = false;
    last_decoded_sync_pos_ = SIZE_MAX;

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
    // Disabled by default due to BUG-006 (CW1 consistently fails when enabled)
    // Set via setChannelInterleave() to match TX encoder setting
    bool is_ofdm = (mode_ == protocol::WaveformMode::OFDM_CHIRP ||
                    mode_ == protocol::WaveformMode::OFDM_COX);
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

    // Step 1: Try to decode CW0 (with channel deinterleaving if OFDM)
    std::vector<float> cw0_bits(soft_bits.begin(), soft_bits.begin() + LDPC_BLOCK);
    cw0_bits = deinterleave_cw(cw0_bits);
    auto [ok0, data0] = codec_->decode(cw0_bits);

    bool try_frame_interleave = false;

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
        auto cw_status = v2::decodeFixedFrame(soft_bits, rate, apply_channel_deinterleave);

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

            // Parse header to get frame type
            if (result.frame_data.size() >= 3) {
                result.frame_type = static_cast<v2::FrameType>(result.frame_data[2]);
            }

            LOG_MODEM(INFO, "[%s] Frame deinterleave decode SUCCESS (%d/%d CWs)",
                      log_prefix_.c_str(), result.codewords_ok, v2::FIXED_FRAME_CODEWORDS);
            return result;
        } else {
            LOG_MODEM(DEBUG, "[%s] Frame deinterleave decode FAILED (%d/%d CWs)",
                      log_prefix_.c_str(), result.codewords_ok, v2::FIXED_FRAME_CODEWORDS);
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

// Legacy methods - do nothing but required by header
bool StreamingDecoder::runCorrelationSearch(size_t) { return false; }
bool StreamingDecoder::tryDecodeFrame() { return false; }
std::vector<float> StreamingDecoder::copySamplesFrom(size_t, size_t) { return {}; }
size_t StreamingDecoder::samplesAvailableFrom(size_t) const { return 0; }
bool StreamingDecoder::isPingOnly(const std::vector<float>&, size_t) { return false; }
void StreamingDecoder::updateNoiseFloor(const float*, size_t) {}

} // namespace gui
} // namespace ultra
