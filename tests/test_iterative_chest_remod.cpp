// =============================================================================
// ULTRA_ITERATIVE_CHEST — RE-MODULATION ROUND-TRIP (the load-bearing correctness pin)
// =============================================================================
//
// Post-FEC data-aided channel estimation computes H = Y / X, where X is recovered
// by RE-ENCODING a decoded frame and replaying the transmit chain. Everything the
// feature does rests on one claim:
//
//     encode -> transmit -> decode -> re-encode -> re-modulate  ==  the original X
//
// If that is off by one bit, one interleaver permutation, one pad byte, one carrier
// rotation or one CarrierLDPC decision, then H = Y/X is a CONFIDENT, GEOMETRY-VALID,
// COMPLETELY WRONG channel — the exact failure that produced months of bogus genie
// numbers (2026-07-28: a nearest-symbol alias gave zero residual, correct geometry
// and content agreement 0.25 = chance). Parity gating cannot catch it, because the
// bits are right and the MAPPING is wrong. So it is pinned here, at zero channel
// cost, before the path is ever wired to the estimator.
//
// The comparison is EXACT complex equality, not a tolerance: both sides run the very
// same mapBits() on the very same floats, so any difference at all is a bug rather
// than a numerical question.
//
// Coverage is the whole waveform family, not the 16QAM cell that motivated the work
// (CLAUDE.md adaptivity rule; BUG-8PSK-001 is what a modulation-specific path costs):
//   {QPSK, 8PSK, 16QAM} x {R1/2, R2/3, R3/4} x {1, 2, 4, 8 codewords}
//   x {Z=27, Z=81} x {channel interleave on/off} x {CarrierLDPC on/off}
//   x {full payload, short payload (exercises the PRBS pad regeneration)}
//
// Negative and partial cases prove the test has teeth.

#include "waveform/ofdm_chirp_waveform.hpp"
#include "protocol/frame_v2.hpp"
#include "ultra/types.hpp"
#include "ultra/ofdm_link_adaptation.hpp"
#include "ofdm/iterative_chest.hpp"

#include <cstdlib>

#include <cstdint>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ultra;
namespace v2 = ultra::protocol::v2;

namespace {

int tests_passed = 0;
int tests_failed = 0;

#define TEST(name)                              \
    std::cout << "Testing " << name << "... "; \
    try

#define PASS()          \
    std::cout << "PASS\n"; \
    ++tests_passed

#define FAIL(msg)                        \
    std::cout << "FAIL: " << msg << "\n"; \
    ++tests_failed

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Production wideband OFDM_CHIRP geometry (59 carriers / FFT 1024), which is also
// what makes the CarrierLDPC plumbing eligible.
ModemConfig makeConfig(Modulation mod, CodeRate rate) {
    ModemConfig cfg;
    cfg.fft_size = 1024;
    cfg.num_carriers = 59;
    cfg.sample_rate = 48000;
    cfg.center_freq = 1500.0f;
    cfg.cp_mode = CyclicPrefixMode::MEDIUM;  // production; the grid is CP-independent
    cfg.symbol_guard = 0;
    cfg.modulation = mod;
    cfg.code_rate = rate;
    cfg.use_pilots = true;
    return cfg;
}

std::vector<float> bytesToSoftBits(const Bytes& encoded) {
    std::vector<float> soft;
    soft.reserve(encoded.size() * 8);
    for (uint8_t byte : encoded) {
        for (int b = 7; b >= 0; --b) {
            soft.push_back(((byte >> b) & 1) ? -5.0f : 5.0f);
        }
    }
    return soft;
}

const char* modName(Modulation m) {
    switch (m) {
        case Modulation::QPSK: return "QPSK";
        case Modulation::QAM8: return "8PSK";
        case Modulation::QAM16: return "16QAM";
        default: return "?";
    }
}

const char* rateName(CodeRate r) {
    switch (r) {
        case CodeRate::R1_2: return "R1/2";
        case CodeRate::R2_3: return "R2/3";
        case CodeRate::R3_4: return "R3/4";
        default: return "?";
    }
}

// The receiver derives its data-carrier count exactly this way
// (streaming_decoder.cpp:893), and the waveform reconfigures pilot spacing per code
// rate in configure(), so this must be read AFTER configure().
size_t dataCarriers(const OFDMChirpWaveform& w) {
    const ModemConfig& cfg = w.getConfig();
    return static_cast<size_t>(ofdm_link_adaptation::dataCarrierCount(
        static_cast<int>(cfg.num_carriers), cfg.use_pilots,
        static_cast<int>(cfg.pilot_spacing)));
}

struct Case {
    Modulation mod;
    CodeRate rate;
    int cw_count;
    int lifting_z;
    bool channel_interleave;
    bool carrier_ldpc;
    bool short_payload;
};

std::string describe(const Case& c) {
    std::ostringstream os;
    os << modName(c.mod) << " " << rateName(c.rate) << " cw=" << c.cw_count
       << " z=" << c.lifting_z << " chan_il=" << (c.channel_interleave ? 1 : 0)
       << " carrier_ldpc=" << (c.carrier_ldpc ? 1 : 0)
       << " payload=" << (c.short_payload ? "short" : "full");
    return os.str();
}

// Build a real DataFrame sized for this rung, serialise it, and return the bytes
// the transmitter would hand to encodeFixedFrame().
Bytes buildFrameBytes(const Case& c, std::mt19937& rng) {
    const size_t bytes_per_cw =
        v2::getBytesPerCodewordZ(c.rate, c.lifting_z);
    const size_t capacity = bytes_per_cw * static_cast<size_t>(c.cw_count);
    require(capacity > v2::DataFrame::HEADER_SIZE + v2::DataFrame::CRC_SIZE,
            "rung capacity too small for a data frame");
    const size_t max_payload =
        capacity - v2::DataFrame::HEADER_SIZE - v2::DataFrame::CRC_SIZE;
    // A SHORT payload leaves the encoder to regenerate the deterministic PRBS pad on
    // both passes — the single most likely place for a re-encode to diverge.
    const size_t payload_len = c.short_payload ? (max_payload / 4 + 1) : max_payload;

    Bytes payload(payload_len);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& b : payload) b = static_cast<uint8_t>(byte_dist(rng));

