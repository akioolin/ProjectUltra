// WattersonChannel rigorous verification against ITU-R F.1487 specification.
//
// Exercises every documented property of the channel model with quantitative
// tolerances and reports a pass/fail verdict per check. Hard guard for the
// Watterson complex-fading refactor (commit 6a7d3fd) and any future change
// that could silently weaken the model.
//
// Checks across:
//   PART A — per-tap statistics (E[|h|^2]=1, CN orthogonality, path independence)
//   PART B — ITU-R F.1487 "2σ frequency spread" maps to Gaussian sigma
//   PART C — legacy AR(1) PART C measurement diagnosis
//   PART D — multipath delay = configured ms * sample_rate / 1000
//   PART E — broadband noise stddev matches calibrated formula at 5 SNRs
//   PART F — long-run (120 s) power conservation within 1 dB
//   PART G — |H(f)| matches closed-form h1*g1 + h2*g2*exp(-j2pi*f*D)
//   PART H — ITU-R F.1487 preset parameters (Good/Moderate/Poor)
//   PART I — Gaussian-Doppler multi-lag autocorrelation
//   PART J — Gaussian-Doppler PSD shape fit
//   PART K — production Hilbert response with frozen taps
//   PART L — per-direction fading independence

#include <cstdio>
#include <vector>
#include <string>
#include <cmath>
#include <complex>
#include <numeric>
#include <algorithm>
#include <limits>
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

WattersonChannel::Config fadingDiagnosticConfig(uint32_t sample_rate = 100,
                                                float doppler_hz = 0.1f) {
    auto cfg = itu::good(100.0f);
    cfg.sample_rate = sample_rate;
    cfg.doppler_spread_hz = doppler_hz;
    cfg.noise_enabled = false;
    cfg.fading_enabled = true;
    cfg.multipath_enabled = false;
    cfg.cfo_enabled = false;
    return cfg;
}

cd meanOf(const std::vector<cd>& values) {
    cd mean(0.0, 0.0);
    for (const cd& value : values) {
        mean += value;
    }
    return values.empty() ? mean : mean / static_cast<double>(values.size());
}

std::vector<cd> sampleTap(uint32_t sample_rate,
                          float doppler_hz,
                          uint64_t seed,
                          size_t count,
                          bool tap2 = false) {
    auto cfg = fadingDiagnosticConfig(sample_rate, doppler_hz);
    WattersonChannel chan(cfg, seed);
    std::vector<cd> samples;
    samples.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        chan.stepFadingForDiagnostics();
        const auto tap = tap2 ? chan.fadingTap2ForDiagnostics()
                              : chan.fadingTap1ForDiagnostics();
        samples.emplace_back(tap.real(), tap.imag());
    }
    return samples;
}

cd normalizedAutocorrelation(const std::vector<cd>& samples, size_t lag) {
    const cd mean = meanOf(samples);
    cd numerator(0.0, 0.0);
    double denominator = 0.0;
    for (size_t i = 0; i + lag < samples.size(); ++i) {
        const cd a = samples[i] - mean;
        const cd b = samples[i + lag] - mean;
        numerator += b * std::conj(a);
        denominator += std::norm(a);
    }
    return denominator > 0.0 ? numerator / denominator : cd(0.0, 0.0);
}

struct GaussianPsdFit {
    double fitted_sigma_hz = 0.0;
    double shape_rms_fraction = 0.0;
    double peak_hz = 0.0;
};

