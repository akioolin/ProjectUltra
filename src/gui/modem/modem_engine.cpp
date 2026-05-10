// ModemEngine - Main implementation
// Constructor, destructor, configuration, and TX functions

#include "modem_engine.hpp"
#include "gui/startup_trace.hpp"
#include "ultra/logging.hpp"
#include <cstring>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace ultra {
namespace gui {

namespace {
Modulation mcDpskModulationForConfig(const MultiCarrierDPSKConfig& config) {
    if (config.bits_per_symbol == 1) return Modulation::DBPSK;
    if (config.bits_per_symbol == 3) return Modulation::D8PSK;
    return Modulation::DQPSK;
}
}

ModemEngine::ModemEngine()
    : ModemEngine(mc_dpsk_presets::robust_mid()) {
}

ModemEngine::ModemEngine(const MultiCarrierDPSKConfig& mc_dpsk_config) {
    startupTrace("ModemEngine", "ctor-enter");
    config_ = presets::balanced();
    startupTrace("ModemEngine", "presets-balanced");

    // CRITICAL: Disable pilots for DQPSK mode - uses all 30 carriers for data
    // This doubles throughput (30 data carriers vs 15 with pilots)
    // DQPSK is differential and doesn't need pilots for channel estimation
    config_.use_pilots = false;

    decoder_ = fec::CodecFactory::create(fec::CodecType::LDPC, CodeRate::R1_4);
    // NOTE: TX uses streaming_encoder_, RX uses streaming_decoder_

    // DPSK config (used by StreamingDecoder, kept here for API compatibility)
    dpsk_config_ = dpsk_presets::medium();

    // Chirp sync for robust presence detection on fading channels
    // Dual chirp (up + down) enables CFO estimation via radar technique
    sync::ChirpConfig chirp_cfg;
    chirp_cfg.sample_rate = config_.sample_rate;
    chirp_cfg.f_start = 300.0f;     // Start frequency (Hz)
    chirp_cfg.f_end = 2700.0f;      // End frequency (Hz)
    chirp_cfg.duration_ms = 500.0f; // 500ms per chirp (up + down = 1.0s chirps + gaps)
    chirp_cfg.gap_ms = 100.0f;      // Gap between up and down chirps
    chirp_cfg.use_dual_chirp = true; // Enable dual chirp for CFO estimation
    chirp_cfg.tx_cfo_hz = config_.tx_cfo_hz;  // Pass TX CFO for simulation
    chirp_sync_ = std::make_unique<sync::ChirpSync>(chirp_cfg);
    startupTrace("ModemEngine", "chirp-sync-created");

    // Multi-Carrier DPSK (for fading channels - frequency diversity)
    // Default cold-call PHY is Robust-Mid: 8 carriers, 46.875 baud, DBPSK.
    // Test presets can override samples_per_symbol and DBPSK at construction.
    // 8 carriers is more robust than 13 at low SNR with CFO (tested: 100% vs 40% at moderate fading)
    // IMPORTANT: Sync chirp config with modem's chirp_sync_ so TX and RX use same chirp
    mc_dpsk_config_ = mc_dpsk_config;
    mc_dpsk_config_.chirp_f_start = chirp_cfg.f_start;
    mc_dpsk_config_.chirp_f_end = chirp_cfg.f_end;
    mc_dpsk_config_.chirp_duration_ms = chirp_cfg.duration_ms;
    mc_dpsk_config_.use_dual_chirp = chirp_cfg.use_dual_chirp;
    // Note: Actual MC-DPSK modulation is done by IWaveform via StreamingDecoder

    // Initialize StreamingEncoder (unified TX path)
    streaming_encoder_ = std::make_unique<StreamingEncoder>();
    startupTrace("ModemEngine", "streaming-encoder-created");
    streaming_encoder_->setOFDMConfig(config_);
    streaming_encoder_->setMCDPSKConfig(mc_dpsk_config_);
    streaming_encoder_->setDataMode(mcDpskModulationForConfig(mc_dpsk_config_), CodeRate::R1_4);
    startupTrace("ModemEngine", "streaming-encoder-configured");

    // Initialize audio filters
    rebuildFilters();
    startupTrace("ModemEngine", "filters-built");

    // ========================================================================
    // Initialize StreamingDecoder (primary RX path)
    // ========================================================================
    streaming_decoder_ = std::make_unique<StreamingDecoder>();
    startupTrace("ModemEngine", "streaming-decoder-created");
    startupTrace("ModemEngine", "decoder-set-log-prefix-skip");

    // Set callbacks to wire into existing ModemEngine callbacks
    startupTrace("ModemEngine", "decoder-set-frame-callback-enter");
    streaming_decoder_->setFrameCallback([this](const DecodeResult& result) {
        // Update SNR/sync stats before delivering frame so downstream callbacks
        // (ProtocolEngine via raw_data_callback_) read fresh channel estimates.
        updateStats([&](LoopbackStats& s) {
            s.snr_db = result.snr_db;
            s.synced = result.success;
        });

        if (result.success && !result.frame_data.empty()) {
            deliverFrame(result.frame_data);
            notifyFrameParsed(result.frame_data, result.frame_type);
        }
        // Save peer CFO for future frames
        if (std::abs(result.cfo_hz) > 0.1f) {
            peer_cfo_hz_ = result.cfo_hz;
        }
        last_rx_complete_time_ = std::chrono::steady_clock::now();
    });
    startupTrace("ModemEngine", "decoder-set-frame-callback-exit");

    startupTrace("ModemEngine", "decoder-set-ping-callback-enter");
    streaming_decoder_->setPingCallback([this](float snr_db, float cfo_hz) {
        // If narrowband chirp detected, switch control waveform for PONG/CONNECT_ACK
        if (streaming_decoder_->getDetectedBandwidth() == BandwidthMode::NARROW) {
            if (streaming_encoder_) {
                streaming_encoder_->setNarrowbandControl(true);
            }
            LOG_MODEM(INFO, "Narrowband chirp detected, switching control waveform");
        }
        if (ping_received_callback_) {
            ping_received_callback_(snr_db);
        }
        updateStats([&](LoopbackStats& s) {
            s.frames_received++;
            s.snr_db = snr_db;
            s.synced = true;
        });
        last_rx_complete_time_ = std::chrono::steady_clock::now();
    });
    startupTrace("ModemEngine", "decoder-set-ping-callback-exit");

    // Sync StreamingDecoder with initial waveform mode
    // When disconnected, use MC_DPSK for PING detection (chirp-based sync)
    // When connected, use the negotiated waveform
    protocol::WaveformMode decoder_mode = connected_ ? waveform_mode_ : protocol::WaveformMode::MC_DPSK;
    startupTrace("ModemEngine", "decoder-set-mode-enter");
    if (connected_ || decoder_mode != protocol::WaveformMode::MC_DPSK) {
        streaming_decoder_->setMode(decoder_mode, connected_);
        startupTrace("ModemEngine", "decoder-mode-set");
    } else {
        // StreamingDecoder ctor already initializes MC_DPSK disconnected defaults.
        startupTrace("ModemEngine", "decoder-mode-skip-default");
    }

    // Sync MC-DPSK carrier count with ModemEngine's config
    startupTrace("ModemEngine", "decoder-set-carriers-enter");
    if (mc_dpsk_config_.num_carriers != 8 ||
        mc_dpsk_config_.samples_per_symbol != 512 ||
        mc_dpsk_config_.bits_per_symbol != 2) {
        streaming_decoder_->setMCDPSKConfig(mc_dpsk_config_);
        streaming_decoder_->setDataMode(mcDpskModulationForConfig(mc_dpsk_config_), CodeRate::R1_4);
        startupTrace("ModemEngine", "decoder-carriers-set");
    } else {
        startupTrace("ModemEngine", "decoder-carriers-skip-default");
    }

    // Defer RX decode thread startup until audio is actually fed.
    // This reduces startup-time failure surface on low-end systems.
    startupTrace("ModemEngine", "ctor-exit");
}

ModemEngine::~ModemEngine() {
    stopRxDecodeThread();
}

void ModemEngine::setLogPrefix(const std::string& prefix) {
    log_prefix_ = prefix;
    if (streaming_decoder_) {
        streaming_decoder_->setLogPrefix(prefix);
    }
}

// ============================================================================
// CONFIGURATION
// ============================================================================

void ModemEngine::setConfig(const ModemConfig& config) {
    LOG_MODEM(INFO, "setConfig called: new code_rate=%d, modulation=%d",
              static_cast<int>(config.code_rate), static_cast<int>(config.modulation));
    config_ = config;

    // BUG FIX: Don't change decoder rate here - it should stay at R1_4 for disconnected mode
    // The decoder rate should only change when setConnected(true) is called
    LOG_MODEM(INFO, "setConfig: decoder_rate=%d (decoder unchanged for disconnected)",
              static_cast<int>(decoder_->getRate()));

    // Propagate OFDM config to StreamingEncoder
    if (streaming_encoder_) {
        streaming_encoder_->setOFDMConfig(config_);
    }

    // Propagate OFDM config to StreamingDecoder for OFDM modes
    // This allows custom FFT/carrier settings (like NVIS mode with 1024 FFT)
    if (streaming_decoder_ && protocol::isOFDMMode(waveform_mode_)) {
        streaming_decoder_->setOFDMConfig(config_);
        LOG_MODEM(INFO, "setConfig: StreamingDecoder OFDM config updated (FFT=%d, carriers=%d)",
                  config_.fft_size, config_.num_carriers);
    }

    // Recreate chirp sync with new CFO setting (for simulation)
    sync::ChirpConfig chirp_cfg;
    chirp_cfg.sample_rate = config_.sample_rate;
    chirp_cfg.f_start = 300.0f;
    chirp_cfg.f_end = 2700.0f;
    chirp_cfg.duration_ms = 500.0f;  // 500ms per chirp (up + down = 1.0s chirps + gaps)
    chirp_cfg.gap_ms = 100.0f;       // Gap between up and down chirps
    chirp_cfg.use_dual_chirp = true; // Enable dual chirp for CFO estimation
    chirp_cfg.tx_cfo_hz = config_.tx_cfo_hz;
    chirp_sync_ = std::make_unique<sync::ChirpSync>(chirp_cfg);

    // Rebuild filters with new sample rate
    rebuildFilters();

    reset();
}

void ModemEngine::setFilterConfig(const FilterConfig& config) {
    filter_config_ = config;
    rebuildFilters();
}

void ModemEngine::setFilterEnabled(bool enabled) {
    filter_config_.enabled = enabled;
}

void ModemEngine::setMCDPSKConfig(const MultiCarrierDPSKConfig& config) {
    mc_dpsk_config_ = config;
    if (streaming_decoder_) {
        streaming_decoder_->setMCDPSKConfig(mc_dpsk_config_);
    }
    if (streaming_encoder_) {
        streaming_encoder_->setMCDPSKConfig(mc_dpsk_config_);
    }
    LOG_MODEM(INFO, "ModemEngine: MC-DPSK config carriers=%d sps=%d bits/sym=%d raw=%.1f bps",
              mc_dpsk_config_.num_carriers, mc_dpsk_config_.samples_per_symbol,
              mc_dpsk_config_.bits_per_symbol, mc_dpsk_config_.getRawBitRate());
}

void ModemEngine::rebuildFilters() {
    // Create bandpass filters for TX and RX
    // Use separate instances so they maintain independent state
    float sample_rate = static_cast<float>(config_.sample_rate);
    float low = filter_config_.lowFreq();
    float high = filter_config_.highFreq();
    int taps = filter_config_.taps;

    // Ensure valid frequency range
    low = std::max(50.0f, low);
    high = std::min(sample_rate / 2.0f - 50.0f, high);

    if (low < high) {
        auto filter = FIRFilter::bandpass(taps, low, high, sample_rate);
        tx_filter_ = std::make_unique<FIRFilter>(filter);
        rx_filter_ = std::make_unique<FIRFilter>(filter);
        LOG_MODEM(INFO, "Audio filters configured: %.0f-%.0f Hz, %d taps",
                  low, high, taps);
    } else {
        LOG_MODEM(WARN, "Invalid filter range: %.0f-%.0f Hz, filters disabled",
                  low, high);
        tx_filter_.reset();
        rx_filter_.reset();
    }
}

// ============================================================================
// TX: TRANSMIT
// ============================================================================

std::vector<float> ModemEngine::transmit(const std::string& text) {
    Bytes data(text.begin(), text.end());
    return transmit(data);
}

std::vector<float> ModemEngine::transmit(const Bytes& data) {
    if (data.empty()) {
        return {};
    }

    LOG_MODEM(INFO, "[%s] TX: Input %zu bytes, first 4: %02x %02x %02x %02x",
              log_prefix_.c_str(),
              data.size(),
              data.size() > 0 ? data[0] : 0,
              data.size() > 1 ? data[1] : 0,
              data.size() > 2 ? data[2] : 0,
              data.size() > 3 ? data[3] : 0);

    // ========================================================================
    // 1. Determine waveform mode (4-way decision)
    // ========================================================================
    protocol::WaveformMode tx_waveform_mode;
    if (use_connected_waveform_once_) {
        tx_waveform_mode = disconnect_waveform_;
        LOG_MODEM(INFO, "[%s] TX: use_connected_waveform_once_ -> disconnect_waveform_=%d",
                  log_prefix_.c_str(), static_cast<int>(tx_waveform_mode));
    } else if (!connected_) {
        tx_waveform_mode = connect_waveform_;
        LOG_MODEM(INFO, "[%s] TX: NOT connected -> connect_waveform_=%d",
                  log_prefix_.c_str(), static_cast<int>(tx_waveform_mode));
    } else if (!handshake_complete_) {
        tx_waveform_mode = last_rx_waveform_;
        LOG_MODEM(INFO, "[%s] TX: Handshake mode -> last_rx_waveform_=%d",
                  log_prefix_.c_str(), static_cast<int>(tx_waveform_mode));
    } else {
        tx_waveform_mode = waveform_mode_;
        LOG_MODEM(INFO, "[%s] TX: Connected+handshake -> waveform_mode_=%d",
                  log_prefix_.c_str(), static_cast<int>(tx_waveform_mode));
    }
    bool is_ofdm = protocol::isOFDMMode(tx_waveform_mode);

    // ========================================================================
    // 2. Determine modulation and code rate
    // ========================================================================
    Modulation tx_modulation = protocol::isOFDMMode(tx_waveform_mode)
        ? Modulation::DQPSK
        : mcDpskModulationForConfig(mc_dpsk_config_);
    CodeRate tx_code_rate = CodeRate::R1_4;

    if ((connected_ && handshake_complete_) || use_connected_waveform_once_) {
        tx_modulation = data_modulation_;
        tx_code_rate = data_code_rate_;
        LOG_MODEM(INFO, "[%s] TX: Using %s %s (%s)",
                  log_prefix_.c_str(),
                  modulationToString(tx_modulation),
                  codeRateToString(tx_code_rate),
                  connected_ ? "negotiated" : "disconnect ACK");
    }

    // Save one-shot flag before clearing (needed for light preamble decision below)
    bool is_disconnect_ack = use_connected_waveform_once_;
    if (use_connected_waveform_once_) {
        use_connected_waveform_once_ = false;
    }

    // ========================================================================
    // 3. Configure StreamingEncoder and encode
    // ========================================================================
    streaming_encoder_->setMode(tx_waveform_mode);
    streaming_encoder_->setDataMode(tx_modulation, tx_code_rate);

    // Use light preamble (LTS only) when the remote station expects it:
    // - Normal connected mode: connected_ && handshake_complete_
    // - Disconnect ACK: remote is still connected and looking for LTS sync, not chirp
    bool use_light = ((connected_ && handshake_complete_) || is_disconnect_ack) && is_ofdm;
    auto samples = use_light ? streaming_encoder_->encodeFrameLight(data)
                             : streaming_encoder_->encodeFrame(data);

    if (samples.empty()) {
        LOG_MODEM(ERROR, "[%s] TX: StreamingEncoder returned empty samples", log_prefix_.c_str());
        return {};
    }

    LOG_MODEM(INFO, "[%s] TX: %zu bytes -> %zu samples (%s, %s, %s preamble)",
              log_prefix_.c_str(), data.size(), samples.size(),
              protocol::waveformModeToString(tx_waveform_mode),
              modulationToString(tx_modulation),
              use_light ? "light" : "full");

    return postProcessTx(samples);
}

// ============================================================================
// PING/PONG PROBE (minimal presence check)
// ============================================================================

std::vector<float> ModemEngine::transmitBurst(const std::vector<Bytes>& frame_data_list) {
    if (frame_data_list.empty()) return {};

    // Burst mode is only for connected OFDM mode
    streaming_encoder_->setMode(waveform_mode_);
    streaming_encoder_->setDataMode(data_modulation_, data_code_rate_);

    auto samples = streaming_encoder_->encodeBurstLight(frame_data_list);

    if (samples.empty()) {
        LOG_MODEM(ERROR, "[%s] TX Burst: encodeBurstLight returned empty", log_prefix_.c_str());
        return {};
    }

    LOG_MODEM(INFO, "[%s] TX Burst: %zu frames -> %zu samples (%s, %s)",
              log_prefix_.c_str(), frame_data_list.size(), samples.size(),
              protocol::waveformModeToString(waveform_mode_),
              modulationToString(data_modulation_));

    return postProcessTx(samples);
}

std::vector<float> ModemEngine::transmitPing() {
    auto samples = streaming_encoder_->encodePing();

    LOG_MODEM(INFO, "[%s] TX PING (chirp): %zu samples",
              log_prefix_.c_str(), samples.size());

    return postProcessTx(samples);
}

std::vector<float> ModemEngine::transmitPong() {
    // Pong is identical to ping - context determines meaning
    // (Ping = initiator probe, Pong = responder reply)
    LOG_MODEM(INFO, "[%s] TX PONG (same as PING)", log_prefix_.c_str());
    return transmitPing();
}

// ============================================================================
// TX POST-PROCESSING (lead-in, filter, scale, stats)
// ============================================================================

std::vector<float> ModemEngine::postProcessTx(const std::vector<float>& samples) {
    // Combine lead-in + signal + tail guard
    const size_t LEAD_IN_SAMPLES = 48000 * 150 / 1000;  // 150ms for AGC settling
    const size_t TAIL_SAMPLES = 2400;  // 50ms guard

    std::vector<float> output;
    output.reserve(LEAD_IN_SAMPLES + samples.size() + TAIL_SAMPLES);
    output.resize(LEAD_IN_SAMPLES, 0.0f);
    output.insert(output.end(), samples.begin(), samples.end());
    output.resize(output.size() + TAIL_SAMPLES, 0.0f);

    // Apply TX bandpass filter
    if (filter_config_.enabled && tx_filter_) {
        SampleSpan span(output.data(), output.size());
        output = tx_filter_->process(span);
    }

    // Scale for audio output
    float max_val = 0.0f;
    for (float s : output) {
        max_val = std::max(max_val, std::abs(s));
    }
    if (max_val > 0.0f) {
        float scale = 0.8f / max_val;
        for (float& s : output) {
            s *= scale;
        }
    }

    // Update stats
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.frames_sent++;

        int bits_per_carrier = static_cast<int>(getBitsPerSymbol(config_.modulation));
        float code_rate = getCodeRateValue(config_.code_rate);
        float symbol_rate = config_.sample_rate / (float)config_.getSymbolDuration();
        stats_.throughput_bps = static_cast<int>(
            config_.getDataCarriers() * bits_per_carrier * code_rate * symbol_rate
        );
    }

