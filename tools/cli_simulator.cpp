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
#include <queue>
#include <cmath>
#include <functional>

#include "waveform/waveform_factory.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
#include "waveform/ofdm_cox_waveform.hpp"
#include "waveform/mc_dpsk_waveform.hpp"
#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"  // TX encoding (mirrors StreamingDecoder)
#include "protocol/protocol_engine.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/logging.hpp"
#include "ultra/fec.hpp"  // ChannelInterleaver, LDPCEncoder
#include "fec/frame_interleaver.hpp"  // FrameInterleaver
#include "sim/hf_channel.hpp"

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

/**
 * SimulatedChannel - The "air" between two stations
 *
 * Handles AWGN noise and optional fading. Each direction (A->B, B->A)
 * has its own buffer that accumulates samples.
 */
class SimulatedChannel {
public:
    void setSeed(uint32_t seed) { seed_ = seed; }

    void configure(float snr_db, ChannelType channel_type = ChannelType::AWGN) {
        snr_db_ = snr_db;
        float snr_linear = std::pow(10.0f, snr_db / 10.0f);
        float signal_power = 0.01f;
        noise_stddev_ = std::sqrt(signal_power / snr_linear);
        rng_.seed(seed_);

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
            cfg.cfo_hz = 0.0f;
            channel_a_to_b_ = std::make_unique<WattersonChannel>(cfg, seed_);
            channel_b_to_a_ = std::make_unique<WattersonChannel>(cfg, seed_ + 1);
        }
    }

    // Station A transmits -> goes to B's RX buffer
    void transmitFromA(const std::vector<float>& samples) {
        auto processed = applyChannel(samples, channel_a_to_b_.get());
        std::lock_guard<std::mutex> lock(mutex_b_rx_);
        for (float s : processed) {
            buffer_b_rx_.push(s);
        }
    }

    // Station B transmits -> goes to A's RX buffer
    void transmitFromB(const std::vector<float>& samples) {
        auto processed = applyChannel(samples, channel_b_to_a_.get());
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
        return result;
    }

private:
    float snr_db_ = 20.0f;
    float noise_stddev_ = 0.01f;
    uint32_t seed_ = 42;
    std::mt19937 rng_{42};
    std::normal_distribution<float> noise_dist_{0.0f, 1.0f};

    std::unique_ptr<WattersonChannel> channel_a_to_b_;
    std::unique_ptr<WattersonChannel> channel_b_to_a_;

    std::mutex mutex_a_rx_, mutex_b_rx_;
    std::queue<float> buffer_a_rx_, buffer_b_rx_;

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

    SimulatedStation(const std::string& callsign, SimulatedChannel& channel, bool is_station_a)
        : callsign_(callsign), channel_(channel), is_station_a_(is_station_a) {

        protocol_.setLocalCallsign(callsign);
        protocol_.setAutoAccept(true);

        // Initialize with default OFDM config (NVIS mode)
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
    }

    // Protocol interface
    void connect(const std::string& remote) { protocol_.connect(remote); }
    void disconnect() { protocol_.disconnect(); }
    void sendMessage(const std::string& msg) { protocol_.sendMessage(msg); }
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

    void setSNR(float snr) { snr_db_ = snr; }
    void setForcedModulation(Modulation mod) { protocol_.setForcedModulation(mod); }
    void setForcedCodeRate(CodeRate rate) { protocol_.setForcedCodeRate(rate); }
    void setPreferredWaveform(WaveformMode mode) { protocol_.setPreferredMode(mode); }

    // Disable burst interleaving (for A/B testing)
    void setNoBurstInterleave(bool v) { no_burst_interleave_ = v; }

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
        protocol_.tick(CALLBACK_INTERVAL_MS);
    }

    float getSimTime() const { return total_samples_ / (float)SAMPLE_RATE; }

    // Stats accessors
    ConnectionStats getConnectionStats() const { return protocol_.getStats(); }
    DecoderStats getDecoderStats() const { return decoder_ ? decoder_->getStats() : DecoderStats{}; }
    std::string getCallsign() const { return callsign_; }

