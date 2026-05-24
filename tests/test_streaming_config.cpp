#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "ultra/logging.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace ultra;
using namespace ultra::gui;

namespace {

int tests_run = 0;
int tests_failed = 0;

#define CHECK(cond, msg) \
    do { \
        ++tests_run; \
        if (!(cond)) { \
            ++tests_failed; \
            std::cout << "FAIL: " << msg << "\n"; \
            return; \
        } \
    } while (0)

ModemConfig makeOFDMConfig(Modulation mod, CodeRate rate) {
    ModemConfig cfg;
    cfg.fft_size = 1024;
    cfg.num_carriers = 59;
    cfg.sample_rate = 48000;
    cfg.center_freq = 1500.0f;
    cfg.cp_mode = CyclicPrefixMode::LONG;
    cfg.modulation = mod;
    cfg.code_rate = rate;
    cfg.use_pilots = true;
    cfg.pilot_spacing = 10;
    return cfg;
}

void checkMatchingOFDMGeometry(Modulation mod,
                               CodeRate rate,
                               int expected_spacing,
                               int expected_data_carriers,
                               int expected_bits_per_symbol) {
    auto cfg = makeOFDMConfig(mod, rate);

    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(cfg);
    encoder.setDataMode(mod, rate);

    StreamingDecoder decoder;
    decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP, cfg, mod, rate);

    auto tx = encoder.getConfig();
    auto rx = decoder.getConfig();

    CHECK(tx.mode == protocol::WaveformMode::OFDM_CHIRP, "encoder should report OFDM_CHIRP mode");
    CHECK(rx.mode == protocol::WaveformMode::OFDM_CHIRP, "decoder should report OFDM_CHIRP mode");
    CHECK(tx.modulation == mod && rx.modulation == mod, "TX/RX modulation should match requested mode");
    CHECK(tx.code_rate == rate && rx.code_rate == rate, "TX/RX code rate should match requested mode");
    CHECK(tx.num_carriers == 59 && rx.num_carriers == 59, "TX/RX total carrier count should match");
    CHECK(tx.pilot_spacing == expected_spacing, "encoder pilot spacing should follow waveform policy");
    CHECK(rx.pilot_spacing == expected_spacing, "decoder pilot spacing should follow waveform policy");
    CHECK(tx.data_carriers == expected_data_carriers, "encoder data carriers should match pilot geometry");
    CHECK(rx.data_carriers == expected_data_carriers, "decoder data carriers should match pilot geometry");
    CHECK(tx.bits_per_symbol == expected_bits_per_symbol, "encoder bits/symbol should match geometry");
    CHECK(rx.bits_per_symbol == expected_bits_per_symbol, "decoder bits/symbol should match geometry");
}

void test_differential_ofdm_config_match() {
    checkMatchingOFDMGeometry(Modulation::DQPSK, CodeRate::R1_2, 10, 53, 106);
    checkMatchingOFDMGeometry(Modulation::DQPSK, CodeRate::R3_4, 15, 55, 110);
    checkMatchingOFDMGeometry(Modulation::D8PSK, CodeRate::R2_3, 8, 51, 153);
}

void test_coherent_ofdm_config_match() {
    checkMatchingOFDMGeometry(Modulation::QPSK, CodeRate::R1_2, 5, 47, 94);
    checkMatchingOFDMGeometry(Modulation::QPSK, CodeRate::R3_4, 8, 51, 102);
}

void test_burst_group_clamps_match() {
    StreamingEncoder encoder;
    StreamingDecoder decoder;

    encoder.setBurstInterleaveGroupSize(1);
    decoder.setBurstInterleaveGroupSize(1);
    CHECK(encoder.getBurstInterleaveGroupSize() == 2, "encoder burst group should clamp low values");
    CHECK(decoder.getBurstInterleaveGroupSize() == 2, "decoder burst group should clamp low values");

    encoder.setBurstInterleaveGroupSize(99);
    decoder.setBurstInterleaveGroupSize(99);
    CHECK(encoder.getBurstInterleaveGroupSize() == 8, "encoder burst group should clamp high values");
    CHECK(decoder.getBurstInterleaveGroupSize() == 8, "decoder burst group should clamp high values");

    encoder.setBurstInterleaveGroupSize(6);
    decoder.setBurstInterleaveGroupSize(6);
    CHECK(encoder.getBurstInterleaveGroupSize() == 6, "encoder burst group should preserve valid values");
    CHECK(decoder.getBurstInterleaveGroupSize() == 6, "decoder burst group should preserve valid values");
}

