/**
 * CLI Simulator - Single Audio I/O Thread Model (like real sound card)
 *
 * Each station has ONE audio thread that handles both TX and RX,
 * exactly like a real sound card callback:
 *   - Every 10ms, read RX samples from channel
 *   - Feed RX to modem decoder
 *   - Check if modem has TX samples pending
 *   - Send TX samples to channel
 *
 * This matches real hardware behavior where you have a single
 * audio callback that handles both input and output.
 */

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <cmath>
#include <functional>

#include "gui/modem/modem_engine.hpp"
#include "protocol/protocol_engine.hpp"
#include "ultra/logging.hpp"
#include "sim/hf_channel.hpp"

using namespace ultra;
using namespace ultra::gui;
using namespace ultra::protocol;
using namespace ultra::sim;

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
 * Like a real sound card, one thread handles both TX and RX:
 * - Every 10ms (480 samples at 48kHz):
 *   1. Read RX samples from channel
 *   2. Feed to modem decoder
 *   3. Check for pending TX
 *   4. Send TX to channel
 */
class SimulatedStation {
public:
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int SAMPLES_PER_CALLBACK = 480;  // 10ms
    static constexpr int CALLBACK_INTERVAL_MS = 10;

    SimulatedStation(const std::string& callsign, SimulatedChannel& channel, bool is_station_a)
        : callsign_(callsign), channel_(channel), is_station_a_(is_station_a) {

        modem_.setLogPrefix(callsign);
        protocol_.setLocalCallsign(callsign);
        protocol_.setAutoAccept(true);

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

    // For receiving messages
    void setMessageCallback(std::function<void(const std::string&)> cb) {
        message_callback_ = cb;
    }

    void setSNR(float snr) { snr_db_ = snr; }

    void tick() {
        protocol_.tick(CALLBACK_INTERVAL_MS);
    }

    float getSimTime() const { return total_samples_ / (float)SAMPLE_RATE; }

private:
    std::string callsign_;
    SimulatedChannel& channel_;
    bool is_station_a_;

    ModemEngine modem_;
    ProtocolEngine protocol_{ConnectionConfig{}};

    std::atomic<bool> running_{false};
    std::thread audio_thread_;

    // TX queue - samples waiting to be transmitted
    std::mutex tx_mutex_;
    std::queue<float> tx_queue_;

    // State
    std::atomic<bool> connected_{false};
    std::atomic<bool> handshake_complete_{false};
    std::function<void(const std::string&)> message_callback_;

    std::atomic<uint64_t> total_samples_{0};
    float snr_db_ = 20.0f;

    void setupCallbacks() {
        // TX callback - queue samples for transmission
        protocol_.setTxDataCallback([this](const Bytes& data) {
            auto samples = modem_.transmit(data);
            queueTx(samples);
        });

        // RX callback
        modem_.setRawDataCallback([this](const Bytes& data) {
            protocol_.setChannelQuality(snr_db_, modem_.getFadingIndex());
            protocol_.onRxData(data);
        });

        // Connection state
        protocol_.setConnectionChangedCallback([this](ConnectionState state, const std::string&) {
            if (state == ConnectionState::CONNECTED) {
                connected_ = true;
                modem_.setConnected(true);
            } else if (state == ConnectionState::DISCONNECTED) {
                connected_ = false;
                modem_.setConnected(false);
            }
        });

        // Mode callbacks
        protocol_.setDataModeChangedCallback([this](Modulation mod, CodeRate rate, float) {
            modem_.setDataMode(mod, rate);
        });
        protocol_.setModeNegotiatedCallback([this](WaveformMode mode) {
            modem_.setWaveformMode(mode);
        });
        protocol_.setConnectWaveformChangedCallback([this](WaveformMode mode) {
            modem_.setConnectWaveform(mode);
        });
        protocol_.setHandshakeConfirmedCallback([this]() {
            modem_.setHandshakeComplete(true);
            handshake_complete_ = true;
        });

        // PING/PONG
        protocol_.setPingTxCallback([this]() {
            auto samples = modem_.transmitPing();
            queueTx(samples);
        });
        protocol_.setPingReceivedCallback([this]() {
            auto samples = modem_.transmitPong();
            queueTx(samples);
        });
        modem_.setPingReceivedCallback([this](float) {
            protocol_.onPingReceived();
        });

        // Message received
        protocol_.setMessageReceivedCallback([this](const std::string&, const std::string& text) {
            if (message_callback_) {
                message_callback_(text);
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

            // Calculate RMS for debug
            float rx_rms = 0.0f;
            for (float s : rx_samples) rx_rms += s * s;
            rx_rms = std::sqrt(rx_rms / rx_samples.size());

            // 2. FEED TO MODEM DECODER
            modem_.feedAudio(rx_samples);

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

        // Setup message callback on BRAVO
        bravo_->setMessageCallback([this](const std::string& msg) {
            std::lock_guard<std::mutex> lock(msg_mutex_);
            received_message_ = msg;
            message_received_ = true;
        });

        // Start audio threads
        alpha_->start();
        bravo_->start();

        // Run protocol test
        bool success = runProtocolTest();

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

    SimulatedChannel channel_;
    std::unique_ptr<SimulatedStation> alpha_;
    std::unique_ptr<SimulatedStation> bravo_;

    std::mutex msg_mutex_;
    std::string received_message_;
    std::atomic<bool> message_received_{false};

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

        // Phase 3: Send message
        std::cout << "\n=== PHASE 3: DATA TRANSFER ===\n";
        std::string test_msg = "Hello from ALPHA!";

        if (!waitFor([this]{ return alpha_->isReadyToSend(); }, 10)) {
            std::cout << "  \033[31m✗ ARQ not ready!\033[0m\n";
            return false;
        }

        std::cout << "  Sending: \"" << test_msg << "\"\n";
        alpha_->sendMessage(test_msg);

        if (!waitFor([this]{ return message_received_.load(); }, 30)) {
            std::cout << "  \033[31m✗ Message not received!\033[0m\n";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(msg_mutex_);
            if (received_message_ == test_msg) {
                std::cout << "  \033[32m✓ Message received: \"" << received_message_ << "\"\033[0m\n";
            } else {
                std::cout << "  \033[31m✗ Message corrupted!\033[0m\n";
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
        std::cout << "║   CLI Simulator - Single Audio I/O Thread Model              ║\n";
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
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "CLI Simulator - Single Audio I/O Thread Model\n\n";
            std::cout << "Each station has ONE audio thread (like real sound card).\n";
            std::cout << "Every 10ms: read RX, feed decoder, get TX, send to channel.\n\n";
            std::cout << "Options:\n";
            std::cout << "  --snr, -s <dB>   SNR (default: 20)\n";
            std::cout << "  --fading, -f     Enable fading channel\n";
            std::cout << "  --verbose, -v    Verbose output\n";
            return 0;
        }
    }

    setLogLevel(LogLevel::INFO);
    return sim.runTest() ? 0 : 1;
}
