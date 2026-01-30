// test_afdm.cpp - AFDM (Affine Frequency Division Multiplexing) test tool
//
// Tests:
// 1. DAFT/IDAFT roundtrip accuracy
// 2. DAFT with c1=c2=0 equals standard FFT
// 3. AFDM modulation/demodulation on AWGN
// 4. AFDM vs OFDM on fading channels (the key test!)
//
// Usage:
//   ./test_afdm                    # Run all tests
//   ./test_afdm --daft             # DAFT transform tests only
//   ./test_afdm --awgn             # AWGN channel test
//   ./test_afdm --fading           # Fading channel comparison
//   ./test_afdm --snr 10           # Specify SNR (default: 10 dB)

#include "afdm/afdm.hpp"
#include "afdm/daft.hpp"
#include "afdm/afdm_config.hpp"
#include "ultra/types.hpp"

#include <iostream>
#include <iomanip>
#include <cmath>
#include <random>
#include <chrono>
#include <cstring>

using namespace ultra;
using namespace ultra::afdm;

// ============================================================================
// Test utilities
// ============================================================================

static std::mt19937 rng(42);

std::vector<Complex> generateRandomSymbols(size_t n) {
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<Complex> symbols(n);
    for (auto& s : symbols) {
        s = Complex(dist(rng), dist(rng));
    }
    return symbols;
}

float computeError(const std::vector<Complex>& a, const std::vector<Complex>& b) {
    if (a.size() != b.size()) return 1e10f;

    float error = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        error += std::norm(a[i] - b[i]);
    }
    return std::sqrt(error / a.size());
}

void addAWGN(std::vector<float>& samples, float snr_db) {
    // Calculate signal power
    float signal_power = 0.0f;
    for (float s : samples) {
        signal_power += s * s;
    }
    signal_power /= samples.size();

    // Calculate noise power
    float noise_power = signal_power / std::pow(10.0f, snr_db / 10.0f);
    float noise_std = std::sqrt(noise_power);

    std::normal_distribution<float> noise(0.0f, noise_std);
    for (float& s : samples) {
        s += noise(rng);
    }
}

// Simple HF fading channel simulation
void applyFading(std::vector<float>& samples, float fading_strength, float doppler_hz, float sample_rate) {
    // Multi-path with Rayleigh fading
    std::normal_distribution<float> rayleigh(0.0f, fading_strength);

    // Time-varying channel (Doppler)
    float phase = 0.0f;
    float phase_inc = 2.0f * M_PI * doppler_hz / sample_rate;

    for (size_t i = 0; i < samples.size(); ++i) {
        // Slow fading envelope
        float fade = 1.0f + rayleigh(rng) * std::sin(phase);
        fade = std::max(0.1f, fade);  // Prevent complete nulls

        samples[i] *= fade;
        phase += phase_inc;
    }
}

// ============================================================================
// Test 1: DAFT Roundtrip
// ============================================================================

bool testDAFTRoundtrip() {
    std::cout << "\n=== Test 1: DAFT Roundtrip ===" << std::endl;

    bool all_passed = true;

    // Test with various c1, c2 values
    std::vector<std::pair<float, float>> params = {
        {0.0f, 0.0f},      // Standard FFT
        {0.025f, 0.0f},    // Standard AFDM
        {0.03f, 0.0f},     // Higher c1
        {0.025f, 0.01f},   // With c2
        {0.05f, 0.02f},    // Both non-zero
    };

    for (auto [c1, c2] : params) {
        auto input = generateRandomSymbols(64);

        auto transformed = daft(input, c1, c2);
        auto recovered = idaft(transformed, c1, c2);

        float error = computeError(input, recovered);

        bool passed = error < 1e-5f;
        all_passed &= passed;

        std::cout << "  c1=" << std::setw(5) << c1
                  << ", c2=" << std::setw(5) << c2
                  << " -> error=" << std::scientific << std::setprecision(2) << error
                  << (passed ? " [PASS]" : " [FAIL]") << std::endl;
    }

    // Test DAFTProcessor (pre-computed tables)
    std::cout << "\n  DAFTProcessor test:" << std::endl;
    DAFTProcessor proc(64, 0.025f, 0.0f);
    auto input = generateRandomSymbols(64);
    auto transformed = proc.forward(input);
    auto recovered = proc.inverse(transformed);
    float error = computeError(input, recovered);

    bool passed = error < 1e-5f;
    all_passed &= passed;
    std::cout << "  DAFTProcessor roundtrip error=" << std::scientific << error
              << (passed ? " [PASS]" : " [FAIL]") << std::endl;

    return all_passed;
}

// ============================================================================
// Test 2: DAFT with c1=c2=0 equals FFT
// ============================================================================

bool testDAFTEqualsFFT() {
    std::cout << "\n=== Test 2: DAFT(c1=c2=0) equals FFT ===" << std::endl;

    // Generate test data
    auto input = generateRandomSymbols(64);

    // DAFT with c1=c2=0
    auto daft_result = daft(input, 0.0f, 0.0f);

    // Manual DFT for comparison
    std::vector<Complex> dft_result(64);
    for (size_t k = 0; k < 64; ++k) {
        Complex sum(0, 0);
        for (size_t n = 0; n < 64; ++n) {
            float phase = -2.0f * M_PI * k * n / 64.0f;
            sum += input[n] * Complex(std::cos(phase), std::sin(phase));
        }
        dft_result[k] = sum;
    }

    float error = computeError(daft_result, dft_result);
    bool passed = error < 1e-4f;

    std::cout << "  Error between DAFT(0,0) and DFT: " << std::scientific << error
              << (passed ? " [PASS]" : " [FAIL]") << std::endl;

    return passed;
}

// ============================================================================
// Test 2.5: Direct Baseband Test (no upmix/downmix)
// ============================================================================

