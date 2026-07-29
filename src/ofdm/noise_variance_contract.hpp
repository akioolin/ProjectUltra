#pragma once

#include <cstddef>

// Phase 0 (EFFECTIVE_SINR handoff §9): "define complex-noise versus per-real
// variance in one typed contract."
//
// WHY THIS EXISTS. Four different noise quantities circulate in the OFDM RX chain
// and they differ by factors of N, 2, and 1/M. They are all called some variant of
// "noise variance" and all stored as bare `float`. That is how a 3.01 dB scale
// error lived in the decode path while the meter path silently compensated for it
// three lines away, and how the same member ends up holding two DIFFERENT
// conventions depending on which branch ran.
//
// This header NAMES the quantities and their conversions. It is deliberately not a
// mass refactor of the hot path -- the types are opt-in wrappers so call sites can
// adopt them incrementally without touching per-symbol code. The value delivered
// today is the definition plus the register below, both pinned by a test
// (`NoiseVarianceContract`).
//
// ============================================================================
// THE FOUR QUANTITIES
// ============================================================================
//
//   sigma_t^2   RealSampleVariance
//               Per-real-component variance of the TIME-DOMAIN audio noise. This
//               is what the simulator injects: `out += noise_stddev_ * N(0,1)`
//               added to a real sample (models.cpp:397, :728, :756;
//               channel.cpp:224-233). Set from snr_db by
//               modemReferenceNoiseStddev() (models.cpp:247-250).
//
//   sigma_bin^2 BinVariancePerSymbol
//               COMPLEX noise variance in ONE FFT bin of ONE symbol.
//               sigma_bin^2 = fft_size * sigma_t^2, because toBaseband multiplies
//               each real sample by a unit-modulus phasor (so per-sample power is
//               unchanged) and extractSymbol takes an UNNORMALIZED forward FFT
//               (channel_equalizer_baseband.cpp:154-155, :185-186).
//               THIS IS WHAT EVERY DECODE-PATH CONSUMER WANTS.
//
//   sigma_avg^2 BinVarianceAveraged
//               COMPLEX noise variance of a channel estimate AVERAGED over M
//               training symbols: sigma_avg^2 = sigma_bin^2 / M. For the 2-LTS
//               estimator M = 2, so it is HALF the per-symbol value. This is the
//               quantity `lts_noise_var` actually holds
//               (channel_equalizer_lts.cpp:752, `noise_sum / (4.0f * count)`,
//               since E|h1-h0|^2 = 2*sigma_bin^2).
//
//   sigma_r^2   PerRealComponentVariance
//               Variance of ONE real component (I or Q) of a complex quantity:
//               sigma_r^2 = sigma_bin^2 / 2. This is the convention max-log LLRs
//               are written in: LLR = (d1^2 - d0^2) / (2*sigma_r^2).
//
// ============================================================================
// REGISTER -- who produces and consumes what (verified 2026-07-29)
// ============================================================================
//
// PRODUCERS
//   channel_equalizer_lts.cpp:752   lts_noise_var        = BinVarianceAveraged(M=2)
//   channel_equalizer_lts.cpp:784   noise_variance       = the above, UNCONVERTED
//                                   (or *2 when ULTRA_CHEST_NOISE_SCALE is on)
//   channel_equalizer_lts.cpp:825   noise_variance       = signal_power/DEFAULT_SNR_LINEAR
//                                   -- the 1-LTS fallback, which is per-symbol
//                                   DATA-noise semantics at a FIXED assumed SNR
//   channel_equalizer_lts.cpp:283-301 guard-bin noise    = BinVariancePerSymbol
//
// CONSUMERS -- every one of these wants BinVariancePerSymbol
//   channel_equalizer_equalize.cpp:612,639,674,687  MMSE denominator |H|^2 + nv
//   channel_equalizer_equalize.cpp:619,690          post-equalization LLR sigma^2
//   channel_equalizer_equalize.cpp:541,697,709      softGrayZone gamma + erasure gate
//   channel_equalizer_equalize.cpp:875-876          decision-directed Kalman var
//   channel_equalizer_pilot.cpp:785-786             Wiener noise_norm = sigma^2/P_h
//
// COMPENSATOR
//   channel_equalizer_lts.cpp:802   meter path multiplies by 2.0f to recover
//                                   BinVariancePerSymbol before reporting SNR
//
// ============================================================================
// KNOWN INCONSISTENCIES (documented, NOT yet fixed -- both measured harmless)
// ============================================================================
//
// I1. The decode path consumes BinVarianceAveraged where it wants
//     BinVariancePerSymbol -- a factor of 2 (3.01 dB). The meter path compensates,
//     the decode path does not, so the two disagree by 2x. MEASURED A WASH on ITU
//     Good @20 over 4 seeds x n=100 (93.5->93.5, 82.2->81.0, 51.0->49.8,
//     27.8->26.8), so the MMSE regulariser is not the binding constraint. The fix
//     is available behind ULTRA_CHEST_NOISE_SCALE, default-OFF.
//
// I2. The two producers of `noise_variance` disagree with EACH OTHER. The 2-LTS
//     path (:784) yields BinVarianceAveraged(M=2); the 1-LTS fallback (:825)
//     yields per-symbol data-noise at a fixed 15 dB assumption. Whichever branch
//     ran last sets the convention for the entire frame. Flagged in-code at :782.
//
// Neither is fixed here because Phase 0 is explicitly "no production behavior
// change". They are recorded so the next person does not have to rediscover them.
namespace ultra::ofdm::noise {

// Per-real-component variance of the time-domain audio noise (simulator input).
struct RealSampleVariance {
    float v = 0.0f;
};

// Complex noise variance in one FFT bin of one symbol. The decode-path convention.
struct BinVariancePerSymbol {
    float v = 0.0f;
};

// Complex noise variance of an estimate averaged over `symbols` training symbols.
struct BinVarianceAveraged {
    float v = 0.0f;
    int symbols = 1;
};

// Variance of a single real component (I or Q). The max-log LLR convention.
struct PerRealComponentVariance {
    float v = 0.0f;
};

// sigma_bin^2 = fft_size * sigma_t^2 (unnormalized forward FFT).
inline BinVariancePerSymbol toBinVariance(RealSampleVariance t, size_t fft_size) {
    return BinVariancePerSymbol{t.v * static_cast<float>(fft_size)};
}

// sigma_avg^2 = sigma_bin^2 / M.
inline BinVarianceAveraged averagedOver(BinVariancePerSymbol bin, int symbols) {
    const int m = symbols > 0 ? symbols : 1;
    return BinVarianceAveraged{bin.v / static_cast<float>(m), m};
}

// The conversion the decode path is currently missing (see I1).
inline BinVariancePerSymbol toPerSymbol(BinVarianceAveraged avg) {
    return BinVariancePerSymbol{avg.v * static_cast<float>(avg.symbols > 0 ? avg.symbols : 1)};
}

// sigma_r^2 = sigma_bin^2 / 2.
inline PerRealComponentVariance toPerRealComponent(BinVariancePerSymbol bin) {
    return PerRealComponentVariance{bin.v * 0.5f};
}

inline BinVariancePerSymbol fromPerRealComponent(PerRealComponentVariance r) {
    return BinVariancePerSymbol{r.v * 2.0f};
}

}  // namespace ultra::ofdm::noise
