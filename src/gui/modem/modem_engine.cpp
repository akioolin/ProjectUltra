// ModemEngine - Main implementation
// Constructor, destructor, configuration, and TX functions

#include "modem_engine.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/logging.hpp"
#include "waveform/waveform_factory.hpp"
#include <cstring>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace ultra {
namespace gui {

ModemEngine::ModemEngine() {
    config_ = presets::balanced();

    // CRITICAL: Disable pilots for DQPSK mode - uses all 30 carriers for data
    // This doubles throughput (30 data carriers vs 15 with pilots)
    // DQPSK is differential and doesn't need pilots for channel estimation
    config_.use_pilots = false;

    encoder_ = fec::CodecFactory::create(fec::CodecType::LDPC, config_.code_rate);
    decoder_ = fec::CodecFactory::create(fec::CodecType::LDPC, CodeRate::R1_4);
    // NOTE: TX uses active_tx_waveform_ (IWaveform), RX uses streaming_decoder_
    // No separate ofdm_modulator_/ofdm_demodulator_ needed

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

    // Multi-Carrier DPSK (for fading channels - frequency diversity)
    // Using level8: 8 carriers, 93.75 baud, DQPSK (~735 bps)
    // 8 carriers is more robust than 13 at low SNR with CFO (tested: 100% vs 40% at moderate fading)
    // IMPORTANT: Sync chirp config with modem's chirp_sync_ so TX and RX use same chirp
    mc_dpsk_config_ = mc_dpsk_presets::level8();
    mc_dpsk_config_.chirp_f_start = chirp_cfg.f_start;
    mc_dpsk_config_.chirp_f_end = chirp_cfg.f_end;
    mc_dpsk_config_.chirp_duration_ms = chirp_cfg.duration_ms;
    mc_dpsk_config_.use_dual_chirp = chirp_cfg.use_dual_chirp;
    // Note: Actual MC-DPSK modulation is done by IWaveform via StreamingDecoder

    // Channel interleaver for time-frequency diversity on fading channels
    // Default: 118 bits/symbol for OFDM (59 data carriers × 2 bits DQPSK)
    updateChannelInterleaver(config_.num_carriers * 2);

    // Initialize audio filters
    rebuildFilters();

    // ========================================================================
    // Initialize StreamingDecoder (primary RX path)
    // ========================================================================
    streaming_decoder_ = std::make_unique<StreamingDecoder>();
    streaming_decoder_->setLogPrefix(log_prefix_);

    // Set callbacks to wire into existing ModemEngine callbacks
    streaming_decoder_->setFrameCallback([this](const DecodeResult& result) {
        if (result.success && !result.frame_data.empty()) {
            deliverFrame(result.frame_data);
            notifyFrameParsed(result.frame_data, result.frame_type);
        }
        // Update stats
        updateStats([&](LoopbackStats& s) {
            s.snr_db = result.snr_db;
            s.synced = result.success;
        });
        // Save peer CFO for future frames
        if (std::abs(result.cfo_hz) > 0.1f) {
            peer_cfo_hz_ = result.cfo_hz;
        }
        last_rx_complete_time_ = std::chrono::steady_clock::now();
    });

    streaming_decoder_->setPingCallback([this](float snr_db, float cfo_hz) {
        if (ping_received_callback_) {
            ping_received_callback_(snr_db);
        }
        updateStats([](LoopbackStats& s) { s.frames_received++; });
        last_rx_complete_time_ = std::chrono::steady_clock::now();
    });

    // Sync StreamingDecoder with initial waveform mode
    // When disconnected, use MC_DPSK for PING detection (chirp-based sync)
    // When connected, use the negotiated waveform
    protocol::WaveformMode decoder_mode = connected_ ? waveform_mode_ : protocol::WaveformMode::MC_DPSK;
    streaming_decoder_->setMode(decoder_mode, connected_);

    // Sync MC-DPSK carrier count with ModemEngine's config
    streaming_decoder_->setMCDPSKCarriers(mc_dpsk_config_.num_carriers);

    LOG_MODEM(INFO, "[%s] StreamingDecoder initialized (MC-DPSK: %d carriers)",
              log_prefix_.c_str(), mc_dpsk_config_.num_carriers);

    // Start RX decode thread (StreamingDecoder handles acquisition)
    startRxDecodeThread();
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

    encoder_->setRate(config.code_rate);
    // BUG FIX: Don't change decoder rate here - it should stay at R1_4 for disconnected mode
    // The decoder rate should only change when setConnected(true) is called
    LOG_MODEM(INFO, "setConfig: encoder_rate=%d, decoder_rate=%d (decoder unchanged for disconnected)",
              static_cast<int>(config.code_rate), static_cast<int>(decoder_->getRate()));

    // NOTE: TX uses active_tx_waveform_, RX uses streaming_decoder_
    // No separate ofdm_modulator_/ofdm_demodulator_ to recreate

    // Update channel interleaver for new carrier count
    // DQPSK = 2 bits per carrier
    size_t bps = config_.num_carriers * 2;
    updateChannelInterleaver(bps);

    // Propagate OFDM config to StreamingDecoder for OFDM modes
    // This allows custom FFT/carrier settings (like NVIS mode with 1024 FFT)
    if (streaming_decoder_ &&
        (waveform_mode_ == protocol::WaveformMode::OFDM_COX ||
         waveform_mode_ == protocol::WaveformMode::OFDM_CHIRP)) {
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
    namespace v2 = protocol::v2;

    if (data.empty()) {
        return {};
    }

    // Check for v2 frame magic "UL" (0x55, 0x4C)
    bool is_v2_frame = (data.size() >= 2 && data[0] == 0x55 && data[1] == 0x4C);

    LOG_MODEM(INFO, "[%s] TX: Input %zu bytes, first 4: %02x %02x %02x %02x, v2=%d",
              log_prefix_.c_str(),
              data.size(),
              data.size() > 0 ? data[0] : 0,
              data.size() > 1 ? data[1] : 0,
              data.size() > 2 ? data[2] : 0,
              data.size() > 3 ? data[3] : 0,
              is_v2_frame);

    Bytes to_modulate;
    CodeRate tx_code_rate = CodeRate::R1_4;  // Default, set below based on frame type

    // Determine if this is a DATA frame (used for modulation selection later)
    bool is_data_frame = false;
    if (is_v2_frame && data.size() >= 3 && connected_) {
        uint8_t frame_type = data[2];
        is_data_frame = (frame_type >= 0x30 && frame_type <= 0x33);
    }

    if (is_v2_frame) {
        // === V2 Frame Path ===
        // Protocol rate selection:
        // - Pre-connection (PING/PONG/CONNECT): R1/4 for robustness
        // - During handshake (CONNECT_ACK): R1/4 (remote not yet confirmed)
        // - Post-handshake: ALL frames (data AND control) use negotiated rate
        //   (ACK/NACK/DISCONNECT must use same rate as data for RX to decode)

        std::vector<Bytes> encoded_cws;
        // Use negotiated rate if connected+handshake OR for disconnect ACK (use_connected_waveform_once_)
        tx_code_rate = ((connected_ && handshake_complete_) || use_connected_waveform_once_) ? data_code_rate_ : CodeRate::R1_4;

        // Patch total_cw in frame header to match actual encoding rate
        // Only for frames with total_cw field (Data frames 0x30-0x33, Connect frames 0x12-0x15)
        // Control frames (ACK 0x20, NACK 0x21, etc.) are fixed 20 bytes = 1 codeword, no patching needed
        Bytes tx_data = data;  // Make mutable copy
        if (tx_data.size() >= 17) {  // Need at least header size for data/connect frames
            uint8_t frame_type = tx_data[2];
            bool is_data_or_connect = (frame_type >= 0x10 && frame_type <= 0x19) ||  // Connect frames
                                      (frame_type >= 0x30 && frame_type <= 0x3F);    // Data frames
            if (is_data_or_connect) {
                // Get payload size from header (bytes 13-14)
                uint16_t payload_len = (static_cast<uint16_t>(tx_data[13]) << 8) | tx_data[14];
                // Calculate correct total_cw for this rate
                uint8_t correct_cw = v2::DataFrame::calculateCodewords(payload_len, tx_code_rate);
                if (tx_data[12] != correct_cw) {
                    LOG_MODEM(DEBUG, "TX v2: Patching total_cw from %d to %d for %s",
                              tx_data[12], correct_cw, codeRateToString(tx_code_rate));
                    tx_data[12] = correct_cw;
                    // Recalculate header CRC (over bytes 0-14)
                    uint16_t hcrc = v2::ControlFrame::calculateCRC(tx_data.data(), 15);
                    tx_data[15] = (hcrc >> 8) & 0xFF;
                    tx_data[16] = hcrc & 0xFF;
                }
            }
        }

        encoded_cws = v2::encodeFrameWithLDPC(tx_data, tx_code_rate);
        LOG_MODEM(INFO, "TX v2: %zu bytes -> %zu codewords (all %s)",
                  data.size(), encoded_cws.size(), codeRateToString(tx_code_rate));

        // Concatenate all encoded codewords (with optional interleaving per-codeword)
        // Channel interleaver spreads bits across OFDM symbols for time diversity
        // IMPORTANT: Determine actual waveform for this TX (not just waveform_mode_)
        // When not connected, waveform_mode_ may still be OFDM_COX but we're actually using MC-DPSK
        // For disconnect ACK (use_connected_waveform_once_), use disconnect_waveform_ (saved negotiated mode)
        protocol::WaveformMode tx_waveform = use_connected_waveform_once_ ? disconnect_waveform_ :
                                             (!connected_ ? connect_waveform_ :
                                              (!handshake_complete_ ? last_rx_waveform_ : waveform_mode_));
        bool use_interleaving = interleaving_enabled_ && channel_interleaver_ &&
                                (tx_waveform == protocol::WaveformMode::OFDM_COX ||
                                 tx_waveform == protocol::WaveformMode::OFDM_CHIRP);
        LOG_MODEM(INFO, "TX v2: interleaving=%d (enabled=%d, interleaver=%p, waveform=%d)",
                  use_interleaving, interleaving_enabled_, (void*)channel_interleaver_.get(),
                  static_cast<int>(tx_waveform));
        for (size_t cw_idx = 0; cw_idx < encoded_cws.size(); cw_idx++) {
            const auto& cw = encoded_cws[cw_idx];
            if (use_interleaving) {
                // Debug: show first 10 bytes BEFORE interleaving
                if (cw_idx == 0 && cw.size() >= 10) {
                    LOG_MODEM(INFO, "TX v2 CW0: BEFORE interleave bytes 0-9: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                              cw[0], cw[1], cw[2], cw[3], cw[4], cw[5], cw[6], cw[7], cw[8], cw[9]);
                }
                Bytes interleaved = channel_interleaver_->interleave(cw);
                // Debug: show first 10 bytes AFTER interleaving
                if (cw_idx == 0 && interleaved.size() >= 10) {
                    LOG_MODEM(INFO, "TX v2 CW0: AFTER interleave bytes 0-9: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                              interleaved[0], interleaved[1], interleaved[2], interleaved[3], interleaved[4],
                              interleaved[5], interleaved[6], interleaved[7], interleaved[8], interleaved[9]);
                }
                to_modulate.insert(to_modulate.end(), interleaved.begin(), interleaved.end());
            } else {
                to_modulate.insert(to_modulate.end(), cw.begin(), cw.end());
            }
        }

        // Print as bits for easier comparison with RX soft bits
        if (to_modulate.size() > 0) {
            LOG_MODEM(INFO, "TX v2: First byte 0x%02x as bits: %d%d%d%d%d%d%d%d",
                      to_modulate[0],
                      (to_modulate[0] >> 7) & 1, (to_modulate[0] >> 6) & 1,
                      (to_modulate[0] >> 5) & 1, (to_modulate[0] >> 4) & 1,
                      (to_modulate[0] >> 3) & 1, (to_modulate[0] >> 2) & 1,
                      (to_modulate[0] >> 1) & 1, (to_modulate[0] >> 0) & 1);
        }
        LOG_MODEM(INFO, "TX v2: Total encoded %zu bytes, first 8: %02x %02x %02x %02x %02x %02x %02x %02x",
                  to_modulate.size(),
                  to_modulate.size() > 0 ? to_modulate[0] : 0,
                  to_modulate.size() > 1 ? to_modulate[1] : 0,
                  to_modulate.size() > 2 ? to_modulate[2] : 0,
                  to_modulate.size() > 3 ? to_modulate[3] : 0,
                  to_modulate.size() > 4 ? to_modulate[4] : 0,
                  to_modulate.size() > 5 ? to_modulate[5] : 0,
                  to_modulate.size() > 6 ? to_modulate[6] : 0,
                  to_modulate.size() > 7 ? to_modulate[7] : 0);
    } else {
        // === Raw Data Path (non-v2 frame) ===
        // Use connected code rate if still connected OR for disconnect ACK
        tx_code_rate = (connected_ || use_connected_waveform_once_) ? data_code_rate_ : CodeRate::R1_4;

        encoder_->setRate(tx_code_rate);
        Bytes encoded = encoder_->encode(data);

        LOG_MODEM(INFO, "TX raw: %zu bytes -> %zu encoded (rate=%d)",
                  data.size(), encoded.size(), static_cast<int>(tx_code_rate));

        // Channel interleaver spreads bits across OFDM symbols for time diversity
        bool use_interleaving = interleaving_enabled_ && channel_interleaver_ &&
                                (waveform_mode_ == protocol::WaveformMode::OFDM_COX ||
                                 waveform_mode_ == protocol::WaveformMode::OFDM_CHIRP);
        to_modulate = use_interleaving ? channel_interleaver_->interleave(encoded) : encoded;
    }

    // Modulation selection
    // During handshake (connected but not handshake_complete), use DQPSK for reliability.
    // After handshake, use negotiated modulation so RX demodulator can decode.
    // Robustness is handled by code rate: CW0 header always uses R1/4.
    // IMPORTANT: When use_connected_waveform_once_ is set (for DISCONNECT ACK),
    // we must also use the connected modulation since the remote is still expecting it.
    Modulation tx_modulation = Modulation::DQPSK;
    if ((connected_ && handshake_complete_) || use_connected_waveform_once_) {
        tx_modulation = data_modulation_;
        LOG_MODEM(INFO, "[%s] TX: Using %s modulation %s",
                  log_prefix_.c_str(),
                  connected_ ? "negotiated" : "preserved (disconnect ACK)",
                  modulationToString(tx_modulation));
    }

    // Determine which waveform to use
    LOG_MODEM(INFO, "[%s] TX WAVEFORM DECISION: connected_=%d, handshake_complete_=%d, "
              "waveform_mode_=%d, connect_waveform_=%d, last_rx_waveform_=%d, use_once_=%d",
              log_prefix_.c_str(), connected_ ? 1 : 0, handshake_complete_ ? 1 : 0,
              static_cast<int>(waveform_mode_), static_cast<int>(connect_waveform_),
              static_cast<int>(last_rx_waveform_), use_connected_waveform_once_ ? 1 : 0);

    protocol::WaveformMode active_waveform;
    if (use_connected_waveform_once_) {
        // For disconnect ACK, use disconnect_waveform_ (saved when setConnected(false) was called)
        // This is the negotiated waveform that was in use during the connection
        active_waveform = disconnect_waveform_;
        use_connected_waveform_once_ = false;
        LOG_MODEM(INFO, "[%s] TX: use_connected_waveform_once_ -> using disconnect_waveform_=%d",
                  log_prefix_.c_str(), static_cast<int>(active_waveform));
    } else if (!connected_) {
        active_waveform = connect_waveform_;
        LOG_MODEM(INFO, "[%s] TX: NOT connected -> using connect_waveform_=%d",
                  log_prefix_.c_str(), static_cast<int>(active_waveform));
    } else if (!handshake_complete_) {
        active_waveform = last_rx_waveform_;
        LOG_MODEM(INFO, "[%s] TX: Handshake mode -> using last_rx_waveform_=%d",
                  log_prefix_.c_str(), static_cast<int>(active_waveform));
    } else {
        active_waveform = waveform_mode_;
        LOG_MODEM(INFO, "[%s] TX: Connected+handshake -> using waveform_mode_=%d",
                  log_prefix_.c_str(), static_cast<int>(active_waveform));
    }

    Samples preamble, modulated;

    // All modes now use IWaveform interface (MC_DPSK, OFDM_CHIRP, OFDM_COX, OTFS)
    ensureTxWaveform(active_waveform, tx_modulation, tx_code_rate);

    if (active_tx_waveform_) {
        // Use light preamble (training only, no chirp) for DATA frames when connected
        // This saves ~1.2 seconds per frame, nearly tripling throughput
        // Requirements: connected, handshake complete, waveform supports it
        bool use_light_preamble = connected_ && handshake_complete_ &&
                                   active_tx_waveform_->supportsDataPreamble();

        if (use_light_preamble) {
            preamble = active_tx_waveform_->generateDataPreamble();
            LOG_MODEM(INFO, "[%s] TX: Using LIGHT preamble (training only, %zu samples)",
                      log_prefix_.c_str(), preamble.size());
        } else {
            preamble = active_tx_waveform_->generatePreamble();
            LOG_MODEM(INFO, "[%s] TX: Using FULL preamble (chirp+training, %zu samples)",
                      log_prefix_.c_str(), preamble.size());
        }

        modulated = active_tx_waveform_->modulate(to_modulate);

        LOG_MODEM(INFO, "[%s] TX: Using %s (%s, %s)",
                  log_prefix_.c_str(),
                  active_tx_waveform_->getName().c_str(),
                  modulationToString(tx_modulation),
                  codeRateToString(tx_code_rate));
    } else {
        LOG_MODEM(ERROR, "[%s] TX: Failed to create waveform for mode %d",
                  log_prefix_.c_str(), static_cast<int>(active_waveform));
        return {};
    }

    // Combine lead-in + preamble + data + tail guard
    const size_t LEAD_IN_SAMPLES = 48000 * 150 / 1000;  // 150ms
    // Small tail guard for processing margin
    // With continuous noise feeding in simulator, decoder gets samples after TX ends
    const size_t TAIL_SAMPLES = 2400;  // 50ms guard
    std::vector<float> output;
    output.reserve(LEAD_IN_SAMPLES + preamble.size() + modulated.size() + TAIL_SAMPLES);

    output.resize(LEAD_IN_SAMPLES, 0.0f);
    output.insert(output.end(), preamble.begin(), preamble.end());
    output.insert(output.end(), modulated.begin(), modulated.end());
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
// PING/PONG PROBE (minimal presence check)
// ============================================================================

std::vector<float> ModemEngine::transmitPing() {
    // Generate chirp sync signal for robust presence detection
    // Chirp spreads energy across 400-2600 Hz, robust to frequency-selective fading
    auto chirp = chirp_sync_->generate();

    // Apply TX bandpass filter
    if (filter_config_.enabled && tx_filter_) {
        SampleSpan span(chirp.data(), chirp.size());
        chirp = tx_filter_->process(span);
    }

    // Scale for audio output
    float max_val = 0.0f;
    for (float s : chirp) {
        max_val = std::max(max_val, std::abs(s));
    }
    if (max_val > 0.0f) {
        float scale = 0.8f / max_val;
        for (float& s : chirp) {
            s *= scale;
        }
    }

    // Lead-in silence for receiver AGC settling + trailing silence
    // All TX must have lead-in for consistent receiver behavior
    constexpr size_t LEAD_IN_SAMPLES = 48000 * 150 / 1000;   // 150ms for AGC
    constexpr size_t TRAILING_SILENCE = 2400;                 // 50ms guard

    std::vector<float> output;
    output.reserve(LEAD_IN_SAMPLES + chirp.size() + TRAILING_SILENCE);
    output.resize(LEAD_IN_SAMPLES, 0.0f);  // Lead-in silence
    output.insert(output.end(), chirp.begin(), chirp.end());
    output.resize(output.size() + TRAILING_SILENCE, 0.0f);  // Trailing silence

    LOG_MODEM(INFO, "[%s] TX PING (chirp): %zu samples (%.2f sec, incl lead-in+trail)",
              log_prefix_.c_str(), output.size(), output.size() / 48000.0f);

    return output;
}

std::vector<float> ModemEngine::transmitPong() {
    // Pong is identical to ping - context determines meaning
    // (Ping = initiator probe, Pong = responder reply)
    LOG_MODEM(INFO, "[%s] TX PONG (same as PING)", log_prefix_.c_str());
    return transmitPing();
}

// ============================================================================
// WAVEFORM ABSTRACTION HELPERS
// ============================================================================

void ModemEngine::ensureTxWaveform(protocol::WaveformMode mode, Modulation mod, CodeRate rate) {
    // Check if we need to create or reconfigure the waveform
    if (active_tx_waveform_ && active_tx_waveform_->getMode() == mode) {
        // Same mode - just reconfigure modulation/rate if needed
        if (active_tx_waveform_->getModulation() != mod ||
            active_tx_waveform_->getCodeRate() != rate) {
            active_tx_waveform_->configure(mod, rate);
            LOG_MODEM(INFO, "[%s] TX waveform reconfigured: %s %s",
                      log_prefix_.c_str(), modulationToString(mod), codeRateToString(rate));
        }
        return;
    }

    // Create new waveform using factory
    // For MC-DPSK, use the configured carrier count from mc_dpsk_config_
    if (mode == protocol::WaveformMode::MC_DPSK) {
        active_tx_waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_.num_carriers);
        LOG_MODEM(INFO, "[%s] TX waveform created: MC-DPSK (%d carriers), %s %s",
                  log_prefix_.c_str(), mc_dpsk_config_.num_carriers,
                  modulationToString(mod), codeRateToString(rate));
    } else {
        active_tx_waveform_ = WaveformFactory::create(mode, config_);
        if (active_tx_waveform_) {
            LOG_MODEM(INFO, "[%s] TX waveform created: %s, %s %s",
                      log_prefix_.c_str(), active_tx_waveform_->getName().c_str(),
                      modulationToString(mod), codeRateToString(rate));
        }
    }

    if (active_tx_waveform_) {
        active_tx_waveform_->configure(mod, rate);
    } else {
        LOG_MODEM(ERROR, "[%s] Failed to create TX waveform for mode %d",
                  log_prefix_.c_str(), static_cast<int>(mode));
    }
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

    // LDPC encode
    CodeRate saved_rate = encoder_->getRate();
    encoder_->setRate(CodeRate::R1_4);
    Bytes encoded = encoder_->encode(test_data);
    encoder_->setRate(saved_rate);
    LOG_MODEM(INFO, "TX Test: %zu bytes -> %zu encoded bytes (R1/4 forced)", test_data.size(), encoded.size());

    // Use waveform interface for TX
    ensureTxWaveform(protocol::WaveformMode::OFDM_CHIRP, Modulation::DQPSK, CodeRate::R1_4);
    if (!active_tx_waveform_) {
        LOG_MODEM(ERROR, "TX Test: Failed to create waveform");
        return {};
    }

    Samples preamble = active_tx_waveform_->generatePreamble();
    Samples modulated = active_tx_waveform_->modulate(encoded);

    std::vector<float> output;
    output.reserve(preamble.size() + modulated.size());
    output.insert(output.end(), preamble.begin(), preamble.end());
    output.insert(output.end(), modulated.begin(), modulated.end());

    float max_val = 0.0f;
    for (float s : output) max_val = std::max(max_val, std::abs(s));
    if (max_val > 0.0f) {
        float scale = 0.8f / max_val;
        for (float& s : output) s *= scale;
    }

    return output;
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

    // Use waveform interface for TX
    ensureTxWaveform(protocol::WaveformMode::OFDM_CHIRP, Modulation::DQPSK, CodeRate::R1_4);
    if (!active_tx_waveform_) {
        LOG_MODEM(ERROR, "TX Raw OFDM: Failed to create waveform");
        return {};
    }

    Samples preamble = active_tx_waveform_->generatePreamble();
    Samples modulated = active_tx_waveform_->modulate(test_data);

    std::vector<float> output;
    output.reserve(preamble.size() + modulated.size());
    output.insert(output.end(), preamble.begin(), preamble.end());
    output.insert(output.end(), modulated.begin(), modulated.end());

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
    return getFadingIndex() > 0.4f;
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

void ModemEngine::updateChannelInterleaver(size_t bits_per_symbol) {
    if (bits_per_symbol == interleaver_bits_per_symbol_ && channel_interleaver_) {
        return;  // Already configured
    }

    interleaver_bits_per_symbol_ = bits_per_symbol;
    channel_interleaver_ = std::make_unique<ChannelInterleaver>(bits_per_symbol, protocol::v2::LDPC_CODEWORD_BITS);

    LOG_MODEM(INFO, "Channel interleaver updated: %zu bits/symbol, symbol separation=%zu",
              bits_per_symbol, channel_interleaver_->getSymbolSeparation());
}

} // namespace gui
} // namespace ultra
