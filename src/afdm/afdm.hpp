// afdm.hpp - AFDM Modulator and Demodulator
//
// Affine Frequency Division Multiplexing for doubly-selective HF channels.
// AFDM uses the Discrete Affine Fourier Transform (DAFT) which provides
// full diversity over delay-Doppler channels when c1 is properly chosen.
//
// Key advantages over OFDM:
// - Full diversity on fading channels (OFDM loses orthogonality)
// - Built-in time-frequency spreading (natural interleaving)
// - ~1 dB gain over OTFS, significant gain over OFDM on fading
//
// References:
// - https://arxiv.org/abs/2507.21704 (AFDM for 6G)

#pragma once

#include "afdm_config.hpp"
#include "daft.hpp"
#include "ultra/types.hpp"
#include <vector>
#include <memory>

namespace ultra {
namespace afdm {

/**
 * AFDM Modulator
 *
 * Converts data bytes to time-domain audio samples using AFDM modulation.
 *
 * TX chain: data → symbol mapping → pilot insertion → IDAFT → add CPP → upmix
 */
class AFDMModulator {
public:
    explicit AFDMModulator(const AFDMConfig& config = AFDMConfig::forHFFading());

    // Reconfigure modulator
    void configure(const AFDMConfig& config);

    // Get current configuration
    const AFDMConfig& config() const { return config_; }

    // ========================================================================
    // High-level TX interface
    // ========================================================================

    /**
     * Transmit data as AFDM frame (without preamble)
     *
     * @param data Input data bytes
     * @param mod Modulation scheme (DQPSK recommended for HF)
     * @return Time-domain audio samples (real-valued, baseband)
     */
    Samples modulate(ByteSpan data, Modulation mod);

    /**
     * Generate preamble for synchronization
     *
     * Uses dual-chirp preamble compatible with existing ChirpSync.
     * @return Preamble audio samples
     */
    Samples generatePreamble();

    /**
     * Full TX: preamble + data frame
     */
    Samples transmitFrame(ByteSpan data, Modulation mod);

    // ========================================================================
    // Low-level TX interface
    // ========================================================================

    /**
     * Map data bytes to DAFT-domain symbols
     */
    std::vector<Complex> mapToSymbols(ByteSpan data, Modulation mod);

    /**
     * Insert pilots into DAFT-domain frame
     */
    std::vector<Complex> insertPilots(const std::vector<Complex>& data_symbols);

    /**
     * Generate one AFDM symbol (IDAFT + CPP)
     */
    std::vector<Complex> generateSymbol(const std::vector<Complex>& daft_bins);

    /**
     * Upmix baseband to audio center frequency
     */
    Samples upmix(const std::vector<Complex>& baseband);

    // ========================================================================
    // Capacity information
    // ========================================================================

    // Data symbols per frame (excluding pilots)
    size_t dataSymbolsPerFrame() const;

    // Bits per frame for given modulation
    size_t bitsPerFrame(Modulation mod) const;

    // Bytes per frame for given modulation
    size_t bytesPerFrame(Modulation mod) const;

    // Samples per frame (excluding preamble)
    size_t samplesPerFrame() const;

    // Preamble samples
    size_t preambleSamples() const;

private:
    AFDMConfig config_;
    DAFTProcessor daft_processor_;

    // Active carrier management (only use subset of N bins for narrowband signal)
    std::vector<int> active_carriers_;   // FFT indices of active carriers
    std::vector<int> pilot_indices_;     // FFT indices of pilot positions
    std::vector<int> data_indices_;      // FFT indices of data positions

    // Pilot sequence (known symbols for channel estimation)
    std::vector<Complex> pilot_sequence_;

    void initPilotSequence();
};

/**
 * AFDM Demodulator
 *
 * Converts received audio samples back to data bytes.
 *
 * RX chain: downmix → remove CPP → DAFT → channel estimation → equalization → demapping
 */
class AFDMDemodulator {
public:
    explicit AFDMDemodulator(const AFDMConfig& config = AFDMConfig::forHFFading());

