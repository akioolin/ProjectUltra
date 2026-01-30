/**
 * ContinuousAudioSimulator - Real-time audio simulation for HF modem testing
 *
 * Models continuous audio flow exactly like real HF radio hardware:
 * - Audio is produced at exactly 48kHz sample rate (time-based)
 * - TX signals are mixed into the continuous stream
 * - Noise is always present when no signal (like real radio)
 * - Half-duplex operation supported
 *
 * This solves the "trailing silence" problem where simulators would need to
 * inject extra silence because audio wasn't continuously flowing.
 *
 * Usage:
 *   ContinuousAudioSimulator sim;
 *   sim.configure(snr_db, channel_condition);
 *
 *   // Station A wants to transmit
 *   sim.queueTx(StationId::A, samples);
 *
 *   // Main loop - call tick() frequently
 *   while (running) {
 *       sim.tick();  // Produces audio at 48kHz rate based on elapsed time
 *
 *       // Get audio for each station's RX
 *       auto rx_a = sim.readRx(StationId::A);  // What station A hears
 *       auto rx_b = sim.readRx(StationId::B);  // What station B hears
 *
 *       // Feed to modems
 *       modem_a.feedAudio(rx_a);
 *       modem_b.feedAudio(rx_b);
 *   }
 */

#pragma once

#include <vector>
#include <deque>
#include <mutex>
#include <random>
#include <chrono>
#include <cmath>
#include <memory>
#include <functional>
#include "hf_channel.hpp"

namespace ultra {
namespace sim {

// Station identifier for two-station simulation
enum class StationId { A, B };

// Channel condition presets
enum class ChannelCondition {
    AWGN,       // No fading, white noise only
    Good,       // ITU-R F.1487 "Good" (0.5ms delay, 0.1Hz Doppler)
    Moderate,   // ITU-R F.1487 "Moderate" (1.0ms, 0.5Hz)
    Poor,       // ITU-R F.1487 "Poor" (2.0ms, 1.0Hz)
    Flutter     // ITU-R F.1487 "Flutter" (0.5ms, 10Hz)
};

/**
 * ContinuousAudioSimulator - Simulates continuous 48kHz audio like real radio
 *
 * Key features:
 * 1. Time-based audio production - produces exactly 48 samples per millisecond
 * 2. Continuous noise floor - always generating noise like real radio
 * 3. TX queue mixing - TX signals are mixed into the stream at the right time
 * 4. Channel effects - AWGN and optional Watterson fading
 * 5. Half-duplex aware - can model PTT delays
 */
class ContinuousAudioSimulator {
public:
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr float SIGNAL_RMS = 0.1f;  // Typical signal RMS

    ContinuousAudioSimulator();
    ~ContinuousAudioSimulator() = default;

    // ========================================================================
    // CONFIGURATION
    // ========================================================================

    /**
     * Configure the channel parameters
     * @param snr_db Signal-to-noise ratio in dB
     * @param condition Channel fading condition
     * @param seed Random seed for reproducibility
     */
    void configure(float snr_db, ChannelCondition condition = ChannelCondition::AWGN,
                   uint32_t seed = 42);

    /**
     * Set SNR (can be changed during simulation)
     */
    void setSNR(float snr_db);
    float getSNR() const { return snr_db_; }

    /**
     * Set CFO (carrier frequency offset) in Hz
     * Note: CFO is typically applied during TX modulation, not in channel
     */
    void setCFO(float cfo_hz) { cfo_hz_ = cfo_hz; }
    float getCFO() const { return cfo_hz_; }

    // ========================================================================
    // TX QUEUE - Queue audio for transmission
    // ========================================================================

    /**
     * Queue TX samples for a station
     * The samples will be mixed into the appropriate RX stream
     * @param station Which station is transmitting
     * @param samples Audio samples to transmit
     */
    void queueTx(StationId station, const std::vector<float>& samples);

    /**
     * Check if station has pending TX
     */
    bool hasPendingTx(StationId station) const;

    /**
     * Get number of pending TX samples
     */
    size_t pendingTxSamples(StationId station) const;

    // ========================================================================
    // RX READ - Get continuous audio for reception
    // ========================================================================

    /**
     * Read RX samples for a station
     * Returns audio that this station "hears" (other station's TX + noise)
     * @param station Which station is receiving
     * @return Available audio samples (may be empty if tick() hasn't been called)
     */
    std::vector<float> readRx(StationId station);

    /**
     * Check if station has audio ready to read
     */
    bool hasRxAudio(StationId station) const;

    /**
     * Get number of available RX samples
     */
    size_t availableRxSamples(StationId station) const;

    // ========================================================================
    // SIMULATION CONTROL
    // ========================================================================

    /**
     * Advance simulation time and produce audio
     * Call this frequently (e.g., every 1-10ms in a loop)
     * Produces the appropriate number of samples based on elapsed real time
     */
    void tick();

    /**
     * Process a specific number of samples (ignores real time)
     * Useful for batch/fast processing where you don't want real-time pacing
     * @param num_samples Number of samples to process
     */
    void processSamples(size_t num_samples);

    /**
     * Reset simulation state (clears all buffers, resets time)
     */
    void reset();

    /**
     * Set maximum speedup factor for batch processing
     * 1.0 = real-time, 10.0 = 10x faster, 0 = unlimited (default)
     */
    void setMaxSpeedup(float factor) { max_speedup_ = factor; }

    // ========================================================================
    // STATISTICS
    // ========================================================================

    uint64_t getTotalSamplesProduced() const { return total_samples_produced_; }
    float getSimulationTimeSeconds() const { return total_samples_produced_ / (float)SAMPLE_RATE; }

private:
    // Configuration
    float snr_db_ = 20.0f;
    float cfo_hz_ = 0.0f;
    ChannelCondition condition_ = ChannelCondition::AWGN;
    uint32_t seed_ = 42;

    // Channel model
    std::unique_ptr<WattersonChannel> fading_channel_;
    std::mt19937 rng_;
    float noise_stddev_ = 0.01f;

    // TX queues (what each station wants to transmit)
    std::deque<float> tx_queue_a_;
    std::deque<float> tx_queue_b_;
    mutable std::mutex tx_mutex_a_;
    mutable std::mutex tx_mutex_b_;

    // RX buffers (what each station hears)
    std::deque<float> rx_buffer_a_;
    std::deque<float> rx_buffer_b_;
    mutable std::mutex rx_mutex_a_;
    mutable std::mutex rx_mutex_b_;

    // Timing
    std::chrono::steady_clock::time_point last_tick_time_;
    bool first_tick_ = true;
    float max_speedup_ = 0.0f;  // 0 = unlimited

    // Statistics
    uint64_t total_samples_produced_ = 0;

    // Internal helpers
    void updateNoiseLevel();
    std::vector<float> generateNoise(size_t count);
    std::vector<float> applyChannel(const std::vector<float>& samples);
    void produceAudio(size_t num_samples);
};

} // namespace sim
} // namespace ultra
