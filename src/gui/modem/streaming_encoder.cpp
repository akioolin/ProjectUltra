// StreamingEncoder - Unified TX encoder for all waveform types
//
// Mirrors StreamingDecoder to ensure TX/RX use identical configurations.

#include "streaming_encoder.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
#include "waveform/ofdm_cox_waveform.hpp"
#include "waveform/mc_dpsk_waveform.hpp"
#include "fec/frame_interleaver.hpp"
#include "fec/burst_interleaver.hpp"
#include "gui/startup_trace.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include <algorithm>
#include <cmath>

namespace ultra {
namespace gui {

namespace v2 = protocol::v2;

namespace {

bool isControlFrameBytes(const Bytes& frame_data) {
    if (frame_data.size() < 3) {
        return false;
    }
    auto ft = static_cast<v2::FrameType>(frame_data[2]);
    return v2::isControlFrame(ft);
}

void markFirstLTSSymbolForBurstGroup(Samples& preamble, size_t symbol_samples) {
    if (preamble.empty() || symbol_samples == 0) {
        return;
    }

    // Full OFDM preambles are [sync][LTS][LTS]; light data preambles are
    // [LTS][LTS]. The burst marker belongs on the first LTS symbol, not on
    // the chirp/STS sync portion of a full anchor.
    const size_t lts_pair_samples = symbol_samples * 2;
    const size_t lts_start = preamble.size() > lts_pair_samples
        ? preamble.size() - lts_pair_samples
        : 0;
    const size_t lts_symbol_end = std::min(preamble.size(), lts_start + symbol_samples);
    for (size_t j = lts_start; j < lts_symbol_end; ++j) {
        preamble[j] = -preamble[j];
    }
}

}  // namespace

// ============================================================================
// CONSTRUCTION / DESTRUCTION
// ============================================================================

StreamingEncoder::StreamingEncoder() {
    startupTrace("StreamingEncoder", "ctor-enter");

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
    startupTrace("StreamingEncoder", "defaults-set");

    // Create default MC-DPSK waveform
    startupTrace("StreamingEncoder", "create-waveform-enter");
    createWaveform();
    startupTrace("StreamingEncoder", "create-waveform-exit");
    startupTrace("StreamingEncoder", "update-interleaver-enter");
    updateInterleaver();
    startupTrace("StreamingEncoder", "update-interleaver-exit");

    startupTrace("StreamingEncoder", "ctor-log-enter");
    LOG_MODEM(INFO, "StreamingEncoder: Initialized (mode=%s, carriers=%d)",
              protocol::waveformModeToString(mode_), ofdm_config_.num_carriers);
    startupTrace("StreamingEncoder", "ctor-log-exit");
    startupTrace("StreamingEncoder", "ctor-exit");
}

StreamingEncoder::~StreamingEncoder() = default;

void StreamingEncoder::setPaprReductionEnabled(bool enable) {
    papr_reduction_enabled_ = enable;
    last_papr_reduction_ = {};
}

void StreamingEncoder::setPaprReductionThresholdDb(float threshold_db) {
    if (!std::isfinite(threshold_db) || !(threshold_db > 0.0f)) {
        threshold_db = phy::kOfdmPaprReductionDefaultThresholdDb;
    }
    papr_reduction_threshold_db_ = threshold_db;
}

void StreamingEncoder::applyPaprReductionIfNeeded(std::vector<float>& samples,
                                                  bool is_ofdm,
                                                  bool is_control_frame,
                                                  const char* label) {
    last_papr_reduction_ = {};
    if (!papr_reduction_enabled_ || !is_ofdm || is_control_frame ||
        samples.empty()) {
        return;
    }

    last_papr_reduction_ = phy::applyPaprReduction(
        samples, papr_reduction_threshold_db_, true);
    if (last_papr_reduction_.applied) {
        LOG_MODEM(INFO,
                  "[%s] PAPR reduction %s threshold=%.2f dB pre=%.2f dB "
                  "post=%.2f dB reduction=%.2f dB clips=%zu rms_delta=%.2f dB",
                  log_prefix_.c_str(),
                  label ? label : "-",
                  last_papr_reduction_.threshold_db,
                  last_papr_reduction_.pre_papr_db,
                  last_papr_reduction_.post_papr_db,
                  last_papr_reduction_.pre_papr_db -
                      last_papr_reduction_.post_papr_db,
                  last_papr_reduction_.clipped_samples,
                  last_papr_reduction_.in_band_rms_delta_db);
    }
}

void StreamingEncoder::setBurstInterleaveGroupSize(int size) {
    burst_group_size_ = ofdm_link_adaptation::sanitizeBurstGroupSize(size);
}

void StreamingEncoder::setCarrierMask(uint64_t active_mask) {
    carrier_mask_ = active_mask;
    if (waveform_) {
        waveform_->setCarrierMask(active_mask);
    }
}

void StreamingEncoder::setCarrierLdpcInterleaver(bool enable) {
    use_carrier_ldpc_interleaver_ = enable;
    if (waveform_) {
        waveform_->setCarrierLdpcInterleaverEnabled(enable);
    }
}

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
        // Sync pilot_spacing from waveform (coherent modes use denser pilots)
        int spacing = waveform_->getPilotSpacing();
        if (spacing > 0) ofdm_config_.pilot_spacing = spacing;
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
    mc_dpsk_config_.num_carriers = num_carriers;

