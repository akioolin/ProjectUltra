#pragma once

// ModemEngine - Main modem interface class
// Handles TX/RX audio processing with OFDM and DPSK modulation

#include "modem_types.hpp"
#include "ultra/types.hpp"
#include "ultra/ofdm.hpp"
#include "ultra/otfs.hpp"
#include "ultra/fec.hpp"  // Interleaver, ChannelInterleaver
#include "fec/codec_factory.hpp"  // ICodec, CodecFactory
#include "ultra/dsp.hpp"  // FIRFilter
#include "psk/dpsk.hpp"   // DPSKModulator, DPSKDemodulator
#include "psk/multi_carrier_dpsk.hpp"  // MultiCarrierDPSK for fading channels
#include "sync/chirp_sync.hpp"  // ChirpSync for robust fading channel detection
#include "../adaptive_mode.hpp"
#include "protocol/frame_v2.hpp"  // v2::FrameType
#include "protocol/waveform_selection.hpp"  // WaveformRecommendation, recommendWaveformAndRate
#include "waveform/waveform_interface.hpp"  // IWaveform abstraction
#include "streaming_decoder.hpp"  // StreamingDecoder - primary decoder
#include <memory>
#include <vector>
#include <queue>
#include <mutex>
#include <functional>
#include <string>
#include <atomic>
#include <thread>

namespace ultra {
namespace gui {

// Real modem engine using OFDM modulator/demodulator and LDPC codec
class ModemEngine {
public:
    ModemEngine();
    ~ModemEngine();

    // Set a name/prefix for logging (e.g., "OUR" or "SIM")
    void setLogPrefix(const std::string& prefix);
    const std::string& getLogPrefix() const { return log_prefix_; }

    // ========================================================================
    // CONFIGURATION
    // ========================================================================
    void setConfig(const ModemConfig& config);
    const ModemConfig& getConfig() const { return config_; }

    void setFilterConfig(const FilterConfig& config);
    const FilterConfig& getFilterConfig() const { return filter_config_; }
    void setFilterEnabled(bool enabled);
    bool isFilterEnabled() const { return filter_config_.enabled; }

    // ========================================================================
    // TX: Convert data to audio samples
    // ========================================================================
    std::vector<float> transmit(const std::string& text);
    std::vector<float> transmit(const Bytes& data);

    // Minimal ping/pong probe (fast presence check, ~1 sec vs ~16 sec CONNECT)
    // Returns: preamble + raw DPSK "ULTR" bytes (no LDPC encoding)
    std::vector<float> transmitPing();
    std::vector<float> transmitPong();  // Same as ping, context determines meaning

    // Test signal generation
    std::vector<float> generateTestTone(float duration_sec = 1.0f);
    std::vector<float> transmitTestPattern(int pattern = 0);
    std::vector<float> transmitRawOFDM(int pattern = 0);

    // ========================================================================
    // RX: Audio input (thread-safe, non-blocking)
    // ========================================================================
    // Feed audio samples - call from any thread (audio callback, simulator, etc.)
    // Samples are buffered internally and processed by background threads.
    // Decoded frames trigger the RawDataCallback automatically.
    void feedAudio(const float* samples, size_t count);
    void feedAudio(const std::vector<float>& samples);

    // Inject test signal from file (for debugging/testing)
    size_t injectSignalFromFile(const std::string& filepath);

    // Check if we have decoded data ready (for polling if not using callbacks)
    bool hasReceivedData() const;

    // Get received data (clears the buffer) - use callbacks instead when possible
    std::string getReceivedText();
    Bytes getReceivedData();

    // ========================================================================
    // STATUS & CALLBACKS
    // ========================================================================
    LoopbackStats getStats() const;
    bool isSynced() const;
    float getCurrentSNR() const;
    float getFadingIndex() const;  // Fading index from per-carrier variance (0-1)
    bool isFading() const;         // True if fading_index > 0.4
    ChannelQuality getChannelQuality() const;

    // Set known CFO for light preamble mode (for testing or external CFO source)
    void setKnownCFO(float cfo_hz);
    std::vector<std::complex<float>> getConstellationSymbols() const;

    using DataCallback = std::function<void(const std::string&)>;
    void setDataCallback(DataCallback callback) { data_callback_ = callback; }

    using RawDataCallback = std::function<void(const Bytes&)>;
    void setRawDataCallback(RawDataCallback callback) { raw_data_callback_ = callback; }

    using StatusCallback = std::function<void(const std::string&)>;
    void setStatusCallback(StatusCallback callback) { status_callback_ = callback; }

    // Ping received callback - called when "ULTR" magic detected via DPSK
    // The measured_snr is estimated from preamble energy
    using PingReceivedCallback = std::function<void(float measured_snr)>;
    void setPingReceivedCallback(PingReceivedCallback callback) { ping_received_callback_ = callback; }

    void reset();
    void clearRxBuffer();  // Clear RX audio buffer (use before TX to prevent echo)

