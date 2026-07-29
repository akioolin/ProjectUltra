// Delay-domain (CIR) pilot reconstruction — ULTRA_PILOT_DFT_INTERP.
//
// FAIL-BEFORE / PASS-AFTER PIN
// ----------------------------
// The claim under test: on a DELAY-SPARSE channel (ITU-R F.1487 "Good" = two paths
// 0.5 ms apart => |H| ripples with period 1/tau = 2000 Hz), linear interpolation
// between pilots is the WRONG reconstruction filter, and a delay-domain model fit
// is the right one. Every test here runs BOTH reconstructions on the SAME pilot
// samples of the SAME synthetic channel and compares them.
//
// Why a gain error is fatal for 16-QAM and invisible to 8PSK (the reason this
// matters at all): 16-QAM's ring bit is an ABSOLUTE amplitude comparison
// (soft_demap.hpp:95, |I| vs QAM16_THRESHOLD = 2/sqrt(10)), so with levels
// +-0.3162/+-0.9487 a gain error s = |H|/|H_hat| flips the OUTER ring bit at
// s < 0.667 and the INNER at s > 2.0. 8PSK LLRs are distances to EIGHT UNIT-RADIUS
// references, so scaling the symbol scales every LLR equally and flips nothing.
// There is no AGC between equalizer and demapper, so |H_hat| is the sole reference.
//
// HONEST SCOPE (this is the part the design brief got wrong):
// "IDFT to CIR, keep taps inside the CP, DFT back" is EXACT for a 2-tap channel
// ONLY when the tap window W is no wider than the pilot count Np. With W > Np the
// system is UNDERDETERMINED — the estimator reproduces H at the pilots but returns
// the minimum-norm/MMSE solution between them, which is very good but not exact.
// Both regimes are pinned below: test 2 is the exactness pin (W <= Np), test 3 is
// the production-geometry accuracy pin (W >> Np).

#include "../src/ofdm/delay_domain_interpolator.hpp"
#include "../src/ofdm/pilot_pattern.hpp"

#include <cmath>
#include <complex>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ultra;

namespace {

int tests_passed = 0;
int tests_failed = 0;

#define TEST(name) \
    std::cout << "Testing " << name << "... "; \
    try

#define PASS() \
    std::cout << "PASS\n"; \
    ++tests_passed

#define FAIL(msg) \
    std::cout << "FAIL: " << msg << "\n"; \
    ++tests_failed

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// ---------------------------------------------------------------------------
// Synthetic channel + geometry helpers
// ---------------------------------------------------------------------------

struct Tap {
    int delay_samples;
    Complex gain;
};

// H(k) = sum_m c_m exp(-j 2 pi k m / Nfft). Sign convention is irrelevant to the
// estimator (its delay window is symmetric about zero), but the model and the
// truth must agree, so both use this one function.
Complex channelAt(const std::vector<Tap>& taps, int k, int fft_size) {
    Complex h(0.0f, 0.0f);
    for (const Tap& t : taps) {
        const float phase = -2.0f * static_cast<float>(M_PI) *
                            static_cast<float>(k) *
                            static_cast<float>(t.delay_samples) /
                            static_cast<float>(fft_size);
        h += t.gain * Complex(std::cos(phase), std::sin(phase));
    }
    return h;
}

// The production carrier layout: k = -neg_limit..+ceil, SKIPPING DC. The skip is
// why logical index is not proportional to frequency.
std::vector<int> physicalCarriers(int num_carriers) {
    std::vector<int> ks;
    const int neg_limit = num_carriers / 2;
    for (int k = -neg_limit; k <= (num_carriers + 1) / 2; ++k) {
        if (k == 0) continue;
        ks.push_back(k);
    }
    return ks;
}

// The REAL scattered-pilot mask, straight from production
// (ofdm_pilots::isPilotLogical). Note it wraps modulo num_carriers, so the grid
// is uniform in the interior with ONE short gap at the wrap — a detail the
// physical-carrier-index model handles for free and a uniform-grid FFT does not.
std::vector<bool> pilotMask(int num_carriers, int spacing, size_t symbol_index) {
    ModemConfig cfg;
    cfg.num_carriers = static_cast<uint32_t>(num_carriers);
    cfg.pilot_spacing = static_cast<uint32_t>(spacing);
    cfg.use_pilots = true;
    cfg.scattered_pilots = true;
    cfg.modulation = Modulation::QAM16;  // coherent => scattered pattern active
    std::vector<bool> mask(static_cast<size_t>(num_carriers), false);
    for (size_t l = 0; l < mask.size(); ++l) {
        mask[l] = ofdm_pilots::isPilotLogical(cfg, l, symbol_index);
    }
    return mask;
}

// Mirror of Impl::buildInterpTable + the complex linear interpolation in
// channel_equalizer_pilot.cpp: interpolate in LOGICAL index between the bracketing
// pilots, hold the nearest pilot outside the pilot span.
std::vector<Complex> linearReconstruct(const std::vector<int>& ks,
                                       const std::vector<bool>& is_pilot,
                                       const std::vector<Complex>& truth) {
    const size_t n = ks.size();
    std::vector<Complex> out(n, Complex(0, 0));
    for (size_t i = 0; i < n; ++i) {
        if (is_pilot[i]) {
            out[i] = truth[i];
            continue;
        }
        int lo = -1, hi = -1;
        for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
            if (is_pilot[static_cast<size_t>(j)]) { lo = j; break; }
        }
        for (size_t j = i + 1; j < n; ++j) {
            if (is_pilot[j]) { hi = static_cast<int>(j); break; }
        }
        if (lo >= 0 && hi >= 0) {
            const float alpha = static_cast<float>(static_cast<int>(i) - lo) /
                                static_cast<float>(hi - lo);
            out[i] = (1.0f - alpha) * truth[static_cast<size_t>(lo)] +
                     alpha * truth[static_cast<size_t>(hi)];
        } else if (lo >= 0) {
            out[i] = truth[static_cast<size_t>(lo)];
        } else if (hi >= 0) {
            out[i] = truth[static_cast<size_t>(hi)];
        }
    }
    return out;
}

