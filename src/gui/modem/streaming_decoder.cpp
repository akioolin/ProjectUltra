// StreamingDecoder - Implementation
//
// Unified RX decoder using sliding window search (like test_iwaveform).
// Fixes the issues in RxPipeline by using:
//   1. Correct IWaveform call sequence: reset(), detectSync(), setFrequencyOffset(), process()
//   2. Circular buffer with bounded size
//   3. Sliding window search that advances past decoded frames

#include "streaming_decoder.hpp"
#include "ultra/logging.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace ultra {
namespace gui {

namespace v2 = protocol::v2;

StreamingDecoder::StreamingDecoder() {
    // Allocate circular buffer
    buffer_.resize(MAX_BUFFER_SAMPLES, 0.0f);

    // Create default waveform (MC-DPSK for disconnected state)
    // The waveform handles its own sync detection internally (ChirpSync or Schmidl-Cox)
    waveform_ = WaveformFactory::create(protocol::WaveformMode::MC_DPSK);

    // Create interleaver (30 carriers x 2 bits DQPSK = 60 bits/symbol for OFDM)
    // MC-DPSK uses 8 carriers x 2 bits = 16 bits/symbol
    interleaver_ = std::make_unique<ChannelInterleaver>(16, v2::LDPC_CODEWORD_BITS);

    // Create FEC codec (LDPC by default)
    codec_ = fec::CodecFactory::create(fec::CodecType::LDPC, CodeRate::R1_4);

    LOG_MODEM(INFO, "StreamingDecoder: Initialized (buffer=%zu samples, mode=MC-DPSK)",
              MAX_BUFFER_SAMPLES);
}

StreamingDecoder::~StreamingDecoder() {
    stop();
}

// ============================================================================
// AUDIO THREAD INTERFACE
// ============================================================================

void StreamingDecoder::feedAudio(const float* samples, size_t count) {
    if (!samples || count == 0) return;

    std::lock_guard<std::mutex> lock(buffer_mutex_);

    static size_t total_fed = 0;
    total_fed += count;
    static int feed_iter = 0;
    if (++feed_iter % 100 == 0) {
        LOG_MODEM(DEBUG, "StreamingDecoder::feedAudio: +%zu samples (total=%zu, buf=%zu)",
                  count, total_fed, samplesInBuffer());
    }

    // Check for buffer overflow
    // IMPORTANT: Use >= to prevent write_pos_ from catching up to read_pos_
    // (which would make samplesInBuffer() return 0 for a full buffer)
    size_t current_samples = samplesInBuffer();
    if (current_samples + count >= MAX_BUFFER_SAMPLES) {
        // Drop just enough samples to make room, plus small margin
        // Don't use SLIDE_STEP here - that's too aggressive
        size_t margin = 1000;  // Small margin
        size_t drop = (current_samples + count) - MAX_BUFFER_SAMPLES + margin;
        drop = std::min(drop, current_samples);
        size_t old_read = read_pos_;
        read_pos_ = (read_pos_ + drop) % MAX_BUFFER_SAMPLES;

        // CRITICAL: If read_pos_ advanced past search_pos_, we must advance search_pos_ too
        // Otherwise search_pos_ points to stale data that will be overwritten
        // Check if search_pos is in the dropped range [old_read, new_read)
        bool search_in_dropped_range = false;
        if (old_read <= read_pos_) {
            // No wrap: dropped range is [old_read, read_pos)
            search_in_dropped_range = (search_pos_ >= old_read && search_pos_ < read_pos_);
        } else {
            // Wrapped: dropped range is [old_read, MAX) + [0, read_pos)
            search_in_dropped_range = (search_pos_ >= old_read || search_pos_ < read_pos_);
        }
        if (search_in_dropped_range) {
            search_pos_ = read_pos_;
        }

        {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            stats_.buffer_overflows++;
        }

        LOG_MODEM(DEBUG, "StreamingDecoder: Buffer overflow, dropped %zu samples (read=%zu)",
                  drop, read_pos_);
    }

    // Copy samples to circular buffer
    for (size_t i = 0; i < count; i++) {
        buffer_[write_pos_] = samples[i];
        write_pos_ = (write_pos_ + 1) % MAX_BUFFER_SAMPLES;
    }

    // Update noise floor from new samples
    updateNoiseFloor(samples, count);

    // Signal decode thread that data is available
    data_cv_.notify_one();
}

// ============================================================================
// DECODE THREAD INTERFACE
// ============================================================================

void StreamingDecoder::processBuffer() {
    // Calculate minimum samples based on connection state
    // Disconnected: always MC-DPSK, need full search buffer for acquisition (3 seconds)
    // Connected: use waveform's getMinSamplesForSearch() which knows its own timing
    size_t min_samples = MIN_SAMPLES_FOR_SEARCH;  // Default for disconnected (MC-DPSK)

    if (connected_ && waveform_) {
        // Connected mode: waveform knows how many samples it needs
        min_samples = static_cast<size_t>(waveform_->getMinSamplesForSearch());
    }

    // Wait for enough data or shutdown
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);

