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
