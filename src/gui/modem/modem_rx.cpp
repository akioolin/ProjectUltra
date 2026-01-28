// ModemEngine RX - Thread Management and Audio Input
// Decode logic is in modem_rx_decode.cpp

#include "modem_engine.hpp"
#include "modem_rx_constants.hpp"
#include "ultra/logging.hpp"
#include <fstream>

namespace ultra {
namespace gui {

using namespace rx_constants;

// ============================================================================
// ACQUISITION THREAD
// ============================================================================

void ModemEngine::startAcquisitionThread() {
    if (acquisition_running_) return;

    acquisition_running_ = true;
    acquisition_thread_ = std::thread(&ModemEngine::acquisitionLoop, this);
    LOG_MODEM(INFO, "[%s] Acquisition thread started", log_prefix_.c_str());
}

void ModemEngine::stopAcquisitionThread() {
    if (!acquisition_running_) return;

    acquisition_running_ = false;
    acquisition_cv_.notify_all();

    if (acquisition_thread_.joinable()) {
        acquisition_thread_.join();
    }
    LOG_MODEM(INFO, "[%s] Acquisition thread stopped", log_prefix_.c_str());
}

void ModemEngine::acquisitionLoop() {
    LOG_MODEM(INFO, "[%s] Acquisition loop starting", log_prefix_.c_str());

    static int acq_iter = 0;  // Debug iteration counter

    while (acquisition_running_) {
        {
            std::unique_lock<std::mutex> lock(acquisition_mutex_);
            acquisition_cv_.wait_for(lock, std::chrono::milliseconds(ACQUISITION_POLL_MS));
        }

        if (!acquisition_running_) break;
        if (connected_) continue;              // RX thread handles connected mode
        if (rx_frame_state_.active) continue;  // RX thread busy
        if (detected_frame_queue_.size() > 0) continue;

        auto samples = getBufferSnapshot();

        // Debug: log buffer state periodically
        if (++acq_iter % 20 == 0 || samples.size() >= MIN_SAMPLES_FOR_ACQUISITION) {
            LOG_MODEM(DEBUG, "[%s] Acq iter %d: buf=%zu, min=%zu",
                      log_prefix_.c_str(), acq_iter, samples.size(), MIN_SAMPLES_FOR_ACQUISITION);
        }

        if (samples.size() < MIN_SAMPLES_FOR_ACQUISITION) continue;

        const size_t chirp_total = chirp_sync_->getTotalSamples();

        // Search for chirp in buffer - limit to reasonable window to avoid slow search
        // Max search: chirp length + 2 seconds of lead-in
        size_t max_search = chirp_total + 96000;  // chirp + 2s
        size_t search_len = std::min(max_search, samples.size());

        SampleSpan search_span(samples.data(), search_len);

        // Use dual chirp detection for robust CFO estimation
        // Works down to -20 dB SNR with ±2 Hz accuracy
        auto chirp_result = chirp_sync_->detectDualChirp(search_span, 0.15f);
        int chirp_start = chirp_result.success ? chirp_result.up_chirp_start : -1;
        float chirp_corr = std::max(chirp_result.up_correlation, chirp_result.down_correlation);
        float cfo_hz = chirp_result.cfo_hz;

        // Debug: log chirp detection result
        LOG_MODEM(DEBUG, "[%s] Acq: chirp_start=%d, corr=%.3f, CFO=%.1f Hz, search_len=%zu",
                  log_prefix_.c_str(), chirp_start, chirp_corr, cfo_hz, search_len);

        if (chirp_start >= 0) {
            size_t chirp_total = chirp_sync_->getTotalSamples();
            size_t chirp_end = chirp_start + chirp_total;

            // Check if there's signal energy after chirp (= DPSK frame)
            // PING is chirp-only, DPSK frames have chirp + training + ref symbol + data
            bool has_data_after = false;
            size_t training_len = mc_dpsk_config_.training_symbols * mc_dpsk_config_.samples_per_symbol;
            size_t ref_symbol_len = mc_dpsk_config_.samples_per_symbol;

            LOG_MODEM(DEBUG, "[%s] Acq: chirp=%d, total=%zu, end=%zu",
                      log_prefix_.c_str(), chirp_start, chirp_total, chirp_end);

            if (chirp_end + training_len + ref_symbol_len + 1000 < samples.size()) {
                // Check energy in the samples after chirp (training sequence)
                // Compare to chirp energy to make detection SNR-independent
                // PING = chirp only (post-chirp is noise), DPSK = chirp + data (similar energy)

                // Measure chirp RMS (last 25% of chirp to avoid transients)
                size_t chirp_measure_start = chirp_start + 3 * chirp_total / 4;
                size_t chirp_measure_len = chirp_total / 4;
                float chirp_energy = 0.0f;
                for (size_t i = chirp_measure_start; i < chirp_measure_start + chirp_measure_len; i++) {
                    chirp_energy += samples[i] * samples[i];
                }
                float chirp_rms = std::sqrt(chirp_energy / chirp_measure_len);

                // Measure post-chirp RMS (training region)
                float post_energy = 0.0f;
                for (size_t i = chirp_end; i < chirp_end + training_len; i++) {
                    post_energy += samples[i] * samples[i];
                }
                float post_rms = std::sqrt(post_energy / training_len);

                // Data present if post-chirp energy is similar to chirp energy (30-140%)
                // PING: post_rms ~ noise floor (much lower than chirp, ratio < 0.3)
                // DPSK: post_rms ~ chirp_rms (similar signal level, ratio 0.3-1.4)
                //       Note: On fading channels, ratio can reach 1.3 due to energy variation
                // Another chirp: post_rms >> chirp_rms (ratio >= 1.4 - different transmission)
                float energy_ratio = (chirp_rms > 0.001f) ? (post_rms / chirp_rms) : 0.0f;
                has_data_after = (energy_ratio > 0.3f && energy_ratio < 1.4f);

                // Extra check for suspicious range (1.1-1.4): search for chirp in post region
                // If we find a strong chirp correlation, it's overlapping chirps, not DPSK data
                if (has_data_after && energy_ratio > 1.1f) {
                    // Quick chirp search in post-chirp region
                    SampleSpan post_span(samples.data() + chirp_end,
                                         std::min(chirp_total, samples.size() - chirp_end));
                    auto post_result = chirp_sync_->detectDualChirp(post_span, 0.15f);
                    if (post_result.success && post_result.up_correlation > 0.5f) {
                        // Found another chirp starting in post region - treat as PING, not DPSK
                        has_data_after = false;
                        LOG_MODEM(INFO, "[%s] Acq: Suspicious ratio %.2f - found chirp in post region (corr=%.3f), treating as PING",
                                  log_prefix_.c_str(), energy_ratio, post_result.up_correlation);
                    }
                }

                LOG_MODEM(DEBUG, "[%s] Acq: chirp_rms=%.4f, post_rms=%.4f, ratio=%.2f, has_data=%d",
                          log_prefix_.c_str(), chirp_rms, post_rms, energy_ratio, has_data_after);
            }

            if (has_data_after) {
                // DPSK frame with chirp preamble
                // Layout: [CHIRP][TRAINING][REF][DATA...]
                // rxDecodeDPSK expects data_start to point to DATA,
                // with reference symbol at data_start - symbol_samples
                size_t data_start = chirp_end + training_len + ref_symbol_len;

                LOG_MODEM(DEBUG, "[%s] Acq: data_start=%zu", log_prefix_.c_str(), data_start);

                DetectedFrame frame;
                frame.data_start = static_cast<int>(data_start);
                frame.absolute_sample_pos = samples_consumed_ + data_start;  // For CFO phase calc
                frame.waveform = protocol::WaveformMode::MC_DPSK;
                frame.timestamp = std::chrono::steady_clock::now();
                frame.has_chirp_preamble = true;  // Use training + ref for CFO estimation
                frame.cfo_hz = cfo_hz;            // Pass CFO for correction in decoder

                detected_frame_queue_.push(frame);
                last_rx_waveform_ = protocol::WaveformMode::MC_DPSK;

                LOG_MODEM(INFO, "[%s] Acquisition: Chirp+DPSK frame, data at %zu (corr=%.3f, CFO=%.1f Hz)",
                          log_prefix_.c_str(), data_start, chirp_corr, cfo_hz);
            } else {
                // PING (chirp only, no data)
                LOG_MODEM(INFO, "[%s] Acquisition: Chirp PING at %d (corr=%.3f, CFO=%.1f Hz)",
                          log_prefix_.c_str(), chirp_start, chirp_corr, cfo_hz);

                // Consume chirp samples plus a guard period (200ms) to avoid detecting
                // partial chirps from overlapping transmissions
                size_t guard_samples = 48000 / 5;  // 200ms @ 48kHz
                size_t consume_end = chirp_end + guard_samples;
                if (consume_end > samples.size()) consume_end = samples.size();
                consumeSamples(consume_end);

                if (ping_received_callback_) {
                    ping_received_callback_(getCurrentSNR());
                }
                updateStats([](LoopbackStats& s) { s.frames_received++; });
                last_rx_complete_time_ = std::chrono::steady_clock::now();
            }
        }
    }

    LOG_MODEM(INFO, "[%s] Acquisition loop exiting", log_prefix_.c_str());
}

// ============================================================================
// RX/DECODE THREAD
// ============================================================================

void ModemEngine::startRxDecodeThread() {
    if (rx_decode_running_) return;

    rx_decode_running_ = true;
    detected_frame_queue_.start();
    rx_decode_thread_ = std::thread(&ModemEngine::rxDecodeLoop, this);
    LOG_MODEM(INFO, "[%s] RX decode thread started", log_prefix_.c_str());
}

void ModemEngine::stopRxDecodeThread() {
    if (!rx_decode_running_) return;

    rx_decode_running_ = false;
    detected_frame_queue_.stop();
    rx_decode_cv_.notify_all();

    if (rx_decode_thread_.joinable()) {
        rx_decode_thread_.join();
    }
    LOG_MODEM(INFO, "[%s] RX decode thread stopped", log_prefix_.c_str());
}

void ModemEngine::rxDecodeLoop() {
    LOG_MODEM(INFO, "[%s] RX decode loop starting", log_prefix_.c_str());

    while (rx_decode_running_) {
        // ====================================================================
        // Connected mode: Use RxPipeline (NEW)
        // ====================================================================
        // RxPipeline handles sync detection, demodulation, and LDPC decode
        // feedAudio() routes audio to RxPipeline when connected
        // This loop just polls for decoded frames
        // ====================================================================
        if (connected_) {
            static int conn_iter = 0;
            if (++conn_iter % 100 == 0 && rx_pipeline_) {
                LOG_MODEM(DEBUG, "[%s] Connected mode: waveform=%d, pipeline_buf=%zu, frames=%zu",
                          log_prefix_.c_str(), static_cast<int>(waveform_mode_),
                          rx_pipeline_->getBufferSize(), rx_pipeline_->getFrameCount());
            }

            // Check for decoded frames from RxPipeline
            // Note: Frame delivery is handled via callbacks set in constructor,
            // but we can also poll here for stats and additional processing
            if (rx_pipeline_ && rx_pipeline_->hasFrame()) {
                auto result = rx_pipeline_->getFrame();
                if (result.success) {
                    LOG_MODEM(INFO, "[%s] RxPipeline: Frame decoded, %d/%d CWs, SNR=%.1f dB",
                              log_prefix_.c_str(), result.codewords_ok,
                              result.codewords_ok + result.codewords_failed,
                              result.snr_estimate);

                    // Update stats
                    updateStats([&](LoopbackStats& s) {
                        s.snr_db = result.snr_estimate;
                        s.synced = true;
                    });

                    // Save peer CFO for future frames
                    if (std::abs(result.cfo_estimate) > 0.1f) {
                        peer_cfo_hz_ = result.cfo_estimate;
                    }

                    last_rx_complete_time_ = std::chrono::steady_clock::now();
                } else if (result.codewords_failed > 0) {
                    LOG_MODEM(INFO, "[%s] RxPipeline: Frame failed, %d/%d CWs OK",
                              log_prefix_.c_str(), result.codewords_ok,
                              result.codewords_ok + result.codewords_failed);
                    updateStats([](LoopbackStats& s) { s.frames_failed++; });
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(DECODE_POLL_MS));
            continue;
        }

        // Disconnected: wait for frame from acquisition thread
        DetectedFrame frame;
        if (!detected_frame_queue_.waitPop(frame, std::chrono::milliseconds(ACQUISITION_POLL_MS))) {
            continue;
        }

        rx_frame_state_.active = true;
        rx_frame_state_.frame = frame;

        LOG_MODEM(INFO, "[%s] Processing %s frame at %d",
                  log_prefix_.c_str(),
                  frame.waveform == protocol::WaveformMode::MC_DPSK ? "DPSK" : "OFDM",
                  frame.data_start);

        bool success = false;
        if (frame.waveform == protocol::WaveformMode::MC_DPSK) {
            success = rxDecodeDPSK(frame);
        }

        if (!success) {
            updateStats([](LoopbackStats& s) { s.frames_failed++; });
        }

        rx_frame_state_.clear();
    }

    LOG_MODEM(INFO, "[%s] RX decode loop exiting", log_prefix_.c_str());
}

// ============================================================================
// AUDIO INPUT
// ============================================================================

void ModemEngine::feedAudio(const float* samples, size_t count) {
    if (count == 0 || samples == nullptr) return;

    // ========================================================================
    // NEW: Route audio based on connection state
    // ========================================================================
    // Connected mode: RxPipeline handles sync, demod, decode internally
    // Disconnected mode: Buffer for acquisition thread to search for chirp
    // ========================================================================

    if (connected_ && rx_pipeline_ && active_rx_waveform_) {
        // Connected: feed directly to RxPipeline
        rx_pipeline_->feedAudio(samples, count);

        // Also update carrier sense
        if (count >= ENERGY_WINDOW_SAMPLES) {
            std::vector<float> window(samples, samples + std::min(count, ENERGY_WINDOW_SAMPLES));
            updateChannelEnergy(window);
        }
        return;
    }

    // Disconnected: use traditional buffer for acquisition
    {
        std::lock_guard<std::mutex> lock(rx_buffer_mutex_);

        // Cap buffer to prevent unbounded growth
        if (rx_sample_buffer_.size() + count > MAX_PENDING_SAMPLES) {
            size_t to_remove = rx_sample_buffer_.size() + count - MAX_PENDING_SAMPLES;
            if (to_remove >= rx_sample_buffer_.size()) {
                samples_consumed_ += rx_sample_buffer_.size();
                rx_sample_buffer_.clear();
            } else {
                samples_consumed_ += to_remove;
                rx_sample_buffer_.erase(rx_sample_buffer_.begin(),
                                        rx_sample_buffer_.begin() + to_remove);
            }
        }
        rx_sample_buffer_.insert(rx_sample_buffer_.end(), samples, samples + count);
        total_samples_fed_ += count;
    }

    // Update carrier sense
    if (count >= ENERGY_WINDOW_SAMPLES) {
        std::vector<float> window(samples, samples + std::min(count, ENERGY_WINDOW_SAMPLES));
        updateChannelEnergy(window);
    }

    acquisition_cv_.notify_one();
}

void ModemEngine::feedAudio(const std::vector<float>& samples) {
    feedAudio(samples.data(), samples.size());
}

size_t ModemEngine::injectSignalFromFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        LOG_MODEM(ERROR, "Failed to open signal file: %s", filepath.c_str());
        return 0;
    }