    // Recreate waveform if currently in MC-DPSK mode
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        createWaveform();
    }

    // Always update control waveform
    control_waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);

    LOG_MODEM(INFO, "[%s] MC-DPSK carriers: %d", log_prefix_.c_str(), num_carriers);
}

void StreamingEncoder::setMCDPSKConfig(const MultiCarrierDPSKConfig& config) {
    const bool changed =
        mc_dpsk_config_.num_carriers != config.num_carriers ||
        mc_dpsk_config_.samples_per_symbol != config.samples_per_symbol ||
        mc_dpsk_config_.bits_per_symbol != config.bits_per_symbol ||
        mc_dpsk_config_.freq_low != config.freq_low ||
        mc_dpsk_config_.freq_high != config.freq_high ||
        mc_dpsk_config_.chirp_f_start != config.chirp_f_start ||
        mc_dpsk_config_.chirp_f_end != config.chirp_f_end ||
        mc_dpsk_config_.chirp_duration_ms != config.chirp_duration_ms ||
        mc_dpsk_config_.use_dual_chirp != config.use_dual_chirp;
    if (!changed) return;

    mc_dpsk_config_ = config;
    mc_dpsk_carriers_ = config.num_carriers;

    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        createWaveform();
    }
    control_waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);

    LOG_MODEM(INFO, "[%s] MC-DPSK config: carriers=%d sps=%d bits/sym=%d raw=%.1f bps",
              log_prefix_.c_str(), mc_dpsk_config_.num_carriers,
              mc_dpsk_config_.samples_per_symbol, mc_dpsk_config_.bits_per_symbol,
              mc_dpsk_config_.getRawBitRate());
}

void StreamingEncoder::setNarrowbandControl(bool narrowband) {
    narrowband_control_ = narrowband;
    if (narrowband) {
        control_waveform_ = WaveformFactory::createNarrowbandMCDPSK();
        // When in MC-DPSK mode (handshake), also switch main waveform to narrowband
        // so CONNECT/CONNECT_ACK frames use narrowband chirp + carriers
        if (mode_ == protocol::WaveformMode::MC_DPSK) {
            waveform_ = WaveformFactory::createNarrowbandMCDPSK();
        }
        LOG_MODEM(INFO, "[%s] Control waveform: narrowband MC-DPSK (500 Hz)", log_prefix_.c_str());
    } else {
        control_waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);
        if (mode_ == protocol::WaveformMode::MC_DPSK) {
            waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);
        }
        LOG_MODEM(INFO, "[%s] Control waveform: wideband MC-DPSK (%d carriers)", log_prefix_.c_str(), mc_dpsk_carriers_);
    }
}

