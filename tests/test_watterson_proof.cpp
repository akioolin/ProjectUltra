// WattersonChannel rigorous verification against ITU-R F.1487 specification.
//
// Exercises every documented property of the channel model with quantitative
// tolerances and reports a pass/fail verdict per check. Hard guard for the
// Watterson complex-fading refactor (commit 6a7d3fd) and any future change
// that could silently weaken the model.
//
// 29 checks across:
//   PART A — per-tap statistics (E[|h|^2]=1, CN orthogonality, path independence)
//   PART B — AR(1) autocorrelation matches Doppler-derived alpha
//   PART C — Doppler PSD narrow + low-frequency consistent with config
//   PART D — multipath delay = configured ms * sample_rate / 1000
//   PART E — broadband noise stddev matches calibrated formula at 5 SNRs
//   PART F — long-run (120 s) power conservation within 1 dB
//   PART G — |H(f)| matches closed-form h1*g1 + h2*g2*exp(-j2pi*f*D)
//   PART H — ITU-R F.1487 preset parameters (Good/Moderate/Poor)

#include <cstdio>
#include <vector>
#include <string>
#include <cmath>
#include <complex>
#include <numeric>
#include <algorithm>
#include "ota_channel_core/models.hpp"
#include "pocketfft_hdronly.h"

using ultra::ota_channel_core::WattersonChannel;
using ultra::ota_channel_core::kModemReferenceInBandRms;
using ultra::ota_channel_core::kModemBroadbandToInBandSnrOffsetDb;
namespace itu = ultra::ota_channel_core::itu_r_f1487;

constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;
using cf = std::complex<float>;

struct Result { std::string name; double measured; double expected; double margin; bool pass; };
std::vector<Result> RESULTS;

void check(std::string name, double measured, double expected, double margin) {
    bool pass = std::abs(measured - expected) <= margin;
    RESULTS.push_back({std::move(name), measured, expected, margin, pass});
}

void checkRange(std::string name, double measured, double lo, double hi) {
    bool pass = measured >= lo && measured <= hi;
    RESULTS.push_back({std::move(name), measured, (lo+hi)/2.0, (hi-lo)/2.0, pass});
}

// AR(1) settling time: ~3/alpha samples. For Good (alpha~1.3e-5): ~230k samples.
// Use 1M warmup steps to be safe.
constexpr size_t kAR1Warmup = 1'000'000;

void partA_TapStatistics() {
    printf("\n=== PART A: per-tap fading statistics (after AR(1) warmup) ===\n");

    auto cfg = itu::good(100.0f);
    cfg.noise_enabled = false;
    cfg.fading_enabled = true;
    WattersonChannel chan(cfg, 12345u);

    // Warmup
    for (size_t i = 0; i < kAR1Warmup; ++i) chan.stepFadingForDiagnostics();

    // Measure 2M samples = 4.2 sec at 48 kHz = 0.42 Doppler cycles.
    // That's still few cycles for an ergodic mean. Use 6M samples = 12.5s = 1.25 cycles.
    constexpr size_t N = 6'000'000;
    std::vector<cd> h1s, h2s; h1s.reserve(N); h2s.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        chan.stepFadingForDiagnostics();
        auto t1 = chan.fadingTap1ForDiagnostics();
        auto t2 = chan.fadingTap2ForDiagnostics();
        h1s.emplace_back(t1.real(), t1.imag());
        h2s.emplace_back(t2.real(), t2.imag());
    }

    cd m1(0,0), m2(0,0);
    for (size_t i = 0; i < N; ++i) { m1 += h1s[i]; m2 += h2s[i]; }
    m1 /= double(N); m2 /= double(N);
    double v1=0,v2=0; for (size_t i=0; i<N; ++i){ v1+=std::norm(h1s[i]); v2+=std::norm(h2s[i]); }
    v1/=N; v2/=N;
    // Mean tolerance: with 1.25 Doppler cycles, statistical std of mean ~0.5-1.0 for unit variance,
    // so use loose tolerance for the mean. The variance is more robust.
    check("E[|h1|^2] = 1 (unit variance, expected for CN(0,1))", v1, 1.0, 0.30);
    check("E[|h2|^2] = 1 (unit variance, expected for CN(0,1))", v2, 1.0, 0.30);

    // Real/imag orthogonality: E[Re·Im]/sqrt(var(Re)*var(Im)) should be 0
    double cre1=0, cre2=0;
    for (size_t i = 0; i < N; ++i) {
        cre1 += h1s[i].real()*h1s[i].imag();
        cre2 += h2s[i].real()*h2s[i].imag();
    }
    cre1 /= N; cre2 /= N;
    check("|E[Re(h1)·Im(h1)]| = 0 (CN orthogonality)", std::abs(cre1), 0.0, 0.2);
    check("|E[Re(h2)·Im(h2)]| = 0 (CN orthogonality)", std::abs(cre2), 0.0, 0.2);

    // Independence between paths
    cd xcor(0,0);
    for (size_t i = 0; i < N; ++i) xcor += h1s[i] * std::conj(h2s[i]);
    xcor /= double(N);
    check("|E[h1·conj(h2)]| = 0 (path independence)", std::abs(xcor), 0.0, 0.3);

    printf("  E[|h1|^2] = %.4f, E[|h2|^2] = %.4f (target 1.0)\n", v1, v2);
    printf("  |E[h1·conj(h2)]| = %.4f (target 0)\n", std::abs(xcor));
}

