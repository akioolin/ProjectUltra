// modem_mode.cpp - Waveform and mode control for ModemEngine

#include "modem_engine.hpp"
#include "adaptive_reanchor_policy.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "protocol/frame_v2.hpp"
#include "protocol/connection_policy.hpp"
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
        if (connected_ && protocol::isOFDMMode(mode)) {
            streaming_decoder_->setConnectedOFDMMode(mode, config_, data_modulation_, data_code_rate_);
            if (mode == protocol::WaveformMode::OFDM_CHIRP) {
                streaming_decoder_->expectFullOFDMAnchorOnce();
            }
            LOG_MODEM(INFO, "setWaveformMode: StreamingDecoder connected OFDM config set (FFT=%d, carriers=%d)",
                      config_.fft_size, config_.num_carriers);
        } else {
            streaming_decoder_->setMode(decoder_mode, connected_);

            // For OFDM modes, propagate the current config (for custom FFT/carriers like NVIS mode)
            if (protocol::isOFDMMode(mode)) {
                streaming_decoder_->setOFDMConfig(config_);
                LOG_MODEM(INFO, "setWaveformMode: StreamingDecoder OFDM config set (FFT=%d, carriers=%d)",
                          config_.fft_size, config_.num_carriers);
            }
        }
    }

    // Update StreamingEncoder to match
    if (streaming_encoder_) {
        streaming_encoder_->setMode(mode);
        if (protocol::isOFDMMode(mode)) {
            streaming_encoder_->setOFDMConfig(config_);
            if (connected_ && mode == protocol::WaveformMode::OFDM_CHIRP) {
                streaming_encoder_->forceNextFrameFullPreamble();
            }
        }
    }

    switch (mode) {
        case protocol::WaveformMode::MC_DPSK:
            LOG_MODEM(INFO, "DPSK mode active: %d-PSK, %d samples/sym, %.1f bps",
                      dpsk_config_.num_phases(),
                      dpsk_config_.samples_per_symbol,
                      dpsk_config_.raw_bps());
            break;

        case protocol::WaveformMode::OFDM_CHIRP:
            LOG_MODEM(INFO, "OFDM_CHIRP mode active: %d carriers, DQPSK (differential)",
                      config_.num_carriers);
            break;

        case protocol::WaveformMode::OFDM_NARROW:
            LOG_MODEM(INFO, "OFDM_NARROW mode active: %d carriers, 500 Hz narrowband",
                      config_.num_carriers);
            break;

        default:
            LOG_MODEM(INFO, "OFDM mode active: %d carriers, %s",
                      config_.num_carriers,
                      connected_ ? "connected" : "disconnected");
            break;
    }
    syncAdaptiveShortDataPreamble();
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

        // The OFDM control profile (DQPSK-on-OFDM vs coherent-QPSK-on-OFDM) is a
        // RECEIVE-decode setting that must follow the NEGOTIATED MODE, which both
        // stations know the moment CONNECT_ACK is exchanged — the initiator on
        // receive, the responder on send — i.e. right here at setConnected().
        // Previously it was gated on handshake_complete_ ("first valid frame
        // received from initiator"), which deadlocked the responder: the
        // initiator's first OFDM frame (the burst descriptor) is sent on the
        // coherent profile, but the responder — still pinned to the legacy
        // DQPSK-OFDM profile until handshake_complete_ — could not decode it, so
        // it never received the frame that would have flipped the profile.
        // CONNECT/CONNECT_ACK ride MC-DPSK (control_waveform_, a separate decoder),
        // so they are unaffected by this OFDM-control profile. Enabling it now lets
        // the responder decode the initiator's coherent control/descriptor frames
        // immediately. profileForDataMode() still resolves to DQPSK for a
        // differential data regime, so this is a no-op for DQPSK/D8PSK links.
        const bool ofdm_control_follows_data = protocol::isOFDMMode(waveform_mode_);
        if (streaming_decoder_) {
            streaming_decoder_->setCoherentOFDMControlProfileEnabled(ofdm_control_follows_data);
        }
        if (streaming_encoder_) {
            streaming_encoder_->setCoherentOFDMControlProfileEnabled(ofdm_control_follows_data);
        }

        // Configure OFDM config FIRST so it's correct when propagated to decoder
        config_.modulation = data_modulation_;
        config_.code_rate = data_code_rate_;
        config_.use_pilots = true;  // Always — must match OFDMChirpWaveform

        config_.pilot_spacing =
            ofdm_link_adaptation::recommendedPilotSpacing(data_modulation_, data_code_rate_);

        decoder_->setRate(data_code_rate_);

        // CRITICAL: Update StreamingDecoder when entering connected state
        // Config must be set above BEFORE propagating here
        if (streaming_decoder_) {
            streaming_decoder_->setMode(waveform_mode_, true);  // true = connected

            // For OFDM modes, propagate the correct config (with proper pilot settings)
            if (protocol::isOFDMMode(waveform_mode_)) {
                streaming_decoder_->setConnectedOFDMMode(
                    waveform_mode_, config_, data_modulation_, data_code_rate_);
            }

            // Propagate known CFO from handshake to StreamingDecoder
            // This seeds the CFO feedback loop so the first OFDM frame uses correct CFO
            if (std::abs(peer_cfo_hz_) > 0.01f) {
                streaming_decoder_->setKnownCFO(peer_cfo_hz_);
                LOG_MODEM(INFO, "setConnected: seeded CFO=%.2f Hz from handshake", peer_cfo_hz_);
            }
            if (waveform_mode_ == protocol::WaveformMode::OFDM_CHIRP) {
                streaming_decoder_->expectFullOFDMAnchorOnce();
            }
        }

        // Update StreamingEncoder for connected state
        if (streaming_encoder_) {
            streaming_encoder_->setMode(waveform_mode_);
            if (protocol::isOFDMMode(waveform_mode_)) {
                streaming_encoder_->setOFDMConfig(config_);
                streaming_encoder_->setDataMode(data_modulation_, data_code_rate_);
                if (waveform_mode_ == protocol::WaveformMode::OFDM_CHIRP) {
                    streaming_encoder_->forceNextFrameFullPreamble();
                }
            }
        }

        LOG_MODEM(INFO, "Entered connected state, configured for %s %s (pilots=%d, spacing=%d)",
                  modulationToString(data_modulation_), codeRateToString(data_code_rate_),
                  config_.use_pilots ? 1 : 0, config_.pilot_spacing);
    } else {
        // Switching to disconnected state - use robust mode for RX
        decoder_->setRate(CodeRate::R1_4);

        // BUG-TNC-SESSION-001 fix (port from tools/ultra_tnc.cpp): a persistent
        // ModemEngine across multiple sessions must perform the same RX/TX
        // state reset on disconnect that ultra_tnc does, otherwise the next
        // session's first CONNECT_ACK gets rejected at the early
        // `cw_ok=0/cw_fail=0/is_ping=0` path because stale negotiated
        // modulation/code-rate, encoder mode, and cached peer CFO survive
        // the session boundary. Audio-engine pause/drain/resume is handled
        // by the App layer around this call (see app.cpp connection-changed
        // callback).
        if (streaming_decoder_) {
            streaming_decoder_->reset();
            streaming_decoder_->setMode(protocol::WaveformMode::MC_DPSK, false);
            streaming_decoder_->setDataMode(Modulation::DQPSK, CodeRate::R1_4);
            streaming_decoder_->setCoherentOFDMControlProfileEnabled(false);
        }
        if (streaming_encoder_) {
            streaming_encoder_->setMode(protocol::WaveformMode::MC_DPSK);
            streaming_encoder_->setDataMode(Modulation::DQPSK, CodeRate::R1_4);
            streaming_encoder_->setCoherentOFDMControlProfileEnabled(false);
        }
        data_modulation_ = Modulation::DQPSK;
        data_code_rate_ = CodeRate::R1_4;
        peer_cfo_hz_ = 0.0f;

        // Keep using connected waveform for the next TX (DISCONNECT ACK)
        // Save the current negotiated waveform BEFORE it might be reset
        disconnect_waveform_ = waveform_mode_;
        use_connected_waveform_once_ = true;
        LOG_MODEM(INFO, "Switched to disconnected mode (RX: DQPSK R1/4, next TX uses disconnect_waveform_=%d)",
                  static_cast<int>(disconnect_waveform_));
        handshake_complete_ = false;  // Reset for next connection
    }
    syncAdaptiveShortDataPreamble();
}

