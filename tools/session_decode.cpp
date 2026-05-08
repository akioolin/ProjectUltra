// Session-aware offline OTA decoder.
//
// WAV path: shared RIFF/WAVE reader for PCM s16, PCM s24, and IEEE float32,
// downmixed to mono and resampled to 48 kHz with the repo Resampler. The tool
// does not normalize or auto-gain unless --rms-target is explicitly supplied.

#include "io/wav_io.hpp"
#include "sim/simulated_station.hpp"

#include "sync/chirp_sync.hpp"
#include "ultra/dsp.hpp"
#include "ultra/timing_profiler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kTargetSampleRate = 48000;

struct Args {
    std::string wav_path;
    std::string callsign = "BRAVO";
    // Offline replay auto-accepts CONNECT by default. --auto-accept is kept
    // as a backward-compatible no-op for existing capture-replay commands.
    bool auto_accept = true;
    int decode_drain_ms = 3000;
    std::optional<float> rms_target_dbfs;
};

struct RmsNormalizeReport {
    bool requested = false;
    float target_dbfs = 0.0f;
    float active_rms = 0.0f;
    float gain_db = 0.0f;
    float peak_before = 0.0f;
    float peak_after = 0.0f;
    bool applied = false;
    bool skipped_would_clip = false;
};

struct LoadedWav {
    std::string path;
    std::vector<float> samples_48k;
    uint32_t source_rate = 0;
    uint16_t source_channels = 0;
    uint16_t source_bits = 0;
    uint16_t source_format = 0;
    RmsNormalizeReport rms_normalize;
};

struct ChirpObservation {
    bool found = false;
    float corr = 0.0f;
    float up_corr = 0.0f;
    float down_corr = 0.0f;
    float cfo_hz = 0.0f;
    int sample_offset = -1;
};

struct FrameSummary {
    int control = 0;
    int data = 0;
    int disconnect = 0;
    int data_byte_exact = 0;
    std::vector<size_t> data_payload_bytes;
    std::vector<std::string> frame_types;

    struct DataFrameMetrics {
        float lts_snr_db = 0.0f;
        float fading = 0.0f;
        float sync_corr = 0.0f;
        float residual_cfo_hz = 0.0f;
    };
    std::vector<DataFrameMetrics> data_metrics;
};

void printUsage() {
    std::cout <<
        "session_decode --wav <file.wav> [--callsign BRAVO] "
        "[--auto-accept (default/no-op)] [--decode-drain-ms 3000] "
        "[--rms-target -16]\n";
}

std::optional<std::string> needValue(int& i, int argc, char** argv) {
    if (i + 1 >= argc) {
        std::cerr << "Missing value for " << argv[i] << "\n";
        return std::nullopt;
    }
    return std::string(argv[++i]);
}

bool parseArgs(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            std::exit(0);
        } else if (arg == "--wav") {
            auto v = needValue(i, argc, argv);
            if (!v) return false;
            args.wav_path = *v;
        } else if (arg == "--callsign") {
            auto v = needValue(i, argc, argv);
            if (!v) return false;
            args.callsign = *v;
        } else if (arg == "--auto-accept") {
            // Backward-compatible no-op: offline decode already auto-accepts.
            args.auto_accept = true;
        } else if (arg == "--decode-drain-ms") {
            auto v = needValue(i, argc, argv);
            if (!v) return false;
            args.decode_drain_ms = std::max(0, std::stoi(*v));
        } else if (arg == "--rms-target") {
            auto v = needValue(i, argc, argv);
            if (!v) return false;
            args.rms_target_dbfs = std::stof(*v);
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }

    if (args.wav_path.empty()) {
        std::cerr << "Need --wav <file.wav>\n";
        return false;
    }
    args.callsign = ultra::protocol::sanitizeCallsign(args.callsign);
    if (args.callsign.empty()) {
        std::cerr << "Invalid --callsign\n";
        return false;
    }
    return true;
}

float medianValue(std::vector<float> values) {
    if (values.empty()) return 0.0f;
    const size_t mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    float median = values[mid];
    if ((values.size() % 2) == 0) {
        std::nth_element(values.begin(), values.begin() + mid - 1, values.end());
        median = 0.5f * (median + values[mid - 1]);
    }
    return median;
}