void partB_Autocorrelation() {
    printf("\n=== PART B: AR(1) autocorrelation matches Doppler-derived alpha ===\n");

    auto cfg = itu::good(100.0f);
    cfg.noise_enabled = false; cfg.fading_enabled = true;
    WattersonChannel chan(cfg, 67890u);
    for (size_t i = 0; i < kAR1Warmup; ++i) chan.stepFadingForDiagnostics();

    constexpr size_t N = 2'000'000;
    std::vector<cd> h1s; h1s.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        chan.stepFadingForDiagnostics();
        auto t = chan.fadingTap1ForDiagnostics();
        h1s.emplace_back(t.real(), t.imag());
    }
    const double normalized_doppler = 0.1 / 48000.0;
    const double alpha = 1.0 - std::exp(-2.0*kPi*normalized_doppler);
    const double rho_predicted = 1.0 - alpha;

    cd a01(0,0); double p0=0;
    for (size_t i = 1; i < N; ++i) { a01 += h1s[i]*std::conj(h1s[i-1]); p0 += std::norm(h1s[i-1]); }
    a01/=double(N-1); p0/=double(N-1);
    double rho_measured = std::abs(a01)/p0;
    check("AR(1) 1-step autocorrelation matches 1-alpha", rho_measured, rho_predicted, 1e-4);
    printf("  alpha=%.4e  rho_predicted=%.10f  rho_measured=%.10f\n", alpha, rho_predicted, rho_measured);
}

void partC_DopplerPSD() {
    printf("\n=== PART C: Doppler PSD shape (post-warmup) ===\n");

    auto cfg = itu::good(100.0f);
    cfg.noise_enabled = false; cfg.fading_enabled = true;
    WattersonChannel chan(cfg, 4242u);
    for (size_t i = 0; i < kAR1Warmup; ++i) chan.stepFadingForDiagnostics();

    constexpr size_t stride = 480;  // 100 Hz fading-domain rate
    constexpr size_t fft_n = 65536;
    const double bin_hz = (48000.0/stride)/fft_n;

    std::vector<cd> h1stride; h1stride.reserve(fft_n);
    size_t step = 0;
    while (h1stride.size() < fft_n) {
        chan.stepFadingForDiagnostics();
        if (step % stride == 0) {
            auto t = chan.fadingTap1ForDiagnostics();
            h1stride.emplace_back(t.real(), t.imag());
        }
        ++step;
    }
    // Subtract mean (windowed estimate of DC) — the Doppler process is zero-mean in expectation,
    // but a finite-sample mean is the dominant low-freq leak in the FFT
    cd m(0,0); for (auto& x: h1stride) m += x; m /= double(fft_n);
    for (auto& x: h1stride) x -= m;

    std::vector<cd> H(fft_n);
    pocketfft::shape_t shape{fft_n};
    pocketfft::stride_t st{(ptrdiff_t)sizeof(cd)};
    pocketfft::shape_t axes{0};
    pocketfft::c2c<double>(shape, st, st, axes, pocketfft::FORWARD, h1stride.data(), H.data(), 1.0);
    std::vector<double> P(fft_n);
    for (size_t k=0;k<fft_n;++k) P[k] = std::norm(H[k]) / fft_n;

    // After mean subtraction, P[0] should be near 0. Real peak is at lowest non-DC bins (slow process)
    // Find argmax over k >= 1
    size_t peak_k = 1;
    for (size_t k = 1; k < fft_n/4; ++k) if (P[k] > P[peak_k]) peak_k = k;
    double peak_hz = peak_k * bin_hz;
    // For AR(1), spectrum is Lorentzian, peak at DC. After mean removal, peak SHOULD be in low-freq region.
    checkRange("PSD peak (post-mean-removal) in 0-0.2 Hz region (config=0.1Hz Doppler)",
               peak_hz, 0.0, 0.2);
    printf("  peak bin: %zu = %.4f Hz\n", peak_k, peak_hz);

    // Half-power from the LOW-FREQ peak
    double peak_p = P[peak_k];
    size_t half_k = peak_k;
    for (size_t k = peak_k; k < fft_n/4; ++k) if (P[k] < 0.5*peak_p) { half_k = k; break; }
    double half_hz = half_k * bin_hz;
    // For AR(1) the spectrum is Lorentzian; the half-power BW at the decimated rate
    // is governed by α·fs/(2π) ≈ Doppler/(2π) ≈ 0.016 Hz at configured 0.1 Hz Doppler.
    // We accept anything between 0.005 and 0.5 Hz — the key check is that the spectrum
    // is narrow and centered near DC, consistent with a slow Doppler process.
    checkRange("Half-power roll-off within an order of magnitude of configured Doppler",
               half_hz, 0.005, 0.5);
    printf("  half-power bin: %zu = %.4f Hz  (configured Doppler=0.1 Hz)\n", half_k, half_hz);
}

