// Measure the tone-burst ACK staircase edges ON THE HONEST SNR SCALE.
//
// WHY THIS EXISTS
// ---------------
// src/gui/app.cpp adds connection_policy::kOfdmLegacyAnchorScaleOffsetDb (+8.70 dB) to
// OFDM-sourced SNR before consulting symbolMsForSNR(). That offset is a COMPATIBILITY SHIM,
// not a physical quantity: the OFDM SNR estimator used to read ~8.7 dB high (a chirp-power
// conversion error, LS |h|^2 inflation, a guard-bin under-read and fading optimism), those
// bugs were fixed at source on 2026-07-07, and the staircase EDGES were left tuned to the
// old, inflated scale. Adding the bias back makes the old edges line up again.
//
// Folding +8.70 into the edge constants would be arithmetically identical and therefore
// NOT A FIX — it would bury the number in four magic thresholds where it is LESS visible
// than the named constant is today. The only honest way to delete it is to MEASURE what the
// edges actually are on the honest scale and replace the table.
//
// This tool measures exactly that.
//
// METHOD
// ------
// For each ACK symbol duration, sweep in-band SNR and measure the decode success rate of a
// real encode -> channel -> detect+decode round trip (the same path production uses,
// including the timing search: real ACK arrival time is always uncertain). The EDGE for a
// duration is the lowest SNR at which it clears the target success rate.
//
// symbolMsForSNR() should then return the SHORTEST duration whose measured edge is <= the
// reading. That table needs no offset, because it is measured in the same units the
// estimator now reports.
//
// The SNR here is the channel model's own snr_db, i.e. the generator's setting — an EXTERNAL
// reference, not anything the modem estimates. That is what makes the result honest and what
// makes it immune to whatever the estimator's calibration happens to be.
//
// Fading and AWGN are measured SEPARATELY because symbolMsForSNR already takes a
// fading_present flag and the two edges genuinely differ (a deep null inside a short symbol
// is unrecoverable; a longer symbol averages over it).
//
// Build:  cmake --build build --target measure_ack_staircase
// Run:    ./build/measure_ack_staircase [trials_per_cell]

#include "ota_channel_core/models.hpp"
#include "waveform/tone_burst_ack/tone_burst_constants.hpp"
#include "waveform/tone_burst_ack/tone_burst_encoder.hpp"
#include "waveform/tone_burst_ack/tone_burst_detector.hpp"

#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

using namespace ultra;
using namespace ultra::waveform::tone_burst_ack;
using ultra::ota_channel_core::WattersonChannel;

namespace {

constexpr uint32_t kFs = 48000;

// Target success rate defining an edge. An ACK is protected by the sender's ARQ, but a lost
// ACK costs a full retransmission cycle (~9.5 s), so the bar is high. 0.95 mirrors the
// reliability the existing edges were chosen for.
constexpr double kEdgeTargetSuccess = 0.95;

ToneBurstAckPayload samplePayload(int t) {
    ToneBurstAckPayload p{};
    p.group_seq = static_cast<uint8_t>(t & 0x3F);
    p.frame_mask = static_cast<uint16_t>(0xA5A5u ^ static_cast<uint16_t>(t));
    p.type = AckType::Ack;
    return p;
}

bool payloadsEqual(const ToneBurstAckPayload& a, const ToneBurstAckPayload& b) {
    return a.group_seq == b.group_seq && a.frame_mask == b.frame_mask && a.type == b.type;
}

// One (SNR, symbol_ms) cell: fraction of trials whose payload decoded correctly.
double runCell(const WattersonChannel::Config& cfg, uint32_t symbol_ms, int trials,
               uint64_t base_seed) {
    ToneBurstEncoder enc;
    ToneBurstDetector det;
    int ok = 0;
    for (int t = 0; t < trials; ++t) {
        WattersonChannel ch(cfg, base_seed + static_cast<uint64_t>(t));
        // Warm-up so the fading oscillators start "live" rather than at their constant-1
        // initial transient, which would flatter short symbols.
        std::vector<float> warm_in(static_cast<size_t>(kFs) * 50 / 1000, 0.0f);
        std::vector<float> warm_out;
        ch.process(std::span<const float>(warm_in), warm_out);

        enc.resetPhase();
        const auto orig = samplePayload(t);
        const auto burst = enc.encode(orig, symbol_ms);

        const uint32_t pad = (kFs * 100u) / 1000u;
        std::vector<float> in;
        in.reserve(2 * pad + burst.size());
        in.insert(in.end(), pad, 0.0f);
        in.insert(in.end(), burst.begin(), burst.end());
        in.insert(in.end(), pad, 0.0f);

        std::vector<float> noisy;
        ch.process(std::span<const float>(in), noisy);

        const auto dr = det.detectAndDecode(noisy.data(), noisy.size(), symbol_ms);
        if (dr.decode.payload.has_value() && payloadsEqual(*dr.decode.payload, orig)) ++ok;
    }
    return static_cast<double>(ok) / static_cast<double>(trials);
}

struct Sweep {
    const char* label;
    bool fading;
    float doppler_hz;
    float delay_ms;
};

}  // namespace

