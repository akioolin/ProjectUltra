// test_ofdm_snr_calibration.cpp
//
// Calibrates the OFDM in-band SNR estimator against KNOWN injected truth —
// the OFDM analog of test_mcdpsk_snr_calibration.cpp. The value under test is
// SNRSource::OFDM_BROADBAND: the OFDMDemodulator's LTS/pilot-residual
// last_snr_db_estimate, surfaced through
// StreamingDecoder::getLastOFDMBroadbandSNREstimate() and routed as the
// consumer-facing snr_db on connected-OFDM decodes
// (streaming_sync_acquisition.cpp populateDecodeMetrics). This is a PHYSICAL
// SNR source (with IDLE_IN_BAND, the only ones that feed rate selection), so
// its calibration against truth is load-bearing for the auto rate ladder.
//
// Harness: real StreamingEncoder -> SimulatedChannel(AWGN) -> StreamingDecoder,
// exactly like the MC-DPSK calibration test. The channel's AWGN is sized
// against the modem in-band reference (encodePing in-band RMS 0.3048 through
// the RX FIR — modemReferenceNoiseStddev), so the dial value IS the true
// receiver in-band SNR (3 kHz convention). Frames are QPSK R1/4 fixed 4-CW
// DATA frames with the full chirp+LTS anchor, decoded on the connected-OFDM
// path (the production geometry measure_ack_fer drives).
//
// KNOWN-TRUTH INJECTION (the load-bearing harness decisions — two of them):
//
// 1. The channel's dial is the in-band SNR of a signal sitting AT
//    kModemReferenceInBandRms (the encodePing chirp reference); the AWGN
//    stddev derives from that reference alone. For the dial to BE the truth
//    for the span the estimator measures (the LTS/data section), that section
//    must sit at the reference. The peak-normalized ~10 dB-PAPR OFDM section
//    naturally rides ~3.2 dB below it (measured 0.210 vs 0.3048), so the
//    harness scales the TX frame by ref/section_rms — both constants are the
//    channel's own calibration values, no fudge. The truth must be injected
//    on the SIGNAL side, never by moving the dial: the pre-fix estimator was
//    level-blind (noise-anchored), so a noise-side truth correction would
//    shift its reading out of any tolerance while a signal-side one cannot.
//    (Scaled chirp peaks reach ~1.1 float — fine in this all-float path; a
//    real DAC would clip, but this is a calibration injection, not a TX
//    recommendation.)
//
// 2. SimulatedChannel models the production TX chain's per-burst in-band
//    normalization (normalizeTxBurstToReference) — a TX level POLICY that
//    re-normalizes the whole burst (chirp duty included) and would silently
//    undo (1), pinning the delivered section ~2.7 dB below reference no
//    matter what the harness injects. The harness disables it
//    (setTxBurstNormalizationEnabled(false)) to keep level POLICY out of an
//    ESTIMATOR calibration. That -2.7 dB burst-average-vs-data-section duty
//    split is a real, separate TX-chain fact — it belongs to level-policy
//    tests, not to this estimator gate. (The MC-DPSK sibling test tolerates
//    the policy because its near-constant-envelope frame has almost no
//    chirp/data level split; OFDM's is ~3 dB and must be controlled.)
//
// The estimator itself is ratiometric (level-invariant post-fix), so neither
// knob can mask a genuine calibration defect.
//
// GATE: |mean(reading) - truth| <= tolerance at every swept point, with the
// tolerance env-overridable: ULTRA_OFDM_SNR_CAL_TOL_DB (default 1.5 dB —
// tightened 2026-07-08 after the estimator recalibration; post-fix bias is
// -0.4..-0.2 dB across 6-20 dB).
// WHY 4.0 TODAY: the estimator carried a known +2.5..+3.2 dB optimistic bias
// (level-blind reference-power crediting + un-debiased LS signal power —
// root-caused and fixed in the parallel 2026-07-07 calibration change in
// channel_equalizer_lts.cpp); 4.0 dB passes both before that fix (bias
// measured +2.5..+3.4 in this harness against the pre-fix estimator, +2.9 at
// 10 dB) and after it (measured -0.2..-0.4). Follow-up tightens to 1.5 dB
// once the fix is committed. The printed per-point table (truth, mean, bias)
// is the calibration record.

#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "ota_channel_core/channel.hpp"
#include "ota_channel_core/models.hpp"  // kModemReferenceInBandRms (channel calibration)
#include "protocol/frame_v2.hpp"
#include "ultra/logging.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ultra/types.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <cstdint>
#include <cstdlib>
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

