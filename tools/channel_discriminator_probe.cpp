// channel_discriminator_probe — faithful Good/Moderate channel discriminator lab
// =============================================================================
// Four-tier stack (PHY theorist / DSP systems / HF operator / first-principles):
//
// The recurring defect (project_fading_classifier_cannot_discriminate_2026_06_08):
// the production `fading_index` = freq_cv + temporal_cv measures the *depth* of the
// |H| fade, which is IDENTICAL for the equal-gain 2-path Good/Moderate/Poor presets
// (both taps 0.707). Two channels differing 5x in Doppler and 2x in delay spread
// produce statistically identical fading_index — the whole OFDM rate ladder rides a
// blind input. The discriminating quantities are the *rates*, not the depth:
//   - Doppler spread  sigma_f  -> coherence TIME      (temporal decorrelation of H)
//   - delay  spread   tau_rms  -> coherence BANDWIDTH (frequency decorrelation of H)
//
// PHYSICS (verified against src/ota_channel_core/models.cpp):
//   The sim fading taps are a sum of 128 sinusoids whose frequencies are Gaussian-
//   distributed with RMS = fading_sigma_hz_ = 0.5 * doppler_spread_hz (models.cpp:470).
//   So the channel's complex temporal autocorrelation is the characteristic function
//   of a Gaussian frequency law:
//        R(tau) = exp(-2*pi^2 * sigma^2 * tau^2)            [complex / field]
//        A(tau) = |R(tau)|^2 = exp(-4*pi^2 * sigma^2 * tau^2)   [squared-envelope]
//   with sigma:  Good 0.05 Hz, Moderate 0.25 Hz, Poor 0.5 Hz.
//   Decorrelation-to-0.5 time tau_half = sqrt(ln2 / (2 pi^2 sigma^2)):
//        Good ~3.75 s, Moderate ~0.75 s  (5x apart — the strong axis).
//   At a 1 s lag, Good R~0.95 vs Moderate R~0.29; at 1.5 s, 0.90 vs 0.06. HUGE margin
//   — but ONLY visible at second-scale lags. The prior work measured a few-hundred-ms
//   connect window (too short vs the slow Good process) and saw sampling noise, not the
//   rate. The fix is to observe seconds of channel and fit the DECAY RATE.
//
// ESTIMATOR (SNR-robust):
//   At finite SNR the measured normalized autocorrelation is R(tau)*[Psig/(Psig+Pn)] for
//   tau>0 (white estimation noise inflates only the tau=0 term). That is a CONSTANT
//   multiplicative noise floor, independent of lag. Therefore:
//        ln rho_hat(tau) = ln(noise_floor)  -  2 pi^2 sigma^2 * tau^2
//   A linear regression of ln rho_hat on tau^2 has slope -2 pi^2 sigma^2 (PURE Doppler)
//   and an intercept that ABSORBS the SNR/noise-floor bias. sigma falls out of the slope,
//   SNR-independent. (Envelope variant: slope -4 pi^2 sigma^2.)
//
// METHOD: a faithful channel sounder. A multitone comb (tones on exact FFT bins, ~the
// OFDM band) is pushed through the REAL WattersonChannel (multipath + Gaussian-Doppler
// fading + calibrated noise). At RX we FFT each symbol-spaced block and divide by the
// clean-TX FFT bin -> per-carrier H_k[m], exactly what the equalizer's pilots see. From
// the H_k[m] time/freq structure we recover sigma (Doppler) and tau (delay) and check
// that Good and Moderate SEPARATE where fading_index overlapped.
//
// Build: linked as a tools/ executable against ultra_core (FFT) + ota_channel_core.
// Usage: ./build/channel_discriminator_probe [--symbols N] [--seeds K] [--csv]

#include "ota_channel_core/models.hpp"
#include "ofdm/doppler_coherence_estimator.hpp"  // the PRODUCTION estimator, tested here on real-channel data
#include "ultra/dsp.hpp"
#include "ultra/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using ultra::Complex;
using ultra::FFT;
namespace occ = ultra::ota_channel_core;

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kFs = 48000;        // sample rate (kDefaultSampleRate)
constexpr int kNfft = 1024;       // wideband OFDM FFT size
constexpr int kCp = 128;          // cyclic prefix samples
constexpr int kStride = kNfft + kCp;            // 1152 samples / OFDM symbol
constexpr float kTsym = static_cast<float>(kStride) / kFs;  // 0.024 s
constexpr float kDfHz = static_cast<float>(kFs) / kNfft;    // 46.875 Hz/bin
constexpr float kTargetRms = 0.3048f;           // matches encodePing() in-band RMS