    size_t file_size = file.tellg();
    size_t num_samples = file_size / sizeof(float);
    file.seekg(0);

    std::vector<float> samples(num_samples);
    file.read(reinterpret_cast<char*>(samples.data()), file_size);

    if (!file) {
        LOG_MODEM(ERROR, "Failed to read signal file: %s", filepath.c_str());
        return 0;
    }

    LOG_MODEM(INFO, "Injecting %zu samples from %s", num_samples, filepath.c_str());

    for (size_t offset = 0; offset < num_samples; offset += INJECT_CHUNK_SIZE) {
        size_t chunk = std::min(INJECT_CHUNK_SIZE, num_samples - offset);
        feedAudio(samples.data() + offset, chunk);
        std::this_thread::sleep_for(std::chrono::milliseconds(INJECT_CHUNK_DELAY_MS));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(INJECT_FINAL_DELAY_MS));
    return num_samples;
}

// ============================================================================
// BUFFER HELPERS
// ============================================================================

std::vector<float> ModemEngine::getBufferSnapshot() const {
    std::lock_guard<std::mutex> lock(rx_buffer_mutex_);
    return rx_sample_buffer_;
}

size_t ModemEngine::getBufferSize() const {
    std::lock_guard<std::mutex> lock(rx_buffer_mutex_);
    return rx_sample_buffer_.size();
}

void ModemEngine::consumeSamples(size_t count) {
    std::lock_guard<std::mutex> lock(rx_buffer_mutex_);
    if (count >= rx_sample_buffer_.size()) {
        rx_sample_buffer_.clear();
    } else {
        rx_sample_buffer_.erase(rx_sample_buffer_.begin(),
                                rx_sample_buffer_.begin() + count);
    }
}

} // namespace gui
} // namespace ultra
