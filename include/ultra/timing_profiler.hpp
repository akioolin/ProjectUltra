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

// Identifies which call site invoked robustDecodeSingleCW() so the
// per-site retry histogram is correctly attributed.
enum class SingleCWCallSite {
    Default,        // code-rate fallback, small-frame recovery, salvage
    ControlFirst,   // control-first ACK path (streaming_decoder.cpp:1098)
    Cw0Peek,        // CW0 peek path (streaming_decoder.cpp:1294)
};

// |LLR|_avg distribution for 1-CW probes, split by decode outcome.
// Used to pick the right pre-screen threshold from data: if the success
// and fail clouds separate at LLR=X, gate the decode at X. If they overlap
// across all bins, mean |LLR| isn't the right discriminator.
//
// Bins: 13 buckets of 0.5-width — [0,0.5), [0.5,1.0), ..., [5.5,6.0), [6.0,inf).
// Skipped calls (gated by MIN_LLR_FOR_1CW_DECODE) record under fail since
// they would have decoded-and-failed.
struct LLRHistogram {
    static constexpr size_t kBins = 13;
    static constexpr float kBinWidth = 0.5f;

    std::atomic<uint64_t> success[kBins] {};
    std::atomic<uint64_t> fail[kBins] {};

    void reset() {
        for (auto& b : success) b.store(0);
        for (auto& b : fail) b.store(0);
    }

    void record(float llr_abs_avg, bool decode_ok) {
        size_t idx = static_cast<size_t>(llr_abs_avg / kBinWidth);
        if (idx >= kBins) idx = kBins - 1;
        if (decode_ok) {
            success[idx].fetch_add(1, std::memory_order_relaxed);
        } else {
            fail[idx].fetch_add(1, std::memory_order_relaxed);
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
    // Default-OFF HARQ counterfactual decoder-region timing. Each sample is
    // one full-schedule CW call (including cache construction); frame-evaluation
    // samples cover assembly, CRC, cache lookup, and any factor recovery for one
    // counterfactual frame. The outer decode_fixed_frame_total remains the
    // authoritative end-to-end RX-worker wall time including all bookkeeping.
    PhaseStats harq_shadow_fresh_decode;
    PhaseStats harq_shadow_frame_evaluation;
    PhaseStats harq_lazy_fresh_decode;
    PhaseStats harq_lazy_frame_evaluation;
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

    // LLR pre-screen counters (skip robustDecodeSingleCW when LLRs look like
    // noise — saves the ~85ms decode-and-fail cost on false-sync attempts).
    std::atomic<uint64_t> low_llr_1cw_skipped_control_first{0};
    std::atomic<uint64_t> low_llr_1cw_skipped_cw0_peek{0};

    // |LLR|_avg distribution split by decode outcome — for picking the
    // pre-screen threshold from data instead of guessing.
    LLRHistogram llr_dist_control_first;
    LLRHistogram llr_dist_cw0_peek;

    // HARQ key-build outcomes. Decoded-header keys are preferred; provisional
    // keys are a default-off, narrowly-gated QPSK R3/4 fallback built from the
    // receiver's ARQ/session context when CW0 is too faded to decode.
    std::atomic<uint64_t> harq_key_build_success{0};
    std::atomic<uint64_t> harq_key_build_failed{0};
    std::atomic<uint64_t> harq_key_build_provisional{0};
    // Provisional-key mirror proven wrong: a header-verified seq contradicted
    // the position prediction (prefix gate), or a decode under a provisional
    // key revealed a different seq (finalize guard). The default-ON decision
    // for provisional keys rides on this staying ~0 across multi-seed runs.
    std::atomic<uint64_t> harq_prediction_mismatch{0};
    // Frame-validated fresh-only rescue: at least one failed combined CW was
    // replaced by its exact fresh schedule and the resulting (possibly hybrid)
    // frame passed complete header+CRC. Counted per frame, never on syndrome
    // alone. all_fresh_frame_rescue is the strict all-fresh subset where the
    // production frame failed CRC and the complete all-fresh baseline passed.
    std::atomic<uint64_t> harq_fresh_rescue{0};
    std::atomic<uint64_t> harq_fresh_rescue_provisional{0};
    std::atomic<uint64_t> harq_all_fresh_frame_rescue{0};
    std::atomic<uint64_t> harq_all_fresh_frame_rescue_provisional{0};
    // Default-OFF causal shadow: after a combined sum decodes, run the exact
    // production schedule on the fresh observation with a separate decoder.
    // These counters distinguish "both would pass" from a proven
    // combine-only win without changing the production verdict; two distinct
    // CRC-valid byte strings are reported as divergent/inconclusive.
    std::atomic<uint64_t> harq_shadow_eligible{0};
    std::atomic<uint64_t> harq_shadow_both_pass{0};
    std::atomic<uint64_t> harq_shadow_combine_only{0};
    std::atomic<uint64_t> harq_shadow_divergent{0};
    std::atomic<uint64_t> harq_shadow_eligible_provisional{0};
    std::atomic<uint64_t> harq_shadow_both_pass_provisional{0};
    std::atomic<uint64_t> harq_shadow_combine_only_provisional{0};
    std::atomic<uint64_t> harq_shadow_divergent_provisional{0};
    // Neither production nor the exact all-fresh counterfactual produced a
    // valid frame. Recorded lazily on failed combined frames even with the
    // shadow diagnostic off.
    std::atomic<uint64_t> harq_double_fail{0};
    std::atomic<uint64_t> harq_double_fail_provisional{0};

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
        harq_shadow_fresh_decode.reset();
        harq_shadow_frame_evaluation.reset();
        harq_lazy_fresh_decode.reset();
        harq_lazy_frame_evaluation.reset();
        low_llr_escalation_skipped.store(0);
        robust_cw_control_first.reset();
        robust_cw_cw0_peek.reset();
        robust_cw_default.reset();
        raw_cw0_probe_skipped.store(0);
        low_llr_1cw_skipped_control_first.store(0);
        low_llr_1cw_skipped_cw0_peek.store(0);
        llr_dist_control_first.reset();
        llr_dist_cw0_peek.reset();
        harq_key_build_success.store(0);
        harq_key_build_failed.store(0);
        harq_key_build_provisional.store(0);
        harq_prediction_mismatch.store(0);
        harq_fresh_rescue.store(0);
        harq_fresh_rescue_provisional.store(0);
        harq_all_fresh_frame_rescue.store(0);
        harq_all_fresh_frame_rescue_provisional.store(0);
        harq_shadow_eligible.store(0);
        harq_shadow_both_pass.store(0);
        harq_shadow_combine_only.store(0);
        harq_shadow_divergent.store(0);
        harq_shadow_eligible_provisional.store(0);
        harq_shadow_both_pass_provisional.store(0);
        harq_shadow_combine_only_provisional.store(0);
        harq_shadow_divergent_provisional.store(0);
        harq_double_fail.store(0);
        harq_double_fail_provisional.store(0);
    }
};

// Magic-static singleton — thread-safe initialization (C++11+).
inline DecoderProfile& globalDecoderProfile() {
    static DecoderProfile p;
    return p;
}

}  // namespace ultra::timing
