// Standalone channel sounder — drives ONLY the WattersonChannel (no modem, no
// protocol) with a known multitone probe and dumps input+output to raw float32.
// Purpose: independently validate that the ITU-R F.1487 "Good" channel deformation
// our 12s bursts actually experience matches textbook Good-fading statistics
// (per-carrier Rayleigh AFD/LCR at fd=0.1 Hz, ~2 kHz comb spacing for 0.5 ms delay),
// i.e. that Watterson is not applying fading "too strongly / wrongly".
//
// Fading + multipath ON; noise + CFO OFF (isolate the channel transfer function).
// Build: see comment at bottom. Analyze with tools/analyze_channel_probe.py.

#include "ota_channel_core/models.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using ultra::ota_channel_core::WattersonChannel;
namespace itu = ultra::ota_channel_core::itu_r_f1487;

int main(int argc, char** argv) {
    const std::string channel = (argc > 1) ? argv[1] : "good";
    const double seconds = (argc > 2) ? std::atof(argv[2]) : 60.0;
    const uint64_t seed = (argc > 3) ? std::strtoull(argv[3], nullptr, 10) : 1u;
    const std::string out_prefix = (argc > 4) ? argv[4] : "/tmp/chanprobe";

    const uint32_t fs = 48000;
    WattersonChannel::Config cfg;
    if (channel == "good") cfg = itu::good(60.0f);
    else if (channel == "moderate") cfg = itu::moderate(60.0f);
    else if (channel == "poor") cfg = itu::poor(60.0f);
    else { std::fprintf(stderr, "unknown channel %s\n", channel.c_str()); return 2; }
    cfg.snr_db = 60.0f;          // effectively noiseless reference
    cfg.noise_enabled = false;   // isolate the multipath+fading transfer function
    cfg.cfo_enabled = false;     // no carrier offset — measure |H(f,t)| cleanly
    cfg.sample_rate = fs;

    const size_t warmup = fs / 2;                 // 0.5 s settle (Hilbert/delay lines)
    const size_t n = static_cast<size_t>(seconds * fs) + warmup;

    // Multitone probe: 51 equal-amplitude tones 250..2750 Hz, Schroeder phases
    // (low PAPR), then normalized so the channel sees a realistic-level signal.
    std::vector<double> freqs;
    for (int k = 0; k < 51; ++k) freqs.push_back(250.0 + 50.0 * k);
    const size_t M = freqs.size();
    std::vector<double> phase(M);
    for (size_t m = 0; m < M; ++m) phase[m] = -M_PI * double(m * m) / double(M);

    std::vector<float> input(n);
    double peak = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double s = 0.0;
        const double t = double(i) / fs;
        for (size_t m = 0; m < M; ++m)
            s += std::cos(2.0 * M_PI * freqs[m] * t + phase[m]);
        input[i] = float(s);
        peak = std::max(peak, std::fabs(s));
    }
    const float norm = float(0.35 / peak);  // ~0.35 peak, like a real TX burst
    for (auto& v : input) v *= norm;

    WattersonChannel wchan(cfg, seed);
    std::vector<float> output = wchan.process(input);

    // Drop warmup from both, dump raw float32 LE.
    auto dump = [&](const std::string& path, const std::vector<float>& buf) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); std::exit(3); }
        std::fwrite(buf.data() + warmup, sizeof(float), buf.size() - warmup, f);
        std::fclose(f);
    };
    dump(out_prefix + "_in.f32", input);
    dump(out_prefix + "_out.f32", output);

    std::printf("channel=%s seconds=%.1f seed=%llu fs=%u samples=%zu\n",
                channel.c_str(), seconds, (unsigned long long)seed, fs, n - warmup);
    std::printf("cfg: delay=%.2fms doppler=%.3fHz p1=%.3f p2=%.3f noise=%d cfo=%d\n",
                cfg.delay_spread_ms, cfg.doppler_spread_hz, cfg.path1_gain,
                cfg.path2_gain, cfg.noise_enabled, cfg.cfo_enabled);
    std::printf("wrote %s_in.f32 %s_out.f32 (%zu f32 each, %.1fs)\n",
                out_prefix.c_str(), out_prefix.c_str(), n - warmup, seconds);
    return 0;
}

// Build (direct, no CMake):
//   c++ -std=c++20 -O2 -I src/ota_channel_core tools/channel_probe.cpp \
//       build/libota_channel_core.a -o build/channel_probe
