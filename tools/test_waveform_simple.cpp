// test_waveform_simple.cpp - Clean waveform test with consistent TX/RX config
//
// This test uses:
// - IWaveform directly for TX (no ModemEngine complexity)
// - StreamingDecoder for RX (properly configured)
// - Same config for both TX and RX
// - Full channel simulation (AWGN, fading, CFO)
//
// Usage:
//   ./test_waveform_simple [--snr N] [-w TYPE] [--rate RATE] [--channel TYPE] [--frames N]

#include "waveform/waveform_factory.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"
#include "waveform/mc_dpsk_waveform.hpp"
#include "gui/modem/streaming_decoder.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/fec.hpp"
#include "ultra/logging.hpp"
#include "ultra/dsp.hpp"
#include "sim/hf_channel.hpp"
#include <iostream>
#include <fstream>
#include <random>
#include <cmath>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <set>
#include <cstring>

using namespace ultra;
using namespace ultra::gui;
using namespace ultra::sim;
namespace v2 = protocol::v2;

// ============================================================================
// TEST CONFIGURATION - Single source of truth
// ============================================================================

struct TestConfig {
    // Waveform settings
    protocol::WaveformMode waveform = protocol::WaveformMode::OFDM_CHIRP;
    Modulation modulation = Modulation::DQPSK;
    CodeRate code_rate = CodeRate::R1_2;

    // OFDM settings (for OFDM modes)
    int fft_size = 1024;      // NVIS mode
    int num_carriers = 59;    // NVIS mode
    bool use_pilots = true;   // Enable pilots for R1/2+
    int pilot_spacing = 10;   // ~6 pilots for R1/2

    // MC-DPSK settings
    int mc_dpsk_carriers = 8;

    // Channel settings
    float snr_db = 20.0f;
    float cfo_hz = 0.0f;
    std::string channel_type = "awgn";

    // Test settings
    int num_frames = 3;
    uint32_t seed = 42;
    bool verbose = true;
    bool save_signals = false;
    std::string save_prefix = "/tmp/waveform";
    bool session_mode = false;  // If true, first frame uses full preamble, rest use light

    // Derived: configure pilots based on code rate
    void configurePilotsForCodeRate() {
        switch (code_rate) {
            case CodeRate::R3_4:
                use_pilots = true;
                pilot_spacing = 15;  // ~4 pilots
                break;
            case CodeRate::R2_3:
            case CodeRate::R1_2:
                use_pilots = true;
                pilot_spacing = 10;  // ~6 pilots
                break;
            case CodeRate::R1_4:
            default:
                use_pilots = false;
                pilot_spacing = 1;   // No pilots
                break;
        }
    }

    // Get number of data carriers (accounting for pilots)
    int getDataCarriers() const {
        if (!use_pilots || pilot_spacing <= 0) return num_carriers;
        int pilot_count = (num_carriers + pilot_spacing - 1) / pilot_spacing;
        return num_carriers - pilot_count;
    }

    // Get bits per OFDM symbol
    int getBitsPerSymbol() const {
        int bpc = 2;  // Default DQPSK
        switch (modulation) {
            case Modulation::DBPSK: bpc = 1; break;
            case Modulation::DQPSK: bpc = 2; break;
            case Modulation::D8PSK: bpc = 3; break;
            default: bpc = 2; break;
        }
        return getDataCarriers() * bpc;
    }

    // Create ModemConfig from this TestConfig
    ModemConfig toModemConfig() const {
        ModemConfig cfg;
        cfg.fft_size = fft_size;
        cfg.num_carriers = num_carriers;
        cfg.modulation = modulation;
        cfg.code_rate = code_rate;
        cfg.use_pilots = use_pilots;
        cfg.pilot_spacing = pilot_spacing;
        cfg.sample_rate = 48000;
        cfg.center_freq = 1500.0f;
        cfg.cp_mode = CyclicPrefixMode::LONG;
        return cfg;
    }

