#pragma once

// StreamingDecoder - Unified RX decoder for all waveform types
//
// Replaces the buggy RxPipeline with a clean sliding-window design.
// Key differences from RxPipeline:
//   1. Circular buffer (bounded) vs growing vector (unbounded)
//   2. Sliding window search (like test_iwaveform) vs periodic full search
//   3. Correct IWaveform call sequence: reset(), detectSync(), setFrequencyOffset(), process()
//   4. Thread-safe with condition variable for blocking wait
//   5. PING detection via energy ratio after chirp
//   6. SNR estimation from chirp correlation strength
//
// Thread model:
//   - Audio thread: feedAudio() - fast, just copies samples to buffer
//   - Decode thread: processBuffer() - does heavy work (chirp detection, LDPC)
//
// Usage:
//   StreamingDecoder decoder;
//   decoder.setMode(WaveformMode::MC_DPSK, false);  // Disconnected
//
//   // Audio thread:
//   decoder.feedAudio(samples, count);
//
//   // Decode thread:
//   while (running) {
//       decoder.processBuffer();  // Blocks until data available
//       while (decoder.hasFrame()) {
//           auto result = decoder.getFrame();
//           // Handle result
//       }
//   }
//
//   // Shutdown:
//   decoder.stop();  // Wakes decode thread to exit

#include "waveform/waveform_interface.hpp"
#include "waveform/waveform_factory.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/fec.hpp"
#include "fec/codec_factory.hpp"  // ICodec for FEC decoding
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <functional>

namespace ultra {
namespace gui {

// Forward declarations
namespace v2 = protocol::v2;

// Result of decoding a frame
struct DecodeResult {
    bool success = false;           // True if frame decoded successfully
    Bytes frame_data;               // Decoded frame payload
    v2::FrameType frame_type = v2::FrameType::PROBE;
    float snr_db = 0.0f;            // Estimated SNR from preamble
    float cfo_hz = 0.0f;            // Measured CFO
    int codewords_ok = 0;           // Number of successful LDPC decodes
    int codewords_failed = 0;       // Number of failed LDPC decodes
    bool is_ping = false;           // True if this is a PING (chirp-only) frame
};

// Decoder statistics for GUI display
struct DecoderStats {
    uint64_t frames_decoded = 0;
    uint64_t frames_failed = 0;
    uint64_t pings_received = 0;
    uint64_t buffer_overflows = 0;
    float avg_decode_time_ms = 0.0f;
};

// Callbacks for frame delivery
using FrameDecodedCallback = std::function<void(const DecodeResult&)>;
using StreamingPingCallback = std::function<void(float snr_db, float cfo_hz)>;

// StreamingDecoder - Unified RX decoder for all waveform types
class StreamingDecoder {
public:
    StreamingDecoder();
    ~StreamingDecoder();

    // ========================================================================
    // AUDIO THREAD INTERFACE
    // ========================================================================

    // Feed audio samples into the decoder
    // Called from audio callback - must be fast (<1ms)
    // Samples are copied to internal circular buffer
    void feedAudio(const float* samples, size_t count);
    void feedAudio(const std::vector<float>& samples) { feedAudio(samples.data(), samples.size()); }

    // ========================================================================
    // DECODE THREAD INTERFACE
    // ========================================================================

    // Process buffered audio, detect chirps, decode frames
    // Blocks until data available (or timeout/shutdown)
    // Call this in a loop from decode thread
    void processBuffer();

    // Check if any decoded frames are available
    bool hasFrame() const;

    // Get the next decoded frame (removes from queue)
    // Returns empty result if no frames available
    DecodeResult getFrame();

    // ========================================================================
    // MODE CONTROL
    // ========================================================================

    // Set the waveform mode and connection state
    // Disconnected: always decode as MC-DPSK (PING/CONNECT)
    // Connected: decode using negotiated waveform
    void setMode(protocol::WaveformMode mode, bool connected);

    // Set MC-DPSK carrier count (recreates waveform if currently in MC-DPSK mode)
    void setMCDPSKCarriers(int num_carriers);

    // Set data mode (modulation and code rate) for the waveform
    // Called when connection is established with negotiated settings
    void setDataMode(Modulation mod, CodeRate rate);

    // Set FEC codec type (for dynamic codec switching based on SNR)
    // Recreates the codec if type changes
    void setCodecType(fec::CodecType type);
    fec::CodecType getCodecType() const { return codec_type_; }

    // Get current mode
    protocol::WaveformMode getMode() const { return mode_; }
    bool isConnected() const { return connected_; }

    // ========================================================================
    // CALLBACKS
    // ========================================================================

    void setFrameCallback(FrameDecodedCallback callback) { frame_callback_ = callback; }
    void setPingCallback(StreamingPingCallback callback) { ping_callback_ = callback; }

    // ========================================================================
    // STATUS
    // ========================================================================

    // Get last measured SNR (from most recent chirp detection)
    float getLastSNR() const { return last_snr_.load(); }

    // Get last measured CFO
    float getLastCFO() const { return last_cfo_.load(); }

    // Get last measured fading index (from per-carrier magnitude variance)
    // 0-1 range, > 0.4 indicates significant fading
    float getLastFadingIndex() const { return last_fading_index_.load(); }

    // Get buffer fill level (0-100%)
    float getBufferFillPercent() const;

    // Get decoder statistics
    DecoderStats getStats() const;