        size_t samples_before = samplesInBuffer();
        auto timeout = std::chrono::milliseconds(100);
        data_cv_.wait_for(lock, timeout, [this, min_samples] {
            return samplesInBuffer() >= min_samples || shutdown_.load();
        });

        if (shutdown_.load()) return;

        size_t samples_now = samplesInBuffer();
        static int proc_iter = 0;
        if (++proc_iter % 20 == 0 || samples_now >= min_samples) {
            LOG_MODEM(DEBUG, "StreamingDecoder::processBuffer: buf=%zu (before=%zu), min=%zu",
                      samples_now, samples_before, min_samples);
        }

        if (samples_now < min_samples) return;  // Timeout, no data
    }

    // Try to detect and decode a frame
    detectAndDecode();
}

bool StreamingDecoder::hasFrame() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return !frame_queue_.empty();
}

DecodeResult StreamingDecoder::getFrame() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (frame_queue_.empty()) {
        return DecodeResult{};
    }
    DecodeResult result = std::move(frame_queue_.front());
    frame_queue_.pop();
    return result;
}

// ============================================================================
// MODE CONTROL
// ============================================================================

void StreamingDecoder::setMode(protocol::WaveformMode mode, bool connected) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    LOG_MODEM(INFO, "StreamingDecoder::setMode called: current=%d, new=%d, connected=%d",
              static_cast<int>(mode_), static_cast<int>(mode), connected);

    if (mode_ == mode && connected_ == connected) {
        LOG_MODEM(INFO, "StreamingDecoder::setMode: No change, returning early");
        return;
    }

    mode_ = mode;
    connected_ = connected;

    // Create new waveform for the mode
    // For MC-DPSK, use the configured carrier count
    if (mode == protocol::WaveformMode::MC_DPSK) {
        waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_carriers_);
        LOG_MODEM(INFO, "StreamingDecoder: Created MC-DPSK waveform with %d carriers", mc_dpsk_carriers_);
    } else {
        waveform_ = WaveformFactory::create(mode);
    }

    // Update interleaver bits per symbol based on mode
    size_t bits_per_symbol = mc_dpsk_carriers_ * 2;  // MC-DPSK: carriers x 2 bits (DQPSK)
    if (mode == protocol::WaveformMode::OFDM_CHIRP) {
        bits_per_symbol = 60;  // 30 carriers x 2 bits
    } else if (mode == protocol::WaveformMode::OFDM_COX) {
        bits_per_symbol = 60;  // 30 carriers x 2 bits
    }
    interleaver_ = std::make_unique<ChannelInterleaver>(bits_per_symbol, v2::LDPC_CODEWORD_BITS);

    // When switching to OFDM mode, save write_pos so we can skip old MC-DPSK data
    // Old data before this point is from previous mode and useless for OFDM detection
    if (connected && (mode == protocol::WaveformMode::OFDM_COX ||
                      mode == protocol::WaveformMode::OFDM_CHIRP)) {
        mode_switch_write_pos_ = write_pos_;
        LOG_MODEM(INFO, "StreamingDecoder: OFDM mode, will skip data before pos %zu", mode_switch_write_pos_);
    } else {
        mode_switch_write_pos_ = 0;  // Disabled for MC-DPSK
    }

    // Reset search position (don't lose buffered audio)
    search_pos_ = read_pos_;

    LOG_MODEM(INFO, "StreamingDecoder: Mode changed to %s (%s)",
              protocol::waveformModeToString(mode),
              connected ? "connected" : "disconnected");
}