// Helper: QPSK hard decision (returns 0-3 for quadrant)
// Must match the constellation used in mapSymbol which is Gray coded
int qpskDemapHard(Complex sym) {
    // Quadrant based on sign of real and imag parts
    int q = 0;
    if (sym.real() < 0) q |= 1;
    if (sym.imag() < 0) q |= 2;
    // Gray decoding to match the constellation mapping
    return q ^ (q >> 1);
}

bool testDirectBaseband() {
    std::cout << "\n=== Test 2.5: Direct Baseband (no frequency conversion) ===" << std::endl;

    auto config = AFDMConfig::forHFFading();
    config.symbols_per_frame = 1;  // Single symbol for simplicity
    config.pilot_guard = 0;        // No guards for simplicity

    DAFTProcessor daft_proc(config.N, config.c1, config.c2);

    // Create known QPSK symbols
    std::vector<Complex> tx_symbols;
    for (size_t i = 0; i < config.N; ++i) {
        // QPSK constellation: (±0.707, ±0.707)
        int quadrant = i % 4;
        float r = (quadrant & 1) ? -0.7071f : 0.7071f;
        float im = (quadrant & 2) ? -0.7071f : 0.7071f;
        tx_symbols.push_back(Complex(r, im));
    }

    std::cout << "  TX symbols: " << tx_symbols.size() << std::endl;
    std::cout << "  First 4 TX: ";
    for (int i = 0; i < 4; ++i) {
        std::cout << "(" << tx_symbols[i].real() << "," << tx_symbols[i].imag() << ") ";
    }
    std::cout << std::endl;

    // IDAFT (modulate to time domain)
    auto time_domain = daft_proc.inverse(tx_symbols);

    // No noise for this test - pure roundtrip
    // Add tiny noise to test robustness
    std::normal_distribution<float> noise(0.0f, 0.001f);
    for (auto& s : time_domain) {
        s += Complex(noise(rng), noise(rng));
    }

    // DAFT (demodulate back to symbol domain)
    auto rx_symbols = daft_proc.forward(time_domain);

    std::cout << "  First 4 RX: ";
    for (int i = 0; i < 4; ++i) {
        std::cout << "(" << std::fixed << std::setprecision(3)
                  << rx_symbols[i].real() << "," << rx_symbols[i].imag() << ") ";
    }
    std::cout << std::endl;

    // Compare actual complex values (should be very close)
    float mse = 0.0f;
    for (size_t i = 0; i < config.N; ++i) {
        mse += std::norm(tx_symbols[i] - rx_symbols[i]);
    }
    mse /= config.N;

    std::cout << "  MSE: " << std::scientific << mse << std::endl;

    // Compare using hard decision
    int errors = 0;
    for (size_t i = 0; i < config.N; ++i) {
        int tx_q = qpskDemapHard(tx_symbols[i]);
        int rx_q = qpskDemapHard(rx_symbols[i]);
        if (tx_q != rx_q) errors++;
    }

    float ser = static_cast<float>(errors) / config.N;
    bool passed = mse < 1e-3f;  // MSE should be small for roundtrip (with noise)

    std::cout << "  Symbol errors: " << errors << "/" << config.N
              << " (SER=" << std::fixed << std::setprecision(3) << ser << ")"
              << (passed ? " [PASS]" : " [FAIL]") << std::endl;

    // Test pilot insertion/extraction
    std::cout << "  Testing pilot insertion/extraction..." << std::endl;

    // Create data symbols (QPSK)
    size_t num_data = config.N - config.N / config.pilot_spacing;
    std::vector<Complex> data_syms(num_data);
    for (size_t i = 0; i < num_data; ++i) {
        int quadrant = i % 4;
        float r = (quadrant & 1) ? -0.7071f : 0.7071f;
        float im = (quadrant & 2) ? -0.7071f : 0.7071f;
        data_syms[i] = Complex(r, im);
    }

    // Insert into frame with pilots
    std::vector<Complex> frame(config.N, Complex(0, 0));
    size_t pilot_idx = 0;
    size_t data_idx = 0;
    std::vector<size_t> pilot_positions;
    std::vector<size_t> data_positions;

    for (size_t i = 0; i < config.N; ++i) {
        if (i % config.pilot_spacing == 0) {
            // Pilot: alternating +1, -1
            float sign = (pilot_idx % 2 == 0) ? 1.0f : -1.0f;
            frame[i] = Complex(sign, 0.0f);
            pilot_positions.push_back(i);
            pilot_idx++;
        } else if (data_idx < data_syms.size()) {
            frame[i] = data_syms[data_idx++];
            data_positions.push_back(i);
        }
    }

    std::cout << "  Pilots at: " << pilot_positions.size() << " positions" << std::endl;
    std::cout << "  Data at: " << data_positions.size() << " positions" << std::endl;

    // IDAFT (no noise this time)
    auto tx_time = daft_proc.inverse(frame);

    // DAFT
    auto rx_frame = daft_proc.forward(tx_time);

    // Check frame MSE first
    float frame_mse = 0.0f;
    for (size_t i = 0; i < config.N; ++i) {
        frame_mse += std::norm(frame[i] - rx_frame[i]);
    }
    frame_mse /= config.N;
    std::cout << "  Frame roundtrip MSE: " << std::scientific << frame_mse << std::endl;

    // Show first few
    std::cout << "  TX frame[0-3]: ";
    for (int i = 0; i < 4; ++i) {
        std::cout << "(" << std::fixed << std::setprecision(2)
                  << frame[i].real() << "," << frame[i].imag() << ") ";
    }
    std::cout << std::endl;
    std::cout << "  RX frame[0-3]: ";
    for (int i = 0; i < 4; ++i) {
        std::cout << "(" << std::fixed << std::setprecision(2)
                  << rx_frame[i].real() << "," << rx_frame[i].imag() << ") ";
    }
    std::cout << std::endl;

    // Extract and compare data using hard decision
    errors = 0;
    for (size_t i = 0; i < data_positions.size(); ++i) {
        size_t pos = data_positions[i];
        int tx_q = qpskDemapHard(frame[pos]);
        int rx_q = qpskDemapHard(rx_frame[pos]);
        if (tx_q != rx_q) errors++;
    }

    ser = static_cast<float>(errors) / data_positions.size();
    bool pilot_passed = frame_mse < 1e-10f;  // Should be essentially perfect

    std::cout << "  Data symbol errors: " << errors << "/" << data_positions.size()
              << " (SER=" << std::fixed << std::setprecision(3) << ser << ")"
              << (pilot_passed ? " [PASS]" : " [FAIL]") << std::endl;

    return passed && pilot_passed;
}

