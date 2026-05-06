#pragma once

// StreamingEncoder - Unified TX encoder for all waveform types
//
// Mirrors StreamingDecoder to ensure TX/RX use identical configurations.
// This is critical for debugging issues like channel interleaving mismatches.
//
// Key features:
//   1. Same waveform configuration as StreamingDecoder
//   2. Same carrier/pilot allocation
//   3. Same interleaver parameters (bits_per_symbol, total_bits)
//   4. Centralized encoding logic (was scattered in cli_simulator)
//
// Usage:
//   StreamingEncoder encoder;
//   encoder.setMode(WaveformMode::OFDM_CHIRP);
//   encoder.setDataMode(Modulation::DQPSK, CodeRate::R1_4);
//
//   // Encode and transmit
//   auto samples = encoder.encodeFrame(frame_data);
//   audio_output(samples);
//
//   // Or for PING only
//   auto ping = encoder.encodePing();

#include "waveform/waveform_interface.hpp"
#include "waveform/waveform_factory.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/fec.hpp"
#include <vector>
#include <memory>

namespace ultra {
namespace gui {

namespace v2 = protocol::v2;

// Encoder configuration for debugging/verification
struct EncoderConfig {
    protocol::WaveformMode mode = protocol::WaveformMode::MC_DPSK;
    Modulation modulation = Modulation::DQPSK;
    CodeRate code_rate = CodeRate::R1_4;
    int num_carriers = 59;          // Total carriers (OFDM)
    int data_carriers = 53;         // Data carriers after pilot allocation
    int bits_per_symbol = 106;      // data_carriers * bits_per_carrier (DQPSK=2)
    bool use_pilots = true;
    int pilot_spacing = 10;
    bool use_channel_interleave = true;   // Spreads coded bits across carriers for fading resistance
    bool use_frame_interleave = true;
};

class StreamingEncoder {
public:
    StreamingEncoder();
    ~StreamingEncoder();

    // ========================================================================
    // MODE CONTROL (mirrors StreamingDecoder)
    // ========================================================================

    // Set waveform mode (MC_DPSK, OFDM_CHIRP, OFDM_COX)
    void setMode(protocol::WaveformMode mode);

    // Set data mode (modulation and code rate)
    void setDataMode(Modulation mod, CodeRate rate);

    // Set OFDM config (for custom carrier/pilot settings)
    void setOFDMConfig(const ModemConfig& config);

    // Set MC-DPSK carrier count
    void setMCDPSKCarriers(int num_carriers);

    // Switch control waveform to narrowband MC-DPSK (for narrowband sessions)
    void setNarrowbandControl(bool narrowband);

    // ========================================================================
    // ENCODING
    // ========================================================================

    // Encode frame data -> audio samples (preamble + modulated data)
    // Uses full preamble (chirp sync)
    std::vector<float> encodeFrame(const Bytes& frame_data);

    // Encode frame with light preamble (for connected mode, faster turnaround)
    // Only works if waveform supports data preamble
    std::vector<float> encodeFrameLight(const Bytes& frame_data);

    // Encode multiple frames as a single burst with one LTS preamble
    // Each frame gets its own training symbols for per-block channel estimation
    // Returns: [LTS] + [train+data_0] + [train+data_1] + ... + [train+data_N]
    std::vector<float> encodeBurstLight(const std::vector<Bytes>& frame_data_list);

    // Encode PING (chirp preamble only, no data)
    std::vector<float> encodePing();

    // Encode just the data portion (no preamble) - for testing
    std::vector<float> encodeDataOnly(const Bytes& frame_data);

    // ========================================================================
    // CONFIGURATION ACCESS (for verification with decoder)
    // ========================================================================

    protocol::WaveformMode getMode() const { return mode_; }
    Modulation getModulation() const { return modulation_; }
    CodeRate getCodeRate() const { return code_rate_; }

    // Get current configuration (for debugging/comparison with decoder)
    EncoderConfig getConfig() const;

    // Get underlying OFDM config
    const ModemConfig& getOFDMConfig() const { return ofdm_config_; }

    // Get the waveform (for advanced use)
    IWaveform* getWaveform() { return waveform_.get(); }

    // Verify config matches a decoder config (returns mismatch description or empty string)
    std::string verifyConfigMatch(const EncoderConfig& decoder_config) const;

    // ========================================================================
    // INTERLEAVING CONTROL
    // ========================================================================

    void setChannelInterleave(bool enable) { use_channel_interleave_ = enable; }
    bool getChannelInterleave() const { return use_channel_interleave_; }

    void setCarrierMask(uint64_t active_mask);
    uint64_t getCarrierMask() const { return carrier_mask_; }

    // Frame interleaving is always enabled for OFDM
    bool getFrameInterleave() const { return use_frame_interleave_; }

    void setFixedFrameCodewords(int cw_count) {
        fixed_frame_codewords_ = v2::sanitizeFixedFrameCodewords(cw_count);
    }
    int getFixedFrameCodewords() const { return fixed_frame_codewords_; }

    // Burst-level long interleaver (spreads CW bytes across N-frame groups)
    void setBurstInterleave(bool enable) { use_burst_interleave_ = enable; }
    bool getBurstInterleave() const { return use_burst_interleave_; }
    void setBurstInterleaveGroupSize(int size);
    int getBurstInterleaveGroupSize() const { return burst_group_size_; }

private:
    // ========================================================================
    // INTERNAL HELPERS
    // ========================================================================

    // Create/recreate waveform based on current mode
    void createWaveform();

    // Create channel interleaver with correct parameters
    void updateInterleaver();

    // Encode frame bytes to coded bytes (LDPC + interleaving)
    Bytes encodeFrameBytes(const Bytes& frame_data);

    // Calculate data carriers from OFDM config
    int calculateDataCarriers() const;

    // ========================================================================
    // STATE (mirrors StreamingDecoder)
    // ========================================================================

    // Waveform
    std::unique_ptr<IWaveform> waveform_;
    std::unique_ptr<IWaveform> control_waveform_;  // MC-DPSK for control frames
    protocol::WaveformMode mode_ = protocol::WaveformMode::MC_DPSK;

    // Modulation/coding
    Modulation modulation_ = Modulation::DQPSK;
    CodeRate code_rate_ = CodeRate::R1_4;

    // OFDM config
    ModemConfig ofdm_config_;
    int mc_dpsk_carriers_ = 8;
    bool narrowband_control_ = false;  // Use narrowband MC-DPSK for control/handshake

    // Interleaving
    std::unique_ptr<ChannelInterleaver> channel_interleaver_;
    bool use_channel_interleave_ = true;
    bool use_frame_interleave_ = true;     // Always on for OFDM
    uint64_t carrier_mask_ = UINT64_MAX;
    int fixed_frame_codewords_ = v2::kDefaultFixedFrameCodewords;
    bool use_burst_interleave_ = false;    // Burst-level long interleaver (N-frame groups)
    int burst_group_size_ = 8;

    // Logging
    std::string log_prefix_ = "StreamingEncoder";
};

} // namespace gui
} // namespace ultra