    return output;
}

// ============================================================================
// TEST SIGNAL GENERATION
// ============================================================================

std::vector<float> ModemEngine::generateTestTone(float duration_sec) {
    size_t num_samples = static_cast<size_t>(config_.sample_rate * duration_sec);
    std::vector<float> tone(num_samples);

    float freq = 1500.0f;
    float phase = 0.0f;
    float phase_inc = 2.0f * M_PI * freq / config_.sample_rate;

    for (size_t i = 0; i < num_samples; i++) {
        tone[i] = 0.7f * std::sin(phase);
        phase += phase_inc;
        if (phase > 2.0f * M_PI) phase -= 2.0f * M_PI;
    }

    LOG_MODEM(INFO, "Generated test tone: %.1f Hz, %.1f sec, %zu samples",
              freq, duration_sec, num_samples);
    return tone;
}

std::vector<float> ModemEngine::transmitTestPattern(int pattern) {
    Bytes test_data(21);

    switch (pattern) {
        case 0:
            std::fill(test_data.begin(), test_data.end(), 0x00);
            LOG_MODEM(INFO, "TX Test Pattern: ALL ZEROS (%zu bytes)", test_data.size());
            break;
        case 1:
            {
                uint8_t deadbeef[] = {0xDE, 0xAD, 0xBE, 0xEF};
                for (size_t i = 0; i < test_data.size(); i++) {
                    test_data[i] = deadbeef[i % 4];
                }
            }
            LOG_MODEM(INFO, "TX Test Pattern: DEADBEEF (%zu bytes)", test_data.size());
            break;
        case 2:
            std::fill(test_data.begin(), test_data.end(), 0x55);
            LOG_MODEM(INFO, "TX Test Pattern: ALTERNATING 0101 (%zu bytes)", test_data.size());
            break;
        default:
            std::fill(test_data.begin(), test_data.end(), 0xAA);
            LOG_MODEM(INFO, "TX Test Pattern: ALTERNATING 1010 (%zu bytes)", test_data.size());
    }

    // Use StreamingEncoder for test pattern TX
    streaming_encoder_->setMode(protocol::WaveformMode::OFDM_CHIRP);
    streaming_encoder_->setDataMode(Modulation::DQPSK, CodeRate::R1_4);
    auto samples = streaming_encoder_->encodeFrame(test_data);

    if (samples.empty()) {
        LOG_MODEM(ERROR, "TX Test: Failed to encode");
        return {};
    }

    LOG_MODEM(INFO, "TX Test: %zu bytes -> %zu samples (R1/4 forced)", test_data.size(), samples.size());
    return postProcessTx(samples);
}