void partD_MultipathDelay() {
    printf("\n=== PART D: Multipath delay ===\n");

    auto cfg = itu::good(100.0f);
    cfg.noise_enabled = false; cfg.fading_enabled = false;
    cfg.multipath_enabled = true;
    WattersonChannel chan(cfg, 1u);

    constexpr size_t N = 8192;
    std::vector<float> impulse(N, 0.0f);
    impulse[2000] = 1.0f;
    std::vector<float> out; chan.process(impulse, out);

    std::vector<size_t> peaks;
    for (size_t i = 1; i+1 < N; ++i)
        if (std::abs(out[i]) > 0.1f && std::abs(out[i])>=std::abs(out[i-1]) && std::abs(out[i])>=std::abs(out[i+1]))
            peaks.push_back(i);
    if (peaks.size() < 2) {
        check("Found 2 multipath peaks", double(peaks.size()), 2.0, 0.5);
        return;
    }
    double dsamp = double(peaks[1]-peaks[0]);
    double expect = cfg.delay_spread_ms * cfg.sample_rate / 1000.0;
    check("Multipath delay matches config", dsamp, expect, 1.0);
    printf("  measured: %.1f samples (%.3f ms)  configured: %.1f samples (%.3f ms)\n",
           dsamp, dsamp*1000.0/cfg.sample_rate, expect, cfg.delay_spread_ms);
}

void partE_SNRCalibration() {
    printf("\n=== PART E: SNR calibration at multiple settings ===\n");
    for (float snr : {0.0f, 5.0f, 10.0f, 15.0f, 20.0f}) {
        auto cfg = itu::good(snr);
        cfg.fading_enabled = false; cfg.multipath_enabled = false;
        cfg.noise_enabled = true; cfg.cfo_enabled = false;
        WattersonChannel chan(cfg, 1u);
        constexpr size_t N = 480'000;
        std::vector<float> silence(N, 0.0f);
        std::vector<float> out; chan.process(silence, out);
        double sq=0; for(float v:out) sq+=double(v)*v;
        double measured = std::sqrt(sq/N);
        // The channel emits broadband noise such that AFTER the modem's in-band FIR,
        // the resulting in-band noise gives the configured SNR. The implementation in
        // modemReferenceNoiseStddev() does:
        //   broadband_stddev = ref_in_band_rms / 10^((snr_db - 9.64) / 20)
        // so the output RMS we measure is exactly that broadband stddev.
        double expected = kModemReferenceInBandRms / std::pow(10.0, (snr - kModemBroadbandToInBandSnrOffsetDb)/20.0);
        check(std::string("broadband noise stddev at in-band SNR=")+std::to_string(int(snr))+" dB",
              measured, expected, 0.005);
        printf("  SNR=%2.0f  measured=%.6f  expected=%.6f  err=%+.2f%%\n",
               snr, measured, expected, 100.0*(measured-expected)/expected);
    }
}

void partF_PowerConservation() {
    printf("\n=== PART F: long-run power conservation ===\n");
    auto cfg = itu::good(100.0f);
    cfg.noise_enabled = false; cfg.fading_enabled = true; cfg.multipath_enabled = true;
    WattersonChannel chan(cfg, 99u);
    // Warmup
    for (size_t i = 0; i < kAR1Warmup; ++i) chan.stepFadingForDiagnostics();
    // 120 s tone
    constexpr size_t N = 48000*120;
    std::vector<float> tone(N);
    const double amp = kModemReferenceInBandRms*std::sqrt(2.0);
    for (size_t i = 0; i < N; ++i) tone[i] = float(amp*std::sin(2.0*kPi*1500.0*i/48000.0));
    std::vector<float> out; chan.process(tone, out);
    // Skip Hilbert warmup at output start
    constexpr size_t skip = 8192;
    double in_p=0, out_p=0;
    for (size_t i = skip; i < N; ++i) { in_p+=double(tone[i])*tone[i]; out_p+=double(out[i])*out[i]; }
    in_p/=(N-skip); out_p/=(N-skip);
    double r_db = 10.0*std::log10(out_p/in_p);
    check("long-run power conservation (Good, 120s)", r_db, 0.0, 1.0);
    printf("  in=%.6f  out=%.6f  ratio=%+.3f dB\n", in_p, out_p, r_db);
}

