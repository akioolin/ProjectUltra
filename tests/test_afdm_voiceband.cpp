#include "afdm/afdm.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace ultra;
using namespace ultra::afdm;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) std::cout << "\n=== TEST: " << name << " ===" << std::endl
#define PASS(msg) do { std::cout << "[PASS] " << msg << std::endl; tests_passed++; } while(0)
#define FAIL(msg) do { std::cout << "[FAIL] " << msg << std::endl; tests_failed++; } while(0)
#define CHECK(cond, msg) do { if (cond) PASS(msg); else FAIL(msg); } while(0)

bool test_config_voiceband_geometry() {
    TEST("AFDM Voice-Band Geometry");

    AFDMConfig cfg = AFDMConfig::forHFFading();
    auto active = cfg.getActiveCarrierIndices();
    auto edges = cfg.occupiedBandEdgesHz();

    const float spacing = cfg.subcarrierSpacing();
    const float expected_spacing = 48000.0f / 512.0f;
    const float expected_bw = static_cast<float>(active.size()) * spacing;

    std::cout << "  active carriers: " << active.size() << " (configured " << cfg.num_carriers << ")" << std::endl;
    std::cout << "  spacing: " << spacing << " Hz" << std::endl;
    std::cout << "  occupied band: " << edges.first << " - " << edges.second << " Hz" << std::endl;

    CHECK(active.size() == cfg.num_carriers, "Active carrier count matches config");
    CHECK(std::abs(spacing - expected_spacing) < 1e-3f, "Subcarrier spacing uses sample_rate/N");
    CHECK(std::abs(cfg.occupiedBandwidthHz() - expected_bw) < 1e-3f,
          "Occupied bandwidth matches active-carrier geometry");

    // Audio-chain practical bounds used elsewhere in this project (around 100-2900 Hz).
    CHECK(edges.first >= 50.0f, "Low edge stays in practical HF audio passband");
    CHECK(edges.second <= 3000.0f, "High edge stays in practical HF audio passband");

    return true;
}

bool test_audio_loopback_noiseless() {
    TEST("AFDM Audio Loopback (Noiseless)");

    AFDMConfig cfg = AFDMConfig::forHFFading();
    AFDMModulator tx(cfg);
    AFDMDemodulator rx(cfg);

    const Modulation mod = Modulation::BPSK;
    const size_t payload_bytes = tx.bytesPerFrame(mod);

    Bytes payload(payload_bytes);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    }

    Samples signal = tx.modulate(payload, mod);
    Bytes decoded = rx.demodulateHard(signal, mod);

    std::cout << "  payload bytes: " << payload.size() << std::endl;
    std::cout << "  decoded bytes: " << decoded.size() << std::endl;

    CHECK(decoded.size() >= payload.size(), "Decoded stream has expected payload length");

    size_t byte_errors = 0;
    const size_t compare_len = std::min(payload.size(), decoded.size());
    for (size_t i = 0; i < compare_len; ++i) {
        if (payload[i] != decoded[i]) {
            byte_errors++;
        }
    }

    std::cout << "  byte errors (first payload bytes): " << byte_errors << std::endl;
    CHECK(byte_errors == 0, "No payload byte errors in noiseless loopback");

    return byte_errors == 0;
}

bool test_c1_guard_on_audio_path() {
    TEST("AFDM c1 Guard (Audio Path)");

    AFDMConfig cfg = AFDMConfig::forWidebandRF();
    AFDMModulator tx(cfg);
    AFDMDemodulator rx(cfg);

    bool tx_threw = false;
    try {
        Bytes payload(8, 0xAA);
        (void)tx.modulate(payload, Modulation::BPSK);
    } catch (const std::runtime_error&) {
        tx_threw = true;
    }

    bool rx_threw = false;
    try {
        std::vector<float> dummy(16, 0.0f);
        (void)rx.demodulate({dummy.data(), dummy.size()}, Modulation::BPSK);
    } catch (const std::runtime_error&) {
        rx_threw = true;
    }

    CHECK(tx_threw, "TX path rejects c1>0 audio operation");
    CHECK(rx_threw, "RX path rejects c1>0 audio operation");

    return tx_threw && rx_threw;
}

int main() {
    std::cout << "========================================\n";
    std::cout << "AFDM Voice-Band Validation\n";
    std::cout << "========================================\n";

    bool ok = true;
    ok &= test_config_voiceband_geometry();
    ok &= test_audio_loopback_noiseless();
    ok &= test_c1_guard_on_audio_path();

    std::cout << "\n========================================\n";
    std::cout << "Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "========================================\n";

    return (ok && tests_failed == 0) ? 0 : 1;
}
