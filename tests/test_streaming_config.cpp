#include "gui/modem/streaming_decoder.hpp"
#include "gui/modem/streaming_encoder.hpp"
#include "ultra/logging.hpp"
#include "waveform/ofdm_chirp_waveform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
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
    decoder.applyPendingConfigForTesting();  // §14.36: connected-OFDM apply is deferred to processBuffer

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

// NOTE: the OFDM band is coherent-only (differential DQPSK/D8PSK retired from OFDM
// and relocated to MC-DPSK — see OFDM_COHERENT_ONLY_DECISION). The waveform now
// applies coherent pilot geometry to every OFDM mode, so the old differential-OFDM
// geometry expectations (spacing 10/15/8) no longer describe a live path. The
// coherent cases below validate the geometry helper on the modes the ladder selects.
void test_coherent_ofdm_config_match() {
    // FIXED-GRID BAND (2026-07-06): R1/2 rides the sp8 grid -> 8 pilots, 51 data.
    checkMatchingOFDMGeometry(Modulation::QPSK, CodeRate::R1_2, 8, 51, 102);
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
    // Ceiling raised 8 -> 32 (2026-05-26): deep burst groups span multiple
    // coherence times for file-class fade diversity (see sanitizeBurstGroupSize).
    CHECK(encoder.getBurstInterleaveGroupSize() == 32, "encoder burst group should clamp high values");
    CHECK(decoder.getBurstInterleaveGroupSize() == 32, "decoder burst group should clamp high values");

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

void test_connected_ofdm_config_arms_full_anchor() {
    auto cfg = makeOFDMConfig(Modulation::DQPSK, CodeRate::R1_4);

    StreamingDecoder decoder;
    decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                 cfg, Modulation::DQPSK, CodeRate::R1_4);
    decoder.applyPendingConfigForTesting();  // §14.36: deferred to processBuffer
    CHECK(decoder.expectsFullOFDMAnchorForTesting(),
          "connected OFDM entry should arm full chirp+LTS acquisition");

    decoder.expectFullOFDMAnchorOnce();
    auto promoted = makeOFDMConfig(Modulation::QAM16, CodeRate::R1_2);
    decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                 promoted, Modulation::QAM16, CodeRate::R1_2);
    decoder.applyPendingConfigForTesting();  // §14.36: deferred to processBuffer
    CHECK(decoder.expectsFullOFDMAnchorForTesting(),
          "connected OFDM reconfiguration must not clear a pending full-anchor expectation");

    decoder.clearFullOFDMAnchorExpectation();
    CHECK(!decoder.expectsFullOFDMAnchorForTesting(),
          "local DATA TX should be able to clear a pending peer-DATA full-anchor expectation");
}

void test_burst_header_current_anchor_controls_receiver_search() {
    struct DescriptorOutcome {
        bool descriptor_matches = false;
        bool expects_full_anchor = false;
        bool warm_cfo_certified = false;
        bool poisoned_before_decode = false;
        float final_cfo_hz = 0.0f;
    };
    auto run = [](bool announce_full) {
        constexpr Modulation kMod = Modulation::QPSK;
        constexpr CodeRate kRate = CodeRate::R3_4;
        const auto cfg = makeOFDMConfig(kMod, kRate);

        uint8_t flags = 0;
        if (announce_full) {
            flags |= protocol::v2::ControlFrame::
                BURST_FLAG_CURRENT_GROUP_FULL_ANCHOR;
        }
        const auto descriptor = protocol::v2::ControlFrame::makeBurstHeader(
            "ALPHA", "BRAVO", /*seq=*/9, /*group_size=*/2,
            /*cw_per_frame=*/8, kMod, kRate, flags, /*lifting_z=*/27);

        StreamingEncoder encoder;
        encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
        encoder.setOFDMConfig(cfg);
        encoder.setDataMode(kMod, kRate);
        const auto tx = encoder.encodeFrame(descriptor.serialize());

        std::vector<float> audio(48000, 0.0f);
        audio.insert(audio.end(), tx.begin(), tx.end());
        audio.resize(audio.size() + 96000, 0.0f);

        StreamingDecoder decoder;
        decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                     cfg, kMod, kRate);
        decoder.applyPendingConfigForTesting();
        constexpr size_t kChunk = 1200;
        bool poisoned_before_decode = false;
        for (size_t pos = 0; pos < audio.size() && !decoder.hasBurstDescriptor();
             pos += kChunk) {
            const size_t n = std::min(kChunk, audio.size() - pos);
            decoder.feedAudio(audio.data() + pos, n);
            decoder.processBuffer();
            if (!poisoned_before_decode &&
                decoder.stateForTesting() == DecoderState::SYNC_FOUND) {
                // Reproduce the live failure's lifecycle: a preceding crater left
                // a stale -0.70 Hz tracker value, while this valid descriptor's
                // own pilot-refined CFO is near zero. Certification without the
                // control-first feedback store would preserve the poison.
                decoder.setKnownCFO(-0.70f);
                poisoned_before_decode = true;
            }
        }
        const auto parsed = decoder.lastBurstDescriptor();
        return DescriptorOutcome{
            decoder.hasBurstDescriptor() &&
                parsed.current_group_full_anchor == announce_full,
            decoder.expectsFullOFDMAnchorForTesting(),
            decoder.warmCFOCertifiedForTesting(),
            poisoned_before_decode,
            decoder.getLastCFO()};
    };

    const auto light = run(/*announce_full=*/false);
    CHECK(light.descriptor_matches && !light.expects_full_anchor,
          "legacy/same-mode descriptor must retain the warm light-LTS handoff");
    CHECK(light.warm_cfo_certified,
          "valid light BURST_HEADER must certify the classic OFDM CFO before DATA");
    CHECK(light.poisoned_before_decode && std::abs(light.final_cfo_hz) < 0.25f,
          "light BURST_HEADER must replace stale -0.70 Hz CFO with its own near-zero estimate");

    const auto full = run(/*announce_full=*/true);
    CHECK(full.descriptor_matches && full.expects_full_anchor,
          "current-group FULL descriptor bit must arm full chirp+LTS acquisition");
    CHECK(full.warm_cfo_certified,
          "valid full BURST_HEADER must restore the CFO certificate before DATA");
    CHECK(full.poisoned_before_decode && std::abs(full.final_cfo_hz) < 0.25f,
          "full BURST_HEADER must replace stale -0.70 Hz CFO before DATA acquisition");
}

