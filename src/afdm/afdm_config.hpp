// afdm_config.hpp - AFDM configuration parameters
//
// Affine Frequency Division Multiplexing configuration for HF channels.
// Based on: https://arxiv.org/abs/2507.21704

#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <utility>
#include <vector>

namespace ultra {
namespace afdm {

/**
 * AFDM Configuration
 *
 * Key parameters:
 * - c1: Time-chirp rate - CRITICAL for full diversity on Doppler channels
 * - c2: Frequency-chirp rate - can be 0 for standard AFDM
 * - N: Number of subcarriers (DAFT size)
 *
 * Full diversity condition: c1 >= (2*k_max + 1) / (2*N)
 * where k_max = ceil(max_doppler_hz / subcarrier_spacing)
 */
struct AFDMConfig {
    // Core DAFT parameters
    // N must be large enough that the occupied bandwidth fits in HF audio band (~2.8 kHz).
    // Bin spacing is sample_rate / N (not "bandwidth / N").
    // With N=512 at 48 kHz: bin spacing = 93.75 Hz (same as OFDM).
    //
    // IMPORTANT: AFDM chirps (c1>0) are INCOMPATIBLE with narrowband audio!
    // The chirped waveform spreads energy across frequencies, overlapping with
    // the 2fc image after DSB modulation. This cannot be filtered without
    // destroying the signal. Even SSB modulation fails due to Hilbert/chirp
    // interaction.
    //
    // For HF audio-band applications (300-3000 Hz), use c1=0 (OFDM mode).
    // AFDM (c1>0) requires wideband RF with complex I/Q modulation.
    //
    // Get time-frequency diversity through interleaving instead of chirps.
    uint32_t N = 512;             // Number of subcarriers (DAFT size)
    float c1 = 0.0f;              // Time-chirp rate (0 = OFDM mode for audio)
    float c2 = 0.0f;              // Frequency-chirp rate (0 = standard)

    // Only use a subset of bins to fit within the HF audio band
    uint32_t num_carriers = 30;   // Active carriers (2.8 kHz bandwidth)

    // Frame structure
    uint32_t cpp_length = 128;    // Chirp-periodic prefix length (same ratio as OFDM CP)
    uint32_t symbols_per_frame = 11;  // DAFT symbols per frame (matches OFDM)

    // Pilot configuration
    uint32_t pilot_spacing = 8;   // Pilot every N subcarriers in DAFT domain
    uint32_t pilot_guard = 0;     // Guard bins around each pilot

    // Audio parameters
    float sample_rate = 48000.0f;
    float center_freq = 1500.0f;
    float bandwidth = 2800.0f;    // Hz (fits in 300-3000 Hz audio band)

    // Derived parameters
    float subcarrierSpacing() const {
        if (N == 0) return 0.0f;
        return sample_rate / static_cast<float>(N);
    }

    float symbolDuration() const {
        return static_cast<float>(N) / sample_rate;
    }

    // Samples per DAFT symbol (including CPP)
    uint32_t samplesPerSymbol() const {
        return N + cpp_length;
    }

    // Total samples per frame (excluding preamble)
    uint32_t samplesPerFrame() const {
        return samplesPerSymbol() * symbols_per_frame;
    }

    // Maximum Doppler bins based on typical HF Doppler (±10 Hz)
    int maxDopplerBins(float max_doppler_hz = 10.0f) const {
        return static_cast<int>(std::ceil(max_doppler_hz / subcarrierSpacing()));
    }

    // Minimum c1 required for full diversity
    float minC1ForDiversity(float max_doppler_hz = 10.0f) const {
        int k_max = maxDopplerBins(max_doppler_hz);
        return static_cast<float>(2 * k_max + 1) / (2.0f * N);
    }

    // Check if current c1 satisfies full diversity condition
    bool hasFullDiversity(float max_doppler_hz = 10.0f) const {
        return c1 >= minC1ForDiversity(max_doppler_hz);
    }

    // Get FFT indices for active carriers (centered around DC, like OFDM).
    // Returns exactly num_carriers bins (excluding DC).
    // Negative frequencies wrap to end of array.
    // Example: for num_carriers=30, N=512: bins -15..-1 and +1..+15.
    std::vector<int> getActiveCarrierIndices() const {
        std::vector<int> indices;
        if (N <= 1 || num_carriers == 0) {
            return indices;
        }

        int requested = static_cast<int>(num_carriers);
        int max_usable = static_cast<int>(N) - 1;  // Exclude DC.
        requested = std::min(requested, max_usable);
        indices.reserve(static_cast<size_t>(requested));

        int neg_count = requested / 2;
        int pos_count = requested - neg_count;

        for (int i = -neg_count; i <= -1; ++i) {
            int idx = (i + static_cast<int>(N)) % static_cast<int>(N);
            indices.push_back(idx);
        }
        for (int i = 1; i <= pos_count; ++i) {
            int idx = i % static_cast<int>(N);
            indices.push_back(idx);
        }
        return indices;
    }

    // Number of active carriers used in DAFT bins.
    uint32_t activeCarrierCount() const {
        return static_cast<uint32_t>(getActiveCarrierIndices().size());
    }