// ============================================================================
// ENCODING
// ============================================================================

std::vector<float> StreamingEncoder::encodeFrame(const Bytes& frame_data) {
    if (!waveform_) {
        LOG_MODEM(ERROR, "[%s] No waveform!", log_prefix_.c_str());
        return {};
    }

    bool is_ofdm = protocol::isOFDMMode(mode_);
    bool use_control_profile = is_ofdm && isControlFrameBytes(frame_data);
    Modulation tx_mod = use_control_profile ? Modulation::DQPSK : modulation_;
    CodeRate tx_rate = use_control_profile ? CodeRate::R1_4 : code_rate_;

    if (force_full_preamble_once_ && is_ofdm) {
        force_full_preamble_once_ = false;
        LOG_MODEM(INFO, "[%s] Full preamble OFDM timing anchor emitted",
                  log_prefix_.c_str());
    }

    if (use_control_profile) {
        LOG_MODEM(DEBUG, "[%s] OFDM control profile TX: %s %s",
                  log_prefix_.c_str(), modulationToString(tx_mod), codeRateToString(tx_rate));
        waveform_->configure(tx_mod, tx_rate);
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

    if (use_control_profile && (modulation_ != tx_mod || code_rate_ != tx_rate)) {
        waveform_->configure(modulation_, code_rate_);
    }

    applyPaprReductionIfNeeded(result, is_ofdm, use_control_profile, "frame");

    LOG_MODEM(INFO, "[%s] Encoded frame: %zu bytes -> %zu coded -> %zu samples",
              log_prefix_.c_str(), frame_data.size(), encoded.size(), result.size());

    return result;
}

std::vector<float> StreamingEncoder::encodeFrameLight(const Bytes& frame_data) {
    if (force_full_preamble_once_) {
        force_full_preamble_once_ = false;
        LOG_MODEM(INFO, "[%s] Full preamble forced for OFDM timing anchor",
                  log_prefix_.c_str());
        return encodeFrame(frame_data);
    }

    if (!waveform_) {
        LOG_MODEM(ERROR, "[%s] No waveform!", log_prefix_.c_str());
        return {};
    }

    bool is_ofdm = protocol::isOFDMMode(mode_);
    bool use_control_profile = is_ofdm && isControlFrameBytes(frame_data);
    Modulation tx_mod = use_control_profile ? Modulation::DQPSK : modulation_;
    CodeRate tx_rate = use_control_profile ? CodeRate::R1_4 : code_rate_;

    if (use_control_profile) {
        LOG_MODEM(DEBUG, "[%s] OFDM control profile TX(light): %s %s",
                  log_prefix_.c_str(), modulationToString(tx_mod), codeRateToString(tx_rate));
        waveform_->configure(tx_mod, tx_rate);
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

    if (use_control_profile && (modulation_ != tx_mod || code_rate_ != tx_rate)) {
        waveform_->configure(modulation_, code_rate_);
    }

    applyPaprReductionIfNeeded(result, is_ofdm, use_control_profile, "frame-light");

    LOG_MODEM(DEBUG, "[%s] Encoded frame (light): %zu bytes -> %zu samples",
              log_prefix_.c_str(), frame_data.size(), result.size());

    return result;
}

std::vector<float> StreamingEncoder::encodeBurstLight(const std::vector<Bytes>& frame_data_list) {
    if (frame_data_list.empty()) return {};
    if (!waveform_) {
        LOG_MODEM(ERROR, "[%s] No waveform!", log_prefix_.c_str());
        return {};
    }

    // Single frame: just use normal encodeFrameLight
    if (frame_data_list.size() == 1) {
        return encodeFrameLight(frame_data_list[0]);
    }

    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        force_full_preamble_once_ = false;
        size_t total_cw = 0;
        std::vector<Bytes> encoded_frames;
        encoded_frames.reserve(frame_data_list.size());
        for (const auto& fd : frame_data_list) {
            Bytes encoded = encodeFrameBytes(fd);
            if (encoded.empty()) {
                LOG_MODEM(WARN, "[%s] MC-DPSK burst: dropping empty encoded frame",
                          log_prefix_.c_str());
                return {};
            }
            total_cw += encoded.size() / v2::LDPC_CODEWORD_BYTES;
            encoded_frames.push_back(std::move(encoded));
        }

        Samples preamble = waveform_->generatePreamble();
        std::vector<float> result;
        result.reserve(preamble.size());
        result.insert(result.end(), preamble.begin(), preamble.end());
        for (const auto& encoded : encoded_frames) {
            Samples modulated = waveform_->modulate(encoded);
            result.insert(result.end(), modulated.begin(), modulated.end());
        }

        LOG_MODEM(INFO, "[%s] MC-DPSK continuous burst: %zu frames, %zu CWs -> %zu samples",
                  log_prefix_.c_str(), frame_data_list.size(), total_cw, result.size());
        return result;
    }

    // Phase 1: LDPC encode all frames
    std::vector<Bytes> encoded_frames;
    for (const auto& fd : frame_data_list) {
        encoded_frames.push_back(encodeFrameBytes(fd));
    }

    // Phase 2: Group into N-frame subgroups, burst-interleave each group
    // Track which groups are burst-interleaved (for LTS marker)
    const int BURST_GROUP_SIZE = std::max(2, burst_group_size_);
    std::vector<bool> frame_is_group_start(encoded_frames.size(), false);
    size_t interleaved_groups = 0;

    if (use_burst_interleave_) {
        size_t full_groups = encoded_frames.size() / BURST_GROUP_SIZE;
        for (size_t g = 0; g < full_groups; g++) {
            size_t base = g * BURST_GROUP_SIZE;

            // Extract the group
            std::vector<Bytes> group(encoded_frames.begin() + base,
                                     encoded_frames.begin() + base + BURST_GROUP_SIZE);

            // Burst-interleave the coded bytes
            auto interleaved = fec::BurstInterleaver::interleave(
                group, fixed_frame_codewords_);

            // Replace in-place
            for (int i = 0; i < BURST_GROUP_SIZE; i++) {
                encoded_frames[base + i] = interleaved[i];
            }
            frame_is_group_start[base] = true;
            interleaved_groups++;

            LOG_MODEM(INFO, "[%s] Burst interleaved group %zu: frames %zu-%zu",
                      log_prefix_.c_str(), g, base, base + BURST_GROUP_SIZE - 1);
        }
    }

    // Phase 3: Modulate with preambles
    std::vector<float> result;
    const bool force_first_full_preamble = force_full_preamble_once_;
    force_full_preamble_once_ = false;

    for (size_t i = 0; i < encoded_frames.size(); i++) {
        // Generate preamble (LTS training symbols)
        Samples preamble;
        if (i == 0 && (force_first_full_preamble || waveform_->supportsDataPreamble())) {
            preamble = waveform_->generatePreamble();
            if (force_first_full_preamble) {
                LOG_MODEM(INFO, "[%s] Full preamble forced for first burst frame OFDM timing anchor",
                          log_prefix_.c_str());
            } else {
                LOG_MODEM(INFO, "[%s] Full preamble emitted for first OFDM burst frame timing anchor",
                          log_prefix_.c_str());
            }
        } else if (i == 0) {
            // Waveforms without a separate data preamble already use their full preamble.
            preamble = waveform_->generatePreamble();
        } else {
            // All subsequent frames: LTS data preamble
            preamble = waveform_->generateDataPreamble();
        }

        // Negate first LTS symbol for burst-interleaved group starts
        if (frame_is_group_start[i] && !preamble.empty()) {
            // LTS preamble = 2 identical symbols. Negate the first one.
            // detectDataSync() correlates sym[n] with sym[n+L]:
            //   Normal: P_real > 0 (same signs multiply to positive)
            //   Negated: P_real < 0 (opposite signs multiply to negative)
            // abs() ensures detection still works; sign indicates burst marker.
            markFirstLTSSymbolForBurstGroup(preamble, ofdm_config_.getSymbolDuration());
            LOG_MODEM(INFO, "[%s] LTS marker: negated first symbol for frame %zu (group start)",
                      log_prefix_.c_str(), i);
        }

        // Modulate data
        Samples modulated = waveform_->modulate(encoded_frames[i]);

        result.insert(result.end(), preamble.begin(), preamble.end());
        result.insert(result.end(), modulated.begin(), modulated.end());
    }

    LOG_MODEM(INFO, "[%s] Encoded burst: %zu blocks -> %zu samples (burst_interleave=%s groups=%zu)",
              log_prefix_.c_str(), frame_data_list.size(), result.size(),
              interleaved_groups > 0 ? "yes" : "no", interleaved_groups);

    applyPaprReductionIfNeeded(result, protocol::isOFDMMode(mode_), false, "burst-light");

    return result;
}

std::vector<float> StreamingEncoder::encodePing() {
    // PING is just the preamble (chirp) with no data
    // Always use MC-DPSK waveform for PING
    if (!control_waveform_) {
        control_waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);
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
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        cfg.num_carriers = mc_dpsk_config_.num_carriers;
        cfg.data_carriers = mc_dpsk_config_.num_carriers;
        cfg.bits_per_symbol = mc_dpsk_config_.num_carriers * mc_dpsk_config_.bits_per_symbol;
        cfg.use_pilots = false;
        cfg.pilot_spacing = 0;
    } else {
        cfg.num_carriers = ofdm_config_.num_carriers;
        cfg.data_carriers = calculateDataCarriers();
        cfg.bits_per_symbol = ofdm_link_adaptation::bitsPerOFDMSymbol(
            static_cast<int>(ofdm_config_.num_carriers),
            ofdm_config_.use_pilots,
            static_cast<int>(ofdm_config_.pilot_spacing),
            modulation_);
        cfg.use_pilots = ofdm_config_.use_pilots;
        cfg.pilot_spacing = ofdm_config_.pilot_spacing;
    }
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
    startupTrace("StreamingEncoder", "create-waveform-internal-enter");

    // Always have MC-DPSK ready for control frames
    if (!control_waveform_) {
        startupTrace("StreamingEncoder", "create-control-waveform-enter");
        control_waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);
        startupTrace("StreamingEncoder", "create-control-waveform-exit");
    }

    switch (mode_) {
        case protocol::WaveformMode::MC_DPSK:
            startupTrace("StreamingEncoder", "create-main-waveform-mcdpsk-enter");
            if (narrowband_control_) {
                waveform_ = WaveformFactory::createNarrowbandMCDPSK();
            } else {
                waveform_ = WaveformFactory::createMCDPSK(mc_dpsk_config_);
            }
            startupTrace("StreamingEncoder", "create-main-waveform-mcdpsk-exit");
            break;

        case protocol::WaveformMode::OFDM_COX:
            startupTrace("StreamingEncoder", "create-main-waveform-ofdm-cox-enter");
            waveform_ = std::make_unique<OFDMNvisWaveform>(ofdm_config_);
            static_cast<OFDMNvisWaveform*>(waveform_.get())->configure(
                modulation_, code_rate_);
            startupTrace("StreamingEncoder", "create-main-waveform-ofdm-cox-exit");
            break;

        case protocol::WaveformMode::OFDM_NARROW:
            startupTrace("StreamingEncoder", "create-main-waveform-ofdm-narrow-enter");
            waveform_ = std::make_unique<OFDMChirpWaveform>(ofdm_config_, protocol::WaveformMode::OFDM_NARROW);
            static_cast<OFDMChirpWaveform*>(waveform_.get())->configure(
                modulation_, code_rate_);
            startupTrace("StreamingEncoder", "create-main-waveform-ofdm-narrow-exit");
            break;

        case protocol::WaveformMode::OFDM_CHIRP:
        default:
            startupTrace("StreamingEncoder", "create-main-waveform-ofdm-chirp-enter");
            waveform_ = std::make_unique<OFDMChirpWaveform>(ofdm_config_);
            static_cast<OFDMChirpWaveform*>(waveform_.get())->configure(
                modulation_, code_rate_);
            startupTrace("StreamingEncoder", "create-main-waveform-ofdm-chirp-exit");
            break;
    }

    // Sync pilot_spacing from waveform after configure()
    // (coherent modes like QPSK use denser pilots than the config may specify)
    if (waveform_) {
        int spacing = waveform_->getPilotSpacing();
        if (spacing > 0) ofdm_config_.pilot_spacing = spacing;
        waveform_->setCarrierMask(carrier_mask_);
        waveform_->setCarrierLdpcInterleaverEnabled(use_carrier_ldpc_interleaver_);
    }

    LOG_MODEM(DEBUG, "[%s] Created waveform: %s",
              log_prefix_.c_str(), protocol::waveformModeToString(mode_));
    startupTrace("StreamingEncoder", "create-waveform-internal-exit");
}