void test_per_request_full_anchor_matches_descriptor_and_phy() {
    constexpr Modulation kMod = Modulation::QPSK;
    constexpr CodeRate kRate = CodeRate::R3_4;
    constexpr int kGroup = 2;
    constexpr int kCw = 2;
    const auto cfg = makeOFDMConfig(kMod, kRate);
    const std::vector<Bytes> frames{
        protocol::v2::makeFixedDataFrame(
            "ALPHA", "BRAVO", 20, Bytes(24, 0x31), kRate, kCw).serialize(),
        protocol::v2::makeFixedDataFrame(
            "ALPHA", "BRAVO", 21, Bytes(24, 0x32), kRate, kCw).serialize()};

    auto configure = [&](StreamingEncoder& encoder) {
        encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
        encoder.setOFDMConfig(cfg);
        encoder.setDataMode(kMod, kRate);
        encoder.setFixedFrameCodewords(kCw);
        encoder.setBurstInterleave(false);
        encoder.setBurstInterleaveGroupSize(kGroup);
        encoder.setBurstDescriptorEnabled(true);
        encoder.setBurstDescriptorIdentity("ALPHA", "BRAVO");
        encoder.setBurstGroupSeq(12);
    };

    // Queue-order analogue: an older ordinary request is encoded first, then a
    // later repair, then another ordinary request. Only the middle call owns the
    // full-anchor option.
    StreamingEncoder request_encoder;
    configure(request_encoder);
    const auto normal = request_encoder.encodeBurstLight(frames);
    const auto descriptor_only_repair = request_encoder.encodeBurstLight(
        frames, BurstAnchorOptions{/*force_full_group_start=*/false,
                                   /*keep_skip_streak=*/false,
                                   /*force_full_descriptor=*/true});
    const auto repair = request_encoder.encodeBurstLight(
        frames, BurstAnchorOptions{/*force_full_group_start=*/true,
                                   /*keep_skip_streak=*/false});
    const auto next_normal = request_encoder.encodeBurstLight(frames);

    CHECK(!normal.empty() && !descriptor_only_repair.empty() && !repair.empty(),
          "normal and both repair descriptor bursts must encode");
    CHECK(descriptor_only_repair.size() == normal.size(),
          "descriptor-only repair must retain the normal light DATA-group geometry");
    CHECK(repair.size() > normal.size() &&
              repair.size() - normal.size() >= 20000,
          "per-request repair must emit a physical full chirp at the DATA group start");
    const size_t exact_group_chirp_samples =
        request_encoder.getWaveform()->generatePreamble().size() -
        2u * static_cast<size_t>(
                 request_encoder.getWaveform()->getSamplesPerSymbol());
    CHECK(repair.size() - descriptor_only_repair.size() ==
              exact_group_chirp_samples,
          "descriptor-only repair must save exactly the second DATA-group chirp");
    CHECK(next_normal.size() == normal.size(),
          "per-request full anchor must not leak into the next encoder call");

    // Isolate the production-emitted descriptor prefix before feeding the RX.
    // Feeding the following DATA full chirp too would turn this contract test
    // into a sync-candidate-selection test (that separate path can prefer the
    // later full chirp in fading).  The prefix bytes/samples below still come
    // from the real repair encode; the throwaway encode is used only to obtain
    // the deterministic descriptor sample length.
    StreamingEncoder descriptor_sizer;
    configure(descriptor_sizer);
    const auto descriptor_shape = protocol::v2::ControlFrame::makeBurstHeader(
        "ALPHA", "BRAVO", /*seq=*/12, /*group_size=*/kGroup,
        /*cw_per_frame=*/kCw, kMod, kRate, /*flags=*/0);
    const size_t descriptor_samples =
        descriptor_sizer.encodeFrame(descriptor_shape.serialize()).size();
    CHECK(descriptor_samples > 0 && repair.size() > descriptor_samples,
          "repair burst must contain a complete descriptor prefix and DATA group");

    // The descriptor bit must announce the same full anchor whose extra samples
    // were established above, and must switch the next search from warm/light to full.
    std::vector<float> audio(48000, 0.0f);
    audio.insert(audio.end(), repair.begin(), repair.begin() + descriptor_samples);
    audio.resize(audio.size() + 96000, 0.0f);

    StreamingDecoder decoder;
    decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                 cfg, kMod, kRate);
    decoder.applyPendingConfigForTesting();
    constexpr size_t kChunk = 1200;
    for (size_t pos = 0; pos < audio.size() && !decoder.hasBurstDescriptor();
         pos += kChunk) {
        const size_t n = std::min(kChunk, audio.size() - pos);
        decoder.feedAudio(audio.data() + pos, n);
        decoder.processBuffer();
    }

    CHECK(decoder.hasBurstDescriptor(),
          "receiver must consume the production-encoded repair descriptor");
    CHECK(decoder.lastBurstDescriptor().current_group_full_anchor,
          "repair descriptor must announce CURRENT_GROUP_FULL_ANCHOR");
    CHECK(decoder.expectsFullOFDMAnchorForTesting(),
          "announced repair must arm full-anchor acquisition for its DATA group");

    // Positive decode path for the new split ownership: its robust full descriptor
    // must announce LIGHT for the immediately-following DATA group, and that group
    // must decode under the production warm handoff with exact geometry provenance.
    std::vector<float> descriptor_only_audio(4800, 0.0f);
    descriptor_only_audio.insert(descriptor_only_audio.end(),
                                 descriptor_only_repair.begin(),
                                 descriptor_only_repair.end());
    descriptor_only_audio.resize(descriptor_only_audio.size() + 192000, 0.0f);

    StreamingDecoder light_decoder;
    light_decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                       cfg, kMod, kRate);
    light_decoder.applyPendingConfigForTesting();
    bool light_group_fired = false;
    bool light_group_ok = false;
    bool light_geometry_proven = false;
    uint16_t light_group_mask = 0;
    bool light_saw_descriptor = false;
    bool light_descriptor_announced_full = false;
    light_decoder.setBurstGroupCallback(
        [&](uint16_t, const std::vector<Bytes>&, bool all_ok, float,
            uint16_t frame_mask, bool, uint8_t, bool geometry_proven) {
            light_group_fired = true;
            light_group_ok = all_ok;
            light_group_mask = frame_mask;
            light_geometry_proven = geometry_proven;
        });
    constexpr size_t kLightChunk = 4800;
    for (size_t pos = 0;
         pos < descriptor_only_audio.size() && !light_group_fired;
         pos += kLightChunk) {
        const size_t n = std::min(kLightChunk,
                                  descriptor_only_audio.size() - pos);
        light_decoder.feedAudio(descriptor_only_audio.data() + pos, n);
        light_decoder.processBuffer();
        if (light_decoder.hasBurstDescriptor()) {
            light_saw_descriptor = true;
            light_descriptor_announced_full =
                light_decoder.lastBurstDescriptor().current_group_full_anchor;
        }
    }
    CHECK(light_saw_descriptor && !light_descriptor_announced_full,
          "descriptor-only repair must decode a robust descriptor that announces LIGHT DATA");
    CHECK(light_group_fired && light_group_ok && light_geometry_proven &&
              light_group_mask == 0x0003,
          "descriptor-only repair's light DATA group must decode with exact 2/2 provenance");
}

