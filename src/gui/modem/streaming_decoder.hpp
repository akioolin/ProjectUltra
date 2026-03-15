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
#include "ultra/dsp.hpp"
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
#include <chrono>

namespace ultra {
namespace gui {

// Forward declarations
namespace v2 = protocol::v2;

// Decoder state machine for continuous correlation
// Real receivers run correlation continuously, not in batches
enum class DecoderState {
    SEARCHING,              // Running correlation on incoming samples
    SYNC_FOUND,             // Chirp detected, collecting frame samples
    DECODING,               // Have enough samples, decoding in progress
    BURST_ACCUMULATING,     // Burst marker detected, waiting for all 4 frames
};

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
    uint64_t overflow_samples_dropped = 0;
    uint64_t overflow_state_resets = 0;
    uint64_t current_unsearched_samples = 0;
    uint64_t peak_unsearched_samples = 0;
    float backlog_ms = 0.0f;
    float peak_backlog_ms = 0.0f;
    float buffer_fill_percent = 0.0f;
    float avg_decode_time_ms = 0.0f;
    uint64_t sync_recovery_attempts = 0;
    uint64_t sync_recovery_successes = 0;
    uint64_t sync_recovery_delta_p8 = 0;
    uint64_t sync_recovery_delta_m8 = 0;
    uint64_t sync_recovery_delta_p16 = 0;
    uint64_t sync_recovery_delta_m16 = 0;
    uint64_t sync_recovery_delta_p24 = 0;
    uint64_t sync_recovery_delta_m24 = 0;
    uint64_t sync_recovery_delta_p32 = 0;
    uint64_t sync_recovery_delta_m32 = 0;
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

    // Set OFDM config (recreates waveform with the given config)
    // Use this for NVIS mode (1024 FFT, 59 carriers) or custom OFDM settings
    void setOFDMConfig(const ModemConfig& config);

    // Atomically apply OFDM connected-mode settings to avoid transient
    // mode/config mismatches during handshake transitions.
    void setConnectedOFDMMode(protocol::WaveformMode mode,
                              const ModemConfig& config,
                              Modulation mod,
                              CodeRate rate);

    // Set data mode (modulation and code rate) for the waveform
    // Called when connection is established with negotiated settings
    void setDataMode(Modulation mod, CodeRate rate);

    // Set FEC codec type (for dynamic codec switching based on SNR)
    // Recreates the codec if type changes
    void setCodecType(fec::CodecType type);
    fec::CodecType getCodecType() const { return codec_type_; }

    void setChannelInterleave(bool enable) { use_channel_interleave_ = enable; }
    bool getChannelInterleave() const { return use_channel_interleave_; }

    // Burst-level long interleaver (N-frame groups)
    void setBurstInterleave(bool enable) { use_burst_interleave_ = enable; }
    bool getBurstInterleave() const { return use_burst_interleave_; }
    void setBurstInterleaveGroupSize(int size);
    int getBurstInterleaveGroupSize() const { return burst_group_size_; }

    // Get current mode
    protocol::WaveformMode getMode() const { return mode_; }
    bool isConnected() const { return connected_; }

    // Get detected bandwidth from dual-listen (valid after sync detection when disconnected)
    BandwidthMode getDetectedBandwidth() const { return detected_bandwidth_; }
    void resetDetectedBandwidth() { detected_bandwidth_ = BandwidthMode::WIDE; }

    // Get current configuration (for comparison with encoder)
    // Returns: mode, modulation, code_rate, carriers, interleaving settings
    struct DecoderConfig {
        protocol::WaveformMode mode = protocol::WaveformMode::MC_DPSK;
        Modulation modulation = Modulation::DQPSK;
        CodeRate code_rate = CodeRate::R1_4;
        int num_carriers = 59;
        int data_carriers = 53;
        int bits_per_symbol = 106;
        bool use_pilots = true;
        int pilot_spacing = 10;
        bool use_channel_interleave = false;
        bool use_frame_interleave = true;
    };
    DecoderConfig getConfig() const;

    // ========================================================================
    // CALLBACKS
    // ========================================================================

    void setFrameCallback(FrameDecodedCallback callback) { frame_callback_ = callback; }
    void setPingCallback(StreamingPingCallback callback) { ping_callback_ = callback; }
    void setLogPrefix(const std::string& prefix) { log_prefix_ = prefix; }

    // ========================================================================
    // STATUS
    // ========================================================================

    // Get last measured SNR (from most recent chirp detection)
    float getLastSNR() const { return last_snr_.load(); }

    // Get last measured CFO
    float getLastCFO() const { return last_cfo_.load(); }

    // Set known CFO (for testing or when CFO is known from other source)
    void setKnownCFO(float cfo_hz) { last_cfo_.store(cfo_hz); }

