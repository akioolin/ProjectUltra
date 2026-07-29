// Phase 0 (EFFECTIVE_SINR handoff §9): pin the physical/logical CARRIER ORDERING
// contract, including the DC gap.
//
// WHY. To compare a channel ESTIMATE against simulator TRUTH you must know which
// frequency each element of the estimate corresponds to. Until 2026-07-29 nothing
// in the tree performed that conversion -- the mapping lived implicitly in the
// mixer and IFFT conventions and was re-derived by hand each time. A comparison
// made in the wrong domain does not look like a bug, it looks like a RESULT, which
// is exactly how two invalid channel-estimation oracles survived for months.
//
// This pins:
//   * the k -> fft_bin -> Hz chain, including the DC skip;
//   * that LOGICAL index is monotonic in frequency while FFT BIN NUMBER is not;
//   * the odd-carrier-count asymmetry (the band is not centred on center_freq);
//   * that data and pilot index sets partition the carriers exactly;
//   * that scattered pilots rotate the data/pilot SPLIT per symbol while leaving
//     the carrier set itself invariant.

#include "ofdm/pilot_pattern.hpp"
#include "ultra/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

namespace {

using ultra::ModemConfig;
namespace pilots = ultra::ofdm_pilots;

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

struct Pattern {
    std::vector<int> all;
    std::vector<int> data;
    std::vector<int> pilot;
    std::vector<size_t> data_logical;
    std::vector<size_t> pilot_logical;
    std::vector<bool> is_pilot_logical;
    std::vector<std::complex<float>> pilot_sequence;
};

Pattern build(const ModemConfig& cfg, size_t symbol_index) {
    Pattern p;
    pilots::buildCarrierPattern(cfg, symbol_index, p.all, p.data, p.pilot,
                                p.data_logical, p.pilot_logical,
                                p.is_pilot_logical, p.pilot_sequence);
    return p;
}

}  // namespace