// ============================================================================
// Test 2.6: Full Modulator/Demodulator in Complex Baseband (no frequency conversion)
// ============================================================================

// ============================================================================
// Test 2.55: Upmix/Downmix Test
// ============================================================================

bool testUpmixDownmix() {
    std::cout << "\n=== Test 2.55: Upmix/Downmix ===" << std::endl;

    auto config = AFDMConfig::forHFFading();
    AFDMModulator mod(config);
    AFDMDemodulator demod(config);

    // Create simple complex baseband signal
    std::vector<Complex> tx_baseband(256);
    for (size_t i = 0; i < tx_baseband.size(); ++i) {
        // Low-frequency sinusoid in baseband
        float t = static_cast<float>(i) / config.sample_rate;
        float freq = 100.0f;  // 100 Hz
        tx_baseband[i] = Complex(std::cos(2.0f * M_PI * freq * t),
                                  std::sin(2.0f * M_PI * freq * t));
    }

    // Upmix
    auto audio = mod.upmix(tx_baseband);
    std::cout << "  Audio samples: " << audio.size() << std::endl;

    // Downmix
    auto rx_baseband = demod.downmix(audio);
    std::cout << "  RX baseband samples: " << rx_baseband.size() << std::endl;

    // Compare (there should be a scale factor of ~2 from the mixing)
    // Find the scale factor from first few samples
    Complex scale_sum(0, 0);
    for (size_t i = 10; i < 50; ++i) {  // Skip first few due to filter settling
        if (std::abs(tx_baseband[i]) > 0.01f) {
            scale_sum += rx_baseband[i] / tx_baseband[i];
        }
    }
    Complex scale = scale_sum / 40.0f;
    std::cout << "  Scale factor: " << std::abs(scale) << " (angle: "
              << (std::arg(scale) * 180.0f / M_PI) << " deg)" << std::endl;

    // After scaling, compute MSE
    float mse = 0.0f;
    for (size_t i = 20; i < tx_baseband.size() - 20; ++i) {  // Skip edges
        Complex expected = tx_baseband[i] * scale;
        mse += std::norm(rx_baseband[i] - expected);
    }
    mse /= (tx_baseband.size() - 40);
    std::cout << "  MSE (after scale): " << std::scientific << mse << std::endl;

    // Show first few
    std::cout << "  TX[20-23]: ";
    for (int i = 20; i < 24; ++i) {
        std::cout << "(" << std::fixed << std::setprecision(3)
                  << tx_baseband[i].real() << "," << tx_baseband[i].imag() << ") ";
    }
    std::cout << std::endl;
    std::cout << "  RX[20-23]: ";
    for (int i = 20; i < 24; ++i) {
        std::cout << "(" << std::fixed << std::setprecision(3)
                  << rx_baseband[i].real() << "," << rx_baseband[i].imag() << ") ";
    }
    std::cout << std::endl;
    std::cout << "  RX/scale[20-23]: ";
    for (int i = 20; i < 24; ++i) {
        Complex scaled = rx_baseband[i] / scale;
        std::cout << "(" << std::fixed << std::setprecision(3)
                  << scaled.real() << "," << scaled.imag() << ") ";
    }
    std::cout << std::endl;

    bool passed = mse < 0.1f;  // Allow some error from filtering
    std::cout << "  " << (passed ? "[PASS]" : "[FAIL]") << std::endl;

    return passed;
}

bool testModDemodBaseband() {
    std::cout << "\n=== Test 2.6: Mod/Demod in Complex Baseband ===" << std::endl;

    auto config = AFDMConfig::forHFFading();
    config.symbols_per_frame = 1;  // Single symbol

    AFDMModulator mod(config);
    AFDMDemodulator demod(config);

    // Generate simple test data
    Bytes test_data(5);  // Small amount
    for (size_t i = 0; i < test_data.size(); ++i) {
        test_data[i] = static_cast<uint8_t>(i * 17 + 3);
    }

    std::cout << "  Input: " << test_data.size() << " bytes" << std::endl;

    // Map to symbols
    auto data_symbols = mod.mapToSymbols(test_data, Modulation::DQPSK);
    std::cout << "  Data symbols: " << data_symbols.size() << std::endl;

    // Check how many data symbols fit per frame
    std::cout << "  Data subcarriers: " << config.dataSubcarriers() << std::endl;

    // Insert pilots
    auto daft_frame = mod.insertPilots(data_symbols);
    std::cout << "  DAFT frame size: " << daft_frame.size() << std::endl;

    // Generate symbol (IDAFT + CPP)
    auto time_symbol = mod.generateSymbol(daft_frame);
    std::cout << "  Time symbol size: " << time_symbol.size() << " (N=" << config.N << " + CPP=" << config.cpp_length << ")" << std::endl;

    // Now demodulate - extract symbol from complex baseband
    // Skip CPP
    std::vector<Complex> time_no_cpp(time_symbol.begin() + config.cpp_length, time_symbol.end());
    std::cout << "  Time samples after CPP removal: " << time_no_cpp.size() << std::endl;

    // DAFT
    DAFTProcessor daft_proc(config.N, config.c1, config.c2);
    auto rx_daft = daft_proc.forward(time_no_cpp);
    std::cout << "  RX DAFT symbols: " << rx_daft.size() << std::endl;

    // Compare TX and RX DAFT frames
    float mse = 0.0f;
    for (size_t i = 0; i < config.N; ++i) {
        mse += std::norm(daft_frame[i] - rx_daft[i]);
    }
    mse /= config.N;
    std::cout << "  DAFT frame MSE: " << std::scientific << mse << std::endl;

    // Show first few
    std::cout << "  TX DAFT[0-7]: ";
    for (int i = 0; i < 8; ++i) {
        std::cout << "(" << std::fixed << std::setprecision(2)
                  << daft_frame[i].real() << "," << daft_frame[i].imag() << ") ";
    }
    std::cout << std::endl;
    std::cout << "  RX DAFT[0-7]: ";
    for (int i = 0; i < 8; ++i) {
        std::cout << "(" << std::fixed << std::setprecision(2)
                  << rx_daft[i].real() << "," << rx_daft[i].imag() << ") ";
    }
    std::cout << std::endl;

    bool passed = mse < 1e-10f;
    std::cout << "  " << (passed ? "[PASS]" : "[FAIL]") << std::endl;

    return passed;
}

