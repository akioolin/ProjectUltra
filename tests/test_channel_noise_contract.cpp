// Phase 0 (EFFECTIVE_SINR handoff §9) acceptance: "analytic/Monte-Carlo AWGN noise
// and SINR agree with truth within 0.25 dB after sufficient averaging."
//
// This pins the NOISE-REFERENCING CONVENTION end to end. Every SNR number in the
// project -- rung anchors, entry floors, the meters, every FER-vs-SNR sweep -- is
// quoted against it, so if it is off by a decibel, every one of those is too.
//
// THE CONVENTION, as implemented:
//   sigma_broadband = kModemReferenceRms * 10^(-(snr_db - 9.64221445)/20)
// added as a PER-REAL-SAMPLE Gaussian. kModemReferenceRms = 0.30482664 is the
// PING's receiver in-band RMS, and 9.64221445 dB is 10*log10(1/0.10858718) where
// 0.10858718 is the fraction of broadband white-noise power the modem bandpass
// retains. Those cancel algebraically:
//   SNR_inband = R^2 / (sigma^2 * frac)
//              = 10^((snr_db - 9.642)/10)^-1 ... = 10^(snr_db/10)
// so the requested --snr IS the in-band SNR. This test measures each link of that
// chain rather than trusting the algebra, because the constants are stored to 8
// digits and nothing was checking they still agree with the filter.
//
// WHY THE "noise added AFTER the channel" CHECK MATTERS. Per-carrier SINR scales
// as |H(f)|^2 ONLY if the noise is injected downstream of the multipath filter. If
// it were injected upstream, the noise would be shaped by |H(f)|^2 too and the
// ratio would be flat -- deep fades would cost nothing, which is physically wrong
// and would silently invalidate every per-carrier SINR and LLR weighting built on
// truth H. The test distinguishes the two by measuring the noise PSD shape through
// a deliberately frequency-selective channel.

#include "ota_channel_core/models.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace {

using Complex = std::complex<float>;
using ultra::ota_channel_core::WattersonChannel;
using ultra::ota_channel_core::kModemInBandNoisePowerFraction;
using ultra::ota_channel_core::kModemReferenceRms;
using ultra::ota_channel_core::modemBandpassFirCoefficients;
using ultra::ota_channel_core::modemReferenceNoiseStddev;

int g_failures = 0;

void check(bool ok, const char* what, double measured_db, double expected_db) {
    std::printf("%s %-62s  measured %+7.3f dB  expected %+7.3f dB  (d=%+.3f)\n",
                ok ? "[ ok ]" : "[FAIL]", what, measured_db, expected_db,
                measured_db - expected_db);
    if (!ok) {
        ++g_failures;
    }
}

void checkPlain(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

constexpr uint32_t kFs = 48000;
constexpr double kToleranceDb = 0.25;  // the handoff's own Phase 0 acceptance bar
constexpr size_t kNoiseSamples = 2000000;  // "sufficient averaging"

double toDb(double power_ratio) {
    return 10.0 * std::log10(std::max(power_ratio, 1e-300));
}

WattersonChannel::Config flatNoisyConfig(float snr_db) {
    WattersonChannel::Config cfg;
    cfg.snr_db = snr_db;
    cfg.delay_spread_ms = 2.0f;
    cfg.doppler_spread_hz = 0.0f;
    cfg.cfo_hz = 0.0f;
    cfg.path1_gain = 0.707f;
    cfg.path2_gain = 0.707f;
    cfg.sample_rate = kFs;
    cfg.fading_enabled = false;
    cfg.multipath_enabled = false;  // pure additive noise, no filtering
    cfg.noise_enabled = true;
    cfg.cfo_enabled = false;
    return cfg;
}

// Run silence through the channel: the output is exactly the injected noise.
std::vector<float> captureInjectedNoise(WattersonChannel::Config cfg, uint64_t seed,
                                        size_t count) {
    WattersonChannel channel(cfg, seed);
    const std::vector<float> silence(count, 0.0f);
    return channel.process(silence);
}

double meanSquare(const std::vector<float>& x, size_t skip = 0) {
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = skip; i < x.size(); ++i) {
        acc += static_cast<double>(x[i]) * static_cast<double>(x[i]);
        ++n;
    }
    return n ? acc / static_cast<double>(n) : 0.0;
}