std::vector<Complex> delayDomainReconstruct(const std::vector<int>& ks,
                                            const std::vector<bool>& is_pilot,
                                            const std::vector<Complex>& truth,
                                            int taps,
                                            uint32_t fft_size,
                                            float noise_norm) {
    std::vector<ofdm_wiener::Observation1D> obs;
    for (size_t i = 0; i < ks.size(); ++i) {
        if (!is_pilot[i]) continue;
        obs.push_back(ofdm_wiener::Observation1D{
            static_cast<float>(ks[i]), truth[i], noise_norm});
    }
    std::vector<Complex> out(ks.size(), Complex(0, 0));
    for (size_t i = 0; i < ks.size(); ++i) {
        const auto est = ofdm_cir::reconstruct(
            obs, static_cast<float>(ks[i]), taps, fft_size, obs.size());
        require(est.valid, "delay-domain reconstruction returned invalid");
        out[i] = est.value;
    }
    return out;
}

struct ErrorStats {
    float rms_rel = 0.0f;         // RMS |H_hat - H| / RMS |H|
    float worst_gain_db = 0.0f;   // worst |H_hat|/|H| over TRUSTED carriers
    float worst_gain_db_all = 0.0f;  // ... over every carrier, nulls included
};

// A carrier sitting in a deep spectral null carries no decision: |H| -> 0 makes ANY
// absolute error an unbounded dB error, which is why the gain metric is evaluated
// over the carriers the LLR path still trusts at face value. The boundary is the
// production one — relativeFadeNoiseInflation() in channel_equalizer_equalize.cpp
// starts inflating a carrier's LLR noise variance below kRelFadeOnset = 0.25 of the
// frame mean |H|^2 (i.e. -6 dB in amplitude) and down-weights it by up to 15 dB.
// Above that boundary the demapper takes |H_hat| literally, so that is exactly where
// a reconstruction gain error turns into a wrong 16-QAM ring bit.
constexpr float kTrustedRelPower = 0.25f;