// ============================================================================
// Test 3: Full AFDM Chain in Complex Baseband (bypass upmix/downmix)
// ============================================================================

bool testAFDMOnAWGN(float snr_db) {
    std::cout << "\n=== Test 3: AFDM Full Chain (Complex Baseband, SNR=" << snr_db << " dB) ===" << std::endl;

    // Configure for HF - use simple config without guards for testing
    auto config = AFDMConfig::forHFFading();
    config.symbols_per_frame = 3;  // Shorter frame
    config.pilot_guard = 0;        // No guards for simpler testing

    std::cout << "  Config: N=" << config.N << ", symbols=" << config.symbols_per_frame
              << ", data_carriers=" << config.dataSubcarriers() << std::endl;

    AFDMModulator mod(config);
    DAFTProcessor daft_proc(config.N, config.c1, config.c2);

    // Generate test data
    size_t data_capacity = config.dataSubcarriers() * config.symbols_per_frame;
    size_t bytes_capacity = (data_capacity * 2) / 8;  // DQPSK = 2 bits/symbol
    Bytes test_data(std::min(bytes_capacity, size_t(15)));
    for (size_t i = 0; i < test_data.size(); ++i) {
        test_data[i] = static_cast<uint8_t>(i * 17 + 3);
    }

    std::cout << "  Input: " << test_data.size() << " bytes" << std::endl;

    // Map to symbols
    auto data_symbols = mod.mapToSymbols(test_data, Modulation::DQPSK);
    std::cout << "  Data symbols: " << data_symbols.size() << std::endl;

    // Process each DAFT symbol
    std::vector<Complex> all_time_samples;
    size_t sym_offset = 0;
    size_t data_per_symbol = config.dataSubcarriers();

    for (size_t sym = 0; sym < config.symbols_per_frame; ++sym) {
        // Extract data for this symbol
        std::vector<Complex> sym_data;
        for (size_t i = 0; i < data_per_symbol && sym_offset + i < data_symbols.size(); ++i) {
            sym_data.push_back(data_symbols[sym_offset + i]);
        }
        sym_offset += data_per_symbol;

        // Insert pilots
        auto daft_frame = mod.insertPilots(sym_data);

        // Generate time-domain symbol (IDAFT + CPP)
        auto symbol = mod.generateSymbol(daft_frame);

        // Debug: verify generateSymbol roundtrip for first symbol
        if (sym == 0) {
            // Extract time portion (skip CPP) and do DAFT
            std::vector<Complex> time_portion(symbol.begin() + config.cpp_length, symbol.end());
            auto recovered_daft = daft_proc.forward(time_portion);

            float verify_mse = 0.0f;
            for (size_t i = 0; i < config.N; ++i) {
                verify_mse += std::norm(daft_frame[i] - recovered_daft[i]);
            }
            verify_mse /= config.N;
            std::cout << "  Verify generateSymbol roundtrip MSE: " << std::scientific << verify_mse << std::endl;

            // Also test with noise
            auto time_with_noise = time_portion;

            // Calculate actual signal power first
            float actual_signal_power = 0.0f;
            for (const auto& s : time_with_noise) {
                actual_signal_power += std::norm(s);
            }
            actual_signal_power /= time_with_noise.size();
            std::cout << "  Signal power in time domain: " << std::scientific << actual_signal_power << std::endl;

            // Calculate noise power relative to actual signal power
            float noise_power_test = actual_signal_power / std::pow(10.0f, snr_db / 10.0f);
            float noise_std_test = std::sqrt(noise_power_test / 2.0f);
            std::cout << "  Noise power: " << std::scientific << noise_power_test
                      << ", noise_std: " << noise_std_test << std::endl;

            std::normal_distribution<float> noise_dist(0.0f, noise_std_test);
            for (auto& s : time_with_noise) {
                s += Complex(noise_dist(rng), noise_dist(rng));
            }
            auto recovered_with_noise = daft_proc.forward(time_with_noise);
            float noise_mse = 0.0f;
            for (size_t i = 0; i < config.N; ++i) {
                noise_mse += std::norm(daft_frame[i] - recovered_with_noise[i]);
            }
            noise_mse /= config.N;
            std::cout << "  With noise (SNR=" << snr_db << " dB) MSE: " << std::scientific << noise_mse << std::endl;
        }

        all_time_samples.insert(all_time_samples.end(), symbol.begin(), symbol.end());
    }

    std::cout << "  TX samples: " << all_time_samples.size() << std::endl;

    // Calculate actual signal power
    float signal_power = 0.0f;
    for (const auto& s : all_time_samples) {
        signal_power += std::norm(s);
    }
    signal_power /= all_time_samples.size();
    std::cout << "  Signal power: " << std::scientific << signal_power << std::endl;

    // Add complex AWGN relative to actual signal power
    float noise_power = signal_power / std::pow(10.0f, snr_db / 10.0f);
    float noise_std = std::sqrt(noise_power / 2.0f);  // Split between I and Q
    std::cout << "  Noise power: " << std::scientific << noise_power << std::endl;
    std::normal_distribution<float> noise(0.0f, noise_std);
    for (auto& s : all_time_samples) {
        s += Complex(noise(rng), noise(rng));
    }

    // Demodulate (bypass downmix, work directly in complex baseband)
    std::vector<Complex> recovered_symbols;
    std::vector<Complex> channel_estimate(config.N, Complex(1, 0));

    // Pilot sequence (must match modulator)
    std::vector<Complex> pilot_seq;
    size_t num_pilots = config.N / config.pilot_spacing;
    for (size_t i = 0; i < num_pilots; ++i) {
        float sign = ((i * 7 + 3) % 4 < 2) ? 1.0f : -1.0f;
        pilot_seq.push_back(Complex(sign, 0.0f));
    }

    // Compute pilot and data indices (with guards)
    std::vector<size_t> pilot_indices, data_indices;
    for (size_t i = 0; i < config.N; ++i) {
        if (i % config.pilot_spacing == 0) {
            pilot_indices.push_back(i);
        } else {
            bool is_guard = false;
            if (config.pilot_guard > 0) {
                size_t pilot_pos = (i / config.pilot_spacing) * config.pilot_spacing;
                size_t next_pilot = pilot_pos + config.pilot_spacing;
                size_t dist_to_prev = i - pilot_pos;
                size_t dist_to_next = (next_pilot < config.N) ? (next_pilot - i) : config.N;
                if (dist_to_prev <= config.pilot_guard || dist_to_next <= config.pilot_guard) {
                    is_guard = true;
                }
            }
            if (!is_guard) {
                data_indices.push_back(i);
            }
        }
    }

    std::cout << "  Data indices: " << data_indices.size() << std::endl;

    // For debugging, store TX DAFT frames
    std::vector<std::vector<Complex>> tx_daft_frames;

    // Re-modulate to get TX DAFT frames for comparison
    sym_offset = 0;
    for (size_t sym = 0; sym < config.symbols_per_frame; ++sym) {
        std::vector<Complex> sym_data;
        for (size_t i = 0; i < data_per_symbol && sym_offset + i < data_symbols.size(); ++i) {
            sym_data.push_back(data_symbols[sym_offset + i]);
        }
        sym_offset += data_per_symbol;
        auto daft_frame = mod.insertPilots(sym_data);
        tx_daft_frames.push_back(daft_frame);
    }

    for (size_t sym = 0; sym < config.symbols_per_frame; ++sym) {
        size_t sym_start = sym * (config.N + config.cpp_length);
        size_t data_start = sym_start + config.cpp_length;

        if (data_start + config.N > all_time_samples.size()) break;

        // Extract time samples (skip CPP)
        std::vector<Complex> time_samples(
            all_time_samples.begin() + data_start,
            all_time_samples.begin() + data_start + config.N
        );

        // DAFT
        auto rx_daft = daft_proc.forward(time_samples);

        // Debug: compare TX and RX DAFT for first symbol
        if (sym == 0) {
            float frame_mse = 0.0f;
            for (size_t i = 0; i < config.N; ++i) {
                frame_mse += std::norm(tx_daft_frames[sym][i] - rx_daft[i]);
            }
            frame_mse /= config.N;
            std::cout << "  Symbol 0 DAFT MSE: " << std::scientific << frame_mse << std::endl;

            std::cout << "  TX DAFT[0-3]: ";
            for (int i = 0; i < 4; ++i) {
                std::cout << "(" << std::fixed << std::setprecision(2)
                          << tx_daft_frames[sym][i].real() << "," << tx_daft_frames[sym][i].imag() << ") ";
            }
            std::cout << std::endl;
            std::cout << "  RX DAFT[0-3]: ";
            for (int i = 0; i < 4; ++i) {
                std::cout << "(" << std::fixed << std::setprecision(2)
                          << rx_daft[i].real() << "," << rx_daft[i].imag() << ") ";
            }
            std::cout << std::endl;
        }

        // Channel estimation from pilots
        for (size_t i = 0; i < pilot_indices.size(); ++i) {
            size_t idx = pilot_indices[i];
            if (std::abs(pilot_seq[i]) > 0.01f) {
                channel_estimate[idx] = rx_daft[idx] / pilot_seq[i];
            }
        }

        // Interpolate channel to data positions
        for (size_t i = 0; i < pilot_indices.size() - 1; ++i) {
            size_t p1 = pilot_indices[i];
            size_t p2 = pilot_indices[i + 1];
            for (size_t j = p1 + 1; j < p2; ++j) {
                float alpha = static_cast<float>(j - p1) / (p2 - p1);
                channel_estimate[j] = channel_estimate[p1] * (1.0f - alpha) + channel_estimate[p2] * alpha;
            }
        }

        // MMSE equalization and extract data
        float noise_var = noise_power;
        for (size_t idx : data_indices) {
            Complex h = channel_estimate[idx];
            float h_mag_sq = std::norm(h);
            Complex eq = rx_daft[idx] * std::conj(h) / (h_mag_sq + noise_var);
            recovered_symbols.push_back(eq);
        }
    }

    std::cout << "  Recovered symbols: " << recovered_symbols.size() << std::endl;

    // Debug: show first 8 recovered symbols
    std::cout << "  First 8 recovered symbols: ";
    for (int i = 0; i < 8 && i < (int)recovered_symbols.size(); ++i) {
        std::cout << "(" << std::fixed << std::setprecision(2)
                  << recovered_symbols[i].real() << "," << recovered_symbols[i].imag() << ") ";
    }
    std::cout << std::endl;

    // Debug: show expected first 8 data symbols
    std::cout << "  Expected first 8 data symbols: ";
    for (int i = 0; i < 8 && i < (int)data_symbols.size(); ++i) {
        std::cout << "(" << std::fixed << std::setprecision(2)
                  << data_symbols[i].real() << "," << data_symbols[i].imag() << ") ";
    }
    std::cout << std::endl;

    // Demap to bits
    Bytes recovered_data;
    size_t bit_idx = 0;
    uint8_t byte = 0;

    for (const auto& sym : recovered_symbols) {
        // QPSK hard decision
        int bits = qpskDemapHard(sym);

        // Add bits MSB first - bits[0] was encoded first (MSB of byte)
        byte = (byte << 1) | (bits & 1);  // bits[0]
        bit_idx++;
        if (bit_idx % 8 == 0) {
            recovered_data.push_back(byte);
            byte = 0;
        }

        byte = (byte << 1) | ((bits >> 1) & 1);  // bits[1]
        bit_idx++;
        if (bit_idx % 8 == 0) {
            recovered_data.push_back(byte);
            byte = 0;
        }
    }

    // Debug: trace first byte reconstruction
    std::cout << "  First 4 symbol bits: ";
    for (int i = 0; i < 4 && i < (int)recovered_symbols.size(); ++i) {
        int bits = qpskDemapHard(recovered_symbols[i]);
        std::cout << bits << " ";
    }
    std::cout << std::endl;

    std::cout << "  Recovered bytes: " << recovered_data.size() << std::endl;

    // Compare
    size_t compare_bytes = std::min(test_data.size(), recovered_data.size());
    int bit_errors = 0;
    for (size_t i = 0; i < compare_bytes; ++i) {
        uint8_t diff = test_data[i] ^ recovered_data[i];
        while (diff) {
            bit_errors += diff & 1;
            diff >>= 1;
        }
    }

    float ber = (compare_bytes > 0) ? static_cast<float>(bit_errors) / (compare_bytes * 8) : 1.0f;
    bool passed = ber < 0.15f;  // Allow 15% BER before FEC

    std::cout << "  Bit errors: " << bit_errors << "/" << (compare_bytes * 8)
              << " (BER=" << std::fixed << std::setprecision(3) << ber << ")"
              << (passed ? " [PASS]" : " [FAIL]") << std::endl;

    // Show first few bytes
    std::cout << "  TX[0-4]: ";
    for (size_t i = 0; i < std::min(size_t(5), test_data.size()); ++i) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)test_data[i] << " ";
    }
    std::cout << std::dec << std::endl;
    std::cout << "  RX[0-4]: ";
    for (size_t i = 0; i < std::min(size_t(5), recovered_data.size()); ++i) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)recovered_data[i] << " ";
    }
    std::cout << std::dec << std::endl;

    return passed;
}