void ModemEngine::setHandshakeComplete(bool complete) {
    if (handshake_complete_ == complete) return;

    handshake_complete_ = complete;

    if (complete) {
        LOG_MODEM(INFO, "Handshake complete, TX now uses waveform_mode_=%d",
                  static_cast<int>(waveform_mode_));
    }
    if (streaming_decoder_) {
        streaming_decoder_->setCoherentOFDMControlProfileEnabled(
            complete && connected_ && protocol::isOFDMMode(waveform_mode_));
    }
    if (streaming_encoder_) {
        streaming_encoder_->setCoherentOFDMControlProfileEnabled(
            complete && connected_ && protocol::isOFDMMode(waveform_mode_));
    }
}

void ModemEngine::setDataMode(Modulation mod, CodeRate rate) {
    data_modulation_ = mod;
    data_code_rate_ = rate;

    // CRITICAL: Pilots are ALWAYS enabled — OFDMChirpWaveform::configurePilotsForCodeRate()
    // always sets use_pilots=true regardless of differential/coherent modulation.
    // The pilot carriers are used for CFO tracking and channel estimation even in
    // differential mode. Disabling them causes data/pilot layout mismatch.
    config_.modulation = mod;
    config_.code_rate = rate;
    config_.use_pilots = true;  // Always — must match OFDMChirpWaveform

    config_.pilot_spacing =
        ofdm_link_adaptation::recommendedPilotSpacing(mod, rate);

    // If already connected, update both TX and RX to match
    if (connected_) {
        decoder_->setRate(rate);

        LOG_MODEM(INFO, "TX/RX OFDM config updated: mod=%d, rate=%d, use_pilots=%d, pilot_spacing=%d",
                  static_cast<int>(mod), static_cast<int>(rate),
                  config_.use_pilots ? 1 : 0, config_.pilot_spacing);
    }

    // Update StreamingDecoder's waveform configuration
    // CRITICAL: Set OFDM config BEFORE setDataMode so decoder has correct pilot layout
    if (streaming_decoder_) {
        if (connected_ && protocol::isOFDMMode(waveform_mode_)) {
            streaming_decoder_->setConnectedOFDMMode(
                waveform_mode_, config_, mod, rate);
        } else {
            if (waveform_mode_ != protocol::WaveformMode::MC_DPSK) {
                streaming_decoder_->setOFDMConfig(config_);
            }
            streaming_decoder_->setDataMode(mod, rate);
        }
    }

    // Update StreamingEncoder to match
    if (streaming_encoder_) {
        if (waveform_mode_ != protocol::WaveformMode::MC_DPSK) {
            streaming_encoder_->setOFDMConfig(config_);
        }
        streaming_encoder_->setDataMode(mod, rate);
    }

    // File-class composite (design §14.14): enable cross-frame burst interleave
    // for connected coherent-QPSK OFDM data. Validated offline — R3/4 Good@20
    // frame recovery ~33%->~92% at group=8. Coherent QPSK uses the wide
    // (kWideOFDMWindowFrames=8) ARQ window, which equals kBurstInterleaveGroupFrames,
    // so a steady-state window fills exactly one interleave group. Both TX and RX
    // run setDataMode from negotiation, keeping the de-interleave matched.
    //
    // 2026-05-28: extended to coherent 8PSK (QAM8) for the throughput-rung
    // probe. Same OFDM machinery as QPSK; the burst interleaver operates on
    // post-LDPC bits and is constellation-agnostic. Without this, forcing
    // QAM8 + ULTRA_BURST_TRANSPORT=1 left the encoder NOT interleaving while
    // the burst transport (BURST_HEADER, group ack, chunker) was active —
    // every CW failed because the bit order didn't match what the receiver
    // expected after the burst header was consumed.
    const bool file_class_composite =
        connected_ && protocol::isOFDMMode(waveform_mode_) &&
        (mod == Modulation::QPSK || mod == Modulation::QAM8);
    // 2026-05-29 step-1 foundation check (ULTRA_BURST_INTERLEAVE=0): turn OFF the
    // cross-frame burst interleave while KEEPING the group size, so a fade hits only
    // some of the 6 frames and each frame decodes INDEPENDENTLY -> the tone-burst
    // frame_mask can show a PARTIAL group (some frames OK, some NACK). That proves
    // per-frame SACK is physically possible — the go/no-go for reviving SR-ARQ +
    // granular resend on Good (where the N=6 interleaver gives ~0 diversity anyway).
    // Default ON (the interleaver stays the shipped behavior; this is a test knob).
    const bool burst_interleave_disabled = [] {
        const char* env = std::getenv("ULTRA_BURST_INTERLEAVE");
        return env && env[0] == '0';
    }();
    const bool burst_interleave_on = file_class_composite && !burst_interleave_disabled;
    const int burst_group =
        static_cast<int>(protocol::connection_policy::burstInterleaveGroupFrames());
    if (streaming_encoder_) {
        streaming_encoder_->setBurstInterleaveGroupSize(burst_group);
        streaming_encoder_->setBurstInterleave(burst_interleave_on);
    }
    if (streaming_decoder_) {
        streaming_decoder_->setBurstInterleaveGroupSize(burst_group);
        streaming_decoder_->setBurstInterleave(burst_interleave_on);
    }

    // burst_interleave reports the ACTUAL byte-permutation decision (burst_interleave_on),
    // not file_class_composite — the latter is only "is this a burst-eligible QPSK/QAM8
    // file frame" and is 1 even when ULTRA_BURST_INTERLEAVE=0 turns the permutation off.
    LOG_MODEM(INFO, "Data mode set to: %s (pilots=%d, spacing=%d, burst_interleave=%d)",
              getModeDescription(mod, rate), config_.use_pilots ? 1 : 0,
              config_.pilot_spacing, burst_interleave_on ? 1 : 0);
    syncAdaptiveShortDataPreamble();
}

