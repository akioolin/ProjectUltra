#include "waveform/ofdm_chirp_waveform.hpp"
#include "sync/signal_policy.hpp"
#include "protocol/frame_v2.hpp"
#include "sim/awgn.hpp"
#include "ultra/ofdm_link_adaptation.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace ultra;
namespace signal_policy = ultra::sync::signal_policy;

namespace {

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg) \
    do { \
        ++tests_run; \
        if (!(cond)) { \
            ++tests_failed; \
            std::cout << "FAIL: " << msg << "\n"; \
            return false; \
        } \
    } while (0)

Bytes makeEncodedCodeword() {
    Bytes encoded(81);
    for (size_t i = 0; i < encoded.size(); ++i) {
        encoded[i] = static_cast<uint8_t>((i * 37u + 0x5Au) & 0xFFu);
    }
    return encoded;
}

void append(Samples& dst, const Samples& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

void appendTailMargin(Samples& dst, int samples) {
    dst.insert(dst.end(), static_cast<size_t>(samples), 0.0f);
}

void addAwgn(Samples& samples, float snr_db, uint32_t seed) {
    std::mt19937 rng(seed);
    sim::awgn::addAWGN(samples, snr_db, rng);
}

bool processFromSync(IWaveform& waveform, const Samples& audio, const SyncResult& sync,
                     const std::string& label) {
    CHECK(sync.start_sample >= 0, label + ": sync start should be non-negative");
    CHECK(static_cast<size_t>(sync.start_sample) < audio.size(),
          label + ": sync start should be inside audio buffer");

    waveform.setFrequencyOffset(sync.cfo_hz);
    waveform.setAbsoluteTrainingPosition(static_cast<size_t>(sync.start_sample));

    SampleSpan frame_samples(audio.data() + sync.start_sample,
                             audio.size() - static_cast<size_t>(sync.start_sample));
    CHECK(waveform.process(frame_samples), label + ": waveform should process clean frame");
    CHECK(waveform.isSynced(), label + ": waveform should report synced after processing");

    auto soft_bits = waveform.getSoftBits();
    CHECK(soft_bits.size() >= 648, label + ": clean frame should yield at least one LDPC codeword");

    bool has_finite_llr = false;
    for (float llr : soft_bits) {
        if (std::isfinite(llr) && std::abs(llr) > 1e-4f) {
            has_finite_llr = true;
            break;
        }
    }
    CHECK(has_finite_llr, label + ": demodulated LLRs should contain finite non-zero values");
    CHECK(waveform.estimatedSNR() > -20.0f, label + ": SNR estimate should be sane");

    waveform.reset();
    return true;
}

bool test_ofdm_chirp_full_preamble_loopback() {
    ModemConfig cfg;
    cfg.sample_rate = 48000;
    cfg.fft_size = 512;
    cfg.num_carriers = 30;
    cfg.cp_mode = CyclicPrefixMode::LONG;
    cfg.modulation = Modulation::QPSK;
    cfg.code_rate = CodeRate::R1_2;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 10;

    OFDMChirpWaveform waveform(cfg);
    waveform.configure(Modulation::QPSK, CodeRate::R1_2);

    Samples audio;
    append(audio, waveform.generatePreamble());
    append(audio, waveform.modulate(makeEncodedCodeword()));
    appendTailMargin(audio, waveform.getSamplesPerSymbol());

    SyncResult sync;
    CHECK(waveform.detectSync(SampleSpan(audio), sync, 0.15f),
          "OFDM-CHIRP: clean chirp preamble should be detected");
    CHECK(sync.detected, "OFDM-CHIRP: sync result should be marked detected");
    CHECK(sync.has_training, "OFDM-CHIRP: sync should expose training");

    return processFromSync(waveform, audio, sync, "OFDM-CHIRP full");
}

bool test_ofdm_chirp_prefers_complete_descriptor_before_stronger_future_anchor() {
    ModemConfig cfg;
    cfg.sample_rate = 48000;
    cfg.fft_size = 1024;
    cfg.num_carriers = 59;
    cfg.cp_mode = CyclicPrefixMode::MEDIUM;
    cfg.modulation = Modulation::QPSK;
    cfg.code_rate = CodeRate::R1_4;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 5;

    OFDMChirpWaveform tx(cfg);
    tx.configure(Modulation::QPSK, CodeRate::R1_4);
    const Samples preamble = tx.generatePreamble();

    // Exact production geometry from the QPSK-Z81 repro: the descriptor's
    // complete anchor starts after one 100 ms feed quantum, while the DATA
    // group's stronger anchor starts 67,680 samples later.  A 120k detector
    // window contains the later UP chirp and most of its DOWN chirp, but not a
    // usable training start.  The old strongest-global detector selected that
    // later UP, reported no complete pair, and the cursor advanced past the
    // descriptor.
    constexpr size_t kSearchWindow = 120000;
    constexpr size_t kDescriptorAnchorStart = 4800;
    constexpr size_t kDataAnchorStart = 72480;
    Samples audio(kSearchWindow, 0.0f);

    std::mt19937 rng(0x20260802u);
    std::normal_distribution<float> noise(0.0f, 0.02f);
    for (float& sample : audio) sample = noise(rng);

    // Make the complete descriptor deliberately weaker than the later anchor.
    // Correlation is normalized, so the common noise floor plus this amplitude
    // ratio is what creates the deterministic peak ordering.
    for (size_t i = 0; i < preamble.size() && kDescriptorAnchorStart + i < audio.size(); ++i) {
        audio[kDescriptorAnchorStart + i] += 0.25f * preamble[i];
    }
    for (size_t i = 0; i < preamble.size() && kDataAnchorStart + i < audio.size(); ++i) {
        audio[kDataAnchorStart + i] += preamble[i];
    }

    OFDMChirpWaveform rx(cfg);
    rx.configure(Modulation::QPSK, CodeRate::R1_4);
    SyncResult sync;
    CHECK(rx.detectSync(SampleSpan(audio), sync, 0.15f),
          "OFDM-CHIRP chronology: complete descriptor must beat stronger incomplete future anchor");
    CHECK(std::abs(sync.preamble_start_sample -
                   static_cast<int>(kDescriptorAnchorStart)) <= 32,
          "OFDM-CHIRP chronology: detector must select the earlier descriptor anchor");
    const int expected_training = static_cast<int>(
        kDescriptorAnchorStart + preamble.size() -
        2 * static_cast<size_t>(rx.getSamplesPerSymbol()));
    CHECK(std::abs(sync.start_sample - expected_training) <= 32,
          "OFDM-CHIRP chronology: selected descriptor training must already be readable");
    return true;
}

bool test_ofdm_chirp_keeps_strongest_anchor_when_its_training_is_readable() {
    ModemConfig cfg;
    cfg.sample_rate = 48000;
    cfg.fft_size = 1024;
    cfg.num_carriers = 59;
    cfg.cp_mode = CyclicPrefixMode::MEDIUM;
    cfg.modulation = Modulation::QPSK;
    cfg.code_rate = CodeRate::R1_4;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 5;

    OFDMChirpWaveform tx(cfg);
    tx.configure(Modulation::QPSK, CodeRate::R1_4);
    const Samples preamble = tx.generatePreamble();

    constexpr size_t kSearchWindow = 120000;
    constexpr size_t kMarginalEarlierStart = 1000;
    constexpr size_t kStrongReadableStart = 60000;
    Samples audio(kSearchWindow, 0.0f);
    std::mt19937 rng(0x20260803u);
    std::normal_distribution<float> noise(0.0f, 0.02f);
    for (float& sample : audio) sample = noise(rng);
    for (size_t i = 0; i < preamble.size() && kMarginalEarlierStart + i < audio.size(); ++i) {
        audio[kMarginalEarlierStart + i] += 0.20f * preamble[i];
    }
    for (size_t i = 0; i < preamble.size() && kStrongReadableStart + i < audio.size(); ++i) {
        audio[kStrongReadableStart + i] += preamble[i];
    }

    OFDMChirpWaveform rx(cfg);
    rx.configure(Modulation::QPSK, CodeRate::R1_4);
    SyncResult sync;
    CHECK(rx.detectSync(SampleSpan(audio), sync, 0.15f),
          "OFDM-CHIRP usable peak: strong complete anchor should detect");
    CHECK(std::abs(sync.preamble_start_sample -
                   static_cast<int>(kStrongReadableStart)) <= 32,
          "OFDM-CHIRP usable peak: marginal earlier pair must not outrank a readable strong lock");
    return true;
}

bool test_ofdm_chirp_data_preamble_loopback() {
    ModemConfig cfg;
    cfg.sample_rate = 48000;
    cfg.fft_size = 512;
    cfg.num_carriers = 30;
    cfg.cp_mode = CyclicPrefixMode::LONG;
    cfg.modulation = Modulation::QPSK;
    cfg.code_rate = CodeRate::R1_2;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 10;

    OFDMChirpWaveform waveform(cfg);
    waveform.configure(Modulation::QPSK, CodeRate::R1_2);

    Samples audio;
    append(audio, waveform.generateDataPreamble());
    append(audio, waveform.modulate(makeEncodedCodeword()));
    appendTailMargin(audio, waveform.getSamplesPerSymbol());

    SyncResult sync;
    CHECK(waveform.detectDataSync(SampleSpan(audio), sync, 0.0f, 0.30f),
          "OFDM-CHIRP: clean data preamble should be detected");
    CHECK(sync.detected, "OFDM-CHIRP data: sync result should be marked detected");
    CHECK(sync.has_training, "OFDM-CHIRP data: sync should expose training");
    CHECK(!waveform.wasBurstInterleaved(),
          "OFDM-CHIRP data: normal LTS should not latch burst-interleave marker");

    return processFromSync(waveform, audio, sync, "OFDM-CHIRP data");
}

bool test_ofdm_chirp_data_preamble_awgn_warm_sync() {
    ModemConfig cfg;
    cfg.sample_rate = 48000;
    cfg.fft_size = 1024;
    cfg.num_carriers = 59;
    cfg.cp_mode = CyclicPrefixMode::LONG;
    cfg.modulation = Modulation::QPSK;
    cfg.code_rate = CodeRate::R1_4;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 10;

    OFDMChirpWaveform tx(cfg);
    tx.configure(Modulation::QPSK, CodeRate::R1_4);
    tx.setTxFrequencyOffset(4.0f);

    Samples audio;
    append(audio, tx.generateDataPreamble());
    append(audio, tx.modulate(makeEncodedCodeword()));
    appendTailMargin(audio, tx.getSamplesPerSymbol());
    addAwgn(audio, 10.0f, 0x20260525u);

    OFDMChirpWaveform rx(cfg);
    rx.configure(Modulation::QPSK, CodeRate::R1_4);

    constexpr size_t wide_window_samples = 9600;
    constexpr size_t narrow_candidate_span = 2176;
    const auto thresholds = signal_policy::lightSyncThresholds(
        false, false, true, 0, true, wide_window_samples, narrow_candidate_span);

    SyncResult sync;
    CHECK(rx.detectDataSync(SampleSpan(audio), sync, 4.0f, thresholds.min_confidence),
          "OFDM-CHIRP warm data: SNR10 LTS should sync with known-CFO matched filter");
    CHECK(sync.detected, "OFDM-CHIRP warm data: sync result should be marked detected");
    CHECK(sync.correlation >= thresholds.min_confidence,
          "OFDM-CHIRP warm data: correlation should clear warm threshold");
    CHECK(std::abs(sync.cfo_hz - 4.0f) < 0.001f,
          "OFDM-CHIRP warm data: known CFO should be preserved");
    return true;
}

bool test_ofdm_chirp_data_preamble_noise_false_sync_rate() {
    ModemConfig cfg;
    cfg.sample_rate = 48000;
    cfg.fft_size = 512;
    cfg.num_carriers = 30;
    cfg.cp_mode = CyclicPrefixMode::LONG;
    cfg.modulation = Modulation::QPSK;
    cfg.code_rate = CodeRate::R1_4;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 10;

    OFDMChirpWaveform waveform(cfg);
    waveform.configure(Modulation::QPSK, CodeRate::R1_4);

    constexpr size_t wide_window_samples = 9600;
    constexpr size_t narrow_candidate_span = 2176;
    const auto thresholds = signal_policy::lightSyncThresholds(
        false, false, true, 0, true, wide_window_samples, narrow_candidate_span);
    CHECK(thresholds.narrow_expected_window,
          "OFDM-CHIRP noise: threshold should be the warm expected-window threshold");

    const size_t window_samples =
        narrow_candidate_span + static_cast<size_t>(waveform.getSamplesPerSymbol()) * 2;
    constexpr size_t noise_seconds = 60;
    const size_t total_samples = static_cast<size_t>(cfg.sample_rate) * noise_seconds;

    std::mt19937 rng(0x20260520u);
    std::normal_distribution<float> noise_dist(0.0f, 0.08f);
    Samples noise(total_samples);
    for (float& sample : noise) {
        sample = noise_dist(rng);
    }

    int false_syncs = 0;
    int windows = 0;
    for (size_t offset = 0; offset + window_samples <= noise.size();
         offset += narrow_candidate_span) {
        SyncResult sync;
        waveform.reset();
        const bool raw_found = waveform.detectDataSync(
            SampleSpan(noise.data() + offset, window_samples),
            sync, 0.0f, thresholds.min_confidence);
        const auto decision = signal_policy::evaluateLightSyncCandidate(
            raw_found, sync.correlation, false, true, 0, thresholds);
        if (decision.found) {
            ++false_syncs;
        }
        ++windows;
    }

    CHECK(windows > 1000, "OFDM-CHIRP noise: should scan about one minute of narrow windows");
    CHECK(false_syncs <= 1,
          "OFDM-CHIRP noise: warm narrow threshold should stay under 1 false sync/min");
    return true;
}

}  // namespace

int main() {
    test_ofdm_chirp_full_preamble_loopback();
    test_ofdm_chirp_prefers_complete_descriptor_before_stronger_future_anchor();
    test_ofdm_chirp_keeps_strongest_anchor_when_its_training_is_readable();
    test_ofdm_chirp_data_preamble_loopback();
    test_ofdm_chirp_data_preamble_awgn_warm_sync();
    test_ofdm_chirp_data_preamble_noise_false_sync_rate();

    if (tests_failed != 0) {
        std::cout << "WaveformLoopback: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "WaveformLoopback: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
