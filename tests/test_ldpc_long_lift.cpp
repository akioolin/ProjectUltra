#include "fec/ldpc_802_11n.hpp"
#include <cstdio>
#include <vector>
#include <random>
using namespace ultra::ldpc_802_11n;
using ultra::CodeRate;

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

static void report(const char* tag, CodeRate r, int Z) {
    ExpandedLDPC e = expand(r, Z);
    std::mt19937 rng(12345);
    bool ok = true; for (int t = 0; t < 20; ++t) ok &= validCodeword(e, rng);
    bool cyc = hasFourCycle(e);
    printf("%-14s n=%4d m=%4d k=%4d rate=%.3f  valid_codeword=%s  4-cycle=%s\n",
           tag, e.n, e.m, e.k, (double)e.k/e.n, ok?"YES":"NO!", cyc?"PRESENT!":"none");
}

int main() {
    printf("=== n=648 (Z=27) regression — must be unchanged + girth-good ===\n");
    report("R1/2 Z=27", CodeRate::R1_2, 27);
    report("R2/3 Z=27", CodeRate::R2_3, 27);
    report("R3/4 Z=27", CodeRate::R3_4, 27);
    printf("=== n=1944 (Z=81) lifted long code — the proper LDPC ===\n");
    report("R1/2 Z=81", CodeRate::R1_2, 81);
    report("R2/3 Z=81", CodeRate::R2_3, 81);
    report("R3/4 Z=81", CodeRate::R3_4, 81);
    return 0;
}
