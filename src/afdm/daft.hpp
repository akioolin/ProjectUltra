// daft.hpp - Discrete Affine Fourier Transform
//
// The DAFT is the core transform for AFDM modulation.
// It generalizes the DFT with chirp modulation parameters c1 and c2.
//
// Basis function: φ_m[n] = exp(j2π(c1·n² + c2·m² + m·n/N))
//
// Key property: Setting c1=c2=0 recovers the standard DFT/IDFT.
//
// References:
// - https://arxiv.org/abs/2507.21704 (AFDM for 6G)
// - https://arxiv.org/html/2509.16643v1 (AFDM Requirements)

#pragma once

#include "ultra/types.hpp"
#include <vector>

namespace ultra {
namespace afdm {

/**
 * Compute the Discrete Affine Fourier Transform (DAFT)
 *
 * Transforms from time-domain to DAFT-domain (demodulation).
 *
 * x[m] = (1/N) Σ X[n] · exp(-j2π(c1·n² + c2·m² + m·n/N))
 *
 * Implementation: chirp(c1) → FFT → chirp(c2)
 *
 * @param time_samples Input time-domain samples (size N)
 * @param c1 Time-chirp rate parameter
 * @param c2 Frequency-chirp rate parameter
 * @return DAFT-domain symbols (size N)
 */
std::vector<Complex> daft(
    const std::vector<Complex>& time_samples,
    float c1,
    float c2
);

/**
 * Compute the Inverse Discrete Affine Fourier Transform (IDAFT)
 *
 * Transforms from DAFT-domain to time-domain (modulation).
 *
 * X[n] = Σ x[m] · exp(j2π(c1·n² + c2·m² + m·n/N))
 *
 * Implementation: chirp(-c2) → IFFT → chirp(-c1)
 *
 * @param daft_symbols Input DAFT-domain symbols (size N)
 * @param c1 Time-chirp rate parameter
 * @param c2 Frequency-chirp rate parameter
 * @return Time-domain samples (size N)
 */
std::vector<Complex> idaft(
    const std::vector<Complex>& daft_symbols,
    float c1,
    float c2
);

/**
 * Pre-compute chirp tables for efficient repeated transforms
 *
 * For real-time operation, pre-computing exp(-j2π·c·n²) avoids
 * repeated trigonometric calculations.
 */
class ChirpTable {
public:
    ChirpTable() = default;
    explicit ChirpTable(size_t N, float c);

    // Get chirp value at index n: exp(-j2π·c·n²)
    Complex operator[](size_t n) const { return table_[n]; }

    // Get conjugate chirp value: exp(+j2π·c·n²)
    Complex conj(size_t n) const { return std::conj(table_[n]); }

    size_t size() const { return table_.size(); }
    bool empty() const { return table_.empty(); }

    // Rebuild table with new parameters
    void rebuild(size_t N, float c);

private:
    std::vector<Complex> table_;
};

/**
 * DAFT Processor with pre-computed chirp tables
 *
 * More efficient for repeated transforms with same parameters.
 */
class DAFTProcessor {
public:
    DAFTProcessor() = default;
    DAFTProcessor(size_t N, float c1, float c2);

    // Initialize or reconfigure
    void configure(size_t N, float c1, float c2);

    // Forward transform (time → DAFT domain)
    std::vector<Complex> forward(const std::vector<Complex>& time_samples) const;

    // Inverse transform (DAFT → time domain)
    std::vector<Complex> inverse(const std::vector<Complex>& daft_symbols) const;

    // In-place transforms (more efficient)
    void forwardInPlace(std::vector<Complex>& samples) const;
    void inverseInPlace(std::vector<Complex>& symbols) const;

    // Accessors
    size_t size() const { return N_; }
    float c1() const { return c1_; }
    float c2() const { return c2_; }

private:
    size_t N_ = 0;
    float c1_ = 0.0f;
    float c2_ = 0.0f;
    ChirpTable chirp_c1_;
    ChirpTable chirp_c2_;
};

// Utility: Check if DAFT reduces to standard FFT
inline bool isStandardFFT(float c1, float c2, float tolerance = 1e-6f) {
    return std::abs(c1) < tolerance && std::abs(c2) < tolerance;
}

} // namespace afdm
} // namespace ultra