    // ========================================================================
    // CARRIER SENSE (Listen Before Talk)
    // ========================================================================
    bool isChannelBusy() const;
    float getChannelEnergy() const;
    void setCarrierSenseThreshold(float threshold);
    float getCarrierSenseThreshold() const;

    // Half-duplex turnaround delay
    void setTurnaroundDelay(uint32_t delay_ms) { turnaround_delay_ms_ = delay_ms; }
    uint32_t getTurnaroundDelay() const { return turnaround_delay_ms_; }
    bool isTurnaroundActive() const;
    uint32_t getTurnaroundRemaining() const;

    // ========================================================================
    // WAVEFORM & MODE CONTROL
    // ========================================================================
    void setWaveformMode(protocol::WaveformMode mode);
    protocol::WaveformMode getWaveformMode() const { return waveform_mode_; }

    void setConnected(bool connected);
    bool isConnected() const { return connected_; }

    void setUseConnectedWaveformOnce() { use_connected_waveform_once_ = true; }

    void setConnectWaveform(protocol::WaveformMode mode);
    protocol::WaveformMode getConnectWaveform() const { return connect_waveform_; }

    void setLastRxWaveform(protocol::WaveformMode mode) { last_rx_waveform_ = mode; }
    protocol::WaveformMode getLastRxWaveform() const { return last_rx_waveform_; }

    void setHandshakeComplete(bool complete);
    bool isHandshakeComplete() const { return handshake_complete_; }

    void setDataMode(Modulation mod, CodeRate rate);
    Modulation getDataModulation() const { return data_modulation_; }
    CodeRate getDataCodeRate() const { return data_code_rate_; }

    // NOTE: For modulation/rate selection, use protocol::recommendDataMode()
    // from waveform_selection.hpp instead
    static protocol::WaveformMode recommendWaveformMode(float snr_db);

    // Waveform + rate recommendation based on SNR AND fading
    // This is the primary recommendation function - uses both metrics
    // Delegates to protocol::recommendWaveformAndRate() for the algorithm
    using WaveformRecommendation = protocol::WaveformRecommendation;
    static WaveformRecommendation recommendWaveformAndRate(float snr_db, float fading_index);

    void setDPSKMode(DPSKModulation mod, int samples_per_symbol = 384);
    DPSKModulation getDPSKModulation() const { return dpsk_config_.modulation; }
    const DPSKConfig& getDPSKConfig() const { return dpsk_config_; }

    // MC-DPSK configuration
    int getMCDPSKCarriers() const { return mc_dpsk_config_.num_carriers; }
    float getMCDPSKThroughput() const { return mc_dpsk_config_.getRawBitRate() * 0.25f; } // R1/4 FEC
    void setMCDPSKCarriers(int num_carriers) {
        mc_dpsk_config_.num_carriers = num_carriers;
        // Update StreamingDecoder (it creates its own waveforms internally)
        if (streaming_decoder_) {
            streaming_decoder_->setMCDPSKCarriers(num_carriers);
        }
    }

    // Recommend MC-DPSK carrier count based on channel conditions
    // Returns 8 for fading/low SNR, up to 13 for stable/high SNR
    static int recommendMCDPSKCarriers(float snr_db, float fading_index);

    // Interleaving control
    void setInterleavingEnabled(bool enabled) { interleaving_enabled_ = enabled; }
    bool isInterleavingEnabled() const { return interleaving_enabled_; }

    // FEC codec control
    void setCodecType(fec::CodecType type);
    fec::CodecType getCodecType() const { return codec_type_; }
    static fec::CodecType recommendCodecType(float snr_db);
    static fec::CodecType getCodecForWaveform(protocol::WaveformMode mode);

private:
    ModemConfig config_;
    std::string log_prefix_ = "MODEM";

    // Waveform mode state
    protocol::WaveformMode waveform_mode_ = protocol::WaveformMode::OFDM_COX;
    protocol::WaveformMode connect_waveform_ = protocol::WaveformMode::MC_DPSK;
    protocol::WaveformMode last_rx_waveform_ = protocol::WaveformMode::MC_DPSK;
    protocol::WaveformMode disconnect_waveform_ = protocol::WaveformMode::MC_DPSK;  // Saved for ACK
    bool connected_ = false;
    bool handshake_complete_ = false;
    bool use_connected_waveform_once_ = false;

    // Data frame modulation (negotiated after probing)
    Modulation data_modulation_ = Modulation::QPSK;
    CodeRate data_code_rate_ = CodeRate::R1_2;
    fec::CodecType codec_type_ = fec::CodecType::LDPC;  // FEC codec type

    // TX chain - OFDM
    fec::CodecPtr encoder_;  // ICodec for encoding (currently LDPC)
    std::unique_ptr<OFDMModulator> ofdm_modulator_;

    // TX chain - OTFS
    std::unique_ptr<OTFSModulator> otfs_modulator_;
    OTFSConfig otfs_config_;