private:
    std::string callsign_;
    SimulatedChannel& channel_;
    bool is_station_a_;

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

    ModemConfig createOFDMConfig() {
        ModemConfig cfg;
        cfg.fft_size = 1024;       // NVIS mode
        cfg.num_carriers = 59;     // NVIS mode
        cfg.sample_rate = SAMPLE_RATE;
        cfg.center_freq = 1500.0f;
        cfg.cp_mode = CyclicPrefixMode::LONG;
        cfg.modulation = Modulation::DQPSK;
        cfg.code_rate = CodeRate::R1_4;
        // Pilots for R1/4 (6 pilots, spacing 10)
        cfg.use_pilots = true;
        cfg.pilot_spacing = 10;
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
        decoder_->setMCDPSKCarriers(8);

        // Set frame callback
        decoder_->setFrameCallback([this](const DecodeResult& result) {
            handleDecodedFrame(result);
        });

        // Set ping callback
        decoder_->setPingCallback([this](float snr_db, float cfo_hz) {
            last_cfo_hz_ = cfo_hz;
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
            float fading_index = decoder_ ? decoder_->getLastFadingIndex() : 0.0f;
            protocol_.setChannelQuality(snr_db_, fading_index);
            protocol_.onRxData(result.frame_data);
        }
    }

    void setWaveformMode(WaveformMode mode) {
        if (tx_waveform_mode_ == mode) return;

        tx_waveform_mode_ = mode;
        createEncoder();

        LOG_MODEM(INFO, "[%s] Switched to waveform: %s",
                  callsign_.c_str(), waveformModeToString(mode));

        // Verify TX/RX configs match
        verifyTxRxConfig();
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

        // Update OFDM config with pilots based on code rate
        ofdm_config_.modulation = mod;
        ofdm_config_.code_rate = rate;

        // Configure pilots based on code rate (matching OFDMChirpWaveform)
        switch (rate) {
            case CodeRate::R3_4:
                ofdm_config_.use_pilots = true;
                ofdm_config_.pilot_spacing = 15;  // ~4 pilots
                break;
            case CodeRate::R2_3:
            case CodeRate::R1_2:
            case CodeRate::R1_4:
            default:
                ofdm_config_.use_pilots = true;
                ofdm_config_.pilot_spacing = 10;  // ~6 pilots
                break;
        }

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

        if (connected) {
            // Switch to negotiated waveform now
            if (negotiated_waveform_ != WaveformMode::MC_DPSK) {
                setWaveformMode(negotiated_waveform_);
                if (decoder_) {
                    decoder_->setMode(negotiated_waveform_, true);
                    decoder_->setOFDMConfig(ofdm_config_);
                    decoder_->setDataMode(data_modulation_, data_code_rate_);
                    decoder_->setKnownCFO(last_cfo_hz_);
                }
                // Enable burst interleaving for OFDM_CHIRP (not COX — no LTS marker)
                if (negotiated_waveform_ == WaveformMode::OFDM_CHIRP && !no_burst_interleave_) {
                    if (encoder_) encoder_->setBurstInterleave(true);
                    if (decoder_) decoder_->setBurstInterleave(true);
                    LOG_MODEM(INFO, "[%s] Burst interleaving ENABLED", callsign_.c_str());
                }
                LOG_MODEM(INFO, "[%s] Entered CONNECTED state, switched to %s, CFO=%.1f Hz",
                          callsign_.c_str(), waveformModeToString(negotiated_waveform_), last_cfo_hz_);
            } else {
                // MC-DPSK: Still need to update decoder's connected state and CFO
                if (decoder_) {
                    decoder_->setMode(WaveformMode::MC_DPSK, true);  // true = connected
                    decoder_->setDataMode(data_modulation_, data_code_rate_);
                    decoder_->setKnownCFO(last_cfo_hz_);
                }
                LOG_MODEM(INFO, "[%s] Entered CONNECTED state (MC-DPSK), CFO=%.1f Hz",
                          callsign_.c_str(), last_cfo_hz_);
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
        return encoder_->encodePing();
    }

    std::vector<float> transmitPong() {
        return transmitPing();
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
        protocol_.setDataModeChangedCallback([this](Modulation mod, CodeRate rate, float) {
            setDataMode(mod, rate);
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
        std::lock_guard<std::mutex> lock(tx_mutex_);
        for (float s : samples) {
            tx_queue_.push(s);
        }
    }

    // THE AUDIO LOOP - like a real sound card callback
    void audioLoop() {
        auto next_callback = std::chrono::steady_clock::now();
        int callback_count = 0;

        while (running_) {
            // ===== AUDIO CALLBACK START =====

            // 1. READ RX - get samples from channel (what the other station transmitted)
            std::vector<float> rx_samples;
            if (is_station_a_) {
                rx_samples = channel_.receiveForA(SAMPLES_PER_CALLBACK);
            } else {
                rx_samples = channel_.receiveForB(SAMPLES_PER_CALLBACK);
            }

            // 2. FEED TO DECODER (audio thread only buffers - decode thread processes)
            if (decoder_) {
                decoder_->feedAudio(rx_samples.data(), rx_samples.size());
            }

            // 3. GET TX SAMPLES - check if we have anything to transmit
            std::vector<float> tx_samples(SAMPLES_PER_CALLBACK, 0.0f);
            size_t tx_pending = 0;
            {
                std::lock_guard<std::mutex> lock(tx_mutex_);
                tx_pending = tx_queue_.size();
                for (int i = 0; i < SAMPLES_PER_CALLBACK && !tx_queue_.empty(); i++) {
                    tx_samples[i] = tx_queue_.front();
                    tx_queue_.pop();
                }
            }

            // 4. SEND TX TO CHANNEL
            if (is_station_a_) {
                channel_.transmitFromA(tx_samples);
            } else {
                channel_.transmitFromB(tx_samples);
            }

            // ===== AUDIO CALLBACK END =====

            total_samples_ += SAMPLES_PER_CALLBACK;
            callback_count++;

            // Log continuous audio status every 2 seconds (200 callbacks at 10ms each)
            if (callback_count % 200 == 0) {
                float rx_rms = 0.0f;
                for (float s : rx_samples) rx_rms += s * s;
                rx_rms = std::sqrt(rx_rms / rx_samples.size());
                printf("[%s] Audio loop: %.1fs, RX_RMS=%.4f, TX_pending=%zu\n",
                       callsign_.c_str(), getSimTime(), rx_rms, tx_pending);
            }

            // Wait for next callback (real-time pacing)
            next_callback += std::chrono::milliseconds(CALLBACK_INTERVAL_MS);
            std::this_thread::sleep_until(next_callback);
        }
    }

    // DECODE THREAD - like the real ModemEngine::rxDecodeLoop()
    // Runs independently from audio feed, just like a real sound card + decoder
    void decodeLoop() {
        while (running_) {
            if (decoder_) {
                // processBuffer() blocks until data is available (via condition variable)
                // or until stop() is called
                decoder_->processBuffer();
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
    void setPreferredWaveform(WaveformMode mode) { forced_waveform_ = mode; }
    void setTestFileTransfer(bool v) { test_file_transfer_ = v; }
    void setTestFileSize(size_t bytes) { test_file_size_ = bytes; }
    void setChannelInterleave(bool enable) { use_channel_interleave_ = enable; }
    void setNoBurstInterleave(bool v) { no_burst_interleave_ = v; }
    void setTestBurst(bool v) { test_burst_ = v; }
    void setSeed(uint32_t seed) { seed_ = seed; }

    bool runTest() {
        printHeader();

        // Setup channel
        channel_.setSeed(seed_);
        channel_.configure(snr_db_, channel_type_);

        // Create stations
        alpha_ = std::make_unique<SimulatedStation>("ALPHA", channel_, true);
        bravo_ = std::make_unique<SimulatedStation>("BRAVO", channel_, false);

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
        if (no_burst_interleave_) {
            std::cout << "  \033[33mBurst interleaving DISABLED\033[0m\n";
        }

        // Setup message callback on BRAVO
        bravo_->setMessageCallback([this](const std::string& msg) {
            std::lock_guard<std::mutex> lock(msg_mutex_);
            received_message_ = msg;
            message_received_ = true;
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
        if (test_burst_) {
            success = runBurstTest();
        } else if (test_file_transfer_) {
            success = runFileTransferTest();
        } else {
            success = runProtocolTest();
        }

        // Stop
        alpha_->stop();
        bravo_->stop();

        if (success) {
            printSummary();
        } else {
            std::cout << "\n";
            std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
            std::cout << "║                     TEST FAILED                              ║\n";
            std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
            printStationStats("ALPHA (TX)", alpha_.get());
            printStationStats("BRAVO (RX)", bravo_.get());
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
    size_t test_file_size_ = 256;  // Default 256 bytes test file
    uint32_t seed_ = 42;
    Modulation forced_mod_ = Modulation::AUTO;
    CodeRate forced_rate_ = CodeRate::AUTO;
    WaveformMode forced_waveform_ = WaveformMode::AUTO;

    SimulatedChannel channel_;
    std::unique_ptr<SimulatedStation> alpha_;
    std::unique_ptr<SimulatedStation> bravo_;

    std::mutex msg_mutex_;
    std::string received_message_;
    std::atomic<bool> message_received_{false};

    // File transfer state
    std::string received_file_path_;
    bool file_transfer_success_ = false;
    std::atomic<bool> file_received_{false};

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

        // Phase 3: Send 5 short + 2 long messages
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
        for (int msg_num = 0; msg_num < total; msg_num++) {
            const std::string& test_msg = test_messages[msg_num];

            if (!waitFor([this]{ return alpha_->isReadyToSend(); }, 30)) {
                std::cout << "  \033[31m✗ ARQ not ready for message " << (msg_num+1) << "!\033[0m\n";
                return false;
            }

            // Reset received flag before sending
            message_received_.store(false);

            std::cout << "  [" << (msg_num+1) << "/" << total << "] Sending (" << test_msg.size() << "b): \"" << test_msg << "\"\n";
            alpha_->sendMessage(test_msg);

            if (!waitFor([this]{ return message_received_.load(); }, 60)) {
                std::cout << "  \033[31m✗ Message " << (msg_num+1) << " not received!\033[0m\n";
                return false;
            }

            {
                std::lock_guard<std::mutex> lock(msg_mutex_);
                if (received_message_ == test_msg) {
                    std::cout << "  \033[32m✓ [" << (msg_num+1) << "/" << total << "] Received (" << received_message_.size() << "b): \"" << received_message_ << "\"\033[0m\n";
                } else {
                    std::cout << "  \033[31m✗ Message " << (msg_num+1) << " corrupted! Got: \"" << received_message_ << "\"\033[0m\n";
                    return false;
                }
            }
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
        std::string test_file = "/tmp/cli_sim_test_file.bin";
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

            if (elapsed >= 120) {  // 2 minute timeout for file transfer
                std::cout << "  \033[31m✗ File transfer timeout!\033[0m\n";
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

        // Verify received file
        {
            std::lock_guard<std::mutex> lock(msg_mutex_);
            if (!file_transfer_success_) {
                std::cout << "  \033[31m✗ File transfer reported failure!\033[0m\n";
                return false;
            }
            std::cout << "  \033[32m✓ File received: " << received_file_path_ << "\033[0m\n";

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
        // With 4-frame grouping: at least 1 burst-interleaved group per message
        std::cout << "\n=== PHASE 3: BURST DATA TRANSFER (3 large messages) ===\n";
        std::cout << "  Burst interleaving: " << (no_burst_interleave_ ? "DISABLED" : "ENABLED") << "\n";

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

    void printHeader() {
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║   CLI Simulator - IWaveform + StreamingDecoder               ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
        std::cout << "\n";
        std::cout << "  SNR:     " << snr_db_ << " dB\n";
        std::cout << "  Channel: " << channelTypeName() << "\n";
        std::cout << "  Model:   Real-time (48kHz, 10ms callbacks)\n";
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

        std::cout << "\n";
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
        std::cout << "  ACK:  acks_sent=" << cs.arq.acks_sent
                  << "  acks_rcvd=" << cs.arq.acks_received
                  << "  sacks_sent=" << cs.arq.sacks_sent
                  << "  sacks_rcvd=" << cs.arq.sacks_received << "\n";

        // Decoder stats
        std::cout << "  RX:   frames_decoded=" << ds.frames_decoded
                  << "  frames_failed=" << ds.frames_failed
                  << "  pings=" << ds.pings_received
                  << "  overflows=" << ds.buffer_overflows << "\n";

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
                } else {
                    std::cerr << "Unknown modulation: " << mod_str << " (use dqpsk, d8psk, dbpsk, qpsk, bpsk)\n";
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
                } else {
                    std::cerr << "Unknown code rate: " << rate_str << " (use r1_4, r1_2, r2_3, r3_4)\n";
                    return 1;
                }
            }
        } else if (arg == "--waveform" || arg == "-w") {
            if (i + 1 < argc) {
                std::string wf_str = argv[++i];
                if (wf_str == "mc_dpsk" || wf_str == "dpsk") {
                    sim.setPreferredWaveform(WaveformMode::MC_DPSK);
                } else if (wf_str == "ofdm_chirp") {
                    sim.setPreferredWaveform(WaveformMode::OFDM_CHIRP);
                } else if (wf_str == "ofdm_cox" || wf_str == "ofdm") {
                    sim.setPreferredWaveform(WaveformMode::OFDM_COX);
                } else {
                    std::cerr << "Unknown waveform: " << wf_str << " (use mc_dpsk, ofdm_chirp, ofdm_cox)\n";
                    return 1;
                }
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
        } else if (arg == "--burst-test") {
            sim.setTestBurst(true);
        } else if (arg == "--seed" && i + 1 < argc) {
            sim.setSeed(static_cast<uint32_t>(std::stoul(argv[++i])));
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
            std::cout << "  --mod, -m <MOD>     Force modulation: dqpsk, d8psk, dbpsk, qpsk, bpsk\n";
            std::cout << "  --rate, -r <RATE>   Force code rate: r1_4, r1_2, r2_3, r3_4\n";
            std::cout << "  --waveform, -w <WF> Force waveform: mc_dpsk, ofdm_chirp, ofdm_cox\n";
            std::cout << "  --seed <N>          Random seed (default: 42)\n";
            std::cout << "  --file [SIZE]       Test file transfer (default: 256 bytes)\n";
            std::cout << "  --channel-interleave, -ci  Enable channel interleaving\n";
            std::cout << "  --no-burst-interleave     Disable burst-level long interleaving\n";
            std::cout << "  --burst-test              Send large messages to test burst interleaving\n";
            std::cout << "  --verbose, -v       Verbose output\n";
            return 0;
        }
    }

    setLogLevel(LogLevel::INFO);
    return sim.runTest() ? 0 : 1;
}