int main() {
    std::printf("=== OFDM carrier ordering contract (Phase 0) ===\n");

    // Production wideband. Verified 2026-07-29 that BOTH GUI construction sites
    // (modem_engine.cpp:43 and app.cpp:456) use presets::balanced(), which inherits
    // the 1024/59 defaults -- NOT the 512/30 in the OFDMChirpWaveform default ctor.
    ModemConfig cfg = ultra::presets::balanced();
    check(cfg.fft_size == 1024 && cfg.num_carriers == 59,
          "production wideband is FFT 1024 / 59 carriers (presets::balanced)");

    const Pattern p0 = build(cfg, 0);

    check(p0.all.size() == cfg.num_carriers,
          "all_carrier_fft_indices holds exactly num_carriers entries");

    // --- The DC gap.
    check(std::find(p0.all.begin(), p0.all.end(), 0) == p0.all.end(),
          "FFT bin 0 (DC, = center_freq) is NEVER occupied");

    // --- k range and the odd-count asymmetry.
    int k_min = 1 << 30;
    int k_max = -(1 << 30);
    int below = 0;
    int above = 0;
    for (int idx : p0.all) {
        const int k = pilots::signedCarrierIndex(cfg, idx);
        k_min = std::min(k_min, k);
        k_max = std::max(k_max, k);
        (k < 0 ? below : above)++;
    }
    std::printf("   k range = [%d, %d]   below DC = %d   above DC = %d\n",
                k_min, k_max, below, above);
    check(k_min == -29 && k_max == 30, "k spans -29..+30 for 59 carriers");
    check(below == 29 && above == 30,
          "odd carrier count => one MORE carrier above DC than below");

    // --- Logical index is monotonic in FREQUENCY; FFT bin number is not.
    bool logical_monotonic_in_freq = true;
    bool bin_number_monotonic = true;
    for (size_t i = 1; i < p0.all.size(); ++i) {
        if (pilots::carrierFrequencyHz(cfg, p0.all[i]) <=
            pilots::carrierFrequencyHz(cfg, p0.all[i - 1])) {
            logical_monotonic_in_freq = false;
        }
        if (p0.all[i] <= p0.all[i - 1]) {
            bin_number_monotonic = false;
        }
    }
    check(logical_monotonic_in_freq,
          "LOGICAL index is strictly increasing in frequency");
    check(!bin_number_monotonic,
          "FFT BIN NUMBER is NOT monotonic (negative k wraps to the upper half)");

    // --- The occupied bin sets.
    std::set<int> occupied(p0.all.begin(), p0.all.end());
    bool lower_ok = true;
    for (int b = 1; b <= 30; ++b) {
        if (!occupied.count(b)) lower_ok = false;
    }
    bool upper_ok = true;
    for (int b = 995; b <= 1023; ++b) {
        if (!occupied.count(b)) upper_ok = false;
    }
    check(lower_ok && upper_ok && occupied.size() == 59,
          "occupied bins are exactly 1..30 and 995..1023");

    // --- Frequency mapping endpoints. Spacing 48000/1024 = 46.875 Hz.
    const float spacing = static_cast<float>(cfg.sample_rate) /
                          static_cast<float>(cfg.fft_size);
    const float f_lowest = pilots::carrierFrequencyHz(cfg, p0.all.front());
    const float f_highest = pilots::carrierFrequencyHz(cfg, p0.all.back());
    std::printf("   spacing = %.4f Hz   lowest = %.4f Hz   highest = %.4f Hz\n",
                static_cast<double>(spacing), static_cast<double>(f_lowest),
                static_cast<double>(f_highest));
    check(std::abs(spacing - 46.875f) < 1e-3f, "carrier spacing is 46.875 Hz");
    check(std::abs(f_lowest - 140.625f) < 1e-2f,
          "lowest carrier is 140.625 Hz (center - 29 * spacing)");
    check(std::abs(f_highest - 2906.25f) < 1e-2f,
          "highest carrier is 2906.25 Hz (center + 30 * spacing)");

    // The band midpoint sits half a bin ABOVE center_freq, not on it.
    const float midpoint = 0.5f * (f_lowest + f_highest);
    check(std::abs(midpoint - (static_cast<float>(cfg.center_freq) + 0.5f * spacing)) <
              1e-2f,
          "band midpoint is center_freq + half a bin (asymmetry is real)");

    // --- Round-trip: logical <-> fft_idx <-> Hz.
    bool roundtrip_ok = true;
    for (size_t logical = 0; logical < p0.all.size(); ++logical) {
        const int idx = pilots::fftIndexForLogical(cfg, logical);
        if (idx != p0.all[logical]) roundtrip_ok = false;
        if (pilots::logicalForFftIndex(cfg, idx) != logical) roundtrip_ok = false;
        const float f_by_logical = pilots::carrierFrequencyHzForLogical(cfg, logical);
        if (std::abs(f_by_logical - pilots::carrierFrequencyHz(cfg, idx)) > 1e-3f) {
            roundtrip_ok = false;
        }
    }
    check(roundtrip_ok, "logical <-> fft_idx <-> Hz round-trips for every carrier");

    // --- data and pilot partition the carrier set exactly (no overlap, no gap).
    {
        std::set<int> d(p0.data.begin(), p0.data.end());
        std::set<int> q(p0.pilot.begin(), p0.pilot.end());
        bool disjoint = true;
        for (int b : d) {
            if (q.count(b)) disjoint = false;
        }
        check(disjoint && d.size() + q.size() == occupied.size(),
              "data and pilot index sets PARTITION the carriers exactly");
        std::printf("   pilots = %zu   data = %zu   (spacing %u)\n", q.size(), d.size(),
                    cfg.pilot_spacing);
    }

    // --- Scattered pilots rotate the SPLIT per symbol, not the carrier SET.
    //
    // NB: presets::balanced() ships with use_pilots=false and a DIFFERENTIAL
    // modulation, so scatteredPilotsActive() is false on the raw preset -- pilots
    // are switched on later by link adaptation (recommendedPilotSpacing). Building
    // the pilot-bearing config explicitly here, because an `if (active)` guard
    // around these checks would SILENTLY SKIP them and report a pass.
    {
        ModemConfig pc = cfg;
        pc.use_pilots = true;
        pc.scattered_pilots = true;
        pc.pilot_spacing = 8;               // the shipped wideband spacing
        pc.modulation = ultra::Modulation::QAM16;  // coherent => scattered active

        check(pilots::scatteredPilotsActive(pc),
              "pilot-bearing config really does activate scattered pilots");

        const Pattern q0 = build(pc, 0);
        const Pattern q1 = build(pc, 1);

        check(q1.all == q0.all,
              "carrier SET is identical across symbols (only the split rotates)");
        check(q1.pilot != q0.pilot,
              "scattered pilots occupy DIFFERENT carriers on the next symbol");
        check(q0.pilot.size() == pilots::pilotCount(pc) && q0.pilot.size() == 8,
              "pilot count is ceil(59/8) = 8 and matches pilotCount(config)");

        std::set<int> qd(q0.data.begin(), q0.data.end());
        std::set<int> qq(q0.pilot.begin(), q0.pilot.end());
        bool disjoint = true;
        for (int b : qd) {
            if (qq.count(b)) disjoint = false;
        }
        check(disjoint && qd.size() == 51 && qq.size() == 8,
              "with pilots on, the split is 8 pilot + 51 data, disjoint");

        // The rotation must be a permutation over a full period, not a drift:
        // every carrier serves as a pilot exactly once per `pilot_spacing` symbols.
        std::set<int> ever_pilot;
        for (size_t s = 0; s < pc.pilot_spacing; ++s) {
            const Pattern ps = build(pc, s);
            for (int b : ps.pilot) {
                ever_pilot.insert(b);
            }
        }
        std::printf("   over one period (%u symbols) %zu distinct carriers act as pilots\n",
                    pc.pilot_spacing, ever_pilot.size());
        check(ever_pilot.size() == occupied.size(),
              "over one pilot period EVERY carrier is sounded at least once");
    }

    // --- Narrowband config obeys the same contract with its own geometry.
    {
        ModemConfig nb = ultra::presets::narrowbandOFDM();
        const Pattern pn = build(nb, 0);
        const float nb_spacing = static_cast<float>(nb.sample_rate) /
                                 static_cast<float>(nb.fft_size);
        const float nb_low = pilots::carrierFrequencyHz(nb, pn.all.front());
        const float nb_high = pilots::carrierFrequencyHz(nb, pn.all.back());
        std::printf("   narrowband: fft=%u Nc=%u spacing=%.4f Hz  band %.3f..%.3f Hz\n",
                    nb.fft_size, nb.num_carriers, static_cast<double>(nb_spacing),
                    static_cast<double>(nb_low), static_cast<double>(nb_high));
        check(pn.all.size() == nb.num_carriers &&
                  std::find(pn.all.begin(), pn.all.end(), 0) == pn.all.end(),
              "narrowband obeys the same DC-skip and count contract");
        check(nb_high - nb_low > 0.0f && nb_low > 1000.0f && nb_high < 1800.0f,
              "narrowband band sits inside the 1250-1750 Hz chirp region");
    }

    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
