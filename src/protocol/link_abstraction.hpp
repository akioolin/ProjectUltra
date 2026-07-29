#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "ultra/types.hpp"

// Phase 5 (EFFECTIVE_SINR handoff §9): RESOURCE-ELEMENT LINK ABSTRACTION.
//
// Turns the measured per-carrier gamma grid into a PREDICTED PER for every
// candidate MCS, so rate selection can stop keying off scalar channel-class
// anchors. The plan's §6.2 "smaller first step: calibrated EESM" is what this
// implements; the codeword-aware MIESM/MMIB end state (§6.1) is deliberately not
// attempted yet.
//
// WHY THIS REPLACES THE ANCHOR TABLE. waveform_selection.hpp carries ten scalar
// (channel-class, SNR) anchors, several of which are documented in-tree as wrong
// or extrapolated: 16QAM R2/3's Good anchor measured 51.4% FER, 8PSK R3/4 was
// disabled after a fading validation it failed, 16QAM R1/2's "strictly dominated"
// justification was measured false. The anchors also depend on a channel-class
// discriminator that is itself a known-open bug. The AWGN column note for 16QAM
// R2/3 states the unlock condition outright: "the effective-SINR/PER selector,
// which makes the column moot because it predicts PER from the measured
// per-carrier grid instead of a channel-class label". That is this module.
//
// ---------------------------------------------------------------------------
// THE GAMMA CONVENTION, AND WHY ITS ABSOLUTE SCALE DOES NOT MATTER
// ---------------------------------------------------------------------------
// The input grid is OFDMDemodulator::getCarrierGammaSnapshot(): per data carrier,
//     gamma_k = |H_k|^2 / noise_variance
// Both terms are receiver-internal, and the value is known to sit ~11 dB above
// the dial (the +8.70 dB legacy anchor offset plus the sigma^2_bin/2 factor
// documented in src/ofdm/noise_variance_contract.hpp).
//
// THAT OFFSET IS HARMLESS HERE, and it is important to understand why: the PER
// curves below are CALIBRATED EMPIRICALLY AGAINST THIS SAME GAMMA DEFINITION. A
// constant multiplicative bias shifts both the calibration and the prediction by
// the same amount and cancels exactly. What would NOT cancel is a bias that
// varies with modulation, rate or channel — so calibration is keyed by profile
// and the key is recorded with the table.
//
// This is a deliberate design choice: an empirically calibrated predictor is
// robust to a mis-scaled input, whereas an analytic one inherits every scale bug
// in the chain. It also means the predictor MUST be recalibrated if the gamma
// definition ever changes; kCalibrationVersion exists to force that.
namespace ultra::protocol::link_abstraction {

// Bump when the gamma definition, the fitting procedure, or the sweep geometry
// changes. A table with a stale version must not be used to command a rate.
inline constexpr int kCalibrationVersion = 1;

// EESM: gamma_eff = -beta * ln( mean_k exp(-gamma_k / beta) )
//
// beta compresses a frequency-selective grid to the equivalent flat-AWGN SNR that
// yields the same codeword error rate. beta -> infinity recovers the arithmetic
// mean (too optimistic on selective channels, because it lets strong carriers mask
// deep fades); small beta approaches the minimum (too pessimistic). It is fitted
// per profile, never derived from the code rate — the plan is explicit that
// deriving beta from rate plus one anchor is what the existing prototype does and
// is not a calibrated predictor.
//
// Computed in the log domain so a deep grid cannot underflow to zero and report a
// spuriously perfect gamma_eff.
inline double effectiveSnrEesm(const std::vector<float>& gamma_linear, double beta) {
    if (gamma_linear.empty() || beta <= 0.0) {
        return 0.0;
    }
    // log-sum-exp over -gamma_k/beta, stabilised by the max term.
    double max_term = -std::numeric_limits<double>::infinity();
    for (float g : gamma_linear) {
        max_term = std::max(max_term, -static_cast<double>(g) / beta);
    }
    if (!std::isfinite(max_term)) {
        return 0.0;
    }
    double acc = 0.0;
    for (float g : gamma_linear) {
        acc += std::exp((-static_cast<double>(g) / beta) - max_term);
    }
    const double log_mean =
        max_term + std::log(acc / static_cast<double>(gamma_linear.size()));
    return -beta * log_mean;
}

// Per-profile calibration: the EESM beta plus a two-parameter PER curve.
//
// The PER curve is a logistic in dB, which the plan requires ("fit the PER slope,
// not only one threshold"):
//     PER(gamma_eff_db) = 1 / (1 + exp( slope * (gamma_eff_db - midpoint_db) ))
// `midpoint_db` is the 50%-PER point and `slope` its steepness in 1/dB. A single
// threshold cannot express confidence, and confidence is what the selector needs
// to avoid commanding a rung it will merely usually hold.
struct RungCalibration {
    Modulation mod = Modulation::QPSK;
    CodeRate rate = CodeRate::R1_4;
    double beta = 1.0;
    double midpoint_db = 0.0;
    double slope = 1.0;
    bool valid = false;      // false => never command this rung from prediction
    int n_frames = 0;        // frames the fit rests on; provenance, not decoration
    double fit_rms_error = 0.0;
};

inline double predictPer(const RungCalibration& c, double gamma_eff_db) {
    if (!c.valid) {
        return 1.0;
    }
    const double x = c.slope * (gamma_eff_db - c.midpoint_db);
    // Logistic, guarded against overflow at the tails.
    if (x > 40.0) return 0.0;
    if (x < -40.0) return 1.0;
    return 1.0 / (1.0 + std::exp(x));
}

inline double toDb(double linear) {
    return 10.0 * std::log10(std::max(linear, 1e-12));
}

// Spectral efficiency in information bits per data carrier. Used to turn a
// predicted PER into an expected-goodput score.
inline double bitsPerCarrier(Modulation mod, CodeRate rate) {
    double bits = 2.0;
    switch (mod) {
        case Modulation::BPSK:  bits = 1.0; break;
        case Modulation::QPSK:
        case Modulation::DQPSK: bits = 2.0; break;
        case Modulation::D8PSK:
        case Modulation::QAM8:  bits = 3.0; break;
        case Modulation::QAM16: bits = 4.0; break;
        case Modulation::QAM32: bits = 5.0; break;
        case Modulation::QAM64: bits = 6.0; break;
        default:                bits = 2.0; break;
    }
    double r = 0.5;
    switch (rate) {
        case CodeRate::R1_4: r = 0.25; break;
        case CodeRate::R1_2: r = 0.50; break;
        case CodeRate::R2_3: r = 2.0 / 3.0; break;
        case CodeRate::R3_4: r = 0.75; break;
        default:             r = 0.50; break;
    }
    return bits * r;
}

// A scored candidate. `predicted_per` and `confidence` are what Phase 6 logs and
// Phase 7 eventually acts on.
struct CandidateScore {
    Modulation mod = Modulation::QPSK;
    CodeRate rate = CodeRate::R1_4;
    double gamma_eff_db = 0.0;
    double predicted_per = 1.0;
    double efficiency = 0.0;        // information bits per carrier
    double expected_goodput = 0.0;  // efficiency * (1 - PER); airtime-relative
    bool calibrated = false;
};

// Score one candidate against a measured grid.
//
// NOTE ON THE OBJECTIVE. `expected_goodput = efficiency * (1 - PER)` is the
// FIRST-ORDER score only. It deliberately does NOT yet include the costs the plan
// requires in §8 — turnaround, ACK, retransmission, rung-switch and recovery
// airtime — because those are half-duplex protocol costs, not PHY properties, and
// folding them in before they are measured would bury an unvalidated model inside
// a number that looks authoritative. Phase 6 logs this score alongside the actual
// outcome so the cost terms can be fitted against reality rather than assumed.
inline CandidateScore scoreCandidate(const RungCalibration& c,
                                     const std::vector<float>& gamma_linear) {
    CandidateScore s;
    s.mod = c.mod;
    s.rate = c.rate;
    s.calibrated = c.valid;
    s.efficiency = bitsPerCarrier(c.mod, c.rate);
    if (!c.valid || gamma_linear.empty()) {
        s.predicted_per = 1.0;
        s.expected_goodput = 0.0;
        return s;
    }
    s.gamma_eff_db = toDb(effectiveSnrEesm(gamma_linear, c.beta));
    s.predicted_per = predictPer(c, s.gamma_eff_db);
    s.expected_goodput = s.efficiency * (1.0 - s.predicted_per);
    return s;
}

// The calibration table. Populated from the fitted sweep; `version` guards use.
struct CalibrationTable {
    int version = 0;
    std::string provenance;  // how/when/where fitted
    std::vector<RungCalibration> rungs;

