// Decode benchmark tool — generates and decodes deterministic audio
// fixtures so AI agents and humans can measure the impact of decoder
// changes without running the full cli_simulator handshake/ARQ stack.
//
// Two modes:
//   gen   — synthesize an OFDM/MC-DPSK frame burst with optional AWGN,
//           save as 32-bit float WAV (listenable in Audacity/VLC).
//   bench — load a WAV, decode at faster-than-realtime, print the
//           DecoderProfile breakdown plus frame validation.
//
// Fixtures are committed to fixtures/ so every bench run measures
// bit-identical audio. Encoder is deterministic given a seed.

#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/timing_profiler.hpp"
#include "ultra/types.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace v2 = ultra::protocol::v2;
using ultra::Bytes;
using ultra::CodeRate;
using ultra::Modulation;
using ultra::gui::DecodeResult;
using ultra::gui::StreamingDecoder;
using ultra::gui::StreamingEncoder;
using ultra::protocol::WaveformMode;

// -------- WAV I/O (32-bit float mono, 48 kHz) -----------------------------

bool writeWavF32(const std::string& path, const std::vector<float>& samples,
                 int sample_rate = 48000) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    const uint32_t fmt_size = 16;
    const uint16_t audio_format = 3;  // IEEE float
    const uint16_t channels = 1;
    const uint16_t bits = 32;
    const uint32_t byte_rate = sample_rate * channels * bits / 8;
    const uint16_t block_align = channels * bits / 8;
    const uint32_t riff_size = 36 + data_bytes;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riff_size), 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    out.write(reinterpret_cast<const char*>(&fmt_size), 4);
    out.write(reinterpret_cast<const char*>(&audio_format), 2);
    out.write(reinterpret_cast<const char*>(&channels), 2);
    out.write(reinterpret_cast<const char*>(&sample_rate), 4);
    out.write(reinterpret_cast<const char*>(&byte_rate), 4);
    out.write(reinterpret_cast<const char*>(&block_align), 2);
    out.write(reinterpret_cast<const char*>(&bits), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&data_bytes), 4);
    out.write(reinterpret_cast<const char*>(samples.data()), data_bytes);
    return out.good();
}

bool readWavF32(const std::string& path, std::vector<float>& samples,
                int& sample_rate) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char riff[4], wave[4], chunk_id[4];
    uint32_t riff_size = 0, chunk_size = 0;
    uint16_t audio_format = 0, channels = 0, bits = 0, block_align = 0;
    uint32_t byte_rate = 0;
    in.read(riff, 4);
    in.read(reinterpret_cast<char*>(&riff_size), 4);
    in.read(wave, 4);
    if (std::strncmp(riff, "RIFF", 4) != 0 || std::strncmp(wave, "WAVE", 4) != 0) {
        std::cerr << "Not a WAV file: " << path << "\n";
        return false;
    }
    bool got_fmt = false, got_data = false;
    while (in.read(chunk_id, 4) && in.read(reinterpret_cast<char*>(&chunk_size), 4)) {
        if (std::strncmp(chunk_id, "fmt ", 4) == 0) {
            uint32_t fmt_size = chunk_size;
            in.read(reinterpret_cast<char*>(&audio_format), 2);
            in.read(reinterpret_cast<char*>(&channels), 2);
            in.read(reinterpret_cast<char*>(&sample_rate), 4);
            in.read(reinterpret_cast<char*>(&byte_rate), 4);
            in.read(reinterpret_cast<char*>(&block_align), 2);
            in.read(reinterpret_cast<char*>(&bits), 2);
            if (fmt_size > 16) in.seekg(fmt_size - 16, std::ios::cur);
            got_fmt = true;
        } else if (std::strncmp(chunk_id, "data", 4) == 0) {
            if (audio_format != 3 || bits != 32 || channels != 1) {
                std::cerr << "Expected 32-bit float mono WAV; got format="
                          << audio_format << " bits=" << bits
                          << " channels=" << channels << "\n";
                return false;
            }
            const size_t n = chunk_size / sizeof(float);
            samples.resize(n);
            in.read(reinterpret_cast<char*>(samples.data()), chunk_size);
            got_data = true;
            break;
        } else {
            in.seekg(chunk_size, std::ios::cur);
        }
    }
    return got_fmt && got_data;
}

