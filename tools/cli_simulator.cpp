/**
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

#ifdef ULTRA_HAVE_SDL2
#include "gui/audio_engine.hpp"  // Real SDL2 audio I/O for --role A|B
#endif

using namespace ultra;
using namespace ultra::gui;
using namespace ultra::protocol;
using namespace ultra::sim;
namespace v2 = protocol::v2;

// Channel condition types (ITU-R F.1487)
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

#ifdef ULTRA_HAVE_SDL2
/**
     * ChannelInjector - Streaming TX-side channel emulator. Applies CFO +
     * Watterson fading + AWGN to each transmitted audio buffer before it
     * reaches the soundcard.
 *
 * Wraps SimulatedChannel and uses only its A→B direction:
 *   transmitFromA(tx)     -> processed samples land in buffer_b_rx_
 *   receiveForB(tx.size())-> drains them back as the channel-degraded TX
 *
 * Buffer never underflows because we push and drain in lockstep, so the
 * "noise on underflow" path in receiveForB doesn't fire.
 */
class ChannelInjector {
public:
    ChannelInjector(float snr_db, ChannelType ch_type,
                    uint32_t seed, float tx_cfo_hz,
                    float output_gain = 0.70f)
        : output_gain_(std::clamp(output_gain, 0.05f, 1.0f)) {
        sim_channel_.setSeed(seed);
        sim_channel_.setTxCFO(tx_cfo_hz);
        sim_channel_.configure(snr_db, ch_type);
    }

    std::vector<float> process(const std::vector<float>& tx) {
        sim_channel_.transmitFromA(tx);
        auto out = sim_channel_.receiveForB(tx.size());
        const float rms_before_gain = sampleRms(out);
        const float peak_before_gain = samplePeak(out);
        const size_t clipped_before_gain = countFullScaleSamples(out);
        if (output_gain_ != 1.0f) {
            for (float& sample : out) {
                sample *= output_gain_;
            }
        }
        process_count_++;
        if (process_count_ <= 8 || (process_count_ % 32) == 0) {
            LOG_MODEM(INFO,
                      "ChannelInjector: process #%llu samples=%zu "
                      "gain=%.3f rms_in=%.4f peak_in=%.4f "
                      "rms_ch=%.4f peak_ch=%.4f clip_ch=%zu "
                      "rms_out=%.4f peak_out=%.4f clip_out=%zu",
                      static_cast<unsigned long long>(process_count_),
                      tx.size(), output_gain_, sampleRms(tx), samplePeak(tx),
                      rms_before_gain, peak_before_gain, clipped_before_gain,
                      sampleRms(out), samplePeak(out), countFullScaleSamples(out));
        }
        return out;
    }

private:
    SimulatedChannel sim_channel_;
    float output_gain_ = 0.70f;
    uint64_t process_count_ = 0;
};

/**
 * HardwareAudioPort - Real soundcard I/O via SDL2 (gui::AudioEngine).
 * Used for --role A|B mode when running across two physical machines
 * connected by an audio cable (or speaker/mic).
 *
     * On RX short read, waits for the SDL capture period to finish before
     * padding. Padding immediately would synthesize callback-sized sample gaps.
 *
 * If a ChannelInjector is supplied, TX samples pass through it before
 * hitting the soundcard, so the receiving station sees a realistic
 * channel-degraded signal even on a clean audio cable.
 */
class HardwareAudioPort : public AudioPort {
public:
    HardwareAudioPort(const std::string& output_device,
                      const std::string& input_device,
                      std::unique_ptr<ChannelInjector> injector = nullptr,
                      int buffer_size = 0)
        : output_device_(output_device),
          input_device_(input_device),
          injector_(std::move(injector)),
          buffer_size_(buffer_size) {}

    bool start() override {
        if (!engine_.initialize()) {
            std::cerr << "AudioEngine init failed\n";
            return false;
        }
        if (buffer_size_ > 0) engine_.setBufferSize(buffer_size_);
        if (!engine_.openOutput(output_device_)) {
            std::cerr << "Failed to open output device '" << output_device_ << "'\n";
            return false;
        }
        if (!engine_.openInput(input_device_)) {
            std::cerr << "Failed to open input device '" << input_device_ << "'\n";
            return false;
        }
        engine_.startPlayback();
        engine_.startCapture();
        return true;
    }

    void stop() override {
        engine_.stopCapture();
        engine_.stopPlayback();
        engine_.closeInput();
        engine_.closeOutput();
        engine_.shutdown();
    }

    std::vector<float> pullRx(size_t count) override {
        const int sample_rate = std::max(1, engine_.getSampleRate());
        const int period_ms = std::max(1, (engine_.getBufferSize() * 1000 + sample_rate - 1) / sample_rate);
        const int wait_ms = std::clamp(period_ms * 2 + 20, 50, 1000);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);

        while (engine_.getRxBufferSize() < count &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        auto got = engine_.getRxSamples(count);
        if (got.size() < count) {
            rx_short_reads_++;
            rx_padded_samples_ += (count - got.size());
            if (rx_short_reads_ <= 4 || (rx_short_reads_ % 16) == 0) {
                LOG_MODEM(WARN,
                          "HardwareAudioPort: RX short read after %dms wait "
                          "(got=%zu need=%zu, padded_total=%llu, events=%llu)",
                          wait_ms, got.size(), count,
                          static_cast<unsigned long long>(rx_padded_samples_),
                          static_cast<unsigned long long>(rx_short_reads_));
            }
            // Only pad after waiting for real captured samples. Padding before
            // the SDL capture period completes creates artificial 400-500 sample
            // discontinuities when the hardware callback size is not 480.
            got.resize(count, 0.0f);
        }
        return got;
    }

    void queueTx(const std::vector<float>& samples) override {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        std::vector<float> queued_samples;
        if (injector_) {
            queued_samples = injector_->process(samples);
        } else {
            queued_samples = samples;
        }
        engine_.queueTxSamples(queued_samples);

        tx_queue_events_++;
        if (tx_queue_events_ <= 8 || (tx_queue_events_ % 32) == 0) {
            LOG_MODEM(INFO,
                      "HardwareAudioPort: TX queue #%llu inject=%s "
                      "in=%zu rms=%.4f peak=%.4f out=%zu rms=%.4f peak=%.4f "
                      "device='%s'",
                      static_cast<unsigned long long>(tx_queue_events_),
                      injector_ ? "yes" : "no",
                      samples.size(), sampleRms(samples), samplePeak(samples),
                      queued_samples.size(), sampleRms(queued_samples),
                      samplePeak(queued_samples), output_device_.c_str());
        }
    }

    bool shouldPaceTxInStationLoop() const override { return false; }

private:
    gui::AudioEngine engine_;
    std::string output_device_;
    std::string input_device_;
    std::unique_ptr<ChannelInjector> injector_;
    int buffer_size_ = 0;  // 0 = engine default
    std::mutex tx_mutex_;
    uint64_t rx_short_reads_ = 0;
    uint64_t rx_padded_samples_ = 0;
    uint64_t tx_queue_events_ = 0;
};
#endif  // ULTRA_HAVE_SDL2

