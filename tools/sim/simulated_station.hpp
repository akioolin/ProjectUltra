#pragma once

/**
 * Shared simulator station/audio plumbing extracted from cli_simulator.cpp.
 * CLI Simulator - Single Audio I/O Thread Model (like real sound card)
 *
 * Each station has ONE audio thread that handles both TX and RX,
 * exactly like a real sound card callback:
 *   - Every 10ms, read RX samples from channel
 *   - Feed RX to StreamingDecoder
 *   - Check if we have TX samples pending
 *   - Send TX samples to channel
 *
 * REFACTORED: Uses IWaveform + StreamingDecoder directly (not ModemEngine)
 * This ensures consistent configuration between TX and RX.
 */

#include <iostream>
#include <iomanip>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
#include "ultra/timing_profiler.hpp"
#include <queue>
#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>

#include "waveform/waveform_factory.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
#include "waveform/ofdm_cox_waveform.hpp"
#include "waveform/mc_dpsk_waveform.hpp"
#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"  // TX encoding (mirrors StreamingDecoder)
#include "protocol/protocol_engine.hpp"
#include "protocol/frame_v2.hpp"
#include "protocol/waveform_selection.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/fec.hpp"  // ChannelInterleaver, LDPCEncoder
#include "ultra/dsp.hpp"  // FFT for analytic-signal CFO injection
#include "fec/frame_interleaver.hpp"  // FrameInterleaver
#include "sim/hf_channel.hpp"

#include <cstdint>
#include <memory>
#include <random>
#include <utility>

using namespace ultra;
using namespace ultra::gui;
using namespace ultra::protocol;
using namespace ultra::sim;
namespace v2 = protocol::v2;

enum class ChannelType {
    AWGN,       // No fading, no multipath
    GOOD,       // 0.5ms delay, 0.1Hz Doppler (quiet mid-latitude)
    MODERATE,   // 1.0ms delay, 0.5Hz Doppler (typical mid-latitude)
    POOR,       // 2.0ms delay, 1.0Hz Doppler (disturbed conditions)
    FLUTTER     // 0.5ms delay, 10Hz Doppler (auroral/polar)
};

enum class OFDMConfigPreset {
    Default,  // OFDM_COX default constructor: 512 FFT, 30 carriers
    Nvis      // OFDMNvisWaveform::createNvisMode(): 1024 FFT, 59 carriers
};

static const char* channelTypeToString(ChannelType t) {
    switch (t) {
        case ChannelType::AWGN:     return "AWGN (no fading)";
        case ChannelType::GOOD:     return "Good (0.5ms, 0.1Hz)";
        case ChannelType::MODERATE: return "Moderate (1ms, 0.5Hz)";
        case ChannelType::POOR:     return "Poor (2ms, 1Hz)";
        case ChannelType::FLUTTER:  return "Flutter (0.5ms, 10Hz)";
        default:                    return "Unknown";
    }
}

static const char* ofdmConfigPresetToString(OFDMConfigPreset preset) {
    switch (preset) {
        case OFDMConfigPreset::Default: return "default";
        case OFDMConfigPreset::Nvis:    return "nvis";
        default:                        return "unknown";
    }
}

static float sampleRms(const std::vector<float>& samples) {
    if (samples.empty()) return 0.0f;
    double sum_sq = 0.0;
    for (float s : samples) {
        sum_sq += static_cast<double>(s) * static_cast<double>(s);
    }
    return static_cast<float>(std::sqrt(sum_sq / static_cast<double>(samples.size())));
}

static float samplePeak(const std::vector<float>& samples) {
    float peak = 0.0f;
    for (float s : samples) {
        peak = std::max(peak, std::fabs(s));
    }
    return peak;
}

static size_t countFullScaleSamples(const std::vector<float>& samples) {
    size_t count = 0;
    for (float s : samples) {
        if (std::fabs(s) > 1.0f) count++;
    }
    return count;
}

static float modeEfficiency(Modulation mod, CodeRate rate) {
    return static_cast<float>(getBitsPerSymbol(mod)) * getCodeRateValue(rate);
}

static const char* adaptationDirection(Modulation from_mod, CodeRate from_rate,
                                       Modulation to_mod, CodeRate to_rate) {
    float from_eff = modeEfficiency(from_mod, from_rate);
    float to_eff = modeEfficiency(to_mod, to_rate);
    if (to_eff > from_eff + 0.05f) return "improving";
    if (to_eff < from_eff - 0.05f) return "degrading";
    return "changing";
}

/**
 * SimulatedChannel - The "air" between two stations
 *
 * Handles AWGN noise and optional fading. Each direction (A->B, B->A)
 * has its own buffer that accumulates samples.
 */
class SimulatedChannel {
public:
    struct CapturedSignals {
        std::vector<float> a_tx_raw;
        std::vector<float> b_tx_raw;
        std::vector<float> a_rx_raw;
        std::vector<float> b_rx_raw;
        bool truncated = false;
        size_t max_samples = 0;
    };

    void setSeed(uint32_t seed) { seed_ = seed; }
    void setTxCFO(float cfo_hz) { tx_cfo_hz_ = cfo_hz; }
    float getTxCFO() const { return tx_cfo_hz_; }

    void setSignalCaptureEnabled(bool enabled) {
        capture_enabled_.store(enabled);
    }

    void setSignalCaptureMaxSamples(size_t max_samples) {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        capture_max_samples_ = max_samples;
    }

    void clearCapturedSignals() {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        captured_ = CapturedSignals{};
        captured_.max_samples = capture_max_samples_;
    }

    CapturedSignals getCapturedSignals() const {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        return captured_;
    }

    void configure(float snr_db, ChannelType channel_type = ChannelType::AWGN) {
        snr_db_ = snr_db;
        float snr_linear = std::pow(10.0f, snr_db / 10.0f);
        float signal_power = 0.01f;
        noise_stddev_ = std::sqrt(signal_power / snr_linear);
        rng_.seed(seed_);
        channel_a_to_b_.reset();
        channel_b_to_a_.reset();
        cfo_phase_a_to_b_ = 0.0f;
        cfo_phase_b_to_a_ = 0.0f;

        if (channel_type != ChannelType::AWGN) {
            WattersonChannel::Config cfg;
            switch (channel_type) {
                case ChannelType::GOOD:
                    cfg = itu_r_f1487::good(snr_db);
                    break;
                case ChannelType::MODERATE:
                    cfg = itu_r_f1487::moderate(snr_db);
                    break;
                case ChannelType::POOR:
                    cfg = itu_r_f1487::poor(snr_db);
                    break;
                case ChannelType::FLUTTER:
                    cfg = itu_r_f1487::flutter(snr_db);
                    break;
                default:
                    cfg = itu_r_f1487::moderate(snr_db);
                    break;
            }
            // Use simulator-side analytic CFO injection (below) for both AWGN and
            // fading modes so CFO behavior is consistent and non-distorting.
            cfg.cfo_hz = 0.0f;
            channel_a_to_b_ = std::make_unique<WattersonChannel>(cfg, seed_);
            channel_b_to_a_ = std::make_unique<WattersonChannel>(cfg, seed_ + 1);
        }
    }

    // Station A transmits -> goes to B's RX buffer
    void transmitFromA(const std::vector<float>& samples) {
        auto with_cfo = applyTxCFO(samples, cfo_phase_a_to_b_);
        auto processed = applyChannel(with_cfo, channel_a_to_b_.get());
        captureTxIfEnabled(samples, true);
        std::lock_guard<std::mutex> lock(mutex_b_rx_);
        for (float s : processed) {
            buffer_b_rx_.push(s);
        }
    }

