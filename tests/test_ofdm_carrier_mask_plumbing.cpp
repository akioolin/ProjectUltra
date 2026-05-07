#include "waveform/ofdm_chirp_waveform.hpp"
#include "fec/carrier_ldpc_interleaver.hpp"
#include "ultra/ofdm_link_adaptation.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <algorithm>
#include <array>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ultra;

namespace {

constexpr size_t kLdpcBits = 648;
constexpr size_t kLdpcBytes = kLdpcBits / 8;
constexpr int kMaskCarrier = 31;  // 30 is a pilot in the current DQPSK R1/2 profile.

int tests_passed = 0;
int tests_failed = 0;

#define TEST(name) \
    std::cout << "Testing " << name << "... "; \
    try

#define PASS() \
    std::cout << "PASS\n"; \
    ++tests_passed

#define FAIL(msg) \
    std::cout << "FAIL: " << msg << "\n"; \
    ++tests_failed

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ModemConfig makeWideChirpConfig() {
    ModemConfig cfg;
    cfg.fft_size = 1024;
    cfg.num_carriers = 59;
    cfg.sample_rate = 48000;
    cfg.center_freq = 1500.0f;
    cfg.cp_mode = CyclicPrefixMode::LONG;
    cfg.symbol_guard = 0;
    cfg.modulation = Modulation::DQPSK;
    cfg.code_rate = CodeRate::R1_2;
    cfg.use_pilots = true;
    cfg.pilot_spacing = ofdm_link_adaptation::recommendedPilotSpacing(
        cfg.modulation, cfg.code_rate);
    return cfg;
}

std::vector<size_t> dataLogicalCarrierIndices(const ModemConfig& cfg) {
    std::vector<size_t> out;
    const int neg_limit = static_cast<int>(cfg.num_carriers / 2);
    const int pos_limit = static_cast<int>((cfg.num_carriers + 1) / 2);
    int pilot_count = 0;
    size_t logical = 0;

    for (int i = -neg_limit; i <= pos_limit; ++i) {
        if (i == 0) {
            continue;
        }
        const bool is_pilot = cfg.use_pilots && cfg.pilot_spacing != 0 &&
            (pilot_count % static_cast<int>(cfg.pilot_spacing) == 0);
        if (!is_pilot) {
            out.push_back(logical);
        }
        ++pilot_count;
        ++logical;
    }
    return out;
}

Bytes randomEncodedCodewords(size_t codewords) {
    Bytes bytes(codewords * kLdpcBytes);
    std::mt19937 rng(0xC0DEC0DEu + static_cast<uint32_t>(codewords));
    for (uint8_t& b : bytes) {
        b = static_cast<uint8_t>(rng() & 0xFFu);
    }
    return bytes;
}

Samples transmitDataPreambleFrame(OFDMChirpWaveform& tx, const Bytes& encoded) {
    Samples audio = tx.generateDataPreamble();
    Samples data = tx.modulate(encoded);
    audio.insert(audio.end(), data.begin(), data.end());
    return audio;
}

std::vector<float> processFrame(OFDMChirpWaveform& rx, const Samples& audio) {
    rx.setFrequencyOffset(0.0f);
    rx.setAbsoluteTrainingPosition(0);
    require(rx.process(SampleSpan(audio.data(), audio.size())),
            "receiver did not process the clean frame");
    auto llrs = rx.getSoftBits();
    require(!llrs.empty(), "receiver returned no LLRs");
    return llrs;
}

std::vector<bool> expectedErasedSlots(size_t codeword_count,
                                      const ModemConfig& cfg,
                                      int masked_logical_carrier) {
    const std::vector<size_t> data_logical = dataLogicalCarrierIndices(cfg);
    const size_t bits_per_carrier = getBitsPerSymbol(cfg.modulation);
    const auto interleaver = ultra::fec::buildCarrierInterleaverV1(codeword_count);

    std::vector<bool> erased(codeword_count * kLdpcBits, false);
    for (size_t i = 0; i < erased.size(); ++i) {
        const ultra::fec::AirGridIndex grid =
            ultra::fec::decomposeAirIndex(interleaver[i],
                                          data_logical.size(),
                                          bits_per_carrier);
        if (grid.carrier < data_logical.size() &&
            data_logical[grid.carrier] == static_cast<size_t>(masked_logical_carrier)) {
            erased[i] = true;
        }
    }
    return erased;
}

void test_all_on_mask_is_exact_noop() {
    TEST("all-on carrier mask is an exact OFDM_CHIRP production-path no-op") {
        const ModemConfig cfg = makeWideChirpConfig();
        const Bytes encoded = randomEncodedCodewords(4);

        OFDMChirpWaveform baseline_tx(cfg);
        baseline_tx.configure(cfg.modulation, cfg.code_rate);
        const Samples baseline_audio = transmitDataPreambleFrame(baseline_tx, encoded);

        OFDMChirpWaveform masked_tx(cfg);
        masked_tx.configure(cfg.modulation, cfg.code_rate);
        masked_tx.setCarrierMask(UINT64_MAX);
        const Samples masked_audio = transmitDataPreambleFrame(masked_tx, encoded);

        require(baseline_audio == masked_audio,
                "all-on mask changed TX samples");

        OFDMChirpWaveform baseline_rx(cfg);
        baseline_rx.configure(cfg.modulation, cfg.code_rate);
        auto baseline_llrs = processFrame(baseline_rx, baseline_audio);

        OFDMChirpWaveform masked_rx(cfg);
        masked_rx.configure(cfg.modulation, cfg.code_rate);
        masked_rx.setCarrierMask(UINT64_MAX);
        auto masked_llrs = processFrame(masked_rx, masked_audio);

        require(baseline_llrs == masked_llrs,
                "all-on mask changed RX LLR vector");
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_single_masked_carrier_zeros_tx_and_rx_slots() {
    TEST("single masked carrier zeros TX grid and exact RX CarrierLDPC slots") {
        const ModemConfig cfg = makeWideChirpConfig();
        const uint64_t mask = UINT64_MAX & ~(uint64_t{1} << kMaskCarrier);
        const std::vector<size_t> data_logical = dataLogicalCarrierIndices(cfg);
        const auto it = std::find(data_logical.begin(), data_logical.end(),
                                  static_cast<size_t>(kMaskCarrier));
        require(it != data_logical.end(), "chosen mask carrier is not a data carrier");
        const size_t masked_data_ordinal =
            static_cast<size_t>(std::distance(data_logical.begin(), it));
        constexpr std::array<size_t, 3> kCases = {{2, 4, 8}};

        for (size_t codewords : kCases) {
            const Bytes encoded = randomEncodedCodewords(codewords);

            OFDMChirpWaveform tx(cfg);
            tx.configure(cfg.modulation, cfg.code_rate);
            tx.setCarrierMask(mask);
            const Samples audio = transmitDataPreambleFrame(tx, encoded);

            const Symbol tx_grid = tx.getLastDataCarrierSymbolsForTesting();
            require(!tx_grid.empty(), "TX debug grid is empty");
            require(tx_grid.size() % data_logical.size() == 0,
                    "TX debug grid is not symbol-aligned");
            for (size_t pos = masked_data_ordinal; pos < tx_grid.size();
                 pos += data_logical.size()) {
                require(tx_grid[pos] == Complex(0, 0),
                        "masked carrier emitted a non-zero TX symbol");
            }

            OFDMChirpWaveform rx(cfg);
            rx.configure(cfg.modulation, cfg.code_rate);
            rx.setCarrierMask(mask);
            const std::vector<float> llrs = processFrame(rx, audio);
            require(llrs.size() == codewords * kLdpcBits,
                    "unexpected RX LLR count");

            const std::vector<bool> expected =
                expectedErasedSlots(codewords, cfg, kMaskCarrier);
            size_t expected_zero = 0;
            size_t observed_zero = 0;
            for (size_t i = 0; i < expected.size(); ++i) {
                if (expected[i]) {
                    ++expected_zero;
                    require(llrs[i] == 0.0f,
                            "expected erasure slot is not literal zero");
                }
                if (llrs[i] == 0.0f) {
                    ++observed_zero;
                }
            }
            require(expected_zero > 0, "mask did not touch any coded slots");
            require(observed_zero == expected_zero,
                    "RX zero LLR count does not match CarrierLDPC geometry");
        }
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_carrier_ldpc_all_on_roundtrip() {
    TEST("CarrierLDPC enabled all-on round-trips hard decisions") {
        const ModemConfig cfg = makeWideChirpConfig();
        const Bytes encoded = randomEncodedCodewords(4);

        OFDMChirpWaveform baseline_tx(cfg);
        baseline_tx.configure(cfg.modulation, cfg.code_rate);
        const Samples baseline_audio = transmitDataPreambleFrame(baseline_tx, encoded);

        OFDMChirpWaveform baseline_rx(cfg);
        baseline_rx.configure(cfg.modulation, cfg.code_rate);
        const std::vector<float> baseline_llrs = processFrame(baseline_rx, baseline_audio);

        OFDMChirpWaveform interleaved_tx(cfg);
        interleaved_tx.configure(cfg.modulation, cfg.code_rate);
        interleaved_tx.setCarrierLdpcInterleaverEnabled(true);
        const Samples interleaved_audio = transmitDataPreambleFrame(interleaved_tx, encoded);

        OFDMChirpWaveform interleaved_rx(cfg);
        interleaved_rx.configure(cfg.modulation, cfg.code_rate);
        interleaved_rx.setCarrierLdpcInterleaverEnabled(true);
        const std::vector<float> interleaved_llrs = processFrame(interleaved_rx, interleaved_audio);

        require(interleaved_llrs.size() == baseline_llrs.size(),
                "CarrierLDPC changed LLR count");
        for (size_t i = 0; i < baseline_llrs.size(); ++i) {
            require(baseline_llrs[i] != 0.0f && interleaved_llrs[i] != 0.0f,
                    "clean all-on CarrierLDPC path produced an erasure");
            require((baseline_llrs[i] > 0.0f) == (interleaved_llrs[i] > 0.0f),
                    "CarrierLDPC forward/inverse hard decision mismatch");
        }
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

void test_ncw1_mask_bypass() {
    TEST("Ncw=1 ignores non-default mask and preserves legacy ordering") {
        const ModemConfig cfg = makeWideChirpConfig();
        const uint64_t mask = UINT64_MAX & ~(uint64_t{1} << kMaskCarrier);
        const Bytes encoded = randomEncodedCodewords(1);
        const std::vector<size_t> data_logical = dataLogicalCarrierIndices(cfg);
        const auto it = std::find(data_logical.begin(), data_logical.end(),
                                  static_cast<size_t>(kMaskCarrier));
        require(it != data_logical.end(), "chosen mask carrier is not a data carrier");
        const size_t masked_data_ordinal =
            static_cast<size_t>(std::distance(data_logical.begin(), it));

        OFDMChirpWaveform tx(cfg);
        tx.configure(cfg.modulation, cfg.code_rate);
        tx.setCarrierMask(mask);
        const Samples audio = transmitDataPreambleFrame(tx, encoded);

        const Symbol tx_grid = tx.getLastDataCarrierSymbolsForTesting();
        require(masked_data_ordinal < tx_grid.size(), "TX grid too short");
        require(std::abs(tx_grid[masked_data_ordinal]) > 0.5f,
                "Ncw=1 mask was not bypassed on TX");

        OFDMChirpWaveform rx(cfg);
        rx.configure(cfg.modulation, cfg.code_rate);
        rx.setCarrierMask(mask);
        const std::vector<float> llrs = processFrame(rx, audio);
        require(llrs.size() == kLdpcBits, "unexpected Ncw=1 LLR count");

        size_t zeros = 0;
        for (float llr : llrs) {
            if (llr == 0.0f) {
                ++zeros;
            }
        }
        require(zeros == 0, "Ncw=1 RX inserted mask erasures");
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }
}

}  // namespace

int main() {
    std::cout << "=== OFDM Carrier Mask Plumbing Tests ===\n\n";

    test_all_on_mask_is_exact_noop();
    test_single_masked_carrier_zeros_tx_and_rx_slots();
    test_carrier_ldpc_all_on_roundtrip();
    test_ncw1_mask_bypass();

    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << tests_passed << "\n";
    std::cout << "Failed: " << tests_failed << "\n";

    return tests_failed == 0 ? 0 : 1;
}