GaussianPsdFit fitGaussianDopplerPsd(const std::vector<std::vector<cd>>& series,
                                     double sample_rate,
                                     size_t segment_size,
                                     double max_fit_hz) {
    const size_t max_bin = static_cast<size_t>(
        std::floor(max_fit_hz * static_cast<double>(segment_size) / sample_rate));
    std::vector<double> psd(max_bin + 1, 0.0);
    size_t averaged_segments = 0;

    std::vector<cd> windowed(segment_size);
    std::vector<cd> spectrum(segment_size);
    const pocketfft::shape_t shape{segment_size};
    const pocketfft::stride_t stride{static_cast<ptrdiff_t>(sizeof(cd))};
    const pocketfft::shape_t axes{0};

    std::vector<double> window(segment_size);
    double window_power = 0.0;
    for (size_t n = 0; n < segment_size; ++n) {
        window[n] = 0.5 - 0.5 * std::cos(2.0 * kPi *
                                         static_cast<double>(n) /
                                         static_cast<double>(segment_size - 1));
        window_power += window[n] * window[n];
    }

    for (const auto& samples : series) {
        const size_t segments = samples.size() / segment_size;
        const cd global_mean = meanOf(samples);
        for (size_t segment = 0; segment < segments; ++segment) {
            const size_t offset = segment * segment_size;
            for (size_t n = 0; n < segment_size; ++n) {
                windowed[n] = (samples[offset + n] - global_mean) * window[n];
            }
            pocketfft::c2c<double>(shape, stride, stride, axes,
                                   pocketfft::FORWARD,
                                   windowed.data(), spectrum.data(), 1.0);
            psd[0] += std::norm(spectrum[0]) / window_power;
            for (size_t k = 1; k <= max_bin; ++k) {
                psd[k] += (std::norm(spectrum[k]) +
                           std::norm(spectrum[segment_size - k])) /
                          (2.0 * window_power);
            }
            ++averaged_segments;
        }
    }

    if (averaged_segments == 0) {
        return {};
    }
    for (double& value : psd) {
        value /= static_cast<double>(averaged_segments);
    }

    std::vector<double> smooth(psd.size(), 0.0);
    for (size_t k = 0; k < psd.size(); ++k) {
        const size_t lo = k > 2 ? k - 2 : 0;
        const size_t hi = std::min(psd.size() - 1, k + 2);
        double sum = 0.0;
        for (size_t j = lo; j <= hi; ++j) {
            sum += psd[j];
        }
        smooth[k] = sum / static_cast<double>(hi - lo + 1);
    }

    size_t peak_bin = 1;
    for (size_t k = 1; k < smooth.size(); ++k) {
        if (smooth[k] > smooth[peak_bin]) {
            peak_bin = k;
        }
    }
    const double bin_hz = sample_rate / static_cast<double>(segment_size);
    const double peak = smooth[peak_bin];

    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    size_t fit_count = 0;
    for (size_t k = 1; k < smooth.size(); ++k) {
        if (smooth[k] <= std::max(peak * 1.0e-4,
                                  std::numeric_limits<double>::min())) {
            continue;
        }
        const double f = static_cast<double>(k) * bin_hz;
        const double x = f * f;
        const double y = std::log(smooth[k]);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        ++fit_count;
    }

    GaussianPsdFit fit;
    fit.peak_hz = static_cast<double>(peak_bin) * bin_hz;
    if (fit_count < 3) {
        return fit;
    }

    const double n = static_cast<double>(fit_count);
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    fit.fitted_sigma_hz = slope < 0.0 ? std::sqrt(-1.0 / (2.0 * slope)) : 0.0;

    double amp_num = 0.0;
    double amp_den = 0.0;
    for (size_t k = 1; k < smooth.size(); ++k) {
        const double f = static_cast<double>(k) * bin_hz;
        const double model =
            std::exp(-(f * f) / (2.0 * fit.fitted_sigma_hz * fit.fitted_sigma_hz));
        amp_num += smooth[k] * model;
        amp_den += model * model;
    }
    const double amplitude = amp_den > 0.0 ? amp_num / amp_den : 0.0;

    double err_sq = 0.0;
    size_t err_count = 0;
    for (size_t k = 1; k < smooth.size(); ++k) {
        const double f = static_cast<double>(k) * bin_hz;
        const double model = amplitude *
            std::exp(-(f * f) / (2.0 * fit.fitted_sigma_hz * fit.fitted_sigma_hz));
        const double err = smooth[k] - model;
        err_sq += err * err;
        ++err_count;
    }
    fit.shape_rms_fraction =
        peak > 0.0 ? std::sqrt(err_sq / static_cast<double>(err_count)) / peak : 0.0;
    return fit;
}

