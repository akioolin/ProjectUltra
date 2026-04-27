// Decode-thread CPU profiler — header-only.
// See .claude/plans/imperative-napping-conway.md for the bucket design.
//
// Usage:
//   #include "ultra/timing_profiler.hpp"
//   using ultra::timing::ScopedTimer;
//   using ultra::timing::globalDecoderProfile;
//
//   void someFunction() {
//       ScopedTimer _t(globalDecoderProfile().some_bucket);
//       ...
//   }

#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace ultra::timing {

struct PhaseStats {
    std::atomic<uint64_t> total_us{0};
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> max_us{0};

    void reset() {
        total_us.store(0);
        count.store(0);
        max_us.store(0);
    }

    // Manual-add helper for sites that can't use ScopedTimer (e.g. when
    // the duration is computed from an outer span and we want to record
    // it under a separate sub-bucket).
    void addSample(uint64_t us) {
        total_us.fetch_add(us, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
        uint64_t prev = max_us.load(std::memory_order_relaxed);
        while (us > prev &&
               !max_us.compare_exchange_weak(prev, us,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {}
    }
};

class ScopedTimer {
public:
    explicit ScopedTimer(PhaseStats& s) : s_(s),
        start_(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() {
        const uint64_t us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start_).count());
        s_.addSample(us);
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    PhaseStats& s_;
    std::chrono::steady_clock::time_point start_;
};

// Per-call-site histogram of which retry attempt of robustDecodeSingleCW()
// succeeded (or whether all failed). Index 0 = success on initial decode (no
// retries). Index 1..MAX = success on retry N. Index MAX+1 = exhausted all
// retries without success. Sized for 4 retries (the default budget) — sites
// using a smaller budget simply leave higher slots at 0.
struct SingleCWHistogram {
    static constexpr size_t kMaxRetries = 4;
    std::atomic<uint64_t> first_try{0};                  // succeeded with no retries
    std::atomic<uint64_t> retry[kMaxRetries] {};         // succeeded on retry N (1..4)
    std::atomic<uint64_t> exhausted{0};                  // failed after all attempts

    void reset() {
        first_try.store(0);
        for (auto& r : retry) r.store(0);
        exhausted.store(0);
    }

    // attempts_used: 0 = first try; 1..kMaxRetries = retry index that succeeded;
    // -1 = all attempts failed.
    void record(int attempts_used) {
        if (attempts_used < 0) {
            exhausted.fetch_add(1, std::memory_order_relaxed);
        } else if (attempts_used == 0) {
            first_try.fetch_add(1, std::memory_order_relaxed);
        } else if (attempts_used >= 1 && attempts_used <= static_cast<int>(kMaxRetries)) {
            retry[attempts_used - 1].fetch_add(1, std::memory_order_relaxed);
        }
    }
};

// All decode-thread profiling buckets. See plan v5 for the design.
struct DecoderProfile {
    PhaseStats detect_data_sync;
    PhaseStats ofdm_process_total;
    PhaseStats lts_channel_estimate;
    PhaseStats data_symbol_loop;
    PhaseStats decode_fixed_frame_total;
    PhaseStats ldpc_cw_total;
    PhaseStats single_cw_decode_total;
    PhaseStats control_first_1cw;
    PhaseStats cw0_peek_1cw;
    PhaseStats ofdm_cw0_probe_decode;
    PhaseStats failed_4cw_after_peek;
    std::atomic<uint64_t> low_llr_escalation_skipped{0};

    // Per-call-site retry-attempt histograms for robustDecodeSingleCW().
    // Three sites of interest (rest grouped under "default"):
    //   control_first — ACK decode path at streaming_decoder.cpp:1098
    //   cw0_peek      — CW0 peek path at streaming_decoder.cpp:1294
    //   default       — code-rate fallback, small-frame recovery, salvage
    SingleCWHistogram robust_cw_control_first;
    SingleCWHistogram robust_cw_cw0_peek;
    SingleCWHistogram robust_cw_default;

    // Probe-skip counter (Step 3: skip raw CW0 probe on known-4-CW data frames).
    std::atomic<uint64_t> raw_cw0_probe_skipped{0};

    void reset() {
        detect_data_sync.reset();
        ofdm_process_total.reset();
        lts_channel_estimate.reset();
        data_symbol_loop.reset();
        decode_fixed_frame_total.reset();
        ldpc_cw_total.reset();
        single_cw_decode_total.reset();
        control_first_1cw.reset();
        cw0_peek_1cw.reset();
        ofdm_cw0_probe_decode.reset();
        failed_4cw_after_peek.reset();
        low_llr_escalation_skipped.store(0);
        robust_cw_control_first.reset();
        robust_cw_cw0_peek.reset();
        robust_cw_default.reset();
        raw_cw0_probe_skipped.store(0);
    }
};

// Magic-static singleton — thread-safe initialization (C++11+).
inline DecoderProfile& globalDecoderProfile() {
    static DecoderProfile p;
    return p;
}

// Identifies which call site invoked robustDecodeSingleCW() so the
// per-site retry histogram is correctly attributed.
enum class SingleCWCallSite {
    Default,        // code-rate fallback, small-frame recovery, salvage
    ControlFirst,   // control-first ACK path (streaming_decoder.cpp:1098)
    Cw0Peek,        // CW0 peek path (streaming_decoder.cpp:1294)
};

}  // namespace ultra::timing
