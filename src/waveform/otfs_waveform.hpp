#pragma once

// OTFSWaveform - OTFS with chirp synchronization
//
// OTFS spreads each symbol across ALL time-frequency resources,
// providing full diversity against frequency-selective fading.
// This should enable R1/2 on fading channels where OFDM fails.
//
// Key difference from OFDM:
// - OFDM: each carrier fades independently → some carriers get nulled
// - OTFS: each symbol sees AVERAGE channel → no complete nulls

#include "waveform_interface.hpp"
#include "ultra/otfs.hpp"
#include "sync/chirp_sync.hpp"
#include <memory>

namespace ultra {

class OTFSWaveform : public IWaveform {
public:
    OTFSWaveform();
    explicit OTFSWaveform(const OTFSConfig& config);
    ~OTFSWaveform() override = default;

    // ========================================================================
    // IWaveform - Identity
    // ========================================================================

    std::string getName() const override { return "OTFS-Chirp"; }
    protocol::WaveformMode getMode() const override { return protocol::WaveformMode::OTFS_EQ; }
    WaveformCapabilities getCapabilities() const override;

    // ========================================================================
    // IWaveform - Configuration
    // ========================================================================

    void configure(Modulation mod, CodeRate rate) override;
    void setFrequencyOffset(float cfo_hz) override;
    void setTxFrequencyOffset(float cfo_hz) override;
    Modulation getModulation() const override { return config_.modulation; }
    CodeRate getCodeRate() const override { return code_rate_; }
    float getFrequencyOffset() const override { return cfo_hz_; }

    // ========================================================================
    // IWaveform - TX
    // ========================================================================

    Samples generatePreamble() override;
    Samples modulate(const Bytes& encoded_data) override;

    // ========================================================================
    // IWaveform - RX
    // ========================================================================

    bool detectSync(SampleSpan samples, SyncResult& result, float threshold = 0.15f) override;
    bool process(SampleSpan samples) override;
    std::vector<float> getSoftBits() override;
    void reset() override;

    // ========================================================================
    // IWaveform - Status
    // ========================================================================

    bool isSynced() const override;
    bool hasData() const override;
    float estimatedSNR() const override;
    float estimatedCFO() const override;
    float getFadingIndex() const override;
    std::vector<std::complex<float>> getConstellationSymbols() const override;

    // ========================================================================
    // IWaveform - GUI Display
    // ========================================================================

    std::string getStatusString() const override;
    int getCarrierCount() const override { return static_cast<int>(config_.M); }
    float getThroughput(CodeRate rate) const override;
    int getSamplesPerSymbol() const override;
    int getPreambleSamples() const override;
    int getMinSamplesForFrame() const override;

private:
    void initComponents();
    sync::ChirpConfig getChirpConfig() const;

    OTFSConfig config_;
    CodeRate code_rate_ = CodeRate::R1_2;
    std::unique_ptr<OTFSModulator> modulator_;
    std::unique_ptr<OTFSDemodulator> demodulator_;
    std::unique_ptr<sync::ChirpSync> chirp_sync_;

    // State
    float cfo_hz_ = 0.0f;
    float tx_cfo_hz_ = 0.0f;
    float last_snr_ = 0.0f;
    float last_cfo_ = 0.0f;
    bool synced_ = false;
    std::vector<float> soft_bits_;
    std::vector<Complex> constellation_;

    size_t training_start_sample_ = 0;
};

} // namespace ultra