void test_repair_burst_recovers_weaker_descriptor_before_stronger_full_group() {
    constexpr Modulation kMod = Modulation::QPSK;
    constexpr CodeRate kRate = CodeRate::R3_4;
    constexpr int kGroup = 2;
    constexpr int kCw = 3;
    constexpr uint8_t kZ = 81;
    auto cfg = makeOFDMConfig(kMod, kRate);
    cfg.cp_mode = CyclicPrefixMode::MEDIUM;

    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(cfg);
    encoder.setDataMode(kMod, kRate);
    encoder.setFixedFrameCodewords(kCw);
    encoder.setLDPCLiftingZ(kZ);
    encoder.setBurstInterleave(false);
    encoder.setBurstInterleaveGroupSize(kGroup);
    encoder.setBurstDescriptorEnabled(true);
    encoder.setBurstDescriptorIdentity("ALPHA", "BRAVO");
    encoder.setBurstGroupSeq(44);

    std::vector<Bytes> frames;
    for (uint16_t seq = 0; seq < kGroup; ++seq) {
        frames.push_back(protocol::v2::makeFixedDataFrame(
            "ALPHA", "BRAVO", seq,
            Bytes(24, static_cast<uint8_t>(0x70 + seq)), kRate, kCw, kZ)
                             .serialize());
    }
    auto repair = encoder.encodeBurstLight(
        frames, BurstAnchorOptions{/*force_full_group_start=*/true,
                                   /*keep_skip_streak=*/false});
    CHECK(!repair.empty(), "full-anchor repair burst must encode");

    // Weaken only the descriptor's dual chirp.  Its LTS/control payload remains
    // full strength, so once chronological acquisition selects the descriptor,
    // BURST_HEADER decoding itself is not the variable under test.  The later
    // DATA-group chirp remains strong and lands in the same 120k detector window.
    const size_t full_preamble = encoder.getWaveform()->generatePreamble().size();
    const size_t symbol_samples =
        static_cast<size_t>(encoder.getWaveform()->getSamplesPerSymbol());
    CHECK(full_preamble > 2 * symbol_samples,
          "repair fixture must expose chirp plus two LTS symbols");
    const size_t descriptor_chirp_samples = full_preamble - 2 * symbol_samples;
    CHECK(descriptor_chirp_samples < repair.size(),
          "descriptor chirp must fit inside repair burst");
    for (size_t i = 0; i < descriptor_chirp_samples; ++i) {
        repair[i] *= 0.25f;
    }

    // One real feed quantum of idle establishes the production geometry:
    // descriptor UP starts at 4,800; the stronger repair-group UP starts after
    // the 67,680-sample descriptor.  Deterministic noise makes the latter the
    // strongest normalized peak while leaving the hardened descriptor easy to
    // decode after the earlier-complete-pair recovery.
    std::vector<float> audio(4800, 0.0f);
    audio.insert(audio.end(), repair.begin(), repair.end());
    audio.resize(audio.size() + 192000, 0.0f);
    uint32_t noise_state = 0x20260802u;
    for (float& sample : audio) {
        noise_state = noise_state * 1664525u + 1013904223u;
        const float uniform =
            static_cast<float>((noise_state >> 8) & 0x00ffffffu) /
            static_cast<float>(0x01000000u);
        sample += (2.0f * uniform - 1.0f) * 0.02f;
    }

    // Prove the fixture really exercises the old failure, rather than merely
    // succeeding through an easy first-peak lock.  The legacy strongest-only
    // primitive must choose the later DATA up-chirp, and that pair's training
    // start must not yet fit in the first production-sized search window.
    auto* ofdm = dynamic_cast<OFDMChirpWaveform*>(encoder.getWaveform());
    CHECK(ofdm != nullptr && ofdm->getChirpSync() != nullptr,
          "repair fixture must expose the OFDM dual-chirp detector");
    constexpr size_t kFirstSearchWindow = 120000;
    const auto strongest = ofdm->getChirpSync()->detectDualChirp(
        SampleSpan(audio.data(), kFirstSearchWindow), 0.15f);
    CHECK(strongest.strongest_up_candidate_start >
              static_cast<int>(full_preamble),
          "fixture must make the later DATA chirp the strongest global candidate");
    const size_t chirp_samples = ofdm->getChirpSync()->getChirpSamples();
    const size_t gap_samples = static_cast<size_t>(
        cfg.sample_rate * ofdm->getChirpSync()->getConfig().gap_ms / 1000.0f);
    const bool strongest_training_readable =
        strongest.success && strongest.down_chirp_start >= 0 &&
        static_cast<size_t>(strongest.down_chirp_start) <= kFirstSearchWindow &&
        chirp_samples + gap_samples <=
            kFirstSearchWindow -
                static_cast<size_t>(strongest.down_chirp_start);
    CHECK(!strongest_training_readable,
          "fixture's stronger later candidate must be incomplete/unreadable in the first window");

    StreamingDecoder decoder;
    decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                 cfg, kMod, kRate);
    decoder.setFixedFrameCodewords(8);  // deliberately stale pre-descriptor geometry
    decoder.setBurstInterleave(false);
    decoder.setBurstInterleaveGroupSize(4);
    decoder.applyPendingConfigForTesting();

    bool saw_descriptor = false;
    bool saw_z81 = false;
    bool saw_current_full = false;
    bool group_fired = false;
    bool group_ok = false;
    bool geometry_proven = false;
    uint16_t group_mask = 0;
    decoder.setBurstGroupCallback(
        [&](uint16_t, const std::vector<Bytes>&, bool all_ok, float,
            uint16_t frame_mask, bool, uint8_t, bool proven) {
            group_fired = true;
            group_ok = all_ok;
            group_mask = frame_mask;
            geometry_proven = proven;
        });

    constexpr size_t kChunk = 4800;
    for (size_t pos = 0; pos < audio.size() && !group_fired; pos += kChunk) {
        const size_t n = std::min(kChunk, audio.size() - pos);
        decoder.feedAudio(audio.data() + pos, n);
        decoder.processBuffer();
        if (decoder.hasBurstDescriptor()) {
            saw_descriptor = true;
            saw_z81 = saw_z81 || decoder.activeBurstLiftingZ() == kZ;
            saw_current_full = saw_current_full ||
                decoder.lastBurstDescriptor().current_group_full_anchor;
        }
    }

    const auto descriptor = decoder.lastBurstDescriptor();
    CHECK(saw_descriptor && saw_z81 && saw_current_full,
          "weaker repair descriptor must be acquired before the stronger DATA chirp and arm Z81/FULL");
    CHECK(descriptor.cw_per_frame == kCw && descriptor.lifting_z == kZ &&
              descriptor.group_size == kGroup,
          "recovered BURST_HEADER must retain the exact cw/Z/group geometry");
    CHECK(group_fired && group_ok && geometry_proven && group_mask == 0x0003,
          "descriptor-proven repair DATA group must decode completely after recovery");
}

void test_measurement_lifting_z_override_survives_reset() {
    auto cfg = makeOFDMConfig(Modulation::QPSK, CodeRate::R3_4);

    StreamingDecoder decoder;
    decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                 cfg, Modulation::QPSK, CodeRate::R3_4);
    decoder.applyPendingConfigForTesting();
    CHECK(decoder.activeBurstLiftingZ() == 27,
          "decoder should default to Z=27 without a burst descriptor");

    decoder.setLDPCLiftingZForTesting(81);
    CHECK(decoder.activeBurstLiftingZ() == 81,
          "test-only lifting override should select Z=81");

    decoder.reset();
    CHECK(decoder.activeBurstLiftingZ() == 81,
          "test-only lifting override should survive per-frame reset");
}

void test_connected_ofdm_r34_cw3_z81_waits_for_full_frame() {
    constexpr Modulation kMod = Modulation::QPSK;
    constexpr CodeRate kRate = CodeRate::R3_4;
    constexpr int kCodewords = 3;
    constexpr int kLiftingZ = 81;

    auto cfg = makeOFDMConfig(kMod, kRate);
    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(cfg);
    encoder.setDataMode(kMod, kRate);
    encoder.setFixedFrameCodewords(kCodewords);
    encoder.setLDPCLiftingZ(kLiftingZ);
    encoder.setCarrierLdpcInterleaver(false);

    auto frame = protocol::v2::makeFixedDataFrame(
        "ALPHA", "BRAVO", 81, Bytes(20, 0x5A), kRate,
        kCodewords, kLiftingZ);
    const auto expected = frame.serialize();
    const auto tx = encoder.encodeFrame(expected);
    CHECK(!tx.empty(), "QPSK R3/4 cw3/Z81 full-anchor frame must encode");

    StreamingDecoder decoder;
    decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                 cfg, kMod, kRate);
    decoder.setFixedFrameCodewords(kCodewords);
    decoder.setCarrierLdpcInterleaver(false);
    decoder.applyPendingConfigForTesting();
    decoder.setLDPCLiftingZForTesting(kLiftingZ);
    decoder.expectFullOFDMAnchorOnce();

    bool callback_fired = false;
    DecodeResult decoded;
    decoder.setFrameCallback([&](const DecodeResult& result) {
        callback_fired = true;
        decoded = result;
    });

    // Match the faithful measurement pump: give chirp search its full window,
    // then enough tail silence for the pending-cw wait to be serviced.
    std::vector<float> audio(48000, 0.0f);
    audio.insert(audio.end(), tx.begin(), tx.end());
    audio.resize(audio.size() + 192000, 0.0f);
    constexpr size_t kChunk = 4800;
    for (size_t pos = 0; pos < audio.size() && !callback_fired;
         pos += kChunk) {
        const size_t n = std::min(kChunk, audio.size() - pos);
        decoder.feedAudio(audio.data() + pos, n);
        decoder.processBuffer();
    }

    CHECK(callback_fired && decoded.success,
          "cw3/Z81 must wait past the first 1944 LLRs and decode the full 5832-bit frame");
    CHECK(decoded.frame_data == expected,
          "connected cw3/Z81 streaming decode must be byte-exact");
}