void ModemEngine::setAdaptivePreamblePeerFading(float peer_fading_index) {
    adaptive_preamble_peer_fading_ = peer_fading_index;
    syncAdaptiveShortDataPreamble();
}

void ModemEngine::syncAdaptiveShortDataPreamble() {
    bool enable = adaptive_reanchor_policy::shouldUseShortReanchor(
        waveform_mode_, data_modulation_, adaptive_preamble_peer_fading_);
    // §16.4: the adaptive 100 ms short re-anchor (commit 66db2d8) is the
    // SUPERSEDED group-boundary strategy — the §16.2 "short re-anchor that
    // broke frame-stride timing". It is mutually exclusive with the warm-sync
    // hand-off: with both active its detector fires on noise (corr≈0.16) at
    // group boundaries and competes with the descriptor-chirp + warm-light
    // path, stalling transfers (seed 1 Good@20: 10 garbage fires + 35 path-5
    // fallbacks, 2/11 groups). When warm-handoff is on it owns the group
    // boundary, so force the legacy short re-anchor OFF on both TX and RX.
    if (const char* s16 = std::getenv("ULTRA_S16_WARM_HANDOFF");
        s16 && std::atoi(s16) != 0) {
        enable = false;
    }
    const bool changed = adaptive_short_reanchor_active_ != enable;
    if (enable || changed) {
        if (streaming_encoder_) {
            streaming_encoder_->setAdaptiveShortDataPreamble(enable);
        }
        if (streaming_decoder_) {
            streaming_decoder_->setAdaptiveShortDataPreamble(enable);
        }
    }
    if (changed) {
        adaptive_short_reanchor_active_ = enable;
        LOG_MODEM(INFO,
                  "Adaptive short data re-anchor %s (waveform=%s mod=%s peer_fading=%.2f chirp=%.0f ms)",
                  enable ? "ENABLED" : "DISABLED",
                  protocol::waveformModeToString(waveform_mode_),
                  modulationToString(data_modulation_),
                  adaptive_preamble_peer_fading_,
                  adaptive_reanchor_policy::shortReanchorChirpDurationMs());
    }
}