    // RX chain - OFDM
    fec::CodecPtr decoder_;  // ICodec for decoding (currently LDPC) - mostly unused, StreamingDecoder handles RX
    std::unique_ptr<OFDMDemodulator> ofdm_demodulator_;

    // RX chain - OTFS
    std::unique_ptr<OTFSDemodulator> otfs_demodulator_;

    // DPSK demodulator (single carrier, for very low SNR mode switching)
    std::unique_ptr<DPSKDemodulator> dpsk_demodulator_;
    DPSKConfig dpsk_config_;

    // Multi-Carrier DPSK config (actual modulation done by IWaveform via StreamingDecoder)
    MultiCarrierDPSKConfig mc_dpsk_config_;

    // Chirp sync for robust presence detection on fading channels
    std::unique_ptr<sync::ChirpSync> chirp_sync_;

    // IWaveform abstraction for TX (gradually transitioning from direct modulators)
    // This will eventually replace the individual modulator pointers
    std::unique_ptr<IWaveform> active_tx_waveform_;

    // ========================================================================
    // NEW: IWaveform + RxPipeline for Connected Mode RX
    // ========================================================================
    // These replace the per-waveform processRxBuffer_* methods
    // Each waveform type has its own IWaveform instance
    std::unique_ptr<IWaveform> rx_waveform_ofdm_;      // OFDM_COX (Schmidl-Cox)
    std::unique_ptr<IWaveform> rx_waveform_ofdm_chirp_;// OFDM_CHIRP (Chirp + OFDM)
    std::unique_ptr<IWaveform> rx_waveform_mc_dpsk_;   // MC-DPSK (Chirp + DPSK)

    // Current active waveform for connected mode RX
    IWaveform* active_rx_waveform_ = nullptr;

    // ========================================================================
    // NEW: StreamingDecoder (replaces RxPipeline + acquisition thread)
    // ========================================================================
    // StreamingDecoder handles BOTH connected and disconnected modes:
    // - Circular buffer with bounded size
    // - Sliding window chirp detection
    // - Correct IWaveform call sequence (fixes BUG-002)
    // - Thread-safe with condition variable
    std::unique_ptr<StreamingDecoder> streaming_decoder_;

    // Helper to switch waveform when mode changes
    void switchRxWaveform(protocol::WaveformMode mode);

    // ========================================================================
    // RX ARCHITECTURE
    // ========================================================================

    // RX/Decode thread
    std::thread rx_decode_thread_;
    std::atomic<bool> rx_decode_running_{false};
    std::condition_variable rx_decode_cv_;
    std::mutex rx_decode_mutex_;

    void rxDecodeLoop();
    void startRxDecodeThread();
    void stopRxDecodeThread();

    // RX decode helpers (implemented in modem_rx_decode.cpp)
    void deliverFrame(const Bytes& frame_data);
    void notifyFrameParsed(const Bytes& frame_data, protocol::v2::FrameType frame_type);
    void updateStats(std::function<void(LoopbackStats&)> updater);

    // Buffer limit
    static constexpr size_t MAX_PENDING_SAMPLES = 960000;
    std::queue<Bytes> rx_data_queue_;
    mutable std::mutex rx_mutex_;

    // Statistics
    LoopbackStats stats_;
    mutable std::mutex stats_mutex_;

    // Callbacks
    DataCallback data_callback_;
    RawDataCallback raw_data_callback_;
    StatusCallback status_callback_;
    PingReceivedCallback ping_received_callback_;

    // Adaptive modulation controller
    AdaptiveModeController adaptive_;

    // Channel interleaver for time-frequency diversity on fading channels
    // Spreads consecutive LDPC bits across different OFDM symbols
    std::unique_ptr<ChannelInterleaver> channel_interleaver_;
    size_t interleaver_bits_per_symbol_ = 60;  // Default for OFDM_CHIRP (30 carriers × 2 bits DQPSK)
    bool interleaving_enabled_ = true;
    void updateChannelInterleaver(size_t bits_per_symbol);

    // Audio filters
    FilterConfig filter_config_;
    std::unique_ptr<FIRFilter> tx_filter_;
    std::unique_ptr<FIRFilter> rx_filter_;

    // Carrier sense
    std::atomic<float> channel_energy_{0.0f};
    float carrier_sense_threshold_ = 0.02f;
    static constexpr float ENERGY_SMOOTHING = 0.3f;

    // Half-duplex turnaround
    std::chrono::steady_clock::time_point last_rx_complete_time_;
    uint32_t turnaround_delay_ms_ = 200;

    // Estimated CFO from peer (detected during connection, persists for connected mode)
    float peer_cfo_hz_ = 0.0f;

    // Helper methods
    void rebuildFilters();
    void updateChannelEnergy(const std::vector<float>& samples);

    // IWaveform TX helper - ensures active_tx_waveform_ is configured for the given mode
    void ensureTxWaveform(protocol::WaveformMode mode, Modulation mod, CodeRate rate);
};

} // namespace gui
} // namespace ultra