void test_burst_descriptor_regrids_equal_capacity_8psk_profiles() {
    constexpr Modulation kMod = Modulation::QAM8;
    constexpr CodeRate kRate = CodeRate::R2_3;
    struct Geometry {
        int cw;
        uint8_t z;
        int stale_rx_cw;
    };

    // Exercise both the default and experimental physical grids through the
    // actual descriptor-control + multi-frame DATA receive path. Both carry
    // 7776 coded bits per frame, so this also pins equal airtime/capacity while
    // proving that the receiver consumes cw_per_frame and lifting_z together.
    const Geometry geometries[] = {
        {12, 27, 4},
        {4, 81, 12},
    };
    const size_t short_capacity =
        protocol::v2::getFixedFramePayloadCapacity(kRate, 12);
    const size_t long_capacity =
        protocol::v2::getFixedFramePayloadCapacityZ(kRate, 4, 81);
    CHECK(short_capacity == long_capacity,
          "descriptor profiles must retain identical DATA capacity");

    auto normalize_physical_tail = [](const Bytes& bytes) {
        auto frame = protocol::v2::DataFrame::deserialize(bytes);
        if (!frame) return bytes;
        frame->flags = static_cast<uint8_t>(
            frame->flags & ~protocol::v2::Flags::PHYSICAL_BURST_END);
        return frame->serialize();
    };

    for (const auto geometry : geometries) {
        auto cfg = makeOFDMConfig(kMod, kRate);
        StreamingEncoder encoder;
        encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
        encoder.setOFDMConfig(cfg);
        encoder.setDataMode(kMod, kRate);
        encoder.setFixedFrameCodewords(geometry.cw);
        encoder.setLDPCLiftingZ(geometry.z);
        encoder.setBurstInterleave(false);
        encoder.setBurstInterleaveGroupSize(2);
        encoder.setBurstDescriptorEnabled(true);
        encoder.setBurstDescriptorIdentity("ALPHA", "BRAVO");
        encoder.setBurstGroupSeq(11);

        std::vector<Bytes> expected;
        for (uint16_t seq = 0; seq < 2; ++seq) {
            auto frame = protocol::v2::makeFixedDataFrame(
                "ALPHA", "BRAVO", seq,
                Bytes(48, static_cast<uint8_t>(0x50 + seq)), kRate,
                geometry.cw, geometry.z);
            expected.push_back(frame.serialize());
        }

        // Do not force another full anchor at the DATA group start: the
        // descriptor is already the full acquisition anchor, and production
        // intentionally follows it with the light group marker.
        const auto burst = encoder.encodeBurstLight(expected);
        CHECK(!burst.empty(), "descriptor-bearing 8PSK burst must encode");

        std::vector<float> audio(48000, 0.0f);
        audio.insert(audio.end(), burst.begin(), burst.end());
        audio.resize(audio.size() + 192000, 0.0f);

        StreamingDecoder decoder;
        decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                     cfg, kMod, kRate);
        decoder.setFixedFrameCodewords(geometry.stale_rx_cw);
        decoder.setBurstInterleave(false);
        decoder.setBurstInterleaveGroupSize(4);
        decoder.applyPendingConfigForTesting();

        bool callback_fired = false;
        bool all_ok = false;
        bool geometry_proven = false;
        uint16_t callback_group_seq = 0;
        uint16_t callback_mask = 0;
        uint8_t callback_group_size = 0;
        std::vector<Bytes> decoded;
        decoder.setBurstGroupCallback(
            [&](uint16_t group_seq, const std::vector<Bytes>& frames,
                bool group_ok, float, uint16_t frame_mask, bool interleaved,
                uint8_t group_size, bool proven) {
                callback_fired = true;
                callback_group_seq = group_seq;
                decoded = frames;
                all_ok = group_ok;
                callback_mask = frame_mask;
                callback_group_size = group_size;
                geometry_proven = proven && !interleaved;
            });

        constexpr size_t kChunk = 4800;
        for (size_t pos = 0; pos < audio.size() && !callback_fired;
             pos += kChunk) {
            const size_t n = std::min(kChunk, audio.size() - pos);
            decoder.feedAudio(audio.data() + pos, n);
            decoder.processBuffer();
        }

        CHECK(callback_fired && all_ok && geometry_proven,
              "descriptor-proven multi-frame 8PSK group must decode cleanly");
        CHECK(callback_group_seq == 11 && callback_group_size == 2 &&
                  callback_mask == 0x0003,
              "decoded group must retain descriptor sequence and two-frame mask");
        CHECK(decoded.size() == expected.size(),
              "decoded descriptor group must contain both logical DATA frames");
        for (size_t i = 0; i < expected.size() && i < decoded.size(); ++i) {
            CHECK(normalize_physical_tail(decoded[i]) ==
                      normalize_physical_tail(expected[i]),
                  "decoded DATA must equal input apart from physical-tail stamping");
        }

        const auto descriptor = decoder.lastBurstDescriptor();
        CHECK(descriptor.cw_per_frame == geometry.cw &&
                  descriptor.lifting_z == geometry.z &&
                  descriptor.group_size == 2 &&
                  descriptor.modulation == kMod &&
                  descriptor.code_rate == kRate,
              "receiver must cache the exact cw/Z/mod/rate descriptor geometry");
    }
}

void test_8psk_cw4_z81_bi1_energy_erasure_reaches_group_callback() {
    constexpr Modulation kMod = Modulation::QAM8;
    constexpr CodeRate kRate = CodeRate::R2_3;
    constexpr int kCw = 4;
    constexpr uint8_t kZ = 81;
    constexpr int kGroup = 5;
    const auto cfg = makeOFDMConfig(kMod, kRate);

    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(cfg);
    encoder.setDataMode(kMod, kRate);
    encoder.setFixedFrameCodewords(kCw);
    encoder.setLDPCLiftingZ(kZ);
    encoder.setBurstInterleave(true);
    encoder.setBurstInterleaveGroupSize(kGroup);
    encoder.setBurstDescriptorEnabled(true);
    encoder.setBurstDescriptorIdentity("ALPHA", "BRAVO");
    encoder.setBurstGroupSeq(41);

    std::vector<Bytes> frames;
    for (uint16_t seq = 0; seq < kGroup; ++seq) {
        frames.push_back(protocol::v2::makeFixedDataFrame(
            "ALPHA", "BRAVO", seq,
            Bytes(64, static_cast<uint8_t>(0x81 + seq)), kRate, kCw, kZ)
                             .serialize());
    }
    auto burst = encoder.encodeBurstLight(
        frames, BurstAnchorOptions{/*force_full_group_start=*/true,
                                   /*keep_skip_streak=*/false,
                                   /*force_full_descriptor=*/true});
    CHECK(!burst.empty(), "cw4/Z81 BI1 erasure fixture must encode");

    // Remove one complete continuation member while preserving the descriptor and
    // group-start anchor. RX must synthesize one 7776-LLR Z81 erasure, deinterleave,
    // and produce a group verdict rather than throwing on the old 2592-LLR Z27 size.
    const size_t member_samples = static_cast<size_t>(
        encoder.getWaveform()->getMinSamplesForCWCount(kCw));
    const size_t light_preamble =
        encoder.getWaveform()->generateDataPreamble().size();
    const size_t full_preamble = encoder.getWaveform()->generatePreamble().size();
    CHECK(member_samples > light_preamble,
          "cw4/Z81 member must contain data after its light preamble");
    const size_t data_samples = member_samples - light_preamble;
    const size_t first_member_samples = full_preamble + data_samples;
    const size_t group_samples =
        first_member_samples + static_cast<size_t>(kGroup - 1) * member_samples;
    CHECK(burst.size() > group_samples,
          "BI1 fixture must contain a descriptor before the DATA group");
    const size_t descriptor_samples = burst.size() - group_samples;
    const size_t erased_member_start = descriptor_samples + first_member_samples;
    CHECK(erased_member_start + member_samples <= burst.size(),
          "continuation erasure must stay inside the encoded burst");
    std::fill(burst.begin() + static_cast<std::ptrdiff_t>(erased_member_start),
              burst.begin() + static_cast<std::ptrdiff_t>(
                                  erased_member_start + member_samples),
              0.0f);

    StreamingDecoder decoder;
    decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                 cfg, kMod, kRate);
    decoder.setFixedFrameCodewords(12);  // stale fallback; descriptor must re-grid it
    decoder.setBurstInterleave(false);   // descriptor bit must engage BI1
    decoder.setBurstInterleaveGroupSize(2);
    decoder.applyPendingConfigForTesting();

    bool callback_fired = false;
    bool callback_interleaved = false;
    bool callback_geometry_proven = false;
    uint8_t callback_group_size = 0;
    decoder.setBurstGroupCallback(
        [&](uint16_t, const std::vector<Bytes>&, bool, float, uint16_t,
            bool interleaved, uint8_t group_size, bool geometry_proven) {
            callback_fired = true;
            callback_interleaved = interleaved;
            callback_group_size = group_size;
            callback_geometry_proven = geometry_proven;
        });

    std::vector<float> audio(48000, 0.0f);
    audio.insert(audio.end(), burst.begin(), burst.end());
    audio.resize(audio.size() + 192000, 0.0f);
    constexpr size_t kChunk = 4800;
    for (size_t pos = 0; pos < audio.size() && !callback_fired; pos += kChunk) {
        const size_t n = std::min(kChunk, audio.size() - pos);
        decoder.feedAudio(audio.data() + pos, n);
        decoder.processBuffer();
    }

    CHECK(callback_fired && callback_interleaved && callback_geometry_proven &&
              callback_group_size == kGroup,
          "cw4/Z81 BI1 physical erasure must still reach one proven group callback");
    const auto descriptor = decoder.lastBurstDescriptor();
    CHECK(descriptor.burst_interleave && descriptor.cw_per_frame == kCw &&
              descriptor.lifting_z == kZ,
          "energy-erasure arm must have engaged exact BI1 cw4/Z81 wire geometry");
}