struct ActiveRegionStats {
    float rms = 0.0f;
    float peak = 0.0f;
};

ActiveRegionStats activeRegionStats(const std::vector<float>& samples) {
    if (samples.empty()) return {};

    constexpr size_t kActiveWindowSamples = kTargetSampleRate / 5;  // 200 ms
    struct Window {
        size_t start = 0;
        size_t len = 0;
        float rms = 0.0f;
    };

    std::vector<Window> windows;
    windows.reserve((samples.size() + kActiveWindowSamples - 1) / kActiveWindowSamples);
    for (size_t start = 0; start < samples.size(); start += kActiveWindowSamples) {
        const size_t len = std::min(kActiveWindowSamples, samples.size() - start);
        double sum_sq = 0.0;
        for (size_t i = 0; i < len; ++i) {
            const double s = samples[start + i];
            sum_sq += s * s;
        }
        windows.push_back({start, len, static_cast<float>(std::sqrt(sum_sq / len))});
    }

    std::vector<float> window_rms;
    window_rms.reserve(windows.size());
    for (const auto& w : windows) {
        window_rms.push_back(w.rms);
    }
    const float median_rms = medianValue(std::move(window_rms));
    const float active_threshold = 3.0f * median_rms;

    std::vector<bool> active(windows.size(), false);
    size_t active_count = 0;
    for (size_t i = 0; i < windows.size(); ++i) {
        active[i] = windows[i].rms > active_threshold;
        if (active[i]) ++active_count;
    }

    std::vector<bool> keep(windows.size(), false);
    for (size_t i = 0; i < windows.size();) {
        if (!active[i]) {
            ++i;
            continue;
        }
        const size_t run_start = i;
        while (i < windows.size() && active[i]) ++i;
        const size_t run_len = i - run_start;
        if (run_len >= 2) {
            for (size_t j = run_start; j < i; ++j) keep[j] = true;
        }
    }

    if (std::none_of(keep.begin(), keep.end(), [](bool v) { return v; })) {
        if (active_count > 0) {
            keep = active;
        } else {
            for (size_t i = 0; i < windows.size(); ++i) {
                keep[i] = windows[i].rms > 0.0f;
            }
        }
    }

    ActiveRegionStats stats;
    double sum_sq = 0.0;
    size_t count = 0;
    for (size_t w = 0; w < windows.size(); ++w) {
        if (!keep[w]) continue;
        for (size_t i = 0; i < windows[w].len; ++i) {
            const double s = samples[windows[w].start + i];
            sum_sq += s * s;
            stats.peak = std::max(stats.peak, static_cast<float>(std::abs(s)));
        }
        count += windows[w].len;
    }
    if (count == 0) return {};
    stats.rms = static_cast<float>(std::sqrt(sum_sq / static_cast<double>(count)));
    return stats;
}

void applyRmsTarget(std::vector<float>& samples, float target_dbfs, RmsNormalizeReport& report) {
    report.requested = true;
    report.target_dbfs = target_dbfs;
    const ActiveRegionStats active = activeRegionStats(samples);
    report.peak_before = active.peak;
    report.active_rms = active.rms;

    if (report.active_rms <= 0.0f) {
        report.peak_after = report.peak_before;
        return;
    }

    const float target_rms = std::pow(10.0f, target_dbfs / 20.0f);
    const float gain = target_rms / report.active_rms;
    report.gain_db = 20.0f * std::log10(std::max(gain, 1.0e-12f));
    report.peak_after = report.peak_before * gain;

    if (report.peak_after > 0.99f) {
        report.skipped_would_clip = true;
        return;
    }

    for (float& s : samples) {
        s *= gain;
    }
    report.applied = true;
}

LoadedWav loadWav(const std::string& path, std::optional<float> rms_target_dbfs) {
    auto loaded = ultra::tools::io::loadWavMono48k(path);

    LoadedWav wav;
    wav.path = std::move(loaded.path);
    wav.samples_48k = std::move(loaded.samples_48k);
    wav.source_rate = loaded.source_rate;
    wav.source_channels = loaded.source_channels;
    wav.source_bits = loaded.source_bits;
    wav.source_format = loaded.source_format;
    if (rms_target_dbfs) {
        applyRmsTarget(wav.samples_48k, *rms_target_dbfs, wav.rms_normalize);
    }
    return wav;
}

