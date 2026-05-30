#pragma once

#include "types.hpp"
#include <memory>
#include <string>

namespace ultra {

// Forward declarations
class FFT;

/**
 * OFDM Modulator
 *
 * Converts data bits into OFDM audio waveform.
 *
 * Design for HF:
 * - Narrow subcarrier spacing (~47 Hz) for Doppler tolerance
 * - Long symbols with cyclic prefix for multipath
 * - Scattered pilots for channel tracking
 * - Supports BPSK through QAM64
 */
class OFDMModulator {
public:
    explicit OFDMModulator(const ModemConfig& config);
    ~OFDMModulator();

    // Modulate data bytes into audio samples.
    // carrier_mask_enabled is used by OFDM_CHIRP CarrierLDPC plumbing only.
    Samples modulate(ByteSpan data, Modulation mod,
                     uint64_t active_carrier_mask = UINT64_MAX,
                     bool carrier_mask_enabled = false);

    // Generate a sync/preamble sequence (Schmidl-Cox: STS + LTS)
    Samples generatePreamble();

    // Generate training symbols for chirp-based acquisition
    // Call AFTER generating chirp, BEFORE modulate()
    // Resets mixer so training + data are phase-coherent
    Samples generateTrainingSymbols(int count = 2);

    // Generate channel probe signal
    Samples generateProbe();

    // Get samples per symbol (including CP)
    size_t samplesPerSymbol() const;

    // Get data bits per symbol at given modulation
    size_t bitsPerSymbol(Modulation mod) const;

    // Test/debug: flattened data-carrier symbols from the last modulate() call,
    // after carrier masking and before IFFT.
    Symbol getLastDataCarrierSymbolsForTesting() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * OFDM Demodulator
 *
 * Converts received audio back to data bits.
 * Includes channel estimation and equalization.
 */
class OFDMDemodulator {
public:
    explicit OFDMDemodulator(const ModemConfig& config);
    ~OFDMDemodulator();

    // Process incoming samples, returns true if frame ready
    bool process(SampleSpan samples);

    // Get demodulated data (call after process returns true)
    Bytes getData();

    // Get soft bits for FEC decoder (better than hard decisions)
    std::vector<float> getSoftBits();

    // Get current channel estimate
    ChannelQuality getChannelQuality() const;

    // Get estimated SNR in dB on the demodulator's internal LLR scale.
    float getEstimatedSNR() const;

    // Get the last in-band SNR estimate derived from OFDM pilot/LTS residuals.
    // The accessor name is historical. This is separate from getEstimatedSNR(),
    // which remains the demodulator's internal per-carrier quality value for
    // LLR scaling.
    bool hasLastOFDMBroadbandSNREstimate() const;
    float getLastOFDMBroadbandSNREstimate() const;

    // Get estimated frequency offset in Hz (from pilot phase tracking)
    float getFrequencyOffset() const;

    // Get the last LTS-derived timing offset in samples.
    // Positive means the FFT window should start later.
    float getLastTimingOffsetSamples() const;

    // Set frequency offset for CFO correction (call before processPresynced)
    // Use when CFO is estimated externally (e.g., from chirp preamble)
    void setFrequencyOffset(float cfo_hz);

    // Set frequency offset with initial phase (for continuous CFO tracking)
    // initial_phase_rad: the accumulated CFO phase at the start of samples
    // This is needed when processing starts at a known position in the audio stream
    // where CFO has already accumulated phase = 2π × CFO × elapsed_samples / sample_rate
    void setFrequencyOffsetWithPhase(float cfo_hz, float initial_phase_rad);

    // Get equalized symbols for constellation display
    Symbol getConstellationSymbols() const;

    // Modulation of the symbols returned by getConstellationSymbols() (the buffer
    // is reset on modulation change, so this is the modulation of the whole set).
    Modulation getConstellationModulation() const;

    // Check if demodulator is currently synchronized (processing a frame)
    bool isSynced() const;

    // Check if demodulator has pending data (samples in buffer or accumulated soft bits)
    // Used to avoid premature reset between codewords of multi-codeword frames
    bool hasPendingData() const;

    // Get last detected sync offset (for testing/debugging)
    // Returns the sample offset where sync was detected in the input buffer
    size_t getLastSyncOffset() const;