ErrorStats measure(const std::vector<Complex>& est,
                   const std::vector<Complex>& truth) {
    double err = 0.0, sig = 0.0;
    for (size_t i = 0; i < truth.size(); ++i) {
        err += std::norm(est[i] - truth[i]);
        sig += std::norm(truth[i]);
    }
    const double mean_power = sig / std::max<size_t>(truth.size(), 1);

    float worst_db = 0.0f, worst_db_all = 0.0f;
    for (size_t i = 0; i < truth.size(); ++i) {
        const float a = std::abs(est[i]);
        const float b = std::abs(truth[i]);
        if (a <= 1e-9f || b <= 1e-9f) continue;
        const float db = 20.0f * std::log10(a / b);
        if (std::abs(db) > std::abs(worst_db_all)) worst_db_all = db;
        if (std::norm(truth[i]) >= kTrustedRelPower * mean_power &&
            std::abs(db) > std::abs(worst_db)) {
            worst_db = db;
        }
    }
    ErrorStats s;
    s.rms_rel = static_cast<float>(std::sqrt(err / std::max(sig, 1e-30)));
    s.worst_gain_db = worst_db;
    s.worst_gain_db_all = worst_db_all;
    return s;
}

// ---------------------------------------------------------------------------
// 1. Kernel + window selection are what they claim to be
// ---------------------------------------------------------------------------

