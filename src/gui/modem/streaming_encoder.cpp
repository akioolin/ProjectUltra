// StreamingEncoder - Unified TX encoder for all waveform types
//
// Mirrors StreamingDecoder to ensure TX/RX use identical configurations.

#include "streaming_encoder.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
#include "waveform/ofdm_cox_waveform.hpp"
#include "waveform/mc_dpsk_waveform.hpp"
#include "fec/frame_interleaver.hpp"
#include "ultra/logging.hpp"
#include <algorithm>

namespace ultra {
namespace gui {

namespace v2 = protocol::v2;

// ============================================================================
// CONSTRUCTION / DESTRUCTION
// ============================================================================

StreamingEncoder::StreamingEncoder() {
    // Initialize with default OFDM config (NVIS mode)
    ofdm_config_.fft_size = 1024;
    ofdm_config_.num_carriers = 59;
    ofdm_config_.sample_rate = 48000;
    ofdm_config_.center_freq = 1500.0f;
    ofdm_config_.cp_mode = CyclicPrefixMode::LONG;
    ofdm_config_.modulation = Modulation::DQPSK;
    ofdm_config_.code_rate = CodeRate::R1_4;
    ofdm_config_.use_pilots = true;
    ofdm_config_.pilot_spacing = 10;

    // Create default MC-DPSK waveform
    createWaveform();
    updateInterleaver();

    LOG_MODEM(INFO, "StreamingEncoder: Initialized (mode=%s, carriers=%d)",
              protocol::waveformModeToString(mode_), ofdm_config_.num_carriers);
}

StreamingEncoder::~StreamingEncoder() = default;

// ============================================================================
// MODE CONTROL
// ============================================================================

void StreamingEncoder::setMode(protocol::WaveformMode mode) {
    if (mode_ == mode) return;

    mode_ = mode;
    createWaveform();
    updateInterleaver();

    LOG_MODEM(INFO, "[%s] Mode changed to %s",
              log_prefix_.c_str(), protocol::waveformModeToString(mode));
}

void StreamingEncoder::setDataMode(Modulation mod, CodeRate rate) {
    if (modulation_ == mod && code_rate_ == rate) return;

    modulation_ = mod;
    code_rate_ = rate;
    ofdm_config_.modulation = mod;
    ofdm_config_.code_rate = rate;

    // Update waveform configuration
    if (waveform_) {
        waveform_->configure(mod, rate);
    }

    // Update interleaver (bits_per_symbol may change with modulation)
    updateInterleaver();

    LOG_MODEM(INFO, "[%s] Data mode: %s R%s",
              log_prefix_.c_str(),
              mod == Modulation::DQPSK ? "DQPSK" :
              mod == Modulation::D8PSK ? "D8PSK" : "other",
              rate == CodeRate::R1_4 ? "1/4" :
              rate == CodeRate::R1_2 ? "1/2" :
              rate == CodeRate::R2_3 ? "2/3" : "other");
}

void StreamingEncoder::setOFDMConfig(const ModemConfig& config) {
    ofdm_config_ = config;
    modulation_ = config.modulation;
    code_rate_ = config.code_rate;

    // Recreate waveform with new config
    createWaveform();
    updateInterleaver();

    LOG_MODEM(INFO, "[%s] OFDM config updated: FFT=%d, carriers=%d, pilots=%s spacing=%d",
              log_prefix_.c_str(), config.fft_size, config.num_carriers,
              config.use_pilots ? "yes" : "no", config.pilot_spacing);
}

void StreamingEncoder::setMCDPSKCarriers(int num_carriers) {
    if (mc_dpsk_carriers_ == num_carriers) return;

    mc_dpsk_carriers_ = num_carriers;

    // Recreate waveform if currently in MC-DPSK mode
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        createWaveform();
    }

    // Always update control waveform
    control_waveform_ = WaveformFactory::createMCDPSK(num_carriers);