void StreamingDecoder::setMCDPSKCarriers(int num_carriers) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    if (mc_dpsk_carriers_ == num_carriers) return;  // No change

    mc_dpsk_carriers_ = num_carriers;

    // If currently in MC-DPSK mode, recreate waveform with new carrier count
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_carriers_);
        LOG_MODEM(INFO, "StreamingDecoder: Recreated MC-DPSK waveform with %d carriers", mc_dpsk_carriers_);

        // Update interleaver for new carrier count
        size_t bits_per_symbol = mc_dpsk_carriers_ * 2;  // DQPSK
        interleaver_ = std::make_unique<ChannelInterleaver>(bits_per_symbol, v2::LDPC_CODEWORD_BITS);
    }
}

void StreamingDecoder::setDataMode(Modulation mod, CodeRate rate) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    LOG_MODEM(INFO, "StreamingDecoder::setDataMode: mod=%d, rate=%d",
              static_cast<int>(mod), static_cast<int>(rate));

    // Store code rate for LDPC decode
    code_rate_ = rate;

    // Configure the waveform with the new modulation and code rate
    if (waveform_) {
        waveform_->configure(mod, rate);
    }
}

void StreamingDecoder::setCodecType(fec::CodecType type) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    if (codec_type_ == type) return;  // No change

    codec_type_ = type;

    // Recreate codec with new type
    codec_ = fec::CodecFactory::create(type, code_rate_);
    LOG_MODEM(INFO, "StreamingDecoder: Switched to codec '%s'", codec_->getName().c_str());
}

// ============================================================================
// STATUS
// ============================================================================

float StreamingDecoder::getBufferFillPercent() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return static_cast<float>(samplesInBuffer()) / MAX_BUFFER_SAMPLES * 100.0f;
}

DecoderStats StreamingDecoder::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

size_t StreamingDecoder::samplesInBuffer() const {
    // Note: caller must hold buffer_mutex_
    if (write_pos_ >= read_pos_) {
        return write_pos_ - read_pos_;
    } else {
        return MAX_BUFFER_SAMPLES - read_pos_ + write_pos_;
    }
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void StreamingDecoder::reset() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    read_pos_ = 0;
    write_pos_ = 0;
    search_pos_ = 0;
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);

    if (waveform_) {
        waveform_->reset();
    }

    // Clear frame queue
    {
        std::lock_guard<std::mutex> qlock(queue_mutex_);
        while (!frame_queue_.empty()) {
            frame_queue_.pop();
        }
    }

    // Reset stats
    {
        std::lock_guard<std::mutex> slock(stats_mutex_);
        stats_ = DecoderStats{};
    }

    noise_floor_ = 0.001f;
    last_snr_.store(0.0f);
    last_cfo_.store(0.0f);
    last_fading_index_.store(0.0f);

    LOG_MODEM(INFO, "StreamingDecoder: Reset");
}

