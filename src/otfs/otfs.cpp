#define _USE_MATH_DEFINES  // For M_PI on MSVC
#include <cmath>
#include "ultra/otfs.hpp"
#include "ultra/dsp.hpp"
#include <random>
#include <stdexcept>
#include <algorithm>

namespace ultra {

// ============================================================================
// ISFFT and SFFT implementations
// ============================================================================

// Simple 1D FFT (in-place, radix-2 Cooley-Tukey)
static void fft1d(std::vector<Complex>& x, bool inverse, bool scale_inverse = true) {
    size_t N = x.size();
    if (N <= 1) return;

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
        float angle = 2 * M_PI / len * (inverse ? 1 : -1);
        Complex wlen(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < N; i += len) {
            Complex w(1, 0);
            for (size_t j = 0; j < len / 2; ++j) {
                Complex u = x[i + j];
                Complex v = x[i + j + len / 2] * w;
                x[i + j] = u + v;
                x[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }

    if (inverse && scale_inverse) {
        for (auto& val : x) {
            val /= static_cast<float>(N);
        }
    }
}

// ISFFT: Delay-Doppler → Time-Frequency
void isfft(const std::vector<Complex>& dd_grid,
           std::vector<Complex>& tf_grid,
           uint32_t M, uint32_t N) {
    if (dd_grid.size() != M * N) {
        throw std::invalid_argument("DD grid size mismatch");
    }

    tf_grid.resize(M * N);
    std::vector<Complex> temp(M * N);

    // Step 1: IFFT along columns (Doppler → time)
    for (uint32_t k = 0; k < M; ++k) {
        std::vector<Complex> col(N);
        for (uint32_t l = 0; l < N; ++l) {
            col[l] = dd_grid[k * N + l];
        }
        fft1d(col, true, false);
        for (uint32_t n = 0; n < N; ++n) {
            temp[k * N + n] = col[n];
        }
    }

    // Step 2: FFT along rows (delay → frequency)
    for (uint32_t n = 0; n < N; ++n) {
        std::vector<Complex> row(M);
        for (uint32_t k = 0; k < M; ++k) {
            row[k] = temp[k * N + n];
        }
        fft1d(row, false, false);
        for (uint32_t m = 0; m < M; ++m) {
            tf_grid[n * M + m] = row[m];
        }
    }
}

// SFFT: Time-Frequency → Delay-Doppler
void sfft(const std::vector<Complex>& tf_grid,
          std::vector<Complex>& dd_grid,
          uint32_t M, uint32_t N) {
    if (tf_grid.size() != M * N) {
        throw std::invalid_argument("TF grid size mismatch");
    }

    dd_grid.resize(M * N);
    std::vector<Complex> temp(M * N);

    // Step 1: FFT along columns (time → Doppler)
    for (uint32_t m = 0; m < M; ++m) {
        std::vector<Complex> col(N);
        for (uint32_t n = 0; n < N; ++n) {
            col[n] = tf_grid[n * M + m];
        }
        fft1d(col, false, false);
        for (uint32_t l = 0; l < N; ++l) {
            temp[m * N + l] = col[l];
        }
    }

    // Step 2: IFFT along rows (frequency → delay)
    for (uint32_t l = 0; l < N; ++l) {
        std::vector<Complex> row(M);
        for (uint32_t m = 0; m < M; ++m) {
            row[m] = temp[m * N + l];
        }
        fft1d(row, true, false);
        for (uint32_t k = 0; k < M; ++k) {
            dd_grid[k * N + l] = row[k];
        }
    }

    // Scale by 1/MN for roundtrip identity
    float scale = 1.0f / static_cast<float>(M * N);
    for (auto& val : dd_grid) {
        val *= scale;
    }
}

// ============================================================================
// Constellation mapping
// ============================================================================

namespace {

constexpr float QPSK_SCALE = 0.7071067811865476f;  // 1/sqrt(2)
const Complex QPSK_MAP[] = {
    Complex(-QPSK_SCALE, -QPSK_SCALE),  // 00
    Complex(-QPSK_SCALE,  QPSK_SCALE),  // 01
    Complex( QPSK_SCALE, -QPSK_SCALE),  // 10
    Complex( QPSK_SCALE,  QPSK_SCALE),  // 11
};

// Known pilot symbol for channel estimation
const Complex PILOT_SYMBOL = Complex(1.0f, 0.0f);

// Demodulator constants
constexpr float REAL_TO_COMPLEX_SCALE = 2.4f;  // Compensates for single-sideband extraction
constexpr float PREAMBLE_TARGET_RMS = 0.1f;

// LLR clipping for numerical stability
constexpr float MAX_LLR = 30.0f;
constexpr float MIN_LLR_MAG = 0.001f;
constexpr float QAM16_THRESHOLD = 0.6324555320336759f;  // 2/sqrt(10)

inline float clipLLR(float llr) {
    float clipped = std::max(-MAX_LLR, std::min(MAX_LLR, llr));
    if (std::abs(clipped) < MIN_LLR_MAG) {
        clipped = (clipped >= 0) ? MIN_LLR_MAG : -MIN_LLR_MAG;
    }
    return clipped;
}

// DQPSK differential phase rotations (Gray-coded: 00→0°, 01→90°, 11→180°, 10→-90°)
const Complex DQPSK_ROTATIONS[] = {
    Complex(1.0f, 0.0f),   // 00 → 0°   (no rotation)
    Complex(0.0f, 1.0f),   // 01 → +90° (multiply by j)
    Complex(0.0f, -1.0f),  // 10 → -90° (multiply by -j)
    Complex(-1.0f, 0.0f),  // 11 → 180° (multiply by -1)
};

// Soft demap DQPSK differential symbol (0°/90°/180°/270° constellation)
// Input: phase difference symbol (current * conj(previous))
// Output: 2 LLRs for the 2 bits
//
// Gray-coded constellation (matching DQPSK_ROTATIONS):
//   00 → 0°   → (+1, 0)
//   01 → 90°  → (0, +1)
//   10 → 270° → (0, -1)
//   11 → 180° → (-1, 0)
//
// Decision boundaries are at 45° angles, not I/Q axes:
//   bit0=0 when I+Q > 0 (upper-right half-plane)
//   bit0=1 when I+Q < 0 (lower-left half-plane)
//   bit1=0 when I-Q > 0 (lower-right half-plane)
//   bit1=1 when I-Q < 0 (upper-left half-plane)
void softDemapDQPSK(const Complex& diff_symbol, float noise_var,
                    std::vector<float>& llrs) {
    noise_var = std::max(0.001f, noise_var);

    float I = diff_symbol.real();
    float Q = diff_symbol.imag();
    float scale = 2.0f / noise_var;

    // LLR for bit 0: based on I+Q (positive → bit0=0)
    float llr0 = clipLLR(scale * (I + Q));

    // LLR for bit 1: based on I-Q (positive → bit1=0)
    float llr1 = clipLLR(scale * (I - Q));

    llrs.push_back(llr0);
    llrs.push_back(llr1);
}

Complex mapBits(uint32_t bits, Modulation mod) {
    switch (mod) {
        case Modulation::BPSK:
            return (bits & 1) ? Complex(1, 0) : Complex(-1, 0);
        case Modulation::QPSK:
            return QPSK_MAP[bits & 3];
        case Modulation::QAM16: {
            static const float levels[] = {-3, -1, 3, 1};
            static const float scale = 0.3162277660168379f;
            int i_bits = (bits >> 2) & 0x3;
            int q_bits = bits & 0x3;
            return Complex(levels[i_bits] * scale, levels[q_bits] * scale);
        }
        default:
            return QPSK_MAP[bits & 3];
    }
}

// Soft demapping with proper LLR scaling
// LLR convention: negative = bit 1, positive = bit 0
void softDemap(const Complex& symbol, Modulation mod, float noise_var,
               std::vector<float>& llrs) {
    // Clamp noise variance to prevent numerical issues
    noise_var = std::max(0.001f, noise_var);

    switch (mod) {
        case Modulation::BPSK: {
            llrs.push_back(clipLLR(-2.0f * symbol.real() / noise_var));
            break;
        }
        case Modulation::QPSK: {
            float scale = -2.0f * QPSK_SCALE / noise_var;
            llrs.push_back(clipLLR(symbol.real() * scale));
            llrs.push_back(clipLLR(symbol.imag() * scale));
            break;
        }
        case Modulation::QAM16: {
            // 16-QAM: 4 bits per symbol
            // Gray code: levels[] = {-3, -1, 3, 1} for bits 00,01,10,11
            float I = symbol.real();
            float Q = symbol.imag();
            float scale = 2.0f / noise_var;

            // I bits (bits 3,2 of 4-bit word)
            llrs.push_back(clipLLR(-scale * I));                              // bit3 (MSB): sign
            llrs.push_back(clipLLR(scale * (std::abs(I) - QAM16_THRESHOLD))); // bit2: outer/inner

            // Q bits (bits 1,0 of 4-bit word)
            llrs.push_back(clipLLR(-scale * Q));                              // bit1: sign
            llrs.push_back(clipLLR(scale * (std::abs(Q) - QAM16_THRESHOLD))); // bit0: outer/inner
            break;
        }
        default: {
            // Fallback for other modulations - treat as QPSK
            float scale = -2.0f * QPSK_SCALE / noise_var;
            llrs.push_back(clipLLR(symbol.real() * scale));
            llrs.push_back(clipLLR(symbol.imag() * scale));
            break;
        }
    }
}

} // anonymous namespace

// ============================================================================
// OTFSModulator implementation
// ============================================================================

struct OTFSModulator::Impl {
    OTFSConfig config;
    FFT fft;
    NCO mixer;
    std::vector<Complex> sync_sequence;

    Impl(const OTFSConfig& cfg)
        : config(cfg)
        , fft(cfg.fft_size)
        , mixer(cfg.center_freq, cfg.sample_rate)
    {
        generateSyncSequence();
    }

    void generateSyncSequence() {
        size_t N = config.M;
        sync_sequence.resize(N);
        for (size_t n = 0; n < N; ++n) {
            float phase = -M_PI * n * (n + 1) / N;
            sync_sequence[n] = Complex(std::cos(phase), std::sin(phase));
        }
    }

    std::vector<Complex> createOFDMSymbol(const std::vector<Complex>& freq_data) {
        std::vector<Complex> freq_domain(config.fft_size, Complex(0, 0));

        // Map to bins 1..M (skip DC, positive frequencies only)
        for (size_t i = 0; i < config.M && i < freq_data.size(); ++i) {
            size_t idx = i + 1;
            if (idx < config.fft_size / 2) {
                freq_domain[idx] = freq_data[i];
            }
        }

        std::vector<Complex> time_domain;
        fft.inverse(freq_domain, time_domain);

        // Add cyclic prefix
        std::vector<Complex> with_cp;
        with_cp.reserve(config.fft_size + config.cp_length);
        for (size_t i = config.fft_size - config.cp_length; i < config.fft_size; ++i) {
            with_cp.push_back(time_domain[i]);
        }
        for (const auto& s : time_domain) {
            with_cp.push_back(s);
        }

        return with_cp;
    }

    Samples complexToReal(const std::vector<Complex>& signal) {
        Samples real(signal.size());
        for (size_t i = 0; i < signal.size(); ++i) {
            Complex mixed = signal[i] * mixer.next();
            real[i] = mixed.real();
        }
        return real;
    }
};

OTFSModulator::OTFSModulator(const OTFSConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

OTFSModulator::~OTFSModulator() = default;

size_t OTFSModulator::symbolsPerFrame() const {
    return impl_->config.total_data_symbols();
}

size_t OTFSModulator::bitsPerFrame(Modulation mod) const {
    return symbolsPerFrame() * getBitsPerSymbol(mod);
}

std::vector<Complex> OTFSModulator::mapToDD(ByteSpan data, Modulation mod) {
    const auto& cfg = impl_->config;

    // Create DD grid
    std::vector<Complex> dd_grid(cfg.M * cfg.N, Complex(0, 0));

    // DD-domain differential encoding: each symbol is a phase rotation from previous
    // This makes OTFS robust to phase errors on fading channels
    if (cfg.dd_differential) {
        // Differential encoding: 2 bits per symbol (DQPSK-style)
        Complex prev_symbol(1.0f, 0.0f);  // Reference symbol
        size_t data_idx = 0;
        size_t bit_idx = 0;

        for (uint32_t k = 0; k < cfg.M; ++k) {
            for (uint32_t l = 0; l < cfg.N; ++l) {
                size_t grid_idx = k * cfg.N + l;

                if (data_idx < data.size()) {
                    // Extract 2 bits for DQPSK
                    uint32_t bits = 0;
                    for (size_t b = 0; b < 2; ++b) {
                        if (data_idx < data.size()) {
                            uint8_t byte = data[data_idx];
                            uint8_t bit = (byte >> (7 - bit_idx)) & 1;
                            bits = (bits << 1) | bit;
                            ++bit_idx;
                            if (bit_idx >= 8) {
                                bit_idx = 0;
                                ++data_idx;
                            }
                        }
                    }

                    // Apply phase rotation to previous symbol
                    Complex rotation = DQPSK_ROTATIONS[bits & 0x3];
                    Complex current = prev_symbol * rotation;
                    dd_grid[grid_idx] = current;
                    prev_symbol = current;
                }
            }
        }
    } else {
        // Non-differential (coherent) encoding with DD-domain pilots
        size_t bits_per_symbol = getBitsPerSymbol(mod);
        size_t data_idx = 0;
        size_t bit_idx = 0;

        // DD pilot configuration
        uint32_t guard_k = cfg.dd_pilot_enable ? cfg.dd_pilot_guard_delay : 0;
        uint32_t guard_l = cfg.dd_pilot_enable ? cfg.dd_pilot_guard_doppler : 0;

        for (uint32_t k = 0; k < cfg.M; ++k) {
            for (uint32_t l = 0; l < cfg.N; ++l) {
                size_t grid_idx = k * cfg.N + l;

                // Check if this is pilot or guard region
                if (cfg.dd_pilot_enable && k < guard_k && l < guard_l) {
                    if (k == 0 && l == 0) {
                        // Pilot symbol at (0,0) - boosted for reliable detection
                        dd_grid[grid_idx] = Complex(2.0f, 0.0f);
                    } else {
                        // Guard region - zeros
                        dd_grid[grid_idx] = Complex(0.0f, 0.0f);
                    }
                    continue;
                }

                // Data symbol
                if (data_idx < data.size()) {
                    uint32_t bits = 0;
                    for (size_t b = 0; b < bits_per_symbol; ++b) {
                        if (data_idx < data.size()) {
                            uint8_t byte = data[data_idx];
                            uint8_t bit = (byte >> (7 - bit_idx)) & 1;
                            bits = (bits << 1) | bit;
                            ++bit_idx;
                            if (bit_idx >= 8) {
                                bit_idx = 0;
                                ++data_idx;
                            }
                        }
                    }
                    dd_grid[grid_idx] = mapBits(bits, mod);
                }
            }
        }
    }

    return dd_grid;
}

Samples OTFSModulator::modulate(const std::vector<Complex>& dd_symbols, Modulation mod) {
    const auto& cfg = impl_->config;

    // Step 1: ISFFT (DD → TF)
    std::vector<Complex> tf_grid;
    isfft(dd_symbols, tf_grid, cfg.M, cfg.N);

    // Step 2: OFDM modulate each TF symbol
    // Channel estimation comes from preamble (no TF pilots needed)
    Samples output;
    impl_->mixer.reset();

    for (uint32_t n = 0; n < cfg.N; ++n) {
        std::vector<Complex> freq_data(cfg.M);

        for (uint32_t m = 0; m < cfg.M; ++m) {
            freq_data[m] = tf_grid[n * cfg.M + m];
        }

        auto ofdm_symbol = impl_->createOFDMSymbol(freq_data);
        auto real_symbol = impl_->complexToReal(ofdm_symbol);
        output.insert(output.end(), real_symbol.begin(), real_symbol.end());
    }

    return output;
}

Samples OTFSModulator::generatePreamble() {
    impl_->mixer.reset();

    Samples preamble;
    auto sync_symbol = impl_->createOFDMSymbol(impl_->sync_sequence);
    auto sync_real = impl_->complexToReal(sync_symbol);

    // Normalize preamble
    float preamble_rms = 0;
    for (float s : sync_real) preamble_rms += s * s;
    preamble_rms = std::sqrt(preamble_rms / sync_real.size());
    if (preamble_rms > 0) {
        float scale = PREAMBLE_TARGET_RMS / preamble_rms;
        for (float& s : sync_real) s *= scale;
    }

    // Repeat 4 times for robust detection
    for (int i = 0; i < 4; ++i) {
        preamble.insert(preamble.end(), sync_real.begin(), sync_real.end());
    }

    return preamble;
}

// ============================================================================
// OTFSDemodulator implementation
// ============================================================================

struct OTFSDemodulator::Impl {
    OTFSConfig config;
    FFT fft;
    NCO mixer;

    enum class State { SEARCHING, SYNCED, FRAME_READY };
    State state = State::SEARCHING;

    std::vector<float> sample_buffer;
    std::vector<Complex> tf_buffer;      // Raw TF grid from OFDM demod
    std::vector<Complex> tf_equalized;   // Equalized TF grid
    std::vector<Complex> channel_est;    // Per-subcarrier channel estimate from preamble
    bool channel_estimated = false;
    uint32_t symbols_received = 0;

    size_t total_samples_processed = 0;
    size_t current_frame_start = 0;

    std::vector<Complex> dd_symbols;
    std::vector<float> soft_bits;
    // Default noise variance (~10 dB SNR) - updated during channel estimation
    float estimated_noise_var = 0.1f;

    std::vector<Complex> sync_sequence;
    float sync_threshold = 0.7f;  // Threshold for sync detection

    // CFO correction (set by external chirp detection)
    float cfo_hz = 0.0f;

    Impl(const OTFSConfig& cfg)
        : config(cfg)
        , fft(cfg.fft_size)
        , mixer(cfg.center_freq, cfg.sample_rate)
    {
        generateSyncSequence();
        tf_buffer.resize(cfg.M * cfg.N);
        tf_equalized.resize(cfg.M * cfg.N);
        channel_est.resize(cfg.M, Complex(1, 0));  // Initialize to unity
    }

    void generateSyncSequence() {
        size_t N = config.M;
        sync_sequence.resize(N);
        for (size_t n = 0; n < N; ++n) {
            float phase = -M_PI * n * (n + 1) / N;
            sync_sequence[n] = Complex(std::cos(phase), std::sin(phase));
        }
    }

    std::vector<Complex> toBaseband(const float* samples, size_t count, size_t sample_offset = 0) {
        std::vector<Complex> baseband(count);
        NCO temp_mixer(config.center_freq, config.sample_rate);
        for (size_t i = 0; i < sample_offset; ++i) temp_mixer.next();

        for (size_t i = 0; i < count; ++i) {
            Complex carrier = temp_mixer.next();
            baseband[i] = Complex(samples[i], 0) * std::conj(carrier);
        }
        return baseband;
    }

    // Baseband conversion with CFO correction (for processPresynced)
    std::vector<Complex> toBasebandWithCFO(const float* samples, size_t count, size_t sample_offset = 0) {
        std::vector<Complex> baseband(count);

        // Combined frequency: center_freq + cfo
        float total_freq = config.center_freq + cfo_hz;
        NCO temp_mixer(total_freq, config.sample_rate);

        // Advance phase to account for preamble samples
        for (size_t i = 0; i < sample_offset; ++i) temp_mixer.next();

        for (size_t i = 0; i < count; ++i) {
            Complex carrier = temp_mixer.next();
            baseband[i] = Complex(samples[i], 0) * std::conj(carrier);
        }
        return baseband;
    }

    // Channel estimation with CFO correction
    void estimateChannelFromPreambleWithCFO(const float* preamble_samples, size_t preamble_len) {
        size_t sym_len = config.fft_size + config.cp_length;
        size_t num_symbols = preamble_len / sym_len;

        if (num_symbols < 1) return;

        // Average channel estimates from preamble symbols
        std::vector<Complex> h_sum(config.M, Complex(0, 0));
        float total_noise_power = 0;
        int noise_samples = 0;

        for (size_t sym = 0; sym < num_symbols; ++sym) {
            size_t sym_offset = sym * sym_len;
            auto baseband = toBasebandWithCFO(preamble_samples + sym_offset, sym_len, sym_offset);

            std::vector<Complex> time_domain(baseband.begin() + config.cp_length,
                                              baseband.begin() + config.cp_length + config.fft_size);

            std::vector<Complex> freq_domain;
            fft.forward(time_domain, freq_domain);

            for (size_t m = 0; m < config.M; ++m) {
                size_t idx = m + 1;
                if (idx < config.fft_size / 2) {
                    Complex received = freq_domain[idx] * REAL_TO_COMPLEX_SCALE;
                    Complex expected = sync_sequence[m];

                    float expected_mag_sq = std::norm(expected);
                    if (expected_mag_sq > 0.01f) {
                        Complex h = received * std::conj(expected) / expected_mag_sq;
                        h_sum[m] += h;

                        // Estimate noise from last symbol
                        if (sym == num_symbols - 1) {
                            Complex error = received - h * expected;
                            total_noise_power += std::norm(error);
                            noise_samples++;
                        }
                    }
                }
            }
        }

        // Average the estimates
        channel_est.resize(config.M);
        for (size_t m = 0; m < config.M; ++m) {
            channel_est[m] = h_sum[m] / static_cast<float>(num_symbols);
            if (std::norm(channel_est[m]) < 0.01f) {
                channel_est[m] = Complex(1, 0);
            }
        }

        // Update noise variance
        if (noise_samples > 0) {
            estimated_noise_var = total_noise_power / noise_samples;
            estimated_noise_var = std::max(0.001f, std::min(1.0f, estimated_noise_var));
        }

        channel_estimated = true;
    }

    bool detectSyncReal(const float* samples, size_t count) {
        size_t sym_len = config.fft_size + config.cp_length;
        if (count < 2 * sym_len) return false;

        float P = 0, R = 0;
        for (size_t i = 0; i < sym_len; ++i) {
            P += samples[i] * samples[i + sym_len];
            R += samples[i + sym_len] * samples[i + sym_len];
        }

        // Minimum energy check to reject false positives on silence
        float avg_energy = R / sym_len;
        if (avg_energy < 1e-6f) return false;

        float metric = std::abs(P) / (R + 1e-10f);
        return metric > sync_threshold;
    }

    // Fine synchronization using cross-correlation with known preamble
    // The preamble has 4 identical OFDM symbols, so Schmidl-Cox gives flat response
    // Instead, find where the correlation metric first exceeds a high threshold
    size_t fineSyncPreamble(const float* samples, size_t count, size_t search_range) {
        size_t sym_len = config.fft_size + config.cp_length;
        if (count < 2 * sym_len + search_range) return 0;

        // Find where metric transitions from rising to stable (preamble start)
        // The metric rises linearly as we enter the preamble
        // Find the first position where metric > 0.98 (nearly perfect)
        for (size_t offset = 0; offset < search_range; ++offset) {
            float P = 0, R = 0;
            for (size_t i = 0; i < sym_len; ++i) {
                P += samples[offset + i] * samples[offset + i + sym_len];
                R += samples[offset + i + sym_len] * samples[offset + i + sym_len];
            }

            float metric = std::abs(P) / (R + 1e-10f);
            if (metric > 0.98f) {
                // Found the transition point - this is approximately where preamble starts
                // Actually we're sym_len samples into the preamble at this point
                // because we need both windows to be in the preamble for metric=1.0
                return offset;
            }
        }

        return 0;
    }

    void demodulateSymbol(const std::vector<Complex>& baseband, uint32_t symbol_idx) {
        size_t sym_start = config.cp_length;
        size_t sym_len = config.fft_size;

        std::vector<Complex> time_domain(baseband.begin() + sym_start,
                                          baseband.begin() + sym_start + sym_len);

        std::vector<Complex> freq_domain;
        fft.forward(time_domain, freq_domain);

        // Extract subcarriers and store in TF buffer
        for (size_t m = 0; m < config.M; ++m) {
            size_t idx = m + 1;  // Skip DC
            if (idx < config.fft_size / 2) {
                tf_buffer[symbol_idx * config.M + m] = freq_domain[idx] * REAL_TO_COMPLEX_SCALE;
            } else {
                tf_buffer[symbol_idx * config.M + m] = Complex(0, 0);
            }
        }
    }

    // Estimate channel from preamble (known Zadoff-Chu sequence)
    // NOTE: TX resets mixer at start of preamble, so we use offset 0 (not accumulated sample count)
    void estimateChannelFromPreamble(const float* preamble_samples, size_t preamble_len) {
        size_t sym_len = config.fft_size + config.cp_length;

        if (preamble_len < 4 * sym_len) return;

        // Average channel estimates from all 4 preamble symbols for robustness
        std::vector<Complex> h_sum(config.M, Complex(0, 0));
        float total_noise_power = 0;
        int noise_samples = 0;

        for (int sym = 0; sym < 4; ++sym) {
            size_t sym_offset = sym * sym_len;
            // TX mixer resets at preamble start, so use sym_offset only (not accumulated position)
            auto baseband = toBaseband(preamble_samples + sym_offset, sym_len, sym_offset);

            std::vector<Complex> time_domain(baseband.begin() + config.cp_length,
                                              baseband.begin() + config.cp_length + config.fft_size);

            std::vector<Complex> freq_domain;
            fft.forward(time_domain, freq_domain);

            for (size_t m = 0; m < config.M; ++m) {
                size_t idx = m + 1;
                if (idx < config.fft_size / 2) {
                    Complex received = freq_domain[idx] * REAL_TO_COMPLEX_SCALE;
                    Complex expected = sync_sequence[m];

                    float expected_mag_sq = std::norm(expected);
                    if (expected_mag_sq > 0.01f) {
                        Complex h = received * std::conj(expected) / expected_mag_sq;
                        h_sum[m] += h;

                        // Estimate noise from last symbol only (to avoid correlation with channel est)
                        if (sym == 3) {
                            Complex error = received - h * expected;
                            total_noise_power += std::norm(error);
                            noise_samples++;
                        }
                    }
                }
            }
        }

        // Average the estimates
        channel_est.resize(config.M);
        for (size_t m = 0; m < config.M; ++m) {
            channel_est[m] = h_sum[m] / 4.0f;
            if (std::norm(channel_est[m]) < 0.01f) {
                channel_est[m] = Complex(1, 0);
            }
        }

        // Update noise variance from preamble error (single symbol for speed)
        if (noise_samples > 0) {
            estimated_noise_var = total_noise_power / noise_samples;
            // Clamp to reasonable range to avoid numerical issues
            estimated_noise_var = std::max(0.001f, std::min(1.0f, estimated_noise_var));
        }

        channel_estimated = true;
    }

    // Equalize TF grid using preamble-based channel estimate
    void equalizeTFGrid() {
        if (!channel_estimated) {
            tf_equalized = tf_buffer;
            return;
        }

        // Apply channel estimate to all OFDM symbols using zero-forcing
        for (uint32_t n = 0; n < config.N; ++n) {
            for (uint32_t m = 0; m < config.M; ++m) {
                Complex received = tf_buffer[n * config.M + m];
                Complex h = channel_est[m];

                float h_mag_sq = std::norm(h);
                if (h_mag_sq > 0.01f) {
                    tf_equalized[n * config.M + m] = received * std::conj(h) / h_mag_sq;
                } else {
                    tf_equalized[n * config.M + m] = received;
                }
            }
        }
    }
};

OTFSDemodulator::OTFSDemodulator(const OTFSConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

OTFSDemodulator::~OTFSDemodulator() = default;

bool OTFSDemodulator::process(SampleSpan samples) {
    impl_->sample_buffer.insert(impl_->sample_buffer.end(),
                                samples.data(), samples.data() + samples.size());

    size_t sym_len = impl_->config.fft_size + impl_->config.cp_length;
    size_t preamble_len = 4 * sym_len;

    while (true) {
        if (impl_->state == Impl::State::SEARCHING) {
            if (impl_->sample_buffer.size() < 2 * sym_len) return false;

            if (impl_->detectSyncReal(impl_->sample_buffer.data(), 2 * sym_len)) {
                // Coarse sync detected - now do fine sync to find exact preamble start
                size_t search_range = sym_len / 4;  // Search within one slide distance
                size_t fine_offset = 0;

                if (impl_->sample_buffer.size() >= preamble_len + search_range) {
                    fine_offset = impl_->fineSyncPreamble(
                        impl_->sample_buffer.data(), impl_->sample_buffer.size(), search_range);

                    // Remove samples before the fine-tuned preamble start
                    if (fine_offset > 0) {
                        impl_->total_samples_processed += fine_offset;
                        impl_->sample_buffer.erase(impl_->sample_buffer.begin(),
                                                   impl_->sample_buffer.begin() + fine_offset);
                    }
                }

                if (impl_->sample_buffer.size() >= preamble_len) {
                    // Estimate channel from preamble BEFORE removing it
                    // TX resets mixer at preamble start, so no absolute offset needed
                    if (impl_->config.tf_equalization) {
                        impl_->estimateChannelFromPreamble(
                            impl_->sample_buffer.data(), preamble_len);
                    }

                    impl_->total_samples_processed += preamble_len;
                    impl_->current_frame_start = impl_->total_samples_processed;
                    impl_->sample_buffer.erase(impl_->sample_buffer.begin(),
                                               impl_->sample_buffer.begin() + preamble_len);
                    impl_->state = Impl::State::SYNCED;
                    impl_->symbols_received = 0;
                } else {
                    return false;
                }
            } else {
                size_t slide = sym_len / 4;
                if (impl_->sample_buffer.size() > slide) {
                    impl_->total_samples_processed += slide;
                    impl_->sample_buffer.erase(impl_->sample_buffer.begin(),
                                               impl_->sample_buffer.begin() + slide);
                } else {
                    return false;
                }
            }
        }
        else if (impl_->state == Impl::State::SYNCED) {
            if (impl_->sample_buffer.size() < sym_len) return false;

            // TX resets mixer at start of data section, so offset is just symbols_received * sym_len
            // (not including any accumulated lead-in or preamble offset)
            size_t symbol_offset = impl_->symbols_received * sym_len;
            auto baseband = impl_->toBaseband(impl_->sample_buffer.data(), sym_len,
                                               symbol_offset);
            impl_->demodulateSymbol(baseband, impl_->symbols_received);
            impl_->symbols_received++;

            impl_->sample_buffer.erase(impl_->sample_buffer.begin(),
                                       impl_->sample_buffer.begin() + sym_len);

            if (impl_->symbols_received >= impl_->config.N) {
                impl_->state = Impl::State::FRAME_READY;
            }
        }
        else if (impl_->state == Impl::State::FRAME_READY) {
            // Step 1: TF equalization (user configurable)
            // tf_equalization=true: Better for stable channels (Good), uses preamble estimate
            // tf_equalization=false: Better for challenging channels (Poor), leverages OTFS diversity
            bool use_tf_eq = impl_->config.tf_equalization && impl_->channel_estimated;

            if (use_tf_eq) {
                impl_->equalizeTFGrid();
            } else {
                impl_->tf_equalized = impl_->tf_buffer;
            }

            // Step 2: SFFT to get DD symbols
            sfft(impl_->tf_equalized, impl_->dd_symbols, impl_->config.M, impl_->config.N);

            // Step 3: Normalize DD symbols to unit power
            // The SFFT spreads energy, so we need to rescale to expected constellation magnitude
            float avg_power = 0;
            size_t nonzero_count = 0;
            for (const auto& sym : impl_->dd_symbols) {
                float p = std::norm(sym);
                if (p > 1e-8f) {
                    avg_power += p;
                    nonzero_count++;
                }
            }

            float scale = 1.0f;
            if (nonzero_count > 0) {
                avg_power /= nonzero_count;
                if (avg_power > 1e-6f) {
                    scale = 1.0f / std::sqrt(avg_power);
                    for (auto& sym : impl_->dd_symbols) {
                        sym *= scale;
                    }
                }
            }

            // Step 3b: Use a fixed noise variance for consistent LLR scaling
            // After normalization, symbols have unit power, so use a fixed noise variance
            // that gives good LDPC performance across typical SNR range (15-35 dB)
            // Noise variance of 0.1 corresponds to ~10 dB SNR, which gives reasonable LLR range
            impl_->estimated_noise_var = 0.1f;

            // Step 4: Generate soft bits from DD symbols
            impl_->soft_bits.clear();

            if (impl_->config.dd_differential) {
                // Differential decoding: compute phase differences between adjacent symbols
                // diff[i] = dd_symbols[i] * conj(dd_symbols[i-1])
                Complex prev_symbol(1.0f, 0.0f);  // Reference (matches TX)

                for (size_t i = 0; i < impl_->dd_symbols.size(); ++i) {
                    Complex current = impl_->dd_symbols[i];
                    Complex diff = current * std::conj(prev_symbol);

                    // Normalize diff to unit magnitude (removes amplitude variation)
                    float mag = std::abs(diff);
                    if (mag > 0.01f) {
                        diff /= mag;
                    }

                    softDemapDQPSK(diff, impl_->estimated_noise_var, impl_->soft_bits);
                    prev_symbol = current;
                }
            } else {
                // Coherent demapping (original behavior)
                for (const auto& sym : impl_->dd_symbols) {
                    softDemap(sym, impl_->config.modulation, impl_->estimated_noise_var, impl_->soft_bits);
                }
            }

            impl_->state = Impl::State::SEARCHING;
            impl_->symbols_received = 0;
            impl_->channel_estimated = false;  // Reset for next frame
            return true;
        }
    }

    return false;
}

std::vector<Complex> OTFSDemodulator::getDDSymbols() {
    return impl_->dd_symbols;
}

std::vector<float> OTFSDemodulator::getSoftBits() {
    return impl_->soft_bits;
}

std::vector<Complex> OTFSDemodulator::getDDChannel() {
    // Return channel estimate (currently in TF domain)
    return impl_->channel_est;
}

void OTFSDemodulator::reset() {
    impl_->state = Impl::State::SEARCHING;
    impl_->sample_buffer.clear();
    impl_->symbols_received = 0;
    impl_->total_samples_processed = 0;
    impl_->current_frame_start = 0;
    impl_->dd_symbols.clear();
    impl_->soft_bits.clear();
    impl_->estimated_noise_var = 0.1f;
    impl_->channel_estimated = false;
    impl_->mixer.reset();
}

bool OTFSDemodulator::isSynced() const {
    return impl_->state == Impl::State::SYNCED;
}

void OTFSDemodulator::setFrequencyOffset(float cfo_hz) {
    // Store CFO for frequency correction during baseband conversion
    impl_->cfo_hz = cfo_hz;
}

float OTFSDemodulator::getEstimatedSNR() const {
    // Estimate SNR from noise variance: SNR = signal_power / noise_power
    // After normalization, signal power is ~1.0
    if (impl_->estimated_noise_var > 1e-6f) {
        float snr_linear = 1.0f / impl_->estimated_noise_var;
        return 10.0f * std::log10(snr_linear);
    }
    return 20.0f;  // Default ~20 dB if no estimate
}

bool OTFSDemodulator::processPresynced(SampleSpan samples, int preamble_symbols) {
    // Process samples after external sync (chirp preamble)
    //
    // Expected input: samples starting at OTFS preamble position
    // Structure: [OTFS_PREAMBLE (4 symbols)] [DATA (N symbols)]
    //
    // The OTFS preamble contains 4 repeated Zadoff-Chu symbols for channel estimation.
    // After channel estimation, we process N data symbols.

    const size_t sym_len = impl_->config.fft_size + impl_->config.cp_length;
    const size_t preamble_len = preamble_symbols * sym_len;
    const size_t data_len = impl_->config.N * sym_len;
    const size_t total_needed = preamble_len + data_len;

    if (samples.size() < total_needed) {
        return false;  // Not enough samples
    }

    // Reset state
    impl_->soft_bits.clear();
    impl_->dd_symbols.clear();
    impl_->mixer.reset();
    impl_->symbols_received = 0;
    impl_->channel_estimated = false;
    std::fill(impl_->channel_est.begin(), impl_->channel_est.end(), Complex(1, 0));

    const float* ptr = samples.data();

    // === PHASE 1: Channel estimation from preamble ===
    if (preamble_symbols > 0 && impl_->config.tf_equalization) {
        impl_->estimateChannelFromPreambleWithCFO(ptr, preamble_len);
    }

    // Skip past preamble to data section
    ptr += preamble_len;

    // === PHASE 2: Demodulate data symbols ===
    // Process N data OFDM symbols into TF grid
    // IMPORTANT: symbol_offset must account for preamble samples to maintain phase continuity
    for (uint32_t n = 0; n < impl_->config.N; ++n) {
        size_t symbol_offset = preamble_len + n * sym_len;  // Continue phase from after preamble
        auto baseband = impl_->toBasebandWithCFO(ptr + n * sym_len, sym_len, symbol_offset);
        impl_->demodulateSymbol(baseband, n);
    }

    // === PHASE 3: TF equalization (optional, may help with coarse correction) ===
    if (impl_->config.tf_equalization && impl_->channel_estimated) {
        impl_->equalizeTFGrid();
    } else {
        impl_->tf_equalized = impl_->tf_buffer;
    }

    // === PHASE 4: SFFT to get DD symbols ===
    sfft(impl_->tf_equalized, impl_->dd_symbols, impl_->config.M, impl_->config.N);

    // === PHASE 5: DD-domain channel estimation and equalization ===
    // Estimate channel from DD pilot region, perform matched-filter style equalization
    if (impl_->config.dd_pilot_enable) {
        uint32_t guard_k = impl_->config.dd_pilot_guard_delay;
        uint32_t guard_l = impl_->config.dd_pilot_guard_doppler;

        // Find the dominant channel tap in the pilot region
        // The pilot region contains the DD channel impulse response (scaled by pilot=2.0)
        Complex h_sum(0, 0);
        float max_power = 0;
        Complex h_dominant(0, 0);

        for (uint32_t k = 0; k < guard_k && k < impl_->config.M; ++k) {
            for (uint32_t l = 0; l < guard_l && l < impl_->config.N; ++l) {
                size_t idx = k * impl_->config.N + l;
                Complex h_tap = impl_->dd_symbols[idx] / Complex(2.0f, 0.0f);
                float tap_power = std::norm(h_tap);

                h_sum += h_tap;  // Coherent sum (matched filter)

                if (tap_power > max_power) {
                    max_power = tap_power;
                    h_dominant = h_tap;
                }
            }
        }

        // Use coherent sum for equalization (acts as matched filter)
        float h_sum_mag = std::abs(h_sum);
        if (h_sum_mag > 0.01f) {
            // Matched-filter style equalization: conjugate of channel sum, normalized
            Complex h_eq = std::conj(h_sum) / (h_sum_mag * h_sum_mag);

            // Apply to all symbols outside pilot/guard region
            for (uint32_t k = 0; k < impl_->config.M; ++k) {
                for (uint32_t l = 0; l < impl_->config.N; ++l) {
                    if (k < guard_k && l < guard_l) continue;

                    size_t idx = k * impl_->config.N + l;
                    impl_->dd_symbols[idx] *= h_eq;
                }
            }
        }

        // Zero out pilot/guard region (don't decode as data)
        for (uint32_t k = 0; k < guard_k && k < impl_->config.M; ++k) {
            for (uint32_t l = 0; l < guard_l && l < impl_->config.N; ++l) {
                size_t idx = k * impl_->config.N + l;
                impl_->dd_symbols[idx] = Complex(0, 0);
            }
        }
    }

    // === PHASE 5b: Normalize DD data symbols to unit power ===
    float avg_power = 0;
    size_t nonzero_count = 0;
    uint32_t guard_k = impl_->config.dd_pilot_enable ? impl_->config.dd_pilot_guard_delay : 0;
    uint32_t guard_l = impl_->config.dd_pilot_enable ? impl_->config.dd_pilot_guard_doppler : 0;

    for (uint32_t k = 0; k < impl_->config.M; ++k) {
        for (uint32_t l = 0; l < impl_->config.N; ++l) {
            if (k < guard_k && l < guard_l) continue;  // Skip pilot region
            size_t idx = k * impl_->config.N + l;
            float p = std::norm(impl_->dd_symbols[idx]);
            if (p > 1e-8f) {
                avg_power += p;
                nonzero_count++;
            }
        }
    }

    if (nonzero_count > 0) {
        avg_power /= nonzero_count;
        if (avg_power > 1e-6f) {
            float scale = 1.0f / std::sqrt(avg_power);
            for (auto& sym : impl_->dd_symbols) {
                sym *= scale;
            }
        }
    }

    // Fixed noise variance for consistent LLR scaling
    impl_->estimated_noise_var = 0.1f;

    // === PHASE 6: Generate soft bits (skip pilot/guard region) ===
    impl_->soft_bits.clear();

    for (uint32_t k = 0; k < impl_->config.M; ++k) {
        for (uint32_t l = 0; l < impl_->config.N; ++l) {
            // Skip pilot and guard region
            if (impl_->config.dd_pilot_enable && k < guard_k && l < guard_l) {
                continue;
            }

            size_t idx = k * impl_->config.N + l;
            const Complex& sym = impl_->dd_symbols[idx];
            softDemap(sym, impl_->config.modulation, impl_->estimated_noise_var, impl_->soft_bits);
        }
    }

    return impl_->soft_bits.size() > 0;
}

} // namespace ultra