// -------- Argument parsing -------------------------------------------------

struct Args {
    std::string mode;          // "gen" or "bench"
    std::string wav_path;
    std::string waveform = "ofdm_chirp";
    std::string code_rate = "r1_4";
    std::string modulation = "dqpsk";
    float snr_db = 100.0f;     // 100 = effectively no noise
    uint32_t seed = 1;
    int payload_bytes = 256;   // info payload per frame
    int num_frames = 4;        // burst size
    std::string text;          // optional readable payload (gen mode)
    std::string preamble = "chirp";  // "chirp" (full chirp+LTS) or "light" (LTS only)
};

WaveformMode parseWaveform(const std::string& s) {
    if (s == "ofdm_chirp") return WaveformMode::OFDM_CHIRP;
    if (s == "ofdm_cox") return WaveformMode::OFDM_COX;
    if (s == "ofdm_narrow") return WaveformMode::OFDM_NARROW;
    if (s == "mc_dpsk") return WaveformMode::MC_DPSK;
    return WaveformMode::OFDM_CHIRP;
}

CodeRate parseCodeRate(const std::string& s) {
    if (s == "r1_4") return CodeRate::R1_4;
    if (s == "r1_2") return CodeRate::R1_2;
    if (s == "r2_3") return CodeRate::R2_3;
    if (s == "r3_4") return CodeRate::R3_4;
    return CodeRate::R1_4;
}

Modulation parseModulation(const std::string& s) {
    if (s == "dqpsk") return Modulation::DQPSK;
    if (s == "qpsk") return Modulation::QPSK;
    if (s == "d8psk") return Modulation::D8PSK;
    if (s == "dbpsk") return Modulation::DBPSK;
    return Modulation::DQPSK;
}

void printUsage() {
    std::cout <<
        "decode_bench --mode gen|bench --wav <path> [options]\n"
        "\n"
        "Common:\n"
        "  --waveform <ofdm_chirp|ofdm_cox|ofdm_narrow|mc_dpsk>  default: ofdm_chirp\n"
        "  --rate <r1_4|r1_2|r2_3|r3_4>                          default: r1_4\n"
        "  --mod <dqpsk|qpsk|d8psk|dbpsk>                        default: dqpsk\n"
        "\n"
        "gen options:\n"
        "  --snr <db>            AWGN target SNR (default: 100 = no noise)\n"
        "  --seed <N>            RNG seed for both payload + AWGN (default: 1)\n"
        "  --payload <N>         Bytes of info per frame (default: 256)\n"
        "  --frames <N>          Number of frames in the burst (default: 4)\n"
        "  --text <string>       Use string as payload (repeats to fill); makes\n"
        "                        the decoded bytes human-readable. Overrides RNG payload.\n"
        "\n"
        "Examples:\n"
        "  decode_bench --mode gen --wav fixtures/ofdm_chirp_r14_snr15_awgn.wav \\\n"
        "               --rate r1_4 --snr 15 --frames 4 --seed 1\n"
        "  decode_bench --mode bench --wav fixtures/ofdm_chirp_r14_snr15_awgn.wav \\\n"
        "               --rate r1_4\n";
}

bool parseArgs(int argc, char** argv, Args& a) {
    auto need = [&](int& i) -> std::optional<std::string> {
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << argv[i] << "\n";
            return std::nullopt;
        }
        return std::string(argv[++i]);
    };
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { printUsage(); std::exit(0); }
        else if (arg == "--mode")     { auto v = need(i); if (!v) return false; a.mode = *v; }
        else if (arg == "--wav")      { auto v = need(i); if (!v) return false; a.wav_path = *v; }
        else if (arg == "--waveform") { auto v = need(i); if (!v) return false; a.waveform = *v; }
        else if (arg == "--rate")     { auto v = need(i); if (!v) return false; a.code_rate = *v; }
        else if (arg == "--mod")      { auto v = need(i); if (!v) return false; a.modulation = *v; }
        else if (arg == "--snr")      { auto v = need(i); if (!v) return false; a.snr_db = std::stof(*v); }
        else if (arg == "--seed")     { auto v = need(i); if (!v) return false; a.seed = static_cast<uint32_t>(std::stoul(*v)); }
        else if (arg == "--payload")  { auto v = need(i); if (!v) return false; a.payload_bytes = std::stoi(*v); }
        else if (arg == "--frames")   { auto v = need(i); if (!v) return false; a.num_frames = std::stoi(*v); }
        else if (arg == "--text")     { auto v = need(i); if (!v) return false; a.text = *v; }
        else if (arg == "--preamble") { auto v = need(i); if (!v) return false; a.preamble = *v; }
        else { std::cerr << "Unknown option: " << arg << "\n"; return false; }
    }
    if (a.mode != "gen" && a.mode != "bench") {
        std::cerr << "Need --mode gen|bench\n"; return false;
    }
    if (a.wav_path.empty()) {
        std::cerr << "Need --wav <path>\n"; return false;
    }
    return true;
}