    void print() const {
        printf("=== Test Configuration ===\n");
        printf("Waveform: %s\n", protocol::waveformModeToString(waveform));
        printf("Modulation: %s\n", modulationToString(modulation));
        printf("Code Rate: %s\n", codeRateToString(code_rate));
        if (waveform == protocol::WaveformMode::OFDM_CHIRP ||
            waveform == protocol::WaveformMode::OFDM_COX) {
            printf("FFT: %d, Carriers: %d\n", fft_size, num_carriers);
            printf("Pilots: %s (spacing=%d, count=%d, data=%d)\n",
                   use_pilots ? "ON" : "OFF", pilot_spacing,
                   use_pilots ? (num_carriers + pilot_spacing - 1) / pilot_spacing : 0,
                   getDataCarriers());
            printf("Bits/symbol: %d\n", getBitsPerSymbol());
        } else if (waveform == protocol::WaveformMode::MC_DPSK) {
            printf("MC-DPSK carriers: %d\n", mc_dpsk_carriers);
        }
        printf("Channel: %s, SNR: %.1f dB, CFO: %.1f Hz\n", channel_type.c_str(), snr_db, cfo_hz);
        printf("Frames: %d, Seed: %u\n", num_frames, seed);
        if (session_mode) {
            printf("Session mode: ON (first frame full preamble, rest light)\n");
        }
        printf("==========================\n\n");
    }
};

// ============================================================================
// CHANNEL SIMULATION
// ============================================================================

void addNoise(std::vector<float>& samples, float snr_db, std::mt19937& rng) {
    float signal_power = 0.0f;
    size_t signal_samples = 0;
    for (float s : samples) {
        if (std::abs(s) > 1e-6f) {
            signal_power += s * s;
            signal_samples++;
        }
    }
    if (signal_samples == 0) return;

    signal_power /= signal_samples;
    float snr_linear = std::pow(10.0f, snr_db / 10.0f);
    float noise_power = signal_power / snr_linear;
    float noise_std = std::sqrt(noise_power);

    std::normal_distribution<float> noise_dist(0.0f, noise_std);
    for (float& s : samples) {
        s += noise_dist(rng);
    }
}

// Apply CFO using FFT-based Hilbert transform (no group delay)
void applyCFO(std::vector<float>& samples, float cfo_hz, float sample_rate = 48000.0f) {
    if (samples.size() < 128 || std::abs(cfo_hz) < 0.001f) return;

    size_t N = samples.size();
    printf("[CFO] Applying %.1f Hz offset to %zu samples\n", cfo_hz, N);

    // Pad to power of 2
    size_t fft_size = 1;
    while (fft_size < N) fft_size *= 2;

    FFT fft(fft_size);
    std::vector<Complex> time_in(fft_size, Complex(0, 0));
    std::vector<Complex> freq(fft_size);

    for (size_t i = 0; i < N; i++) {
        time_in[i] = Complex(samples[i], 0);
    }

    fft.forward(time_in.data(), freq.data());

    // Create analytic signal
    for (size_t i = 1; i < fft_size / 2; i++) {
        freq[i] *= 2.0f;
    }
    for (size_t i = fft_size / 2 + 1; i < fft_size; i++) {
        freq[i] = Complex(0, 0);
    }

    std::vector<Complex> analytic(fft_size);
    fft.inverse(freq.data(), analytic.data());

    // Apply frequency shift
    float phase = 0.0f;
    float phase_inc = 2.0f * static_cast<float>(M_PI) * cfo_hz / sample_rate;

    for (size_t i = 0; i < N; i++) {
        Complex rot(std::cos(phase), std::sin(phase));
        samples[i] = std::real(analytic[i] * rot);
        phase += phase_inc;
        if (phase > M_PI) phase -= 2.0f * M_PI;
        else if (phase < -M_PI) phase += 2.0f * M_PI;
    }
}

