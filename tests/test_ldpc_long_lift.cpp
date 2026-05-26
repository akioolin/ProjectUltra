#include "fec/ldpc_802_11n.hpp"
#include "ultra/fec.hpp"
#include <cstdio>
#include <vector>
#include <random>
using namespace ultra::ldpc_802_11n;
using ultra::CodeRate;

static int g_failures = 0;

// End-to-end round-trip through the PRODUCTION LDPCEncoder/LDPCDecoder at the
// given lifting size, over a clean channel (strong LLRs). Proves the n=1944
// long code actually encodes + decodes through the real codec path, not just
// that its matrix expands. Returns true iff success flag set AND info recovered.
static bool roundTrip(CodeRate r, int Z, int& n_out, int& k_out) {
    ultra::LDPCEncoder enc(r, Z);
    ultra::LDPCDecoder dec(r, Z);
    ExpandedLDPC e = expand(r, Z);
    n_out = e.n; k_out = e.k;
    const int kbytes = e.k / 8;   // floor -> exactly one (zero-padded) block
    std::mt19937 rng(777);
    std::uniform_int_distribution<int> byteval(0, 255);
    std::vector<uint8_t> info(kbytes);
    for (auto& b : info) b = static_cast<uint8_t>(byteval(rng));

    ultra::Bytes coded = enc.encode(info);     // packed n-bit codeword (n%8==0 here)
    std::vector<float> llr;
    llr.reserve(coded.size() * 8);
    for (uint8_t bb : coded)
        for (int i = 7; i >= 0; --i)
            llr.push_back(((bb >> i) & 1) ? -8.0f : +8.0f);  // bit1 -> -LLR, bit0 -> +LLR
    llr.resize(e.n);

    ultra::Bytes out = dec.decodeSoft(llr);
    bool ok = dec.lastDecodeSuccess() && out.size() >= static_cast<size_t>(kbytes);
    for (int i = 0; ok && i < kbytes; ++i) ok &= (out[i] == info[i]);
    return ok;
}

static void reportRT(const char* tag, CodeRate r, int Z) {
    int n = 0, k = 0;
    bool ok = roundTrip(r, Z, n, k);
    printf("%-14s n=%4d k=%4d  clean round-trip(enc->dec)=%s\n",
           tag, n, k, ok ? "RECOVERED" : "FAIL!");
    if (!ok) ++g_failures;
}

// Build a valid codeword (info[0..k), parity[k..n)) and confirm every parity check = 0.
static bool validCodeword(const ExpandedLDPC& e, std::mt19937& rng) {
    std::vector<int> cw(e.n, 0);
    std::uniform_int_distribution<int> bit(0, 1);
    for (int j = 0; j < e.k; ++j) cw[j] = bit(rng);          // info bits
    for (int i = 0; i < e.m; ++i) {                           // parity = XOR(info in enc_rows)
        int p = 0; for (int j : e.enc_rows[i]) p ^= cw[j];
        cw[e.k + i] = p;
    }
    for (const auto& vars : e.H_rows) {                       // verify H * cw == 0
        int s = 0; for (int v : vars) s ^= cw[v];
        if (s != 0) return false;
    }
    return true;
}

// expect_clean: true if this code is required to be girth>=6 (no 4-cycle).
// R3/4 has a 4-cycle in the base matrix at every Z (documented weak base) so
// it is reported but not gated.
static void report(const char* tag, CodeRate r, int Z, bool expect_clean) {
    ExpandedLDPC e = expand(r, Z);
    std::mt19937 rng(12345);
    bool ok = true; for (int t = 0; t < 20; ++t) ok &= validCodeword(e, rng);
    bool cyc = hasFourCycle(e);
    printf("%-14s n=%4d m=%4d k=%4d rate=%.3f  valid_codeword=%s  4-cycle=%s\n",
           tag, e.n, e.m, e.k, (double)e.k/e.n, ok?"YES":"NO!", cyc?"PRESENT!":"none");
    if (!ok) ++g_failures;                       // every code must encode valid codewords
    if (expect_clean && cyc) ++g_failures;       // workhorse rates must be girth-clean
}

int main() {
    printf("=== n=648 (Z=27) regression — must be unchanged + girth-good ===\n");
    report("R1/2 Z=27", CodeRate::R1_2, 27, /*expect_clean=*/true);
    report("R2/3 Z=27", CodeRate::R2_3, 27, /*expect_clean=*/true);
    report("R3/4 Z=27", CodeRate::R3_4, 27, /*expect_clean=*/false);  // known weak base
    printf("=== n=1944 (Z=81) lifted long code — the proper LDPC ===\n");
    report("R1/2 Z=81", CodeRate::R1_2, 81, /*expect_clean=*/true);
    report("R2/3 Z=81", CodeRate::R2_3, 81, /*expect_clean=*/true);
    report("R3/4 Z=81", CodeRate::R3_4, 81, /*expect_clean=*/false);  // known weak base

    printf("\n=== production encoder/decoder round-trip (clean channel) ===\n");
    reportRT("R1/2 Z=27", CodeRate::R1_2, 27);   // canary: confirms LLR sign + n=648 path
    reportRT("R2/3 Z=27", CodeRate::R2_3, 27);
    reportRT("R1/2 Z=81", CodeRate::R1_2, 81);   // the long code through the real codec
    reportRT("R2/3 Z=81", CodeRate::R2_3, 81);

    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
