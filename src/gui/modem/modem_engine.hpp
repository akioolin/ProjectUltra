#pragma once

// ModemEngine - Main modem interface class
// Handles TX/RX audio processing with OFDM and DPSK modulation

#include "modem_types.hpp"
#include "ultra/types.hpp"
#include "ultra/ofdm.hpp"
#include "ultra/fec.hpp"  // Interleaver, ChannelInterleaver
#include "fec/codec_factory.hpp"  // ICodec, CodecFactory
#include "ultra/dsp.hpp"  // FIRFilter
#include "psk/dpsk.hpp"   // DPSKModulator, DPSKDemodulator
#include "psk/multi_carrier_dpsk.hpp"  // MultiCarrierDPSK for fading channels
#include "sync/chirp_sync.hpp"  // ChirpSync for robust fading channel detection
#include "../adaptive_mode.hpp"
#include "fec/soft_combine.hpp"
#include "protocol/frame_v2.hpp"  // v2::FrameType
#include "protocol/waveform_selection.hpp"  // WaveformRecommendation, recommendWaveformAndRate
#include "waveform/waveform_interface.hpp"  // IWaveform abstraction
#include "audio/channel_busy_detector.hpp"  // shared carrier-sense (listen-before-talk)
#include "streaming_decoder.hpp"  // StreamingDecoder - primary decoder
#include "streaming_encoder.hpp"  // StreamingEncoder - unified TX encoder
#include <memory>
#include <vector>
#include <queue>
#include <mutex>
#include <functional>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>

namespace ultra {
namespace gui {

// Real modem engine using OFDM modulator/demodulator and LDPC codec
class ModemEngine {
public:
    ModemEngine();
    explicit ModemEngine(const MultiCarrierDPSKConfig& mc_dpsk_config);
    ~ModemEngine();

    // Set a name/prefix for logging (e.g., "OUR" or "SIM")
    void setLogPrefix(const std::string& prefix);
    const std::string& getLogPrefix() const { return log_prefix_; }
    void setSoftCombineBuffer(fec::SoftCombineBuffer* buffer);
    void setHarqProvisionalContextCallback(
        StreamingDecoder::HarqProvisionalContextCallback cb);

    // ========================================================================
    // CONFIGURATION
    // ========================================================================
    void setConfig(const ModemConfig& config);
    const ModemConfig& getConfig() const { return config_; }

    void setFilterConfig(const FilterConfig& config);
    const FilterConfig& getFilterConfig() const { return filter_config_; }
    void setFilterEnabled(bool enabled);
    bool isFilterEnabled() const { return filter_config_.enabled; }
    void setPaprReductionEnabled(bool enabled);
    bool isPaprReductionEnabled() const;

    // ========================================================================
    // TX: Convert data to audio samples
    // ========================================================================
    std::vector<float> transmit(const std::string& text);
    std::vector<float> transmit(const Bytes& data);

    // Transmit multiple frames as a single waveform burst.
    // Used for connected OFDM and MC-DPSK DATA windows.
    std::vector<float> transmitBurst(const std::vector<Bytes>& frame_data_list,
                                     uint16_t group_seq = 0);

    // §16.4 escalation: latch a full chirp+LTS group-start anchor for the next
    // burst (consumed once by the encoder). Called on RESENDS so a fade-hit
    // group re-acquires deterministically even under warm-sync. No-op if the
    // encoder is absent.
    void forceNextBurstFullPreamble() {
        // Use the group-start-only latch so the BURST_HEADER descriptor's
        // encodeFrame does not consume it before the group-start loop reads it.
        if (streaming_encoder_) streaming_encoder_->forceNextBurstGroupStartFullPreamble();
    }

    // Minimal ping/pong probe (fast presence check, ~1 sec vs ~16 sec CONNECT)
    // Returns: preamble + raw DPSK "ULTR" bytes (no LDPC encoding)
    std::vector<float> transmitPing();
    std::vector<float> transmitPong();  // Same as ping, context determines meaning

    // §15 step 4d-iii: encode a tone-burst ACK to raw audio samples (4-FSK
    // in the 2400-2700 Hz subband). Delegates to StreamingEncoder::
    // encodeToneBurstAck (step 4c). The caller queues the returned samples
    // on the audio output ring just like any other transmit*() result.
    std::vector<float> transmitToneBurstAck(
        const ultra::waveform::tone_burst_ack::ToneBurstAckPayload& payload,
        uint32_t symbol_ms =
            ultra::waveform::tone_burst_ack::kBaselineSymbolMs);

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

    // Synchronous mode: caller drives decode instead of internal thread.
    // Use for simulation where feed+decode must be lockstep (no overflows).
    void setSynchronousMode(bool enabled);
    bool isSynchronousMode() const { return synchronous_mode_; }