    // Reconfigure demodulator
    void configure(const AFDMConfig& config);

    // Get current configuration
    const AFDMConfig& config() const { return config_; }

    // ========================================================================
    // High-level RX interface
    // ========================================================================

    /**
     * Demodulate received samples to soft bits
     *
     * @param samples Received audio samples (after sync, at frame start)
     * @param mod Modulation scheme used
     * @return Soft bits (LLRs) for FEC decoder
     */
    std::vector<float> demodulate(SampleSpan samples, Modulation mod);

    /**
     * Demodulate to hard bits (for testing without FEC)
     */
    Bytes demodulateHard(SampleSpan samples, Modulation mod);

    // ========================================================================
    // Low-level RX interface
    // ========================================================================

    /**
     * Downmix from audio to baseband
     */
    std::vector<Complex> downmix(SampleSpan samples);

    /**
     * Extract one DAFT symbol (remove CPP + DAFT)
     */
    std::vector<Complex> extractSymbol(const std::vector<Complex>& baseband, size_t symbol_idx);

    /**
     * Estimate channel from pilots
     */
    void estimateChannel(const std::vector<Complex>& daft_symbols);

    /**
     * Equalize received symbols using channel estimate
     */
    std::vector<Complex> equalize(const std::vector<Complex>& daft_symbols);

    /**
     * Extract data symbols (remove pilots)
     */
    std::vector<Complex> extractDataSymbols(const std::vector<Complex>& equalized);

    /**
     * Demap symbols to soft bits
     */
    std::vector<float> demapSoft(const std::vector<Complex>& symbols, Modulation mod);

    // ========================================================================
    // Status and diagnostics
    // ========================================================================

    // Estimated SNR from pilots (dB)
    float estimatedSNR() const { return estimated_snr_db_; }

    // Fading index (coefficient of variation of channel magnitudes)
    float fadingIndex() const { return fading_index_; }

    // Is channel significantly fading? (fading_index > 0.65)
    bool isFading() const { return fading_index_ > 0.65f; }

    // Get channel estimate in DAFT domain
    const std::vector<Complex>& channelEstimate() const { return channel_estimate_; }

    // Apply CFO correction (call before demodulate if CFO known from sync)
    void setCFO(float cfo_hz);

    // Reset state
    void reset();

    // Bits per frame for given modulation (matches modulator)
    size_t bitsPerFrame(Modulation mod) const;

private:
    AFDMConfig config_;
    DAFTProcessor daft_processor_;

    // Channel state
    std::vector<Complex> channel_estimate_;
    float estimated_snr_db_ = 0.0f;
    float fading_index_ = 0.0f;
    float cfo_hz_ = 0.0f;

    // Active carrier management (only use subset of N bins for narrowband signal)
    std::vector<int> active_carriers_;   // FFT indices of active carriers
    std::vector<int> pilot_indices_;     // FFT indices of pilot positions
    std::vector<int> data_indices_;      // FFT indices of data positions

    // Pilot sequence (must match modulator)
    std::vector<Complex> pilot_sequence_;

    void initPilotSequence();
    void computePilotIndices();
};

// ============================================================================
// Symbol mapping utilities (shared between mod/demod)
// ============================================================================

namespace mapping {

// Map bits to constellation symbol
Complex mapSymbol(uint32_t bits, Modulation mod);

// Demap symbol to hard bits
uint32_t demapHard(Complex symbol, Modulation mod);

// Demap symbol to soft bits (LLRs)
// Returns vector of LLRs, one per bit
std::vector<float> demapSoft(Complex symbol, Modulation mod, float noise_var);

// Get constellation points for modulation
std::vector<Complex> getConstellation(Modulation mod);

} // namespace mapping

} // namespace afdm
} // namespace ultra
