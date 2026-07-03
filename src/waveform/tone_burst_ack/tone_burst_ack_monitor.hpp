// tone_burst_ack_monitor.hpp — sliding-window detector for the tone-burst ACK.
// PHY_ADAPTATION_DESIGN §15 step 4a.
//
// Wraps the ToneBurstDetector with the streaming-audio machinery the
// production path will need:
//   - feedAudio() interface matching StreamingDecoder's pattern
//   - sliding circular buffer big enough to hold one full burst at the
//     longest staircase duration + guard time
//   - runs detection at a configurable cadence (every N samples) to keep
//     CPU bounded
//   - emits a callback when a tone-burst ACK is decoded
//
// Designed so step 4b can construct one inside StreamingDecoder and route
// feedAudio() into it with a single line; integration risk stays low.

#pragma once

#include "tone_burst_constants.hpp"
#include "tone_burst_detector.hpp"
#include "tone_burst_payload.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ultra {
namespace waveform {
namespace tone_burst_ack {

// Callback invoked when a complete tone-burst ACK is decoded.
//   payload         — the decoded payload (group_seq, mask, rate_hint, type)
//   detected_offset — sample offset (within the monitor's internal stream
//                     counter) of the first Costas symbol
//   correlation_peak — Costas matched-filter score at the detection
struct ToneBurstAckDetection {
    ToneBurstAckPayload payload;
    uint64_t detected_stream_offset = 0;
    float correlation_peak = 0.0f;
    int hamming_corrected_blocks = 0;
    uint32_t symbol_ms_used = 0;
};
using ToneBurstAckCallback = std::function<void(const ToneBurstAckDetection&)>;

class ToneBurstAckMonitor {
public:
    struct Config {
        // Symbol durations to scan for. The detector tries each in order
        // and stops at the first CRC-passing decode. Default covers the
        // §15.5 staircase from high-SNR (12 ms) through marginal-SNR
        // (100 ms). 200 ms / 50 ms are omitted from the default to keep
        // detection latency bounded; callers targeting weak-signal modes
        // can include them.
        std::vector<uint32_t> symbol_durations_ms = {
            kSymbolMsHighSNR,   // 12 ms
            kBaselineSymbolMs,  // 25 ms
            kSymbolMsLowSNR,    // 50 ms
            kSymbolMsMargSNR,   // 100 ms
        };
        // §15 step 4d-late: when armed_only=true, detection only runs while
        // the monitor is "armed" (via arm()). Outside the armed window,
        // runDetectionPass() returns immediately with zero work. This makes
        // the audio-thread CPU cost bounded by the active ACK window
        // (typically ~3 s out of every ~10 s in the burst transport),
        // eliminating the always-on polling jitter that the test-default
        // configuration accepts.
        //
        // armed_only=false (default) preserves the original polling
        // behavior — required by the unit tests, which don't have a
        // protocol layer to arm the monitor.
        bool armed_only = false;
        // Run detection every `detect_interval_samples` samples (cadence-
        // limiter). Default: every 240 samples = 5 ms at 48 kHz. Detection
        // window is ~850 ms (kTotalSymbols × kBaselineSymbolMs), so we
        // overlap windows densely without re-scanning at sample granularity.
        size_t detect_interval_samples = 240;
        // §15 step 4d-late: cadence to use WHILE armed. Defaults to the
        // same value as detect_interval_samples so callers that only set
        // armed_only get sensible behavior. Production overrides this to
        // a tight value (~100 ms = 4800 samples) so detection latency is
        // dominated by the burst airtime (~850 ms) not the cadence wait.
        size_t detect_interval_samples_armed = 240;
        // Coarse sweep step inside the detector for each cadence-triggered
        // pass. 8 samples is the default; smaller = finer + slower.
        uint32_t sweep_step_samples = 8;
        // Maximum samples the internal sliding buffer holds. Needs to be at
        // least kTotalSymbols × max_symbol_ms × kSampleRate / 1000 + slack.
        // For 100 ms max symbol: 34 × 100 × 48 = 163,200 (34 symbols since the
        // 2026-07-02 frame_mask 8->16 widen; was 27). Default = 180,000
        // (~3.75 s) still accommodates 100 ms + guard.
        size_t buffer_capacity_samples = 180000;
        // Suppress duplicate detections within this many samples of the
        // previous successful decode. Prevents re-firing on the same burst
        // when the next cadence tick lands inside it.
        size_t suppress_window_samples = static_cast<size_t>(kSampleRate) * 1000u / 1000u;  // 1 s