void StreamingEncoder::updateInterleaver() {
    if (mode_ == protocol::WaveformMode::MC_DPSK) {
        // No channel interleaving for MC-DPSK
        channel_interleaver_.reset();
        return;
    }

    // Calculate bits per OFDM symbol
    int data_carriers = calculateDataCarriers();
    int bits_per_carrier = static_cast<int>(getBitsPerSymbol(modulation_));
    int bits_per_symbol = ofdm_link_adaptation::bitsPerOFDMSymbol(
        static_cast<int>(ofdm_config_.num_carriers),
        ofdm_config_.use_pilots,
        static_cast<int>(ofdm_config_.pilot_spacing),
        modulation_);

    // Create channel interleaver
    channel_interleaver_ = std::make_unique<ChannelInterleaver>(
        bits_per_symbol, v2::LDPC_CODEWORD_BITS);

    LOG_MODEM(INFO, "[%s] Channel interleaver: %d data carriers × %d bits = %d bits/symbol",
              log_prefix_.c_str(), data_carriers, bits_per_carrier, bits_per_symbol);
}

int StreamingEncoder::calculateDataCarriers() const {
    return ofdm_link_adaptation::dataCarrierCount(
        static_cast<int>(ofdm_config_.num_carriers),
        ofdm_config_.use_pilots,
        static_cast<int>(ofdm_config_.pilot_spacing));
}