    // Process RX buffer synchronously (call after feedAudio in sync mode).
    // Does one round of search/decode, same as the internal decode thread would.
    void processRxBuffer();

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
    DecoderStats getDecoderStats() const;
    bool isSynced() const;
    float getCurrentSNR() const;
    float getFadingIndex() const;  // Fading index from per-carrier variance (0-1)
    bool isFading() const;         // True if fading_index > 0.65
    ChannelQuality getChannelQuality() const;

    // Set known CFO for light preamble mode (for testing or external CFO source)
    void setKnownCFO(float cfo_hz);
    std::vector<std::complex<float>> getConstellationSymbols() const;
    Modulation getConstellationModulation() const;

    using DataCallback = std::function<void(const std::string&)>;
    void setDataCallback(DataCallback callback) { data_callback_ = callback; }

    using RawDataCallback = std::function<void(const Bytes&)>;
    void setRawDataCallback(RawDataCallback callback) { raw_data_callback_ = callback; }

    // §14.27 one-way burst transport: a decoded interleaved burst delivered as a
    // unit (group_seq, ordered DATA frames, all-logical-frames-decoded flag).
    using BurstGroupCallback =
        std::function<void(uint16_t group_seq, const std::vector<Bytes>& frames, bool all_ok,
                           float quality)>;
    void setBurstGroupCallback(BurstGroupCallback callback) {
        burst_group_callback_ = std::move(callback);
    }
    void setBurstTransportRxEnabled(bool enabled) {
        burst_transport_rx_enabled_ = enabled;
        if (streaming_decoder_) streaming_decoder_->setBurstTransportRxEnabled(enabled);
    }

    using DataSyncAcceptedCallback = std::function<void(float sync_correlation)>;
    void setDataSyncAcceptedCallback(DataSyncAcceptedCallback callback) {
        data_sync_accepted_callback_ = callback;
    }

    using StatusCallback = std::function<void(const std::string&)>;
    void setStatusCallback(StatusCallback callback) { status_callback_ = callback; }

    // Ping received callback - called when "ULTR" magic detected via DPSK
    // The measured_snr is estimated from preamble energy
    using PingReceivedCallback = std::function<void(float measured_snr)>;
    void setPingReceivedCallback(PingReceivedCallback callback) { ping_received_callback_ = callback; }

    // §15 step 4d-i: Tone-burst ACK detection callback. Delegates to the
    // StreamingDecoder's always-on tone-burst ACK monitor (installed in
    // step 4b). The owner of ModemEngine (typically the GUI / ProtocolEngine)
    // installs this to bridge tone-burst detections to the protocol layer
    // — step 4d-ii will translate the detection into a synthetic GROUP_ACK
    // for Connection's ACK-arrived handler.
    using ToneBurstAckCallback =
        ultra::waveform::tone_burst_ack::ToneBurstAckCallback;
    void setToneBurstAckCallback(ToneBurstAckCallback callback) {
        if (streaming_decoder_) {
            streaming_decoder_->setToneBurstAckCallback(std::move(callback));
        }
    }

    // §15 step 4d-late: arm the receiver's tone-burst ACK monitor for an
    // expected ACK arrival. Connection calls this right after queueing a
    // data burst; the monitor runs detection at a tight cadence until a
    // successful decode fires or the window elapses.
    void armToneBurstAckMonitor(uint32_t window_ms) {
        if (streaming_decoder_) {
            streaming_decoder_->armToneBurstMonitor(window_ms);
        }
    }

    // Check if last detected chirp was narrowband (valid after ping callback)
    bool isNarrowbandDetected() const;

    // Switch encoder to narrowband control chirps (for initiator forcing OFDM_NARROW)
    void setNarrowbandControl(bool narrowband);

    void reset();
    void clearRxBuffer();  // Clear RX audio buffer (use before TX to prevent echo)

    // Carrier sense (listen-before-talk) lives in the shared ChannelBusyDetector
    // (src/audio/channel_busy_detector.cpp), reached by simulator/TNC stations via
    // AudioPort::isChannelIdleFor(). The old fixed-threshold ModemEngine energy
    // detector was an unwired dead stub and has been removed.

    // Half-duplex turnaround delay
    void setTurnaroundDelay(uint32_t delay_ms) { turnaround_delay_ms_ = delay_ms; }
    uint32_t getTurnaroundDelay() const { return turnaround_delay_ms_; }
    bool isTurnaroundActive() const;
    uint32_t getTurnaroundRemaining() const;