// -------- Channel: AWGN at target SNR -------------------------------------

void applyAWGN(std::vector<float>& samples, float snr_db, uint32_t seed) {
    if (snr_db >= 80.0f) return;  // sentinel for "no noise"

    // Signal power = mean(s^2) over non-zero samples. Encoder output is
    // ~peak-normalized; we measure RMS to get a stable scale.
    double sig_pow = 0.0;
    size_t n = 0;
    for (float s : samples) {
        if (std::abs(s) > 1e-6f) {
            sig_pow += static_cast<double>(s) * s;
            ++n;
        }
    }
    if (n == 0) return;
    sig_pow /= static_cast<double>(n);

    const double snr_linear = std::pow(10.0, snr_db / 10.0);
    const double noise_pow = sig_pow / snr_linear;
    const double sigma = std::sqrt(noise_pow);

    std::mt19937 rng(seed);
    std::normal_distribution<float> nz(0.0f, static_cast<float>(sigma));
    for (float& s : samples) {
        s += nz(rng);
    }
}

// -------- gen mode --------------------------------------------------------

// Build the OFDM config used by both bench encoder and decoder.
// MUST match what production code uses post-handshake — otherwise
// fixtures generated by the bench can't be decoded by ultra_gui /
// ultra_tnc and vice-versa. Production path:
//   ModemEngine has a default ModemConfig (cp=MEDIUM, use_pilots=false,
//   spacing=2). On setConnected it sets use_pilots=true and
//   pilot_spacing=recommended (10 for DQPSK R1/4). cp_mode stays
//   MEDIUM. setWaveformMode propagates the same config to the
//   encoder via setOFDMConfig(), so encoder and decoder agree.
//
// We mirror that here. cp_mode=MEDIUM is the critical one — using
// LONG (the StreamingEncoder constructor default, which gets
// overridden in production) yields fixtures that decode in the
// bench but NOT in the GUI's monitor mode or in any production
// receiver.
ultra::ModemConfig benchOFDMConfig() {
    ultra::ModemConfig c;
    c.fft_size = 1024;
    c.num_carriers = 59;
    c.sample_rate = 48000;
    c.center_freq = 1500;
    c.cp_mode = ultra::CyclicPrefixMode::MEDIUM;
    c.use_pilots = true;
    c.pilot_spacing = 10;
    return c;
}

