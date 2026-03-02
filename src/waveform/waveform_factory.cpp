// WaveformFactory - Implementation

#include "waveform_factory.hpp"
#include "mc_dpsk_waveform.hpp"
#include "ofdm_cox_waveform.hpp"
#include "ofdm_chirp_waveform.hpp"
#include "otfs_waveform.hpp"
#include "ultra/logging.hpp"

namespace ultra {

WaveformPtr WaveformFactory::create(protocol::WaveformMode mode) {
    switch (mode) {
        case protocol::WaveformMode::MC_DPSK:
            return std::make_unique<MCDPSKWaveform>();

        case protocol::WaveformMode::OFDM_COX:
            return std::make_unique<OFDMNvisWaveform>();

        case protocol::WaveformMode::OFDM_CHIRP:
            return std::make_unique<OFDMChirpWaveform>();

        case protocol::WaveformMode::OFDM_NARROW:
            return std::make_unique<OFDMChirpWaveform>(
                presets::narrowbandOFDM(), protocol::WaveformMode::OFDM_NARROW);

        case protocol::WaveformMode::AUTO:
            // Default to MC-DPSK for AUTO (most robust for connection)
            return std::make_unique<MCDPSKWaveform>();

        case protocol::WaveformMode::OTFS_EQ:
        case protocol::WaveformMode::OTFS_RAW:
            return std::make_unique<OTFSWaveform>();

        case protocol::WaveformMode::MFSK:
            // MFSK deprecated - use MC-DPSK instead
            LOG_MODEM(WARN, "WaveformFactory: MFSK deprecated, using MC-DPSK");
            return std::make_unique<MCDPSKWaveform>();

        default:
            LOG_MODEM(ERROR, "WaveformFactory: Unknown mode %d", static_cast<int>(mode));
            return nullptr;
    }
}

WaveformPtr WaveformFactory::create(protocol::WaveformMode mode, const ModemConfig& config) {
    switch (mode) {
        case protocol::WaveformMode::MC_DPSK: {
            MultiCarrierDPSKConfig mc_cfg;
            mc_cfg.sample_rate = static_cast<float>(config.sample_rate);
            // Other fields use defaults
            return std::make_unique<MCDPSKWaveform>(mc_cfg);
        }

        case protocol::WaveformMode::OFDM_COX:
            return std::make_unique<OFDMNvisWaveform>(config);

        case protocol::WaveformMode::OFDM_CHIRP:
            return std::make_unique<OFDMChirpWaveform>(config);

        case protocol::WaveformMode::OFDM_NARROW:
            return std::make_unique<OFDMChirpWaveform>(config, protocol::WaveformMode::OFDM_NARROW);

        default:
            return create(mode);
    }
}

WaveformPtr WaveformFactory::createMCDPSK(int num_carriers) {
    return std::make_unique<MCDPSKWaveform>(num_carriers);
}

WaveformPtr WaveformFactory::createNarrowbandMCDPSK() {
    return std::make_unique<MCDPSKWaveform>(mc_dpsk_presets::narrowband());
}

std::vector<protocol::WaveformMode> WaveformFactory::getAvailableModes() {
    return {
        protocol::WaveformMode::MC_DPSK,
        protocol::WaveformMode::OFDM_CHIRP,
        protocol::WaveformMode::OFDM_COX,
        protocol::WaveformMode::OFDM_NARROW,
    };
}

bool WaveformFactory::isSupported(protocol::WaveformMode mode) {
    switch (mode) {
        case protocol::WaveformMode::MC_DPSK:
        case protocol::WaveformMode::OFDM_COX:
        case protocol::WaveformMode::OFDM_CHIRP:
        case protocol::WaveformMode::OFDM_NARROW:
        case protocol::WaveformMode::AUTO:
            return true;

        case protocol::WaveformMode::OTFS_EQ:
        case protocol::WaveformMode::OTFS_RAW:
            return true;  // Now supported

        case protocol::WaveformMode::MFSK:
            return false;  // Deprecated

        default:
            return false;
    }
}

std::string WaveformFactory::getModeName(protocol::WaveformMode mode) {
    switch (mode) {
        case protocol::WaveformMode::MC_DPSK:    return "MC-DPSK";
        case protocol::WaveformMode::OFDM_COX:  return "OFDM-COX";
        case protocol::WaveformMode::OFDM_CHIRP: return "OFDM-Chirp";
        case protocol::WaveformMode::OFDM_NARROW: return "OFDM-Narrow";
        case protocol::WaveformMode::OTFS_EQ:    return "OTFS-EQ";
        case protocol::WaveformMode::OTFS_RAW:   return "OTFS-Raw";
        case protocol::WaveformMode::MFSK:       return "MFSK";
        case protocol::WaveformMode::AUTO:       return "AUTO";
        default:                                 return "Unknown";
    }
}

protocol::WaveformMode WaveformFactory::recommendMode(float snr_db) {
    // Conservative thresholds calibrated for real HF channels
    // HF has multipath fading that requires margin vs AWGN