// Apply the convention-defining bandpass and return the retained power.
double inBandPower(const std::vector<float>& x) {
    const std::vector<float> coeffs = modemBandpassFirCoefficients();
    std::vector<float> filtered;
    filtered.reserve(x.size());
    std::vector<float> delay(coeffs.size(), 0.0f);
    size_t idx = 0;
    for (float s : x) {
        delay[idx] = s;
        float out = 0.0f;
        size_t j = idx;
        for (size_t i = 0; i < coeffs.size(); ++i) {
            out += coeffs[i] * delay[j];
            if (j == 0) {
                j = coeffs.size();
            }
            --j;
        }
        idx = (idx + 1) % coeffs.size();
        filtered.push_back(out);
    }
    // Skip the FIR transient before measuring.
    return meanSquare(filtered, coeffs.size() * 2);
}

// Narrowband noise power at freq_hz, by WELCH AVERAGING over independent segments.
//
// NB: a single long correlation is the wrong tool and gives a wildly wrong answer.
// A one-bin periodogram is chi-squared with 2 DOF, so its variance stays at 100% of
// its mean NO MATTER how long the record -- more samples shrink the bin width rather
// than the error. Measured that way this test read a 31 dB "spread" across a band
// whose noise is flat. Averaging K independent segments is what actually reduces the
// variance, by ~1/K.
double bandPowerAt(const std::vector<float>& x, float freq_hz, size_t skip,
                   size_t segments = 400) {
    if (x.size() <= skip) {
        return 0.0;
    }
    const size_t usable = x.size() - skip;
    const size_t seg_len = usable / segments;
    if (seg_len < 64) {
        return 0.0;
    }
    double acc_power = 0.0;
    for (size_t s = 0; s < segments; ++s) {
        const size_t base = skip + s * seg_len;
        std::complex<double> acc(0.0, 0.0);
        for (size_t i = 0; i < seg_len; ++i) {
            const double ph = -2.0 * M_PI * static_cast<double>(freq_hz) *
                              static_cast<double>(base + i) / static_cast<double>(kFs);
            acc += static_cast<double>(x[base + i]) *
                   std::complex<double>(std::cos(ph), std::sin(ph));
        }
        acc_power += std::norm(acc) / (static_cast<double>(seg_len) *
                                       static_cast<double>(seg_len));
    }
    return acc_power / static_cast<double>(segments);
}

}  // namespace