void test_8psk_cw4_z81_bi1_malformed_soft_fails_closed_with_callback() {
    constexpr int kCw = 4;
    constexpr int kGroup = 2;
    constexpr size_t kZ81Bits = static_cast<size_t>(kCw * 1944);
    constexpr size_t kWrongZ27Bits = static_cast<size_t>(kCw * 648);

    StreamingDecoder decoder;
    decoder.setFixedFrameCodewords(kCw);
    decoder.setLDPCLiftingZForTesting(81);
    decoder.setBurstInterleave(true);
    decoder.setBurstInterleaveGroupSize(kGroup);
    CHECK(decoder.burstErasureSoftBitsForTesting() == kZ81Bits,
          "central burst erasure helper must allocate cw4/Z81 as 7776 LLRs");

    bool callback_fired = false;
    bool callback_ok = true;
    bool callback_interleaved = false;
    bool callback_geometry_proven = false;
    uint16_t callback_mask = 0xffff;
    uint8_t callback_group_size = 0;
    size_t callback_frames = 99;
    decoder.setBurstGroupCallback(
        [&](uint16_t, const std::vector<Bytes>& frames, bool all_ok, float,
            uint16_t frame_mask, bool interleaved, uint8_t group_size,
            bool geometry_proven) {
            callback_fired = true;
            callback_ok = all_ok;
            callback_interleaved = interleaved;
            callback_geometry_proven = geometry_proven;
            callback_mask = frame_mask;
            callback_group_size = group_size;
            callback_frames = frames.size();
        });

    std::vector<std::vector<float>> malformed{
        std::vector<float>(kZ81Bits, 0.0f),
        std::vector<float>(kWrongZ27Bits, 0.0f),
    };
    decoder.finalizeMalformedBurstForTesting(std::move(malformed), kGroup);

    CHECK(callback_fired && !callback_ok && callback_interleaved &&
              callback_geometry_proven && callback_mask == 0 &&
              callback_group_size == kGroup && callback_frames == 0,
          "deinterleave size exception must emit one proven 0/N group callback");
}

void test_z81_descriptor_mode_hop_preserves_live_geometry() {
#ifdef _WIN32
    _putenv_s("ULTRA_DESCRIPTOR_MODE_SWITCH", "1");
#else
    setenv("ULTRA_DESCRIPTOR_MODE_SWITCH", "1", /*overwrite=*/1);
#endif
    constexpr Modulation kTargetMod = Modulation::QAM8;
    constexpr CodeRate kTargetRate = CodeRate::R2_3;
    constexpr int kCw = 4;
    constexpr uint8_t kZ = 81;
    constexpr int kGroup = 2;
    const auto target_cfg = makeOFDMConfig(kTargetMod, kTargetRate);

    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(target_cfg);
    encoder.setDataMode(kTargetMod, kTargetRate);
    encoder.setFixedFrameCodewords(kCw);
    encoder.setLDPCLiftingZ(kZ);
    encoder.setBurstInterleave(false);
    encoder.setBurstInterleaveGroupSize(kGroup);
    encoder.setBurstDescriptorEnabled(true);
    encoder.setBurstDescriptorIdentity("ALPHA", "BRAVO");
    encoder.setBurstGroupSeq(31);

    std::vector<Bytes> frames;
    for (uint16_t seq = 0; seq < kGroup; ++seq) {
        frames.push_back(protocol::v2::makeFixedDataFrame(
            "ALPHA", "BRAVO", seq,
            Bytes(48, static_cast<uint8_t>(0x60 + seq)), kTargetRate,
            kCw, kZ).serialize());
    }
    const auto burst = encoder.encodeBurstLight(frames);
    CHECK(!burst.empty(), "Z81 mode-hop burst must encode");

    std::vector<float> audio(48000, 0.0f);
    audio.insert(audio.end(), burst.begin(), burst.end());
    audio.resize(audio.size() + 192000, 0.0f);

    StreamingDecoder decoder;
    const auto source_cfg = makeOFDMConfig(Modulation::QPSK, CodeRate::R3_4);
    decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                 source_cfg, Modulation::QPSK, CodeRate::R3_4);
    decoder.setFixedFrameCodewords(8);
    decoder.setBurstInterleave(false);
    decoder.setBurstInterleaveGroupSize(4);
    decoder.applyPendingConfigForTesting();

    StreamingDecoder z27_reference;
    z27_reference.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                       target_cfg, kTargetMod, kTargetRate);
    z27_reference.applyPendingConfigForTesting();
    const size_t z27_samples =
        z27_reference.waveformFrameSamplesForTesting(kCw);

    bool mode_hop_announced = false;
    decoder.setDescriptorModeChangeCallback(
        [&](Modulation mod, CodeRate rate, int cw) {
            mode_hop_announced =
                mod == kTargetMod && rate == kTargetRate && cw == kCw;
            decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                         target_cfg, mod, rate);
        });

    bool group_fired = false;
    bool group_ok = false;
    uint16_t group_mask = 0;
    decoder.setBurstGroupCallback(
        [&](uint16_t, const std::vector<Bytes>&, bool all_ok, float,
            uint16_t frame_mask, bool, uint8_t, bool) {
            group_fired = true;
            group_ok = all_ok;
            group_mask = frame_mask;
        });

    constexpr size_t kChunk = 4800;
    for (size_t pos = 0; pos < audio.size() && !group_fired; pos += kChunk) {
        const size_t n = std::min(kChunk, audio.size() - pos);
        decoder.feedAudio(audio.data() + pos, n);
        decoder.processBuffer();
    }

    CHECK(mode_hop_announced,
          "Z81 BURST_HEADER must announce the 8PSK R2/3 mode hop");
    CHECK(group_fired && group_ok && group_mask == 0x0003,
          "mode-hop rebuild must retain Z81 long-LDPC geometry through DATA decode");
    CHECK(!decoder.hasBurstDescriptor() && decoder.activeBurstLiftingZ() == 27,
          "successful Z81 group completion must retire descriptor geometry");
    CHECK(decoder.waveformFrameSamplesForTesting(kCw) == z27_samples,
          "post-mode-hop group cleanup must restore live waveform sizing to Z27");
}

void test_z81_timeout_abort_restores_default_geometry_before_callback() {
    constexpr Modulation kMod = Modulation::QAM8;
    constexpr CodeRate kRate = CodeRate::R2_3;
    constexpr int kCw = 4;
    constexpr uint8_t kZ = 81;
    constexpr int kGroup = 2;
    const auto cfg = makeOFDMConfig(kMod, kRate);

    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(cfg);
    encoder.setDataMode(kMod, kRate);
    encoder.setFixedFrameCodewords(kCw);
    encoder.setLDPCLiftingZ(kZ);
    encoder.setBurstInterleave(false);
    encoder.setBurstInterleaveGroupSize(kGroup);
    encoder.setBurstDescriptorEnabled(true);
    encoder.setBurstDescriptorIdentity("ALPHA", "BRAVO");
    encoder.setBurstGroupSeq(32);

    std::vector<Bytes> frames;
    for (uint16_t seq = 0; seq < kGroup; ++seq) {
        frames.push_back(protocol::v2::makeFixedDataFrame(
            "ALPHA", "BRAVO", seq, Bytes(32, 0x72), kRate, kCw, kZ)
                             .serialize());
    }
    const auto burst = encoder.encodeBurstLight(frames);
    CHECK(!burst.empty(), "descriptor-backed Z81 timeout fixture must encode");

    std::vector<float> audio(48000, 0.0f);
    audio.insert(audio.end(), burst.begin(), burst.end());
    audio.resize(audio.size() + 192000, 0.0f);

    StreamingDecoder decoder;
    decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                 cfg, kMod, kRate);
    decoder.setFixedFrameCodewords(12);
    decoder.setBurstInterleave(false);
    decoder.setBurstInterleaveGroupSize(4);
    decoder.applyPendingConfigForTesting();
    const size_t z27_samples = decoder.waveformFrameSamplesForTesting(kCw);

    bool callback_fired = false;
    bool callback_ok = true;
    bool callback_geometry_proven = false;
    int z_at_callback = 0;
    size_t samples_at_callback = 0;
    float cfo_at_callback = 99.0f;
    bool warm_cfo_at_callback = true;
    decoder.setBurstGroupCallback(
        [&](uint16_t, const std::vector<Bytes>&, bool all_ok, float,
            uint16_t, bool, uint8_t, bool geometry_proven) {
            callback_fired = true;
            callback_ok = all_ok;
            callback_geometry_proven = geometry_proven;
            z_at_callback = decoder.activeBurstLiftingZ();
            samples_at_callback = decoder.waveformFrameSamplesForTesting(kCw);
            cfo_at_callback = decoder.getLastCFO();
            warm_cfo_at_callback = decoder.warmCFOCertifiedForTesting();
        });

    bool armed = false;
    constexpr size_t kChunk = 1200;
    for (size_t pos = 0; pos < audio.size() && !armed; pos += kChunk) {
        const size_t n = std::min(kChunk, audio.size() - pos);
        decoder.feedAudio(audio.data() + pos, n);
        decoder.processBuffer();
        armed = decoder.stateForTesting() == DecoderState::BURST_ACCUMULATING;
    }

    CHECK(armed && decoder.hasBurstDescriptor() &&
              decoder.activeBurstLiftingZ() == 81,
          "timeout fixture must first arm from the wire Z81 descriptor");
    CHECK(decoder.waveformFrameSamplesForTesting(kCw) > z27_samples,
          "armed Z81 group must use long live waveform sizing");
    CHECK(decoder.warmCFOCertifiedForTesting(),
          "valid descriptor must establish the timeout fixture's CFO snapshot");

    // A timed-out group may already have ingested unverified pilot residuals.
    // Poison the mutable value exactly like the live partial-group incident and
    // require reconciliation before the synchronous failed-group callback.
    decoder.setKnownCFO(-0.79f);

    decoder.expireBurstTimeoutForTesting();
    const float zero = 0.0f;
    decoder.feedAudio(&zero, 1);
    decoder.processBuffer();

    CHECK(callback_fired && !callback_ok && callback_geometry_proven,
          "descriptor-backed timeout must emit one proven failed-group callback");
    CHECK(z_at_callback == 27 && samples_at_callback == z27_samples,
          "timeout must restore Z27 live geometry before its synchronous callback");
    CHECK(std::abs(cfo_at_callback) < 0.25f && !warm_cfo_at_callback,
          "timeout callback must observe restored numeric CFO with warm trust revoked");
    CHECK(decoder.stateForTesting() == DecoderState::SEARCHING &&
              !decoder.hasBurstDescriptor() &&
              decoder.activeBurstLiftingZ() == 27,
          "timeout cleanup must leave no Z81 descriptor state behind");
}