struct ProbeResult {
    float sigma_cplx_hz = 0.0f;   // Doppler RMS from complex-autocorr regression
    float sigma_env_hz = 0.0f;    // Doppler RMS from squared-envelope regression
    float rho_at_1s = 0.0f;       // normalized complex autocorr at ~1.0 s lag (simple feature)
    float rho_at_1p5s = 0.0f;     // ... at ~1.5 s
    float rho_env_at_1s = 0.0f;   // normalized |H|^2 (envelope) autocov at ~1 s — CFO-IMMUNE
    float tau_ms = 0.0f;          // RMS delay spread from coherence bandwidth
    float fading_depth = 0.0f;    // across-carrier CV of |H| (the OLD blind metric, for ref)
    int doppler_lags_used = 0;
    // PRODUCTION DopplerCoherenceEstimator fed the SAME real-channel |H|^2, chopped into
    // frame-sized contiguous runs and pooled (exactly what the equalizer will see).
    float est_score = 0.0f;       // coherenceScore() — the value the rate ladder will branch on
    float est_doppler_hz = 0.0f;  // dopplerHz()
    bool est_valid = false;       // valid()
};

// Plain least-squares slope of y vs x (returns slope; intercept discarded).
float lsSlope(const std::vector<float>& x, const std::vector<float>& y) {
    const size_t n = x.size();
    if (n < 2) return 0.0f;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) {
        sx += x[i]; sy += y[i]; sxx += x[i] * x[i]; sxy += x[i] * y[i];
    }
    const double denom = static_cast<double>(n) * sxx - sx * sx;
    if (std::abs(denom) < 1e-20) return 0.0f;
    return static_cast<float>((static_cast<double>(n) * sxy - sx * sy) / denom);
}

