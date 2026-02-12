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
// RX/DECODE THREAD
// ============================================================================

void ModemEngine::setSynchronousMode(bool enabled) {
    if (synchronous_mode_ == enabled) return;

    synchronous_mode_ = enabled;

    if (enabled) {
        // Stop decode thread — caller will drive processRxBuffer() directly
        stopRxDecodeThread();
        LOG_MODEM(INFO, "[%s] Synchronous RX mode enabled (no decode thread)", log_prefix_.c_str());
    } else {
        // Restart decode thread for async operation
        startRxDecodeThread();
        LOG_MODEM(INFO, "[%s] Asynchronous RX mode enabled (decode thread)", log_prefix_.c_str());
    }
}

void ModemEngine::processRxBuffer() {
    if (!streaming_decoder_) return;

    // Same as one iteration of rxDecodeLoop — process + deliver frames
    streaming_decoder_->processBuffer();

    while (streaming_decoder_->hasFrame()) {
        auto result = streaming_decoder_->getFrame();

        if (result.success) {
            updateStats([&](LoopbackStats& s) {
                s.frames_received++;
                s.snr_db = result.snr_db;
                s.synced = true;
            });
        } else if (result.codewords_failed > 0) {
            updateStats([](LoopbackStats& s) { s.frames_failed++; });
        }
    }
}

void ModemEngine::startRxDecodeThread() {
    if (rx_decode_running_ || synchronous_mode_) return;

    rx_decode_running_ = true;
    rx_decode_thread_ = std::thread(&ModemEngine::rxDecodeLoop, this);
    LOG_MODEM(INFO, "[%s] RX decode thread started", log_prefix_.c_str());
}

void ModemEngine::stopRxDecodeThread() {
    if (!rx_decode_running_) return;

    rx_decode_running_ = false;
    rx_decode_cv_.notify_all();

    // Signal StreamingDecoder to wake up and exit
    if (streaming_decoder_) {
        streaming_decoder_->stop();
    }

    if (rx_decode_thread_.joinable()) {
        rx_decode_thread_.join();
    }

    // Clear shutdown flag so decoder can be reused:
    // - In synchronous mode via processRxBuffer()
    // - Or via a new decode thread (startRxDecodeThread)
    if (streaming_decoder_) {
        streaming_decoder_->clearShutdown();
    }

    LOG_MODEM(INFO, "[%s] RX decode thread stopped", log_prefix_.c_str());
}

void ModemEngine::rxDecodeLoop() {
    LOG_MODEM(INFO, "[%s] RX decode loop starting", log_prefix_.c_str());

    while (rx_decode_running_) {
        // ====================================================================
        // StreamingDecoder is PRIMARY decoder
        // ====================================================================
        // Handles BOTH connected and disconnected modes:
        // - Circular buffer with sliding window chirp detection
        // - Correct IWaveform call sequence (fixes BUG-002)
        // - PING detection via energy ratio
        // - Frame delivery via callbacks (set in constructor)
        // ====================================================================

        if (!streaming_decoder_) {
            // Fallback: no decoder available, just sleep
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Drive the decode process (blocks until data available or timeout)
        streaming_decoder_->processBuffer();

        // Check for decoded frames
        // Note: Frame delivery happens via callback (set in constructor)
        // This loop is for logging and stats updates
        while (streaming_decoder_->hasFrame()) {
            auto result = streaming_decoder_->getFrame();

            if (result.success) {
                // Frame already logged by StreamingDecoder - just update stats
                updateStats([&](LoopbackStats& s) {
                    s.frames_received++;
                    s.snr_db = result.snr_db;
                    s.synced = true;
                });

            } else if (result.is_ping) {
                // PING already logged by StreamingDecoder
                // PING callback already fired

            } else if (result.codewords_failed > 0) {
                // Failure already logged by StreamingDecoder
                updateStats([](LoopbackStats& s) { s.frames_failed++; });
            }
        }
    }

    LOG_MODEM(INFO, "[%s] RX decode loop exiting", log_prefix_.c_str());
}

// ============================================================================
// AUDIO INPUT
// ============================================================================

void ModemEngine::feedAudio(const float* samples, size_t count) {
    if (count == 0 || samples == nullptr) return;

    // Lazy-start async decode thread on first audio, instead of in constructor.
    if (!synchronous_mode_ && !rx_decode_running_) {
        startRxDecodeThread();
    }

    // ========================================================================
    // AUDIO ROUTING - StreamingDecoder is PRIMARY
    // ========================================================================
    // StreamingDecoder handles BOTH connected and disconnected modes:
    // - Circular buffer with bounded size (4 seconds)
    // - Sliding window chirp detection
    // - Correct IWaveform call sequence (fixes BUG-002)
    // - Thread-safe with condition variable
    // ========================================================================

    if (streaming_decoder_) {
        streaming_decoder_->feedAudio(samples, count);
    }

    // Update carrier sense
    if (count >= ENERGY_WINDOW_SAMPLES) {
        std::vector<float> window(samples, samples + std::min(count, ENERGY_WINDOW_SAMPLES));
        updateChannelEnergy(window);
    }
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

} // namespace gui
} // namespace ultra