void test_z81_headnull_abort_restores_default_geometry_before_callback() {
#ifdef _WIN32
    _putenv_s("ULTRA_DESC_ARMED_ACCUM", "0");
#else
    setenv("ULTRA_DESC_ARMED_ACCUM", "0", /*overwrite=*/1);
#endif
    constexpr Modulation kMod = Modulation::QAM8;
    constexpr CodeRate kRate = CodeRate::R2_3;
    constexpr int kCw = 4;
    constexpr uint8_t kZ = 81;
    constexpr int kGroup = 2;
    const auto cfg = makeOFDMConfig(kMod, kRate);

    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(cfg);
    encoder.setDataMode(kMod, kRate);
    encoder.setFixedFrameCodewords(kCw);
    encoder.setLDPCLiftingZ(kZ);
    encoder.setBurstInterleave(false);
    encoder.setBurstInterleaveGroupSize(kGroup);
    encoder.setBurstDescriptorEnabled(true);
    encoder.setBurstDescriptorIdentity("ALPHA", "BRAVO");
    encoder.setBurstGroupSeq(33);

    std::vector<Bytes> frames;
    for (uint16_t seq = 0; seq < kGroup; ++seq) {
        frames.push_back(protocol::v2::makeFixedDataFrame(
            "ALPHA", "BRAVO", seq, Bytes(32, 0x7A), kRate, kCw, kZ)
                             .serialize());
    }
    auto burst = encoder.encodeBurstLight(frames);
    CHECK(!burst.empty(), "descriptor-backed HEADNULL fixture must encode");

    // Remove only the group-start marker: flip the first LTS back to its normal
    // sign while preserving chirp acquisition, training quality, and DATA energy.
    // Every descriptor-declared member then reaches the HEADNULL resync guard
    // instead of arming accumulation.
    const size_t member_samples = static_cast<size_t>(
        encoder.getWaveform()->getMinSamplesForCWCount(kCw));
    const size_t light_preamble =
        encoder.getWaveform()->generateDataPreamble().size();
    const size_t full_preamble = encoder.getWaveform()->generatePreamble().size();
    const size_t symbol_samples = cfg.getSymbolDuration();
    CHECK(member_samples > light_preamble &&
              full_preamble >= 2 * symbol_samples,
          "HEADNULL fixture must expose valid OFDM preamble geometry");
    const size_t data_samples = member_samples - light_preamble;
    const size_t group_samples =
        full_preamble + data_samples + (kGroup - 1) * member_samples;
    CHECK(burst.size() > group_samples,
          "descriptor samples must precede the HEADNULL DATA group");
    const size_t descriptor_samples = burst.size() - group_samples;
    const size_t first_lts =
        descriptor_samples + full_preamble - 2 * symbol_samples;
    CHECK(first_lts + symbol_samples <= burst.size(),
          "HEADNULL first-LTS edit must remain inside the encoded burst");
    for (size_t i = first_lts; i < first_lts + symbol_samples; ++i) {
        burst[i] = -burst[i];
    }

    std::vector<float> audio(48000, 0.0f);
    audio.insert(audio.end(), burst.begin(), burst.end());
    audio.resize(audio.size() + 192000, 0.0f);

    StreamingDecoder decoder;
    decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                 cfg, kMod, kRate);
    decoder.setFixedFrameCodewords(kCw);
    decoder.setBurstInterleave(false);
    decoder.setBurstInterleaveGroupSize(kGroup);
    decoder.applyPendingConfigForTesting();
    const size_t z27_samples = decoder.waveformFrameSamplesForTesting(kCw);

    bool callback_fired = false;
    bool callback_ok = true;
    uint16_t callback_mask = 0xffff;
    int z_at_callback = 0;
    size_t samples_at_callback = 0;
    decoder.setBurstGroupCallback(
        [&](uint16_t, const std::vector<Bytes>&, bool all_ok, float,
            uint16_t frame_mask, bool, uint8_t, bool) {
            callback_fired = true;
            callback_ok = all_ok;
            callback_mask = frame_mask;
            z_at_callback = decoder.activeBurstLiftingZ();
            samples_at_callback = decoder.waveformFrameSamplesForTesting(kCw);
        });

    constexpr size_t kChunk = 2400;
    for (size_t pos = 0; pos < audio.size() && !callback_fired; pos += kChunk) {
        const size_t n = std::min(kChunk, audio.size() - pos);
        decoder.feedAudio(audio.data() + pos, n);
        decoder.processBuffer();
    }

    CHECK(callback_fired && !callback_ok && callback_mask == 0,
          "exhausted HEADNULL group must emit its failed-group backstop");
    CHECK(z_at_callback == 27 && samples_at_callback == z27_samples,
          "HEADNULL backstop must restore Z27 live geometry before callback");
    CHECK(!decoder.hasBurstDescriptor() && decoder.activeBurstLiftingZ() == 27,
          "HEADNULL cleanup must leave no Z81 descriptor state behind");
}

void test_decoder_lifecycle_clears_anchored_backstop_epoch() {
    StreamingDecoder decoder;

    decoder.armAnchoredBurstBackstopForTesting(987654);
    CHECK(decoder.anchoredBurstBackstopArmedForTesting(),
          "fixture should arm the anchored no-group backstop");
    CHECK(decoder.anchoredBurstBackstopArmAbsForTesting() == 987654,
          "fixture should retain the old receive epoch's absolute sample identity");

    decoder.reset();
    CHECK(!decoder.anchoredBurstBackstopArmedForTesting(),
          "sample-clock reset must disarm the anchored no-group backstop");
    CHECK(decoder.anchoredBurstBackstopArmAbsForTesting() == 0,
          "sample-clock reset must clear the stale absolute arm timestamp");

    decoder.armAnchoredBurstBackstopForTesting(456789);
    decoder.setMode(protocol::WaveformMode::OFDM_CHIRP, true);
    CHECK(!decoder.anchoredBurstBackstopArmedForTesting(),
          "waveform/session transition must disarm the old anchored backstop");
    CHECK(decoder.anchoredBurstBackstopArmAbsForTesting() == 0,
          "waveform/session transition must clear the old anchor epoch");

    // A connection-state-only transition takes the same setMode lifecycle path.
    decoder.armAnchoredBurstBackstopForTesting(123456);
    decoder.setMode(protocol::WaveformMode::OFDM_CHIRP, false);
    CHECK(!decoder.anchoredBurstBackstopArmedForTesting(),
          "connection epoch transition must disarm the anchored backstop");
    CHECK(decoder.anchoredBurstBackstopArmAbsForTesting() == 0,
          "connection epoch transition must clear the absolute arm timestamp");
}