void test_decoder_buffer_capacity_policy() {
    StreamingDecoder default_decoder;
    CHECK(default_decoder.bufferCapacitySamples() == StreamingDecoder::kDefaultBufferSamples,
          "default decoder ring capacity should preserve historical size");

    constexpr size_t smaller_real_radio_ring = 144000;  // 3 seconds at 48 kHz
    StreamingDecoder compact_decoder(smaller_real_radio_ring);
    CHECK(compact_decoder.bufferCapacitySamples() == smaller_real_radio_ring,
          "constructor should preserve caller-provided valid ring capacity");
    std::vector<float> compact_samples(smaller_real_radio_ring + 8, 0.001f);
    compact_decoder.feedAudio(compact_samples.data(), compact_samples.size());
    CHECK(compact_decoder.samplesInBuffer() == smaller_real_radio_ring,
          "custom ring should wrap at the caller-provided capacity");

    bool rejected = false;
    try {
        StreamingDecoder invalid_decoder(StreamingDecoder::kMinimumBufferSamples - 1);
        (void)invalid_decoder;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected, "decoder should reject rings smaller than the sync search window");
}

void test_forced_full_preamble_is_one_shot() {
    auto cfg = makeOFDMConfig(Modulation::DQPSK, CodeRate::R1_4);

    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(cfg);
    encoder.setDataMode(Modulation::DQPSK, CodeRate::R1_4);

    Bytes frame = protocol::v2::ControlFrame::makeAck("BRAVO", "ALPHA", 7).serialize();
    auto light = encoder.encodeFrameLight(frame);

    encoder.forceNextFrameFullPreamble();
    auto forced_full = encoder.encodeFrameLight(frame);
    auto light_again = encoder.encodeFrameLight(frame);

    CHECK(!light.empty(), "light OFDM encode should produce samples");
    CHECK(forced_full.size() > light.size(), "forced anchor should use a longer full preamble");
    CHECK(forced_full.size() - light.size() >= 20000,
          "forced anchor should include the OFDM chirp, not just LTS");
    CHECK(light_again.size() == light.size(), "forced full preamble should be one-shot");
}

void test_multi_frame_ofdm_burst_starts_with_full_anchor() {
    auto cfg = makeOFDMConfig(Modulation::DQPSK, CodeRate::R1_4);

    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(cfg);
    encoder.setDataMode(Modulation::DQPSK, CodeRate::R1_4);

    auto data = protocol::v2::makeFixedDataFrame(
        "ALPHA", "BRAVO", 8, Bytes(16, 0x42), CodeRate::R1_4).serialize();

    const auto full = encoder.encodeFrame(data);
    const auto light = encoder.encodeFrameLight(data);
    const auto burst = encoder.encodeBurstLight({data, data, data});
    const auto light_after_burst = encoder.encodeFrameLight(data);

    CHECK(!full.empty(), "full OFDM encode should produce samples");
    CHECK(!light.empty(), "light OFDM encode should produce samples");
    CHECK(full.size() > light.size(), "full OFDM preamble should be longer than data preamble");
    CHECK(burst.size() == full.size() + 2 * light.size(),
          "3-frame OFDM burst should use full anchor then light preambles");
    CHECK(light_after_burst.size() == light.size(),
          "multi-frame burst anchor should not leave one-shot full preamble armed");
}

void test_single_frame_ofdm_burst_uses_full_anchor() {
    auto cfg = makeOFDMConfig(Modulation::QAM16, CodeRate::R1_2);

    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(cfg);
    encoder.setDataMode(Modulation::QAM16, CodeRate::R1_2);

    auto data = protocol::v2::makeFixedDataFrame(
        "BRAVO", "ALPHA", 8, Bytes(24, 0x42), CodeRate::R1_2).serialize();

    const auto light = encoder.encodeFrameLight(data);
    const auto burst = encoder.encodeBurstLight({data});
    const auto light_after_burst = encoder.encodeFrameLight(data);

    CHECK(!light.empty(), "light OFDM encode should produce samples");
    CHECK(burst.size() > light.size(),
          "single-frame OFDM DATA burst should use full anchor after idle");
    CHECK(burst.size() - light.size() >= 20000,
          "single-frame OFDM DATA burst should include chirp+LTS anchor");
    CHECK(light_after_burst.size() == light.size(),
          "single-frame burst anchor should not leave one-shot full preamble armed");
}

}  // namespace

int main() {
    setLogLevel(LogLevel::ERROR);

    test_differential_ofdm_config_match();
    test_coherent_ofdm_config_match();
    test_burst_group_clamps_match();
    test_decoder_buffer_capacity_policy();
    test_forced_full_preamble_is_one_shot();
    test_multi_frame_ofdm_burst_starts_with_full_anchor();
    test_single_frame_ofdm_burst_uses_full_anchor();

    if (tests_failed != 0) {
        std::cout << "StreamingConfig: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "StreamingConfig: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