// Production wideband OFDM geometry (mirrors measure_ack_fer::makeOFDMConfig):
// FFT 1024, 59 carriers, LONG CP, pilots at the production adaptive spacing.
ModemConfig ofdmGeometry(Modulation mod, CodeRate rate) {
    ModemConfig cfg;
    cfg.fft_size = 1024;
    cfg.num_carriers = 59;
    cfg.sample_rate = 48000;
    cfg.center_freq = 1500.0f;
    cfg.cp_mode = CyclicPrefixMode::LONG;
    cfg.modulation = mod;
    cfg.code_rate = rate;
    cfg.use_pilots = true;
    cfg.pilot_spacing = ofdm_link_adaptation::recommendedPilotSpacing(mod, rate);
    return cfg;
}

// One trial's routed OFDM_BROADBAND reading (NaN if the decoder never produced
// one) plus whether the frame decoded CRC-clean.
struct MeasuredSNR {
    float reading = std::nanf("");
    bool decoded = false;
};

double rmsOf(const std::vector<float>& x) {
    double sum = 0.0;
    for (float v : x) sum += static_cast<double>(v) * v;
    return x.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(x.size()));
}

// In-band RMS of the LTS+data section at the encoder's natural (peak-
// normalized) level, measured on a light-preamble encode of the SAME frame:
// the light frame IS the LTS+data span (no chirp), uniform-power, and fully
// inside the 50-2950 Hz band (59 x 46.875 Hz carriers on 1500 Hz => 117-2883
// Hz), so its plain RMS is its in-band RMS to <0.1 dB.
double ltsDataSectionRms(const Bytes& serialized, Modulation mod, CodeRate rate) {
    StreamingEncoder probe;
    probe.setMode(protocol::WaveformMode::OFDM_CHIRP);
    probe.setOFDMConfig(ofdmGeometry(mod, rate));
    probe.setDataMode(mod, rate);
    return rmsOf(probe.encodeFrameLight(serialized));
}

MeasuredSNR measureEstimatedSNR(float snr_db, uint32_t seed, const Bytes& payload,
                              bool fading = false,
                              bool colored = false) {
    constexpr Modulation kMod = Modulation::QPSK;
    constexpr CodeRate kRate = CodeRate::R1_4;
    MeasuredSNR out;

    auto frame = v2::makeFixedDataFrame("ALPHA", "BRAVO", 7, payload, kRate);
    const Bytes serialized = frame.serialize();

    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(ofdmGeometry(kMod, kRate));
    encoder.setDataMode(kMod, kRate);
    auto samples = encoder.encodeFrame(serialized);  // full chirp+LTS anchor
    if (samples.empty()) return out;

    // KNOWN-truth injection (see file header): put the LTS/data section AT the
    // channel's calibrated reference so the dial IS the injected in-band SNR.
    const double section_rms = ltsDataSectionRms(serialized, kMod, kRate);
    if (section_rms <= 0.0) return out;
    const float scale = static_cast<float>(
        ultra::ota_channel_core::kModemReferenceInBandRms / section_rms);
    for (float& v : samples) v *= scale;

    auto audio = withSilence(samples);

    if (colored) {
        // COLORED-NOISE section (external review 2026-07-10, finding 1): real
        // SSB receive chains band-limit the noise (~50-2950 Hz); the guard
        // bins above the occupied band then read the FILTER SKIRT, not the
        // in-band noise — a guard-preferred meter over-reads by the whole
        // stopband depth (measured 20.9 dB on a checked-in HF capture). White
        // AWGN can never catch this; this section can: noise shaped by the
        // same 101-tap 50-2950 bandpass the receive convention uses, sized so
        // the IN-BAND S:N equals truth, PASSTHROUGH channel (no added noise).
        ultra::ota_channel_core::SimulatedChannel channel;
        channel.setSeed(seed);
        channel.configure(0.0f, ultra::ota_channel_core::ChannelType::PASSTHROUGH);
        channel.setTxBurstNormalizationEnabled(false);
        std::mt19937 rng(seed ^ 0xC01Du);
        std::normal_distribution<float> gauss(0.0f, 1.0f);
        FIRFilter shape = FIRFilter::bandpass(101, 50.0f, 2950.0f, 48000.0f);
        std::vector<float> noise(audio.size());
        double nss = 0.0;
        for (size_t i = 0; i < noise.size(); ++i) {
            noise[i] = shape.process(gauss(rng));
            nss += static_cast<double>(noise[i]) * noise[i];
        }
        const double n_rms = std::sqrt(nss / noise.size());
        const double target_rms =
            ultra::ota_channel_core::kModemReferenceInBandRms /
            std::pow(10.0, snr_db / 20.0);
        const float k = static_cast<float>(target_rms / std::max(n_rms, 1e-12));
        for (size_t i = 0; i < audio.size(); ++i) audio[i] += noise[i] * k;
        channel.transmitFromA(audio);
        audio = channel.receiveForB(audio.size());
    } else if (fading) {
        // FADING section (Stage-2 contract): ITU-R F.1487 Good (0.1 Hz/0.5 ms
        // Watterson, unit mean tap power) at the same noise convention — the
        // per-seed reading samples ONE fade state; the ENSEMBLE across seeds
        // is the fade average (ergodicity), and this harness records readings
        // even for frames that fail to decode, so unlike the live decoded-
        // frames ring it has NO survivor bias.
        ultra::ota_channel_core::WattersonChannelModel model(
            ultra::ota_channel_core::itu_r_f1487::good(snr_db), seed);
        std::vector<float> faded;
        model.process(audio, 0, faded);
        audio = std::move(faded);
    } else {
        ultra::ota_channel_core::SimulatedChannel channel;
        channel.setSeed(seed);
        channel.configure(snr_db, ultra::ota_channel_core::ChannelType::AWGN);
        // Keep the channel's per-burst TX level policy out of the calibration
        // (it would re-normalize the burst and undo the known-truth injection
        // above — see file header, decision 2).
        channel.setTxBurstNormalizationEnabled(false);
        channel.transmitFromA(audio);
        audio = channel.receiveForB(audio.size());
    }

    StreamingDecoder decoder;
    decoder.setLogPrefix("CAL");
    // 4-CW DATA frames are a connected-mode construct; connected entry arms the
    // full chirp+LTS anchor expectation (see measure_ack_fer configureDecoder).
    decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                 ofdmGeometry(kMod, kRate), kMod, kRate);

    feedInChunks(decoder, audio);
    while (decoder.hasFrame()) {
        auto result = decoder.getFrame();
        if (result.is_ping) continue;
        if (result.success) out.decoded = true;
        if (result.has_ofdm_broadband_snr_db &&
            std::isfinite(result.ofdm_broadband_snr_db)) {
            out.reading = result.ofdm_broadband_snr_db;
        }
    }
    // The named surface under test: the decoder-level routed value. Also covers
    // decode attempts whose DecodeResult never reached the frame queue (below
    // the LDPC floor the LTS reading still lands — and rate selection consumes
    // it there too).
    if (!std::isfinite(out.reading) && decoder.hasLastOFDMBroadbandSNREstimate()) {
        out.reading = decoder.getLastOFDMBroadbandSNREstimate();
    }
    return out;
}

