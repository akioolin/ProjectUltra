#pragma once

#include <SDL.h>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>
#include <string>
#include <functional>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#endif

namespace ultra {
namespace gui {

#ifdef _WIN32
class AudioEngineMutex {
public:
    AudioEngineMutex() noexcept = default;
    AudioEngineMutex(const AudioEngineMutex&) = delete;
    AudioEngineMutex& operator=(const AudioEngineMutex&) = delete;

    void lock() noexcept { AcquireSRWLockExclusive(&lock_); }
    void unlock() noexcept { ReleaseSRWLockExclusive(&lock_); }

private:
    SRWLOCK lock_ = SRWLOCK_INIT;
};
#else
using AudioEngineMutex = std::mutex;
#endif

// Audio engine for real-time audio I/O using SDL2
class AudioEngine {
public:
    enum class InputCaptureMode {
        Auto,
        Callback,
        Queue
    };

    AudioEngine();
    ~AudioEngine();

    // Initialize SDL audio subsystem
    bool initialize();
    void shutdown();
    bool isInitialized() const { return initialized_; }

    // Device enumeration
    std::vector<std::string> getOutputDevices();
    std::vector<std::string> getInputDevices();

    // Open/close devices
    bool openOutput(const std::string& device = "");  // Empty = default
    bool openInput(const std::string& device = "");
    void closeOutput();
    void closeInput();
    bool hasOutputDevice() const { return output_device_ != 0; }
    bool hasInputDevice() const { return input_device_ != 0; }

    // TX: Queue samples to play
    void queueTxSamples(const std::vector<float>& samples);
    void clearTxQueue();
    bool isTxQueueEmpty() const;
    size_t getTxQueueSize() const;

    // RX: Get captured samples (for real input mode)
    std::vector<float> getRxSamples(size_t max_samples);
    size_t getRxBufferSize() const;

    // Loopback mode: TX samples feed directly to RX
    void setLoopbackEnabled(bool enabled) { loopback_enabled_ = enabled; }
    bool isLoopbackEnabled() const { return loopback_enabled_; }

    // Input capture backend selection (for Linux USB edge cases)
    void setInputCaptureMode(InputCaptureMode mode) { input_capture_mode_ = mode; }
    InputCaptureMode getInputCaptureMode() const { return input_capture_mode_; }
    bool isInputQueueMode() const { return input_queue_mode_; }

    // Loopback channel simulation
    void setLoopbackSNR(float snr_db) { loopback_snr_db_ = snr_db; }
    float getLoopbackSNR() const { return loopback_snr_db_; }
    void resetNoiseSeed() { noise_seed_ = 12345; }  // Reset noise to deterministic start

    // Playback control
    void startPlayback();
    void stopPlayback();
    bool isPlaying() const { return playing_; }

    // Capture control
    void startCapture();
    void stopCapture();
    bool isCapturing() const { return capturing_; }
    void clearRxBuffer();  // Clear captured audio buffer
    void setRxMuted(bool muted) { rx_muted_ = muted; }  // Prevent callback from firing
    SDL_AudioStatus getInputStatus() const;
    Uint32 getQueuedInputBytes() const;

    // Audio parameters
    int getSampleRate() const { return sample_rate_; }

    // Audio level metering
    float getInputLevel() const { return input_level_; }
    float getOutputLevel() const { return output_level_; }

    // Input gain control (0.0 to 2.0, default 1.0)
    void setInputGain(float gain) { input_gain_ = gain; }
    float getInputGain() const { return input_gain_; }

    // Output gain control (0.0 to 1.0, default 1.0)
    void setOutputGain(float gain);
    float getOutputGain() const { return output_gain_; }

    // Callback for when RX has data ready
    using RxCallback = std::function<void(const std::vector<float>&)>;
    void setRxCallback(RxCallback callback);

private:
    // SDL audio callbacks
    static void outputCallback(void* userdata, Uint8* stream, int len);
    static void inputCallback(void* userdata, Uint8* stream, int len);

    // Add noise for loopback channel simulation
    void addChannelNoise(std::vector<float>& samples);
    void appendCapturedSamples(const float* input, size_t samples, float gain);

    SDL_AudioDeviceID output_device_ = 0;
    SDL_AudioDeviceID input_device_ = 0;
    bool input_queue_mode_ = false;
    InputCaptureMode input_capture_mode_ = InputCaptureMode::Auto;

    // TX buffer (samples waiting to be played)
    std::queue<float> tx_queue_;
    mutable AudioEngineMutex tx_mutex_;

    // RX buffer (captured samples)
    std::vector<float> rx_buffer_;
    mutable AudioEngineMutex rx_mutex_;

    // Loopback settings
    std::atomic<bool> loopback_enabled_{false};
    std::atomic<float> loopback_snr_db_{25.0f};

    // State
    std::atomic<bool> playing_{false};
    std::atomic<bool> capturing_{false};
    std::atomic<bool> rx_muted_{false};  // Prevent RX callback from firing during TX
    bool initialized_ = false;
    bool owns_audio_subsystem_ = false;

    // Audio parameters
    int sample_rate_ = 48000;
    int buffer_size_ = 1024;

    // Buffer limits (prevent unbounded growth if main loop stalls)
    static constexpr size_t MAX_RX_BUFFER_SAMPLES = 96000;  // 2 seconds at 48kHz

    // RX callback
    RxCallback rx_callback_;
    mutable AudioEngineMutex rx_callback_mutex_;

    // Random generator for noise
    uint32_t noise_seed_ = 12345;

    // Audio level metering (RMS, 0.0-1.0)
    std::atomic<float> input_level_{0.0f};
    std::atomic<float> output_level_{0.0f};

    // Input gain (0.0 to 2.0, default 1.0)
    std::atomic<float> input_gain_{1.0f};
    float input_dc_x_prev_ = 0.0f;
    float input_dc_y_prev_ = 0.0f;

    // Output gain (0.0 to 1.0, default 1.0)
    std::atomic<float> output_gain_{1.0f};
};

} // namespace gui
} // namespace ultra
