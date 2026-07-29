#pragma once

// 2026-05-29 DIAGNOSTIC ONLY (ULTRA_GENIE_DATA_AIDED). True per-symbol "data-aided"
// channel genie: the OFDM modulator captures the exact transmitted frequency-domain
// symbol X[k] per data OFDM symbol; the demodulator's equalizer then overrides its
// channel estimate with the EXACT effective channel H[k] = Y[k] / X[k]. Because both
// Y and X are in the demod's own units, this is the exact end-to-end per-carrier
// channel with no model, no interpolation, and no temporal staleness — the only genie
// proxy without a confound. Used to split the 16QAM decodability wall into estimation
// (genie -> 16QAM decodes) vs post-equalization (genie -> still fails).
// Single-process harness (measure_ack_fer) only; not wired into any production path.
//
// ---------------------------------------------------------------------------
// 2026-07-28 ALIGNMENT REWRITE — why the FIFO cursor could never work
// ---------------------------------------------------------------------------
// The original design was an in-order FIFO: one push per TX data symbol, one
// consuming read per equalize(). Measured on the live decoder, that premise is
// false in BOTH directions:
//
//   * The decoder equalizes the SAME physical symbol 2-6 times. A clean 4-CW frame
//     runs three passes over the same audio — a control-first peek (7 symbols, at the
//     CONTROL carrier geometry: 47 data / 12 pilots, not the data profile's 51/8), a
//     CW0 header peek (7 symbols), then the authoritative full pass (26 symbols) —
//     i.e. 40 equalize() calls for 26 pushed symbols, +14 spurious consumptions per
//     frame. Failure paths (smallframe-1cw, sync-recovery +/-8 samples, cw-discovery,
//     marker-retry) add whole further passes.
//   * The decoder equalizes symbols the encoder never pushed (the control-geometry
//     peek, post-burst false locks, trailing partial symbols) AND skips frames the
//     encoder did push (a frame the RX never syncs to). Both are a function of the
//     fade realization, so the read set is channel-dependent.
//
// A consuming cursor therefore cannot be repaired by better keying of the cursor
// itself. The fix is to stop counting and start addressing: every capture entry
// carries its ABSOLUTE SAMPLE OFFSET in the transmitted stream, and the decoder looks
// up (never consumes) by the absolute sample position of the symbol it is equalizing:
//
//     abs_sym = abs_train + (training_symbols + data_symbol_index) * symbol_samples
//
// which the RX already computes (`absolute_training_start_sample_` -> phase_ref_sample).
// This is invariant to re-decodes, peeks, retries, dropped frames and out-of-order
// recovery — no counting scheme has that property. A single constant TX->RX stream
// origin is learned once per chunk. Every lookup also checks the CARRIER GEOMETRY
// (n_data/n_pilot), which is what declines the control-first peek (47 vs 51). A miss
// leaves the production estimate untouched — the genie degrades to baseline, never to
// garbage — and misses/geometry rejects are counted and printed (ULTRA_GENIE_DEBUG),
// because a SILENT mispair is exactly what produced the old bogus numbers.

#include "ultra/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace ultra {
namespace genie {

// One transmitted data OFDM symbol: the full freq-domain symbol (fft_size bins; data
// on data carriers, pilots on pilot carriers, else 0) plus the identity needed to
// address it from the receiver.
struct TxSymbol {
    std::vector<Complex> x;
    long long frame_base = 0;  // TX offset of the FIRST data symbol of this frame
    int sym_in_frame = 0;      // index within the frame == RX current_data_symbol_index_
    std::size_t n_data = 0;    // carrier geometry at capture time (the self-check)
    std::size_t n_pilot = 0;
};

struct TxCapture {
    bool enabled = false;
    std::vector<TxSymbol> symbols;         // push order == wire order
    std::vector<long long> frame_bases;    // strictly increasing, one per modulate() call
    std::vector<std::size_t> frame_first;  // index into symbols of each frame's symbol 0
    std::vector<std::size_t> frame_count;  // data symbols in each frame

    // --- TX-side addressing -------------------------------------------------
    // Absolute offset, within the stream the harness will transmit, of the FIRST data
    // symbol of the frame currently being modulated. The encoder sets this immediately
    // before waveform_->modulate(); the modulator adds the running per-symbol offset.
    long long frame_data_base = 0;