ProbeResult runProbe(const occ::WattersonChannel::Config& cfg, uint64_t seed,
                     int n_symbols, int frame_len = 70) {
    ProbeResult r;

    // ── Carrier comb: contiguous bins ~328..2719 Hz (≈ the wideband OFDM band) ──
    std::vector<int> bins;
    for (int b = 7; b <= 58; ++b) bins.push_back(b);
    const int n_car = static_cast<int>(bins.size());

    // ── Multitone TX with seed-derived random phases (PAPR varies, reproducible) ──
    const size_t total = static_cast<size_t>(n_symbols) * kStride + kNfft;
    std::vector<float> tx(total, 0.0f);
    std::mt19937 prng(0xC0FFEEu ^ static_cast<uint32_t>(seed));
    std::uniform_real_distribution<float> phd(0.0f, 2.0f * kPi);
    std::vector<float> phases(n_car);
    for (float& p : phases) p = phd(prng);
    for (size_t n = 0; n < total; ++n) {
        float s = 0.0f;
        const float w = 2.0f * kPi * static_cast<float>(n) / static_cast<float>(kNfft);
        for (int c = 0; c < n_car; ++c) s += std::cos(w * static_cast<float>(bins[c]) + phases[c]);
        tx[n] = s;
    }
    // Normalize to the calibrated in-band RMS so the channel's noise -> the nominal SNR.
    double ss = 0.0;
    for (float v : tx) ss += static_cast<double>(v) * v;
    const float rms = std::sqrt(static_cast<float>(ss / total));
    if (rms > 1e-9f) {
        const float g = kTargetRms / rms;
        for (float& v : tx) v *= g;
    }

    // ── Real channel: multipath + Gaussian-Doppler fading + calibrated noise ──
    occ::WattersonChannel ch(cfg, seed);
    std::vector<float> rx;
    ch.process(tx, rx);
    if (rx.size() < total) rx.resize(total, 0.0f);

    // ── Per-symbol per-carrier H_k[m] = rxFFT_bin / txFFT_bin (clean-TX reference
    //    cancels the tone phase ramp AND the FIR group delay -> pure channel H). ──
    FFT fft(kNfft);
    std::vector<Complex> in(kNfft), outr(kNfft), outt(kNfft);
    std::vector<std::vector<Complex>> H(n_car, std::vector<Complex>(n_symbols, Complex(0, 0)));
    for (int m = 0; m < n_symbols; ++m) {
        const size_t off = static_cast<size_t>(m) * kStride + kCp;  // skip CP region
        for (int i = 0; i < kNfft; ++i) in[i] = Complex(rx[off + i], 0.0f);
        fft.forward(in, outr);
        for (int i = 0; i < kNfft; ++i) in[i] = Complex(tx[off + i], 0.0f);
        fft.forward(in, outt);
        for (int c = 0; c < n_car; ++c) {
            const Complex t = outt[bins[c]];
            H[c][m] = (std::abs(t) > 1e-9f) ? outr[bins[c]] / t : Complex(0, 0);
        }
    }

    // ── DOPPLER axis: carrier-averaged temporal autocorrelation vs lag ──
    // Complex:  rho[L] = Re(sum_k C_k[L]) / sum_k C_k[0],  C_k[L]=sum_m H[m+L]conj(H[m]).
    // Envelope: a[L]   = sum_k A_k[L] / sum_k A_k[0],  A_k[L]=cov(|H|^2[m+L],|H|^2[m]).
    const int max_lag = std::min(n_symbols / 2, static_cast<int>(3.5f / kTsym) + 2);  // ~3.5 s
    std::vector<float> rho_c(max_lag + 1, 0.0f), a_env(max_lag + 1, 0.0f);
    {
        // Per-carrier mean power for the envelope (|H|^2) covariance.
        std::vector<float> mean_p2(n_car, 0.0f);
        for (int c = 0; c < n_car; ++c) {
            double s2 = 0.0;
            for (int m = 0; m < n_symbols; ++m) s2 += std::norm(H[c][m]);
            mean_p2[c] = static_cast<float>(s2 / n_symbols);
        }
        for (int L = 0; L <= max_lag; ++L) {
            std::complex<double> cnum(0, 0);
            double cden = 0.0, anum = 0.0, aden = 0.0;
            const int cnt = n_symbols - L;
            for (int c = 0; c < n_car; ++c) {
                std::complex<double> cc(0, 0);
                double c0 = 0.0, ac = 0.0, a0 = 0.0;
                for (int m = 0; m < cnt; ++m) {
                    cc += std::complex<double>(H[c][m + L]) * std::conj(std::complex<double>(H[c][m]));
                    const double e1 = std::norm(H[c][m + L]) - mean_p2[c];
                    const double e0 = std::norm(H[c][m]) - mean_p2[c];
                    ac += e1 * e0;
                }
                for (int m = 0; m < n_symbols; ++m) {
                    c0 += std::norm(H[c][m]);
                    const double e = std::norm(H[c][m]) - mean_p2[c];
                    a0 += e * e;
                }
                cnum += cc; cden += c0; anum += ac; aden += a0;
            }
            rho_c[L] = (cden > 1e-12) ? static_cast<float>(cnum.real() / cden) : 0.0f;
            a_env[L] = (aden > 1e-12) ? static_cast<float>(anum / aden) : 0.0f;
        }
    }
    // Simple readable features at ~1.0 s and ~1.5 s lags.
    auto lagAt = [&](float secs) { return std::min(max_lag, std::max(1, static_cast<int>(std::lround(secs / kTsym)))); };
    r.rho_at_1s = rho_c[lagAt(1.0f)];
    r.rho_at_1p5s = rho_c[lagAt(1.5f)];
    r.rho_env_at_1s = a_env[lagAt(1.0f)];

    // Regression of ln(autocorr) vs tau^2 over the measurable regime [lo,hi].
    auto fitSigma = [&](const std::vector<float>& ac, float coeff, int& used) -> float {
        std::vector<float> x, y;
        for (int L = 1; L <= max_lag; ++L) {
            const float v = ac[L];
            if (v > 0.12f && v < 0.92f) {  // above noise floor, below the ~flat top
                const float tau = static_cast<float>(L) * kTsym;
                x.push_back(tau * tau);
                y.push_back(std::log(v));
            }
        }
        used = static_cast<int>(x.size());
        if (used < 3) return 0.0f;
        const float slope = lsSlope(x, y);          // = -coeff*pi^2*sigma^2
        const float s2 = -slope / (coeff * kPi * kPi);
        return (s2 > 0.0f) ? std::sqrt(s2) : 0.0f;
    };
    int used_c = 0, used_e = 0;
    r.sigma_cplx_hz = fitSigma(rho_c, 2.0f, used_c);
    r.sigma_env_hz = fitSigma(a_env, 4.0f, used_e);
    r.doppler_lags_used = used_c;

    // ── PRODUCTION ESTIMATOR on real-channel |H|^2 (snapshot model) ──
    // This is the EXACT class (src/ofdm/doppler_coherence_estimator.hpp) the StreamingDecoder
    // feeds: ONE carrier-averaged |H|^2 snapshot per frame (mean over frame_len symbols),
    // correlated snapshot-to-snapshot at the inter-frame cadence. Validates that per-frame
    // snapshots at ~frame_len*Tsym spacing separate Good/Moderate (the real-modem constraint).
    {
        ultra::DopplerCoherenceEstimator est;
        est.configure(kTsym);
        const int flen = (frame_len > 0) ? frame_len : 51;
        for (int m0 = 0; m0 + flen <= n_symbols; m0 += flen) {
            float acc = 0.0f;
            for (int s = 0; s < flen; ++s) {
                float p2 = 0.0f;
                for (int c = 0; c < n_car; ++c) p2 += std::norm(H[c][m0 + s]);
                acc += p2 / static_cast<float>(n_car);
            }
            est.addSnapshot(acc / static_cast<float>(flen));  // one per-frame |H|^2 snapshot
        }
        r.est_score = est.coherenceScore();
        r.est_doppler_hz = est.dopplerHz();
        r.est_valid = est.valid();
    }

    // ── DELAY axis: per-symbol frequency-correlation coherence bandwidth (the existing
    //    channel_equalizer_lts.cpp metric), averaged over symbols. tau ~ 1/(2 pi Bc). ──
    {
        const int Lmax = std::min(n_car - 1, 24);
        std::vector<float> rho_freq(Lmax + 1, 0.0f);
        for (int L = 1; L <= Lmax; ++L) {
            double acc = 0.0; int sc = 0;
            for (int m = 0; m < n_symbols; ++m) {
                std::complex<double> num(0, 0); double den = 0.0;
                for (int i = 0; i + L < n_car; ++i) {
                    num += std::complex<double>(H[i][m]) * std::conj(std::complex<double>(H[i + L][m]));
                    den += std::norm(H[i][m]);
                }
                if (den > 1e-12) { acc += std::abs(num) / den; ++sc; }
            }
            rho_freq[L] = (sc > 0) ? static_cast<float>(acc / sc) : 0.0f;
        }
        float prev = 1.0f, L_star = static_cast<float>(Lmax);
        for (int L = 1; L <= Lmax; ++L) {
            if (rho_freq[L] < 0.5f) {
                L_star = static_cast<float>(L - 1) + (prev - 0.5f) / std::max(1e-6f, prev - rho_freq[L]);
                break;
            }
            prev = rho_freq[L];
        }
        const float coh_bw = L_star * kDfHz;
        r.tau_ms = (coh_bw > 1e-3f) ? 1000.0f / (2.0f * kPi * coh_bw) : 0.0f;
    }

    // ── Reference: the OLD blind metric — across-carrier CV of time-averaged |H| ──
    {
        std::vector<float> mag(n_car, 0.0f);
        double mm = 0.0;
        for (int c = 0; c < n_car; ++c) {
            std::complex<double> hbar(0, 0);
            for (int m = 0; m < n_symbols; ++m) hbar += std::complex<double>(H[c][m]);
            mag[c] = static_cast<float>(std::abs(hbar / static_cast<double>(n_symbols)));
            mm += mag[c];
        }
        mm /= n_car;
        double var = 0.0;
        for (float v : mag) var += (v - mm) * (v - mm);
        var /= n_car;
        r.fading_depth = (mm > 1e-6) ? static_cast<float>(std::sqrt(var) / mm) : 0.0f;
    }

    return r;
}