void test_control_only_teardown_accepts_control_and_rejects_data_fallback() {
    constexpr Modulation kDataMod = Modulation::QAM8;
    constexpr CodeRate kDataRate = CodeRate::R2_3;
    constexpr int kDataCw = 12;
    const auto cfg = makeOFDMConfig(kDataMod, kDataRate);

    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(cfg);
    encoder.setDataMode(kDataMod, kDataRate);
    encoder.setFixedFrameCodewords(kDataCw);

    const Bytes data = protocol::v2::makeFixedDataFrame(
        "ALPHA", "BRAVO", 9, Bytes(64, 0x5A), kDataRate, kDataCw).serialize();
    const Bytes ack = protocol::v2::ControlFrame::makeAck(
        "ALPHA", "BRAVO", 9).serialize();
    const auto data_tx = encoder.encodeFrame(data);
    const auto ack_tx = encoder.encodeFrame(ack);
    CHECK(!data_tx.empty() && !ack_tx.empty(),
          "teardown fixture must encode both DATA and hardened control frames");

    auto feed_one = [&](const std::vector<float>& tx, int& data_callbacks,
                        int& ack_callbacks) {
        StreamingDecoder decoder;
        decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                     cfg, kDataMod, kDataRate);
        decoder.setFixedFrameCodewords(kDataCw);
        decoder.applyPendingConfigForTesting();
        decoder.setControlOnlyReceive(true);
        decoder.expectFullOFDMAnchorOnce();
        decoder.setFrameCallback([&](const DecodeResult& result) {
            if (!result.success) return;
            const auto hdr = protocol::v2::parseHeader(result.frame_data);
            if (!hdr.valid) return;
            if (hdr.type == protocol::v2::FrameType::DATA) {
                ++data_callbacks;
            } else if (protocol::v2::isControlFrame(hdr.type)) {
                ++ack_callbacks;
            }
        });

        std::vector<float> audio(48000, 0.0f);
        audio.insert(audio.end(), tx.begin(), tx.end());
        audio.resize(audio.size() + 192000, 0.0f);
        constexpr size_t kChunk = 4800;
        for (size_t pos = 0; pos < audio.size(); pos += kChunk) {
            const size_t n = std::min(kChunk, audio.size() - pos);
            decoder.feedAudio(audio.data() + pos, n);
            decoder.processBuffer();
        }
        CHECK(decoder.controlOnlyReceiveForTesting(),
              "teardown control-only receive gate must remain armed");
    };

    int data_callbacks = 0;
    int ack_callbacks = 0;
    feed_one(data_tx, data_callbacks, ack_callbacks);
    CHECK(data_callbacks == 0 && ack_callbacks == 0,
          "a valid connected DATA waveform must not fall through during teardown");

    feed_one(ack_tx, data_callbacks, ack_callbacks);
    CHECK(data_callbacks == 0 && ack_callbacks == 1,
          "teardown must still decode the hardened QPSK R1/4 control profile");
}

void test_control_only_transition_retires_stale_data_wait_for_disconnect_ack() {
    constexpr Modulation kDataMod = Modulation::QAM8;
    constexpr CodeRate kDataRate = CodeRate::R2_3;
    constexpr int kDataCw = 12;
    const auto cfg = makeOFDMConfig(kDataMod, kDataRate);

    StreamingEncoder encoder;
    encoder.setMode(protocol::WaveformMode::OFDM_CHIRP);
    encoder.setOFDMConfig(cfg);
    encoder.setDataMode(kDataMod, kDataRate);
    encoder.setFixedFrameCodewords(kDataCw);

    const Bytes data = protocol::v2::makeFixedDataFrame(
        "ALPHA", "BRAVO", 41, Bytes(64, 0xA5), kDataRate, kDataCw).serialize();
    const Bytes disconnect_ack = protocol::v2::ControlFrame::makeAck(
        "ALPHA", "BRAVO", protocol::v2::DISCONNECT_SEQ).serialize();
    const auto data_tx = encoder.encodeFrameLight(data);
    const auto ack_light_reference = encoder.encodeFrameLight(disconnect_ack);
    const auto ack_tx = encoder.encodeFrame(disconnect_ack);
    CHECK(!data_tx.empty() && !ack_light_reference.empty() && !ack_tx.empty(),
          "transition fixture must encode DATA and hardened disconnect ACK");

    StreamingDecoder decoder;
    decoder.setConnectedOFDMMode(protocol::WaveformMode::OFDM_CHIRP,
                                 cfg, kDataMod, kDataRate);
    decoder.setFixedFrameCodewords(kDataCw);
    decoder.applyPendingConfigForTesting();
    decoder.clearFullOFDMAnchorExpectation();

    int data_callbacks = 0;
    int disconnect_ack_callbacks = 0;
    decoder.setFrameCallback([&](const DecodeResult& result) {
        if (!result.success) return;
        const auto hdr = protocol::v2::parseHeader(result.frame_data);
        if (!hdr.valid) return;
        if (hdr.type == protocol::v2::FrameType::DATA) {
            ++data_callbacks;
        } else if (hdr.type == protocol::v2::FrameType::ACK &&
                   hdr.seq == protocol::v2::DISCONNECT_SEQ) {
            ++disconnect_ack_callbacks;
        }
    });

    // Drive one decoder through a connected DATA control-first rejection and
    // stop as soon as its fixed-CW continuation is armed.  The remainder of the
    // DATA frame is deliberately never supplied: this models teardown beginning
    // while the decoder is waiting on stale DATA geometry.
    std::vector<float> candidate_audio(2400, 0.0f);
    const size_t candidate_prefix =
        std::min(data_tx.size(), ack_light_reference.size());
    candidate_audio.insert(candidate_audio.end(), data_tx.begin(),
                           data_tx.begin() + candidate_prefix);
    candidate_audio.resize(candidate_audio.size() + 4800, 0.0f);
    constexpr size_t kChunk = 2400;
    decoder.feedAudio(candidate_audio.data(), candidate_audio.size());
    decoder.processBuffer();

    // Search may need a few state-machine wakeups after the deliberately short
    // candidate stops.  One-sample ticks advance the decoder without supplying
    // anything close to the missing fixed-CW body.
    const float silence = 0.0f;
    for (int tick = 0;
         tick < 128 && decoder.pendingTotalCodewordsForTesting() == 0;
         ++tick) {
        decoder.feedAudio(&silence, 1);
        decoder.processBuffer();
    }
    CHECK(decoder.pendingTotalCodewordsForTesting() == kDataCw &&
              decoder.stateForTesting() == DecoderState::SYNC_FOUND,
          "connected DATA candidate must arm the 12-CW continuation before teardown");
    CHECK(data_callbacks == 0 && disconnect_ack_callbacks == 0,
          "truncated DATA candidate must not emit a frame callback");

    decoder.setControlOnlyReceive(true);

    // Two one-sample wakeups are enough to cross readiness and reject the old
    // candidate as illegal teardown traffic.  This assertion is the regression:
    // the decoder must not wait for the missing 12-CW DATA body before it can
    // return to SEARCHING for the disconnect ACK.
    decoder.feedAudio(&silence, 1);
    decoder.processBuffer();
    CHECK(decoder.pendingTotalCodewordsForTesting() == 0 &&
              decoder.stateForTesting() == DecoderState::DECODING,
          "control-only transition must retire stale DATA geometry at readiness");
    decoder.feedAudio(&silence, 1);
    decoder.processBuffer();
    CHECK(decoder.stateForTesting() == DecoderState::SEARCHING,
          "failed pre-transition DATA candidate must return directly to control search");
    CHECK(data_callbacks == 0 && disconnect_ack_callbacks == 0,
          "control-only transition must not surface the failed DATA candidate");

    decoder.expectFullOFDMAnchorOnce();
    std::vector<float> teardown_audio(48000, 0.0f);
    teardown_audio.insert(teardown_audio.end(), ack_tx.begin(), ack_tx.end());
    teardown_audio.resize(teardown_audio.size() + 192000, 0.0f);
    for (size_t pos = 0; pos < teardown_audio.size(); pos += kChunk) {
        const size_t n = std::min(kChunk, teardown_audio.size() - pos);
        decoder.feedAudio(teardown_audio.data() + pos, n);
        decoder.processBuffer();
    }

    CHECK(data_callbacks == 0,
          "teardown transition must never fall back to DATA delivery");
    CHECK(disconnect_ack_callbacks == 1,
          "same decoder must deliver the hardened disconnect ACK exactly once");
    CHECK(decoder.controlOnlyReceiveForTesting(),
          "control-only receive must remain armed throughout teardown");
}

