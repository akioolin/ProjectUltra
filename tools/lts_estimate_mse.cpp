// tools/lts_estimate_mse.cpp
//
// OFFLINE LTS channel-estimate MSE harness.
//
// Goal: measure the per-carrier LTS channel-estimate error ||H_hat - H_true||
// with and without the DFT-domain denoise (ULTRA_LTS_DFT_DENOISE), in isolation
// from the streaming state machine and the GUI. This makes it possible to debug
// the denoise fast and exactly:
//
//   - MSE_on < MSE_off          => the denoise is removing estimator noise (good)
//   - self_distortion ~= 0      => the denoise leaves a CLEAN estimate untouched
//   - self_distortion large     => the denoise is corrupting H even with no noise
//                                  (a bug: it is throwing away real channel energy
//                                  or ringing the band edges)
//
// Ground truth H_true is the noiseless LTS estimate measured THROUGH the
// demodulator's own frontend (denoise OFF). That folds the receiver frontend
// (FIR/FFT/windowing) into the reference, so we compare like-for-like and the
// metric isolates the denoise, not the frontend.
//
// Build:  cmake --build build -j4 --target lts_estimate_mse
// Run:    ./build/lts_estimate_mse

#include "ultra/ofdm.hpp"
#include "ultra/types.hpp"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using ultra::Complex;

namespace {

// Normalized MSE: sum|a-b|^2 / sum|b|^2 over the active carriers.
double normMSE(const std::vector<Complex>& a, const std::vector<Complex>& b) {
    const size_t n = std::min(a.size(), b.size());
    double num = 0.0;
    double den = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const Complex d = a[i] - b[i];
        num += static_cast<double>(d.real()) * d.real() +
               static_cast<double>(d.imag()) * d.imag();
        den += static_cast<double>(b[i].real()) * b[i].real() +
               static_cast<double>(b[i].imag()) * b[i].imag();
    }
    if (den <= 0.0) return 0.0;
    return num / den;
}

// Real linear convolution of a passband sample buffer with a real multipath
// impulse response. Output is truncated to the input length (we only care about
// the body of the LTS, and the trailing tap energy lands in the CP/next-symbol
// guard region which the demod ignores).
std::vector<float> convolveStatic(const std::vector<float>& x,
                                  const std::vector<std::pair<int, float>>& taps) {
    std::vector<float> y(x.size(), 0.0f);
    for (size_t n = 0; n < x.size(); ++n) {
        float acc = 0.0f;
        for (const auto& tap : taps) {
            const int delay = tap.first;
            const float gain = tap.second;
            const long src = static_cast<long>(n) - delay;
            if (src >= 0 && static_cast<size_t>(src) < x.size()) {
                acc += gain * x[static_cast<size_t>(src)];
            }
        }
        y[n] = acc;
    }
    return y;
}

double rms(const std::vector<float>& x) {
    double sum = 0.0;
    for (float v : x) sum += static_cast<double>(v) * v;
    return std::sqrt(sum / static_cast<double>(std::max<size_t>(1, x.size())));
}

}  // namespace

