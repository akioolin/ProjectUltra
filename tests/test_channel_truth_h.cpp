// Phase 0 (EFFECTIVE_SINR handoff §9): validate the simulator TRUTH channel
// response H[k,t] against the channel model's actual sample-domain filter.
//
// WHY THIS TEST EXISTS. The question "is channel estimation the throughput
// limiter?" has been answered twice from oracles that turned out to be invalid:
//   * ULTRA_GENIE_DATA_AIDED sets H=Y/X, which makes the MMSE combiner return X
//     exactly -- circular (it decoded at ~4.7x Shannon capacity).
//   * ULTRA_GENIE_LTS_FREEZE was cited as "perfect frequency CSI" but holds ONE
//     noisy LTS snapshot across a frame; its harm is lost noise-averaging.
// Both were believed because nothing checked them against ground truth. So the
// truth source itself must be validated against the implementation, not merely
// asserted to match a formula someone typed twice.
//
// WHAT IS VALIDATED. WattersonChannel::trueFrequencyResponse(f) claims
//     H(f) = g1*a1 + g2*a2*exp(-j*2*pi*f*D/fs)
// from the tap structure in processWithComplexFading:
//     out(n) = x_a(n)*g1*a1(n) + x_a(n-D)*g2*a2(n)
// We measure the channel's response empirically -- drive a pure tone, correlate
// the output against the same tone -- and compare.
//
// THE DOMAIN CONTRACT (the actual Phase 0 deliverable). The measured response is
// NOT equal to H(f); it differs by a common linear phase from the analytic front
// end:
//     H_measured(f) = H_true(f) * exp(-j*2*pi*f*d/fs)
// A pure delay is exactly the ambiguity an OFDM receiver absorbs in its FFT-window
// timing, and a global complex scale is the ambiguity it absorbs in its phase
// reference. So the honest comparison removes those TWO and nothing else.
//
// MEASURED 2026-07-29 (this test's own output):
//   * d = -64.00 samples exactly -- a 64-sample ADVANCE. The analytic front end is
//     delay-compensated, so this is NOT the 1793-tap FIR's ~896-sample group delay.
//   * global scale = 1.0000 at +0.00 deg (no scale or constant-phase offset).
//   * the phase difference is a PURE ramp: worst deviation 0.01 deg across the band.
//   * residual NMSE after removing both: -81.3 dB.
//
// Any future truth-vs-estimate comparison must remove a global complex scale and a
// linear phase ramp before computing NMSE, and nothing more. Removing more would
// hide real estimator error; removing less reports a wrong answer -- as an earlier
// draft of this very test did, fitting the ramp through the origin and reporting
// NMSE 1.91 on a channel it now matches to -81 dB.

#include "ota_channel_core/models.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace {

using Complex = std::complex<float>;
using ultra::ota_channel_core::WattersonChannel;

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

constexpr uint32_t kFs = 48000;
// Analytic front end is a 1793-tap FIR; its transient must clear before we
// measure, and we then correlate over a whole number of periods.
constexpr size_t kWarmup = 8192;
constexpr size_t kMeasure = 24000;

// Drive a pure tone through the channel and return the complex response at that
// frequency: correlate the output against exp(-j*2*pi*f*n/fs).
Complex measureResponse(WattersonChannel::Config cfg, float freq_hz, uint64_t seed) {
    WattersonChannel channel(cfg, seed);

    const size_t total = kWarmup + kMeasure;
    std::vector<float> input(total);
    for (size_t n = 0; n < total; ++n) {
        input[n] = std::cos(2.0f * static_cast<float>(M_PI) * freq_hz *
                            static_cast<float>(n) / static_cast<float>(kFs));
    }

    const std::vector<float> output = channel.process(input);

    std::complex<double> acc(0.0, 0.0);
    for (size_t n = kWarmup; n < total; ++n) {
        const double phase = -2.0 * M_PI * static_cast<double>(freq_hz) *
                             static_cast<double>(n) / static_cast<double>(kFs);
        acc += static_cast<double>(output[n]) *
               std::complex<double>(std::cos(phase), std::sin(phase));
    }
    // Re{A*e^{jwt}} correlated against e^{-jwt} over N samples gives A*N/2.
    acc *= 2.0 / static_cast<double>(kMeasure);
    return Complex(static_cast<float>(acc.real()), static_cast<float>(acc.imag()));
}

