// modem_mode.cpp - Waveform and mode control for ModemEngine

#include "modem_engine.hpp"
#include "ultra/logging.hpp"
#include "protocol/frame_v2.hpp"
#include <cstdio>

namespace ultra {
namespace gui {

// Helper to get mode description string
static const char* getModeDescription(Modulation mod, CodeRate rate) {
    static char buf[32];
    const char* mod_name;
    switch (mod) {
        case Modulation::DBPSK: mod_name = "DBPSK"; break;
        case Modulation::BPSK:  mod_name = "BPSK"; break;
        case Modulation::DQPSK: mod_name = "DQPSK"; break;
        case Modulation::QPSK:  mod_name = "QPSK"; break;
        case Modulation::D8PSK: mod_name = "D8PSK"; break;
        case Modulation::QAM8:  mod_name = "8QAM"; break;
        case Modulation::QAM16: mod_name = "16QAM"; break;
        case Modulation::QAM32: mod_name = "32QAM"; break;
        case Modulation::QAM64: mod_name = "64QAM"; break;
        default: mod_name = "???"; break;
    }
    const char* rate_name;
    switch (rate) {
        case CodeRate::R1_4: rate_name = "R1/4"; break;
        case CodeRate::R1_2: rate_name = "R1/2"; break;
        case CodeRate::R2_3: rate_name = "R2/3"; break;
        case CodeRate::R3_4: rate_name = "R3/4"; break;
        case CodeRate::R5_6: rate_name = "R5/6"; break;
        default: rate_name = "R?"; break;
    }
    snprintf(buf, sizeof(buf), "%s %s", mod_name, rate_name);
    return buf;
}

void ModemEngine::setWaveformMode(protocol::WaveformMode mode) {
    if (waveform_mode_ == mode) return;

    LOG_MODEM(INFO, "Switching waveform mode: %d -> %d",
              static_cast<int>(waveform_mode_), static_cast<int>(mode));

    waveform_mode_ = mode;

    // Update StreamingDecoder to use the new waveform
    // When disconnected, always use MC_DPSK for PING detection (chirp-based sync)
    if (streaming_decoder_) {
        protocol::WaveformMode decoder_mode = connected_ ? mode : protocol::WaveformMode::MC_DPSK;
        streaming_decoder_->setMode(decoder_mode, connected_);

        // For OFDM modes, propagate the current config (for custom FFT/carriers like NVIS mode)
        if (mode == protocol::WaveformMode::OFDM_COX ||
            mode == protocol::WaveformMode::OFDM_CHIRP) {
            streaming_decoder_->setOFDMConfig(config_);
            LOG_MODEM(INFO, "setWaveformMode: StreamingDecoder OFDM config set (FFT=%d, carriers=%d)",
                      config_.fft_size, config_.num_carriers);
        }
    }

    // Switch RX waveform (when connected)
    if (connected_) {
        switchRxWaveform(mode);
    }

    switch (mode) {
        case protocol::WaveformMode::MC_DPSK:
            dpsk_demodulator_->reset();
            LOG_MODEM(INFO, "DPSK mode active: %d-PSK, %d samples/sym, %.1f bps",
                      dpsk_config_.num_phases(),
                      dpsk_config_.samples_per_symbol,
                      dpsk_config_.raw_bps());
            break;

        case protocol::WaveformMode::OTFS_EQ:
        case protocol::WaveformMode::OTFS_RAW:
            otfs_demodulator_->reset();
            LOG_MODEM(INFO, "OTFS mode active: M=%d, N=%d",
                      otfs_config_.M, otfs_config_.N);
            break;

        case protocol::WaveformMode::OFDM_CHIRP: {
            // OFDM_CHIRP uses DQPSK (differential) for fading channels
            // Chirp provides robust sync at low SNR, DQPSK is robust to phase drift
            // No pilots needed - differential modulation compares adjacent symbols
            ModemConfig chirp_config = config_;
            chirp_config.modulation = Modulation::DQPSK;
            chirp_config.use_pilots = false;  // DQPSK doesn't need pilots
            ofdm_modulator_ = std::make_unique<OFDMModulator>(chirp_config);
            ofdm_demodulator_ = std::make_unique<OFDMDemodulator>(chirp_config);
            LOG_MODEM(INFO, "OFDM_CHIRP mode active: %d data carriers, DQPSK (differential)",
                      chirp_config.num_carriers);
            break;
        }

        case protocol::WaveformMode::OFDM_COX:
        default:
            ofdm_demodulator_->reset();
            LOG_MODEM(INFO, "OFDM mode active: %d carriers, %s",
                      config_.num_carriers,
                      connected_ ? "connected" : "disconnected");
            break;
    }
}

void ModemEngine::setConnectWaveform(protocol::WaveformMode mode) {
    LOG_MODEM(INFO, "Switching connect waveform: %s -> %s",
              protocol::waveformModeToString(connect_waveform_),
              protocol::waveformModeToString(mode));

    connect_waveform_ = mode;

    // Clear any leftover flag from previous disconnect - we're starting fresh
    use_connected_waveform_once_ = false;

    // Configure DPSK for medium preset (DQPSK 62b R1/4) for connection attempts
    if (mode == protocol::WaveformMode::MC_DPSK) {
        dpsk_config_ = dpsk_presets::medium();  // DQPSK 62.5 baud
        dpsk_demodulator_ = std::make_unique<DPSKDemodulator>(dpsk_config_);
        LOG_MODEM(INFO, "DPSK connect mode: %d-PSK, %.1f baud",
                  dpsk_config_.num_phases(), dpsk_config_.symbol_rate());
    }
}

void ModemEngine::setConnected(bool connected) {
    LOG_MODEM(INFO, "[%s] setConnected(%d) called, was connected_=%d",
              log_prefix_.c_str(), connected ? 1 : 0, connected_ ? 1 : 0);

    if (connected_ == connected) return;

    connected_ = connected;

    if (connected) {
        // Reset handshake state - we'll complete it when we receive first post-ACK frame
        handshake_complete_ = false;
        use_connected_waveform_once_ = false;  // Clear any leftover flag

        // CRITICAL: Update StreamingDecoder when entering connected state
        // This resets the buffer and updates the connected flag
        if (streaming_decoder_) {
            streaming_decoder_->setMode(waveform_mode_, true);  // true = connected

            // For OFDM modes, propagate the current config (for custom FFT/carriers like NVIS mode)
            if (waveform_mode_ == protocol::WaveformMode::OFDM_COX ||
                waveform_mode_ == protocol::WaveformMode::OFDM_CHIRP) {
                streaming_decoder_->setOFDMConfig(config_);
            }
        }

        // Configure OFDM modulator/demodulator to match data_modulation_
        // This ensures TX and RX use the same constellation when connected
        bool is_differential = (data_modulation_ == Modulation::DBPSK ||
                               data_modulation_ == Modulation::DQPSK ||
                               data_modulation_ == Modulation::D8PSK);
        config_.modulation = data_modulation_;
        config_.code_rate = data_code_rate_;
        config_.use_pilots = !is_differential;

        ofdm_modulator_ = std::make_unique<OFDMModulator>(config_);
        decoder_->setRate(data_code_rate_);
        ofdm_demodulator_ = std::make_unique<OFDMDemodulator>(config_);

        // Switch RX waveform for connected mode
        switchRxWaveform(waveform_mode_);

        LOG_MODEM(INFO, "Entered connected state, configured for %s %s (pilots=%d)",
                  modulationToString(data_modulation_), codeRateToString(data_code_rate_),
                  config_.use_pilots ? 1 : 0);
    } else {
        // Switching to disconnected state - use robust mode for RX
        ModemConfig rx_config = config_;
        rx_config.modulation = Modulation::DQPSK;
        rx_config.code_rate = CodeRate::R1_4;

        decoder_->setRate(CodeRate::R1_4);
        ofdm_demodulator_ = std::make_unique<OFDMDemodulator>(rx_config);

        // CRITICAL: Update StreamingDecoder for disconnected state
        // Use MC_DPSK to detect new PINGs (chirp-based sync)
        if (streaming_decoder_) {
            streaming_decoder_->setMode(protocol::WaveformMode::MC_DPSK, false);  // false = disconnected
        }
        dpsk_demodulator_->reset();
        active_rx_waveform_ = nullptr;

        // Keep using connected waveform for the next TX (DISCONNECT ACK)
        // Save the current negotiated waveform BEFORE it might be reset
        disconnect_waveform_ = waveform_mode_;
        use_connected_waveform_once_ = true;
        LOG_MODEM(INFO, "Switched to disconnected mode (RX: DQPSK R1/4, next TX uses disconnect_waveform_=%d)",
                  static_cast<int>(disconnect_waveform_));
        handshake_complete_ = false;  // Reset for next connection
    }
}

void ModemEngine::setHandshakeComplete(bool complete) {
    if (handshake_complete_ == complete) return;

    handshake_complete_ = complete;

    if (complete) {
        LOG_MODEM(INFO, "Handshake complete, TX now uses waveform_mode_=%d",
                  static_cast<int>(waveform_mode_));
    }
}

void ModemEngine::setDataMode(Modulation mod, CodeRate rate) {
    data_modulation_ = mod;
    data_code_rate_ = rate;

    // Determine if modulation is differential (doesn't need pilots)
    bool is_differential = (mod == Modulation::DBPSK ||
                           mod == Modulation::DQPSK ||
                           mod == Modulation::D8PSK);

    // If already connected, update both TX and RX to match
    if (connected_) {
        // Update base config for TX modulator
        config_.modulation = mod;
        config_.code_rate = rate;
        config_.use_pilots = !is_differential;  // QAM needs pilots, DQPSK doesn't

        // Recreate modulator with new config
        ofdm_modulator_ = std::make_unique<OFDMModulator>(config_);

        // Recreate demodulator with matching config
        decoder_->setRate(rate);
        ofdm_demodulator_ = std::make_unique<OFDMDemodulator>(config_);

        LOG_MODEM(INFO, "TX/RX OFDM updated: mod=%d, rate=%d, use_pilots=%d",
                  static_cast<int>(mod), static_cast<int>(rate), config_.use_pilots ? 1 : 0);
    }

    // Update StreamingDecoder's waveform configuration
    if (streaming_decoder_) {
        streaming_decoder_->setDataMode(mod, rate);
    }

    LOG_MODEM(INFO, "Data mode set to: %s", getModeDescription(mod, rate));
}

void ModemEngine::recommendDataMode(float snr_db, Modulation& mod, CodeRate& rate) {
    // Conservative thresholds calibrated for REAL HF channels (not just AWGN)
    // HF has multipath fading that requires 3-6 dB extra margin vs AWGN
    // These match the thresholds in connection.cpp for consistency
    if (snr_db >= 30.0f) {
        // Excellent conditions (rare) - use high throughput
        mod = Modulation::QAM16;
        rate = CodeRate::R3_4;
    } else if (snr_db >= 25.0f) {
        // Very good conditions
        mod = Modulation::QAM16;
        rate = CodeRate::R2_3;
    } else if (snr_db >= 20.0f) {
        // Good conditions - sweet spot for speed
        mod = Modulation::DQPSK;
        rate = CodeRate::R2_3;
    } else if (snr_db >= 16.0f) {
        // Typical good HF - balanced speed/reliability
        mod = Modulation::DQPSK;
        rate = CodeRate::R1_2;
    } else if (snr_db >= 12.0f) {
        // Typical HF - prioritize reliability
        mod = Modulation::DQPSK;
        rate = CodeRate::R1_4;
    } else if (snr_db >= 8.0f) {
        // Marginal conditions
        mod = Modulation::BPSK;
        rate = CodeRate::R1_4;
    } else {
        // Very poor conditions - maximum robustness
        mod = Modulation::BPSK;
        rate = CodeRate::R1_4;
    }
}

protocol::WaveformMode ModemEngine::recommendWaveformMode(float snr_db) {
    // Legacy SNR-only selection (use recommendWaveformAndRate for better results)
    // DPSK works down to -11 dB SNR (tested), so we use it for low SNR
    // OFDM requires ~17 dB for reliable sync detection
    if (snr_db < 17.0f) {
        return protocol::WaveformMode::MC_DPSK;
    } else {
        return protocol::WaveformMode::OFDM_COX;
    }
}

ModemEngine::WaveformRecommendation ModemEngine::recommendWaveformAndRate(float snr_db, float fading_index) {
    // Delegate to shared algorithm in protocol namespace
    auto rec = protocol::recommendWaveformAndRate(snr_db, fading_index);
    LOG_MODEM(DEBUG, "recommendWaveformAndRate: SNR=%.1f, fading=%.2f -> %s %s (%.0f bps)",
              snr_db, fading_index,
              protocol::waveformModeToString(rec.waveform),
              codeRateToString(rec.rate),
              rec.estimated_throughput_bps);
    return rec;
}

void ModemEngine::setDPSKMode(DPSKModulation mod, int samples_per_symbol) {
    // Configure DPSK based on modulation type and symbol rate
    dpsk_config_.modulation = mod;
    dpsk_config_.samples_per_symbol = samples_per_symbol;

    // Recreate demodulator with new config
    dpsk_demodulator_ = std::make_unique<DPSKDemodulator>(dpsk_config_);

    const char* mod_name = "DQPSK";
    switch (mod) {
        case DPSKModulation::DBPSK: mod_name = "DBPSK"; break;
        case DPSKModulation::DQPSK: mod_name = "DQPSK"; break;
        case DPSKModulation::D8PSK: mod_name = "D8PSK"; break;
    }

    LOG_MODEM(INFO, "DPSK mode set: %s, %d samples/sym (%.1f baud), %.1f bps",
              mod_name,
              dpsk_config_.samples_per_symbol,
              dpsk_config_.symbol_rate(),
              dpsk_config_.raw_bps());
}

void ModemEngine::setCodecType(fec::CodecType type) {
    if (codec_type_ == type) return;  // No change

    codec_type_ = type;

    // Recreate encoder and decoder with new codec type
    encoder_ = fec::CodecFactory::create(type, config_.code_rate);
    decoder_ = fec::CodecFactory::create(type, data_code_rate_);

    // Update StreamingDecoder to use the same codec
    if (streaming_decoder_) {
        streaming_decoder_->setCodecType(type);
    }

    LOG_MODEM(INFO, "Codec type set to: %s", encoder_->getName().c_str());
}

fec::CodecType ModemEngine::recommendCodecType(float snr_db) {
    // Codec selection based on SNR:
    // - LDPC: Works well at moderate-high SNR (>5 dB), steep waterfall curve
    // - Convolutional: Better at very low SNR (<5 dB), graceful degradation
    // - Turbo: Excellent near Shannon limit, but high latency
    //
    // For now, always use LDPC since it's the only implemented codec.
    // When convolutional codec is implemented, use it for SNR < 5 dB.

    if (snr_db < 5.0f) {
        // Low SNR: Would prefer convolutional, but LDPC is all we have
        // return fec::CodecType::CONVOLUTIONAL;  // Future
        return fec::CodecType::LDPC;
    } else {
        // Moderate to high SNR: LDPC is optimal
        return fec::CodecType::LDPC;
    }
}

fec::CodecType ModemEngine::getCodecForWaveform(protocol::WaveformMode mode) {
    // Map waveform modes to optimal codec types:
    // - MC-DPSK (low SNR): Would benefit from convolutional (when implemented)
    // - OFDM_CHIRP (medium SNR): LDPC with R1/4 or R1/2
    // - OFDM_COX (high SNR): LDPC with higher rates
    //
    // For now, always return LDPC since it's the only implemented codec.

    switch (mode) {
        case protocol::WaveformMode::MC_DPSK:
            // Low SNR waveform - would prefer convolutional
            // return fec::CodecType::CONVOLUTIONAL;  // Future
            return fec::CodecType::LDPC;

        case protocol::WaveformMode::OFDM_CHIRP:
        case protocol::WaveformMode::OFDM_COX:
        default:
            return fec::CodecType::LDPC;
    }
}

int ModemEngine::recommendMCDPSKCarriers(float snr_db, float fading_index) {
    // MC-DPSK is used for SNR 0-10 dB range (above 10 dB switches to OFDM)
    // Testing with 20Hz CFO shows 8 carriers is optimal for this range:
    //   - 8 carriers: 100% at SNR 5, moderate fading, 20Hz CFO
    //   - 9+ carriers: 40-60% at same conditions
    //
    // Always use 8 carriers for MC-DPSK - it's the most robust choice
    // for the challenging conditions where MC-DPSK is selected.
    (void)snr_db;       // Unused - always 8
    (void)fading_index; // Unused - always 8

    return 8;
}

} // namespace gui
} // namespace ultra