    LOG_MODEM(INFO, "[%s] MC-DPSK carriers: %d", log_prefix_.c_str(), num_carriers);
}

// ============================================================================
// ENCODING
// ============================================================================

std::vector<float> StreamingEncoder::encodeFrame(const Bytes& frame_data) {
    if (!waveform_) {
        LOG_MODEM(ERROR, "[%s] No waveform!", log_prefix_.c_str());
        return {};
    }

    // Encode frame bytes (LDPC + interleaving)
    Bytes encoded = encodeFrameBytes(frame_data);

    // Generate full preamble
    Samples preamble = waveform_->generatePreamble();

    // Modulate
    Samples modulated = waveform_->modulate(encoded);

    // Combine preamble + data
    std::vector<float> result;
    result.reserve(preamble.size() + modulated.size());
    result.insert(result.end(), preamble.begin(), preamble.end());
    result.insert(result.end(), modulated.begin(), modulated.end());

    LOG_MODEM(INFO, "[%s] Encoded frame: %zu bytes -> %zu coded -> %zu samples",
              log_prefix_.c_str(), frame_data.size(), encoded.size(), result.size());

    return result;
}

std::vector<float> StreamingEncoder::encodeFrameLight(const Bytes& frame_data) {
    if (!waveform_) {
        LOG_MODEM(ERROR, "[%s] No waveform!", log_prefix_.c_str());
        return {};
    }

    // Encode frame bytes
    Bytes encoded = encodeFrameBytes(frame_data);

    // Generate light preamble if supported
    Samples preamble;
    if (waveform_->supportsDataPreamble()) {
        preamble = waveform_->generateDataPreamble();
    } else {
        // Fall back to full preamble
        preamble = waveform_->generatePreamble();
    }

    // Modulate
    Samples modulated = waveform_->modulate(encoded);

    // Combine
    std::vector<float> result;
    result.reserve(preamble.size() + modulated.size());
    result.insert(result.end(), preamble.begin(), preamble.end());
    result.insert(result.end(), modulated.begin(), modulated.end());

    LOG_MODEM(DEBUG, "[%s] Encoded frame (light): %zu bytes -> %zu samples",
              log_prefix_.c_str(), frame_data.size(), result.size());

    return result;
}

std::vector<float> StreamingEncoder::encodePing() {
    // PING is just the preamble (chirp) with no data
    // Always use MC-DPSK waveform for PING
    if (!control_waveform_) {
        control_waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_carriers_);
    }

    auto preamble = control_waveform_->generatePreamble();
    return std::vector<float>(preamble.begin(), preamble.end());
}

std::vector<float> StreamingEncoder::encodeDataOnly(const Bytes& frame_data) {
    if (!waveform_) {
        LOG_MODEM(ERROR, "[%s] No waveform!", log_prefix_.c_str());
        return {};
    }

    // Encode and modulate without preamble
    Bytes encoded = encodeFrameBytes(frame_data);
    Samples modulated = waveform_->modulate(encoded);

    return std::vector<float>(modulated.begin(), modulated.end());
}

// ============================================================================
// CONFIGURATION ACCESS
// ============================================================================

EncoderConfig StreamingEncoder::getConfig() const {
    EncoderConfig cfg;
    cfg.mode = mode_;
    cfg.modulation = modulation_;
    cfg.code_rate = code_rate_;
    cfg.num_carriers = ofdm_config_.num_carriers;
    cfg.data_carriers = calculateDataCarriers();
    cfg.bits_per_symbol = cfg.data_carriers * getBitsPerSymbol(modulation_);
    cfg.use_pilots = ofdm_config_.use_pilots;
    cfg.pilot_spacing = ofdm_config_.pilot_spacing;
    cfg.use_channel_interleave = use_channel_interleave_;
    cfg.use_frame_interleave = use_frame_interleave_;
    return cfg;
}

std::string StreamingEncoder::verifyConfigMatch(const EncoderConfig& other) const {
    auto mine = getConfig();
    std::string mismatches;

    if (mine.mode != other.mode) {
        mismatches += "mode mismatch; ";
    }
    if (mine.modulation != other.modulation) {
        mismatches += "modulation mismatch; ";
    }
    if (mine.code_rate != other.code_rate) {
        mismatches += "code_rate mismatch; ";
    }
    if (mine.num_carriers != other.num_carriers) {
        char buf[64];
        snprintf(buf, sizeof(buf), "num_carriers: TX=%d RX=%d; ",
                 mine.num_carriers, other.num_carriers);
        mismatches += buf;
    }
    if (mine.data_carriers != other.data_carriers) {
        char buf[64];
        snprintf(buf, sizeof(buf), "data_carriers: TX=%d RX=%d; ",
                 mine.data_carriers, other.data_carriers);
        mismatches += buf;
    }
    if (mine.bits_per_symbol != other.bits_per_symbol) {
        char buf[64];
        snprintf(buf, sizeof(buf), "bits_per_symbol: TX=%d RX=%d; ",
                 mine.bits_per_symbol, other.bits_per_symbol);
        mismatches += buf;
    }
    if (mine.use_pilots != other.use_pilots) {
        mismatches += "use_pilots mismatch; ";
    }
    if (mine.pilot_spacing != other.pilot_spacing) {
        char buf[64];
        snprintf(buf, sizeof(buf), "pilot_spacing: TX=%d RX=%d; ",
                 mine.pilot_spacing, other.pilot_spacing);
        mismatches += buf;
    }
    if (mine.use_channel_interleave != other.use_channel_interleave) {
        char buf[64];
        snprintf(buf, sizeof(buf), "channel_interleave: TX=%s RX=%s; ",
                 mine.use_channel_interleave ? "yes" : "no",
                 other.use_channel_interleave ? "yes" : "no");
        mismatches += buf;
    }
    if (mine.use_frame_interleave != other.use_frame_interleave) {
        mismatches += "frame_interleave mismatch; ";
    }

    return mismatches;
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

void StreamingEncoder::createWaveform() {
    // Always have MC-DPSK ready for control frames
    if (!control_waveform_) {
        control_waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_carriers_);
    }

    switch (mode_) {
        case protocol::WaveformMode::MC_DPSK:
            waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_carriers_);
            break;

        case protocol::WaveformMode::OFDM_COX:
            waveform_ = std::make_unique<OFDMNvisWaveform>(ofdm_config_);
            static_cast<OFDMNvisWaveform*>(waveform_.get())->configure(
                modulation_, code_rate_);
            break;

        case protocol::WaveformMode::OFDM_CHIRP:
        default:
            waveform_ = std::make_unique<OFDMChirpWaveform>(ofdm_config_);
            static_cast<OFDMChirpWaveform*>(waveform_.get())->configure(
                modulation_, code_rate_);
            break;
    }

    LOG_MODEM(DEBUG, "[%s] Created waveform: %s",
              log_prefix_.c_str(), protocol::waveformModeToString(mode_));
}

