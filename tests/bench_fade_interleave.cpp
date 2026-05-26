// bench_fade_interleave — quantify the time-interleave diversity prize on a
// slow Good-HF temporal fade, ACROSS MODULATION/RATE RUNGS, and the interleave
// DEPTH required to realize it.
//
// WHY: Good fading has coherence time Tc ~= 4 s (Doppler ~0.1 Hz). The 3000 bps
// target is ABOVE the QPSK R2/3 ceiling (2611 bps), so reaching it needs a
// higher rung (16QAM, or QPSK R3/4). Higher rungs are FADE-FRAGILE (16QAM needs
// ~6 dB more instantaneous SNR). The question this harness answers: does deep
// TIME interleaving (long codeword spread across many frames so the slow fade
// varies underneath) rescue the high rung at Good@20 — i.e., is the
// long-LDPC + deep-interleave plan the path to 3000?
//
// It isolates that from transport/timing confounds: real LDPC encoder/decoder,
// genie-CSI flat Rayleigh channel with Jakes Doppler, block-time-interleave at
// depth D codewords, real soft demapper (QPSK + 16QAM Gray). Reports codeword
// FER vs D per rung. MEASUREMENT TOOL (not a ctest gate).

#include "ultra/fec.hpp"
#include "fec/ldpc_802_11n.hpp"
#include <cstdio>
#include <vector>
#include <random>
#include <cmath>
#include <complex>
#include <algorithm>
#include <string>

using ultra::CodeRate;
using ultra::ldpc_802_11n::expand;

