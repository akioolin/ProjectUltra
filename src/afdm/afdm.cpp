// afdm.cpp - AFDM Modulator and Demodulator implementation

#define _USE_MATH_DEFINES
#include "afdm.hpp"
#include "ultra/dsp.hpp"  // For HilbertTransform
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <numeric>
#include <string>

namespace ultra {
namespace afdm {

namespace {

constexpr float kAudioC1Epsilon = 1e-6f;

bool usesAudioIncompatibleC1(const AFDMConfig& config) {
    return std::abs(config.c1) > kAudioC1Epsilon;
}

void validateAudioChainConfig(const AFDMConfig& config, const char* path_name) {
    if (usesAudioIncompatibleC1(config)) {
        throw std::runtime_error(
            std::string("AFDM ") + path_name +
            " audio path requires c1=0. c1>0 needs complex I/Q baseband."
        );
    }
}

}  // namespace

// ============================================================================
// Symbol Mapping Utilities
// ============================================================================

namespace mapping {

// DQPSK constellation (Gray coded)
static const Complex DQPSK_CONSTELLATION[4] = {
    Complex( 0.7071f,  0.7071f),  // 00: +45°
    Complex(-0.7071f,  0.7071f),  // 01: +135°
    Complex(-0.7071f, -0.7071f),  // 11: -135°
    Complex( 0.7071f, -0.7071f),  // 10: -45°
};

// QPSK constellation (same as DQPSK points)
static const Complex QPSK_CONSTELLATION[4] = {
    Complex( 0.7071f,  0.7071f),  // 00
    Complex(-0.7071f,  0.7071f),  // 01
    Complex(-0.7071f, -0.7071f),  // 11
    Complex( 0.7071f, -0.7071f),  // 10
};

// BPSK constellation
static const Complex BPSK_CONSTELLATION[2] = {
    Complex( 1.0f, 0.0f),  // 0
    Complex(-1.0f, 0.0f),  // 1
};

Complex mapSymbol(uint32_t bits, Modulation mod) {
    switch (mod) {
        case Modulation::BPSK:
        case Modulation::DBPSK:
            return BPSK_CONSTELLATION[bits & 0x01];

        case Modulation::QPSK:
        case Modulation::DQPSK: {
            // Apply Gray encoding: constellation is stored in Gray code order
            // bits=0 (00) → index 0, bits=1 (01) → index 1
            // bits=2 (10) → index 3, bits=3 (11) → index 2
            uint32_t gray_index = (bits & 0x03) ^ ((bits & 0x03) >> 1);
            return QPSK_CONSTELLATION[gray_index];
        }

        case Modulation::QAM16: {
            // 16-QAM: 4 bits -> 4x4 grid
            int i = (bits & 0x03);       // 2 LSBs for I
            int q = ((bits >> 2) & 0x03); // 2 MSBs for Q
            float scale = 1.0f / std::sqrt(10.0f);  // Normalize power
            return Complex((2*i - 3) * scale, (2*q - 3) * scale);
        }

        default:
            return QPSK_CONSTELLATION[bits & 0x03];
    }
}

uint32_t demapHard(Complex symbol, Modulation mod) {
    switch (mod) {
        case Modulation::BPSK:
        case Modulation::DBPSK:
            return (symbol.real() < 0) ? 1 : 0;

        case Modulation::QPSK:
        case Modulation::DQPSK: {
            // Quadrant detection: directly gives the original bit pattern
            // (+,+) → 00, (-,+) → 01, (+,-) → 10, (-,-) → 11
            uint32_t bits = 0;
            if (symbol.real() < 0) bits |= 0x01;
            if (symbol.imag() < 0) bits |= 0x02;
            return bits;  // No Gray decoding needed with Gray-encoded mapSymbol
        }

        case Modulation::QAM16: {
            float scale = std::sqrt(10.0f);
            int i = static_cast<int>(std::round((symbol.real() * scale + 3) / 2));
            int q = static_cast<int>(std::round((symbol.imag() * scale + 3) / 2));
            i = std::clamp(i, 0, 3);
            q = std::clamp(q, 0, 3);
            return (q << 2) | i;
        }

        default:
            return demapHard(symbol, Modulation::QPSK);
    }
}

std::vector<float> demapSoft(Complex symbol, Modulation mod, float noise_var) {
    uint32_t bits_per_sym = getBitsPerSymbol(mod);
    std::vector<float> llrs(bits_per_sym);

    if (noise_var < 1e-10f) noise_var = 1e-10f;

    switch (mod) {
        case Modulation::BPSK:
        case Modulation::DBPSK:
            // LLR = 2 * real(symbol) / noise_var
            llrs[0] = 2.0f * symbol.real() / noise_var;
            break;

        case Modulation::QPSK:
        case Modulation::DQPSK:
            // Approximate LLR for Gray-coded QPSK
            llrs[0] = 2.0f * symbol.real() / noise_var;
            llrs[1] = 2.0f * symbol.imag() / noise_var;
            break;

        case Modulation::QAM16: {
            // Approximate LLR for 16-QAM
            float scale = std::sqrt(10.0f);
            float x = symbol.real() * scale;
            float y = symbol.imag() * scale;

            // Simplified LLR calculation
            llrs[0] = 2.0f * x / noise_var;
            llrs[1] = 2.0f * (2.0f - std::abs(x)) / noise_var;
            llrs[2] = 2.0f * y / noise_var;
            llrs[3] = 2.0f * (2.0f - std::abs(y)) / noise_var;
            break;
        }

        default:
            return demapSoft(symbol, Modulation::QPSK, noise_var);
    }

    return llrs;
}

std::vector<Complex> getConstellation(Modulation mod) {
    switch (mod) {
        case Modulation::BPSK:
        case Modulation::DBPSK:
            return {BPSK_CONSTELLATION[0], BPSK_CONSTELLATION[1]};

        case Modulation::QPSK:
        case Modulation::DQPSK:
            return {QPSK_CONSTELLATION[0], QPSK_CONSTELLATION[1],
                    QPSK_CONSTELLATION[2], QPSK_CONSTELLATION[3]};

        default:
            return getConstellation(Modulation::QPSK);
    }
}

} // namespace mapping

// ============================================================================
// AFDM Modulator
// ============================================================================

AFDMModulator::AFDMModulator(const AFDMConfig& config)
    : config_(config)
    , daft_processor_(config.N, config.c1, config.c2)
{
    initPilotSequence();
}

void AFDMModulator::configure(const AFDMConfig& config) {
    config_ = config;
    daft_processor_.configure(config.N, config.c1, config.c2);
    initPilotSequence();
}

void AFDMModulator::initPilotSequence() {
    // Get active carrier indices (centered around DC, like OFDM)
    active_carriers_ = config_.getActiveCarrierIndices();

    // Determine pilot and data positions within active carriers
    pilot_indices_.clear();
    data_indices_.clear();

    for (size_t i = 0; i < active_carriers_.size(); ++i) {
        if (i % config_.pilot_spacing == 0) {
            pilot_indices_.push_back(active_carriers_[i]);
        } else {
            data_indices_.push_back(active_carriers_[i]);
        }
    }

    // Generate BPSK pilot sequence (pseudo-random but deterministic)
    pilot_sequence_.resize(pilot_indices_.size());
    for (size_t i = 0; i < pilot_sequence_.size(); ++i) {
        float sign = ((i * 7 + 3) % 4 < 2) ? 1.0f : -1.0f;
        pilot_sequence_[i] = Complex(sign, 0.0f);
    }
}

std::vector<Complex> AFDMModulator::mapToSymbols(ByteSpan data, Modulation mod) {
    uint32_t bits_per_sym = getBitsPerSymbol(mod);
    size_t num_symbols = (data.size() * 8 + bits_per_sym - 1) / bits_per_sym;

    std::vector<Complex> symbols;
    symbols.reserve(num_symbols);

    size_t bit_idx = 0;
    for (size_t i = 0; i < num_symbols; ++i) {
        uint32_t bits = 0;
        for (uint32_t b = 0; b < bits_per_sym; ++b) {
            size_t byte_idx = bit_idx / 8;
            size_t bit_in_byte = 7 - (bit_idx % 8);  // MSB first

            if (byte_idx < data.size()) {
                bits |= ((data[byte_idx] >> bit_in_byte) & 0x01) << b;
            }
            bit_idx++;
        }
        symbols.push_back(mapping::mapSymbol(bits, mod));
    }

    return symbols;
}

std::vector<Complex> AFDMModulator::insertPilots(const std::vector<Complex>& data_symbols) {
    // Create frame with all bins zero initially
    std::vector<Complex> frame(config_.N, Complex(0, 0));

    // Insert pilots at their positions
    for (size_t i = 0; i < pilot_indices_.size(); ++i) {
        frame[pilot_indices_[i]] = pilot_sequence_[i];
    }

    // Insert data at data positions
    size_t data_idx = 0;
    for (size_t i = 0; i < data_indices_.size() && data_idx < data_symbols.size(); ++i) {
        frame[data_indices_[i]] = data_symbols[data_idx++];
    }

    return frame;
}

std::vector<Complex> AFDMModulator::generateSymbol(const std::vector<Complex>& daft_bins) {
    // IDAFT: DAFT-domain → time-domain
    auto time_samples = daft_processor_.inverse(daft_bins);

    // Add chirp-periodic prefix (CPP)
    // CPP is last cpp_length samples prepended
    std::vector<Complex> symbol;
    symbol.reserve(config_.N + config_.cpp_length);

    // Prepend CPP (last cpp_length samples)
    for (size_t i = 0; i < config_.cpp_length; ++i) {
        size_t src_idx = config_.N - config_.cpp_length + i;
        symbol.push_back(time_samples[src_idx]);
    }

    // Append main symbol
    symbol.insert(symbol.end(), time_samples.begin(), time_samples.end());

    return symbol;
}

Samples AFDMModulator::upmix(const std::vector<Complex>& baseband) {
    Samples audio(baseband.size());

    // Use carrier frequency that completes an integer number of cycles per symbol
    // This ensures the carrier phase is aligned at symbol boundaries.
    // For N=64 samples at 48kHz, we want fc such that fc*N/fs = integer
    // fc = integer * fs / N = integer * 750 Hz
    // Using 1500 Hz = 2 * 750 Hz gives 2 cycles per symbol
    const float two_pi_fc = 2.0f * static_cast<float>(M_PI) * config_.center_freq / config_.sample_rate;

    for (size_t i = 0; i < baseband.size(); ++i) {
        float phase = two_pi_fc * i;
        // Real output: Re{baseband * exp(j*2*pi*fc*t)}
        audio[i] = baseband[i].real() * std::cos(phase) - baseband[i].imag() * std::sin(phase);
    }

    return audio;
}

Samples AFDMModulator::modulate(ByteSpan data, Modulation mod) {
    validateAudioChainConfig(config_, "TX");

    // Map data to symbols
    auto data_symbols = mapToSymbols(data, mod);

    // Build frame symbol by symbol
    std::vector<Complex> baseband;
    baseband.reserve(config_.samplesPerFrame());

    size_t symbols_needed = config_.symbols_per_frame;
    size_t data_per_symbol = config_.dataSubcarriers();
    size_t sym_offset = 0;

    for (size_t sym = 0; sym < symbols_needed; ++sym) {
        // Extract data for this symbol
        std::vector<Complex> sym_data;
        for (size_t i = 0; i < data_per_symbol && sym_offset + i < data_symbols.size(); ++i) {
            sym_data.push_back(data_symbols[sym_offset + i]);
        }
        sym_offset += data_per_symbol;

        // Insert pilots
        auto daft_frame = insertPilots(sym_data);

        // Generate time-domain symbol
        auto symbol = generateSymbol(daft_frame);

        // Append to baseband
        baseband.insert(baseband.end(), symbol.begin(), symbol.end());
    }

    // Upmix to audio
    return upmix(baseband);
}

Samples AFDMModulator::generatePreamble() {
    // Generate dual-chirp preamble compatible with ChirpSync
    // Chirp parameters: sweep from center_freq - bw/2 to center_freq + bw/2

    const float duration = 0.05f;  // 50ms per chirp
    const size_t chirp_samples = static_cast<size_t>(duration * config_.sample_rate);
    const float f_start = config_.center_freq - config_.bandwidth / 2;
    const float f_end = config_.center_freq + config_.bandwidth / 2;

    Samples preamble;
    preamble.reserve(chirp_samples * 2 + 480);  // 2 chirps + gap

    // First chirp (up-sweep)
    for (size_t i = 0; i < chirp_samples; ++i) {
        float t = static_cast<float>(i) / config_.sample_rate;
        float freq = f_start + (f_end - f_start) * t / duration;
        float phase = 2.0f * static_cast<float>(M_PI) * (f_start * t + 0.5f * (f_end - f_start) * t * t / duration);
        preamble.push_back(0.8f * std::sin(phase));
    }

    // Small gap (10ms)
    for (size_t i = 0; i < 480; ++i) {
        preamble.push_back(0.0f);
    }

    // Second chirp (up-sweep, same as first for correlation)
    for (size_t i = 0; i < chirp_samples; ++i) {
        float t = static_cast<float>(i) / config_.sample_rate;
        float freq = f_start + (f_end - f_start) * t / duration;
        float phase = 2.0f * static_cast<float>(M_PI) * (f_start * t + 0.5f * (f_end - f_start) * t * t / duration);
        preamble.push_back(0.8f * std::sin(phase));
    }

    return preamble;
}

Samples AFDMModulator::transmitFrame(ByteSpan data, Modulation mod) {
    auto preamble = generatePreamble();
    auto frame = modulate(data, mod);

    Samples output;
    output.reserve(preamble.size() + frame.size());
    output.insert(output.end(), preamble.begin(), preamble.end());
    output.insert(output.end(), frame.begin(), frame.end());

    return output;
}

size_t AFDMModulator::dataSymbolsPerFrame() const {
    return config_.dataSubcarriers() * config_.symbols_per_frame;
}

size_t AFDMModulator::bitsPerFrame(Modulation mod) const {
    return dataSymbolsPerFrame() * getBitsPerSymbol(mod);
}

size_t AFDMModulator::bytesPerFrame(Modulation mod) const {
    return bitsPerFrame(mod) / 8;
}

size_t AFDMModulator::samplesPerFrame() const {
    return config_.samplesPerFrame();
}

size_t AFDMModulator::preambleSamples() const {
    // 2 chirps of 50ms + 10ms gap
    return static_cast<size_t>(0.11f * config_.sample_rate);
}

// ============================================================================
// AFDM Demodulator
// ============================================================================

AFDMDemodulator::AFDMDemodulator(const AFDMConfig& config)
    : config_(config)
    , daft_processor_(config.N, config.c1, config.c2)
{
    initPilotSequence();
    computePilotIndices();
    channel_estimate_.resize(config.N, Complex(1, 0));
}

void AFDMDemodulator::configure(const AFDMConfig& config) {
    config_ = config;
    daft_processor_.configure(config.N, config.c1, config.c2);
    initPilotSequence();
    computePilotIndices();
    channel_estimate_.resize(config.N, Complex(1, 0));
}

void AFDMDemodulator::initPilotSequence() {
    // Get active carrier indices (must match modulator)
    active_carriers_ = config_.getActiveCarrierIndices();

    // Determine pilot and data positions within active carriers
    pilot_indices_.clear();
    data_indices_.clear();

    for (size_t i = 0; i < active_carriers_.size(); ++i) {
        if (i % config_.pilot_spacing == 0) {
            pilot_indices_.push_back(active_carriers_[i]);
        } else {
            data_indices_.push_back(active_carriers_[i]);
        }
    }

    // Generate BPSK pilot sequence (must match modulator)
    pilot_sequence_.resize(pilot_indices_.size());
    for (size_t i = 0; i < pilot_sequence_.size(); ++i) {
        float sign = ((i * 7 + 3) % 4 < 2) ? 1.0f : -1.0f;
        pilot_sequence_[i] = Complex(sign, 0.0f);
    }
}

void AFDMDemodulator::computePilotIndices() {
    // Now handled by initPilotSequence - this is kept for compatibility
    // but does nothing as indices are computed in initPilotSequence
}

std::vector<Complex> AFDMDemodulator::downmix(SampleSpan samples) {
    const float two_pi_fc = 2.0f * static_cast<float>(M_PI) * config_.center_freq / config_.sample_rate;

    // ========================================================================
    // DSB downmix
    //
    // After mixing with exp(-jωt), we get: baseband + image_at_2fc
    //
    // For OFDM mode (c1=0): The FFT naturally separates the signal (bins around
    // DC) from the 2fc image (at bin ~32 for fc=1500Hz). No filtering needed!
    // The 2fc component lands in unused bins and doesn't affect demodulation.
    //
    // For AFDM mode (c1>0): The chirped signal spreads across frequencies,
    // overlapping with the 2fc image. This mode is NOT supported for audio
    // transmission - use complex baseband only.
    // ========================================================================

    std::vector<Complex> baseband(samples.size());
    for (size_t n = 0; n < samples.size(); ++n) {
        float phase = two_pi_fc * n;

        // Apply CFO correction
        if (std::abs(cfo_hz_) > 0.01f) {
            float cfo_phase = 2.0f * static_cast<float>(M_PI) * cfo_hz_ * n / config_.sample_rate;
            phase += cfo_phase;
        }

        // Downmix: multiply by 2*exp(-j*phase)
        // For OFDM (c1=0), the 2fc image at bin ~32 doesn't affect signal bins
        baseband[n] = Complex(
            2.0f * samples[n] * std::cos(phase),
            -2.0f * samples[n] * std::sin(phase)
        );
    }

    return baseband;
}

std::vector<Complex> AFDMDemodulator::extractSymbol(const std::vector<Complex>& baseband, size_t symbol_idx) {
    size_t symbol_start = symbol_idx * (config_.N + config_.cpp_length);

    // Skip CPP, extract N samples
    size_t data_start = symbol_start + config_.cpp_length;

    if (data_start + config_.N > baseband.size()) {
        return {};  // Not enough samples
    }

    std::vector<Complex> time_samples(baseband.begin() + data_start,
                                       baseband.begin() + data_start + config_.N);

    // DAFT: time-domain → DAFT-domain
    return daft_processor_.forward(time_samples);
}

void AFDMDemodulator::estimateChannel(const std::vector<Complex>& daft_symbols) {
    if (daft_symbols.size() != config_.N) return;

    // Estimate channel at pilot positions
    float signal_power = 0.0f;

    for (size_t i = 0; i < pilot_indices_.size(); ++i) {
        int idx = pilot_indices_[i];
        Complex rx_pilot = daft_symbols[idx];
        Complex tx_pilot = pilot_sequence_[i];

        // LS channel estimate: H = Y / X
        if (std::abs(tx_pilot) > 0.01f) {
            channel_estimate_[idx] = rx_pilot / tx_pilot;
        }

        signal_power += std::norm(rx_pilot);
    }

    // For narrowband AFDM with active carriers, just use nearest-neighbor interpolation
    // for data positions (simpler than linear, avoids FFT bin wrap issues)
    if (!pilot_indices_.empty()) {
        for (int data_idx : data_indices_) {
            // Find nearest pilot
            int nearest_pilot = pilot_indices_[0];
            int min_dist = std::abs(data_idx - nearest_pilot);

            for (int pilot_idx : pilot_indices_) {
                int dist = std::abs(data_idx - pilot_idx);
                // Also check wrap-around distance
                int wrap_dist = static_cast<int>(config_.N) - dist;
                int actual_dist = std::min(dist, wrap_dist);

                if (actual_dist < min_dist) {
                    min_dist = actual_dist;
                    nearest_pilot = pilot_idx;
                }
            }

            channel_estimate_[data_idx] = channel_estimate_[nearest_pilot];
        }
    }

    // Compute fading index only from active carrier positions
    std::vector<float> magnitudes;
    for (int idx : active_carriers_) {
        magnitudes.push_back(std::abs(channel_estimate_[idx]));
    }

    float sum = std::accumulate(magnitudes.begin(), magnitudes.end(), 0.0f);
    float mean = magnitudes.empty() ? 0.0f : sum / magnitudes.size();

    if (mean > 0.001f) {
        float var_sum = 0.0f;
        for (float m : magnitudes) {
            float diff = m - mean;
            var_sum += diff * diff;
        }
        float std_dev = std::sqrt(var_sum / magnitudes.size());
        fading_index_ = std_dev / mean;
    } else {
        fading_index_ = 0.0f;
    }

    // Estimate SNR
    float avg_channel_power = pilot_indices_.empty() ? 0.0f : signal_power / pilot_indices_.size();
    estimated_snr_db_ = avg_channel_power > 0.001f ? 10.0f * std::log10(avg_channel_power * 10.0f) : 30.0f;
}

std::vector<Complex> AFDMDemodulator::equalize(const std::vector<Complex>& daft_symbols) {
    std::vector<Complex> equalized(daft_symbols.size());

    // MMSE equalization
    float noise_var = std::pow(10.0f, -estimated_snr_db_ / 10.0f);

    for (size_t i = 0; i < daft_symbols.size(); ++i) {
        Complex h = channel_estimate_[i];
        float h_mag_sq = std::norm(h);

        if (h_mag_sq > 0.001f) {
            // MMSE: Y * conj(H) / (|H|^2 + noise_var)
            equalized[i] = daft_symbols[i] * std::conj(h) / (h_mag_sq + noise_var);
        } else {
            equalized[i] = Complex(0, 0);  // Deep fade, mark as erasure
        }
    }

    return equalized;
}

std::vector<Complex> AFDMDemodulator::extractDataSymbols(const std::vector<Complex>& equalized) {
    std::vector<Complex> data;
    data.reserve(data_indices_.size());

    for (int idx : data_indices_) {
        if (idx >= 0 && static_cast<size_t>(idx) < equalized.size()) {
            data.push_back(equalized[idx]);
        }
    }

    return data;
}

std::vector<float> AFDMDemodulator::demapSoft(const std::vector<Complex>& symbols, Modulation mod) {
    float noise_var = std::pow(10.0f, -estimated_snr_db_ / 10.0f);
    if (noise_var < 0.001f) noise_var = 0.001f;

    std::vector<float> soft_bits;
    soft_bits.reserve(symbols.size() * getBitsPerSymbol(mod));

    for (const auto& sym : symbols) {
        auto llrs = mapping::demapSoft(sym, mod, noise_var);
        soft_bits.insert(soft_bits.end(), llrs.begin(), llrs.end());
    }

    return soft_bits;
}

std::vector<float> AFDMDemodulator::demodulate(SampleSpan samples, Modulation mod) {
    validateAudioChainConfig(config_, "RX");

    // Downmix to baseband
    auto baseband = downmix(samples);

    std::vector<float> all_soft_bits;
    all_soft_bits.reserve(bitsPerFrame(mod));

    // Process each symbol
    for (size_t sym = 0; sym < config_.symbols_per_frame; ++sym) {
        // Extract DAFT symbol
        auto daft_symbols = extractSymbol(baseband, sym);
        if (daft_symbols.empty()) break;

        // Channel estimation (use first symbol's pilots, update with each)
        estimateChannel(daft_symbols);

        // Equalize
        auto equalized = equalize(daft_symbols);

        // Extract data
        auto data_symbols = extractDataSymbols(equalized);

        // Demap to soft bits
        auto soft_bits = demapSoft(data_symbols, mod);
        all_soft_bits.insert(all_soft_bits.end(), soft_bits.begin(), soft_bits.end());
    }

    return all_soft_bits;
}

Bytes AFDMDemodulator::demodulateHard(SampleSpan samples, Modulation mod) {
    auto soft_bits = demodulate(samples, mod);

    // Convert soft bits to hard bits, then to bytes
    // LLR convention: positive LLR = bit likely 0, negative LLR = bit likely 1
    Bytes data;
    data.reserve((soft_bits.size() + 7) / 8);

    uint8_t byte = 0;
    for (size_t i = 0; i < soft_bits.size(); ++i) {
        byte <<= 1;
        if (soft_bits[i] < 0) byte |= 1;  // Negative LLR = bit is 1

        if ((i + 1) % 8 == 0) {
            data.push_back(byte);
            byte = 0;
        }
    }

    // Handle remaining bits
    if (soft_bits.size() % 8 != 0) {
        byte <<= (8 - soft_bits.size() % 8);
        data.push_back(byte);
    }

    return data;
}

void AFDMDemodulator::setCFO(float cfo_hz) {
    cfo_hz_ = cfo_hz;
}

void AFDMDemodulator::reset() {
    std::fill(channel_estimate_.begin(), channel_estimate_.end(), Complex(1, 0));
    estimated_snr_db_ = 0.0f;
    fading_index_ = 0.0f;
    cfo_hz_ = 0.0f;
}

// Helper to match modulator
size_t AFDMDemodulator::bitsPerFrame(Modulation mod) const {
    return config_.dataSubcarriers() * config_.symbols_per_frame * getBitsPerSymbol(mod);
}

} // namespace afdm
} // namespace ultra