// NOTE: recommendDataMode() removed - use protocol::recommendDataMode() from waveform_selection.hpp

protocol::WaveformMode ModemEngine::recommendWaveformMode(float snr_db) {
    // Legacy SNR-only selection (use recommendWaveformAndRate for better results)
    // DPSK works down to -11 dB SNR (tested), so we use it for low SNR
    // OFDM requires ~27 dB on the unified in-band meter for reliable sync detection
    if (snr_db < 27.0f) {
        return protocol::WaveformMode::MC_DPSK;
    } else {
        return protocol::WaveformMode::OFDM_CHIRP;
    }
}

ModemEngine::WaveformRecommendation ModemEngine::recommendWaveformAndRate(float snr_db, float fading_index) {
    // Delegate to shared algorithm in protocol namespace
    auto rec = protocol::recommendWaveformAndRate(snr_db, fading_index);
    LOG_MODEM(DEBUG, "recommendWaveformAndRate: SNR=%.1f (%s), fading=%.2f -> %s %s (%.0f bps)",
              snr_db, snrSourceToString(SNRSource::NONE), fading_index,
              protocol::waveformModeToString(rec.waveform),
              codeRateToString(rec.rate),
              rec.estimated_throughput_bps);
    return rec;
}

void ModemEngine::setDPSKMode(DPSKModulation mod, int samples_per_symbol) {
    // Configure DPSK based on modulation type and symbol rate
    dpsk_config_.modulation = mod;
    dpsk_config_.samples_per_symbol = samples_per_symbol;

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

    // Recreate decoder with new codec type (encoder is managed by StreamingEncoder)
    decoder_ = fec::CodecFactory::create(type, data_code_rate_);

    // Update StreamingDecoder to use the same codec
    if (streaming_decoder_) {
        streaming_decoder_->setCodecType(type);
    }

    LOG_MODEM(INFO, "Codec type set to: %s", decoder_->getName().c_str());
}