    v2::DataFrame frame;
    frame.type = v2::FrameType::DATA;
    frame.seq = 7;
    frame.src_hash = 0x0A0B0C;
    frame.dst_hash = 0x010203;
    frame.total_cw = static_cast<uint8_t>(c.cw_count);
    frame.payload_len = static_cast<uint16_t>(payload.size());
    frame.payload = payload;
    return frame.serialize();
}

// One full round trip. Returns the number of data-carrier symbols compared.
size_t runRoundTrip(const Case& c, std::mt19937& rng) {
    const ModemConfig cfg = makeConfig(c.mod, c.rate);

    OFDMChirpWaveform tx(cfg);
    tx.configure(c.mod, c.rate);
    tx.setCarrierLdpcInterleaverEnabled(c.carrier_ldpc);
    tx.setActiveLDPCLiftingZ(static_cast<uint8_t>(c.lifting_z));

    OFDMChirpWaveform rx(cfg);
    rx.configure(c.mod, c.rate);
    rx.setCarrierLdpcInterleaverEnabled(c.carrier_ldpc);
    rx.setActiveLDPCLiftingZ(static_cast<uint8_t>(c.lifting_z));

    // bits_per_symbol drives the ChannelInterleaver's geometry. The receiver derives
    // it from its own carrier count and modulation; use the same expression so the
    // test exercises the production agreement rather than papering over it.
    const size_t data_carriers = dataCarriers(tx);
    require(data_carriers > 0, "no data carriers");
    const size_t bits_per_symbol =
        data_carriers * static_cast<size_t>(getBitsPerSymbol(c.mod));

    // ---- TRANSMIT ----------------------------------------------------------
    const Bytes frame_bytes = buildFrameBytes(c, rng);
    const Bytes encoded = v2::encodeFixedFrame(frame_bytes, c.rate, c.cw_count,
                                               c.channel_interleave, bits_per_symbol,
                                               c.lifting_z);
    require(!encoded.empty(), "encodeFixedFrame produced nothing");
    (void)tx.modulate(encoded);
    const Symbol tx_grid = tx.getLastDataCarrierSymbolsForTesting();
    require(!tx_grid.empty(), "transmit produced an empty data-carrier grid");
    require(tx_grid.size() % data_carriers == 0,
            "transmit grid is not symbol-aligned");

    // ---- RECEIVE (noiseless LLRs — this test is about the MAPPING, not the PHY) --
    bool provisional = false;
    auto status = v2::decodeFixedFrame(bytesToSoftBits(encoded), c.rate, c.cw_count,
                                       c.channel_interleave, bits_per_symbol,
                                       nullptr, nullptr, c.lifting_z, provisional);
    require(status.allSuccess(), "noiseless decode failed");
    require(!status.usedAnyPerturbation(), "noiseless decode needed perturbation");

    const Bytes reassembled = status.reassemble();
    require(!reassembled.empty(), "reassemble() returned nothing");
    require(reassembled == frame_bytes,
            "reassembled frame bytes differ from the transmitted frame bytes");

    // ---- RE-ENCODE + RE-MODULATE (what the feature actually does) -----------
    const Bytes re_encoded = v2::encodeFixedFrame(reassembled, c.rate, c.cw_count,
                                                  c.channel_interleave,
                                                  bits_per_symbol, c.lifting_z);
    require(re_encoded == encoded,
            "re-encoded air bytes differ from the transmitted air bytes");

    const Symbol rx_grid = rx.remodulateDataCarrierSymbols(re_encoded);
    require(rx_grid.size() == tx_grid.size(),
            "re-modulated grid has a different symbol count");
    for (size_t i = 0; i < tx_grid.size(); ++i) {
        if (!(rx_grid[i] == tx_grid[i])) {
            std::ostringstream os;
            os << "re-modulated X differs at flat index " << i << " (symbol "
               << (i / data_carriers) << ", data carrier " << (i % data_carriers)
               << "): tx=(" << tx_grid[i].real() << "," << tx_grid[i].imag()
               << ") rx=(" << rx_grid[i].real() << "," << rx_grid[i].imag() << ")";
            throw std::runtime_error(os.str());
        }
    }
    return tx_grid.size();
}

}  // namespace

