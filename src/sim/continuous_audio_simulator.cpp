/**
 * ContinuousAudioSimulator - Implementation
 *
 * This simulates continuous audio flow like a real HF radio:
 * - Audio is always flowing at 48kHz
 * - TX signals are mixed into the continuous stream
 * - Noise floor is always present
 */

#include "continuous_audio_simulator.hpp"
#include "ultra/logging.hpp"
#include <algorithm>
#include <cstring>

namespace ultra {
namespace sim {

ContinuousAudioSimulator::ContinuousAudioSimulator()
    : rng_(42)
{
    updateNoiseLevel();
}

void ContinuousAudioSimulator::configure(float snr_db, ChannelCondition condition, uint32_t seed) {
    snr_db_ = snr_db;
    condition_ = condition;
    seed_ = seed;
    rng_.seed(seed);

    // Create fading channel if needed
    if (condition != ChannelCondition::AWGN) {
        WattersonChannel::Config cfg;
        switch (condition) {
            case ChannelCondition::Good:
                cfg = itu_r_f1487::good(snr_db);
                break;
            case ChannelCondition::Moderate:
                cfg = itu_r_f1487::moderate(snr_db);
                break;
            case ChannelCondition::Poor:
                cfg = itu_r_f1487::poor(snr_db);
                break;
            case ChannelCondition::Flutter:
                cfg = itu_r_f1487::flutter(snr_db);
                break;
            default:
                cfg = itu_r_f1487::awgn(snr_db);
                break;
        }
        // Don't apply CFO in channel - it's applied at TX
        cfg.cfo_hz = 0.0f;
        fading_channel_ = std::make_unique<WattersonChannel>(cfg, seed);
    } else {
        fading_channel_.reset();
    }

    updateNoiseLevel();
}

void ContinuousAudioSimulator::setSNR(float snr_db) {
    snr_db_ = snr_db;
    updateNoiseLevel();

    // Update fading channel SNR if present
    if (fading_channel_) {
        configure(snr_db, condition_, seed_);
    }
}

void ContinuousAudioSimulator::updateNoiseLevel() {
    // Calculate noise standard deviation from SNR
    // SNR = signal_power / noise_power
    // noise_power = signal_power / SNR
    // noise_stddev = sqrt(noise_power)
    float snr_linear = std::pow(10.0f, snr_db_ / 10.0f);
    float signal_power = SIGNAL_RMS * SIGNAL_RMS;
    float noise_power = signal_power / snr_linear;
    noise_stddev_ = std::sqrt(noise_power);
}

void ContinuousAudioSimulator::queueTx(StationId station, const std::vector<float>& samples) {
    if (samples.empty()) return;

    // Calculate signal RMS for debugging
    float rms = 0.0f;
    for (float s : samples) rms += s * s;
    rms = std::sqrt(rms / samples.size());

    if (station == StationId::A) {
        std::lock_guard<std::mutex> lock(tx_mutex_a_);
        tx_queue_a_.insert(tx_queue_a_.end(), samples.begin(), samples.end());
        LOG_MODEM(DEBUG, "AudioSim: Station A queued TX: %zu samples, RMS=%.4f, queue_size=%zu",
                  samples.size(), rms, tx_queue_a_.size());
    } else {
        std::lock_guard<std::mutex> lock(tx_mutex_b_);
        tx_queue_b_.insert(tx_queue_b_.end(), samples.begin(), samples.end());
        LOG_MODEM(DEBUG, "AudioSim: Station B queued TX: %zu samples, RMS=%.4f, queue_size=%zu",
                  samples.size(), rms, tx_queue_b_.size());
    }
}

bool ContinuousAudioSimulator::hasPendingTx(StationId station) const {
    if (station == StationId::A) {
        std::lock_guard<std::mutex> lock(tx_mutex_a_);
        return !tx_queue_a_.empty();
    } else {
        std::lock_guard<std::mutex> lock(tx_mutex_b_);
        return !tx_queue_b_.empty();
    }
}

size_t ContinuousAudioSimulator::pendingTxSamples(StationId station) const {
    if (station == StationId::A) {
        std::lock_guard<std::mutex> lock(tx_mutex_a_);
        return tx_queue_a_.size();
    } else {
        std::lock_guard<std::mutex> lock(tx_mutex_b_);
        return tx_queue_b_.size();
    }
}

std::vector<float> ContinuousAudioSimulator::readRx(StationId station) {
    std::vector<float> result;

    if (station == StationId::A) {
        std::lock_guard<std::mutex> lock(rx_mutex_a_);
        result.assign(rx_buffer_a_.begin(), rx_buffer_a_.end());
        rx_buffer_a_.clear();
    } else {
        std::lock_guard<std::mutex> lock(rx_mutex_b_);
        result.assign(rx_buffer_b_.begin(), rx_buffer_b_.end());
        rx_buffer_b_.clear();
    }

    return result;
}

bool ContinuousAudioSimulator::hasRxAudio(StationId station) const {
    if (station == StationId::A) {
        std::lock_guard<std::mutex> lock(rx_mutex_a_);
        return !rx_buffer_a_.empty();
    } else {
        std::lock_guard<std::mutex> lock(rx_mutex_b_);
        return !rx_buffer_b_.empty();
    }
}

size_t ContinuousAudioSimulator::availableRxSamples(StationId station) const {
    if (station == StationId::A) {
        std::lock_guard<std::mutex> lock(rx_mutex_a_);
        return rx_buffer_a_.size();
    } else {
        std::lock_guard<std::mutex> lock(rx_mutex_b_);
        return rx_buffer_b_.size();
    }
}

void ContinuousAudioSimulator::tick() {
    auto now = std::chrono::steady_clock::now();

    if (first_tick_) {
        last_tick_time_ = now;
        first_tick_ = false;
        return;  // First tick just initializes time
    }

    // Calculate elapsed time in microseconds
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now - last_tick_time_).count();