fec::CodecType ModemEngine::recommendCodecType(float snr_db) {
    // Codec selection based on SNR:
    // - LDPC: Works well at moderate-high in-band SNR (>15 dB), steep waterfall curve
    // - Convolutional: Better at very low in-band SNR (<15 dB), graceful degradation
    // - Turbo: Excellent near Shannon limit, but high latency
    //
    // For now, always use LDPC since it's the only implemented codec.
    // When convolutional codec is implemented, use it for in-band SNR < 15 dB.

    if (snr_db < 15.0f) {
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
    // - OFDM_CHIRP (medium/high SNR): LDPC with R1/4 up through higher rates
    //
    // For now, always return LDPC since it's the only implemented codec.

    switch (mode) {
        case protocol::WaveformMode::MC_DPSK:
            // Low SNR waveform - would prefer convolutional
            // return fec::CodecType::CONVOLUTIONAL;  // Future
            return fec::CodecType::LDPC;

        case protocol::WaveformMode::OFDM_CHIRP:
        case protocol::WaveformMode::OFDM_NARROW:
        default:
            return fec::CodecType::LDPC;
    }
}

int ModemEngine::recommendMCDPSKCarriers(float snr_db, float fading_index) {
    // MC-DPSK is used for roughly in-band SNR 10-20 dB range
    // (above 20 dB switches to OFDM)
    // Testing with 20Hz CFO shows 8 carriers is optimal for this range:
    //   - 8 carriers: 100% at legacy SNR 5 (about 15 dB in-band),
    //     moderate fading, 20Hz CFO
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