void applyChannel(std::vector<float>& audio, const TestConfig& cfg, std::mt19937& rng) {
    // Apply CFO first (before fading/noise)
    if (std::abs(cfg.cfo_hz) > 0.001f) {
        applyCFO(audio, cfg.cfo_hz);
    }

    // Apply channel
    printf("Applying %s channel (SNR=%.1f dB)...\n", cfg.channel_type.c_str(), cfg.snr_db);

    if (cfg.channel_type == "awgn") {
        addNoise(audio, cfg.snr_db, rng);
    } else {
        WattersonChannel::Config ch_cfg;

        if (cfg.channel_type == "good") {
            ch_cfg = itu_r_f1487::good(cfg.snr_db);
        } else if (cfg.channel_type == "moderate") {
            ch_cfg = itu_r_f1487::moderate(cfg.snr_db);
        } else if (cfg.channel_type == "poor") {
            ch_cfg = itu_r_f1487::poor(cfg.snr_db);
        } else if (cfg.channel_type == "flutter") {
            ch_cfg = itu_r_f1487::flutter(cfg.snr_db);
        } else {
            fprintf(stderr, "Unknown channel: %s, using AWGN\n", cfg.channel_type.c_str());
            addNoise(audio, cfg.snr_db, rng);
            return;
        }

        ch_cfg.cfo_enabled = false;  // We apply CFO separately

        WattersonChannel channel(ch_cfg, cfg.seed);
        SampleSpan input_span(audio.data(), audio.size());
        audio = channel.process(input_span);
    }
}

// ============================================================================
// TX: Generate frames using IWaveform directly
// ============================================================================

struct TxFrame {
    uint16_t seq;
    Bytes payload;
    size_t audio_start;
    size_t audio_len;
};

std::vector<float> generateFrames(const TestConfig& cfg, std::mt19937& rng,
                                   std::vector<TxFrame>& frames_out) {
    std::vector<float> full_audio;
    frames_out.clear();

    // Create waveform with proper config
    std::unique_ptr<IWaveform> waveform;

    if (cfg.waveform == protocol::WaveformMode::OFDM_CHIRP) {
        ModemConfig modem_cfg = cfg.toModemConfig();
        auto chirp = std::make_unique<OFDMChirpWaveform>(modem_cfg);
        chirp->configure(cfg.modulation, cfg.code_rate);
        waveform = std::move(chirp);
        printf("TX: Created OFDMChirpWaveform (%s, %s, %d data carriers)\n",
               modulationToString(cfg.modulation), codeRateToString(cfg.code_rate),
               cfg.getDataCarriers());
    } else if (cfg.waveform == protocol::WaveformMode::MC_DPSK) {
        waveform = WaveformFactory::createMCDPSK(cfg.mc_dpsk_carriers);
        printf("TX: Created MCDPSKWaveform (%d carriers)\n", cfg.mc_dpsk_carriers);
    } else {
        fprintf(stderr, "Unsupported waveform for TX\n");
        return full_audio;
    }

    // Add leading silence
    const size_t LEAD_IN = 48000;  // 1 second
    full_audio.resize(LEAD_IN, 0.0f);

    for (int i = 0; i < cfg.num_frames; i++) {
        TxFrame frame;
        frame.seq = i + 1;

        // Generate random payload
        size_t payload_size = 20 + (rng() % 30);  // 20-50 bytes
        frame.payload.resize(payload_size);
        for (auto& b : frame.payload) b = rng() & 0xFF;

        // Build v2 DATA frame
        auto df = v2::DataFrame::makeData("TEST", "RX", frame.seq, frame.payload, cfg.code_rate);
        Bytes frame_data = df.serialize();

        // Encode - MC-DPSK uses non-interleaved, OFDM uses frame interleaving
        Bytes encoded;
        if (cfg.waveform == protocol::WaveformMode::MC_DPSK) {
            // MC-DPSK: LDPC encode WITHOUT frame interleaving
            // CW0 can be decoded independently to parse header
            auto codewords = v2::encodeFrameWithLDPC(frame_data, cfg.code_rate);
            for (const auto& cw : codewords) {
                encoded.insert(encoded.end(), cw.begin(), cw.end());
            }
            if (cfg.verbose) {
                printf("  Frame %d: %zu bytes payload -> %zu bytes frame -> %zu CWs -> %zu bytes encoded (non-interleaved)\n",
                       frame.seq, frame.payload.size(), frame_data.size(), codewords.size(), encoded.size());
            }
        } else {
            // OFDM: Encode with frame interleaving (4 CWs per frame)
            encoded = v2::encodeFixedFrame(frame_data, cfg.code_rate);
            if (cfg.verbose) {
                printf("  Frame %d: %zu bytes payload -> %zu bytes frame -> %zu bytes encoded (interleaved)\n",
                       frame.seq, frame.payload.size(), frame_data.size(), encoded.size());
            }
        }

        // Modulate with appropriate preamble
        // - Session mode: first frame uses full preamble (chirp + LTS), rest use light (LTS only)
        // - Normal mode: all frames use full preamble (for independent frame testing)
        Samples preamble;
        bool use_light = cfg.session_mode && i > 0 && waveform->supportsDataPreamble();
        if (use_light) {
            preamble = waveform->generateDataPreamble();
            if (cfg.verbose) {
                printf("  Frame %d: Using LIGHT preamble (%zu samples)\n", frame.seq, preamble.size());
            }
        } else {
            preamble = waveform->generatePreamble();
            if (cfg.verbose && cfg.session_mode) {
                printf("  Frame %d: Using FULL preamble (%zu samples) - establishes CFO\n", frame.seq, preamble.size());
            }
        }
        Samples modulated = waveform->modulate(encoded);

        // Record position
        frame.audio_start = full_audio.size();
        frame.audio_len = preamble.size() + modulated.size();

        // Add to stream
        full_audio.insert(full_audio.end(), preamble.begin(), preamble.end());
        full_audio.insert(full_audio.end(), modulated.begin(), modulated.end());

        // Add gap between frames
        const size_t FRAME_GAP = 48000;  // 1 second
        full_audio.resize(full_audio.size() + FRAME_GAP, 0.0f);

        frames_out.push_back(frame);
    }

    // Add trailing silence
    full_audio.resize(full_audio.size() + 48000 * 2, 0.0f);  // 2 seconds

    // Normalize
    float max_val = 0.0f;
    for (float s : full_audio) max_val = std::max(max_val, std::abs(s));
    if (max_val > 0.0f) {
        float scale = 0.8f / max_val;
        for (float& s : full_audio) s *= scale;
    }

    return full_audio;
}