int runGen(const Args& a) {
    StreamingEncoder enc;
    enc.setMode(parseWaveform(a.waveform));
    enc.setOFDMConfig(benchOFDMConfig());
    enc.setDataMode(parseModulation(a.modulation), parseCodeRate(a.code_rate));
    // Bench targets the connected-mode 4-CW fixed-frame data path —
    // that's the throughput hot path agents will be optimizing.
    enc.setFixedFrameCodewords(4);
    // Channel interleave defaults to true on both encoder and decoder.
    // Match the default so fixtures are decodable by anything that
    // hasn't explicitly overridden — including the GUI in monitor mode
    // and any real-radio receiver running the production defaults.

    // Cap payload to fixed-frame capacity so the encoder doesn't spill
    // into multi-frame fragmentation. We want a deterministic single-
    // frame burst per iteration.
    const size_t cap = v2::getFixedFramePayloadCapacity(parseCodeRate(a.code_rate), 4);
    const size_t payload_bytes = std::min(static_cast<size_t>(a.payload_bytes), cap);

    std::cout << "[gen] waveform=" << a.waveform
              << " rate=" << a.code_rate
              << " mod=" << a.modulation
              << " snr=" << a.snr_db
              << " frames=" << a.num_frames
              << " payload=" << payload_bytes << " bytes/frame (capacity=" << cap << ")"
              << " seed=" << a.seed << "\n";

    // Deterministic payload: derive bytes from a seeded LCG so the same
    // (seed, payload_bytes) always yields identical input.
    std::mt19937 rng(a.seed);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    std::vector<float> all_samples;
    all_samples.reserve(static_cast<size_t>(a.num_frames) * 24000);  // rough preallocate

    for (int f = 0; f < a.num_frames; ++f) {
        Bytes payload(payload_bytes);
        if (!a.text.empty()) {
            // Repeat the supplied text across the payload so the decoded
            // bytes spell out a human-readable message. Easier to verify
            // an OTA test by eye than diffing pseudo-random bytes.
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<uint8_t>(a.text[i % a.text.size()]);
            }
        } else {
            for (auto& b : payload) b = static_cast<uint8_t>(byte_dist(rng));
        }

        // Use v2::makeFixedDataFrame so total_cw is explicitly set to
        // 4. DataFrame::makeData() calls calculateCodewords() which for
        // a 60-byte payload at R1/4 returns 5 CWs (continuation CWs
        // reserve DATA_CW_HEADER_SIZE bytes). The OFDM encoder trusts
        // byte 12 of the serialized frame and frame-interleaves over
        // that count — if it's 5 while the decoder expects 4, the
        // de-interleave permutation is wrong and LDPC fails on every
        // CW with saturated-but-wrong-position bits. (Codex review.)
        auto frame = v2::makeFixedDataFrame(
            "BENCH1", "BENCH2", static_cast<uint16_t>(f), payload,
            parseCodeRate(a.code_rate), /*cw_count=*/4);
        Bytes serialized = frame.serialize();

        // Preamble selection:
        //   "chirp" — full chirp + LTS, decodable by an idle receiver
        //   in disconnected mode (what the GUI does by default, what
        //   any real-radio OTA listener expects when not in session).
        //   This is the right default for OTA fixtures.
        //   "light" — LTS-only, smaller per-frame overhead. Decodable
        //   only by a receiver already in connected mode (e.g. our
        //   bench harness).
        std::vector<float> frame_samples;
        if (a.preamble == "light") {
            frame_samples = enc.encodeFrameLight(serialized);
        } else {
            frame_samples = enc.encodeFrame(serialized);
        }
        all_samples.insert(all_samples.end(), frame_samples.begin(), frame_samples.end());

        // ~50 ms gap between frames so the decoder sees clean boundaries.
        all_samples.insert(all_samples.end(), 2400, 0.0f);
    }

    // Decoder needs ≥2.5 s of buffered audio before it begins LTS
    // correlation search. Pad with silence so a small payload doesn't
    // get stuck in "not enough samples" forever.
    constexpr size_t kMinTrailingSilenceSamples = 48000 * 3;
    if (all_samples.size() < kMinTrailingSilenceSamples) {
        all_samples.insert(all_samples.end(),
                           kMinTrailingSilenceSamples - all_samples.size(),
                           0.0f);
    } else {
        all_samples.insert(all_samples.end(), 24000, 0.0f);  // 0.5 s tail
    }

    std::cout << "[gen] synthesized " << all_samples.size() << " samples ("
              << std::fixed << static_cast<double>(all_samples.size()) / 48000.0
              << " s)\n";

    if (a.snr_db < 80.0f) {
        // Apply AWGN with a deterministic offset of the seed so two
        // fixtures at different SNR but same seed don't share noise.
        applyAWGN(all_samples, a.snr_db, a.seed ^ 0xA5A5A5A5u);
        std::cout << "[gen] applied AWGN at " << a.snr_db << " dB\n";
    } else {
        std::cout << "[gen] noiseless (SNR>=80)\n";
    }

    if (!writeWavF32(a.wav_path, all_samples)) {
        std::cerr << "Failed to write " << a.wav_path << "\n";
        return 1;
    }
    std::cout << "[gen] wrote " << a.wav_path << "\n";
    return 0;
}

// -------- bench mode ------------------------------------------------------