    if (snr_db < 10.0f) {
        // Low SNR: Use MC-DPSK with chirp sync
        // Reliable from -3 dB to 10 dB
        return protocol::WaveformMode::MC_DPSK;
    } else if (snr_db < 17.0f) {
        // Mid SNR: Use OFDM with chirp sync + DQPSK
        // Higher throughput than MC-DPSK, more robust than Schmidl-Cox
        return protocol::WaveformMode::OFDM_CHIRP;
    } else {
        // High SNR: Use OFDM with Schmidl-Cox + coherent modulation
        // Maximum throughput
        return protocol::WaveformMode::OFDM_COX;
    }
}

// NOTE: recommendDataMode() removed - use protocol::recommendDataMode() from waveform_selection.hpp

float WaveformFactory::getMinSNR(protocol::WaveformMode mode) {
    switch (mode) {
        case protocol::WaveformMode::MC_DPSK:    return -3.0f;
        case protocol::WaveformMode::OFDM_CHIRP: return 10.0f;
        case protocol::WaveformMode::OFDM_NARROW: return 3.0f;
        case protocol::WaveformMode::OFDM_COX:  return 17.0f;
        case protocol::WaveformMode::OTFS_EQ:    return 15.0f;
        case protocol::WaveformMode::OTFS_RAW:   return 10.0f;
        case protocol::WaveformMode::MFSK:       return -17.0f;
        default:                                 return 0.0f;
    }
}

float WaveformFactory::getMaxThroughput(protocol::WaveformMode mode) {
    // Approximate max throughput at high SNR with R3/4
    switch (mode) {
        case protocol::WaveformMode::MC_DPSK:    return 1500.0f;   // 20 carriers, DQPSK R3/4
        case protocol::WaveformMode::OFDM_CHIRP: return 4000.0f;   // 30 carriers, D8PSK R2/3
        case protocol::WaveformMode::OFDM_NARROW: return 230.0f;  // 21 carriers, DQPSK R1/2
        case protocol::WaveformMode::OFDM_COX:  return 8000.0f;   // 30 carriers, 32QAM R3/4
        case protocol::WaveformMode::OTFS_EQ:    return 6000.0f;
        case protocol::WaveformMode::OTFS_RAW:   return 4000.0f;
        case protocol::WaveformMode::MFSK:       return 200.0f;
        default:                                 return 1000.0f;
    }
}

int WaveformFactory::recommendMCDPSKCarriers(float snr_db) {
    // More carriers = higher throughput but need better SNR
    // Carrier spacing affects fading diversity

    if (snr_db < 0.0f) {
        return 5;   // 234 bps with R1/4
    } else if (snr_db < 3.0f) {
        return 5;   // 234 bps
    } else if (snr_db < 8.0f) {
        return 8;   // 375 bps
    } else if (snr_db < 10.0f) {
        return 10;  // 469 bps
    } else if (snr_db < 15.0f) {
        return 13;  // 609 bps
    } else {
        return 20;  // 938 bps
    }
}

} // namespace ultra
