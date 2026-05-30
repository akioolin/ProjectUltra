// tools/null_decode_test.cpp
//
// CONTROLLED frequency-null injection → OFDM data-frame decode test.
//
// Purpose: prove CAUSALLY whether "carriers hitting a fade null" is what makes
// 16QAM R2/3 frames fail to decode on Good@20 — by controlling the null and
// watching the decode break, with EVERYTHING ELSE perfect (perfect sync, perfect
// CFO, PERFECT CSI / genie equalize, clean AWGN background). The only variable is
// the null (depth × width), so any pass→fail flip is caused by the null and
// nothing else. This removes the estimator/sync/CFO confounds that make a live
// GUI run only correlational.
//
// Model: one LDPC codeword (the real ultra LDPCEncoder/Decoder) mapped to 16QAM
// (the real qam16 constellation + soft_demap::demapQAM16), laid across N_DATA
// carriers over several OFDM symbols. A PERSISTENT frequency null (fixed carriers,
// every symbol — i.e. a fade held static over the codeword, which is < coherence
// time) attenuates |H| on W contiguous carriers by `null_db`. Genie ZF equalize
// (perfect CSI): s_hat = rx / H, per-carrier post-eq noise = sigma^2/|H|^2 → the
// demap LLR magnitude collapses on nulled carriers (a soft erasure), exactly as in
// the modem. NOTE: this is the WITHIN-codeword case (a single short frame can't
// escape a null over its own duration). Cross-FRAME interleave over >Tc (the
// §14.14 chunk harness) is a SEPARATE mechanism not modeled here.
//
// Build:  cmake --build build -j4 --target null_decode_test
// Run:    ./build/null_decode_test

#include "ultra/fec.hpp"
#include "ultra/types.hpp"
#include "ofdm/soft_demap.hpp"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using ultra::Complex;
using ultra::CodeRate;

namespace {

// Mirror of modulator.cpp:33 qam16_point (static there, so duplicated here).
constexpr float QS = 0.3162277660168379f;  // 1/sqrt(10)
Complex qam16_map(int word) {
    static const float lv[] = {-3, -1, 3, 1};  // Gray
    return Complex(lv[(word >> 2) & 3] * QS, lv[word & 3] * QS);
}

constexpr int N_DATA = 47;  // production OFDM data carriers (59 total - 12 pilots)

int infoBitsForRate(CodeRate r) {
    switch (r) {
        case CodeRate::R1_2: return 324;
        case CodeRate::R2_3: return 432;
        case CodeRate::R3_4: return 486;
        case CodeRate::R5_6: return 540;
        default:             return 432;
    }
}
const char* rateName(CodeRate r) {
    switch (r) {
        case CodeRate::R1_2: return "R1/2";
        case CodeRate::R2_3: return "R2/3";
        case CodeRate::R3_4: return "R3/4";
        case CodeRate::R5_6: return "R5/6";
        default:             return "?";
    }
}

// One trial: encode random info, 16QAM-map across carriers, apply a persistent
// null on W carriers (depth null_db) + AWGN at bg_snr_db, genie ZF-equalize,
// demap, LDPC-decode. Returns true if the decoded info matches exactly.
bool trial(CodeRate rate, int null_w, float null_db, float bg_snr_db,
           std::mt19937& rng) {
    const int K = infoBitsForRate(rate);
    const int Kbytes = K / 8;

    ultra::Bytes info(Kbytes);
    std::uniform_int_distribution<int> byteDist(0, 255);
    for (auto& b : info) b = static_cast<uint8_t>(byteDist(rng));

    ultra::LDPCEncoder enc(rate, 27);
    ultra::Bytes coded = enc.encode(info);     // N = 648 bits = 81 bytes
    const int Nbits = static_cast<int>(coded.size()) * 8;

    std::vector<int> cb(Nbits);
    for (int i = 0; i < Nbits; ++i)
        cb[i] = (coded[i / 8] >> (7 - (i % 8))) & 1;

    const int nsym = Nbits / 4;                // 162 QAM16 symbols
    const int null_c0 = (N_DATA - null_w) / 2; // center the null in the band

    // AWGN sized to the background SNR vs the 16QAM symbol power E|s|^2 = QS^2*5 = 0.5.
    const float sig_pow = 0.5f;
    const float noise_var = sig_pow / std::pow(10.0f, bg_snr_db / 10.0f);
    const float nstd = std::sqrt(noise_var / 2.0f);
    std::normal_distribution<float> g01(0.0f, 1.0f);

    std::vector<float> llr(Nbits);
    for (int s = 0; s < nsym; ++s) {
        const int word = (cb[4 * s] << 3) | (cb[4 * s + 1] << 2) |
                         (cb[4 * s + 2] << 1) | cb[4 * s + 3];
        const Complex tx = qam16_map(word);
        const int c = s % N_DATA;
        const bool nulled = (null_w > 0 && c >= null_c0 && c < null_c0 + null_w);
        const float h = nulled ? std::pow(10.0f, -null_db / 20.0f) : 1.0f;

        const Complex rx = h * tx + Complex(nstd * g01(rng), nstd * g01(rng));
        // Genie ZF equalize (PERFECT CSI): the estimate is NOT a confound.
        const Complex seq = rx / h;
        const float pe_nv = noise_var / (h * h);  // post-eq noise blows up on a null
        const auto l4 = ultra::soft_demap::demapQAM16(seq, pe_nv);
        llr[4 * s + 0] = l4[0];
        llr[4 * s + 1] = l4[1];
        llr[4 * s + 2] = l4[2];
        llr[4 * s + 3] = l4[3];
    }

    ultra::LDPCDecoder dec(rate, 27);
    ultra::Bytes out = dec.decodeSoft(llr);
    if (out.size() < info.size()) return false;
    for (size_t i = 0; i < info.size(); ++i)
        if (out[i] != info[i]) return false;
    return true;
}

float successRate(CodeRate rate, int null_w, float null_db, float bg_snr_db,
                  int trials, unsigned seed) {
    std::mt19937 rng(seed);
    int ok = 0;
    for (int t = 0; t < trials; ++t)
        if (trial(rate, null_w, null_db, bg_snr_db, rng)) ++ok;
    return 100.0f * ok / trials;
}

}  // namespace

