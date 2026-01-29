// daft.cpp - Discrete Affine Fourier Transform implementation
//
// DAFT(x) = chirp(c2) ⊙ FFT(chirp(c1) ⊙ x)
// IDAFT(x) = chirp(-c1) ⊙ IFFT(chirp(-c2) ⊙ x)

#define _USE_MATH_DEFINES
#include "daft.hpp"
#include <cmath>
#include <stdexcept>
#include <algorithm>

namespace ultra {
namespace afdm {

// ============================================================================
// FFT Implementation (radix-2 Cooley-Tukey)
// ============================================================================

static void fft_inplace(std::vector<Complex>& x, bool inverse) {
    const size_t N = x.size();
    if (N <= 1) return;

    // Check power of 2
    if ((N & (N - 1)) != 0) {
        throw std::invalid_argument("FFT size must be power of 2");
    }

    // Bit-reversal permutation
    for (size_t i = 1, j = 0; i < N; ++i) {
        size_t bit = N >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }

    // Cooley-Tukey iterative FFT
    for (size_t len = 2; len <= N; len *= 2) {
        float angle = 2.0f * static_cast<float>(M_PI) / len;
        if (!inverse) angle = -angle;

        Complex wlen(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < N; i += len) {
            Complex w(1.0f, 0.0f);
            for (size_t j = 0; j < len / 2; ++j) {
                Complex u = x[i + j];
                Complex v = x[i + j + len / 2] * w;
                x[i + j] = u + v;
                x[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }

    // Scale for inverse FFT
    if (inverse) {
        float scale = 1.0f / static_cast<float>(N);
        for (auto& val : x) {
            val *= scale;
        }
    }
}

static std::vector<Complex> fft(const std::vector<Complex>& x, bool inverse) {
    std::vector<Complex> result = x;
    fft_inplace(result, inverse);
    return result;
}

// ============================================================================
// Chirp Table
// ============================================================================

ChirpTable::ChirpTable(size_t N, float c) {
    rebuild(N, c);
}

void ChirpTable::rebuild(size_t N, float c) {
    table_.resize(N);

    // Compute exp(-j2π·c·n²) for n = 0..N-1
    const float two_pi_c = 2.0f * static_cast<float>(M_PI) * c;

    for (size_t n = 0; n < N; ++n) {
        float phase = two_pi_c * static_cast<float>(n * n);
        table_[n] = Complex(std::cos(phase), -std::sin(phase));
    }
}

// ============================================================================
// DAFT / IDAFT Functions
// ============================================================================

std::vector<Complex> daft(
    const std::vector<Complex>& time_samples,
    float c1,
    float c2
) {
    const size_t N = time_samples.size();
    if (N == 0) return {};

    // Special case: c1=c2=0 is standard FFT
    if (isStandardFFT(c1, c2)) {
        return fft(time_samples, false);
    }

    std::vector<Complex> result(N);
    const float two_pi = 2.0f * static_cast<float>(M_PI);

    // Step 1: Pre-chirp with c1
    // chirped[n] = x[n] · exp(-j2π·c1·n²)
    std::vector<Complex> chirped(N);
    for (size_t n = 0; n < N; ++n) {
        float phase_c1 = two_pi * c1 * static_cast<float>(n * n);
        Complex chirp_c1(std::cos(phase_c1), -std::sin(phase_c1));
        chirped[n] = time_samples[n] * chirp_c1;
    }

    // Step 2: FFT
    auto fft_result = fft(chirped, false);

    // Step 3: Post-chirp with c2
    // result[m] = fft_result[m] · exp(-j2π·c2·m²)
    for (size_t m = 0; m < N; ++m) {
        float phase_c2 = two_pi * c2 * static_cast<float>(m * m);
        Complex chirp_c2(std::cos(phase_c2), -std::sin(phase_c2));
        result[m] = fft_result[m] * chirp_c2;
    }

    return result;
}

std::vector<Complex> idaft(
    const std::vector<Complex>& daft_symbols,
    float c1,
    float c2
) {
    const size_t N = daft_symbols.size();
    if (N == 0) return {};

    // Special case: c1=c2=0 is standard IFFT
    if (isStandardFFT(c1, c2)) {
        return fft(daft_symbols, true);
    }

    std::vector<Complex> result(N);
    const float two_pi = 2.0f * static_cast<float>(M_PI);

    // Step 1: Pre-chirp with -c2 (conjugate of forward c2 chirp)
    // chirped[m] = x[m] · exp(+j2π·c2·m²)
    std::vector<Complex> chirped(N);
    for (size_t m = 0; m < N; ++m) {
        float phase_c2 = two_pi * c2 * static_cast<float>(m * m);
        Complex chirp_c2_conj(std::cos(phase_c2), std::sin(phase_c2));
        chirped[m] = daft_symbols[m] * chirp_c2_conj;
    }

    // Step 2: IFFT
    auto ifft_result = fft(chirped, true);

    // Step 3: Post-chirp with -c1 (conjugate of forward c1 chirp)
    // result[n] = ifft_result[n] · exp(+j2π·c1·n²)
    for (size_t n = 0; n < N; ++n) {
        float phase_c1 = two_pi * c1 * static_cast<float>(n * n);
        Complex chirp_c1_conj(std::cos(phase_c1), std::sin(phase_c1));
        result[n] = ifft_result[n] * chirp_c1_conj;
    }

    return result;
}

// ============================================================================
// DAFT Processor (optimized with pre-computed tables)
// ============================================================================

DAFTProcessor::DAFTProcessor(size_t N, float c1, float c2) {
    configure(N, c1, c2);
}

void DAFTProcessor::configure(size_t N, float c1, float c2) {
    N_ = N;
    c1_ = c1;
    c2_ = c2;

    // Pre-compute chirp tables
    chirp_c1_.rebuild(N, c1);
    chirp_c2_.rebuild(N, c2);
}

std::vector<Complex> DAFTProcessor::forward(const std::vector<Complex>& time_samples) const {
    if (time_samples.size() != N_) {
        throw std::invalid_argument("Input size must match DAFT size N");
    }

    // Special case: standard FFT
    if (isStandardFFT(c1_, c2_)) {
        return fft(time_samples, false);
    }

    // Step 1: Pre-chirp with c1
    std::vector<Complex> chirped(N_);
    for (size_t n = 0; n < N_; ++n) {
        chirped[n] = time_samples[n] * chirp_c1_[n];
    }

    // Step 2: FFT
    fft_inplace(chirped, false);

    // Step 3: Post-chirp with c2
    for (size_t m = 0; m < N_; ++m) {
        chirped[m] *= chirp_c2_[m];
    }

    return chirped;
}

std::vector<Complex> DAFTProcessor::inverse(const std::vector<Complex>& daft_symbols) const {
    if (daft_symbols.size() != N_) {
        throw std::invalid_argument("Input size must match DAFT size N");
    }

    // Special case: standard IFFT
    if (isStandardFFT(c1_, c2_)) {
        return fft(daft_symbols, true);
    }

    // Step 1: Pre-chirp with conjugate of c2
    std::vector<Complex> chirped(N_);
    for (size_t m = 0; m < N_; ++m) {
        chirped[m] = daft_symbols[m] * chirp_c2_.conj(m);
    }

    // Step 2: IFFT
    fft_inplace(chirped, true);

    // Step 3: Post-chirp with conjugate of c1
    for (size_t n = 0; n < N_; ++n) {
        chirped[n] *= chirp_c1_.conj(n);
    }

    return chirped;
}

void DAFTProcessor::forwardInPlace(std::vector<Complex>& samples) const {
    if (samples.size() != N_) {
        throw std::invalid_argument("Input size must match DAFT size N");
    }

    if (isStandardFFT(c1_, c2_)) {
        fft_inplace(samples, false);
        return;
    }

    // Pre-chirp with c1
    for (size_t n = 0; n < N_; ++n) {
        samples[n] *= chirp_c1_[n];
    }

    // FFT
    fft_inplace(samples, false);

    // Post-chirp with c2
    for (size_t m = 0; m < N_; ++m) {
        samples[m] *= chirp_c2_[m];
    }
}

void DAFTProcessor::inverseInPlace(std::vector<Complex>& symbols) const {
    if (symbols.size() != N_) {
        throw std::invalid_argument("Input size must match DAFT size N");
    }

    if (isStandardFFT(c1_, c2_)) {
        fft_inplace(symbols, true);
        return;
    }

    // Pre-chirp with conjugate of c2
    for (size_t m = 0; m < N_; ++m) {
        symbols[m] *= chirp_c2_.conj(m);
    }

    // IFFT
    fft_inplace(symbols, true);

    // Post-chirp with conjugate of c1
    for (size_t n = 0; n < N_; ++n) {
        symbols[n] *= chirp_c1_.conj(n);
    }
}

} // namespace afdm
} // namespace ultra