float toleranceDb() {
    // Default 4.0 dB: absorbs the known pre-fix estimator bias (measured
    // +2.5..+3.4 here) so this gate passes before AND immediately after the
    // parallel calibration change. Tighten to 1.5 dB once that fix lands
    // (post-fix measured bias: -0.2..-0.4 dB).
    if (const char* env = std::getenv("ULTRA_OFDM_SNR_CAL_TOL_DB")) {
        char* end = nullptr;
        const float v = std::strtof(env, &end);
        if (end != env && std::isfinite(v) && v > 0.0f) return v;
    }
    return 1.5f;
}

}  // namespace

int main() {
    setLogLevel(LogLevel::ERROR);  // keep the calibration table readable

    Bytes payload(32);
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<uint8_t>(0x40 + (i * 7) % 64);

    constexpr uint32_t kSeeds = 5;  // >=3 required; 5 matches the MC-DPSK harness
    const float tol = toleranceDb();

    std::cout << "== OFDM in-band SNR estimator calibration (OFDM_BROADBAND): "
                 "measured vs true ==\n";
    std::cout << "   QPSK R1/4 4-CW full-anchor frames; LTS/data section normalized to the\n";
    std::cout << "   channel's in-band reference so dial == injected truth (3 kHz convention).\n";
    std::cout << "   tolerance +/-" << std::fixed << std::setprecision(1) << tol
              << " dB (env ULTRA_OFDM_SNR_CAL_TOL_DB)\n\n";
    std::cout << " true | mean  bias  | n/est n/dec (of " << kSeeds << " seeds)\n";

    int failed_points = 0;
    for (float snr : {6.f, 10.f, 15.f, 20.f}) {
        double sum = 0.0;
        int n_est = 0, n_dec = 0;
        for (uint32_t s = 0; s < kSeeds; ++s) {
            const auto m = measureEstimatedSNR(snr, 0x3000u + s * 7919u, payload);
            if (std::isfinite(m.reading)) { sum += m.reading; ++n_est; }
            if (m.decoded) ++n_dec;
        }
        const double mean = n_est ? sum / n_est : std::nan("");
        std::cout << std::setw(5) << (int)snr << " | " << std::fixed << std::setprecision(1);
        if (std::isfinite(mean)) {
            std::cout << std::setw(4) << mean << " " << std::showpos << std::setw(5)
                      << (mean - snr) << std::noshowpos;
        } else {
            std::cout << "  -- (no est)";
        }
        std::cout << " |   " << n_est << "     " << n_dec;

        // GATE: need enough readings for a meaningful mean, and the mean must
        // sit within tolerance of the injected truth.
        bool point_ok = (n_est >= 3) && std::isfinite(mean) &&
                        std::abs(mean - snr) <= tol;
        if (!point_ok) {
            ++failed_points;
            std::cout << "   <- FAIL ("
                      << (n_est < 3 ? "insufficient readings" : "bias out of tolerance")
                      << ")";
        }
        std::cout << "\n";
    }

    // ═══ COLORED-NOISE contract (real-radio noise shape; finding 1) ═══
    std::cout << "\n colored (band-limited) | mean  bias | n\n";
    for (float snr : {10.f, 20.f}) {
        double sum = 0.0; int n = 0;
        for (uint32_t s3 = 0; s3 < 3; ++s3) {
            const auto m = measureEstimatedSNR(snr, 0x9100u + s3 * 6151u, payload,
                                               /*fading=*/false, /*colored=*/true);
            if (std::isfinite(m.reading)) { sum += m.reading; ++n; }
        }
        const double mean = n ? sum / n : std::nan("");
        std::cout << std::setw(11) << (int)snr << "            | " << std::fixed
                  << std::setprecision(1) << std::setw(4) << mean << " "
                  << std::showpos << std::setw(5) << (mean - snr)
                  << std::noshowpos << " | " << n;
        const bool ok = n >= 3 && std::isfinite(mean) && std::abs(mean - snr) <= tol;
        if (!ok) { ++failed_points; std::cout << "   <- FAIL (colored noise)"; }
        std::cout << "\n";
    }

    // ═══ FADING contract (Stage-2: what the RX-authority consumes) ═══
    // Per-frame readings on ITU Good fading sample single fade states; the
    // ENSEMBLE linear-mean must equal the dial (mean-power definition) and the
    // dB-mean must sit BELOW the linear-mean (Jensen). Tolerance 3.0 dB on the
    // ensemble mean: 16 one-frame fade samples, per-sample dB std ~4 ->
    // std(mean) ~1.1 dB, so 3.0 is a ~2.7-sigma gate (stable across seeds).
    std::cout << "\n fading (ITU Good) | lin-mean  bias | dB-mean | n\n";
    for (float snr : {10.f, 20.f}) {
        double lin_sum = 0.0, db_sum = 0.0;
        int n = 0;
        for (uint32_t s2 = 0; s2 < 16; ++s2) {
            const auto m = measureEstimatedSNR(snr, 0x7000u + s2 * 104729u,
                                               payload, /*fading=*/true);
            if (std::isfinite(m.reading)) {
                lin_sum += std::pow(10.0, m.reading / 10.0);
                db_sum += m.reading;
                ++n;
            }
        }
        const double lin_mean_db = n ? 10.0 * std::log10(lin_sum / n) : std::nan("");
        const double db_mean = n ? db_sum / n : std::nan("");
        std::cout << std::setw(10) << (int)snr << "         | " << std::fixed
                  << std::setprecision(1) << std::setw(7) << lin_mean_db << " "
                  << std::showpos << std::setw(5) << (lin_mean_db - snr)
                  << std::noshowpos << " | " << std::setw(6) << db_mean
                  << " | " << n;
        const bool ok = n >= 10 && std::isfinite(lin_mean_db) &&
                        std::abs(lin_mean_db - snr) <= 3.0 &&
                        db_mean <= lin_mean_db + 0.3;
        if (!ok) { ++failed_points; std::cout << "   <- FAIL (fading ensemble)"; }
        std::cout << "\n";
    }

    if (failed_points > 0) {
        std::cout << "\nFAIL: " << failed_points << " point(s) outside +/-" << std::fixed
                  << std::setprecision(1) << tol
                  << " dB (or with <3 readings) -- OFDM_BROADBAND estimator "
                     "calibration regression.\n";
        return 1;
    }
    std::cout << "\nPASS: OFDM_BROADBAND estimate within +/-" << std::fixed
              << std::setprecision(1) << tol
              << " dB of true in-band SNR at every swept point (AWGN).\n";
    return 0;
}