void test_kernel_is_the_dirichlet_kernel_of_the_window() {
    TEST("Dirichlet kernel matches a direct sum over the delay window") {
        const uint32_t fft = 1024;
        const int taps = 49;   // m in [-24, +24]
        const int half = 24;
        float worst = 0.0f;
        for (int dk = -80; dk <= 80; ++dk) {
            // Direct evaluation of (1/W) * sum_{m=-M..M} exp(-j2 pi dk m / Nfft).
            Complex sum(0, 0);
            for (int m = -half; m <= half; ++m) {
                const float ph = -2.0f * static_cast<float>(M_PI) *
                                 static_cast<float>(dk) * static_cast<float>(m) /
                                 static_cast<float>(fft);
                sum += Complex(std::cos(ph), std::sin(ph));
            }
            const float direct = sum.real() / static_cast<float>(taps);
            require(std::abs(sum.imag()) < 1e-3f,
                    "symmetric window kernel must be real");
            const float closed = ofdm_cir::carrierCorrelation(
                static_cast<float>(dk), taps, fft);
            worst = std::max(worst, std::abs(direct - closed));
        }
        std::printf("[kernel max |direct-closed| = %.2e] ", worst);
        require(worst < 1e-4f, "closed form disagrees with the direct sum");
        require(std::abs(ofdm_cir::carrierCorrelation(0.0f, taps, fft) - 1.0f) < 1e-6f,
                "R(0) must be 1");
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_window_selection_respects_aliasing_and_cp() {
    TEST("delay window is clamped by the pilot grid and the cyclic prefix") {
        // Production wideband: fft 1024, CP 96 (MEDIUM at 1024 = 48*2), 48 kHz.
        // 1 ms span = 48 taps -> 47 odd, under both caps.
        auto w = ofdm_cir::chooseDelayWindow(1.0e-3f, 48000, 1024, 8, 96);
        require(w.valid(), "window must be valid");
        require(w.taps == 47, "1 ms at 48 kHz should give 47 taps, got " +
                                  std::to_string(w.taps));
        require(!w.alias_clamped && !w.cp_clamped, "1 ms should not be clamped");

        // Unambiguous delay range = fft/spacing. At spacing 12 that is 85 samples;
        // a 3 ms (144-tap) request must clamp there, not silently alias.
        auto w12 = ofdm_cir::chooseDelayWindow(3.0e-3f, 48000, 1024, 12, 1024);
        require(w12.taps == 85, "spacing-12 alias cap should be 85 taps, got " +
                                    std::to_string(w12.taps));
        require(w12.alias_clamped, "spacing-12 3 ms request must report aliasing clamp");

        // A tap past the cyclic prefix is ISI, not a channel the OFDM model can
        // represent: CP is a hard cap and it binds before aliasing here.
        auto wcp = ofdm_cir::chooseDelayWindow(3.0e-3f, 48000, 1024, 5, 96);
        require(wcp.taps == 95, "CP cap should give 95 taps, got " +
                                    std::to_string(wcp.taps));
        require(wcp.cp_clamped, "must report the CP clamp");

        // Always odd (keeps the kernel real, even, and fft-periodic).
        for (float ms = 0.1f; ms < 4.0f; ms += 0.1f) {
            auto wi = ofdm_cir::chooseDelayWindow(ms * 1e-3f, 48000, 1024, 8, 1024);
            require((wi.taps % 2) == 1, "window width must be odd");
            require(wi.taps == 2 * wi.half_taps + 1, "half_taps inconsistent");
        }
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

// ---------------------------------------------------------------------------
// 2. EXACTNESS PIN: W <= Np and the true taps inside the window
// ---------------------------------------------------------------------------
//
// Geometry chosen so that the ripple is the PRODUCTION ripple (period ~43
// carriers = the ITU Good 2000 Hz ripple over a 46.875 Hz carrier grid) while the
// delay window still fits inside the pilot count: fft_size 128, taps at 0 and 3
// samples => ripple period 128/3 = 42.7 carriers. 59 carriers, pilots every 8 =>
// Np = 8 pilots, window W = 7 taps (m in [-3,+3]) <= Np. Overdetermined and
// noiseless => the LS/MMSE fit must return the truth to float precision.

void test_two_tap_channel_is_reconstructed_exactly() {
    TEST("2-tap delay-sparse channel: delay-domain is EXACT where linear is not") {
        const uint32_t fft = 128;
        const int num_carriers = 59;
        const int spacing = 8;
        const std::vector<Tap> taps = {{0, Complex(0.707f, 0.0f)},
                                       {3, Complex(0.0f, 0.707f)}};

        const auto ks = physicalCarriers(num_carriers);
        auto win = ofdm_cir::chooseDelayWindow(
            7.0f / 48000.0f, 48000, fft, spacing, fft);
        // 7 samples of span at 48 kHz -> 7 taps; assert we got the window we meant.
        require(win.taps == 7, "expected a 7-tap window, got " +
                                   std::to_string(win.taps));

        std::vector<Complex> truth(ks.size());
        for (size_t i = 0; i < ks.size(); ++i) {
            truth[i] = channelAt(taps, ks[i], static_cast<int>(fft));
        }

        // Worst case over every scattered-pilot phase (pilots rotate one carrier
        // per symbol), so neither method gets a lucky alignment.
        float worst_cir_rms = 0.0f, worst_lin_rms = 0.0f;
        float worst_cir_db = 0.0f, worst_lin_db = 0.0f;
        size_t np = 0;
        for (int offset = 0; offset < spacing; ++offset) {
            const auto is_pilot = pilotMask(num_carriers, spacing,
                                            static_cast<size_t>(offset));
            np = 0;
            for (bool p : is_pilot) np += p ? 1 : 0;
            require(static_cast<size_t>(win.taps) <= np,
                    "exactness requires W <= Np");

            const auto cir = delayDomainReconstruct(ks, is_pilot, truth, win.taps,
                                                    fft, 1.0e-6f);
            const auto lin = linearReconstruct(ks, is_pilot, truth);
            const auto e_cir = measure(cir, truth);
            const auto e_lin = measure(lin, truth);
            worst_cir_rms = std::max(worst_cir_rms, e_cir.rms_rel);
            worst_lin_rms = std::max(worst_lin_rms, e_lin.rms_rel);
            worst_cir_db = std::max(worst_cir_db, std::abs(e_cir.worst_gain_db));
            worst_lin_db = std::max(worst_lin_db, std::abs(e_lin.worst_gain_db));
        }

        std::printf("[Np=%zu W=%d | CIR rms=%.2e gain=%.2f dB | "
                    "LINEAR rms=%.3f gain=%.2f dB] ",
                    np, win.taps, worst_cir_rms, worst_cir_db,
                    worst_lin_rms, worst_lin_db);

        // Tight tolerance: an overdetermined, noiseless fit in float arithmetic.
        const float tol = 1.0e-3f;
        require(worst_cir_rms < tol,
                "delay-domain reconstruction is not exact (rms " +
                    std::to_string(worst_cir_rms) + ")");
        // The SAME tolerance must fail for the current linear path — that is the
        // fail-before half of the pin.
        require(worst_lin_rms > tol * 20.0f,
                "linear interpolation unexpectedly met the tolerance (rms " +
                    std::to_string(worst_lin_rms) + ") — the geometry no longer "
                    "exercises the defect");
        // And the failure mode is specifically a GAIN error, which is what breaks
        // 16-QAM's absolute ring threshold (outer ring flips at +3.52 dB).
        require(worst_lin_db > 3.52f,
                "linear path should cross the 16-QAM ring-flip point");
        require(worst_cir_db < 0.05f,
                "delay-domain path must not introduce a gain error");
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

// ---------------------------------------------------------------------------
// 3. PRODUCTION GEOMETRY: W >> Np — accuracy, not exactness
// ---------------------------------------------------------------------------
//
// fft 1024, 59 carriers, pilot spacing 8 (the R2/3 production grid), ITU Good taps
// at 0 and 24 samples (0.5 ms at 48 kHz), window 47 taps (~1 ms). W = 47 >> Np = 8,
// so this is underdetermined and the estimator returns the MMSE solution. It is NOT
// exact — 8 pilots spanning 56 carriers resolve delays no finer than 1024/56 = 18.3
// samples, and the two taps are only 24 samples apart. The pin is that it is still
// far better than linear, and specifically that the GAIN error collapses.

void test_production_geometry_beats_linear_on_itu_good() {
    TEST("production sp8 grid, ITU Good 2-tap: gain error collapses vs linear") {
        const uint32_t fft = 1024;
        const int num_carriers = 59;
        const int spacing = 8;
        // 0.5 ms at 48 kHz = 24 samples; equal-power paths (path1_gain =
        // path2_gain = 0.707, models.cpp itu_r_f1487::good()).
        const std::vector<Tap> taps = {{0, Complex(0.707f, 0.0f)},
                                       {24, Complex(0.0f, 0.707f)}};

        const auto ks = physicalCarriers(num_carriers);
        float worst_lin_rms = 0.0f, worst_cir_rms = 0.0f;
        float worst_lin_db = 0.0f, worst_cir_db = 0.0f;
        float worst_lin_db_all = 0.0f, worst_cir_db_all = 0.0f;

        // Sweep the scattered-pilot offset: pilots rotate one carrier per symbol
        // (pilot_pattern.hpp isPilotLogical), so every offset is a real operating
        // point and the worst one is what the FER sees.
        for (int offset = 0; offset < spacing; ++offset) {
            const auto is_pilot = pilotMask(num_carriers, spacing,
                                            static_cast<size_t>(offset));
            std::vector<Complex> truth(ks.size());
            for (size_t i = 0; i < ks.size(); ++i) {
                truth[i] = channelAt(taps, ks[i], static_cast<int>(fft));
            }
            auto win = ofdm_cir::chooseDelayWindow(1.0e-3f, 48000, fft, spacing, 96);
            const auto cir = delayDomainReconstruct(ks, is_pilot, truth, win.taps,
                                                    fft, 1.0e-4f);
            const auto lin = linearReconstruct(ks, is_pilot, truth);
            const auto e_cir = measure(cir, truth);
            const auto e_lin = measure(lin, truth);
            worst_cir_rms = std::max(worst_cir_rms, e_cir.rms_rel);
            worst_lin_rms = std::max(worst_lin_rms, e_lin.rms_rel);
            worst_cir_db = std::max(worst_cir_db, std::abs(e_cir.worst_gain_db));
            worst_lin_db = std::max(worst_lin_db, std::abs(e_lin.worst_gain_db));
            worst_cir_db_all = std::max(worst_cir_db_all,
                                        std::abs(e_cir.worst_gain_db_all));
            worst_lin_db_all = std::max(worst_lin_db_all,
                                        std::abs(e_lin.worst_gain_db_all));
        }

        std::printf("[worst over 8 pilot offsets | CIR rms=%.4f gain=%.2f dB "
                    "(all carriers %.2f) | LINEAR rms=%.4f gain=%.2f dB "
                    "(all carriers %.2f)] ",
                    worst_cir_rms, worst_cir_db, worst_cir_db_all,
                    worst_lin_rms, worst_lin_db, worst_lin_db_all);

        // 16-QAM flips its OUTER ring bit at a +3.52 dB over-read of |H_hat| and
        // its INNER at -6.02 dB (levels +-0.3162/+-0.9487 against
        // QAM16_THRESHOLD = 2/sqrt(10)). Linear must cross that; the delay-domain
        // fit must stay comfortably inside it.
        require(worst_lin_db > 3.52f,
                "linear path no longer crosses the 16-QAM outer-ring flip point "
                "(" + std::to_string(worst_lin_db) + " dB) — geometry changed");
        require(worst_cir_db < 3.52f,
                "delay-domain path crosses the 16-QAM ring flip point (" +
                    std::to_string(worst_cir_db) + " dB)");
        require(worst_cir_rms < 0.5f * worst_lin_rms,
                "delay-domain must at least halve the reconstruction error");
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

// ---------------------------------------------------------------------------
// 4. Noise rejection: truncating the CIR rejects out-of-support pilot noise
// ---------------------------------------------------------------------------

void test_delay_truncation_rejects_pilot_noise() {
    TEST("delay-window truncation rejects pilot noise (helps at low SNR too)") {
        const uint32_t fft = 1024;
        const int num_carriers = 59;
        const int spacing = 8;
        const std::vector<Tap> taps = {{0, Complex(0.707f, 0.0f)},
                                       {24, Complex(0.0f, 0.707f)}};
        const auto ks = physicalCarriers(num_carriers);
        const auto is_pilot = pilotMask(num_carriers, spacing, 0);
        std::vector<Complex> truth(ks.size());
        for (size_t i = 0; i < ks.size(); ++i) {
            truth[i] = channelAt(taps, ks[i], static_cast<int>(fft));
        }

        // Deterministic pseudo-noise on the PILOTS only (that is what the LS pilot
        // estimate carries), same realization for both reconstructions.
        const float sigma = 0.10f;  // ~20 dB pilot SNR
        std::vector<Complex> noisy = truth;
        uint32_t rng = 12345u;
        auto nextUniform = [&rng]() {
            rng = rng * 1664525u + 1013904223u;
            return static_cast<float>((rng >> 8) & 0xFFFFu) / 65535.0f - 0.5f;
        };
        for (size_t i = 0; i < ks.size(); ++i) {
            if (!is_pilot[i]) continue;
            noisy[i] += Complex(sigma * 3.46f * nextUniform(),
                                sigma * 3.46f * nextUniform());
        }

        auto win = ofdm_cir::chooseDelayWindow(1.0e-3f, 48000, fft, spacing, 96);
        const auto cir = delayDomainReconstruct(ks, is_pilot, noisy, win.taps, fft,
                                                sigma * sigma);
        const auto lin = linearReconstruct(ks, is_pilot, noisy);
        const auto e_cir = measure(cir, truth);
        const auto e_lin = measure(lin, truth);
        std::printf("[pilot sigma=%.2f | CIR rms=%.4f | LINEAR rms=%.4f] ",
                    sigma, e_cir.rms_rel, e_lin.rms_rel);
        require(e_cir.rms_rel < e_lin.rms_rel,
                "delay-domain must not be worse than linear under pilot noise");
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

// ---------------------------------------------------------------------------
// 5. Band edges must not blow up, and must self-report their uncertainty
// ---------------------------------------------------------------------------

void test_band_edges_extrapolate_without_blowup() {
    TEST("band-edge carriers extrapolate sanely and report a larger error_var") {
        const uint32_t fft = 1024;
        const int num_carriers = 59;
        const int spacing = 8;
        const std::vector<Tap> taps = {{0, Complex(0.707f, 0.0f)},
                                       {24, Complex(0.0f, 0.707f)}};
        const auto ks = physicalCarriers(num_carriers);
        // offset 0 => last pilot at logical 56, so logical 57/58 are OUTSIDE the
        // pilot span and must be extrapolated.
        const auto is_pilot = pilotMask(num_carriers, spacing, 0);
        std::vector<Complex> truth(ks.size());
        for (size_t i = 0; i < ks.size(); ++i) {
            truth[i] = channelAt(taps, ks[i], static_cast<int>(fft));
        }
        std::vector<ofdm_wiener::Observation1D> obs;
        for (size_t i = 0; i < ks.size(); ++i) {
            if (is_pilot[i]) {
                obs.push_back(ofdm_wiener::Observation1D{
                    static_cast<float>(ks[i]), truth[i], 1.0e-3f});
            }
        }
        auto win = ofdm_cir::chooseDelayWindow(1.0e-3f, 48000, fft, spacing, 96);

        // Interior reference: a carrier in the middle of a pilot gap.
        const auto interior = ofdm_cir::reconstruct(
            obs, static_cast<float>(ks[28]), win.taps, fft, obs.size());
        const auto edge = ofdm_cir::reconstruct(
            obs, static_cast<float>(ks.back()), win.taps, fft, obs.size());
        require(interior.valid && edge.valid, "reconstruction must be valid");

        const float edge_gain_db = 20.0f * std::log10(
            std::abs(edge.value) / std::abs(truth.back()));
        std::printf("[edge gain=%+.2f dB, error_var interior=%.4f edge=%.4f] ",
                    edge_gain_db, interior.error_var, edge.error_var);

        // No DFT wrap-around, no edge-hold: this is a model evaluation, so the
        // extrapolated carrier must stay within the 16-QAM ring-flip margin.
        require(std::abs(edge_gain_db) < 3.52f,
                "band-edge extrapolation crossed the 16-QAM ring flip point");
        require(std::isfinite(edge.value.real()) && std::isfinite(edge.value.imag()),
                "band-edge estimate must be finite");
        // The cost of extrapolating is paid honestly, in the reported variance.
        require(edge.error_var >= interior.error_var,
                "extrapolated carrier must report at least the interior error var");
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

// ---------------------------------------------------------------------------
// 6. The DC gap really is a geometry error for logical-index interpolation
// ---------------------------------------------------------------------------

void test_physical_index_differs_from_logical_across_dc() {
    TEST("logical index is not proportional to frequency across the DC gap") {
        ModemConfig cfg;
        cfg.num_carriers = 59;
        cfg.fft_size = 1024;
        const auto ks = physicalCarriers(static_cast<int>(cfg.num_carriers));
        require(ks.size() == cfg.num_carriers, "carrier count mismatch");
        // Verify against the production mapping.
        for (size_t logical = 0; logical < ks.size(); ++logical) {
            const int fft_idx = ofdm_pilots::fftIndexForLogical(cfg, logical);
            const int k = (fft_idx <= static_cast<int>(cfg.fft_size / 2))
                ? fft_idx
                : fft_idx - static_cast<int>(cfg.fft_size);
            require(k == ks[logical],
                    "physicalCarriers disagrees with fftIndexForLogical at " +
                        std::to_string(logical));
        }
        // The one place logical delta 1 means frequency delta 2:
        size_t dc_edge = 0;
        for (size_t i = 0; i + 1 < ks.size(); ++i) {
            if (ks[i + 1] - ks[i] != 1) { dc_edge = i; break; }
        }
        require(ks[dc_edge] == -1 && ks[dc_edge + 1] == 1,
                "expected the gap to straddle DC");
        std::printf("[DC gap at logical %zu: k=-1 -> k=+1] ", dc_edge);
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

}  // namespace

int main() {
    std::cout << "=== OFDM delay-domain (CIR) pilot interpolation ===\n";
    test_kernel_is_the_dirichlet_kernel_of_the_window();
    test_window_selection_respects_aliasing_and_cp();
    test_two_tap_channel_is_reconstructed_exactly();
    test_production_geometry_beats_linear_on_itu_good();
    test_delay_truncation_rejects_pilot_noise();
    test_band_edges_extrapolate_without_blowup();
    test_physical_index_differs_from_logical_across_dc();

    std::cout << "\nPassed: " << tests_passed
              << "  Failed: " << tests_failed << "\n";
    return tests_failed == 0 ? 0 : 1;
}
