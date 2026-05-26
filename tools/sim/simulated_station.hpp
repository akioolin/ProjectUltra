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
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <sstream>

#include "waveform/waveform_factory.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
#include "waveform/mc_dpsk_waveform.hpp"
#include "audio/channel_busy_detector.hpp"
#include "gui/modem/adaptive_reanchor_policy.hpp"
#include "psk/multi_carrier_dpsk.hpp"
#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"  // TX encoding (mirrors StreamingDecoder)
#include "protocol/protocol_engine.hpp"
#include "protocol/frame_v2.hpp"
#include "protocol/connection_policy.hpp"
#include "protocol/waveform_selection.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/phy_diagnostics.hpp"
#include "ultra/tx_burst_normalization.hpp"
#include "ultra/fec.hpp"  // ChannelInterleaver, LDPCEncoder
#include "fec/frame_interleaver.hpp"  // FrameInterleaver
#include "ota_channel_core/channel.hpp"

#include <cstdint>
#include <memory>
#include <utility>

#include "sim/cli_enums.hpp"

using namespace ultra;
using namespace ultra::gui;
using namespace ultra::protocol;
namespace v2 = protocol::v2;
using SimulatedChannel = ultra::ota_channel_core::SimulatedChannel;

enum class OFDMConfigPreset {
    Default,  // canonical OFDM geometry: 1024 FFT, 59 carriers
    Nvis      // legacy alias for the canonical 1024-FFT / 59-carrier geometry
};

[[maybe_unused]] static const char* channelTypeToString(ChannelType t) {
    switch (t) {
        case ChannelType::PASSTHROUGH: return "Passthrough";
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

[[maybe_unused]] static bool samplesHaveEnergy(const std::vector<float>& samples) {
    constexpr float kActiveSampleEpsilon = 1.0e-6f;
    for (float s : samples) {
        if (std::fabs(s) > kActiveSampleEpsilon) {
            return true;
        }
    }
    return false;
}

static bool sampleBlockHasEnergy(const float* samples, size_t count) {
    if (samples == nullptr) {
        return false;
    }
    constexpr float kActiveSampleEpsilon = 1.0e-6f;
    for (size_t i = 0; i < count; ++i) {
        if (std::fabs(samples[i]) > kActiveSampleEpsilon) {
            return true;
        }
    }
    return false;
}

[[maybe_unused]] static size_t countFullScaleSamples(const std::vector<float>& samples) {
    size_t count = 0;
    for (float s : samples) {
        if (std::fabs(s) > 1.0f) count++;
    }
    return count;
}

// ULTRA_CARRIER_SENSE_DEBUG=1 gates per-event logging for carrier-sense
// decisions made at the station / audio-port layer. Pairs with the same
// env var in src/audio/channel_busy_detector.cpp.
inline bool csDebugEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("ULTRA_CARRIER_SENSE_DEBUG");
        return env && env[0] != '\0' && !(env[0] == '0' && env[1] == '\0');
    }();
    return enabled;
}

inline void e2eDebugLine(const std::string& line) {
    const char* path = std::getenv("ULTRA_E2E_DEBUG_LOG");
    if (path == nullptr || *path == '\0') {
        return;
    }
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    std::ofstream out(path, std::ios::app);
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto epoch_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    out << "epoch_ms=" << epoch_ms << ' ' << line << '\n';
}

static float modeEfficiency(Modulation mod, CodeRate rate) {
    return static_cast<float>(getBitsPerSymbol(mod)) * getCodeRateValue(rate);
}

static Modulation mcDpskModulationForConfig(const MultiCarrierDPSKConfig& config) {
    return config.bits_per_symbol == 1 ? Modulation::DBPSK : Modulation::DQPSK;
}

static const char* adaptationDirection(Modulation from_mod, CodeRate from_rate,
                                       Modulation to_mod, CodeRate to_rate) {
    float from_eff = modeEfficiency(from_mod, from_rate);
    float to_eff = modeEfficiency(to_mod, to_rate);
    if (to_eff > from_eff + 0.05f) return "improving";
    if (to_eff < from_eff - 0.05f) return "degrading";
    return "changing";
}

inline bool isCarrierSenseCeilingQamMode(Modulation mod) {
    return mod == Modulation::QAM16 ||
           mod == Modulation::QAM32 ||
           mod == Modulation::QAM64 ||
           mod == Modulation::QAM256;
}

inline float carrierSenseDbpskNoiseFloorSampleCeiling() {
    return 0.75f * ultra::ota_channel_core::kModemReferenceInBandRms;
}

inline float carrierSenseQamNoiseFloorSampleCeiling() {
    return 0.77f * ultra::ota_channel_core::kModemReferenceInBandRms;
}

inline float carrierSenseNoiseFloorSampleCeilingForMode(Modulation mod) {
    switch (mod) {
        case Modulation::DBPSK:
            return carrierSenseDbpskNoiseFloorSampleCeiling();
        default:
            return 0.0f;
    }
}

class RadioPttStateMachine;

struct RadioTxPullResult {
    size_t emitted_samples = 0;
    size_t active_samples = 0;
    size_t audio_chain_pending_samples = 0;
    bool tx_active = false;
    bool tx_draining = false;
    std::string label;
};

class IRadioModem {
public:
    virtual ~IRadioModem() = default;
    virtual RadioTxPullResult pullTxSamples(float* out, size_t count) = 0;
    virtual void pushRxSamples(const float* in, size_t count) = 0;
};

inline ultra::audio::ChannelBusyDetectorConfig virtualAudioCarrierSenseConfig() {
    auto config = ultra::audio::ChannelBusyDetectorConfig{};
    // The virtual channel injects calibrated AWGN as continuous RX audio. At
    // low SNR that idle floor is intentionally much higher than the hardware
    // detector's conservative bootstrap ceiling, so the simulator must learn
    // the modem-calibrated floor before applying carrier-sense guards.
    config.noise_floor_bootstrap_rms_ceiling = std::max(
        config.noise_floor_bootstrap_rms_ceiling,
        ultra::ota_channel_core::kModemReferenceRms);
    config.noise_floor_percentile = 0.50f;
    return config;
}

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
    explicit AudioPort(ultra::audio::ChannelBusyDetectorConfig config = {})
        : channel_busy_detector_(config) {}
    virtual ~AudioPort() = default;
    // Pull up to `count` RX samples; pads with noise/zero if buffer underruns.
    virtual std::vector<float> pullRx(size_t count) = 0;
    virtual std::vector<float> pullRxBlocking(size_t count,
                                              std::chrono::milliseconds timeout) {
        (void)timeout;
        return pullRx(count);
    }
    // Push TX samples for transmission.
    virtual void queueTx(const std::vector<float>& samples) = 0;
    // VirtualAudioPort needs the simulator's 10 ms loop to pace samples into
    // the in-memory channel. HardwareAudioPort is already paced by SDL's real
    // device callback, so feeding it through a second 10 ms pacer can create
    // callback-phase underflows and synthetic gaps in the transmitted waveform.
    virtual bool shouldPaceTxInStationLoop() const { return true; }
    // Simulator audio transports need modem bursts normalized before the
    // soundcard-style 10 ms loop fragments them. Hardware radios keep the
    // modem output unchanged and rely on the real PA/ALC chain.
    virtual bool requiresTxBurstNormalization() const { return false; }
    // Optional lifecycle hooks; default no-op.
    virtual bool start() { return true; }
    virtual void stop() {}
    virtual void attachRadioState(const RadioPttStateMachine* ptt) {
        attached_ptt_ = ptt;
    }
    bool isChannelIdle() const {
        return channel_busy_detector_.isIdle();
    }
    bool isChannelIdleFor(uint32_t guard_ms) const {
        if (carrier_sense_sample_clock_time_) {
            return channel_busy_detector_.isIdleFor(std::chrono::milliseconds(guard_ms),
                                                    *carrier_sense_sample_clock_time_);
        }
        return channel_busy_detector_.isIdleFor(std::chrono::milliseconds(guard_ms));
    }
    bool waitForChannelIdle(uint32_t guard_ms) {
        return channel_busy_detector_.waitUntilIdle(std::chrono::milliseconds(guard_ms));
    }
    bool waitForChannelIdle(uint32_t guard_ms, uint32_t max_wait_ms) {
        return channel_busy_detector_.waitUntilIdle(std::chrono::milliseconds(guard_ms),
                                                    std::chrono::milliseconds(max_wait_ms));
    }
    float channelRms() const {
        return channel_busy_detector_.currentRms();
    }
    void setCarrierSenseNoiseFloorSampleCeiling(float ceiling) const {
        channel_busy_detector_.setNoiseFloorEstimateRmsCeiling(ceiling);
    }
    void setCarrierSenseSampleClock(uint64_t sample_index,
                                    uint32_t sample_rate = 48000) const {
        const auto micros = static_cast<int64_t>(
            (sample_index * 1'000'000ULL) / std::max<uint32_t>(1, sample_rate));
        carrier_sense_sample_clock_time_ =
            ultra::audio::ChannelBusyDetector::TimePoint{} +
            std::chrono::microseconds(micros);
    }
    void clearCarrierSenseSampleClock() const {
        carrier_sense_sample_clock_time_.reset();
    }

protected:
    bool attachedRadioInRxBlackout() const;
    std::vector<float> shapeRxForLocalRadio(std::vector<float> samples,
                                            size_t count) const;
    ultra::audio::ChannelBusyDetector::TimePoint carrierSenseNow() const {
        return carrier_sense_sample_clock_time_.value_or(
            ultra::audio::ChannelBusyDetector::Clock::now());
    }