    // Station B transmits -> goes to A's RX buffer
    void transmitFromB(const std::vector<float>& samples) {
        auto with_cfo = applyTxCFO(samples, cfo_phase_b_to_a_);
        auto processed = applyChannel(with_cfo, channel_b_to_a_.get());
        captureTxIfEnabled(samples, false);
        std::lock_guard<std::mutex> lock(mutex_a_rx_);
        for (float s : processed) {
            buffer_a_rx_.push(s);
        }
    }

    // Station A reads its RX (what B transmitted + noise)
    std::vector<float> receiveForA(size_t count) {
        std::lock_guard<std::mutex> lock(mutex_a_rx_);
        std::vector<float> result(count);
        for (size_t i = 0; i < count; i++) {
            if (!buffer_a_rx_.empty()) {
                result[i] = buffer_a_rx_.front();
                buffer_a_rx_.pop();
            } else {
                // No signal - just noise
                result[i] = noise_dist_(rng_) * noise_stddev_;
            }
        }
        captureRxIfEnabled(result, true);
        return result;
    }

    // Station B reads its RX (what A transmitted + noise)
    std::vector<float> receiveForB(size_t count) {
        std::lock_guard<std::mutex> lock(mutex_b_rx_);
        std::vector<float> result(count);
        for (size_t i = 0; i < count; i++) {
            if (!buffer_b_rx_.empty()) {
                result[i] = buffer_b_rx_.front();
                buffer_b_rx_.pop();
            } else {
                result[i] = noise_dist_(rng_) * noise_stddev_;
            }
        }
        captureRxIfEnabled(result, false);
        return result;
    }

private:
    float snr_db_ = 20.0f;
    float noise_stddev_ = 0.01f;
    float tx_cfo_hz_ = 0.0f;
    float cfo_phase_a_to_b_ = 0.0f;
    float cfo_phase_b_to_a_ = 0.0f;
    uint32_t seed_ = 42;
    std::mt19937 rng_{42};
    std::normal_distribution<float> noise_dist_{0.0f, 1.0f};

    std::unique_ptr<WattersonChannel> channel_a_to_b_;
    std::unique_ptr<WattersonChannel> channel_b_to_a_;

    std::mutex mutex_a_rx_, mutex_b_rx_;
    std::queue<float> buffer_a_rx_, buffer_b_rx_;

    std::atomic<bool> capture_enabled_{false};
    mutable std::mutex capture_mutex_;
    CapturedSignals captured_;
    size_t capture_max_samples_ = 0;  // 0 = unlimited

    void appendLimited(std::vector<float>& dst, const std::vector<float>& src) {
        if (src.empty()) return;

        if (capture_max_samples_ == 0) {
            dst.insert(dst.end(), src.begin(), src.end());
            return;
        }

        if (dst.size() >= capture_max_samples_) {
            captured_.truncated = true;
            return;
        }

        size_t room = capture_max_samples_ - dst.size();
        size_t take = std::min(room, src.size());
        dst.insert(dst.end(), src.begin(), src.begin() + static_cast<std::ptrdiff_t>(take));
        if (take < src.size()) {
            captured_.truncated = true;
        }
    }

    void captureTxIfEnabled(const std::vector<float>& tx_raw, bool from_a) {
        if (!capture_enabled_.load()) return;
        std::lock_guard<std::mutex> lock(capture_mutex_);
        captured_.max_samples = capture_max_samples_;
        if (from_a) {
            appendLimited(captured_.a_tx_raw, tx_raw);
        } else {
            appendLimited(captured_.b_tx_raw, tx_raw);
        }
    }

    void captureRxIfEnabled(const std::vector<float>& rx_raw, bool for_a) {
        if (!capture_enabled_.load()) return;
        std::lock_guard<std::mutex> lock(capture_mutex_);
        captured_.max_samples = capture_max_samples_;
        if (for_a) {
            appendLimited(captured_.a_rx_raw, rx_raw);
        } else {
            appendLimited(captured_.b_rx_raw, rx_raw);
        }
    }

    // Apply TX CFO as an analytic-signal frequency shift with phase continuity.
    // This models radio tuning offset without the amplitude distortion seen in the
    // Watterson helper's simplified CFO path.
    std::vector<float> applyTxCFO(const std::vector<float>& samples, float& phase_acc) {
        if (std::abs(tx_cfo_hz_) < 0.001f || samples.empty()) {
            return samples;
        }

        const size_t n = samples.size();
        size_t fft_size = 1;
        while (fft_size < n) fft_size <<= 1;

        FFT fft(fft_size);
        std::vector<Complex> time_in(fft_size, Complex(0.0f, 0.0f));
        std::vector<Complex> freq(fft_size, Complex(0.0f, 0.0f));
        std::vector<Complex> analytic(fft_size, Complex(0.0f, 0.0f));
        for (size_t i = 0; i < n; i++) {
            time_in[i] = Complex(samples[i], 0.0f);
        }

        fft.forward(time_in.data(), freq.data());

        // Hilbert transform in frequency domain to form analytic signal.
        if (fft_size >= 2) {
            for (size_t i = 1; i < fft_size / 2; i++) {
                freq[i] *= 2.0f;
            }
            for (size_t i = fft_size / 2 + 1; i < fft_size; i++) {
                freq[i] = Complex(0.0f, 0.0f);
            }
        }

        fft.inverse(freq.data(), analytic.data());

        std::vector<float> out(n, 0.0f);
        const float phase_inc = 2.0f * static_cast<float>(M_PI) * tx_cfo_hz_ / 48000.0f;
        float phase = phase_acc;
        for (size_t i = 0; i < n; i++) {
            Complex rot(std::cos(phase), std::sin(phase));
            out[i] = std::real(analytic[i] * rot);
            phase += phase_inc;
            if (phase > static_cast<float>(M_PI)) phase -= 2.0f * static_cast<float>(M_PI);
            else if (phase < -static_cast<float>(M_PI)) phase += 2.0f * static_cast<float>(M_PI);
        }
        phase_acc = phase;
        return out;
    }

    std::vector<float> applyChannel(const std::vector<float>& samples, WattersonChannel* fading) {
        if (fading) {
            SampleSpan span(const_cast<float*>(samples.data()), samples.size());
            return fading->process(span);
        } else {
            std::vector<float> result = samples;
            for (float& s : result) {
                s += noise_dist_(rng_) * noise_stddev_;
            }
            return result;
        }
    }
};

/**
 * AudioPort - Pluggable audio backend for a SimulatedStation.
 *
 * Two implementations:
 *   VirtualAudioPort  - Wraps SimulatedChannel for in-process two-station
 *                       tests (--role both). Audio flows in-memory between
 *                       stations through the channel sim.
 *   HardwareAudioPort - Wraps gui::AudioEngine (SDL2) for real-soundcard
 *                       I/O (--role A or --role B). One station per process,
 *                       audio flows out through speaker/cable to the peer.
 *
 * The SimulatedStation itself doesn't care which backend is used.
 */
class AudioPort {
public:
    virtual ~AudioPort() = default;
    // Pull up to `count` RX samples; pads with noise/zero if buffer underruns.
    virtual std::vector<float> pullRx(size_t count) = 0;
    // Push TX samples for transmission.
    virtual void queueTx(const std::vector<float>& samples) = 0;
    // VirtualAudioPort needs the simulator's 10 ms loop to pace samples into
    // the in-memory channel. HardwareAudioPort is already paced by SDL's real
    // device callback, so feeding it through a second 10 ms pacer can create
    // callback-phase underflows and synthetic gaps in the transmitted waveform.
    virtual bool shouldPaceTxInStationLoop() const { return true; }
    // Optional lifecycle hooks; default no-op.
    virtual bool start() { return true; }
    virtual void stop() {}
};