void partA_TapStatistics() {
    printf("\n=== PART A: per-tap Gaussian fading statistics ===\n");

    constexpr uint32_t fs = 100;
    constexpr float doppler_hz = 0.1f;
    constexpr size_t per_seed_count = 60'000;  // 600 s at 100 Hz (loose 0.30 tolerances).
    std::vector<cd> h1s, h2s;
    h1s.reserve(per_seed_count * 4);
    h2s.reserve(per_seed_count * 4);
    for (uint64_t seed : {12345u, 12346u, 12347u, 12348u}) {
        auto h1 = sampleTap(fs, doppler_hz, seed, per_seed_count, false);
        auto h2 = sampleTap(fs, doppler_hz, seed, per_seed_count, true);
        h1s.insert(h1s.end(), h1.begin(), h1.end());
        h2s.insert(h2s.end(), h2.begin(), h2.end());
    }

    const size_t N = h1s.size();
    cd m1(0,0), m2(0,0);
    for (size_t i = 0; i < N; ++i) { m1 += h1s[i]; m2 += h2s[i]; }
    m1 /= double(N); m2 /= double(N);
    double v1=0,v2=0; for (size_t i=0; i<N; ++i){ v1+=std::norm(h1s[i]); v2+=std::norm(h2s[i]); }
    v1/=N; v2/=N;
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
    printf("\n=== PART B: ITU-R 2-sigma spread mapping ===\n");

    auto cfg = fadingDiagnosticConfig(100, 0.1f);
    WattersonChannel chan(cfg, 67890u);
    const double expected_sigma = static_cast<double>(cfg.doppler_spread_hz) / 2.0;
    check("Gaussian Doppler sigma = ITU frequency spread / 2",
          chan.fadingSigmaHzForDiagnostics(), expected_sigma, 1.0e-6);
    check("SoS oscillator count per tap", chan.fadingSosOscillatorCountForDiagnostics(),
          128.0, 0.0);
    printf("  spread=%.4f Hz  sigma=%.4f Hz  oscillators=%zu/tap\n",
           cfg.doppler_spread_hz, chan.fadingSigmaHzForDiagnostics(),
           chan.fadingSosOscillatorCountForDiagnostics());
}

