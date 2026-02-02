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
#include "waveform/mc_dpsk_waveform.hpp"
#include "gui/modem/streaming_decoder.hpp"
#include "protocol/protocol_engine.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/logging.hpp"
#include "sim/hf_channel.hpp"

using namespace ultra;
using namespace ultra::gui;
using namespace ultra::protocol;
using namespace ultra::sim;
namespace v2 = protocol::v2;

/**
 * SimulatedChannel - The "air" between two stations
 *
 * Handles AWGN noise and optional fading. Each direction (A->B, B->A)
 * has its own buffer that accumulates samples.
 */
class SimulatedChannel {
public:
    void configure(float snr_db, bool use_fading = false) {
        snr_db_ = snr_db;
        float snr_linear = std::pow(10.0f, snr_db / 10.0f);
        float signal_power = 0.01f;
        noise_stddev_ = std::sqrt(signal_power / snr_linear);

        if (use_fading) {
            auto cfg = itu_r_f1487::moderate(snr_db);
            cfg.cfo_hz = 0.0f;
            channel_a_to_b_ = std::make_unique<WattersonChannel>(cfg, 42);
            channel_b_to_a_ = std::make_unique<WattersonChannel>(cfg, 43);
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

        // Create TX waveform and RX decoder
        createWaveform();
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
    }

    void stop() {
        running_ = false;
        if (decoder_) decoder_->stop();
        if (audio_thread_.joinable()) {
            audio_thread_.join();
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

    void tick() {
        protocol_.tick(CALLBACK_INTERVAL_MS);
    }

    float getSimTime() const { return total_samples_ / (float)SAMPLE_RATE; }

private:
    std::string callsign_;
    SimulatedChannel& channel_;
    bool is_station_a_;

    // TX: IWaveform directly
    std::unique_ptr<IWaveform> tx_waveform_;
    std::unique_ptr<IWaveform> control_waveform_;  // Always MC-DPSK for CONNECT/CONNECT_ACK
    WaveformMode tx_waveform_mode_ = WaveformMode::MC_DPSK;  // Start with DPSK for PING/CONNECT
    WaveformMode negotiated_waveform_ = WaveformMode::MC_DPSK;  // Store negotiated mode, switch after handshake

    // RX: StreamingDecoder directly
    std::unique_ptr<StreamingDecoder> decoder_;

    // OFDM configuration (shared between TX and RX)
    ModemConfig ofdm_config_;
    Modulation data_modulation_ = Modulation::DQPSK;
    CodeRate data_code_rate_ = CodeRate::R1_4;

    // Protocol engine
    ProtocolEngine protocol_{ConnectionConfig{}};

    std::atomic<bool> running_{false};
    std::thread audio_thread_;

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

    void createWaveform() {
        // Always have an MC-DPSK waveform ready for control frames
        if (!control_waveform_) {
            control_waveform_ = WaveformFactory::createMCDPSK(8);
        }

        if (tx_waveform_mode_ == WaveformMode::MC_DPSK) {
            tx_waveform_ = WaveformFactory::createMCDPSK(8);  // 8 carriers for DPSK
        } else {
            // OFDM_CHIRP with proper config
            tx_waveform_ = std::make_unique<OFDMChirpWaveform>(ofdm_config_);
            static_cast<OFDMChirpWaveform*>(tx_waveform_.get())->configure(
                data_modulation_, data_code_rate_);
        }
        LOG_MODEM(INFO, "[%s] TX waveform: %s", callsign_.c_str(),
                  waveformModeToString(tx_waveform_mode_));
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
            protocol_.setChannelQuality(snr_db_, 0.0f);  // TODO: get fading from decoder
            protocol_.onRxData(result.frame_data);
        }
    }

    void setWaveformMode(WaveformMode mode) {
        if (tx_waveform_mode_ == mode) return;

        tx_waveform_mode_ = mode;
        createWaveform();

        LOG_MODEM(INFO, "[%s] Switched to waveform: %s",
                  callsign_.c_str(), waveformModeToString(mode));
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
            createWaveform();
        }

        // Update RX decoder
        if (decoder_) {
            decoder_->setOFDMConfig(ofdm_config_);
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
                LOG_MODEM(INFO, "[%s] Entered CONNECTED state, switched to %s, CFO=%.1f Hz",
                          callsign_.c_str(), waveformModeToString(negotiated_waveform_), last_cfo_hz_);
            } else {
                LOG_MODEM(INFO, "[%s] Entered CONNECTED state (MC-DPSK), CFO=%.1f Hz",
                          callsign_.c_str(), last_cfo_hz_);
            }
        } else {
            // Switch back to disconnected mode (MC_DPSK for PING detection)
            if (decoder_) {
                decoder_->setMode(WaveformMode::MC_DPSK, false);
            }
            // Reset TX waveform to MC-DPSK
            if (tx_waveform_mode_ != WaveformMode::MC_DPSK) {
                tx_waveform_mode_ = WaveformMode::MC_DPSK;
                createWaveform();
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
        if (!tx_waveform_) {
            LOG_MODEM(ERROR, "[%s] No TX waveform!", callsign_.c_str());
            return {};
        }

        // Check if this is a connection setup frame (CONNECT, CONNECT_ACK)
        // These ALWAYS use MC-DPSK. DISCONNECT uses the negotiated waveform.
        // Frame format: Magic (2B) + Type (1B) + ...
        bool is_control_frame = false;
        if (data.size() >= 3) {
            uint8_t frame_type = data[2];  // Type is at byte 2 (after 2-byte magic)
            // Only CONNECT (0x12) and CONNECT_ACK (0x13) use MC-DPSK
            is_control_frame = (frame_type == 0x12 || frame_type == 0x13);
        }

        // Encode frame data based on waveform mode
        // Control frames always use MC-DPSK encoding
        Bytes encoded;
        Bytes tx_data = data;  // Make mutable copy for header patching

        bool use_mcdpsk = (tx_waveform_mode_ == WaveformMode::MC_DPSK) || is_control_frame;

        if (use_mcdpsk) {
            // MC-DPSK: variable CW encoding (single or multiple CWs)
            auto cws = v2::encodeFrameWithLDPC(tx_data, data_code_rate_);

            // Patch total_cw in header to match actual CW count (like ModemEngine)
            uint8_t actual_cw = static_cast<uint8_t>(cws.size());
            if (tx_data.size() >= 17 && tx_data[12] != actual_cw) {
                LOG_MODEM(DEBUG, "[%s] Patching total_cw from %d to %d",
                          callsign_.c_str(), tx_data[12], actual_cw);
                tx_data[12] = actual_cw;
                // Recalculate header CRC (bytes 0-14)
                uint16_t hcrc = v2::ControlFrame::calculateCRC(tx_data.data(), 15);
                tx_data[15] = (hcrc >> 8) & 0xFF;
                tx_data[16] = hcrc & 0xFF;
                // Re-encode with corrected header
                cws = v2::encodeFrameWithLDPC(tx_data, data_code_rate_);
            }

            for (const auto& cw : cws) {
                encoded.insert(encoded.end(), cw.begin(), cw.end());
            }
            LOG_MODEM(INFO, "[%s] TX MC-DPSK: %zu bytes -> %zu CWs (%zu coded bytes)",
                      callsign_.c_str(), tx_data.size(), cws.size(), encoded.size());
        } else {
            // OFDM: 4-CW encoding with frame interleaving
            encoded = v2::encodeFixedFrame(tx_data, data_code_rate_);
            LOG_MODEM(INFO, "[%s] TX OFDM: %zu bytes -> 4 CWs (%zu coded bytes)",
                      callsign_.c_str(), tx_data.size(), encoded.size());
        }

        // Select waveform for preamble/modulation
        // Control frames always use MC-DPSK waveform
        IWaveform* waveform = is_control_frame ? control_waveform_.get() : tx_waveform_.get();

        // Generate preamble based on connection state
        // Control frames always use full preamble (chirp sync)
        Samples preamble;
        bool use_light = !is_control_frame && connected_.load() && handshake_complete_.load() &&
                         waveform->supportsDataPreamble();

        if (use_light) {
            preamble = waveform->generateDataPreamble();
            LOG_MODEM(DEBUG, "[%s] TX: Light preamble (%zu samples)",
                      callsign_.c_str(), preamble.size());
        } else {
            preamble = waveform->generatePreamble();
            LOG_MODEM(DEBUG, "[%s] TX: Full preamble (%zu samples)",
                      callsign_.c_str(), preamble.size());
        }

        // Modulate
        Samples modulated = waveform->modulate(encoded);

        // Combine preamble + data
        std::vector<float> result;
        result.reserve(preamble.size() + modulated.size());
        result.insert(result.end(), preamble.begin(), preamble.end());
        result.insert(result.end(), modulated.begin(), modulated.end());

        return result;
    }

    std::vector<float> transmitPing() {
        // PING is just chirp preamble with no data
        if (!tx_waveform_) return {};

        // Ensure we're using MC_DPSK waveform for PING
        if (tx_waveform_mode_ != WaveformMode::MC_DPSK) {
            auto saved_mode = tx_waveform_mode_;
            tx_waveform_mode_ = WaveformMode::MC_DPSK;
            createWaveform();
            auto preamble = tx_waveform_->generatePreamble();
            tx_waveform_mode_ = saved_mode;
            createWaveform();
            return std::vector<float>(preamble.begin(), preamble.end());
        }

        auto preamble = tx_waveform_->generatePreamble();
        return std::vector<float>(preamble.begin(), preamble.end());
    }

    std::vector<float> transmitPong() {
        // PONG is same as PING
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

            // 2. FEED TO DECODER (StreamingDecoder directly)
            if (decoder_) {
                decoder_->feedAudio(rx_samples.data(), rx_samples.size());
                decoder_->processBuffer();
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
};

// =============================================================================
// MAIN SIMULATOR
// =============================================================================

class CLISimulator {
public:
    void setSNR(float snr) { snr_db_ = snr; }
    void setVerbose(bool v) { verbose_ = v; }
    void setFading(bool f) { use_fading_ = f; }
    void setForcedModulation(Modulation mod) { forced_mod_ = mod; }
    void setForcedCodeRate(CodeRate rate) { forced_rate_ = rate; }
    void setPreferredWaveform(WaveformMode mode) { forced_waveform_ = mode; }
    void setTestFileTransfer(bool v) { test_file_transfer_ = v; }
    void setTestFileSize(size_t bytes) { test_file_size_ = bytes; }

    bool runTest() {
        printHeader();

        // Setup channel
        channel_.configure(snr_db_, use_fading_);

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

        // Run protocol test (message or file)
        bool success = test_file_transfer_ ? runFileTransferTest() : runProtocolTest();

        // Stop
        alpha_->stop();
        bravo_->stop();

        if (success) {
            printSummary();
        }
        return success;
    }

private:
    float snr_db_ = 20.0f;
    bool verbose_ = false;
    bool use_fading_ = false;
    bool test_file_transfer_ = false;
    size_t test_file_size_ = 256;  // Default 256 bytes test file
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

        // Phase 3: Send 5 numbered messages
        std::cout << "\n=== PHASE 3: DATA TRANSFER (5 messages) ===\n";

        for (int msg_num = 1; msg_num <= 5; msg_num++) {
            std::string test_msg = "Message " + std::to_string(msg_num) + " from ALPHA";

            if (!waitFor([this]{ return alpha_->isReadyToSend(); }, 10)) {
                std::cout << "  \033[31m✗ ARQ not ready for message " << msg_num << "!\033[0m\n";
                return false;
            }

            // Reset received flag before sending
            message_received_.store(false);

            std::cout << "  [" << msg_num << "/5] Sending: \"" << test_msg << "\"\n";
            alpha_->sendMessage(test_msg);

            if (!waitFor([this]{ return message_received_.load(); }, 30)) {
                std::cout << "  \033[31m✗ Message " << msg_num << " not received!\033[0m\n";
                return false;
            }

            {
                std::lock_guard<std::mutex> lock(msg_mutex_);
                if (received_message_ == test_msg) {
                    std::cout << "  \033[32m✓ [" << msg_num << "/5] Received: \"" << received_message_ << "\"\033[0m\n";
                } else {
                    std::cout << "  \033[31m✗ Message " << msg_num << " corrupted! Got: \"" << received_message_ << "\"\033[0m\n";
                    return false;
                }
            }
        }

        std::cout << "  \033[32m✓ All 5 messages transferred successfully!\033[0m\n";

        // Phase 4: Disconnect
        std::cout << "\n=== PHASE 4: DISCONNECT ===\n";
        alpha_->disconnect();

        if (!waitFor([this]{ return !alpha_->isConnected() && !bravo_->isConnected(); }, 30)) {
            std::cout << "  \033[31m✗ Disconnect timeout!\033[0m\n";
            return false;
        }
        std::cout << "  \033[32m✓ Disconnected!\033[0m\n";

        return true;
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

        // Phase 4: Disconnect
        std::cout << "\n=== PHASE 4: DISCONNECT ===\n";
        alpha_->disconnect();

        if (!waitFor([this]{ return !alpha_->isConnected() && !bravo_->isConnected(); }, 30)) {
            std::cout << "  \033[31m✗ Disconnect timeout!\033[0m\n";
            return false;
        }
        std::cout << "  \033[32m✓ Disconnected!\033[0m\n";

        // Cleanup
        std::remove(test_file.c_str());
        std::remove(received_file_path_.c_str());

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

    void printHeader() {
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║   CLI Simulator - IWaveform + StreamingDecoder               ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
        std::cout << "\n";
        std::cout << "  SNR:     " << snr_db_ << " dB\n";
        std::cout << "  Channel: " << (use_fading_ ? "Fading" : "AWGN") << "\n";
        std::cout << "  Model:   Real-time (48kHz, 10ms callbacks)\n";
        std::cout << "\n";
    }

    void printSummary() {
        std::cout << "\n";
        std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                     TEST PASSED                              ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
        std::cout << "\n";
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
            sim.setFading(true);
        } else if (arg == "--mod" || arg == "-m") {
            if (i + 1 < argc) {
                std::string mod_str = argv[++i];
                if (mod_str == "dqpsk" || mod_str == "DQPSK") {
                    sim.setForcedModulation(Modulation::DQPSK);
                } else if (mod_str == "d8psk" || mod_str == "D8PSK") {
                    sim.setForcedModulation(Modulation::D8PSK);
                } else if (mod_str == "dbpsk" || mod_str == "DBPSK") {
                    sim.setForcedModulation(Modulation::DBPSK);
                } else {
                    std::cerr << "Unknown modulation: " << mod_str << " (use dqpsk, d8psk, dbpsk)\n";
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
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "CLI Simulator - IWaveform + StreamingDecoder Model\n\n";
            std::cout << "Uses IWaveform for TX and StreamingDecoder for RX directly.\n";
            std::cout << "Every 10ms: read RX, feed decoder, get TX, send to channel.\n\n";
            std::cout << "Options:\n";
            std::cout << "  --snr, -s <dB>      SNR (default: 20)\n";
            std::cout << "  --fading, -f        Enable fading channel\n";
            std::cout << "  --mod, -m <MOD>     Force modulation: dqpsk, d8psk, dbpsk\n";
            std::cout << "  --rate, -r <RATE>   Force code rate: r1_4, r1_2, r2_3, r3_4\n";
            std::cout << "  --waveform, -w <WF> Force waveform: mc_dpsk, ofdm_chirp, ofdm_cox\n";
            std::cout << "  --file [SIZE]       Test file transfer (default: 256 bytes)\n";
            std::cout << "  --verbose, -v       Verbose output\n";
            return 0;
        }
    }

    setLogLevel(LogLevel::INFO);
    return sim.runTest() ? 0 : 1;
}