// ============================================================================
// RX: Decode using StreamingDecoder
// ============================================================================

std::set<uint16_t> decodeFrames(const std::vector<float>& audio, const TestConfig& cfg) {
    std::set<uint16_t> decoded_seqs;

    // Create and configure StreamingDecoder
    StreamingDecoder decoder;
    decoder.setLogPrefix("RX");

    // Set waveform mode (connected = true for DATA frames)
    decoder.setMode(cfg.waveform, true);

    // Configure OFDM if needed
    if (cfg.waveform == protocol::WaveformMode::OFDM_CHIRP ||
        cfg.waveform == protocol::WaveformMode::OFDM_COX) {
        ModemConfig modem_cfg = cfg.toModemConfig();
        decoder.setOFDMConfig(modem_cfg);
        decoder.setDataMode(cfg.modulation, cfg.code_rate);
        printf("RX: Configured StreamingDecoder for %s %s (%d data carriers)\n",
               modulationToString(cfg.modulation), codeRateToString(cfg.code_rate),
               cfg.getDataCarriers());
    } else if (cfg.waveform == protocol::WaveformMode::MC_DPSK) {
        decoder.setMCDPSKCarriers(cfg.mc_dpsk_carriers);
        // MC-DPSK always uses DQPSK R1/4 per protocol, but still need to configure
        // interleaver via setDataMode()
        decoder.setDataMode(Modulation::DQPSK, CodeRate::R1_4);
        printf("RX: Configured StreamingDecoder for MC-DPSK (%d carriers)\n",
               cfg.mc_dpsk_carriers);
    }

    // Set up callback for decoded frames
    decoder.setFrameCallback([&](const DecodeResult& result) {
        if (result.success && result.frame_type == v2::FrameType::DATA) {
            // Parse the frame to get sequence number
            if (result.frame_data.size() >= 4) {
                auto hdr = v2::parseHeader(result.frame_data);
                if (hdr.valid && hdr.type == v2::FrameType::DATA) {
                    // Extract seq from data frame
                    if (result.frame_data.size() >= 6) {
                        uint16_t seq = (result.frame_data[4] << 8) | result.frame_data[5];
                        decoded_seqs.insert(seq);
                        if (cfg.verbose) {
                            printf("  DECODED seq=%d (SNR=%.1f, CFO=%.1f)\n",
                                   seq, result.snr_db, result.cfo_hz);
                        }
                    }
                }
            }
        }
    });

    // Feed audio at ~real-time rate
    const size_t CHUNK_SIZE = 4800;  // 100ms chunks
    const size_t SAMPLE_RATE = 48000;

    printf("Feeding %zu samples (%.1f sec)...\n",
           audio.size(), audio.size() / (float)SAMPLE_RATE);

    size_t fed = 0;
    while (fed < audio.size()) {
        size_t chunk = std::min(CHUNK_SIZE, audio.size() - fed);
        decoder.feedAudio(audio.data() + fed, chunk);
        fed += chunk;

        // Process and simulate real-time
        decoder.processBuffer();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Final processing
    for (int i = 0; i < 20; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        decoder.processBuffer();
    }

    return decoded_seqs;
}

// ============================================================================
// MAIN
// ============================================================================

void printUsage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --snr N       SNR in dB (default: 20)\n");
    printf("  --cfo N       CFO in Hz (default: 0)\n");
    printf("  --channel T   Channel type:\n");
    printf("                  awgn     - Pure AWGN (default)\n");
    printf("                  good     - 0.5ms delay, 0.1Hz Doppler\n");
    printf("                  moderate - 1.0ms delay, 0.5Hz Doppler\n");
    printf("                  poor     - 2.0ms delay, 1.0Hz Doppler\n");
    printf("                  flutter  - 0.5ms delay, 10Hz Doppler\n");
    printf("  -w TYPE       Waveform: ofdm_chirp, mc_dpsk (default: ofdm_chirp)\n");
    printf("  --rate RATE   Code rate: r1_4, r1_2, r2_3, r3_4 (default: r1_2)\n");
    printf("  --mod MOD     Modulation: dqpsk, d8psk, dbpsk, qpsk, bpsk (default: dqpsk)\n");
    printf("  --frames N    Number of frames (default: 3)\n");
    printf("  --carriers N  MC-DPSK carriers (default: 8)\n");
    printf("  --seed N      Random seed (default: 42)\n");
    printf("  --save        Save signals to files\n");
    printf("  --session     Session mode: first frame full preamble, rest light\n");
    printf("  -q            Quiet mode\n");
    printf("  -h            Show this help\n");
}