/**
 * VirtualAudioPort - In-process audio path through SimulatedChannel.
 * Preserves exact behavior of the original simulator pipe.
 */
class VirtualAudioPort : public AudioPort {
public:
    VirtualAudioPort(SimulatedChannel& channel, bool is_station_a)
        : channel_(channel), is_station_a_(is_station_a) {}

    std::vector<float> pullRx(size_t count) override {
        return is_station_a_ ? channel_.receiveForA(count)
                             : channel_.receiveForB(count);
    }

    void queueTx(const std::vector<float>& samples) override {
        if (is_station_a_) channel_.transmitFromA(samples);
        else               channel_.transmitFromB(samples);
    }

private:
    SimulatedChannel& channel_;
    bool is_station_a_;
};


class SimulatedStation {
public:
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int SAMPLES_PER_CALLBACK = 480;  // 10ms
    static constexpr int CALLBACK_INTERVAL_MS = 10;

    SimulatedStation(const std::string& callsign, std::unique_ptr<AudioPort> port,
                     OFDMConfigPreset ofdm_config_preset = OFDMConfigPreset::Default)
        : callsign_(callsign),
          port_(std::move(port)),
          ofdm_config_preset_(ofdm_config_preset) {

        protocol_.setLocalCallsign(callsign);
        protocol_.setAutoAccept(true);

        // Initialize OFDM_COX from the selected CLI preset.
        ofdm_config_ = createOFDMConfig();

        // Create TX encoder and RX decoder (both use same config)
        createEncoder();
        createDecoder();

        setupCallbacks();
    }

    ~SimulatedStation() {
        stop();
    }

    void start() {
        if (running_) return;
        if (port_ && !port_->start()) {
            LOG_MODEM(ERROR, "[%s] AudioPort start failed", callsign_.c_str());
            return;
        }
        running_ = true;
        audio_thread_ = std::thread(&SimulatedStation::audioLoop, this);
        decode_thread_ = std::thread(&SimulatedStation::decodeLoop, this);
    }

    void stop() {
        running_ = false;
        if (decoder_) decoder_->stop();
        if (audio_thread_.joinable()) {
            audio_thread_.join();
        }
        if (decode_thread_.joinable()) {
            decode_thread_.join();
        }
        if (port_) port_->stop();
    }

    // Protocol interface
    void connect(const std::string& remote) { protocol_.connect(remote); }
    void disconnect() { protocol_.disconnect(); }
    void sendMessage(const std::string& msg) { protocol_.sendMessage(msg); }
    void sendMessages(const std::vector<std::string>& msgs) { protocol_.sendMessages(msgs); }
    bool isConnected() const { return connected_; }
    bool isHandshakeComplete() const { return handshake_complete_; }
    bool isReadyToSend() { return protocol_.isReadyToSend(); }

    // File transfer interface
    bool sendFile(const std::string& filepath) { return protocol_.sendFile(filepath); }
    void setReceiveDirectory(const std::string& dir) { protocol_.setReceiveDirectory(dir); }
    bool isFileTransferInProgress() const { return protocol_.isFileTransferInProgress(); }
    protocol::FileTransferProgress getFileProgress() const { return protocol_.getFileProgress(); }

    // For receiving messages
    void setMessageCallback(std::function<void(const std::string&)> cb) {
        message_callback_ = cb;
    }

    // For receiving files
    void setFileReceivedCallback(std::function<void(const std::string&, bool)> cb) {
        file_received_callback_ = cb;
    }
    void setFileSentCallback(std::function<void(bool, const std::string&)> cb) {
        protocol_.setFileSentCallback(std::move(cb));
    }
    void setDecodedFrameCallback(std::function<void(const DecodeResult&)> cb) {
        decoded_frame_callback_ = std::move(cb);
    }

    void setSNR(float snr) { snr_db_ = snr; }
    void setAutoAccept(bool auto_accept) { protocol_.setAutoAccept(auto_accept); }
    void setForcedModulation(Modulation mod) { protocol_.setForcedModulation(mod); }
    void setForcedCodeRate(CodeRate rate) { protocol_.setForcedCodeRate(rate); }
    // forced=true → operator override, propagates via wire.
    // forced=false → boot-time default (encoder/decoder bootstrap only).
    void setFixedFrameCodewords(int cw_count, bool forced = true) {
        fixed_frame_codewords_ = v2::sanitizeFixedFrameCodewords(cw_count);
        protocol_.setForcedFrameCodewords(fixed_frame_codewords_, forced);
        if (encoder_) encoder_->setFixedFrameCodewords(fixed_frame_codewords_);
        if (decoder_) decoder_->setFixedFrameCodewords(fixed_frame_codewords_);
    }
    void setCarrierMask(uint64_t active_mask) {
        carrier_mask_ = active_mask;
        if (encoder_) encoder_->setCarrierMask(active_mask);
        if (decoder_) decoder_->setCarrierMask(active_mask);
    }
    void setCarrierLdpcInterleaver(bool enable) {
        carrier_ldpc_interleaver_enabled_ = enable;
        if (encoder_) encoder_->setCarrierLdpcInterleaver(enable);
        if (decoder_) decoder_->setCarrierLdpcInterleaver(enable);
        LOG_MODEM(INFO, "[%s] CarrierLDPC interleaver: %s",
                  callsign_.c_str(), enable ? "ENABLED" : "disabled");
    }
    void setSoftCombiningHARQ(bool enable) {
        protocol_.setSoftCombiningHARQ(enable);
        if (decoder_) decoder_->setSoftCombineBuffer(protocol_.softCombineBuffer());
    }
    void setPreferredWaveform(WaveformMode mode) {
        protocol_.setPreferredMode(mode);
        // For narrowband, use narrowband chirp for PING/PONG/CONNECT control frames
        if (mode == WaveformMode::OFDM_NARROW && encoder_) {
            encoder_->setNarrowbandControl(true);
        }
    }

    // Disable burst interleaving (for A/B testing)
    void setNoBurstInterleave(bool v) { no_burst_interleave_ = v; }
    void setBurstInterleaveGroupSize(int n) {
        burst_group_size_ = ofdm_link_adaptation::sanitizeBurstGroupSize(n);
        if (encoder_) encoder_->setBurstInterleaveGroupSize(burst_group_size_);
        if (decoder_) decoder_->setBurstInterleaveGroupSize(burst_group_size_);
    }
    void setRxOverfeedFactor(int n) { rx_overfeed_factor_ = std::clamp(n, 1, 200); }
    void setDecodeDelayMs(int ms) { decode_delay_ms_ = std::clamp(ms, 0, 500); }
    void setRxBatchCallbacks(int n) { rx_batch_callbacks_ = std::clamp(n, 1, 1000); }

    // Enable/disable channel interleaving on both TX encoder and RX decoder
    void setChannelInterleave(bool enable) {
        if (encoder_) {
            encoder_->setChannelInterleave(enable);
        }
        if (decoder_) {
            decoder_->setChannelInterleave(enable);
        }
        LOG_MODEM(INFO, "[%s] Channel interleaving: %s (TX+RX)",
                  callsign_.c_str(), enable ? "ENABLED" : "disabled");
    }

