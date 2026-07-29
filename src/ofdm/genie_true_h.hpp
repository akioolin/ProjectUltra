#pragma once

#include <complex>
#include <vector>

// 2026-07-29 diag: a VALID perfect-CSI oracle.
//
// WHY THIS EXISTS. Two earlier oracles were used to argue about whether channel
// estimation is the throughput limiter, and BOTH are invalid:
//
//   1. ULTRA_GENIE_DATA_AIDED (H = Y/X_true) is circular. Substituting it into
//      the MMSE combiner conj(H)Y/(|H|^2+nv) reconstructs X exactly, noise and
//      all -- it reports a near-zero FER that says nothing about estimation.
//
//   2. ULTRA_GENIE_LTS_FREEZE holds the frame's LTS channel estimate across the
//      data symbols. Its own comment states the validity condition -- "on a
//      NOISELESS frozen channel the stored LTS H is the exact true H" -- but it
//      is run on ITU Good at finite SNR, where the LTS estimate carries full
//      estimation noise. It therefore replaces per-symbol pilot tracking (fresh
//      pilots every symbol, noise averaged down by the EMA) with a SINGLE noisy
//      snapshot held for the whole frame. Measured 2026-07-29 on ITU Good @20,
//      seeds 7/11/23/42, n=100: it is dramatically WORSE than baseline
//      (QPSK R3/4 93.5 -> 76.8, 16QAM R2/3 51.0 -> 27.2). That harm is fully
//      explained by un-averaged estimation noise. Channel correlation across a
//      frame at ITU Good's 0.1 Hz Doppler is J0(2*pi*0.1*0.35) ~= 0.988, so
//      staleness cannot account for it.
//
// Conclusion: "perfect CSI does not help" was never actually measured. This
// oracle measures it.
//
// HOW IT IS VALID. The channel is seeded and deterministic, so a second
// SimulatedChannel with the SAME seed and channel type, driven with the SAME
// transmit buffer and pumped by the SAME number of samples, reproduces the same
// fade trajectory. Configure that shadow channel at a very high SNR and its
// output is H(x)X with negligible noise, so the LTS estimate taken from it IS
// the true H -- and, crucially, it arrives already expressed in the equalizer's
// own domain, scaling and normalization convention. No tap export, no domain
// guesswork, no analytic reconstruction to get wrong.
//
// Capture on the clean pass, Inject on the noisy pass. The data path's noise is
// untouched; only the channel estimate is replaced. That is exactly the
// perfect-CSI bound.
//
// This is a process-global rather than an env knob deliberately: both passes run
// inside one process and the mode must flip between them, which a latched
// getenv() cannot express.
namespace ultra::ofdm::genie_true_h {

enum class Mode {
    Off,      // production behaviour
    Capture,  // clean pass: store the (noiseless) LTS channel estimate
    Inject,   // noisy pass: overwrite the data-symbol estimate with the stored H
};

inline Mode& mode() {
    static Mode m = Mode::Off;
    return m;
}

inline std::vector<std::complex<float>>& buffer() {
    static std::vector<std::complex<float>> b;
    return b;
}

// Instrumentation. An oracle that silently fails to engage reports "no effect",
// which is indistinguishable from "perfect CSI does not help" -- the exact trap
// that produced two invalid oracles before this one. Always check these counters
// before believing a genie result.
struct Stats {
    long captures = 0;    // clean passes that produced a true H
    long injections = 0;  // data symbols that actually received it
    long misses = 0;      // inject requested but no usable H was stored
    long rejected_profile = 0;  // capture skipped: geometry != transmitted profile
    double nmse_sum = 0.0;   // sum of ||injected - replaced||^2 / ||replaced||^2
    long nmse_count = 0;
};

inline Stats& stats() {
    static Stats s;
    return s;
}

// CAPTURE PROVENANCE (2026-07-29, external review). A raw "first capture per frame"
// rule is WRONG and produced a +6.44 dB identity-control NMSE that looked like a
// catastrophic estimator. The receiver runs a CONTROL-FIRST hypothesis before the
// data profile (streaming_ofdm_decode.cpp:968: "control always rides coherent QPSK
// R1/4"), so the first LTS estimate of a frame belongs to a speculative decode at a
// DIFFERENT pilot spacing (5) than the transmitted data profile (8). What gets
// captured is then
//     H_captured = H_channel * X_spacing8 / X_spacing5
// -- a deterministic quotient of two nearly-unit-modulus training sequences. |H|
// looks clean while adjacent-carrier phases jump 50-140 deg, which is exactly the
// "impossible" signature observed. Confirmed empirically: transmitting the control
// profile itself (qpsk r1_4, so transmitted == speculative geometry) drops the
// identity-control NMSE from 4.401 to 0.0166 (-17.79 dB) and the fitted delay from
// 122 samples to -0.007.
//
// So: never "first capture wins", and never "last capture wins" either. Accept only
// the pass whose geometry matches the transmitted profile.
struct Expect {
    bool active = false;
    uint32_t pilot_spacing = 0;
    int modulation = -1;
    uint32_t num_carriers = 0;
    uint32_t fft_size = 0;
};

inline Expect& expect() {
    static Expect e;
    return e;
}

// True when the stored H is usable for a frame of `carriers` FFT bins.
inline bool haveTrueH(std::size_t carriers) {
    return !buffer().empty() && buffer().size() == carriers;
}

}  // namespace ultra::ofdm::genie_true_h