void StreamingEncoder::updateInterleaver() {
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        // No channel interleaving for MC-DPSK
        channel_interleaver_.reset();
        return;
    }

    // Calculate bits per OFDM symbol
    int data_carriers = calculateDataCarriers();
    int bits_per_carrier = getBitsPerSymbol(modulation_);
    int bits_per_symbol = data_carriers * bits_per_carrier;

    // Create channel interleaver
    channel_interleaver_ = std::make_unique<ChannelInterleaver>(
        bits_per_symbol, v2::LDPC_CODEWORD_BITS);

    LOG_MODEM(INFO, "[%s] Channel interleaver: %d data carriers × %d bits = %d bits/symbol",
              log_prefix_.c_str(), data_carriers, bits_per_carrier, bits_per_symbol);
}

int StreamingEncoder::calculateDataCarriers() const {
    if (!ofdm_config_.use_pilots) {
        return ofdm_config_.num_carriers;
    }

    // Calculate pilot count (same formula as modulator/demodulator)
    int pilot_count = (ofdm_config_.num_carriers + ofdm_config_.pilot_spacing - 1)
                      / ofdm_config_.pilot_spacing;
    return ofdm_config_.num_carriers - pilot_count;
}

Bytes StreamingEncoder::encodeFrameBytes(const Bytes& frame_data) {
    Bytes tx_data = frame_data;  // Mutable copy for header patching

    bool is_ofdm = (mode_ == protocol::WaveformMode::OFDM_CHIRP ||
                    mode_ == protocol::WaveformMode::OFDM_COX);

    if (!is_ofdm) {
        // MC-DPSK: variable CW encoding
        auto cws = v2::encodeFrameWithLDPC(tx_data, code_rate_);

        // Patch total_cw in header if needed
        uint8_t actual_cw = static_cast<uint8_t>(cws.size());
        if (tx_data.size() >= 17 && tx_data[12] != actual_cw) {
            LOG_MODEM(DEBUG, "[%s] Patching total_cw %d -> %d",
                      log_prefix_.c_str(), tx_data[12], actual_cw);
            tx_data[12] = actual_cw;
            // Recalculate header CRC
            uint16_t hcrc = v2::ControlFrame::calculateCRC(tx_data.data(), 15);
            tx_data[15] = (hcrc >> 8) & 0xFF;
            tx_data[16] = hcrc & 0xFF;
            // Re-encode with corrected header
            cws = v2::encodeFrameWithLDPC(tx_data, code_rate_);
        }

        // Concatenate codewords
        Bytes encoded;
        for (const auto& cw : cws) {
            encoded.insert(encoded.end(), cw.begin(), cw.end());
        }

        LOG_MODEM(DEBUG, "[%s] MC-DPSK: %zu bytes -> %zu CWs (%zu coded)",
                  log_prefix_.c_str(), tx_data.size(), cws.size(), encoded.size());
        return encoded;
    }

    // OFDM: 4-CW fixed frame encoding with frame interleaving
    // Channel interleaving is controlled by use_channel_interleave_ flag
    Bytes encoded = v2::encodeFixedFrame(tx_data, code_rate_, use_channel_interleave_);

    LOG_MODEM(DEBUG, "[%s] OFDM: %zu bytes -> 4 CWs (%zu coded, frame_interleave=%s, channel_interleave=%s)",
              log_prefix_.c_str(), tx_data.size(), encoded.size(),
              use_frame_interleave_ ? "yes" : "no",
              use_channel_interleave_ ? "yes" : "no");

    return encoded;
}

} // namespace gui
} // namespace ultra