    // Set timing offset adjustment (samples to skip after preamble)
    // Positive = start symbols later, Negative = start symbols earlier
    void setTimingOffset(int offset);

    // 2026-05-28: runtime-settable LDPC codeword bit count used as the
    // "we have a codeword's worth of soft bits" gate inside processPresynced
    // and getSoftBits. Default 648 (Z=27 legacy); set to 1944 when the
    // active burst announces Z=81 (n=1944) so the demod waits for the full
    // long-LDPC codeword before returning. Without this, processPresynced
    // returns after 648 bits of a 1944-bit codeword and the BurstInterleaver
    // throws "soft bits size mismatch" downstream.
    void setActiveLDPCBlockSize(size_t bits);
    size_t getActiveLDPCBlockSize() const;

    // Process pre-synced samples (bypass Schmidl-Cox preamble detection)
    // Use when external timing sync is provided (e.g., chirp preamble)
    //
    // ROBUST ACQUISITION:
    // - samples should start at the first OFDM symbol after chirp
    // - training_symbols: number of LTS training symbols at start (default: 2)
    //   These are used for channel estimation (not decoded as data)
    // - Remaining symbols are demodulated as data
    //
    // Returns true when enough soft bits accumulated for LDPC decode
    bool processPresynced(SampleSpan samples, int training_symbols = 2);

    // Reset state (e.g., after sync loss)
    void reset();

    // Search for Schmidl-Cox sync without changing internal state
    // Use for IWaveform::detectSync() to find sync position
    // Returns true if preamble found, fills out_position and out_cfo_hz
    // Does NOT consume samples or change demodulator state
    bool searchForSync(SampleSpan samples, size_t& out_position, float& out_cfo_hz, float threshold = 0.5f);

    // Get fading index from per-carrier channel estimate magnitudes (frequency-only CV)
    // Returns coefficient of variation (std_dev / mean) of carrier magnitudes
    // 0-1 range: < 0.1 = flat (AWGN), 0.15-0.30 = mild fading, > 0.30 = heavy fading
    // Note: OFDM internal thresholds use this directly; MC-DPSK adds temporal CV on top
    float getFadingIndex() const;
    float getLastPilotFrequencyCV() const;
    float getLastPilotTemporalCV() const;
    float getLastPilotSymbolMeanCV() const;

    // Last LTS channel-estimation metrics from processPresynced().
    float getLastLTSSignalPower() const;
    float getLastLTSChannelMagnitude() const;
    float getLastLTSResidualCFOHz() const;

    // RX-local carrier erasure is only LDPC-safe for multi-codeword OFDM
    // frames. Callers that know they are decoding a 1-CW control frame must
    // leave this disabled.
    void setRXCarrierErasureEnabled(bool enabled);

    // Compact post-equalizer diagnostics for the most recently demodulated
    // frame. Intended for failure attribution logging only.
    std::string getFailureAttributionDiagnosticsText() const;

    // ==========================================================================
    // OFFLINE TEST HOOKS (tools/lts_estimate_mse.cpp)
    // ==========================================================================
    // These bypass the streaming state machine to exercise the LTS channel
    // estimator in isolation. Not used by the production path.

    // Override the LTS DFT-denoise gate + tap window (normally env-initialized).
    void setLtsDftDenoise(bool on, int taps);

    // Override the CFO-clean 2-LTS averaging gate (normally env-initialized).
    void setLtsCfoAvg(bool on);

    // Run estimateChannelFromLTS() directly on raw passband training samples.
    void estimateChannelFromLTSTest(const float* samples, size_t num_symbols);

    // Return the per-active-carrier channel estimate (gathered at the active
    // carrier FFT bins, in frequency order) from the last LTS estimate.
    std::vector<Complex> getActiveChannelEstimate() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Channel Estimator
 *
 * Estimates and tracks HF channel response using pilots.
 * Critical for good performance on fading channels.
 */
class ChannelEstimator {
public:
    explicit ChannelEstimator(const ModemConfig& config);
    ~ChannelEstimator();

    // Update estimate from received pilots
    void updateFromPilots(const Symbol& received_pilots,
                          const Symbol& expected_pilots);

    // Equalize a received symbol
    Symbol equalize(const Symbol& received);

    // Get channel quality metrics
    ChannelQuality getQuality() const;

    // Interpolate channel between pilots
    void interpolate();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ultra
