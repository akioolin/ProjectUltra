#include "ultra/ofdm.hpp"
#include "ultra/dsp.hpp"
#include "ofdm/soft_demap.hpp"
#include <iostream>
#include <cmath>
#include <random>

namespace {

bool checkCoherent8PskLlrSigns() {
    static const int data_to_phase[8] = {0, 1, 3, 2, 7, 6, 4, 5};
    constexpr float pi = 3.14159265358979f;
    constexpr float noise_var = 0.05f;

    for (int bits = 0; bits < 8; ++bits) {
        const float angle = data_to_phase[bits] * (pi / 4.0f) + pi / 8.0f;
        ultra::Complex sym(std::cos(angle), std::sin(angle));
        auto llrs = ultra::soft_demap::demap8PSK(sym, noise_var);
        if (llrs.size() != 3) {
            std::cout << "  8PSK LLR size failed for bits=" << bits << "\n";
            return false;
        }
        for (int bit = 0; bit < 3; ++bit) {
            const bool expected_one = (bits & (1 << (2 - bit))) != 0;
            if (expected_one && llrs[bit] >= 0.0f) {
                std::cout << "  8PSK LLR sign failed for bits=" << bits
                          << " bit=" << bit << " llr=" << llrs[bit] << "\n";
                return false;
            }
            if (!expected_one && llrs[bit] <= 0.0f) {
                std::cout << "  8PSK LLR sign failed for bits=" << bits
                          << " bit=" << bit << " llr=" << llrs[bit] << "\n";
                return false;
            }
        }
    }
    return true;
}

bool checkCoherent8PskLlrMagnitudes() {
    constexpr float noise_var_per_real = 0.4f;
    constexpr float coeff = 0.5f;
    const ultra::Complex probe(0.31f, -0.47f);  // Deliberately off-constellation.

    const auto deltas = ultra::soft_demap::psk8MaxLogDistanceDeltas(probe);
    const auto raw = ultra::soft_demap::demap8PSKUnclipped(
        probe, noise_var_per_real, coeff);
    for (size_t bit = 0; bit < raw.size(); ++bit) {
        const float expected = deltas[bit] / (2.0f * noise_var_per_real);
        if (std::abs(raw[bit] - expected) > 1.0e-6f) {
            std::cout << "  8PSK LLR magnitude failed for bit=" << bit
                      << " got=" << raw[bit] << " expected=" << expected << "\n";
            return false;
        }
    }

    const float helper_min = ultra::soft_demap::psk8MinAbsLLRNoClip(
        probe, noise_var_per_real);
    const auto production_raw = ultra::soft_demap::demap8PSKUnclipped(
        probe, noise_var_per_real);
    const float expected_min = std::min(
        std::abs(production_raw[0]),
        std::min(std::abs(production_raw[1]), std::abs(production_raw[2])));
    if (std::abs(helper_min - expected_min) > 1.0e-6f) {
        std::cout << "  8PSK DD/demapper helper mismatch: got=" << helper_min
                  << " expected=" << expected_min << "\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    std::cout << "Testing OFDM implementation...\n\n";

    std::cout << "Testing coherent 8PSK soft demapper...\n";
    if (!checkCoherent8PskLlrSigns()) {
        return 1;
    }
    if (!checkCoherent8PskLlrMagnitudes()) {
        return 1;
    }
    std::cout << "  8PSK LLR signs and max-log magnitudes OK\n";

    ultra::ModemConfig config;
    config.sample_rate = 48000;
    config.fft_size = 512;
    config.num_carriers = 48;
    config.cp_mode = ultra::CyclicPrefixMode::LONG;  // 64 samples

    // Test modulator
    std::cout << "Testing OFDM modulator...\n";
    ultra::OFDMModulator modulator(config);

    // Generate test data
    std::vector<uint8_t> test_data(32);
    for (size_t i = 0; i < test_data.size(); ++i) {
        test_data[i] = static_cast<uint8_t>(i * 7 + 13);
    }

    // Test each modulation scheme
    for (auto mod : {ultra::Modulation::BPSK, ultra::Modulation::QPSK,
                     ultra::Modulation::QAM8, ultra::Modulation::QAM16}) {
        auto samples = modulator.modulate(test_data, mod);

        const char* mod_name = "";
        switch (mod) {
            case ultra::Modulation::BPSK: mod_name = "BPSK"; break;
            case ultra::Modulation::QPSK: mod_name = "QPSK"; break;
            case ultra::Modulation::QAM8: mod_name = "8PSK"; break;
            case ultra::Modulation::QAM16: mod_name = "QAM16"; break;
            default: mod_name = "?"; break;
        }

        std::cout << "  " << mod_name << ": " << samples.size() << " samples";

        // Check signal properties
        float peak = ultra::dsp::peak(samples);
        float rms = ultra::dsp::rms(samples);

        std::cout << " (peak=" << peak << ", rms=" << rms << ")";

        if (samples.size() > 0 && peak < 10 && peak > 0.01) {
            std::cout << " OK\n";
        } else {
            std::cout << " FAILED\n";
            return 1;
        }
    }

    // Test preamble generation
    std::cout << "\nTesting preamble generation...\n";
    auto preamble = modulator.generatePreamble();
    std::cout << "  Preamble: " << preamble.size() << " samples";

    float preamble_duration_ms = preamble.size() * 1000.0f / config.sample_rate;
    std::cout << " (" << preamble_duration_ms << " ms)";

    if (preamble.size() > 0 && preamble_duration_ms > 10 && preamble_duration_ms < 500) {
        std::cout << " OK\n";
    } else {
        std::cout << " FAILED\n";
        return 1;
    }

    // Test AWGN channel simulation
    std::cout << "\nTesting with AWGN channel...\n";

    // Generate a frame
    std::vector<uint8_t> tx_data = {0x55, 0xAA, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    auto tx_samples = modulator.modulate(tx_data, ultra::Modulation::QPSK);

    // Add noise
    std::mt19937 rng(42);
    std::normal_distribution<float> noise(0, 0.1f);

    std::vector<float> rx_samples = tx_samples;
    for (auto& s : rx_samples) {
        s += noise(rng);
    }

    // Create demodulator and process
    ultra::OFDMDemodulator demodulator(config);

    // Feed samples (would need preamble for sync in real scenario)
    // For now just verify the demodulator doesn't crash
    bool frame_ready = demodulator.process(rx_samples);
    std::cout << "  Demodulator processed " << rx_samples.size() << " samples";
    std::cout << " (frame_ready=" << frame_ready << ") OK\n";

    // Test data rate calculations
    std::cout << "\nTheoretical data rates:\n";
    for (auto mod : {ultra::Modulation::BPSK, ultra::Modulation::QPSK,
                     ultra::Modulation::QAM8, ultra::Modulation::QAM16,
                     ultra::Modulation::QAM64}) {
        size_t bits_per_symbol = modulator.bitsPerSymbol(mod);
        size_t samples_per_symbol = modulator.samplesPerSymbol();
        float symbol_rate = static_cast<float>(config.sample_rate) / samples_per_symbol;
        float raw_bps = bits_per_symbol * symbol_rate;

        const char* mod_name = "";
        switch (mod) {
            case ultra::Modulation::BPSK: mod_name = "BPSK"; break;
            case ultra::Modulation::QPSK: mod_name = "QPSK"; break;
            case ultra::Modulation::QAM8: mod_name = "8PSK"; break;
            case ultra::Modulation::QAM16: mod_name = "QAM16"; break;
            case ultra::Modulation::QAM64: mod_name = "QAM64"; break;
            default: break;
        }

        std::cout << "  " << mod_name << ": " << bits_per_symbol << " bits/sym, "
                  << static_cast<int>(raw_bps) << " raw bps\n";
    }

    std::cout << "\nAll OFDM tests passed!\n";
    return 0;
}