void partG_ChannelResponseFrozen() {
    printf("\n=== PART G: Channel response H(f) using applyComplexMultipathForDiagnostics ===\n");
    // Use the diagnostic helper that applies fixed taps (no fading drift, no Hilbert).
    // Build analytic-signal input manually (pure complex exponential = perfect analytic sinusoid).
    cf tap1(0.6f, -0.4f), tap2(-0.3f, 0.5f);
    auto cfg = itu::good(100.0f);
    const double D = cfg.delay_spread_ms / 1000.0;
    const size_t delay_samples = size_t(cfg.delay_spread_ms * cfg.sample_rate / 1000.0);

    for (double f : {500.0, 1000.0, 1500.0, 2000.0, 2500.0}) {
        constexpr size_t N = 96000;
        std::vector<cf> input(N);
        const double amp = 0.3;
        for (size_t i = 0; i < N; ++i)
            input[i] = cf(float(amp*std::cos(2.0*kPi*f*i/48000.0)),
                          float(amp*std::sin(2.0*kPi*f*i/48000.0)));

        auto out = WattersonChannel::applyComplexMultipathForDiagnostics(
            std::span<const cf>(input), delay_samples, tap1, tap2, cfg.path1_gain, cfg.path2_gain);

        // After tap-line warmup (delay_samples), measure |out|/|input|
        size_t skip = delay_samples + 100;
        double sum_out_sq = 0, sum_in_sq = 0;
        for (size_t i = skip; i < N; ++i) {
            sum_out_sq += std::norm(out[i]);
            sum_in_sq += std::norm(input[i]);
        }
        double measured_H = std::sqrt(sum_out_sq / sum_in_sq);

        cd Hf = cd(tap1.real(), tap1.imag())*double(cfg.path1_gain) +
                cd(tap2.real(), tap2.imag())*double(cfg.path2_gain)*std::polar(1.0, -2.0*kPi*f*D);
        double expected_H = std::abs(Hf);

        char nbuf[80]; snprintf(nbuf, 80, "|H(%g Hz)| measured vs closed-form", f);
        check(std::string(nbuf), measured_H, expected_H, 0.01);
        printf("  f=%4.0f Hz  measured |H|=%.6f  closed-form |H|=%.6f  err=%+.3f%%\n",
               f, measured_H, expected_H, 100.0*(measured_H-expected_H)/expected_H);
    }
}

void partH_PresetParameters() {
    printf("\n=== PART H: ITU-R F.1487 preset parameters ===\n");
    auto g = itu::good(10.0f); auto m = itu::moderate(10.0f); auto p = itu::poor(10.0f);
    check("Good delay = 0.5 ms",     g.delay_spread_ms, 0.5, 0.01);
    check("Good Doppler = 0.1 Hz",   g.doppler_spread_hz, 0.1, 0.001);
    check("Moderate delay = 1.0 ms", m.delay_spread_ms, 1.0, 0.01);
    check("Moderate Doppler = 0.5 Hz", m.doppler_spread_hz, 0.5, 0.01);
    check("Poor delay = 2.0 ms",     p.delay_spread_ms, 2.0, 0.01);
    check("Poor Doppler = 1.0 Hz",   p.doppler_spread_hz, 1.0, 0.01);
    check("Good path1_gain ≈ 0.707", g.path1_gain, 0.7071, 0.01);
    check("Good path2_gain ≈ 0.707", g.path2_gain, 0.7071, 0.01);
    // Conservation: path1² + path2² = 1
    check("path1² + path2² = 1 (energy preserving)",
          double(g.path1_gain*g.path1_gain) + double(g.path2_gain*g.path2_gain), 1.0, 0.005);
}

int main() {
    partA_TapStatistics();
    partB_Autocorrelation();
    partC_DopplerPSD();
    partD_MultipathDelay();
    partE_SNRCalibration();
    partF_PowerConservation();
    partG_ChannelResponseFrozen();
    partH_PresetParameters();

    printf("\n\n========== RESULTS ==========\n");
    int pass=0, fail=0;
    for (auto& r : RESULTS) {
        printf("  [%s] %-58s  measured=%-13.6g  expected=%-13.6g  margin=%.4g\n",
               r.pass?"PASS":"FAIL", r.name.c_str(), r.measured, r.expected, r.margin);
        if (r.pass) ++pass; else ++fail;
    }
    printf("\n  TOTAL: %d PASS, %d FAIL of %zu checks\n", pass, fail, RESULTS.size());
    return fail == 0 ? 0 : 1;
}