    void tick() {
        // Measure real wall-clock elapsed time since the previous tick, then
        // advance the protocol timers by that amount. Hard-coding
        // CALLBACK_INTERVAL_MS (10ms) caused a critical timing skew on the
        // hardware path: waitForRole sleeps 20ms between ticks, so ARQ timers
        // ran at HALF wall-clock speed — a configured 4.5s ACK timeout
        // actually fired at ~9s, causing 14-second "frame gaps" during file
        // transfer when ACKs were lost. Real-time tick advances fixes this
        // without requiring all callers to pass exact intervals.
        auto now = std::chrono::steady_clock::now();
        if (last_tick_time_.time_since_epoch().count() == 0) {
            last_tick_time_ = now;
            protocol_.tick(CALLBACK_INTERVAL_MS);
            return;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_tick_time_).count();
        // Clamp to a sane range — ticks longer than 1s probably mean the
        // process was suspended; clamp to 1s to avoid huge timer jumps.
        uint32_t elapsed_ms = static_cast<uint32_t>(
            std::clamp<int64_t>(elapsed, 1, 1000));
        last_tick_time_ = now;
        protocol_.tick(elapsed_ms);
    }

    float getSimTime() const { return total_samples_ / (float)SAMPLE_RATE; }

    // Stats accessors
    ConnectionStats getConnectionStats() const { return protocol_.getStats(); }
    DecoderStats getDecoderStats() const { return decoder_ ? decoder_->getStats() : DecoderStats{}; }
    std::string getCallsign() const { return callsign_; }
    WaveformMode getNegotiatedWaveform() const { return negotiated_waveform_; }
    Modulation getDataModulation() const { return data_modulation_; }
    CodeRate getDataCodeRate() const { return data_code_rate_; }
    int getFixedFrameCodewords() const { return fixed_frame_codewords_; }
    float getLastCFO() const { return last_cfo_hz_; }

    void resetAdaptiveAdvisory() {
        std::lock_guard<std::mutex> lock(adapt_mutex_);
        adapt_snr_window_.clear();
        adapt_fading_window_.clear();
        adapt_candidate_valid_ = false;
        adapt_candidate_hits_ = 0;
        adapt_virtual_mode_valid_ = false;
        adapt_upgrade_hold_logged_ = false;
    }

private:
    std::string callsign_;
    std::unique_ptr<AudioPort> port_;

    // TX: StreamingEncoder (unified TX encoding, mirrors StreamingDecoder)
    std::unique_ptr<StreamingEncoder> encoder_;
    WaveformMode tx_waveform_mode_ = WaveformMode::MC_DPSK;  // Start with DPSK for PING/CONNECT
    WaveformMode negotiated_waveform_ = WaveformMode::MC_DPSK;  // Store negotiated mode, switch after handshake

    // RX: StreamingDecoder
    std::unique_ptr<StreamingDecoder> decoder_;

    // OFDM configuration (shared between TX encoder and RX decoder)
    ModemConfig ofdm_config_;
    Modulation data_modulation_ = Modulation::DQPSK;
    CodeRate data_code_rate_ = CodeRate::R1_4;

    // Protocol engine
    ProtocolEngine protocol_{ConnectionConfig{}};

    std::atomic<bool> running_{false};
    std::thread audio_thread_;
    std::thread decode_thread_;
    // Real-time tick accounting (see tick() above).
    std::chrono::steady_clock::time_point last_tick_time_{};

    // TX queue - samples waiting to be transmitted
    std::mutex tx_mutex_;
    std::queue<float> tx_queue_;
    uint64_t tx_sample_clock_ = 0;  // Continuous TX capture sample cursor.

    // State
    std::atomic<bool> connected_{false};
    std::atomic<bool> handshake_complete_{false};
    float last_cfo_hz_ = 0.0f;  // CFO from chirp detection, used for light preamble

    std::function<void(const std::string&)> message_callback_;
    std::function<void(const std::string&, bool)> file_received_callback_;
    std::function<void(const DecodeResult&)> decoded_frame_callback_;

    std::atomic<uint64_t> total_samples_{0};
    float snr_db_ = 20.0f;
    bool no_burst_interleave_ = false;  // Disable burst interleaving for A/B testing
    int burst_group_size_ = 8;
    int fixed_frame_codewords_ = v2::kDefaultFixedFrameCodewords;
    uint64_t carrier_mask_ = UINT64_MAX;
    bool carrier_ldpc_interleaver_enabled_ = false;
    int rx_overfeed_factor_ = 1;
    int decode_delay_ms_ = 0;
    int rx_batch_callbacks_ = 1;
    int rx_batch_counter_ = 0;
    std::vector<float> rx_batch_buffer_;

    // Local adaptive advisory (log-only)
    std::deque<float> adapt_snr_window_;
    std::deque<float> adapt_fading_window_;
    std::mutex adapt_mutex_;
    bool adapt_candidate_valid_ = false;
    Modulation adapt_candidate_mod_ = Modulation::DQPSK;
    CodeRate adapt_candidate_rate_ = CodeRate::R1_4;
    int adapt_candidate_hits_ = 0;
    bool adapt_virtual_mode_valid_ = false;
    Modulation adapt_virtual_mod_ = Modulation::DQPSK;
    CodeRate adapt_virtual_rate_ = CodeRate::R1_4;
    std::chrono::steady_clock::time_point adapt_last_virtual_switch_;
    bool adapt_upgrade_hold_logged_ = false;
    Modulation adapt_upgrade_hold_mod_ = Modulation::DQPSK;
    CodeRate adapt_upgrade_hold_rate_ = CodeRate::R1_4;
    static constexpr size_t ADAPT_WINDOW_FRAMES = 5;
    static constexpr int ADAPT_DOWNGRADE_WINDOWS = 2;
    static constexpr int ADAPT_UPGRADE_WINDOWS = 4;
    static constexpr int ADAPT_UPGRADE_HOLD_MS = 8000;
    OFDMConfigPreset ofdm_config_preset_ = OFDMConfigPreset::Default;

    ModemConfig createOFDMConfig() {
        ModemConfig cfg;
        if (ofdm_config_preset_ == OFDMConfigPreset::Nvis) {
            auto nvis = OFDMNvisWaveform::createNvisMode();
            cfg = nvis->getConfig();
        } else {
            OFDMNvisWaveform default_cox;
            cfg = default_cox.getConfig();
        }

        cfg.sample_rate = SAMPLE_RATE;
        cfg.center_freq = 1500.0f;
        cfg.modulation = data_modulation_;
        cfg.code_rate = data_code_rate_;
        cfg.use_pilots = true;
        cfg.pilot_spacing =
            ofdm_link_adaptation::recommendedPilotSpacing(cfg.modulation, cfg.code_rate);

        LOG_MODEM(INFO, "[%s] OFDM_COX config preset=%s FFT=%d carriers=%d CP=%d pilots=%d spacing=%d",
                  callsign_.c_str(), ofdmConfigPresetToString(ofdm_config_preset_),
                  static_cast<int>(cfg.fft_size), static_cast<int>(cfg.num_carriers),
                  static_cast<int>(cfg.getCyclicPrefix()),
                  cfg.use_pilots ? 1 : 0, static_cast<int>(cfg.pilot_spacing));
        return cfg;
    }

    void createEncoder() {
        if (!encoder_) {
            encoder_ = std::make_unique<StreamingEncoder>();
        }

        // Configure encoder with current settings
        encoder_->setOFDMConfig(ofdm_config_);
        encoder_->setMode(tx_waveform_mode_);
        encoder_->setDataMode(data_modulation_, data_code_rate_);
        encoder_->setFixedFrameCodewords(fixed_frame_codewords_);
        encoder_->setCarrierMask(carrier_mask_);
        encoder_->setCarrierLdpcInterleaver(carrier_ldpc_interleaver_enabled_);
        encoder_->setBurstInterleaveGroupSize(burst_group_size_);
        encoder_->setMCDPSKCarriers(8);

        LOG_MODEM(INFO, "[%s] TX encoder: mode=%s, carriers=%d, data_carriers=%d",
                  callsign_.c_str(),
                  waveformModeToString(tx_waveform_mode_),
                  encoder_->getConfig().num_carriers,
                  encoder_->getConfig().data_carriers);
    }