void StreamingDecoder::stop() {
    shutdown_.store(true);
    data_cv_.notify_all();
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

std::vector<float> StreamingDecoder::copyOutSamples(size_t count) {
    // Note: caller must hold buffer_mutex_
    // Calculate available samples from search_pos_ to write_pos_ (NOT from read_pos_!)
    size_t available = 0;
    if (write_pos_ >= search_pos_) {
        available = write_pos_ - search_pos_;
    } else {
        available = MAX_BUFFER_SAMPLES - search_pos_ + write_pos_;
    }

    count = std::min(count, available);
    std::vector<float> result(count);

    size_t pos = search_pos_;
    for (size_t i = 0; i < count; i++) {
        result[i] = buffer_[pos];
        pos = (pos + 1) % MAX_BUFFER_SAMPLES;
    }

    return result;
}

void StreamingDecoder::advanceReadPos(size_t count) {
    // Note: caller must hold buffer_mutex_
    read_pos_ = (read_pos_ + count) % MAX_BUFFER_SAMPLES;
    if (search_pos_ < read_pos_ || (search_pos_ > read_pos_ + MAX_BUFFER_SAMPLES / 2)) {
        search_pos_ = read_pos_;
    }
}

void StreamingDecoder::trimOldSamples(size_t keep_samples) {
    // Note: caller must hold buffer_mutex_
    size_t current = samplesInBuffer();
    if (current > keep_samples) {
        size_t trim = current - keep_samples;
        size_t old_read = read_pos_;
        read_pos_ = (read_pos_ + trim) % MAX_BUFFER_SAMPLES;

        // Only adjust search_pos if it's now behind read_pos (in circular buffer sense)
        // Check if search_pos is in the range [old_read, new_read) that got trimmed
        bool search_was_trimmed = false;
        if (old_read <= read_pos_) {
            // No wrap: trimmed range is [old_read, read_pos)
            search_was_trimmed = (search_pos_ >= old_read && search_pos_ < read_pos_);
        } else {
            // Wrapped: trimmed range is [old_read, MAX) + [0, read_pos)
            search_was_trimmed = (search_pos_ >= old_read || search_pos_ < read_pos_);
        }

        if (search_was_trimmed) {
            search_pos_ = read_pos_;
        }
    }
}

bool StreamingDecoder::detectAndDecode() {
    if (!waveform_) {
        return false;
    }

    // Waveform handles its own sync detection internally:
    // - MC-DPSK and OFDM_CHIRP use ChirpSync
    // - OFDM_COX uses Schmidl-Cox

    // Calculate minimum samples (same logic as processBuffer)
    // Disconnected: always MC-DPSK, need full buffer for robust acquisition
    // Connected: waveform knows its own timing requirements
    size_t min_samples_for_search = MIN_SAMPLES_FOR_SEARCH;  // Default for disconnected

    if (connected_ && waveform_) {
        min_samples_for_search = static_cast<size_t>(waveform_->getMinSamplesForSearch());
    }

    // Copy samples for processing (release lock during heavy work)
    std::vector<float> work_buffer;
    size_t search_start_pos = 0;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        // OPTIMIZATION: Skip old MC-DPSK data when in OFDM mode
        // mode_switch_write_pos_ marks where OFDM data starts (set during mode switch)
        if (mode_switch_write_pos_ != 0) {
            // Check if search_pos is behind mode_switch_write_pos_ (old data)
            bool search_behind = false;
            if (mode_switch_write_pos_ >= search_pos_) {
                // No wrap: search_pos is behind if it's less than switch pos
                search_behind = (mode_switch_write_pos_ - search_pos_) > 1000;
            } else {
                // Wrapped: search_pos is behind if it's in the "old" region
                // Old region is [search_pos_, MAX) + [0, mode_switch_write_pos_)
                search_behind = true;  // If wrapped, search is definitely behind
            }

            if (search_behind) {
                LOG_MODEM(INFO, "StreamingDecoder: Skipping old data, search %zu -> %zu (mode_switch_pos)",
                          search_pos_, mode_switch_write_pos_);
                search_pos_ = mode_switch_write_pos_;
                read_pos_ = search_pos_;
            }
            // Clear the marker - only skip once
            mode_switch_write_pos_ = 0;
        }

        // Calculate available samples from search position
        size_t available = 0;
        if (write_pos_ >= search_pos_) {
            available = write_pos_ - search_pos_;
        } else {
            available = MAX_BUFFER_SAMPLES - search_pos_ + write_pos_;
        }

        if (available < min_samples_for_search) {
            return false;
        }

        search_start_pos = search_pos_;

        // Need enough samples to find chirp AND decode frame after it
        // Chirp can be up to ~1.5s into buffer (72000 samples of lead-in)
        // Plus chirp itself (~58000) plus frame data (up to ~70000 for MC-DPSK 3 CWs)
        // So we need at least 200000 samples for reliable decode
        size_t max_search = std::min(available, size_t(250000));  // 5+ seconds
        work_buffer = copyOutSamples(max_search);

        LOG_MODEM(DEBUG, "StreamingDecoder::detectAndDecode: search_pos=%zu, available=%zu, work_buf=%zu",
                  search_start_pos, available, work_buffer.size());
    }

    // Create sample span for search
    SampleSpan search_span(work_buffer.data(), work_buffer.size());

    // Use waveform's detectSync - it properly calculates where training starts
    // The waveform knows its own preamble structure and handles the position correctly
    waveform_->reset();  // Reset before detection

    SyncResult sync_result;
    bool sync_found = waveform_->detectSync(search_span, sync_result, 0.15f);

    if (sync_found) {
        LOG_MODEM(INFO, "StreamingDecoder: Sync at %d, CFO=%.1f Hz, corr=%.3f",
                  sync_result.start_sample, sync_result.cfo_hz, sync_result.correlation);
    }

    if (!sync_found) {
        // No sync - advance search position and trim old samples
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        // Advance search position
        size_t old_search = search_pos_;
        search_pos_ = (search_pos_ + SLIDE_STEP) % MAX_BUFFER_SAMPLES;

        // Trim old samples (this may reset search_pos to read_pos if we advanced too far)
        trimOldSamples(MIN_SAMPLES_FOR_SEARCH * 2);

        LOG_MODEM(DEBUG, "StreamingDecoder: No sync, corr=%.3f, search %zu->%zu, read=%zu",
                  sync_result.correlation, old_search, search_pos_, read_pos_);
        return false;
    }

    LOG_MODEM(INFO, "StreamingDecoder: Sync detected at %d, CFO=%.1f Hz, corr=%.3f",
              sync_result.start_sample, sync_result.cfo_hz, sync_result.correlation);

    // Store SNR and CFO for status
    float snr = estimateSNRFromChirp(sync_result.correlation, noise_floor_);
    last_snr_.store(snr);
    last_cfo_.store(sync_result.cfo_hz);
    // Fading index will be updated after process() when we have per-carrier data

    // Check if this is a chirp-based mode (MC-DPSK, OFDM_CHIRP)
    // OFDM_COX uses Schmidl-Cox sync and doesn't have chirp-based PINGs
    bool uses_chirp_ping = (mode_ == protocol::WaveformMode::MC_DPSK ||
                            mode_ == protocol::WaveformMode::OFDM_CHIRP);

    // Helper lambda to handle PING detection when decode fails
    auto handlePingDetection = [&]() {
        if (!uses_chirp_ping) return false;

        // PING detected - queue result and advance
        DecodeResult ping_result;
        ping_result.success = true;
        ping_result.is_ping = true;
        ping_result.frame_type = v2::FrameType::PING;
        ping_result.snr_db = snr;
        ping_result.cfo_hz = sync_result.cfo_hz;

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            frame_queue_.push(ping_result);
        }

        if (ping_callback_) {
            ping_callback_(snr, sync_result.cfo_hz);
        }

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.pings_received++;
        }

        // Advance past chirp
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            // Advance past the chirp position + some guard
            size_t chirp_samples = 57600;  // ~1.2 sec chirp
            size_t advance = static_cast<size_t>(sync_result.start_sample) + chirp_samples;
            search_pos_ = (search_pos_ + advance) % MAX_BUFFER_SAMPLES;
            read_pos_ = search_pos_;
            trimOldSamples(MIN_SAMPLES_FOR_SEARCH * 2);
        }

        LOG_MODEM(INFO, "StreamingDecoder: PING detected (no valid data after chirp), SNR=%.1f dB, CFO=%.1f Hz",
                  snr, sync_result.cfo_hz);
        return true;
    };

    // Data frame - apply CFO and process
    // NOTE: Don't call reset() here - detectSync already set up waveform state
    // CRITICAL: setFrequencyOffset BEFORE process (per INV-CFO-002)
    waveform_->setFrequencyOffset(sync_result.cfo_hz);

    // Get frame data starting from sync position
    int data_start = sync_result.start_sample;
    if (data_start < 0 || static_cast<size_t>(data_start) >= work_buffer.size()) {
        // Invalid position - might be PING (chirp at end of buffer with no data)
        if (handlePingDetection()) return true;
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        search_pos_ = (search_pos_ + SLIDE_STEP) % MAX_BUFFER_SAMPLES;
        return false;
    }

    // Check if we have enough samples for a frame
    size_t min_frame_samples = waveform_->getMinSamplesForFrame();
    size_t available_after_chirp = work_buffer.size() - static_cast<size_t>(data_start);

    if (available_after_chirp < min_frame_samples) {
        // Not enough samples for a full data frame
        // This could be a PING (chirp-only, no data follows)
        // Check if we have SOME samples after chirp but they're mostly silence/noise
        if (available_after_chirp > 1000) {
            // We have some samples - check if they look like data or noise
            float energy = 0.0f;
            size_t check_len = std::min(available_after_chirp, size_t(5000));
            for (size_t i = 0; i < check_len; i++) {
                float s = work_buffer[data_start + i];
                energy += s * s;
            }
            energy = std::sqrt(energy / check_len);

            // If energy is very low (< 0.1 RMS), it's likely a PING
            if (energy < 0.1f) {
                if (handlePingDetection()) return true;
            }
        }

        // Not enough samples yet - wait for more data
        LOG_MODEM(DEBUG, "StreamingDecoder: Need more samples (have %zu, need %zu)",
                  available_after_chirp, min_frame_samples);
        return false;
    }

    // Limit frame span to avoid processing too much data
    size_t max_frame_samples = min_frame_samples * 4;  // Allow up to 4 codewords
    size_t frame_span_size = std::min(available_after_chirp, max_frame_samples);
    SampleSpan frame_span(work_buffer.data() + data_start, frame_span_size);

    // Process frame
    auto decode_start = std::chrono::steady_clock::now();
    bool process_ok = waveform_->process(frame_span);

    if (!process_ok) {
        LOG_MODEM(DEBUG, "StreamingDecoder: process() returned false - checking for PING");
        // process() failed - could be PING (no training/data after chirp)
        if (handlePingDetection()) return true;
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        size_t advance = sync_result.start_sample + SLIDE_STEP;
        search_pos_ = (search_pos_ + advance) % MAX_BUFFER_SAMPLES;
        read_pos_ = search_pos_;  // Prevent infinite loop
        return false;
    }

    // Get soft bits
    auto soft_bits = waveform_->getSoftBits();
    if (soft_bits.empty()) {
        LOG_MODEM(DEBUG, "StreamingDecoder: No soft bits from waveform - checking for PING");
        // No soft bits - could be PING
        if (handlePingDetection()) return true;
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        size_t advance = sync_result.start_sample + SLIDE_STEP;
        search_pos_ = (search_pos_ + advance) % MAX_BUFFER_SAMPLES;
        read_pos_ = search_pos_;  // Prevent infinite loop
        return false;
    }

    // Update fading index from waveform (available after process/getSoftBits)
    last_fading_index_.store(waveform_->getFadingIndex());

    // Decode frame
    DecodeResult result = decodeFrame(soft_bits, snr, sync_result.cfo_hz);

    auto decode_end = std::chrono::steady_clock::now();
    float decode_time_ms = std::chrono::duration<float, std::milli>(decode_end - decode_start).count();

    // Update stats
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        if (result.success) {
            stats_.frames_decoded++;
        } else {
            stats_.frames_failed++;
        }
        // Exponential moving average for decode time
        stats_.avg_decode_time_ms = 0.9f * stats_.avg_decode_time_ms + 0.1f * decode_time_ms;
    }

    // Check if decode failed to produce a valid frame - might be PING
    // A PING has chirp but no data, so LDPC will decode garbage without valid "UL" header
    // This happens when:
    //   1. LDPC decode fails completely (codewords_ok == 0)
    //   2. LDPC succeeds but decoded data has no valid "UL" magic (frame_data empty)
    if (!result.success && result.frame_data.empty()) {
        LOG_MODEM(INFO, "StreamingDecoder: No valid frame after chirp (cw_ok=%d) - checking for PING",
                  result.codewords_ok);
        if (handlePingDetection()) return true;
    }

    // Queue result if we got any data
    if (result.success || result.codewords_ok > 0) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            frame_queue_.push(result);
        }

        if (result.success && frame_callback_) {
            frame_callback_(result);
        }

        LOG_MODEM(INFO, "StreamingDecoder: Frame decode %s (%d/%d codewords, %.1f ms)",
                  result.success ? "OK" : "PARTIAL",
                  result.codewords_ok, result.codewords_ok + result.codewords_failed,
                  decode_time_ms);
    }

    // Advance past this frame
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        size_t old_search = search_pos_;
        size_t advance = sync_result.start_sample + min_frame_samples;
        search_pos_ = (search_pos_ + advance) % MAX_BUFFER_SAMPLES;
        // Also advance read_pos to release consumed samples
        // This prevents infinite loop when samplesInBuffer() > MIN but available < MIN
        read_pos_ = search_pos_;
        trimOldSamples(MIN_SAMPLES_FOR_SEARCH * 2);

        LOG_MODEM(INFO, "StreamingDecoder: search_pos %zu -> %zu (advance=%zu = start_sample=%d + min_frame=%zu)",
                  old_search, search_pos_, advance, sync_result.start_sample, min_frame_samples);
    }

    return result.success;
}