// ============================================================================
// Test 4: AFDM vs OFDM on Fading (The Key Test!)
// ============================================================================

bool testAFDMOnFading(float snr_db) {
    std::cout << "\n=== Test 4: AFDM on Fading Channel (SNR=" << snr_db << " dB) ===" << std::endl;

    // Configure for HF fading
    auto config = AFDMConfig::forHFFading();
    config.symbols_per_frame = 5;

    std::cout << "  Config: N=" << config.N
              << ", c1=" << config.c1
              << ", c2=" << config.c2
              << ", full_diversity=" << (config.hasFullDiversity() ? "yes" : "no")
              << std::endl;

    AFDMModulator mod(config);
    AFDMDemodulator demod(config);

    // First, test the audio chain without fading/noise
    {
        Bytes test_data(20);
        for (size_t i = 0; i < test_data.size(); ++i) {
            test_data[i] = static_cast<uint8_t>(i * 17 + 3);
        }

        // Get the data symbols for comparison
        auto data_symbols = mod.mapToSymbols(test_data, Modulation::DQPSK);

        // Test simple known signal through upmix/downmix
        {
            std::vector<Complex> simple_bb(100, Complex(1.0f, 0.0f));  // DC signal
            auto simple_audio = mod.upmix(simple_bb);
            auto simple_rx = demod.downmix(simple_audio);
            std::cout << "  DC test: TX=(1,0), RX=(" << simple_rx[50].real()
                      << "," << simple_rx[50].imag() << ")" << std::endl;
        }
        {
            std::vector<Complex> simple_bb(100);
            for (size_t i = 0; i < 100; ++i) {
                simple_bb[i] = Complex(0.7071f, 0.7071f);  // Constant QPSK symbol
            }
            auto simple_audio = mod.upmix(simple_bb);
            auto simple_rx = demod.downmix(simple_audio);
            std::cout << "  QPSK test: TX=(0.71,0.71), RX=(" << simple_rx[50].real()
                      << "," << simple_rx[50].imag() << ")" << std::endl;
        }

        // Test a single DAFT symbol through the audio chain WITH CPP
        {
            std::cout << "\n  === Detailed DAFT symbol trace (with CPP) ===" << std::endl;

            // Create a known DAFT frame
            std::vector<Complex> daft_frame(config.N);
            for (size_t i = 0; i < config.N; ++i) {
                daft_frame[i] = Complex(0.7071f, 0.7071f);  // All QPSK (+,+)
            }

            // IDAFT to time domain
            DAFTProcessor daft_proc(config.N, config.c1, config.c2);
            auto time_domain = daft_proc.inverse(daft_frame);

            std::cout << "  Time domain (IDAFT) [0-3]: ";
            for (int i = 0; i < 4; ++i) {
                std::cout << "(" << std::fixed << std::setprecision(3)
                          << time_domain[i].real() << "," << time_domain[i].imag() << ") ";
            }
            std::cout << std::endl;

            // Add CPP (like the modulator does)
            auto symbol_with_cpp = mod.generateSymbol(daft_frame);
            std::cout << "  Symbol with CPP length: " << symbol_with_cpp.size()
                      << " (N=" << config.N << " + CPP=" << config.cpp_length << ")" << std::endl;

            // Upmix the full symbol
            auto audio_sym = mod.upmix(symbol_with_cpp);

            // Downmix
            auto rx_baseband = demod.downmix(audio_sym);

            // Extract symbol (skip CPP) - like the demodulator does
            std::vector<Complex> rx_data(config.N);
            for (size_t i = 0; i < config.N; ++i) {
                rx_data[i] = rx_baseband[config.cpp_length + i];
            }

            std::cout << "  RX data (after CPP skip) [0-3]: ";
            for (int i = 0; i < 4; ++i) {
                std::cout << "(" << std::fixed << std::setprecision(3)
                          << rx_data[i].real() << "," << rx_data[i].imag() << ") ";
            }
            std::cout << std::endl;

            // Compare with TX time domain
            float bb_mse = 0;
            for (size_t i = 0; i < config.N; ++i) {
                bb_mse += std::norm(time_domain[i] - rx_data[i]);
            }
            bb_mse /= config.N;
            std::cout << "  Baseband MSE (TX vs RX, post-CPP): " << std::scientific << bb_mse << std::endl;

            // DAFT on received data
            auto rx_daft = daft_proc.forward(rx_data);
            std::cout << "  RX DAFT [0-3]: ";
            for (int i = 0; i < 4; ++i) {
                std::cout << "(" << std::fixed << std::setprecision(3)
                          << rx_daft[i].real() << "," << rx_daft[i].imag() << ") ";
            }
            std::cout << std::endl;

            // DAFT MSE through audio
            float daft_audio_mse = 0;
            for (size_t i = 0; i < config.N; ++i) {
                daft_audio_mse += std::norm(daft_frame[i] - rx_daft[i]);
            }
            daft_audio_mse /= config.N;
            std::cout << "  DAFT MSE through audio: " << std::scientific << daft_audio_mse << std::endl;
            std::cout << "  ==================================\n" << std::endl;
        }

        auto audio = mod.modulate(test_data, Modulation::DQPSK);
        std::cout << "  Audio samples: " << audio.size() << std::endl;

        // Downmix manually to see what we get
        auto baseband_rx = demod.downmix(audio);
        std::cout << "  Baseband RX samples: " << baseband_rx.size() << std::endl;

        // Extract first DAFT symbol
        auto daft_sym0 = demod.extractSymbol(baseband_rx, 0);
        std::cout << "  DAFT symbol 0 size: " << daft_sym0.size() << std::endl;
        if (daft_sym0.size() > 0) {
            std::cout << "  DAFT[0-3]: ";
            for (int i = 0; i < 4 && i < (int)daft_sym0.size(); ++i) {
                std::cout << "(" << std::fixed << std::setprecision(2)
                          << daft_sym0[i].real() << "," << daft_sym0[i].imag() << ") ";
            }
            std::cout << std::endl;
            std::cout << "  Expected[0-3]: pilot, ";
            for (int i = 0; i < 3 && i < (int)data_symbols.size(); ++i) {
                std::cout << "(" << std::fixed << std::setprecision(2)
                          << data_symbols[i].real() << "," << data_symbols[i].imag() << ") ";
            }
            std::cout << std::endl;
        }

        auto recovered = demod.demodulateHard(audio, Modulation::DQPSK);

        size_t compare_bytes = std::min(test_data.size(), recovered.size());
        int bit_errors = 0;
        for (size_t i = 0; i < compare_bytes; ++i) {
            uint8_t diff = test_data[i] ^ recovered[i];
            while (diff) { bit_errors += diff & 1; diff >>= 1; }
        }
        float ber = static_cast<float>(bit_errors) / (compare_bytes * 8);
        std::cout << "  Audio chain test (no noise): BER=" << std::fixed << std::setprecision(3)
                  << ber << " (" << bit_errors << "/" << (compare_bytes * 8) << " bits)"
                  << (ber < 0.01f ? " [OK]" : " [PROBLEM]") << std::endl;
        if (ber > 0.01f) {
            std::cout << "  TX[0-4]: ";
            for (int i = 0; i < 5; ++i) std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)test_data[i] << " ";
            std::cout << std::dec << std::endl;
            std::cout << "  RX[0-4]: ";
            for (int i = 0; i < 5 && i < (int)recovered.size(); ++i) std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)recovered[i] << " ";
            std::cout << std::dec << std::endl;
        }
    }

    // Test on different channel conditions
    struct ChannelCondition {
        const char* name;
        float fading_strength;
        float doppler_hz;
    };

    std::vector<ChannelCondition> channels = {
        {"AWGN",     0.0f,  0.0f},
        {"Good",     0.15f, 0.5f},
        {"Moderate", 0.25f, 1.0f},
        {"Poor",     0.35f, 2.0f},
    };

    bool all_passed = true;
    const int num_trials = 5;

    for (const auto& ch : channels) {
        int success_count = 0;

        for (int trial = 0; trial < num_trials; ++trial) {
            // Generate test data
            Bytes test_data(20);
            for (size_t i = 0; i < test_data.size(); ++i) {
                test_data[i] = static_cast<uint8_t>((i + trial) * 17 + 3);
            }

            // Modulate
            auto audio = mod.modulate(test_data, Modulation::DQPSK);

            // Apply fading
            if (ch.fading_strength > 0) {
                applyFading(audio, ch.fading_strength, ch.doppler_hz, 48000.0f);
            }

            // Add noise
            addAWGN(audio, snr_db);

            // Demodulate
            auto recovered = demod.demodulateHard(audio, Modulation::DQPSK);

            // Count bit errors
            size_t compare_bytes = std::min(test_data.size(), recovered.size());
            int bit_errors = 0;
            for (size_t i = 0; i < compare_bytes; ++i) {
                uint8_t diff = test_data[i] ^ recovered[i];
                while (diff) {
                    bit_errors += diff & 1;
                    diff >>= 1;
                }
            }

            float ber = static_cast<float>(bit_errors) / (compare_bytes * 8);
            if (ber < 0.15f) {  // <15% BER = success (would be corrected by FEC)
                success_count++;
            }
        }

        float success_rate = 100.0f * success_count / num_trials;
        bool passed = (ch.fading_strength == 0 && success_rate >= 80) ||
                      (ch.fading_strength > 0 && success_rate >= 40);  // Lower bar for fading

        all_passed &= passed;

        std::cout << "  " << std::setw(10) << ch.name << ": "
                  << success_count << "/" << num_trials << " ("
                  << std::fixed << std::setprecision(0) << success_rate << "%)"
                  << ", fading_idx=" << std::fixed << std::setprecision(2) << demod.fadingIndex()
                  << (passed ? " [PASS]" : " [FAIL]") << std::endl;
    }

    return all_passed;
}