int main() {
    std::printf("=== AWGN noise-reference contract (Phase 0 acceptance, %.2f dB bar) ===\n",
                kToleranceDb);

    const float snr_db = 20.0f;
    const auto cfg = flatNoisyConfig(snr_db);

    // ---- Link 1: injected per-real-sample sigma matches the published formula.
    const std::vector<float> noise = captureInjectedNoise(cfg, 12345, kNoiseSamples);
    const double measured_var = meanSquare(noise);
    const double expected_sigma = static_cast<double>(modemReferenceNoiseStddev(snr_db));
    const double expected_var = expected_sigma * expected_sigma;
    check(std::abs(toDb(measured_var) - toDb(expected_var)) < kToleranceDb,
          "injected per-real-sample noise power == modemReferenceNoiseStddev^2",
          toDb(measured_var), toDb(expected_var));

    // ---- Link 2: the bandpass retains exactly kModemInBandNoisePowerFraction.
    const double in_band = inBandPower(noise);
    const double measured_fraction = in_band / measured_var;
    check(std::abs(toDb(measured_fraction) - toDb(kModemInBandNoisePowerFraction)) <
              kToleranceDb,
          "bandpass retains kModemInBandNoisePowerFraction of broadband noise",
          toDb(measured_fraction), toDb(kModemInBandNoisePowerFraction));

    // ---- Link 3: the whole convention closes -- requested --snr IS in-band SNR.
    // Signal reference power is kModemReferenceRms^2 by definition of the constant.
    const double signal_power = static_cast<double>(kModemReferenceRms) *
                                static_cast<double>(kModemReferenceRms);
    const double measured_inband_snr_db = toDb(signal_power / in_band);
    check(std::abs(measured_inband_snr_db - static_cast<double>(snr_db)) < kToleranceDb,
          "in-band SNR of a reference-RMS signal == the requested --snr",
          measured_inband_snr_db, static_cast<double>(snr_db));

    // Same closure at a second operating point, so the check is not a coincidence
    // at one SNR (the offset constant would cancel wrongly only at one value).
    {
        const float snr2 = 8.0f;
        const std::vector<float> n2 = captureInjectedNoise(flatNoisyConfig(snr2), 999,
                                                           kNoiseSamples);
        const double snr2_meas = toDb(signal_power / inBandPower(n2));
        check(std::abs(snr2_meas - static_cast<double>(snr2)) < kToleranceDb,
              "same closure holds at a second operating point (8 dB)",
              snr2_meas, static_cast<double>(snr2));
    }

    // ---- Link 4: noise is injected AFTER the multipath filter.
    // Through a deliberately frequency-selective two-path channel the truth response
    // varies by many dB across the band. If noise were injected upstream it would be
    // shaped by |H(f)|^2 and would show the SAME variation; injected downstream it is
    // flat. This is what makes per-carrier SINR scale as |H(f)|^2.
    {
        auto sel = flatNoisyConfig(snr_db);
        sel.fading_enabled = true;
        sel.multipath_enabled = true;
        WattersonChannel reference(sel, 777);

        const std::vector<float> sel_noise = captureInjectedNoise(sel, 777, kNoiseSamples);

        const std::vector<float> probes = {700.0f, 1100.0f, 1500.0f, 1900.0f, 2300.0f};
        double truth_spread_db = 0.0;
        double noise_spread_db = 0.0;
        double truth_min = 1e9;
        double truth_max = -1e9;
        double noise_min = 1e9;
        double noise_max = -1e9;
        for (float f : probes) {
            const double h_db = 20.0 * std::log10(
                std::max(static_cast<double>(std::abs(reference.trueFrequencyResponse(f))),
                         1e-9));
            const double n_db = toDb(bandPowerAt(sel_noise, f, 4096));
            truth_min = std::min(truth_min, h_db);
            truth_max = std::max(truth_max, h_db);
            noise_min = std::min(noise_min, n_db);
            noise_max = std::max(noise_max, n_db);
            std::printf("   f=%6.0f Hz   |H|=%+6.2f dB   noise=%+8.2f dB\n",
                        static_cast<double>(f), h_db, n_db);
        }
        truth_spread_db = truth_max - truth_min;
        noise_spread_db = noise_max - noise_min;
        std::printf("   channel |H| spread = %.2f dB   noise spread = %.2f dB\n",
                    truth_spread_db, noise_spread_db);

        checkPlain(truth_spread_db > 6.0,
                   "probe channel is strongly frequency-selective (test is not vacuous)");
        // MEASURED 2026-07-29: 0.68 dB of noise spread across a band where |H| varies
        // 10.20 dB. Both bounds are asserted -- the absolute one pins the measurement,
        // the relative one is the discriminating comparison (|H|^2-shaped noise would
        // show the channel's full spread).
        checkPlain(noise_spread_db < 1.5,
                   "noise floor is FLAT in absolute terms (< 1.5 dB across the band)");
        checkPlain(noise_spread_db < truth_spread_db * 0.35,
                   "noise floor is FLAT across the band => injected AFTER the channel");
    }

    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