    // Carrier sense (listen-before-talk). Energy detection is only meaningful in
    // the OFDM regime: at MC-DPSK SNRs the peer sits at/below the noise floor and
    // is not energy-detectable, so CCA is skipped there (stop-and-wait turnaround
    // coordinates instead). Gating to OFDM also matches where the half-duplex
    // collision actually occurs (multi-frame OFDM bursts).
    bool carrierSenseActiveForTx() const {
        return protocol::isOFDMMode(waveform_mode_);
    }
    bool channelIdleForTx(std::chrono::milliseconds guard = std::chrono::milliseconds(0)) const {
        return !carrierSenseActiveForTx() || channel_busy_detector_.isIdleFor(guard);
    }
    bool channelBusyForTx() const {
        return !channelIdleForTx();
    }
    float channelRms() const { return channel_busy_detector_.currentRms(); }
    float channelQuietThreshold() const { return channel_busy_detector_.quietThreshold(); }

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
    void setAdaptivePreamblePeerFading(float peer_fading_index);

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

    // §14.36 live decoder state for the GUI's RX panel. Reflects the descriptor-
    // driven rate switch from adaptive bursts (not just the negotiated rate).
    CodeRate getDecoderCodeRate() const {
        return streaming_decoder_ ? streaming_decoder_->getCodeRate() : CodeRate::AUTO;
    }
    Modulation getDecoderModulation() const {
        return streaming_decoder_ ? streaming_decoder_->getModulation() : Modulation::DQPSK;
    }
    float getMCDPSKThroughput() const { return mc_dpsk_config_.getRawBitRate() * 0.25f; } // R1/4 FEC
    void setMCDPSKConfig(const MultiCarrierDPSKConfig& config);
    void setMCDPSKProfile(int num_carriers, int samples_per_symbol, int bits_per_symbol) {
        if (mc_dpsk_config_.num_carriers == num_carriers &&
            mc_dpsk_config_.samples_per_symbol == samples_per_symbol &&
            mc_dpsk_config_.bits_per_symbol == bits_per_symbol) {
            return;
        }
        mc_dpsk_config_.num_carriers = num_carriers;
        mc_dpsk_config_.samples_per_symbol = samples_per_symbol;
        mc_dpsk_config_.bits_per_symbol = bits_per_symbol;
        setMCDPSKConfig(mc_dpsk_config_);
    }
    void setMCDPSKCarriers(int num_carriers) {
        mc_dpsk_config_.num_carriers = num_carriers;
        setMCDPSKConfig(mc_dpsk_config_);
    }

    // Recommend MC-DPSK carrier count based on channel conditions
    // Returns 8 for fading/low SNR, up to 13 for stable/high SNR
    static int recommendMCDPSKCarriers(float snr_db, float fading_index);

    // Burst interleave control (for connected OFDM_CHIRP mode)
    void setBurstInterleave(bool enable) {
        if (streaming_encoder_) streaming_encoder_->setBurstInterleave(enable);
        if (streaming_decoder_) streaming_decoder_->setBurstInterleave(enable);
    }

    // Force fixed-frame codeword count on encoder + decoder. Used by
    // the GUI's monitor-mode override so the decoder doesn't default
    // to 1-CW control-frame demod on incoming OFDM data frames.
    void setFixedFrameCodewords(int cw_count) {
        if (streaming_encoder_) streaming_encoder_->setFixedFrameCodewords(cw_count);
        if (streaming_decoder_) streaming_decoder_->setFixedFrameCodewords(cw_count);
    }
    void expectFullOFDMAnchorOnce() {
        if (streaming_decoder_) streaming_decoder_->expectFullOFDMAnchorOnce();
    }
    void clearFullOFDMAnchorExpectation() {
        if (streaming_decoder_) streaming_decoder_->clearFullOFDMAnchorExpectation();
    }
    void setFixedFrameHeaderDiscovery(bool enable) {
        if (streaming_decoder_) streaming_decoder_->setFixedFrameHeaderDiscovery(enable);
    }
    void setCarrierLdpcInterleaver(bool enable) {
        if (streaming_encoder_) streaming_encoder_->setCarrierLdpcInterleaver(enable);
        if (streaming_decoder_) streaming_decoder_->setCarrierLdpcInterleaver(enable);
    }

    // FEC codec control
    void setCodecType(fec::CodecType type);
    fec::CodecType getCodecType() const { return codec_type_; }
    static fec::CodecType recommendCodecType(float snr_db);
    static fec::CodecType getCodecForWaveform(protocol::WaveformMode mode);

private:
    ModemConfig config_;
    std::string log_prefix_ = "MODEM";