WattersonChannel::Config staticTwoPathConfig() {
    WattersonChannel::Config cfg;
    cfg.snr_db = 60.0f;
    cfg.delay_spread_ms = 2.0f;
    cfg.doppler_spread_hz = 0.0f;  // taps constant => H is time-invariant
    cfg.cfo_hz = 0.0f;
    cfg.path1_gain = 0.707f;
    cfg.path2_gain = 0.707f;
    cfg.sample_rate = kFs;
    cfg.fading_enabled = true;
    cfg.multipath_enabled = true;
    cfg.noise_enabled = false;  // isolate the filter from the noise process
    cfg.cfo_enabled = false;
    return cfg;
}

}  // namespace

int main() {
    std::printf("=== simulator truth H[k,t] validation (Phase 0) ===\n");

    const auto cfg = staticTwoPathConfig();
    // In-band OFDM frequencies, kept clear of the analytic filter's edges.
    const std::vector<float> freqs = {700.0f,  900.0f,  1100.0f, 1300.0f, 1500.0f,
                                      1700.0f, 1900.0f, 2100.0f, 2300.0f};
    const uint64_t seed = 42;

    // Reference instance: taps are frozen (Doppler 0), so its tap state is the
    // same one every measurement instance starts from.
    WattersonChannel reference(cfg, seed);

    check(reference.multipathDelaySamples() > 0,
          "two-path config actually has a non-zero delay");

    std::vector<Complex> measured;
    std::vector<Complex> truth;
    measured.reserve(freqs.size());
    truth.reserve(freqs.size());
    for (float f : freqs) {
        measured.push_back(measureResponse(cfg, f, seed));
        truth.push_back(reference.trueFrequencyResponse(f));
    }

    // The two-path response must actually vary across the band, otherwise this
    // test would pass on a trivially flat channel and prove nothing.
    float mag_min = 1e9f;
    float mag_max = 0.0f;
    for (const Complex& h : truth) {
        mag_min = std::min(mag_min, std::abs(h));
        mag_max = std::max(mag_max, std::abs(h));
    }
    check(mag_max / std::max(mag_min, 1e-6f) > 1.5f,
          "truth response is frequency-selective across the band (not trivially flat)");

    // THE DOMAIN CONTRACT: remove a global complex scale AND a linear phase ramp,
    // and nothing else. Those two are exactly what an OFDM receiver absorbs (a
    // common phase reference, and the FFT-window timing). Anything left over is a
    // genuine mismatch between the truth formula and the implementation.
    //
    // Step 1: linear fit WITH intercept on unwrapped arg(H_measured/H_truth) vs
    // frequency. Fitting through the origin instead silently folds any constant
    // phase into the slope and reports a wrong delay.
    std::vector<double> unwrapped(freqs.size(), 0.0);
    {
        double offset = 0.0;
        double prev = 0.0;
        for (size_t i = 0; i < freqs.size(); ++i) {
            const Complex ratio = measured[i] / truth[i];
            double ph = std::atan2(static_cast<double>(ratio.imag()),
                                   static_cast<double>(ratio.real()));
            if (i > 0) {
                while (ph + offset - prev > M_PI) offset -= 2.0 * M_PI;
                while (ph + offset - prev < -M_PI) offset += 2.0 * M_PI;
            }
            ph += offset;
            prev = ph;
            unwrapped[i] = ph;
        }
    }

    const double n = static_cast<double>(freqs.size());
    double sum_f = 0.0;
    double sum_p = 0.0;
    double sum_ff = 0.0;
    double sum_fp = 0.0;
    for (size_t i = 0; i < freqs.size(); ++i) {
        const double f = static_cast<double>(freqs[i]);
        sum_f += f;
        sum_p += unwrapped[i];
        sum_ff += f * f;
        sum_fp += f * unwrapped[i];
    }
    const double denom = n * sum_ff - sum_f * sum_f;
    const double slope = (n * sum_fp - sum_f * sum_p) / denom;   // rad per Hz
    const double intercept = (sum_p - slope * sum_f) / n;        // rad
    // phase = -2*pi*f*d/fs  =>  d = -slope*fs/(2*pi)
    const double delay_samples = -slope * static_cast<double>(kFs) / (2.0 * M_PI);
    std::printf("  fitted linear phase: delay=%.2f samples, constant=%+.2f deg\n",
                delay_samples, intercept * 180.0 / M_PI);

    // Residual after removing the fitted ramp must be flat -- that is what makes
    // "one global delay" the correct and complete timing description.
    double worst_residual_deg = 0.0;
    for (size_t i = 0; i < freqs.size(); ++i) {
        const double model = slope * static_cast<double>(freqs[i]) + intercept;
        double r = unwrapped[i] - model;
        worst_residual_deg = std::max(worst_residual_deg, std::abs(r) * 180.0 / M_PI);
    }
    std::printf("  worst deviation from a pure linear ramp: %.2f deg\n",
                worst_residual_deg);
    check(worst_residual_deg < 2.0,
          "phase difference is a PURE linear ramp (a single delay describes it fully)");

    // MEASURED 2026-07-29: exactly -64.00 samples (a 64-sample ADVANCE), with the
    // fitted constant phase landing on a full wrap (i.e. zero). The analytic front
    // end is delay-compensated, so this is NOT the 1793-tap FIR's ~896-sample group
    // delay. Pinned because a change here silently reinterprets every truth-vs-
    // estimate comparison built on top of it.
    check(std::abs(delay_samples + 64.0) < 1.0,
          "analytic front end contributes exactly the known -64 sample advance");

    // Step 2: derotate by the fitted ramp, then divide out the single best complex
    // scale (LS over all carriers) -- the global-scale half of the contract.
    std::vector<Complex> aligned(freqs.size());
    for (size_t i = 0; i < freqs.size(); ++i) {
        const double ph = slope * static_cast<double>(freqs[i]) + intercept;
        const Complex derot(static_cast<float>(std::cos(-ph)),
                            static_cast<float>(std::sin(-ph)));
        aligned[i] = measured[i] * derot;
    }
    std::complex<double> scale_num(0.0, 0.0);
    double scale_den = 0.0;
    for (size_t i = 0; i < freqs.size(); ++i) {
        scale_num += std::conj(std::complex<double>(truth[i].real(), truth[i].imag())) *
                     std::complex<double>(aligned[i].real(), aligned[i].imag());
        scale_den += static_cast<double>(std::norm(truth[i]));
    }
    const std::complex<double> alpha =
        scale_den > 0.0 ? scale_num / scale_den : std::complex<double>(1.0, 0.0);
    std::printf("  fitted global scale: |alpha|=%.4f  arg=%+.2f deg\n",
                std::abs(alpha), std::arg(alpha) * 180.0 / M_PI);

    double err_num = 0.0;
    double err_den = 0.0;
    float worst_mag_ratio_db = 0.0f;
    for (size_t i = 0; i < freqs.size(); ++i) {
        const Complex scaled = aligned[i] / Complex(static_cast<float>(alpha.real()),
                                                    static_cast<float>(alpha.imag()));
        const Complex diff = scaled - truth[i];
        err_num += static_cast<double>(std::norm(diff));
        err_den += static_cast<double>(std::norm(truth[i]));

        const float ratio_db =
            20.0f * std::log10(std::abs(scaled) / std::max(std::abs(truth[i]), 1e-9f));
        worst_mag_ratio_db = std::max(worst_mag_ratio_db, std::abs(ratio_db));
        std::printf("  f=%6.0f Hz  |truth|=%.4f  |meas|=%.4f  dphase=%+6.2f deg\n",
                    static_cast<double>(freqs[i]), static_cast<double>(std::abs(truth[i])),
                    static_cast<double>(std::abs(scaled)),
                    static_cast<double>(std::arg(scaled / truth[i]) * 180.0 / M_PI));
    }
    const double nmse = err_den > 0.0 ? err_num / err_den : 1.0;
    std::printf("  NMSE(measured vs truth, one global delay removed) = %.4f (%.1f dB)\n",
                nmse, 10.0 * std::log10(std::max(nmse, 1e-12)));

    // MEASURED -81.3 dB. The bar is set at -40 dB: far below the measurement so it
    // is not brittle, far above the ~0 dB (NMSE ~1) that a wrong delay convention,
    // a sign error, or swapped path gains would produce.
    check(nmse < 1e-4, "truth H matches the measured channel to better than -40 dB NMSE");
    check(worst_mag_ratio_db < 1.0f, "no frequency deviates more than 1 dB in magnitude");

    // Degenerate configurations must not silently claim a two-path response.
    {
        auto flat = staticTwoPathConfig();
        flat.multipath_enabled = false;
        WattersonChannel ch(flat, seed);
        const Complex h0 = ch.trueFrequencyResponse(800.0f);
        const Complex h1 = ch.trueFrequencyResponse(2200.0f);
        check(std::abs(h0 - h1) < 1e-6f,
              "multipath disabled => response is flat across frequency");
    }

    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
