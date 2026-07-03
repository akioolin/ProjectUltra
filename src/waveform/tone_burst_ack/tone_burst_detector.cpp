// tone_burst_detector.cpp — Goertzel-based 4-FSK demodulator + Costas sweep.

#include "tone_burst_detector.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ultra {
namespace waveform {
namespace tone_burst_ack {

namespace {

constexpr float kTwoPi = 6.283185307179586f;

// Goertzel single-frequency power for `freq_hz` over a window of N samples.
// Returns the squared magnitude (proportional to power, no sqrt for speed).
float goertzelPowerAt(SampleSpan samples, size_t n_samples, float freq_hz) {
    const float coeff = 2.0f * std::cos(kTwoPi * freq_hz /
                                         static_cast<float>(kSampleRate));
    float q0 = 0.0f;
    float q1 = 0.0f;
    float q2 = 0.0f;
    for (size_t i = 0; i < n_samples; ++i) {
        q0 = coeff * q1 - q2 + samples[i];
        q2 = q1;
        q1 = q0;
    }
    // Squared magnitude of the resulting complex tone amplitude.
    return q1 * q1 + q2 * q2 - coeff * q1 * q2;
}

}  // namespace

// ----- static helpers -----

std::vector<float> ToneBurstDetector::toneMagnitudes(SampleSpan window_samples,
                                                      size_t window_samples_count) {
    std::vector<float> mags;
    mags.reserve(kNumTones);
    for (float f : kToneFreqHz) {
        const float power = goertzelPowerAt(window_samples, window_samples_count, f);
        // Use sqrt(power) = magnitude. Cheaper to compare on power, but the
        // tests want a "confidence ratio" so we want amplitude here.
        mags.push_back(std::sqrt(std::max(power, 0.0f)));
    }
    return mags;
}

uint8_t ToneBurstDetector::demapDibit(const std::vector<float>& magnitudes,
                                       float& confidence) {
    uint8_t best_idx = 0;
    float best = -1.0f;
    float second = -1.0f;
    for (uint8_t i = 0; i < magnitudes.size() && i < kNumTones; ++i) {
        const float m = magnitudes[i];
        if (m > best) {
            second = best;
            best = m;
            best_idx = i;
        } else if (m > second) {
            second = m;
        }
    }
    if (second <= 0.0f) {
        confidence = (best > 0.0f) ? 1e6f : 0.0f;
    } else {
        confidence = best / second;
    }
    return best_idx;
}

float ToneBurstDetector::costasCorrelationAtOffset(SampleSpan samples,
                                                    size_t num_samples,
                                                    size_t start_offset,
                                                    uint32_t samples_per_symbol) {
    // Sum of expected-tone Goertzel magnitudes across the 4 Costas symbols.
    // High when the actual tones at those positions match the expected
    // Costas pattern; near-noise-floor otherwise.
    float sum = 0.0f;
    for (uint32_t s = 0; s < kCostasSymbols; ++s) {
        const uint8_t expected_tone = kCostasPattern[s];
        const size_t window_start = start_offset + s * samples_per_symbol;
        if (window_start + samples_per_symbol > num_samples) return 0.0f;
        const float f = kToneFreqHz[expected_tone];
        const float power = goertzelPowerAt(samples + window_start,
                                             samples_per_symbol, f);
        sum += std::sqrt(std::max(power, 0.0f));
    }
    return sum;
}

// ----- decode aligned -----

DecodeAlignedResult ToneBurstDetector::decodeAligned(SampleSpan samples,
                                                      size_t num_samples,
                                                      uint32_t symbol_ms) {
    DecodeAlignedResult result;
    const uint32_t samples_per_symbol = (kSampleRate * symbol_ms) / 1000u;
    const size_t needed = static_cast<size_t>(kTotalSymbols) * samples_per_symbol;
    if (num_samples < needed) {
        return result;
    }

    // Skip Costas; demap the kPayloadSymbols (30) payload symbols.
    std::vector<uint8_t> dibits;
    dibits.reserve(kPayloadSymbols);
    result.per_symbol_confidence.reserve(kPayloadSymbols);
    float min_conf = 1e30f;

    for (uint32_t s = 0; s < kPayloadSymbols; ++s) {
        const size_t window_start =
            (static_cast<size_t>(kCostasSymbols) + s) * samples_per_symbol;
        const auto mags = toneMagnitudes(samples + window_start, samples_per_symbol);
        float conf = 0.0f;
        const uint8_t dibit = demapDibit(mags, conf);
        dibits.push_back(dibit);
        result.per_symbol_confidence.push_back(conf);
        if (conf < min_conf) min_conf = conf;
    }
    result.min_confidence = min_conf;
    result.payload = decodePayloadDibits(dibits, result.stats);
    return result;
}

// ----- detect + decode (sweep for Costas) -----

DetectAndDecodeResult ToneBurstDetector::detectAndDecode(SampleSpan samples,
                                                          size_t num_samples,
                                                          uint32_t symbol_ms,
                                                          uint32_t sweep_step_samples) {
    DetectAndDecodeResult result;
    const uint32_t samples_per_symbol = (kSampleRate * symbol_ms) / 1000u;
    const size_t total_needed =
        static_cast<size_t>(kTotalSymbols) * samples_per_symbol;
    if (num_samples < total_needed) return result;

    // Coarse sweep: collect ALL correlation values above a noise-floor
    // threshold. We can't just take the GLOBAL peak — a Costas pattern can
    // alias against any 4-tone subsequence in the payload (the payload is
    // random-ish 4-FSK dibits; some 4-tuple of dibits will inevitably look
    // like the Costas pattern {0, 1, 3, 2}). So we collect candidates and
    // verify each by CRC.
    const size_t max_offset = num_samples - total_needed;

    // Noise-floor proxy: average correlation across the sweep. Anything more
    // than ~3× the average is a candidate worth trying.
    std::vector<std::pair<size_t, float>> candidates;  // (offset, corr)
    candidates.reserve(64);
    float corr_sum = 0.0f;
    size_t corr_n = 0;
    for (size_t offset = 0; offset <= max_offset; offset += sweep_step_samples) {
        const float corr = costasCorrelationAtOffset(samples, num_samples,
                                                      offset, samples_per_symbol);
        corr_sum += corr;
        ++corr_n;
        candidates.emplace_back(offset, corr);
    }
    const float corr_avg = (corr_n > 0) ? (corr_sum / static_cast<float>(corr_n)) : 0.0f;
    const float candidate_threshold = std::max(corr_avg * 3.0f, 1.0f);

    // Sort candidates by correlation strength descending.
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    // Try the top-K candidates. K=8 covers the worst case where the global
    // peak is a payload alias and the real burst is somewhere lower in the
    // sorted list.
    const size_t kTryCandidates = 8;
    float global_peak = 0.0f;
    size_t global_offset = 0;
    DecodeAlignedResult best_decode;  // empty by default
    size_t best_decode_offset = 0;

    for (size_t k = 0; k < std::min(kTryCandidates, candidates.size()); ++k) {
        size_t offset = candidates[k].first;
        const float corr_coarse = candidates[k].second;
        if (corr_coarse < candidate_threshold && k > 0) break;
        if (corr_coarse > global_peak) {
            global_peak = corr_coarse;
            global_offset = offset;
        }

        // Fine sweep around this candidate to refine the offset.
        float best_corr = corr_coarse;
        size_t best_offset = offset;
        if (sweep_step_samples > 1) {
            const size_t lo = (offset >= sweep_step_samples)
                ? offset - sweep_step_samples : 0;
            const size_t hi = std::min(offset + sweep_step_samples, max_offset);
            for (size_t o = lo; o <= hi; ++o) {
                const float c = costasCorrelationAtOffset(samples, num_samples,
                                                          o, samples_per_symbol);
                if (c > best_corr) { best_corr = c; best_offset = o; }
            }
        }

        // Try to decode at the refined offset. First CRC pass wins.
        const size_t remaining = num_samples - best_offset;
        auto decode = decodeAligned(samples + best_offset, remaining, symbol_ms);
        if (decode.stats.crc_ok) {
            best_decode = std::move(decode);
            best_decode_offset = best_offset;
            global_peak = std::max(global_peak, best_corr);
            global_offset = best_offset;
            break;
        }
    }

    result.detected_offset_samples = best_decode.stats.crc_ok ? best_decode_offset : global_offset;
    result.costas_correlation_peak = global_peak;
    result.decode = std::move(best_decode);
    result.sync_acquired = result.decode.stats.crc_ok;
    return result;
}

}  // namespace tone_burst_ack
}  // namespace waveform
}  // namespace ultra