std::vector<float> ModemEngine::transmitRawOFDM(int pattern) {
    // Generate raw OFDM test (no LDPC) for layer-by-layer debugging
    size_t test_size = 81;  // Size of one R1/4 encoded codeword
    Bytes test_data(test_size);

    switch (pattern) {
        case 0:
            for (size_t i = 0; i < test_size; i++) {
                test_data[i] = (i % 2 == 0) ? 0xAA : 0x55;
            }
            LOG_MODEM(INFO, "TX Raw OFDM: AA/55 alternating (%zu bytes)", test_size);
            break;
        case 1:
            {
                uint8_t deadbeef[] = {0xDE, 0xAD, 0xBE, 0xEF};
                for (size_t i = 0; i < test_size; i++) {
                    test_data[i] = deadbeef[i % 4];
                }
            }
            LOG_MODEM(INFO, "TX Raw OFDM: DEADBEEF (%zu bytes)", test_size);
            break;
        default:
            std::fill(test_data.begin(), test_data.end(), 0xAA);
            LOG_MODEM(INFO, "TX Raw OFDM: ALL 0xAA (%zu bytes)", test_size);
    }

    // Use StreamingEncoder's waveform directly for raw (no LDPC) modulation
    streaming_encoder_->setMode(protocol::WaveformMode::OFDM_CHIRP);
    streaming_encoder_->setDataMode(Modulation::DQPSK, CodeRate::R1_4);
    IWaveform* wfm = streaming_encoder_->getWaveform();
    if (!wfm) {
        LOG_MODEM(ERROR, "TX Raw OFDM: Failed to create waveform");
        return {};
    }

    Samples preamble = wfm->generatePreamble();
    Samples modulated = wfm->modulate(test_data);

    std::vector<float> output;
    output.reserve(preamble.size() + modulated.size());
    output.insert(output.end(), preamble.begin(), preamble.end());
    output.insert(output.end(), modulated.begin(), modulated.end());

    // Scale for audio output
    float max_val = 0.0f;
    for (float s : output) max_val = std::max(max_val, std::abs(s));
    if (max_val > 0.0f) {
        float scale = 0.8f / max_val;
        for (float& s : output) s *= scale;
    }

    LOG_MODEM(INFO, "TX Raw OFDM: %zu bytes -> %zu samples",
              test_size, output.size());
    return output;
}