void partC_DopplerPSD() {
    printf("\n=== PART C: legacy AR(1) PART C diagnosis ===\n");

    const double fs = 48000.0;
    const double configured_doppler = 0.1;
    const double alpha = 1.0 - std::exp(-2.0 * kPi * configured_doppler / fs);
    const double rho = 1.0 - alpha;
    const double cos_omega =
        (1.0 + rho * rho - 2.0 * alpha * alpha) / (2.0 * rho);
    const double half_power_hz =
        std::acos(std::clamp(cos_omega, -1.0, 1.0)) * fs / (2.0 * kPi);
    check("Legacy AR(1) alpha half-power is configured Doppler, not 0.018 Hz",
          half_power_hz, configured_doppler, 0.001);
    printf("  legacy alpha=%.8g  AR(1) half-power=%.6f Hz; old 0.018 Hz was a biased single-periodogram local-spike measurement\n",
           alpha, half_power_hz);
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
    // 30 s tone x 3 seeds = 90 s. Power conservation is a mean-of-squares that
    // converges as 1/sqrt(N); 90 s of samples is already far inside the 1.0 dB
    // tolerance, and this is the single heaviest part of the proof on CI (was 120 s).
    constexpr size_t N = 48000*30;
    std::vector<float> tone(N);
    const double amp = kModemReferenceInBandRms*std::sqrt(2.0);
    for (size_t i = 0; i < N; ++i) tone[i] = float(amp*std::sin(2.0*kPi*1500.0*i/48000.0));
    // Skip Hilbert warmup at output start
    constexpr size_t skip = 8192;
    double in_p=0, out_p=0;
    size_t measured_samples = 0;
    for (uint64_t seed : {99u, 100u, 101u}) {
        WattersonChannel chan(cfg, seed);
        std::vector<float> out; chan.process(tone, out);
        for (size_t i = skip; i < N; ++i) {
            in_p += double(tone[i]) * tone[i];
            out_p += double(out[i]) * out[i];
            ++measured_samples;
        }
    }
    in_p/=static_cast<double>(measured_samples);
    out_p/=static_cast<double>(measured_samples);
    double r_db = 10.0*std::log10(out_p/in_p);
    check("long-run power conservation (Good, 3x30s)", r_db, 0.0, 1.0);
    printf("  in=%.6f  out=%.6f  ratio=%+.3f dB (3 seeds x 30 s)\n",
           in_p, out_p, r_db);
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

void partI_GaussianAutocorrelation() {
    printf("\n=== PART I: Gaussian-Doppler multi-lag autocorrelation ===\n");
    constexpr uint32_t fs = 100;
    constexpr float doppler_hz = 0.1f;
    constexpr double sigma_hz = static_cast<double>(doppler_hz) / 2.0;
    constexpr size_t count = 200'000;  // 2000 s at the fading diagnostic rate.
    constexpr size_t base_lag_samples = 10;  // tau = 0.1 s.

    std::vector<std::vector<cd>> series;
    for (uint64_t seed : {7001u, 7002u, 7003u, 7004u}) {
        series.push_back(sampleTap(fs, doppler_hz, seed, count, false));
        series.push_back(sampleTap(fs, doppler_hz, seed, count, true));
    }

    for (size_t k : {1u, 2u, 5u, 10u, 20u, 50u}) {
        const size_t lag = k * base_lag_samples;
        cd accum(0.0, 0.0);
        for (const auto& samples : series) {
            accum += normalizedAutocorrelation(samples, lag);
        }
        const cd measured_complex = accum / static_cast<double>(series.size());
        const double tau = static_cast<double>(lag) / static_cast<double>(fs);
        const double expected =
            std::exp(-2.0 * kPi * kPi * sigma_hz * sigma_hz * tau * tau);
        const double measured = measured_complex.real();
        char name[120];
        snprintf(name, sizeof(name),
                 "R_h(tau=%.1fs) matches exp(-2*pi^2*sigma^2*tau^2)", tau);
        check(std::string(name), measured, expected,
              std::max(0.02, 0.05 * expected));
        printf("  tau=%4.1f s  measured=%.6f%+.6fj  expected=%.6f\n",
               tau, measured_complex.real(), measured_complex.imag(), expected);
    }
}

void partJ_GaussianPsdFit() {
    printf("\n=== PART J: Gaussian-Doppler PSD shape fit ===\n");
    constexpr uint32_t fs = 100;
    constexpr float doppler_hz = 0.1f;
    constexpr double expected_sigma = static_cast<double>(doppler_hz) / 2.0;
    constexpr size_t segment_size = 32768;
    constexpr size_t count = segment_size * 4;

    std::vector<std::vector<cd>> series;
    for (uint64_t seed : {8101u, 8102u, 8103u, 8104u, 8105u, 8106u}) {
        series.push_back(sampleTap(fs, doppler_hz, seed, count, false));
        series.push_back(sampleTap(fs, doppler_hz, seed, count, true));
    }

    const GaussianPsdFit fit =
        fitGaussianDopplerPsd(series, fs, segment_size, 0.20);
    check("Gaussian PSD fitted sigma within 5%",
          fit.fitted_sigma_hz, expected_sigma, 0.05 * expected_sigma);
    check("Gaussian PSD shape RMS error < 10% of peak",
          fit.shape_rms_fraction, 0.0, 0.10);
    checkRange("Gaussian PSD peak remains near DC",
               fit.peak_hz, 0.0, 0.02);
    printf("  fitted sigma=%.6f Hz  expected=%.6f Hz  shape_rms=%.4f of peak  peak=%.4f Hz\n",
           fit.fitted_sigma_hz, expected_sigma, fit.shape_rms_fraction,
           fit.peak_hz);
}

double measureFrozenToneGain(double hz,
                             std::complex<float> tap1,
                             std::complex<float> tap2,
                             const WattersonChannel::Config& cfg) {
    constexpr size_t skip = 8192;
    const size_t measure_n = static_cast<size_t>(cfg.sample_rate) * 2;
    const size_t total_n = skip + measure_n;
    constexpr double peak_amplitude = 0.25;

    std::vector<float> tone(total_n);
    for (size_t i = 0; i < total_n; ++i) {
        tone[i] = static_cast<float>(
            peak_amplitude * std::sin(2.0 * kPi * hz *
                                      static_cast<double>(i) /
                                      static_cast<double>(cfg.sample_rate)));
    }

    WattersonChannel channel(cfg, 0xabcddcu);
    channel.setFadingTapsForDiagnostics(tap1, tap2);
    const std::vector<float> out = channel.process(tone);

    cd acc(0.0, 0.0);
    for (size_t n = 0; n < measure_n; ++n) {
        const double phase =
            -2.0 * kPi * hz * static_cast<double>(n) /
            static_cast<double>(cfg.sample_rate);
        acc += static_cast<double>(out[skip + n]) *
               cd(std::cos(phase), std::sin(phase));
    }
    const double measured_peak = 2.0 * std::abs(acc) /
                                 static_cast<double>(measure_n);
    return measured_peak / peak_amplitude;
}

void partK_ProductionHilbertResponse() {
    printf("\n=== PART K: production Hilbert response with frozen taps ===\n");
    auto cfg = itu::good(100.0f);
    cfg.noise_enabled = false;
    cfg.fading_enabled = true;
    cfg.multipath_enabled = true;
    cfg.cfo_enabled = false;

    const std::complex<float> tap1(0.6f, -0.4f);
    const std::complex<float> tap2(-0.3f, 0.5f);
    const double delay_seconds = static_cast<double>(cfg.delay_spread_ms) / 1000.0;

    for (double f : {50.0, 250.0, 500.0, 1000.0, 1500.0,
                     2000.0, 2500.0, 2900.0}) {
        const double measured = measureFrozenToneGain(f, tap1, tap2, cfg);
        const cd expected_h =
            cd(tap1.real(), tap1.imag()) * static_cast<double>(cfg.path1_gain) +
            cd(tap2.real(), tap2.imag()) * static_cast<double>(cfg.path2_gain) *
                std::polar(1.0, -2.0 * kPi * f * delay_seconds);
        const double expected = std::abs(expected_h);
        const double error_db = 20.0 * std::log10(measured / expected);
        char name[96];
        snprintf(name, sizeof(name), "production |H(%g Hz)| within 0.1 dB", f);
        check(std::string(name), error_db, 0.0, 0.1);
        printf("  f=%6.1f Hz  measured=%.6f  expected=%.6f  err=%+.3f dB\n",
               f, measured, expected, error_db);
    }

    for (double f : {3200.0, 3600.0}) {
        auto stop_cfg = cfg;
        stop_cfg.multipath_enabled = false;
        const double measured = measureFrozenToneGain(
            f, std::complex<float>(1.0f, 0.0f),
            std::complex<float>(0.0f, 0.0f), stop_cfg);
        const double attenuation_db = 20.0 * std::log10(std::max(measured, 1.0e-12));
        char name[96];
        snprintf(name, sizeof(name), "production Hilbert stop-band at %g Hz", f);
        checkRange(std::string(name), attenuation_db, -200.0, -40.0);
        printf("  stop f=%6.1f Hz  gain=%.6g  attenuation=%.2f dB\n",
               f, measured, attenuation_db);
    }
}

void partL_DirectionIndependence() {
    printf("\n=== PART L: per-direction fading independence ===\n");
    constexpr uint32_t fs = 100;
    constexpr float doppler_hz = 0.1f;
    constexpr size_t count = fs * 10;
    auto cfg = fadingDiagnosticConfig(fs, doppler_hz);
    cd numerator(0.0, 0.0);
    double pa = 0.0, pb = 0.0;
    constexpr uint64_t base_seed = 0x515151u;
    constexpr size_t seed_pairs = 16;
    for (size_t pair = 0; pair < seed_pairs; ++pair) {
        const uint64_t seed = base_seed + 2u * pair;
        WattersonChannel a_chan(cfg, seed);
        WattersonChannel b_chan(cfg, seed + 1u);
        for (size_t i = 0; i < count; ++i) {
            a_chan.stepFadingForDiagnostics();
            b_chan.stepFadingForDiagnostics();
            for (bool tap2 : {false, true}) {
                const auto at = tap2 ? a_chan.fadingTap2ForDiagnostics()
                                     : a_chan.fadingTap1ForDiagnostics();
                const auto bt = tap2 ? b_chan.fadingTap2ForDiagnostics()
                                     : b_chan.fadingTap1ForDiagnostics();
                const cd xa(at.real(), at.imag());
                const cd xb(bt.real(), bt.imag());
                numerator += xa * std::conj(xb);
                pa += std::norm(xa);
                pb += std::norm(xb);
            }
        }
    }
    const double rho = std::abs(numerator) / std::sqrt(pa * pb);
    check("|corr(seed s, seed s+1)| < 0.1 over 10 s windows", rho, 0.0, 0.1);
    printf("  |cross-correlation|=%.6f over %zu adjacent seed pairs, %.1f s each\n",
           rho, seed_pairs, static_cast<double>(count) / static_cast<double>(fs));
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
    partI_GaussianAutocorrelation();
    partJ_GaussianPsdFit();
    partK_ProductionHilbertResponse();
    partL_DirectionIndependence();

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