    // --- RX-side join -------------------------------------------------------
    // ONE constant per chunk: abs_rx(first data symbol of a frame) - frame_base. It is
    // the receiver's sync convention (the FFT window is deliberately placed inside the
    // CP) plus the channel's group delay. MEASURED: +796 samples on ITU Good, -148 on
    // AWGN — i.e. it EXCEEDS half a symbol (576). That is precisely why the join must be
    // anchored at FRAME granularity: frames are >=9 symbols apart, so a sub-symbol sync
    // convention can never alias to the neighbouring frame, whereas a nearest-SYMBOL
    // match aliases by exactly one symbol and produces a confident, geometry-valid,
    // small-residual, COMPLETELY WRONG pairing (measured: content agreement 0.25 = chance).
    bool origin_valid = false;
    long long origin = 0;
    std::vector<long long> origin_votes;  // pre-lock candidates

    // --- accounting (a silent mispair is a bug, so make it loud) ------------
    std::size_t lookups = 0;
    std::size_t hits = 0;
    std::size_t misses = 0;       // no frame anchor / no such symbol index
    std::size_t geom_rejects = 0; // frame+symbol found, but a different carrier geometry
    long long worst_residual = 0; // max |frame anchor residual| over hits

    void reset() {
        symbols.clear();
        frame_bases.clear();
        frame_first.clear();
        frame_count.clear();
        frame_data_base = 0;
        origin_valid = false;
        origin = 0;
        origin_votes.clear();
        lookups = hits = misses = geom_rejects = 0;
        worst_residual = 0;
    }

    void push(const std::vector<Complex>& freq_domain, long long frame_base,
              std::size_t n_data, std::size_t n_pilot) {
        if (frame_bases.empty() || frame_bases.back() != frame_base) {
            frame_bases.push_back(frame_base);
            frame_first.push_back(symbols.size());
            frame_count.push_back(0);
        }
        TxSymbol s;
        s.x = freq_domain;
        s.frame_base = frame_base;
        s.sym_in_frame = static_cast<int>(frame_count.back());
        s.n_data = n_data;
        s.n_pilot = n_pilot;
        symbols.push_back(std::move(s));
        ++frame_count.back();
    }

    // Address a captured symbol by (frame anchor, symbol index within the frame).
    //
    //   anchor_rx = abs_train + training_symbols * symbol_samples
    //              (the receiver's absolute position for THIS pass's first data symbol)
    //   sym_index = current_data_symbol_index_ (frame-relative on the presynced path,
    //              and identical to the transmitter's per-modulate() symbol_index)
    //
    // NEVER mutates the table and NEVER advances a cursor: the same physical symbol is
    // legitimately looked up 2-6 times per frame. Returns nullptr (and counts why) when
    // the receiver is equalizing something the encoder did not transmit here.
    const TxSymbol* lookup(long long anchor_rx, int sym_index, long long symbol_samples,
                           std::size_t n_data, std::size_t n_pilot) {
        ++lookups;
        if (frame_bases.empty() || symbol_samples <= 0 || sym_index < 0) {
            ++misses;
            return nullptr;
        }

        // Frame anchor. Tolerance = one full symbol: a frame whose sync error exceeds
        // the cyclic prefix does not decode at all, so one symbol is a generous bound,
        // while the nearest OTHER frame is >=9 symbols away — no aliasing is possible.
        const long long tol = symbol_samples;
        long long key = anchor_rx - (origin_valid ? origin : 0);
        std::size_t f = nearestFrame(key);
        long long residual = key - frame_bases[f];

        if (!origin_valid) {
            // Bootstrap: require the candidate to be unambiguous (>=4x closer than the
            // runner-up) and physically plausible (<=2 symbols under the origin=0 guess,
            // which holds by construction when the harness pumps the TX vector from
            // sample 0 into a fresh decoder). Then require 4 agreeing votes, so a single
            // false lock cannot pin a wrong origin. Until locked the genie is INERT —
            // it degrades to the production estimate, it never guesses.
            if (std::llabs(residual) > 2 * symbol_samples || !unambiguous(key, f)) {
                ++misses;
                return nullptr;
            }
            origin_votes.push_back(residual);
            std::size_t agree = 0;
            for (long long v : origin_votes) {
                if (std::llabs(v - residual) <= symbol_samples / 2) ++agree;
            }
            if (agree < 4) {
                ++misses;
                return nullptr;
            }
            origin = residual;
            origin_valid = true;
            key = anchor_rx - origin;
            f = nearestFrame(key);
            residual = key - frame_bases[f];
        }

        if (std::llabs(residual) > tol || !unambiguous(key, f)) {
            ++misses;
            return nullptr;
        }
        if (static_cast<std::size_t>(sym_index) >= frame_count[f]) {
            ++misses;  // pass ran past the end of the transmitted frame
            return nullptr;
        }
        const TxSymbol& s = symbols[frame_first[f] + static_cast<std::size_t>(sym_index)];
        if (s.n_data != n_data || s.n_pilot != n_pilot) {
            ++geom_rejects;
            return nullptr;
        }
        ++hits;
        worst_residual = std::max(worst_residual, std::llabs(residual));
        return &s;
    }

