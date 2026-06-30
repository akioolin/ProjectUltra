// test_mcdpsk_snr_calibration.cpp
//
// Characterizes the MC-DPSK training-SNR estimator
// (MultiCarrierDPSKDemodulator::updateTrainingSNREstimate, surfaced as
// MCDPSK_IN_BAND). It feeds an MC-DPSK CONNECT-geometry frame through the real
// StreamingEncoder -> AWGN(SimulatedChannel) -> StreamingDecoder path at a swept,
// KNOWN true SNR and reports the routed mcdpsk_in_band estimate vs the truth.
//
// WHY: the estimator computes 10*log10(signal_power/residual_power) where the
// residual comes from a TIME-DOMAIN reconstruction (fitted = 2*Re(sum_c H_c*train*
// carrier); residual = sample - fitted). Any reconstruction mismatch (timing,
// phase, the 2*Re passband form, carrier-basis non-orthogonality, in-sample LS
// fit) leaves an alpha*signal residual floor that does NOT vanish at high SNR, so
// the measured SNR SATURATES at ~1/alpha (rig: 16 dB @ true~20, 18.7 dB @ true~32).
// This harness makes that curve repeatable and is the gate for any fix: a proper
// estimator must track the true SNR (within a few dB) well past 18 dB.
//
// Report-only table today; PASS criterion lands with the residual-floor fix.

#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "ota_channel_core/channel.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace ultra;
using namespace ultra::gui;
namespace v2 = ultra::protocol::v2;

namespace {

using Bytes = std::vector<uint8_t>;

std::vector<float> withSilence(const std::vector<float>& frame) {
    std::vector<float> audio;
    audio.resize(48000, 0.0f);  // 1 s lead-in for chirp-search / noise-floor estimation
    audio.insert(audio.end(), frame.begin(), frame.end());
    audio.resize(audio.size() + 96000, 0.0f);  // trailing room for final decode
    return audio;
}

void feedInChunks(StreamingDecoder& decoder, const std::vector<float>& audio) {
    constexpr size_t kChunk = 4800;
    for (size_t pos = 0; pos < audio.size(); pos += kChunk) {
        const size_t len = std::min(kChunk, audio.size() - pos);
        decoder.feedAudio(audio.data() + pos, len);
        decoder.processBuffer();
    }
}

// CONNECT-class geometry: 8 carriers, 1024 samples/symbol (46.875 baud), DQPSK.
MultiCarrierDPSKConfig connectGeometry() {
    MultiCarrierDPSKConfig cfg = mc_dpsk_presets::level8();
    cfg.samples_per_symbol = 1024;
    return cfg;
}

// Returns the routed mcdpsk_in_band SNR estimate for a frame at true `snr_db`, or NaN.
float measureEstimatedSNR(float snr_db, uint32_t seed, const Bytes& payload,
                          ultra::ota_channel_core::ChannelType chan =
                              ultra::ota_channel_core::ChannelType::AWGN) {
    auto frame = v2::DataFrame::makeData("ALPHA", "BRAVO", 7, payload, CodeRate::R1_4);
    const Bytes serialized = frame.serialize();

    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::MC_DPSK);
    encoder.setMCDPSKConfig(connectGeometry());
    encoder.setDataMode(Modulation::DQPSK, CodeRate::R1_4);
    auto samples = encoder.encodeFrame(serialized);
    if (samples.empty()) return std::nanf("");

    auto audio = withSilence(samples);

    ultra::ota_channel_core::SimulatedChannel channel;
    channel.setSeed(seed);
    channel.configure(snr_db, chan);
    channel.transmitFromA(audio);
    audio = channel.receiveForB(audio.size());

    StreamingDecoder decoder;
    decoder.setLogPrefix("CAL");
    decoder.setMode(protocol::WaveformMode::MC_DPSK, true);  // connected -> routes MCDPSK_IN_BAND
    decoder.setMCDPSKConfig(connectGeometry());
    decoder.setDataMode(Modulation::DQPSK, CodeRate::R1_4);

    feedInChunks(decoder, audio);
    float est = std::nanf("");
    while (decoder.hasFrame()) {
        auto result = decoder.getFrame();
        if (result.snr_source == SNRSource::MCDPSK_IN_BAND &&
            std::isfinite(result.snr_db)) {
            est = result.snr_db;  // last MC-DPSK estimate for this frame
        }
    }
    return est;
}

}  // namespace

int main() {
    Bytes payload(96);
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<uint8_t>(0x40 + (i * 7) % 64);

    using CT = ultra::ota_channel_core::ChannelType;
    std::cout << "== MC-DPSK training-SNR estimator calibration: measured vs true ==\n";
    std::cout << "   AWGN proves the ESTIMATOR; GOOD (CCIR multipath, 0.1 Hz/0.5 ms)\n";
    std::cout << "   shows the FADING coherence limit over the 8-symbol (~170 ms) window.\n\n";
    std::cout << " true | AWGN  err  | GOOD  err   (mean of 5 noise seeds)\n";

    int awgn_sat = 0, awgn_hi = 0;
    for (float snr : {6.f, 9.f, 12.f, 15.f, 18.f, 21.f, 24.f, 27.f, 30.f, 35.f}) {
        auto meanFor = [&](CT ct) -> double {
            double sum = 0.0; int n = 0;
            for (uint32_t s = 0; s < 5; ++s) {
                float e = measureEstimatedSNR(snr, 0x1000u + s * 7919u, payload, ct);
                if (std::isfinite(e)) { sum += e; ++n; }
            }
            return n ? sum / n : std::nan("");
        };
        const double ma = meanFor(CT::AWGN);
        const double mg = meanFor(CT::GOOD);
        std::cout << std::setw(5) << (int)snr << " | " << std::fixed << std::setprecision(1);
        if (std::isfinite(ma)) std::cout << std::setw(4) << ma << " " << std::showpos
                                         << std::setw(5) << (ma - snr) << std::noshowpos;
        else std::cout << "  -- (no est)";
        std::cout << " | ";
        if (std::isfinite(mg)) std::cout << std::setw(4) << mg << " " << std::showpos
                                         << std::setw(5) << (mg - snr) << std::noshowpos;
        else std::cout << "  -- (no est)";
        std::cout << "\n";
        if (snr >= 18.f) { ++awgn_hi; if (std::isfinite(ma) && (ma - snr) < -3.0) ++awgn_sat; }
    }
    std::cout << "\nAWGN high-SNR (>=18) saturating (>3 dB low): " << awgn_sat << "/" << awgn_hi
              << "   <- estimator is sound iff this is 0\n";
    std::cout << "GOOD caps well below true at high SNR = the FADING coherence limit\n";
    std::cout << "(the estimator correctly reports the EFFECTIVE SNR through the fading channel)\n";

    // REGRESSION GATE: the MC-DPSK training SNR estimator must remain LEVEL-/SNR-
    // accurate in AWGN (it backs the connect-time ratiometric rate decision). It must
    // NOT saturate at high SNR in AWGN -- any saturation there would be a real estimator
    // defect (vs the GOOD column, where the cap is the physical fading coherence limit,
    // which is correct and intentionally NOT gated).
    if (awgn_sat > 0) {
        std::cout << "\nFAIL: AWGN estimate saturated at high SNR (" << awgn_sat
                  << " point(s)) -- estimator regression.\n";
        return 1;
    }
    std::cout << "\nPASS: AWGN estimate tracks true SNR (no estimator saturation).\n";
    return 0;
}