    void createDecoder() {
        decoder_ = std::make_unique<StreamingDecoder>();
        decoder_->setLogPrefix(callsign_);

        // Start in disconnected mode (MC_DPSK for PING detection)
        decoder_->setMode(WaveformMode::MC_DPSK, false);
        decoder_->setBurstInterleaveGroupSize(burst_group_size_);
        decoder_->setFixedFrameCodewords(fixed_frame_codewords_);
        decoder_->setCarrierMask(carrier_mask_);
        decoder_->setCarrierLdpcInterleaver(carrier_ldpc_interleaver_enabled_);
        decoder_->setSoftCombineBuffer(protocol_.softCombineBuffer());
        decoder_->setMCDPSKCarriers(8);

        // Set frame callback
        decoder_->setFrameCallback([this](const DecodeResult& result) {
            handleDecodedFrame(result);
        });

        // Set ping callback
        decoder_->setPingCallback([this](float snr_db, float cfo_hz) {
            last_cfo_hz_ = cfo_hz;
            // If narrowband chirp was detected, switch control waveform to narrowband
            // and set session-scoped override so negotiateMode() picks OFDM_NARROW
            if (decoder_->getDetectedBandwidth() == BandwidthMode::NARROW) {
                if (encoder_) {
                    encoder_->setNarrowbandControl(true);
                }
                protocol_.setNarrowbandOverride(WaveformMode::OFDM_NARROW);
                LOG_MODEM(INFO, "[%s] Narrowband chirp detected, switching control waveform + narrowband override", callsign_.c_str());
            }
            protocol_.onPingReceived();
        });
    }

    void handleDecodedFrame(const DecodeResult& result) {
        if (!result.success) return;

        if (result.is_ping) {
            // PING handled by ping callback
            return;
        }

        // Update CFO from frame
        last_cfo_hz_ = result.cfo_hz;

        // Pass frame data to protocol
        if (!result.frame_data.empty()) {
            auto header = v2::parseHeader(result.frame_data);
            if (header.valid && !v2::isAddressedToCallsign(header, callsign_)) {
                return;
            }

            if (decoded_frame_callback_) {
                decoded_frame_callback_(result);
            }

            float fading_index = decoder_ ? decoder_->getLastFadingIndex() : 0.0f;
            protocol_.setChannelQuality(snr_db_, fading_index);
            protocol_.onRxData(result.frame_data);
            updateAdaptiveAdvisory(snr_db_, fading_index);
        }
    }

