#include "waveform/ofdm_chirp_waveform.hpp"
#include "waveform/ofdm_cox_waveform.hpp"
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

Bytes makePayload(size_t size, uint8_t seed) {
    Bytes payload(size);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>((seed + i * 17u + (i >> 1)) & 0xFFu);
    }
    return payload;
}

bool isDifferential(Modulation mod) {
    return mod == Modulation::DBPSK ||
           mod == Modulation::DQPSK ||
           mod == Modulation::D8PSK;
}

ModemConfig makeCoxConfig(Modulation mod = Modulation::DQPSK,
                          CodeRate rate = CodeRate::R1_2) {
    ModemConfig cfg;
    cfg.sample_rate = 48000;
    cfg.fft_size = 1024;
    cfg.num_carriers = 59;
    cfg.cp_mode = CyclicPrefixMode::SHORT;
    cfg.symbol_guard = 0;
    cfg.center_freq = 1500.0f;
    cfg.modulation = mod;
    cfg.code_rate = rate;
    cfg.use_pilots = !isDifferential(mod);
    cfg.pilot_spacing = ofdm_link_adaptation::recommendedPilotSpacing(mod, rate);
    return cfg;
}

int bitsPerOFDMSymbol(const ModemConfig& cfg) {
    return ofdm_link_adaptation::bitsPerOFDMSymbol(
        static_cast<int>(cfg.num_carriers),
        cfg.use_pilots,
        static_cast<int>(cfg.pilot_spacing),
        cfg.modulation);
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

Samples encodeCoxFixedFrame(OFDMNvisWaveform& waveform, const Bytes& frame_data,
                            const ModemConfig& cfg, int cw_count) {
    const int bps = bitsPerOFDMSymbol(cfg);
    Bytes encoded = protocol::v2::encodeFixedFrame(
        frame_data, cfg.code_rate, cw_count, true, static_cast<size_t>(bps));

    Samples audio;
    append(audio, waveform.generatePreamble());
    append(audio, waveform.modulate(encoded));
    return audio;
}

bool decodeCoxFixedFrameAt(OFDMNvisWaveform& waveform, const Samples& audio,
                           size_t search_offset, const Bytes& expected_frame,
                           const ModemConfig& cfg, int cw_count,
                           size_t& next_offset, const std::string& label) {
    CHECK(search_offset < audio.size(), label + ": search offset should be in audio");

    SampleSpan search(audio.data() + search_offset, audio.size() - search_offset);
    SyncResult sync;
    CHECK(waveform.detectSync(search, sync, 0.50f), label + ": sync should be detected");
    CHECK(sync.start_sample >= 0, label + ": sync start should be non-negative");

    const size_t abs_sync = search_offset + static_cast<size_t>(sync.start_sample);
    const size_t frame_samples = static_cast<size_t>(waveform.getMinSamplesForCWCount(cw_count));
    CHECK(frame_samples > static_cast<size_t>(waveform.getMinSamplesForControlFrame()),
          label + ": 4-CW sample requirement should exceed 1-CW control size");
    CHECK(abs_sync + frame_samples <= audio.size(),
          label + ": audio should contain the full fixed-CW frame after sync");

    waveform.setFrequencyOffset(sync.cfo_hz);
    waveform.setAbsoluteTrainingPosition(abs_sync);
    CHECK(waveform.process(SampleSpan(audio.data() + abs_sync, frame_samples)),
          label + ": waveform should process full fixed-CW slice");

    auto soft_bits = waveform.getSoftBits();
    const size_t expected_bits = static_cast<size_t>(cw_count) * protocol::v2::LDPC_CODEWORD_BITS;
    CHECK(soft_bits.size() >= expected_bits,
          label + ": fixed-CW slice should yield all LDPC codewords");

    auto status = protocol::v2::decodeFixedFrame(
        soft_bits, cfg.code_rate, cw_count, true,
        static_cast<size_t>(bitsPerOFDMSymbol(cfg)));
    CHECK(status.allSuccess(), label + ": LDPC fixed-frame decode should succeed");

    auto decoded = status.reassemble();
    CHECK(decoded == expected_frame, label + ": decoded frame should match TX frame");

    auto parsed = protocol::v2::DataFrame::deserialize(decoded);
    CHECK(parsed.has_value(), label + ": decoded frame should parse as DATA");
    CHECK(parsed->total_cw == static_cast<uint8_t>(cw_count),
          label + ": decoded frame should preserve fixed CW count");

    next_offset = abs_sync + frame_samples;
    waveform.reset();
    return true;
}

bool qamCoxFixedFrameRoundtrip(Modulation mod, CodeRate rate,
                               bool use_awgn, float snr_db,
                               uint32_t noise_seed) {
    const ModemConfig cfg = makeCoxConfig(mod, rate);
    constexpr int CW_COUNT = 4;

    const std::string label = std::string("OFDM-COX ") +
                              modulationToString(mod) + " " +
                              codeRateToString(rate) +
                              (use_awgn ? " AWGN" : " clean");

    CHECK(cfg.use_pilots, label + ": coherent QAM should enable pilots");
    CHECK(cfg.pilot_spacing ==
              static_cast<uint32_t>(ofdm_link_adaptation::recommendedPilotSpacing(mod, rate)),
          label + ": pilot spacing should follow coherent QAM policy");

    OFDMNvisWaveform tx(cfg);
    tx.configure(cfg.modulation, cfg.code_rate);
    OFDMNvisWaveform rx(cfg);
    rx.configure(cfg.modulation, cfg.code_rate);

    CHECK(rx.getMinSamplesForCWCount(CW_COUNT) > rx.getMinSamplesForControlFrame(),
          label + ": 4-CW sample requirement should not collapse to 1 CW");

    auto frame = protocol::v2::makeFixedDataFrame(
        "ALPHA", "BRAVO", static_cast<uint16_t>(200 + getBitsPerSymbol(mod) * 10 +
                                                 static_cast<uint8_t>(rate)),
        makePayload(96, static_cast<uint8_t>(0x50 + getBitsPerSymbol(mod) * 3)),
        cfg.code_rate, CW_COUNT).serialize();
    Samples audio = encodeCoxFixedFrame(tx, frame, cfg, CW_COUNT);
    appendTailMargin(audio, rx.getSamplesPerSymbol());

    if (use_awgn) {
        addAwgn(audio, snr_db, noise_seed);
    }

    size_t next_offset = 0;
    return decodeCoxFixedFrameAt(
        rx, audio, 0, frame, cfg, CW_COUNT, next_offset, label);
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

bool test_ofdm_cox_fixed_frame_roundtrip() {
    const ModemConfig cfg = makeCoxConfig();
    constexpr int CW_COUNT = 4;

    OFDMNvisWaveform tx(cfg);
    tx.configure(cfg.modulation, cfg.code_rate);
    OFDMNvisWaveform rx(cfg);
    rx.configure(cfg.modulation, cfg.code_rate);

    CHECK(rx.getMinSamplesForCWCount(CW_COUNT) > rx.getMinSamplesForControlFrame(),
          "OFDM-COX fixed frame: 4-CW sample requirement should not collapse to 1 CW");

    auto frame = protocol::v2::makeFixedDataFrame(
        "ALPHA", "BRAVO", 42, makePayload(96, 0x31), cfg.code_rate, CW_COUNT).serialize();
    Samples audio = encodeCoxFixedFrame(tx, frame, cfg, CW_COUNT);
    appendTailMargin(audio, rx.getSamplesPerSymbol());

    size_t next_offset = 0;
    return decodeCoxFixedFrameAt(
        rx, audio, 0, frame, cfg, CW_COUNT, next_offset,
        "OFDM-COX fixed frame");
}

bool test_ofdm_cox_qam_fixed_frame_roundtrip() {
    const struct {
        Modulation mod;
        CodeRate rate;
    } cases[] = {
        {Modulation::QAM16, CodeRate::R1_2},
        {Modulation::QAM16, CodeRate::R3_4},
        {Modulation::QAM32, CodeRate::R1_2},
        {Modulation::QAM32, CodeRate::R3_4},
        {Modulation::QAM64, CodeRate::R1_2},
        {Modulation::QAM64, CodeRate::R3_4},
    };

    for (const auto& tc : cases) {
        if (!qamCoxFixedFrameRoundtrip(tc.mod, tc.rate, false, 0.0f, 0)) {
            return false;
        }
    }

    return true;
}

bool test_ofdm_cox_qam_awgn_margin() {
    return qamCoxFixedFrameRoundtrip(
               Modulation::QAM16, CodeRate::R1_2, true, 17.0f, 0xC0517u) &&
           qamCoxFixedFrameRoundtrip(
               Modulation::QAM32, CodeRate::R3_4, true, 25.0f, 0xC0532u) &&
           qamCoxFixedFrameRoundtrip(
               Modulation::QAM64, CodeRate::R3_4, true, 28.0f, 0xC0564u);
}

bool test_ofdm_cox_nvis_preset_qam16_r34_roundtrip() {
    auto nvis = OFDMNvisWaveform::createNvisMode();
    ModemConfig cfg = nvis->getConfig();
    constexpr int CW_COUNT = 4;

    cfg.modulation = Modulation::QAM16;
    cfg.code_rate = CodeRate::R3_4;
    cfg.use_pilots = true;
    cfg.pilot_spacing =
        ofdm_link_adaptation::recommendedPilotSpacing(cfg.modulation, cfg.code_rate);

    CHECK(cfg.fft_size == 1024, "OFDM-COX NVIS preset: FFT should be 1024");
    CHECK(cfg.num_carriers == 59, "OFDM-COX NVIS preset: carrier count should be 59");
    CHECK(cfg.cp_mode == CyclicPrefixMode::MEDIUM,
          "OFDM-COX NVIS preset: cyclic prefix should be MEDIUM");

    OFDMNvisWaveform tx(cfg);
    tx.configure(cfg.modulation, cfg.code_rate);
    OFDMNvisWaveform rx(cfg);
    rx.configure(cfg.modulation, cfg.code_rate);

    CHECK(rx.getSamplesPerSymbol() == static_cast<int>(cfg.getSymbolDuration()),
          "OFDM-COX NVIS preset: samples per symbol should follow config");
    CHECK(rx.getMinSamplesForCWCount(CW_COUNT) > rx.getMinSamplesForControlFrame(),
          "OFDM-COX NVIS preset: 4-CW sample requirement should not collapse to 1 CW");

    auto frame = protocol::v2::makeFixedDataFrame(
        "ALPHA", "BRAVO", 501, makePayload(96, 0x73),
        cfg.code_rate, CW_COUNT).serialize();
    Samples audio = encodeCoxFixedFrame(tx, frame, cfg, CW_COUNT);
    appendTailMargin(audio, rx.getSamplesPerSymbol());

    size_t next_offset = 0;
    return decodeCoxFixedFrameAt(
        rx, audio, 0, frame, cfg, CW_COUNT, next_offset,
        "OFDM-COX NVIS preset QAM16 R3/4 fixed frame");
}

bool test_ofdm_cox_16_frame_burst_roundtrip() {
    const ModemConfig cfg = makeCoxConfig();
    constexpr int CW_COUNT = 4;
    constexpr int FRAME_COUNT = 16;

    OFDMNvisWaveform tx(cfg);
    tx.configure(cfg.modulation, cfg.code_rate);
    OFDMNvisWaveform rx(cfg);
    rx.configure(cfg.modulation, cfg.code_rate);

    Samples burst;
    std::vector<Bytes> frames;
    frames.reserve(FRAME_COUNT);
    for (int i = 0; i < FRAME_COUNT; ++i) {
        auto frame = protocol::v2::makeFixedDataFrame(
            "ALPHA", "BRAVO", static_cast<uint16_t>(100 + i),
            makePayload(80 + static_cast<size_t>(i % 7), static_cast<uint8_t>(0x40 + i)),
            cfg.code_rate, CW_COUNT).serialize();
        auto one = encodeCoxFixedFrame(tx, frame, cfg, CW_COUNT);
        append(burst, one);
        frames.push_back(std::move(frame));
    }
    appendTailMargin(burst, rx.getSamplesPerSymbol());

    size_t search_offset = 0;
    for (int i = 0; i < FRAME_COUNT; ++i) {
        size_t next_offset = 0;
        CHECK(decodeCoxFixedFrameAt(
                  rx, burst, search_offset, frames[static_cast<size_t>(i)],
                  cfg, CW_COUNT, next_offset,
                  "OFDM-COX 16-frame burst frame " + std::to_string(i)),
              "OFDM-COX 16-frame burst: frame should decode");
        search_offset = next_offset;
    }

    return true;
}

}  // namespace

int main() {
    test_ofdm_cox_clean_loopback();
    test_ofdm_chirp_full_preamble_loopback();
    test_ofdm_chirp_data_preamble_loopback();
    test_ofdm_cox_fixed_frame_roundtrip();
    test_ofdm_cox_qam_fixed_frame_roundtrip();
    test_ofdm_cox_qam_awgn_margin();
    test_ofdm_cox_nvis_preset_qam16_r34_roundtrip();
    test_ofdm_cox_16_frame_burst_roundtrip();

    if (tests_failed != 0) {
        std::cout << "WaveformLoopback: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "WaveformLoopback: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