std::vector<float> makeEvmLifecycleFrame(const ModemConfig& cfg,
                                         float noise_amplitude,
                                         uint32_t seed) {
    OFDMChirpWaveform tx(cfg);
    Bytes encoded(81);  // one Z27 LDPC-sized air block
    for (size_t i = 0; i < encoded.size(); ++i) {
        encoded[i] = static_cast<uint8_t>(0x31u + 37u * i);
    }
    auto samples = tx.generateDataPreamble();
    const auto data = tx.modulate(encoded);
    samples.insert(samples.end(), data.begin(), data.end());

    // Deterministic zero-mean broadband perturbation. It is deliberately added
    // after TX generation so the two fixtures differ only in received scatter.
    for (float& sample : samples) {
        seed = seed * 1664525u + 1013904223u;
        const float u = static_cast<float>((seed >> 8) & 0x00ffffffu) /
                        static_cast<float>(0x01000000u);
        sample += noise_amplitude * (2.0f * u - 1.0f);
    }
    return samples;
}

void test_evm_is_finalized_and_consumed_on_its_own_frame() {
    const auto cfg = makeOFDMConfig(Modulation::QPSK, CodeRate::R1_2);
    const auto clean = makeEvmLifecycleFrame(cfg, 0.0002f, 0x13579BDFu);
    const auto noisy = makeEvmLifecycleFrame(cfg, 0.5000f, 0x2468ACE1u);

    OFDMChirpWaveform rx(cfg);
    CHECK(rx.process(SampleSpan(clean.data(), clean.size())),
          "clean EVM lifecycle frame must finish demodulation");
    float clean_db = 0.0f;
    size_t clean_carriers = 0;
    CHECK(rx.takeEvmSnrEstimate(clean_db, clean_carriers) &&
              std::isfinite(clean_db) && clean_carriers > 0,
          "frame 1 must publish its finalized post-EQ EVM immediately");
    float duplicate_db = 0.0f;
    size_t duplicate_carriers = 0;
    CHECK(!rx.takeEvmSnrEstimate(duplicate_db, duplicate_carriers),
          "a finalized frame-local EVM observation must be one-shot");

    CHECK(rx.process(SampleSpan(noisy.data(), noisy.size())),
          "noisy EVM lifecycle frame must finish demodulation");
    float noisy_db = 0.0f;
    size_t noisy_carriers = 0;
    CHECK(rx.takeEvmSnrEstimate(noisy_db, noisy_carriers) &&
              std::isfinite(noisy_db) && noisy_carriers == clean_carriers,
          "frame 2 must publish its own finalized EVM with matching geometry");
    CHECK(clean_db > noisy_db + 1.0f,
          "immediate EVM readings must align clean frame 1 above noisy frame 2");

    // Leave a fresh value unread, then begin an invalid pass. The invalid pass
    // must revoke it before returning: erasure/no-process callers cannot borrow
    // the preceding frame's metric.
    CHECK(rx.process(SampleSpan(clean.data(), clean.size())),
          "third EVM lifecycle frame must finish demodulation");
    const float too_short = 0.0f;
    CHECK(!rx.process(SampleSpan(&too_short, 1)),
          "too-short EVM lifecycle pass must fail demodulation");
    CHECK(!rx.takeEvmSnrEstimate(duplicate_db, duplicate_carriers),
          "failed pass must invalidate an unread predecessor observation");
}

void test_group_evm_requires_complete_coverage_and_includes_tail() {
    StreamingDecoder decoder;
    std::vector<DecodeResult> metrics(3);
    const float db[] = {20.0f, 14.0f, 5.0f};
    const size_t carriers[] = {100, 200, 400};
    double weighted_error = 0.0;
    size_t total_carriers = 0;
    for (size_t i = 0; i < metrics.size(); ++i) {
        metrics[i].has_evm_snr_db = true;
        metrics[i].evm_snr_db = db[i];
        metrics[i].evm_carrier_count = carriers[i];
        weighted_error += static_cast<double>(carriers[i]) *
                          std::pow(10.0, -static_cast<double>(db[i]) / 10.0);
        total_carriers += carriers[i];
    }
    const float expected = static_cast<float>(
        -10.0 * std::log10(weighted_error / static_cast<double>(total_carriers)));

    decoder.finalizeGroupEvmForTesting(metrics, /*geometry_proven=*/true, 3);
    CHECK(decoder.hasLastEvmSnr() &&
              std::abs(decoder.getLastEvmSnrDb() - expected) < 1.0e-4f,
          "group EVM must pool every fresh frame in linear error-energy space");
    const auto& frame_db = decoder.getLastGroupEvmSnrFrames();
    const auto& frame_counts = decoder.getLastGroupEvmCarrierCounts();
    CHECK(frame_db.size() == 3 && frame_counts.size() == 3 &&
              frame_db[2] == db[2] && frame_counts[2] == carriers[2],
          "group EVM coverage must retain the finalized tail observation");
    const float without_tail = static_cast<float>(-10.0 * std::log10(
        (100.0 * std::pow(10.0, -2.0) +
         200.0 * std::pow(10.0, -1.4)) / 300.0));
    CHECK(std::abs(decoder.getLastEvmSnrDb() - without_tail) > 1.0f,
          "group EVM scalar must materially include, not omit, the tail frame");

    auto missing_tail = metrics;
    missing_tail.pop_back();
    decoder.finalizeGroupEvmForTesting(missing_tail, /*geometry_proven=*/true, 3);
    CHECK(!decoder.hasLastEvmSnr(),
          "missing physical tail must invalidate the group EVM scalar");

    auto erased_tail = metrics;
    erased_tail.back().has_evm_snr_db = false;
    erased_tail.back().evm_carrier_count = 0;
    decoder.finalizeGroupEvmForTesting(erased_tail, /*geometry_proven=*/true, 3);
    CHECK(!decoder.hasLastEvmSnr(),
          "no-process tail erasure must invalidate rather than survivor-average EVM");

    decoder.finalizeGroupEvmForTesting(metrics, /*geometry_proven=*/false, 3);
    CHECK(!decoder.hasLastEvmSnr(),
          "unproven group geometry must not publish an EVM scalar");
}

}  // namespace

int main() {
    setLogLevel(LogLevel::ERROR);

    test_coherent_ofdm_config_match();
    test_burst_group_clamps_match();
    test_decoder_buffer_capacity_policy();
    test_forced_full_preamble_is_one_shot();
    test_multi_frame_ofdm_burst_starts_with_full_anchor();
    test_single_frame_ofdm_burst_uses_full_anchor();
    test_connected_ofdm_config_arms_full_anchor();
    test_burst_header_current_anchor_controls_receiver_search();
    test_per_request_full_anchor_matches_descriptor_and_phy();
    test_repair_burst_recovers_weaker_descriptor_before_stronger_full_group();
    test_measurement_lifting_z_override_survives_reset();
    test_connected_ofdm_r34_cw3_z81_waits_for_full_frame();
    test_burst_descriptor_regrids_equal_capacity_8psk_profiles();
    test_8psk_cw4_z81_bi1_energy_erasure_reaches_group_callback();
    test_8psk_cw4_z81_bi1_malformed_soft_fails_closed_with_callback();
    test_z81_descriptor_mode_hop_preserves_live_geometry();
    test_z81_timeout_abort_restores_default_geometry_before_callback();
    test_z81_headnull_abort_restores_default_geometry_before_callback();
    test_decoder_lifecycle_clears_anchored_backstop_epoch();
    test_control_only_teardown_accepts_control_and_rejects_data_fallback();
    test_control_only_transition_retires_stale_data_wait_for_disconnect_ack();
    test_evm_is_finalized_and_consumed_on_its_own_frame();
    test_group_evm_requires_complete_coverage_and_includes_tail();

    if (tests_failed != 0) {
        std::cout << "StreamingConfig: " << (tests_run - tests_failed)
                  << "/" << tests_run << " passed\n";
        return 1;
    }

    std::cout << "StreamingConfig: " << tests_run << "/" << tests_run << " passed\n";
    return 0;
}