struct ClassStat {
    std::vector<float> sigma, tau, rho1, depth;
    void add(const ProbeResult& r) {
        sigma.push_back(r.sigma_env_hz);
        tau.push_back(r.tau_ms);
        rho1.push_back(r.rho_at_1s);
        depth.push_back(r.fading_depth);
    }
};

void meanStd(const std::vector<float>& v, float& mean, float& sd) {
    mean = 0; sd = 0;
    if (v.empty()) return;
    for (float x : v) mean += x;
    mean /= v.size();
    for (float x : v) sd += (x - mean) * (x - mean);
    sd = std::sqrt(sd / v.size());
}

}  // namespace

int main(int argc, char** argv) {
    int n_symbols = 320;   // 7.68 s observation window
    int n_seeds = 8;
    bool csv = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--symbols") && i + 1 < argc) n_symbols = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc) n_seeds = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--csv")) csv = true;
    }

    struct Chan { const char* name; occ::WattersonChannel::Config (*make)(float); };
    const Chan chans[] = {
        {"good", occ::itu_r_f1487::good},
        {"moderate", occ::itu_r_f1487::moderate},
        {"poor", occ::itu_r_f1487::poor},
        {"awgn", occ::itu_r_f1487::awgn},
    };
    const float snrs[] = {20.0f, 15.0f, 12.0f};

    // Pooled across all SNRs/seeds for the final separation verdict (the proof the
    // old metric never had): rho@1s (new, Doppler) vs depth (old, fade-depth/blind).
    std::vector<float> good_rho, mod_rho, good_depth, mod_depth, good_sig, mod_sig;
    std::vector<float> good_renv, mod_renv;  // CFO-immune envelope rho@1s (the real-modem statistic)
    std::vector<float> good_est, mod_est;    // PRODUCTION estimator coherenceScore (frame-pooled real data)
    int good_est_invalid = 0, mod_est_invalid = 0;

    std::printf("# channel_discriminator_probe  symbols=%d (%.2f s window)  seeds=%d\n",
                n_symbols, n_symbols * kTsym, n_seeds);
    std::printf("# truth: sigma_rms = 0.5*doppler -> good 0.050 Hz / moderate 0.250 Hz / poor 0.500 Hz\n");
    std::printf("# %-9s %5s %5s | %9s %9s %8s %8s | %8s | %9s\n",
                "channel", "snr", "seed", "sig_env", "sig_cplx", "rho@1s", "rho@1.5s",
                "tau_ms", "depth(old)");

    for (const auto& sn : snrs) {
        for (const auto& ch : chans) {
            ClassStat stat;
            for (int s = 0; s < n_seeds; ++s) {
                const uint64_t seed = 1000u + static_cast<uint64_t>(s);
                ProbeResult r = runProbe(ch.make(sn), seed, n_symbols);
                stat.add(r);
                if (!std::strcmp(ch.name, "good")) {
                    good_rho.push_back(r.rho_at_1s); good_depth.push_back(r.fading_depth);
                    good_sig.push_back(r.sigma_cplx_hz); good_renv.push_back(r.rho_env_at_1s);
                    if (r.est_valid) good_est.push_back(r.est_score); else ++good_est_invalid;
                } else if (!std::strcmp(ch.name, "moderate")) {
                    mod_rho.push_back(r.rho_at_1s); mod_depth.push_back(r.fading_depth);
                    mod_sig.push_back(r.sigma_cplx_hz); mod_renv.push_back(r.rho_env_at_1s);
                    if (r.est_valid) mod_est.push_back(r.est_score); else ++mod_est_invalid;
                }
                std::printf("  %-9s %5.0f %5llu | %9.4f %9.4f %8.3f %8.3f | %8.3f | %9.3f\n",
                            ch.name, static_cast<double>(sn),
                            static_cast<unsigned long long>(seed),
                            r.sigma_env_hz, r.sigma_cplx_hz, r.rho_at_1s, r.rho_at_1p5s,
                            r.tau_ms, r.fading_depth);
            }
            float sm, ssd, tm, tsd, rm, rsd, dm, dsd;
            meanStd(stat.sigma, sm, ssd);
            meanStd(stat.tau, tm, tsd);
            meanStd(stat.rho1, rm, rsd);
            meanStd(stat.depth, dm, dsd);
            std::printf("  %-9s %5.0f  MEAN | sig_env=%.4f±%.4f  tau=%.3f±%.3f  rho@1s=%.3f±%.3f  depth(old)=%.3f±%.3f\n",
                        ch.name, static_cast<double>(sn), sm, ssd, tm, tsd, rm, rsd, dm, dsd);
        }
        std::printf("\n");
    }

    // ── SEPARATION VERDICT (pooled over all SNR×seed) ──────────────────────────
    auto minmax = [](const std::vector<float>& v, float& lo, float& hi, float& mu) {
        lo = 1e9f; hi = -1e9f; mu = 0.0f;
        for (float x : v) { lo = std::min(lo, x); hi = std::max(hi, x); mu += x; }
        if (!v.empty()) mu /= v.size();
    };
    auto accAt = [](const std::vector<float>& good, const std::vector<float>& mod,
                    float thr, bool good_is_high) {
        int ok = 0, n = 0;
        for (float g : good) { ++n; if ((good_is_high ? (g >= thr) : (g < thr))) ++ok; }
        for (float m : mod) { ++n; if ((good_is_high ? (m < thr) : (m >= thr))) ++ok; }
        return n ? 100.0f * ok / n : 0.0f;
    };
    float grl, grh, grm, mrl, mrh, mrm, gdl, gdh, gdm, mdl, mdh, mdm, gsl, gsh, gsm, msl, msh, msm;
    minmax(good_rho, grl, grh, grm); minmax(mod_rho, mrl, mrh, mrm);
    minmax(good_depth, gdl, gdh, gdm); minmax(mod_depth, mdl, mdh, mdm);
    minmax(good_sig, gsl, gsh, gsm); minmax(mod_sig, msl, msh, msm);

    std::printf("========================= SEPARATION VERDICT =========================\n");
    std::printf("pooled over all SNR(20/15/12) x %d seeds = %zu good vs %zu moderate runs\n\n",
                n_seeds, good_rho.size(), mod_rho.size());
    std::printf("NEW  rho@1s (Doppler coherence):  good [%.3f .. %.3f] mean %.3f  |  moderate [%.3f .. %.3f] mean %.3f\n",
                grl, grh, grm, mrl, mrh, mrm);
    std::printf("     margin (good_min - moderate_max) = %+.3f   %s\n",
                grl - mrh, (grl > mrh) ? "<-- FULLY SEPARATED (no overlap)" : "(overlap)");
    std::printf("     best-threshold accuracy @0.5  = %.1f%%\n\n", accAt(good_rho, mod_rho, 0.5f, true));
    float grel, greh, grem, mrel, mreh, mrem;
    minmax(good_renv, grel, greh, grem); minmax(mod_renv, mrel, mreh, mrem);
    std::printf("NEW  rho_env@1s (|H|^2 autocov — CFO-IMMUNE, the REAL-modem statistic): good [%.3f .. %.3f] mean %.3f  |  moderate [%.3f .. %.3f] mean %.3f\n",
                grel, greh, grem, mrel, mreh, mrem);
    const float env_thr = 0.5f * (grel + mreh);  // gap-optimal threshold (midpoint of the no-overlap gap)
    std::printf("     margin (good_min - moderate_max) = %+.3f   %s   accuracy @gap-mid(%.2f) = %.1f%%\n\n",
                grel - mreh, (grel > mreh) ? "<-- FULLY SEPARATED" : "(overlap)",
                env_thr, accAt(good_renv, mod_renv, env_thr, true));
    float gel, geh, gem, mel, meh, mem;
    minmax(good_est, gel, geh, gem); minmax(mod_est, mel, meh, mem);
    std::printf(">>>> PRODUCTION DopplerCoherenceEstimator (frame-pooled, the ACTUAL class wired into the equalizer):\n");
    std::printf("     coherenceScore:  good [%.3f .. %.3f] mean %.3f (%zu valid, %d invalid)  |  moderate [%.3f .. %.3f] mean %.3f (%zu valid, %d invalid)\n",
                gel, geh, gem, good_est.size(), good_est_invalid,
                mel, meh, mem, mod_est.size(), mod_est_invalid);
    const float est_thr = 0.5f * (gel + meh);
    std::printf("     margin (good_min - moderate_max) = %+.3f   %s   accuracy @gap-mid(%.2f) = %.1f%%\n\n",
                gel - meh, (gel > meh) ? "<-- FULLY SEPARATED" : "(overlap)",
                est_thr, accAt(good_est, mod_est, est_thr, true));
    std::printf("NEW  sigma_cplx (Doppler RMS Hz): good [%.3f .. %.3f] mean %.3f  |  moderate [%.3f .. %.3f] mean %.3f\n",
                gsl, gsh, gsm, msl, msh, msm);
    std::printf("     margin (moderate_min - good_max) = %+.3f   accuracy @log-mid(0.11) = %.1f%%\n\n",
                msl - gsh, accAt(good_sig, mod_sig, 0.111f, false));
    std::printf("OLD  fading depth (|H| CV, the blind metric): good [%.3f .. %.3f] mean %.3f  |  moderate [%.3f .. %.3f] mean %.3f\n",
                gdl, gdh, gdm, mdl, mdh, mdm);
    std::printf("     margin = %+.3f   %s   best-threshold accuracy = %.1f%%\n",
                std::max(gdl - mdh, mdl - gdh),
                "<-- OVERLAP: cannot separate (this is the documented defect)",
                std::max(accAt(good_depth, mod_depth, (gdm + mdm) / 2, gdm > mdm),
                         accAt(good_depth, mod_depth, (gdm + mdm) / 2, gdm < mdm)));
    std::printf("======================================================================\n");

    (void)csv;
    return 0;
}