float rms(const std::vector<float>& samples) {
    if (samples.empty()) return 0.0f;
    double sum = 0.0;
    for (float s : samples) sum += static_cast<double>(s) * static_cast<double>(s);
    return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}

std::string dbfsString(float value) {
    float db = 20.0f * std::log10(std::max(value, 1.0e-12f));
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << db;
    return oss.str();
}

std::string yesNo(bool v) {
    return v ? "yes" : "no";
}

std::string fixedFloat(float value, int precision) {
    if (!std::isfinite(value)) return "nan";
    const float zero_epsilon = 0.5f * std::pow(10.0f, -static_cast<float>(precision));
    if (std::abs(value) < zero_epsilon) value = 0.0f;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}

ChirpObservation scanFirstChirp(const std::vector<float>& samples) {
    ChirpObservation best;
    if (samples.empty()) return best;

    ultra::sync::ChirpSync chirp;
    constexpr size_t kWindow = 120000;
    constexpr size_t kStep = 24000;  // 0.5 s; reporting only, decoder still does real search.
    const size_t min_needed = chirp.getTotalSamples();

    for (size_t off = 0; off + min_needed <= samples.size(); off += kStep) {
        const size_t len = std::min(kWindow, samples.size() - off);
        ultra::SampleSpan span(samples.data() + off, len);
        auto r = chirp.detectDualChirp(span, 0.15f);
        const float observed = std::max(r.up_correlation, r.down_correlation);
        if (observed > best.corr) {
            best.corr = observed;
            best.up_corr = r.up_correlation;
            best.down_corr = r.down_correlation;
        }
        if (r.success) {
            ChirpObservation obs;
            obs.found = true;
            obs.corr = std::min(r.up_correlation, r.down_correlation);
            obs.up_corr = r.up_correlation;
            obs.down_corr = r.down_correlation;
            obs.cfo_hz = r.cfo_hz;
            obs.sample_offset = static_cast<int>(off) + r.up_chirp_start;
            return obs;
        }
    }
    return best;
}

std::string dataBytesList(const std::vector<size_t>& bytes) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i) oss << ", ";
        oss << bytes[i];
    }
    oss << "]";
    return oss.str();
}

std::string sanitizeForLine(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (c == '\n' || c == '\r' || c == '\t') out.push_back(' ');
        else if (uc >= 0x20 && uc < 0x7F) out.push_back(c);
        else out.push_back('.');
    }
    return out;
}

class WavReplayAudioPort : public AudioPort {
public:
    WavReplayAudioPort(const std::string& wav_path,
                       int decode_drain_ms,
                       std::optional<float> rms_target_dbfs)
        : wav_(loadWav(wav_path, rms_target_dbfs)) {
        drain_samples_ = static_cast<size_t>(
            (static_cast<int64_t>(decode_drain_ms) * kTargetSampleRate) / 1000);
        total_samples_to_serve_ = wav_.samples_48k.size() + drain_samples_;
    }

    bool start() override {
        cursor_.store(0);
        stopped_.store(false);
        return true;
    }

    void stop() override {
        stopped_.store(true);
        cursor_.store(total_samples_to_serve_);
    }

    std::vector<float> pullRx(size_t count) override {
        std::vector<float> out(count, 0.0f);
        if (stopped_.load()) return out;

        const size_t start = cursor_.fetch_add(count);
        for (size_t i = 0; i < count; ++i) {
            const size_t idx = start + i;
            if (idx < wav_.samples_48k.size()) {
                out[i] = wav_.samples_48k[idx];
            }
        }
        return out;
    }

    void queueTx(const std::vector<float>&) override {}

    bool isDrained() const {
        return cursor_.load() >= total_samples_to_serve_;
    }

    const LoadedWav& wav() const { return wav_; }
    size_t drainSamples() const { return drain_samples_; }

private:
    LoadedWav wav_;
    size_t drain_samples_ = 0;
    size_t total_samples_to_serve_ = 0;
    std::atomic<size_t> cursor_{0};
    std::atomic<bool> stopped_{false};
};