    void reportIfDebug(const char* tag) const {
        if (!enabled) return;
        if (std::getenv("ULTRA_GENIE_DEBUG") == nullptr) return;
        std::fprintf(stderr,
                     "[genie] %s frames=%zu pushed=%zu lookups=%zu hits=%zu misses=%zu "
                     "geom_rejects=%zu origin=%lld worst_residual=%lld\n",
                     tag, frame_bases.size(), symbols.size(), lookups, hits, misses,
                     geom_rejects, origin_valid ? origin : 0LL, worst_residual);
    }

  private:
    std::size_t nearestFrame(long long key) const {
        auto it = std::lower_bound(frame_bases.begin(), frame_bases.end(), key);
        std::size_t best = 0;
        long long best_d = 0;
        bool have = false;
        if (it != frame_bases.end()) {
            best = static_cast<std::size_t>(it - frame_bases.begin());
            best_d = std::llabs(*it - key);
            have = true;
        }
        if (it != frame_bases.begin()) {
            auto prev = it - 1;
            const long long d = std::llabs(*prev - key);
            if (!have || d < best_d) {
                best = static_cast<std::size_t>(prev - frame_bases.begin());
                best_d = d;
            }
        }
        return best;
    }

    // The nearest frame must be at least 4x closer than any other frame.
    bool unambiguous(long long key, std::size_t f) const {
        if (frame_bases.size() < 2) return true;
        const long long d = std::llabs(frame_bases[f] - key);
        long long runner = -1;
        if (f > 0) runner = std::llabs(frame_bases[f - 1] - key);
        if (f + 1 < frame_bases.size()) {
            const long long d2 = std::llabs(frame_bases[f + 1] - key);
            if (runner < 0 || d2 < runner) runner = d2;
        }
        return runner < 0 || runner >= 4 * d || d == 0;
    }
};

// One shared instance per process (inline => single definition across TUs).
inline TxCapture& txCapture() {
    static TxCapture instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Receive-pass context (DIAGNOSTIC). The absolute sample identity of the symbols a
// decode pass is about to equalize. Written by the waveform/stream processor ONLY
// when a diagnostic consumer is armed (genie capture or the equalize tracer), read by
// the genie lookup and by the tracer. Inert with both off.
// ---------------------------------------------------------------------------
struct PassContext {
    std::size_t abs_train = 0;      // absolute sample of this pass's first LTS symbol
    std::size_t symbol_samples = 0; // fft + CP + guard
    int training_symbols = 0;       // LTS symbols the pass skips before data
    bool presynced = false;         // false => streaming SYNCED path (no abs identity)
};

inline PassContext& passContext() {
    static PassContext instance;
    return instance;
}

// ---------------------------------------------------------------------------
// 2026-07-28 DIAGNOSTIC ONLY (ULTRA_EQ_TRACE=1): decoder equalize() call-tree
// tracer. Answers "how many times, from which call site, for which symbol" so
// the genie alignment can be attributed to a specific out-of-band
// probe/peek/retry rather than guessed at. Pure stderr, no production effect.
// ---------------------------------------------------------------------------
struct EqTrace {
    bool enabled = false;
    const char* site = "-";        // decoder call-site tag (set before process())
    long pass = 0;                 // processPresynced()/process() invocation ordinal
    long calls = 0;                // global equalize() ordinal
};

inline EqTrace& eqTrace() {
    static EqTrace instance = [] {
        EqTrace t;
        const char* v = std::getenv("ULTRA_EQ_TRACE");
        t.enabled = v && v[0] == '1';
        return t;
    }();
    return instance;
}

// True when any diagnostic consumer needs the per-pass absolute symbol identity.
inline bool passContextNeeded() {
    return txCapture().enabled || eqTrace().enabled;
}

}  // namespace genie
}  // namespace ultra