/**
 * SimulatedStation - One station with single audio I/O thread
 *
 * Uses IWaveform for TX and StreamingDecoder for RX (NOT ModemEngine).
 * This ensures consistent configuration between TX and RX paths.
 */
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

    void setSNR(float snr) { snr_db_ = snr; }
    void setForcedModulation(Modulation mod) { protocol_.setForcedModulation(mod); }
    void setForcedCodeRate(CodeRate rate) { protocol_.setForcedCodeRate(rate); }
    void setFixedFrameCodewords(int cw_count) {
        fixed_frame_codewords_ = v2::sanitizeFixedFrameCodewords(cw_count);
        protocol_.setForcedFrameCodewords(fixed_frame_codewords_);
        if (encoder_) encoder_->setFixedFrameCodewords(fixed_frame_codewords_);
        if (decoder_) decoder_->setFixedFrameCodewords(fixed_frame_codewords_);
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

    // State
    std::atomic<bool> connected_{false};
    std::atomic<bool> handshake_complete_{false};
    float last_cfo_hz_ = 0.0f;  // CFO from chirp detection, used for light preamble

    std::function<void(const std::string&)> message_callback_;
    std::function<void(const std::string&, bool)> file_received_callback_;

    std::atomic<uint64_t> total_samples_{0};
    float snr_db_ = 20.0f;
    bool no_burst_interleave_ = false;  // Disable burst interleaving for A/B testing
    int burst_group_size_ = 8;
    int fixed_frame_codewords_ = v2::kDefaultFixedFrameCodewords;
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
            queueTx(samples);
        });

        // Connection state changes
        protocol_.setConnectionChangedCallback([this](ConnectionState state, const std::string&) {
            if (state == ConnectionState::CONNECTED) {
                setConnected(true);
            } else if (state == ConnectionState::DISCONNECTED) {
                setConnected(false);
            }
        });

        // Data mode changes (modulation + code rate)
        protocol_.setDataModeChangedCallback([this](Modulation mod, CodeRate rate,
                                                    float peer_snr_db, float peer_fading) {
            setDataMode(mod, rate);
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

        // Handshake confirmed (initiator only - responder switches in setConnected)
        protocol_.setHandshakeConfirmedCallback([this]() {
            setHandshakeComplete(true);
            LOG_MODEM(INFO, "[%s] Handshake confirmed", callsign_.c_str());
        });

        // Burst TX callback - encode multiple frames as single OFDM burst
        protocol_.setTransmitBurstCallback([this](const std::vector<Bytes>& frames) {
            auto samples = transmitBurst(frames);
            queueTx(samples);
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

    void queueTx(const std::vector<float>& samples) {
        if (port_ && !port_->shouldPaceTxInStationLoop()) {
            port_->queueTx(samples);
            return;
        }

        std::lock_guard<std::mutex> lock(tx_mutex_);
        for (float s : samples) {
            tx_queue_.push(s);
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

// =============================================================================
// MAIN SIMULATOR
// =============================================================================

class CLISimulator {
public:
    void setSNR(float snr) { snr_db_ = snr; }
    void setVerbose(bool v) { verbose_ = v; }
    void setFading(bool f) { use_fading_ = f; }
    void setChannelType(ChannelType t) { channel_type_ = t; use_fading_ = (t != ChannelType::AWGN); }
    void setForcedModulation(Modulation mod) { forced_mod_ = mod; }
    void setForcedCodeRate(CodeRate rate) { forced_rate_ = rate; }
    void setOFDMConfigPreset(OFDMConfigPreset preset) { ofdm_config_preset_ = preset; }
    void setFixedFrameCodewords(int cw_count) {
        fixed_frame_codewords_ = v2::sanitizeFixedFrameCodewords(cw_count);
    }
    void setPreferredWaveform(WaveformMode mode) { forced_waveform_ = mode; }
    void setTestFileTransfer(bool v) { test_file_transfer_ = v; }
    void setTestFileSize(size_t bytes) { test_file_size_ = bytes; }
    void setChannelInterleave(bool enable) { use_channel_interleave_ = enable; }
    void setNoBurstInterleave(bool v) { no_burst_interleave_ = v; }
    void setBurstInterleaveGroupSize(int n) {
        burst_group_size_ = ofdm_link_adaptation::sanitizeBurstGroupSize(n);
    }
    void setTestBurst(bool v) { test_burst_ = v; }
    void setSeed(uint32_t seed) { seed_ = seed; }
    void setTxCFO(float cfo_hz) { tx_cfo_hz_ = cfo_hz; }
    void setSaveSignals(bool enable, int message_limit = 0) {
        save_signals_ = enable;
        save_signals_message_limit_ = std::max(0, message_limit);
    }
    void setSaveSignalsPrefix(const std::string& prefix) { save_signals_prefix_ = prefix; }
    void setSaveSignalsMaxSamples(size_t max_samples) { save_signals_max_samples_ = max_samples; }
    void setAdaptiveTest(bool enable) { adaptive_test_ = enable; }
    void setAdaptiveHopSNR(float snr) { adaptive_hop_snr_db_ = snr; }
    void setAdaptiveHopChannel(ChannelType t) { adaptive_hop_channel_ = t; }
    void setRxOverfeedFactor(int factor) { rx_overfeed_factor_ = std::clamp(factor, 1, 200); }
    void setDecodeDelayMs(int ms) { decode_delay_ms_ = std::clamp(ms, 0, 500); }
    void setRxBatchCallbacks(int n) { rx_batch_callbacks_ = std::clamp(n, 1, 1000); }
    void setSoftCombiningHARQ(bool enable) { soft_combining_harq_ = enable; }

    // Hardware-audio mode (real soundcard I/O across two physical machines)
    void setRoleBoth() { role_ = Role::Both; }
    void setRoleA() { role_ = Role::A; }
    void setRoleB() { role_ = Role::B; }
    void setSelfCallsign(const std::string& c) { self_callsign_ = c; }
    void setPeerCallsign(const std::string& c) { peer_callsign_ = c; }
    void setAudioOutputDevice(const std::string& d) { audio_output_device_ = d; }
    void setAudioInputDevice(const std::string& d) { audio_input_device_ = d; }
    void setListAudioDevices(bool v) { list_audio_devices_ = v; }
    void setRoleBIdleSeconds(int s) { role_b_idle_seconds_ = std::max(0, s); }
    void setInjectChannel(bool v) { inject_channel_ = v; }
    void setInjectGain(float gain) { inject_gain_ = std::clamp(gain, 0.05f, 1.0f); }
    void setAudioBufferSize(int n) { audio_buffer_size_ = n; }

    bool runTest() {
        // Hardware-audio mode (real soundcard, single station per process).
        // Dispatched here so we don't spin up the in-process SimulatedChannel
        // or two-station orchestration.
        if (role_ != Role::Both) {
            return runHardwareTest();
        }

        printHeader();

        // Setup channel
        channel_.setSeed(seed_);
        channel_.setTxCFO(tx_cfo_hz_);
        channel_.configure(snr_db_, channel_type_);
        channel_.setSignalCaptureEnabled(false);
        if (save_signals_) {
            capture_limit_hit_.store(false);
            channel_.setSignalCaptureMaxSamples(save_signals_max_samples_);
            channel_.clearCapturedSignals();
            channel_.setSignalCaptureEnabled(true);
            std::cout << "  [capture] enabled";
            if (save_signals_message_limit_ > 0) {
                std::cout << ", will stop after " << save_signals_message_limit_
                          << " received app message(s)";
            }
            if (save_signals_max_samples_ > 0) {
                std::cout << ", max " << save_signals_max_samples_ << " samples/stream";
            }
            std::cout << "\n";
        }

        // Create stations with virtual audio ports (in-process channel sim)
        alpha_ = std::make_unique<SimulatedStation>(
            "ALPHA", std::make_unique<VirtualAudioPort>(channel_, /*is_station_a=*/true),
            ofdm_config_preset_);
        bravo_ = std::make_unique<SimulatedStation>(
            "BRAVO", std::make_unique<VirtualAudioPort>(channel_, /*is_station_a=*/false),
            ofdm_config_preset_);
        alpha_->setRxOverfeedFactor(rx_overfeed_factor_);
        bravo_->setRxOverfeedFactor(rx_overfeed_factor_);
        alpha_->setDecodeDelayMs(decode_delay_ms_);
        bravo_->setDecodeDelayMs(decode_delay_ms_);
        alpha_->setRxBatchCallbacks(rx_batch_callbacks_);
        bravo_->setRxBatchCallbacks(rx_batch_callbacks_);
        alpha_->setFixedFrameCodewords(fixed_frame_codewords_);
        bravo_->setFixedFrameCodewords(fixed_frame_codewords_);
        alpha_->setSoftCombiningHARQ(soft_combining_harq_);
        bravo_->setSoftCombiningHARQ(soft_combining_harq_);

        // Set channel SNR for mode negotiation
        alpha_->setSNR(snr_db_);
        bravo_->setSNR(snr_db_);

        // Set forced settings on INITIATOR only (alpha)
        // Responder (bravo) reads these from the CONNECT frame and honors them
        if (forced_mod_ != Modulation::AUTO) {
            alpha_->setForcedModulation(forced_mod_);
        }
        if (forced_rate_ != CodeRate::AUTO) {
            alpha_->setForcedCodeRate(forced_rate_);
        }
        if (forced_waveform_ != WaveformMode::AUTO) {
            alpha_->setPreferredWaveform(forced_waveform_);
        }

        // Apply channel interleaving setting to both stations
        alpha_->setChannelInterleave(use_channel_interleave_);
        bravo_->setChannelInterleave(use_channel_interleave_);
        if (!use_channel_interleave_) {
            std::cout << "  \033[33mChannel interleaving DISABLED\033[0m\n";
        }

        // Apply burst interleave setting to both stations
        alpha_->setNoBurstInterleave(no_burst_interleave_);
        bravo_->setNoBurstInterleave(no_burst_interleave_);
        alpha_->setBurstInterleaveGroupSize(burst_group_size_);
        bravo_->setBurstInterleaveGroupSize(burst_group_size_);
        if (no_burst_interleave_) {
            std::cout << "  \033[33mBurst interleaving DISABLED\033[0m\n";
        }
        if (burst_group_size_ != 8) {
            std::cout << "  \033[36mBurst interleave group size = " << burst_group_size_ << "\033[0m\n";
        }
        if (fixed_frame_codewords_ != v2::kDefaultFixedFrameCodewords) {
            std::cout << "  \033[36mFixed frame CW count = " << fixed_frame_codewords_ << "\033[0m\n";
        }
        if (soft_combining_harq_) {
            std::cout << "  \033[36mRX soft-combining HARQ ENABLED\033[0m\n";
        }

        // Setup message callback on BRAVO
        bravo_->setMessageCallback([this](const std::string& msg) {
            std::lock_guard<std::mutex> lock(msg_mutex_);
            received_message_ = msg;
            message_received_ = true;
            received_messages_.push_back(msg);
            int count = static_cast<int>(received_messages_.size());
            messages_received_count_.store(count);

            if (save_signals_ &&
                save_signals_message_limit_ > 0 &&
                count >= save_signals_message_limit_ &&
                !capture_limit_hit_.exchange(true)) {
                channel_.setSignalCaptureEnabled(false);
                LOG_MODEM(INFO, "[capture] reached message limit (%d), signal capture stopped",
                          save_signals_message_limit_);
            }
        });

        // Setup file received callback on BRAVO
        bravo_->setReceiveDirectory("/tmp");
        bravo_->setFileReceivedCallback([this](const std::string& path, bool success) {
            std::lock_guard<std::mutex> lock(msg_mutex_);
            received_file_path_ = path;
            file_transfer_success_ = success;
            file_received_ = true;
        });

        // Start audio threads
        alpha_->start();
        bravo_->start();

        // Run protocol test (message, file, or burst)
        bool success;
        if (adaptive_test_) {
            success = runAdaptiveTest();
        } else if (test_burst_) {
            success = runBurstTest();
        } else if (test_file_transfer_) {
            success = runFileTransferTest();
        } else {
            success = runProtocolTest();
        }

        // Stop
        alpha_->stop();
        bravo_->stop();
        channel_.setSignalCaptureEnabled(false);
        if (save_signals_) {
            saveCapturedSignals(success);
        }

        if (success) {
            printSummary();
        } else {
            std::cout << "\n";
            std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
            std::cout << "║                     TEST FAILED                              ║\n";
            std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
            printStationStats("ALPHA (TX)", alpha_.get());
            printStationStats("BRAVO (RX)", bravo_.get());
            printDecoderPhaseBreakdown();
            std::cout << "\n";
        }
        return success;
    }

private:
    float snr_db_ = 20.0f;
    bool verbose_ = false;
    bool use_fading_ = false;
    ChannelType channel_type_ = ChannelType::AWGN;
    bool test_file_transfer_ = false;
    bool test_burst_ = false;              // --burst-test mode: send large messages for burst interleaving
    bool use_channel_interleave_ = true;   // Enabled by default for OFDM fading resistance
    bool no_burst_interleave_ = false;     // --no-burst-interleave for A/B testing
    int burst_group_size_ = 8;             // --burst-group-size N (experimental)
    int rx_overfeed_factor_ = 1;           // --rx-overfeed-factor N (decoder overload stress)
    int decode_delay_ms_ = 0;              // --decode-delay-ms N (simulated slow decoder)
    int rx_batch_callbacks_ = 1;           // --rx-batch-callbacks N (batched decoder feed)
    int fixed_frame_codewords_ = v2::kDefaultFixedFrameCodewords;
    bool soft_combining_harq_ = false;
    bool save_signals_ = false;
    int save_signals_message_limit_ = 0;   // 0 = full run
    size_t save_signals_max_samples_ = 0;  // 0 = unlimited
    std::string save_signals_prefix_ = "/tmp/cli_signals";
    std::atomic<bool> capture_limit_hit_{false};
    bool adaptive_test_ = false;
    float adaptive_hop_snr_db_ = 12.0f;
    ChannelType adaptive_hop_channel_ = ChannelType::MODERATE;
    size_t test_file_size_ = 256;  // Default 256 bytes test file
    uint32_t seed_ = 42;
    float tx_cfo_hz_ = 0.0f;
    Modulation forced_mod_ = Modulation::AUTO;
    CodeRate forced_rate_ = CodeRate::AUTO;
    WaveformMode forced_waveform_ = WaveformMode::AUTO;
    OFDMConfigPreset ofdm_config_preset_ = OFDMConfigPreset::Default;

    // Hardware-audio role (--role A|B|both, default both = current sim behavior)
    enum class Role { Both, A, B };
    Role role_ = Role::Both;
    std::string self_callsign_;        // empty -> default per role
    std::string peer_callsign_;        // empty -> default per role (only used by role A)
    std::string audio_output_device_;  // empty -> SDL default device
    std::string audio_input_device_;   // empty -> SDL default device
    bool list_audio_devices_ = false;
    int role_b_idle_seconds_ = 0;      // 0 = run until peer disconnects (no idle cap)
    bool inject_channel_ = false;       // --inject-channel: apply TX-side channel sim
                                        // to real-audio output (uses snr_db_/channel_type_)
    float inject_gain_ = 0.70f;         // Post-injection headroom before DAC full scale
    int audio_buffer_size_ = 0;         // 0 = AudioEngine default (4096)

    SimulatedChannel channel_;
    std::unique_ptr<SimulatedStation> alpha_;
    std::unique_ptr<SimulatedStation> bravo_;

    std::mutex msg_mutex_;
    std::string received_message_;
    std::atomic<bool> message_received_{false};
    std::vector<std::string> received_messages_;  // For batch receive
    std::atomic<int> messages_received_count_{0};

    // File transfer state
    std::string received_file_path_;
    bool file_transfer_success_ = false;
    std::atomic<bool> file_received_{false};

    static bool writeF32File(const std::string& path, const std::vector<float>& data) {
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size() * sizeof(float)));
        return static_cast<bool>(out);
    }

    std::string capturePrefixForRun() const {
        std::ostringstream oss;
        oss << save_signals_prefix_ << "_seed" << seed_;
        return oss.str();
    }

    void saveCapturedSignals(bool test_success) {
        auto cap = channel_.getCapturedSignals();
        std::string prefix = capturePrefixForRun();

        std::string a_tx = prefix + "_a_tx_raw.f32";
        std::string b_tx = prefix + "_b_tx_raw.f32";
        std::string a_rx = prefix + "_a_rx_raw.f32";
        std::string b_rx = prefix + "_b_rx_raw.f32";
        std::string meta = prefix + "_meta.txt";

        bool ok = true;
        ok = writeF32File(a_tx, cap.a_tx_raw) && ok;
        ok = writeF32File(b_tx, cap.b_tx_raw) && ok;
        ok = writeF32File(a_rx, cap.a_rx_raw) && ok;
        ok = writeF32File(b_rx, cap.b_rx_raw) && ok;

        std::ofstream meta_out(meta);
        if (meta_out) {
            const char* mod_str =
                (forced_mod_ == Modulation::AUTO) ? "AUTO" : modulationToString(forced_mod_);
            const char* rate_str =
                (forced_rate_ == CodeRate::AUTO) ? "AUTO" : codeRateToString(forced_rate_);
            const char* wf_str =
                (forced_waveform_ == WaveformMode::AUTO) ? "AUTO" : waveformModeToString(forced_waveform_);

            meta_out << "sample_rate=48000\n";
            meta_out << "seed=" << seed_ << "\n";
            meta_out << "snr_db=" << snr_db_ << "\n";
            meta_out << "tx_cfo_hz=" << tx_cfo_hz_ << "\n";
            meta_out << "channel=" << channelTypeName() << "\n";
            meta_out << "forced_modulation=" << mod_str << "\n";
            meta_out << "forced_code_rate=" << rate_str << "\n";
            meta_out << "forced_waveform=" << wf_str << "\n";
            meta_out << "ofdm_config=" << ofdmConfigPresetToString(ofdm_config_preset_) << "\n";
            meta_out << "test_type="
                     << (test_file_transfer_ ? "file_transfer" : (test_burst_ ? "burst" : "messages"))
                     << "\n";
            meta_out << "test_success=" << (test_success ? "1" : "0") << "\n";
            meta_out << "capture_message_limit=" << save_signals_message_limit_ << "\n";
            meta_out << "capture_limit_hit=" << (capture_limit_hit_.load() ? "1" : "0") << "\n";
            meta_out << "capture_max_samples=" << cap.max_samples << "\n";
            meta_out << "capture_truncated=" << (cap.truncated ? "1" : "0") << "\n";
            meta_out << "a_tx_samples=" << cap.a_tx_raw.size() << "\n";
            meta_out << "b_tx_samples=" << cap.b_tx_raw.size() << "\n";
            meta_out << "a_rx_samples=" << cap.a_rx_raw.size() << "\n";
            meta_out << "b_rx_samples=" << cap.b_rx_raw.size() << "\n";
            meta_out << "files=" << a_tx << "," << b_tx << "," << a_rx << "," << b_rx << "\n";
            meta_out << "notes=Compare TX vs RX using training/LTS CFO estimator to validate residual CFO after correction.\n";
            meta_out.close();
        } else {
            ok = false;
        }

        if (ok) {
            std::cout << "\n  [capture] wrote:\n"
                      << "    " << a_tx << "\n"
                      << "    " << b_tx << "\n"
                      << "    " << a_rx << "\n"
                      << "    " << b_rx << "\n"
                      << "    " << meta << "\n";
        } else {
            std::cout << "\n  \033[33m[capture] warning: failed to write one or more capture files under prefix "
                      << prefix << "\033[0m\n";
        }
    }

    bool sendAndVerifyMessage(const std::string& msg, int timeout_seconds = 90) {
        if (!waitFor([this]{ return alpha_->isReadyToSend(); }, 30)) {
            std::cout << "  \033[31m✗ ARQ not ready!\033[0m\n";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(msg_mutex_);
            received_message_.clear();
        }
        message_received_.store(false);

        std::cout << "  TX (" << msg.size() << " bytes): " << msg << "\n";
        alpha_->sendMessage(msg);

        if (!waitFor([this]{ return message_received_.load(); }, timeout_seconds)) {
            std::cout << "  \033[31m✗ Message receive timeout!\033[0m\n";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(msg_mutex_);
            if (received_message_ != msg) {
                std::cout << "  \033[31m✗ Message mismatch!\033[0m\n";
                return false;
            }
        }

        std::cout << "  \033[32m✓ Message delivered\033[0m\n";
        return true;
    }

    bool runAdaptiveTest() {
        std::cout << "\n=== PHASE 1: CONNECTION ===\n";
        std::cout << "  ALPHA connecting to BRAVO...\n";
        alpha_->connect("BRAVO");

        if (!waitFor([this]{ return alpha_->isConnected() && bravo_->isConnected(); }, 30)) {
            std::cout << "  \033[31m✗ Connection timeout!\033[0m\n";
            return false;
        }
        std::cout << "  \033[32m✓ Both stations connected!\033[0m\n";

        std::cout << "\n=== PHASE 2: HANDSHAKE ===\n";
        if (!waitFor([this]{ return alpha_->isHandshakeComplete(); }, 30)) {
            std::cout << "  \033[31m✗ Handshake timeout!\033[0m\n";
            return false;
        }
        std::cout << "  \033[32m✓ Handshake complete!\033[0m\n";

        std::cout << "\n=== PHASE 3: ADAPTIVE SMOKE (2 conditions) ===\n";
        std::cout << "  Condition A: SNR=" << snr_db_ << " dB, channel=" << channelTypeName() << "\n";
        std::string msg_a = "[ADPT_TEST] Phase A baseline ";
        msg_a += std::string(900, 'A');  // Force fragmentation for richer advisory samples
        if (!sendAndVerifyMessage(msg_a)) {
            return false;
        }

        std::cout << "  Switching to Condition B: SNR=" << adaptive_hop_snr_db_
                  << " dB, channel=" << channelTypeToString(adaptive_hop_channel_) << "\n";
        channel_.configure(adaptive_hop_snr_db_, adaptive_hop_channel_);
        alpha_->setSNR(adaptive_hop_snr_db_);
        bravo_->setSNR(adaptive_hop_snr_db_);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        std::string msg_b = "[ADPT_TEST] Phase B changed condition ";
        msg_b += std::string(900, 'B');  // Force fragmentation under changed channel
        if (!sendAndVerifyMessage(msg_b)) {
            return false;
        }

        std::cout << "  \033[32m✓ Adaptive smoke sequence complete\033[0m\n";

        std::cout << "\n=== PHASE 4: DISCONNECT ===\n";
        alpha_->disconnect();
        if (!waitFor([this]{ return !alpha_->isConnected() && !bravo_->isConnected(); }, 15)) {
            std::cout << "  \033[33m! Disconnect timeout (non-fatal)\033[0m\n";
        } else {
            std::cout << "  \033[32m✓ Disconnected!\033[0m\n";
        }

        return true;
    }

    bool runProtocolTest() {
        // Phase 1: Connect
        std::cout << "\n=== PHASE 1: CONNECTION ===\n";
        std::cout << "  ALPHA connecting to BRAVO...\n";
        alpha_->connect("BRAVO");

        if (!waitFor([this]{ return alpha_->isConnected() && bravo_->isConnected(); }, 30)) {
            std::cout << "  \033[31m✗ Connection timeout!\033[0m\n";
            return false;
        }
        std::cout << "  \033[32m✓ Both stations connected!\033[0m\n";

        // Phase 2: Mode negotiation
        std::cout << "\n=== PHASE 2: MODE NEGOTIATION ===\n";
        if (!waitFor([this]{ return alpha_->isHandshakeComplete(); }, 30)) {
            std::cout << "  \033[31m✗ Mode negotiation timeout!\033[0m\n";
            return false;
        }
        std::cout << "  \033[32m✓ Handshake complete!\033[0m\n";

        // Phase 3: Send 5 short + 2 long messages as a burst
        std::cout << "\n=== PHASE 3: DATA TRANSFER (7 messages) ===\n";

        std::vector<std::string> test_messages;
        for (int i = 1; i <= 5; i++) {
            test_messages.push_back("Message " + std::to_string(i) + " from ALPHA");
        }
        // Long messages that exceed single-frame capacity (61 bytes at R1/4)
        test_messages.push_back(
            "This is a long test message that exceeds the 61-byte frame capacity "
            "and must be fragmented across multiple OFDM frames for delivery.");
        test_messages.push_back(
            "CQ CQ CQ de ALPHA. Testing long message fragmentation over HF radio. "
            "The quick brown fox jumps over the lazy dog. 73 de ALPHA.");

        int total = static_cast<int>(test_messages.size());

        if (!waitFor([this]{ return alpha_->isReadyToSend(); }, 30)) {
            std::cout << "  \033[31m✗ ARQ not ready!\033[0m\n";
            return false;
        }

        // Clear received state
        {
            std::lock_guard<std::mutex> lock(msg_mutex_);
            received_messages_.clear();
            messages_received_count_.store(0);
        }

        // Batch-send all messages (burst-interleaved)
        for (int i = 0; i < total; i++) {
            std::cout << "  [" << (i+1) << "/" << total << "] Queuing (" << test_messages[i].size() << "b): \"" << test_messages[i] << "\"\n";
        }
        ultra::timing::globalDecoderProfile().reset();
        alpha_->sendMessages(test_messages);
        std::cout << "  Sent " << total << " messages as burst\n";

        // Wait for all messages to arrive
        // Narrowband needs much longer: ~4.4s/frame RTT with window=1, plus retransmissions
        int burst_timeout = (forced_waveform_ == WaveformMode::OFDM_NARROW) ? 300 : 120;
        if (!waitFor([this, total]{ return messages_received_count_.load() >= total; }, burst_timeout)) {
            int got = messages_received_count_.load();
            std::cout << "  \033[31m✗ Only received " << got << "/" << total << " messages!\033[0m\n";
            return false;
        }

        // Verify all messages
        {
            std::lock_guard<std::mutex> lock(msg_mutex_);
            bool all_ok = true;
            for (int i = 0; i < total; i++) {
                const auto idx = static_cast<size_t>(i);
                if (idx < received_messages_.size() && received_messages_[idx] == test_messages[idx]) {
                    std::cout << "  \033[32m✓ [" << (i+1) << "/" << total << "] Received (" << received_messages_[idx].size() << "b): \"" << received_messages_[idx] << "\"\033[0m\n";
                } else {
                    std::string got = (idx < received_messages_.size()) ? received_messages_[idx] : "(missing)";
                    std::cout << "  \033[31m✗ Message " << (i+1) << " mismatch! Got: \"" << got << "\"\033[0m\n";
                    all_ok = false;
                }
            }
            if (!all_ok) return false;
        }

        std::cout << "  \033[32m✓ All " << total << " messages transferred successfully!\033[0m\n";

        // Phase 4: Disconnect (non-fatal if timeout - data transfer already proved)
        std::cout << "\n=== PHASE 4: DISCONNECT ===\n";
        alpha_->disconnect();

        if (!waitFor([this]{ return !alpha_->isConnected() && !bravo_->isConnected(); }, 15)) {
            std::cout << "  \033[33m! Disconnect timeout (non-fatal)\033[0m\n";
        } else {
            std::cout << "  \033[32m✓ Disconnected!\033[0m\n";
        }

        return true;  // Data transfer succeeded, disconnect is best-effort
    }

    bool runFileTransferTest() {
        // Create test file
        std::string test_file = "/tmp/cli_sim_test_file_" + std::to_string(::getpid()) + ".bin";
        {
            std::ofstream ofs(test_file, std::ios::binary);
            if (!ofs) {
                std::cout << "  \033[31m✗ Failed to create test file!\033[0m\n";
                return false;
            }
            // Write test pattern
            for (size_t i = 0; i < test_file_size_; i++) {
                ofs.put(static_cast<char>(i & 0xFF));
            }
        }
        std::cout << "  Created test file: " << test_file << " (" << test_file_size_ << " bytes)\n";

        // Phase 1: Connect
        std::cout << "\n=== PHASE 1: CONNECTION ===\n";
        std::cout << "  ALPHA connecting to BRAVO...\n";
        alpha_->connect("BRAVO");

        if (!waitFor([this]{ return alpha_->isConnected() && bravo_->isConnected(); }, 30)) {
            std::cout << "  \033[31m✗ Connection timeout!\033[0m\n";
            return false;
        }
        std::cout << "  \033[32m✓ Both stations connected!\033[0m\n";

        // Phase 2: Mode negotiation
        std::cout << "\n=== PHASE 2: MODE NEGOTIATION ===\n";
        if (!waitFor([this]{ return alpha_->isHandshakeComplete(); }, 30)) {
            std::cout << "  \033[31m✗ Mode negotiation timeout!\033[0m\n";
            return false;
        }
        std::cout << "  \033[32m✓ Handshake complete!\033[0m\n";

        // Phase 3: File transfer
        std::cout << "\n=== PHASE 3: FILE TRANSFER ===\n";

        if (!waitFor([this]{ return alpha_->isReadyToSend(); }, 10)) {
            std::cout << "  \033[31m✗ ARQ not ready!\033[0m\n";
            return false;
        }

        file_received_.store(false);
        ultra::timing::globalDecoderProfile().reset();
        std::cout << "  Sending file: " << test_file << " (" << test_file_size_ << " bytes)\n";

        if (!alpha_->sendFile(test_file)) {
            std::cout << "  \033[31m✗ Failed to start file transfer!\033[0m\n";
            return false;
        }

        // Wait for file transfer with progress updates
        auto start = std::chrono::steady_clock::now();
        int last_progress = -1;
        while (!file_received_.load()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();

            // Channel-aware file-transfer budget; see fileTransferTimeoutSeconds().
            const long timeout_s = fileTransferTimeoutSeconds(test_file_size_, false);
            if (elapsed >= timeout_s) {
                std::cout << "  \033[31m✗ File transfer timeout (budget=" << timeout_s
                          << "s, channel=" << channelTypeName() << ")!\033[0m\n";
                return false;
            }

            alpha_->tick();
            bravo_->tick();

            // Show progress
            auto progress = alpha_->getFileProgress();
            int pct = static_cast<int>(progress.percentage());
            if (pct != last_progress && pct % 10 == 0) {
                std::cout << "  Progress: " << pct << "% (" << progress.transferred_bytes << "/" << progress.total_bytes << " bytes)\n";
                last_progress = pct;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Measure transfer time (from sendFile to file_received)
        auto transfer_end = std::chrono::steady_clock::now();
        float transfer_sec = std::chrono::duration<float>(transfer_end - start).count();
        float throughput_bps = (transfer_sec > 0.01f)
            ? (test_file_size_ * 8.0f / transfer_sec) : 0.0f;

        // Verify received file
        {
            std::lock_guard<std::mutex> lock(msg_mutex_);
            if (!file_transfer_success_) {
                std::cout << "  \033[31m✗ File transfer reported failure!\033[0m\n";
                return false;
            }
            std::cout << "  \033[32m✓ File received: " << received_file_path_ << "\033[0m\n";
            std::cout << "  Transfer: " << test_file_size_ << " bytes in "
                      << std::fixed << std::setprecision(1) << transfer_sec << "s = "
                      << std::setprecision(0) << throughput_bps << " bps\n";

            // Verify contents
            std::ifstream ifs(received_file_path_, std::ios::binary);
            if (!ifs) {
                std::cout << "  \033[31m✗ Cannot open received file!\033[0m\n";
                return false;
            }

            bool content_ok = true;
            for (size_t i = 0; i < test_file_size_; i++) {
                char c;
                if (!ifs.get(c) || static_cast<uint8_t>(c) != (i & 0xFF)) {
                    content_ok = false;
                    break;
                }
            }

            if (content_ok) {
                std::cout << "  \033[32m✓ File contents verified!\033[0m\n";
            } else {
                std::cout << "  \033[31m✗ File contents corrupted!\033[0m\n";
                return false;
            }
        }

        // Phase 4: Disconnect (non-fatal if timeout - file transfer already proved)
        std::cout << "\n=== PHASE 4: DISCONNECT ===\n";
        alpha_->disconnect();

        if (!waitFor([this]{ return !alpha_->isConnected() && !bravo_->isConnected(); }, 15)) {
            std::cout << "  \033[33m! Disconnect timeout (non-fatal)\033[0m\n";
        } else {
            std::cout << "  \033[32m✓ Disconnected!\033[0m\n";
        }

        // Cleanup
        std::remove(test_file.c_str());
        std::remove(received_file_path_.c_str());

        return true;
    }

    bool runBurstTest() {
        // Phase 1: Connect
        std::cout << "\n=== PHASE 1: CONNECTION ===\n";
        std::cout << "  ALPHA connecting to BRAVO...\n";
        alpha_->connect("BRAVO");

        if (!waitFor([this]{ return alpha_->isConnected() && bravo_->isConnected(); }, 30)) {
            std::cout << "  \033[31m✗ Connection timeout!\033[0m\n";
            return false;
        }
        std::cout << "  \033[32m✓ Both stations connected!\033[0m\n";

        // Phase 2: Mode negotiation
        std::cout << "\n=== PHASE 2: MODE NEGOTIATION ===\n";
        if (!waitFor([this]{ return alpha_->isHandshakeComplete(); }, 30)) {
            std::cout << "  \033[31m✗ Mode negotiation timeout!\033[0m\n";
            return false;
        }
        std::cout << "  \033[32m✓ Handshake complete!\033[0m\n";

        // Phase 3: Send 3 large messages that fragment into 5+ frames each
        // At R1/2: payload capacity = 141 bytes, so 600 bytes → ceil(600/141) = 5 frames
        // At R1/4: payload capacity = 61 bytes, so 600 bytes → ceil(600/61) = 10 frames
        // With N-frame grouping: at least one burst-interleaved group per large message
        std::cout << "\n=== PHASE 3: BURST DATA TRANSFER (3 large messages) ===\n";
        std::cout << "  Burst interleaving: " << (no_burst_interleave_ ? "DISABLED" : "ENABLED") << "\n";
        std::cout << "  Burst group size: " << burst_group_size_ << "\n";

        std::vector<std::string> test_messages;
        // Generate 3 large messages (~600 bytes each)
        for (int i = 0; i < 3; i++) {
            std::string msg;
            msg.reserve(600);
            for (int j = 0; j < 60; j++) {
                char buf[16];
                snprintf(buf, sizeof(buf), "BLK%d_%02d ", i + 1, j);
                msg += buf;
            }
            // Trim to exactly 600 bytes
            msg.resize(600, 'X');
            test_messages.push_back(msg);
        }

        int total = static_cast<int>(test_messages.size());
        for (int msg_num = 0; msg_num < total; msg_num++) {
            const std::string& test_msg = test_messages[msg_num];

            if (!waitFor([this]{ return alpha_->isReadyToSend(); }, 60)) {
                std::cout << "  \033[31m✗ ARQ not ready for message " << (msg_num+1) << "!\033[0m\n";
                return false;
            }

            message_received_.store(false);

            std::cout << "  [" << (msg_num+1) << "/" << total << "] Sending (" << test_msg.size() << " bytes)...\n";
            alpha_->sendMessage(test_msg);

            if (!waitFor([this]{ return message_received_.load(); }, 120)) {
                std::cout << "  \033[31m✗ Message " << (msg_num+1) << " not received (timeout)!\033[0m\n";
                return false;
            }

            {
                std::lock_guard<std::mutex> lock(msg_mutex_);
                if (received_message_ == test_msg) {
                    std::cout << "  \033[32m✓ [" << (msg_num+1) << "/" << total << "] Received (" << received_message_.size() << " bytes) OK\033[0m\n";
                } else {
                    std::cout << "  \033[31m✗ Message " << (msg_num+1) << " corrupted!\033[0m\n";
                    return false;
                }
            }
        }

        // Print decoder stats
        auto stats = bravo_->getDecoderStats();
        std::cout << "\n=== RESULTS ===\n";
        std::cout << "  Frames decoded: " << stats.frames_decoded << "\n";
        std::cout << "  Frames failed:  " << stats.frames_failed << "\n";
        if (stats.frames_decoded + stats.frames_failed > 0) {
            float success_rate = 100.0f * stats.frames_decoded / (stats.frames_decoded + stats.frames_failed);
            std::cout << "  Success rate:   " << std::fixed << std::setprecision(1) << success_rate << "%\n";
        }
        std::cout << "  \033[32m✓ All " << total << " large messages transferred successfully!\033[0m\n";

        // Phase 4: Disconnect
        std::cout << "\n=== PHASE 4: DISCONNECT ===\n";
        alpha_->disconnect();
        if (!waitFor([this]{ return !alpha_->isConnected() && !bravo_->isConnected(); }, 15)) {
            std::cout << "  \033[33m! Disconnect timeout (non-fatal)\033[0m\n";
        } else {
            std::cout << "  \033[32m✓ Disconnected!\033[0m\n";
        }

        return true;
    }

    bool waitFor(std::function<bool()> condition, int timeout_seconds) {
        auto start = std::chrono::steady_clock::now();
        int last_print = -1;

        while (!condition()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();

            if (elapsed >= timeout_seconds) {
                return false;
            }

            // Tick protocols
            alpha_->tick();
            bravo_->tick();

            // Progress indicator
            if (elapsed != last_print && elapsed % 2 == 0) {
                std::cout << "  [" << alpha_->getSimTime() << "s sim]\n";
                last_print = elapsed;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

    const char* channelTypeName() const {
        switch (channel_type_) {
            case ChannelType::AWGN:     return "AWGN (no fading)";
            case ChannelType::GOOD:     return "Good (0.5ms, 0.1Hz)";
            case ChannelType::MODERATE: return "Moderate (1ms, 0.5Hz)";
            case ChannelType::POOR:     return "Poor (2ms, 1Hz)";
            case ChannelType::FLUTTER:  return "Flutter (0.5ms, 10Hz)";
            default:                    return "Unknown";
        }
    }

    // Channel-aware file-transfer timeout. Sizes for worst-case sustained
    // throughput at R1/4 OFDM (the slowest rate), then adds base-overhead
    // seconds for handshake + disconnect. Values are floors observed
    // empirically — measured numbers are typically 1.5-3x faster.
    //   AWGN:     ~60 B/s in-process sim, ~30 B/s on real-audio hardware
    //   Good:     ~20 B/s sim, ~12 B/s hardware
    //   Moderate: ~12 B/s sim, ~8 B/s hardware
    //   Poor/Flutter: ~8 B/s sim, ~6 B/s hardware
    // Hardware mode is slower: soundcard jitter, ACK turnaround latency,
    // and ~5-15% retx overhead from USB-1.1 audio devices. Hardware also
    // gets 90s base (vs 60s sim) for two-machine handshake setup time.
    long fileTransferTimeoutSeconds(size_t bytes, bool hardware_mode = false) const {
        long bps_floor;
        if (hardware_mode) {
            switch (channel_type_) {
                case ChannelType::AWGN:     bps_floor = 30; break;
                case ChannelType::GOOD:     bps_floor = 12; break;
                case ChannelType::MODERATE: bps_floor = 8;  break;
                case ChannelType::POOR:     // fall through
                case ChannelType::FLUTTER:  bps_floor = 6;  break;
                default:                    bps_floor = 30; break;
            }
        } else {
            switch (channel_type_) {
                case ChannelType::AWGN:     bps_floor = 60; break;
                case ChannelType::GOOD:     bps_floor = 20; break;
                case ChannelType::MODERATE: bps_floor = 12; break;
                case ChannelType::POOR:     // fall through
                case ChannelType::FLUTTER:  bps_floor = 8;  break;
                default:                    bps_floor = 60; break;
            }
        }
        const long base_overhead_s = hardware_mode ? 90 : 60;
        return base_overhead_s + static_cast<long>(bytes) / bps_floor;
    }

    // ======================================================================
    // Hardware-audio mode (--role A|B): single station per process, real
    // soundcard I/O, peer is on another machine connected by audio cable.
    // ======================================================================

    bool runHardwareTest() {
#ifndef ULTRA_HAVE_SDL2
        std::cerr << "Hardware audio (--role A|B) requires SDL2. "
                     "This build was compiled without SDL2 support.\n";
        return false;
#else
        // Pick callsigns based on role unless overridden on the CLI.
        const std::string self = !self_callsign_.empty()
            ? self_callsign_
            : (role_ == Role::A ? std::string("ALPHA") : std::string("BRAVO"));
        const std::string peer = !peer_callsign_.empty()
            ? peer_callsign_
            : (role_ == Role::A ? std::string("BRAVO") : std::string("ALPHA"));

        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║   CLI Simulator - HARDWARE AUDIO MODE                        ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
        std::cout << "  Role:     " << (role_ == Role::A ? "A (initiator)" : "B (responder)") << "\n";
        std::cout << "  Self:     " << self << "\n";
        if (role_ == Role::A) std::cout << "  Peer:     " << peer << "\n";
        std::cout << "  Output:   " << (audio_output_device_.empty() ? "(default)" : audio_output_device_) << "\n";
        std::cout << "  Input:    " << (audio_input_device_.empty() ? "(default)" : audio_input_device_) << "\n";
        std::cout << "  OFDM cfg: " << ofdmConfigPresetToString(ofdm_config_preset_) << "\n";

        if (list_audio_devices_) {
            gui::AudioEngine probe;
            if (!probe.initialize()) {
                std::cerr << "Failed to init SDL audio for device listing\n";
                return false;
            }
            std::cout << "\n  Output devices:\n";
            for (const auto& d : probe.getOutputDevices()) std::cout << "    " << d << "\n";
            std::cout << "\n  Input devices:\n";
            for (const auto& d : probe.getInputDevices()) std::cout << "    " << d << "\n";
            probe.shutdown();
            return true;
        }

        // Optional TX-side channel injection (CFO + Watterson + AWGN
        // applied to outgoing audio before the soundcard). Each side runs
        // its own injector so both directions get realistic channel.
        std::unique_ptr<ChannelInjector> injector;
        if (inject_channel_) {
            const uint32_t injector_seed = (role_ == Role::A)
                ? seed_                  // ALPHA's TX uses base seed
                : seed_ + 0x9E3779B9u;   // BRAVO's TX uses a decorrelated seed
            injector = std::make_unique<ChannelInjector>(
                snr_db_, channel_type_, injector_seed, tx_cfo_hz_, inject_gain_);
            std::cout << "  Inject:   " << channelTypeName()
                      << " @ " << snr_db_ << " dB SNR (TX-side), gain="
                      << inject_gain_ << "\n";
        }

        // Build the single station with hardware I/O
        auto port = std::make_unique<HardwareAudioPort>(
            audio_output_device_, audio_input_device_, std::move(injector),
            audio_buffer_size_);
        auto station = std::make_unique<SimulatedStation>(
            self, std::move(port), ofdm_config_preset_);

        // Forced settings (only meaningful on initiator A — responder B picks
        // them up from the CONNECT frame):
        if (role_ == Role::A) {
            if (forced_mod_ != Modulation::AUTO) station->setForcedModulation(forced_mod_);
            if (forced_rate_ != CodeRate::AUTO) station->setForcedCodeRate(forced_rate_);
            if (forced_waveform_ != WaveformMode::AUTO) station->setPreferredWaveform(forced_waveform_);
        }

        // Pretend SNR for adaptive-mode logic. With real audio this is just
        // a hint to the link-adaptation layer; the real channel is whatever
        // the soundcard cable + --inject-channel produces.
        station->setSNR(snr_db_);
        station->setChannelInterleave(use_channel_interleave_);
        station->setNoBurstInterleave(no_burst_interleave_);
        station->setBurstInterleaveGroupSize(burst_group_size_);
        station->setFixedFrameCodewords(fixed_frame_codewords_);
        station->setSoftCombiningHARQ(soft_combining_harq_);
        if (soft_combining_harq_) {
            std::cout << "  HARQ:     RX soft-combining enabled\n";
        }

        // Role-B receive callbacks
        std::atomic<bool> peer_connected{false};
        std::atomic<bool> peer_disconnected{false};
        std::atomic<int> rx_message_count{0};
        std::atomic<bool> rx_file_done{false};

        if (role_ == Role::B) {
            station->setReceiveDirectory("/tmp");
            station->setMessageCallback([&](const std::string& msg) {
                int n = rx_message_count.fetch_add(1) + 1;
                std::cout << "  [RX MSG #" << n << " (" << msg.size() << "b)]: " << msg << "\n";
            });
            station->setFileReceivedCallback([&](const std::string& path, bool ok) {
                std::cout << "  [RX FILE] " << path
                          << (ok ? "  ✓" : "  ✗") << "\n";
                rx_file_done.store(true);
            });
        }

        std::cout << "\n  Starting station...\n";
        station->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let audio open

        bool ok = false;
        if (role_ == Role::A) {
            ok = runRoleA_initiator(*station, peer);
        } else {
            ok = runRoleB_responder(*station, peer_connected, peer_disconnected,
                                    rx_message_count, rx_file_done);
        }

        std::cout << "\n  Stopping station...\n";
        station->stop();

        // Print stats from the local station (peer's stats live in its own log)
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << (ok ? "║                     TEST PASSED                              ║\n"
                         : "║                     TEST FAILED                              ║\n");
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
        printStationStats(self.c_str(), station.get());
        printDecoderPhaseBreakdown();

        return ok;
#endif
    }

#ifdef ULTRA_HAVE_SDL2
    // Block while ticking only one station (no peer in this process).
    bool waitForRole(SimulatedStation& s,
                     std::function<bool()> cond,
                     int timeout_seconds,
                     const char* label) {
        auto start = std::chrono::steady_clock::now();
        int last_print = -1;
        while (!cond()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeout_seconds) return false;
            s.tick();
            if (elapsed != last_print && elapsed % 5 == 0) {
                std::cout << "  [" << s.getSimTime() << "s] " << label << " ...\n";
                last_print = static_cast<int>(elapsed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    }

    bool runRoleA_initiator(SimulatedStation& station, const std::string& peer) {
        // 1. Connect
        std::cout << "\n=== PHASE 1: CONNECT ===\n";
        std::cout << "  Connecting to " << peer << "...\n";
        station.connect(peer);
        if (!waitForRole(station, [&]{ return station.isConnected(); }, 60, "waiting for connect")) {
            std::cout << "  \033[31m✗ Connect timeout (60s)\033[0m\n";
            return false;
        }
        std::cout << "  \033[32m✓ Connected\033[0m\n";

        // 2. Mode negotiation / handshake
        std::cout << "\n=== PHASE 2: MODE NEGOTIATION ===\n";
        if (!waitForRole(station, [&]{ return station.isHandshakeComplete(); }, 30, "handshake")) {
            std::cout << "  \033[31m✗ Handshake timeout\033[0m\n";
            return false;
        }
        std::cout << "  \033[32m✓ Handshake complete\033[0m\n";

        if (!waitForRole(station, [&]{ return station.isReadyToSend(); }, 10, "ARQ ready")) {
            std::cout << "  \033[31m✗ ARQ not ready\033[0m\n";
            return false;
        }

        // 3. Send file or message
        bool data_ok = false;
        if (test_file_transfer_) {
            std::atomic<bool> file_sent_done{false};
            std::atomic<bool> file_sent_success{false};
            std::mutex file_sent_mutex;
            std::string file_sent_error;
            station.setFileSentCallback([&](bool success, const std::string& error) {
                {
                    std::lock_guard<std::mutex> lock(file_sent_mutex);
                    file_sent_error = error;
                }
                file_sent_success.store(success);
                file_sent_done.store(true);
            });

            std::string test_file = "/tmp/cli_sim_role_a_" + std::to_string(::getpid()) + ".bin";
            {
                std::ofstream ofs(test_file, std::ios::binary);
                if (!ofs) { std::cout << "  ✗ create file failed\n"; return false; }
                for (size_t i = 0; i < test_file_size_; i++) ofs.put(static_cast<char>(i & 0xFF));
            }
            std::cout << "\n=== PHASE 3: FILE TRANSFER ===\n";
            std::cout << "  Sending " << test_file << " (" << test_file_size_ << " bytes)\n";
            ultra::timing::globalDecoderProfile().reset();
            if (!station.sendFile(test_file)) {
                std::cout << "  ✗ sendFile failed\n";
                return false;
            }
            // Hardware-mode budget: channel-aware floor + 90s base for
            // soundcard jitter. See fileTransferTimeoutSeconds().
            const long timeout_s = fileTransferTimeoutSeconds(test_file_size_, true);
            std::cout << "  Budget: " << timeout_s << "s (channel=" << channelTypeName() << ")\n";
            data_ok = waitForRole(station,
                [&]{ return file_sent_done.load() || !station.isFileTransferInProgress(); },
                static_cast<int>(timeout_s), "file transfer");
            if (!data_ok) {
                std::cout << "  \033[31m✗ File transfer timeout (budget=" << timeout_s
                          << "s, channel=" << channelTypeName() << ")\033[0m\n";
            } else if (!file_sent_done.load()) {
                std::cout << "  \033[31m✗ File transfer ended without completion callback\033[0m\n";
                data_ok = false;
            } else if (!file_sent_success.load()) {
                std::lock_guard<std::mutex> lock(file_sent_mutex);
                std::cout << "  \033[31m✗ File transfer failed";
                if (!file_sent_error.empty()) {
                    std::cout << ": " << file_sent_error;
                }
                std::cout << "\033[0m\n";
                data_ok = false;
            } else {
                std::cout << "  Transferred " << test_file_size_ << "/" << test_file_size_
                          << " bytes (100%)\n";
                std::cout << "  \033[32m✓ File transfer complete\033[0m\n";
                data_ok = true;
            }
            station.setFileSentCallback({});
        } else {
            std::cout << "\n=== PHASE 3: MESSAGE TEST ===\n";
            const std::string msg = "Hello from station A (hw test, pid=" +
                                    std::to_string(::getpid()) + ")";
            station.sendMessage(msg);
            // Wait for ARQ idle (no pending TX) as a proxy for ack received.
            data_ok = waitForRole(station, [&]{ return station.isReadyToSend(); },
                                  90, "message ack");
            std::cout << (data_ok ? "  \033[32m✓ Message sent\033[0m\n"
                                  : "  \033[31m✗ Message timeout\033[0m\n");
        }

        // 4. Disconnect (best-effort)
        std::cout << "\n=== PHASE 4: DISCONNECT ===\n";
        station.disconnect();
        if (!waitForRole(station, [&]{ return !station.isConnected(); }, 15, "disconnect")) {
            std::cout << "  \033[33m! Disconnect timeout (non-fatal)\033[0m\n";
        } else {
            std::cout << "  \033[32m✓ Disconnected\033[0m\n";
        }
        return data_ok;
    }

    bool runRoleB_responder(SimulatedStation& station,
                            std::atomic<bool>& peer_connected,
                            std::atomic<bool>& peer_disconnected,
                            std::atomic<int>& rx_message_count,
                            std::atomic<bool>& rx_file_done) {
        std::cout << "\n  Listening for incoming connections...\n";
        std::cout << "  (Ctrl-C or peer disconnect to exit";
        if (role_b_idle_seconds_ > 0) std::cout << ", idle cap=" << role_b_idle_seconds_ << "s";
        std::cout << ")\n";

        auto start = std::chrono::steady_clock::now();
        bool was_connected = false;
        int last_print = -1;
        ultra::timing::globalDecoderProfile().reset();

        while (true) {
            station.tick();

            const bool connected = station.isConnected();
            if (connected && !was_connected) {
                std::cout << "  \033[32m✓ Peer connected\033[0m\n";
                peer_connected.store(true);
                was_connected = true;
            } else if (!connected && was_connected) {
                std::cout << "  Peer disconnected — exiting\n";
                peer_disconnected.store(true);
                return true;
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();

            if (role_b_idle_seconds_ > 0 && elapsed >= role_b_idle_seconds_) {
                std::cout << "  Idle cap reached (" << role_b_idle_seconds_
                          << "s), exiting\n";
                return rx_message_count.load() > 0 || rx_file_done.load();
            }

            if (elapsed != last_print && elapsed % 5 == 0) {
                std::cout << "  [" << station.getSimTime() << "s] connected="
                          << (connected ? "yes" : "no")
                          << "  rx_msgs=" << rx_message_count.load()
                          << "  rx_file=" << (rx_file_done.load() ? "yes" : "no")
                          << "\n";
                last_print = static_cast<int>(elapsed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
#endif  // ULTRA_HAVE_SDL2

    void printHeader() {
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║   CLI Simulator - IWaveform + StreamingDecoder               ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
        std::cout << "\n";
        std::cout << "  SNR:     " << snr_db_ << " dB\n";
        std::cout << "  TX CFO:  " << tx_cfo_hz_ << " Hz\n";
        std::cout << "  Channel: " << channelTypeName() << "\n";
        std::cout << "  OFDM cfg: " << ofdmConfigPresetToString(ofdm_config_preset_) << "\n";
        if (adaptive_test_) {
            std::cout << "  ADPT:    enabled (hop -> "
                      << channelTypeToString(adaptive_hop_channel_)
                      << " @ " << adaptive_hop_snr_db_ << " dB)\n";
        }
        std::cout << "  Model:   Real-time (48kHz, 10ms callbacks)\n";
        if (rx_overfeed_factor_ > 1) {
            std::cout << "  Stress:  RX overfeed x" << rx_overfeed_factor_ << "\n";
        }
        if (decode_delay_ms_ > 0) {
            std::cout << "  Stress:  Decode delay " << decode_delay_ms_ << " ms\n";
        }
        if (rx_batch_callbacks_ > 1) {
            std::cout << "  Stress:  RX batch " << rx_batch_callbacks_ << " callbacks/feed\n";
        }
        if (fixed_frame_codewords_ != v2::kDefaultFixedFrameCodewords) {
            std::cout << "  CW/frame: " << fixed_frame_codewords_ << "\n";
        }
        std::cout << "\n";
    }

    void printSummary() {
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                     TEST PASSED                              ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

        // Print detailed stats from both stations
        printStationStats("ALPHA (TX)", alpha_.get());
        printStationStats("BRAVO (RX)", bravo_.get());
        printDecoderPhaseBreakdown();

        std::cout << "\n";
    }

    void printDecoderPhaseBreakdown() {
        auto& dp = ultra::timing::globalDecoderProfile();
        auto fmt = [](const ultra::timing::PhaseStats& s) -> std::string {
            const uint64_t cnt = s.count.load();
            const uint64_t tot = s.total_us.load();
            const uint64_t mx  = s.max_us.load();
            if (cnt == 0) return "(0 calls)";
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "n=%llu  total=%.1fms  mean=%.1fus  max=%lluus",
                     static_cast<unsigned long long>(cnt),
                     tot / 1000.0,
                     static_cast<double>(tot) / static_cast<double>(cnt),
                     static_cast<unsigned long long>(mx));
            return std::string(buf);
        };

        std::cout << "\n  --- Decoder phase breakdown (decode thread, this transfer) ---\n";
        std::cout << "  detect_data_sync          " << fmt(dp.detect_data_sync) << "\n";
        std::cout << "  ofdm_process_total        " << fmt(dp.ofdm_process_total) << "\n";
        std::cout << "  lts_channel_estimate      " << fmt(dp.lts_channel_estimate) << "\n";
        std::cout << "  data_symbol_loop          " << fmt(dp.data_symbol_loop)
                  << "  (full per-symbol loop incl. erase/updateQuality)\n";
        std::cout << "  decode_fixed_frame_total  " << fmt(dp.decode_fixed_frame_total) << "\n";
        std::cout << "  ldpc_cw_total             " << fmt(dp.ldpc_cw_total)
                  << "  (subset of decode_fixed_frame_total)\n";
        std::cout << "  single_cw_decode_total    " << fmt(dp.single_cw_decode_total) << "\n";
        std::cout << "  control_first_1cw         " << fmt(dp.control_first_1cw)
                  << "  (subset of single_cw_decode_total - ACK decode path)\n";
        std::cout << "  cw0_peek_1cw              " << fmt(dp.cw0_peek_1cw)
                  << "  (subset of single_cw_decode_total)\n";
        std::cout << "  ofdm_cw0_probe_decode     " << fmt(dp.ofdm_cw0_probe_decode)
                  << "  (codec_->decode probes in decodeFrame)\n";
        std::cout << "  failed_4cw_after_peek     " << fmt(dp.failed_4cw_after_peek)
                  << "  (subset of decode_fixed_frame_total - incl. real-channel failures)\n";
        std::cout << "  low_llr_escalation_skipped count="
                  << dp.low_llr_escalation_skipped.load()
                  << "  (counter only)\n";
        std::cout << "  raw_cw0_probe_skipped     count="
                  << dp.raw_cw0_probe_skipped.load()
                  << "  (gated when known-4-CW data frame)\n";
        std::cout << "  low_llr_1cw_skipped       control_first="
                  << dp.low_llr_1cw_skipped_control_first.load()
                  << "  cw0_peek="
                  << dp.low_llr_1cw_skipped_cw0_peek.load()
                  << "  (LLR pre-screen avoided the ~85ms decode-and-fail)\n";

        auto fmt_hist = [](const ultra::timing::SingleCWHistogram& h) -> std::string {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "first=%llu  retry1=%llu  retry2=%llu  retry3=%llu  retry4=%llu  exhausted=%llu",
                     static_cast<unsigned long long>(h.first_try.load()),
                     static_cast<unsigned long long>(h.retry[0].load()),
                     static_cast<unsigned long long>(h.retry[1].load()),
                     static_cast<unsigned long long>(h.retry[2].load()),
                     static_cast<unsigned long long>(h.retry[3].load()),
                     static_cast<unsigned long long>(h.exhausted.load()));
            return std::string(buf);
        };
        std::cout << "  robustDecodeSingleCW retry histogram (per call site):\n";
        std::cout << "    control_first  " << fmt_hist(dp.robust_cw_control_first) << "\n";
        std::cout << "    cw0_peek       " << fmt_hist(dp.robust_cw_cw0_peek) << "\n";
        std::cout << "    default        " << fmt_hist(dp.robust_cw_default) << "\n";

        // |LLR|_avg distribution split by decode outcome — for picking the
        // pre-screen threshold from data. Bins of 0.5 width; last bin is 6.0+.
        auto print_llr_dist = [](const char* label,
                                 const ultra::timing::LLRHistogram& h) {
            using LH = ultra::timing::LLRHistogram;
            uint64_t s_total = 0, f_total = 0;
            for (size_t i = 0; i < LH::kBins; ++i) {
                s_total += h.success[i].load();
                f_total += h.fail[i].load();
            }
            std::cout << "    " << label << "  (n=" << (s_total + f_total)
                      << ", success=" << s_total << ", fail=" << f_total << ")\n";
            if (s_total + f_total == 0) return;
            for (size_t i = 0; i < LH::kBins; ++i) {
                const uint64_t s = h.success[i].load();
                const uint64_t f = h.fail[i].load();
                if (s + f == 0) continue;
                char range[32];
                if (i + 1 == LH::kBins) {
                    snprintf(range, sizeof(range), "[%.1f,inf)",
                             i * LH::kBinWidth);
                } else {
                    snprintf(range, sizeof(range), "[%.1f,%.1f)",
                             i * LH::kBinWidth,
                             (i + 1) * LH::kBinWidth);
                }
                const double p_succ = static_cast<double>(s)
                                    / static_cast<double>(s + f);
                char buf[160];
                snprintf(buf, sizeof(buf),
                         "      %-12s success=%-5llu fail=%-5llu  P(ok)=%.2f",
                         range,
                         static_cast<unsigned long long>(s),
                         static_cast<unsigned long long>(f),
                         p_succ);
                std::cout << buf << "\n";
            }
        };
        std::cout << "  |LLR|_avg distribution (1-CW pre-screen tuning):\n";
        print_llr_dist("control_first", dp.llr_dist_control_first);
        print_llr_dist("cw0_peek     ", dp.llr_dist_cw0_peek);
    }

    void printStationStats(const char* label, SimulatedStation* station) {
        if (!station) return;

        auto cs = station->getConnectionStats();
        auto ds = station->getDecoderStats();

        std::cout << "\n  --- " << label << " ---\n";

        // ARQ stats
        std::cout << "  ARQ:  frames_sent=" << cs.arq.frames_sent
                  << "  frames_rcvd=" << cs.arq.frames_received
                  << "  retransmissions=" << cs.arq.retransmissions
                  << "  timeouts=" << cs.arq.timeouts
                  << "  failed=" << cs.arq.failed << "\n";
        std::cout << "  RETX: timeout=" << cs.arq.retransmissions_timeout
                  << "  fast_hole=" << cs.arq.retransmissions_fast_hole
                  << "  hole_probe=" << cs.arq.retransmissions_hole_probe
                  << "  nack=" << cs.arq.retransmissions_nack
                  << "  hole_events=" << cs.arq.hole_events << "\n";
        std::cout << "  ACK:  acks_sent=" << cs.arq.acks_sent
                  << "  acks_rcvd=" << cs.arq.acks_received
                  << "  sacks_sent=" << cs.arq.sacks_sent
                  << "  sacks_rcvd=" << cs.arq.sacks_received << "\n";
        std::cout << "  ACKf: stale_ignored=" << cs.arq.stale_acks_ignored
                  << "  future_ignored=" << cs.arq.future_acks_ignored
                  << "  dup_ignored=" << cs.arq.duplicate_acks_ignored
                  << "  repeat_coalesced=" << cs.arq.ack_repeat_jobs_coalesced
                  << "  repeat_dropped=" << cs.arq.ack_repeat_jobs_dropped << "\n";

        // Effective ACK rate: how many ACK frames BRAVO sent per data frame
        // received. Baseline reference today on 50 KB OFDM ~1.1.
        if (cs.arq.frames_received > 0) {
            float ack_ratio = static_cast<float>(cs.arq.acks_sent) /
                              static_cast<float>(cs.arq.frames_received);
            std::cout << "  AckR: acks_sent/frames_received=" << std::fixed
                      << std::setprecision(2) << ack_ratio
                      << std::defaultfloat << "\n";

            // SACK trigger-reason breakdown (Phase 2). Each SACK send bumps
            // exactly one counter, so these four sum to sacks_sent.
            std::cout << "  SACKw: threshold=" << cs.arq.sack_trigger_threshold
                      << "  out_of_order=" << cs.arq.sack_trigger_out_of_order
                      << "  timer=" << cs.arq.sack_trigger_timer
                      << "  out_of_window=" << cs.arq.sack_trigger_out_of_window
                      << "\n";
        }

        // Decoder stats
        std::cout << "  RX:   frames_decoded=" << ds.frames_decoded
                  << "  frames_failed=" << ds.frames_failed
                  << "  pings=" << ds.pings_received
                  << "  overflows=" << ds.buffer_overflows
                  << "  drop_samples=" << ds.overflow_samples_dropped
                  << "  resets=" << ds.overflow_state_resets
                  << "  unsearched=" << ds.current_unsearched_samples
                  << "  backlog_ms=" << std::fixed << std::setprecision(1) << ds.backlog_ms
                  << "  peak_backlog_ms=" << ds.peak_backlog_ms
                  << std::defaultfloat << "\n";
        std::cout << "  SyncR: attempts=" << ds.sync_recovery_attempts
                  << "  success=" << ds.sync_recovery_successes
                  << "  d(+8/-8/+16/-16/+24/-24/+32/-32)="
                  << ds.sync_recovery_delta_p8 << "/"
                  << ds.sync_recovery_delta_m8 << "/"
                  << ds.sync_recovery_delta_p16 << "/"
                  << ds.sync_recovery_delta_m16 << "/"
                  << ds.sync_recovery_delta_p24 << "/"
                  << ds.sync_recovery_delta_m24 << "/"
                  << ds.sync_recovery_delta_p32 << "/"
                  << ds.sync_recovery_delta_m32 << "\n";

        // CW success rate (from log grep is imprecise, this is the real number)
        uint64_t total_frames = ds.frames_decoded + ds.frames_failed;
        if (total_frames > 0) {
            float success_pct = 100.0f * ds.frames_decoded / total_frames;
            std::cout << "  Rate: frame_success=" << std::fixed << std::setprecision(1)
                      << success_pct << "%" << std::defaultfloat << "\n";
        }
    }
};

int main(int argc, char* argv[]) {
    try {
        CLISimulator sim;

        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if ((arg == "--snr" || arg == "-s") && i + 1 < argc) {
                sim.setSNR(std::stof(argv[++i]));
            } else if (arg == "--verbose" || arg == "-v") {
                sim.setVerbose(true);
            } else if (arg == "--fading" || arg == "-f") {
                // --fading alone = moderate, --fading <type> = specified type
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    std::string ftype = argv[++i];
                    if (ftype == "good" || ftype == "GOOD") {
                        sim.setChannelType(ChannelType::GOOD);
                    } else if (ftype == "moderate" || ftype == "MODERATE") {
                        sim.setChannelType(ChannelType::MODERATE);
                    } else if (ftype == "poor" || ftype == "POOR") {
                        sim.setChannelType(ChannelType::POOR);
                    } else if (ftype == "flutter" || ftype == "FLUTTER") {
                        sim.setChannelType(ChannelType::FLUTTER);
                    } else {
                        std::cerr << "Unknown fading type: " << ftype << " (use good, moderate, poor, flutter)\n";
                        return 1;
                    }
                } else {
                    sim.setChannelType(ChannelType::MODERATE);  // Default fading = moderate
                }
            } else if (arg == "--channel" || arg == "-c") {
                if (i + 1 < argc) {
                    std::string ch_str = argv[++i];
                    if (ch_str == "awgn" || ch_str == "AWGN") {
                        sim.setChannelType(ChannelType::AWGN);
                    } else if (ch_str == "good" || ch_str == "GOOD") {
                        sim.setChannelType(ChannelType::GOOD);
                    } else if (ch_str == "moderate" || ch_str == "MODERATE") {
                        sim.setChannelType(ChannelType::MODERATE);
                    } else if (ch_str == "poor" || ch_str == "POOR") {
                        sim.setChannelType(ChannelType::POOR);
                    } else if (ch_str == "flutter" || ch_str == "FLUTTER") {
                        sim.setChannelType(ChannelType::FLUTTER);
                    } else {
                        std::cerr << "Unknown channel: " << ch_str << " (use awgn, good, moderate, poor, flutter)\n";
                        return 1;
                    }
                }
            } else if (arg == "--hop-channel") {
                if (i + 1 < argc) {
                    std::string ch_str = argv[++i];
                    if (ch_str == "awgn" || ch_str == "AWGN") {
                        sim.setAdaptiveHopChannel(ChannelType::AWGN);
                    } else if (ch_str == "good" || ch_str == "GOOD") {
                        sim.setAdaptiveHopChannel(ChannelType::GOOD);
                    } else if (ch_str == "moderate" || ch_str == "MODERATE") {
                        sim.setAdaptiveHopChannel(ChannelType::MODERATE);
                    } else if (ch_str == "poor" || ch_str == "POOR") {
                        sim.setAdaptiveHopChannel(ChannelType::POOR);
                    } else if (ch_str == "flutter" || ch_str == "FLUTTER") {
                        sim.setAdaptiveHopChannel(ChannelType::FLUTTER);
                    } else {
                        std::cerr << "Unknown hop channel: " << ch_str << " (use awgn, good, moderate, poor, flutter)\n";
                        return 1;
                    }
                }
            } else if (arg == "--mod" || arg == "-m") {
                if (i + 1 < argc) {
                    std::string mod_str = argv[++i];
                    if (mod_str == "dqpsk" || mod_str == "DQPSK") {
                        sim.setForcedModulation(Modulation::DQPSK);
                    } else if (mod_str == "d8psk" || mod_str == "D8PSK") {
                        sim.setForcedModulation(Modulation::D8PSK);
                    } else if (mod_str == "dbpsk" || mod_str == "DBPSK") {
                        sim.setForcedModulation(Modulation::DBPSK);
                    } else if (mod_str == "qpsk" || mod_str == "QPSK") {
                        sim.setForcedModulation(Modulation::QPSK);
                    } else if (mod_str == "bpsk" || mod_str == "BPSK") {
                        sim.setForcedModulation(Modulation::BPSK);
                    } else if (mod_str == "qam16" || mod_str == "QAM16") {
                        sim.setForcedModulation(Modulation::QAM16);
                    } else if (mod_str == "qam32" || mod_str == "QAM32") {
                        sim.setForcedModulation(Modulation::QAM32);
                    } else if (mod_str == "qam64" || mod_str == "QAM64") {
                        sim.setForcedModulation(Modulation::QAM64);
                    } else {
                        std::cerr << "Unknown modulation: " << mod_str
                                  << " (use dqpsk, d8psk, dbpsk, qpsk, bpsk, qam16, qam32, qam64)\n";
                        return 1;
                    }
                }
            } else if (arg == "--rate" || arg == "-r") {
                if (i + 1 < argc) {
                    std::string rate_str = argv[++i];
                    if (rate_str == "r1_4" || rate_str == "R1_4") {
                        sim.setForcedCodeRate(CodeRate::R1_4);
                    } else if (rate_str == "r1_2" || rate_str == "R1_2") {
                        sim.setForcedCodeRate(CodeRate::R1_2);
                    } else if (rate_str == "r2_3" || rate_str == "R2_3") {
                        sim.setForcedCodeRate(CodeRate::R2_3);
                    } else if (rate_str == "r3_4" || rate_str == "R3_4") {
                        sim.setForcedCodeRate(CodeRate::R3_4);
                    } else if (rate_str == "auto" || rate_str == "AUTO") {
                        sim.setForcedCodeRate(CodeRate::AUTO);
                    } else {
                        std::cerr << "Unknown code rate: " << rate_str << " (use auto, r1_4, r1_2, r2_3, r3_4)\n";
                        return 1;
                    }
                }
            } else if (arg == "--cw-count" && i + 1 < argc) {
                int cw_count = std::stoi(argv[++i]);
                if (cw_count < v2::kMinFixedFrameCodewords ||
                    cw_count > v2::kMaxFixedFrameCodewords) {
                    std::cerr << "Invalid --cw-count: " << cw_count
                              << " (use " << v2::kMinFixedFrameCodewords
                              << ".." << v2::kMaxFixedFrameCodewords << ")\n";
                    return 1;
                }
                sim.setFixedFrameCodewords(cw_count);
            } else if (arg == "--waveform" || arg == "-w") {
                if (i + 1 < argc) {
                    std::string wf_str = argv[++i];
                    if (wf_str == "mc_dpsk" || wf_str == "dpsk") {
                        sim.setPreferredWaveform(WaveformMode::MC_DPSK);
                    } else if (wf_str == "ofdm_chirp") {
                        sim.setPreferredWaveform(WaveformMode::OFDM_CHIRP);
                    } else if (wf_str == "ofdm_cox" || wf_str == "ofdm") {
                        sim.setPreferredWaveform(WaveformMode::OFDM_COX);
                    } else if (wf_str == "ofdm_narrow" || wf_str == "narrow") {
                        sim.setPreferredWaveform(WaveformMode::OFDM_NARROW);
                    } else {
                        std::cerr << "Unknown waveform: " << wf_str << " (use mc_dpsk, ofdm_chirp, ofdm_cox, ofdm_narrow)\n";
                        return 1;
                    }
                }
            } else if (arg == "--ofdm-config") {
                if (i + 1 >= argc) {
                    std::cerr << "Missing value for --ofdm-config (use default or nvis)\n";
                    return 1;
                }
                std::string cfg_str = argv[++i];
                if (cfg_str == "default" || cfg_str == "DEFAULT") {
                    sim.setOFDMConfigPreset(OFDMConfigPreset::Default);
                } else if (cfg_str == "nvis" || cfg_str == "NVIS") {
                    sim.setOFDMConfigPreset(OFDMConfigPreset::Nvis);
                } else {
                    std::cerr << "Unknown OFDM config: " << cfg_str
                              << " (use default or nvis)\n";
                    return 1;
                }
            } else if (arg == "--file" || arg == "--test-file") {
                sim.setTestFileTransfer(true);
                // Optional file size argument
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    sim.setTestFileSize(std::stoul(argv[++i]));
                }
            } else if (arg == "--no-channel-interleave" || arg == "--nci") {
                sim.setChannelInterleave(false);
            } else if (arg == "--channel-interleave" || arg == "-ci") {
                sim.setChannelInterleave(true);
            } else if (arg == "--no-burst-interleave" || arg == "--nbi") {
                sim.setNoBurstInterleave(true);
            } else if (arg == "--burst-group-size" && i + 1 < argc) {
                sim.setBurstInterleaveGroupSize(std::stoi(argv[++i]));
            } else if (arg == "--harq") {
                sim.setSoftCombiningHARQ(true);
            } else if (arg == "--rx-overfeed-factor" && i + 1 < argc) {
                sim.setRxOverfeedFactor(std::stoi(argv[++i]));
            } else if (arg == "--decode-delay-ms" && i + 1 < argc) {
                sim.setDecodeDelayMs(std::stoi(argv[++i]));
            } else if (arg == "--rx-batch-callbacks" && i + 1 < argc) {
                sim.setRxBatchCallbacks(std::stoi(argv[++i]));
            } else if (arg == "--burst-test") {
                sim.setTestBurst(true);
            } else if (arg == "--seed" && i + 1 < argc) {
                sim.setSeed(static_cast<uint32_t>(std::stoul(argv[++i])));
            } else if ((arg == "--tx-cfo" || arg == "--cfo") && i + 1 < argc) {
                sim.setTxCFO(std::stof(argv[++i]));
            } else if (arg == "--save-signals") {
                int message_limit = 0;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    message_limit = std::stoi(argv[++i]);
                }
                sim.setSaveSignals(true, message_limit);
            } else if (arg == "--adpt-test") {
                sim.setAdaptiveTest(true);
            } else if (arg == "--hop-snr" && i + 1 < argc) {
                sim.setAdaptiveHopSNR(std::stof(argv[++i]));
            } else if (arg == "--save-prefix" && i + 1 < argc) {
                sim.setSaveSignalsPrefix(argv[++i]);
            } else if (arg == "--save-max-samples" && i + 1 < argc) {
                sim.setSaveSignalsMaxSamples(static_cast<size_t>(std::stoull(argv[++i])));
            } else if (arg == "--role" && i + 1 < argc) {
                std::string r = argv[++i];
                if (r == "A" || r == "a")          sim.setRoleA();
                else if (r == "B" || r == "b")     sim.setRoleB();
                else if (r == "both" || r == "BOTH") sim.setRoleBoth();
                else {
                    std::cerr << "Unknown --role: " << r << " (use A, B, or both)\n";
                    return 1;
                }
            } else if (arg == "--peer" && i + 1 < argc) {
                sim.setPeerCallsign(argv[++i]);
            } else if (arg == "--callsign" && i + 1 < argc) {
                sim.setSelfCallsign(argv[++i]);
            } else if (arg == "--audio-output" && i + 1 < argc) {
                sim.setAudioOutputDevice(argv[++i]);
            } else if (arg == "--audio-input" && i + 1 < argc) {
                sim.setAudioInputDevice(argv[++i]);
            } else if (arg == "--list-audio-devices") {
                sim.setListAudioDevices(true);
            } else if (arg == "--idle-seconds" && i + 1 < argc) {
                sim.setRoleBIdleSeconds(std::stoi(argv[++i]));
            } else if (arg == "--inject-channel") {
                sim.setInjectChannel(true);
            } else if (arg == "--inject-gain" && i + 1 < argc) {
                sim.setInjectGain(std::stof(argv[++i]));
            } else if (arg == "--audio-buffer-size" && i + 1 < argc) {
                sim.setAudioBufferSize(std::stoi(argv[++i]));
            } else if (arg == "--help" || arg == "-h") {
                std::cout << "CLI Simulator - IWaveform + StreamingDecoder Model\n\n";
                std::cout << "Uses IWaveform for TX and StreamingDecoder for RX directly.\n";
                std::cout << "Every 10ms: read RX, feed decoder, get TX, send to channel.\n\n";
                std::cout << "Options:\n";
                std::cout << "  --snr, -s <dB>      SNR (default: 20)\n";
                std::cout << "  --channel, -c <CH>  Channel type: awgn, good, moderate, poor, flutter\n";
                std::cout << "                        awgn     - No fading, no multipath\n";
                std::cout << "                        good     - 0.5ms delay, 0.1Hz Doppler (quiet)\n";
                std::cout << "                        moderate - 1.0ms delay, 0.5Hz Doppler (typical)\n";
                std::cout << "                        poor     - 2.0ms delay, 1.0Hz Doppler (disturbed)\n";
                std::cout << "                        flutter  - 0.5ms delay, 10Hz Doppler (auroral)\n";
                std::cout << "  --fading, -f        Alias for --channel moderate\n";
                std::cout << "  --mod, -m <MOD>     Force modulation: dqpsk, d8psk, dbpsk, qpsk, bpsk, qam16, qam32, qam64\n";
                std::cout << "  --rate, -r <RATE>   Force code rate: auto, r1_4, r1_2, r2_3, r3_4\n";
                std::cout << "  --cw-count <N>      Fixed OFDM data-frame codewords (1-8, default: 4)\n";
                std::cout << "  --waveform, -w <WF> Force waveform: mc_dpsk, ofdm_chirp, ofdm_cox, ofdm_narrow\n";
                std::cout << "  --ofdm-config <CFG> OFDM_COX config: default (512/30) or nvis (1024/59)\n";
                std::cout << "  --seed <N>          Random seed (default: 42)\n";
                std::cout << "  --tx-cfo <Hz>       Inject TX CFO in channel model (default: 0)\n";
                std::cout << "  --cfo <Hz>          Alias for --tx-cfo\n";
                std::cout << "  --file [SIZE]       Test file transfer (default: 256 bytes)\n";
                std::cout << "  --adpt-test         Two-message adaptive advisory smoke test\n";
                std::cout << "  --hop-snr <dB>      Condition-B SNR for --adpt-test (default: 12)\n";
                std::cout << "  --hop-channel <CH>  Condition-B channel for --adpt-test\n";
                std::cout << "  --channel-interleave, -ci  Enable channel interleaving\n";
                std::cout << "  --no-burst-interleave     Disable burst-level long interleaving\n";
                std::cout << "  --burst-group-size <N>    Burst interleave group size (2-8, default: 8)\n";
                std::cout << "  --harq                    Enable RX soft-combining HARQ (default: off)\n";
                std::cout << "  --rx-overfeed-factor <N>  Run audio callbacks N× faster wall-clock (stress, default: 1)\n";
                std::cout << "  --decode-delay-ms <N>     Add decode-thread delay (0-500 ms, stress)\n";
                std::cout << "  --rx-batch-callbacks <N>  Batch N callbacks per decoder feed (stress)\n";
                std::cout << "  --burst-test              Send large messages to test burst interleaving\n";
                std::cout << "  --save-signals [N]        Save TX/RX raw float traces (.f32)\n";
                std::cout << "                           N = stop capture after BRAVO receives N app messages\n";
                std::cout << "                           (default: 0 = capture full run)\n";
                std::cout << "  --save-prefix <PATH>      Capture file prefix (default: /tmp/cli_signals)\n";
                std::cout << "  --save-max-samples <N>    Per-stream capture cap (0 = unlimited)\n";
                std::cout << "  --verbose, -v       Verbose output\n";
                std::cout << "\nHardware audio mode (real soundcard, two-machine setup):\n";
                std::cout << "  --role A|B|both     A=initiator, B=responder, both=in-process sim (default)\n";
                std::cout << "  --callsign <NAME>   Local callsign (default: ALPHA for A, BRAVO for B)\n";
                std::cout << "  --peer <NAME>       Peer callsign for --role A (default: BRAVO)\n";
                std::cout << "  --audio-output <D>  SDL2 output device name (empty = default)\n";
                std::cout << "  --audio-input <D>   SDL2 input device name (empty = default)\n";
                std::cout << "  --list-audio-devices  List available audio devices and exit\n";
                std::cout << "  --idle-seconds <N>  Role B: max idle seconds before giving up (0 = forever)\n";
                std::cout << "  --inject-channel    Apply --snr/--channel/--cfo to outgoing audio\n";
                std::cout << "                      (lets Mac-to-Pi cable carry a synthetic HF channel)\n";
                std::cout << "  --inject-gain <G>   Post-injection output gain/headroom (0.05-1.0, default 0.70)\n";
                std::cout << "  --audio-buffer-size <N>  SDL2 period size, samples (default 8192)\n";
                std::cout << "                      Smaller = lower latency. Larger = more XRUN headroom.\n";
                return 0;
            }
        }
        setLogLevel(LogLevel::INFO);
        return sim.runTest() ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Fatal exception in cli_simulator: " << e.what() << "\n";
        return 2;
    } catch (...) {
        std::cerr << "Fatal unknown exception in cli_simulator\n";
        return 3;
    }
}