    // Number of data subcarriers (excluding pilots and guards)
    // Only count within the num_carriers active bins
    uint32_t dataSubcarriers() const {
        uint32_t active = activeCarrierCount();
        if (active == 0) return 0;
        uint32_t spacing = std::max<uint32_t>(pilot_spacing, 1u);
        // Pilots at indices 0, pilot_spacing, 2*pilot_spacing, ...
        // Use ceiling division to get correct count
        uint32_t num_pilots = (active + spacing - 1) / spacing;
        uint32_t guard_total = num_pilots * pilot_guard * 2;
        if (guard_total + num_pilots >= active) return 0;
        return active - num_pilots - guard_total;
    }

    // Occupied RF/audio band edges for active carriers after upmix.
    std::pair<float, float> occupiedBandEdgesHz() const {
        auto active = getActiveCarrierIndices();
        if (active.empty()) {
            return {center_freq, center_freq};
        }

        int min_bin = 0;
        int max_bin = 0;
        bool first = true;
        const int half = static_cast<int>(N / 2);

        for (int idx : active) {
            int signed_bin = (idx <= half) ? idx : (idx - static_cast<int>(N));
            if (first) {
                min_bin = signed_bin;
                max_bin = signed_bin;
                first = false;
                continue;
            }
            min_bin = std::min(min_bin, signed_bin);
            max_bin = std::max(max_bin, signed_bin);
        }

        const float spacing = subcarrierSpacing();
        return {
            center_freq + static_cast<float>(min_bin) * spacing,
            center_freq + static_cast<float>(max_bin) * spacing
        };
    }

    float occupiedBandwidthHz() const {
        auto edges = occupiedBandEdgesHz();
        return edges.second - edges.first;
    }

    // Data symbols per frame
    uint32_t dataSymbolsPerFrame() const {
        return dataSubcarriers() * symbols_per_frame;
    }

    // Create config optimized for HF fading channels
    // NOTE: Uses c1=0 (OFDM mode) because AFDM chirps are incompatible with
    // narrowband audio transmission. The chirped waveform spreads energy into
    // the 2fc image band, which cannot be filtered without destroying the signal.
    // Get diversity through time-frequency interleaving instead.
    static AFDMConfig forHFFading() {
        AFDMConfig cfg;
        cfg.N = 512;
        cfg.num_carriers = 30;
        cfg.c1 = 0.0f;            // OFDM mode (c1>0 incompatible with audio)
        cfg.c2 = 0.0f;
        cfg.cpp_length = 128;     // ~2.7ms for HF delay spread
        cfg.symbols_per_frame = 11;
        cfg.pilot_spacing = 8;
        cfg.pilot_guard = 0;
        return cfg;
    }

    // Create config for AWGN (OFDM mode for audio compatibility)
    static AFDMConfig forAWGN() {
        AFDMConfig cfg;
        cfg.N = 512;
        cfg.num_carriers = 30;
        cfg.c1 = 0.0f;            // OFDM mode for audio compatibility
        cfg.c2 = 0.0f;
        cfg.cpp_length = 64;
        cfg.symbols_per_frame = 11;
        cfg.pilot_spacing = 16;   // Fewer pilots needed
        cfg.pilot_guard = 0;
        return cfg;
    }

    // Create config that falls back to OFDM (c1=c2=0)
    static AFDMConfig asOFDM() {
        AFDMConfig cfg;
        cfg.N = 512;
        cfg.num_carriers = 30;
        cfg.c1 = 0.0f;
        cfg.c2 = 0.0f;
        return cfg;
    }

    // Create compact config for quick testing (smaller N)
    // Note: With N=64, bin spacing is 750 Hz. Using 8 carriers gives ~6 kHz BW.
    // This is still too wide for HF audio (~2.8 kHz), so this config is for
    // complex baseband testing only, not full audio chain testing.
    static AFDMConfig forTesting() {
        AFDMConfig cfg;
        cfg.N = 64;
        cfg.num_carriers = 8;     // Narrower bandwidth for testing
        cfg.c1 = 0.0f;            // OFDM mode for audio
        cfg.c2 = 0.0f;
        cfg.cpp_length = 16;
        cfg.symbols_per_frame = 5;
        cfg.pilot_spacing = 4;
        cfg.pilot_guard = 0;
        return cfg;
    }

    // Create config for wideband RF with complex I/Q (true AFDM)
    // WARNING: This does NOT work with audio-band DSB/SSB modulation!
    // Only use with complex baseband processing or high-rate RF systems.
    static AFDMConfig forWidebandRF() {
        AFDMConfig cfg;
        cfg.N = 512;
        cfg.num_carriers = 30;
        cfg.c1 = 0.003f;          // Full diversity for Doppler
        cfg.c2 = 0.0f;
        cfg.cpp_length = 128;
        cfg.symbols_per_frame = 11;
        cfg.pilot_spacing = 8;
        cfg.pilot_guard = 0;
        return cfg;
    }
};

} // namespace afdm
} // namespace ultra