        // Minimum Costas correlation peak as a FRACTION of the ideal
        // clean-channel value, below which we reject the decode as a
        // false positive. Costas+Hamming+CRC alone can occasionally pass
        // on random noise; pairing them with a peak floor closes that
        // gap. Real bursts at SNR ≥ -10 dB sit at ~30%+ of ideal; 5% is
        // a generous cliff that no real burst dips below. See
        // computeMinPeakForSymbolMs() for the per-duration absolute floor.
        float min_peak_fraction = 0.05f;
    };

    // Ideal-clean-channel Costas peak magnitude at the given symbol
    // duration. Used both internally as a gate and exposed for tests.
    //   sum over kCostasSymbols of (samples_per_symbol/2) × kToneAmplitude
    // (Goertzel magnitude for a matched tone of amplitude A over N
    // samples is N·A/2.)
    static float idealCostasPeak(uint32_t symbol_ms);

    ToneBurstAckMonitor();
    explicit ToneBurstAckMonitor(Config cfg);

    // Audio thread: append samples to the internal buffer. Cheap (memcpy
    // into a circular buffer). Triggers detection passes at the configured
    // cadence; on a successful decode invokes the callback synchronously.
    void feedAudio(const float* samples, size_t count);
    void feedAudio(const std::vector<float>& samples) {
        feedAudio(samples.data(), samples.size());
    }

    // Install the callback. May be called once; subsequent calls replace.
    void setCallback(ToneBurstAckCallback cb) { callback_ = std::move(cb); }

    // §15 step 4d-late: arm the detector for an expected ACK arrival.
    // While armed, detection runs every detect_interval_samples_armed (the
    // tight production cadence). When the armed window elapses without a
    // successful decode, the monitor disarms automatically and goes idle.
    // Calling arm() while already armed extends the window to the larger
    // of (current deadline, new deadline).
    //
    // window_samples is relative to the current stream offset. Pass
    // (window_ms × kSampleRate / 1000) for a duration-based window.
    //
    // Has no effect when Config.armed_only is false (the always-on
    // polling configuration; arm() is a no-op there).
    void arm(size_t window_samples);

    // True iff a successful decode has not yet fired AND the armed window
    // has not yet elapsed. Always false when Config.armed_only is false.
    bool isArmed() const { return armed_; }

    // Reset internal state — drops the audio buffer + suppression window.
    // Use between protocol sessions.
    void reset();

    // For tests + diagnostics.
    uint64_t totalSamplesFed() const { return total_samples_fed_; }
    uint64_t detectionsEmitted() const { return detections_emitted_; }
    uint64_t suppressedAttempts() const { return suppressed_attempts_; }
    const Config& config() const { return cfg_; }

private:
    // Run one detection pass over the current buffer contents. Iterates
    // through symbol_durations_ms in order; stops at the first decode that
    // passes CRC.
    void runDetectionPass();

    Config cfg_;
    ToneBurstDetector detector_;
    ToneBurstAckCallback callback_;

    // Linear buffer (not strictly circular — we drop the oldest samples
    // when over capacity). Simpler than ring buffer; size cap keeps memory
    // bounded.
    std::vector<float> buffer_;
    // Stream-counter offset of buffer_[0] in the overall feedAudio stream.
    uint64_t buffer_start_stream_offset_ = 0;

    // Last triggered cadence position (relative to start of buffer_).
    size_t last_detect_pos_in_buffer_ = 0;
    // Cumulative samples fed since construction.
    uint64_t total_samples_fed_ = 0;
    // Stream offset of the last successful detection (for suppression).
    uint64_t last_detection_stream_offset_ = static_cast<uint64_t>(-1);

    uint64_t detections_emitted_ = 0;
    uint64_t suppressed_attempts_ = 0;

    // §15 step 4d-late: armed-window state. When armed_, runDetectionPass()
    // runs at cfg_.detect_interval_samples_armed cadence. When unset,
    // detection is gated by cfg_.armed_only (false = original always-on;
    // true = idle until armed again).
    bool armed_ = false;
    uint64_t arm_deadline_stream_offset_ = 0;
};

}  // namespace tone_burst_ack
}  // namespace waveform
}  // namespace ultra
