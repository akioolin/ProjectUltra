#include "waveform/ofdm_chirp_waveform.hpp"
#include "waveform/ofdm_cox_waveform.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace ultra;

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

bool test_ofdm_cox_clean_loopback() {
    OFDMNvisWaveform waveform;
    waveform.configure(Modulation::QPSK, CodeRate::R1_2);

    Samples audio;
    append(audio, waveform.generatePreamble());
    append(audio, waveform.modulate(makeEncodedCodeword()));
    appendTailMargin(audio, waveform.getSamplesPerSymbol());

    SyncResult sync;
    CHECK(waveform.detectSync(SampleSpan(audio), sync, 0.50f),
          "OFDM-COX: clean preamble should be detected");
    CHECK(sync.detected, "OFDM-COX: sync result should be marked detected");
    CHECK(sync.has_training, "OFDM-COX: sync should expose training");

    return processFromSync(waveform, audio, sync, "OFDM-COX");
}

bool test_ofdm_chirp_full_preamble_loopback() {
    ModemConfig cfg;
    cfg.sample_rate = 48000;
    cfg.fft_size = 512;
    cfg.num_carriers = 30;
    cfg.cp_mode = CyclicPrefixMode::LONG;
    cfg.modulation = Modulation::DQPSK;
    cfg.code_rate = CodeRate::R1_2;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 10;

    OFDMChirpWaveform waveform(cfg);
    waveform.configure(Modulation::DQPSK, CodeRate::R1_2);

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

bool test_ofdm_chirp_data_preamble_loopback() {
    ModemConfig cfg;
    cfg.sample_rate = 48000;
    cfg.fft_size = 512;
    cfg.num_carriers = 30;
    cfg.cp_mode = CyclicPrefixMode::LONG;
    cfg.modulation = Modulation::DQPSK;
    cfg.code_rate = CodeRate::R1_2;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 10;

    OFDMChirpWaveform waveform(cfg);
    waveform.configure(Modulation::DQPSK, CodeRate::R1_2);

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

}  // namespace

int main() {
    test_ofdm_cox_clean_loopback();
    test_ofdm_chirp_full_preamble_loopback();
    test_ofdm_chirp_data_preamble_loopback();

    if (tests_failed != 0) {
        std::cout << "WaveformLoopback: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "WaveformLoopback: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