    // Get last measured fading index (from per-carrier magnitude variance)
    // 0-1 range, > 0.4 indicates significant fading
    float getLastFadingIndex() const { return last_fading_index_.load(); }

    // Get buffer fill level (0-100%)
    float getBufferFillPercent() const;

    // Get decoder statistics
    DecoderStats getStats() const;

    // Get number of samples in buffer
    size_t samplesInBuffer() const;

    // Check if waveform is synchronized
    bool isSynced() const;

    // Get constellation symbols for display
    std::vector<std::complex<float>> getConstellationSymbols() const;

    // ========================================================================
    // LIFECYCLE
    // ========================================================================

    // Reset decoder state (clear buffer, reset waveform)
    void reset();

    // Signal shutdown - wakes processBuffer() to return
    void stop();

    // Clear shutdown flag (call after stop() + join() to reuse decoder)
    // Needed when switching from async decode thread to synchronous mode
    void clearShutdown() { shutdown_.store(false); }

    // Check if decoder is running (not stopped)
    bool isRunning() const { return !shutdown_.load(); }

private:
    // ========================================================================
    // INTERNAL HELPERS
    // ========================================================================

    // Search for sync in recent samples
    void searchForSync();

    // Check if we have enough samples to decode
    void checkIfReadyToDecode();

    // Decode the current frame
    void decodeCurrentFrame();

    // Estimate SNR from chirp correlation strength
    float estimateSNRFromChirp(float correlation, float noise_floor);

    // Decode soft bits into frame data
    DecodeResult decodeFrame(const std::vector<float>& soft_bits, float snr, float cfo);

    // Cache latest non-empty constellation snapshot so GUI can render
    // even when control-path profile switching temporarily clears waveform state.
    void captureConstellationSnapshot();

    // MC-DPSK specific decode (simple sequential, no frame interleaving)
    DecodeResult decodeMCDPSKFrame(const std::vector<float>& soft_bits,
                                    CodeRate rate, size_t bytes_per_cw,
                                    float snr, float cfo);

    // CFO pre-correction: remove CFO from real passband samples before demodulation.
    // Uses Hilbert transform → analytic signal → complex rotation → real part.
    // After pre-correction, waveform should be told CFO=0.
    // Returns the CFO value used for pre-correction (for feedback adjustment).
    float applyCFOPreCorrection(std::vector<float>& samples, float cfo_hz,
                                 size_t absolute_start_sample);

    // Burst interleave accumulation
    enum class BurstFrameResult {
        SUCCESS,    // Frame demodulated, soft bits appended to burst_soft_buffer_
        WAITING,    // Not enough samples yet — caller should return and wait
        FAILED,     // Hard failure (energy lost or process error) — abort group
    };
    void accumulateBurstFrames();
    BurstFrameResult tryDemodulateNextBurstFrame();
    void finalizeBurstGroup();

    // Legacy methods (kept for compatibility, do nothing)
    bool runCorrelationSearch(size_t new_samples);
    bool tryDecodeFrame();
    std::vector<float> copySamplesFrom(size_t start_pos, size_t count);
    size_t samplesAvailableFrom(size_t pos) const;
    bool isPingOnly(const std::vector<float>& samples, size_t chirp_end);
    void updateNoiseFloor(const float* samples, size_t count);

    // ========================================================================
    // STATE
    // ========================================================================

    // Circular buffer for audio samples
    std::vector<float> buffer_;
    size_t write_pos_ = 0;          // Next position to write (only pointer we need)
    mutable std::mutex buffer_mutex_;
    std::condition_variable data_cv_;

    // Continuous correlation state machine (like real receivers)
    DecoderState state_ = DecoderState::SEARCHING;
    size_t sync_position_ = 0;        // Buffer position where sync was found
    size_t samples_since_sync_ = 0;   // How many samples collected since sync
    float sync_cfo_ = 0.0f;           // CFO from sync detection
    float sync_snr_ = 0.0f;           // SNR estimate from sync detection
    size_t correlation_pos_ = 0;      // Current position for correlation search
    size_t last_decoded_sync_pos_ = SIZE_MAX;  // Last successfully decoded sync position (to prevent duplicates)

    // Reset generation counter - incremented on reset(), checked after slow operations
    // to detect if state was reset mid-operation (e.g., during correlation)
    std::atomic<uint32_t> reset_generation_{0};

    // Active waveform for demodulation (handles its own sync internally)
    WaveformFactory waveform_factory_;
    std::unique_ptr<IWaveform> waveform_;
    protocol::WaveformMode mode_ = protocol::WaveformMode::MC_DPSK;
    bool connected_ = false;