    // Waveform mode state
    protocol::WaveformMode waveform_mode_ = protocol::WaveformMode::OFDM_CHIRP;
    protocol::WaveformMode connect_waveform_ = protocol::WaveformMode::MC_DPSK;
    protocol::WaveformMode last_rx_waveform_ = protocol::WaveformMode::MC_DPSK;
    protocol::WaveformMode disconnect_waveform_ = protocol::WaveformMode::MC_DPSK;  // Saved for ACK
    bool connected_ = false;
    bool handshake_complete_ = false;
    bool use_connected_waveform_once_ = false;
    float adaptive_preamble_peer_fading_ = -1.0f;
    bool adaptive_short_reanchor_active_ = false;

    // Data frame modulation (negotiated after probing)
    Modulation data_modulation_ = Modulation::DBPSK;
    CodeRate data_code_rate_ = CodeRate::R1_4;
    fec::CodecType codec_type_ = fec::CodecType::LDPC;  // FEC codec type

    // TX chain - StreamingEncoder (unified encoding for all waveform types)
    std::unique_ptr<StreamingEncoder> streaming_encoder_;

    // RX chain - OFDM
    fec::CodecPtr decoder_;  // ICodec for decoding (currently LDPC) - mostly unused, StreamingDecoder handles RX
    // NOTE: RX uses streaming_decoder_ (which has its own waveform) - no separate demodulator

    // DPSK config (used by StreamingDecoder, kept here for API compatibility)
    DPSKConfig dpsk_config_;

    // Multi-Carrier DPSK config (actual modulation done by IWaveform via StreamingDecoder)
    MultiCarrierDPSKConfig mc_dpsk_config_;

    // Chirp sync for robust presence detection on fading channels
    std::unique_ptr<sync::ChirpSync> chirp_sync_;

    // ========================================================================
    // StreamingDecoder (primary RX path)
    // ========================================================================
    // StreamingDecoder handles BOTH connected and disconnected modes:
    // - Circular buffer with bounded size
    // - Sliding window chirp detection
    // - Correct IWaveform call sequence (fixes BUG-002)
    // - Thread-safe with condition variable
    std::unique_ptr<StreamingDecoder> streaming_decoder_;

    // ========================================================================
    // RX ARCHITECTURE
    // ========================================================================

    // RX/Decode thread (disabled in synchronous mode)
    bool synchronous_mode_ = false;
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
    void syncAdaptiveShortDataPreamble();

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
    BurstGroupCallback burst_group_callback_;
    bool burst_transport_rx_enabled_ = false;
    DataSyncAcceptedCallback data_sync_accepted_callback_;
    StatusCallback status_callback_;
    PingReceivedCallback ping_received_callback_;

    // Adaptive modulation controller
    AdaptiveModeController adaptive_;

    // Audio filters
    FilterConfig filter_config_;
    std::unique_ptr<FIRFilter> tx_filter_;
    std::unique_ptr<FIRFilter> rx_filter_;

    // Half-duplex turnaround
    std::chrono::steady_clock::time_point last_rx_complete_time_;
    uint32_t turnaround_delay_ms_ = 200;

    // Carrier sense (listen-before-talk): the shared adaptive detector, fed RX
    // audio in feedAudio(). Same component the simulator/TNC stations use.
    //
    // RATIOMETRIC, level-independent calibration (no absolute RMS constants, so
    // it works at any HF noise level / AF-gain setting):
    //   - MEDIAN floor (percentile 0.50): tracks the typical noise, robust to
    //     the bursty excursions of real HF and to a transient signal (minority
    //     of a long window).
    //   - busy threshold = floor x 2.0 (+6 dB): dimensionless; rides over noise
    //     peaks and trips on any occupant >= ~6 dB above the floor.
    //   - signal kept out of the floor by the detector's RATIOMETRIC admission
    //     gate (samples > floor x multiplier are not learned as "noise") — no
    //     absolute ceiling needed, so estimate_ceiling stays disabled (0).
    //   - bootstrap admits any startup level (ceiling 2.0 ~ full scale):
    //     assume-idle-at-startup / listen-before-talk, so the floor seeds to
    //     whatever the real noise is regardless of absolute level.
    // Validated live (real_hf_loop OTASim bed, 2026-05-23) at multiple noise
    // levels; see docs/tools probe. Energy CCA is only consulted in OFDM mode
    // (carrierSenseActiveForTx()) — at MC-DPSK SNRs the peer is at/below the
    // noise floor and is not energy-detectable.
    ultra::audio::ChannelBusyDetector channel_busy_detector_{
        ultra::audio::ratiometricHfCarrierSenseConfig()};
    uint64_t cca_samples_since_log_ = 0;

    // Estimated CFO from peer (detected during connection, persists for connected mode)
    float peer_cfo_hz_ = 0.0f;

    // Helper methods
    void rebuildFilters();

    // Post-process TX samples (lead-in, filter, scale, stats)
    std::vector<float> postProcessTx(const std::vector<float>& samples);
};

} // namespace gui
} // namespace ultra