// ============================================================================
// STATUS & DATA ACCESS
// ============================================================================

bool ModemEngine::hasReceivedData() const {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    return !rx_data_queue_.empty();
}

std::string ModemEngine::getReceivedText() {
    Bytes data = getReceivedData();
    std::string text(data.begin(), data.end());
    text.erase(std::remove(text.begin(), text.end(), '\0'), text.end());
    return text;
}

Bytes ModemEngine::getReceivedData() {
    std::lock_guard<std::mutex> lock(rx_mutex_);

    if (rx_data_queue_.empty()) {
        return {};
    }

    Bytes data = rx_data_queue_.front();
    rx_data_queue_.pop();
    return data;
}

LoopbackStats ModemEngine::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

DecoderStats ModemEngine::getDecoderStats() const {
    if (streaming_decoder_) {
        return streaming_decoder_->getStats();
    }
    return DecoderStats{};
}

bool ModemEngine::isSynced() const {
    if (streaming_decoder_) {
        return streaming_decoder_->isSynced();
    }
    return false;
}

float ModemEngine::getCurrentSNR() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_.snr_db;
}

float ModemEngine::getFadingIndex() const {
    if (streaming_decoder_) {
        return streaming_decoder_->getLastFadingIndex();
    }
    return 0.0f;
}