Bytes StreamingEncoder::encodeFrameBytes(const Bytes& frame_data) {
    Bytes tx_data = frame_data;  // Mutable copy for header patching

    bool is_ofdm = protocol::isOFDMMode(mode_);

    if (!is_ofdm) {
        // MC-DPSK: Simple variable CW encoding (no frame interleaving)
        // Control frames (20 bytes): ACK, NACK, etc. - encode as-is, no patching
        // Data frames (>20 bytes): May need total_cw patching

        if (tx_data.size() >= 3 &&
            tx_data[2] == static_cast<uint8_t>(v2::FrameType::DATA_REPAIR)) {
            auto repair = v2::DataRepairFrame::deserialize(tx_data);
            if (!repair) {
                LOG_MODEM(WARN, "[%s] MC-DPSK: dropping invalid DATA_REPAIR frame",
                          log_prefix_.c_str());
                return {};
            }
            auto cws = v2::encodeInfoCodewordsWithLDPC(repair->infoCodewords(), repair->rate);
            Bytes encoded;
            for (const auto& cw : cws) {
                encoded.insert(encoded.end(), cw.begin(), cw.end());
            }
            LOG_MODEM(INFO, "[%s] MC-DPSK DATA_REPAIR: seq=%d bitmap=0x%04X repair_cw=%d (%zu coded)",
                      log_prefix_.c_str(), repair->target_seq, repair->repair_bitmap,
                      repair->repair_count, encoded.size());
            return encoded;
        }

        // Check if this is a control frame (20 bytes, type 0x10-0x21 or 0x40)
        bool is_control_frame = false;
        if (tx_data.size() == 20 && tx_data.size() >= 3) {
            uint8_t frame_type = tx_data[2];
            // Control frame types: PROBE(0x10), PROBE_ACK(0x11), CONNECT(0x12),
            // CONNECT_ACK(0x13), CONNECT_NAK(0x14), DISCONNECT(0x15), KEEPALIVE(0x16),
            // MODE_CHANGE(0x17), ACK(0x20), NACK(0x21), BEACON(0x40)
            is_control_frame = (frame_type >= 0x10 && frame_type <= 0x21) ||
                               (frame_type == 0x40);
        }

        auto cws = v2::encodeFrameWithLDPC(tx_data, code_rate_);

        // Only patch DATA frames (not control frames)
        // DATA frames have total_cw at byte 12, header_crc at bytes 15-16
        if (!is_control_frame && tx_data.size() >= 17) {
            uint8_t actual_cw = static_cast<uint8_t>(cws.size());
            if (tx_data[12] != actual_cw) {
                LOG_MODEM(DEBUG, "[%s] Patching total_cw %d -> %d",
                          log_prefix_.c_str(), tx_data[12], actual_cw);
                tx_data[12] = actual_cw;
                // Recalculate header CRC (over first 15 bytes for DATA frames)
                uint16_t hcrc = v2::ControlFrame::calculateCRC(tx_data.data(), 15);
                tx_data[15] = (hcrc >> 8) & 0xFF;
                tx_data[16] = hcrc & 0xFF;
                // Recalculate frame CRC (covers patched bytes 12, 15-16)
                if (tx_data.size() >= v2::DataFrame::HEADER_SIZE + v2::DataFrame::CRC_SIZE) {
                    uint16_t fcrc = v2::ControlFrame::calculateCRC(tx_data.data(), tx_data.size() - 2);
                    tx_data[tx_data.size() - 2] = (fcrc >> 8) & 0xFF;
                    tx_data[tx_data.size() - 1] = fcrc & 0xFF;
                }
                // Re-encode with corrected header and frame CRC
                cws = v2::encodeFrameWithLDPC(tx_data, code_rate_);
            }
        }

        // Concatenate codewords
        Bytes encoded;
        for (const auto& cw : cws) {
            encoded.insert(encoded.end(), cw.begin(), cw.end());
        }

        LOG_MODEM(DEBUG, "[%s] MC-DPSK: %zu bytes -> %zu CWs (%zu coded, control=%s)",
                  log_prefix_.c_str(), tx_data.size(), cws.size(), encoded.size(),
                  is_control_frame ? "yes" : "no");
        return encoded;
    }

    // OFDM: Check if this is a control frame or data/connect frame
    // Control frames (ACK, NACK, MODE_CHANGE, DISCONNECT, etc.) are 20 bytes = 1 CW, no interleaving
    // Connect handshake frames are always MC-DPSK and do not reach this path.
    // Data frames use fixed-CW frame encoding with frame interleaving
    bool is_variable_cw_frame = false;
    if (tx_data.size() >= 3) {
        uint8_t frame_type = tx_data[2];
        auto ft = static_cast<v2::FrameType>(frame_type);
        if (v2::isControlFrame(ft)) {
            is_variable_cw_frame = true;
        }
        // Non-control frames go through encodeFixedFrame() for fixed-CW interleaving
    }

    if (is_variable_cw_frame) {
        // Variable-CW encoding (no frame interleaving needed)
        // Control frames = 1 CW
        // Control frames always use R1/4: exact fit (20 bytes = 162 info bits / 8)
        // and maximum LDPC redundancy for fading resilience
        auto cws = v2::encodeFrameWithLDPC(tx_data, CodeRate::R1_4);
        uint8_t actual_cw = static_cast<uint8_t>(cws.size());

        // Patch total_cw for ConnectFrames (CONNECT, DISCONNECT, etc.)
        // ConnectFrame::serialize() hardcodes total_cw=4 at byte 12, but actual
        // encoding may produce fewer CWs (e.g. 2 at R1/2 for 44-byte ConnectFrame)
        // ControlFrames (20 bytes) don't have total_cw field — parseHeader() returns 1
        auto ft = static_cast<v2::FrameType>(tx_data[2]);
        if (v2::isConnectFrame(ft) &&
            tx_data.size() > v2::ControlFrame::SIZE &&
            tx_data[12] != actual_cw) {
            LOG_MODEM(INFO, "[%s] OFDM: Patching ConnectFrame total_cw %d -> %d",
                      log_prefix_.c_str(), tx_data[12], actual_cw);
            tx_data[12] = actual_cw;
            // Recalculate header CRC (bytes 0..14 → CRC at 15-16)
            uint16_t hcrc = v2::ControlFrame::calculateCRC(tx_data.data(), 15);
            tx_data[15] = (hcrc >> 8) & 0xFF;
            tx_data[16] = hcrc & 0xFF;
            // Recalculate frame CRC (last 2 bytes cover everything before them)
            size_t fcrc_offset = tx_data.size() - 2;
            uint16_t fcrc = v2::ControlFrame::calculateCRC(tx_data.data(), fcrc_offset);
            tx_data[fcrc_offset] = (fcrc >> 8) & 0xFF;
            tx_data[fcrc_offset + 1] = fcrc & 0xFF;
            // Re-encode with patched header (always R1/4 for control frames)
            cws = v2::encodeFrameWithLDPC(tx_data, CodeRate::R1_4);
        }

        Bytes encoded;
        for (const auto& cw : cws) {
            encoded.insert(encoded.end(), cw.begin(), cw.end());
        }

        LOG_MODEM(INFO, "[%s] OFDM control: %zu bytes -> %zu CW (%zu coded bytes), rate=R1/4 (hardened)",
                  log_prefix_.c_str(), tx_data.size(), cws.size(), encoded.size());
        return encoded;
    }

    // Large OFDM block frames explicitly advertise a size beyond the fixed
    // aggregation range and use
    // variable-CW encoding. The fixed-frame path remains the default for normal
    // ARQ chunks so they keep frame-level interleaving.
    if (tx_data.size() >= v2::DataFrame::HEADER_SIZE &&
        tx_data[12] > v2::kMaxFixedFrameCodewords) {
        auto cws = v2::encodeFrameWithLDPC(tx_data, code_rate_);
        Bytes encoded;
        for (const auto& cw : cws) {
            encoded.insert(encoded.end(), cw.begin(), cw.end());
        }

        LOG_MODEM(INFO, "[%s] OFDM variable data: %zu bytes -> %zu CWs (%zu coded)",
                  log_prefix_.c_str(), tx_data.size(), cws.size(), encoded.size());
        return encoded;
    }

    int frame_cw_count = fixed_frame_codewords_;
    if (tx_data.size() >= v2::DataFrame::HEADER_SIZE &&
        tx_data[12] >= v2::kMinFixedFrameCodewords &&
        tx_data[12] <= v2::kMaxFixedFrameCodewords) {
        frame_cw_count = tx_data[12];
    }

    // Data frames: fixed-CW frame encoding with frame interleaving
    // Channel interleaving is controlled by use_channel_interleave_ flag
    size_t bps = static_cast<size_t>(ofdm_link_adaptation::bitsPerOFDMSymbol(
        static_cast<int>(ofdm_config_.num_carriers),
        ofdm_config_.use_pilots,
        static_cast<int>(ofdm_config_.pilot_spacing),
        modulation_));
    Bytes encoded = v2::encodeFixedFrame(tx_data, code_rate_, frame_cw_count,
                                         use_channel_interleave_, bps);

    LOG_MODEM(DEBUG, "[%s] OFDM data: %zu bytes -> %d CWs (%zu coded, frame_interleave=%s, channel_interleave=%s)",
              log_prefix_.c_str(), tx_data.size(), frame_cw_count, encoded.size(),
              use_frame_interleave_ ? "yes" : "no",
              use_channel_interleave_ ? "yes" : "no");

    return encoded;
}

} // namespace gui
} // namespace ultra