private:
    const RadioPttStateMachine* attached_ptt_ = nullptr;
    mutable ultra::audio::ChannelBusyDetector channel_busy_detector_;
    mutable std::optional<ultra::audio::ChannelBusyDetector::TimePoint>
        carrier_sense_sample_clock_time_;
};

/**
 * VirtualAudioPort - In-process audio path through SimulatedChannel.
 * Preserves exact behavior of the original simulator pipe.
 */
class VirtualAudioPort : public AudioPort {
public:
    VirtualAudioPort(SimulatedChannel& channel, bool is_station_a)
        : AudioPort(virtualAudioCarrierSenseConfig()),
          channel_(channel), is_station_a_(is_station_a) {
        // This port emits fixed-size audio callback chunks, not logical TX
        // bursts. Burst normalization must happen before packetization or on a
        // full vector at SimulatedChannel::transmitFrom*(); per-callback
        // normalization would make callback boundaries part of the PHY.
        channel_.setTxBurstNormalizationEnabled(false);
    }

    std::vector<float> pullRx(size_t count) override {
        auto samples = is_station_a_ ? channel_.receiveForA(count)
                                     : channel_.receiveForB(count);
        return shapeRxForLocalRadio(std::move(samples), count);
    }

    void queueTx(const std::vector<float>& samples) override {
        if (is_station_a_) channel_.transmitFromA(samples);
        else               channel_.transmitFromB(samples);
    }

    bool requiresTxBurstNormalization() const override { return true; }

private:
    SimulatedChannel& channel_;
    bool is_station_a_;
};


enum class PttState {
    RX,
    TX,
    TX_TR_SWITCH,
    TX_COOLDOWN
};

inline const char* pttStateName(PttState state) {
    switch (state) {
        case PttState::RX: return "RX";
        case PttState::TX: return "TX";
        case PttState::TX_TR_SWITCH: return "TX_TR_SWITCH";
        case PttState::TX_COOLDOWN: return "TX_COOLDOWN";
    }
    return "UNKNOWN";
}

class RadioPttStateMachine {
public:
    explicit RadioPttStateMachine(uint32_t sample_rate = 48000)
        : sample_rate_(sample_rate) {}

    void setRecoveryTimings(uint32_t tr_switch_ms, uint32_t cooldown_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        tx_tr_switch_ms_ = tr_switch_ms;
        tx_cooldown_ms_ = cooldown_ms;
        if (ptt_state_ == PttState::TX_TR_SWITCH && tx_tr_switch_ms_ == 0) {
            tx_tr_switch_samples_remaining_ = 0;
            enterCooldownOrRxLocked();
        } else if (ptt_state_ == PttState::TX_COOLDOWN && tx_cooldown_ms_ == 0) {
            tx_cooldown_samples_remaining_ = 0;
            ptt_state_ = PttState::RX;
        }
    }

    void setTxTrSwitchMs(uint32_t ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        tx_tr_switch_ms_ = ms;
    }

    void setTxCooldownMs(uint32_t ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        tx_cooldown_ms_ = ms;
    }