namespace {

constexpr double kSymbolRateHz = 41.667;   // OFDM symbol rate (baud)
constexpr int    kDataCarriers = 47;       // data carriers at pilot spacing 5

enum class Mod { QPSK = 2, QAM16 = 4 };    // value = bits/symbol

// Jakes sum-of-sinusoids flat Rayleigh fade, E[|g|^2] = 1, Doppler f_d.
struct JakesFade {
    int M; double fd;
    std::vector<double> alpha, phi_c, phi_s;
    JakesFade(double f_d, std::mt19937& rng, int n_sin = 16) : M(n_sin), fd(f_d) {
        std::uniform_real_distribution<double> ph(0.0, 2.0 * M_PI);
        for (int m = 0; m < M; ++m) {
            alpha.push_back(M_PI * (2.0 * (m + 1) - 1.0) / (2.0 * M));
            phi_c.push_back(ph(rng)); phi_s.push_back(ph(rng));
        }
    }
    std::complex<double> gain(double t) const {
        double re = 0.0, im = 0.0;
        for (int m = 0; m < M; ++m) {
            double w = 2.0 * M_PI * fd * std::cos(alpha[m]) * t;
            re += std::cos(w + phi_c[m]); im += std::cos(w + phi_s[m]);
        }
        double norm = std::sqrt(2.0 / M);
        return { norm * re / std::sqrt(2.0), norm * im / std::sqrt(2.0) };
    }
};

int grayToBin(int g, int m) { int b = 0; for (int i = m - 1; i >= 0; --i) b ^= (g >> i); return b; }

// PAM level value for m-bit Gray axis, unit-symbol-energy scale applied by caller.
double pamLevel(int gray_bits, int m) {
    int l = 0;                                  // invert Gray: binary index
    for (int i = 0; i < m; ++i) l |= (((grayToBin(gray_bits, m)) >> i) & 1) << i;
    return (double)(2 * l - ((1 << m) - 1));    // -(L-1)..(L-1) step 2
}
// Encode `bits` coded bits -> complex symbol (unit average energy).
std::complex<double> mapSym(const int* bits, int nbits, double scale) {
    int m = nbits / 2;
    int gi = 0, gq = 0;
    for (int i = 0; i < m; ++i) { gi = (gi << 1) | bits[i]; gq = (gq << 1) | bits[m + i]; }
    return { pamLevel(gi, m) * scale, pamLevel(gq, m) * scale };
}
// max-log LLR demap of one axis (m bits) given received y, gain a, N0. LLR>0 => bit 0.
void demapAxis(double y, double a, double N0, int m, double scale, float* out) {
    int L = 1 << m;
    std::vector<double> d0(m, 1e30), d1(m, 1e30);
    for (int l = 0; l < L; ++l) {
        int g = l ^ (l >> 1);                   // Gray code of level l
        double v = (double)(2 * l - (L - 1)) * scale;
        double dist = (y - a * v) * (y - a * v);
        for (int b = 0; b < m; ++b) {
            int bit = (g >> (m - 1 - b)) & 1;
            if (bit == 0) d0[b] = std::min(d0[b], dist); else d1[b] = std::min(d1[b], dist);
        }
    }
    for (int b = 0; b < m; ++b) out[b] = (float)((d1[b] - d0[b]) / N0);
}

std::vector<int> codedBits(ultra::LDPCEncoder& enc, const std::vector<uint8_t>& info, int n) {
    ultra::Bytes c = enc.encode(info);
    std::vector<int> bits; bits.reserve(n);
    for (uint8_t b : c) for (int i = 7; i >= 0 && (int)bits.size() < n; --i) bits.push_back((b >> i) & 1);
    bits.resize(n, 0); return bits;
}

double measureFER(CodeRate rate, int Z, Mod mod, double mean_snr_db, double fd,
                  int depth, int num_blocks, uint32_t seed) {
    auto e = expand(rate, Z);
    const int n = e.n, kbytes = e.k / 8;
    const int bps = (int)mod;                 // bits per QAM symbol
    const int m = bps / 2;                    // bits per axis
    const double scale = std::sqrt(3.0 / (2.0 * ((1 << bps) - 1.0) / ((1 << m) + 1.0)));
    // unit energy: E[v^2]/axis = (L^2-1)/3, two axes -> scale^2 * 2*(L^2-1)/3 = 1
    const int L = 1 << m;
    const double scale_e = std::sqrt(3.0 / (2.0 * (double)(L * L - 1)));
    (void)scale;

    ultra::LDPCEncoder enc(rate, Z);
    ultra::LDPCDecoder dec(rate, Z);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> byteval(0, 255);
    std::normal_distribution<double> gauss(0.0, 1.0);
    JakesFade fade(fd, rng);

    const double snr_lin = std::pow(10.0, mean_snr_db / 10.0);
    const double N0 = 1.0 / snr_lin;          // Es=1 per QAM symbol, avg|g|^2=1
    const double sig = std::sqrt(N0 / 2.0);
    const double dt_sym = 1.0 / kSymbolRateHz;
    const int carriers_per_ofdm = kDataCarriers;

    int total_cw = 0, failed_cw = 0;
    double t0 = 0.0;

    for (int blk = 0; blk < num_blocks; ++blk) {
        std::vector<std::vector<int>> cw(depth);
        for (int d = 0; d < depth; ++d) {
            std::vector<uint8_t> info(kbytes);
            for (auto& b : info) b = (uint8_t)byteval(rng);
            cw[d] = codedBits(enc, info, n);
        }
        // Block time-interleave depth D: tx slot t = col*D + row -> cw row, bit col.
        const long total_bits = (long)depth * n;
        std::vector<int> tx(total_bits);
        std::vector<std::pair<int,int>> origin(total_bits); // (row,col) for scatter-back
        for (long t = 0; t < total_bits; ++t) {
            int col = (int)(t / depth), row = (int)(t % depth);
            tx[t] = cw[row][col]; origin[t] = {row, col};
        }
        std::vector<std::vector<float>> llr(depth, std::vector<float>(n, 0.0f));
        // Group bits into QAM symbols; each symbol = one carrier-slot in time.
        long sym_idx = 0;
        for (long t = 0; t + bps <= total_bits; t += bps, ++sym_idx) {
            int b[8]; for (int i = 0; i < bps; ++i) b[i] = tx[t + i];
            std::complex<double> s = mapSym(b, bps, scale_e);
            double tt = t0 + (double)(sym_idx / carriers_per_ofdm) * dt_sym;
            std::complex<double> g = fade.gain(tt);
            double a = std::abs(g);                       // genie-CSI magnitude
            double yI = a * s.real() + sig * gauss(rng);
            double yQ = a * s.imag() + sig * gauss(rng);
            float lI[4], lQ[4];
            demapAxis(yI, a, N0, m, scale_e, lI);
            demapAxis(yQ, a, N0, m, scale_e, lQ);
            for (int i = 0; i < m; ++i) {
                auto [r0, c0] = origin[t + i];       llr[r0][c0] = lI[i];
                auto [r1, c1] = origin[t + m + i];   llr[r1][c1] = lQ[i];
            }
        }
        t0 += (double)((sym_idx) / carriers_per_ofdm + 1) * dt_sym;
        for (int d = 0; d < depth; ++d) {
            (void)dec.decodeSoft(std::span<const float>(llr[d].data(), n));
            ++total_cw; if (!dec.lastDecodeSuccess()) ++failed_cw;
        }
    }
    return (double)failed_cw / (double)total_cw;
}

double netBps(CodeRate rate, Mod mod) {
    double rcr = (rate == CodeRate::R1_2) ? 0.5 : (rate == CodeRate::R2_3) ? 2.0/3 :
                 (rate == CodeRate::R3_4) ? 0.75 : 0.25;
    return kDataCarriers * (double)(int)mod * kSymbolRateHz * rcr;
}

void sweep(const char* tag, CodeRate rate, Mod mod, int Z, double snr, double fd, const char* fade) {
    auto e = expand(rate, Z);
    double span_ms = (double)e.n / (int)mod / kDataCarriers / kSymbolRateHz * 1000.0;
    printf("\n## %s  n=%d  %s f_d=%.2fHz  SNR=%.0f dB  raw-net ceiling=%.0f bps  (+1 depth=+%.0f ms)\n",
           tag, e.n, fade, fd, snr, netBps(rate, mod), span_ms);
    printf("  depth | span   | CW FER\n  ------+--------+-------\n");
    for (int D : {1, 4, 8, 16, 32, 64}) {
        int nb = std::max(20, 4000 / D);
        double fer = measureFER(rate, Z, mod, snr, fd, D, nb, 0xC0FFEE + D);
        printf("  %4d  | %5.2fs | %.4f\n", D, D * span_ms / 1000.0, fer);
    }
}

} // namespace

int main() {
    printf("=== Does deep TIME interleave rescue the HIGH rung at Good@20? (genie-CSI flat Rayleigh) ===\n");
    printf("3000 bps target is ABOVE QPSK R2/3 ceiling (2611). The high rung is fade-fragile.\n");

    // Baseline rung that already works but caps < 3000:
    sweep("QPSK R2/3  n=648", CodeRate::R2_3, Mod::QPSK, 27, 20.0, 0.1, "Good");
    // The 3000-capable rungs (fragile) — does interleave depth rescue them?
    sweep("16QAM R1/2 n=648", CodeRate::R1_2, Mod::QAM16, 27, 20.0, 0.1, "Good");
    sweep("16QAM R1/2 n=1944", CodeRate::R1_2, Mod::QAM16, 81, 20.0, 0.1, "Good");
    sweep("16QAM R2/3 n=648", CodeRate::R2_3, Mod::QAM16, 27, 20.0, 0.1, "Good");
    return 0;
}