int main() {
    std::printf(
        "LTS channel-estimate MSE harness (offline, no GUI)\n"
        "  ground truth = noiseless LTS estimate, denoise OFF (through the demod frontend)\n"
        "  MSE_off / MSE_on : normalized ||H_hat - H_true||^2 / ||H_true||^2\n"
        "  self_distortion  : denoise ON applied to the NOISELESS estimate -> should be ~0\n"
        "                     (large => the denoise corrupts the clean estimate = a BUG)\n"
        "  verdict          : DENOISE HELPS if MSE_on < MSE_off, else DENOISE HURTS\n\n");

    // --- Config: MUST mirror the PRODUCTION OFDM data path (StreamingEncoder ctor:
    //     fft_size=1024, num_carriers=59, CP=LONG, pilots every 10). The estimator behaves
    //     differently per FFT/carrier geometry, so the harness has to match it to be a
    //     faithful test (an earlier 512/48 config tested the wrong modem).
    ultra::ModemConfig config;
    config.sample_rate = 48000;
    config.fft_size = 1024;
    config.num_carriers = 59;
    config.center_freq = 1500.0f;
    config.cp_mode = ultra::CyclicPrefixMode::LONG;
    config.use_pilots = true;
    config.pilot_spacing = 10;

    // --- TX: generate 2 LTS training symbols (real passband samples).
    ultra::OFDMModulator modulator(config);
    ultra::Samples lts = modulator.generateTrainingSymbols(2);
    std::printf("Generated %zu LTS samples (2 training symbols), tx RMS=%.4f\n",
                lts.size(), rms(lts));

    // --- Static multipath channel: main tap at delay 0 + a 0.3 echo at 24
    //     samples (~0.5 ms at 48 kHz, "Good"-like). Real linear convolution.
    const std::vector<std::pair<int, float>> channel_taps = {{0, 1.0f}, {24, 0.3f}};
    ultra::Samples lts_ch = convolveStatic(lts, channel_taps);
    std::printf("Channel: tap0=1.0 @0, tap1=0.3 @24 samp; lts_ch RMS=%.4f\n\n",
                rms(lts_ch));

    ultra::OFDMDemodulator demod(config);

    // --- GROUND TRUTH: noiseless, denoise OFF.
    demod.reset();
    demod.setLtsDftDenoise(false, 0);
    demod.estimateChannelFromLTSTest(lts_ch.data(), 2);
    const std::vector<Complex> H_true = demod.getActiveChannelEstimate();

    // Sanity: H_true must carry energy (the estimate actually populated).
    double h_true_energy = 0.0;
    for (const Complex& h : H_true) h_true_energy += std::norm(h);
    std::printf("H_true: %zu active carriers, energy=%.4f\n\n",
                H_true.size(), h_true_energy);

    // --- Diagnostic: carrier-domain impulse of H_true (no de-slope) — where does
    //     the channel energy actually sit? If it's beyond Lc it gets clipped.
    {
        const size_t M = H_true.size();
        std::vector<double> mag(M, 0.0);
        double tot = 0.0;
        for (size_t n = 0; n < M; ++n) {
            Complex acc(0, 0);
            for (size_t i = 0; i < M; ++i)
                acc += H_true[i] *
                       std::polar(1.0f, 2.0f * float(M_PI) * float(n * i) / float(M));
            mag[n] = std::abs(acc) / float(M);
            tot += mag[n] * mag[n];
        }
        std::printf("Carrier-domain impulse of H_true (taps with >0.5%% energy):\n  ");
        for (size_t n = 0; n < M; ++n) {
            const double frac = mag[n] * mag[n] / std::max(1e-12, tot);
            if (frac > 0.005)
                std::printf("[%zu]=%.0f%% ", n, 100.0 * frac);
        }
        std::printf("\n\n");
    }

    // --- Lc sweep: self_distortion (denoise ON on the NOISELESS channel) vs window.
    //     -> ~0 as Lc grows  => the window was clipping real channel (fix Lc sizing).
    //     stays high         => the transform/de-slope itself is the bug.
    std::printf("Lc sweep (self_distortion on noiseless channel; want ~0):\n");
    for (int Lc : {2, 4, 6, 12, 24, 40, static_cast<int>(H_true.size()) - 2}) {
        if (Lc < 1) continue;
        demod.reset();
        demod.setLtsDftDenoise(true, Lc);
        demod.estimateChannelFromLTSTest(lts_ch.data(), 2);
        const double sd = normMSE(demod.getActiveChannelEstimate(), H_true);
        std::printf("  Lc=%-3d  self_distortion=%.4f\n", Lc, sd);
    }
    std::printf("\n");

    // Sweep the smoothing half-width W per SNR to find the optimum (narrow = the channel
    // is 92% in tap 0). For each SNR: MSE_off once, then MSE_on(W) for several W.
    const int W_sweep[] = {1, 2, 3, 4, 6, 10};
    const int snrs[] = {12, 16, 20};
    for (int snr_db : snrs) {
        std::mt19937 rng0(0xC0FFEEu + static_cast<unsigned>(snr_db));
        std::normal_distribution<float> g0(0.0f, 1.0f);
        const double sr = rms(lts_ch);
        const double nr = sr / std::sqrt(std::pow(10.0, snr_db / 10.0));
        ultra::Samples noisy = lts_ch;
        for (size_t i = 0; i < noisy.size(); ++i) noisy[i] += static_cast<float>(nr) * g0(rng0);
        demod.reset(); demod.setLtsDftDenoise(false, 0);
        demod.estimateChannelFromLTSTest(noisy.data(), 2);
        const double mse_off_q = normMSE(demod.getActiveChannelEstimate(), H_true);
        std::printf("SNR=%-2d  MSE_off=%.4f  ", snr_db, mse_off_q);
        for (int W : W_sweep) {
            demod.reset(); demod.setLtsDftDenoise(true, W);
            demod.estimateChannelFromLTSTest(noisy.data(), 2);
            const double mse_on_q = normMSE(demod.getActiveChannelEstimate(), H_true);
            std::printf("W%d=%.4f%s ", W, mse_on_q, (mse_on_q < mse_off_q ? "*" : " "));
        }
        std::printf("  (* = beats MSE_off)\n");
    }
    std::printf("\n--- detail table (fixed W) ---\n");

    const int denoise_taps = 2;
    const int snrs2[] = {12, 16, 20};

    for (int snr_db : snrs2) {
        // Build a FIXED-SEED AWGN vector scaled so the per-sample SNR vs the
        // channel-output RMS equals snr_db. The SAME noise vector is reused for
        // the OFF and ON runs so the comparison is apples-to-apples.
        std::mt19937 rng(0xC0FFEEu + static_cast<unsigned>(snr_db));
        std::normal_distribution<float> gauss(0.0f, 1.0f);

        const double sig_rms = rms(lts_ch);
        const double snr_lin = std::pow(10.0, snr_db / 10.0);
        const double noise_rms = sig_rms / std::sqrt(snr_lin);

        std::vector<float> noise(lts_ch.size());
        for (size_t i = 0; i < noise.size(); ++i) {
            noise[i] = static_cast<float>(noise_rms) * gauss(rng);
        }

        ultra::Samples lts_noisy = lts_ch;
        for (size_t i = 0; i < lts_noisy.size(); ++i) lts_noisy[i] += noise[i];

        // Denoise OFF on the noisy LTS.
        demod.reset();
        demod.setLtsDftDenoise(false, 0);
        demod.estimateChannelFromLTSTest(lts_noisy.data(), 2);
        const std::vector<Complex> H_off = demod.getActiveChannelEstimate();
        const double mse_off = normMSE(H_off, H_true);

        // Denoise ON on the SAME noisy LTS.
        demod.reset();
        demod.setLtsDftDenoise(true, denoise_taps);
        demod.estimateChannelFromLTSTest(lts_noisy.data(), 2);
        const std::vector<Complex> H_on = demod.getActiveChannelEstimate();
        const double mse_on = normMSE(H_on, H_true);

        // Self-distortion: denoise ON applied to the NOISELESS lts_ch. With no
        // noise this should recover H_true (selfdist ~ 0). A large value means
        // the denoise is discarding real channel energy / ringing.
        demod.reset();
        demod.setLtsDftDenoise(true, denoise_taps);
        demod.estimateChannelFromLTSTest(lts_ch.data(), 2);
        const std::vector<Complex> H_cd = demod.getActiveChannelEstimate();
        const double selfdist = normMSE(H_cd, H_true);

        const char* verdict = (mse_on < mse_off) ? "DENOISE HELPS" : "DENOISE HURTS";
        std::printf("SNR=%-2d  MSE_off=%.4f  MSE_on=%.4f  self_distortion=%.4f  -> %s\n",
                    snr_db, mse_off, mse_on, selfdist, verdict);
    }

    // --- NOTCH FIDELITY TEST (GPT-5.5 warning, 2026-05-30): a channel with a DEEP
    //     frequency null. A linear frequency smoother averages neighbor energy INTO the
    //     null -> inflates |H| there -> over-confident LLR on a carrier that carries ~no
    //     signal -> 16QAM/32QAM poison. A correct delay-domain denoise PRESERVES the null
    //     (it is a real within-CP tap-interference feature, not noise). This MEASURES
    //     whether the active denoise smears it. The smooth-channel tables above are blind
    //     to this — the test was incomplete without a notch.
    // --- LEVER (1): CFO-clean 2-LTS averaging vs last-symbol-only (denoise OFF).
    //     The static channel carries no inter-symbol CFO, so this isolates the averaging
    //     gain: two INDEPENDENT noise realizations of the SAME H -> expect MSE_avg ~ MSE_last/2
    //     (-3 dB). (The CFO-alignment robustness is exercised separately on the GUI, where a
    //     real residual CFO rotates symbol-to-symbol.)
    std::printf("\n--- LEVER (1) CFO-clean 2-LTS averaging (denoise OFF; want -3 dB) ---\n");
    for (int snr_db : {8, 12, 16, 20}) {
        std::mt19937 rng(0x1357u + static_cast<unsigned>(snr_db));
        std::normal_distribution<float> g(0.0f, 1.0f);
        const double nr = rms(lts_ch) / std::sqrt(std::pow(10.0, snr_db / 10.0));
        ultra::Samples noisy = lts_ch;
        for (size_t i = 0; i < noisy.size(); ++i) noisy[i] += static_cast<float>(nr) * g(rng);

        demod.reset(); demod.setLtsDftDenoise(false, 0); demod.setLtsCfoAvg(false);
        demod.estimateChannelFromLTSTest(noisy.data(), 2);
        const double mse_last = normMSE(demod.getActiveChannelEstimate(), H_true);

        demod.reset(); demod.setLtsDftDenoise(false, 0); demod.setLtsCfoAvg(true);
        demod.estimateChannelFromLTSTest(noisy.data(), 2);
        const double mse_avg = normMSE(demod.getActiveChannelEstimate(), H_true);

        std::printf("  SNR=%-2d  MSE_last=%.4f  MSE_avg=%.4f  gain=%.2f dB  %s\n",
                    snr_db, mse_last, mse_avg,
                    10.0 * std::log10(mse_last / std::max(1e-12, mse_avg)),
                    (mse_avg < mse_last) ? "<- averaging helps" : "");
    }
    demod.setLtsCfoAvg(false);  // restore default for the tests below

    for (const auto& nc : std::vector<std::pair<const char*, float>>{
             {"shallow g=0.90", 0.90f}, {"deep g=0.98", 0.98f}, {"perfect g=1.00", 1.00f}}) {
        std::printf("\n--- NOTCH FIDELITY [%s, Good delay=24] (want denoise to PRESERVE |H@null|) ---\n",
                    nc.first);
        const std::vector<std::pair<int, float>> notch_taps = {{0, 1.0f}, {24, nc.second}};
        ultra::Samples notch_ref = convolveStatic(lts, notch_taps);

        demod.reset();
        demod.setLtsDftDenoise(false, 0);
        demod.estimateChannelFromLTSTest(notch_ref.data(), 2);
        const std::vector<Complex> Hn_true = demod.getActiveChannelEstimate();

        size_t kn = 0;
        double minmag = 1e9, meanmag = 0.0;
        for (size_t i = 0; i < Hn_true.size(); ++i) {
            const double m = std::abs(Hn_true[i]);
            meanmag += m;
            if (m < minmag) { minmag = m; kn = i; }
        }
        meanmag /= static_cast<double>(std::max<size_t>(1, Hn_true.size()));
        std::printf("  null carrier idx=%zu  |H@null|=%.4f  mean|H|=%.4f  depth=%.1f dB\n",
                    kn, minmag, meanmag, 20.0 * std::log10(meanmag / std::max(1e-9, minmag)));

        const int snr_db = 20;
        std::mt19937 rng(0xBADC0DEu);
        std::normal_distribution<float> g(0.0f, 1.0f);
        const double nr = rms(notch_ref) / std::sqrt(std::pow(10.0, snr_db / 10.0));
        ultra::Samples noisy = notch_ref;
        for (size_t i = 0; i < noisy.size(); ++i) noisy[i] += static_cast<float>(nr) * g(rng);

        demod.reset();
        demod.setLtsDftDenoise(false, 0);
        demod.estimateChannelFromLTSTest(noisy.data(), 2);
        const std::vector<Complex> Hn_off = demod.getActiveChannelEstimate();

        std::printf("  (SNR=%d)  |H@null| true=%.4f  off=%.4f\n",
                    snr_db, std::abs(Hn_true[kn]), std::abs(Hn_off[kn]));
        for (int W : {2, 3, 4, 6}) {
            demod.reset();
            demod.setLtsDftDenoise(true, W);
            demod.estimateChannelFromLTSTest(noisy.data(), 2);
            const std::vector<Complex> Hn_on = demod.getActiveChannelEstimate();
            const double inflate = std::abs(Hn_on[kn]) / std::max(1e-9, static_cast<double>(std::abs(Hn_true[kn])));
            std::printf("  W%d: |H@null| on=%.4f (%.1fx true)  MSE_off=%.4f MSE_on=%.4f  %s\n",
                        W, std::abs(Hn_on[kn]), inflate,
                        normMSE(Hn_off, Hn_true), normMSE(Hn_on, Hn_true),
                        (inflate > 1.5) ? "<-- NULL SMEARED (16QAM poison)" : "");
        }
    }

    std::printf("\n");
    return 0;
}