    // Get number of samples in buffer
    size_t samplesInBuffer() const;

    // ========================================================================
    // LIFECYCLE
    // ========================================================================

    // Reset decoder state (clear buffer, reset waveform)
    void reset();

    // Signal shutdown - wakes processBuffer() to return
    void stop();

    // Check if decoder is running (not stopped)
    bool isRunning() const { return !shutdown_.load(); }

private:
    // ========================================================================
    // INTERNAL HELPERS
    // ========================================================================

    // Copy samples from circular buffer for processing
    // Returns span of samples, caller owns the data
    std::vector<float> copyOutSamples(size_t count);

    // Advance read position past processed samples
    void advanceReadPos(size_t count);

    // Trim old samples to prevent overflow
    void trimOldSamples(size_t keep_samples);

    // Detect chirp and decode frame
    // Returns true if frame decoded (result queued)
    bool detectAndDecode();

    // Check if post-chirp energy indicates data or just PING
    bool isPingOnly(const std::vector<float>& samples, size_t chirp_end);

    // Estimate SNR from chirp correlation strength
    float estimateSNRFromChirp(float correlation, float noise_floor);

    // Update noise floor estimate
    void updateNoiseFloor(const float* samples, size_t count);

    // Decode soft bits into frame data
    DecodeResult decodeFrame(const std::vector<float>& soft_bits, float snr, float cfo);

    // ========================================================================
    // STATE
    // ========================================================================

    // Circular buffer for audio samples
    std::vector<float> buffer_;
    size_t write_pos_ = 0;          // Next position to write
    size_t read_pos_ = 0;           // Next position to read (for search)
    size_t search_pos_ = 0;         // Current search position (can be > read_pos_)
    mutable std::mutex buffer_mutex_;
    std::condition_variable data_cv_;

    // Active waveform for demodulation (handles its own sync internally)
    WaveformFactory waveform_factory_;
    std::unique_ptr<IWaveform> waveform_;
    protocol::WaveformMode mode_ = protocol::WaveformMode::MC_DPSK;
    bool connected_ = false;
    int mc_dpsk_carriers_ = 8;  // MC-DPSK carrier count (default 8)
    CodeRate code_rate_ = CodeRate::R1_4;  // Code rate for FEC decode
    fec::CodecType codec_type_ = fec::CodecType::LDPC;  // FEC codec type
    size_t mode_switch_write_pos_ = 0;  // write_pos at mode switch (skip old data)

    // Interleaver (matches TX)
    std::unique_ptr<ChannelInterleaver> interleaver_;

    // FEC codec (uses ICodec interface)
    fec::CodecPtr codec_;

    // Decoded frame queue
    std::queue<DecodeResult> frame_queue_;
    mutable std::mutex queue_mutex_;

    // Callbacks
    FrameDecodedCallback frame_callback_;
    StreamingPingCallback ping_callback_;

    // Statistics
    DecoderStats stats_;
    mutable std::mutex stats_mutex_;

    // Status (atomic for lock-free read from GUI)
    std::atomic<float> last_snr_{0.0f};
    std::atomic<float> last_cfo_{0.0f};
    std::atomic<float> last_fading_index_{0.0f};
    float noise_floor_ = 0.001f;

    // Lifecycle
    std::atomic<bool> shutdown_{false};

    // Pending frame state (for dynamic codeword handling)
    // When we detect sync and decode header but don't have enough samples for full frame,
    // we save state here and wait for more audio
    bool pending_frame_ = false;              // True if waiting for more samples
    size_t pending_search_pos_ = 0;           // search_pos when sync was found
    int pending_data_start_ = 0;              // Where frame data starts (relative to search_pos)
    int pending_total_cw_ = 0;                // Total codewords from header
    float pending_snr_ = 0.0f;                // SNR from sync detection
    float pending_cfo_ = 0.0f;                // CFO from sync detection
    int pending_retry_count_ = 0;             // How many times we've tried to resume
    static constexpr int MAX_PENDING_RETRIES = 5;  // Give up after this many attempts

    // Constants - Buffer sizes
    // Buffer must hold worst-case test scenario: 5 frames × ~150k samples = 750k + margin
    // Real-world: audio arrives at 48kHz so buffer just needs to hold 2-3 frames
    static constexpr size_t MAX_BUFFER_SAMPLES = 960000;    // 20 seconds at 48kHz
    static constexpr size_t MIN_SAMPLES_FOR_SEARCH = 144000; // 3 seconds - ensure full chirp visible
    static constexpr size_t SLIDE_STEP = 4800;              // 100ms between searches
    static constexpr size_t CHIRP_SAMPLES = 53000;          // ~1.1 second (dual chirp + gap)

    // Constants - Adaptive acquisition thresholds (disabled for now)
    static constexpr float CORR_NOISE_THRESHOLD = 0.05f;    // Below = pure noise, don't advance
    static constexpr float CORR_WEAK_THRESHOLD = 0.10f;     // Below = weak, advance slowly
    static constexpr float CORR_DETECT_THRESHOLD = 0.15f;   // At/above = detected
    static constexpr float ENERGY_GATE_MULTIPLIER = 0.0f;   // Disabled - noise floor estimation is inaccurate
    static constexpr float PING_ENERGY_RATIO = 0.3f;        // Post-chirp/chirp energy ratio
};

} // namespace gui
} // namespace ultra
