// ACE PAPR reduction — proves the two safety invariants and the benefit.
#include "ofdm/papr_ace.hpp"
#include "ultra/dsp.hpp"

#include <cstdio>
#include <random>
#include <vector>

using namespace ultra;
using ultra::ofdm::papr_ace::applyAce;
using ultra::ofdm::papr_ace::AceResult;
using ultra::ofdm::papr_ace::qamAxisSpec;
using ultra::ofdm::papr_ace::isPskMod;

namespace {
int g_failures = 0;
void check(bool ok, const char* msg) {
    if (!ok) { std::printf("FAIL: %s\n", msg); ++g_failures; }
}

// Build a random OFDM freq-domain symbol for `mod` on `data_indices`.
std::vector<Complex> makeSymbol(Modulation mod, const std::vector<int>& data_indices,
                                size_t fft, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<Complex> X(fft, Complex(0, 0));
    const auto qam = qamAxisSpec(mod);
    for (int bin : data_indices) {
        if (isPskMod(mod)) {
            int npsk = (mod == Modulation::QAM8 || mod == Modulation::D8PSK) ? 8
                       : (mod == Modulation::DBPSK || mod == Modulation::BPSK) ? 2 : 4;
            float ph = (2.0f * float(M_PI) / npsk) * (rng() % npsk) +
                       ((npsk == 4) ? float(M_PI) / 4.0f : 0.0f);
            X[bin] = Complex(std::cos(ph), std::sin(ph));
        } else {
            // QAM: pick random levels
            const float sI = qam.scale;
            int iL, qL;
            if (mod == Modulation::QAM16) { int L[4]={-3,-1,1,3}; iL=L[rng()%4]; qL=L[rng()%4]; }
            else { int LI[4]={-3,-1,1,3}; int LQ[8]={-7,-5,-3,-1,1,3,5,7}; iL=LI[rng()%4]; qL=LQ[rng()%8]; }
            X[bin] = Complex(iL * sI, qL * sI);
        }
    }
    return X;
}

// Decision-region check: does the ACE'd point still decode to the SAME symbol
// as the original (i.e. it only moved within/beyond its own region)?
bool sameDecision(Complex orig, Complex aced, Modulation mod) {
    const auto qam = qamAxisSpec(mod);
    if (isPskMod(mod)) {
        // phase sector unchanged, magnitude only grew
        int npsk = (mod == Modulation::QAM8 || mod == Modulation::D8PSK) ? 8
                   : (mod == Modulation::DBPSK || mod == Modulation::BPSK) ? 2 : 4;
        float sector = 2.0f * float(M_PI) / npsk;
        float d = std::arg(aced * std::conj(orig));  // phase change
        bool phase_ok = std::abs(d) < sector * 0.499f;
        bool mag_ok = std::abs(aced) >= std::abs(orig) - 1e-4f;  // outward (or unchanged)
        return phase_ok && mag_ok;
    }
    // QAM: each coordinate must stay in the same level bin (or extend an outer one out)
    auto axisOk = [](float o, float a, float outer) {
        if (o > outer) return a >= o - 1e-4f;      // was +outer, only grew
        if (o < -outer) return a <= o + 1e-4f;     // was -outer, only grew (more negative)
        return std::abs(a - o) < 1e-4f;            // inner: must be pinned
    };
    return axisOk(orig.real(), aced.real(), qam.i_outer_boundary) &&
           axisOk(orig.imag(), aced.imag(), qam.q_outer_boundary);
}

void testMod(Modulation mod, const char* name) {
    const size_t fft = 1024;
    // 59 carriers, k=-29..30 skip 0 (production layout)
    std::vector<int> data_indices;
    for (int k = -29; k <= 30; ++k) {
        if (k == 0) continue;
        data_indices.push_back((k + int(fft)) % int(fft));
    }
    FFT fftp(fft);
    int improved = 0, samples = 12;
    double total_reduction = 0.0;
    for (int t = 0; t < samples; ++t) {
        auto X = makeSymbol(mod, data_indices, fft, 0x1000u + t * 131u);
        auto X0 = X;  // original (pre-ACE)
        AceResult r = applyAce(X, data_indices, mod, fftp,
                               /*clip_ratio=*/1.6f, /*max_iters=*/10, /*mu=*/1.0f);
        // INVARIANT 2: PAPR did not increase
        check(r.post_papr_db <= r.pre_papr_db + 0.05f, "PAPR must not increase");
        if (r.post_papr_db < r.pre_papr_db - 0.1f) ++improved;
        total_reduction += (r.pre_papr_db - r.post_papr_db);
        // INVARIANT 1: every data carrier still decodes to its original symbol
        for (int bin : data_indices) {
            if (!sameDecision(X0[bin], X[bin], mod)) {
                check(false, "data carrier left its decision region");
                break;
            }
        }
        // pilots/empty bins untouched
        for (size_t i = 0; i < fft; ++i) {
            bool is_data = false;
            for (int b : data_indices) if (int(i) == b) { is_data = true; break; }
            if (!is_data) check(std::abs(X[i] - X0[i]) < 1e-6f, "non-data bin modified");
        }
    }
    std::printf("  %-6s: mean PAPR reduction %.2f dB, improved %d/%d\n",
                name, total_reduction / samples, improved, samples);
    check(improved >= samples / 2, "ACE should reduce PAPR on most symbols");
}
}  // namespace

int main() {
    std::printf("== ACE PAPR reduction: invariants + benefit ==\n");
    testMod(Modulation::QPSK, "QPSK");
    testMod(Modulation::QAM8, "8PSK");
    testMod(Modulation::QAM16, "16QAM");
    testMod(Modulation::QAM32, "32QAM");
    if (g_failures == 0) std::printf("PASS: ACE invariants hold; PAPR reduced.\n");
    else std::printf("FAIL: %d check(s) failed.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