    if (elapsed_us <= 0) return;

    // Calculate how many samples should have been produced
    // At 48kHz, that's 48 samples per millisecond = 0.048 samples per microsecond
    size_t samples_to_produce = static_cast<size_t>(elapsed_us * SAMPLE_RATE / 1000000.0);

    // Apply speedup limit if set
    if (max_speedup_ > 0.0f) {
        // Real-time would produce 'samples_to_produce' samples
        // Limit to max_speedup_ times that
        size_t max_samples = static_cast<size_t>(
            max_speedup_ * elapsed_us * SAMPLE_RATE / 1000000.0);
        samples_to_produce = std::min(samples_to_produce, max_samples);
    }

    if (samples_to_produce > 0) {
        produceAudio(samples_to_produce);
        last_tick_time_ = now;
    }
}

void ContinuousAudioSimulator::processSamples(size_t num_samples) {
    if (num_samples > 0) {
        produceAudio(num_samples);
    }
}

void ContinuousAudioSimulator::reset() {
    // Clear TX queues
    {
        std::lock_guard<std::mutex> lock(tx_mutex_a_);
        tx_queue_a_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(tx_mutex_b_);
        tx_queue_b_.clear();
    }

    // Clear RX buffers
    {
        std::lock_guard<std::mutex> lock(rx_mutex_a_);
        rx_buffer_a_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(rx_mutex_b_);
        rx_buffer_b_.clear();
    }

    // Reset timing
    first_tick_ = true;
    total_samples_produced_ = 0;

    // Reset RNG
    rng_.seed(seed_);

    // Recreate fading channel if present
    if (condition_ != ChannelCondition::AWGN) {
        configure(snr_db_, condition_, seed_);
    }
}

std::vector<float> ContinuousAudioSimulator::generateNoise(size_t count) {
    std::vector<float> noise(count);
    std::normal_distribution<float> dist(0.0f, noise_stddev_);
    for (float& s : noise) {
        s = dist(rng_);
    }
    return noise;
}

std::vector<float> ContinuousAudioSimulator::applyChannel(const std::vector<float>& samples) {
    if (samples.empty()) return samples;

    if (fading_channel_) {
        // Use Watterson fading channel (includes noise)
        SampleSpan span(const_cast<float*>(samples.data()), samples.size());
        return fading_channel_->process(span);
    } else {
        // AWGN only - add noise to signal
        std::vector<float> result = samples;
        std::normal_distribution<float> dist(0.0f, noise_stddev_);
        for (float& s : result) {
            s += dist(rng_);
        }
        return result;
    }
}

void ContinuousAudioSimulator::produceAudio(size_t num_samples) {
    // This is the core of continuous audio simulation
    // For each sample, we:
    // 1. Pull from TX queue if available (signal from transmitting station)
    // 2. Apply channel effects (fading + noise) OR just AWGN
    // 3. Push to other station's RX buffer
    //
    // If no TX signal, just produce noise

    // Process in chunks for efficiency
    constexpr size_t CHUNK_SIZE = 480;  // 10ms at 48kHz

    size_t remaining = num_samples;
    while (remaining > 0) {
        size_t chunk = std::min(remaining, CHUNK_SIZE);

        // === Station A TX -> Station B RX ===
        std::vector<float> a_to_b(chunk, 0.0f);
        {
            std::lock_guard<std::mutex> lock(tx_mutex_a_);
            size_t tx_available = std::min(chunk, tx_queue_a_.size());
            for (size_t i = 0; i < tx_available; i++) {
                a_to_b[i] = tx_queue_a_.front();
                tx_queue_a_.pop_front();
            }
        }

        // Apply channel (fading + noise or AWGN)
        a_to_b = applyChannel(a_to_b);

        // Add to station B's RX buffer
        {
            std::lock_guard<std::mutex> lock(rx_mutex_b_);
            rx_buffer_b_.insert(rx_buffer_b_.end(), a_to_b.begin(), a_to_b.end());

            // Prevent unbounded growth (keep max 20 seconds)
            constexpr size_t MAX_RX_SAMPLES = 20 * SAMPLE_RATE;
            while (rx_buffer_b_.size() > MAX_RX_SAMPLES) {
                rx_buffer_b_.pop_front();
            }
        }

        // === Station B TX -> Station A RX ===
        std::vector<float> b_to_a(chunk, 0.0f);
        size_t b_tx_taken = 0;
        {
            std::lock_guard<std::mutex> lock(tx_mutex_b_);
            size_t tx_available = std::min(chunk, tx_queue_b_.size());
            b_tx_taken = tx_available;
            for (size_t i = 0; i < tx_available; i++) {
                b_to_a[i] = tx_queue_b_.front();
                tx_queue_b_.pop_front();
            }
        }

        // Debug: log when we're actually processing signal (not just noise)
        static size_t b_signal_samples = 0;
        if (b_tx_taken > 0) {
            b_signal_samples += b_tx_taken;
            // Log every 10000 samples processed
            if (b_signal_samples % 10000 < chunk) {
                float rms = 0.0f;
                for (size_t i = 0; i < b_tx_taken; i++) rms += b_to_a[i] * b_to_a[i];
                rms = std::sqrt(rms / b_tx_taken);
                LOG_MODEM(DEBUG, "AudioSim: B->A: processed %zu signal samples (total=%zu), RMS=%.4f",
                          b_tx_taken, b_signal_samples, rms);
            }
        }

        // Apply channel
        b_to_a = applyChannel(b_to_a);

        // Add to station A's RX buffer
        {
            std::lock_guard<std::mutex> lock(rx_mutex_a_);
            rx_buffer_a_.insert(rx_buffer_a_.end(), b_to_a.begin(), b_to_a.end());

            // Prevent unbounded growth
            constexpr size_t MAX_RX_SAMPLES = 20 * SAMPLE_RATE;
            while (rx_buffer_a_.size() > MAX_RX_SAMPLES) {
                rx_buffer_a_.pop_front();
            }
        }

        remaining -= chunk;
        total_samples_produced_ += chunk;
    }
}

} // namespace sim
} // namespace ultra