    // Dual-listen: narrowband waveform for detecting narrowband chirps when disconnected
    // Lazy-initialized on first search to avoid startup cost
    std::unique_ptr<IWaveform> narrow_waveform_;
    bool narrow_waveform_initialized_ = false;
    BandwidthMode detected_bandwidth_ = BandwidthMode::WIDE;
    int mc_dpsk_carriers_ = 8;  // MC-DPSK carrier count (default 8)
    int ofdm_carriers_ = 30;    // OFDM carrier count (default 30 for standard mode)
    int ofdm_data_carriers_ = 30;  // Data carriers after pilot allocation (for interleaver)
    Modulation current_modulation_ = Modulation::DQPSK;  // Current modulation for interleaver
    CodeRate code_rate_ = CodeRate::R1_4;  // Code rate for FEC decode
    fec::CodecType codec_type_ = fec::CodecType::LDPC;  // FEC codec type
    size_t mode_switch_write_pos_ = 0;  // write_pos at mode switch (skip old data)

    // Interleaver (matches TX)
    std::unique_ptr<ChannelInterleaver> interleaver_;
    bool use_channel_interleave_ = true;

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
    float pre_correction_cfo_ = 0.0f;  // CFO used for last pre-correction (for feedback adjustment)
    uint64_t overflow_events_ = 0;
    uint64_t sync_reject_streak_ = 0;

    // Constellation cache (protected by buffer_mutex_)
    mutable std::vector<std::complex<float>> constellation_cache_;
    mutable std::chrono::steady_clock::time_point constellation_cache_time_{};
    static constexpr int CONSTELLATION_CACHE_HOLD_MS = 1500;

    // Lifecycle
    std::atomic<bool> shutdown_{false};
    bool new_data_available_ = false;  // Flag to wake decode thread immediately

    // Logging
    std::string log_prefix_ = "StreamingDecoder";
    size_t total_fed_ = 0;      // Total samples fed (per-instance)
    int feed_iter_ = 0;         // Feed counter (per-instance)

    // Burst mode continuation (OFDM only)
    // After decoding a frame in connected OFDM mode, check for next block at known position
    int burst_blocks_decoded_ = 0;     // Blocks decoded in current burst
    static constexpr int MAX_BURST_BLOCKS = 8;  // Safety limit

    // Burst interleave accumulation state (valid only in BURST_ACCUMULATING)
    bool use_burst_interleave_ = false;
    std::vector<std::vector<float>> burst_soft_buffer_;  // collected soft bits per frame
    size_t burst_next_pos_ = 0;          // buffer position for next continuation frame
    size_t burst_min_block_ = 0;         // samples per frame (cached from first frame)
    float burst_snr_ = 0.0f;             // SNR from sync detection
    float burst_cfo_ = 0.0f;             // CFO (updated per frame from pilot tracking)
    std::chrono::steady_clock::time_point burst_start_time_;  // timeout reference
    int burst_group_size_ = 4;
    static constexpr int BURST_TIMEOUT_MS_BASE = 8000;  // 4 frames × ~0.7s + margin

    // Pending frame state for multi-codeword frames
    // After reading header, if more codewords needed, wait for more samples
    int pending_total_cw_ = 0;                // Total codewords expected (0 = unknown)
    std::chrono::steady_clock::time_point sync_start_time_;  // When sync was found
    static constexpr int FRAME_TIMEOUT_MS = 5000;  // Give up after 5 seconds

    // Constants - Buffer sizes
    // Need enough for chirp (~1.2s) + frame (~1s) + search margin
    // Larger buffer to avoid wraparound issues during testing
    static constexpr size_t MAX_BUFFER_SAMPLES = 480000;    // 10 seconds at 48kHz
    static constexpr size_t CHIRP_SAMPLES = 57600;          // ~1.2 second (dual chirp)
    static constexpr size_t CORRELATION_STEP = 4800;        // 100ms at 48kHz (faster search)
    static constexpr size_t CORR_INVARIANT_GUARD = 9600;    // 200ms guard to detect pointer drift
    static constexpr size_t OVERFLOW_RECOVERY_KEEP = 120000; // Keep ~2.5s newest audio when overloaded

    // Constants - Adaptive acquisition thresholds (disabled for now)
    static constexpr float CORR_NOISE_THRESHOLD = 0.05f;    // Below = pure noise, don't advance
    static constexpr float CORR_WEAK_THRESHOLD = 0.10f;     // Below = weak, advance slowly
    static constexpr float CORR_DETECT_THRESHOLD = 0.15f;   // At/above = detected
    static constexpr float ENERGY_GATE_MULTIPLIER = 0.0f;   // Disabled - noise floor estimation is inaccurate
    static constexpr float PING_ENERGY_RATIO = 0.3f;        // Post-chirp/chirp energy ratio
};

} // namespace gui
} // namespace ultra