    uint32_t txTrSwitchMs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tx_tr_switch_ms_;
    }

    uint32_t txCooldownMs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tx_cooldown_ms_;
    }

    PttState state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ptt_state_;
    }

    bool isInRxBlackout() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ptt_state_ == PttState::TX ||
               ptt_state_ == PttState::TX_TR_SWITCH;
    }

    bool isReadyForNextTx() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ptt_state_ == PttState::RX;
    }

    void noteTxQueued(size_t sample_count) {
        if (sample_count == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        tx_in_flight_samples_remaining_ += static_cast<uint64_t>(sample_count);
        tx_tr_switch_samples_remaining_ = 0;
        tx_cooldown_samples_remaining_ = 0;
        ptt_state_ = PttState::TX;
    }

    void noteTxDrained(size_t drained_samples) {
        if (drained_samples == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (ptt_state_ != PttState::TX) {
            return;
        }
        const uint64_t drained = static_cast<uint64_t>(drained_samples);
        if (drained >= tx_in_flight_samples_remaining_) {
            tx_in_flight_samples_remaining_ = 0;
        } else {
            tx_in_flight_samples_remaining_ -= drained;
        }
        if (tx_in_flight_samples_remaining_ == 0) {
            beginPostTxRecoveryLocked();
        }
    }

    void noteTxSampleBlock(bool tx_active, bool tx_draining_block = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tx_active || tx_draining_block) {
            tx_in_flight_samples_remaining_ = 0;
            tx_tr_switch_samples_remaining_ = 0;
            tx_cooldown_samples_remaining_ = 0;
            ptt_state_ = PttState::TX;
            return;
        }

        if (ptt_state_ == PttState::TX) {
            beginPostTxRecoveryLocked();
        }
    }

    void advanceSamples(size_t elapsed_samples) {
        if (elapsed_samples == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t remaining = static_cast<uint64_t>(elapsed_samples);
        if (ptt_state_ == PttState::TX_TR_SWITCH) {
            remaining = consumeLocked(tx_tr_switch_samples_remaining_, remaining);
            if (tx_tr_switch_samples_remaining_ > 0) {
                return;
            }
            enterCooldownOrRxLocked();
        }
        if (remaining > 0 && ptt_state_ == PttState::TX_COOLDOWN) {
            consumeLocked(tx_cooldown_samples_remaining_, remaining);
            if (tx_cooldown_samples_remaining_ == 0) {
                ptt_state_ = PttState::RX;
            }
        }
    }

private:
    uint64_t samplesForMsLocked(uint32_t ms) const {
        return (static_cast<uint64_t>(ms) * static_cast<uint64_t>(sample_rate_)) /
               1000ULL;
    }

    static uint64_t consumeLocked(uint64_t& counter, uint64_t elapsed) {
        if (elapsed >= counter) {
            const uint64_t leftover = elapsed - counter;
            counter = 0;
            return leftover;
        }
        counter -= elapsed;
        return 0;
    }

    void beginPostTxRecoveryLocked() {
        tx_tr_switch_samples_remaining_ = samplesForMsLocked(tx_tr_switch_ms_);
        tx_cooldown_samples_remaining_ = samplesForMsLocked(tx_cooldown_ms_);
        if (tx_tr_switch_samples_remaining_ > 0) {
            ptt_state_ = PttState::TX_TR_SWITCH;
            return;
        }
        enterCooldownOrRxLocked();
    }

    void enterCooldownOrRxLocked() {
        ptt_state_ = tx_cooldown_samples_remaining_ > 0 ? PttState::TX_COOLDOWN
                                                        : PttState::RX;
    }

    const uint32_t sample_rate_;
    mutable std::mutex mutex_;
    PttState ptt_state_ = PttState::RX;
    uint64_t tx_in_flight_samples_remaining_ = 0;
    uint64_t tx_tr_switch_samples_remaining_ = 0;
    uint64_t tx_cooldown_samples_remaining_ = 0;
    uint32_t tx_tr_switch_ms_ = 20;
    uint32_t tx_cooldown_ms_ = 100;
};


class SimulatedStation : public IRadioModem {
public:
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int SAMPLES_PER_CALLBACK = 480;  // 10ms
    static constexpr int CALLBACK_INTERVAL_MS = 10;
    static constexpr int TX_CONTINUATION_GRACE_MS = CALLBACK_INTERVAL_MS * 2;
    static constexpr int POST_TX_ACK_LISTEN_MS = CALLBACK_INTERVAL_MS * 3;
    static constexpr uint32_t DEFAULT_TR_GUARD_MS = 50;

    SimulatedStation(const std::string& callsign, std::unique_ptr<AudioPort> port,
                     OFDMConfigPreset ofdm_config_preset = OFDMConfigPreset::Default,
                     const MultiCarrierDPSKConfig& mc_dpsk_config = mc_dpsk_presets::robust_mid(),
                     const ConnectionConfig& connection_config = ConnectionConfig{})
        : callsign_(callsign),
          port_(std::move(port)),
          protocol_(connection_config) {
        ofdm_config_preset_ = ofdm_config_preset;
        mc_dpsk_config_ = mc_dpsk_config;
        control_mc_dpsk_config_ = mc_dpsk_config;
        protocol_.setLocalCallsign(callsign);
        protocol_.setAutoAccept(true);
        protocol_.setMCDPSKConfig(mc_dpsk_config_.num_carriers,
                                  mc_dpsk_config_.samples_per_symbol);
        data_modulation_ = mcDpskModulationForConfig(mc_dpsk_config_);

        // Initialize OFDM geometry from the selected CLI preset.
        ofdm_config_ = createOFDMConfig();

        // Create TX encoder and RX decoder (both use same config)
        createEncoder();
        createDecoder();

        if (port_) {
            port_->attachRadioState(&ptt_);
        }
        setupCallbacks();
    }

    ~SimulatedStation() {
        stop();
        if (port_) {
            port_->attachRadioState(nullptr);
        }
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
    void setRxDecodeResultCallback(std::function<void(const DecodeResult&)> cb) {
        rx_decode_result_callback_ = std::move(cb);
    }

    void setSNR(float snr) { snr_db_ = snr; }
    void setChannelType(ChannelType channel_type) {
        channel_type_ = channel_type;
        syncBurstInterleaveForConnectedMode();
        syncAdaptiveShortReanchorForConnectedMode();
    }
    void setAutoAccept(bool auto_accept) { protocol_.setAutoAccept(auto_accept); }
    void setForcedModulation(Modulation mod) { protocol_.setForcedModulation(mod); }
    void setForcedCodeRate(CodeRate rate) { protocol_.setForcedCodeRate(rate); }
    void setMCDPSKConfig(const MultiCarrierDPSKConfig& config) {
        mc_dpsk_config_ = config;
        control_mc_dpsk_config_ = config;
        data_modulation_ = mcDpskModulationForConfig(mc_dpsk_config_);
        protocol_.setMCDPSKConfig(mc_dpsk_config_.num_carriers,
                                  mc_dpsk_config_.samples_per_symbol);
        if (encoder_) {
            encoder_->setMCDPSKConfig(mc_dpsk_config_);
            encoder_->setDataMode(data_modulation_, data_code_rate_);
        }
        if (decoder_) {
            decoder_->setMCDPSKConfig(mc_dpsk_config_);
            decoder_->setDataMode(data_modulation_, data_code_rate_);
        }
    }
    void applyNegotiatedMCDPSKConfig(Modulation mod,
                                     int num_carriers,
                                     int samples_per_symbol) {
        if (num_carriers <= 0 || samples_per_symbol <= 0) {
            return;
        }
        const int bits_per_symbol = (mod == Modulation::DBPSK) ? 1 :
                                    (mod == Modulation::D8PSK) ? 3 : 2;
        if (mc_dpsk_config_.num_carriers == num_carriers &&
            mc_dpsk_config_.samples_per_symbol == samples_per_symbol &&
            mc_dpsk_config_.bits_per_symbol == bits_per_symbol) {
            return;
        }

        mc_dpsk_config_.num_carriers = num_carriers;
        mc_dpsk_config_.samples_per_symbol = samples_per_symbol;
        mc_dpsk_config_.bits_per_symbol = bits_per_symbol;
        if (encoder_) {
            encoder_->setMCDPSKConfig(mc_dpsk_config_);
        }
        if (decoder_) {
            decoder_->setMCDPSKConfig(mc_dpsk_config_);
        }
        LOG_MODEM(INFO, "[%s] Negotiated MC-DPSK config: carriers=%d sps=%d bits/sym=%d",
                  callsign_.c_str(), mc_dpsk_config_.num_carriers,
                  mc_dpsk_config_.samples_per_symbol,
                  mc_dpsk_config_.bits_per_symbol);
    }
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
    void setPaprReductionEnabled(bool enable) {
        papr_reduction_enabled_ = enable;
        if (encoder_) encoder_->setPaprReductionEnabled(enable);
    }
    void setPreferredWaveform(WaveformMode mode) {
        protocol_.setPreferredMode(mode);
        // For narrowband, use narrowband chirp for PING/PONG/CONNECT control frames
        if (mode == WaveformMode::OFDM_NARROW && encoder_) {
            encoder_->setNarrowbandControl(true);
        }
    }

    // Disable burst interleaving (for A/B testing)
    void setNoBurstInterleave(bool v) {
        no_burst_interleave_ = v;
        syncBurstInterleaveForConnectedMode();
    }
    void setBurstInterleaveGroupSize(int n) {
        burst_group_size_ = ofdm_link_adaptation::sanitizeBurstGroupSize(n);
        if (encoder_) encoder_->setBurstInterleaveGroupSize(burst_group_size_);
        if (decoder_) decoder_->setBurstInterleaveGroupSize(burst_group_size_);
    }
    // File-class profile rollout stage (0=off/behavior-preserving). See
    // file_profile_stage_ for the staged-knob meaning.
    void setFileProfileStage(int stage) { file_profile_stage_ = std::clamp(stage, 0, 3); }
    void setRxOverfeedFactor(int n) { rx_overfeed_factor_ = std::clamp(n, 1, 200); }
    void setDecodeDelayMs(int ms) { decode_delay_ms_ = std::clamp(ms, 0, 500); }
    void setRxBatchCallbacks(int n) { rx_batch_callbacks_ = std::clamp(n, 1, 1000); }
    void setTxRecoveryTimings(uint32_t tr_switch_ms, uint32_t cooldown_ms) {
        ptt_.setRecoveryTimings(tr_switch_ms, cooldown_ms);
    }
    void setTxTrSwitchMs(uint32_t ms) { ptt_.setTxTrSwitchMs(ms); }
    void setTxCooldownMs(uint32_t ms) { ptt_.setTxCooldownMs(ms); }
    void setRxSettlingMs(uint32_t ms) { ptt_.setRecoveryTimings(ms, 0); }
    void setCarrierSenseGuardMs(uint32_t ms) { tx_turnaround_guard_ms_ = ms; }
    PttState pttState() const {
        return ptt_.state();
    }
    bool isInRxBlackout() const {
        return ptt_.isInRxBlackout();
    }
    bool isReadyForNextTx() const {
        return ptt_.isReadyForNextTx();
    }

    RadioTxPullResult pullTxSamples(float* out, size_t count) override {
        RadioTxPullResult result;
        if (out == nullptr || count == 0) {
            return result;
        }

        std::fill(out, out + count, 0.0f);

        size_t copied = 0;
        while (copied < count) {
            if (!ensureActiveTx()) {
                break;
            }
            const size_t n = copyActiveTxSamples(out + copied, count - copied,
                                                 result.label);
            if (n == 0) {
                break;
            }
            copied += n;
            result.emitted_samples += n;
        }

        {
            std::lock_guard<std::mutex> lock(tx_mutex_);
            tx_sample_clock_ += count;
            tx_emitted_sample_clock_ += result.emitted_samples;
        }

        result.tx_active = sampleBlockHasEnergy(out, count);
        result.tx_draining = result.emitted_samples > 0;
        result.active_samples = result.tx_active ? count : 0;
        result.audio_chain_pending_samples = result.tx_draining ? count : 0;
        notePttTxSampleBlock(result.tx_active, result.tx_draining);
        return result;
    }

    void pushRxSamples(const float* in, size_t count) override {
        if (decoder_ == nullptr || in == nullptr || count == 0) {
            return;
        }
        if (rx_batch_callbacks_ <= 1) {
            decoder_->feedAudio(in, count);
            return;
        }

        rx_batch_buffer_.insert(rx_batch_buffer_.end(), in, in + count);
        rx_batch_counter_++;
        if (rx_batch_counter_ >= rx_batch_callbacks_) {
            decoder_->feedAudio(rx_batch_buffer_.data(), rx_batch_buffer_.size());
            rx_batch_buffer_.clear();
            rx_batch_counter_ = 0;
        }
    }

#ifdef ULTRA_SIM_STATION_TEST_HOOKS
    void testQueueTxSamples(const std::vector<float>& samples,
                            const std::string& label = std::string()) {
        queueTx(samples, label);
    }

    size_t testDrainLocalTxSamples(size_t max_samples) {
        std::vector<float> drained_samples(max_samples, 0.0f);
        auto result = pullTxSamples(drained_samples.data(), drained_samples.size());
        return result.emitted_samples;
    }

    void testAdvanceRadioSamples(size_t elapsed_samples) {
        advancePttRecovery(elapsed_samples);
    }

    void testObserveIdleTxBlock() {
        notePttTxSampleBlock(false);
    }

    void testFlushDeferredTxIfReady() {
        flushDeferredTxIfReady();
    }

    void testMarkRxObserved() {
        rx_observation_epoch_.fetch_add(1, std::memory_order_relaxed);
    }

    size_t testTxQueueDepth() {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        return pendingTxSampleEstimateLocked();
    }

    size_t testTxLogicalDepth() {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        return tx_submissions_.size() + (active_tx_.hasRemaining() ? 1 : 0) +
               (tx_job_starting_ ? 1 : 0);
    }

    size_t testDeferredTxDepth() {
        std::lock_guard<std::mutex> lock(deferred_tx_mutex_);
        return deferred_tx_submissions_.size();
    }
#endif

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
            if (protocolTimersCanAdvance()) {
                protocol_.tick(CALLBACK_INTERVAL_MS);
            }
            return;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_tick_time_).count();
        // Clamp to a sane range — ticks longer than 1s probably mean the
        // process was suspended; clamp to 1s to avoid huge timer jumps.
        uint32_t elapsed_ms = static_cast<uint32_t>(
            std::clamp<int64_t>(elapsed, 1, 1000));
        last_tick_time_ = now;
        if (!protocolTimersCanAdvance()) {
            return;
        }
        protocol_.tick(elapsed_ms);
        maybeSyncFileProfileOnTransition();
    }

    void tickByMs(uint32_t elapsed_ms) {
        if (!protocolTimersCanAdvance()) {
            return;
        }
        protocol_.tick(elapsed_ms);
        // Must run here too: the OTASim sample-clock pump advances stations via
        // tickByMs(), NOT tick(), so the file-profile edge-detect has to live on
        // both paths or it never fires during an OTASim file transfer.
        maybeSyncFileProfileOnTransition();
    }

    // File-transfer regime edge-detect: apply the file-class PHY profile when a
    // transfer starts and revert it when it ends/cancels. No-op at stage 0
    // (syncFileProfile leaves pilot_spacing unchanged).
    void maybeSyncFileProfileOnTransition() {
        const bool xfer = isFileTransferInProgress();
        if (xfer != file_profile_active_cached_) {
            file_profile_active_cached_ = xfer;
            syncFileProfile();
        }
    }

    bool startSampleClockPump() {
        if (running_) return true;
        if (port_ && !port_->start()) {
            LOG_MODEM(ERROR, "[%s] AudioPort start failed", callsign_.c_str());
            return false;
        }
        running_ = true;
        return true;
    }

    void stopSampleClockPump() {
        running_ = false;
        if (decoder_) decoder_->stop();
        if (port_) {
            port_->clearCarrierSenseSampleClock();
            port_->stop();
        }
    }

    std::vector<float> sampleClockPullTx(size_t count) {
        if (port_) {
            port_->setCarrierSenseSampleClock(total_samples_.load(std::memory_order_relaxed),
                                              SAMPLE_RATE);
        }
        advancePttRecovery(count);
        flushDeferredTxIfReady();

        std::vector<float> tx_samples(count, 0.0f);
        const bool pace_tx = !port_ || port_->shouldPaceTxInStationLoop();
        if (pace_tx) {
            (void)pullTxSamples(tx_samples.data(), tx_samples.size());
        }
        return tx_samples;
    }

    void sampleClockQueueTx(const std::vector<float>& samples) {
        if (port_) {
            port_->queueTx(samples);
        }
    }

    std::vector<float> sampleClockPullRx(size_t count,
                                         std::chrono::milliseconds timeout) {
        if (!port_) {
            return std::vector<float>(count, 0.0f);
        }
        port_->setCarrierSenseSampleClock(
            total_samples_.load(std::memory_order_relaxed) + count,
            SAMPLE_RATE);
        return port_->pullRxBlocking(count, timeout);
    }

    void sampleClockPushRx(const std::vector<float>& samples) {
        if (samples.empty()) {
            return;
        }
        pushRxSamples(samples.data(), samples.size());
        rx_observation_epoch_.fetch_add(1, std::memory_order_relaxed);
        if (decoder_) {
            decoder_->processBuffer();
        }
        flushDeferredTxIfReady();
        total_samples_ += samples.size();
    }

    float getSimTime() const { return total_samples_ / (float)SAMPLE_RATE; }

    // Stats accessors
    ConnectionStats getConnectionStats() const { return protocol_.getStats(); }
    ConnectionState getConnectionState() const { return protocol_.getState(); }
    DecoderStats getDecoderStats() const { return decoder_ ? decoder_->getStats() : DecoderStats{}; }
    std::string getCallsign() const { return callsign_; }
    WaveformMode getNegotiatedWaveform() const { return negotiated_waveform_; }
    Modulation getDataModulation() const { return data_modulation_; }
    CodeRate getDataCodeRate() const { return data_code_rate_; }
    int getFixedFrameCodewords() const { return fixed_frame_codewords_; }
    MultiCarrierDPSKConfig getMCDPSKConfig() const { return mc_dpsk_config_; }
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
    MultiCarrierDPSKConfig mc_dpsk_config_ = mc_dpsk_presets::robust_mid();
    MultiCarrierDPSKConfig control_mc_dpsk_config_ = mc_dpsk_presets::robust_mid();
    ChannelType channel_type_ = ChannelType::AWGN;
    float adaptive_preamble_peer_fading_ = -1.0f;
    bool adaptive_short_reanchor_active_ = false;

    // Protocol engine
    ProtocolEngine protocol_{ConnectionConfig{}};

    std::atomic<bool> running_{false};
    std::thread audio_thread_;
    std::thread decode_thread_;
    // Real-time tick accounting (see tick() above).
    std::chrono::steady_clock::time_point last_tick_time_{};

    struct TxSubmission {
        enum class Kind {
            Frame,
            Burst,
            Ping,
            Pong,
            RawSamples
        };

        Kind kind = Kind::RawSamples;
        Bytes frame;
        std::vector<Bytes> burst;
        std::vector<float> raw_samples;
        std::string label;
        uint64_t min_rx_observation_epoch = 0;
        bool expect_full_ofdm_anchor_after_tx = false;
    };

    struct ActiveTx {
        std::vector<float> samples;
        size_t offset = 0;
        std::string label;

        bool hasRemaining() const {
            return offset < samples.size();
        }

        size_t remaining() const {
            return hasRemaining() ? samples.size() - offset : 0;
        }

        void clear() {
            samples.clear();
            offset = 0;
            label.clear();
        }
    };

    // TX state. Protocol callbacks enqueue logical submissions; the audio loop
    // pulls one callback block at a time from active_tx_.
    std::mutex tx_mutex_;
    std::deque<TxSubmission> tx_submissions_;
    ActiveTx active_tx_;
    bool tx_job_starting_ = false;
    uint64_t tx_sample_clock_ = 0;  // Continuous TX capture sample cursor.
    uint64_t tx_emitted_sample_clock_ = 0;  // Actual non-idle waveform samples emitted.

public:
    // Continuous TX callback cursor since station start. This includes idle
    // pulls and remains useful for TX ordering diagnostics.
    uint64_t txSampleClock() const { return tx_sample_clock_; }

    // Actual queued waveform samples emitted by this station. Unlike
    // txSampleClock(), this excludes idle/silent callbacks and is the right
    // numerator for DATA+ACK on-air goodput accounting.
    uint64_t txEmittedSampleClock() const { return tx_emitted_sample_clock_; }

private:

    std::mutex deferred_tx_mutex_;
    std::deque<TxSubmission> deferred_tx_submissions_;
    uint64_t deferred_tx_count_ = 0;
    uint64_t rejected_tx_count_ = 0;
    static constexpr size_t kMaxDeferredTxSubmissions = 16;
    std::atomic<uint64_t> rx_observation_epoch_{0};

    // Explicit radio PTT state. TX and TX_TR_SWITCH deafen RX; TX_COOLDOWN
    // reopens RX while holding off the next TX keying edge.
    RadioPttStateMachine ptt_{SAMPLE_RATE};
    std::atomic<uint64_t> tx_continuation_grace_samples_{0};
    std::atomic<uint64_t> post_tx_ack_listen_samples_{0};
    uint32_t tx_turnaround_guard_ms_ = DEFAULT_TR_GUARD_MS;

    // State
    std::atomic<bool> connected_{false};
    std::atomic<bool> handshake_complete_{false};
    float last_cfo_hz_ = 0.0f;  // CFO from chirp detection, used for light preamble

    std::function<void(const std::string&)> message_callback_;
    std::function<void(const std::string&, bool)> file_received_callback_;
    std::function<void(const DecodeResult&)> decoded_frame_callback_;
    std::function<void(const DecodeResult&)> rx_decode_result_callback_;

    std::atomic<uint64_t> total_samples_{0};
    float snr_db_ = 20.0f;
    bool no_burst_interleave_ = false;  // Disable burst interleaving for A/B testing
    bool burst_interleave_active_ = false;
    int burst_group_size_ = 8;
    // File-profile regime rollout control (2026-05-26). The file-class profile
    // (thin pilots + long LDPC + deep interleave) activates ONLY during a file
    // transfer. Staged so each PHY knob flips one at a time with a measurement
    // between: 0=off (behavior-preserving, today's values), 1=+thin pilots
    // (spacing 10 -> 53 data carriers), 2=+n=1944 long LDPC, 3=+deep interleave.
    int file_profile_stage_ = 0;
    bool file_profile_active_cached_ = false;  // last-applied file-xfer state (edge detect)
    int fixed_frame_codewords_ = v2::kDefaultFixedFrameCodewords;
    uint64_t carrier_mask_ = UINT64_MAX;
    bool carrier_ldpc_interleaver_enabled_ = false;
    bool papr_reduction_enabled_ = ultra::phy::kPaprReductionDefaultEnabled;
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
        // A default-constructed ModemConfig already carries the canonical OFDM
        // geometry (1024-FFT, 59 carriers, MEDIUM CP) that OFDM-CHIRP uses; the
        // NVIS preset shares the same geometry. (This used to borrow a legacy
        // OFDM waveform class as a geometry source, which made the logs read as
        // if COX were in play.)
        ModemConfig cfg;

        cfg.sample_rate = SAMPLE_RATE;
        cfg.center_freq = 1500.0f;
        cfg.modulation = data_modulation_;
        cfg.code_rate = data_code_rate_;
        cfg.use_pilots = true;
        cfg.pilot_spacing =
            ofdm_link_adaptation::recommendedPilotSpacing(cfg.modulation, cfg.code_rate);

        LOG_MODEM(INFO, "[%s] OFDM config preset=%s FFT=%d carriers=%d CP=%d pilots=%d spacing=%d",
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
        encoder_->setMCDPSKConfig(mc_dpsk_config_);
        encoder_->setOFDMConfig(ofdm_config_);
        encoder_->setMode(tx_waveform_mode_);
        encoder_->setDataMode(data_modulation_, data_code_rate_);
        encoder_->setFixedFrameCodewords(fixed_frame_codewords_);
        encoder_->setCarrierMask(carrier_mask_);
        encoder_->setCarrierLdpcInterleaver(carrier_ldpc_interleaver_enabled_);
        encoder_->setBurstInterleaveGroupSize(burst_group_size_);
        encoder_->setPaprReductionEnabled(papr_reduction_enabled_);
        syncAdaptiveShortReanchorForConnectedMode();

        LOG_MODEM(INFO, "[%s] TX encoder: mode=%s, carriers=%d, data_carriers=%d",
                  callsign_.c_str(),
                  waveformModeToString(tx_waveform_mode_),
                  encoder_->getConfig().num_carriers,
                  encoder_->getConfig().data_carriers);
    }

    void createDecoder() {
        decoder_ = std::make_unique<StreamingDecoder>();
        decoder_->setLogPrefix(callsign_);
        decoder_->setMCDPSKConfig(mc_dpsk_config_);

        // Start in disconnected mode (MC_DPSK for PING detection)
        decoder_->setMode(WaveformMode::MC_DPSK, false);
        decoder_->setDataMode(data_modulation_, data_code_rate_);
        decoder_->setBurstInterleaveGroupSize(burst_group_size_);
        decoder_->setFixedFrameCodewords(fixed_frame_codewords_);
        decoder_->setCarrierMask(carrier_mask_);
        decoder_->setCarrierLdpcInterleaver(carrier_ldpc_interleaver_enabled_);
        syncAdaptiveShortReanchorForConnectedMode();
        decoder_->setSoftCombineBuffer(protocol_.softCombineBuffer());
        decoder_->setHarqProvisionalContextCallback([this]() {
            return protocol_.harqProvisionalContext();
        });

        // Set frame callback
        decoder_->setFrameCallback([this](const DecodeResult& result) {
            handleDecodedFrame(result);
        });

        decoder_->setDataSyncAcceptedCallback([this](float sync_correlation) {
            protocol_.onAcceptedOFDMDataSync(sync_correlation);
        });

        // Set ping callback
        decoder_->setPingCallback([this](float, float cfo_hz) {
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

    bool shouldEnableBurstInterleaveForConnectedMode() const {
        return connected_.load() &&
               negotiated_waveform_ == WaveformMode::OFDM_CHIRP &&
               !no_burst_interleave_ &&
               ((data_modulation_ == Modulation::QAM16 &&
                 channel_type_ != ChannelType::AWGN &&
                 connection_policy::isBurstInterleavedOFDMMode(data_modulation_,
                                                               data_code_rate_)) ||
                connection_policy::isSpeculativeHighRateOFDM(data_modulation_,
                                                             data_code_rate_));
    }

    // Pilot spacing for the data path, accounting for the file-transfer regime.
    // During an active file transfer at stage >= 1 the regime thins pilots to
    // spacing 10 (6 scattered pilots -> 53 data carriers) for ALL frames
    // (data + the interspersed control/ACK), which is safe at the good SNR
    // where the file profile is allowed and is what makes a thin-pilot
    // FILE_CANCEL decode against the receiver's thin-pilot demod. Outside a
    // file transfer (and at stage 0) it falls back to the recommended spacing,
    // so the default path is byte-identical to before.
    uint32_t dataPilotSpacingForFileProfile() const {
        if (file_profile_stage_ >= 1 &&
            negotiated_waveform_ == WaveformMode::OFDM_CHIRP &&
            isFileTransferInProgress()) {
            return 10;  // file regime: 6 scattered pilots, 53 data carriers
        }
        return ofdm_link_adaptation::recommendedPilotSpacing(data_modulation_,
                                                             data_code_rate_);
    }

    // Apply / revert the file-class PHY regime when the file-transfer state
    // changes mid-connection. Re-pushes the OFDM config to both encoder and
    // decoder so the thin-pilot map is in effect for the duration of the
    // transfer and reverts cleanly at DATA_END / FILE_CANCEL. Stage 0 keeps
    // this a no-op (spacing unchanged) so the default path cannot regress.
    void syncFileProfile() {
        if (!connected_.load() ||
            negotiated_waveform_ != WaveformMode::OFDM_CHIRP) {
            return;
        }
        const uint32_t want_spacing = dataPilotSpacingForFileProfile();
        if (ofdm_config_.pilot_spacing == want_spacing) {
            return;  // nothing to do (stage 0, or already in the right regime)
        }
        ofdm_config_.pilot_spacing = want_spacing;
        if (encoder_) encoder_->setOFDMConfig(ofdm_config_);
        if (decoder_) decoder_->setOFDMConfig(ofdm_config_);
        LOG_MODEM(INFO,
                  "[%s] File profile: pilot_spacing -> %u (file_xfer=%d stage=%d)",
                  callsign_.c_str(), want_spacing,
                  isFileTransferInProgress() ? 1 : 0, file_profile_stage_);
    }

    void syncBurstInterleaveForConnectedMode() {
        const bool enable = shouldEnableBurstInterleaveForConnectedMode();
        if (encoder_) encoder_->setBurstInterleave(enable);
        if (decoder_) decoder_->setBurstInterleave(enable);

        if (burst_interleave_active_ != enable) {
            burst_interleave_active_ = enable;
            LOG_MODEM(INFO, "[%s] Burst interleaving %s (group=%d)",
                      callsign_.c_str(), enable ? "ENABLED" : "DISABLED",
                      burst_group_size_);
        }
    }

    bool shouldEnableShortReanchorForConnectedMode() const {
        const bool measured_fading =
            adaptive_reanchor_policy::shouldUseShortReanchor(
                negotiated_waveform_, data_modulation_,
                adaptive_preamble_peer_fading_);
        const bool simulator_fading_class =
            negotiated_waveform_ == WaveformMode::OFDM_CHIRP &&
            ofdm_link_adaptation::isCoherentModulation(data_modulation_) &&
            channel_type_ != ChannelType::AWGN;
        return connected_.load() && (measured_fading || simulator_fading_class);
    }

    void syncAdaptiveShortReanchorForConnectedMode() {
        const bool enable = shouldEnableShortReanchorForConnectedMode();
        const bool changed = adaptive_short_reanchor_active_ != enable;
        if (enable || changed) {
            if (encoder_) encoder_->setAdaptiveShortDataPreamble(enable);
            if (decoder_) decoder_->setAdaptiveShortDataPreamble(enable);
        }
        if (changed) {
            adaptive_short_reanchor_active_ = enable;
            LOG_MODEM(INFO,
                      "[%s] Adaptive short data re-anchor %s (channel=%s peer_fading=%.2f chirp=%.0f ms)",
                      callsign_.c_str(), enable ? "ENABLED" : "DISABLED",
                      channelTypeToString(channel_type_),
                      adaptive_preamble_peer_fading_,
                      adaptive_reanchor_policy::shortReanchorChirpDurationMs());
        }
    }

    void handleDecodedFrame(const DecodeResult& result) {
        if (rx_decode_result_callback_) {
            rx_decode_result_callback_(result);
        }

        if (!result.success) {
            if (result.has_partial_codewords) {
                float fading_index = decoder_ ? decoder_->getLastFadingIndex() : 0.0f;
                protocol_.setChannelQuality(snr_db_, fading_index);
                protocol_.onMCDPSKPartialFrame(result.partial_codewords);
            }
            return;
        }

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
            const bool use_quality_sample =
                protocol_.shouldUseRxFrameForChannelQuality(result.frame_data);
            if (phyDiagnosticsEnabled()) {
                char line[512];
                std::snprintf(line, sizeof(line),
                              "event=%s station=%s frame_type=%s "
                              "seq=%d snr_db=%.2f fading=%.6f data_mod=%d data_rate=%d "
                              "cw_ok=%d cw_fail=%d",
                              use_quality_sample
                                  ? "station_frame_quality"
                                  : "station_frame_quality_rejected",
                              callsign_.c_str(),
                              header.valid ? v2::frameTypeToString(header.type) : "INVALID",
                              header.valid ? header.seq : -1,
                              snr_db_,
                              fading_index,
                              static_cast<int>(data_modulation_),
                              static_cast<int>(data_code_rate_),
                              result.codewords_ok,
                              result.codewords_failed);
                phyDiagLine(line);
            }
            if (use_quality_sample && port_) {
                if (isCarrierSenseCeilingQamMode(data_modulation_)) {
                    const bool low_fading_frame =
                        std::isfinite(fading_index) && fading_index >= 0.0f &&
                        fading_index <= 0.05f;
                    port_->setCarrierSenseNoiseFloorSampleCeiling(
                        low_fading_frame
                            ? carrierSenseQamNoiseFloorSampleCeiling()
                            : 0.0f);
                }
            }
            if (use_quality_sample) {
                protocol_.setChannelQuality(snr_db_, fading_index);
            }
            protocol_.onRxData(result.frame_data);
            if (use_quality_sample) {
                updateAdaptiveAdvisory(snr_db_, fading_index);
            }
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
        if (connected_.load() && encoder_ && mode == WaveformMode::OFDM_CHIRP) {
            encoder_->forceNextFrameFullPreamble();
        }

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
        if (port_) {
            port_->setCarrierSenseNoiseFloorSampleCeiling(
                carrierSenseNoiseFloorSampleCeilingForMode(mod));
        }

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
        syncBurstInterleaveForConnectedMode();
        syncAdaptiveShortReanchorForConnectedMode();

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
                    if (negotiated_waveform_ == WaveformMode::OFDM_CHIRP) {
                        decoder_->expectFullOFDMAnchorOnce();
                    }
                }
                if (encoder_ && negotiated_waveform_ == WaveformMode::OFDM_CHIRP) {
                    encoder_->forceNextFrameFullPreamble();
                }
                syncBurstInterleaveForConnectedMode();
                syncAdaptiveShortReanchorForConnectedMode();
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
            burst_interleave_active_ = false;
            adaptive_preamble_peer_fading_ = -1.0f;
            syncAdaptiveShortReanchorForConnectedMode();
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
        auto saved_mc_dpsk_config = mc_dpsk_config_;

        if (is_handshake_frame) {
            // Handshake frames must use the cold-call control profile until the
            // initiator has decoded CONNECT_ACK and can switch to DATA mode.
            encoder_->setMCDPSKConfig(control_mc_dpsk_config_);
            encoder_->setMode(WaveformMode::MC_DPSK);
            encoder_->setDataMode(mcDpskModulationForConfig(control_mc_dpsk_config_), CodeRate::R1_4);
        }

        const auto header = v2::parseHeader(data);
        const bool is_data_frame = header.valid && v2::isDataFrame(header.type);

        // Encode frame using the encoder.
        // MC-DPSK: always full preamble.
        // OFDM controls: light preamble after handshake.
        // OFDM single DATA turns: full preamble so the peer can re-anchor after
        // idle or after an ACK/NACK turnaround.
        std::vector<float> result;
        bool is_mc_dpsk = (encoder_->getMode() == WaveformMode::MC_DPSK);
        bool use_light = !is_handshake_frame && !is_mc_dpsk &&
                         connected_.load() && handshake_complete_.load() &&
                         !is_data_frame;

        if (use_light) {
            result = encoder_->encodeFrameLight(data);
        } else {
            result = encoder_->encodeFrame(data);
        }

        if (is_mc_dpsk && !is_handshake_frame) {
            constexpr int kGuardSamples =
                SAMPLE_RATE * connection_policy::kMCDPSKInterFrameGuardMs / 1000;
            result.insert(result.end(), kGuardSamples, 0.0f);
        }

        // Restore encoder mode if we changed it
        if (is_handshake_frame) {
            encoder_->setMCDPSKConfig(saved_mc_dpsk_config);
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

    void notePttTxQueued(size_t sample_count) {
        tx_continuation_grace_samples_.store(0, std::memory_order_relaxed);
        post_tx_ack_listen_samples_.store(0, std::memory_order_relaxed);
        ptt_.noteTxQueued(sample_count);
    }

    void markPacedTxSubmissionQueued() {
        tx_continuation_grace_samples_.store(0, std::memory_order_relaxed);
    }

    void notePttTxDrained(size_t drained_samples) {
        const PttState before = ptt_.state();
        ptt_.noteTxDrained(drained_samples);
        const PttState after = ptt_.state();
        if (drained_samples > 0 && before == PttState::TX && after != PttState::TX) {
            tx_continuation_grace_samples_.store(
                samplesForMs(TX_CONTINUATION_GRACE_MS),
                std::memory_order_relaxed);
        }
    }

    void notePttTxSampleBlock(bool tx_active, bool tx_draining_block = false) {
        const PttState before = ptt_.state();
        ptt_.noteTxSampleBlock(tx_active, tx_draining_block);
        const PttState after = ptt_.state();
        if (tx_active || tx_draining_block) {
            tx_continuation_grace_samples_.store(0, std::memory_order_relaxed);
            post_tx_ack_listen_samples_.store(0, std::memory_order_relaxed);
        } else if (before == PttState::TX && after != PttState::TX) {
            tx_continuation_grace_samples_.store(
                samplesForMs(TX_CONTINUATION_GRACE_MS),
                std::memory_order_relaxed);
        }
    }

    void advancePttRecovery(size_t elapsed_samples) {
        const PttState before = ptt_.state();
        ptt_.advanceSamples(elapsed_samples);
        const PttState after = ptt_.state();
        advanceTxContinuationGrace(elapsed_samples);
        if (before != PttState::RX && after == PttState::RX) {
            post_tx_ack_listen_samples_.store(
                samplesForMs(POST_TX_ACK_LISTEN_MS),
                std::memory_order_relaxed);
        } else {
            advancePostTxAckListen(elapsed_samples);
        }
    }

    void setupCallbacks() {
        // TX callback - enqueue a logical frame. The audio loop owns the
        // soundcard clock and pulls encoded samples from the active TX cursor.
        protocol_.setTxDataCallback([this](const Bytes& data,
                                           bool expect_full_ofdm_anchor_after_tx) {
            queueTxFrame(data, txFrameLabel(data), expect_full_ofdm_anchor_after_tx);
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
                                                    float peer_snr_db, float peer_fading,
                                                    int mc_dpsk_num_carriers,
                                                    int mc_dpsk_samples_per_symbol) {
            adaptive_preamble_peer_fading_ = peer_fading;
            applyNegotiatedMCDPSKConfig(mod, mc_dpsk_num_carriers,
                                        mc_dpsk_samples_per_symbol);
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
            queueTxBurst(frames, txBurstLabel(frames));
        });

        // PING/PONG
        protocol_.setPingTxCallback([this]() {
            queueTxPing();
        });

        protocol_.setPingReceivedCallback([this]() {
            queueTxPong();
        });

        // Message received
        protocol_.setMessageReceivedCallback([this](const std::string&, const std::string& text) {
            if (message_callback_) {
                message_callback_(text);
            }
        });

        // File received
        protocol_.setFileReceivedCallback([this](const std::string& filepath, bool success,
                                                 const std::string&) {
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

    static bool requiresFreshCarrierSense(const std::string& label) {
        return label.find("frame_type=CONNECT") != std::string::npos;
    }

    static bool isDeferredArqAck(const std::string& label) {
        return label.find("frame_type=ACK ") != std::string::npos &&
               label.find(" seq=65535") == std::string::npos;
    }

    void queueTxFrame(const Bytes& frame,
                      const std::string& label,
                      bool expect_full_ofdm_anchor_after_tx = false) {
        TxSubmission submission;
        submission.kind = TxSubmission::Kind::Frame;
        submission.frame = frame;
        submission.label = label;
        submission.expect_full_ofdm_anchor_after_tx = expect_full_ofdm_anchor_after_tx;
        queueTxSubmission(std::move(submission));
    }

    void queueTxBurst(const std::vector<Bytes>& frames, const std::string& label) {
        if (frames.empty()) {
            return;
        }
        TxSubmission submission;
        submission.kind = TxSubmission::Kind::Burst;
        submission.burst = frames;
        submission.label = label;
        queueTxSubmission(std::move(submission));
    }

    void queueTxPing() {
        TxSubmission submission;
        submission.kind = TxSubmission::Kind::Ping;
        submission.label = "frame_type=PING";
        queueTxSubmission(std::move(submission));
    }

    void queueTxPong() {
        TxSubmission submission;
        submission.kind = TxSubmission::Kind::Pong;
        submission.label = "frame_type=PONG";
        queueTxSubmission(std::move(submission));
    }

    void queueTx(const std::vector<float>& samples, const std::string& label = std::string()) {
        if (samples.empty()) {
            return;
        }

        TxSubmission submission;
        submission.kind = TxSubmission::Kind::RawSamples;
        submission.raw_samples = samples;
        submission.label = label;
        queueTxSubmission(std::move(submission));
    }

    void queueTxSubmission(TxSubmission submission) {
        if (txSubmissionEmpty(submission)) {
            return;
        }

        if (requiresFreshCarrierSense(submission.label)) {
            deferTxSubmission(std::move(submission), true);
            return;
        }

        if (pttState() == PttState::RX && hasDeferredTxSubmission()) {
            deferTxSubmission(std::move(submission));
            flushDeferredTxIfReady();
            return;
        }

        if (pttState() == PttState::RX && !channelIdleForTxGuard()) {
            deferTxSubmission(std::move(submission));
            return;
        }

        if (!canAcceptTxSubmission()) {
            deferTxSubmission(std::move(submission));
            return;
        }
        submitTxNow(std::move(submission));
    }

    bool canAcceptTxSubmission() {
        const PttState state = pttState();
        if (txContinuationGraceActive() && !hasLocalTxQueued()) {
            return true;
        }
        if (postTxAckListenActive()) {
            return false;
        }
        return state == PttState::RX && !hasLocalTxQueued();
    }

    bool txContinuationGraceActive() const {
        return tx_continuation_grace_samples_.load(std::memory_order_relaxed) > 0;
    }

    bool postTxAckListenActive() const {
        return post_tx_ack_listen_samples_.load(std::memory_order_relaxed) > 0;
    }

    bool protocolTimersCanAdvance() {
        // The ARQ clock must measure time spent able to hear the peer's reply,
        // not time spent draining our own audio queue. Otherwise a legal ACK can
        // race a timeout retransmission at the first RX-open callback.
        return ptt_.isReadyForNextTx() &&
               !hasLocalTxQueued() &&
               !txContinuationGraceActive() &&
               !postTxAckListenActive();
    }

    bool hasDeferredTxSubmission() {
        std::lock_guard<std::mutex> lock(deferred_tx_mutex_);
        return !deferred_tx_submissions_.empty();
    }

    bool hasLocalTxQueued() {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        return active_tx_.hasRemaining() || !tx_submissions_.empty() || tx_job_starting_;
    }

    static uint64_t samplesForMs(uint32_t ms) {
        return (static_cast<uint64_t>(SAMPLE_RATE) * static_cast<uint64_t>(ms)) /
               1000ULL;
    }

    void seedWarmSyncReplyPrediction(size_t tx_samples) {
        if (!decoder_ || tx_samples == 0) {
            return;
        }
        const size_t delay = tx_samples +
            static_cast<size_t>(samplesForMs(tx_turnaround_guard_ms_));
        decoder_->seedExpectedFrameArrivalAfterSamples(delay);
    }

    void advanceTxContinuationGrace(size_t elapsed_samples) {
        uint64_t remaining =
            tx_continuation_grace_samples_.load(std::memory_order_relaxed);
        while (remaining > 0) {
            const uint64_t next = elapsed_samples >= remaining
                ? 0
                : remaining - static_cast<uint64_t>(elapsed_samples);
            if (tx_continuation_grace_samples_.compare_exchange_weak(
                    remaining, next, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    void advancePostTxAckListen(size_t elapsed_samples) {
        uint64_t remaining =
            post_tx_ack_listen_samples_.load(std::memory_order_relaxed);
        while (remaining > 0) {
            const uint64_t next = elapsed_samples >= remaining
                ? 0
                : remaining - static_cast<uint64_t>(elapsed_samples);
            if (post_tx_ack_listen_samples_.compare_exchange_weak(
                    remaining, next, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    static bool txSubmissionEmpty(const TxSubmission& submission) {
        switch (submission.kind) {
            case TxSubmission::Kind::Frame:
                return submission.frame.empty();
            case TxSubmission::Kind::Burst:
                return submission.burst.empty();
            case TxSubmission::Kind::RawSamples:
                return submission.raw_samples.empty();
            case TxSubmission::Kind::Ping:
            case TxSubmission::Kind::Pong:
                return false;
        }
        return true;
    }

    static size_t txSubmissionSizeHint(const TxSubmission& submission) {
        switch (submission.kind) {
            case TxSubmission::Kind::Frame:
                return submission.frame.size();
            case TxSubmission::Kind::Burst:
                return submission.burst.size();
            case TxSubmission::Kind::RawSamples:
                return submission.raw_samples.size();
            case TxSubmission::Kind::Ping:
            case TxSubmission::Kind::Pong:
                return 0;
        }
        return 0;
    }

    size_t pendingTxSampleEstimateLocked() const {
        size_t total = active_tx_.remaining();
        for (const auto& submission : tx_submissions_) {
            if (submission.kind == TxSubmission::Kind::RawSamples) {
                total += submission.raw_samples.size();
            }
        }
        return total;
    }

    void deferTxSubmission(TxSubmission submission,
                           bool require_fresh_rx_observation = false) {
        bool rejected = false;
        size_t depth = 0;
        size_t coalesced_acks = 0;
        uint64_t event = 0;
        const size_t size_hint = txSubmissionSizeHint(submission);
        const std::string label = submission.label;
        const uint64_t min_rx_observation_epoch =
            rx_observation_epoch_.load(std::memory_order_relaxed) +
            (require_fresh_rx_observation ? 1ULL : 0ULL);
        {
            std::lock_guard<std::mutex> lock(deferred_tx_mutex_);
            if (isDeferredArqAck(label)) {
                for (auto it = deferred_tx_submissions_.begin();
                     it != deferred_tx_submissions_.end();) {
                    if (isDeferredArqAck(it->label)) {
                        it = deferred_tx_submissions_.erase(it);
                        coalesced_acks++;
                    } else {
                        ++it;
                    }
                }
            }
            if (deferred_tx_submissions_.size() >= kMaxDeferredTxSubmissions) {
                rejected = true;
                event = ++rejected_tx_count_;
            } else {
                submission.min_rx_observation_epoch = min_rx_observation_epoch;
                deferred_tx_submissions_.push_back(std::move(submission));
                depth = deferred_tx_submissions_.size();
                event = ++deferred_tx_count_;
            }
        }

        if (coalesced_acks > 0) {
            LOG_MODEM(INFO,
                      "[%s] Coalesced %zu stale deferred ACK/SACK submission(s) before %s",
                      callsign_.c_str(),
                      coalesced_acks,
                      label.empty() ? "-" : label.c_str());
        }

        if (rejected) {
            if (event <= 4 || (event % 32) == 0) {
                LOG_MODEM(WARN,
                          "[%s] TX rejected while radio recovery queue is full "
                          "(state=%s size_hint=%zu label=%s rejected=%llu)",
                          callsign_.c_str(), pttStateName(pttState()),
                          size_hint,
                          label.empty() ? "-" : label.c_str(),
                          static_cast<unsigned long long>(event));
            }
            if (ultra::phyDiagnosticsEnabled()) {
                std::ostringstream oss;
                oss << "event=station_tx_reject"
                    << " station=" << callsign_
                    << " ptt=" << pttStateName(pttState())
                    << " size_hint=" << size_hint
                    << " label=" << (label.empty() ? "-" : label)
                    << " rejected=" << event;
                ultra::phyDiagLine(oss.str());
            }
            return;
        }

        if (event <= 8 || (event % 32) == 0) {
            LOG_MODEM(INFO,
                      "[%s] TX deferred until radio RX (state=%s samples=%zu "
                      "depth=%zu label=%s deferred=%llu)",
                      callsign_.c_str(), pttStateName(pttState()),
                      size_hint, depth,
                      label.empty() ? "-" : label.c_str(),
                      static_cast<unsigned long long>(event));
        }
        if (ultra::phyDiagnosticsEnabled()) {
            std::ostringstream oss;
            oss << "event=station_tx_defer"
                << " station=" << callsign_
                << " ptt=" << pttStateName(pttState())
                << " size_hint=" << size_hint
                << " depth=" << depth
                << " min_rx_epoch=" << min_rx_observation_epoch
                << " coalesced_acks=" << coalesced_acks
                << " label=" << (label.empty() ? "-" : label);
            ultra::phyDiagLine(oss.str());
        }
    }

    void flushDeferredTxIfReady() {
        if (!ptt_.isReadyForNextTx()) {
            if (csDebugEnabled() && hasDeferredTxSubmission()) {
                LOG_MODEM(WARN, "[%s] flushDeferred blocked: ptt=%s (not RX yet)",
                          callsign_.c_str(), pttStateName(pttState()));
            }
            return;
        }
        if (postTxAckListenActive()) {
            if (csDebugEnabled() && hasDeferredTxSubmission()) {
                LOG_MODEM(WARN, "[%s] flushDeferred blocked: post-TX ACK listen window",
                          callsign_.c_str());
            }
            return;
        }
        if (hasLocalTxQueued()) {
            if (csDebugEnabled() && hasDeferredTxSubmission()) {
                LOG_MODEM(WARN, "[%s] flushDeferred blocked: local TX queue not empty",
                          callsign_.c_str());
            }
            return;
        }
        if (!channelIdleForTxGuard()) {
            if (csDebugEnabled() && hasDeferredTxSubmission()) {
                LOG_MODEM(WARN, "[%s] flushDeferred blocked: carrier sense (channel busy)",
                          callsign_.c_str());
            }
            return;
        }

        while (true) {
            TxSubmission submission;
            {
                std::lock_guard<std::mutex> lock(deferred_tx_mutex_);
                if (deferred_tx_submissions_.empty()) {
                    return;
                }
                if (deferred_tx_submissions_.front().min_rx_observation_epoch >
                    rx_observation_epoch_.load(std::memory_order_relaxed)) {
                    return;
                }
                submission = std::move(deferred_tx_submissions_.front());
                deferred_tx_submissions_.pop_front();
            }

            if (isStaleDeferredTxSubmission(submission)) {
                LOG_MODEM(INFO,
                          "[%s] Dropping stale deferred TX: %s",
                          callsign_.c_str(),
                          submission.label.empty() ? "-" : submission.label.c_str());
                continue;
            }

            submitTxNow(std::move(submission));
            return;
        }
    }

    bool isStaleDeferredTxSubmission(const TxSubmission& submission) const {
        return handshake_complete_.load(std::memory_order_relaxed) &&
               submission.label.find("frame_type=CONNECT_ACK") != std::string::npos;
    }

    bool channelIdleForTxGuard() const {
        return !port_ || port_->isChannelIdleFor(tx_turnaround_guard_ms_);
    }

    std::vector<float> encodeTxSubmission(const TxSubmission& submission) {
        switch (submission.kind) {
            case TxSubmission::Kind::Frame:
                return transmitFrame(submission.frame);
            case TxSubmission::Kind::Burst:
                return transmitBurst(submission.burst);
            case TxSubmission::Kind::Ping:
                return transmitPing();
            case TxSubmission::Kind::Pong:
                return transmitPong();
            case TxSubmission::Kind::RawSamples:
                return submission.raw_samples;
        }
        return {};
    }

    void normalizeTxSubmissionIfNeeded(std::vector<float>& samples,
                                       const TxSubmission& submission) const {
        if (!port_ || !port_->requiresTxBurstNormalization() ||
            submission.kind == TxSubmission::Kind::RawSamples ||
            samples.empty()) {
            return;
        }

        const auto measurement = ultra::sim::normalizeTxBurstToReference(samples);
        if (measurement.peak_warning || measurement.peak_clip_error) {
            LOG_MODEM(WARN,
                      "[%s] TX burst normalization %s peak_after_gain=%.3f "
                      "clip_samples=%zu gain=%.3f active=%zu in_band_rms=%.6f%s",
                      callsign_.c_str(),
                      submission.label.empty() ? "-" : submission.label.c_str(),
                      measurement.peak_after_gain,
                      measurement.peak_clip_samples,
                      measurement.gain_to_reference,
                      measurement.active_samples,
                      measurement.in_band_rms,
                      measurement.peak_clip_error ? " CLIP_EXPECTED" : "");
        }
    }

    bool ensureActiveTx() {
        {
            std::lock_guard<std::mutex> lock(tx_mutex_);
            if (active_tx_.hasRemaining() || tx_job_starting_) {
                return active_tx_.hasRemaining();
            }
            if (tx_submissions_.empty()) {
                return false;
            }
            tx_job_starting_ = true;
        }

        TxSubmission submission;
        {
            std::lock_guard<std::mutex> lock(tx_mutex_);
            submission = std::move(tx_submissions_.front());
            tx_submissions_.pop_front();
        }

        const std::string label = submission.label;
        auto samples = encodeTxSubmission(submission);
        normalizeTxSubmissionIfNeeded(samples, submission);
        seedWarmSyncReplyPrediction(samples.size());
        if (submission.expect_full_ofdm_anchor_after_tx && decoder_) {
            decoder_->expectFullOFDMAnchorOnce();
        }

        std::lock_guard<std::mutex> lock(tx_mutex_);
        tx_job_starting_ = false;
        if (samples.empty()) {
            return false;
        }

        const uint64_t start_sample = tx_sample_clock_;
        const uint64_t end_sample = start_sample + samples.size();
        active_tx_.samples = std::move(samples);
        active_tx_.offset = 0;
        active_tx_.label = label;

        if (!label.empty()) {
            LOG_MODEM(INFO,
                      "[%s] TX active cursor: %s start_sample=%llu end_sample=%llu samples=%zu",
                      callsign_.c_str(), label.c_str(),
                      static_cast<unsigned long long>(start_sample),
                      static_cast<unsigned long long>(end_sample),
                      active_tx_.samples.size());
        }
        if (ultra::phyDiagnosticsEnabled()) {
            std::ostringstream diag;
            diag << "event=station_tx_active"
                 << " station=" << callsign_
                 << " start_sample=" << start_sample
                 << " end_sample=" << end_sample
                 << " samples=" << active_tx_.samples.size()
                 << " sim_t=" << getSimTime()
                 << " ptt=" << pttStateName(pttState())
                 << " label=" << (label.empty() ? "-" : label);
            ultra::phyDiagLine(diag.str());
        }
        return active_tx_.hasRemaining();
    }

    size_t copyActiveTxSamples(float* out, size_t count, std::string& label) {
        if (out == nullptr || count == 0) {
            return 0;
        }

        std::lock_guard<std::mutex> lock(tx_mutex_);
        if (!active_tx_.hasRemaining()) {
            return 0;
        }

        const size_t n = std::min(count, active_tx_.remaining());
        std::copy(active_tx_.samples.begin() + static_cast<std::ptrdiff_t>(active_tx_.offset),
                  active_tx_.samples.begin() + static_cast<std::ptrdiff_t>(active_tx_.offset + n),
                  out);
        if (label.empty()) {
            label = active_tx_.label;
        }
        active_tx_.offset += n;
        if (!active_tx_.hasRemaining()) {
            active_tx_.clear();
        }
        return n;
    }

    void submitTxNow(TxSubmission submission) {
        const std::string label = submission.label;
        if (port_ && !port_->shouldPaceTxInStationLoop()) {
            auto samples = encodeTxSubmission(submission);
            normalizeTxSubmissionIfNeeded(samples, submission);
            if (samples.empty()) {
                return;
            }
            seedWarmSyncReplyPrediction(samples.size());
            if (submission.expect_full_ofdm_anchor_after_tx && decoder_) {
                decoder_->expectFullOFDMAnchorOnce();
            }
            notePttTxQueued(samples.size());
            {
                std::lock_guard<std::mutex> lock(tx_mutex_);
                tx_emitted_sample_clock_ += samples.size();
            }
            port_->queueTx(samples);
            std::ostringstream oss;
            oss << "station_tx_direct station=" << callsign_
                << " sim_t=" << getSimTime()
                << " samples=" << samples.size()
                << " rms=" << sampleRms(samples)
                << " ptt=" << pttStateName(pttState())
                << " label=" << (label.empty() ? "-" : label);
            e2eDebugLine(oss.str());
            if (!label.empty()) {
                LOG_MODEM(INFO, "[%s] TX direct: %s samples=%zu",
                          callsign_.c_str(), label.c_str(), samples.size());
            }
            if (ultra::phyDiagnosticsEnabled()) {
                std::ostringstream diag;
                diag << "event=station_tx_submit"
                     << " station=" << callsign_
                     << " path=direct"
                     << " samples=" << samples.size()
                     << " sim_t=" << getSimTime()
                     << " ptt=" << pttStateName(pttState())
                     << " label=" << (label.empty() ? "-" : label);
                ultra::phyDiagLine(diag.str());
            }
            return;
        }

        std::lock_guard<std::mutex> lock(tx_mutex_);
        const size_t queued_before = tx_submissions_.size() +
            (active_tx_.hasRemaining() ? 1U : 0U) + (tx_job_starting_ ? 1U : 0U);
        markPacedTxSubmissionQueued();
        tx_submissions_.push_back(std::move(submission));
        if (!label.empty()) {
            LOG_MODEM(INFO,
                      "[%s] TX logical queue: %s queued_before=%zu logical_depth=%zu",
                      callsign_.c_str(), label.c_str(),
                      queued_before, tx_submissions_.size());
        }
        if (ultra::phyDiagnosticsEnabled()) {
            std::ostringstream diag;
            diag << "event=station_tx_submit"
                 << " station=" << callsign_
                 << " path=paced_queue"
                 << " queued_before=" << queued_before
                 << " logical_depth=" << tx_submissions_.size()
                 << " sim_t=" << getSimTime()
                 << " ptt=" << pttStateName(pttState())
                 << " label=" << (label.empty() ? "-" : label);
            ultra::phyDiagLine(diag.str());
        }
    }

    // THE AUDIO LOOP - like a real sound card callback
    void audioLoop() {
        ultra::setLogStationTag(callsign_.c_str());
        auto next_callback = std::chrono::steady_clock::now();
        int callback_count = 0;

        while (running_) {
            // ===== AUDIO CALLBACK START =====
            advancePttRecovery(SAMPLES_PER_CALLBACK);
            flushDeferredTxIfReady();

            // 1. GET TX SAMPLES - the radio/modem interface is pull-clocked
            // by this simulated soundcard callback.
            // HardwareAudioPort bypasses this simulated pacer because SDL's
            // output callback is already the hardware clock.
            std::vector<float> tx_samples(SAMPLES_PER_CALLBACK, 0.0f);
            size_t tx_pending = 0;
            size_t tx_drained_samples = 0;
            bool tx_active_block = false;
            bool tx_draining_block = false;
            const bool pace_tx = !port_ || port_->shouldPaceTxInStationLoop();
            if (pace_tx) {
                auto tx_result = pullTxSamples(tx_samples.data(), tx_samples.size());
                tx_pending = tx_result.audio_chain_pending_samples;
                tx_drained_samples = tx_result.emitted_samples;
                tx_active_block = tx_result.tx_active;
                tx_draining_block = tx_result.tx_draining;
            }

            // 2. READ RX - get samples from audio port (virtual channel or soundcard).
            // RX blackout follows samples leaving the local audio port. A silent
            // block inside a queued waveform still keeps PTT keyed; the blackout
            // ends on the first callback after the waveform drain finishes.
            std::vector<float> rx_samples = port_
                ? port_->pullRx(SAMPLES_PER_CALLBACK)
                : std::vector<float>(SAMPLES_PER_CALLBACK, 0.0f);
            rx_observation_epoch_.fetch_add(1, std::memory_order_relaxed);

            // 3. FEED TO DECODER (audio thread only buffers - decode thread processes)
            pushRxSamples(rx_samples.data(), rx_samples.size());

            if (pace_tx) {
                // 4. SEND TX TO AUDIO PORT (virtual channel only; hardware TX
                // is queued directly in queueTx() above)
                if (port_) {
                    port_->queueTx(tx_samples);
                    if (tx_drained_samples > 0 || callsign_ == "BRAVO") {
                        std::ostringstream oss;
                        oss << "station_tx_to_port station=" << callsign_
                            << " sim_t=" << getSimTime()
                            << " drained=" << tx_drained_samples
                            << " tx_pending_before=" << tx_pending
                            << " tx_clock=" << tx_sample_clock_
                            << " rms=" << sampleRms(tx_samples)
                            << " tx_active=" << (tx_active_block ? 1 : 0)
                            << " tx_draining=" << (tx_draining_block ? 1 : 0)
                            << " ptt=" << pttStateName(pttState());
                        e2eDebugLine(oss.str());
                    }
                }
            } else {
                tx_drained_samples = SAMPLES_PER_CALLBACK;
                notePttTxDrained(tx_drained_samples);
            }
            flushDeferredTxIfReady();
            if (callsign_ == "ALPHA" || callsign_ == "BRAVO") {
                std::ostringstream oss;
                oss << "station_tick station=" << callsign_
                    << " sim_t=" << getSimTime()
                    << " rx_rms=" << sampleRms(rx_samples)
                    << " tx_drained=" << tx_drained_samples
                    << " tx_pending_before=" << tx_pending
                    << " tx_active=" << (tx_active_block ? 1 : 0)
                    << " tx_draining=" << (tx_draining_block ? 1 : 0)
                    << " ptt_after=" << pttStateName(pttState())
                    << " blackout=" << (isInRxBlackout() ? 1 : 0);
                e2eDebugLine(oss.str());
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

inline bool AudioPort::attachedRadioInRxBlackout() const {
    return attached_ptt_ && attached_ptt_->isInRxBlackout();
}

inline std::vector<float> AudioPort::shapeRxForLocalRadio(
    std::vector<float> samples,
    size_t count) const {
    const bool rx_blackout = attachedRadioInRxBlackout();
    const auto now = carrierSenseNow();
    if (rx_blackout) {
        channel_busy_detector_.observeRms(0.0f, true, now);
        return std::vector<float>(count, 0.0f);
    }
    samples.resize(count, 0.0f);
    channel_busy_detector_.observeSamples(samples, false, now);
    return samples;
}
