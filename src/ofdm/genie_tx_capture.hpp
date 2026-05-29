#pragma once

// 2026-05-29 DIAGNOSTIC ONLY (ULTRA_GENIE_DATA_AIDED). True per-symbol "data-aided"
// channel genie: the OFDM modulator captures the exact transmitted frequency-domain
// symbol X[k] per data OFDM symbol into this process-global FIFO; the demodulator's
// equalizer then overrides its channel estimate with the EXACT effective channel
// H[k] = Y[k] / X[k]. Because both Y and X are in the demod's own units, this is the
// exact end-to-end per-carrier channel with no model, no interpolation, and no
// temporal staleness — the only genie proxy without a confound. Used to split the
// 16QAM decodability wall into estimation (genie -> 16QAM decodes) vs
// post-equalization (genie -> still fails). Single-process harness (measure_ack_fer)
// only: encoder pushes during encode, decoder reads in the same data-symbol order
// during decode; reset() per chunk. Not wired into any production path.

#include "ultra/types.hpp"

#include <cstddef>
#include <vector>

namespace ultra {
namespace genie {

struct TxCapture {
    bool enabled = false;
    // One entry per transmitted data OFDM symbol: the full freq-domain symbol
    // (fft_size bins; data on data carriers, pilots on pilot carriers, else 0).
    std::vector<std::vector<Complex>> symbols;
    std::size_t read_index = 0;
    // ALIGNMENT CAVEAT (2026-05-29): the in-order FIFO read aligns 1:1 ONLY when the
    // decoder processes the frame in a SINGLE continuous presynced pass (e.g. a 1-CW
    // frame). A 4-CW frame-interleaved frame decodes as re-synced chunks whose
    // per-chunk carrier-pattern reset diverges from the encoder's continuous push
    // order, so the FIFO read_index drifts +1 per codeword and the genie mispairs.
    // Self-aligning best-match (min frequency-jaggedness of H=Y/tx) was tried and is
    // too weak a discriminator (timing-ramp + 53-vs-51 carrier-pattern mismatch
    // flatten the contrast). Use --frame-cw 1 for genie measurements. See
    // docs/16QAM_DECODABILITY_DIAGNOSIS_2026_05_29.md.

    void reset() {
        symbols.clear();
        read_index = 0;
    }
};

// One shared instance per process (inline => single definition across TUs).
inline TxCapture& txCapture() {
    static TxCapture instance;
    return instance;
}

}  // namespace genie
}  // namespace ultra