void recordFrame(const ultra::gui::DecodeResult& result, FrameSummary& summary) {
    auto hdr = v2::parseHeader(result.frame_data);
    if (!hdr.valid) {
        summary.control++;
        summary.frame_types.push_back("UNKNOWN");
        return;
    }

    summary.frame_types.push_back(v2::frameTypeToString(hdr.type));
    if (hdr.type == v2::FrameType::DISCONNECT) {
        summary.disconnect++;
    } else if (v2::isDataFrame(hdr.type)) {
        summary.data++;
        auto df = v2::DataFrame::deserialize(result.frame_data);
        if (df) {
            summary.data_payload_bytes.push_back(df->payload.size());
            summary.data_byte_exact++;
        } else {
            summary.data_payload_bytes.push_back(hdr.payload_len);
        }
        summary.data_metrics.push_back({
            result.lts_snr_db,
            result.lts_fading_index,
            result.sync_correlation,
            result.lts_residual_cfo_hz
        });
    } else {
        summary.control++;
    }
}

void printSummary(const LoadedWav& wav,
                  const ChirpObservation& chirp,
                  const SimulatedStation& station,
                  ultra::protocol::WaveformMode observed_waveform,
                  ultra::Modulation observed_modulation,
                  ultra::CodeRate observed_code_rate,
                  int observed_cw_count,
                  bool ever_connected,
                  bool ever_handshake_complete,
                  const FrameSummary& frames,
                  const std::vector<std::string>& messages) {
    const float wav_rms = rms(wav.samples_48k);
    const double duration_s = static_cast<double>(wav.samples_48k.size()) /
                              static_cast<double>(kTargetSampleRate);
    const auto cs = station.getConnectionStats();
    const auto ds = station.getDecoderStats();

    std::cout << "\n=== session_decode summary ===\n";
    std::cout << "wav_path                  " << wav.path << "\n";
    std::cout << "wav_duration_s            " << std::fixed << std::setprecision(3)
              << duration_s << "\n";
    std::cout << "wav_rms_dbfs              " << dbfsString(wav_rms) << "\n";
    std::cout << "wav_source                " << wav.source_rate << " Hz, "
              << wav.source_channels << " ch, format=" << wav.source_format
              << ", bits=" << wav.source_bits << "\n";
    if (wav.rms_normalize.requested) {
        const auto& rn = wav.rms_normalize;
        std::cout << "rms_normalize: target_rms=" << fixedFloat(rn.target_dbfs, 1)
                  << " dBFS active_rms=" << dbfsString(rn.active_rms)
                  << " dBFS gain=" << fixedFloat(rn.gain_db, 1) << " dB\n";
        std::cout << "               peak_before=" << fixedFloat(rn.peak_before, 6)
                  << " peak_after=" << fixedFloat(rn.peak_after, 6)
                  << " applied=" << yesNo(rn.applied) << "\n";
        if (rn.skipped_would_clip) {
            std::cout << "rms_normalize_skipped: peak_after="
                      << fixedFloat(rn.peak_after, 6) << " would clip\n";
        }
    }
    std::cout << "connected                 " << yesNo(ever_connected) << "\n";
    std::cout << "handshake_complete        " << yesNo(ever_handshake_complete) << "\n";
    std::cout << "negotiated_waveform       "
              << ultra::protocol::waveformModeToString(observed_waveform) << "\n";
    std::cout << "negotiated_modulation     "
              << ultra::modulationToString(observed_modulation) << "\n";
    std::cout << "negotiated_code_rate      "
              << ultra::codeRateToString(observed_code_rate) << "\n";
    std::cout << "negotiated_cw_count       " << observed_cw_count << "\n";

    if (chirp.found) {
        std::cout << "first_sync                yes chirp_corr=" << std::setprecision(3)
                  << chirp.corr << " up=" << chirp.up_corr
                  << " down=" << chirp.down_corr
                  << " cfo_hz=" << std::fixed << std::setprecision(2) << chirp.cfo_hz
                  << " sample_offset=" << chirp.sample_offset << "\n";
    } else {
        std::cout << "first_sync                no chirp_corr=" << std::setprecision(3)
                  << chirp.corr << " up=" << chirp.up_corr
                  << " down=" << chirp.down_corr
                  << " cfo_hz=n/a sample_offset=n/a\n";
    }

    std::cout << "frames_control            " << frames.control << "\n";
    std::cout << "frames_data               " << frames.data
              << " bytes=" << dataBytesList(frames.data_payload_bytes)
              << " byte_exact=" << frames.data_byte_exact << "/" << frames.data << "\n";
    for (size_t i = 0; i < frames.data_metrics.size(); ++i) {
        const auto& m = frames.data_metrics[i];
        std::cout << "  frame[" << (i + 1) << "] lts_snr_db="
                  << fixedFloat(m.lts_snr_db, 1)
                  << " fading=" << fixedFloat(m.fading, 2)
                  << " sync_corr=" << fixedFloat(m.sync_corr, 2)
                  << " residual_cfo_hz=" << fixedFloat(m.residual_cfo_hz, 1)
                  << "\n";
    }
    std::cout << "frames_disconnect         " << frames.disconnect << "\n";
    std::cout << "messages_received         " << messages.size() << "\n";
    for (size_t i = 0; i < messages.size(); ++i) {
        std::cout << "message[" << (i + 1) << "]                 \""
                  << sanitizeForLine(messages[i]) << "\"\n";
    }
    std::cout << "arq_retransmissions       " << cs.arq.retransmissions << "\n";
    std::cout << "ldpc_failures             " << ds.frames_failed
              << " (decoder frame failures; CW failure count unavailable)\n";
    std::cout << "decoder_frames_decoded    " << ds.frames_decoded << "\n";
    std::cout << "decoder_frames_failed     " << ds.frames_failed << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parseArgs(argc, argv, args)) {
        printUsage();
        return 1;
    }

    try {
        auto port = std::make_unique<WavReplayAudioPort>(
            args.wav_path, args.decode_drain_ms, args.rms_target_dbfs);
        WavReplayAudioPort* replay = port.get();
        ChirpObservation chirp = scanFirstChirp(replay->wav().samples_48k);

        FrameSummary frames;
        std::vector<std::string> messages;
        std::mutex summary_mutex;

        SimulatedStation station(args.callsign, std::move(port));
        station.setAutoAccept(args.auto_accept);
        station.setDecodedFrameCallback([&](const ultra::gui::DecodeResult& result) {
            std::lock_guard<std::mutex> lock(summary_mutex);
            recordFrame(result, frames);
        });
        station.setMessageCallback([&](const std::string& msg) {
            std::lock_guard<std::mutex> lock(summary_mutex);
            messages.push_back(msg);
        });

        ultra::timing::globalDecoderProfile().reset();
        station.start();

        bool ever_connected = false;
        bool ever_handshake_complete = false;
        auto observed_waveform = station.getNegotiatedWaveform();
        auto observed_modulation = station.getDataModulation();
        auto observed_code_rate = station.getDataCodeRate();
        int observed_cw_count = station.getFixedFrameCodewords();
        auto sampleNegotiatedState = [&]() {
            const auto wf = station.getNegotiatedWaveform();
            if (station.isConnected() || wf != ultra::protocol::WaveformMode::MC_DPSK) {
                observed_waveform = wf;
                observed_modulation = station.getDataModulation();
                observed_code_rate = station.getDataCodeRate();
                observed_cw_count = station.getFixedFrameCodewords();
            }
        };
        while (!replay->isDrained()) {
            station.tick();
            ever_connected = ever_connected || station.isConnected();
            ever_handshake_complete =
                ever_handshake_complete || station.isHandshakeComplete();
            sampleNegotiatedState();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        station.tick();
        ever_connected = ever_connected || station.isConnected();
        ever_handshake_complete =
            ever_handshake_complete || station.isHandshakeComplete();
        sampleNegotiatedState();
        station.stop();

        {
            std::lock_guard<std::mutex> lock(summary_mutex);
            printSummary(replay->wav(), chirp, station, observed_waveform,
                         observed_modulation, observed_code_rate, observed_cw_count, ever_connected,
                         ever_handshake_complete, frames, messages);
        }
    } catch (const std::exception& e) {
        std::cerr << "session_decode: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