int runBench(const Args& a) {
    std::vector<float> samples;
    int sr = 0;
    if (!readWavF32(a.wav_path, samples, sr)) {
        std::cerr << "Failed to read " << a.wav_path << "\n";
        return 1;
    }
    if (sr != 48000) {
        std::cerr << "Expected 48 kHz, got " << sr << "\n";
        return 1;
    }
    std::cout << "[bench] loaded " << samples.size() << " samples ("
              << static_cast<double>(samples.size()) / 48000.0 << " s) from "
              << a.wav_path << "\n";

    StreamingDecoder dec;
    // Configure connected-mode OFDM with the SAME ModemConfig the
    // encoder used. A mismatched cp_mode / pilot layout makes the
    // decoder's LTS template diverge from the encoder's preamble and
    // correlation collapses (saw 0.24 on default ModemConfig). With
    // matching configs the LTS template + data demod line up and the
    // fixed-frame data path actually fires.
    //
    // Each fixture frame has a full chirp preamble. Connected mode
    // ordinarily uses LTS-only sync, but the chirp preamble in our
    // fixture also embeds the same LTS the data preamble uses, so
    // connected-mode LTS sync still locks on each frame.
    dec.setConnectedOFDMMode(parseWaveform(a.waveform), benchOFDMConfig(),
                             parseModulation(a.modulation),
                             parseCodeRate(a.code_rate));
    dec.setFixedFrameCodewords(4);
    // Match encoder default (channel_interleave=true) so the bench
    // self-test agrees with how production receivers interpret the
    // fixture. Forcing false here would diverge from the GUI/TNC
    // defaults and turn decoded output into bit-permuted garbage.
    dec.setKnownCFO(0.0f);
    dec.clearShutdown();

    int frames_decoded = 0;
    int frames_failed = 0;
    std::vector<std::string> decoded_payloads;
    dec.setFrameCallback([&](const DecodeResult& r) {
        if (r.success) {
            ++frames_decoded;
            // Try to extract the readable payload from a v2::DataFrame.
            // For text fixtures this gives an immediate eyeball check
            // that decode actually produced the right bytes.
            if (auto df = v2::DataFrame::deserialize(r.frame_data)) {
                std::string text(df->payload.begin(), df->payload.end());
                decoded_payloads.push_back(std::move(text));
            }
        } else {
            ++frames_failed;
        }
    });

    // Reset profiling counters so we measure only this decode.
    ultra::timing::globalDecoderProfile().reset();

    // Spawn a worker thread that drives processBuffer() repeatedly,
    // matching how production code uses the decoder. We then feed all
    // samples and wait until either: (a) all expected frames decoded,
    // or (b) a quiet period elapses with no new frame deliveries.
    std::atomic<bool> stop_worker{false};
    std::thread worker([&]() {
        while (!stop_worker.load(std::memory_order_acquire)) {
            dec.processBuffer();
        }
    });

    const auto wall_start = std::chrono::steady_clock::now();

    // Feed in small chunks so each chunk re-arms new_data_available_,
    // matching how the audio thread feeds in production. processBuffer
    // is a single-step state machine that consumes one wakeup per call;
    // dumping all audio at once would only fire SEARCHING, then time
    // out. We pace at faster-than-realtime (~10× audio rate) so the
    // decoder thread has room to advance through SEARCHING→SYNC_FOUND→
    // DECODING between chunks but the bench still finishes quickly.
    constexpr size_t kChunkSamples = 4096;     // ~85 ms at 48 kHz
    constexpr int kInterChunkSleepMs = 8;      // ~10× realtime feed
    for (size_t off = 0; off < samples.size(); off += kChunkSamples) {
        const size_t chunk = std::min(kChunkSamples, samples.size() - off);
        dec.feedAudio(samples.data() + off, chunk);
        std::this_thread::sleep_for(std::chrono::milliseconds(kInterChunkSleepMs));
    }
    // Drip silence after the real audio so the decoder's state machine
    // gets new_data_available_ wakeups to advance from SYNC_FOUND →
    // DECODING after the last frame arrives. Without this the worker
    // sits in its 50 ms timeout loop and never finishes the last frame.
    std::vector<float> tail_silence(kChunkSamples, 0.0f);
    for (int i = 0; i < 25; ++i) {  // ~2 s of silence wakeups
        dec.feedAudio(tail_silence);
        std::this_thread::sleep_for(std::chrono::milliseconds(kInterChunkSleepMs));
    }

    // Wait until decode quiesces. "Quiet" = no new frame for 200 ms
    // AND no samples remaining in the input buffer. Cap total wait at
    // 5 s so a busted fixture doesn't hang the bench.
    const auto poll_start = std::chrono::steady_clock::now();
    int last_total = -1;
    auto last_change = poll_start;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const int total = frames_decoded + frames_failed;
        const auto now = std::chrono::steady_clock::now();
        if (total != last_total) {
            last_total = total;
            last_change = now;
        }
        const auto since_change = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_change).count();
        const auto since_start = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - poll_start).count();
        const bool buffer_empty = dec.samplesInBuffer() == 0;
        if (buffer_empty && since_change > 200) break;
        if (since_start > 5000) break;
    }

    stop_worker.store(true, std::memory_order_release);
    dec.stop();          // wakes processBuffer() out of any blocking wait
    worker.join();

    const auto wall_end = std::chrono::steady_clock::now();
    const double wall_ms =
        std::chrono::duration<double, std::milli>(wall_end - wall_start).count();

    // ----- Print profile -----
    auto& dp = ultra::timing::globalDecoderProfile();
    auto fmt = [](const ultra::timing::PhaseStats& s) -> std::string {
        const uint64_t cnt = s.count.load();
        const uint64_t tot = s.total_us.load();
        const uint64_t mx  = s.max_us.load();
        if (cnt == 0) return "(0 calls)";
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                 "n=%llu  total=%.2fms  mean=%.1fus  max=%lluus",
                 static_cast<unsigned long long>(cnt),
                 tot / 1000.0,
                 static_cast<double>(tot) / static_cast<double>(cnt),
                 static_cast<unsigned long long>(mx));
        return std::string(buf);
    };

    std::cout << "\n=== decode_bench results ===\n";
    std::cout << "wall_clock_ms             " << wall_ms << "\n";
    std::cout << "frames_decoded            " << frames_decoded << "\n";
    std::cout << "frames_failed             " << frames_failed << "\n";

    if (!decoded_payloads.empty()) {
        // Print the first decoded payload verbatim — eyeball check
        // for text fixtures. Drop non-printable bytes so a mangled
        // payload doesn't garble the terminal.
        std::cout << "first_decoded_payload     \"";
        for (char c : decoded_payloads.front()) {
            if (c >= 0x20 && c < 0x7F) std::cout << c;
            else std::cout << '.';
        }
        std::cout << "\"\n";
    }
    std::cout << "\n--- DecoderProfile (this decode) ---\n";
    std::cout << "  detect_data_sync          " << fmt(dp.detect_data_sync) << "\n";
    std::cout << "  ofdm_process_total        " << fmt(dp.ofdm_process_total) << "\n";
    std::cout << "  lts_channel_estimate      " << fmt(dp.lts_channel_estimate) << "\n";
    std::cout << "  data_symbol_loop          " << fmt(dp.data_symbol_loop) << "\n";
    std::cout << "  decode_fixed_frame_total  " << fmt(dp.decode_fixed_frame_total) << "\n";
    std::cout << "  ldpc_cw_total             " << fmt(dp.ldpc_cw_total) << "\n";
    std::cout << "  single_cw_decode_total    " << fmt(dp.single_cw_decode_total) << "\n";
    std::cout << "  control_first_1cw         " << fmt(dp.control_first_1cw) << "\n";
    std::cout << "  cw0_peek_1cw              " << fmt(dp.cw0_peek_1cw) << "\n";
    std::cout << "  ofdm_cw0_probe_decode     " << fmt(dp.ofdm_cw0_probe_decode) << "\n";
    std::cout << "  failed_4cw_after_peek     " << fmt(dp.failed_4cw_after_peek) << "\n";
    std::cout << "  low_llr_escalation_skipped count=" << dp.low_llr_escalation_skipped.load() << "\n";
    std::cout << "  raw_cw0_probe_skipped     count=" << dp.raw_cw0_probe_skipped.load() << "\n";
    std::cout << "  harq_key_build            success=" << dp.harq_key_build_success.load()
              << "  failed=" << dp.harq_key_build_failed.load() << "\n";

    return frames_failed > 0 ? 2 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parseArgs(argc, argv, a)) {
        printUsage();
        return 1;
    }
    if (a.mode == "gen") return runGen(a);
    if (a.mode == "bench") return runBench(a);
    return 1;
}