int main(int argc, char** argv) {
    TestConfig cfg;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        else if (arg == "--snr" && i + 1 < argc) {
            cfg.snr_db = std::stof(argv[++i]);
        }
        else if (arg == "--cfo" && i + 1 < argc) {
            cfg.cfo_hz = std::stof(argv[++i]);
        }
        else if (arg == "--channel" && i + 1 < argc) {
            cfg.channel_type = argv[++i];
        }
        else if (arg == "-w" && i + 1 < argc) {
            std::string w = argv[++i];
            if (w == "ofdm_chirp") cfg.waveform = protocol::WaveformMode::OFDM_CHIRP;
            else if (w == "mc_dpsk" || w == "dpsk") cfg.waveform = protocol::WaveformMode::MC_DPSK;
            else {
                fprintf(stderr, "Unknown waveform: %s\n", w.c_str());
                return 1;
            }
        }
        else if (arg == "--rate" && i + 1 < argc) {
            std::string r = argv[++i];
            if (r == "r1_4") cfg.code_rate = CodeRate::R1_4;
            else if (r == "r1_2") cfg.code_rate = CodeRate::R1_2;
            else if (r == "r2_3") cfg.code_rate = CodeRate::R2_3;
            else if (r == "r3_4") cfg.code_rate = CodeRate::R3_4;
            else {
                fprintf(stderr, "Unknown rate: %s\n", r.c_str());
                return 1;
            }
        }
        else if (arg == "--mod" && i + 1 < argc) {
            std::string m = argv[++i];
            if (m == "dqpsk") cfg.modulation = Modulation::DQPSK;
            else if (m == "d8psk") cfg.modulation = Modulation::D8PSK;
            else if (m == "dbpsk") cfg.modulation = Modulation::DBPSK;
            else if (m == "qpsk") cfg.modulation = Modulation::QPSK;
            else if (m == "bpsk") cfg.modulation = Modulation::BPSK;
            else {
                fprintf(stderr, "Unknown modulation: %s\n", m.c_str());
                return 1;
            }
        }
        else if (arg == "--frames" && i + 1 < argc) {
            cfg.num_frames = std::stoi(argv[++i]);
        }
        else if (arg == "--carriers" && i + 1 < argc) {
            cfg.mc_dpsk_carriers = std::stoi(argv[++i]);
        }
        else if (arg == "--seed" && i + 1 < argc) {
            cfg.seed = std::stoul(argv[++i]);
        }
        else if (arg == "--save") {
            cfg.save_signals = true;
        }
        else if (arg == "--session") {
            cfg.session_mode = true;
        }
        else if (arg == "-q") {
            cfg.verbose = false;
        }
    }

    // MC-DPSK always uses R1/4 per protocol
    if (cfg.waveform == protocol::WaveformMode::MC_DPSK) {
        cfg.code_rate = CodeRate::R1_4;
        cfg.modulation = Modulation::DQPSK;
    }

    // Configure pilots based on code rate
    cfg.configurePilotsForCodeRate();

    // Print config
    cfg.print();

    std::mt19937 rng(cfg.seed);

    // Generate TX frames
    printf("Generating %d frames...\n", cfg.num_frames);
    std::vector<TxFrame> tx_frames;
    auto full_audio = generateFrames(cfg, rng, tx_frames);

    if (full_audio.empty()) {
        fprintf(stderr, "Failed to generate frames\n");
        return 1;
    }

    printf("Total audio: %.1f sec (%zu samples)\n",
           full_audio.size() / 48000.0f, full_audio.size());

    // Print frame positions
    for (const auto& frame : tx_frames) {
        printf("  Frame %d at %.2f sec (sample %zu, len %zu)\n",
               frame.seq, frame.audio_start / 48000.0f,
               frame.audio_start, frame.audio_len);
    }

    // Save original if requested
    if (cfg.save_signals) {
        std::string fname = cfg.save_prefix + "_original.f32";
        std::ofstream f(fname, std::ios::binary);
        f.write(reinterpret_cast<char*>(full_audio.data()), full_audio.size() * sizeof(float));
        printf("[SAVE] Original: %s\n", fname.c_str());
    }

    // Apply channel
    applyChannel(full_audio, cfg, rng);

    // Save after channel if requested
    if (cfg.save_signals) {
        std::string fname = cfg.save_prefix + "_channel.f32";
        std::ofstream f(fname, std::ios::binary);
        f.write(reinterpret_cast<char*>(full_audio.data()), full_audio.size() * sizeof(float));
        printf("[SAVE] After channel: %s\n", fname.c_str());
    }

    // Decode
    printf("\nDecoding...\n");
    auto decoded = decodeFrames(full_audio, cfg);

    // Results
    printf("\n=== RESULTS ===\n");
    printf("TX frames: %d\n", cfg.num_frames);
    printf("Decoded: %zu/%d (%.0f%%)\n",
           decoded.size(), cfg.num_frames,
           100.0f * decoded.size() / cfg.num_frames);

    for (const auto& frame : tx_frames) {
        bool ok = decoded.count(frame.seq) > 0;
        printf("  Frame %d: %s\n", frame.seq, ok ? "OK" : "MISSED");
    }

    return decoded.size() == (size_t)cfg.num_frames ? 0 : 1;
}