int main(int argc, char** argv) {
    const int trials = (argc > 1) ? std::atoi(argv[1]) : 24;
    const uint32_t durations[] = {kSymbolMsHighSNR, kSymbolMsMidSNR, kSymbolMsLowSNR,
                                  kSymbolMsMargSNR, kSymbolMsWeakSNR};
    // ITU-R F.1487 "Good" is the class the staircase's fading edge is for.
    const Sweep sweeps[] = {
        {"AWGN  (fading_present=false)", false, 0.0f, 0.0f},
        {"GOOD  (fading_present=true)", true, 0.1f, 0.5f},
    };

    std::printf("Tone-burst ACK staircase — MEASURED on the honest in-band SNR scale\n");
    std::printf("trials/cell=%d  target success=%.0f%%\n", trials, kEdgeTargetSuccess * 100);
    std::printf("(SNR is the CHANNEL GENERATOR's setting — an external reference, not a\n"
                " modem estimate, so this result cannot inherit an estimator's calibration)\n\n");

    for (const auto& sw : sweeps) {
        std::printf("=== %s ===\n", sw.label);
        std::printf("%8s", "SNR dB");
        for (uint32_t d : durations) std::printf("%9u", d);
        std::printf("   <- symbol_ms\n");

        // Track, per duration, the lowest SNR meeting the target.
        double edge[5];
        for (int i = 0; i < 5; ++i) edge[i] = 1e9;

        for (float snr = -10.0f; snr <= 26.0f; snr += 2.0f) {
            WattersonChannel::Config cfg;
            cfg.snr_db = snr;
            cfg.sample_rate = kFs;
            cfg.fading_enabled = sw.fading;
            cfg.multipath_enabled = sw.fading;
            cfg.doppler_spread_hz = sw.doppler_hz;
            cfg.delay_spread_ms = sw.delay_ms;
            cfg.noise_enabled = true;
            cfg.cfo_enabled = false;

            std::printf("%8.0f", snr);
            for (int i = 0; i < 5; ++i) {
                const double p = runCell(cfg, durations[i], trials, 0xACE0u + i * 977u);
                std::printf("%8.0f%%", p * 100.0);
                if (p >= kEdgeTargetSuccess && snr < edge[i]) edge[i] = snr;
            }
            std::printf("\n");
            std::fflush(stdout);
        }

        std::printf("\n  MEASURED EDGES (lowest SNR clearing %.0f%%):\n", kEdgeTargetSuccess * 100);
        for (int i = 0; i < 5; ++i) {
            if (edge[i] > 1e8) std::printf("    %3u ms : never reached in the swept range\n", durations[i]);
            else               std::printf("    %3u ms : %+.0f dB\n", durations[i], edge[i]);
        }
        std::printf("\n  => symbolMsForSNR should return the SHORTEST duration whose edge <= snr.\n");
        std::printf("     Current hard-coded edges: fast=%.1f/%.1f, then 12.0, 5.0, -5.0\n\n",
                    kFastAckEdgeAwgnDb, kFastAckEdgeFadingDb);
    }
    return 0;
}