int main() {
    const float BG = 25.0f;     // clean background SNR (dB) — only the null matters
    const int TRIALS = 60;
    const unsigned SEED = 0xC0FFEE;

    std::printf(
        "Controlled frequency-null decode test (genie/perfect CSI, BG SNR=%.0f dB)\n"
        "  Proves causally whether a fade NULL on W carriers breaks the decode.\n"
        "  %d carriers, persistent null centered in band, %d trials/cell.\n\n",
        BG, N_DATA, TRIALS);

    // --- SANITY: no null must decode 100% (else the harness/mapping is wrong). ---
    std::printf("--- SANITY (no null, W=0): every rate must be 100%% ---\n");
    for (CodeRate r : {CodeRate::R1_2, CodeRate::R2_3, CodeRate::R3_4, CodeRate::R5_6})
        std::printf("  16QAM %s : %.0f%%\n", rateName(r),
                    successRate(r, 0, 0.0f, BG, TRIALS, SEED));
    std::printf("\n");

    // --- DEPTH sweep at fixed width (W=8 carriers ~ 17%% of band) on R2/3 ---
    std::printf("--- DEPTH sweep, 16QAM R2/3, null width W=8 carriers ---\n");
    std::printf("  null_depth:  ");
    for (float d : {3.0f, 6.0f, 10.0f, 15.0f, 20.0f, 40.0f})
        std::printf("%5.0fdB ", d);
    std::printf("\n  decode OK:   ");
    for (float d : {3.0f, 6.0f, 10.0f, 15.0f, 20.0f, 40.0f})
        std::printf("%5.0f%% ", successRate(CodeRate::R2_3, 8, d, BG, TRIALS, SEED));
    std::printf("\n\n");

    // --- WIDTH sweep at a DEEP null (40 dB), per 16QAM rung ---
    std::printf("--- WIDTH sweep, DEEP null (40 dB), decode OK%% per rung ---\n");
    const int widths[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 20};
    std::printf("  null_width(carriers):     ");
    for (int w : widths) std::printf("%4d ", w);
    std::printf("\n  (band fraction):          ");
    for (int w : widths) std::printf("%3.0f%% ", 100.0f * w / N_DATA);
    std::printf("\n");
    for (CodeRate r : {CodeRate::R1_2, CodeRate::R2_3, CodeRate::R3_4, CodeRate::R5_6}) {
        std::printf("  16QAM %-5s OK%%:           ", rateName(r));
        for (int w : widths)
            std::printf("%3.0f%% ", successRate(r, w, 40.0f, BG, TRIALS, SEED));
        std::printf("\n");
    }
    std::printf("\n  (R2/3 redundancy = 33%% of bits; a null erases ~%d bits/carrier-width)\n",
                4 * (162 / N_DATA + 1));

    return 0;
}