bool StreamingDecoder::isPingOnly(const std::vector<float>& samples, size_t chirp_end) {
    // PING frames are chirp-only (no data after)
    // Compare energy in chirp region vs post-chirp region

    if (samples.size() < chirp_end + 5000) {
        return false;  // Not enough samples to determine
    }

    // Measure energy in chirp region (last part of chirp)
    size_t chirp_measure_start = chirp_end > 24000 ? chirp_end - 24000 : 0;
    float chirp_energy = 0.0f;
    size_t chirp_count = chirp_end - chirp_measure_start;
    for (size_t i = chirp_measure_start; i < chirp_end && i < samples.size(); i++) {
        chirp_energy += samples[i] * samples[i];
    }
    if (chirp_count > 0) chirp_energy /= chirp_count;

    // Measure energy after chirp
    float post_energy = 0.0f;
    size_t post_count = 0;
    for (size_t i = chirp_end; i < chirp_end + 5000 && i < samples.size(); i++) {
        post_energy += samples[i] * samples[i];
        post_count++;
    }
    if (post_count > 0) post_energy /= post_count;

    // If post-chirp energy is much lower than chirp energy, it's PING only
    float ratio = (chirp_energy > 1e-10f) ? (post_energy / chirp_energy) : 0.0f;

    LOG_MODEM(DEBUG, "StreamingDecoder: PING check ratio=%.3f (chirp=%.6f, post=%.6f)",
              ratio, chirp_energy, post_energy);

    return ratio < PING_ENERGY_RATIO;
}

