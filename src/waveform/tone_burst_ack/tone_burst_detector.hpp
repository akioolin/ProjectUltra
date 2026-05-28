// tone_burst_detector.hpp — matched-filter detector for the tone-burst ACK.
// PHY_ADAPTATION_DESIGN §15 step 2.
//
// Detection pipeline:
//   1. (optional) Costas correlation sweep to find timing (detectAndDecode).
//   2. Per-symbol Goertzel at the 4 tone frequencies; pick the strongest.
//   3. Dibits -> coded bits -> Hamming(15,11) decode -> CRC check -> payload.
//
// Goertzel vs FFT: for 4 known frequencies and a sliding window, Goertzel is
// O(N) per frequency with no FFT overhead. For 4 tones × 1200-sample windows,
// each detection is ~9600 multiply-adds — trivial CPU.

#pragma once

#include "tone_burst_constants.hpp"
#include "tone_burst_payload.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace ultra {
namespace waveform {
namespace tone_burst_ack {

using SampleSpan = const float*;

struct DecodeAlignedResult {
    std::optional<ToneBurstAckPayload> payload;
    PayloadDecodeStats stats;
    // Per-payload-symbol confidence: ratio of strongest-tone magnitude to
    // second-strongest. 1.0 = tie (worst), >>1 = clearly the right tone.
    std::vector<float> per_symbol_confidence;
    // Aggregate confidence: minimum over the payload symbols. Useful as a
    // single-number quality metric.
    float min_confidence = 0.0f;
};

struct DetectAndDecodeResult {
    DecodeAlignedResult decode;
    // Index of the FIRST sample of the FIRST Costas symbol within the
    // input buffer. Only meaningful when costas_correlation_peak > threshold.
    size_t detected_offset_samples = 0;
    // Costas-pattern matched-filter peak energy. Higher = stronger
    // pattern lock. Detection threshold is symbol_ms-dependent.
    float costas_correlation_peak = 0.0f;
    bool sync_acquired = false;
};

class ToneBurstDetector {
public:
    ToneBurstDetector() = default;

    // Demap and decode an ALIGNED tone-burst. samples[0..kTotalSymbols*N-1]
    // must be the on-air burst starting at the first Costas symbol.
    // Used for tests + post-sync demap.
    DecodeAlignedResult decodeAligned(SampleSpan samples,
                                      size_t num_samples,
                                      uint32_t symbol_ms = kBaselineSymbolMs);

    // Sweep `samples` for the Costas pattern, then demap+decode the burst.
    // Used when timing is uncertain (sender's "expected window" + guard).
    // `sweep_step_samples` controls the timing-search granularity (smaller
    // = finer + slower).
    DetectAndDecodeResult detectAndDecode(SampleSpan samples,
                                          size_t num_samples,
                                          uint32_t symbol_ms = kBaselineSymbolMs,
                                          uint32_t sweep_step_samples = 8);

    // ----- helpers exposed for testing -----

    // Compute Goertzel magnitudes for all 4 tones over a sample window.
    // Caller provides the window (single symbol worth of samples).
    static std::vector<float> toneMagnitudes(SampleSpan window_samples,
                                             size_t window_samples_count);

    // Pick the strongest of 4 tone magnitudes and return its dibit index +
    // confidence (strongest/second-strongest magnitude ratio).
    static uint8_t demapDibit(const std::vector<float>& magnitudes,
                              float& confidence);

    // Costas correlation at a given timing offset. Returns the sum of
    // expected-tone Goertzel magnitudes over the 4 Costas symbols. Higher
    // = better sync. samples[start_offset..start_offset+4*N-1] is used.
    static float costasCorrelationAtOffset(SampleSpan samples,
                                           size_t num_samples,
                                           size_t start_offset,
                                           uint32_t samples_per_symbol);
};

}  // namespace tone_burst_ack
}  // namespace waveform
}  // namespace ultra