int main() {
    std::cout << "=== ULTRA_ITERATIVE_CHEST re-modulation round-trip ===\n\n";

    // -------------------------------------------------------------------------
    // 1. The family sweep. This is the adaptivity gate: the genie table shows the
    //    estimator limits QPSK and 8PSK too (QPSK R3/4 @14: 69.8 -> 98.8), so a
    //    16QAM-only reconstruction would be BUG-8PSK-001 all over again.
    // -------------------------------------------------------------------------
    {
        std::mt19937 rng(20260728u);
        size_t case_count = 0;
        size_t symbols_compared = 0;
        std::string failure;

        TEST("round-trip is bit-exact across mod x rate x cw x Z x interleavers") {
            for (Modulation mod : {Modulation::QPSK, Modulation::QAM8, Modulation::QAM16}) {
                for (CodeRate rate : {CodeRate::R1_2, CodeRate::R2_3, CodeRate::R3_4}) {
                    for (int cw : {1, 2, 4, 8}) {
                        for (int z : {27, 81}) {
                            // Z=81 IS the production burst geometry (measured on the
                            // GUI gate: 8PSK R2/3 z=81 cw=4 => 4 x 1944 bits => 51 data
                            // symbols in a 59360-sample block). An earlier version of
                            // this sweep skipped z=81 above cw=2 and therefore skipped
                            // the exact configuration production runs — do not narrow
                            // it again.
                            for (bool chan_il : {false, true}) {
                                for (bool carrier_ldpc : {false, true}) {
                                    for (bool short_payload : {false, true}) {
                                        Case c{mod, rate, cw, z, chan_il,
                                               carrier_ldpc, short_payload};
                                        try {
                                            symbols_compared += runRoundTrip(c, rng);
                                            ++case_count;
                                        } catch (const std::exception& e) {
                                            failure = describe(c) + ": " + e.what();
                                            throw std::runtime_error(failure);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            require(case_count >= 200, "sweep did not cover enough cases");
            std::cout << "(" << case_count << " cases, " << symbols_compared
                      << " carrier symbols) ";
            PASS();
        } catch (const std::exception& e) {
            FAIL(e.what());
        }
    }

    // -------------------------------------------------------------------------
    // 2. NEGATIVE CONTROL. Corrupt one information byte and the reconstruction must
    //    differ. Without this the sweep above could be passing vacuously.
    // -------------------------------------------------------------------------
    TEST("a single corrupted information byte changes the re-modulated X") {
        const Case c{Modulation::QAM16, CodeRate::R2_3, 4, 27, false, false, false};
        const ModemConfig cfg = makeConfig(c.mod, c.rate);
        OFDMChirpWaveform tx(cfg);
        tx.configure(c.mod, c.rate);
        const size_t data_carriers = dataCarriers(tx);
        const size_t bits_per_symbol =
            data_carriers * static_cast<size_t>(getBitsPerSymbol(c.mod));

        std::mt19937 rng(4242u);
        Bytes frame_bytes = buildFrameBytes(c, rng);
        const Bytes encoded = v2::encodeFixedFrame(frame_bytes, c.rate, c.cw_count,
                                                   c.channel_interleave,
                                                   bits_per_symbol, c.lifting_z);
        (void)tx.modulate(encoded);
        const Symbol tx_grid = tx.getLastDataCarrierSymbolsForTesting();

        Bytes corrupted = frame_bytes;
        require(corrupted.size() > 40, "frame too small to corrupt meaningfully");
        corrupted[40] = static_cast<uint8_t>(corrupted[40] ^ 0x01u);
        const Bytes bad_encoded = v2::encodeFixedFrame(corrupted, c.rate, c.cw_count,
                                                       c.channel_interleave,
                                                       bits_per_symbol, c.lifting_z);
        require(bad_encoded != encoded, "corruption did not change the air bytes");

        OFDMChirpWaveform rx(cfg);
        rx.configure(c.mod, c.rate);
        const Symbol bad_grid = rx.remodulateDataCarrierSymbols(bad_encoded);
        require(bad_grid.size() == tx_grid.size(), "grid size changed unexpectedly");
        bool differs = false;
        for (size_t i = 0; i < tx_grid.size() && !differs; ++i) {
            differs = !(bad_grid[i] == tx_grid[i]);
        }
        require(differs, "a corrupted frame re-modulated to the SAME X — the "
                         "round-trip check has no teeth");
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }

    // -------------------------------------------------------------------------
    // 3. The knob itself. It must NOT be a latching function-local static const:
    //    one binary has to run both A/B arms and a test has to toggle it.
    // -------------------------------------------------------------------------
    TEST("ULTRA_ITERATIVE_CHEST is default-off and re-readable in-process") {
        ::unsetenv("ULTRA_ITERATIVE_CHEST");
        require(!ultra::ofdm::iterativeChestEnabled(), "knob is not default-off");
        ::setenv("ULTRA_ITERATIVE_CHEST", "1", 1);
        require(ultra::ofdm::iterativeChestEnabled(), "knob did not turn on");
        ::setenv("ULTRA_ITERATIVE_CHEST", "0", 1);
        require(!ultra::ofdm::iterativeChestEnabled(),
                "knob latched — a function-local static const cannot be A/B'd");
        ::unsetenv("ULTRA_ITERATIVE_CHEST");
        require(!ultra::ofdm::iterativeChestEnabled(), "knob did not return to off");
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }

    // -------------------------------------------------------------------------
    // 4. Feature is INERT when the knob is off: no receive grid is retained, and an
    //    ingest attempt does nothing. (The byte-identity of the decode path itself
    //    is proven separately by the full CTest suite running knob-off.)
    // -------------------------------------------------------------------------
    TEST("data-aided path is inert until explicitly enabled") {
        const ModemConfig cfg = makeConfig(Modulation::QAM16, CodeRate::R2_3);
        OFDMChirpWaveform rx(cfg);
        rx.configure(Modulation::QAM16, CodeRate::R2_3);
        Bytes junk(81 * 4, 0xA5);
        require(rx.ingestDataAidedFrame(junk) == 0,
                "ingest was not inert with the feature disabled");
        rx.setDataAidedFeedbackEnabled(true);
        // Still 0: no frame has been demodulated, so there is no retained receive
        // grid and no origin to match. "Degrade to baseline, never to garbage."
        require(rx.ingestDataAidedFrame(junk) == 0,
                "ingest fabricated observations with no retained receive grid");
        rx.setDataAidedFeedbackEnabled(false);
        PASS();
    } catch (const std::exception& e) {
        FAIL(e.what());
    }

    std::cout << "\n=== " << tests_passed << " passed, " << tests_failed
              << " failed ===\n";
    return tests_failed == 0 ? 0 : 1;
}