    void updateAdaptiveAdvisory(float snr_db, float fading_index) {
        if (!std::isfinite(snr_db) || !std::isfinite(fading_index)) {
            return;
        }
        if (!connected_.load()) {
            return;
        }
        std::lock_guard<std::mutex> lock(adapt_mutex_);

        adapt_snr_window_.push_back(snr_db);
        adapt_fading_window_.push_back(fading_index);
        if (adapt_snr_window_.size() > ADAPT_WINDOW_FRAMES) {
            adapt_snr_window_.pop_front();
        }
        if (adapt_fading_window_.size() > ADAPT_WINDOW_FRAMES) {
            adapt_fading_window_.pop_front();
        }
        if (adapt_snr_window_.size() < ADAPT_WINDOW_FRAMES ||
            adapt_fading_window_.size() < ADAPT_WINDOW_FRAMES) {
            return;
        }

        float avg_snr = 0.0f;
        for (float v : adapt_snr_window_) avg_snr += v;
        avg_snr /= static_cast<float>(adapt_snr_window_.size());

        float avg_fading = 0.0f;
        for (float v : adapt_fading_window_) avg_fading += v;
        avg_fading /= static_cast<float>(adapt_fading_window_.size());

        WaveformMode wf = tx_waveform_mode_;
        if (wf == WaveformMode::MC_DPSK) {
            // During data exchange we evaluate against negotiated waveform.
            wf = negotiated_waveform_;
        }

        Modulation current_mod = data_modulation_;
        CodeRate current_rate = data_code_rate_;

        if (!adapt_virtual_mode_valid_) {
            adapt_virtual_mode_valid_ = true;
            adapt_virtual_mod_ = current_mod;
            adapt_virtual_rate_ = current_rate;
            adapt_last_virtual_switch_ = std::chrono::steady_clock::now();
        }

        Modulation rec_mod = current_mod;
        CodeRate rec_rate = current_rate;
        protocol::recommendDataMode(avg_snr, wf, rec_mod, rec_rate, avg_fading);

        Modulation eval_mod = adapt_virtual_mod_;
        CodeRate eval_rate = adapt_virtual_rate_;

        if (rec_mod == eval_mod && rec_rate == eval_rate) {
            adapt_candidate_valid_ = false;
            adapt_candidate_hits_ = 0;
            adapt_upgrade_hold_logged_ = false;
            return;
        }

        if (adapt_candidate_valid_ &&
            adapt_candidate_mod_ == rec_mod &&
            adapt_candidate_rate_ == rec_rate) {
            ++adapt_candidate_hits_;
        } else {
            adapt_candidate_valid_ = true;
            adapt_candidate_mod_ = rec_mod;
            adapt_candidate_rate_ = rec_rate;
            adapt_candidate_hits_ = 1;
        }

        bool is_upgrade = modeEfficiency(rec_mod, rec_rate) > modeEfficiency(eval_mod, eval_rate) + 0.05f;
        int required_windows = is_upgrade ? ADAPT_UPGRADE_WINDOWS : ADAPT_DOWNGRADE_WINDOWS;
        if (adapt_candidate_hits_ < required_windows) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        if (is_upgrade) {
            auto elapsed_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - adapt_last_virtual_switch_).count());
            int hold_remaining_ms = ADAPT_UPGRADE_HOLD_MS - elapsed_ms;
            if (hold_remaining_ms > 0) {
                if (!adapt_upgrade_hold_logged_ ||
                    adapt_upgrade_hold_mod_ != rec_mod ||
                    adapt_upgrade_hold_rate_ != rec_rate) {
                    LOG_MODEM(INFO,
                              "[%s][ADPT] Local improving conditions (SNR=%.1f dB, F.I.=%.2f): "
                              "hysteresis hold %.1fs before upgrade to %s %s",
                              callsign_.c_str(), avg_snr, avg_fading,
                              hold_remaining_ms / 1000.0f,
                              modulationToString(rec_mod), codeRateToString(rec_rate));
                    adapt_upgrade_hold_logged_ = true;
                    adapt_upgrade_hold_mod_ = rec_mod;
                    adapt_upgrade_hold_rate_ = rec_rate;
                }
                return;
            }
        }

        adapt_upgrade_hold_logged_ = false;
        const char* direction = adaptationDirection(eval_mod, eval_rate, rec_mod, rec_rate);
        LOG_MODEM(INFO,
                  "[%s][ADPT] Local %s conditions (SNR=%.1f dB, F.I.=%.2f): "
                  "hysteresis allows switch %s %s -> %s %s",
                  callsign_.c_str(), direction, avg_snr, avg_fading,
                  modulationToString(eval_mod), codeRateToString(eval_rate),
                  modulationToString(rec_mod), codeRateToString(rec_rate));

        adapt_virtual_mod_ = rec_mod;
        adapt_virtual_rate_ = rec_rate;
        adapt_last_virtual_switch_ = now;
        adapt_candidate_valid_ = false;
        adapt_candidate_hits_ = 0;
    }

    void logPeerAdaptiveAdvisory(Modulation current_mod, CodeRate current_rate,
                                 float peer_snr_db, float peer_fading) {
        if (!std::isfinite(peer_snr_db) || !std::isfinite(peer_fading) || peer_fading < 0.0f) {
            return;
        }

        WaveformMode wf = negotiated_waveform_;
        if (wf == WaveformMode::AUTO) {
            wf = tx_waveform_mode_;
        }

        Modulation peer_mod = current_mod;
        CodeRate peer_rate = current_rate;
        protocol::recommendDataMode(peer_snr_db, wf, peer_mod, peer_rate, peer_fading);

        bool peer_change = (peer_mod != current_mod || peer_rate != current_rate);
        if (peer_change) {
            const char* direction = adaptationDirection(current_mod, current_rate, peer_mod, peer_rate);
            LOG_MODEM(INFO,
                      "[%s][ADPT] Peer reports %s conditions (SNR=%.1f dB, F.I.=%.2f): %s -> %s %s",
                      callsign_.c_str(), direction, peer_snr_db, peer_fading,
                      direction, modulationToString(peer_mod), codeRateToString(peer_rate));
        } else {
            LOG_MODEM(INFO,
                      "[%s][ADPT] Peer reports stable conditions (SNR=%.1f dB, F.I.=%.2f): keep %s %s",
                      callsign_.c_str(), peer_snr_db, peer_fading,
                      modulationToString(current_mod), codeRateToString(current_rate));
        }
    }

    void setWaveformMode(WaveformMode mode) {
        if (tx_waveform_mode_ == mode) return;

        tx_waveform_mode_ = mode;
        createEncoder();

        LOG_MODEM(INFO, "[%s] Switched to waveform: %s",
                  callsign_.c_str(), waveformModeToString(mode));
    }

    // Verify TX encoder and RX decoder have matching configs
    void verifyTxRxConfig() {
        if (!encoder_ || !decoder_) return;

        auto tx_cfg = encoder_->getConfig();
        auto rx_cfg = decoder_->getConfig();

        bool mismatch = false;
        std::string issues;

        if (tx_cfg.mode != rx_cfg.mode) {
            issues += "mode; ";
            mismatch = true;
        }
        if (tx_cfg.data_carriers != rx_cfg.data_carriers) {
            char buf[64];
            snprintf(buf, sizeof(buf), "data_carriers(TX=%d RX=%d); ",
                     tx_cfg.data_carriers, rx_cfg.data_carriers);
            issues += buf;
            mismatch = true;
        }
        if (tx_cfg.bits_per_symbol != rx_cfg.bits_per_symbol) {
            char buf[64];
            snprintf(buf, sizeof(buf), "bits_per_symbol(TX=%d RX=%d); ",
                     tx_cfg.bits_per_symbol, rx_cfg.bits_per_symbol);
            issues += buf;
            mismatch = true;
        }
        if (tx_cfg.use_channel_interleave != rx_cfg.use_channel_interleave) {
            char buf[64];
            snprintf(buf, sizeof(buf), "channel_interleave(TX=%s RX=%s); ",
                     tx_cfg.use_channel_interleave ? "yes" : "no",
                     rx_cfg.use_channel_interleave ? "yes" : "no");
            issues += buf;
            mismatch = true;
        }

        if (mismatch) {
            LOG_MODEM(WARN, "[%s] TX/RX CONFIG MISMATCH: %s", callsign_.c_str(), issues.c_str());
        } else {
            LOG_MODEM(INFO, "[%s] TX/RX config verified: %d data carriers, %d bits/symbol, ch_interleave=%s",
                      callsign_.c_str(), tx_cfg.data_carriers, tx_cfg.bits_per_symbol,
                      tx_cfg.use_channel_interleave ? "yes" : "no");
        }
    }

    void setDataMode(Modulation mod, CodeRate rate) {
        data_modulation_ = mod;
        data_code_rate_ = rate;
        resetAdaptiveAdvisory();

        // Update OFDM config with pilots based on code rate
        ofdm_config_.modulation = mod;
        ofdm_config_.code_rate = rate;
        ofdm_config_.use_pilots = true;
        ofdm_config_.pilot_spacing = ofdm_link_adaptation::recommendedPilotSpacing(mod, rate);

        // Recreate TX waveform if it's OFDM
        if (tx_waveform_mode_ != WaveformMode::MC_DPSK) {
            createEncoder();
        }

        // Update RX decoder
        if (decoder_) {
            // Only update OFDM config for OFDM modes - MC-DPSK doesn't use it
            if (negotiated_waveform_ != WaveformMode::MC_DPSK) {
                decoder_->setOFDMConfig(ofdm_config_);
            }
            decoder_->setDataMode(mod, rate);
        }

        LOG_MODEM(INFO, "[%s] Data mode: %s %s (pilots=%d, spacing=%d)",
                  callsign_.c_str(), modulationToString(mod), codeRateToString(rate),
                  ofdm_config_.use_pilots ? 1 : 0, ofdm_config_.pilot_spacing);
    }

    void setConnected(bool connected) {
        if (connected_.load() == connected) return;

        connected_ = connected;
        resetAdaptiveAdvisory();

        if (connected) {
            // For OFDM_NARROW, switch to narrowband OFDM config
            if (negotiated_waveform_ == WaveformMode::OFDM_NARROW) {
                ofdm_config_ = presets::narrowbandOFDM();
            }

            // Keep OFDM config in sync with negotiated data mode before switching RX/TX.
            ofdm_config_.modulation = data_modulation_;
            ofdm_config_.code_rate = data_code_rate_;
            ofdm_config_.use_pilots = true;
            ofdm_config_.pilot_spacing =
                ofdm_link_adaptation::recommendedPilotSpacing(data_modulation_, data_code_rate_);

            // Switch to negotiated waveform now
            if (negotiated_waveform_ != WaveformMode::MC_DPSK) {
                setWaveformMode(negotiated_waveform_);
                if (decoder_) {
                    decoder_->setConnectedOFDMMode(negotiated_waveform_, ofdm_config_,
                                                   data_modulation_, data_code_rate_);
                    decoder_->setBurstInterleaveGroupSize(burst_group_size_);
                    decoder_->setKnownCFO(last_cfo_hz_);
                }
                // Enable burst interleaving for OFDM_CHIRP (not COX — no LTS marker)
                if (negotiated_waveform_ == WaveformMode::OFDM_CHIRP && !no_burst_interleave_) {
                    if (encoder_) encoder_->setBurstInterleave(true);
                    if (decoder_) decoder_->setBurstInterleave(true);
                    LOG_MODEM(INFO, "[%s] Burst interleaving ENABLED (group=%d)",
                              callsign_.c_str(), burst_group_size_);
                }
                LOG_MODEM(INFO, "[%s] Entered CONNECTED state, switched to %s, CFO=%.1f Hz",
                          callsign_.c_str(), waveformModeToString(negotiated_waveform_), last_cfo_hz_);
                verifyTxRxConfig();
            } else {
                // MC-DPSK: Still need to update decoder's connected state and CFO
                if (decoder_) {
                    decoder_->setMode(WaveformMode::MC_DPSK, true);  // true = connected
                    decoder_->setDataMode(data_modulation_, data_code_rate_);
                    decoder_->setKnownCFO(last_cfo_hz_);
                }
                LOG_MODEM(INFO, "[%s] Entered CONNECTED state (MC-DPSK), CFO=%.1f Hz",
                          callsign_.c_str(), last_cfo_hz_);
                verifyTxRxConfig();
            }
        } else {
            // Switch back to disconnected mode (MC_DPSK for PING detection)
            if (decoder_) {
                decoder_->setMode(WaveformMode::MC_DPSK, false);
            }
            // Clear burst interleave state on disconnect
            if (encoder_) encoder_->setBurstInterleave(false);
            if (decoder_) decoder_->setBurstInterleave(false);
            // Reset TX encoder to MC-DPSK
            if (tx_waveform_mode_ != WaveformMode::MC_DPSK) {
                tx_waveform_mode_ = WaveformMode::MC_DPSK;
                createEncoder();
            }
            handshake_complete_ = false;
            negotiated_waveform_ = WaveformMode::MC_DPSK;
            LOG_MODEM(INFO, "[%s] Entered DISCONNECTED state", callsign_.c_str());
        }
    }

    void setHandshakeComplete(bool complete) {
        handshake_complete_ = complete;
        if (complete) {
            LOG_MODEM(INFO, "[%s] Handshake complete", callsign_.c_str());
        }
    }

    std::vector<float> transmitFrame(const Bytes& data) {
        if (!encoder_) {
            LOG_MODEM(ERROR, "[%s] No TX encoder!", callsign_.c_str());
            return {};
        }

        // Check frame type to determine encoding mode
        // CONNECT/CONNECT_ACK always use MC-DPSK (even before negotiation)
        // All other frames use the negotiated waveform
        bool is_handshake_frame = false;
        if (data.size() >= 3) {
            uint8_t frame_type = data[2];  // Type is at byte 2 (after 2-byte magic)
            is_handshake_frame = (frame_type == 0x12 || frame_type == 0x13);  // CONNECT, CONNECT_ACK
        }

        // Temporarily switch encoder mode for handshake frames
        auto saved_mode = encoder_->getMode();
        auto saved_rate = encoder_->getCodeRate();

        if (is_handshake_frame) {
            // Handshake frames always use MC-DPSK R1/4
            encoder_->setMode(WaveformMode::MC_DPSK);
            encoder_->setDataMode(Modulation::DQPSK, CodeRate::R1_4);
        }

        // Encode frame using the encoder
        // MC-DPSK: ALWAYS use full preamble (is_mc_dpsk covers all MC-DPSK frames incl ACK/NACK)
        // OFDM: Use light preamble for all frames after handshake (DATA, ACK, NACK, etc.)
        // Handshake frames (CONNECT/CONNECT_ACK): Always full preamble (pre-negotiation)
        std::vector<float> result;
        bool is_mc_dpsk = (encoder_->getMode() == WaveformMode::MC_DPSK);
        bool use_light = !is_handshake_frame && !is_mc_dpsk &&
                         connected_.load() && handshake_complete_.load();

        if (use_light) {
            result = encoder_->encodeFrameLight(data);
        } else {
            result = encoder_->encodeFrame(data);
        }

        // Restore encoder mode if we changed it
        if (is_handshake_frame) {
            encoder_->setMode(saved_mode);
            encoder_->setDataMode(data_modulation_, saved_rate);
        }

        LOG_MODEM(INFO, "[%s] TX frame: %zu bytes -> %zu samples (mode=%s, %s)",
                  callsign_.c_str(), data.size(), result.size(),
                  is_handshake_frame ? "MC-DPSK" : waveformModeToString(encoder_->getMode()),
                  use_light ? "light" : "full");

        return result;
    }

    std::vector<float> transmitBurst(const std::vector<Bytes>& frame_data_list) {
        if (!encoder_ || frame_data_list.empty()) return {};

        // All burst frames use connected OFDM mode
        auto saved_mode = encoder_->getMode();
        auto saved_rate = encoder_->getCodeRate();

        // Ensure encoder is in connected OFDM mode
        if (tx_waveform_mode_ != WaveformMode::MC_DPSK) {
            encoder_->setMode(tx_waveform_mode_);
            encoder_->setDataMode(data_modulation_, data_code_rate_);
        }

        auto result = encoder_->encodeBurstLight(frame_data_list);

        LOG_MODEM(INFO, "[%s] TX burst: %zu frames -> %zu samples (mode=%s)",
                  callsign_.c_str(), frame_data_list.size(), result.size(),
                  waveformModeToString(tx_waveform_mode_));

        return result;
    }

    std::vector<float> transmitPing() {
        if (!encoder_) return {};
        auto samples = encoder_->encodePing();
        LOG_MODEM(INFO, "[%s] TX PING waveform: samples=%zu rms=%.4f peak=%.4f",
                  callsign_.c_str(), samples.size(), sampleRms(samples),
                  samplePeak(samples));
        return samples;
    }

    std::vector<float> transmitPong() {
        if (!encoder_) return {};
        auto samples = encoder_->encodePing();
        LOG_MODEM(INFO, "[%s] TX PONG waveform: samples=%zu rms=%.4f peak=%.4f",
                  callsign_.c_str(), samples.size(), sampleRms(samples),
                  samplePeak(samples));
        return samples;
    }

    void setupCallbacks() {
        // TX callback - encode and modulate frame
        protocol_.setTxDataCallback([this](const Bytes& data) {
            auto samples = transmitFrame(data);
            queueTx(samples, txFrameLabel(data));
        });

        // Connection state changes
        protocol_.setConnectionChangedCallback([this](ConnectionState state, const std::string&) {
            if (state == ConnectionState::CONNECTED) {
                setConnected(true);
            } else if (state == ConnectionState::DISCONNECTED) {
                setConnected(false);
            }
        });

        // Data mode changes (modulation + code rate + CW count). The CW
        // count comes from the wire (CONNECT_ACK / MODE_CHANGE) — protocol
        // layer has already set its own data_frame_cw_count_, this just
        // syncs the encoder + decoder. NO re-entry into protocol_ here:
        // the engine mutex is held while the callback fires (the deadlock
        // we caught on 2026-05-04).
        protocol_.setDataModeChangedCallback([this](Modulation mod, CodeRate rate,
                                                    int cw_count,
                                                    float peer_snr_db, float peer_fading) {
            setDataMode(mod, rate);
            if (cw_count > 0 && cw_count != fixed_frame_codewords_) {
                fixed_frame_codewords_ = v2::sanitizeFixedFrameCodewords(cw_count);
                if (encoder_) encoder_->setFixedFrameCodewords(fixed_frame_codewords_);
                if (decoder_) decoder_->setFixedFrameCodewords(fixed_frame_codewords_);
                LOG_MODEM(INFO, "[%s] Negotiated CW count: %d for %s %s "
                                 "(peer SNR=%.1f, fading=%.2f)",
                          callsign_.c_str(), fixed_frame_codewords_,
                          modulationToString(mod), codeRateToString(rate),
                          peer_snr_db, peer_fading);
            }
            logPeerAdaptiveAdvisory(mod, rate, peer_snr_db, peer_fading);
        });

        // Waveform mode changes - store but DON'T switch yet (still need MC-DPSK for CONNECT_ACK)
        protocol_.setModeNegotiatedCallback([this](WaveformMode mode) {
            negotiated_waveform_ = mode;
            LOG_MODEM(INFO, "[%s] Mode negotiated: %s (will switch after handshake)",
                      callsign_.c_str(), waveformModeToString(mode));
        });

        protocol_.setConnectWaveformChangedCallback([this](WaveformMode mode) {
            negotiated_waveform_ = mode;
            LOG_MODEM(INFO, "[%s] Connect waveform set: %s (staying on MC-DPSK for handshake)",
                      callsign_.c_str(), waveformModeToString(mode));
        });

        protocol_.setPhyMaskV1NegotiatedCallback([this](bool enabled) {
            setCarrierLdpcInterleaver(enabled);
        });

        // Handshake confirmed (initiator only - responder switches in setConnected)
        protocol_.setHandshakeConfirmedCallback([this]() {
            setHandshakeComplete(true);
            LOG_MODEM(INFO, "[%s] Handshake confirmed", callsign_.c_str());
        });

        // Burst TX callback - encode multiple frames as single OFDM burst
        protocol_.setTransmitBurstCallback([this](const std::vector<Bytes>& frames) {
            auto samples = transmitBurst(frames);
            queueTx(samples, txBurstLabel(frames));
        });

        // PING/PONG
        protocol_.setPingTxCallback([this]() {
            auto samples = transmitPing();
            queueTx(samples);
        });

        protocol_.setPingReceivedCallback([this]() {
            auto samples = transmitPong();
            queueTx(samples);
        });

        // Message received
        protocol_.setMessageReceivedCallback([this](const std::string&, const std::string& text) {
            if (message_callback_) {
                message_callback_(text);
            }
        });

        // File received
        protocol_.setFileReceivedCallback([this](const std::string& filepath, bool success) {
            if (file_received_callback_) {
                file_received_callback_(filepath, success);
            }
        });
    }

    std::string txFrameLabel(const Bytes& data) const {
        std::ostringstream oss;
        if (data.size() >= 3) {
            auto type = static_cast<v2::FrameType>(data[2]);
            oss << "frame_type=" << v2::frameTypeToString(type);
            if (data.size() >= 6) {
                uint16_t seq = (static_cast<uint16_t>(data[4]) << 8) | data[5];
                oss << " seq=" << seq;
            }
        } else if (v2::PingFrame::isPing(data)) {
            oss << "frame_type=PING_OR_PONG";
        } else {
            oss << "frame_type=RAW";
        }
        return oss.str();
    }

    std::string txBurstLabel(const std::vector<Bytes>& frames) const {
        std::ostringstream oss;
        oss << "frame_type=BURST frame_count=" << frames.size();
        int data_frames = 0;
        if (!frames.empty() && frames.front().size() >= 3) {
            auto first = static_cast<v2::FrameType>(frames.front()[2]);
            oss << " first_frame_type=" << v2::frameTypeToString(first);
        }
        for (const auto& f : frames) {
            if (f.size() >= 3 && v2::isDataFrame(static_cast<v2::FrameType>(f[2]))) {
                ++data_frames;
            }
        }
        oss << " data_frames=" << data_frames;
        return oss.str();
    }

    void queueTx(const std::vector<float>& samples, const std::string& label = std::string()) {
        if (port_ && !port_->shouldPaceTxInStationLoop()) {
            port_->queueTx(samples);
            if (!label.empty()) {
                LOG_MODEM(INFO, "[%s] TX direct: %s samples=%zu",
                          callsign_.c_str(), label.c_str(), samples.size());
            }
            return;
        }

        uint64_t start_sample = 0;
        uint64_t end_sample = 0;
        size_t queued_before = 0;
        std::lock_guard<std::mutex> lock(tx_mutex_);
        queued_before = tx_queue_.size();
        start_sample = tx_sample_clock_ + queued_before;
        end_sample = start_sample + samples.size();
        for (float s : samples) {
            tx_queue_.push(s);
        }
        if (!label.empty()) {
            LOG_MODEM(INFO,
                      "[%s] TX queue: %s start_sample=%llu end_sample=%llu samples=%zu queued_before=%zu",
                      callsign_.c_str(), label.c_str(),
                      static_cast<unsigned long long>(start_sample),
                      static_cast<unsigned long long>(end_sample),
                      samples.size(), queued_before);
        }
    }

    // THE AUDIO LOOP - like a real sound card callback
    void audioLoop() {
        ultra::setLogStationTag(callsign_.c_str());
        auto next_callback = std::chrono::steady_clock::now();
        int callback_count = 0;

        while (running_) {
            // ===== AUDIO CALLBACK START =====

            // 1. READ RX - get samples from audio port (virtual channel or soundcard)
            std::vector<float> rx_samples = port_
                ? port_->pullRx(SAMPLES_PER_CALLBACK)
                : std::vector<float>(SAMPLES_PER_CALLBACK, 0.0f);

            // 2. FEED TO DECODER (audio thread only buffers - decode thread processes)
            if (decoder_) {
                if (rx_batch_callbacks_ <= 1) {
                    decoder_->feedAudio(rx_samples.data(), rx_samples.size());
                } else {
                    rx_batch_buffer_.insert(rx_batch_buffer_.end(),
                                            rx_samples.begin(), rx_samples.end());
                    rx_batch_counter_++;
                    if (rx_batch_counter_ >= rx_batch_callbacks_) {
                        decoder_->feedAudio(rx_batch_buffer_.data(), rx_batch_buffer_.size());
                        rx_batch_buffer_.clear();
                        rx_batch_counter_ = 0;
                    }
                }
            }

            // 3. GET TX SAMPLES - check if we have anything to transmit.
            // HardwareAudioPort bypasses this simulated pacer because SDL's
            // output callback is already the hardware clock.
            std::vector<float> tx_samples(SAMPLES_PER_CALLBACK, 0.0f);
            size_t tx_pending = 0;
            const bool pace_tx = !port_ || port_->shouldPaceTxInStationLoop();
            if (pace_tx) {
                {
                    std::lock_guard<std::mutex> lock(tx_mutex_);
                    tx_sample_clock_ += SAMPLES_PER_CALLBACK;
                    tx_pending = tx_queue_.size();
                    for (int i = 0; i < SAMPLES_PER_CALLBACK && !tx_queue_.empty(); i++) {
                        tx_samples[i] = tx_queue_.front();
                        tx_queue_.pop();
                    }
                }

                // 4. SEND TX TO AUDIO PORT (virtual channel only; hardware TX
                // is queued directly in queueTx() above)
                if (port_) port_->queueTx(tx_samples);
            }

            // ===== AUDIO CALLBACK END =====

            total_samples_ += SAMPLES_PER_CALLBACK;
            callback_count++;

            // Log continuous audio status every 2 seconds (200 callbacks at 10ms each)
            if (callback_count % 200 == 0) {
                float rx_rms = 0.0f;
                for (float s : rx_samples) rx_rms += s * s;
                rx_rms = std::sqrt(rx_rms / rx_samples.size());
                printf("[%s] Audio loop: %.1fs, RX_RMS=%.4f, TX_pending=%zu, pace=%dx, batch=%d\n",
                       callsign_.c_str(), getSimTime(), rx_rms, tx_pending,
                       rx_overfeed_factor_, rx_batch_callbacks_);
            }

            // Wait for next callback (real-time pacing)
            int callback_us = (CALLBACK_INTERVAL_MS * 1000) / std::max(1, rx_overfeed_factor_);
            callback_us = std::max(100, callback_us);  // allow aggressive stress pacing
            next_callback += std::chrono::microseconds(callback_us);
            std::this_thread::sleep_until(next_callback);
        }
    }

    // DECODE THREAD - like the real ModemEngine::rxDecodeLoop()
    // Runs independently from audio feed, just like a real sound card + decoder
    void decodeLoop() {
        ultra::setLogStationTag(callsign_.c_str());
        while (running_) {
            if (decoder_) {
                // processBuffer() blocks until data is available (via condition variable)
                // or until stop() is called
                decoder_->processBuffer();
                if (decode_delay_ms_ > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(decode_delay_ms_));
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
};