bool ModemEngine::isFading() const {
    return getFadingIndex() > 0.65f;
}

ChannelQuality ModemEngine::getChannelQuality() const {
    // ChannelQuality not exposed through streaming_decoder_ yet
    // Return default for now
    return ChannelQuality{};
}

void ModemEngine::setKnownCFO(float cfo_hz) {
    if (streaming_decoder_) {
        streaming_decoder_->setKnownCFO(cfo_hz);
    }
}

std::vector<std::complex<float>> ModemEngine::getConstellationSymbols() const {
    if (streaming_decoder_) {
        return streaming_decoder_->getConstellationSymbols();
    }
    return {};
}

bool ModemEngine::isNarrowbandDetected() const {
    if (streaming_decoder_) {
        return streaming_decoder_->getDetectedBandwidth() == BandwidthMode::NARROW;
    }
    return false;
}

void ModemEngine::setNarrowbandControl(bool narrowband) {
    if (streaming_encoder_) {
        streaming_encoder_->setNarrowbandControl(narrowband);
    }
}

void ModemEngine::reset() {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    std::queue<Bytes> empty;
    std::swap(rx_data_queue_, empty);

    adaptive_.reset();
    use_connected_waveform_once_ = false;

    // Reset StreamingDecoder (primary decoder)
    if (streaming_decoder_) {
        streaming_decoder_->reset();
    }

    // Reset carrier sense
    channel_energy_.store(0.0f);

    {
        std::lock_guard<std::mutex> lock2(stats_mutex_);
        stats_ = LoopbackStats{};
    }
}

void ModemEngine::clearRxBuffer() {
    // Clear streaming decoder buffer to discard any pending audio
    // Use this before TX to prevent decoding our own transmission (acoustic echo)
    if (streaming_decoder_) {
        streaming_decoder_->reset();
    }
}

} // namespace gui
} // namespace ultra