// ============================================================================
// Test 5: Throughput and Timing
// ============================================================================

void testThroughput() {
    std::cout << "\n=== Test 5: Throughput Analysis ===" << std::endl;

    auto config = AFDMConfig::forHFFading();
    AFDMModulator mod(config);

    // Calculate theoretical throughput
    size_t data_symbols = config.dataSubcarriers() * config.symbols_per_frame;
    size_t bits_per_frame = data_symbols * 2;  // DQPSK = 2 bits/symbol
    size_t samples_per_frame = config.samplesPerFrame();
    float frame_duration = samples_per_frame / config.sample_rate;

    float raw_throughput = bits_per_frame / frame_duration;

    std::cout << "  Data subcarriers: " << config.dataSubcarriers() << "/" << config.N << std::endl;
    std::cout << "  Symbols per frame: " << config.symbols_per_frame << std::endl;
    std::cout << "  Bits per frame (DQPSK): " << bits_per_frame << std::endl;
    std::cout << "  Frame duration: " << std::fixed << std::setprecision(1)
              << (frame_duration * 1000.0f) << " ms" << std::endl;
    std::cout << "  Raw throughput: " << std::fixed << std::setprecision(0)
              << raw_throughput << " bps" << std::endl;

    // With FEC overhead (R1/2)
    float fec_throughput = raw_throughput * 0.5f;
    std::cout << "  With R1/2 FEC: " << std::fixed << std::setprecision(0)
              << fec_throughput << " bps" << std::endl;

    // Timing test
    const int num_iterations = 100;
    Bytes test_data(mod.bytesPerFrame(Modulation::DQPSK));

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_iterations; ++i) {
        auto audio = mod.modulate(test_data, Modulation::DQPSK);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    float avg_us = duration.count() / static_cast<float>(num_iterations);

    std::cout << "  Modulation time: " << std::fixed << std::setprecision(1)
              << avg_us << " us/frame" << std::endl;
    std::cout << "  Real-time margin: " << std::fixed << std::setprecision(1)
              << (frame_duration * 1e6f / avg_us) << "x" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --daft     DAFT transform tests only\n"
              << "  --awgn     AWGN channel test\n"
              << "  --fading   Fading channel test\n"
              << "  --snr N    SNR in dB (default: 10)\n"
              << "  --all      Run all tests (default)\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "  AFDM Test Suite" << std::endl;
    std::cout << "  Affine Frequency Division Multiplexing" << std::endl;
    std::cout << "========================================" << std::endl;

    // Parse arguments
    bool test_daft = false;
    bool test_awgn = false;
    bool test_fading = false;
    bool test_all = true;
    float snr_db = 10.0f;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--daft") == 0) {
            test_daft = true;
            test_all = false;
        } else if (strcmp(argv[i], "--awgn") == 0) {
            test_awgn = true;
            test_all = false;
        } else if (strcmp(argv[i], "--fading") == 0) {
            test_fading = true;
            test_all = false;
        } else if (strcmp(argv[i], "--snr") == 0 && i + 1 < argc) {
            snr_db = std::stof(argv[++i]);
        } else if (strcmp(argv[i], "--all") == 0) {
            test_all = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        }
    }

    bool all_passed = true;

    // Run tests
    if (test_all || test_daft) {
        all_passed &= testDAFTRoundtrip();
        all_passed &= testDAFTEqualsFFT();
        all_passed &= testDirectBaseband();
        all_passed &= testUpmixDownmix();
        all_passed &= testModDemodBaseband();
    }

    if (test_all || test_awgn) {
        all_passed &= testAFDMOnAWGN(snr_db);
    }

    if (test_all || test_fading) {
        all_passed &= testAFDMOnFading(snr_db);
    }

    if (test_all) {
        testThroughput();
    }

    // Summary
    std::cout << "\n========================================" << std::endl;
    if (all_passed) {
        std::cout << "  ALL TESTS PASSED" << std::endl;
    } else {
        std::cout << "  SOME TESTS FAILED" << std::endl;
    }
    std::cout << "========================================" << std::endl;

    return all_passed ? 0 : 1;
}
