#pragma once

// OFDMChirpWaveform - OFDM with chirp synchronization
//
// Combines robust chirp sync with OFDM modulation for mid-range SNR.
// Works at 10-17 dB where Schmidl-Cox struggles but DPSK is too slow.
//
// Features:
// - Chirp preamble for robust sync at lower SNR than Schmidl-Cox
// - DQPSK modulation (differential, no pilots needed)
// - Good fading performance with frequency diversity
// - CFO-tolerant via complex correlation chirp detection

#include "waveform_interface.hpp"
#include "ultra/ofdm.hpp"
#include "sync/chirp_sync.hpp"
#include <memory>

namespace ultra {

class OFDMChirpWaveform : public IWaveform {
public:
    // Create with default configuration (wideband OFDM_CHIRP)
    OFDMChirpWaveform();

    // Create with specific config (defaults to OFDM_CHIRP mode)
    explicit OFDMChirpWaveform(const ModemConfig& config);

    // Create with specific config and explicit waveform mode
    // Use OFDM_NARROW for narrowband operation
    OFDMChirpWaveform(const ModemConfig& config, protocol::WaveformMode mode);

    ~OFDMChirpWaveform() override = default;

    // ========================================================================
    // IWaveform - Identity
    // ========================================================================

    std::string getName() const override {
        return mode_ == protocol::WaveformMode::OFDM_NARROW ? "OFDM-Narrow" : "OFDM-Chirp";
    }
    protocol::WaveformMode getMode() const override { return mode_; }
    WaveformCapabilities getCapabilities() const override;

    // ========================================================================
    // IWaveform - Configuration
    // ========================================================================

    void configure(Modulation mod, CodeRate rate) override;
    void setFrequencyOffset(float cfo_hz) override;
    void setTxFrequencyOffset(float cfo_hz) override;
    Modulation getModulation() const override { return config_.modulation; }
    CodeRate getCodeRate() const override { return config_.code_rate; }
    float getFrequencyOffset() const override { return cfo_hz_; }

    // ========================================================================
    // IWaveform - TX
    // ========================================================================

    Samples generatePreamble() override;
    Samples generateDataPreamble() override;  // Training only, no chirp
    Samples modulate(const Bytes& encoded_data) override;
    void setCarrierMask(uint64_t active_mask) override;
    uint64_t getCarrierMask() const override { return carrier_mask_; }
    void setCarrierLdpcInterleaverEnabled(bool enabled) override;

    // ========================================================================
    // IWaveform - RX
    // ========================================================================

    bool detectSync(SampleSpan samples, SyncResult& result, float threshold = 0.15f) override;
    bool detectDataSync(SampleSpan samples, SyncResult& result,
                        float known_cfo_hz = 0.0f, float threshold = 0.3f) override;
    bool supportsDataPreamble() const override { return true; }
    void setAbsoluteTrainingPosition(size_t pos) override;
    bool process(SampleSpan samples) override;
    std::vector<float> getSoftBits() override;
    void setRXCarrierErasureEnabled(bool enabled) override;
    void reset() override;

    // ========================================================================
    // IWaveform - Status
    // ========================================================================

    bool isSynced() const override;
    bool hasData() const override;
    float estimatedSNR() const override;
    float estimatedCFO() const override;
    float getFadingIndex() const override;  // From demodulator pilot variance
    float getLastLTSSignalPower() const override;
    float getLastLTSChannelMagnitude() const override;
    float getLastLTSResidualCFOHz() const override;
    std::vector<std::complex<float>> getConstellationSymbols() const override;

    // ========================================================================
    // IWaveform - GUI Display
    // ========================================================================

    std::string getStatusString() const override;
    int getCarrierCount() const override { return static_cast<int>(config_.num_carriers); }
    float getThroughput(CodeRate rate) const override;
    int getSamplesPerSymbol() const override;
    int getPreambleSamples() const override;
    int getDataPreambleSamples() const override;  // Training only
    int getMinSamplesForFrame() const override;
    int getMinSamplesForControlFrame() const override;
    int getMinSamplesForCWCount(int num_cw) const override;

    // ========================================================================
    // OFDM-Chirp Specific
    // ========================================================================

    // Burst interleave marker: true if last detectDataSync() found negated LTS
    bool wasBurstInterleaved() const override { return burst_interleave_latched_; }
    float getLastTimingOffsetSamples() const override;

    // Pilot spacing for interleaver geometry
    int getPilotSpacing() const override { return config_.pilot_spacing; }

    // Get internal config
    const ModemConfig& getConfig() const { return config_; }

    // Get chirp sync for direct access
    sync::ChirpSync* getChirpSync() { return chirp_sync_.get(); }

    Symbol getLastDataCarrierSymbolsForTesting() const;

private:
    void initComponents();
    sync::ChirpConfig getChirpConfig() const;
    void configurePilotsForCodeRate(CodeRate rate);
    bool carrierLdpcPlumbingEligible() const;
    bool carrierLdpcCodewordCountSupported(size_t codeword_count) const;

    protocol::WaveformMode mode_ = protocol::WaveformMode::OFDM_CHIRP;
    ModemConfig config_;
    std::unique_ptr<OFDMModulator> modulator_;
    std::unique_ptr<OFDMDemodulator> demodulator_;
    std::unique_ptr<sync::ChirpSync> chirp_sync_;

    // State
    float cfo_hz_ = 0.0f;
    float last_snr_ = 0.0f;
    float last_cfo_ = 0.0f;
    uint64_t carrier_mask_ = UINT64_MAX;
    bool carrier_ldpc_interleaver_enabled_ = false;
    bool rx_carrier_erasure_enabled_ = false;
    bool synced_ = false;
    std::vector<float> soft_bits_;

    // Sample position where training starts (from detectSync)
    // Used to calculate initial CFO phase for correct phase tracking
    size_t training_start_sample_ = 0;
    size_t absolute_training_start_sample_ = 0;
    bool has_absolute_training_start_sample_ = false;

    // Burst interleave marker detection (LTS sign)
    // Two-flag design:
    //   burst_interleaved_detected_: consumed by process() to undo LTS negation (one-shot)
    //   burst_interleave_latched_: readable by streaming_decoder after process() returns
    bool burst_interleaved_detected_ = false;
    bool burst_interleave_latched_ = false;
};

} // namespace ultra