float StreamingDecoder::estimateSNRFromChirp(float correlation, float noise_floor) {
    // Correlation strength maps to SNR
    // At SNR=0dB, correlation ~0.15-0.20
    // At SNR=10dB, correlation ~0.50-0.70
    // At SNR=20dB, correlation ~0.85-0.95

    // Simple linear approximation (can be calibrated better)
    // correlation = 0.15 + 0.03 * SNR_dB (roughly)
    // SNR_dB = (correlation - 0.15) / 0.03
    float snr_db = (correlation - 0.15f) / 0.03f;
    return std::max(-5.0f, std::min(30.0f, snr_db));  // Clamp to reasonable range
}

void StreamingDecoder::updateNoiseFloor(const float* samples, size_t count) {
    // Use exponential moving average for stability
    float energy = 0.0f;
    for (size_t i = 0; i < count; i++) {
        energy += samples[i] * samples[i];
    }
    if (count > 0) {
        energy = std::sqrt(energy / count);
        float alpha = 0.01f;  // Slow adaptation
        noise_floor_ = alpha * energy + (1.0f - alpha) * noise_floor_;
    }
}

DecodeResult StreamingDecoder::decodeFrame(const std::vector<float>& soft_bits, float snr, float cfo) {
    DecodeResult result;
    result.snr_db = snr;
    result.cfo_hz = cfo;

    constexpr size_t LDPC_BLOCK = v2::LDPC_CODEWORD_BITS;

    if (soft_bits.size() < LDPC_BLOCK) {
        LOG_MODEM(DEBUG, "StreamingDecoder: Not enough soft bits (%zu < %zu)",
                  soft_bits.size(), LDPC_BLOCK);
        return result;
    }

    // Deinterleave first codeword
    // NOTE: Interleaving is ONLY used for OFDM modes (OFDM_CHIRP, OFDM_COX)
    // MC-DPSK does NOT use interleaving (to preserve differential encoding)
    std::vector<float> cw0_bits(soft_bits.begin(), soft_bits.begin() + LDPC_BLOCK);
    bool use_interleaving = (mode_ == protocol::WaveformMode::OFDM_CHIRP ||
                             mode_ == protocol::WaveformMode::OFDM_COX);
    if (use_interleaving && interleaver_) {
        cw0_bits = interleaver_->deinterleave(cw0_bits);
    }

    // Decode CW0 with negotiated code rate (or R1_4 for disconnected)
    CodeRate frame_rate = connected_ ? code_rate_ : CodeRate::R1_4;
    size_t bytes_per_cw = v2::getBytesPerCodeword(frame_rate);

    codec_->setRate(frame_rate);
    auto [cw0_success, cw0_data] = codec_->decode(cw0_bits);

    if (!cw0_success) {
        LOG_MODEM(DEBUG, "StreamingDecoder: CW0 LDPC decode failed");
        result.codewords_failed++;
        return result;
    }

    // Resize CW0 to exactly bytes_per_cw (same as CW1+)
    if (cw0_data.size() > bytes_per_cw) {
        cw0_data.resize(bytes_per_cw);
    }

    result.codewords_ok++;

    // Check for valid header
    if (cw0_data.size() < 2 || cw0_data[0] != 0x55 || cw0_data[1] != 0x4C) {
        LOG_MODEM(DEBUG, "StreamingDecoder: Invalid magic (got 0x%02x 0x%02x)",
                  cw0_data.size() > 0 ? cw0_data[0] : 0,
                  cw0_data.size() > 1 ? cw0_data[1] : 0);
        return result;
    }

    // Parse header
    auto header = v2::parseHeader(cw0_data);
    if (!header.valid) {
        LOG_MODEM(WARN, "StreamingDecoder: Invalid header");
        return result;
    }

    result.frame_type = header.type;
    int expected_cw = header.total_cw;

    LOG_MODEM(INFO, "StreamingDecoder: Header OK, type=%d, total_cw=%d",
              static_cast<int>(header.type), expected_cw);

    // Check if we have enough soft bits for all codewords
    int available_cw = static_cast<int>(soft_bits.size() / LDPC_BLOCK);
    if (available_cw < expected_cw) {
        LOG_MODEM(DEBUG, "StreamingDecoder: Need more codewords (%d/%d)",
                  available_cw, expected_cw);
        // Could implement accumulation here, but for now just return partial
        result.frame_data = cw0_data;
        return result;
    }

    // Decode remaining codewords
    v2::CodewordStatus cw_status;
    cw_status.decoded.resize(expected_cw, false);
    cw_status.data.resize(expected_cw);
    cw_status.decoded[0] = true;
    cw_status.data[0] = cw0_data;

    for (int i = 1; i < expected_cw; i++) {
        size_t offset = i * LDPC_BLOCK;
        std::vector<float> cw_bits(soft_bits.begin() + offset,
                                    soft_bits.begin() + offset + LDPC_BLOCK);

        // Deinterleave (only for OFDM modes)
        if (use_interleaving && interleaver_) {
            cw_bits = interleaver_->deinterleave(cw_bits);
        }

        // Decode using ICodec
        auto [cw_success, cw_data] = codec_->decode(cw_bits);

        if (cw_success && cw_data.size() >= bytes_per_cw) {
            cw_data.resize(bytes_per_cw);
            cw_status.decoded[i] = true;
            cw_status.data[i] = cw_data;
            result.codewords_ok++;
            LOG_MODEM(DEBUG, "StreamingDecoder: CW%d OK", i);
        } else {
            result.codewords_failed++;
            LOG_MODEM(DEBUG, "StreamingDecoder: CW%d FAILED", i);
        }
    }

    // Check if all codewords decoded
    if (cw_status.allSuccess()) {
        result.success = true;
        result.frame_data = cw_status.reassemble();
        LOG_MODEM(INFO, "StreamingDecoder: Frame complete (%d codewords, %zu bytes)",
                  expected_cw, result.frame_data.size());
    }

    return result;
}

} // namespace gui
} // namespace ultra
