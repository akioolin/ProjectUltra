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
        fprintf(stderr, "[SD-DEBUG] Failed to create dump file: %s\n", filename);
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

    fprintf(stderr, "[SD-DEBUG] Dumped %s: %zu samples to %s (RMS=%.4f, peak=%.4f)\n",
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

    // Get preamble size from waveform (chirp length)
    size_t preamble = static_cast<size_t>(waveform_->getPreambleSamples());

    // Minimum samples needed for one search window
    // Need enough for the full dual chirp + margin for detection
    size_t min_search = preamble + 20000;

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

            fprintf(stderr, "[SD-DEBUG] Search #%d: dumped %zu samples to %s (start=%zu, RMS=%.4f, peak=%.4f)\n",
                    search_dump_count, search_buffer.size(), filename, search_start, rms, max_val);
        }
        search_dump_count++;
    }

    // Search for sync (no lock held - this is the slow part)
    auto search_start_time = std::chrono::steady_clock::now();

    waveform_->reset();
    SyncResult sync_result;
    bool found = waveform_->detectSync(
        SampleSpan(search_buffer.data(), search_buffer.size()),
        sync_result, CORR_DETECT_THRESHOLD);

    auto search_end_time = std::chrono::steady_clock::now();
    float search_ms = std::chrono::duration<float, std::milli>(search_end_time - search_start_time).count();

    // Log timing: total_fed_ tells us how much audio has arrived
    float audio_sec = total_fed_ / 48000.0f;
    if (found || search_ms > 100) {  // Log if found or if search was slow
        LOG_MODEM(INFO, "[%s] searchForSync: audio=%.2fs, search=%.1fms, found=%d, corr=%.3f",
                  log_prefix_.c_str(), audio_sec, search_ms, found ? 1 : 0, sync_result.correlation);
    }

    if (found) {
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        sync_position_ = (search_start + sync_result.start_sample) % MAX_BUFFER_SAMPLES;
        sync_cfo_ = sync_result.cfo_hz;
        sync_snr_ = estimateSNRFromChirp(sync_result.correlation, noise_floor_);
        sync_start_time_ = std::chrono::steady_clock::now();
        pending_total_cw_ = 0;

        state_ = DecoderState::SYNC_FOUND;

        last_snr_.store(sync_snr_);
        last_cfo_.store(sync_cfo_);

        LOG_MODEM(INFO, "[%s] SYNC at pos=%zu, CFO=%.1f Hz, SNR=%.1f dB",
                  log_prefix_.c_str(), sync_position_, sync_cfo_, sync_snr_);

        // Skip past this sync for next search
        // Don't jump ahead of actual data - cap at write_pos_
        size_t skip_to = (sync_position_ + min_search) % MAX_BUFFER_SAMPLES;
        // If skip_to is ahead of write_pos_ (in a non-wrapped buffer), cap it
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
    float rms = 0.0f;
    size_t check_len = std::min(frame_buffer.size(), size_t(5000));
    for (size_t i = 0; i < check_len; i++) {
        rms += frame_buffer[i] * frame_buffer[i];
    }
    rms = std::sqrt(rms / check_len);

    LOG_MODEM(INFO, "[%s] PING check: RMS=%.4f (threshold=0.08), sync_pos=%zu",
              log_prefix_.c_str(), rms, sync_position_);

    if (rms < 0.08f) {
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

        // CRITICAL: Skip all old audio - only process new audio from now on
        // This prevents re-detecting old chirps that are still in the circular buffer
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            correlation_pos_ = write_pos_;
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

    // Check if we need more codewords
    constexpr size_t LDPC_BLOCK = v2::LDPC_CODEWORD_BITS;

    if (pending_total_cw_ == 0 && soft_bits.size() >= LDPC_BLOCK) {
        // Peek at header
        std::vector<float> cw0(soft_bits.begin(), soft_bits.begin() + LDPC_BLOCK);

        bool use_interleave = (mode_ == protocol::WaveformMode::OFDM_CHIRP ||
                               mode_ == protocol::WaveformMode::OFDM_COX);
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

    // CRITICAL: Skip all old audio - only process new audio from now on
    // This prevents re-detecting old syncs that are still in the circular buffer
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        correlation_pos_ = write_pos_;
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

    // Update interleaver for new carrier count
    // DQPSK = 2 bits per carrier
    size_t bps = config.num_carriers * 2;
    interleaver_ = std::make_unique<ChannelInterleaver>(bps, v2::LDPC_CODEWORD_BITS);
}

void StreamingDecoder::setDataMode(Modulation mod, CodeRate rate) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    code_rate_ = rate;
    if (waveform_) waveform_->configure(mod, rate);
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

size_t StreamingDecoder::samplesInBuffer() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return std::min(total_fed_, MAX_BUFFER_SAMPLES);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void StreamingDecoder::reset() {
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

DecodeResult StreamingDecoder::decodeFrame(const std::vector<float>& soft_bits, float snr, float cfo) {
    DecodeResult result;
    result.snr_db = snr;
    result.cfo_hz = cfo;

    constexpr size_t LDPC_BLOCK = v2::LDPC_CODEWORD_BITS;
    if (soft_bits.size() < LDPC_BLOCK) return result;

    bool use_interleave = (mode_ == protocol::WaveformMode::OFDM_CHIRP ||
                           mode_ == protocol::WaveformMode::OFDM_COX);

    std::vector<float> cw0(soft_bits.begin(), soft_bits.begin() + LDPC_BLOCK);
    if (use_interleave && interleaver_) {
        cw0 = interleaver_->deinterleave(cw0);
    }

    CodeRate rate = connected_ ? code_rate_ : CodeRate::R1_4;
    size_t bytes_per_cw = v2::getBytesPerCodeword(rate);
    codec_->setRate(rate);

    auto [ok0, data0] = codec_->decode(cw0);
    if (!ok0) {
        LOG_MODEM(DEBUG, "[%s] LDPC decode failed for cw0 (%zu soft bits)", log_prefix_.c_str(), soft_bits.size());
        result.codewords_failed++;
        return result;
    }
    if (data0.size() > bytes_per_cw) data0.resize(bytes_per_cw);
    result.codewords_ok++;

    if (data0.size() < 2 || data0[0] != 0x55 || data0[1] != 0x4C) {
        LOG_MODEM(DEBUG, "[%s] Invalid header: size=%zu, [0]=0x%02X, [1]=0x%02X",
                  log_prefix_.c_str(), data0.size(), data0.size()>0 ? data0[0] : 0, data0.size()>1 ? data0[1] : 0);
        return result;
    }

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
        if (use_interleave && interleaver_) {
            bits = interleaver_->deinterleave(bits);
        }

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