    const RungCalibration* find(Modulation mod, CodeRate rate) const {
        for (const auto& r : rungs) {
            if (r.mod == mod && r.rate == rate) {
                return &r;
            }
        }
        return nullptr;
    }

    bool usable() const { return version == kCalibrationVersion && !rungs.empty(); }
};

// Score every calibrated candidate, best expected goodput first.
//
// Candidates whose calibration is missing or invalid are returned with
// predicted_per = 1.0 and calibrated = false rather than omitted, so a caller can
// see that a rung was considered and rejected for lack of evidence — an absent
// entry and a rejected one must not look identical.
inline std::vector<CandidateScore> scoreAll(const CalibrationTable& table,
                                            const std::vector<float>& gamma_linear) {
    std::vector<CandidateScore> out;
    out.reserve(table.rungs.size());
    for (const auto& rung : table.rungs) {
        out.push_back(scoreCandidate(rung, gamma_linear));
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const CandidateScore& a, const CandidateScore& b) {
                         return a.expected_goodput > b.expected_goodput;
                     });
    return out;
}

// Reliability-constrained pick: the highest expected goodput among candidates whose
// predicted PER clears a ceiling. Returns nullptr when nothing qualifies, which the
// caller must treat as "keep the current rung", never as "pick the fastest anyway".
inline const CandidateScore* selectUnderReliability(
        const std::vector<CandidateScore>& scored, double max_per) {
    const CandidateScore* best = nullptr;
    for (const auto& s : scored) {
        if (!s.calibrated || s.predicted_per > max_per) {
            continue;
        }
        if (best == nullptr || s.expected_goodput > best->expected_goodput) {
            best = &s;
        }
    }
    return best;
}

}  // namespace ultra::protocol::link_abstraction
